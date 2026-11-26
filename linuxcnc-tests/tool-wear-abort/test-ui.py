#!/usr/bin/env python3
"""Abort or E-stop with an automatic tool offset interpreted but not executed.

Built-in M6 and M61 queue their geometry-plus-wear offset behind the tool
change. When the change (or the move ahead of it) is aborted, the spindle
keeps the old tool, and so must every copy of the applied offset: canon's,
the interpreter's, #5081-#5089 and motion's. The check that matters is where
the next move goes: G0 X0 Z0 must land on the loaded tool's geometry plus
wear in machine coordinates.
"""

import linuxcnc
import hal

import math
import os
import sys
import time

sys.stdout = os.fdopen(sys.stdout.fileno(), 'w')

EPS = 1e-6
HERE = os.path.dirname(os.path.abspath(__file__))

# geometry plus wear (X, Z) per tool; T0 is the empty spindle
APPLIED = {0: (0.0, 0.0), 1: (5.2, 9.9), 2: (-2.8, 20.15)}
POCKET = {1: 1, 2: 2}


def fail(msg):
    print("ERROR: " + msg)
    os.system("halcmd show pin python-ui")
    sys.exit(1)


failures = []


def check(what, got, expected):
    """Record a mismatch and carry on, so one run shows every copy of the
    offset that went wrong; the test fails at the end."""
    print("%s = %.6f (expecting %.6f)" % (what, got, expected))
    if math.fabs(got - expected) > EPS:
        print("MISMATCH: %s = %f, expected %f" % (what, got, expected))
        failures.append(what)


def wait_for(predicate, what, timeout=10):
    start = time.time()
    while not predicate():
        if time.time() - start > timeout:
            fail("timeout waiting for " + what)
        time.sleep(0.01)
        s.poll()


def wait_pin(name, value, timeout=5):
    start = time.time()
    while h[name] != value:
        if time.time() - start > timeout:
            fail("pin %s did not reach %s" % (name, value))
        time.sleep(0.01)


def wait_done(what, timeout=5):
    start = time.time()
    while time.time() - start < timeout:
        s.poll()
        if s.state == linuxcnc.RCS_DONE and \
           s.interp_state == linuxcnc.INTERP_IDLE:
            return
        time.sleep(0.01)
    fail("not idle after %s" % what)


def prepare_handshake():
    wait_pin('tool-prepare', 1)
    h['tool-prepared'] = 1
    wait_pin('tool-prepare', 0)
    h['tool-prepared'] = 0


def change_handshake():
    wait_pin('tool-change', 1)
    h['tool-changed'] = 1
    wait_pin('tool-change', 0)
    h['tool-changed'] = 0


def mdi(cmd):
    print("mdi: " + cmd)
    c.mode(linuxcnc.MODE_MDI)
    c.wait_complete()
    c.mdi(cmd)
    if c.wait_complete(30) == -1:
        fail("timeout running '%s'" % cmd)
    s.poll()


def param(n):
    c.mode(linuxcnc.MODE_MDI)
    c.wait_complete()
    c.mdi("(debug,#%d)" % n)
    c.wait_complete()
    start = time.time()
    while time.time() - start < 10:
        err = e.poll()
        if err is None:
            time.sleep(0.01)
            continue
        kind, text = err
        if kind == linuxcnc.OPERATOR_DISPLAY:
            return float(text)
        print(text)
    fail("no value for #%d" % n)


def drain_errors():
    while True:
        err = e.poll()
        if err is None:
            return
        print("(error channel: %s)" % (err[1],))


def check_loaded(toolno, what):
    """The spindle holds toolno and the next move uses its offset."""
    check_applied(toolno, toolno, what)


