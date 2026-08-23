# ADR-081 — Does routing `execute_code` through a real, separately-jailed worker process close the
# gap between what 008/010 require for T3 code and what the embedded CPython interpreter actually
# ran with, without reopening I2/I3 or regressing the existing native-jail Windows suite?

**Status:** Judged (design → red-team → prove complete for the Windows Slice 1 scope; independently
re-built and re-run on this machine for this ADR, 2026-08-23). Formalizes, as a numbered decision
record, work already implemented and merged in commit `a60fc3d` under project-owner direction; no
new code changes in this ADR.

**Relates to:** `decisions/ADR-004-appcontainer-native-jail-windows-backend.md` (Spiked, not
Judged — the AppContainer + Job Object primitives this worker process reuses, and the `cpu_ms`
best-effort / `wall_ms`-is-the-dependable-bound finding this design inherits directly),
`decisions/ADR-041-appcontainer-ace-leak-accepted-residual.md` (the accepted Windows ACL residual
this design does not change), `decisions/ADR-071-native-unsandboxed-process-execution-providers.md`
(a structurally distinct, deliberately weaker Tier-2 backend this ADR's worker process is not a
variant of — see §2), `decisions/ADR-014-worktree-mount-path-canonicalization.md` (the
`open_within_mount_root` mount-boundary check this design's Slice 2 residual, not yet built, would
need), `008-Sandbox-and-Isolation.md` §1b/§3 (the three-layer interpreter isolation model and the
"locked to `native-jail` permanently" requirement this ADR closes the layer-3 gap against),
`010-Python-Code-Interpreter.md` §2/§6 (the T3 trust-tier classification `execute_code` runs at),
`docs/planning/2026-08-22-component-role-audit-tracker.md` Findings Q and R (the audit that found
and traced this gap — the primary source record for this ADR's design/red-team phase; see §2's
disclosure on what is and is not preserved from that pass).

## 1. The question

**Stated so it has a wrong answer:** `008-Sandbox-and-Isolation.md` §1b states plainly that "relying
solely on interpreter mediation has no answer for a bug in the mediation itself" and names a
kernel-level jail as the mandatory third layer for the code interpreter; §3's profile table defines
`native-jail`'s boundary as "Kernel-enforced... **plus** mediated at the point of use," and states
the interpreter "is locked to `native-jail` permanently." `010-Python-Code-Interpreter.md` §6 states
code executed via `execute_code` runs at the sandbox's trust tier, **T3** — the least-trusted tier,
by the RFC's own vocabulary exactly what `none`-profile in-process execution is defined to refuse.

As of the 2026-08-22/23 audit (tracker Findings Q/R), the shipped code did not meet this: the
embedded CPython interpreter (`PythonLockdownInterpreter`/`MediatedPythonRunner`) ran in-process
with the host — sharing its address space, its `AgentSession` state, and any capability/secret
material resident in that process's memory — under no `SandboxBackend`, no Job Object, no cgroup, no
namespace, and no preemptible deadline (the one deadline check that existed was pre-flight only,
`tool_pipeline.hpp:579-583`). A same-day code comment on `native_jail_backend.hpp` had asserted this
gap was permanently resolved by interpreter-level mediation alone, without any ADR amending 008 §3's
or 010 §6's explicit text — the tracker calls this out directly as an unreviewed reinterpretation of
a security-critical requirement.

**The actual design question is narrower than "add a resource cap":** can `execute_code` be routed
through a real, separately-jailed OS process — closing both the missing kernel backstop (008 §1b
layer 3) and the missing preemptible resource ceiling (Machine Safety) — using only the
already-proven AppContainer + Job Object primitives this codebase already has (ADR-004), without
(a) reopening I2 by letting the worker process reach host state or capabilities it wasn't handed,
(b) reopening I3 by trusting anything the worker process claims about its own kill reason or state,
or (c) silently widening scope to also solve the still-separate `execute_shell` process-spawn
question (tracker Finding O) in the same pass.

## 2. Provenance and what this ADR does and does not reconstruct

