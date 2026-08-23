# Design draft: Linux parity for the jailed Python worker process (ADR-081 Slice 1's own named
# residual)

**Status:** Design draft, self-red-teamed once (below) — **not an ADR, no code written, not built or
executed.** This machine is Windows-only for this session (no Linux dev/test environment available),
matching this repository's own existing posture for the *other* unbuilt Linux native-jail gap
(`docs/planning/linux-native-jail-pivot-root-containment-design-draft.md`, its own header: "document
what implementation needs, do not implement without the ability to prove it"). `ADR-081` closed the
Windows half of `docs/planning/2026-08-22-component-role-audit-tracker.md` Finding R and named "Linux
(`LinuxNativeJailBackend`) parity for the jailed-worker model does not exist" as an open residual —
this document is that residual's design phase, seeding a future ADR once a Linux build/test
environment is available. Per CLAUDE.md's own `decisions/README.md` rule, this deliberately stays out
of `decisions/` until it has real, executed evidence behind it (build, `ctest`, per-claim verdicts
decided by observed output) — a paper design alone does not meet that bar, and a file that doesn't
meet it gets removed from that directory outright rather than kept as an aspirational entry.

**Relates to:** `decisions/ADR-081-jailed-python-worker-process-slice-1.md` (the Windows design this
document ports), `decisions/ADR-004-appcontainer-native-jail-windows-backend.md` (the Windows
AppContainer/Job-Object primitives being mapped to their Linux analogues), the existing, already-real
`src/backends/native_jail/{linux_native_jail_backend.cpp, cgroup_limits.{hpp,cpp},
seccomp_filter.{hpp,cpp}}` (the per-exec Linux primitives this design reuses, not reimplements),
`docs/planning/linux-native-jail-pivot-root-containment-design-draft.md` (a sibling, still-unbuilt
Linux gap — filesystem/process containment for the *per-exec* path — genuinely independent of this
one: that draft's mount-namespace work benefits the worker process too, once built, but neither
depends on the other landing first).

## 0. What "parity with ADR-081" actually requires

ADR-081 §3 gives the Windows shape precisely: a persistent, per-session worker process
(`agentengine_python_worker.exe`), kernel-jailed via AppContainer + Job Object, communicating over two
anonymous pipes with a length-prefixed JSON wire protocol
(`mediated_python_worker_protocol.hpp`), governed by a session-scoped watchdog thread that gives a
real, externally-preemptible wall-clock kill and a real memory cap, with host-minted `exec_seq`
anti-replay (`RT1 Finding 1`), single-flight `call_mutex` (`RT1 Finding 2`), an idle-phase background
CPU budget (`RT2 Finding 1`), and a watchdog started before the guest's first instruction runs
(`RT2 Finding 4`). Parity means **all of that, unchanged in shape**, on Linux — not a redesign, and
not a subset. The only things that change are the OS-level primitives underneath:

| Windows (ADR-081, built) | Linux (this draft, unbuilt) |
|---|---|
| `CreateProcessW` + `SECURITY_CAPABILITIES` (AppContainer, zero capabilities) + `CREATE_SUSPENDED` → `assign_process()` (Job Object) → `ResumeThread()` | `clone(CLONE_NEWPID\|CLONE_NEWNET\|CLONE_NEWNS\|CLONE_NEWUTS\|CLONE_NEWIPC\|SIGCHLD)` (the existing per-exec flags, `linux_native_jail_backend.cpp:240`) → `CgroupLimits::add_process()` → release the child's sync-pipe block, matching the file's own existing sync-pipe idiom |
| `JobObjectLimits` (memory/pids/wall via Job Object + completion port) | `CgroupLimits` (`memory.max`/`memory.swap.max=0`/`pids.max`, already built and proven for the per-exec path) |
| Kernel backstop = zero AppContainer capabilities + restricted token | Kernel backstop = PID/net/mount/UTS/IPC namespaces + seccomp-BPF denylist (`install_seccomp_filter()`, already built, already installed in the per-exec `child_entry`) + `no_new_privs` |
| `TerminateJobObject` (single call, kills every process assigned to the job) | `cgroup.kill` (single write of `"1"`, kernel ≥ 5.14 — the real, modern one-shot "kill every process in this cgroup" primitive; see §2 item 4 for why this, not a `cgroup.procs` iterate-and-`SIGKILL`-each loop) |
| Job Object memory-limit completion-port notification, drained continuously by the watchdog | `memory.events`' `oom_kill` counter (already read by `CgroupLimits::query_usage()`), **polled** by the watchdog — cgroups v2 has no waitable "budget exceeded" handle, the same reason the existing per-exec `exec()` wait loop already polls rather than waits (`linux_native_jail_backend.cpp:266-268`'s own comment) |
| `JobUsage::total_user_time_100ns` (idle-phase CPU-budget accounting) | `CgroupUsage::cpu_usage_usec` (`cpu.stat`'s `usage_usec` field, already read) |
| `FramedChannel(HANDLE, HANDLE)` over anonymous pipes, `ReadFile`/`WriteFile` | `FramedChannel(int, int)` over `pipe2(..., O_CLOEXEC)` pipes (the existing per-exec path's own pipe-creation idiom), `read`/`write` with `EINTR` retry loops |
| `agentengine_python_worker.exe`'s `main()` parses `argv[1]`/`argv[2]` as decimal `HANDLE` values | `agentengine_python_worker`'s Linux `main()` parses `argv[1]`/`argv[2]` as decimal fd numbers — the identical convention, ported, not redesigned (see §1 item 2 for why this beats a fixed-`dup2`-target alternative) |

## 1. The design

1. **`LinuxNativeJailBackend::create_python_worker()`/`exec_session()`** — new methods, additive to
   the existing `create()`/`exec()`/`destroy()` (untouched), mirroring `NativeJailBackend`'s own
   additive relationship to its per-exec surface (ADR-081 §3). `PythonWorkerState`'s Windows-typed
   fields (`PROCESS_INFORMATION`, `HANDLE downstream_write`/`upstream_read`) get a Linux-side
   counterpart struct with `pid_t` and `int` fds — **a separate, platform-specific `Instance`/
   `PythonWorkerState` shape per backend class, not a templated or `#ifdef`-unified one**, matching
   this codebase's own established convention for this exact pair of classes (`NativeJailBackend`/
   `LinuxNativeJailBackend` are already two fully separate files, not one file branching on
   platform — `job_object_limits.hpp`/`cgroup_limits.hpp` are the same pattern one layer down). The
   host-side *logic* that must carry over byte-for-byte (not reimplemented, not reinterpreted) is
   `exec_session()`'s `exec_seq` check-before-dispatch (`RT1 Finding 1`), the `call_mutex`
   `try_to_lock` single-flight guard (`RT1 Finding 2`), and `dispatch_worker_query()`'s relay through
   the real host-side `bridge_tool_call()` — none of this is Windows-specific and all of it is
   security-load-bearing, so a Linux implementation that re-derives this logic independently (rather
   than porting the same control flow) is itself a red-team-worthy risk (§3 item 5).

2. **Worker process bootstrap.** The worker binary is `execve`'d directly from inside the cloned
   child (replacing the per-exec path's `/bin/sh -c <command>` — the worker needs no shell, it *is*
   the guest-facing process), after `install_seccomp_filter()` (§1 item 3) and cgroup join, exactly
   where the per-exec `child_entry` already does its own `execve` (`linux_native_jail_backend.cpp:113-117`).
   **The two pipe fds cross into the worker via the same decimal-argv convention Windows already
   uses** (`argv[1]`/`argv[2]` as ASCII-decimal fd numbers), not a fixed `dup2` target (e.g., always fd
   3/4). Chosen deliberately over the fixed-target alternative even though POSIX makes fixed targets
   easy: it keeps `python_worker_main.cpp`'s bootstrap logic structurally identical across both
   platforms (parse two argv values into "the platform's pipe-handle type," construct
   `FramedChannel`), which matters because everything *after* that line — the protocol loop, `run()`
   dispatch, `worker::initialize()` — is already fully portable C++ with no OS-specific calls (per
   direct inspection of `python_worker_main.cpp`'s control flow, the only Windows-specific lines are
   the `<windows.h>` include and the two `HANDLE` casts). A fixed-`dup2`-target design would save a
   few lines of argv parsing at the cost of the two platforms' `main()` diverging in shape for no
   security reason — not worth it for logic this security-sensitive, where "the two implementations
   read as the same program" is itself a review aid.

3. **Containment ordering, preserved from both existing designs.** The per-exec Linux path already
   proves the right sequencing: the child blocks on a sync-pipe `read()` immediately after `clone()`,
   the parent joins it to the cgroup, *then* releases the block — cgroup membership is established
   before any guest instruction runs. ADR-081's Windows worker gets the equivalent guarantee via
   `CREATE_SUSPENDED` + `assign_process()` + `ResumeThread()`, with the watchdog thread started
   *before* `ResumeThread()` (`RT2 Finding 4`). The Linux worker needs both properties at once: the
   existing sync-pipe block (cgroup membership before the guest's first instruction) **and** the
   watchdog thread started before the sync-pipe release (an init-phase hang has a kill path from the
   guest's very first instruction, the same property `RT2 Finding 4` established on Windows) — the
   watchdog thread must be spawned host-side before the parent writes the sync-pipe release byte, not
   after.

4. **Kill primitive: `cgroup.kill`, not a `cgroup.procs` iterate-and-signal loop.** `TerminateJobObject`
   is a single, atomic "every process assigned to this job dies now" call — the Linux equivalent
   needs the same atomicity, because the worker is PID 1 of its own new PID namespace (per the
   existing `clone()` flags) and could, in principle, have spawned children of its own before a kill
   arrives (defense-in-depth: `python_worker_mediation.cpp`'s import/os-mediation should already deny
   `os.fork`/`os.exec`/`subprocess`, the same claims `E4-PY5`/`E4-PY6` already prove for the in-process
   embed and that the ported mediation engine must re-prove for the jailed worker specifically, §3
   item 3 below). Reading and `SIGKILL`-ing each PID from `cgroup.procs` in a loop races against a
   process forking a new child between the read and the kill — genuinely worse than the Windows
   primitive it would be replacing. `cgroup.kill` (a single `write()` of `"1"`) closes that race by
   construction; it requires **kernel ≥ 5.14**, a real deployment precondition this design should
   state plainly (matching `cgroup_limits.cpp`'s own existing "kernel ≥ 5.19 for `memory.peak`,
   fall back to `memory.current` on older kernels" disclosed pattern) rather than assume. **Named open
   question for the real implementation, not resolved here**: whether to add a `memory.current`-style
   graceful fallback for pre-5.14 kernels (loop-and-signal, accepting the race as a disclosed residual
   matching ADR-041/ADR-071's own disclosure norm) or to fail `create_python_worker()` closed on a
   kernel too old to have `cgroup.kill` — the latter is the more defensible default given this is a
   security boundary, not a resource accounting nicety, but the actual minimum-kernel-version policy
   is a real decision the prove phase, not this draft, should make on measured evidence.

5. **Watchdog thread — poll-based, same shape as the existing per-exec wait loop, generalized to a
   session lifetime.** No waitable "budget exceeded" handle exists on Linux (the existing per-exec
   `exec()` comment already states this reason for why it polls); the persistent worker's watchdog
   thread polls `CgroupLimits::query_usage()` (`memory.current`/`memory.peak` for the memory axis via
   `memory.events`' `oom_kill` counter, `cpu.stat`'s `usage_usec` for the CPU axis) on the same
   interval discipline ADR-081's Windows watchdog already uses (5ms during `call_active`/
   `awaiting_init`, 100ms during `idle`) — Linux forces polling for the per-exec path already, so this
   is not a new cost class, just a longer-lived poller. The `idle`-phase background CPU budget
   (`RT2 Finding 1`'s fix) ports directly: track `cpu_usage_usec` at each idle-window start, compare
   the delta against `idle_cpu_budget_ms`, exactly as `session_watchdog_loop`'s Windows
   implementation does against `total_user_time_100ns`.

6. **`FramedChannel` needs a Linux-typed sibling** (`FramedChannel(int read_fd, int write_fd)`),
   same length-prefixed-JSON framing and `kMaxFrameBytes` cap, `read`/`write` with `EINTR`-retry loops
   in place of `ReadFile`/`WriteFile`'s partial-write loop — a new file
   (`posix_jailed_worker_rpc.{hpp,cpp}`, or platform-specific translation units behind the same
   `jailed_worker_rpc.hpp` name via the build's existing per-platform source-selection, matching how
   `native_jail_backend.cpp`/`linux_native_jail_backend.cpp` are already separate TUs selected by
   `CMakeLists.txt` rather than one `#ifdef`-branched file), compiled only into the Linux
   `agentengine_python_worker` binary and `agentengine_native_jail_backend`'s Linux build.

7. **`python_worker_mediation.{hpp,cpp}` (1022 lines) needs a portability audit, not necessarily a
   rewrite.** This is the C-level import allowlist / file-I/O mediation engine that now runs *inside*
   the worker process on Windows. Whether it is already platform-portable (pure CPython C-API +
   `agentengine::json`, no `<windows.h>`) or has Windows-specific paths (e.g., drive-letter/backslash
   path handling inherited from `native_jail_win32_helpers.hpp`'s `widen`/`narrow`) is **not
   determined by this draft** — a real implementation pass must read this file specifically before
   estimating scope, since it is by far the largest file in the worker's own dependency graph and the
   one most likely to hide platform assumptions that were never a problem when the *host* process
   (already Windows-only for this whole subsystem) was the only place it ran.

## 2. Self red-team pass

1. **Does the seccomp denylist (already installed, already proven for the short-lived per-exec case)
   actually work for a long-running event-loop process?** Read `seccomp_filter.hpp` directly for this
   draft: it is a **denylist** (namespace/mount manipulation, kernel module loading, `ptrace`, and
   similar unconditionally-dangerous syscalls), explicitly *not* an allowlist — the header's own
   comment states this shape was chosen specifically so it does not need re-verifying against every
   syscall a legitimate guest workload might need (008 §1b's "blocklists rot" critique is aimed at
   layers 1-2, not this layer). A persistent process's ongoing `read`/`write`/`poll` loop, CPython's
   own `mmap`/`brk`/`futex` calls, none of these are namespace/mount/module/ptrace syscalls — **on
   paper, the existing filter should already be compatible with a long-running worker with no
   changes**, but this is a claim about *design* compatibility only; nothing here executes it. Real
   verification (does a persistent CPython process actually run under this filter without hitting a
   denied syscall in practice) is prove-phase work, not something a design draft can claim as CORRECT.
2. **Is the worker, as PID 1 of its own PID namespace, a reaping hazard for orphaned grandchildren?**
   Genuinely Linux-specific — Windows Job Objects have no equivalent shape. If the CPython interpreter
   inside the worker ever spawned a subprocess (via `os.fork`/`subprocess`/similar) and that child
   later became orphaned, PID-namespace semantics make PID 1 (the worker itself) responsible for
   reaping it; an unreaped zombie doesn't escalate to a security issue on its own, but a worker that
   never reaps could, over a long enough session, accumulate zombies. **Mitigated in the common case
   by an existing, separate control**: `os.fork`/`os.exec*`/`subprocess` denial is already proven for
   the in-process embed (`E4-PY5`/`E4-PY6`, `test_mediated_python_runner_hostile_corpus.cpp`) — if
   that denial genuinely carries over unchanged to the ported mediation engine (§1 item 7's own open
   question), no guest-spawned grandchild can exist in the first place, and this finding is moot in
   practice. **Named as a real, not-yet-closed gap regardless**: the *mediation engine's own*
   subprocess denial is the single control this reasoning depends on — if it has any gap (a bypass
   this design doesn't currently know about), the PID-1-reaping hazard becomes live. A real
   implementation should add a `waitpid(-1, ..., WNOHANG)` reap sweep to the watchdog's own poll loop
   as defense-in-depth regardless, cheap to add and closes the hazard even if the mediation engine's
   denial were ever found to have a gap.
3. **Does the ported mediation engine's import/os denial actually hold, or was some of what looked
   like "engine-level" denial actually relying on something Windows-specific?** A real risk this draft
   cannot resolve on paper: `PythonLockdownInterpreter`'s original meta-path-finder-based import
   allowlist (ADR-002/ADR-003) is CPython-level, not OS-level, so it should be platform-neutral by
   construction — but the *file-I/O* mediation half (`MediatedFileSystemAdapter`, ADR-014) has real
   POSIX/Windows path-handling differences (`core/worktree_mount_fs_posix.hpp` already exists as a
   separate POSIX implementation per the sibling pivot-root design draft's own §0 findings) that the
   ported `python_worker_mediation.cpp` must actually reuse, not reimplement, or the Linux worker
   risks a *weaker* file-boundary than the Windows one for reasons unrelated to the process-jail work
   this document is actually about. Flagged as a scope-boundary risk for the prove phase to check
   explicitly, not assumed resolved by this draft.
4. **Does `argv`-based fd-number passing leak anything a fixed `dup2` target wouldn't?** No — both are
   host-controlled values in a process the host itself spawned; there is no guest-influenced input on
   this path either way (I2 is not implicated by this choice). This finding exists only to record that
   the choice in §1 item 2 was actually considered as a security question, not merely a style
   preference, and closed on those grounds.
5. **Does re-deriving `exec_seq`/`call_mutex`/idle-CPU-budget logic independently for
   `LinuxNativeJailBackend` (rather than sharing it with `NativeJailBackend`) risk the two platforms
   silently drifting on a security-load-bearing property?** Real risk, named plainly: this whole
   subsystem's established convention (separate classes per platform, no shared template) means there
   is no compiler-enforced guarantee the Linux port's `exec_session()` actually re-implements
   `RT1 Finding 1`/`RT1 Finding 2`'s fix correctly rather than a subtly different, weaker variant — the
   only guard against that is the same kind of test-per-platform proof ADR-081 §4 already ran for
   Windows (a real `exec_seq`-mismatch test, a real concurrent-call test), which a Linux prove phase
   must run independently, not infer from the Windows evidence already existing.

## 3. What promotes this to a real ADR

Per this project's own bar (`decisions/README.md` §"What an ADR must contain"): real implementation
of §1 above, a real Linux build (`gcc-14`, this project's established second-compiler target),
`ctest` evidence mirroring ADR-081 §4's own shape — the design's own positive controls
(busy-loop → timeout, memory-bomb → contained, post-kill → `session_terminated` fail-fast) re-run
directly on Linux, not inferred from the Windows results — a real teardown-cycle census (mirroring
`test_native_jail_teardown_cycles_linux.cpp`'s existing pattern) checked against cgroup-directory and
fd counts specifically for the worker-process lifecycle, and the two named open questions in §2
(seccomp compatibility with a long-running process, and whether the ported mediation engine's
subprocess/file-boundary denial genuinely matches the Windows original) resolved by observed test
output, not by this document's own reasoning. Until then, this stays a design draft.
