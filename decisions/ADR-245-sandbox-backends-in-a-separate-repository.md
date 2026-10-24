# ADR-245 — Should the sandbox backends leave the AgentEngine repository, and where is the cut, so that the engine still decides every I2, I3 and I8 question whatever repository a backend comes from?

**Status:** Proposed (2026-10-24), revision 5. Design only; prove pending. Four red-team rounds:
rounds 1 and 2 rejected revisions 1 and 2 (3 FATAL each); rounds 3 and 4 found no FATAL (6 and 10
MAJOR). All findings are folded in; the log is `docs/planning/adr-245-red-team.md`. Depends on
[ADR-243](ADR-243-layer-and-developer-api.md), itself Proposed: this ADR cannot be Judged before it,
or its ADR-243 amendments lapse. Amends
[ADR-242](ADR-242-direction-reset-developer-engine-linux-server-explicit-choice.md) (§2, R2, R3 rules
1–2, 4, 6, R8, §3a, §5, §6). Evidence: `docs/planning/proofs/adr245/`. Per-backend matrix and
comparison with existing systems: `docs/research/2026-10-24-sandbox-system-comparison.md`.

## 1. The question, and the baseline

### 1.1 Owner direction (2026-10-24)

> "To have more focus to AgentEngine I want to move all of sandbox backend out of repo and maintain
> with a separate repo. AgentEngine only focus to agent logic, we use too much time to manage
> sandboxes."

And: we build our own sandbox code only where it is richer or better than what exists, judged per
backend, never by the union of our backends.

The direction is decided; this ADR decides **where the cut runs**. Two wrong answers:

- **Cut too high:** the contract, or any decision that reads authority (I2), interprets guest output
  (I3) or accounts a budget (I8), leaves with the backends.
- **Cut too low:** only Kata moves; the engine keeps native-jail's OS jail code, its hostile corpora
  and its CI legs.

### 1.2 Baseline (main @ a6e6709f, measured 2026-10-24)

**Decisions made inside backends today:**

| Where | Decision | Evidence |
|---|---|---|
| Every backend's `create()` | Mount and egress grants (`authorize_spec`) | `kata_backend.cpp:141`, `native_jail_backend.cpp:352`, `:785`, `linux_native_jail_backend.cpp:290`; the registry forwards straight to the backend (`sandbox_backend_registry.hpp:132`) |
| Every backend verb | Receives the caller's `EffectContext` | `sandbox_backend_registry.hpp:78-79` |
| `SandboxSpec` | Carries the whole `CapabilitySet` | `sandbox.hpp:162` |
| `NativeJailBackend::create_python_worker` | Adds deployment mounts, re-runs `authorize_spec` outside `create` | `native_jail_backend.cpp:752-787` |
| `NativeJailBackend` | `agent.*` nonce check, worker-query dispatch, tool dispatch | `:1126-1215`, `:992-1068`, `:1020` |
| native-jail HandleRelay | `FsRead`/`FsWrite`/`NetOut` at open and connect; an RPC relay (a handle duplicated into AppContainer is unusable); ids live until teardown | `native_jail_handle_relay.cpp:125-143`, `:166`, `:172`, `:241`, `:307`, `:353`; `native_jail_backend.hpp:289-299` |
| `KataBackend` | Resolves allowlist hostnames, pins them in the guest's `/etc/hosts`; renders and installs the nft/tc allowlist and the `net_bytes` quota | `kata_backend.cpp:214-220`, `:648-654`; `egress_policy.hpp:12-23`, `:57` |
| `WasmBackend` | Decides capabilities per WIT interface; reads deadline, cancellation, trace ids | `wasm_backend.cpp:121-139`, `:254`, `:270`, `:392`, `:880` |
| Registry | `Strict` ranks by a backend-declared strength; registration defaults to `eligible` | `sandbox.hpp:78-80`, `sandbox_backend_registry.hpp:126` |

