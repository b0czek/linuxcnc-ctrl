#!/usr/bin/env python3
"""Native tool wear through task, on a metric lathe.

The tool table gives a front tool (T1) and a back tool (T2) X and Z wear.
Built-in M6 and M61 apply geometry plus wear; G10 L3 edits stored wear
without touching the applied offset until G43; the table file keeps the
W* words and reads back the same. Checked from both sides: interpreter
parameters (#5401-#5409 stored geometry, #5430-#5438 stored wear,
#5081-#5089 applied offset) and the Python status (tool_table, toolinfo,
tool_offset).
"""

import linuxcnc
import linuxcnc_util

import math
import os
import sys
import time

sys.stdout = os.fdopen(sys.stdout.fileno(), 'w')

c = linuxcnc.command()
e = linuxcnc.error_channel()
s = linuxcnc.stat()
l = linuxcnc_util.LinuxCNC(command=c, status=s, error=e)

TOOL_TABLE = "tool.tbl"
EPS = 1e-6


def fail(msg):
    print("ERROR: " + msg)
    sys.exit(1)


def check(what, got, expected):
    print("%s = %.6f (expecting %.6f)" % (what, got, expected))
    if math.fabs(got - expected) > EPS:
        fail("%s = %f, expected %f" % (what, got, expected))


def mdi(cmd):
    print("mdi: " + cmd)
    c.mdi(cmd)
    if c.wait_complete(10) == -1:
        fail("timeout running '%s'" % cmd)
    # The status the checks below read must be from after the command.
    s.poll()


def param(n):
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


def check_params(stored_x, stored_z, wear_x, wear_z, applied_x, applied_z):
    check("#5401 stored geometry X", param(5401), stored_x)
    check("#5403 stored geometry Z", param(5403), stored_z)
    check("#5430 stored wear X", param(5430), wear_x)
    check("#5432 stored wear Z", param(5432), wear_z)
    check("#5081 applied X", param(5081), applied_x)
    check("#5083 applied Z", param(5083), applied_z)
    s.poll()
    check("stat.tool_offset X", s.tool_offset[0], applied_x)
    check("stat.tool_offset Z", s.tool_offset[2], applied_z)


def reload_table():
    """Reload the table file and wait until task has done it."""
    c.load_tool_table()
    c.wait_complete(10)
    start = time.time()
    while True:
        s.poll()
        if s.state == linuxcnc.RCS_DONE and s.exec_state == linuxcnc.EXEC_DONE:
            return
        if time.time() - start > 10:
            fail("tool table reload did not complete")
        time.sleep(0.01)


def machine_at(x, z, machine_x, machine_z):
    """G0 to program X, Z and check where the machine was commanded."""
    mdi("g0 x%s z%s" % (x, z))
    s.poll()
    check("machine X after G0 X%s" % x, s.position[0], machine_x)
    check("machine Z after G0 Z%s" % z, s.position[2], machine_z)


def check_tool(toolno, x, z, wx, wz):
    s.poll()
    # index 0 is the spindle mirror of the loaded tool
    entry = [t for t in s.tool_table[1:] if t.id == toolno]
    if len(entry) != 1:
        fail("tool %d not in stat.tool_table" % toolno)
    t = entry[0]
    check("stat.tool_table T%d xoffset" % toolno, t.xoffset, x)
    check("stat.tool_table T%d zoffset" % toolno, t.zoffset, z)
    check("stat.tool_table T%d wxoffset" % toolno, t.wxoffset, wx)
    check("stat.tool_table T%d wzoffset" % toolno, t.wzoffset, wz)
    info = s.toolinfo(toolno)
    check("stat.toolinfo(%d) wxoffset" % toolno, info["wxoffset"], wx)
    check("stat.toolinfo(%d) wzoffset" % toolno, info["wzoffset"], wz)


def table_line(toolno):
    with open(TOOL_TABLE) as f:
        for line in f:
            if line.split() and line.split()[0] == "T%d" % toolno:
                return line.strip()
    fail("T%d missing from %s" % (toolno, TOOL_TABLE))


