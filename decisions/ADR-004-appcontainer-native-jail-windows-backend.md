# ADR-004 — Does AppContainer + Job Object satisfy `native-jail`'s Windows contract for `PythonRunner`/`ShellRunner`, across pure-Python, native-extension, and CodeAct-authored guest code?

- **Status:** **Spiked, not Judged.** A single design was stated and exercised against a real
  interpreter this session (2026-08-02) — not a full `design → red-team → prove → judge` cycle
  (`decisions/README.md`). No competing design was implemented side-by-side, and no independent
  red-team pass attacked the design before evidence was gathered. What follows is real, executed
  evidence with a positive control for its headline finding (§6), not argument — but it is scoped
  honestly as a spike, and this ADR should not be cited as settling 008 §3's `native-jail` Windows
  backend without a follow-up red-team pass. **§10 (added later the same day) builds and measures
  the Job Object resource-limit layer §6 finding 3 originally left unbuilt** — real code
  (`src/backends/native_jail/job_object_limits.{hpp,cpp}`), a real test
  (`tests/test_job_object_limits.cpp`), and an 11-run measurement with a headline finding of its own:
  `cpu_ms`/`JOB_OBJECT_LIMIT_JOB_TIME` is unreliable and must not be the trusted enforcement point.
- **Date:** 2026-08-02 (design + AppContainer spike); 2026-08-02, later the same day (§10, Job
  Object build-and-measure addendum).
- **Scope:** The Windows kernel-jail layer of the `native-jail` profile (`src/backends/native_jail/`)
  — AppContainer process isolation plus Job Object resource limits — as the §1b **layer 3 backstop**
  underneath `ShellRunner` (ADR-001) and `PythonRunner`'s import gating (ADR-002, ADR-003), both of
  which already exist and are unaffected by this ADR. Excludes: Job Object resource-limit proving
  (stated as design only, not executed — §9.3), Linux/macOS backends, `wasm`/`remote` profiles, and
  VHDX-backed worktree rollback (a `025` worktree-adapter question, out of scope here — raised and
  spiked in this session's conversation but deliberately not folded in, to keep this ADR's claim
  surface honest).
- **Related specs:** `008-Sandbox-and-Isolation.md` §1b, §2 (contract), §3 (profile table), §4
  (capability enforcement), §7 (abuse cases), §9 G1–G3 · `021-Platform-Support-and-Portability.md`
  Q2 (the exact open question this ADR provides first evidence against) · `010-Python-Code-
  Interpreter.md` §1a, §2, §5 (package policy tiers) · ADR-001 (`ShellRunner`) · ADR-002/ADR-003
  (`PythonRunner` import mediation — this ADR extends their layering argument to `open()`).

## 1. The question

**Can AppContainer (process identity/authority isolation) plus a Job Object (resource limits) serve
as `native-jail`'s Windows backend for the code interpreter — the profile that must run pure-Python
guest code, guest code that imports native (`.pyd`-backed) C extensions, and CodeAct-authored code
the agent itself wrote at runtime — while upholding 008 §2's contract (empty-by-default authority,
enforced limits, structured outcomes, full teardown, attribution, no ambient authority)?**

This has a wrong answer, stated up front because it is the one a plausible-sounding design would
give: **"AppContainer's default-deny ACL model is the filesystem boundary; grant read+execute on the
interpreter's install directory and read+write on the worktree, and reads outside those grants are
denied."** §6 below shows this is false as a general claim — Windows itself grants a curated set of
OS files broad read access to *every* AppContainer process, by design, for Store-app compatibility,
independent of what AgentEngine grants or withholds. Treating AppContainer ACLs as the filesystem
boundary is exactly the kind of "convenient-looking change that breaks I2" CLAUDE.md warns against:
it looks like default-deny and is not, in a way no amount of *more restrictive* AppContainer
configuration on our side can close.

## 2. Background

