#!/usr/bin/env python3
"""A table file that no longer holds what was loaded or last saved has
changes the table in memory lacks: a save is refused until it is reloaded.
The comparison is of the contents, so an edit is found even when size and
timestamp are the same, and a mere touch or chmod refuses nothing."""

from common import *

import os


def refused(edit_line, zoffset):
    before = table()
    mdi("g10 l1 p%d z%s" % edit_line)
    if table() != before:
        fail("the edited table file was replaced:\n" + table())
    check("T%d zoffset after the refused save" % edit_line[0],
          tools()[edit_line[0]].zoffset, zoffset)
    mdi_mode()


def reload_table():
    c.load_tool_table()
    c.wait_complete()
    mdi_mode()


start()
print("*** saves follow each other: a save does not look like an edit")
mdi("g10 l1 p1 z5")
wait_for(lambda: "T1 P1 Z+5.000000" in saved(), "the first save")
mdi("g10 l1 p1 z6")
wait_for(lambda: "T1 P1 Z+6.000000" in saved(), "the second save")

print("*** a file replaced by an editor is not saved over")
with open("tool.tbl.new", "w") as f:
    f.write("T1 P1 Z+6.000000\nT2 P2 Z+2.000000\nT3 P3 Z+3.000000\n")
os.rename("tool.tbl.new", "tool.tbl")
refused((1, 7), 6.0)
if 3 in tools():
    fail("T3 loaded without a reload")

print("*** reloaded, it is used and saved")
reload_table()
wait_for(lambda: 3 in tools(), "T3 after the reload")
mdi("g10 l1 p3 z4")
wait_for(lambda: "T3 P3 Z+4.000000" in saved(), "the saved edit")

print("*** an edit in place with the same size and time is found")
info = os.stat("tool.tbl")
edited = table().replace("Z+2.000000", "Z+2.500000")
with open("tool.tbl", "w") as f:
    f.write(edited)
os.utime("tool.tbl", ns=(info.st_atime_ns, info.st_mtime_ns))
refused((1, 8), 6.0)
reload_table()
check("T2 zoffset after the reload", tools()[2].zoffset, 2.5)

print("*** a new time or mode alone does not stop saving, and the mode is kept")
os.utime("tool.tbl", (1000000000, 1000000000))
os.chmod("tool.tbl", 0o640)
mdi("g10 l1 p1 z9")
wait_for(lambda: "T1 P1 Z+9.000000" in saved(), "the save after touch and chmod")
if os.stat("tool.tbl").st_mode & 0o777 != 0o640:
    fail("file mode %o" % (os.stat("tool.tbl").st_mode & 0o777))

print("*** a deleted file holds nothing to lose: a save creates it again")
os.unlink("tool.tbl")
mdi("g10 l1 p1 z10")
wait_for(lambda: os.path.exists("tool.tbl") and "T1 P1 Z+10.000000" in saved(),
         "the recreated file")
print(table())
print("*** all checks passed")
sys.exit(0)
