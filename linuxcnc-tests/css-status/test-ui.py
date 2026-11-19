#!/usr/bin/env python3

import math
import sys
import time

import linuxcnc


def fail(message):
    print(message)
    sys.exit(1)


def wait_complete(message, timeout=10.0):
    if command.wait_complete(timeout) != linuxcnc.RCS_DONE:
        fail(message)
    status.poll()


def assert_css_maximum(expected, message):
    status.poll()
    actual = status.spindle[0]["css_maximum"]
    if not math.isclose(actual, expected, abs_tol=0.001):
        fail("%s: expected %.3f, got %.3f" % (message, expected, actual))


command = linuxcnc.command()
status = linuxcnc.stat()

deadline = time.time() + 10.0
while time.time() < deadline:
    status.poll()
    if status.task_state == linuxcnc.STATE_ESTOP:
        break
    time.sleep(0.02)
else:
    fail("LinuxCNC did not initialize")

command.state(linuxcnc.STATE_ESTOP_RESET)
command.state(linuxcnc.STATE_ON)
wait_complete("machine did not turn on")

command.mode(linuxcnc.MODE_MDI)
wait_complete("machine did not enter MDI")

command.mdi("G96 S100 D2111")
wait_complete("G96 command did not complete")
assert_css_maximum(2111.0, "G96 did not publish its D word")
if status.spindle[0]["enabled"]:
    fail("G96 reported a stopped spindle as running")

command.mdi("M5")
wait_complete("M5 command did not complete")
assert_css_maximum(2111.0, "M5 discarded the modal CSS maximum")

command.mdi("G97")
wait_complete("G97 command did not complete")
assert_css_maximum(0.0, "G97 did not clear the CSS maximum")

print("CSS status scenario ok")
