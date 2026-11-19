#!/bin/sh
# Shared checkresult for the resumable-stop/<group> tests.
# Usage: check-result.sh RESULT SCENARIO-FILE COUNT
# Passes only if the group ran exactly COUNT scenarios, COUNT is the number
# of entries in SCENARIO-FILE, and every entry reported success.
set -e
result=$1 list=$2 count=$3
entries=$(grep -cv '^[[:space:]]*\(#\|$\)' "$list")
test "$entries" -eq "$count"
test "$(grep -c '^resumable stop scenario ok:' "$result")" -eq "$count"
test "$(grep -c '^resumable stop scenario passed:' "$result")" -eq "$count"
grep -v '^[[:space:]]*\(#\|$\)' "$list" | while read -r item; do
    grep -qxF "resumable stop scenario passed: $item" "$result"
done
grep -q '^resumable stop scenarios complete$' "$result"
