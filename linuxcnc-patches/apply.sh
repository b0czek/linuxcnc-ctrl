#!/usr/bin/env bash

set -euo pipefail

readonly PATCH_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly BASE_REVISION_FILE="${PATCH_DIR}/base-revision"
readonly DEFAULT_BRANCH="linuxcnc-ctrl/patch-stack"
readonly COMMITTER_NAME="linuxcnc-ctrl patch stack"
readonly COMMITTER_EMAIL="patch-stack@linuxcnc-ctrl.local"

usage() {
  cat >&2 <<EOF
Usage: $0 [--branch NAME | --detach] [--rebuild | --adopt] /path/to/linuxcnc

Materialize the patches as one Git commit per patch. Developer checkouts use
the ${DEFAULT_BRANCH} branch by default; CI and image builds should use
--detach. --rebuild replaces a stale managed branch after saving a backup ref.
--adopt converts an exact legacy, uncommitted application of the full series.
EOF
  exit 2
}

fail() {
  echo "$*" >&2
  exit 1
}

branch_name="${DEFAULT_BRANCH}"
branch_option_set=false
detach=false
rebuild=false
adopt=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --branch)
      [[ $# -ge 2 ]] || usage
      branch_name="$2"
      branch_option_set=true
      shift 2
      ;;
    --detach)
      detach=true
      shift
      ;;
    --rebuild)
      rebuild=true
      shift
      ;;
    --adopt)
      adopt=true
      shift
      ;;
    --help|-h)
      usage
      ;;
    --)
      shift
      break
      ;;
    -*)
      usage
      ;;
    *)
      break
      ;;
  esac
done

