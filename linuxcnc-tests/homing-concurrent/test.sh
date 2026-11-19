#!/bin/sh
set -e
rm -f sim.var sim.var.bak
linuxcnc -r test.ini
