# shared by run1.py and run2.py
import linuxcnc
import linuxcnc_util

import math
import sys
import time

c = linuxcnc.command()
e = linuxcnc.error_channel()
s = linuxcnc.stat()
l = linuxcnc_util.LinuxCNC(command=c, status=s, error=e)


def fail(msg):
    print("ERROR: " + msg)
    sys.exit(1)


def check(what, got, expected):
    print("%s = %.6f (expecting %.6f)" % (what, got, expected))
    if math.fabs(got - expected) > 1e-6:
        fail("%s = %f, expected %f" % (what, got, expected))


def start():
    l.wait_for_linuxcnc_startup()
    c.state(linuxcnc.STATE_ESTOP_RESET)
    c.state(linuxcnc.STATE_ON)
    c.mode(linuxcnc.MODE_MDI)
    c.wait_complete()


def mdi(cmd):
    print("mdi: " + cmd)
    c.mdi(cmd)
    if c.wait_complete(10) == -1:
        fail("timeout running '%s'" % cmd)
    s.poll()


def param(n):
    c.mdi("(debug,#%d)" % n)
    c.wait_complete()
    start_time = time.time()
    while time.time() - start_time < 10:
        err = e.poll()
        if err is None:
            time.sleep(0.01)
            continue
        if err[0] == linuxcnc.OPERATOR_DISPLAY:
            return float(err[1])
        print(err[1])
    fail("no value for #%d" % n)


def check_state(toolno, stored, wear, applied):
    s.poll()
    check("tool_in_spindle", s.tool_in_spindle, toolno)
    check("#5400 tool", param(5400), toolno)
    check("#5401 stored geometry X", param(5401), stored[0])
    check("#5403 stored geometry Z", param(5403), stored[1])
    check("#5430 stored wear X", param(5430), wear[0])
    check("#5432 stored wear Z", param(5432), wear[1])
    check("#5081 applied X", param(5081), applied[0])
    check("#5083 applied Z", param(5083), applied[1])
    s.poll()
    check("stat.tool_offset X", s.tool_offset[0], applied[0])
    check("stat.tool_offset Z", s.tool_offset[2], applied[1])
    mdi("g0 x0 z0")
    check("machine X after G0 X0 Z0", s.position[0], applied[0])
    check("machine Z after G0 X0 Z0", s.position[2], applied[1])
