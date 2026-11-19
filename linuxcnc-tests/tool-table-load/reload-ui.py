#!/usr/bin/env python3
"""Reload accepts what startup accepts for a nonrandom changer, where P is
only a label. A reload refused for a file that still matches the table in
memory tells the operator and does not stop saves."""

from common import *

start()
duplicate = "T5 P6" in table()
if duplicate:
    print("*** a refused reload of the unchanged file does not stop saves")
    c.load_tool_table()
    c.wait_complete()
    wait_error("can't load tool table")
else:
    print("*** a pocket label above the changer limit reloads")
    wait_for(lambda: sorted(tools()) == [1, 99999], "T1 and T99999")
    c.load_tool_table()
    c.wait_complete()

mdi_mode()
mdi("g10 l1 p1 z5")
wait_for(lambda: "T1 P1 Z+5.000000" in saved(), "the saved edit")
print(table())
print("*** all checks passed")
sys.exit(0)
