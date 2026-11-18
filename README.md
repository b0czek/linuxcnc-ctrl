# linuxcnc-ctrl

`linuxcnc-ctrl` is the open-source LinuxCNC integration used by
[ctrlcnc.xyz](https://ctrlcnc.xyz). It owns
the maintained LinuxCNC patch stack, the canonical `linuxcnc.v1` protobuf
contract, the C++ gRPC and telemetry server, simulator containers, and native
tests.

The server attaches to one patched LinuxCNC instance. It exposes machine,
INI, program, HAL, and scope services over gRPC, plus binary WebSocket streams
for position history, selected HAL values, and G-code preview. See the
[architecture document](./docs/native-grpc-architecture.md).

## Scope

LinuxCNC runs here as a headless backend, and clients use the APIs this
repository provides. LinuxCNC's own GUIs and GUI tools (AXIS, Touchy, gmoccapy,
QtDragon, `tooledit` and similar) are out of scope: we do not add features to
them, fix them, test them, or keep patches compatible with them. `halui` is a
headless HAL interface and stays in scope.

The older remote interfaces (`linuxcncrsh`, `halrmt`, `schedrmt`) and tool
database programs (`[EMCIO]DB_PROGRAM`) are out of scope in the same way:
clients use this repository's gRPC APIs, and tool data lives in the file-backed
tool table.

Out of scope means we do not care about these features. It does not mean we
remove them: patches neither fix them nor strip them out, and code a patch
already touches stays as it is unless other work requires a change.

## Repository layout

- `proto/`: canonical versioned protobuf wire contract and its existing license.
- `native/server/`: C++20 domain library, daemon, health check, and native tests.
- `linuxcnc-patches/`: maintained patch series and pinned upstream baseline.
- `docker/`, `Dockerfile`, and `compose.yaml`: headless simulator container.
- `scripts/`: native formatting and clang-tidy wrappers.

## Native build

Build and test the transport-neutral contract without a LinuxCNC runtime:

```sh
cmake -S . -B build/native-grpc \
  -DLINUXCNC_GRPC_BUILD_WIRE=OFF \
  -DLINUXCNC_GRPC_ENABLE_NML=OFF \
  -DLINUXCNC_GRPC_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native-grpc --parallel
ctest --test-dir build/native-grpc --output-on-failure
```

A full server build requires LinuxCNC built from the pinned revision with the
complete patch series applied:

```sh
./linuxcnc-patches/apply.sh /path/to/linuxcnc
cmake -S . -B build/native-grpc-linuxcnc \
  -DLINUXCNC_ROOT=/path/to/linuxcnc \
  -DLINUXCNC_GRPC_BUILD_WIRE=ON \
  -DLINUXCNC_GRPC_BUILD_TESTS=ON \
  -DLINUXCNC_GRPC_ENABLE_NML=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native-grpc-linuxcnc --parallel
ctest --test-dir build/native-grpc-linuxcnc --output-on-failure
```

Run native source checks directly:

```sh
./scripts/native-format.sh check
./scripts/native-lint.sh build/native-grpc-linuxcnc
./linuxcnc-patches/test-stack.sh
```

## Simulator container

The `linuxcnc-simulator` image packages the pinned patched LinuxCNC backend and
`linuxcnc-grpc-server`. It exposes gRPC on port `50051` and read-only telemetry
on WebSocket port `50052`. Configuration, capabilities, and acceptance commands
are documented in the [Docker guide](./docker/README.md).

## License

The project is licensed under GPL-2.0-only. LinuxCNC remains GPLv2 software.
The protobuf definitions under `proto/` are covered by
[`proto/LICENSE`](./proto/LICENSE).
