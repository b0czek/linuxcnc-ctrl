#!/bin/bash

rs274 -g test.ngc | awk '{$1=""; print}'
result=${PIPESTATUS[0]}
if (( result != 0 )); then
    exit "$result"
fi

# A G76.1 pass must not feed past either end of the programmed Z range, even
# on the mirrored passes of alternating infeed.
range_output=$(rs274 -g alternating-z-range.ngc 2>&1) || {
    echo "alternating-z-range.ngc failed" >&2
    echo "$range_output" >&2
    exit 1
}
if ! LC_ALL=C awk -F'[(),]' '
    /COMMENT\("range / {
        gsub(/"/, "", $2); split($2, r, " "); a = r[2] + 0; b = r[3] + 0
        lo = a < b ? a : b; hi = a < b ? b : a; cycles++; next
    }
    /STRAIGHT_FEED\(/ {
        feeds++
        z = $4 + 0
        if (z < lo - 1e-4 || z > hi + 1e-4) {
            printf "feed to Z%s outside Z%s..Z%s\n", $4, lo, hi > "/dev/stderr"
            bad = 1
        }
    }
    END { exit bad || cycles != 6 || feeds == 0 }' <<< "$range_output"; then
    echo "alternating-z-range.ngc: a G76.1 feed leaves the programmed Z range" >&2
    exit 1
fi

# Each invalid program must be rejected with its own message.
declare -A expected_error=(
    [invalid-both-modes.ngc]="Two g codes used from same modal group"
    [invalid-d.ngc]="D word with no G41, G41.1, G42, G42.1, G71, G71.1, G71.2 G73, G83 or G96 to use it"
    [invalid-y.ngc]="Cannot move Y axis with G76"
    [invalid-zero-z.ngc]="In G76, a tapered drive line must have nonzero Z length"
)

for invalid_program in invalid-*.ngc; do
    message=${expected_error[$invalid_program]}
    if [[ -z $message ]]; then
        echo "$invalid_program has no expected error message" >&2
        exit 1
    fi
    output=$(rs274 -g "$invalid_program" 2>&1)
    if ! grep -qxF "$message" <<< "$output"; then
        echo "$invalid_program was not rejected with: $message" >&2
        echo "$output" >&2
        exit 1
    fi
done
