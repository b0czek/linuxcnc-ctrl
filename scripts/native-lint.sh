#!/usr/bin/env bash

set -euo pipefail

if (($# != 1)); then
  echo "usage: $0 <build-dir>" >&2
  exit 2
fi

readonly REPO_ROOT="$(git rev-parse --show-toplevel)"
readonly BUILD_DIR="$1"
readonly CLANG_TIDY="${CLANG_TIDY:-clang-tidy-21}"
readonly RUN_CLANG_TIDY="${RUN_CLANG_TIDY:-run-clang-tidy-21}"
readonly COMPILE_COMMANDS="${BUILD_DIR}/compile_commands.json"
readonly NATIVE_SOURCE_FILTER="^${REPO_ROOT}/native/(server|transport)/.*[.](cc|hpp|h)$"

if ! command -v "${CLANG_TIDY}" >/dev/null 2>&1; then
  echo "error: ${CLANG_TIDY} is required (install Clang 21 tooling)" >&2
  exit 1
fi

if ! "${CLANG_TIDY}" --version | grep -Eq 'version 21([.]| )'; then
  echo "error: ${CLANG_TIDY} must be Clang major version 21" >&2
  "${CLANG_TIDY}" --version >&2
  exit 1
fi

if ! command -v "${RUN_CLANG_TIDY}" >/dev/null 2>&1; then
  echo "error: ${RUN_CLANG_TIDY} is required (install Clang 21 tooling)" >&2
  exit 1
fi

if [[ ! -f "${COMPILE_COMMANDS}" ]]; then
  echo "error: compilation database not found at ${COMPILE_COMMANDS}" >&2
  echo "configure CMake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
  exit 1
fi

exec "${RUN_CLANG_TIDY}" \
  -clang-tidy-binary "${CLANG_TIDY}" \
  -config-file "${REPO_ROOT}/native/server/.clang-tidy" \
  -p "${BUILD_DIR}" \
  -header-filter "${NATIVE_SOURCE_FILTER}" \
  "${NATIVE_SOURCE_FILTER}"
