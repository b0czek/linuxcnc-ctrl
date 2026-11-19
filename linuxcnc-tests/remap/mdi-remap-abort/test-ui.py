#!/usr/bin/env python3
#
# Interrupt an MDI command whose remap is mid-execution in every way that
# resets the interpreter (E-stop, machine off, mode change, abort followed by
# an interpreter re-init once idle), then bring the machine back and check that
# task is still alive and MDI still works.

import os
import sys
import time

import linuxcnc

c = linuxcnc.command()
s = linuxcnc.stat()


def fail(msg):
    print("FAIL: %s" % msg)
    sys.exit(1)


def task_alive():
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open("/proc/%s/comm" % pid) as f:
                if f.read().strip() == "milltask":
                    return True
        except OSError:
            pass
    return False


def wait_for(cond, what, timeout=10.0):
    start = time.time()
    while time.time() - start < timeout:
        s.poll()
        if cond():
            return
        time.sleep(0.02)
    if not task_alive():
        fail("milltask died (waiting for %s)" % what)
    fail("timeout waiting for %s: task_state=%d mode=%d interp_state=%d "
         "exec_state=%d call_level=%d" % (what, s.task_state, s.task_mode,
         s.interp_state, s.exec_state, s.call_level))


def set_state(state, reported=None):
    c.state(state)
    c.wait_complete()
    if reported is None:
        reported = state
    wait_for(lambda: s.task_state == reported, "task state %d" % reported)


def set_mode(mode):
    c.mode(mode)
    c.wait_complete()
    wait_for(lambda: s.task_mode == mode, "task mode %d" % mode)


def machine_on():
    set_state(linuxcnc.STATE_ESTOP_RESET)
    set_state(linuxcnc.STATE_ON)


def estop():
    set_state(linuxcnc.STATE_ESTOP)
    machine_on()


def machine_off():
    # task reports a machine that is out of E-stop but off as ESTOP_RESET
    set_state(linuxcnc.STATE_OFF, linuxcnc.STATE_ESTOP_RESET)
    set_state(linuxcnc.STATE_ON)


def abort_then_plan_init():
    # A re-init is only valid on an idle machine, so stop the work first.
    c.abort()
    c.wait_complete()
    wait_for(lambda: s.interp_state == linuxcnc.INTERP_IDLE
             and s.exec_state == linuxcnc.EXEC_DONE
             and s.queue == 0 and s.inpos, "abort to finish")
    c.reset_interpreter()
    c.wait_complete()


def mode_auto():
    set_mode(linuxcnc.MODE_AUTO)
    set_mode(linuxcnc.MODE_MDI)


target = [0.0]


def check_mdi_works(name):
    # The machine may have dropped out of MDI mode; select it as a GUI would,
    # then give task's main loop time to act on any stale MDI state.
    set_mode(linuxcnc.MODE_MDI)
    time.sleep(0.5)
    if not task_alive():
        fail("%s: milltask died" % name)
    target[0] = 1.0 if target[0] != 1.0 else 2.0
    c.mdi("G0 X%.1f" % target[0])
    c.wait_complete()
    wait_for(lambda: abs(s.position[0] - target[0]) < 1e-6
             and s.interp_state == linuxcnc.INTERP_IDLE,
             "%s: MDI G0 X%.1f" % (name, target[0]))
    if not task_alive():
        fail("%s: milltask died" % name)


def scenario(name, code, interrupt):
    set_mode(linuxcnc.MODE_MDI)
    c.mdi(code)
    wait_for(lambda: s.interp_state != linuxcnc.INTERP_IDLE,
             "%s: %s to start" % (name, code))
    # M400 waits for 3 s, M401 until interrupted; make sure it is waiting.
    time.sleep(0.3)
    s.poll()
    if s.interp_state == linuxcnc.INTERP_IDLE:
        fail("%s: %s is not waiting inside its remap" % (name, code))
    # An MDI command sent meanwhile is queued behind the waiting remap; the
    # interrupt drops it together with the remap's call.
    c.mdi("G0 X5")
    wait_for(lambda: s.queued_mdi_commands == 1,
             "%s: G0 X5 to be queued behind %s" % (name, code))
    interrupt()
    # Check before set_mode: selecting a mode clears the queue by itself.
    s.poll()
    if s.queued_mdi_commands != 0:
        fail("%s: %d queued MDI commands survived"
             % (name, s.queued_mdi_commands))
    if s.command != "":
        fail("%s: interrupted command still reported: %r" % (name, s.command))
    set_mode(linuxcnc.MODE_MDI)
    time.sleep(0.5)
    s.poll()
    if abs(s.position[0] - target[0]) > 1e-6:
        fail("%s: queued G0 X5 ran (X=%.4f)" % (name, s.position[0]))
    check_mdi_works(name)
    print("%s: ok" % name, flush=True)


wait_for(lambda: s.task_state == linuxcnc.STATE_ESTOP, "task to start")
machine_on()
set_mode(linuxcnc.MODE_MDI)
check_mdi_works("startup")

for code in ("M400", "M401"):
    scenario("%s E-stop" % code, code, estop)
    scenario("%s machine off" % code, code, machine_off)
    scenario("%s abort and re-init" % code, code, abort_then_plan_init)
    scenario("%s mode change" % code, code, mode_auto)

print("all scenarios passed")
set_state(linuxcnc.STATE_ESTOP)
sys.exit(0)
