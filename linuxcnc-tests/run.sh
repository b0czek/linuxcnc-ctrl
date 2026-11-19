#!/usr/bin/env bash
# Runs the LinuxCNC regression suite for the patched build: the upstream tests
# in the LinuxCNC tree plus this directory's tests, leaving out tests for
# LinuxCNC features that are out of scope for this repository.
#
# Usage: run.sh /path/to/linuxcnc [runtests options]
#
# Set SHARD_INDEX and SHARD_COUNT to run one shard of the suite.

set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 /path/to/linuxcnc [runtests options]" >&2
  exit 2
fi

readonly TESTS_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly LINUXCNC_DIR="$(cd -- "$1" && pwd)"
shift
readonly SHARD_COUNT="${SHARD_COUNT:-1}"
readonly SHARD_INDEX="${SHARD_INDEX:-0}"

cd -- "${LINUXCNC_DIR}"

# linuxcnc-tests/resumable-stop/* are the longest tests (about 2.5 minutes
# each); spread them one per shard. Everything else is hashed across shards.
selected=()
resumable_index=0
while IFS= read -r -d '' test_path; do
  name="${test_path#"${TESTS_DIR}/"}"
  [[ "${name}" == "${test_path}" ]] || name="linuxcnc-tests/${name}"
  # Out of scope: GUIs and GUI tools, older remote interfaces, and tool
  # database programs.
  case "${name}" in
    tests/build/ui/* | tests/glcanon/* | tests/halrmt/* | \
      tests/linuxcncrsh/* | tests/linuxcncrsh-tcp/* | \
      tests/pyvcp/* | tests/tooldb/* | tests/tooledit/* | \
      tests/ui-smoke/*)
      continue
      ;;
  esac
  if [[ "${name}" == linuxcnc-tests/resumable-stop/* ]]; then
    shard=$((resumable_index % SHARD_COUNT))
    resumable_index=$((resumable_index + 1))
  else
    checksum="$(printf '%s' "${name}" | cksum)"
    shard=$((${checksum%% *} % SHARD_COUNT))
  fi
  if ((shard == SHARD_INDEX)); then
    selected+=("${test_path}")
  fi
done < <(
  find tests "${TESTS_DIR}" \
    \( -name test -o -name test.sh -o -name test.hal \) -print0 | sort -z
)

((${#selected[@]} > 0)) || { echo "No tests selected" >&2; exit 1; }
echo "Running ${#selected[@]} tests (shard $((SHARD_INDEX + 1))/${SHARD_COUNT})"
exec scripts/rip-environment runtests "$@" "${selected[@]}"
