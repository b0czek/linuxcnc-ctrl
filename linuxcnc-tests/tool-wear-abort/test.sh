#!/bin/bash

cp -f tool.tbl.original tool.tbl
rm -f sim.var

../tool-wear-common/make-ini.sh lathe.ini halfile=core_sim_handshake.hal
exec linuxcnc -r lathe.ini
