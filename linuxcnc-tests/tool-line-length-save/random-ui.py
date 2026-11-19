#!/usr/bin/env python3
"""A random toolchanger saves the table after every change. With a line
refused on load the save is refused, so M6 fails with an operator error
instead of reporting success: the tools are swapped in memory (as they are
in the changer) and the file is left as it was."""

import linuxcnc
import linuxcnc_util

import sys
import time

c = linuxcnc.command()
e = linuxcnc.error_channel()
s = linuxcnc.stat()
l = linuxcnc_util.LinuxCNC(command=c, status=s, error=e)


def fail(msg):
    print("ERROR: " + msg)
    sys.exit(1)


def table():
    with open("tool.tbl") as f:
        return f.read()


def wait_error(text, timeout=10):
    start = time.time()
    while time.time() - start < timeout:
        err = e.poll()
        if err is None:
            time.sleep(0.01)
            continue
        print("(error channel: %s)" % (err[1],))
        if err[0] in (linuxcnc.OPERATOR_ERROR, linuxcnc.NML_ERROR) and text in err[1]:
            return
    fail("no error containing '%s'" % text)


l.wait_for_linuxcnc_startup()
c.state(linuxcnc.STATE_ESTOP_RESET)
c.state(linuxcnc.STATE_ON)
c.mode(linuxcnc.MODE_MDI)
c.wait_complete()

before = table()
print("*** M6 with a table that may not be saved reports an error")
c.mdi("t1 m6")
c.wait_complete()
wait_error("not saved after the tool change")
s.poll()
if s.tool_in_spindle != 1:
    fail("tool in spindle %d, expected 1" % s.tool_in_spindle)
if s.tool_table[0].id != 1:
    fail("spindle pocket holds T%d, expected T1" % s.tool_table[0].id)
if table() != before:
    fail("the table file was rewritten:\n" + table())
print("tool.tbl unchanged, T3's line kept")

print("*** a reload is refused while pocket 0 does not show the spindle tool")
short_t3 = "T3 P3 Z+2.000000 ;shortened\n"
with open("tool.tbl", "w") as f:
    f.write("T2 P0 Z+1.000000\nT1 P1 Z+10.000000\n" + short_t3)
c.load_tool_table()
c.wait_complete()
wait_error("pocket 0")
s.poll()
if s.tool_in_spindle != 1 or s.tool_table[0].id != 1:
    fail("reload changed the spindle: status T%d, pocket 0 T%d"
         % (s.tool_in_spindle, s.tool_table[0].id))

print("*** with the file matching the machine, the reload and changes work")
with open("tool.tbl", "w") as f:
    f.write("T1 P0 Z+10.000000\nT2 P1 Z+1.000000\n" + short_t3)
c.load_tool_table()
c.wait_complete()
c.mode(linuxcnc.MODE_MDI)
c.wait_complete()
c.mdi("t2 m6")
c.wait_complete()
deadline = time.time() + 10
while time.time() < deadline:
    s.poll()
    lines = [" ".join(line.split()) for line in table().split("\n")]
    if s.tool_in_spindle == 2 and any(l.startswith("T2 P0") for l in lines):
        break
    time.sleep(0.01)
else:
    fail("T2 M6 after the reload was not saved:\n" + table())
print("*** all checks passed")
sys.exit(0)