**This ADR formalizes work that already happened**, per project-owner direction recorded in the
tracker (`2026-08-22-component-role-audit-tracker.md`, "Update (2026-08-23)"): a real
design → red-team → prove pass — 2 independent design candidates merged by a judge pass, 3 parallel
red-team lenses (authority-leak, resource/DoS/lifecycle, parity/buildability), a finalized
build-ready spec, then implementation and tests — run as an agent workflow whose own transcript is
not preserved as a repository artifact. What survives in the tree is: the tracker's own prose account
of the process and outcome, the shipped code itself, and inline comments citing specific findings by
label (`RT1 Finding 1`, `RT1 Finding 2`, `RT2 Finding 1`, `RT2 Finding 4`) at the exact lines those
findings' fixes live.

**Disclosed limitation, not silently patched over:** the two competing design candidates' own text —
what was rejected and why, beyond what the shipped design's own comments say about itself — is not
recoverable from this repository. CLAUDE.md's ADR template calls for competing designs "each
steelmanned"; this ADR cannot do that for the rejected candidate because no record of it survived
the workflow that produced it. What this ADR *can* and does do, meeting the same evidentiary bar a
normal ADR's §5/§6 sections require: independently re-verify the shipped design's own falsifiable
claims against the real, current code and a real build (§5), and reconstruct the red-team record from
the specific, labeled findings actually embedded as comments at their fix sites (§6) rather than
inventing findings that cannot be checked against anything. Per CLAUDE.md's own README.md rule for
this directory — a record that turns out not to meet "what an ADR must contain" gets removed, not
kept — the honest path here is disclosure, not a reconstructed-from-nothing steelman for a design
nobody can now check.

**Distinctness from ADR-071, stated once and not repeated in §6:** `NativeShellProvider`/
`NativeBashProvider`/`NativePythonProvider`/`NativeNodeProvider` (ADR-071) are a deliberately
*weaker*, unsandboxed, host-opt-in Tier-2 backend for a different job (native host-installed-package
automation, explicitly never substituted for `execute_code`). The worker process this ADR covers is
the mediated code interpreter itself, strengthened, not a new capability class. The tracker's own
Finding Q named the resulting asymmetry (the weaker Tier-2 backend had Job Object resource caps
before the interpreter did) as evidence the two were "built independently and never reconciled," not
as evidence either was wrong on its own — this ADR is that reconciliation for the interpreter's own
axis, and does not touch `native_process/`.

## 3. The shipped design

A per-session Python worker process, spawned and owned by
`NativeJailBackend::create_python_worker()` / `exec_session()`
(`src/backends/native_jail/native_jail_backend.{hpp,cpp}`), replacing the prior in-process embed:

