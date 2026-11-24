#!/bin/bash -e

# The configuration borrows the remap-io HAL file and tool table and the
# stdglue library from the LinuxCNC tree.
sed "s#@EMC2_HOME@#${EMC2_HOME}#g" test.ini.in > test.ini

for scenario in success failure timeout abort readahead stop pause stop-timeout; do
    echo "Running executor remap scenario: ${scenario}"
    EXECUTOR_SCENARIO="${scenario}" linuxcnc -r test.ini
done

echo "executor remap scenarios complete"