**Other coupling:** `MediatedPythonRunner(..., NativeJailBackend&)` (`mediated_python_runner.hpp:121`);
`extract_pdf_text.hpp` includes both native-jail backends (`:74`, `:78`) and bakes in a worker path
(`CMakeLists.txt:1319`); the engine's worktree opens follow in-mount symlinks by design
(`src/core/worktree_mount_fs_posix.cpp:141`, ADR-014); the guest worker is a Windows-only native
executable (`python_worker_main.cpp:22`, `python_worker_mediation.cpp:28`); contract headers change
layout under build options (`net_egress_proxy.hpp:139,178,290`; `sandbox.hpp:52,207`).

**Sizes and edges:** `src/backends/native_jail` 14,776 lines, `kata` 3,063, `native_process` 2,094,
`wasm` 1,404. 79 include edges cross the cut from code that stays (72 tests, 2 from
`extract_pdf_text.hpp`, 5 from 4 developer tools); 63 test files cross it.

## 2. Decision

### 2.1 Two repositories, one dependency direction

- **`AgentEngine`** keeps the agent logic, the contract, **every decision** (§2.2), the plugin host
  (`WasmBackend`) and `NoSandboxBackend`. It builds, tests and runs alone.
- **`agentengine-sandboxes`** (new, MIT, same owner) holds backend mechanisms, and tools whose only
  implementation needs one (§2.4). It is consumed as source against one exact AgentEngine version.
  AgentEngine never includes, links, fetches or tests against it in library code, tests, examples,
  tools or required CI.
- `decisions/` stays here and binds both repositories. Gates this ADR assigns to
  `agentengine-sandboxes` are **required** checks in that repository.

### 2.2 The rule: a backend never decides

A backend implements mechanism; the engine makes every decision that reads authority, interprets
guest output, or accounts a budget. Bytes the engine originates (such as the `agent.*` nonce) do pass
through a backend, which can therefore drop or forge frames; that is why every frame is checked by
the engine (item 5), not why the backend may be trusted.

**Scope and the API.** Every backend, `NoSandboxBackend` included, implements `sandbox/backend_api/`,
whose headers (transitively) include neither `trust/` nor `core/effect_context.hpp`; limits use
`Bound<std::uint64_t>` (`core/bound.hpp`), not `cap::Limit`; no type-erased callbacks except
`ExecControl`'s stop token. The one exemption is `WasmBackend` (ADR-010 decides capabilities per WIT
interface); its registration path takes an `EngineResidentBackendKey` with a private constructor.
That key prevents accidents, not malice: a backend is host code the developer links.

**What a backend receives:**

- **`BackendSpec`:** no `CapabilitySet`. Mounts as wrapper-minted handles (read-write only as a
  `StagingRoot`, §2.3); limits; the image by digest; host-authored environment (ADR-242 R3 rule 1's
  allowlisted names); the egress endpoint of the engine's proxy (item 3), and nothing else for the
  network.
- **`ExecControl`:** stop token, deadline, run, trace and span ids (the shape of ADR-241 §8.3's
  `EgressCallControl`).
- Bytes, and channels the engine created.

**What the wrapper (`AnySandboxBackend`) decides:**

1. `authorize_spec` on the effective spec, at `create` and at interpreter start (the facet declares
   deployment mounts as data; the wrapper adds and authorizes them, and checks `mount_roots`).
