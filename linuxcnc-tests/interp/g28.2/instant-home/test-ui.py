#!/usr/bin/env python3

import linuxcnc
import hal

import sys
import time

h = hal.component("python-ui")
h.ready()

c = linuxcnc.command()
s = linuxcnc.stat()
e = linuxcnc.error_channel()


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def wait_for(cond, what, timeout=10.0):
    start = time.time()
    while time.time() - start < timeout:
        s.poll()
        if cond():
            return
        time.sleep(0.01)
    fail("timeout waiting for " + what)


def errors():
    found = []
    while True:
        err = e.poll()
        if not err:
            return found
        found.append(err[1])


def home(joint, what):
    s.poll()
    mode = s.motion_mode
    c.mdi("G28.2 P%d" % joint)
    result = c.wait_complete(8)
    s.poll()
    reported = errors()
    if result != linuxcnc.RCS_DONE or reported:
        fail("%s: status %s, errors %s" % (what, result, reported))
    if not s.homed[joint]:
        fail("%s: joint %d is not homed" % (what, joint))
    if s.motion_mode != mode:
        fail("%s: motion mode %d, was %d" % (what, s.motion_mode, mode))
    print("%s: ok" % what, flush=True)


wait_for(lambda: s.task_state == linuxcnc.STATE_ESTOP, "task to start")
c.state(linuxcnc.STATE_ESTOP_RESET)
c.wait_complete()
c.state(linuxcnc.STATE_ON)
c.wait_complete()
c.mode(linuxcnc.MODE_MDI)
c.wait_complete()

home(0, "absolute encoder")
home(1, "absolute encoder with final move")
home(1, "already homed, HOME_NO_REHOME")
home(2, "zero homing velocities")

hal.set_p("motion.homing-inhibit", "1")
c.mdi("G28.2 P2")
result = c.wait_complete(8)
reported = errors()
if result != linuxcnc.RCS_ERROR or not any("did not start" in m for m in reported):
    fail("inhibited home: status %s, errors %s" % (result, reported))
hal.set_p("motion.homing-inhibit", "0")
print("inhibited home: ok", flush=True)

print("all checks passed")
sys.exit(0)
