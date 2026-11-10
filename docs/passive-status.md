# Passive local status snapshots

`linuxcnc-status-snapshot CONFIG` is a separate, bounded local status reader
using the opt-in `CMS_SHMEM_SNAPSHOT` API in libnml. Existing message layouts
and default client/writer locking are unchanged. It never falls back to an
ordinary NML or Python stat channel. It uses actual CMS decoding and the
`EMC_STAT` value owner, without tool-table or HOME-based mappings.

Configuration probes the matching header and library with a compile-and-link
check; stale header/library combinations fail. Without the API header, an
explicit unavailable implementation exits unsuccessfully before opening a
channel. Installation uses `CMAKE_INSTALL_LIBEXECDIR`, normally `/usr/libexec`
with a `/usr` prefix.

## Access ownership

The API privately owns an existing SysV attachment made with kernel
`SHM_RDONLY`. It checks the complete buffer name and segment size without
writing shared storage, and never creates, resets or removes IPC. Only raw,
single-message, unqueued, nondiagnostic PEEK is supported. The writer must use
the same OS semaphore for its complete update. Inherited read/write/clear paths
are rejected at the access owner, including when private inheritance is
bypassed. Name, size and CMS message bounds are rechecked under this reader's
own hold before CMS decoding.

Every successful take uses `SEM_UNDO`; normal release uses matching
`SEM_UNDO` to cancel the adjustment. Failed or timed-out takes grant no right
to release. Kernel process termination restores only an outstanding adjustment
owned by that reader. Removed/stale IDs and detected unexpected owners are
terminal faults. GETPID is fault detection only: it is not atomic with SETVAL
and cannot make arbitrary concurrent resets safe. Release posts only while the
value is still zero, in the same atomic operation, so it never makes the value
non-binary; an external reset after the check makes release a terminal fault,
and exit undo still applies the reader's outstanding adjustment. The
coordinated lease excludes such resets through release and detach. There is no manual
compensation or release against a replacement ID. Use a dedicated bounded
process and terminate it on terminal faults. Do not fork, exec or share its
open-file description while attached or after a terminal fault; the lease and
SEM_UNDO belong to the same process. If release or detach fails, its
lease and attachment remain held until process exit.

## Initialization lifecycle

A passive status buffer requires exactly one explicit
`SNAPSHOT_LOCK=/absolute/path` option. Configured SHMEM masters with this option
take an exclusive flock lease before any IPC create, initialization or reset.
The wait is bounded to two seconds; failure rejects construction without
opening IPC. Ordinary steady-state writers/nonmasters and configurations
without the option retain their existing semantics. When the option is
present, every SHMEM teardown also takes the exclusive lease, bounded to two
seconds, while it counts attachments and removes IPC, so an attached reader
cannot make the last process leave the segment and semaphore behind. If that
lease cannot be taken, teardown proceeds as before. Passive readers take a
shared lease, bounded to 50 ms, before attaching, and hold it through their own
semaphore release and shared-memory detach. Normal destruction releases and
detaches before closing the lease. Process death requires Linux to apply
semaphore undo and detach shared memory before closing flock descriptors.

Masters and teardown wait for shared leases to end, and flock does not queue an
exclusive waiter ahead of new shared holders. Keep every observation's lease
short and bounded, as the executable's one-second alarm does; a stopped or
traced reader, or continuously overlapping readers, can make master startup
fail after its two-second wait. Readers and the master must read the same
configuration file so they agree on the lease.

The integration owner provisions one stable empty regular inode on local Linux
tmpfs before either participant starts. The path may contain only ASCII letters,
digits, `/`, `.`, `_` and `-`, and must not contain `MAO_W_OS_SEM` or
`AUTOCNUM` in any case, because libnml matches those buffer-line options as
substrings. Anyone who can open the file can hold a lease, so grant read access
only to the participants. Its owner must be root or the
participant's effective UID. Group/other writes and unsafe writable parents
are rejected; the containing directory must prevent unauthorized replacement.
Path components are opened without following symlinks. O_PATH checks type
before a read-only open of the pinned inode, avoiding FIFO/device open effects.
The selected inode is rechecked after flock. The library does not create,
repair, truncate, replace or remove the file. Ordinary start/restart/stop must
preserve its inode. Every initializer of that IPC must use the same contract.
Hardcoded constructors, external SETVAL tools and initializers omitting or
changing the option must not run concurrently with observations. Arbitrary
privileged inode replacement or IPC mutation is unsupported.

## Configuration and output

The API admits a bounded canonical subset: unique explicit `B emcStatus` and
`P xemc emcStatus` rows, fixed local hosts and
`LOCAL R 0 TIMEOUT 0 CONNECTION`. Relevant defaults, ambiguous selection,
alternate record spellings, leading whitespace, continuations, inline comments
and unrecognized options are rejected. Integer fields use bounded canonical
decimal; the positive SysV key fits signed key_t without truncation. Optional
TCP-port and XDR annotations may describe a server, but this reader opens no
network port. Configuration is read once into bounded private storage. A caller
may inherit a sealed configuration at `/proc/self/fd/N`, retaining that exact
fd through exec and the complete observation.

Success writes one bounded line:

```text
linuxcnc-status-v1 TASK_HEARTBEAT TASK_STATE IN_POSITION INTERPRETER_STATE INI_UTF8_HEX
```

Heartbeat is unsigned decimal; states are LinuxCNC integer enums; in-position
is `0` or `1`; the bounded INI filename is lowercase hexadecimal UTF-8 bytes.
Failures write a bounded diagnostic to stderr and return nonzero without a
success snapshot. The executable requires heartbeat advancement within 200 ms.
Each semaphore wait is at most 50 ms. An independent one-second alarm and
parent-death signal bound the process, including cancellation while holding a
semaphore. Callers may impose a shorter process-group deadline.

Consistent local snapshots do not protect against unauthorized writers, a
modified kernel or a compromised native process.

## Validation

`linuxcnc-passive-status-fixture` publishes ordinary EMC_STAT messages through
the real NML writer. `tests/test_passive_status.py` uses private user and IPC
namespaces to check kernel read-only enforcement, foreign storage preservation,
no IPC creation, fresh repeated snapshots without tool mappings, admission
faults, normal release, removed IDs and detected ownership faults, and forced
termination both while owning and waiting for a hold. The last master exiting
while a reader is attached must leave no segment or semaphore. Actual configured SHMEM
master initialization waits behind a live reader. Its first shmget is delayed
after EX acquisition to check that release/undo and detach are complete before
initialization. The release-window fixture delays the post syscall after ownership checks and
confirms that the real initializer still cannot reset. Master timeout/death and
reader refusal during master EX are
also exercised. Only exact fixture resources are removed.

Host CTest registers these effects when the passive API is enabled; missing
private namespaces or strace causes an explicit skip. For cross-built binaries,
set `LINUXCNC_SNAPSHOT_READER`, `LINUXCNC_SNAPSHOT_FIXTURE` and JSON
`LINUXCNC_SNAPSHOT_EXEC_PREFIX` emulator arguments and run the same Python
suite. Invoke the target loader explicitly rather than QEMU `-L` when checking
absolute directory traversal: QEMU's prefix otherwise redirects opening `/`
into the sysroot. These are host-kernel effects with host/emulated binaries,
not execution on the target kernel.
