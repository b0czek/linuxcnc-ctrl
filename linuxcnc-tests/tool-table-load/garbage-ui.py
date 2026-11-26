#!/usr/bin/env python3
"""A table file with a line that is not a tool line does not load: the
operator is told at startup and the machine has no tools. A table loaded
later lifts the block on saving; a reload of the file that fails keeps the
table in memory but again keeps the file from being saved over. The block
after a failed startup is exercised by random-start-ui.py: without a tool
in the table, no save can be attempted here."""

from common import *

start()
wait_error("can't load tool table")
if tools():
    fail("tools %s loaded from a table that did not load" % sorted(tools()))

print("*** with the line fixed and the table reloaded, saving works")
with open("tool.tbl", "w") as f:
    f.write("T1 P1 Z+1.000000\nT2 P2 Z+2.000000\n")
c.load_tool_table()
c.wait_complete()
wait_for(lambda: sorted(tools()) == [1, 2], "T1 and T2 after the reload")
mdi_mode()
mdi("g10 l1 p1 z5")
wait_for(lambda: "T1 P1 Z+5.000000" in saved(), "the saved edit")

print("*** a reload that fails keeps the table, and the file is not saved over")
# A refused load changes nothing else either: it does not abort the machine,
# so a running spindle keeps running.
mdi("m3 s100")
edited = "T1 P1 Z+5.000000\nT2 P2 Z+2.000000\nT4 P4 Z+4.000000\nnot a tool line\n"
with open("tool.tbl", "w") as f:
    f.write(edited)
c.load_tool_table()
c.wait_complete()
wait_error("can't load tool table")
time.sleep(0.2)
s.poll()
if hal.get_value("spindle.0.speed-out") <= 0 or s.task_state != linuxcnc.STATE_ON:
    fail("the refused table load stopped the machine")
mdi("m5")
mdi_mode()
mdi("g10 l1 p2 z7")
if table() != edited:
    fail("the table file was rewritten:\n" + table())
check("T2 zoffset after the refused save", tools()[2].zoffset, 2.0)
if 4 in tools():
    fail("T4 loaded from a table that did not load")

print("*** fixed and reloaded again, saving works")
with open("tool.tbl", "w") as f:
    f.write(edited.replace("not a tool line\n", ""))
c.load_tool_table()
c.wait_complete()
wait_for(lambda: 4 in tools(), "T4 after the reload")
mdi_mode()
mdi("g10 l1 p2 z7")
wait_for(lambda: "T2 P2 Z+7.000000" in saved(), "the saved edit")
print(table())
print("*** all checks passed")
sys.exit(0)
