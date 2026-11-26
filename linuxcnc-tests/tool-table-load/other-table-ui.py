#!/usr/bin/env python3
"""The configured table file did not load. Loading another file gives the
machine tools, but says nothing about the configured file: it is still not
saved over until it has been corrected and loaded itself."""

from common import *

start()
wait_error("can't load tool table")
before = table()

print("*** another table loads, the configured file stays protected")
load_table_file("other.tbl")
wait_for(lambda: sorted(tools()) == [8, 9], "T8 and T9 from the other file")
mdi_mode()
mdi("g10 l1 p8 z3")
if table() != before:
    fail("the protected table file was rewritten:\n" + table())
check("T8 zoffset after the refused save", tools()[8].zoffset, 8.0)

print("*** corrected and loaded, the configured file is saved again")
mdi_mode()
with open("tool.tbl", "w") as f:
    f.write("T1 P1 Z+1.000000\nT2 P2 Z+2.000000\n")
c.load_tool_table()
c.wait_complete()
wait_for(lambda: sorted(tools()) == [1, 2], "T1 and T2 after the reload")
mdi_mode()
mdi("g10 l1 p1 z5")
wait_for(lambda: "T1 P1 Z+5.000000" in saved(), "the saved edit")
print(table())
print("*** all checks passed")
sys.exit(0)
