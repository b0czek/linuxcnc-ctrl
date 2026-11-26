#!/bin/sh
set -eu

test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT
cc -std=gnu11 -Wall -Wextra -Werror -DULAPI -I"$EMC2_HOME/src/hal" -I"$EMC2_HOME/src/rtapi" \
    -I"$EMC2_HOME/src" -I"$EMC2_HOME/src/include" \
    -o "$test_dir/arithmetic" arithmetic.c
"$test_dir/arithmetic"
