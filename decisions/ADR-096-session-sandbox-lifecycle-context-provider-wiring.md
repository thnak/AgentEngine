# ADR-096 — Does session→sandbox lifecycle wiring for Shell go through `SandboxBackendRegistry`/`SandboxHandle`, or through a `ContextProvider` composed via the existing `Ms...` mechanism?

**Status:** Proposed — design and red-team phases complete (5 independent rounds, 2026-08-25);
**`SandboxToolProvider` itself is now implemented and tested (2026-08-25, post-ADR), still awaiting
Judged sign-off.** `src/backends/native_jail/sandbox_tool_provider.hpp` is real code: a
`ContextProvider` conformer matching Design B exactly (§2), proven by `tests/
test_sandbox_tool_provider.cpp` (19 checks — concept conformance, the non-copyable/move-only
property C2 depends on, lazy sandbox construction + `ctx.sandbox_fs` population + `run_shell`
contribution on `on_context()`, reuse across a later round with no directory recreated, the real
`invoke_tool()` pipeline actually running a command against the created directory, and C8's
digest-validation helper both against synthetic and real `compute_digest()` output). Full project
rebuild + full `ctest` suite (252/252) green, zero regressions. This still deviates from this file's
usual convention in one respect: implementation exists but this ADR has not yet been marked
`Judged` by the project owner — do not treat that sign-off step as already satisfied. §8's residual
list is updated to reflect what closing implementation did and did not resolve.