`src/backends/native_jail/README.md` describes the target: "namespaces + seccomp-BPF + cgroups v2 on
Linux, AppContainer + Job Object + restricted token on Windows, sandbox profile + resource limits on
macOS." As of this session, `python_runner.hpp`/`shell_runner.hpp` implement 008 §1b's **layers
1–2** (closed import allowlist, mediated `open`/`socket`/`subprocess` — ADR-001/002/003) on Windows.
**Layer 3, the kernel jail itself, has no implementation** — no reference to `AppContainer`,
`JobObject`, or `seccomp` exists anywhere in `src/backends/native_jail/` before this session. 021 Q2
already flags the risk directly: *"Windows `native-jail`: AppContainer + Job Objects is the plan, but
proving it enforces the same limits as cgroups v2 (particularly memory and pid caps) is
unvalidated."* This ADR is the first evidence against that open question — partial, not closing it.

## 3. The design

A per-session (008 §6, default `per_session` lifetime) or per-exec sandbox creation does, in order:

1. **Host-side, once per deployment (not per session):** a **vendored** CPython + package set —
   built or bundled by AgentEngine's own install/release process into a directory the running
   account owns — not a reference to whatever Python happens to be on the host. §6.3 is why this is
   load-bearing, not a packaging preference.
2. **Per session, host-side (trusted):**
   - `CreateAppContainerProfile`/`DeriveAppContainerSidFromAppContainerName` (`userenv.dll`) to get
     a package SID for this deployment. One profile, reused across sessions — the SID is stable
     identity, not per-session state.
   - Grant the SID `(RX)` on the vendored interpreter tree and every vendored third-party package
     tree, `(M)` on this session's worktree mount, nothing else — via ACL, not via any
     `ALL_APPLICATION_PACKAGES`-wide grant.
   - Build `SECURITY_CAPABILITIES{ AppContainerSid = sid, Capabilities = nullptr, CapabilityCount =
     0 }` — zero capabilities granted, which per 008 §4 denies `NetOut` (no `internetClient`) and
     leaves every other Windows-defined capability un-granted by default.
   - Set `PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`
     on the same attribute list — a **kernel/token-level** denial of nested process creation, not
     interpreter mediation, directly satisfying 008 §4's "`Exec` (nested): denied" row with a second,
     independent enforcement point beneath ADR-002/003's `subprocess` gating.
   - Create a Job Object; set `JobObjectExtendedLimitInformation` (memory cap, active-process cap,
     `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`) and `JobObjectCpuRateControlInformation` (hard CPU cap).
     **Not executed this session** — design only; §9.3 states this precisely.
   - `CreateProcessW` with `EXTENDED_STARTUPINFO_PRESENT`, the attribute list, and the child assigned
     into the Job Object; stdout/stderr redirected through pipes (the AppContainer child has no
     console of its own to inherit cleanly).
3. **Wall-clock kill:** not a Job Object primitive by itself — a host-side watcher (thread or an
   IOCP tied to the job) calling `TerminateJobObject` past the deadline. **Not executed this
   session** — design only.

**Alternatives considered and rejected, briefly** (008 §1 already rejected the profile-level
alternative — `microvm` — for reasons independent of this ADR):

- **Plain restricted token, no AppContainer.** Loses the capability-SID model entirely (008 §4's
  `NetOut`/`ToolCall` rows have no clean Windows analogue without it) and gains nothing AppContainer
  doesn't already give — rejected without a spike.
- **LPAC (Less Privileged App Container) instead of regular AppContainer.** Plausible and not yet
  spiked — §9.3 lists it as open. §6's win.ini finding is unlikely to change under LPAC specifically
  (the ACL evidence in §6 shows `ALL RESTRICTED APPLICATION PACKAGES` — the LPAC-relevant SID — is
  *also* granted on the affected files), so LPAC is not expected to close the headline finding, but
  this is stated as an expectation from ACL inspection, not as tested behavior.

## 4. Falsifiable claims