[[ $# -eq 1 ]] || usage
[[ "${rebuild}" != true || "${adopt}" != true ]] || \
  fail "--rebuild and --adopt are mutually exclusive"
[[ "${detach}" != true || "${rebuild}" != true ]] || \
  fail "--rebuild is only valid for a managed branch"
[[ "${detach}" != true || "${adopt}" != true ]] || \
  fail "--adopt is only valid for a managed branch"
[[ "${detach}" != true || "${branch_option_set}" != true ]] || \
  fail "--branch and --detach are mutually exclusive"
[[ -f "${BASE_REVISION_FILE}" ]] || \
  fail "Missing LinuxCNC base revision file: ${BASE_REVISION_FILE}"

readonly BASE_REVISION="$(<"${BASE_REVISION_FILE}")"
readonly LINUXCNC_DIR="$1"

git -C "${LINUXCNC_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1 || \
  fail "Not a Git checkout: ${LINUXCNC_DIR}"
git -C "${LINUXCNC_DIR}" cat-file -e "${BASE_REVISION}^{commit}" 2>/dev/null || \
  fail "Pinned LinuxCNC base commit is unavailable: ${BASE_REVISION}"
git check-ref-format --branch "${branch_name}" >/dev/null 2>&1 || \
  fail "Invalid branch name: ${branch_name}"

shopt -s nullglob
patches=("${PATCH_DIR}"/*.patch)
[[ ${#patches[@]} -gt 0 ]] || fail "No LinuxCNC patches found in ${PATCH_DIR}"
previous_number=0
for patch in "${patches[@]}"; do
  filename="$(basename -- "${patch}")"
  [[ "${filename}" =~ ^([0-9]{4})-.+\.patch$ ]] || \
    fail "Patch filename ${filename} does not start with a four-digit sequence number"
  number=$((10#${BASH_REMATCH[1]}))
  ((number > previous_number)) || \
    fail "Patch filename ${filename} does not continue the ascending sequence"
  previous_number=${number}
done

readonly TEMP_DIR="$(mktemp -d)"
readonly STACK_WORKTREE="${TEMP_DIR}/stack"
worktree_added=false

cleanup() {
  if [[ "${worktree_added}" == true ]]; then
    git -C "${LINUXCNC_DIR}" worktree remove --force "${STACK_WORKTREE}" \
      >/dev/null 2>&1 || true
  fi
  rm -rf -- "${TEMP_DIR}"
}
trap cleanup EXIT

git -C "${LINUXCNC_DIR}" worktree add --quiet --detach "${STACK_WORKTREE}" \
  "${BASE_REVISION}" >/dev/null
worktree_added=true

for patch in "${patches[@]}"; do
  echo "Materializing $(basename -- "${patch}")"
  GIT_COMMITTER_NAME="${COMMITTER_NAME}" \
    GIT_COMMITTER_EMAIL="${COMMITTER_EMAIL}" \
    git -C "${STACK_WORKTREE}" am --committer-date-is-author-date --keep-cr \
      "${patch}" || fail "Patch does not apply cleanly: $(basename -- "${patch}")"
done

readonly EXPECTED_TIP="$(git -C "${STACK_WORKTREE}" rev-parse HEAD)"
readonly EXPECTED_COUNT="$(git -C "${STACK_WORKTREE}" rev-list --count \
  "${BASE_REVISION}..${EXPECTED_TIP}")"
[[ "${EXPECTED_COUNT}" -eq "${#patches[@]}" ]] || \
  fail "Expected one commit per patch; got ${EXPECTED_COUNT} commits for ${#patches[@]} patches"

readonly ACTUAL_HEAD="$(git -C "${LINUXCNC_DIR}" rev-parse HEAD)"
current_branch="$(git -C "${LINUXCNC_DIR}" symbolic-ref --quiet --short HEAD || true)"

if [[ "${adopt}" == true ]]; then
  [[ "${ACTUAL_HEAD}" == "${BASE_REVISION}" ]] || \
    fail "--adopt requires HEAD at the pinned base revision"

  adopt_index="${TEMP_DIR}/adopt-index"
  GIT_INDEX_FILE="${adopt_index}" git -C "${LINUXCNC_DIR}" read-tree \
    "${BASE_REVISION}"
  GIT_INDEX_FILE="${adopt_index}" git -C "${LINUXCNC_DIR}" add -A
  adopted_tree="$(GIT_INDEX_FILE="${adopt_index}" \
    git -C "${LINUXCNC_DIR}" write-tree)"
  expected_tree="$(git -C "${LINUXCNC_DIR}" rev-parse "${EXPECTED_TIP}^{tree}")"
  [[ "${adopted_tree}" == "${expected_tree}" ]] || \
    fail "Working tree is not an exact application of the patch series; adoption refused"

  if git -C "${LINUXCNC_DIR}" show-ref --verify --quiet \
    "refs/heads/${branch_name}"; then
    branch_tip="$(git -C "${LINUXCNC_DIR}" rev-parse "refs/heads/${branch_name}")"
    [[ "${branch_tip}" == "${EXPECTED_TIP}" ]] || \
      fail "Branch ${branch_name} already exists at a different commit"
  else
    git -C "${LINUXCNC_DIR}" branch "${branch_name}" "${EXPECTED_TIP}"
  fi

  git -C "${LINUXCNC_DIR}" symbolic-ref HEAD "refs/heads/${branch_name}"
  git -C "${LINUXCNC_DIR}" reset --mixed "${EXPECTED_TIP}" >/dev/null
  [[ -z "$(git -C "${LINUXCNC_DIR}" status --porcelain --untracked-files=all)" ]] || \
    fail "Adoption produced an unexpected dirty checkout"
  echo "Adopted patch stack on ${branch_name} at ${EXPECTED_TIP}"
  exit 0
fi

[[ -z "$(git -C "${LINUXCNC_DIR}" status --porcelain --untracked-files=all)" ]] || \
  fail "LinuxCNC checkout must be completely clean before materializing patches"

if [[ "${ACTUAL_HEAD}" == "${EXPECTED_TIP}" ]]; then
  if [[ "${detach}" == true ]]; then
    [[ -z "${current_branch}" ]] || git -C "${LINUXCNC_DIR}" switch --quiet --detach \
      "${EXPECTED_TIP}" >/dev/null
  elif [[ "${current_branch}" != "${branch_name}" ]]; then
    if git -C "${LINUXCNC_DIR}" show-ref --verify --quiet \
      "refs/heads/${branch_name}"; then
      branch_tip="$(git -C "${LINUXCNC_DIR}" rev-parse "refs/heads/${branch_name}")"
      [[ "${branch_tip}" == "${EXPECTED_TIP}" ]] || \
        fail "Branch ${branch_name} already exists at a different commit"
      git -C "${LINUXCNC_DIR}" switch --quiet "${branch_name}"
    else
      git -C "${LINUXCNC_DIR}" switch --quiet -c "${branch_name}"
    fi
  fi
  echo "LinuxCNC patch stack already materialized: ${EXPECTED_COUNT} commits"
  exit 0
fi

if [[ "${detach}" == true ]]; then
  [[ "${ACTUAL_HEAD}" == "${BASE_REVISION}" ]] || \
    fail "Detached materialization requires HEAD at the pinned base revision"
  git -C "${LINUXCNC_DIR}" switch --quiet --detach "${EXPECTED_TIP}"
  echo "Materialized ${EXPECTED_COUNT} patch commits at ${EXPECTED_TIP} (detached)"
  exit 0
fi

if [[ "${rebuild}" == true ]]; then
  [[ "${current_branch}" == "${branch_name}" ]] || \
    fail "--rebuild requires the managed branch ${branch_name} to be checked out"
  backup_branch="linuxcnc-ctrl/backups/patch-stack-$(date -u +%Y%m%dT%H%M%SZ)-$$"
  git -C "${LINUXCNC_DIR}" branch "${backup_branch}" "${ACTUAL_HEAD}"
  git -C "${LINUXCNC_DIR}" switch --quiet --detach
  git -C "${LINUXCNC_DIR}" branch --force "${branch_name}" "${EXPECTED_TIP}"
  git -C "${LINUXCNC_DIR}" switch --quiet "${branch_name}"
  echo "Rebuilt ${branch_name} at ${EXPECTED_TIP}"
  echo "Previous tip retained as ${backup_branch}"
  exit 0
fi

[[ "${ACTUAL_HEAD}" == "${BASE_REVISION}" ]] || \
  fail "Checkout is neither the pinned base nor the exact patch stack; use --rebuild only for a stale managed branch"

if git -C "${LINUXCNC_DIR}" show-ref --verify --quiet \
  "refs/heads/${branch_name}"; then
  branch_tip="$(git -C "${LINUXCNC_DIR}" rev-parse "refs/heads/${branch_name}")"
  [[ "${branch_tip}" == "${EXPECTED_TIP}" ]] || \
    fail "Branch ${branch_name} already exists at a different commit; check it out and use --rebuild"
  git -C "${LINUXCNC_DIR}" switch --quiet "${branch_name}"
else
  git -C "${LINUXCNC_DIR}" branch "${branch_name}" "${EXPECTED_TIP}"
  git -C "${LINUXCNC_DIR}" switch --quiet "${branch_name}"
fi

echo "Materialized ${EXPECTED_COUNT} patch commits on ${branch_name} at ${EXPECTED_TIP}"
