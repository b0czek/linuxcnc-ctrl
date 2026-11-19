#!/bin/sh
set -eu

cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-format-truncation \
    -I"$EMC2_HOME/src" -I"$EMC2_HOME/src/rtapi" \
    "$EMC2_HOME/src/rtapi/rtmod_path.c" test.c -o test_rtmod_path
./test_rtmod_path
rm -f test_rtmod_path
