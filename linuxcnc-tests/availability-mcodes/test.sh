#!/bin/sh
set -e
python3 ../runtime-availability/make-fixtures.py
linuxcnc -r test.ini
AVAILABILITY_NO_FORCE=1 linuxcnc -r no-force.ini
