#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: run_gcode_parser_integration <integration-binary>" >&2
  exit 2
fi

binary=$1

# The parser integration fixtures are owned by this native test and remain
# independent from package-owned files.
fixture_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/fixtures" && pwd)
ini="$fixture_dir/config.ini"
program="$fixture_dir/simple_linear.ngc"
operations_program="$fixture_dir/preview_operations.ngc"
cutter_compensation_program="$fixture_dir/cutter_compensation.ngc"
python_remap_program="$fixture_dir/python_remap.ngc"
modal_free_metric_program="$fixture_dir/modal_free_metric.ngc"
rotary_axis_program="$fixture_dir/rotary_axis.ngc"
parameter_file="$fixture_dir/sim_mm.var"
tool_table="$fixture_dir/sim_mm.tbl"
workdir=$(mktemp -d "${TMPDIR:-/tmp}/linuxcnc-gcode-parser.XXXXXX")
trap 'rm -rf "$workdir"' EXIT

# The fixture INI names sim_mm.var relative to the process directory. Keep
# that parameter state in the temporary directory so an integration run never writes
# sim_mm.var or sim_mm.var.bak into the repository.
cp "$ini" "$workdir/included.ini"
printf '#INCLUDE included.ini\n' >"$workdir/machine.ini"
sed -e '/^LINEAR_UNITS[[:space:]]*=/d' \
    -e '/^RS274NGC_STARTUP_CODE[[:space:]]*=/d' \
    "$ini" >"$workdir/missing-linear-units.ini"
# The same machine with an A and a U axis, in lower case to match task's
# case-insensitive reading of COORDINATES.
sed -e 's/^COORDINATES[[:space:]]*=.*/COORDINATES = x y z a u/' \
    "$ini" >"$workdir/xyzau.ini"
cp "$parameter_file" "$workdir/sim_mm.var"
cp "$tool_table" "$workdir/sim_mm.tbl"
cp "$fixture_dir/remap.py" "$workdir/remap.py"
cp "$fixture_dir/toplevel.py" "$workdir/toplevel.py"

cd "$workdir"
EMC2_HOME="${EMC2_HOME:-}" "$binary" "$workdir/machine.ini" "$program" \
    "$operations_program" "$cutter_compensation_program" \
    "$python_remap_program" "$modal_free_metric_program" \
    "$workdir/missing-linear-units.ini" "$workdir/xyzau.ini" \
    "$rotary_axis_program"
