# linuxcnc-ctrl simulator

`ghcr.io/b0czek/linuxcnc-simulator` contains the pinned LinuxCNC build with
the `linuxcnc-ctrl` repository's complete patch series and
`linuxcnc-grpc-server`. It is a
simulation environment only; do not use it to control physical hardware.

## Configuration

Mount a complete LinuxCNC configuration directory at `/config`. The default
INI is `/config/linuxcnc.ini`; set `LINUXCNC_INI` to select another path.
The image does not rewrite or restrict the configuration.

To start the embedded server after LinuxCNC has initialized NML and HAL, use:

```ini
[DISPLAY]
DISPLAY = linuxcnc-grpc-display
PROGRAM_PREFIX = /var/lib/linuxcnc-grpc/active-program
```

Additional display arguments are passed to `linuxcnc-grpc-server`, so daemon
options such as TLS, reflection, quotas, and polling periods remain available:

```ini
DISPLAY = linuxcnc-grpc-display --reflection --workspace-ttl-seconds=3600
```

The wrapper derives the NML path from LinuxCNC and resolves
`PROGRAM_PREFIX` relative to the INI when necessary. The configuration mount
must be writable if LinuxCNC stores its parameter file, tool table, or active
programs there. Persistent gRPC workspaces live under
`/var/lib/linuxcnc-grpc`.

## Run with Compose

```sh
LINUXCNC_CONFIG_DIR=/absolute/path/to/machine docker compose up
```

Optional variables are:

- `LINUXCNC_SIMULATOR_IMAGE` — image name/tag; defaults to the published
  `edge` image.
- `LINUXCNC_INI_FILE` — filename below `/config`; defaults to `linuxcnc.ini`.
- `LINUXCNC_GRPC_PORT` — host gRPC port; defaults to `50051`.
- `LINUXCNC_TELEMETRY_PORT` — host WebSocket port; defaults to `50052`.
- `LINUXCNC_UID` / `LINUXCNC_GID` — numeric runtime identity; both default
  to `1000`. Set these to the owner of the mounted configuration when it must
  be writable (for example, `LINUXCNC_UID=$(id -u) LINUXCNC_GID=$(id -g)`).

Compose grants `SYS_NICE`, `IPC_LOCK`, and `IPC_OWNER`, configures realtime
and memory locking limits, and uses a private 256 MiB shared-memory segment.
`IPC_OWNER` lets LinuxCNC's setuid realtime helper reopen segments created by
the unprivileged runtime user. The gRPC health service drives the container
health check.

To build locally instead of pulling the published image:

```sh
LINUXCNC_CONFIG_DIR=/absolute/path/to/machine docker compose build
LINUXCNC_CONFIG_DIR=/absolute/path/to/machine docker compose up
```

Run these commands from the `linuxcnc-ctrl` checkout.
Compilation uses all processors available to the Docker builder by default. Set
the `BUILD_JOBS` build argument to cap parallelism when memory is constrained;
for example, `docker compose build --build-arg BUILD_JOBS=4`.

## Endpoints and image tags

- gRPC: `localhost:50051`
- Position telemetry: `ws://localhost:50052/v1/position-history`
- HAL values: gRPC-created paths below `ws://localhost:50052/v1/hal-values/`
- Program preview: `ws://localhost:50052/v1/program-preview?workspace_id=…&relative_path=…`

- `edge`: current `main`
- `X.Y.Z`, `X.Y`, and `latest`: a `vX.Y.Z` repository tag

Both `linux/amd64` and `linux/arm64` are published.

Plaintext listeners are the default. TLS and mTLS affect gRPC only; mount the
certificate files and pass the existing server options through `DISPLAY`.

## Full-stack acceptance test

The disposable `test` target contains the native full-stack acceptance client:

```sh
docker build --target test -t linuxcnc-simulator:test .
./docker/acceptance-test.sh linuxcnc-simulator:test
```

The acceptance test copies its fixture to a temporary writable directory, exercises
the live machine, program, HAL, scope, and telemetry APIs, then checks graceful
container shutdown.
