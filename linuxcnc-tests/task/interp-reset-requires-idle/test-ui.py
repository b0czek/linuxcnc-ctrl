#!/usr/bin/env python3
#
# Check that task refuses an interpreter reset while the machine is busy and
# accepts it once the machine is idle.

import sys
import time

import linuxcnc

c = linuxcnc.command()
s = linuxcnc.stat()
e = linuxcnc.error_channel()


def fail(msg):
    print("FAIL: %s" % msg)
    sys.exit(1)


def wait_for(cond, what, timeout=10.0):
    start = time.time()
    while time.time() - start < timeout:
        s.poll()
        if cond():
            return
        time.sleep(0.02)
    fail("timeout waiting for %s: task_state=%d mode=%d interp_state=%d "
         "exec_state=%d queue=%d inpos=%d" % (what, s.task_state, s.task_mode,
         s.interp_state, s.exec_state, s.queue, s.inpos))


def set_state(state):
    c.state(state)
    c.wait_complete()
    wait_for(lambda: s.task_state == state, "task state %d" % state)


def set_mode(mode):
    c.mode(mode)
    c.wait_complete()
    wait_for(lambda: s.task_mode == mode, "task mode %d" % mode)


def drain_errors():
    while e.poll():
        pass


def wait_idle(what):
    wait_for(lambda: s.interp_state == linuxcnc.INTERP_IDLE
             and s.exec_state == linuxcnc.EXEC_DONE
             and s.queue == 0 and s.inpos and s.current_vel == 0.0,
             what)


def incremental():
    s.poll()
    return 910 in s.gcodes


def mdi(code):
    set_mode(linuxcnc.MODE_MDI)
    c.mdi(code)
    c.wait_complete()


def expect_reset_refused(name):
    drain_errors()
    c.reset_interpreter()
    c.wait_complete()
    deadline = time.time() + 5.0
    while time.time() < deadline:
        msg = e.poll()
        if msg:
            if "interpreter reset refused" in msg[1]:
                break
            fail("%s: unexpected error %r" % (name, msg[1]))
        time.sleep(0.02)
    else:
        fail("%s: reset was not refused" % name)
    if not incremental():
        fail("%s: refused reset still restored the startup modes" % name)
    print("%s: refused" % name, flush=True)


def expect_reset_accepted(name):
    drain_errors()
    c.reset_interpreter()
    c.wait_complete()
    wait_for(lambda: 900 in s.gcodes, "%s: reset to restore G90" % name)
    msg = e.poll()
    if msg:
        fail("%s: unexpected error %r" % (name, msg[1]))
    print("%s: ok" % name, flush=True)


def abort_and_settle(name):
    c.abort()
    c.wait_complete()
    wait_idle("%s: abort to finish" % name)


wait_for(lambda: s.task_state == linuxcnc.STATE_ESTOP, "task to start")
# Task is still busy with its startup work for a few cycles.
wait_idle("task startup to finish")

# Resets of a machine at rest are allowed in every state and mode.
mdi_g91 = lambda: mdi("G91")
expect_reset_accepted("E-stop")
set_state(linuxcnc.STATE_ESTOP_RESET)
set_state(linuxcnc.STATE_ON)
mdi_g91()
expect_reset_accepted("idle MDI")
mdi_g91()
set_mode(linuxcnc.MODE_MANUAL)
expect_reset_accepted("idle manual")

# An MDI move at 60 mm/min takes 5 s for 5 mm.
mdi_g91()
c.mdi("G1 X5 F60")
wait_for(lambda: s.current_vel > 0.0, "MDI move to start")
expect_reset_refused("MDI move")
s.poll()
if s.current_vel <= 0.0:
    fail("MDI move: the refused reset stopped the move")
abort_and_settle("MDI move")
expect_reset_accepted("after aborted MDI move")

# A dwell keeps task busy without any motion.
mdi_g91()
c.mdi("G4 P3")
wait_for(lambda: s.exec_state != linuxcnc.EXEC_DONE, "dwell to start")
expect_reset_refused("dwell")
abort_and_settle("dwell")
expect_reset_accepted("after aborted dwell")

# A continuous teleop jog moves the machine outside any program.
mdi_g91()
set_mode(linuxcnc.MODE_MANUAL)
c.teleop_enable(1)
c.wait_complete()
c.jog(linuxcnc.JOG_CONTINUOUS, False, 0, 1.0)
wait_for(lambda: s.current_vel > 0.0, "jog to start")
expect_reset_refused("jog")
c.jog(linuxcnc.JOG_STOP, False, 0)
c.wait_complete()
wait_idle("jog to stop")
expect_reset_accepted("after stopped jog")

print("all scenarios passed")
set_state(linuxcnc.STATE_ESTOP)
sys.exit(0)
