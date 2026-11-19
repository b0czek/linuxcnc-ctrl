#!/bin/bash
set -e
cp -f tool.tbl.original tool.tbl
rm -f sim.var

# run 1: load T1, change its wear, shut down with it in the spindle
../tool-wear-common/make-ini.sh run1.ini ui=./run1.py random=1
linuxcnc -r run1.ini
cat tool.tbl

# run 2: T1 is still in the spindle with its stored wear, but no offset is
# applied until G43, M6 or M61
../tool-wear-common/make-ini.sh run2.ini ui=./run2.py random=1
linuxcnc -r run2.ini
