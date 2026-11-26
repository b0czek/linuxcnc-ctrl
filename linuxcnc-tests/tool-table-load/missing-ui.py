#!/usr/bin/env python3
"""With no table file at startup a save creates one, but a file that appeared
in the meantime is someone's work and is not replaced until it is loaded."""

from common import *

import os

made = "T3 P0 Z+3.000000\nT4 P1 Z+4.000000\n"


def write_made():
    with open("tool.tbl", "w") as f:
        f.write(made)


start()
wait_error("can't load tool table")

print("*** a file made after startup is not saved over")
write_made()
mdi("m61 q0")
wait_error("not saved after the tool change")
if table() != made:
    fail("the new table file was replaced:\n" + table())

print("*** with the file gone again, a save creates it")
os.unlink("tool.tbl")
mdi_mode()
mdi("m61 q0")
wait_for(lambda: os.path.exists("tool.tbl"), "the created table file")
first = os.stat("tool.tbl").st_ino

print("*** the file a save made does not stop the next save")
mdi("m61 q0")
wait_for(lambda: os.stat("tool.tbl").st_ino != first, "the second save")

print("*** a file made now is kept until it is loaded, then used")
write_made()
mdi("m61 q0")
wait_error("not saved after the tool change")
if table() != made:
    fail("the new table file was replaced:\n" + table())
c.load_tool_table()
c.wait_complete()
wait_for(lambda: spindle_tool_is(3), "T3 from the loaded file")
mdi_mode()
mdi("m61 q4")
wait_for(lambda: "T4 P0 Z+4.000000" in saved(), "the saved tool change")
print(table())
print("*** all checks passed")
sys.exit(0)