**Relates to:** `decisions/ADR-080-sandbox-backend-registry.md` (Proposed, awaiting Judged — this
ADR's own residual, "no real production consumption of a resolved backend," is the gap this ADR's
question responds to, and this ADR narrows but does not close it — see §7), `decisions/ADR-074-
composed-context-provider-consolidation.md` (the move-only `ComposedContextProvider<Ms...>` fix
this design's central safety property depends on), `decisions/ADR-030-session-scoped-codeact-
wiring.md` (`CodeActRunnerBinding`'s bind-once-forever contract — this ADR's Shell design explicitly
does not touch it; §7 states the Python question separately), `decisions/ADR-060-tool-call-
progress-channel.md` (the `report_progress`-reset precedent §5's shipped fix mirrors exactly),
`docs/planning/session-sandbox-lifecycle-wiring-design-draft.md` (the full record — six revisions,
five red-team rounds, summarized not duplicated below), this session's own Tier-1 work
(`EffectContext::sandbox_fs`, `include/agentengine/tools/read_sandbox_file.hpp`,
`src/backends/native_jail/session_shell_wiring.hpp`'s `SessionShellSandbox` — all real, built, and
tested; this ADR is about how something wires THEM into a real session, not about them).

## 1. The question

**Stated so it has a wrong answer:** two real, production-shaped sandbox-consuming code paths exist
in this codebase today — `tools/cli_chat.cpp`'s Python wiring and this session's own
`SessionShellSandbox` (Shell) — and both deliberately bypass `SandboxBackendRegistry`/`SandboxHandle`
entirely, going straight to `MediatedFileSystemAdapter`/runner types. Neither is reachable from a
real `AgentSession` today except by hand-wiring in one CLI harness; nothing central owns "does this
session have a sandbox, and how do its tools reach it."

Does closing this — giving Shell (this ADR's scope; Python is explicitly out of scope, see §7) a
real, reusable, per-session lifecycle — mean routing execution through the generic
`SandboxBackend::exec()`/`SandboxHandle` abstraction `SandboxBackendRegistry` already resolves
(making that abstraction, finally, a real consumer), or does it mean a narrower mechanism that never
touches `SandboxHandle` at all, reusing this codebase's own already-Judged `ContextProvider`/
`ComposedContextProvider<Ms...>` composition instead?

## 2. The competing designs

### Design A (rejected, deferred) — route Shell execution through `SandboxBackend::exec()`/`SandboxHandle`

Resolve a `SandboxBackendRegistry` entry per session, `create()` a real `SandboxHandle`, route every
`run_shell` call through `backend->exec(handle, req, ctx)`.

**Steelman.** This is the abstraction's stated purpose (008 §2a) — a session becomes portable
across `native_jail`/`wasm`/`kata`/a future `microvm` backend without its own code changing, which
neither of today's two direct-wiring paths can do.

**Rejected because:** `SandboxBackend::exec()`'s generic `ExecRequest const&, EffectContext& ->
result<ExecOutcome>` signature is coarser than what `MediatedShellRunner` actually exposes —
`ExecState` persistence across calls (real, tested this session:
`test_session_shell_wiring.cpp`'s "cd survives across calls" case) lives inside the concrete runner
type, not in the generic return shape. Making this real means either widening `SandboxBackend`'s
own contract (a bigger, contested change to a locked, Judged shape per 008) or the registry's
`exec()` closures becoming thin adapters that still, underneath, hold the same concrete state they
do today — in which case the portability benefit is illusory for the one backend (`native_jail`)
that actually implements Shell. Deferred, not permanently rejected: **if** `SandboxBackend::exec()`
is ever called this way, `EffectContext&` already threads through its real signature so per-call
attribution is not structurally lost — *provided* every real call stays strictly inside
`invoke_tool()`'s existing 10-step pipeline, never from a hand-built `EffectContext` outside it. A
future ADR pursuing this should be triggered by a real second backend needing to run Shell, not by
this ADR's own momentum.

### Design B (accepted) — `SandboxToolProvider`, a `ContextProvider` composed via `ComposedContextProvider<Ms...>`

A new type conforming to the existing `ContextProvider` concept
(`include/agentengine/core/context_provider.hpp`), constructing `SessionShellSandbox` (already
real, `src/backends/native_jail/session_shell_wiring.hpp`) lazily on its own first `on_context()`
call, holding it as an owned member. In that same call it (a) contributes `run_shell`'s
`ToolDescriptor` into `ContextContribution.tools`, and (b) assigns `ctx.sandbox_fs` directly via
`on_context()`'s mutable `EffectContext&` parameter — the exact seam Tier-1's own
`EffectContext::sandbox_fs` field was added for. Composed into a session via the existing
`Ms.../ComposedContextProvider<Ms...>` mechanism `ComposedQuickstartSessionBuilder::providers(...)`
already supports today (`HistoryProvider<Window<8>>`/`SkillsProvider`'s own precedent) — no new
builder step, no new `AgentSession` mutator.

**Steelman.** Reuses three already-Judged/already-real mechanisms instead of inventing one:
`ContextProvider`'s mutable `EffectContext&` (the write seam), `ComposedContextProvider<Ms...>`
(the composition seam, `ADR-074`), and `SessionShellSandbox` (the sandbox itself, this session's
Tier-1 work). Never touches `ADR-030`'s Judged claim-arbitration decision. `cli_chat.cpp`'s own
real, shipped `ToolDeclaringHistoryProvider` already proves the general "provider contributes
tools + mutates session state from one `on_context()` call" shape works in production for Python —
this design applies the identical shape to Shell.

**Cost, stated honestly, not oversold:** does not make backend selection portable the way Design A
would — `SandboxBackendRegistry`'s `Strict`/named resolution stays structurally disconnected from
this mechanism (§7, ADR-080's residual narrowed, not closed). Also: composing a non-copyable
provider this way makes `AgentSession::fork_from()` a **compile error**, not a runtime bug, for any
session type composed with it — empirically confirmed (§5), not merely reasoned, and treated here
as the correct, safe-by-construction outcome (mirroring how `capabilities_` is already
"deliberately still NOT copied" by `fork_from()`), not a defect.

### Design C (out of scope, not attempted) — a generic `PythonToolProvider` composed the same way, for sessions wanting both Python and Shell

Investigated, not designed further, because `CodeActRunnerBinding`'s (ADR-030) bind-once-forever,
no-release, non-copyable-in-the-relevant-sense state does not fit `ComposedContextProvider<Ms...>`'s
own move-only-composition assumptions the way `SandboxToolProvider`'s ownership does. **Rejected
for the generic case; a narrower alternative is named as future work, not designed here** (§7).

## 3. Falsifiable claims (Design B)

- **C1 (no new `AgentSession` mutator).** `SandboxToolProvider` composed via `Ms...` requires zero
  new public method on `AgentSession` — `ctx.sandbox_fs` is written entirely from within
  `on_context()`'s existing `EffectContext&` parameter. *Disproof: implementing this design requires
  adding any new public `AgentSession` method beyond what `ComposedContextProvider<Ms...>` already
  has.*
- **C2 (`fork_from()` fails closed at compile time, not runtime).** Composing `SandboxToolProvider`
  (holding a non-copyable `unique_ptr<SessionShellSandbox>`) into a session's `HistoryProviderT`
  makes `AgentSession::fork_from()` fail to compile for that session type, at the exact statement
  that would otherwise alias live sandbox state across sessions. *Disproof: such a session compiles
  and `fork_from()` either succeeds or fails at runtime instead of compile time.*
- **C3 (per-round freshness — no `fork_from()`/`clear_in_process_state()` re-arm needed for
  `sandbox_fs` itself).** `on_context()` runs fresh on every real `AgentSession` round (all resume
  paths included), so `ctx.sandbox_fs` never depends on state surviving a prior round's
  `effect_context_` reset. *Disproof: any real `AgentSession` code path reuses a
  `ContextContribution`/`ToolTable` from a previous round without a fresh `on_context()` call.*
- **C4 (`EffectContext` ordering hazard is closed by contract, not code).** No `ContextProvider`
  conformer in this tree reads a field another provider wrote via `EffectContext` in the same round
  — `Tool::invoke()` is the only real reader of `ctx.sandbox_fs`, and it runs strictly after every
  provider's `on_context()` for that round. *Disproof: a real `ContextProvider::on_context()`
  reads an `EffectContext` field another composed provider wrote in the same round.*
- **C5 (`clear_in_process_state()` needs a documented re-`engage()` obligation, not new mechanism).**
  Session reuse after `clear_in_process_state()` is a real, exercised pattern in this codebase;
  for a `ComposedContextProvider<Ms...>`-based session specifically, the existing `engage()` API
  (already proven as "a real recovery path") is necessary and sufficient to re-arm it. *Disproof:
  `engage()` has an undocumented precondition this claim misses, or no real code in this tree
  reuses a session after `clear_in_process_state()`.*
- **C6 (background-thread `sandbox_fs` dangling-pointer hazard, closed).** `background_task()`'s
  detached thread never observes a non-null `ctx.sandbox_fs` from the caller's `EffectContext`,
  regardless of what the caller held. *Disproof: a `Backgroundable` tool given a live `sandbox_fs`
  in the caller's context observes non-null on the detached thread.*
- **C7 (`SandboxBackendRegistry` has no live data path to `SandboxToolProvider`).**
  `check_sandbox_profile_availability()` is the only place `resolve_strict()`'s result is consulted
  at registration time, and it discards the resolved backend, keeping only pass/fail — no other
  call site in the tree retains a resolved backend's identity past registration. *Disproof: any
  call site anywhere retains and exposes a resolved `RegisteredSandboxBackend`'s name/identity past
  `register_agent<A>()`'s own call.*
- **C8 (per-session subdirectory naming is safe by construction, with defense in depth).** A
  session's scratch subdirectory name, derived as `compute_digest(session_id_bytes)` (already
  hex-encoded — no separate `to_hex()` step), is validated with an explicit `[0-9a-f]`/fixed-length
  character-class check before being spliced into a host path, matching this codebase's own
  `agent_spawn_worktree.hpp` "WT-6" precedent for an identically-shaped value. *Disproof: the
  digest-based name is spliced into a path with no explicit validation step, relying solely on the
  type's own construction guarantee.*

## 4. The red-team attack

Five rounds, 2026-08-25, all independent (fresh agents, not forks sharing context) — summarized;
full blow-by-blow in the design draft.

- **Round 1 (3 agents: security/I2-I3, C++ correctness, architecture-fit) on Revision 1.**
  **CONFIRMED, load-bearing, changed the design's central thesis**: Revision 1 claimed Option B
  "doesn't touch `ADR-030`'s claim semantics at all," specifically via a fabricated
  `CodeActRunnerBinding` "release-on-drop" mechanism that does not exist — `ADR-030` deliberately
  made Python binding permanent for the process's life, having already rejected release/reclaim.
  **Fixed**: Design B's Shell half constructs-and-releases per session (no such constraint); the
  Python question is separated out entirely, closing this ADR's Design C rather than extending
  Design B to cover it. Also confirmed: no new `HostSandboxSelection`-style wrapper type is needed
  for the mount-policy host root (an ordinary host-code constructor argument already matches
  `.api_key()`/`.store()`'s trust tier); `EffectContext&` already threads through
  `SandboxBackend::exec()`'s real signature (relevant only if Design A is ever revisited).
- **Round 2 (2 agents, one an empirical MSVC compile probe) on Revision 3.** **CONFIRMED by actual
  compile, not reasoning**: C1 and C2 above — a throwaway probe (a fake provider holding a
  non-copyable member, composed via `ComposedContextProvider<...>`, instantiated as a real
  `AgentSession`'s `HistoryProviderT`) produced a real MSVC error at the exact predicted line
  (`agent_session.hpp:1194`, `history_provider_ = source.history_provider_`). C3 also confirmed
  directly against all four real `AgentSession` `on_context()` call sites. **New gaps found, not
  previously considered**: C4's ordering hazard (`EffectContext` silently chains across composed
  providers in `Ms...` order, unlike `ContextContribution`, which is deliberately non-chained per
  OQ-18); a `clear_in_process_state()` unengaged-provider question (later itself corrected twice,
  see below); C7's registry-disconnection finding.
- **Round 3 (1 agent) re-checking the self-directed resolutions to those new gaps.** **CONFIRMED**
  C4's resolution (no real `ContextProvider` in the tree reads another provider's `EffectContext`
  write) and C7 (`check_sandbox_profile_availability()`'s discard, independently re-grepped).
  **REFUTED** the `clear_in_process_state()` resolution: the claim "no second caller exists,
  grep-confirmed" was false — two tests call the unlocked method directly and one (`test_rt_agent_
  session_tooling_and_delegation.cpp`'s case S5) actually reuses the session object afterward.
  **This is the third time an independent pass caught this line of work's own self-directed
  reasoning wrong**, always in a "grep-confirmed" empirical claim specifically (Round 1's
  fabrication, this round's own predecessor's unproven-until-Round-2 claim, now this) — recorded as
  a standing caution, not smoothed over.
- **Round 4 (1 agent) re-checking the correction to round 3's finding.** **CONFIRMED**, including
  one previously-unchecked detail: `ComposedContextProvider::engage()` has no hidden precondition
  beyond its own `engaged_` flag, so C5's proposed fix (re-`engage()` with a fresh tuple before the
  next `on_context()` call) is both correct and sufficient. First of five rounds to confirm a
  self-directed revision without finding a new error.
- **Round 5 (1 agent) on the two remaining open items (subdirectory sanitization, Python
  composition), before this ADR was written.** **REFUTED, corrected in this ADR's C8**: the
  originally-proposed `to_hex(compute_digest(...))` double-hex-encodes an already-hex `Digest` —
  corrected to `compute_digest(session_id_bytes)` directly. **REFUTED** the "no filtering needed at
  all" framing — real in-tree precedent (`agent_spawn_worktree.hpp`'s WT-6) adds an explicit
  character-class check even for a provably-hex-only value, as defense in depth against a
  security-relevant path splice; C8 above adopts that same discipline rather than relying solely on
  construction. **CONFIRMED, new finding**: nothing today creates the host scratch root or handles
  an already-existing directory at that path — this design must add that (§8, residual, not yet
  designed). On Python composition: **CONFIRMED** the general shape is buildable (a hand-authored
  combined provider mirroring `ToolDeclaringHistoryProvider`'s real, shipped "contribute tools +
  touch state in one `on_context()` call" pattern) but **understated** by the original framing —
  reconciling `ToolDeclaringHistoryProvider`'s deferred-`configure()`/non-owning-pointer Python
  init protocol with `SessionShellSandbox`'s owned/constructor-injected shape is real integration
  work, and such a combined provider, if used directly as `HistoryProviderT`, inherits the identical
  `fork_from()`-becomes-a-compile-error property C2 already accepts for `SandboxToolProvider` alone
  — a consequence the original framing never stated. Carried into §7 as explicitly unresolved,
  named future work, not designed further here.

## 5. Design evidence (not "executed evidence" — see Status)

Two pieces of this design have shipped as real, tested code, added at different times:

- **C6**, the `background_task()` `sandbox_fs` reset (shipped before this ADR was written).
  `EffectContext::sandbox_fs` is a raw pointer into session-owned state, the same dangling-pointer/
  unsynchronized-race hazard `ADR-060` already closed for `report_progress` in this exact function
  (a caller's `EffectContext` is copied by value onto a detached thread with no synchronization
  against `fork_from()`/`clear_in_process_state()`). Judged safe to implement without its own
  red-team pass, given how closely it mirrors an already-Judged line: `ctx.sandbox_fs = nullptr;`
  added immediately next to the existing `ctx.report_progress = [](ContentItem) {};` reset in
  `tool_pipeline.hpp::background_task()`. Regression-proofed with a new case "E" in `tests/
  test_agent_session_tool_call_progress.cpp`, mirroring that file's existing case "D" for
  `report_progress` exactly — a `Backgroundable` tool given a live, non-null `sandbox_fs` in the
  caller's `EffectContext` observes `nullptr` on the detached thread.
- **`SandboxToolProvider` itself** (shipped 2026-08-25, after this ADR's initial version was
  written): `src/backends/native_jail/sandbox_tool_provider.hpp`, exactly matching Design B's
  construction (§2) — lazy `SessionShellSandbox` construction on first `on_context()`, `run_shell`
  contribution plus `ctx.sandbox_fs` assignment on every call, digest-based per-session subdirectory
  naming with the C8 defense-in-depth check, and idempotent host-directory creation (closing the
  round-5 residual named below, §8). Proven by `tests/test_sandbox_tool_provider.cpp`: static
  `ContextProvider` concept conformance and non-copyable/move-only proofs (C1, C2's premise), a real
  `on_context()` call constructing the sandbox and populating `ctx.sandbox_fs`, the contributed
  `ToolDescriptor` running a real command through the actual `invoke_tool()` ten-step pipeline
  (mirroring `test_session_shell_wiring.cpp`'s own end-to-end shape) with the write landing on the
  real host filesystem at the digest-named directory, a second `on_context()` call (a later round)
  reusing the same sandbox with no new directory created and the shell's state still showing the
  first call's file, and direct unit coverage of `check_session_digest` (empty, wrong-length,
  uppercase, non-hex all rejected; a real `compute_digest()` output accepted). Full project rebuild
  and the full `ctest` suite (252/252) green — zero regressions, including the pre-existing suite
  this ADR did not touch.

C1-C5, C7, C8 were originally verified against real, already-existing code (an empirical compile
probe for C2; direct reads of `context_provider.hpp`/`composed_context_provider.hpp`/
`agent_registry.hpp`/real test files for the rest) — that verification of the design's *premises*
is now corroborated by the finished type itself actually compiling, composing, and running as
designed (C1, C3's freshness premise, C6, and C8 all directly exercised by `test_sandbox_tool_
provider.cpp`; C2, C4, C5, C7 remain proven the way §4/§6 originally record, not re-proven here,
since nothing about implementing `SandboxToolProvider` touched `fork_from()`, another real
`ContextProvider`'s own code, `clear_in_process_state()`, or the registry).

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| C1 — no new `AgentSession` mutator | **CORRECT** | `context_provider.hpp:101-104`'s concept signature already takes `EffectContext&`; no code changed in `agent_session.hpp` to support this design. |
| C2 — `fork_from()` fails closed at compile time | **CORRECT, empirically proven** | Real MSVC compile of a throwaway probe, error at the exact predicted line (`agent_session.hpp:1194`), Round 2. |
| C3 — per-round freshness | **CORRECT** | All four real `on_context()` call sites checked directly (`resolve_interaction()`, `resolve_codeact_ask()`, `resolve_hook_decision()`, `run_rounds()`'s own loop), Round 2. |
| C4 — `EffectContext` ordering hazard closed by contract | **CORRECT** | Every real `ContextProvider` conformer in the tree checked (`skill_provider.hpp`, `memory_provider.hpp`, `vector_rag_context_provider.hpp`, `ToolDeclaringHistoryProvider`) — none reads a field another provider wrote, Round 3. |
| C5 — `clear_in_process_state()` re-`engage()` obligation | **CORRECT, after correction** | Round 3 refuted the original "not a gap" claim (real reuse exists — S5, `test_rt_agent_session_codeact_ask_max_turns.cpp:283`); Round 4 independently confirmed the corrected fix (re-`engage()`) is both necessary and sufficient. |
| C6 — background-thread dangling pointer closed | **CORRECT, shipped** | `tests/test_agent_session_tool_call_progress.cpp` case "E," full rebuild + affected suite green. |
| C7 — registry has no live data path | **CORRECT** | `check_sandbox_profile_availability()` (`agent_registry.hpp:366-371`) read directly; every `resolve_strict`/`resolve_named` call site in the tree independently re-grepped, Round 3. |
| C8 — subdirectory naming safe with defense in depth | **CORRECT, after correction** | Round 5 caught the double-hex-encoding error and the "no filtering needed" overclaim; corrected form matches `agent_spawn_worktree.hpp`'s own WT-6 precedent. |

## 7. The decision

**Design B is accepted as the design direction for Shell's session-sandbox lifecycle wiring** —
`SandboxToolProvider`, a `ContextProvider` composed via the existing `Ms.../ComposedContextProvider<Ms...>`
mechanism, is not yet implemented, and this ADR does not authorize treating it as implementation-
ready without closing §8's residuals first.

**Binds, once implemented:**
- `include/agentengine/core/context_provider.hpp`'s `ContextProvider` concept and `include/
  agentengine/core/composed_context_provider.hpp`'s `ComposedContextProvider<Ms...>` — reused
  exactly as they exist today; this ADR proposes no change to either.
- `src/backends/native_jail/session_shell_wiring.hpp`'s `SessionShellSandbox` — reused exactly as
  built (Tier-1, this session), becomes `SandboxToolProvider`'s owned internal state.
- `tool_pipeline.hpp::background_task()`'s `ctx.sandbox_fs = nullptr;` reset — already shipped (§5).

**Explicitly out of scope, named rather than left implied:**
- **Design A (routing through `SandboxBackend::exec()`/`SandboxHandle`) remains available, not
  rejected outright** — deferred until a second real backend needs to run Shell, per §2's own
  steelman. `ADR-080`'s own "no real production consumption" residual is **narrowed, not closed**
  by this ADR: `wasm`'s tool-bridge path remains the only real `SandboxHandle` consumer either way,
  unchanged by anything here.
- **Python's session-lifecycle wiring is untouched by this ADR.** `cli_chat.cpp`'s existing direct
  wiring (`ToolDeclaringHistoryProvider`-shaped, not composed via `Ms...`) stays exactly as it is.
  A session wanting BOTH Python and Shell tools needs a hand-authored combined provider (Round 5's
  finding) — real, buildable, but genuinely more integration work than "compose two providers," and
  **not designed in this ADR**. Whoever builds it must also account for the same `fork_from()`-
  becomes-a-compile-error consequence C2 already accepts for `SandboxToolProvider` alone.
- **`SandboxBackendRegistry`'s resolved backend does not connect to `SandboxToolProvider`'s
  construction, and this ADR does not build that connection.** C7's own finding: no live data path
  exists (`check_sandbox_profile_availability()` discards the resolved backend). Building one would
  need three new pieces (§8) — deliberately not built speculatively; `native_jail` is, in practice,
  the only real backend implementing Shell today.
- **`wasm`'s tool-bridge path is untouched** — stays on its own `WasmToolBridge` construction,
  independent of this design.

## 8. Residual risks

- **`SandboxToolProvider` was unimplemented at this ADR's original writing; it is now implemented
  and tested (2026-08-25, §5), still awaiting `Judged` sign-off.** The gap between "design accepted"
  and "code exists" that used to be this section's largest residual is closed. What remains open:
  whoever reviews this for `Judged` status should independently re-verify §3's claims against the
  real code (per the standing caution below), and no host application anywhere actually composes
  `SandboxToolProvider` into a real, production `AgentSession` yet — only the test file does. That
  last gap (a real caller) is not itself designed or scoped here.
- **Nothing today creates the per-session scratch directory or handles a pre-existing one at that
  path** (Round 5 finding, C8's own gap) — **closed by the implementation**:
  `SandboxToolProvider::ensure_sandbox()` calls `std::filesystem::create_directories()` idempotently
  before constructing `SessionShellSandbox`, mirroring `cli_chat.cpp:239`'s own idiom, proven by
  `test_sandbox_tool_provider.cpp`'s Part 1 (constructs against a scratch root that does not exist
  yet) and Part 3 (a second round reuses the same directory, nothing is recreated). What is still
  genuinely undecided, not merely untested: what SHOULD happen on a leftover directory from an
  abandoned prior session sharing the same digest (impossible today only because `compute_digest` is
  deterministic and no two live sessions can share one `session_id` — an abandoned-then-reused
  `session_id` is not ruled out at this layer). `create_directories()` on an existing path is a
  harmless no-op either way; whether that is the RIGHT behavior for a leftover directory with stale
  content inside it (silently reuse vs. refuse vs. clean first) was not decided, is not enforced,
  and is not tested.
- **Windows `MAX_PATH`/full-path-length budget for `host_root` + digest subdirectory** was not
  checked this pass — named as unverified, not assumed safe.
- **`session_id`'s own admission-path trust tier** (whether it can ever be influenced by anything
  short of pure host code) was not independently re-verified beyond Revision 3's own finding that
  `QuickstartSessionBuilder::session_id(id)` accepts a plain `std::string` with no upstream
  validation — C8's digest-based design makes this moot for path-splice safety specifically, but the
  broader question of `session_id`'s provenance guarantees is unaddressed.
- **The Python/Shell combined-provider design (§7) is named, not designed.** A real attempt needs
  its own scrutiny of the `ToolDeclaringHistoryProvider`-shape/`SessionShellSandbox`-shape
  reconciliation Round 5 found understated, and should get its own red-team pass before
  implementation, matching this ADR's own process rather than being treated as a trivial extension.
- **This ADR's own track record**: three of five red-team rounds on this line of work found this
  design's own self-directed reasoning wrong or overstated, always in a "grep-confirmed"/empirical
  claim specifically (§4). The two rounds that found nothing wrong (rounds 4 and — partially — 5)
  are evidence those specific corrections happened to be right, not evidence the process has become
  more reliable. The implementation pass (§5) independently re-verified C1, C3, C6, and C8 by
  actually building and testing against them, and found no new error in any of the four — real
  corroboration, not just another self-directed claim, but **still not independent red-team
  scrutiny**: the same person who designed and red-teamed this ADR also wrote the implementation and
  its own tests, so it cannot rule out a shared blind spot the way a genuinely independent round
  could. **C2, C4, C5, and C7 were NOT re-touched by implementation at all** — nothing in
  `SandboxToolProvider`'s own code path exercises `fork_from()`, another real `ContextProvider`'s
  own `EffectContext` reads, `clear_in_process_state()`, or the registry, so those four claims still
  rest entirely on §4/§6's original verification. Whoever reviews this for `Judged` status should
  weigh all of this — not trust this ADR's own verdicts in §6 by default, and treat C2/C4/C5/C7
  as the ones most worth independently re-checking, since C5, C7, and C8 already each started as a
  correction of an earlier, wrong self-directed claim.
- **`ADR-080`'s "selection without consumption" residual is narrowed, not closed** (§7) — carried
  forward exactly as that ADR's own §8 already names it, unchanged by this work.
- **`ADR-100` (2026-08-27) reconciled this ADR against `ADR-098`, per explicit project-owner
  direction, and found no real gap between them** — `ADR-080`'s own Finding O already correctly
  decided neither Python nor Shell routes through `SandboxBackendRegistry`, so there was never a
  bridge to build here. What that reconciliation pass actually surfaced, worth more than the
  reconciliation itself: `run_shell` has zero OS-level containment (Python's own
  `NativeJailBackend::create_python_worker()` jail is reached via a direct `NativeJailBackend&`
  dependency, never the registry — the real precedent for any future Shell equivalent), and the
  mediated-shell evaluation loop (`mediated_shell_grammar.hpp`) had no wall-clock/iteration cap,
  a live, model-reachable DoS gap this `SandboxToolProvider`'s own capability grants (`FsRead`/
  `FsWrite`, path-scope only) do not offset. **That specific gap is now closed** (same day):
  `MediatedShellRunner`'s `wall_clock_budget` parameter + a per-statement deadline check in
  `evaluate_statement()`, proven against a real 3^20-body-execution exponential-blowup script in
  `tests/test_mediated_shell_runner_wall_clock_timeout.cpp`. Shell's broader OS-level-containment gap
  (no AppContainer/Job Object at all) is unchanged — see `ADR-100`'s still-deferred
  `create_shell_worker()` follow-on.
