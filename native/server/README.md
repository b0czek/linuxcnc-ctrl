# Native LinuxCNC gRPC server foundation

`linuxcnc-grpc-domain` contains transport-neutral C++20 building blocks for
the standalone daemon: serialized command coordination, typed status replay,
bounded position history, exact-width HAL values, secure program workspaces,
and exclusive/coalesced scope frames. These classes do not include Node,
protobuf, or gRPC headers.

With `LINUXCNC_GRPC_BUILD_WIRE=ON`, `linuxcnc-grpc-server` is a real listener:
it registers the generated `linuxcnc.v1` Machine, INI, Program, HAL, and Scope
services, enables standard gRPC health, optionally enables reflection, and
supports plaintext/TLS/mTLS credentials. The NML adapter mechanically maps all
51 command oneof cases, including spindle indices, tool offsets/wear, operator
messages, and immutable-workspace program opens. It publishes the complete
status surface extracted from the pinned LinuxCNC status structures and emits
typed task, trajectory, joint/axis/spindle, I/O, and tool-table deltas. Live
CTest validation launches an isolated pinned-LinuxCNC simulation and exercises
real status, accepted/completed commands, sparse replay, MDI-backed packed
position deltas, workspace upload and rs274 parsing, exact 64-bit HAL values,
retained remote HAL proxies, and exclusive scope ownership. The harness refuses
to start when another LinuxCNC/HAL runtime exists and reclaims only the runtime
and NML resources it created.

`IniService.Read` returns the complete parsed active INI as section/key/value
entries, preserving repeated-key order and resolving includes through LinuxCNC's
parser. The daemon loads this immutable snapshot before binding its listeners.
Clients read once per session and query locally. There are no per-key RPCs,
typed conversion RPCs, or filename parameters. Disk edits require a new session.

## Tool table mutation support

`setTool` and `deleteTool` use LinuxCNC-owned transactional NML commands. They
support file-backed nonrandom tables and LinuxCNC's native random toolchanger
model. For random changers, pockets are ATC-addressable machine positions.
The pinned patched LinuxCNC rejects `[EMCIO]DB_PROGRAM` during IO
initialization; managed tool data must use a file-backed `[EMCIO]TOOL_TABLE`.

## Protobuf WebSocket data plane

Position-history configuration and clearing remain on `MachineService` through
`ConfigurePositionHistory` and `ClearPositionHistory`. Telemetry is published
directly to renderers at `ws://127.0.0.1:50052/v1/position-history` by default;
set `--telemetry-endpoint=HOST:PORT` to change the shared listener. This
read-only telemetry listener is always a plaintext `ws://` endpoint; `--tls`
and `--mtls` secure only the gRPC control plane. A non-loopback telemetry bind
requires `--allow-plaintext-non-loopback`.

The application protocol is server-to-client only. Client data messages close
the connection with WebSocket policy error 1008. Every binary WebSocket
message contains exactly one protobuf message from
`proto/linuxcnc/v1/websocket.proto`; the route determines its concrete type:

- `/v1/position-history` uses `PositionHistoryFrame`.
- `/v1/hal-values/{token}` uses `HalValueFrame`.
- `/v1/scope/{token}` uses `ScopeTelemetryFrame`.
- `/v1/program-preview?workspace_id=…&relative_path=…` uses
  `ProgramPreviewEvent`.

There is no custom header or top-level envelope. Position connections begin
with a replacement followed by deltas. A clear, reconfiguration, generation
change, or retention rollover produces another replacement. Values retain the
stable ten-double X, Y, Z, A, B, C, U, V, W, motion-type layout.

HAL value telemetry uses the same attachment lifecycle as scope telemetry.
Create or update a subscription with `HalService.CreateValueSubscription` and
`UpdateValueSubscription`; both return its stable WebSocket path. At most one
WebSocket may be attached at a time. Configuration changes preserve the socket
and advance its revision; the first frame for each revision is a complete
replacement. A disconnect releases the attachment for 30 seconds, during which
the same path can reconnect. `DeleteValueSubscription` explicitly releases the
subscription.

HAL frames contain slot/value entries. An absent `HalScalar` marks a slot as
temporarily unavailable; `s64`, `u64`, revisions, and sequences remain exact
64-bit integers.

Scope status, configuration, run, stop, and trigger are unary `ScopeService`
RPCs on the shared gRPC control plane. Their response supplies a WebSocket
attachment path; capture and roll frames flow only over the shared telemetry
listener, which owns frame acknowledgement and backpressure.
Position history keeps its acquisition cadence while coalescing mutations into
50 ms delivery windows for viewers. Slow consumers receive coalesced
latest-state deltas rather than every sampled transition. Client WebSocket
messages are forbidden on both telemetry routes.

