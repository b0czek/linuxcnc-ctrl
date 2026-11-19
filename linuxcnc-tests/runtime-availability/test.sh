#!/bin/sh
set -e
python3 ../runtime-availability/make-fixtures.py
linuxcnc -r test.ini
linuxcnc -r initial.ini