2. `cap::Exec` (enforced by no backend today, ADR-242 §3a) and, for `NoSandboxBackend`,
   `cap::NativeExec` (moved out of the backend, superseding ADR-242 R8 rule 2's placement).
3. **Egress, for every backend: a guest's only network path is the engine's egress proxy** (008 §4,
   "egress is always host-mediated"). The proxy resolves, validates (`resolve_and_validate`,
   `net_egress_proxy.hpp:93`), applies the `NetOut` allowlist by host name (SNI, `Host`), counts
   `net_bytes`, and attributes every connection (I4). The backend enforces one rule: guest traffic
   reaches the proxy endpoint or is dropped. Under Kata, ADR-240's bridge layers shrink to that
   rule; the `/etc/hosts` pinning and per-destination tuples go. Non-TCP egress is unavailable to
   guests; disclosed (§6). This amends ADR-242 R3 rule 6 and ADR-240 (Proposed).
4. Budgets (I8): accounting in the engine; a limit the backend does not declare fails closed before
   `create`.
5. The `agent.*` loop (I3): frame codec, per-exec nonce, exec-in-flight state, bridged-tool
   allowlist, dispatch through the tool pipeline.
6. **HandleRelay, wholly, as an RPC relay.** No host handle enters a guest. Authority is the
   **run's** (ADR-242 R3 rule 1: a run admitted under live authority runs to its end under it): every
   relayed operation is checked against it, and every id is closed when the next run's authority
   differs. Relayed file I/O targets the `StagingRoot`, so it reaches the worktree only by harvest.
7. **The authority-bound rebuild (ADR-242 R3 rule 1) for every persistent facet**: VM, jailed
   interpreter, held shell. On a change of principal or capability set, the wrapper destroys the
   sandbox and its facets and creates new ones. A backend that pools must hand back a sandbox with no
   file, environment variable or process from a previous one (G-S17).
8. **Ownership and tenancy (ADR-242 R7).** The wrapper binds every handle to its agent, principal and
   tenant and refuses cross-owner use, including `destroy`; backend handle ids are opaque to agents.
   Backends must tolerate concurrent calls from unrelated sessions (`sandbox_backend_registry.hpp:
   116-121`).
9. **`Strict` eligibility is the developer's, not the backend's.** A `backend_api/` registration is
   `named_only` unless the developer states eligibility, and §3a marks only vetted backends as
   eligible (ADR-242 R2). A self-declared strength orders eligible backends; it never makes one
   eligible.
10. **Recorded nondeterminism (I5).** `ExecOutcome` and each harvested change set cross the recorded
    seam.

**What a backend still enforces** (and §3a says so per backend; the kit proves conformance, not
honesty): the boundary; declared limits; stop and deadline; "proxy or drop"; `quiesce()`; the
staging export method (H1); staging disk quota; spec fidelity; leaving no residue on reuse.

### 2.3 Staging and harvest: requirements for the contract-slice ADR

The mechanisms are a security-critical refactor and get their own ADR (§2.7 step 2). This ADR fixes
what they must meet:

- **H1 `StagingRoot`.** An opaque, wrapper-minted handle to an open directory, never a stored path.
  Checks are per backend: for an engine-created root (native-jail, `NoSandboxBackend`), a fresh,
  empty, per-sandbox directory with an engine-owned ACL; for a backend-supplied root (Kata), a
  dedicated filesystem (distinct `st_dev`, `nosuid,nodev,noexec`, quota within the declared size,
  empty at handover). The export method (virtiofs, never a block device the host later mounts,
  ADR-242 R3 rule 4) is backend-enforced and kit-tested. Where a path is unavoidable (an OCI bind
  source, an AppContainer ACE), it is derived from the handle and the handle is held for the
  sandbox's lifetime.
- **H2 Opens that never follow.** Linux: `openat2(O_PATH | O_NOFOLLOW, RESOLVE_BENEATH |
  RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV)`, `fstat` (regular, `st_nlink == 1`), reopen
  `O_RDONLY | O_NONBLOCK` (a guest's lease cannot block it), read at most the `fstat` size.
  Windows: component-by-component `NtCreateFile` relative to the parent handle,
  `FILE_FLAG_OPEN_REPARSE_POINT` with reparse points refused, `FILE_COMPLETE_IF_OPLOCKED`,
  `FILE_TYPE_DISK`, `NumberOfLinks == 1`, no `:` and no reserved names. These are new primitives,
  separate from the worktree's following opens.
- **H3 Bounded.** Depth, entry count, total bytes and a deadline bound every walk; exceeding any is a
  named failure, nothing written.
- **H4 Semantics.** Harvest is a diff against the copy-in baseline: creations, modifications,
  deletions, renames and mode bits, each a ledger effect checked against the grant's `path_prefix`,
  size and count; collisions over case, Unicode normalization, 8.3 short names and trailing dot or
  space are refused. ADR-242's "worktree wins" conflict rule applies on every backend.
- **H5 Crash consistency.** A harvest commits to the worktree and ledger atomically or not at all;
  staging roots and relay state left by a crash are swept at startup by owner.
- **H6 `quiesce()`.** Stops every guest writer, flushes the guest's view so the host reads what the
  guest wrote, and also blocks the guest's network (ADR-242 R3 rule 2's between-run pause and
  blackhole). It has a deadline; on timeout the exec fails closed, nothing is harvested, and the
  sandbox is destroyed.

**Live read-write consumers that change semantics** (each needs a test in the contract slice):
`MediatedPythonRunner`'s `mount_roots`, memory mounts, `agent.notes` write-back (ADR-156),
HandleRelay writes, the held native shell (ADR-209). Under staging, a guest's writes reach the
worktree at harvest, not live.

### 2.4 What stays and what moves

**Stays in AgentEngine:** the contract (`sandbox/`, `sandbox/backend_api/`), the wrapper and every
§2.2 decision, the registry, `SandboxRuntime`, `MandatorySandboxProvider`, the egress proxy; the
worktree, ledger, staging and harvest; the parsed shell (ADR-001), the shell, sandbox and CodeAct
`ContextProvider`s, the materializers, the capability-gated filesystem adapters; `NoSandboxBackend`
(on `native_process`, with `job_object_limits`) and ADR-071's providers; `WasmBackend`; the conformance
kit and `tests/support/memory_cap.hpp`; and **the `agent.*` guest library**, redefined because the
guest half is a Windows-only executable today:

- Python source (the `agent.*` module from `agent_*_codegen`), the frame protocol and codec, the
  worker's own import gate (`python_worker_mediation.cpp:60-151`), and a per-platform C shim
  (Windows pipes today; a Linux transport built in the contract slice). The microvm guest runs the
  same source over the Linux transport with its pinned `python3`.
- Its identity is a hash the **host** computes from sources and pinned versions, never one a guest
  reports. Under `microvm` the guest-side import gate is not a security boundary (a root guest can
  switch it off, ADR-242 R3); the VM is.
- A **test interpreter host** (`tests/support/`, never installed) runs the same guest library in an
  unjailed child so the engine's loop is tested against production guest code.

**Moves to `agentengine-sandboxes`:** native-jail's mechanisms (AppContainer and Job Object setup,
namespaces, `pivot_root`, seccomp, cgroups, the worker launcher, `python_lockdown` and the in-process
`PythonRunner` it serves, `quiesce()`); `KataBackend` (its egress reduced to "proxy or drop"), the Kata
CI, `tools/kata-host-check.sh`, the runbooks; the PDF tools and worker (they need an isolated worker
for untrusted input, which only a backend supplies); `test_reference_agent_containment_invariance`
and every hostile corpus, fuzz target and CI leg that exercises only moved code; the `remote`
backend, when built.