- **Process boundary.** `agentengine_python_worker.exe`
  (`src/backends/native_jail/python_worker_main.cpp`) is launched via `CreateProcessW` with
  `SECURITY_CAPABILITIES{AppContainerSid, CapabilityCount=0}` (zero capabilities — the same
  ADR-004-proven AppContainer configuration) and `PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`,
  assigned to a `JobObjectLimits`-backed Job Object via `CREATE_SUSPENDED` + `assign_process()` +
  `ResumeThread()` — every `ResourceLimits` axis applies from the guest's very first instruction, the
  same idiom the existing per-exec `NativeJailBackend::exec()` path already used and proved (ADR-004).
  Handle inheritance is hardened relative to the per-exec path: only the two pipe ends the worker
  actually needs are named in `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, host-side ends are marked
  non-inheritable, and the environment block is the existing fixed minimal block (never the host's
  ambient environment).
- **Wire protocol.** `mediated_python_worker_protocol.hpp` defines the message catalog
  (`init_request`/`init_response`/`exec_request`/`exec_response`/`worker_query`/
  `worker_query_response`/`refresh_tools_request`/`refresh_tools_response`/`shutdown`) exchanged over
  two anonymous pipes via `jailed_worker_rpc.{hpp,cpp}`'s `FramedChannel`. Every `exec_request` and
  `worker_query`/`worker_query_response` carries an `exec_seq` — a host-minted, monotonically
  increasing counter, never worker-chosen.
- **Session-scoped watchdog** (`session_watchdog_loop()`, its own dedicated thread per worker,
  started *before* `ResumeThread()` so an init-phase hang is covered from the guest's first
  instruction — `RT2 Finding 4`'s fix): continuously drains the Job Object's memory-limit
  notification (`poll_memory_limit_once()`, not just inside a per-call wait — closing the
  "stale-notification" gap a call-scoped-only drain would have), tracks a `phase_deadline` for
  `awaiting_init`/`call_active` and enforces it with `TerminateJobObject`, and — for the `idle` phase
  specifically — enforces a *background* CPU budget per idle window (`idle_cpu_window_ms`/
  `idle_cpu_budget_ms`) against `JobUsage::total_user_time_100ns`, closing `RT2 Finding 1`: a
  guest-spawned daemon thread that outlives the call which spawned it would otherwise pin CPU forever,
  undetected, once no `exec_request` is outstanding.
- **Single-flight and anti-replay** (`exec_session()`): a `std::try_lock` on `call_mutex` rejects a
  second concurrent call on the same handle outright (`RT1 Finding 2`) rather than queuing or
  interleaving it. Every `worker_query` received while an `exec_request` is in flight is checked
  against the *currently active* `exec_seq`; a mismatch — replayed, queued, or otherwise misattributed
  — is **never dispatched**: the worker is terminated outright, not trusted to self-correct
  (`RT1 Finding 1`). The same fail-closed path fires for any unexpected frame type or a mismatched
  `exec_seq` on an `exec_response`.
- **I2 — no live authority crosses the process boundary.** A `call_tool` `worker_query` is relayed
  through the *host's own* `bridge_tool_call()` (`dispatch_worker_query()`), the same tool pipeline
  every other tool call in this codebase goes through — the worker process never receives a
  `ToolDescriptor` (a live closure over host state); only the *rendered Python source text*
  `generate_agent_tools_module_source()` produces from it, built host-side before `init_request` is
  ever sent.
- **I3 — nothing the worker claims about itself is trusted.** An `exec_session()` frame-recv failure
  (broken pipe) is classified from the *watchdog's own* observed `kill_reason` — a real, external,
  host-side signal — never from anything the worker process itself might have said.
- **Fail-closed, no silent respawn.** Once `ws.alive` is false (a watchdog kill or a protocol
  violation), every subsequent `exec_session()` call on that handle fails immediately with
  `native_jail.session_terminated` — proven, not merely asserted in a comment (§5).
- **`mediated_python_runner.cpp` is now a thin IPC client** — reduced from 1561 lines to 97, and no
  longer includes `Python.h` at all (confirmed by direct inspection of the current file); the C-level
  import allowlist, module mediation, and file-I/O mediation (`python_worker_mediation.{hpp,cpp}`,
  1022 lines) now run **inside** the jailed worker process, not the host.

**Explicitly, narrowly out of scope for this pass, matching the finalized spec's own minimum-slice
boundary (not a silent reduction):**
- **Slice 2 — real file/listdir/socket relay** (`HandleRelay` for the `open`/`listdir`/
  `connect_authorize`/`connect_send`/`connect_recv`/`connect_close` `worker_query` kinds). Every such
  call is denied today (`not_implemented_this_slice` → a guest-visible `PermissionError`/
  `OSError`), not silently permitted and not crashing.
- **Linux (`LinuxNativeJailBackend`) parity.** This pass is Windows-only.
- **`execute_shell`'s own, still-separate process-spawn question** (tracker Finding O) — deliberately
  not unified with this fix in this pass.

## 4. Independent re-verification (this ADR's own evidence pass)

Per this project's stated discipline ("independently re-verified by the judging pass, not taken on
self-report" — ADR-079's own precedent), the claims below were re-built and re-run directly for this
ADR, not taken from the commit message alone.

**Build.** `cmake --build . --config Debug` (MSVC, Windows) for
`test_native_jail_python_worker_slice1` and the 15 other native-jail/mediated-Python-runner test
executables below: clean build, zero errors.

**`test_native_jail_python_worker_slice1` (the design's own G-Q1/G-Q2/G-lifecycle-2 positive
controls), run directly, this pass:**

| Claim | Disproving experiment | Result | Verdict |
|---|---|---|---|
| An infinite busy-loop (`while True: pass`) under a short wall budget is actually preempted mid-call, not merely pre-flight-checked. | 800ms `exec_wall_ms` budget against a real busy-loop; assert `timeout` classification and bounded elapsed time. | Measured **804ms** (budget 800ms) — killed by the watchdog's `call_active`-phase deadline, not a hang. | **CORRECT** (G-Q1). |
| After a watchdog kill, the same handle fails closed — no silent respawn. | Issue a second, trivial call (`1 + 1`) on the same handle immediately after the kill. | Failed fast with `native_jail.session_terminated`, well under the 2000ms bound. | **CORRECT** (G-lifecycle-2, `RT2 Finding 5`'s documented contract). |
| A 512MiB allocation under a 64MiB Job Object memory cap never completes as an unconstrained success. | `bytearray(512*1024*1024)` under `memory_bytes=64MiB`. | Never printed `ALLOCATED_OK`; the still-alive worker's own allocator raised `MemoryError` — the cap acted before the guest could use the memory, one of the two design-acceptable containment outcomes. | **CORRECT** (G-Q2). |

All 3 checks pass, 0 failures — `test_native_jail_python_worker_slice1: ALL PASS`.

**Regression sweep against the pre-existing native-jail suite, run directly, this pass** (the 10
tests unaffected by Slice 1's scope):

```
 1/10 test_native_jail_runner_stubs .................. Passed  0.01s
 2/10 test_native_jail_backend_windows ................ Passed  5.11s
 3/10 test_native_jail_abuse_corpus_windows ........... Passed  5.24s
 4/10 test_native_jail_parity_windows ................. Passed 10.13s
 5/10 test_native_jail_ambient_authority_windows ...... Passed  3.58s
 6/10 test_native_jail_teardown_cycles_windows ........ Passed  4.73s
 7/10 test_native_jail_python_worker_slice1 ........... Passed  1.06s
 8/10 test_mediated_python_runner_agent_tools ......... Passed  0.49s
 9/10 test_agent_session_suspend_codeact_ask .......... Passed  0.87s
