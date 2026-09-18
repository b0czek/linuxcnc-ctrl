# LinuxCNC patch series

`linuxcnc-ctrl` is purpose-built for LinuxCNC with the patches in this
directory. It is not compatible with an arbitrary stock LinuxCNC build.

## Baseline

The series is based on the LinuxCNC checkout currently used by this workspace:

- Repository: <https://github.com/LinuxCNC/linuxcnc>
- Revision: the commit recorded in [`base-revision`](./base-revision)
- Version line: LinuxCNC 2.10 development

`base-revision` is the single source of truth used by the stack tooling and
CI. Patch files remain the reviewable source of truth in this repository, but
they are materialized into LinuxCNC as a linear Git history with exactly one
commit per patch. Changing the baseline requires rebasing and validating the
complete series.

## Continuous integration

Changes under `linuxcnc-patches/` trigger the
[`LinuxCNC patch tests`](../.github/workflows/linuxcnc-patches.yml) workflow on
pushes and pull requests. The workflow materializes the complete series on the
pinned revision, builds LinuxCNC in run-in-place mode with its upstream CI
tooling, and runs the full LinuxCNC regression suite with failure output
enabled.

## Applying the series

From a clean checkout of the baseline revision:

```sh
./linuxcnc-patches/apply.sh /path/to/linuxcnc
```

The script builds the complete series in a temporary worktree first, then
creates and checks out `linuxcnc-ctrl/patch-stack`. Every patch is applied with
`git am`, so `git log`, `git show`, rebase, revert, and bisect work normally.
It is safe to rerun for the exact stack, but refuses dirty, partial, stale, or
divergent checkouts.

CI and image builds use detached mode while retaining the same commit history:

```sh
./linuxcnc-patches/apply.sh --detach /path/to/linuxcnc
```

For a checkout produced by the former uncommitted workflow, `--adopt` is a
one-time migration. It succeeds only when the complete working tree exactly
matches a separately materialized series; it never absorbs extra changes:

```sh
./linuxcnc-patches/apply.sh --adopt /path/to/linuxcnc
```

If tracked patch files changed while a clean managed branch still contains an
older stack, rebuild it explicitly. The previous tip is retained under
`linuxcnc-ctrl/backups/`:

```sh
./linuxcnc-patches/apply.sh --rebuild /path/to/linuxcnc
```

## Developing the series

Follow the repository's
[`patch-linuxcnc` skill](../skills/patch-linuxcnc/SKILL.md) for the development,
finalization, and full-validation workflow. Keep each logical patch as one
commit on the managed branch and export commits with the repository tooling:

```sh
./linuxcnc-patches/refresh.sh /path/to/linuxcnc
```

`refresh.sh` requires a linear history rooted at `base-revision`, preserves
existing filenames by ordinal, names newly appended patches from their commit
subjects, and replays the generated series before replacing any patch file. It
then normalizes the checked-out branch to those deterministic replayed commit
IDs, retaining its prior tip under `linuxcnc-ctrl/backups/` when the IDs
change. It refuses to remove patches implicitly.

Patch filenames begin with a sequence number. New patches must use the next
number so their application order remains explicit. The filename order and
commit order must match.

## Patch inventory

### 0001 — Export spindle speed feedback through spindle status

`spindle.N.speed-in` is already sampled by the motion controller into
`emcmotStatus.spindle_status[N].spindleSpeedIn`, but the value stops at the
motion/task boundary. Clients that only consume `EMC_STAT` therefore need a
second HAL connection solely to display actual spindle speed.

This patch adds `feedback` to `EMC_SPINDLE_STAT`, copies `spindleSpeedIn` into
it for every configured spindle, serializes it through NML, and exposes it as
`feedback` in the Python spindle status dictionary. The value is the signed
speed supplied to `spindle.N.speed-in`, converted from revolutions per second
to RPM to match the other spindle status speed fields.

The corresponding transport property is `motion.spindle[N].feedback`. Keeping
the feedback in spindle status gives GUI
and remote status consumers one coherent source without requiring direct HAL
access.

### 0002 — Expose interpreter coordinate data in task status

`EMC_TASK_STAT` already carries `g5x_offset` and `rotation_xy` for the
currently active coordinate system, but all other interpreter coordinate data
(G5x rotations, G28/G30 home positions, and the other eight G5x offsets) is
only reachable by syncing the RS274NGC parameter file or triggering a parameter
dump.

