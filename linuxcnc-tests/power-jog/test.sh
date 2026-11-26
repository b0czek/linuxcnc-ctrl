#!/bin/sh
set -eu
linuxcnc -r test.ini
sed '/\[TRAJ\]/a PLANNER_TYPE = 1' test.ini > scurve.ini
linuxcnc -r scurve.ini
sed 's/LATHE = 1/LATHE = 0/' test.ini > nonlathe.ini
POWER_JOG_NONLATHE=1 linuxcnc -r nonlathe.ini
