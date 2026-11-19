#!/usr/bin/env python3
"""A tool table with a line refused on load (too long) is not saved: the
save would delete the user's line. A G10 edit is refused, the file is kept
as it was and the edit is not kept in memory; once the line is fixed and
the table reloaded, saving works again."""

import linuxcnc
import linuxcnc_util

import math
import os
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


def wait_for(predicate, what, timeout=10):
    start = time.time()
    while not predicate():
        if time.time() - start > timeout:
            fail("timeout waiting for " + what)
        time.sleep(0.01)
        s.poll()


def tools():
    s.poll()
    return {t.id: t for t in s.tool_table[1:] if t.id > 0}


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

print("*** T3's line was refused on load")
if 3 in tools():
    fail("T3 was loaded from a 600-character line")
check("T1 wxoffset", tools()[1].wxoffset, 0.2)

print("*** a G10 edit is refused: file and memory unchanged")
before = table()
c.mdi("g10 l3 p1 x0.1")
result = c.wait_complete()
if result != linuxcnc.RCS_ERROR:
    fail("MDI G10 with a refused save finished with status %d" % result)
wait_error("offsets not stored")
if table() != before:
    fail("the table file was rewritten:\n" + table())
print("tool.tbl unchanged, T3's line kept")
check("T1 wxoffset after the refused save", tools()[1].wxoffset, 0.2)
check("toolinfo(1) wxoffset after the refused save", s.toolinfo(1)["wxoffset"], 0.2)

print("*** a program with a G10 that cannot be saved stops and says why")
with open("g10.ngc", "w") as f:
    f.write("g10 l3 p1 x0.1\ng4 p30\nm2\n")
c.mode(linuxcnc.MODE_AUTO)
c.wait_complete()
c.program_open(os.path.abspath("g10.ngc"))
c.wait_complete()
c.auto(linuxcnc.AUTO_RUN, 0)
wait_error("offsets not stored")
wait_for(lambda: s.interp_state == linuxcnc.INTERP_IDLE, "the program to stop")
check("T1 wxoffset after the program", tools()[1].wxoffset, 0.2)
c.mode(linuxcnc.MODE_MDI)
c.wait_complete()

print("*** with T3's line fixed and the table reloaded, saving works")
with open("tool.tbl", "w") as f:
    f.write(before.splitlines()[0] + "\nT3 P3 X+1 Z+2 WZ-0.25 ;fixed\n")
c.load_tool_table()
c.wait_complete()
wait_for(lambda: 3 in tools(), "T3 after the reload")
c.mode(linuxcnc.MODE_MDI)
c.wait_complete()
c.mdi("g10 l3 p1 x0.1")
c.wait_complete()
wait_for(lambda: "WX+0.100000" in table(), "the saved edit")
check("T1 wxoffset after the save", tools()[1].wxoffset, 0.1)
print(table())
print("*** all checks passed")
sys.exit(0)
