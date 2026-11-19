#!/bin/bash
# Usage: make-ini.sh OUTPUT [ui=PROGRAM] [random=0|1] [halfile=FILE]
set -e
output=$1
shift
ui=./test-ui.py random=0 halfile=LIB:core_sim.hal
for setting in "$@"; do
    case "$setting" in
        ui=*) ui=${setting#ui=} ;;
        random=*) random=${setting#random=} ;;
        halfile=*) halfile=${setting#halfile=} ;;
        *) echo "make-ini.sh: unknown setting $setting" >&2; exit 2 ;;
    esac
done
sed -e "s#^DISPLAY = ./test-ui.py\$#DISPLAY = $ui#" \
    -e "s#^RANDOM_TOOLCHANGER = 0\$#RANDOM_TOOLCHANGER = $random#" \
    -e "s#^HALFILE = LIB:core_sim.hal\$#HALFILE = $halfile#" \
    "$(dirname "$0")/lathe.ini.in" > "$output"
