#!/bin/bash -e

# The configuration borrows the remap-io HAL file and tool table and the
# stdglue library from the LinuxCNC tree.
sed "s#@EMC2_HOME@#${EMC2_HOME}#g" test.ini.in > test.ini

for scenario in success failure timeout abort readahead stop pause stop-timeout comp mdi-pause; do
    echo "Running executor remap scenario: ${scenario}"
    EXECUTOR_SCENARIO="${scenario}" linuxcnc -r test.ini
done

# The preview runs the same remaps without task; they must not fail there.
preview=$(rs274 -i test.ini -n 0 -g preview.ngc 2>&1)
# Importing hal there leaves the HAL shared memory behind.
halrun -U >/dev/null 2>&1 || true
echo "$preview"
if grep -q StopIteration <<<"$preview" || ! grep -q STRAIGHT_FEED <<<"$preview"; then
    echo "executor remaps failed in the preview" >&2
    exit 1
fi
echo "executor remap preview ok"

echo "executor remap scenarios complete"
