#!/bin/bash
# Canon output, then the loader's refusals (stderr), in a stable order.
rs274 -g test.ngc -t test.tbl 2> err.txt | awk '{$1=""; print}'
result=${PIPESTATUS[0]}
grep "longer than" err.txt
rm -f err.txt
if (( result != 0 )); then
    exit "$result"
fi

# a refused line's tool is not in the table at all
for missing in missing-t*.ngc; do
    if rs274 -g "$missing" -t test.tbl >/dev/null 2>&1; then
        echo "$missing: tool loaded from an over-long line" >&2
        exit 1
    fi
done
