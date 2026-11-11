#!/usr/bin/env bash

set -euo pipefail

readonly SOURCE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly TEMP_DIR="$(mktemp -d)"
trap 'rm -rf -- "${TEMP_DIR}"' EXIT

fail() {
  echo "patch-stack test failed: $*" >&2
  exit 1
}

assert_clean() {
  [[ -z "$(git -C "$1" status --porcelain --untracked-files=all)" ]] || \
    fail "$1 is not clean"
}

readonly UPSTREAM="${TEMP_DIR}/upstream"
readonly STACK="${TEMP_DIR}/stack"
readonly TARGET="${TEMP_DIR}/target"
readonly STALE="${TEMP_DIR}/stale"
readonly ADOPT="${TEMP_DIR}/adopt"
readonly REPLAY="${TEMP_DIR}/replay"
readonly PARTIAL="${TEMP_DIR}/partial"
readonly WRONG_BASE="${TEMP_DIR}/wrong-base"
readonly MERGED="${TEMP_DIR}/merged"

mkdir -p "${UPSTREAM}" "${STACK}"
cp "${SOURCE_DIR}/apply.sh" "${SOURCE_DIR}/refresh.sh" "${STACK}/"
git -C "${UPSTREAM}" init --quiet
git -C "${UPSTREAM}" config user.name "Patch Stack Test"
git -C "${UPSTREAM}" config user.email "patch-stack-test@example.invalid"

printf 'base\n' >"${UPSTREAM}/base.txt"
git -C "${UPSTREAM}" add base.txt
git -C "${UPSTREAM}" commit --quiet -m "upstream base"
base_revision="$(git -C "${UPSTREAM}" rev-parse HEAD)"
printf '%s\n' "${base_revision}" >"${STACK}/base-revision"

printf 'first\n' >"${UPSTREAM}/first.txt"
git -C "${UPSTREAM}" add first.txt
git -C "${UPSTREAM}" commit --quiet -m "fixture: first patch"
git -C "${UPSTREAM}" format-patch --zero-commit --no-signature --stdout -1 \
  >"${STACK}/0001-first.patch"

printf 'second\n' >"${UPSTREAM}/second.txt"
git -C "${UPSTREAM}" add second.txt
git -C "${UPSTREAM}" commit --quiet -m "fixture: second patch"
git -C "${UPSTREAM}" format-patch --zero-commit --no-signature --stdout -1 \
  >"${STACK}/0002-second.patch"

git clone --quiet "${UPSTREAM}" "${TARGET}"
git -C "${TARGET}" checkout --quiet --detach "${base_revision}"
"${STACK}/apply.sh" "${TARGET}" >/dev/null
[[ "$(git -C "${TARGET}" branch --show-current)" == \
  "linuxcnc-ctrl/patch-stack" ]] || fail "managed branch was not selected"
[[ "$(git -C "${TARGET}" rev-list --count "${base_revision}..HEAD")" == 2 ]] || \
  fail "materialization did not create one commit per patch"
[[ "$(git -C "${TARGET}" log -2 --format=%s)" == $'fixture: second patch\nfixture: first patch' ]] || \
  fail "patch commit subjects or order are incorrect"
assert_clean "${TARGET}"

first_tip="$(git -C "${TARGET}" rev-parse HEAD)"
"${STACK}/apply.sh" "${TARGET}" >/dev/null
[[ "$(git -C "${TARGET}" rev-parse HEAD)" == "${first_tip}" ]] || \
  fail "idempotent materialization changed the tip"

printf 'unrelated\n' >"${TARGET}/unrelated.txt"
if "${STACK}/apply.sh" "${TARGET}" >/dev/null 2>&1; then
  fail "dirty checkout was accepted"
fi
rm "${TARGET}/unrelated.txt"

git clone --quiet "${TARGET}" "${PARTIAL}"
git -C "${PARTIAL}" switch --quiet linuxcnc-ctrl/patch-stack
git -C "${PARTIAL}" reset --quiet --hard HEAD^
if "${STACK}/apply.sh" "${PARTIAL}" >/dev/null 2>&1; then
  fail "partial managed stack was accepted"
fi

git clone --quiet "${UPSTREAM}" "${WRONG_BASE}"
if "${STACK}/apply.sh" "${WRONG_BASE}" >/dev/null 2>&1; then
  fail "checkout at the wrong base was accepted"
fi

git clone --quiet "${UPSTREAM}" "${STALE}"
git -C "${STALE}" checkout --quiet --detach "${base_revision}"
"${STACK}/apply.sh" "${STALE}" >/dev/null

git -C "${TARGET}" config user.name "Patch Stack Test"
git -C "${TARGET}" config user.email "patch-stack-test@example.invalid"
printf 'third\n' >"${TARGET}/third.txt"
git -C "${TARGET}" add third.txt
git -C "${TARGET}" commit --quiet -m "fixture: third patch"
"${STACK}/refresh.sh" "${TARGET}" >/dev/null
[[ "$(find "${STACK}" -maxdepth 1 -name '*.patch' | wc -l)" == 3 ]] || \
  fail "refresh did not add the new patch"
normalized_tip="$(git -C "${TARGET}" rev-parse HEAD)"
"${STACK}/apply.sh" "${TARGET}" >/dev/null
[[ "$(git -C "${TARGET}" rev-parse HEAD)" == "${normalized_tip}" ]] || \
  fail "refreshed branch was not normalized to the replayable commit IDs"

if "${STACK}/apply.sh" "${STALE}" >/dev/null 2>&1; then
  fail "stale managed stack was accepted without --rebuild"
fi
"${STACK}/apply.sh" --rebuild "${STALE}" >/dev/null
[[ "$(git -C "${STALE}" rev-list --count "${base_revision}..HEAD")" == 3 ]] || \
  fail "rebuild did not install the refreshed stack"
