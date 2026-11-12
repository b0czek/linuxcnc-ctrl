#!/usr/bin/env bash

set -euo pipefail

readonly PATCH_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly BASE_REVISION_FILE="${PATCH_DIR}/base-revision"
readonly COMMITTER_NAME="linuxcnc-ctrl patch stack"
readonly COMMITTER_EMAIL="patch-stack@linuxcnc-ctrl.local"

fail() {
  echo "$*" >&2
  exit 1
}

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /path/to/linuxcnc" >&2
  exit 2
fi

[[ -f "${BASE_REVISION_FILE}" ]] || \
  fail "Missing LinuxCNC base revision file: ${BASE_REVISION_FILE}"
readonly BASE_REVISION="$(<"${BASE_REVISION_FILE}")"
readonly LINUXCNC_DIR="$1"

git -C "${LINUXCNC_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1 || \
  fail "Not a Git checkout: ${LINUXCNC_DIR}"
[[ -z "$(git -C "${LINUXCNC_DIR}" status --porcelain --untracked-files=all)" ]] || \
  fail "LinuxCNC checkout must be completely clean before refreshing patches"
git -C "${LINUXCNC_DIR}" merge-base --is-ancestor "${BASE_REVISION}" HEAD || \
  fail "Patch branch is not based on the pinned LinuxCNC revision"
current_branch="$(git -C "${LINUXCNC_DIR}" symbolic-ref --quiet --short HEAD || true)"
[[ -n "${current_branch}" ]] || \
  fail "Refresh requires a checked-out patch branch, not detached HEAD"
[[ -z "$(git -C "${LINUXCNC_DIR}" rev-list --merges "${BASE_REVISION}..HEAD")" ]] || \
  fail "Patch stack must be linear; merge commits are not supported"

mapfile -t commits < <(git -C "${LINUXCNC_DIR}" rev-list --reverse \
  "${BASE_REVISION}..HEAD")
[[ ${#commits[@]} -gt 0 ]] || fail "No patch commits found after the pinned base"

shopt -s nullglob
existing_patches=("${PATCH_DIR}"/*.patch)
[[ ${#commits[@]} -ge ${#existing_patches[@]} ]] || \
  fail "Refusing to remove patches implicitly; delete obsolete patch files explicitly first"

readonly TEMP_DIR="$(mktemp -d)"
readonly OUTPUT_DIR="${TEMP_DIR}/patches"
readonly VERIFY_WORKTREE="${TEMP_DIR}/verify"
mkdir -p "${OUTPUT_DIR}"
worktree_added=false

cleanup() {
  if [[ "${worktree_added}" == true ]]; then
    git -C "${LINUXCNC_DIR}" worktree remove --force "${VERIFY_WORKTREE}" \
      >/dev/null 2>&1 || true
  fi
  rm -rf -- "${TEMP_DIR}"
}
trap cleanup EXIT

generated=()
next_number=1
for index in "${!commits[@]}"; do
  if [[ ${index} -lt ${#existing_patches[@]} ]]; then
    filename="$(basename -- "${existing_patches[index]}")"
    [[ "${filename}" =~ ^([0-9]{4})-.+\.patch$ ]] || \
      fail "Patch filename ${filename} does not start with a four-digit sequence number"
    number=$((10#${BASH_REMATCH[1]}))
    ((number >= next_number)) || \
      fail "Patch filename ${filename} does not continue the ascending sequence"
  else
    number=${next_number}
    slug="$(git -C "${LINUXCNC_DIR}" show -s --format=%f "${commits[index]}")"
    filename="$(printf '%04d' "${number}")-${slug}.patch"
  fi
  next_number=$((number + 1))

  git -C "${LINUXCNC_DIR}" format-patch --zero-commit --no-signature \
    --stdout -1 "${commits[index]}" >"${OUTPUT_DIR}/${filename}"
  generated+=("${OUTPUT_DIR}/${filename}")
done

git -C "${LINUXCNC_DIR}" worktree add --quiet --detach "${VERIFY_WORKTREE}" \
  "${BASE_REVISION}" >/dev/null
worktree_added=true
for patch in "${generated[@]}"; do
  GIT_COMMITTER_NAME="${COMMITTER_NAME}" \
    GIT_COMMITTER_EMAIL="${COMMITTER_EMAIL}" \
    git -C "${VERIFY_WORKTREE}" am --committer-date-is-author-date --keep-cr \
      "${patch}" >/dev/null || fail "Generated patch failed replay: $(basename -- "${patch}")"
done

source_tree="$(git -C "${LINUXCNC_DIR}" rev-parse HEAD^{tree})"
verified_tree="$(git -C "${VERIFY_WORKTREE}" rev-parse HEAD^{tree})"
[[ "${source_tree}" == "${verified_tree}" ]] || \
  fail "Generated patches do not reproduce the source branch tree"

source_tip="$(git -C "${LINUXCNC_DIR}" rev-parse HEAD)"
verified_tip="$(git -C "${VERIFY_WORKTREE}" rev-parse HEAD)"
if [[ "${source_tip}" != "${verified_tip}" ]]; then
  backup_branch="linuxcnc-ctrl/backups/refresh-$(date -u +%Y%m%dT%H%M%SZ)-$$"
  git -C "${LINUXCNC_DIR}" branch "${backup_branch}" "${source_tip}"
  git -C "${LINUXCNC_DIR}" reset --soft "${verified_tip}"
  echo "Pre-normalization tip retained as ${backup_branch}"
fi

for patch in "${generated[@]}"; do
  mv -- "${patch}" "${PATCH_DIR}/$(basename -- "${patch}")"
done

echo "Refreshed ${#generated[@]} patch files and normalized ${current_branch}"
