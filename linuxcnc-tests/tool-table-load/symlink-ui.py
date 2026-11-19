#!/usr/bin/env python3
"""tool.tbl is a link to a link to the real table in another directory.
Saving writes the real file and leaves every link in place, also when the
real file is missing."""

from common import *

import os

REAL = "linked/real.tbl"


def check_layout():
    if os.readlink("tool.tbl") != "linked/alias.tbl":
        fail("tool.tbl is no longer the link it was")
    if os.readlink("linked/alias.tbl") != "real.tbl":
        fail("linked/alias.tbl is no longer the link it was")
    if os.path.islink(REAL) or not os.path.isfile(REAL):
        fail("the real table is not a regular file")
    left = [n for d in (".", "linked") for n in os.listdir(d) if ".tmp." in n]
    if left:
        fail("temporary files left: %s" % left)


def real():
    with open(REAL) as f:
        return " ".join(f.read().split())


start()
print("*** a save goes to the real file through the links")
mdi("g10 l1 p1 z5")
wait_for(lambda: "T1 P1 Z+5.000000" in real(), "the saved edit")
check_layout()
if os.stat(REAL).st_mode & 0o777 != 0o640:
    fail("file mode %o" % (os.stat(REAL).st_mode & 0o777))

print("*** the link still names the table: reload and save again")
c.load_tool_table()
c.wait_complete()
mdi_mode()
check("T1 zoffset after the reload", tools()[1].zoffset, 5.0)
mdi("g10 l1 p2 z7")
wait_for(lambda: "T2 P2 Z+7.000000" in real(), "the second save")
check_layout()

print("*** with the real file gone, a save creates it behind the links")
os.unlink(REAL)
mdi("g10 l1 p1 z9")
wait_for(lambda: os.path.exists(REAL) and "T1 P1 Z+9.000000" in real(),
         "the recreated file")
check_layout()
print(real())
print("*** all checks passed")
sys.exit(0)