def check_table_words(toolno, **words):
    """The tool's line in the table file, as numbers: a word the file
    leaves out (zero values may be omitted) counts as zero."""
    line = table_line(toolno)
    print("%s: %s" % (TOOL_TABLE, line))
    values = {}
    for token in line.split(";")[0].split()[2:]:
        letters = token.rstrip("+-.0123456789")
        values[letters] = float(token[len(letters):])
    for word, expected in words.items():
        check("%s T%d %s" % (TOOL_TABLE, toolno, word), values.get(word, 0.0),
              expected)


l.wait_for_linuxcnc_startup()
c.state(linuxcnc.STATE_ESTOP_RESET)
c.state(linuxcnc.STATE_ON)
c.mode(linuxcnc.MODE_MDI)
c.wait_complete()

print("*** the table as loaded")
check_tool(1, 5.0, 10.0, 0.2, -0.1)
check_tool(2, -2.5, 20.0, -0.3, 0.15)

print("*** T1 M6 applies T1's geometry plus wear (front tool)")
mdi("t1 m6")
l.wait_for_tool_in_spindle(1)
check_params(5.0, 10.0, 0.2, -0.1, 5.2, 9.9)
check("stat.tool_table[0] wxoffset", s.tool_table[0].wxoffset, 0.2)
check("stat.tool_table[0] wzoffset", s.tool_table[0].wzoffset, -0.1)
machine_at(0, 0, 5.2, 9.9)

print("*** G10 L3 edits stored wear; the applied offset waits for G43")
mdi("g10 l3 p1 x0.3 z-0.05")
check_params(5.0, 10.0, 0.3, -0.05, 5.2, 9.9)
check_tool(1, 5.0, 10.0, 0.3, -0.05)
check_table_words(1, X=5.0, Z=10.0, WX=0.3, WZ=-0.05)

mdi("g43")
check_params(5.0, 10.0, 0.3, -0.05, 5.3, 9.95)

print("*** G10 L3 keeps the axes it is not given")
mdi("g10 l3 p1 z0")
check_tool(1, 5.0, 10.0, 0.3, 0.0)
mdi("g43")
check_params(5.0, 10.0, 0.3, 0.0, 5.3, 10.0)

print("*** G7: G10 L3 X is a diameter")
mdi("g7")
mdi("g10 l3 p2 x0.4")
mdi("g8")
check_tool(2, -2.5, 20.0, 0.2, 0.15)

print("*** M61 Q2 applies T2's geometry plus wear (back tool)")
mdi("m61 q2")
l.wait_for_tool_in_spindle(2)
check_params(-2.5, 20.0, 0.2, 0.15, -2.3, 20.15)
machine_at(0, 0, -2.3, 20.15)

print("*** G43 H1 applies T1's geometry plus wear with T2 loaded")
mdi("g43 h1")
check_params(-2.5, 20.0, 0.2, 0.15, 5.3, 10.0)

print("*** G49 cancels the applied offset, stored values stay")
mdi("g49")
check_params(-2.5, 20.0, 0.2, 0.15, 0.0, 0.0)

print("*** a G49 in the M6 block wins")
mdi("t1 m6 g49")
l.wait_for_tool_in_spindle(1)
check_params(5.0, 10.0, 0.3, 0.0, 0.0, 0.0)

