#!/usr/bin/env python3
"""A table with CRLF line ends, blank lines (CR included) and a
comment-only line loads whole, and saving it works."""

from common import *

start()
loaded = tools()
print("loaded: %s" % sorted(loaded))
if sorted(loaded) != [1, 2, 3]:
    fail("tools %s loaded, expected 1, 2 and 3" % sorted(loaded))
check("T2 zoffset", loaded[2].zoffset, 2.0)
check("T3 zoffset", loaded[3].zoffset, 3.0)
for toolno, comment in ((1, "first"), (3, "third")):
    got = s.toolinfo(toolno)["comment"]
    if got != comment:
        fail("T%d comment %r, expected %r" % (toolno, got, comment))

mdi("g10 l1 p2 z5")
wait_for(lambda: "T2 P2 Z+5.000000" in saved(), "the saved edit")
print(table())
if "\r" in table():
    fail("carriage return saved")
if "T1 P1 Z+1.000000 ;first" not in saved() or "T3 P3 Z+3.000000 ;third" not in saved():
    fail("tools lost from the saved table")
print("*** all checks passed")
sys.exit(0)