This patch adds the following fields to `EMC_TASK_STAT` and populates them
directly from the interpreter's parameter array every task status update:

- `g5x_offsets[9]` — all G5x coordinate system offsets (G54–G59.3)
- `g5x_rotations[9]` — all G5x XY rotation angles (G54–G59.3)
- `g28_position` — G28 home position
- `g30_position` — G30 home position

All pose values are in user units, consistent with the existing `g5x_offset`.
Index 0 of the arrays is G54, index 8 is G59.3. The fields are serialized
through NML and exposed in the Python `linuxcnc.stat` object as
`g5x_offsets`, `g5x_rotations`, `g28_position`, and `g30_position`.

The corresponding transport properties are `task.g5xOffsets[N]`,
`task.g5xRotations[N]`, `task.g28Position`, and `task.g30Position`.

### 0003 — Preserve G96/G97 modal state across task-mode switches

`Interp::synch()` is called on every MANUAL ↔ MDI task-mode transition and at
other interpreter synchronization points. It used to reset
`_setup.spindle_mode[s]` to `SPINDLE_MODE::CONSTANT_RPM` (G97) for every
spindle, even though the motion controller and canon layer retained the CSS
(G96) state. This made the interpreter model diverge from the actual machine
state, so the active-G-code display and subsequent MDI commands behaved as if
G97 were active.

This patch:

- Moves the default G97 initialization from `Interp::synch()` into
  `Interp::init()`, so all spindles start in CONSTANT_RPM at interpreter
  startup.
- Removes the unconditional reset in `Interp::synch()`, letting G96/G97
  survive task-mode switches and MDI command boundaries.
- Updates `Interp::convert_stop()` (M2/M30) to set
  `settings->spindle_mode[s] = SPINDLE_MODE::CONSTANT_RPM` after calling
  `SET_SPINDLE_MODE(s, 0)`, keeping the interpreter model consistent with
  canon when a program ends.

No new `EMC_STAT` fields or public transport fields are added.

### 0004 — Resumable Stop for active AUTO programs

Adds a dedicated `EMC_TASK_PLAN_STOP` command. Stop decelerates an active AUTO
program without clearing interpreter or motion queues, safely finishes
position-synchronized threading and rigid tapping, stops saved spindle and
coolant outputs, and restores them before Run, Resume, or Single Step continues
the program. The command is rejected when no AUTO program is active; callers
must issue `EMC_TASK_ABORT` explicitly when execution should be terminated.
Abort, E-stop, faults, mode changes, M0/M1, and Pause retain their existing
behavior.

The command is exposed as Python `command.program_stop()`, protobuf
`stop_program`, and `halui.program.stop`. Stop progress is
reported as `EMC_TASK_STOP_STATE` (`IDLE`, `STOPPING`, `STOPPED`, `STARTING`)
through Python `stop_state` and the task-status protobuf. The patch adds a
`tests/resumable-stop` LinuxCNC regression that verifies active AUTO
Stop/Resume and Stop/Single-Step preserve the queued program, disable
spindle/coolant while stopped, restore them without adding a task-level spindle-at-speed
wait, preserves the normal next-feed at-speed wait after rapid motion,
completes the same program, and waits for active `G33` threading and `G33.1`
rigid-tap synchronized motion to finish before entering `STOPPED`.

### 0005 — Headless and documentation-free build options

Adds the Autoconf-standard `--enable-headless` option for backend-only builds
and the independent `--disable-build-manpages` option. Headless builds retain
the controller, realtime/HAL stack, interpreters, remote interfaces, and core
Python and Tcl APIs while omitting LinuxCNC-supplied GUIs, graphical probes,
assets, samples, and desktop integration. The launcher requires an explicit
INI path but continues to execute an external `[DISPLAY] DISPLAY` command.

The Debian build profiles `pkg.linuxcnc.headless` and `nodoc` select the
corresponding configure options and packaging manifests. With no new options
or profiles, the existing GUI, documentation, manpage, and packaging behavior
is unchanged.

### 0006 — Ordered realtime component search path