print("*** an explicit offset in the change block wins")
mdi("t2 m6 g43 h1")
l.wait_for_tool_in_spindle(2)
check_params(-2.5, 20.0, 0.2, 0.15, 5.3, 10.0)
machine_at(0, 0, 5.3, 10.0)
mdi("m61 q1 g49")
l.wait_for_tool_in_spindle(1)
check_params(5.0, 10.0, 0.3, 0.0, 0.0, 0.0)
mdi("g43")
mdi("m61 q0")
l.wait_for_tool_in_spindle(0)
check_params(0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
mdi("t1 m6 g49")
l.wait_for_tool_in_spindle(1)

print("*** T0 M6 clears everything")
mdi("t0 m6")
l.wait_for_tool_in_spindle(0)
check_params(0.0, 0.0, 0.0, 0.0, 0.0, 0.0)

print("*** the table file round-trips the W* words")
check_table_words(1, X=5.0, Z=10.0, WX=0.3, WZ=0.0)
check_table_words(2, X=-2.5, Z=20.0, WX=0.2, WZ=0.15, Q=2)
reload_table()
check_tool(1, 5.0, 10.0, 0.3, 0.0)
check_tool(2, -2.5, 20.0, 0.2, 0.15)
mdi("t2 m6")
l.wait_for_tool_in_spindle(2)
check_params(-2.5, 20.0, 0.2, 0.15, -2.3, 20.15)

print("*** an inch program on the metric lathe")
# offsets and #5081-#5089 stay in machine units (mm); positions and
# G10 L3 values are inches
mdi("g20")
mdi("t1 m6")
l.wait_for_tool_in_spindle(1)
check_params(5.0, 10.0, 0.3, 0.0, 5.3, 10.0)
machine_at(1, 1, 25.4 + 5.3, 25.4 + 10.0)
mdi("g10 l3 p1 x0.01 z-0.002")
check_tool(1, 5.0, 10.0, 0.254, -0.0508)
check_table_words(1, X=5.0, Z=10.0, WX=0.254, WZ=-0.0508)
mdi("g43")
check_params(5.0, 10.0, 0.254, -0.0508, 5.254, 9.9492)
machine_at(1, 1, 25.4 + 5.254, 25.4 + 9.9492)
mdi("g21")
mdi("g10 l3 p1 x0.3 z0")
mdi("t2 m6")
l.wait_for_tool_in_spindle(2)
check_params(-2.5, 20.0, 0.2, 0.15, -2.3, 20.15)

print("*** a tool line too long to write is refused, the file kept")
before = table_line(1)
# each %+f of 1e100 is over 100 characters: D, X and Z fit in a line,
# adding WX and WZ does not
mdi("#1 = [10 ** 100]")
mdi("g10 l1 p1 x#1 z#1 r#1")
long_line = table_line(1)
print("%s: T1 line is %d characters" % (TOOL_TABLE, len(long_line)))
if long_line == before or len(long_line) > 510:
    fail("expected a long but valid T1 line, got %d characters" % len(long_line))
c.mdi("g10 l3 p1 x#1 z#1")
c.wait_complete(10)
s.poll()
if table_line(1) != long_line:
    fail("T1 line changed although it no longer fits: %s" % table_line(1))
print("%s: T1 line unchanged" % TOOL_TABLE)
# what could not be stored is not kept in memory either
s.poll()
t1 = [t for t in s.tool_table[1:] if t.id == 1][0]
check("stat.tool_table T1 wxoffset after the refused save", t1.wxoffset, 0.3)
check("stat.tool_table T1 wzoffset after the refused save", t1.wzoffset, 0.0)
check("stat.toolinfo(1) wxoffset after the refused save",
      s.toolinfo(1)["wxoffset"], 0.3)
check_table_words(2, X=-2.5, Z=20.0, WX=0.2, WZ=0.15, Q=2)
mdi("g10 l1 p1 x5 z10 r0.4")
mdi("g10 l3 p1 x0.3 z0")
check_table_words(1, X=5.0, Z=10.0, WX=0.3, WZ=0.0, D=0.8)
reload_table()
check_tool(1, 5.0, 10.0, 0.3, 0.0)

print("*** tool entries out of range are empty, wear included")
mdi("m7")
expected = "toolno -1 wear %r" % ([0.0] * 9,)
seen = 0
start = time.time()
while seen < 2 and time.time() - start < 10:
    err = e.poll()
    if err is None:
        time.sleep(0.01)
        continue
    print(err[1])
    if err[1].startswith("out-of-range"):
        seen += 1
        if not err[1].endswith(expected):
            fail("out-of-range tool entry is not empty: %s" % err[1])
if seen != 2:
    fail("no out-of-range report from M7")

print("*** all checks passed")
sys.exit(0)
