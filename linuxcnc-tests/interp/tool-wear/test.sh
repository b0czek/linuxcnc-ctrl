#!/bin/bash
rs274 -g test.ngc -t test.tbl | awk '{$1=""; print}' | sed 's/-0\.0000/0.0000/g'
result=${PIPESTATUS[0]}
if (( result != 0 )); then
    exit "$result"
fi

for invalid_program in invalid-*.ngc; do
    if rs274 -g "$invalid_program" -t test.tbl >/dev/null 2>&1; then
        echo "$invalid_program unexpectedly succeeded" >&2
        exit 1
    fi
done

# M61 under cutter compensation is refused like M6, before anything reaches
# canon: no tool number change and no offset are emitted.
out=$(rs274 -g invalid-m61-comp.ngc -t test.tbl 2>&1)
if ! grep -q "Cannot change tools with cutter radius compensation on" <<<"$out"; then
    echo "invalid-m61-comp.ngc: not refused as a tool change under compensation" >&2
    echo "$out" >&2
    exit 1
fi
if grep -q "CHANGE_TOOL_NUMBER" <<<"$out"; then
    echo "invalid-m61-comp.ngc: M61 changed the tool before failing" >&2
    echo "$out" >&2
    exit 1
fi