10/10 test_reference_agent_containment_invariance ..... Passed  0.14s
100% tests passed, 0 tests failed out of 10
```

`test_mediated_python_runner_agent_tools` confirms the `call_tool`/`agent.tools` relay round-trips
through the real `bridge_tool_call()` path end to end via the worker process, not merely by
inspection. `test_agent_session_suspend_codeact_ask` confirms the B7 fix (§5's own account below)
holds. `test_native_jail_backend_windows`/`_abuse_corpus_windows`/`_parity_windows`/
`_ambient_authority_windows`/`_teardown_cycles_windows` — the existing ADR-004/M2-Phase-C proof suite
for the per-exec `NativeJailBackend` path this design leaves byte-for-byte untouched — show zero
regression from adding the worker-process surface alongside it.

**The disclosed Slice 2 regression, reproduced directly, this pass** (6 test files depending on real
file/socket relay, run to confirm the failures are clean denials, never a crash or hang):

```
63% tests passed, 6 tests failed out of 16
	test_mediated_python_runner_smoke
	test_mediated_python_runner_agent_files_data
	test_mediated_python_runner_skill_mounts
	test_mediated_python_runner_error_mapping
	test_reference_agent_task_corpus
	test_mediated_python_runner_hostile_corpus
```

Confirmed by direct inspection of the output: every failure is a specific, named assertion mismatch
(e.g. `test_mediated_python_runner_hostile_corpus`'s own **positive control**, "the mediated `open()`
still reads the real, untouched file after every `os.*` denial above," fails because Slice 2's file
relay isn't built yet — the test's own design correctly detects the gap it was written to detect) —
never a timeout, crash, or silently-wrong success. This matches the commit's own disclosure exactly:
6 files, clean fails, named as Slice 2's scope boundary rather than a hidden regression. One test in
this same file family, `test_mediated_python_runner_agent_tools` (call-tool relay, not file/socket
relay), is unaffected and passes — consistent with the design's claim that only the file/socket
`HandleRelay` axis, not the tool-call axis, was deferred.

**The B7 fix, independently reproduced.** `test_agent_session_suspend_codeact_ask`'s B7 scenario
(a residual-demonstration test for ADR-057 §4, unrelated in its own original intent to this
redesign) previously called `.front()` unconditionally on a vector of open Interactions; once Slice 1
made the write it depends on fail closed pre-`agent.ask()`, no Interaction ever opened and `.front()`
on an empty vector was UB — invisible because the file never called
`support/crt_fail_fast.hpp`'s `fail_fast_on_windows()`, unlike every other native-jail test in the
suite. Fixed to assert the empty-vector case explicitly and call `fail_fast_on_windows()`; the
original ADR-057 §4 residual claim is preserved in a comment for restoration once Slice 2 lands. This
pass's own run above confirms the fixed test passes in 0.87s (no 60s hang).

## 5. The red-team record (reconstructed from labeled findings actually embedded at their fix sites)

Three lenses per the tracker's own account — authority-leak, resource/DoS/lifecycle,
parity/buildability. What follows cites the specific comment and code at each finding's fix site,
independently located and read for this ADR, not restated from memory of the commit message.

**Authority-leak lens (`RT1`):**
1. **`RT1 Finding 1`** — a `worker_query` (e.g. a `call_tool` request) carrying a stale, queued, or
   otherwise misattributed `exec_seq` could be dispatched against the *wrong* in-flight call if the
   host trusted the worker's own claim about which call it belongs to.
   **Fix, verified at the fix site** (`native_jail_backend.cpp:858-872`): the host checks
   `exec_seq` against its own `active_exec_seq` before ever dispatching; a mismatch terminates the
   worker outright rather than attempting recovery. `mediated_python_worker_protocol.hpp:59-62`
   documents the same contract from the wire-format side: "a protocol violation observed by the
   host... the worker is terminated, never trusted to self-correct."
2. **`RT1 Finding 2`** — a second concurrent `exec_session()` call on the same handle (e.g. from a
   racing caller) could interleave with an in-flight call against the same single worker process and
   its single set of pipes. **Fix, verified at the fix site** (`native_jail_backend.cpp:803-809`):
   `std::try_lock` on `call_mutex`; a caller that loses the race is rejected immediately with
   `native_jail.already_in_progress`, never queued or interleaved.

**Resource/DoS/lifecycle lens (`RT2`):**
1. **`RT2 Finding 1`** — a guest-spawned background/daemon thread that outlives the `exec_request`
   which spawned it would pin CPU indefinitely, undetected, once the watchdog's `call_active`-phase
   deadline no longer applies (the call already returned). **Fix, verified at the fix site**
   (`native_jail_backend.cpp:445-467`): a background CPU budget enforced specifically during the
   `idle` phase, against `JobUsage::total_user_time_100ns` measured from the start of each idle
   window — independent of, and in addition to, the `call_active`-phase wall-clock deadline.
2. **`RT2 Finding 4`** — an init-phase hang (the worker process wedges before ever sending
   `init_response`) would have no kill path if the watchdog only started after `ResumeThread()`.
   **Fix, verified at the fix site** (`native_jail_backend.cpp:625-630`): the watchdog thread is
   started *before* `ResumeThread()`, so `phase_deadline` (set to `init_timeout_ms`) covers the
   guest's very first instruction, not just the post-`init_response` period.
   (`RT2 Finding 5`, cited by the test file's own comment as governing "no silent respawn" after a
   kill, is verified directly in §4's G-lifecycle-2 result above.)

**Parity/buildability lens:** the tracker's and commit's own account states this lens's disposition
plainly rather than naming a numbered finding fixed in code: Linux (`LinuxNativeJailBackend`) parity
was found to be out of reach within this pass's scope and was named as a real, explicit residual
(§3) rather than silently assumed covered or forced into scope under time pressure — a legitimate,
disclosed outcome for this lens, not a gap in the process.

**This ADR's own additional check, not previously reported:** whether the disclosed Slice 2
regression is real and clean (never a crash, hang, or silently-wrong success) was independently
verified, not taken on the commit message's word — see §4's regression-sweep result.

## 6. The decision

**Adopted, as already implemented.** This ADR formalizes the design in §3 as Judged, binding:
- `src/backends/native_jail/{python_worker_main.cpp, jailed_worker_rpc.{hpp,cpp},
  mediated_python_worker_protocol.hpp, python_worker_mediation.{hpp,cpp}}` — new.
- `src/backends/native_jail/native_jail_backend.{hpp,cpp}` — `create_python_worker()`/
  `exec_session()`/`dispatch_worker_query()`/`session_watchdog_loop()`, additive to the existing,
  untouched per-exec `create()`/`exec()`/`destroy()` surface (not part of the `SandboxBackend`
  concept — `sandbox/sandbox.hpp`'s own `requires` clause deliberately excludes it).
- `src/backends/native_jail/mediated_python_runner.{hpp,cpp}` — now a thin IPC client; no longer
  embeds CPython in the host process.
- **008 §1b/§3's "kernel jail as a second layer, permanently, for the interpreter" requirement is
  reaffirmed, not amended** — this ADR is the real work that requirement was already calling for; no
  spec-downgrade was made or needed. `native_jail_backend.hpp`'s prior, unreviewed "Correction
  (2026-08-23)" comment asserting the requirement no longer applied is superseded by this ADR and
  should be corrected in place to point here.

**Residual risks, named rather than implied:**
- **Slice 2 (real file/listdir/socket relay) is not built.** Every guest `open`/`listdir`/
  `connect_*` call is denied today. This is a real, disclosed regression in guest capability
  (6 test files, §4), not a silent one — but it means sessions depending on file or network I/O
  through `execute_code` do not yet work under the jailed-worker model at all. This is the highest-
  priority named follow-on.
- **Linux parity (`LinuxNativeJailBackend`) does not exist for the jailed-worker model.** The
  in-process embed's Linux behavior (if any) versus this Windows-only worker-process redesign is an
  open cross-platform-parity gap, distinct from and in addition to 008 §9 G1's own pre-existing,
  never-fully-Judged parity requirement (see the M2 Phase C tracker). Design phase (only) done:
  `docs/planning/linux-jailed-python-worker-design-draft.md` — maps every Windows primitive this ADR
  used (AppContainer/Job Object/`TerminateJobObject`/completion-port draining) to its Linux analogue
  (namespaces+seccomp/cgroups v2/`cgroup.kill`/polled `memory.events`), self-red-teams the port, and
  names what a real prove phase (Linux build/test environment, not available this session) still owes
  before it can become its own numbered ADR.
- **`execute_shell`'s own process-spawn model (Finding O) is unreconciled with this design** —
  deliberately left as a separate, still-open question rather than unified under time pressure.
- **Inherits ADR-004's own measured residual**: `cpu_ms_cap` enforcement via
  `JOB_OBJECT_LIMIT_JOB_TIME` remains best-effort (fired in only 3/11 of ADR-004's own measured
  runs); `wall_ms` (this design's `call_active`-phase watchdog deadline, independently re-measured at
  804ms against an 800ms budget in §4) is the dependable bound, not `cpu_ms`.
- **Inherits ADR-041's accepted Windows AppContainer ACL residual** unchanged — curated host files
  (`win.ini`/`hosts`) remain leakable via inherited ACEs to native code that bypasses interpreter-
  level mediation entirely; this design does not touch that boundary.
- **The design/red-team candidate-comparison record itself is not preserved** (§2) — a process gap
  for *this* ADR's own provenance, not a defect in the shipped code. Future security-critical passes
  run as ephemeral agent workflows should land their design-phase transcript (or a condensed
  design-draft doc, matching this project's usual `docs/planning/*-design-draft.md` pattern) as a
  committed artifact before the ADR is written, not after.
