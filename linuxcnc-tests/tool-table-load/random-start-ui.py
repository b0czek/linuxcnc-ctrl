#!/usr/bin/env python3
"""A random changer whose table did not load at startup: a tool change saves
the table, and that save must not replace the file the operator still has
to correct. Once corrected and reloaded, the file says which tool is in the
spindle: status and the HAL pin follow it, and tool changes save again."""

from common import *

start()
wait_error("can't load tool table")
before = table()
if not spindle_tool_is(0):
    fail("a spindle tool before any table loaded: T%d" % s.tool_in_spindle)

print("*** M61 cannot save over the file that did not load")
mdi("m61 q0")
wait_error("not saved after the tool change")
if table() != before:
    fail("the table file was rewritten:\n" + table())

print("*** the corrected table says T7 is in the spindle")
with open("tool.tbl", "w") as f:
    f.write("T7 P0 Z+7.000000\nT2 P1 Z+2.000000\n")
c.load_tool_table()
c.wait_complete()
wait_for(lambda: spindle_tool_is(7), "T7 in status, pocket 0 and the HAL pin")

print("*** tool changes save again")
mdi_mode()
mdi("m61 q2")
wait_for(lambda: spindle_tool_is(2), "T2 in the spindle")
wait_for(lambda: "T2 P0 Z+2.000000" in saved() and "T7 P1 Z+7.000000" in saved(),
         "the saved tool change")
print(table())
print("*** all checks passed")
sys.exit(0)
