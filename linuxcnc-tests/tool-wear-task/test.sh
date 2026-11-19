#!/bin/bash

cp -f tool.tbl.original tool.tbl
rm -f sim.var

../tool-wear-common/make-ini.sh lathe.ini
# M7 reads tool entries out of range through emccanon (remap.py)
sed -i '/^\[RS274NGC\]$/a REMAP = M7 modalgroup=8 python=out_of_range_tools' lathe.ini
printf '\n[PYTHON]\nPATH_PREPEND = .\nTOPLEVEL = toplevel.py\n' >> lathe.ini
exec linuxcnc -r lathe.ini