The HAL service uses the pinned LinuxCNC HAL runtime for topology, exact
scalar reads/writes, signals, and server-owned remote components. Each
`RunComponent` stream creates or authenticates one retained proxy, activates it
with a complete client-owned state, and then exchanges sequenced updates and
sampled deltas. Transport loss, heartbeat timeout, and `DETACH` drive the
managed `<prefix>.online` pin low and apply optional typed disconnect
values without removing pins, parameters, or links. Only authenticated
`DESTROY` or daemon shutdown removes the native component. The scope service
loads `scope_rt` on first use when needed, attaches its sampling function to
the configured thread, and keeps shared-memory polling off the realtime path.
Each upload is one Zstandard-compressed tar archive and creates one immutable
workspace. Compressed chunks are spooled to bounded staging storage, safely
extracted synchronously by libarchive after upload EOF, and atomically published
only after validation succeeds. Errors and cancellation discard the entire
staged revision. One upload is admitted at a time, matching the single-machine
GUI workload without involving the serialized machine-command worker.

LinuxCNC's fixed program prefix is a server-owned symlink. Program Open
atomically exchanges it with a link to the selected immutable workspace; the
previous link and both workspace leases are retained until LinuxCNC accepts
the open, and the exchange is rolled back on failure. Active and interrupted
exchange links are recovered and pinned after daemon restart. Safety commands
therefore never wait for recursive workspace copying.

Workspace count, archive entries, upload metadata, and upload duration are
bounded by `--max-workspaces`, `--max-workspace-entries`,
`--max-upload-metadata`, and `--upload-timeout-seconds`, respectively. Byte
storage remains bounded by `--workspace-quota` and `--total-quota`. The GUI
sets a deadline within the upload timeout. Workspace expiry is fixed at publish
time, persists across restart, defaults to 24 hours, and is capped at 30 days.
The workspace root must be private to the daemon user and must not overlap the
active program path.

With wire generation disabled, CMake builds the transport-neutral domain
library without creating a server executable.

Wire generation is the default and requires the complete pinned LinuxCNC NML,
rs274, HAL, and scope integrations. Pass `-DLINUXCNC_GRPC_BUILD_WIRE=OFF` for
an explicit domain-only build.

## Native code quality

Formatting and static analysis are pinned to Clang 21. Install
`clang-format-21`, `clang-tidy-21`, and `run-clang-tidy-21`, then run the
formatting workflow from the repository root:

```sh
./scripts/native-format.sh format
./scripts/native-format.sh check
```

Clang-Tidy consumes a CMake compilation database, so configure and build the
same feature set that you want to analyze before invoking it. For example, the
domain-only workflow is:

```sh
cmake -S . -B build/native-grpc \
  -DLINUXCNC_GRPC_BUILD_WIRE=OFF \
  -DLINUXCNC_GRPC_ENABLE_NML=OFF \
  -DLINUXCNC_GRPC_BUILD_TESTS=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native-grpc --parallel
./scripts/native-lint.sh build/native-grpc
ctest --test-dir build/native-grpc --output-on-failure
```

For full runtime coverage, configure a second build with
`LINUXCNC_GRPC_ENABLE_NML=ON` and `LINUXCNC_ROOT` set to the built, pinned
LinuxCNC checkout, again exporting the compilation database. The lint wrapper
only selects repository-owned files below `native/server`; generated
protobuf/gRPC sources, the LinuxCNC checkout, system headers, caches, and build
trees remain outside the analysis pipeline.

Wire builds also install `linuxcnc-grpc-health-check`. It calls the standard
gRPC health service at `127.0.0.1:50051` by default, or at the endpoint passed
as its first argument, and exits nonzero unless the server reports `SERVING`.
For TLS, pass `--tls-ca=PATH`; mTLS additionally accepts
`--tls-certificate=PATH` and `--tls-private-key=PATH`. The equivalent
`LINUXCNC_GRPC_TLS_CA`, `LINUXCNC_GRPC_TLS_CERT`, and
`LINUXCNC_GRPC_TLS_KEY` environment variables are read automatically, so the
Docker health check inherits the same client credentials as the container.
Use `--tls-server-name=NAME` or `LINUXCNC_GRPC_TLS_SERVER_NAME` when the
certificate identity differs from the health-check endpoint.

## Program preview WebSocket stream

Program preview is a finite WebSocket stream for any safe uploaded workspace
entry, whether or not LinuxCNC has loaded it. A successful stream contains zero or more
coalesced `progress` events and ordered, nonempty `batch` events, followed by
exactly one terminal `summary`. The summary carries the authoritative extents
and total operation count. An `error` event is terminal for an interpreter
failure.

Each batch contains at most `--gcode-batch-size` operations (128 by default).
The final partial batch is sent before the summary. The server retains at most
two encoded batches ahead of the active write, so a slow client applies
backpressure to the serialized interpreter instead of growing an unbounded
preview in daemon memory. Progress may be coalesced under load; operation
batches are never coalesced or discarded.

G41/G42 and G40 are relayed as ordered
`CUTTER_COMPENSATION_CHANGE` operations. Clients can track the `OFF`, `LEFT`,
or `RIGHT` mode and style the following compensated tool-center path
independently.

Clients should append `batch.operations` as events arrive and use the summary
to finalize camera fitting or other whole-program state. Cancelling or closing
the WebSocket stops interpretation at the next interpreter-step boundary, releases
the workspace lease, and does not emit a success summary.
