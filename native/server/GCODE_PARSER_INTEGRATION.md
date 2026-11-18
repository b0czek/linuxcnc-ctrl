# Native rs274 parser integration

The extracted parser is deliberately independent of Node, protobuf,
and gRPC. Add these sources to the daemon's native target:

```text
native/server/src/gcode/parser.cc
native/server/src/gcode/canon_preview.cc
native/server/src/gcode/python_modules.cc
native/server/src/gcode/recording_canon.cc
native/server/src/gcode/recording_canon.hpp
native/server/include/linuxcnc_grpc/gcode/parser.hpp
native/server/include/linuxcnc_grpc/gcode/canon_preview.hpp
native/server/include/linuxcnc_grpc/gcode/operation_types.hpp
```

Compile as C++20 with `-DULAPI`. The LinuxCNC include roots required by the
implementation files are:

```text
<linuxcnc>/include
<linuxcnc>/src
<linuxcnc>/src/emc
<linuxcnc>/src/emc/rs274ngc
<linuxcnc>/src/emc/nml_intf
<linuxcnc>/src/emc/tooldata
<linuxcnc>/src/emc/motion
<linuxcnc>/src/libnml/buffer
<linuxcnc>/src/libnml/cms
<linuxcnc>/src/libnml/linklist
<linuxcnc>/src/libnml/nml
<linuxcnc>/src/libnml/os_intf
<linuxcnc>/src/libnml/posemath
<linuxcnc>/src/libnml/rcs
<linuxcnc>/src/rtapi
```

Add the Python development include directory reported by
`python3-config --includes` and link the matching Python runtime. The small
`gcode_python_modules.cc` adapter registers LinuxCNC's `interpreter` and
`emccanon` built-ins for remap support. Python is not part of the preview
backend.

`recording_canon.cc` is a separate link-selected implementation of LinuxCNC's
global CANON API, like `gcodemodule.cc` (Axis) and `saicanon.cc`
(standalone interpretation). A parse-scoped `recording::Session` owns a queue
of transport-neutral events. The daemon drains that queue only after an
interpreter step and translates the events into its native operation model.
There is no virtual dispatcher in Axis, no nullable check in every motion
function, and no callback from canonical code into gRPC-facing code.

Python and NGC remaps still use the same interpreter pipeline. `self.execute()`
re-enters rs274, while direct `emccanon` calls resolve to the selected global
CANON implementation; both are therefore recorded automatically. The backend
defines `_task = 0`, preserving LinuxCNC's established preview behavior.

LinuxCNC resolves G41/G42 cutter compensation before canonical output. Motion
operations therefore contain the compensated tool-center path, rather than a
separate cutter-compensation state operation or the original part contour.

Link the LinuxCNC libraries `rs274`, `linuxcncini`, and `tooldata` (plus the
normal transitive Python/plugin and `dl` libraries supplied by the pinned
LinuxCNC build). The parser itself has no transport libraries. The recording
canon translation unit must be linked beside the parser because it supplies
every canonical callback required by `rs274ngc`.

`ParseOptions::ini_path` is required. It is passed to `InterpBase::ini_load`
and installed as LinuxCNC's serialized `INI_FILE_NAME` environment convention,
which is how interpreter initialization discovers `[PYTHON]` and `REMAP`
settings. Callers should use the daemon's configured INI for every workspace
parse. `batch_size` bounds each `OperationBatch` delivered to
`on_batch`. Callers request cancellation through the `std::stop_source`
associated with `ParseOptions::stop_token`; the parser stops at the next
read/execute boundary. A callback never performs network I/O from
canonical/realtime code: it runs on the parser worker thread after the
interpreter step.

The optional `gcode_parser_integration` test checks complete batching, extents,
cutter-compensated geometry, Python remap motion through both `self.execute()`
and direct `emccanon`, preview `_task` behavior, and cancellation. Use
`tests/run_gcode_parser_integration.sh <binary>`; the runner owns its native
fixtures and copies the INI parameter/tool files into a temporary working
directory so parser state cannot create files in the repository.