**Deleted:** the Docker and containerd execution surfaces (ADR-243 §5, ADR-242 §6 step 3).
`SandboxRuntime`, `MandatorySandboxProvider`, task branches, merge-on-join and `reset_to_turn` are
re-proven over `NoSandboxBackend`.

**Hosted backends** (`remote`): copy-in sends worktree content off the host, so the backend's
control-plane destination is a declared `NetOut` grant, checked by the wrapper.

### 2.5 Version skew

Backends are consumed as source (`find_package(AgentEngine <version> EXACT)`); no binaries. Contract
types live in an ABI-tagged inline namespace hashed over the contract headers and every option that
changes their layout (platform, `AGENTENGINE_WITH_HTTPS`, ...), exported as interface compile
definitions; on MSVC, `#pragma detect_mismatch` also covers `_ITERATOR_DEBUG_LEVEL` and the runtime
library.

### 2.6 Ownership of gates

| Gate | Owner, required there |
|---|---|
| G-R1, G-R3, G-R5, G-R6, G-R7, G-R13; G-R12's AgentEngine part; G-R8 (over the test interpreter host); G-R14 (a), (b), (d), (e); G-R4's harvest and egress cases | AgentEngine |
| G-R14 (c) and G-R2 | The kit, defined here, run in both |
| G-R4's boundary cases, G-R9, G-R10, G-R11 | `agentengine-sandboxes` |

