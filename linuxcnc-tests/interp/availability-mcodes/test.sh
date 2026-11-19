#!/bin/bash
set -e

output_dir=$(mktemp -d)
trap 'rm -rf "$output_dir"' EXIT

check_valid() {
    program=$1
    kind=$2
    selector=$3
    if ! rs274 -n 0 -i test.ini -g "$program" >"$output_dir/canon" 2>"$output_dir/diagnostics"; then
        cat "$output_dir/diagnostics" >&2
        exit 1
    fi
    # rs274 can return zero on interpreter errors. Only its normal startup
    # message is permitted on stderr; also verify every resource operation.
    sed '/^executing$/d; /^[[:space:]]*$/d' "$output_dir/diagnostics" >"$output_dir/errors"
    if test -s "$output_dir/errors"; then
        cat "$output_dir/errors" >&2
        exit 1
    fi
    sed -nE 's/^[[:space:]]*[0-9]+[[:space:]]+N[^[:space:]]+[[:space:]]+(SET_(JOINT|SPINDLE)_AVAILABILITY\(.*\))$/\1/p' \
        "$output_dir/canon" >"$output_dir/actual"
    printf 'SET_%s_AVAILABILITY(%s, 1)\nSET_%s_AVAILABILITY(%s, 0)\n' \
        "$kind" "$selector" "$kind" "$selector" >"$output_dir/expected"
    diff -u "$output_dir/expected" "$output_dir/actual"
}
check_valid valid-joint.ngc JOINT 99
check_valid valid-spindle.ngc SPINDLE 99

check_rejected() {
    program=$1
    expected=$2
    output=$(rs274 -n 0 -i test.ini -g "$program" 2>&1) || :
    case "$output" in
        *"$expected"*) ;;
        *)
            echo "expected $program to report: $expected" >&2
            echo "$output" >&2
            exit 1
            ;;
    esac
}

check_rejected missing-selector.ngc "requires exactly one P joint selector or $ spindle selector"
check_rejected double-selector.ngc "requires exactly one P joint selector or $ spindle selector"
check_rejected fractional-joint.ngc "P selector must be a non-negative integer joint number"
check_rejected group-spindle.ngc '$ selector must be a non-negative integer spindle number'
check_rejected mixed-motion.ngc "must be the only command in its block"
check_rejected mixed-stop.ngc "must be the only command in its block"
check_rejected mixed-feed.ngc "must be the only command in its block"
check_rejected mixed-speed.ngc "must be the only command in its block"
check_rejected mixed-modal.ngc "must be the only command in its block"
check_rejected mixed-tool.ngc "must be the only command in its block"
check_rejected mixed-polar.ngc "Cannot use axis values without a g code that uses them"
check_rejected mixed-parameter.ngc "must be the only command in its block"
check_rejected mixed-word.ngc "must be the only command in its block"
check_rejected bad-joint.ngc "P selector must be a non-negative integer joint number"
check_rejected negative-joint.ngc "P selector must be a non-negative integer joint number"

check_rejected fractional-spindle.ngc '$ selector must be a non-negative integer spindle number'
check_rejected negative-fractional-spindle.ngc '$ selector must be a non-negative integer spindle number'
check_rejected huge-spindle.ngc '$ selector must be a non-negative integer spindle number'

# Legacy remaps accept their own words without built-in resource selectors.
rs274 -n 0 -i remap.ini -g remapped.ngc >"$output_dir/remap" 2>"$output_dir/diagnostics"
sed '/^executing$/d; /^[[:space:]]*$/d' "$output_dir/diagnostics" >"$output_dir/errors"
if test -s "$output_dir/errors"; then
    cat "$output_dir/errors" >&2
    exit 1
fi
grep -q 'MESSAGE(" legacy M54 override")' "$output_dir/remap"
grep -q 'MESSAGE(" legacy M55 override")' "$output_dir/remap"
if grep -q 'SET_.*_AVAILABILITY' "$output_dir/remap"; then
    cat "$output_dir/remap" >&2
    exit 1
fi

# Remaps must not inherit the built-in interpreter synchronization return.
python3 execute-status.py