| # | Claim | Disproving experiment |
|---|---|---|
| AC-C1 | Pure-Python stdlib code executes correctly inside the AppContainer given RX on the vendored interpreter tree. | Run a stdlib-only script (`json`/`math`/`textwrap`); non-zero exit or wrong output falsifies it. |
| AC-C2 | A stdlib native extension (`.pyd`, built into the interpreter tree) loads and executes correctly given the same RX grant. | Import and exercise `_decimal`/`_socket`; failure to import or a wrong result falsifies it. |
| AC-C3 | A third-party native extension (its own bundled native DLLs, in a *separate* granted tree) loads and executes correctly. | Import `numpy`, perform an array operation, check the result; failure or wrong result falsifies it. |
| AC-S1 | With zero granted capabilities, guest code cannot open an outbound socket. | `socket.connect()` to an external host; anything other than a permission-classed exception falsifies it. |
| AC-S2 | With `CHILD_PROCESS_RESTRICTED` set, guest code cannot create a child process by any route CPython exposes. | `subprocess.run(["cmd.exe", ...])`; anything other than a creation failure falsifies it. |
| AC-S3 | Guest code can read/write only paths under an explicitly ACL-granted tree; **no other host path is reachable regardless of grants.** | Attempt reads across a corpus of paths outside every grant (user directories, common OS config paths); **any successful read falsifies it.** This is the claim §6 disproves. |
| AC-S4 | Granting ACL access to a system-owned interpreter installation (not vendored) is possible from an unprivileged deployment process. | `icacls` the install directory as a standard user; failure falsifies it (expected to fail — §6.3). |

## 5. Executed evidence

Environment: Windows 11 (`10.0.26200.0`), standard (non-administrator) user token — confirmed via
`whoami /priv` to hold none of `SeManageVolumeName`/backup/restore/security privileges. MSVC (Visual
Studio 18 Community, toolset `14.51.36231`) via `cl.exe /std:c++20`. CPython 3.14.0
(`C:\Python314`), numpy 2.5.1 (per-user `site-packages`, a separate directory tree from the
interpreter — itself a finding, §6.4).

Two small C++ programs were built and run against the real Win32 API (not `diskpart`/PowerShell
wrappers, which proved unreliable as a proxy for actual API behavior in an earlier part of this
session — process/attribute-list behavior was exercised directly):

- `ac_setup.cpp` — `CreateAppContainerProfile("AgentEngine.Spike", ...)`, prints the SID.
- `ac_run.cpp` — derives the SID, builds `SECURITY_CAPABILITIES` with zero capabilities, optionally
  sets `PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY`, launches a target command with piped
  stdout/stderr, reports exit code and wall time.

ACL provisioning: a copy of `C:\Python314` into a directory the current (unprivileged) user owns
(`vendored_python/`, simulating a vendored interpreter build), plus the existing per-user numpy
`site-packages` tree, each granted `(OI)(CI)(RX)` to the AppContainer SID via `icacls`; the session's
worktree scratch directory granted `(OI)(CI)(M)`.

### 5.1 Tier 1 — pure Python (AC-C1)

```
TIER1_PURE_PYTHON_OK {"pi": 3.141592653589793, "wrapped": ["pure", "python", "stdlib", "works"]}
exit_code=0 wall_ms=116.033
```
**CORRECT.**

### 5.2 Tier 2 — native extensions, stdlib and third-party (AC-C2, AC-C3)

```
TIER2_NATIVE _decimal (stdlib .pyd): OK :: ...\vendored_python\DLLs\_decimal.pyd
TIER2_NATIVE _socket (stdlib .pyd): OK :: ...\vendored_python\DLLs\_socket.pyd
TIER2_NATIVE numpy (3rd-party native): OK :: 2.5.1 sum_check=90
exit_code=0 wall_ms=211.038
```
`sum_check=90` is `numpy.arange(10) * 2` summed — a real computation through numpy's own native code,
not merely a successful `import`. **CORRECT** for both claims.

### 5.3 Tier 3 — hostile/CodeAct-authored code (AC-S1, AC-S2, AC-S3, plus a positive control)

```
TIER3 [PASS] write inside granted worktree (expect ALLOWED): ALLOWED
TIER3 [FAIL-UNEXPECTED] read C:\Windows\win.ini (expect DENIED): ALLOWED
TIER3 [PASS] listdir C:\Users (expect DENIED): DENIED (PermissionError: [WinError 5] Access is denied)
TIER3 [PASS] write into Python install dir (expect DENIED): DENIED (PermissionError: [Errno 13])
TIER3 [PASS] raw outbound socket connect (expect DENIED): DENIED (PermissionError: [WinError 10013])
TIER3 [PASS] subprocess.run(cmd.exe) (expect DENIED): DENIED (OSError: [WinError 367])
exit_code=0 wall_ms=40.256
```

