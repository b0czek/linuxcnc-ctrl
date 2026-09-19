---
name: patch-linuxcnc
description: Use when developing, modifying, finalizing, rebasing, or validating LinuxCNC patches in linuxcnc-patches/. Covers the persistent development checkout, deterministic patch-stack tooling, focused iteration, final replay, and LinuxCNC/native integration boundaries.
---

# Patching LinuxCNC for linuxcnc-ctrl

This project maintains a linear patch series in `linuxcnc-patches/` against the
revision in `linuxcnc-patches/base-revision`. The pinned revision and maintained
patch files are the source of truth. The native daemon is ABI-locked to a
LinuxCNC build containing the complete series.

Use the cheapest phase that can provide new information. Do not turn final
validation into the ordinary edit-build-test loop.

## Invariants

- Keep patch order explicit and keep exactly one logical patch per commit on
  the managed LinuxCNC patch branch.
- Use `linuxcnc-patches/apply.sh` to materialize the series and
  `linuxcnc-patches/refresh.sh` to export it. Do not reproduce their behavior
  with ad hoc clones, `git am` loops, or hand-built patch files.
- A request to develop or fix a LinuxCNC patch authorizes the local staging,
  commits, amendments, rebases, and export needed to deliver the maintained
  patch files. Complete that workflow without a separate permission request.
- Never reset, discard, or absorb unrelated user changes. Preserve the parent
  repository's existing index; patch-stack commits belong in the LinuxCNC
  checkout. Use an isolated worktree when unrelated edits prevent finalization.
- `apply.sh` and `refresh.sh` cleanliness, linear-history, replay, ordering,
  backup, and deterministic-ID checks are guarantees, not optional ceremony.
- LinuxCNC tests must work with its userspace simulator under ordinary POSIX
  scheduling. Do not require hard real-time privileges or timing.

## Phase 1: fast development loop

Treat `linuxcnc/` as a persistent development workspace. Materialize it once
when it does not already contain the current managed stack:

```sh
./linuxcnc-patches/apply.sh linuxcnc
```

Before editing, inspect its branch, status, and patch commits. If it is dirty,
preserve the existing work and determine whether it belongs to the current
task; do not invoke stack reconstruction to erase or bypass it.

```sh
git -C linuxcnc status --short
git -C linuxcnc branch --show-current
git -C linuxcnc log --oneline \
  "$(cat linuxcnc-patches/base-revision)..HEAD"
```

During iteration:

- Edit the persistent checkout directly. An ordinary working-tree diff is
  expected while exploring a change.
- Reuse its existing run-in-place configuration and build outputs.
- Reconfigure only when configuration inputs changed or the build tree is
  demonstrably invalid.
- Compile incrementally. Prefer the narrowest useful build target when one is
  known; otherwise run the existing build without cleaning it.
- Run focused tests that exercise the changed behavior, followed by nearby
  subsystem tests when useful.
- Build or test native/protobuf consumers only when the affected boundary
  reaches them.

Do not refresh patch files, reconstruct the whole stack, clean-build LinuxCNC,
or run the full regression suite after every edit. Repeat an expensive check
only when the implementation or relevant inputs changed enough for it to
provide new evidence.

## Phase 2: patch finalization

When the implementation is stable, turn the development state into the
maintained commit stack and export it as part of the requested work. Source
edits alone do not complete a patch fix:

1. Ensure each logical patch is represented by one commit. Append a commit for
   a new patch. To modify an existing patch, use interactive rebase to stop at
   that commit, amend it, and rebase later patch commits.
2. Keep the history linear and rooted at `base-revision`. Finish with a clean
   LinuxCNC checkout; do not export an uncommitted tree.
3. Run broader relevant LinuxCNC tests and any affected native contract or
   integration tests. Progressively broaden coverage according to the risk of
   the change.
4. Export with the repository tooling:

   ```sh
   ./linuxcnc-patches/refresh.sh linuxcnc
   ```

   `refresh.sh` preserves established filenames by ordinal, creates names for
   appended commits, uses normalized `git format-patch` output, replays the
   complete generated series before replacing files, compares the replayed
   tree, and normalizes the managed branch to deterministic replayed commit
   IDs. It retains the previous tip under `linuxcnc-ctrl/backups/` when needed
   and refuses implicit patch removal.
5. Update the patch inventory in `linuxcnc-patches/README.md` when adding,
   removing, or materially changing a documented patch.

After refresh, verify the repository diff and run
`./linuxcnc-patches/test-stack.sh` when patch tooling changed or when final
stack-integrity evidence is needed. Do not manually edit generated patch
content as a substitute for amending its commit and refreshing.

## Phase 3: full validation and CI

Use this phase for release-quality validation, baseline changes, CI, or when a
change is stable enough that complete-stack evidence is worth its cost.

1. Materialize the complete series from the pinned revision with
   `apply.sh`. Use `--detach` for CI and image builds. If patch files changed
   while a clean managed checkout contains the older stack, use `--rebuild`;
   the script retains the prior tip under `linuxcnc-ctrl/backups/`.
2. Confirm the replayed commit count and ordering match the patch series.
3. Perform a clean build where isolation from stale artifacts matters.
4. Run the full LinuxCNC regression suite and affected integration checks.
5. Build the native daemon against that LinuxCNC tree and run its complete
   contract/integration suite when ABI, NML, protobuf, or mapped status data is
   involved.

This is the final validation path, not the default response to a source edit.
CI remains the authoritative clean-environment execution of this phase.

## Tooling modes

From a clean checkout at the pinned baseline, materialize a managed branch:

```sh
./linuxcnc-patches/apply.sh /path/to/linuxcnc
```

Use `--detach` for a disposable CI/image result. `--adopt` is only for the
one-time migration of a legacy uncommitted checkout whose complete tree
exactly matches an independently materialized series. Never use adoption to
absorb extra work.

## Cross-boundary changes

When changing status or commands, trace the complete boundary as applicable:

- `src/emc/nml_intf/emc_nml.hh`: NML/status structures.
- `src/emc/nml_intf/emc.cc`: matching serialization updates.
- `src/emc/nml_intf/emcops.cc`: initialization.
- `src/emc/task/emctask.cc`: task status population. Interpreter parameter
  data is available through `_is` only after a null check.
- `src/emc/usr_intf/axis/extensions/emcmodule.cc`: Python status exposure.
- `native/server/src/` and `proto/`: native mapping and wire contract.

Coordinate downstream clients when the protobuf boundary changes. This
repository owns the canonical schema and native implementation, not every
consumer.

Common interpreter parameter blocks are:

- G5x offsets and rotations: `#5221–#5390`, 20 parameters per system across
  nine systems. Offsets use `base+0` through `base+8`; rotation is `base+9`.
- G28 home: `#5161–#5169`.
- G30 home: `#5181–#5189`.

## Baseline changes

Changing `base-revision` requires rebasing the complete linear patch stack,
refreshing it with `refresh.sh`, and running full validation. Never update the
pinned revision without replaying and testing the entire series.