Adds the trusted `HAL_RTMOD_PATH` runtime search path and the configure-time
`--with-extra-rtlib-dirs=DIR[:DIR...]` trust set. Ordinary realtime components
are resolved by name across the ordered path and the first regular-file match
is final, while the internal `rtapi` and `hal_lib` modules remain pinned to the
standard LinuxCNC realtime module directory. Invalid, relative, empty, and
unauthorized path configurations are rejected as a whole, and canonical path
checks prevent trusted-directory symlink escapes.

The shared resolver is used by `halcmd`, `halrmt`, userspace `rtapi_app`, shell
completion, and the setuid module helper. `linuxcnc` and `halrun` continue to
export `HAL_RTMOD_DIR` and only supply the standard `HAL_RTMOD_PATH` default
when the caller did not set one. No public transport changes are needed.

### 0007 — Native automatic tool wear offsets

Adds a separate nine-axis `wear_offset` to every tool record and extends tool
tables with `WX/WY/WZ/WA/WB/WC/WU/WV/WW`. `G10 L3` edits stored wear with
partial-axis semantics, while `G43` applies geometry plus wear. Built-in M6
and M61 activate the resulting combined offset automatically; G49 still
cancels compensation and same-block explicit G-codes take precedence.

Stored wear is available in interpreter parameters `#5430–#5438`, Python tool
status, and the interpreter Python tool object. The tool database protocol is
v2.2 and the tool-entry line limit is 512 bytes. The protobuf contract exposes
`ToolEntry.wearOffset`, including status deltas and partial `setTool` updates.

### 0008 — Tapered G76 drive lines and alternating infeed

Extends the RS274 G76 threading cycle with an optional X endpoint. X is the
normal final coordinate of the thread drive line and uses the interpreter's
existing G90/G91, coordinate-transform, and G7/G8 endpoint handling. Omitting X
retains the existing cylindrical cycle unchanged.

For an XZ drive line, every cutting and spring pass follows the programmed
taper. The synchronized path lead is increased geometrically so the axial Z
advance per spindle revolution remains the programmed P pitch. Compound infeed
offsets and E/L entry and exit tapers are constructed along the tapered drive
line, including their independently corrected synchronized leads. Interpreter
regressions cover external and internal threads, both taper directions,
absolute and incremental endpoints, radius and diameter modes, entry/exit
tapers in both Z directions, compound infeed, spring passes, and rejection of
Y endpoints. G76 also accepts `D0` for the existing fixed compound-infeed
direction (the default) and `D1` to alternate roughing passes across the final
thread line; full-depth and spring passes remain centered, and `Q0` keeps every
pass on the centered line. Both cylindrical and tapered alternating paths are
covered. No NML or public transport changes are required.

### 0009 — Safe G76 Stop clearance and two-stage Single Step

G76 marks each approach, synchronized cut, and particular clearance retract
with an internal pass ID. When Stop is requested during the cut or clearance,
the trajectory planner latches that pass and completes exactly its marked
retract before entering `STOPPED`, binding the target when it reaches motion
if necessary. This leaves the tool clear instead of cutting a groove at thread
depth, while ordinary G33, rigid tapping, and unrelated following rapids
retain their prior Stop boundaries. The clearance is an exact, non-blendable
endpoint, so arc blending cannot trim it. Synthetic blend arcs within a
synchronized cut inherit the pass owner, keeping Stop bound to that pass's
marked clearance.

Each generated cylindrical or tapered G76 pass is split into two Single Step
units. The first returns to the pass start and stops at the safe clearance
depth. The second performs
the infeed, synchronized thread, and marked clearance retract together. These
internal motion fences are active only while stepping, so uninterrupted
`AUTO_RUN` retains task readahead. Motion-level targeting preserves the same
safe-approach/infeed-thread-clearance sequence after Stop when later G76 passes
were already queued during normal `AUTO_RUN`; ordinary unmarked motion retains
source-line stepping. Twenty-five
runtime scenarios cover direct first passes, fresh and prequeued cylindrical
and tapered safe-approach/thread-clearance pairs, synchronized exit tapers,
arc-blend clearance endpoints, an unmarked post-G33 rapid, and repeated Stops
with deterministic cut-side timing near the cut/retract handoff. The internal
NML and motion fields add no public status or transport field.

### 0010 — Recording canon backend for native G-code preview

Adds `recordingcanon.cc` and `recordingcanon.hh`, a separate link-selected
implementation of LinuxCNC's global CANON API for native offline interpreters.
It follows the same backend boundary already used by task (`emccanon.cc`), Axis
(`gcodemodule.cc`), and the standalone interpreter (`saicanon.cc`), leaving
those existing implementations untouched.

