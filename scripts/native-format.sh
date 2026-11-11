#!/usr/bin/env bash

set -euo pipefail

readonly MODE="${1:-}"
readonly REPO_ROOT="$(git rev-parse --show-toplevel)"
readonly CLANG_FORMAT="${CLANG_FORMAT:-clang-format-21}"

case "${MODE}" in
  format)
    readonly FORMAT_ARGS=(-i)
    ;;
  check)
    readonly FORMAT_ARGS=(--dry-run --Werror)
    ;;
  *)
    echo "usage: $0 {format|check}" >&2
    exit 2
    ;;
esac

if ! command -v "${CLANG_FORMAT}" >/dev/null 2>&1; then
  echo "error: ${CLANG_FORMAT} is required (install Clang 21 tooling)" >&2
  exit 1
fi

if ! "${CLANG_FORMAT}" --version | grep -Eq 'version 21([.]| )'; then
  echo "error: ${CLANG_FORMAT} must be Clang major version 21" >&2
  "${CLANG_FORMAT}" --version >&2
  exit 1
fi

NATIVE_FILES=()
while IFS= read -r -d '' native_file; do
  NATIVE_FILES+=("${native_file}")
done < <(
  git -C "${REPO_ROOT}" ls-files -z --cached --others --exclude-standard -- \
    'native/server/*.cc' 'native/server/**/*.cc' \
    'native/server/*.hpp' 'native/server/**/*.hpp' \
    'native/server/*.h' 'native/server/**/*.h' \
    'native/transport/*.cc' 'native/transport/**/*.cc' \
    'native/transport/*.hpp' 'native/transport/**/*.hpp'
)

if ((${#NATIVE_FILES[@]} == 0)); then
  echo "error: no native C/C++ files found" >&2
  exit 1
fi

cd "${REPO_ROOT}"
"${CLANG_FORMAT}" "${FORMAT_ARGS[@]}" "${NATIVE_FILES[@]}"