[[ "$(git -C "${STALE}" for-each-ref --format='%(refname)' \
  'refs/heads/linuxcnc-ctrl/backups/patch-stack-*' | wc -l)" -ge 1 ]] || \
  fail "rebuild did not retain a backup branch"
assert_clean "${STALE}"

git clone --quiet "${UPSTREAM}" "${ADOPT}"
git -C "${ADOPT}" checkout --quiet --detach "${base_revision}"
for patch in "${STACK}"/*.patch; do
  git -C "${ADOPT}" apply "${patch}"
done
"${STACK}/apply.sh" --adopt "${ADOPT}" >/dev/null
[[ "$(git -C "${ADOPT}" branch --show-current)" == \
  "linuxcnc-ctrl/patch-stack" ]] || fail "adoption did not create the managed branch"
assert_clean "${ADOPT}"

git -C "${ADOPT}" switch --quiet --detach "${base_revision}"
for patch in "${STACK}"/*.patch; do
  git -C "${ADOPT}" apply "${patch}"
done
printf 'unrelated\n' >"${ADOPT}/unrelated.txt"
if "${STACK}/apply.sh" --adopt --branch linuxcnc-ctrl/adopt-reject \
  "${ADOPT}" >/dev/null 2>&1; then
  fail "adoption accepted an unrelated change"
fi

git clone --quiet "${UPSTREAM}" "${REPLAY}"
git -C "${REPLAY}" checkout --quiet --detach "${base_revision}"
"${STACK}/apply.sh" --detach "${REPLAY}" >/dev/null
[[ -z "$(git -C "${REPLAY}" branch --show-current)" ]] || \
  fail "--detach created a branch"
[[ "$(git -C "${REPLAY}" rev-parse HEAD^{tree})" == \
  "$(git -C "${TARGET}" rev-parse HEAD^{tree})" ]] || \
  fail "refreshed patches did not round-trip to the same tree"
[[ "$(git -C "${REPLAY}" rev-parse HEAD)" == \
  "$(git -C "${TARGET}" rev-parse HEAD)" ]] || \
  fail "refreshed patches did not reproduce deterministic commit IDs"
assert_clean "${REPLAY}"

git clone --quiet "${TARGET}" "${MERGED}"
git -C "${MERGED}" switch --quiet linuxcnc-ctrl/patch-stack
git -C "${MERGED}" config user.name "Patch Stack Test"
git -C "${MERGED}" config user.email "patch-stack-test@example.invalid"
git -C "${MERGED}" switch --quiet -c fixture-side HEAD^
printf 'side\n' >"${MERGED}/side.txt"
git -C "${MERGED}" add side.txt
git -C "${MERGED}" commit --quiet -m "fixture: side"
git -C "${MERGED}" switch --quiet linuxcnc-ctrl/patch-stack
printf 'main\n' >"${MERGED}/main.txt"
git -C "${MERGED}" add main.txt
git -C "${MERGED}" commit --quiet -m "fixture: main"
git -C "${MERGED}" merge --quiet --no-ff fixture-side -m "fixture: merge"
if "${STACK}/refresh.sh" "${MERGED}" >/dev/null 2>&1; then
  fail "refresh accepted a merge commit"
fi

readonly GAP_STACK="${TEMP_DIR}/gap-stack"
readonly GAP="${TEMP_DIR}/gap"
readonly GAP_DUPLICATE="${TEMP_DIR}/gap-duplicate"
mkdir -p "${GAP_STACK}"
cp "${SOURCE_DIR}/apply.sh" "${SOURCE_DIR}/refresh.sh" \
  "${STACK}/base-revision" "${GAP_STACK}/"
cp "${STACK}/0001-first.patch" "${GAP_STACK}/0001-first.patch"
cp "${STACK}/0002-second.patch" "${GAP_STACK}/0004-second.patch"

git clone --quiet "${UPSTREAM}" "${GAP}"
git -C "${GAP}" checkout --quiet --detach "${base_revision}"
"${GAP_STACK}/apply.sh" "${GAP}" >/dev/null || \
  fail "a gap left by a removed patch was rejected"
[[ "$(git -C "${GAP}" rev-list --count "${base_revision}..HEAD")" == 2 ]] || \
  fail "gapped series did not create one commit per patch"

git -C "${GAP}" config user.name "Patch Stack Test"
git -C "${GAP}" config user.email "patch-stack-test@example.invalid"
printf 'appended\n' >"${GAP}/appended.txt"
git -C "${GAP}" add appended.txt
git -C "${GAP}" commit --quiet -m "fixture: appended patch"
"${GAP_STACK}/refresh.sh" "${GAP}" >/dev/null
[[ -f "${GAP_STACK}/0001-first.patch" && \
  -f "${GAP_STACK}/0004-second.patch" ]] || \
  fail "refresh renumbered a gapped series"
[[ -f "${GAP_STACK}/0005-fixture-appended-patch.patch" ]] || \
  fail "refresh did not number the appended patch after the highest number"
[[ "$(find "${GAP_STACK}" -maxdepth 1 -name '*.patch' | wc -l)" == 3 ]] || \
  fail "refresh of a gapped series produced unexpected patch files"

cp "${GAP_STACK}/0001-first.patch" "${GAP_STACK}/0004-duplicate.patch"
git clone --quiet "${UPSTREAM}" "${GAP_DUPLICATE}"
git -C "${GAP_DUPLICATE}" checkout --quiet --detach "${base_revision}"
if "${GAP_STACK}/apply.sh" --detach "${GAP_DUPLICATE}" >/dev/null 2>&1; then
  fail "a duplicated sequence number was accepted"
fi

echo "patch-stack tests passed"
