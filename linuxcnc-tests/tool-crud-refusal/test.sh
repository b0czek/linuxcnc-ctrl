#!/bin/bash
set -e
# RIP layout: $HEADERS is $TOPDIR/include; the NML headers are not installed
TOPDIR=$(dirname "$HEADERS")
g++ -I"$HEADERS" -I"$TOPDIR/src" -I"$TOPDIR/src/emc" \
    -I"$TOPDIR/src/emc/nml_intf" -I"$TOPDIR/src/libnml" \
    tool-cmd.cc -L"$LIBDIR" -Wl,-rpath,"$LIBDIR" \
    -lnml -llinuxcnc -o tool-cmd
printf 'T1 P1 Z+1.000000\nT2 P2 Z+2.000000\nT3 P3 Z+3.000000\n' > tool.tbl
rm -f sim.var
../tool-wear-common/make-ini.sh run.ini
linuxcnc -r run.ini
