#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 SERVER LIVE_INTEGRATION LINUXCNC_ROOT" >&2
  exit 2
fi

server="$1"
integration="$2"
linuxcnc_root="$3"
fixture_dir="$(cd "$(dirname "$0")/fixtures" && pwd)"
root="$(mktemp -d "${TMPDIR:-/tmp}/linuxcnc-grpc-live.XXXXXX")"
grpc_port=$((55000 + ($$ % 1000)))
telemetry_port=$((57000 + ($$ % 1000)))
nml_port=$((56000 + ($$ % 1000)))
nml_base=$((20000 + ($$ % 10000)))
server_pid=""
linuxcnc_pid=""
owns_linuxcnc=false

stop_process() {
  local pid="$1"
  [[ -n "$pid" ]] || return 0
  if ! kill -0 "$pid" 2>/dev/null; then
    wait "$pid" 2>/dev/null || true
    return 0
  fi
  kill -TERM "$pid" 2>/dev/null || true
  for _ in $(seq 1 50); do
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" 2>/dev/null || true
      return 0
    fi
    sleep 0.1
  done
  kill -KILL "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

stop_process_group() {
  local pid="$1"
  [[ -n "$pid" ]] || return 0
  if ! kill -0 "$pid" 2>/dev/null; then
    return 0
  fi
  kill -TERM -- "-$pid" 2>/dev/null || true
  for _ in $(seq 1 50); do
    if ! kill -0 "$pid" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  kill -KILL -- "-$pid" 2>/dev/null || true
}

stop_named_processes() {
  local process_name="$1"
  while IFS= read -r pid; do
    stop_process "$pid"
  done < <(pgrep -x "$process_name" 2>/dev/null || true)
}

cleanup() {
  stop_process "$server_pid"
  stop_process_group "$linuxcnc_pid"
  while IFS= read -r pid; do
    stop_process "$pid"
  done < <(pgrep -f -x "linuxcncsvr -ini $root/live.ini" 2>/dev/null || true)
  ipcrm -M "$((nml_base + 1))" 2>/dev/null || true
  ipcrm -M "$((nml_base + 2))" 2>/dev/null || true
  ipcrm -M "$((nml_base + 3))" 2>/dev/null || true
  if $owns_linuxcnc; then
    # The launcher starts some daemons in their own sessions. The preflight
    # above guarantees these singleton runtime processes did not predate us,
    # so any survivors here belong to this test run.
    stop_named_processes linuxcncsvr
    stop_named_processes milltask
    stop_named_processes rtapi_app
    ipcrm -M 0x48484c34 2>/dev/null || true
    ipcrm -M 0x48414c32 2>/dev/null || true
    ipcrm -M 0x00000064 2>/dev/null || true
    rm -f /tmp/linuxcnc.lock
  fi
  rm -rf "$root"
}
trap cleanup EXIT

if [[ -e /tmp/linuxcnc.lock ]]; then
  echo "linuxcnc-grpc-live-integration: /tmp/linuxcnc.lock exists; refusing to disturb another LinuxCNC instance" >&2
  exit 77
fi
if LC_ALL=C ipcs -m 2>/dev/null |
    awk '$1 == "0x48414c32" { found = 1 } END { exit found ? 0 : 1 }'; then
  echo "linuxcnc-grpc-live-integration: a HAL runtime already exists; refusing to disturb it" >&2
  exit 77
fi
for process_name in linuxcncsvr milltask rtapi_app halui; do
  if pgrep -x "$process_name" >/dev/null 2>&1; then
    echo "linuxcnc-grpc-live-integration: $process_name is already running; refusing to disturb it" >&2
    exit 77
  fi
done
if [[ ! -x "$linuxcnc_root/scripts/rip-environment" ]]; then
  echo "linuxcnc-grpc-live-integration: LinuxCNC run-in-place environment is unavailable: $linuxcnc_root" >&2
  exit 77
fi

mkdir -p "$root/active" "$root/workspaces" "$root/health"
chmod 700 "$root/workspaces" "$root/health"
cp "$fixture_dir/sim_mm.var" "$root/sim_mm.var"
cp "$fixture_dir/sim_mm.tbl" "$root/sim_mm.tbl"
cp "$linuxcnc_root/configs/common/linuxcnc.nml" "$root/live.nml"
sed -i \
  -e "s/TCP=5005/TCP=$nml_port/g" \
  -e "s/ 1001 / $((nml_base + 1)) /g" \
  -e "s/ 1002 / $((nml_base + 2)) /g" \
  -e "s/ 1003 / $((nml_base + 3)) /g" \
  "$root/live.nml"
sed \
  -e "s#__NML_FILE__#$root/live.nml#g" \
  -e "s#__ACTIVE_PROGRAM_DIRECTORY__#$root/active#g" \
  "$fixture_dir/sim_grpc.ini.in" > "$root/live.ini"

setsid "$linuxcnc_root/scripts/rip-environment" linuxcnc -r "$root/live.ini" \
  > "$root/linuxcnc.log" 2>&1 &
linuxcnc_pid=$!
owns_linuxcnc=true

# LinuxCNC creates the NML master and loads the HAL files asynchronously. Do
# not construct the daemon's HAL adapter until all three NML buffers exist and
# the HAL repository is queryable. This keeps startup races from
# turning a temporary unavailable channel into a daemon crash.
key_hex() {
  printf '0x%08x' "$1"
}

hal_is_ready() {
  # The RIP wrapper execs halcmd, but a wedged HAL attach can ignore TERM.
  # Always provide a KILL deadline so this readiness probe stays bounded.
  timeout --kill-after=1s 1s \
    "$linuxcnc_root/scripts/rip-environment" halcmd show comp 2>/dev/null |
    grep -q "motmod"
}

start_server() {
  local log_file="$1"
  "$server" \
    --allow-plaintext \
    "--endpoint=127.0.0.1:${grpc_port}" \
    "--telemetry-endpoint=127.0.0.1:${telemetry_port}" \
    "--health-endpoint=unix:$root/health/health.sock" \
    "--ini=$root/live.ini" \
    "--nml=$root/live.nml" \
    "--workspace-root=$root/workspaces" \
    "--active-program-directory=$root/active" \
    "--workspace-ttl-seconds=3600" \
    "--command-queue-capacity=1" \
    "--gcode-batch-size=8" \
    > "$log_file" 2>&1 &
  server_pid=$!

  local deadline=$((SECONDS + 10))
  while (( SECONDS < deadline )); do
    if ! kill -0 "$server_pid" 2>/dev/null; then
      cat "$root/linuxcnc.log" "$log_file" >&2
      return 1
    fi
    if grep -q "listening on 127.0.0.1:${grpc_port}" "$log_file"; then
      return 0
    fi
    sleep 0.05
  done
  cat "$root/linuxcnc.log" "$log_file" >&2
  return 1
}

stop_server_with_deadline() {
  local log_file="$1"
  kill -TERM "$server_pid"
  for _ in $(seq 1 20); do
    kill -0 "$server_pid" 2>/dev/null || break
    sleep 0.1
  done
  if kill -0 "$server_pid" 2>/dev/null; then
    echo "linuxcnc-grpc-live-integration: daemon exceeded two-second shutdown limit" >&2
    cat "$log_file" >&2
    return 1
  fi
  wait "$server_pid"
  server_pid=""
}

readiness_deadline=$((SECONDS + 30))
while (( SECONDS < readiness_deadline )); do
  if ! kill -0 "$linuxcnc_pid" 2>/dev/null; then
    cat "$root/linuxcnc.log" >&2
    exit 1
  fi
  if ipcs -m 2>/dev/null | grep -q "$(key_hex "$((nml_base + 1))")" &&
     ipcs -m 2>/dev/null | grep -q "$(key_hex "$((nml_base + 2))")" &&
     ipcs -m 2>/dev/null | grep -q "$(key_hex "$((nml_base + 3))")" &&
     hal_is_ready; then
    break
  fi
  sleep 0.1
done
if ! ipcs -m 2>/dev/null | grep -q "$(key_hex "$((nml_base + 1))")" ||
   ! ipcs -m 2>/dev/null | grep -q "$(key_hex "$((nml_base + 2))")" ||
   ! ipcs -m 2>/dev/null | grep -q "$(key_hex "$((nml_base + 3))")" ||
   ! hal_is_ready; then
  cat "$root/linuxcnc.log" >&2
  exit 1
fi

start_server "$root/server.log"

if ! timeout 60s "$integration" "127.0.0.1:${grpc_port}" \
    "$fixture_dir/simple_linear.ngc" "127.0.0.1:${telemetry_port}" \
    --batch-size=8; then
  cat "$root/linuxcnc.log" "$root/server.log" >&2
  exit 1
fi

# The local health endpoint reports the running simulator's task state from
# the status poller's cache.
if ! python3 - "$root/health/health.sock" "$root/live.ini" <<'PY'; then
import json, os, socket, sys, time

deadline = time.monotonic() + 10
while True:
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(2)
        client.connect(sys.argv[1])
        client.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
        reply = b""
        while chunk := client.recv(4096):
            reply += chunk
    head, _, body = reply.partition(b"\r\n\r\n")
    health = json.loads(body)
    machine = health["machine"]
    if machine["available"] or time.monotonic() > deadline:
        break
    time.sleep(0.1)
assert head.startswith(b"HTTP/1.1 200 "), head
assert health["serving"] is True, health
assert machine["available"] is True, health
assert 0 <= machine["age_ms"] < 1000, health
assert machine["task_state"] in (1, 2, 3, 4), health
assert machine["interp_state"] in (1, 2, 3, 4), health
assert isinstance(machine["in_position"], bool), health
assert os.path.realpath(machine["ini"]) == os.path.realpath(sys.argv[2]), health
PY
  cat "$root/server.log" >&2
  exit 1
fi

# Give the shutdown-race phase a fresh bounded executor while preserving the
# same real LinuxCNC/HAL runtime and every mutation made by default acceptance.
stop_server_with_deadline "$root/server.log"
start_server "$root/hold-server.log"

timeout 30s "$integration" "127.0.0.1:${grpc_port}" \
  "$fixture_dir/simple_linear.ngc" "127.0.0.1:${telemetry_port}" \
  --hold-shutdown \
  > "$root/hold.log" 2>&1 &
hold_pid=$!
hold_deadline=$((SECONDS + 15))
while (( SECONDS < hold_deadline )); do
  if ! kill -0 "$hold_pid" 2>/dev/null; then
    cat "$root/hold.log" "$root/hold-server.log" >&2
    exit 1
  fi
  grep -q "LIVE_SHUTDOWN_READY" "$root/hold.log" && break
  sleep 0.05
done
if ! grep -q "LIVE_SHUTDOWN_READY" "$root/hold.log"; then
  cat "$root/hold.log" "$root/hold-server.log" >&2
  exit 1
fi

stop_server_with_deadline "$root/hold-server.log"
if ! wait "$hold_pid"; then
  cat "$root/hold.log" "$root/hold-server.log" >&2
  exit 1
fi
grep -q "LIVE_SHUTDOWN_TERMINATED" "$root/hold.log"

if "$linuxcnc_root/scripts/rip-environment" halcmd show comp 2>/dev/null |
    grep -q "grpc-shutdown-owned"; then
  echo "linuxcnc-grpc-live-integration: shutdown-owned HAL component survived" >&2
  exit 1
fi
if "$linuxcnc_root/scripts/rip-environment" halcmd show pin 2>/dev/null |
    grep -q "grpc-shutdown-owned.value"; then
  echo "linuxcnc-grpc-live-integration: shutdown-owned HAL pin survived" >&2
  exit 1
fi

start_server "$root/restarted-server.log"
if ! timeout 15s "$integration" "127.0.0.1:${grpc_port}" \
    "$fixture_dir/simple_linear.ngc" "127.0.0.1:${telemetry_port}" \
    --probe-reacquire \
    > "$root/reacquire.log" 2>&1; then
  cat "$root/reacquire.log" "$root/restarted-server.log" >&2
  exit 1
fi
grep -q "LIVE_REACQUIRE_READY" "$root/reacquire.log"
stop_server_with_deadline "$root/restarted-server.log"