### 0011 — In-flight Single Step arming

`AUTO_STEP` is forwarded to motion while an AUTO program is already running.
The trajectory planner latches the active source block, or the active G76
approach or thread-plus-clearance unit supplied by patch 0009, and finishes it
before stopping at an exact boundary. Later motion remains queued and does not
begin, including when several following blocks or threading passes were
prequeued by readahead.

The temporary exact boundary is restored if Step is canceled by Resume or Stop.
Existing paused source-line stepping and two-stage G76 stepping remain intact.
Two additional runtime scenarios arm Step during an ordinary block with three
later blocks queued and during an active G76 cut with later passes queued,
bringing the resumable-stop suite to twenty-seven scenarios. During an
in-flight step, `single_stepping` reports the armed/active state while task and
motion remain unpaused; both paused flags become true only after motion reaches
the boundary.

### 0012 — LinuxCNC-owned transactional tool CRUD

Adds explicit NML commands for partial tool upserts and deletion. The task
process validates changer state, stages complete table snapshots, persists
file-backed tables through an fsync-and-rename save, and only then atomically
publishes the resulting mmap contents. Random changer pockets address their
exact mmap slots, while nonrandom loaded tools keep spindle slot zero
synchronized.

Remote CRUD supports LinuxCNC's native random and nonrandom table models for
file-backed configurations. Random support covers ATC-addressable pockets and
the spindle; it does not model off-machine tool inventory. This patched build
rejects `[EMCIO]DB_PROGRAM` during IO initialization and requires file-backed
`[EMCIO]TOOL_TABLE` configuration for managed tool data.

Tool-table reload now preserves the live table when opening, parsing, or
validation fails. Read-only mmap clients detect inode replacement after a
LinuxCNC restart and remap under the same process lock used by all mmap
readers.

### 0013 — Composable executor standard glue

Adds transport-independent `executor_transact` and `executor_execute`
generators to LinuxCNC's curated Python `stdglue`. Machine configurations keep
ordinary, explicitly named remaps and pass an endpoint object into either
helper. `transact` owns cooperative waiting, timeout, release, and abort
cancellation while returning the terminal response for machine-specific
policy. `execute` adds the common policy that only a successful response
continues program execution. The default timeout is 10 seconds.

The supplied `HalExecutor` endpoint adds namespaced request and response pins
to a HAL component provided and owned by the machine configuration. It neither
constructs the component nor calls `ready()`, allowing multiple endpoints and
machine-specific pins to share one component. HAL remains optional: an
integration regression exercises the same helpers with both the standard HAL
endpoint and an in-memory endpoint.

Python remap generators are closed during interpreter unwind so their
`finally` cleanup runs on task or operator abort. The regression also covers
success, raw failure handling, standard failure policy, timeout, stale
responses, monotonic request IDs, request release, and cancellation. Headless
installs now include the curated `stdglue`.

### 0014 — Publish modal CSS maximum in spindle status

Publishes the canonical G96 `D` limit through the existing
`EMC_SPINDLE_STAT.css_maximum` field when spindle mode changes. The value
survives M5 because it is modal state rather than a running-speed command, and
G97 clears it. Spindle `enabled` is now derived from the motion controller's
explicit on/off state instead of commanded speed, so a stopped spindle with a
nonzero CSS limit remains disabled and is not restarted by resumable Stop.

The `tests/css-status` regression covers G96 while stopped, persistence across
M5, clearing on G97, and stopped-spindle enabled state. The patch exposes
`css_maximum` through Python spindle status; the canonical protobuf schema
already carries it, so no transport-contract change is required.

### 0015 — Pass spindle selectors to remaps

Adds `$` to the remap `argspec` grammar. When a remapped block includes a
spindle selector, named NGC parameters and Python keyword arguments receive it
as `spindle`; positional remaps receive it at the `$` position in the
argument specification. Omitting `$` omits the parameter, and each remap
retains responsibility for validating the spindle values it supports.
When `^` requires a positive spindle speed, `$` selects which spindle is
checked and supplied as `s`; omitting `$` preserves the spindle 0 default.

