#!/bin/bash
set -e
python3 make-table.py > tool.tbl
rm -f sim.var
../tool-wear-common/make-ini.sh lathe.ini
linuxcnc -r lathe.ini

# a random toolchanger: M6 cannot save the table and fails
python3 make-table.py random > tool.tbl
rm -f sim.var
../tool-wear-common/make-ini.sh random.ini ui=./random-ui.py random=1
linuxcnc -r random.ini
