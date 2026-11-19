#!/bin/sh
set -eu
python3 ../runtime-availability/make-fixtures.py
for scenario in ${AVAILABILITY_MCODE_REGRESSION_CASES:-sync step}; do
    AVAILABILITY_REGRESSION_CASE=$scenario linuxcnc -r test.ini
done