def check_applied(toolno, offset_of, what):
    """The spindle holds toolno, and motion, canon, the interpreter and
    #5081-#5089 all have tool offset_of's geometry plus wear, so the next
    move lands there."""
    print("*** after %s: T%d loaded, T%d's offset applied"
          % (what, toolno, offset_of))
    s.poll()
    check("tool_in_spindle", s.tool_in_spindle, toolno)
    ax, az = APPLIED[offset_of]
    check("stat.tool_offset X", s.tool_offset[0], ax)
    check("stat.tool_offset Z", s.tool_offset[2], az)
    check("#5081 applied X", param(5081), ax)
    check("#5083 applied Z", param(5083), az)
    mdi("g0 x0 z0")
    s.poll()
    check("machine X after G0 X0 Z0", s.position[0], ax)
    check("machine Z after G0 X0 Z0", s.position[2], az)


def rebaseline():
    """Put T1 back with every copy of its offset, whatever the previous
    scenario left behind, so each scenario is checked on its own."""
    mdi("m61 q1")
    mdi("g43")


def load_tool(toolno):
    c.mode(linuxcnc.MODE_MDI)
    c.wait_complete()
    c.mdi("t%d m6" % toolno)
    prepare_handshake()
    change_handshake()
    if c.wait_complete(10) == -1:
        fail("t%d m6 did not complete" % toolno)
    check_loaded(toolno, "t%d m6" % toolno)


def machine_on():
    c.state(linuxcnc.STATE_ESTOP_RESET)
    c.state(linuxcnc.STATE_ON)
    c.mode(linuxcnc.MODE_MDI)
    c.wait_complete()


def run_and_abort(program, abort_at_line):
    """Run program in AUTO and abort once it has read abort_at_line, while
    the slow move ahead of it is still running."""
    print("*** running %s, aborting after line %d is read" % (program, abort_at_line))
    c.mode(linuxcnc.MODE_AUTO)
    c.wait_complete()
    c.program_open(os.path.join(HERE, program))
    c.auto(linuxcnc.AUTO_RUN, 0)
    start = time.time()
    while True:
        s.poll()
        if s.read_line >= abort_at_line and s.current_vel > 0:
            break
        if time.time() - start > 10:
            fail("%s never got to line %d while moving" % (program, abort_at_line))
        time.sleep(0.01)
    time.sleep(0.2)
    c.abort()
    wait_done("abort of " + program)
    drain_errors()


h = hal.component("python-ui")
h.newpin("tool-number", hal.Type.SINT, hal.Dir.IN)
h.newpin("tool-prepare", hal.Type.BOOL, hal.Dir.IN)
h.newpin("tool-prepared", hal.Type.BOOL, hal.Dir.OUT)
h.newpin("tool-change", hal.Type.BOOL, hal.Dir.IN)
h.newpin("tool-changed", hal.Type.BOOL, hal.Dir.OUT)
h.ready()
os.system("halcmd source ./postgui.hal")

c = linuxcnc.command()
s = linuxcnc.stat()
e = linuxcnc.error_channel()

machine_on()

load_tool(1)

print("*** abort while the PREPARE handshake for T2 is pending")
c.mdi("t2 m6")
wait_pin('tool-prepare', 1)
c.abort()
wait_pin('tool-prepare', 0)
wait_done("abort during prepare")
drain_errors()
check_loaded(1, "abort during prepare")

rebaseline()
print("*** abort while the CHANGE handshake for T2 is pending")
c.mode(linuxcnc.MODE_MDI)
c.wait_complete()
c.mdi("t2 m6")
prepare_handshake()
wait_pin('tool-change', 1)
c.abort()
wait_pin('tool-change', 0)
wait_done("abort during change")
drain_errors()
check_loaded(1, "abort during change")

rebaseline()
print("*** E-stop while the CHANGE handshake for T2 is pending")
c.mode(linuxcnc.MODE_MDI)
c.wait_complete()
c.mdi("t2 m6")
prepare_handshake()
wait_pin('tool-change', 1)
c.state(linuxcnc.STATE_ESTOP)
wait_pin('tool-change', 0)
wait_for(lambda: s.task_state == linuxcnc.STATE_ESTOP and
         s.state == linuxcnc.RCS_DONE, "E-stop")
drain_errors()
machine_on()
check_loaded(1, "E-stop during change")