ADR-242 §3a: a moved backend's cell says "backend enforces" and cites a test in
`agentengine-sandboxes`, whose required CI fails on a missing, smoke-only or stale (14-day) cited
test, and publishes a result manifest per commit signed through the repository's CI identity
(Sigstore keyless, GitHub OIDC); AgentEngine verifies the signer and reads it as a non-required
check.

### 2.7 Order of work

1. Finish ADR-244.
2. **The contract-slice ADR and its build, in AgentEngine:** `backend_api/`, the wrapper decisions,
   proxy-only egress, H1–H6, the interpreter facet, the guest library and its Linux transport,
   `NoSandboxBackend`, the kit, the test interpreter host; delete the Docker/containerd surfaces;
   move every engine test off them and off `NativeJailBackend`.
3. ADR-237, then ADR-243 slices 1–3 (ADR-243 §5's own order). `AnySandboxBackend` is built in step 2
   and adopted by ADR-243's slices 2 and 4.
4. Remove the remaining upward edges (developer tools, PDF tools).
5. Carve in place: `sandboxes/`, its own CMake project against the installed AgentEngine. G-S1
   passes here.
6. Extract with `git filter-repo`; apply §5.
7. The backend half of ADR-242 §6 step 3 continues in `agentengine-sandboxes`, subject to the
   comparison note's per-backend verdict.

## 3. Alternatives

- **A. Keep everything.** Rejected by the owner, for focus.
- **B. Move the contract too.** Decisions leave the engine. Rejected.
- **C. Move the backends as they are** (revision 1). Moves `authorize_spec`, the `agent.*` loop and
  HandleRelay's checks. Rejected, round 1.
- **D. Strip `EffectContext` from verb signatures only** (revision 2). `SandboxSpec` still carries the
  `CapabilitySet`; interpreter start re-authorizes in the backend. Rejected, round 2.
- **E. Keep per-destination egress enforcement in each backend** (revision 4). I2 and I8 at the
  microvm edge leave the engine with Kata. Rejected, round 4.
- **F. A permanent CMake subproject here.** No separation of issues, CI and attention. Kept as step 5.
- **G. Chosen: §2.**

## 4. Gates

Each needs a positive control (spec 022 §5). Kit gates run per backend in both repositories.

| Gate | Claim | Disproof / positive control |
|---|---|---|
| **G-S1** | AgentEngine stands alone; nothing it keeps lost its tests | A clean checkout without `sandboxes/` and without network access to the other repository builds and passes on Windows and Linux; a lint fails on any include, CMake target, compile definition or CI step naming a moved path; a manifest lists every kept component with a tagged behaviour or enforcement test (not smoke). Controls: a planted include, a planted `FetchContent`, a deleted last test, a test re-tagged smoke, each fail |
| **G-S2** | Backends cannot reach authority, and only the wrapper calls them | A lint over `backend_api/`'s transitive includes and names (`trust/`, `effect_context.hpp`, `CapabilitySet`, `EffectContext`); a lint that no engine code outside the wrapper calls a backend verb. A kit backend that accepts any spec is refused an ungranted mount at `create` and at interpreter start. Controls: a planted include in `backend_api/`; a planted direct call; the kit backend called without the wrapper accepts the mount |
| **G-S3** | Harvest is engine-enforced and bounded (H2–H4) | From a real staging root: symlinks out and in, a hard link, a FIFO, a device, `..`, case, Unicode, 8.3 and trailing-dot collisions, an alternate data stream and a reparse point (Windows), an oversize sparse file, a file outside `path_prefix`, a held write lease (Linux) and an oplock (Windows), a tree past the depth and count caps: each refused or bounded, nothing written, nothing blocks past the deadline; deletions and renames arrive as ledger effects; a live writer without `quiesce()` never causes a read outside the root. Control: a naive copier in the test copies the symlink's target and blocks on the lease |
| **G-S4** | Read-write mounts are `StagingRoot` only; roots are dedicated (H1) | `BackendSpec` cannot take a read-write mount from a path (compile-fail beside a compiling `StagingRoot`). Engine-created roots: a non-empty or foreign-ACL directory is refused. Backend-supplied roots: a shared `/tmp`, missing `nosuid,nodev,noexec`, over quota, a shared `st_dev` are refused. Control: a correct root of each kind is accepted |
| **G-S5** | The `agent.*` loop is engine code, against the production guest | Over the test interpreter host: stale or foreign nonce, no exec in flight, a call outside the allowlist, a forged frame each refused; parser fuzzed to ADR-015's criteria; the host-computed guest-library hash matches each backend's guest image. Control: a well-formed in-exec call under a live grant succeeds and appears in approvals, budgets and audit. The Linux leg reports "not run" until the Linux transport lands |
| **G-S6** | No host handle enters a guest; relayed bytes are counted | Guest handles enumerated from the host before and after open and connect (`NtQuerySystemInformation(SystemExtendedHandleInformation)` by pid; `/proc/<pid>/fd`); bytes past `net_bytes` or the file cap refused. Control: the relay function built in the test without its counter exceeds the cap |
| **G-S7** | Stop and deadline reach every backend | Kit: a stop during an exec ends it within a bound; a passed deadline ends it. Control: a kit backend that ignores `ExecControl` fails |
| **G-S8** | Skew fails to link | A backend built with one layout option different fails to link (and, on MSVC, fails `detect_mismatch`). Control: built with the same options, it links |
| **G-S9** | Drift is caught | A PR touching contract paths triggers a non-required build of `agentengine-sandboxes` against it; a nightly job builds against `main`. Control: both run against a branch with a planted break, and both fail |
| **G-S10** | §3a stays true across repositories | `agentengine-sandboxes`' required CI rejects a cell citing a missing, smoke-only or stale test; AgentEngine rejects a manifest not signed by that repository's CI identity. Controls: a planted smoke-test cell; a manifest signed by another identity |
| **G-S11** | Nothing implicit; `Strict` is the developer's | With only `NoSandboxBackend`, `resolve_strict` fails and an agent naming no backend fails with G-R3's error; a `backend_api/` backend declaring maximum strength but registered without a stated eligibility is never chosen by `resolve_strict`. Control: the same backend with eligibility stated by the developer is chosen |
| **G-S12** | The test interpreter host stays a test | A lint fails if a non-test target links it. Control: a planted link from `cli_chat` fails |
| **G-S13** | Only `WasmBackend` uses the engine-resident path | `try_compile`: an out-of-tree type registering through it fails (no key). Control: the engine's `WasmBackend` registration compiles |
| **G-S14** | Relay ids follow the run's authority | A file opened for writing and a socket connected in run 1 under grant A are refused in run 2 under narrower grant B, within the same persistent interpreter. Control: run 2 under identical A with a new expiry keeps them |
| **G-S15** | `quiesce()` holds (H6) | Kit: a continuous writer (`write`, `mmap`) makes no progress after `quiesce()` and the host reads what it wrote; a network connect during quiesce never reaches the proxy; a quiesce past its deadline fails the exec and harvests nothing. Control: without `quiesce()`, the writer progresses and the connect arrives |
| **G-S16** | Egress is the engine's (item 3) | Kit: a guest reaches an allowlisted host only through the proxy, which counts and attributes it; a direct connect to the same IP, a UDP send and a connect to the host gateway are dropped. Control: the backend's "proxy or drop" rule removed in a test build lets the direct connect through |
| **G-S17** | Rebuild leaves no residue (item 7) | Kit: a principal or capability change destroys and recreates; a planted file, environment variable and background process from the first sandbox are absent in the second, also when the backend pools. Control: a backend that recycles without cleaning fails |
| **G-S18** | Harvest is crash-consistent (H5) | Kill the engine mid-harvest: on restart the worktree and ledger match either the before or the after state, and the orphaned staging root is swept. Control: a non-atomic harvest in the test leaves a mismatch |
| **G-S19** | Handles are owned; backends are concurrent | Kit: N sessions across 2 tenants create, exec and destroy concurrently; using or destroying another owner's handle is refused by the wrapper. Control: the bare backend called directly accepts the cross-owner handle |

## 5. Amendments applied on Judged

| File | Change |
|---|---|
| `CLAUDE.md` | "sandboxed execution and a Python code interpreter as built-in subsystems" becomes "sandboxed execution through the `SandboxBackend` contract, whose decisions are the engine's; first-party backend mechanisms live in `agentengine-sandboxes`". The execution-options bullet names both repositories |
| ADR-242 §2 | "The engine ships managed options for every resource": the execution backends are shipped by `agentengine-sandboxes`, still chosen explicitly; the engine ships the contract, `NoSandboxBackend` and `WasmBackend` |
| ADR-242 R2, R3 rules 1, 2, 4, 6, R8, §3a, §5, §6 | Option list by repository; rebuild for every persistent facet; `quiesce()` carries the between-run pause and blackhole; `StagingRoot` and H1–H6; proxy-only egress; the wrapper decisions (superseding R8's "`create`/`exec` take `EffectContext&`" and rule 2's in-backend check); gate ownership; §6's order (1b, step 3's contract half, step 4, step 2); correct §3a's Docker row (ADR-171's isolation defaults exist; the authority gap remains) |
| ADR-240 (Proposed) | Under Kata the bridge enforces "proxy or drop" only |
| ADR-243 §2.1, §2.3, §4, §5 | `sandbox/backend_api/`; only `wasm` on the engine-resident path; `AnySandboxBackend` built in ADR-245's contract slice; G-A1's server program over a kit backend here, its Kata variant there; §5's "only consumer" ends at extraction |
| `rfcs/008` §2/§2a, §4 | `BackendSpec`, `ExecControl`, `StagingRoot`, `quiesce()`, the interpreter facet; no host handle in a guest; egress only through the engine's proxy |
| `rfcs/010` §6 | One `agent.*` guest library and host loop, engine-owned |
| `CONVENTIONS.md` | Tier 2 applies in `agentengine-sandboxes` |
| `.github/workflows/` | Kata and native-jail legs move; AgentEngine keeps Windows and Linux legs over `NoSandboxBackend`, `WasmBackend`, the kit and the test interpreter host |