The worktree write is the **positive control** for AC-S3: it proves the mechanism can both allow and
deny, so the later denials are not an artifact of everything failing (e.g., a broken launch). `listdir
C:\Users` and the write into the RX-only interpreter tree confirm ACL-based denial works as designed
for paths AgentEngine controls. `WinError 10013` (socket) and `WinError 367` (process creation,
`ERROR_CHILD_PROCESS_BLOCKED`) confirm AC-S1/AC-S2 at the kernel/token level, independent of
ADR-002/003's interpreter-level mediation of the same operations — a second, structurally different
enforcement point, which is worth having even though ADR-002/003 already gate these at the Python
layer.

**`win.ini` is the exception, and it is not a fluke:**

```
> icacls C:\Windows\win.ini
C:\Windows\win.ini NT AUTHORITY\SYSTEM:(I)(F)
                    BUILTIN\Administrators:(I)(F)
                    BUILTIN\Users:(I)(RX)
                    APPLICATION PACKAGE AUTHORITY\ALL APPLICATION PACKAGES:(I)(RX)
                    APPLICATION PACKAGE AUTHORITY\ALL RESTRICTED APPLICATION PACKAGES:(I)(RX)
```

Identical ACL confirmed on `C:\Windows\System32\drivers\etc\hosts`. Both `ALL APPLICATION PACKAGES`
and `ALL RESTRICTED APPLICATION PACKAGES` are present — every AppContainer process, LPAC or not,
carries one of these SIDs by construction. **This is not something our AppContainer profile
configuration caused or can suppress**: the ACE is inherited (`(I)`) from a Windows-maintained
default, present on a curated set of OS files independent of which capabilities we grant.

### 5.4 A related finding, out of this ADR's scope but recorded for cross-reference (AC-S4)

Granting the AppContainer SID an ACE on the *real* `C:\Python314` (owned by `BUILTIN\Administrators`,
installed system-wide) failed under the unprivileged token: `icacls: Access is denied.` Copying the
same tree into a directory the current account owns (§5, `vendored_python/`) made the grant succeed
immediately. This is the executed evidence behind §3 step 1's vendoring requirement — recorded here
because it was produced by the same spike, not because this ADR designs the vendoring/build pipeline.

## 6. Red-team findings

Informal — this session's own adversarial pass over its own design, not an independent red-team.
Findings are marked using ADR-001/002/003's severity vocabulary.

