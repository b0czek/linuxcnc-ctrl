# Native gRPC architecture

`linuxcnc-grpc-server` is the transport boundary between one patched LinuxCNC
instance and remote applications. It owns the NML command, status, and error
channels, rs274 interpretation, HAL access, position history, scope shared
memory, and uploaded-program workspaces.

## Contract ownership

The protobuf files under `proto/` are the canonical wire contract.
`proto/linuxcnc/v1/linuxcnc.proto` is the aggregate entrypoint. Messages use
typed fields and oneofs; removed fields stay reserved and field numbers are
never reused.

Client implementations live outside this repository and consume the versioned
protobuf boundary. A wire change is complete when the canonical schema and
server implementation agree.

## Services and data planes

`MachineService` provides status snapshots and sparse deltas, command
execution, error events, and position-history configuration. A serialized NML
queue orders commands. Cancelling an RPC cancels only its wait, not a command
already accepted by LinuxCNC. The
[status delta boundaries](./status-delta-boundaries.md) define the minimum
atomic values carried by the status stream.

`IniService` returns the parsed active configuration. `ProgramService` accepts
bounded tar.zst uploads and publishes immutable workspaces after validation.
`HalService` provides topology, exact-width scalar access, subscriptions,
signals, metadata, and server-owned remote component proxies. A
`RunComponent` stream attaches to exactly one proxy, but one transport
connection may carry multiple independent streams. Proxies, values, pins,
parameters, and HAL links survive arbitrary disconnects; attachment generation
checks prevent queued work from a prior stream from mutating a reattached
proxy. `ScopeService` exposes unary
scope controls on the shared gRPC control plane and keeps shared-memory polling
outside realtime code. Scope capture and roll data use only WebSocket telemetry.

High-rate, read-only telemetry uses binary WebSocket routes on the shared
telemetry listener:

- `/v1/position-history`
- `/v1/hal-values/{token}`
- `/v1/scope/{token}`
- `/v1/program-preview?workspace_id=...&relative_path=...`

Each frame is a route-specific protobuf from
`proto/linuxcnc/v1/websocket.proto`. The stable position layout is ten
`float64` values in X, Y, Z, A, B, C, U, V, W, motion-type order. HAL `s64`
and `u64` values remain exact integers across the boundary.

## Endpoints and security

The default gRPC endpoint is `127.0.0.1:50051`; telemetry defaults to
`ws://127.0.0.1:50052`. gRPC supports TLS and mutual TLS. Telemetry is always
plaintext WebSocket, and non-loopback plaintext binds require the explicit
unsafe-bind option. The daemon has no machine lease or application
authorization layer; application policy belongs outside this repository.

Subscriber queues and workspace resources are bounded. Status watchers receive
sparse deltas after synchronization, preview batches are bounded, and scope
frames are coalesced for slow clients.