## 6. Residuals and risks (disclosed)

- **The engine alone has no isolation**: `NoSandboxBackend` (never `Strict`) and `WasmBackend`.
- **No non-TCP egress for guests**, and only what the proxy understands (TLS SNI, HTTP `Host`).
- **Backend-enforced properties remain** (§2.2's list). Windows native-jail has no staging quota.
- **Writes reach the worktree at harvest, not live**, on every backend.
- **The contract slice is large and security-critical** (decision/transfer split, new primitives on
  two platforms, the proxy-only path under Kata, a Linux guest transport). It is its own ADR.
- **Copy-in/copy-out and proxy cost.** Not measured.
- **Drift** while the contract moves; G-S9 catches, does not prevent.
- **"Less time on sandboxes" is a separation**, and the contract slice lands here first.

## 7. What the Judge should challenge

- Whether proxy-only egress (item 3) is acceptable for agents that need non-HTTP protocols, or
  whether a per-protocol extension belongs in the contract-slice ADR.
- Whether `WasmBackend`'s exemption should end, with the wrapper minting its capability handles.
- Whether `quiesce()` and "no residue on reuse" being backend-enforced is acceptable for hosted and
  pooled backends the comparison note recommends.

## 8. Red-team record

Four rounds, every cited location re-checked before folding in; the full log, with each finding and
where it went, is `docs/planning/adr-245-red-team.md`.

| Round | Revision | Result | Turning findings |
|---|---|---|---|
| 1 | 1 | not ready: 3 FATAL, 8 MAJOR, 3 MINOR | `authorize_spec`, the `agent.*` loop and HandleRelay's checks live in backends |
| 2 | 2 | not ready: 3 FATAL, 6 MAJOR, 2 MINOR | `SandboxSpec` carries `CapabilitySet`; handle passing falsified under AppContainer; interpreter start re-authorizes |
| 3 | 3 | not ready: 0 FATAL, 6 MAJOR, 3 MINOR | exemption unenforced; guest library Windows-only; relay ids outlive authority |
| 4 (fresh reviewer) | 4 | not ready: 0 FATAL, 10 MAJOR, 8 MINOR | Kata's egress decides I2/I8 in the backend; rebuild only for VMs; harvest hangs, semantics, crash consistency; tenancy; self-declared `Strict` |