1. **[BLOCKING for AC-S3 as originally stated] AppContainer's ACL model is not the filesystem
   boundary — it is inherently, structurally leaky for a curated set of OS files, on every Windows
   installation, regardless of configuration.** §5.3's `win.ini`/`hosts` result is the flagship case,
   but the mechanism generating it — `ALL APPLICATION PACKAGES`/`ALL RESTRICTED APPLICATION
   PACKAGES` ACEs inherited from Windows' own defaults — is not scoped to those two files; it is
   whatever set of files Microsoft has decided Store apps should be able to read for compatibility,
   an OS-version-dependent set AgentEngine cannot enumerate in advance and does not control. **This
   should be added to 008 §7's abuse-case list** (alongside symlink/`..` escape and TOCTOU on
   mounts) as: *read-access leak via inherited `ALL (RESTRICTED) APPLICATION PACKAGES` ACEs on
   host-OS files, independent of granted capabilities.* **Fix, and it is the fix 008 §1b already
   specifies for exactly this reason:** interpreter-level mediation of `open()` — reject any path
   that does not canonicalize under the worktree mount, in Python, before the OS call — must be the
   **primary** filesystem boundary for reads, not merely a defense-in-depth layer alongside
   AppContainer's ACLs. AppContainer's ACL denial (confirmed working for `C:\Users`, for the RX-only
   interpreter tree) remains valuable as backstop layer 3 exactly as 008 §1b frames it — this finding
   does not weaken that framing, it is the concrete case proving why 008 chose it over "ACL is
   enough." ADR-002/003 already build this kind of mediation for `import`; this finding means the
   same discipline is required for `open()`, which 008 §1b §4 already lists as mediated ("`open`
   resolves against the worktree mount and `FsRead`/`FsWrite`") — this ADR's contribution is the
   concrete, reproduced evidence that skipping it and relying on AppContainer ACLs alone fails.
2. **[REAL GAP] Two-directory-tree ACL provisioning is fragile in the same shape ADR-003 already
   found for module names.** §5's numpy lives in a *different* directory tree than the interpreter
   (per-user `site-packages` versus the vendored interpreter root). A vendored build that puts every
   package under one tree the deployment owns removes this specific instance, but the general
   problem — "grant enumeration must track every directory a package's own transitive native-load
   behavior touches" — is exactly the lesson OQ-15/ADR-003 already drew for import names ("granting
   numpy required ~130 names, not 2"). This ADR does not solve that; it notes the ACL-grant surface
   has the same shape of hazard and should not be assumed complete after one working example.
3. **[RESOLVED — see §10] Wall-clock kill and Job Object resource limits were design-only when this
   finding was first written; §10 (added later the same session) builds and measures them.** Memory
   and process-count limits are confirmed reliable; the CPU-time limit is confirmed *unreliable* —
   a new, more specific finding than "unvalidated" (§10.5). 021 Q2's core question is now answered
   for memory/pid (matches expectations) and answered differently than assumed for CPU time (does
   not).
4. **[REAL GAP] LPAC was not tested.** §3's rejection of LPAC as unlikely to close finding 1 is
   reasoned from the ACL evidence in §5.3 (the `ALL RESTRICTED APPLICATION PACKAGES` ACE is already
   present), not from running the design under LPAC. If LPAC turns out to change AC-S3's outcome for
   some file class it should be re-verified, not assumed from this ADR's reasoning alone.
5. **[HOLDS]** AC-C1/AC-C2/AC-C3/AC-S1/AC-S2 all held under the executed evidence, with the
   worktree-write positive control confirming the mechanism can both allow and deny rather than
   failing everything indiscriminately.

## 7. Per-claim verdicts

| Claim | Verdict |
|---|---|
| AC-C1 (pure Python) | **CORRECT** |
| AC-C2 (stdlib native extension) | **CORRECT** |
| AC-C3 (third-party native extension) | **CORRECT** |
| AC-S1 (no outbound socket, zero capabilities) | **CORRECT** |
| AC-S2 (no nested process creation) | **CORRECT** |
| AC-S3 (only granted paths reachable) | **WRONG, as originally stated** — falsified by §5.3/§6.1. True only for paths outside the OS's own curated readable set; not true in general. |
| AC-S4 (unprivileged ACL grant on a system-owned install) | **WRONG** (expected) — confirms the vendoring requirement in §3 step 1. |
| AC-JOB-1 (memory limit contains, positive control doesn't) | **CORRECT** — §10.2, clean and fast (14-22ms). |
| AC-JOB-2 (wall-clock kill fires with no native limit involved) | **CORRECT** — §10.2, precise (fires within a few ms of the deadline). |
| AC-JOB-3 (`cpu_ms`/`JOB_OBJECT_LIMIT_JOB_TIME` is a reliable enforcement point) | **WRONG** — §10.2/§10.5: fired in 3/11 runs, with 1.38x-8.22x overrun when it did; the host `wall_ms` watcher was the actual enforcement in 8/11 runs. |
| AC-JOB-4 (active-process limit contains, positive control doesn't) | **CORRECT** — §10.2, exact (2 of 5 spawn attempts succeeded under a cap of 3 total). |
| AC-JOB-5 (`JobObjectLimits` destructor tears down its process) | **CORRECT** — §10.2. |
| LPAC vs. regular AppContainer for finding 1 | **INCONCLUSIVE** — reasoned, not tested (§6.4). |

## 8. The decision

### 8.1 What is accepted, provisionally, and how far it goes

AppContainer + `CHILD_PROCESS_RESTRICTED` + zero-capability `SECURITY_CAPABILITIES`, launching a
**vendored** interpreter tree, is a workable mechanism for the identity/authority axis of
`native-jail`'s Windows backend — process-creation denial and network denial are real, kernel-
enforced, and confirmed by execution across all three CodeAct library tiers (pure Python, stdlib
native extension, third-party native extension). This is a genuine second enforcement layer beneath
ADR-002/003's interpreter-level mediation of the same operations, not a replacement for it.

**It is explicitly not accepted as the filesystem boundary.** §6.1's finding means `native-jail`'s
Windows backend must treat AppContainer's ACL denial as backstop only for reads, with interpreter-
level `open()` mediation (008 §1b, already specified) doing the real work — this ADR is the executed
evidence for why 008 chose that layering, not a reason to revisit it toward "ACL is sufficient."

**Job Object resource limits (§10) are accepted for memory and process-count, not for CPU time.**
`JOBOBJECT_EXTENDED_LIMIT_INFORMATION`'s memory and active-process caps are confirmed reliable,
fast, and precise. `JOB_OBJECT_LIMIT_JOB_TIME` (the mechanism behind `ResourceLimits::cpu_ms`) is
confirmed *unreliable* — §10.5's measurement is the reason `wall_ms` is not optional the way a
"belt and suspenders" backstop usually is: it is the mechanism a caller must actually depend on for
any CPU/time bound, with `cpu_ms` treated as best-effort only.

### 8.2 What this decision does not claim

Not a `native-jail` promotion (008 §9's G1–G8 gates are far broader than this ADR's scope — no
cross-platform parity, no G4 teardown-cycle census at scale, no G7 session-boundary proof). Not a
full resolution of 021 Q2 (memory/pid now have real evidence; CPU time now has real evidence that it
does *not* match a hard cgroups-v2-style bound — see §10.5 for what would still be needed to fully
close Q2, e.g. an actual Linux cgroups v2 backend to compare against). Not a decision between plain
AppContainer and LPAC (§6.4). Not a design for the vendored-interpreter build/release pipeline
itself (§5.4's finding motivates one; this ADR does not specify it). Not a root-cause explanation
for *why* `JOB_OBJECT_LIMIT_JOB_TIME` behaves inconsistently (§10.5 states the measurement, not a
diagnosed mechanism).

### 8.3 Residual risks, carried forward explicitly

- 008 §7's abuse-case list should gain the `ALL (RESTRICTED) APPLICATION PACKAGES` read-leak case
  named in §6.1, with a hostile-suite test that fails if interpreter-level `open()` mediation is ever
  bypassed or disabled — the same positive-control discipline 008 §9 G2 already requires.
- `cpu_ms` must be documented, wherever `ResourceLimits` is exposed above this backend, as
  best-effort on Windows `native-jail` — a caller that needs a dependable CPU/time bound must set
  `wall_ms` to the value it actually wants enforced (§10.5). This is a spec-level consequence of
  §10's measurement, not just an implementation note.
- The two-directory-tree ACL fragility (§6.2) means any future addition to the vendored package set
  needs the same transitive-surface check ADR-003 already learned to do for import names, applied to
  filesystem grants.
- This spike used one interpreter (CPython 3.14.0) and one compiler (MSVC). No clang cross-check was
  run (unlike ADR-001/002/003's practice of proving on both) for either the AppContainer or the Job
  Object work.
- `disk_bytes`/`net_bytes`/`output_bytes`/`fds` remain unenforced by any kernel-jail mechanism on
  Windows (§10's `JobObjectLimits` deliberately does not attempt them — see that header's own scope
  note); they still depend entirely on interpreter-level mediation, same as the filesystem finding.

### 8.4 What this binds

Nothing is promoted. This ADR is evidence toward `008-Sandbox-and-Isolation.md` §1b/§3's Windows
`native-jail` backend and toward `021-Platform-Support-and-Portability.md` Q2, cited as partial,
scoped evidence — not as closing either.

### 8.5 What would reopen this

A future measurement showing Job Object limits do not match cgroups v2 behavior in a way that
matters for a stated 023 budget; an LPAC test that changes §6.1's verdict for some file class; a
finding that the `open()` mediation layer (008 §1b, ADR-002/003's territory) has a gap analogous to
this ADR's `win.ini` case, which would mean the "backstop is enough for everything else" framing in
§8.1 needs its own re-verification, not just the primary layer's.

## 10. Addendum — Job Object build-and-measure follow-up (2026-08-02, later the same session)

Fulfils this ADR's own §9 next-step 3 (now folded in here rather than left as a future item). Real
code, a real test, real measurement — not design reasoning.

### 10.1 What was built

`src/backends/native_jail/job_object_limits.{hpp,cpp}` — an RAII `JobObjectLimits` wrapping one
Windows Job Object:

- `create(ResourceLimits)` sets `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` unconditionally, plus
  `JOB_OBJECT_LIMIT_JOB_MEMORY` (from `memory_bytes`), `JOB_OBJECT_LIMIT_ACTIVE_PROCESS` (from
  `pids`), and `JOB_OBJECT_LIMIT_JOB_TIME` (from `cpu_ms`, converted to 100ns units) whenever the
  corresponding `ResourceLimits` field is nonzero.
- `assign_process`/`wait_or_kill`/`query_usage` round out the seam `job_object_limits.hpp` documents
  in full (not repeated here).
- `wait_or_kill`'s wall-clock enforcement (`wall_ms`) is a bounded `WaitForSingleObject` on the
  caller's own thread followed by `TerminateJobObject` on timeout — this ADR's §3 step 3 "watcher,"
  built as a synchronous call rather than a separate thread since the caller already blocks for the
  duration.
- `tests/helpers/hostile_child.cpp` — a small test-only process with four modes (`alloc <mb>`,
  `spin`, `sleep <ms>`, `spawn <n> <self_path>`) used to exercise each limit.
- `tests/test_job_object_limits.cpp` — five tests, each with a **positive control** run against the
  identical hostile behavior with the limit under test disabled (008 §9 G2's discipline).
- Wired into `CMakeLists.txt` (`agentengine_job_object_limits`, `WIN32`-gated, no third-party
  dependency — pure Win32 API) and `tests/CMakeLists.txt`.

### 10.2 Executed evidence

Built with MSVC (Debug config; the same toolchain as §5), run via `ctest` and directly. All five
tests pass; `ALL PASS`, exit code 0.

**Memory limit** (`test_memory_limit`): contained arm (32 MB cap, child tries to commit+touch 512
MB) — child hit `std::bad_alloc`, unhandled, aborted (`exit_code=3`) at **14-22ms**, never printed
its "done" marker. Positive control (no limit, 80 MB target) completed normally, `exit_code=0`,
printed its "done" marker, in ~23ms. **CORRECT, not vacuous.**

**Wall-clock kill, standalone** (`test_wall_clock_kill_standalone`): a CPU-spinning child under a
500ms `wall_ms` deadline with *no* Job Object limit set at all was terminated at **measured_wall_ms
between 500 and 504ms** across every run — precise, and independent of any native Job Object
primitive firing. **CORRECT.**

**Active-process limit** (`test_process_count_limit`): contained arm (`pids=3`, parent tries to
spawn 5 children) — `SPAWN_RESULT requested=5 succeeded=2` **every run**: exactly 2 children
succeeded, bringing the job to precisely 3 total (parent + 2), matching the configured cap exactly,
with no overrun. Positive control (no `pids` limit): `succeeded=5` — all children created.
**CORRECT, exact, and — unlike the CPU-time result below — reliable.**

**Teardown on destroy** (`test_teardown_on_destroy`): a child assigned to a job with a 30-second
sleep ahead of it was confirmed terminated (`WaitForSingleObject` signaled) within 3 seconds of the
owning `JobObjectLimits` going out of scope with no explicit `wait_or_kill` call.
**CORRECT** — 008 §2 clause 4 ("Full teardown") holds even on an unclean/early-return path.

**CPU-time limit — the headline finding** (`test_job_time_limit`), run 11 times across three
invocations of the test binary, a CPU-spinning child under `cpu_ms=500` with a generous
`wall_ms=5000` backstop so the measurement shows which mechanism actually fires:

| Run | Fired via `JOB_OBJECT_LIMIT_JOB_TIME`? | CPU time consumed before termination | Overrun vs. 500ms budget |
|---|---|---|---|
| 1 | Yes (`STATUS_QUOTA_EXCEEDED`) | 2921.9 ms | 5.84x |
| 2 | Yes | 687.5 ms | 1.38x |
| 3 | No — `wall_ms` backstop fired at 5000ms | 4765.6 ms | 9.53x |
| 4 | No — backstop | 4843.8 ms | 9.69x |
| 5 | No — backstop | 4859.4 ms | 9.72x |
| 6 | Yes | 4109.4 ms | 8.22x |
| 7 | No — backstop | 4890.6 ms | 9.78x |
| 8 | No — backstop | 4875.0 ms | 9.75x |
| 9 | No — backstop | 4890.6 ms | 9.78x |
| 10 | No — backstop | 4890.6 ms | 9.78x |
| 11 | No — backstop | 4875.0 ms | 9.75x |

**3 of 11 fired automatically via the kernel's own `JOB_OBJECT_LIMIT_JOB_TIME`**, with overrun
ranging **1.38x-8.22x** the configured budget and no discernible relationship between the configured
limit and the actual overrun (the smallest and largest overruns are 6x apart). **8 of 11 did not
fire at all** within the 5-second observation window, letting **~9.5x-9.8x** the configured CPU
budget accumulate before the host-side `wall_ms` watcher (proven reliable above) terminated the job
instead.

### 10.3 What this confirms and what it changes

Confirms (no completion port needed): `JOB_OBJECT_LIMIT_JOB_TIME` *can* auto-terminate a process on
its own — when it fires, no `IOCP`/`JobObjectAssociateCompletionPortInformation` wiring is required,
resolving the ambiguity `job_object_limits.cpp`'s original comment flagged as unverified from MSDN
alone.

Changes the plan: `cpu_ms` cannot be the *trusted* enforcement point for a CPU/time budget on
Windows `native-jail` — it fires unpredictably, both in whether it fires at all within a reasonable
window and in how much it overruns when it does. **`wall_ms` is not a redundant backstop on top of
a trustworthy native limit; it is the mechanism a caller must actually depend on.** §8.3 restates
this as a residual risk with a spec-level consequence (document `cpu_ms` as best-effort wherever
`ResourceLimits` is surfaced above this backend).

### 10.4 What was not investigated

*Why* `JOB_OBJECT_LIMIT_JOB_TIME` behaves this way — whether it is checked by a coarse periodic
timer, whether `JobObjectAssociateCompletionPortInformation` would change the reliability (a
completion port was deliberately not wired up, to test the "no completion port" case directly), or
whether behavior differs across Windows builds — is unexplored. This is a measurement, not a root
cause, and should not be over-read as more than that.

### 10.5 Verdict this addendum contributes to 021 Q2

021 Q2 asked whether AppContainer + Job Objects "enforces the same limits as cgroups v2 (particularly
memory and pid caps)." This addendum has no cgroups v2 counterpart to compare against yet (no Linux
`native-jail` backend exists), so it cannot close Q2 outright — but it does answer the Windows half
concretely: **memory and pid caps are precise and reliable; CPU-time caps are not**, which is new,
specific information Q2 did not have before, and a real basis for whatever the eventual Linux-side
comparison measures against.

## 11. Suggested next steps (not part of this ADR's evidence)

1. ~~Add the read-access-leak abuse case to 008 §7, with a hostile-suite test and the positive
   control §8.3 describes.~~ **Done** — `tests/test_native_jail_abuse_corpus_windows.cpp`'s Case 4
   (`M2 Phase C task C3`, commit `b39f5ea`) asserts `win.ini` reads `ESCAPE_OK` (the documented gap,
   not silently regressed) directly alongside the same case's primary assertion that an arbitrary
   non-curated file (`secret_file`) reads `ESCAPE_DENIED` — the pairing that makes it a real positive
   control: the leak is bounded to the OS's own curated file set, not a general containment failure.
   Green as of `decisions/ADR-040-fs-quota-capability-gate-fix.md`'s full-suite run (2026-08-14,
   175/175). This entry was stale before that check — the test existed since the M2 implementation but
   this checklist item was never updated to reflect it (see `decisions/ADR-041-appcontainer-ace-leak-
   accepted-residual.md` for the full accounting).
2. A real, adversarial red-team pass on this design — by a reviewer who did not write it — before any
   claim here is treated as more than a spike.
3. ~~Build and measure the Job Object resource-limit layer against 021 Q2~~ **Done — §10.**
   Remaining: a Linux cgroups v2 backend to give 021 Q2 its comparison point, and root-causing §10.4's
   open "why" for `JOB_OBJECT_LIMIT_JOB_TIME`'s unreliability.
4. Decide LPAC vs. regular AppContainer by running §5's tier-3 corpus under both, not by reasoning
   from static ACL inspection alone.
5. A clang build/run of both the AppContainer and Job Object code (§8.3's cross-compiler gap).