The `tests/remap/dollar-argspec` regression covers omitted, numbered, and
all-spindle selectors through Python, named NGC, and positional NGC remaps,
plus selected-spindle behavior for `^`.

### 0016 — Motion joint-target preparation

Stages the entire inverse-kinematics target vector, forward-kinematics branch
flags, and cubic interpolation buffers before committing joint commands. Rejected
targets retain the previous branch flags; accepted targets publish the new flags. A failure at a later joint cannot
partially commit earlier targets. A small trajectory-planner helper resets an
axis planner at a stationary position while retaining its configured limits.
This patch exposes no availability controls and builds independently of the
runtime feature.

`tests/motion-target-preparation` compiles the production target-preparation
function with deterministic kinematics and the real cubic implementation. It
covers late invalid targets, interpolation failure, and no partial commitment.

### 0017 — Runtime joint and spindle availability

Adds runtime acquisition and release of individual joints and spindles, allowing
external hardware to hand physical resources to LinuxCNC and take them back.
Availability is separate from Machine On and amplifier enable: a resource must
be acquired before LinuxCNC can use it for ordinary motion or spindle commands.

Each resource opts in through `AVAILABILITY_CONTROL`. Spindle
`INITIAL_AVAILABILITY` selects whether Machine On acquires it; a controlled
joint always starts released and is acquired explicitly after Machine On.
`AVAILABILITY_TIMEOUT` limits the hardware handshake. HAL
`availability-request` and `availability-ack` pins coordinate the handoff;
additional pins expose availability, lifecycle state, and fault information.
NML and Python provide acquisition/release commands and status for application
control.

Releasing an availability-controlled joint freezes its motion-state position;
its raw HAL feedback pin remains live for diagnostics. Nonvolatile acquisition
checks that raw feedback still matches the held logical position within the
joint's existing `MIN_FERROR` tolerance. A mismatch faults the operation without
changing the held joint or Cartesian position. If the joint is unhomed,
acquisition treats hardware acknowledgment as an internal step, automatically
homes the joint, and reports AVAILABLE only after homing succeeds.
`VOLATILE_HOME` controls whether release invalidates that reference. Release
waits for queued, jog, joint interpolation, and compensation state to settle
before inhibiting outputs and completing the hardware handoff; global
`MOTION_INPOS` keeps upstream behavior.

Availability-controlled joints cannot define `HOME_SEQUENCE`; configuring any
such joint rejects Home All. Controlled joints are homed individually only by
acquisition and reject user HOME and UNHOME commands, so AVAILABLE implies a
valid homed state.
Other resources can continue operating while a joint or spindle is released.
Coordinated and teleop motion are permitted only when inverse kinematics leaves
every unavailable joint at its held command position. Commands requiring an
unavailable spindle are rejected. Deliberately released joints do not block Run
or MDI solely because they are unhomed.

Direct API requests require an idle task, and only one acquisition or release
may be pending at a time. Abort cancels a pending operation; hardware handshake,
homing, or nonvolatile position-validation failures report a fault and inhibit
motion. Resources are managed independently, with no automatic pairing or
switching sequence. Patch 0018 adds the M54/M55 G-code interface to these
operations.

### 0018 — Standalone M54/M55 interface

Adds G-code commands for the runtime availability operations in patch 0017.
`M54` acquires a joint or spindle, and `M55` releases it. Programs can explicitly
hand resources to and from external hardware as part of an AUTO sequence or
through MDI.

Each command selects exactly one resource: `P` identifies a joint, and `$`
identifies a spindle. Selectors are zero-based integer indices, and the selected
resource must have availability control enabled. For example, `M54 P2` acquires
joint 2, while `M55 $0` releases spindle 0. Each command occupies its own block;
line numbers and comments are allowed, but other commands and parameter
assignments cannot share the block.

Execution waits for preceding work to finish before starting the handoff.
Acquisition then waits for hardware acknowledgment, homing if the joint is
unhomed, otherwise held-position validation, and normal task/interpreter
synchronization. Release waits for the resource to reach its released state.
Subsequent program blocks continue only after the operation completes
successfully; a fault stops execution. Abort cancels a pending operation through
the same runtime lifecycle as the direct API.

The commands use the configured availability policy and timeout for each
resource. They acquire and release resources independently, so the program
specifies the order of any multi-resource handoff. `REMAP` definitions can
replace either code with custom behavior and argument rules.
