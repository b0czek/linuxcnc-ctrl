#!/bin/bash -e
# Shared driver for the resumable-stop/<group> runtests tests.
#
# Usage, from a group's test.sh:  exec ../run-scenarios.sh scenarios
# where the file "scenarios" lists one scenario of test-ui.py per line. An
# entry trapezoidal:NAME runs NAME with PLANNER_TYPE = 0 instead of the
# jerk-limited planner test.ini selects. The group's checkresult
# (check-result.sh) requires every entry to report success.
#
# Every scenario runs in a fresh LinuxCNC session started from a private copy
# of the shared configuration, so groups can run in parallel (also in one
# directory tree) without sharing sim.var or other files LinuxCNC writes next
# to its INI file.

set -o pipefail

shared=$(cd "$(dirname "$0")" && pwd)

if nm -D -u "$EMC2_HOME/rtlib/motmod.so" | grep -q '[[:space:]]tpStop$'; then
    echo "motmod unexpectedly requires the nonstandard tpStop symbol" >&2
    exit 1
fi

if nm -D -u "$EMC2_HOME/rtlib/motmod.so" | grep -q '[[:space:]]tpStep$'; then
    echo "motmod unexpectedly requires the nonstandard tpStep symbol" >&2
    exit 1
fi

[ $# -eq 1 ] && [ -f "$1" ] || { echo "usage: run-scenarios.sh SCENARIO-FILE" >&2; exit 1; }
mapfile -t items < <(grep -v '^[[:space:]]*\(#\|$\)' "$1")
[ ${#items[@]} -gt 0 ] || { echo "run-scenarios.sh: no scenarios in $1" >&2; exit 1; }

config=$(mktemp -d)
scenario_log=$(mktemp)
trap 'rm -rf "$config" "$scenario_log"' EXIT
cp "$shared"/test-ui.py "$shared"/test.ini "$shared"/core_sim.hal \
    "$shared"/tool.tbl "$shared"/*.ngc "$config"/
sed 's/^PLANNER_TYPE = 1$/PLANNER_TYPE = 0/' "$config/test.ini" \
    > "$config/test-trapezoidal.ini"
grep -q '^PLANNER_TYPE = 0$' "$config/test-trapezoidal.ini"

for item in "${items[@]}"; do
    case "$item" in
        trapezoidal:*) ini=test-trapezoidal.ini; scenario=${item#trapezoidal:} ;;
        *) ini=test.ini; scenario=$item ;;
    esac
    echo "Running resumable-stop scenario: $scenario ($ini)"
    : > "$scenario_log"
    (cd "$config" && RESUMABLE_STOP_SCENARIO="$scenario" linuxcnc -r "$ini") 2>&1 |
        tee "$scenario_log"
    if grep -Eq 'ruckig_plan_(position|velocity): invalid limits|Ruckig (velocity control )?planning failed' "$scenario_log"; then
        echo "$scenario emitted a Ruckig invalid-limit or planning failure" >&2
        exit 1
    fi
    if ! grep -qxF "resumable stop scenario ok: $scenario" "$scenario_log"; then
        echo "$item did not report success" >&2
        exit 1
    fi
    echo "resumable stop scenario passed: $item"
done
echo "resumable stop scenarios complete"