rebaseline()
print("*** machine off while the CHANGE handshake for T2 is pending")
c.mode(linuxcnc.MODE_MDI)
c.wait_complete()
c.mdi("t2 m6")
prepare_handshake()
wait_pin('tool-change', 1)
c.state(linuxcnc.STATE_OFF)
wait_pin('tool-change', 0)
# status reports a machine that is off as STATE_ESTOP_RESET
wait_for(lambda: s.task_state != linuxcnc.STATE_ON and
         s.state == linuxcnc.RCS_DONE, "machine off")
drain_errors()
machine_on()
check_loaded(1, "machine off during change")

rebaseline()
run_and_abort("readahead-m61.ngc", 5)
# M61 Q2 and its offset take effect together, after the move: the abort
# during the move discards both.
check_loaded(1, "abort with M61 Q2 read ahead")

rebaseline()
run_and_abort("readahead-g43.ngc", 5)
check_loaded(1, "abort with G43 H2 read ahead")

rebaseline()
def hold_change_then_abort(hold, what):
    """Run change-move.ngc, hold the program (hold()) while the change
    handshake is pending, complete the change, then abort: the change
    completed, so T2 and its offset are both in effect."""
    print("*** %s at the change handshake, change completed, then abort" % what)
    c.mode(linuxcnc.MODE_AUTO)
    c.wait_complete()
    c.program_open(os.path.join(HERE, "change-move.ngc"))
    c.auto(linuxcnc.AUTO_RUN, 0)
    prepare_handshake()
    wait_pin('tool-change', 1)
    hold()
    change_handshake()
    wait_for(lambda: s.tool_in_spindle == 2, "T2 in the spindle")
    c.abort()
    wait_done("abort after a held change")
    drain_errors()
    check_loaded(2, "%s, change completed, abort" % what)


def pause():
    c.auto(linuxcnc.AUTO_PAUSE)
    wait_for(lambda: s.task_paused, "pause")


def program_stop():
    c.program_stop()
    wait_for(lambda: s.stop_state != linuxcnc.STOP_IDLE, "Stop")


rebaseline()
hold_change_then_abort(pause, "pause")

rebaseline()
hold_change_then_abort(program_stop, "resumable Stop")

rebaseline()
print("*** an interpreter error on the M6 line after the change was queued")
c.mode(linuxcnc.MODE_AUTO)
c.wait_complete()
c.program_open(os.path.join(HERE, "interp-error-m6.ngc"))
c.auto(linuxcnc.AUTO_RUN, 0)
wait_done("interp-error-m6.ngc")
drain_errors()
# the next program in AUTO, before any mode switch (which would resync)
c.program_open(os.path.join(HERE, "move-origin.ngc"))
c.auto(linuxcnc.AUTO_RUN, 0)
reported = None
start = time.time()
while reported is None and time.time() - start < 10:
    err = e.poll()
    if err is None:
        time.sleep(0.01)
    elif err[0] == linuxcnc.OPERATOR_DISPLAY and err[1].startswith("applied"):
        reported = [float(v) for v in err[1].split()[1:]]
    else:
        print(err[1])
wait_done("move-origin.ngc")
if reported is None:
    fail("move-origin.ngc reported nothing")
s.poll()
check("#5081 in the next AUTO program", reported[0], APPLIED[1][0])
check("#5083 in the next AUTO program", reported[1], APPLIED[1][1])
check("machine X after its G0 X0 Z0", s.position[0], APPLIED[1][0])
check("machine Z after its G0 X0 Z0", s.position[2], APPLIED[1][1])
check_loaded(1, "interpreter error on the M6 line")

rebaseline()
print("*** a completed change at program end")
c.mode(linuxcnc.MODE_AUTO)
c.wait_complete()
c.program_open(os.path.join(HERE, "change-then-end.ngc"))
c.auto(linuxcnc.AUTO_RUN, 0)
prepare_handshake()
change_handshake()
wait_done("change-then-end.ngc", timeout=10)
check_loaded(2, "T2 M6 then M2")

if failures:
    fail("%d mismatches" % len(failures))
print("*** all checks passed")
sys.exit(0)
