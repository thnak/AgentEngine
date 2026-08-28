# ADR-102 — Phases 1-5 of merging the identity-native sandbox/worktree design (ADR-099) into production: does `IdentityAuthority`/`Grant<T>`/`AsyncQuota<T>`/`Ledger<Store>`/`SandboxRuntime`/`MandatorySandboxProvider` port cleanly into `include/agentengine/`, does the real `agentengine::trust::Principal` collision get resolved by reuse or by a distinct name, and does the first real host wiring (`tools/cli_chat.cpp`) actually work end to end?

**Status:** Proposed — design scoped, Phases 1 through 5 (the first real, user-reachable host wiring)
implementation complete and independently red-teamed (2026-08-28), plus closing `AgentSession::
fork_from()`'s own long-disclosed session-serialization gap (Phase 3 §22/Phase 4 §29), plus a second
Phase 5 slice (2026-08-28) making `ComposedContextProvider<Ms...>` a real production consumer for the
first time anywhere in this codebase (§41 onward). **This ADR does not authorize the whole
identity-native design's production merge** — it authorizes Phases 1-4 (identity/quota primitives,
then `Ledger`, then `SandboxRuntime`/`ExecutionSurface`, then a session-wired `MandatorySandboxProvider`)
plus two specific slices of Phase 5 (`tools/cli_chat.cpp` wiring plus the `fork_from()` fix; and
`ComposedContextProvider<Ms...>` composing `SandboxToolProvider`+`MandatorySandboxProvider` into a real
session for the first time — `ContainerdExecutionSurface`/ADR-101 promotion, Windows/Linux parity, and
`SandboxToolProvider`'s own `fork_from`-becomes-compile-error negative probe remain deferred,
unauthorized follow-on work), matching this project's own precedent of narrow implementation ADRs
following a broad design-acceptance ADR (ADR-012→ADR-080, ADR-086→ADR-087, both cited approvingly
inside ADR-099 itself). `ADR-099` §7 states explicitly: "this ADR authorizes the design, not an
implementation merge... binds once a future implementation ADR (or ADRs) actually wires this in." This
is that ADR, for Phases 1-5's `cli_chat.cpp` slice, the `fork_from()` fix, and the `ComposedContextProvider
<Ms...>` slice (this file's own name still reads "phase-1" — kept for URL/citation stability now that
other work references it; §9 onward covers Phase 2, §16 onward covers Phase 3, §23 onward covers Phase 4,
§30 onward covers Phase 5's `cli_chat.cpp` slice, §37 onward covers the `fork_from()` fix, §41 onward
covers the `ComposedContextProvider<Ms...>` slice).
Real, compiled, tested:
- **Phase 1**: `include/agentengine/trust/identity_authority.hpp`, `include/agentengine/rt/
  async_quota.hpp`, `tests/test_identity_authority_grant.cpp` (14 checks, passing), two
  `tests/compile_fail/identity_*.cpp` negative probes (both passing at CMake configure time). One
  independent red-team round found and fixed a real MUST-FIX (a cross-tenant identity-collision gap
  in `adopt()`) same day.
- **Phase 2**: `include/agentengine/core/ledger.hpp` (`Ledger<Store>`/`BranchHandle<Store>`/
  `Checkpoint`/`merge_trees()`), `tests/test_ledger.cpp` (47 checks, passing). One independent
  red-team round found and fixed a real MUST-FIX (§10/§12: after a legitimate descendant identity
  merges its work into a parent branch, the parent's own creator was permanently locked out of
  reading its own branch's new head — breaking the "orchestrator spawns a sub-agent, sub-agent merges
  back, orchestrator resumes" flow this whole design exists to support) plus two disclosed,
  not-yet-fixed residuals (§12).
- **Phase 3**: `include/agentengine/sandbox/{execution_surface,docker_execution_surface,
  real_io_filesystem,sandbox_runtime}.hpp` (`ExecutionSurface`/`SurfaceRunOutcome`,
  `DockerCliBackend`/`DockerExecutionSurface`, `RealIoFileSystem`, `SandboxRuntime`/
  `SandboxRunOutcome`/`RunCost`/`ResetCost`), `tests/test_sandbox_runtime.cpp` (10 checks, all
  requiring and exercising a REAL Docker daemon, passing). One independent red-team round found and
  fixed a real SHOULD-FIX (§19: `merge_into()`/`discard()` mutated `branch_` without holding
  `exclusivity_`, unlike every sibling method) plus corrected one disclosure comment that was found to
  overclaim a leak's real trigger condition (§19) — no MUST-FIX found this phase.
- **Phase 4**: `include/agentengine/sandbox/mandatory_sandbox_provider.hpp` (`MandatorySandboxProvider
  <Surface>`/`RunCommandTool`), composed for the first time as a REAL, production `agentengine::rt::
  AgentSession<...>`'s actual `HistoryProviderT`, and driven for the first time in this design's
  entire history through the real, unmodified `invoke_tool()` 10-step pipeline (`tests/
  test_mandatory_sandbox_provider.cpp`, 6 check groups, requiring and exercising a REAL Docker
  daemon). One independent red-team round found and EMPIRICALLY PROVED a real MUST-FIX (§26: the
  port's own first version used a naive drive loop that a targeted repro showed breaks `AsyncMutex`'s
  mutual exclusion — a genuine, reproducible use-after-free — under real cross-thread contention on an
  `AsyncQuota` shared across sibling sessions, a real, demonstrated usage pattern, not a hypothetical
  one) — fixed by porting the prove-phase original's own ASan-hardened `block_on<T>()` for real, into
  a NEW shared production primitive (`include/agentengine/rt/block_on.hpp`), with its own dedicated
  positive-control test (`tests/test_rt_block_on.cpp`) proving the fix under the identical contention
  shape the red-team's own repro used. One further SHOULD-FIX disclosed, not fixed (§26: `AgentSession::
  fork_from()` is unlocked and could in principle race a concurrent in-flight `run()` on the same
  source session — not live today, no real production caller of `fork_from()` exists anywhere in this
  codebase).
- **Phase 5 (`tools/cli_chat.cpp` slice only)**: `run_command` wired into the REAL, actual CLI host for
  the first time this whole design has ever been reachable by a real user running a real binary, not
  just a test. Found and fixed a real, independently-undetected-for-3-phases MUST-FIX during bring-up:
  `Ledger`'s own `MergeConflict`/`MergeResult` (Phase 2) silently collided, byte-for-byte on the fully-
  qualified name, with an unrelated, already-shipped production type in `core/worktree_merge.hpp` (025
  §4's own branch-merge mechanism) — no build before this one had ever `#include`d both headers in one
  translation unit, so the redefinition compile error stayed latent since Phase 2 landed. Renamed to
  `LedgerMergeConflict`/`LedgerMergeResult` (§30). A real, live, OpenRouter-backed interactive smoke
  test confirmed `run_command` genuinely executes a real command in a real container end to end, with
  the result independently verified from the raw request/response dump, not merely the model's own
  narration — and surfaced two further real findings: a CONFIRMED (not merely disclosed) third trigger
  for Phase 3's already-named Docker container-leak residual (this CLI's own deliberate `std::_Exit(0)`
  skips `DockerExecutionSurface`'s destructor on every ordinary exit, not only a crash), and a genuine
  test-coverage gap an independent red-team round found (the exact "one `ContextProvider` embeds
  `MandatorySandboxProvider` and merges its contribution outside any skill-scoping" composition
  `cli_chat.cpp` uses had zero automated coverage anywhere) — closed with a new, Docker-independent
  test, `tests/test_mandatory_sandbox_provider_composed.cpp` (§30).
- **Closing `AgentSession::fork_from()`'s own session-serialization gap** (§37 onward): the SAME
  structural finding Phase 3 §22 and Phase 4 §29 each independently disclosed but did not fix —
  `fork_from()` ran with no serialization against a concurrent in-flight round on its own `source` —
  is now closed for real, on the single most heavily-used class in the codebase, proven with a real
  two-thread race test whose own status as a genuine positive control was empirically verified via a
  forced-revert (fails 3/3 without the fix, passes with it). An independent, dedicated red-team round
  found and disclosed (not fixed) a real, empirically-confirmed new hazard the fix itself introduces —
  a self-deadlock if `fork_from()` is ever called reentrantly against a lock the calling thread already
  holds — not reachable through any real call site today, but plausibly exactly what a near-future
  `agent.spawn`-style tool would hit. A real process incident during this same work is also disclosed
  (§39): a red-team subagent's own cleanup step reverted `tests/CMakeLists.txt` via git rather than
  undoing only its own scratch additions, silently discarding seven of this session's own legitimate
  test registrations — caught via a test-count drop, fully recovered, no test source content lost.

Full project rebuild (370/370 targets after Phase 1, 372/372 after Phase 2, 373/373 after Phase 3,
376/376 after Phase 4, 306/306 including `agentengine_cli_chat` plus 378/378 test-suite targets after
Phase 5 and the `fork_from()` fix — adding `test_mandatory_sandbox_provider_composed` and `test_rt_
agent_session_fork_from_serialization`) and full `ctest` suite green across twelve independent full runs
(280/280 after Phase 1 twice, 281/281 after Phase 2 twice, 282/282 after Phase 3 twice, 283/283 then
284/284 after Phase 4's own two rounds, 284/284 then 285/285 after Phase 5's own two rounds, then 286/286
three times after the `fork_from()` fix, including once after recovering from the process incident above),
zero regressions from any phase (`test_native_jail_backend_windows` flaked twice and `test_rt_workflow_
supervisor_merge_on_join`/`test_rt_spawn_cost_budget` each flaked once under `-j4` parallel execution
across the whole effort — all spawn real OS-level processes/threads under real resource contention, a
known environment-sensitivity class for `-j4`, unrelated to any file this ADR's phases touch, and passed
cleanly in isolation and on a subsequent full re-run every time they were checked). Still awaiting the
project owner's own `Judged`
sign-off — this session cannot self-Judge a change to security-critical identity/authority/checkpoint/
execution code, per CLAUDE.md.

**Relates to:** `decisions/ADR-099-identity-native-sandbox-worktree-capability-model.md` (the
design-acceptance record this ADR narrows from; §3's claims for `IdentityAuthority`/`Grant<T>`/
`AsyncQuota<T>` are re-proven here against the ported code, not re-derived), `decisions/ADR-096-
session-sandbox-lifecycle-context-provider-wiring.md` (`SandboxToolProvider`, the coexistence
decision — §7 below), `include/agentengine/trust/principal.hpp` (the real, shipped `Principal` this
ADR's central finding is about), `include/agentengine/trust/capability.hpp` (`CapabilitySet`, the
existing sole production authority model — the "empty by construction, one mint entry point" pattern
`IdentityAuthority` already independently converged on), `include/agentengine/rt/spawn_cost_budget.hpp`
(`SpawnCostBudget`, the real, shipped precedent for a coroutine-native, `AsyncMutex`-guarded quota
primitive `AsyncQuota<T>`'s own port follows), `include/agentengine/sandbox/sandbox.hpp` (the real,
shipped `SandboxBackend`/`ExecOutcome` vocabulary Phase 3's `ExecutionSurface`/`SurfaceRunOutcome`
deliberately does NOT conform to or reuse), `decisions/ADR-101-sandbox-backend-tree-refinement-
reconciliation.md` (a separate, still-Proposed/unjudged tree-capable `SandboxBackend` refinement track
Phase 3 deliberately stays independent of), `027-Vocabulary-and-Naming.md` §4 (new rows added by
this ADR), `docs/planning/proofs/identity_authority/identity_authority.hpp`, `docs/planning/proofs/
async_quota/async_quota.hpp`, `docs/planning/proofs/worktree_io/{worktree_ledger.hpp,merge_trees.hpp,
real_io_filesystem.hpp}`, `docs/planning/proofs/execution_surface/{execution_surface.hpp,
docker_execution_surface.hpp,sandbox_runtime.hpp}`, `docs/planning/proofs/mandatory_sandbox/
mandatory_sandbox_provider.hpp`, and `docs/planning/proofs/common/block_on.hpp` (the prove-phase source
this ADR ports from, unmodified as source material — the port is a new, separate file, not an edit to
the prove-phase original), `include/agentengine/core/context_provider.hpp` (the real `ContextProvider`
concept `MandatorySandboxProvider` conforms to), `include/agentengine/core/tool_pipeline.hpp` (the real
10-step `invoke_tool()` pipeline Phase 4 drives `RunCommandTool` through for the first time),
`include/agentengine/rt/agent_session.hpp` (`AgentSession<ChatClientT,StateT,HistoryProviderT>`,
`fork_from()`/`clear_in_process_state()`/`history_provider()`, unmodified), `tools/cli_chat.cpp`
(`ToolDeclaringHistoryProvider`, the real, shipped bare-`HistoryProviderT` precedent Phase 4 mirrors,
deliberately not `ComposedContextProvider<Ms...>`), `include/agentengine/rt/agent_workflow_executor.hpp`
(`agent_executor_detail::drive()`, the real, production precedent for a naive drive loop's own
CONCURRENCY CONTRACT discipline — the same discipline Phase 4's own red-team found insufficient for
this specific composition, closed instead by the new `rt/block_on.hpp`), `include/agentengine/core/
worktree_merge.hpp` (025 §4's own real, already-shipped branch-merge mechanism — the source of Phase
5's own real `MergeConflict`/`MergeResult` naming-collision finding, §30).

## 1. The question

**Stated so it has a wrong answer:** the identity-native design's prove-phase `IdentityAuthority`/
`Principal`/`Grant<T>`/`AsyncQuota<T>` (`docs/planning/proofs/`) is real, compiler-verified,
red-teamed code — but it was deliberately built standalone, never linked against `include/
agentengine/`, per ADR-099 §0's own "no-reuse-at-design-time" framing. Porting it into production
means it now sits in the SAME namespace/vocabulary space as the real, already-shipped `agentengine::
Principal` (`trust/principal.hpp`, a per-request authenticated identity: `{id, tenant_id, kind,
on_behalf_of, delegation_depth}`) — a name and shape the prove-phase `probe::Principal`
(`{id_: uint64_t, label_: string}`, `IdentityAuthority`-minted, durable, ancestor-tracked) does not
match. Does the port **reuse** the real `Principal` type directly (retrofitting `IdentityAuthority`'s
durable-ancestry semantics onto a type never built to carry them), or does it keep a **distinct**
identity-authority-scoped type, bridged to the real `Principal` at the one seam that needs it?

## 2. The competing designs

### Design A (rejected) — retrofit the real `agentengine::Principal` to carry `IdentityAuthority`'s semantics

Make the real, shipped `Principal` durable/ancestor-tracked/`IdentityAuthority`-minted directly,
eliminating the separate prove-phase type.

**Rejected because:** this is the same class of mistake ADR-099 §2 already rejected once, one level
up — Design A there retrofitted a durable, identity-scoped authority model onto `CapabilitySet`, a
type never built to carry it, and four independent red-team rounds each found a genuine structural
gap (no identity field, closed-variant extension tax, sync/async mismatch) that "no amount of
additional design prose" could close without adding what the type was never built for. The real
`Principal` has the identical shape of problem: no mint-chain, no restart-stable allocator, freely
constructible by anyone who can call `make_embedded_principal` (a plain value type, by design, per
018's own "the host is trusted" provenance model) — retrofitting `IdentityAuthority`'s ancestry
semantics onto it would either weaken the real type's own simplicity for every one of its many
existing callers, or produce a partial, unsound hybrid. Not attempted past this framing.

### Design B (accepted) — keep a distinct type, bridge via a retyped `adopt()`

Port `probe::Principal` as a new, real, `agentengine`-namespaced type under a **different name**
(`IdentityHandle`, this ADR's choice — see §7's naming rationale), keeping `IdentityAuthority`'s own
durable, ancestor-tracked, restart-stable-id semantics unchanged. Bridge to the real `Principal` via
`IdentityAuthority::adopt()`, retyped from two bare strings (`real_id`, `real_on_behalf_of`) to a
single, type-safe `adopt(agentengine::Principal const&)` parameter — this bridge is not new work, it
already exists in the prove-phase source (`identity_authority.hpp:161-179`, §24.3) and is already
exercised by `mandatory_sandbox_provider.hpp:256`; this ADR promotes and retypes it, does not invent
it.

**Steelman.** `027-Vocabulary-and-Naming.md` §1's own rule: "keep a different name only where the
concept genuinely differs — and say why." The two types answer genuinely different questions — real
`Principal` answers "who is this request attributed to, right now" (007's per-request attribution
model); `IdentityHandle`/`IdentityAuthority` answers "does this durable authority-subject, once
minted, remain the same subject across a process restart, and what is its real ancestry chain" — a
question `Principal` was never asked to answer and, per Design A's rejection, should not be
retrofitted to answer. `Principal` is also registered, canonical, MAF-derived vocabulary
(`027-Vocabulary-and-Naming.md:129`) — not this project's own coinage to redefine.

**Cost, stated honestly:** two identity vocabularies now coexist in the trust module, bridged at
exactly one seam (`adopt()`). A caller unfamiliar with the distinction could genuinely confuse them —
mitigated by the name choice (§7) and by every new type's own file-top comment stating the
distinction explicitly, not left implicit.

## 3. Falsifiable claims

Re-proving ADR-099 §3's own claims for these three types, against the **ported**, `agentengine`-
namespaced code specifically — not re-deriving them, and not assuming a rename/promotion is
behavior-preserving by default (ADR-099 §35's own standing lesson: "fixing one primitive silently
broke two other probes" — a real precedent for exactly this kind of change).

- **C1 (non-copyability/move-only is compiler-enforced on the ported code).** `IdentityAuthority` is
  non-copyable/non-movable; `IdentityHandle`/`Grant<T>` have no public constructor other than through
  `IdentityAuthority`. *Disproof: any of these types compiles when copied/default-constructed outside
  `IdentityAuthority`, in the ported code specifically.*
- **C2 (minting fails closed against an unminted subject).** `AsyncQuota<Kind>::mint_root()` rejects
  an `IdentityHandle` the calling `IdentityAuthority` never actually minted. *Disproof: a caller
  fabricates its own `IdentityHandle`-shaped value and successfully obtains a quota without ever
  having been minted by the real authority, in the ported code.*
- **C3 (`AsyncQuota::try_consume()` is identity-scoped).** A spender must be the quota's owner or a
  subject it explicitly split a share to. *Disproof: an unrelated `IdentityHandle` successfully
  consumes from a quota it has no relationship to, in the ported code.*
- **C4 (the `Principal` collision is resolved by a distinct name, not reuse, with a typed bridge).**
  No type named `Principal` is introduced by this port; `IdentityAuthority::adopt()` takes
  `agentengine::Principal const&`, not bare strings. *Disproof: a type named `Principal` appears
  anywhere in the ported files, or `adopt()` still takes bare strings.*
- **C5 (naming lint passes for the right reason).** Every new public name (`IdentityAuthority`,
  `IdentityHandle`, `Grant<Payload>`, `AsyncQuota<Kind>`) is registered in `027-Vocabulary-and-Naming.md`
  §4 with an explicit rationale, not merely suppressed. *Disproof: `tools/naming_lint.py` only passes
  via an `// ae-naming-lint: allow` suppression comment for one of these names, with no corresponding
  027 row.*

## 4. The red-team attack

One independent, fresh-agent adversarial round (2026-08-28), against the actually-landed code (not
the plan) — read `ADR-102` in full, diffed both ported files line-by-line against their prove-phase
originals, read the real `Principal`/018 §6, independently re-ran the test and both compile-fail
probes, independently re-ran `tools/naming_lint.py` and read its real scan logic (not its docstring).

**MUST-FIX, found and fixed same day**: `IdentityAuthority::adopt()`'s first version keyed its
`adopted_` bridging map by `real_principal.id` ALONE, never consulting `tenant_id` — `Principal::id`
is documented as "opaque to the core," not globally unique; `tenant_id` is what actually scopes it
(018 §6: "Tenant is a first-class dimension of principal, session, memory scope, sandbox, and
QUOTA... a cross-tenant leak is a release-blocking defect class"). Two different tenants' principals
sharing the same `id` string (plausible under any non-globally-namespaced id scheme, e.g. usernames)
would have silently merged into ONE `IdentityHandle` — same internal id, same ancestry entry, and
(once `AsyncQuota`/`Grant<T>` get real callers in Phase 4/5) the same durable quota/grant subject
across tenants — exactly the leak class 018 §6 names as release-blocking, on the exact primitive
(quota) it names explicitly. Not exploitable at landing time (no production caller of `adopt()`
existed yet, confirmed by grep), but silent — ADR-102's own text never mentioned `tenant_id`
anywhere. **Fixed**: `adopted_` is now a two-level map keyed by `(tenant_id, id)`, not a single map
keyed by a concatenated string (no delimiter-collision risk); the `on_behalf_of` ancestry lookup is
likewise scoped to the child's own tenant, so cross-tenant delegation is never silently recognized
either. Durable persistence (`identity_adopted.log`) updated to a three-field record
(`tenant\treal_id\tid`). Proven with a new, real test case: two `Principal`s with the SAME `id` but
DIFFERENT `tenant_id` get distinct `IdentityHandle`s; idempotency re-confirmed within one tenant after
the fix; a cross-tenant `on_behalf_of` claim is confirmed NOT recognized as ancestry.

**Checked, no finding — the test's "structurally unreachable-false" claim for C2** (`AsyncQuota::
mint_root()`'s `is_known()` gate): independently traced all three construction paths
(`mint_root`/`derive_child`/`adopt`), confirmed each inserts into `ancestry_`/`adopted_` under the
same lock before returning a handle to any caller — no TOCTOU gap, no second construction path,
matches the test's own claim. One NIT-level, non-live caveat named: `bootstrap()`'s magic-static is
defined inline in the header, so linking `agentengine` core code into more than one shared
library/DLL would give each module its own singleton — not live today (no such split exists in this
build), noted as a residual (§8), not fixed.

**Checked, no finding — C1/C4/C5**: both compile-fail probes fail for the claimed reason, not a
vacuous unrelated error (correct include paths, exactly one offending statement each); no type named
`Principal` appears in either ported file; `naming_lint.py`'s actual scan (not its docstring) covers
`include/agentengine/rt/` too, and genuinely reports "OK" against the real vocabulary rows.

**Checked, no finding — `AsyncQuota<Kind>` move/copy semantics, `failure_class` choices, I2/I3
relevance, `IdentityHandle`/`Principal` type confusion**: all verified by direct reasoning against
the member list / real call sites, not assumed by analogy to the prove-phase original. See the full
agent report (session transcript) for the detailed trace of each.

**SHOULD-FIX, inherited from the prove-phase original, not introduced by the port, named as a
residual (§8) rather than fixed here**: `persist_high_water_mark()`/`load_durable_state()` defend
only the very-first-run concurrent-start race: two long-running processes sharing the same
`durable_dir` can still independently advance `next_id_` and issue colliding ids once either has
moved past what the other read at startup. Same code, same partial defense as the prove-phase
original — carried forward, not silently worsened, but now sitting in code this ADR calls "real
production code," so it is named here explicitly rather than left implicit a second time.

## 5. Executed evidence

- `include/agentengine/trust/identity_authority.hpp`, `include/agentengine/rt/async_quota.hpp`:
  compiled clean under MSVC 19.51 (`cl /std:c++latest`), zero new warnings.
- `tests/test_identity_authority_grant.cpp`: 14 checks (11 original + 3 added by the red-team fix),
  100% passing, run directly (`test_identity_authority_grant.exe`) and via `ctest`.
- `tests/compile_fail/identity_handle_no_direct_construction.cpp` and
  `tests/compile_fail/identity_authority_no_copy.cpp`: both confirmed failing to compile as intended,
  via `try_compile()` at CMake configure time (`ADR-102 §3 C1 compile-fail proof: OK`), re-confirmed
  after the tenant-scoping fix landed.
- `python tools/naming_lint.py`: `027 naming-lint: OK` — all four new names
  (`IdentityAuthority`/`IdentityHandle`/`Grant<Payload>`/`AsyncQuota<Kind>`) recognized via real
  `027-Vocabulary-and-Naming.md` §4 rows, no suppression comments used.
- Full project rebuild: 370/370 targets, zero errors.
- Full `ctest` suite: 280/280 passing (including the new `test_identity_authority_grant`), run twice
  (before and after the red-team fix), zero regressions from this change either time.

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| C1 — non-copyable/move-only compiler-enforced | **CORRECT** | Two real `try_compile()` negative probes, both confirmed failing to compile for the claimed reason. |
| C2 — minting fails closed against an unminted subject | **CORRECT, structurally guaranteed, not live-testable** | Independently re-traced: no construction path for an IdentityHandle exists outside IdentityAuthority's own three minting methods, each of which registers the handle atomically under the same lock — the negative case is unreachable via the public API within one process (disclosed in the test itself, independently re-verified by the red-team round, not merely asserted). |
| C3 — `try_consume()` identity-scoped | **CORRECT** | Live test: an unrelated `IdentityHandle` is refused (`async_quota.unauthorized_spender`), the quota's own owner and a real child share both succeed. |
| C4 — `Principal` collision resolved by distinct name + typed bridge | **CORRECT** | No `Principal` type in either ported file (grep-confirmed independently); `adopt()` takes `agentengine::Principal const&`. |
| C5 — naming lint passes for the right reason | **CORRECT** | Real `027` §4 rows added for all four names; `naming_lint.py`'s actual scan logic (not docstring) independently confirmed to cover the relevant directories; zero suppression comments used. |
| (new) Cross-tenant identity isolation in `adopt()` | **CORRECT, after a real, found-and-fixed defect** | Red-team round found `adopted_` keyed by `id` alone, ignoring `tenant_id` — a real 018 §6 violation, not yet exploitable (no production caller existed). Fixed: two-level `(tenant_id, id)` keying, tenant-scoped ancestry lookup, proven live with a new test case. |

## 7. The decision

**Design B is accepted for Phase 1**: `IdentityAuthority`, `IdentityHandle` (the renamed
authority-subject type — chosen over `AuthoritySubject`/`GrantSubject` for brevity and consistency
with this codebase's existing `*Handle` naming convention, e.g. `BranchHandle`, `SandboxHandle`), and
`Grant<Payload>` land in `include/agentengine/trust/identity_authority.hpp`, declared directly in
`namespace agentengine` (matching `CapabilitySet`'s own real placement — `trust/capability.hpp`
declares `class CapabilitySet` in bare `agentengine`, not `agentengine::trust`; the directory is the
module boundary, not a C++ namespace). `AsyncQuota<Kind>` lands in `include/agentengine/rt/
async_quota.hpp`, `namespace agentengine::rt` (matching `SpawnCostBudget`'s real placement).

**Binds, once Phase 1 lands:**
- `IdentityAuthority::adopt(agentengine::Principal const&)` as the one, sole bridge between the real
  per-request `Principal` and this design's own durable `IdentityHandle` — no second bridge, no
  reverse conversion (`IdentityHandle` → `Principal`) is built, since nothing in this phase needs one.
- The real `agentengine::error`/`agentengine::result<T>` vocabulary (`core/error.hpp`) replaces the
  prove-phase `probe::error`/`probe::result<T>` shim throughout the ported files — this is not a
  cosmetic rename; `agentengine::error` carries a `failure_class` the prove-phase shim did not, and
  every constructed `error{}` in the ported code must pick a real class (matching `SpawnCostBudget`'s
  own `failure_class::resource` for quota exhaustion, `capability.hpp`'s own `failure_class::policy`
  for authorization refusals).
- `027-Vocabulary-and-Naming.md` §4 gains real rows for all four new names, not suppression comments.

**Explicitly out of scope — named, not silently dropped, per the approved plan
(`~/.claude/plans/virtual-herding-cocoa.md`):**
- **Phase 2** — `Ledger<Store>`/`BranchHandle`/`Checkpoint` production port.
- **Phase 3** — `ExecutionSurface`/`SandboxRuntime`/`DockerExecutionSurface` production port.
- **Phase 4** — `MandatorySandboxProvider`/`RunCommandTool`, wired as a session's bare `HistoryProviderT`
  (explicitly NOT via `ComposedContextProvider<Ms...>`, which has zero real production consumers
  anywhere in this codebase today — confirmed by direct search of `tools/`, `src/`, and `examples/`),
  driven through the real 10-step `invoke_tool()` pipeline for the first time.
- **Phase 5** — `ContainerdExecutionSurface`/ADR-101 promotion (ADR-101 is itself Proposed, not
  Judged — stacking two unjudged designs in one landing is avoidable risk); Windows/Linux parity;
  `tools/cli_chat.cpp` wiring (closed, §30 onward); `ComposedContextProvider<Ms...>` becoming a real
  production consumer (closed, §41 onward).
- **`SandboxToolProvider` (ADR-096, uncopyable, `fork_from()`-becomes-compile-error) vs. this design's
  own `MandatorySandboxProvider` (copyable, fork-on-copy via `Ledger::branch_from()`) copy-semantics
  divergence is a decided, disclosed COEXISTENCE, not reconciled by this ADR.** They manage genuinely
  different resource shapes — a live OS process (`SessionShellSandbox`) vs. a cheap, content-addressed
  ledger branch — and divergent philosophies may be the *correct* per-resource answer, not an
  inconsistency. One real gap worth naming: `SandboxToolProvider`'s own "fork_from becomes a compile
  error" claim has never actually been triggered anywhere in this codebase (`tests/test_sandbox_tool_
  provider.cpp` only asserts it in a comment) — a real, independent, must-fail-to-compile negative
  probe proving it is named as future work (tracked with Phase 5, not blocking here).
- **`SandboxToolProvider`/`CodeActRunnerBinding`/`SandboxBackendRegistry` remain untouched and
  unreconciled by Phase 1 specifically** — narrowing ADR-099 §7's own residual by exactly this much
  (Phase 1 touches none of the four), not claiming to close it.

## 8. Residual risks

- **Sustained multi-process durability races are not addressed.** `persist_high_water_mark()`/
  `load_durable_state()` (`identity_authority.hpp`) defend only the very-first-run concurrent-start
  case; two long-running processes sharing the same `durable_dir` can still independently advance
  `next_id_` and issue colliding ids once either has moved past what the other read at startup.
  Inherited unchanged from the prove-phase original (same code, same partial defense) — not
  introduced by this port, but now sitting in code this ADR calls production, so named explicitly
  rather than left implicit. Not exploitable today (no real caller passes a `durable_dir` yet).
- **Single-process-per-authority assumption.** `IdentityAuthority::bootstrap()`'s Meyer's-singleton
  is defined inline in the header — if `agentengine` core code is ever linked into more than one
  shared library/DLL in the same process, each module gets its own singleton instance, making an
  "unknown to this instance" `IdentityHandle` reachable across that module boundary (defeating C2's
  own structural guarantee, which assumes exactly one authority instance per process). Not live
  today — the current build has no such split (`SHARED` targets in this tree are only vendored
  `wasmtime`/`mbedtls`) — named as a residual for whoever eventually builds a plugin-host-shaped
  consumer of this code (009's WASM component host is the one real candidate for introducing exactly
  this shape).
- **This design has never been Judged, and neither has ADR-099 itself.** Whoever judges this ADR
  should weigh the pattern in §4: even a narrow, two-file, ~450-line Phase 1 port had one real,
  security-relevant defect (the tenant-collision gap) that survived this session's own initial
  implementation and was caught only by a genuinely independent red-team round — matching this whole
  design line's own repeatedly-observed track record (ADR-096 §8, ADR-100 §2, ADR-101 §4/§8) of
  self-directed reasoning getting exactly this class of thing wrong. Phases 2-5 should budget for the
  same, not assume Phase 1's clean second pass means the pattern has stopped.
- **`adopt()`'s bridge still ignores `Principal::kind` and `Principal::delegation_depth`** — only
  `id`/`tenant_id`/`on_behalf_of` are consulted. Nothing in Phase 1 needs them; whether a future phase
  should thread `kind` through (e.g. to distinguish a human vs. an agent-derived principal at the
  `IdentityHandle` level) is a real, open question, not decided here.
- **No second `AsyncQuota<Kind>` instantiation was exercised beyond the test's own synthetic
  `BranchCost`-shaped tag** — the design's real target kinds (`StorageBytes`/`RunCost`/`ResetCost`/a
  `SpawnDepth`-shaped kind) are named in ADR-099 but not instantiated against the ported type in this
  phase; deferred to whichever later phase actually needs them (Phase 3's `SandboxRuntime` is the
  first real consumer). **Update, Phase 2 (§9 below): `StorageBytes`/`BranchCost` are now real,
  instantiated, exercised Kind tags** (`core/ledger.hpp`), closing this specific residual for those
  two; `RunCost`/`ResetCost`/a `SpawnDepth`-shaped kind remain Phase 3+ work.

---

# Phase 2 — `Ledger<Store>`/`BranchHandle<Store>`/`Checkpoint`/`merge_trees()`

## 9. The question (Phase 2)

**Stated so it has a wrong answer:** Phase 1 ported the identity/quota primitives in isolation, with
no session/execution-surface wiring and no content-addressed history mechanism. Does `Ledger<Store>`
(ADR-099's own checkpoint/branch/merge system) port onto those Phase 1 primitives, and onto the REAL,
already-shipped `agentengine::WorktreeObjectStore`/`InMemoryWorktreeObjectStore`
(`core/worktree_types.hpp`), as a faithful, behavior-preserving mechanical translation — or does
promoting it to production surface a real, previously-unexamined design gap the prove-phase original's
own standalone testing never actually exercised?

## 10. Design and real finding

**Design**: a direct, line-by-line port of `docs/planning/proofs/worktree_io/{worktree_ledger.hpp,
merge_trees.hpp}` into `include/agentengine/core/ledger.hpp` — `probe::Principal` →
`agentengine::IdentityHandle` throughout (Phase 1's own naming decision), the 2-field `probe::error`
shim → the real 4-field `agentengine::error` (a `failure_class` chosen per error site: `policy` for
authorization refusals, `resource` for the ACL-cap bound, `contract` for caller-side violations,
`fatal` for object-store failures), `agentengine::rt::task<T>` fully qualified (this file lives in
bare `agentengine`, not `agentengine::rt`), and `merge_trees()`/`MergeResult`/`MergeConflict` promoted
alongside `Ledger` in the same file (its only real caller). `StorageBytes`/`BranchCost` (Kind tags for
`AsyncQuota<Kind>`) move here too, as real, instantiated types, since `Ledger` is their first real
consumer. Scope: only `Store = InMemoryWorktreeObjectStore` (already real, already shipped) is
exercised — a durable object-store conformer is explicitly NOT ported in this phase.

**The real finding, not anticipated at design time**: `tests/test_ledger.cpp`'s own first working
version used TWO independent `IdentityAuthority::mint_root()` calls to model a "parent session" and
a "branch child" — and several checks failed for a specific, structural reason, not a port defect:
`authorized_for()`'s ACL check is keyed on `IdentityAuthority`-tracked IDENTITY ancestry
(`derive_child()`/`is_ancestor_of()`), a COMPLETELY SEPARATE relationship from `Ledger`'s own BRANCH
ancestry (`branch_from()`). A branch-child that is not also a real identity-descendant (via
`authority.derive_child(owner, ...)`) inherits only the coarser tree-digest-level grant
`branch_from()` itself inserts directly, never blob-level access to individual blobs nested in that
tree. Correcting the test to use `authority.derive_child(owner, ...)` for the branch-child identity
(matching how `mandatory_sandbox_provider.hpp`'s own real `spawn_child_branch()`/`IdentityAuthority::
adopt()` usage is intended to work) surfaced the real MUST-FIX below — the first genuine exercise of
a real, two-identity orchestrator/sub-agent merge flow against this `Ledger` anywhere in this
project's history, prove-phase included.

## 11. Falsifiable claims (Phase 2)

- **C6 (the port is a faithful, behavior-preserving translation).** Every claim the prove-phase
  original's own §26-§35 already proved (non-synchronization hazard fixed by serializing every
  `store_` access under `mutex_`; the ACL root cap fails closed and correctly no-ops for an
  already-present root; `mark_digest_shared()`'s escape hatch correctly exempts a digest from the cap
  permanently; every one of `merge()`'s 8 early-return paths correctly registers the child as a
  reclaimable orphan; the case-folding collision check correctly covers all pairs, not just adjacent
  ones; no `std::mutex` is ever held across a `co_await`) holds unchanged against the ported code.
  *Disproof: any of the above regresses in the ported file.* — **CORRECT**, independently re-verified
  by a fresh red-team pass against the real, ported code (not re-derived from the original).
- **C7 (a legitimate orchestrator/sub-agent merge-back flow does not lock the orchestrator out of its
  own branch).** After a real identity-descendant merges its work into a parent branch, the parent
  branch's own creator retains real read access (`get_tree_safe()`/`head_tree_digest()`) to that
  branch's new head. *Disproof: the parent branch's own creator is refused access to its own branch's
  post-merge head.* — **INITIALLY WRONG, found and fixed same day** (§12): the first version of this
  port (a faithful translation of the prove-phase original, which never actually tested this scenario)
  failed this claim outright — confirmed live with a standalone probe, not just reasoned about.
  Corrected by granting the parent branch's own creator a real ACL root on the merged tree digest,
  alongside the actual merge requester. **CORRECT after the fix**, re-verified live by a second,
  independent verification round including an empirical forced-revert test (temporarily disabling the
  fix reproduced exactly the two new checks failing, nothing else — confirming the checks are real
  positive controls, not incidental passes).

## 12. The red-team attack (Phase 2)

Two independent rounds, 2026-08-28, both against the real, ported, compiled, live-tested code:

1. **First round.** Verified C6 in full (see §11) — the mechanical translation itself is faithful,
   with three minor, non-blocking findings: (a) a silent error-code prefix rename
   (`"worktree_ledger.*"` → `"ledger.*"`) not listed in the file's own "real changes made during the
   port" enumeration — fixed by adding it to that list; (b) `reset_to()`'s pre-existing "no
   `authorized_for()` check of its own" residual (already named at the ADR-099 level, §42) was not
   carried inline into the ported file the way the sibling `mark_digest_shared()` residual is — fixed
   with an inline disclosure comment, no behavior change; (c) `merge()` is not `AsyncQuota`-gated
   (an I8 gap of the same shape ADR-099 §42 already fixed one level over for `SandboxRuntime::
   reset_to_turn()`) — disclosed inline as real, contained follow-on work, not fixed in this same pass
   (fixing it correctly requires the same lock-before-`co_await` restructure `commit()`/
   `branch_from()` already went through, across `merge()`'s 8 distinct early-return paths).
   **The load-bearing finding**: C7 above — found FALSE, empirically, and traced to a false comparison
   in the test's own comment claiming this flow "matches" the real task-branch tool track (A10,
   `docs/planning/proofs/task_branch_tool/`) — independently re-checked against A10's actual code and
   found A10 uses exactly ONE identity throughout (`spawn_child_branch(owner_,...)`, `merge_into(
   *main_, owner_)`), never a real `derive_child()`-distinct sub-identity, so A10 never actually
   exercises (or is protected from) this gap at all.
2. **Fix, same day**: `merge()` now also grants `parent_state.created_by_id` (the parent branch's own
   creator) a real ACL root on the merged tree digest, skipping the redundant grant when the merger
   and the branch's own creator are the same identity. The false A10 comparison in the test's own
   comment was corrected to state plainly that A10 sidesteps this scenario rather than answering it.
3. **Second, independent verification round.** Traced the fix's placement, its own failure-path
   orphan-registration discipline (matches every sibling path), and reasoned through the ACL-cap
   interaction concretely (the grant is a true no-op for an already-present root, checked before the
   cap check, so it costs at most one permanent slot per digest, never a multiplier across many
   merges) — **CORRECT, no new cap-exhaustion issue**. Empirically forced a revert (temporarily
   disabling the new grant) and re-ran the test: exactly the two new positive-control checks failed,
   nothing else — confirming they are real, not incidental. **Found one new, real, disclosed-not-fixed
   SHOULD-FIX**: the fix is TREE-DIGEST-LEVEL only (`tree_acl_`) — it does not extend to nested
   blob/subtree digests the child alone contributed (`blob_acl_` untouched for those), so the parent
   branch's own creator can list the merged tree's structure but cannot yet fetch the actual bytes of
   a blob the child alone wrote, without some other mechanism (`mark_digest_shared()`'s all-or-nothing
   escape hatch, or a real identity-design change). "The orchestrator resumes" is therefore only
   partially restored — structurally, not content-wise. Disclosed inline in `merge()`'s own comment
   and here, not fixed in this pass — the reported symptom (the literal access-denied lockout) is
   fully and correctly closed; the narrower content-level gap is real follow-on work.

## 13. Executed evidence (Phase 2)

- `include/agentengine/core/ledger.hpp`: compiled clean under MSVC 19.51, zero new warnings, linked
  against the real `agentengine::worktree_store` library (real SHA-256 via Windows CNG/BCrypt,
  `src/core/worktree_digest.cpp`).
- `tests/test_ledger.cpp`: 47 checks, 100% passing, covering real branch creation, commit, read-back,
  branch-from with genuine identity-descendant inheritance, a clean three-way merge, a real detected
  merge conflict with reclaimable-orphan recovery, `reset_to()`, drop-triggered abandon +
  `reap_pending_abandons()`, the ACL root cap (both the rejection and the no-op-on-already-present-
  root/already-shared-digest cases), `mark_digest_shared()`, the case-folding collision guard, and a
  real durability round-trip (Ledger destruction + reconstruction against the same `durable_dir`,
  confirming branch/ACL bookkeeping durability — content durability explicitly out of this phase's
  scope, not overclaimed).
- Full project rebuild: 372/372 targets (370 + `test_ledger` + one CMake-generated helper), zero
  errors, run twice (before and after the merge fix).
- Full `ctest` suite: 281/281 passing (280 + `test_ledger`), zero regressions, run twice.
- A standalone, throwaway adversarial probe (not part of the checked-in tree) independently confirmed
  both the original C7 failure (live `ledger.tree_access_denied` on the parent's own creator) and the
  fix's correctness, including a forced-revert test proving the new checks are real positive controls.

## 14. Per-claim verdicts (Phase 2)

| Claim | Verdict | Evidence |
|---|---|---|
| C6 — faithful, behavior-preserving translation | **CORRECT, after three minor disclosure fixes** | Independent red-team round traced every claim against the ported code; three non-blocking findings (error-code prefix, `reset_to()` disclosure, `merge()` quota-gap disclosure) fixed as documentation, not behavior. |
| C7 — orchestrator not locked out of its own branch post-merge | **CORRECT, after a real, found-and-fixed MUST-FIX** | Live probe confirmed the original failure; the fix confirmed correct, cap-safe, and non-widening by a second independent round, including an empirical forced-revert test. One real, disclosed SHOULD-FIX residual (tree-level, not blob-content-level). |

## 15. Residual risks (Phase 2)

- **The blob-content-level gap** (§12's second-round finding): the parent branch's own creator cannot
  yet read the actual bytes of a blob a merged-in descendant alone wrote, only list that it exists.
  Real follow-on work, not blocking Phase 2's own stated fix (the literal access-denied lockout on
  `get_tree_safe()`/`head_tree_digest()` is fully closed).
- **`merge()` remains ungated by any `AsyncQuota`** (§12, point (c)) — a real I8 gap, disclosed inline,
  not fixed this phase. Fixing it correctly needs the same lock-before-`co_await` restructure
  `commit()`/`branch_from()` already underwent, across `merge()`'s 8 early-return paths — real,
  contained follow-on work, not a same-pass mechanical addition.
- **Durable content storage is out of scope.** `Store` stays `InMemoryWorktreeObjectStore` even in
  this phase's own durability test — only `Ledger`'s own branch/ACL bookkeeping durability is real and
  tested; a durable object-store conformer (the prove-phase original's own `FileWorktreeObjectStore`)
  is Phase 3+ work.
- **This design has never been Judged.** The pattern named in Phase 1's own §8 residual repeated here:
  a faithful, ~700-line mechanical port still surfaced one real, previously-unexamined,
  security/usability-relevant structural gap (C7) that neither the prove-phase original's own testing
  nor this port's own first pass caught — only a test correctly modeling real identity ancestry
  (`derive_child()`, not a second `mint_root()`) surfaced it, and only an independent red-team round
  caught that the fix's own justifying comment was itself factually wrong about a real precedent
  (A10). Phases 3-5 should budget for the same standing cost, not assume two clean phases means the
  pattern has stopped.

## 16. The question (Phase 3)

**Stated so it has a wrong answer:** Phases 1-2 ported the identity/quota primitives and the
content-addressed `Ledger`, with no `run(command)`-shaped verb anywhere in production — nothing before
this phase let a session execute anything inside an isolated surface and have the result flow through
the real `Ledger` checkpoint chain. Does `ExecutionSurface`/`SandboxRuntime`/`DockerExecutionSurface`/
`RealIoFileSystem` port onto Phases 1-2's own real primitives as a faithful, behavior-preserving
mechanical translation — or does composing four previously-standalone prove-phase files together for
the first time inside production surface a new gap none of them, alone, could have shown?

## 17. Design

A direct port of `docs/planning/proofs/execution_surface/{execution_surface.hpp,
docker_execution_surface.hpp,sandbox_runtime.hpp}` and `docs/planning/proofs/worktree_io/
real_io_filesystem.hpp` into `include/agentengine/sandbox/`, four new files: `execution_surface.hpp`
(the `ExecutionSurface` concept, `SurfaceRunOutcome`), `docker_execution_surface.hpp`
(`DockerCliBackend`, a real `docker` CLI shell-out wrapper, and `DockerExecutionSurface`, the one real
conformer this phase builds), `real_io_filesystem.hpp` (`RealIoFileSystem`, real Win32 file I/O staging
against `Ledger`), `sandbox_runtime.hpp` (`SandboxRuntime`, `RunCost`/`ResetCost`, `SandboxRunOutcome`).
Same translation discipline as Phases 1-2: `probe::Principal` → `agentengine::IdentityHandle`,
`probe::Ledger<>`/`BranchHandle<>`/`Checkpoint` → the real, ported Phase 2 types, `probe::AsyncQuota<T>`
→ the real, ported Phase 1 `agentengine::rt::AsyncQuota<T>`, `probe::error{message,code}` → the real
`agentengine::error{failure_class,message,code}`. Two real renames, not cosmetic: `probe::ExecOutcome`
→ `SurfaceRunOutcome` (the real, shipped `agentengine::ExecOutcome`, `sandbox/sandbox.hpp`'s own
`SandboxBackend` outcome vocabulary, answers a genuinely different question and has no raw exit-code
field this concept's real callers depend on); `probe::RunOutcome` → `SandboxRunOutcome` (a real name
collision found during vocabulary registration against the existing, shipped `agentengine::a2a::
RunOutcome` — a different concept, an A2A task's outcome, not one sandboxed command's). Deliberately
NOT a `SandboxBackend` conformer and NOT wired to `NativeJailBackend`/`WasmBackend`/`KataBackend` —
carrying ADR-099 §7's own explicit project-owner scope boundary forward unchanged. Deliberately NOT
`ContainerdExecutionSurface` or ADR-101's own, separate, still-Proposed `TreeCapableSandboxBackend` —
both extra, avoidable risk in a first landing, per this plan's own scope decision to stay independent
of ADR-101.

## 18. Falsifiable claims (Phase 3)

- **C8 (the port is a faithful, behavior-preserving translation).** Every claim the prove-phase
  originals' own multi-round red-teaming already proved (RunCost consumed before the real command
  executes, never merely before commit; RunCost refunded on every "nothing happened" path including a
  surface-level rejection; the shell-injection defenses cover every real `_popen` call site; the
  Windows TOCTOU fix via `open_within_mount_root` is used correctly; `materialize()` validates every
  entry name before touching disk) holds unchanged against the ported, `agentengine`-namespaced code.
  *Disproof: any of the above regresses in the ported files.* — **CORRECT**, independently re-verified
  by a fresh red-team pass against the real, ported, compiled, live-Docker-tested code — including a
  live rebuild and test run, not just a reasoned-through diff.
- **C9 (SandboxRuntime's own locking discipline is internally consistent — every method that reads or
  mutates shared state takes `exclusivity_`).** *Disproof: a method that reads/mutates `branch_`/
  `io_fs_` without holding `exclusivity_` exists.* — **INITIALLY WRONG, found and fixed same day**
  (§19): `merge_into()`/`discard()` mutated `branch_` (via `Ledger::merge()`/`abandon()` taking their
  `child`/branch parameter by value, so `std::move(branch_)` at the call site is a synchronous mutation)
  without taking the lock every OTHER method that touches `branch_` already takes. This defect existed
  identically in the prove-phase original, surviving five prior red-team rounds on that file without
  being named — carried forward faithfully by the port (so C8 itself is not disproven), but newly
  caught here because this phase's own red-team round checked the class's locking discipline as a
  WHOLE, not method-by-method against the original. **CORRECT after the fix.**

## 19. The red-team attack (Phase 3)

One independent round, 2026-08-28, against the real, ported, compiled, live-Docker-tested code
(rebuild + `./tests/test_sandbox_runtime.exe` run against the real Docker daemon on this machine,
plus a `docker ps -a` before/after check to confirm container lifecycle behavior empirically):

1. **C8 verified in full** — zero silent behavior changes found from any of the `probe::` →
   `agentengine::` renames across all four files; every constructed `agentengine::error`'s
   `failure_class` is defensible and matches what `tests/test_sandbox_runtime.cpp` actually asserts on
   (`docker_cli_backend.unsafe_shell_argument`, `async_quota.exhausted`, `ledger.no_such_checkpoint`).
2. **The load-bearing finding (C9, SHOULD-FIX)**: `merge_into()` (then at what is now
   `sandbox_runtime.hpp:275-278` pre-fix) and `discard()` (then `:311-313` pre-fix) mutate `branch_`
   via `std::move(branch_)` — legal because `Ledger::merge()`/`Ledger::abandon()` both take their
   branch-handle parameter BY VALUE (`core/ledger.hpp:628,774`) — without first taking
   `exclusivity_`, unlike `run()`/`reset_to_turn()`/`spawn_child_branch()`, which all take it before
   touching `branch_`. Concrete scenario: once a future caller (Phase 4/5) holds a `SandboxRuntime`
   reachable from two concurrently-scheduled coroutines, one mid-`run()` past its own lock (e.g. inside
   step 7's `co_await ledger_->commit(branch_, ...)`, reading `branch_.name()`) and a second calling
   `std::move(*ptr).merge_into(parent, id)` on the SAME object could interleave `BranchHandle`'s plain,
   unsynchronized move constructor with `run()`'s in-flight read of the same member — the identical
   interleaving hazard class `run()`'s own comment already names `exclusivity_` as existing to prevent,
   just via a method pair the original five prove-phase red-team rounds never checked. Not reachable
   through any real caller this phase builds (a host-level API, no concurrent orchestrator wiring
   exists yet) — same "not yet reachable" caveat this class already states for its other reentrancy
   notes — but fixed rather than left as a silent gap ahead of Phase 4/5 wiring a real concurrent
   caller. **Fixed same day**: both methods now take `agentengine::rt::AsyncMutex::Guard guard =
   co_await exclusivity_->lock();` before touching `branch_`, matching every sibling method.
3. **A second finding, disclosed-but-inaccurate (not a defect in the port, a defect in the prove-phase
   original's own disclosure, carried forward unread)**: `docker_execution_surface.hpp`'s own top
   comment claimed the orphaned-container leak residual "requires an actual process crash (not a
   normal exit path)." The destructor discards `destroy()`'s result via `(void)` with no retry — so an
   entirely ORDINARY, non-crash destructor call whose `docker rm -f` transiently fails (daemon
   contention, a network hiccup — the same transient-failure class `reset()` itself already defends
   against, by NOT clearing `instance_` on a failed `destroy()` there, since a live caller can retry)
   leaks the container just as silently. Corroborated by a real, pre-existing orphaned container found
   on the development host during this same pass (an `alpine:latest` container running the exact
   `create()` command this class emits, with no crash known to have produced it). **Corrected same
   day**: the disclosure comment now states the real trigger condition accurately; the underlying leak
   itself is not fixed in this pass (real follow-on work — persisting instance ids somewhere
   reclaimable, mirroring `Ledger`'s own orphan-branch design).
4. No MUST-FIX found this phase — first phase of the three so far where the independent red-team round
   did not need a second verification round of its own (both findings were narrow, mechanically clear
   fixes, re-verified by rebuild + full test-suite re-run rather than a separate adversarial pass).

## 20. Executed evidence (Phase 3)

- `include/agentengine/sandbox/{execution_surface,docker_execution_surface,real_io_filesystem,
  sandbox_runtime}.hpp`: compiled clean under MSVC 19.51 on the first attempt (no iteration needed to
  get the port to compile at all), zero new warnings, linked against the real `agentengine::
  worktree_store` library.
- `tests/test_sandbox_runtime.cpp`: 10 checks (ported from `docs/planning/proofs/execution_surface/
  {probe_execution_surface.cpp,probe_sandbox_rollback.cpp}`'s own coverage), 100% passing against a
  REAL, running Docker daemon — real multi-turn persistence through the actual Ledger checkpoint chain
  across genuinely fresh containers, a non-zero exit code as a normal result, RunCost consumed before
  execution and refunded on both a quota-exhaustion rejection and a shell-guard rejection (with real
  Docker container counts confirmed unchanged), `reset_to_turn()` as a new forward checkpoint (never an
  in-place rewrite) whose effect a subsequent `run()` genuinely observes, and ResetCost exhaustion
  leaving the branch's real head digest completely unchanged.
- Full project rebuild: 373/373 targets, zero errors, run twice (before and after the two §19 fixes).
- Full `ctest` suite: 282/282 passing, zero regressions, run three times total across this phase (once
  before the fixes, once immediately after, once more after `test_rt_workflow_supervisor_merge_on_join`
  flaked under `-j4` — confirmed passing in isolation and confirmed the full suite passes clean on the
  next full run).
- Confirmed via `docker ps -a` before/after both test runs that no container was left running by this
  phase's own test — the real `--rm`/`destroy()` cleanup path works correctly on the tested path.
- `python tools/naming_lint.py`: as a byproduct of registering Phase 3's own new names, found and fixed
  a real, previously-unregistered latent gap FROM PHASE 2 (`MergeConflict`/`StorageBytes` were never
  actually recognized by the lint tool, because its table-row regex only captures the FIRST backtick
  name in a combined "`X` / `Y`" row — `027-Vocabulary-and-Naming.md`'s own Phase 2 rows had combined
  two names per row). Fixed by splitting every combined row (Phase 2's `MergeResult`/`MergeConflict`
  and `BranchCost`/`StorageBytes`, and this phase's own `RunCost`/`ResetCost`) into one row per name;
  confirmed the lint now reports zero unregistered names.

## 21. Per-claim verdicts (Phase 3)

| Claim | Verdict | Evidence |
|---|---|---|
| C8 — faithful, behavior-preserving translation | **CORRECT** | Independent red-team round traced every claim against the ported, compiled, live-Docker-tested code; zero silent behavior changes found. |
| C9 — `SandboxRuntime`'s own locking discipline is internally consistent | **CORRECT, after a real, found-and-fixed SHOULD-FIX** | `merge_into()`/`discard()` were missing the `exclusivity_` lock every sibling method takes; fixed, rebuilt, full suite re-verified green. |

## 22. Residual risks (Phase 3)

- **The `merge_into()`/`discard()` concurrency gap was fixed, but nothing in this codebase yet drives
  `SandboxRuntime` from more than one concurrent coroutine on the same instance** — the fix is real and
  correct, but genuinely exercising it (two coroutines racing `run()` against `merge_into()` on the
  same object) needs a real concurrent caller this phase does not build. Real follow-on verification
  work for whichever phase first wires concurrent access.
- **The Docker container leak disclosure is now accurate, but the leak itself is still unfixed** — a
  transient `docker rm -f` failure inside `DockerExecutionSurface`'s destructor (not only a process
  crash) silently orphans a running container with no id persisted anywhere and no reclaim mechanism
  analogous to `Ledger`'s own `orphaned_branches()`. Real follow-on work.
- **Every residual already named by Phases 1-2 remains unchanged and unaddressed by this phase**: the
  blob-content-level ACL gap post-merge, `Ledger::merge()`'s missing `AsyncQuota` gate, durable content
  storage out of scope, and this design overall still awaiting `Judged` sign-off.
- **The pattern named in Phases 1-2's own residuals held a third time, in a new shape**: this phase's
  own red-team round found real issues neither the prove-phase original's five prior rounds nor this
  port's own first pass caught (the class-wide locking-discipline gap) AND found that a PRIOR phase's
  own disclosure text (Phase 2's combined vocabulary rows; the prove-phase original's leak-trigger
  wording) was itself inaccurate, not just incomplete. Phase 4-5 should budget for the same standing
  cost — a mechanically faithful port and passing tests are not evidence that an independent adversarial
  pass will find nothing, and disclosure text itself needs the same scrutiny as behavior.

## 23. The question (Phase 4)

**Stated so it has a wrong answer:** Phase 3 gave production a real `run(command)` verb; nothing before
this phase composed it into a real session's actual tool-dispatch surface. Does `MandatorySandboxProvider
<Surface>`/`RunCommandTool` port onto the real `agentengine::ContextProvider`/`Tool<>`/`invoke_tool()`
machinery as a faithful, behavior-preserving mechanical translation, driven for the first time through the
real, unmodified `AgentSession::start_run()` → `invoke_tool()` 10-step pipeline (every prior proof in this
design's history, prove-phase included, only ever drove `MandatorySandboxProvider` through direct
`history_provider()` accessor calls, never through a real tool-calling round) — or does composing a
coroutine-heavy identity/quota primitive into a REAL session for the first time surface a class of defect
none of the isolated, standalone proofs could have shown?

## 24. Design

A direct port of `docs/planning/proofs/mandatory_sandbox/mandatory_sandbox_provider.hpp` into
`include/agentengine/sandbox/mandatory_sandbox_provider.hpp` — `MandatorySandboxProvider<Surface>` (a
real `ContextProvider` conformer: default-constructible into a real "no sandbox bound yet" state,
required for `AgentSession::clear_in_process_state()`'s fixed `HistoryProviderT{}` statement to
compile; `bind_sandbox()` the real, host-only, config-time binding call; copy-assignment a fully
self-contained `SandboxRuntime::spawn_child_branch()` call, the mechanism `AgentSession::fork_from()`'s
own plain copy-assign statement relies on for real session forking) and `RunCommandTool`/
`RunCommandArgs`/`RunCommandReply` (the one `Tool<>` it contributes via `make_tool_descriptor_with_
invoke()`, deliberately with NO static `Capabilities<...>` ceiling — it authorizes against this
design's own `Grant<T>`/`IdentityAuthority`/`AsyncQuota<T>` model, which has no `CapabilitySet`-shaped
capability notion to declare a ceiling against, unlike the real, shipped `RunShellTool`, ADR-096). Same
translation discipline as Phases 1-3: `probe::Principal` → `agentengine::IdentityHandle`, the
two-string `adopt(id, on_behalf_of)` call → the real, typed `adopt(agentengine::Principal const&)`
(Phase 1), `probe::error{message,code}` → `agentengine::error{failure_class,message,code}` with a real
class picked per site (`contract` for the two "sandbox never bound" preconditions, `resource` for
quota exhaustion), and a REAL FIDELITY FIX (not a rename): the prove-phase original's tool closure
re-wrapped every failed `SandboxRuntime::run()` outcome as a fixed `failure_class::policy` — an
artifact of the prove-phase's own 2-field error having nothing real to preserve — this port passes
`outcome.error()` through unchanged, so a caller sees the REAL classification of what actually failed.

## 25. Falsifiable claims (Phase 4)

- **C10 (the port is a faithful, behavior-preserving translation of the ContextProvider/Tool<>
  composition itself).** *Disproof: `MandatorySandboxProvider` fails to satisfy `ContextProvider`, or
  `RunCommandTool`'s contract with `invoke_tool()`'s real 10 steps diverges from what the prove-phase
  original established against `FakeAgentSession`.* — **CORRECT**, confirmed by `static_assert(
  agentengine::ContextProvider<Provider>)` and by `tests/test_mandatory_sandbox_provider.cpp`'s checks
  [1]/[2]/[4]/[5]/[6] (unbound-zero-tools, direct bind+invoke, real `fork_from()` isolation, real
  `clear_in_process_state()`, real `would_fork_succeed()` quota reflection) — all passing on the first
  real build, mirroring the prove-phase original's own already-proven claims against the REAL
  `AgentSession`, not merely `FakeAgentSession`.
- **C11 (a `run_command` tool call driven through the REAL, unmodified `invoke_tool()` 10-step
  pipeline, via `session.start_run()`, actually executes a real command in a real container and commits
  a real Ledger checkpoint).** *Disproof: the pipeline-driven call never reaches `RunCommandTool`'s
  closure, or reaches it but the result is not observable through `session.history()`.* — **CORRECT**,
  proved by check [3] (`tests/test_mandatory_sandbox_provider.cpp`) — the first time in this entire
  design's history (prove-phase included) this exact composition has been driven end to end this way.
- **C12 (SandboxRuntime's own locking discipline, driven through this new composition's own call
  pattern, is internally sound under real cross-thread contention this composition's own usage pattern
  makes reachable).** *Disproof: a real, reproducible corruption of `AsyncMutex`'s mutual-exclusion
  guarantee under a contention shape this composition's own real usage pattern (a shared `AsyncQuota`
  across sibling sessions) can trigger.* — **INITIALLY WRONG, found and fixed same day, EMPIRICALLY
  PROVED both ways** (§26): the port's first version used a naive drive loop, reasoned safe under an
  I1-based argument that turned out to be TRUE but insufficient — a targeted repro against the real,
  unmodified `AsyncMutex`/`task<T>` reproduced the corruption 5/5 runs. **CORRECT after the fix**
  (porting the prove-phase original's own ASan-hardened `block_on<T>()` for real), re-verified by a
  SECOND, independent positive-control test (`tests/test_rt_block_on.cpp`) proving the fix holds under
  the identical contention shape, 5/5 rounds, zero corruption.

## 26. The red-team attack (Phase 4)

One independent round, 2026-08-28, against the real, ported, compiled, live-Docker-tested code —
almost entirely focused on the one deliberate design decision this port's own first version made:
skipping the prove-phase original's ASan-hardened `block_on()` in favor of a naive local drive loop,
under the claim "I1 (one session, one executor) plus `invoke_tool()`'s sequential dispatch mean nothing
here ever drives two concurrent invocations into the SAME `MandatorySandboxProvider`/`SandboxRuntime`
instance from two different threads."

1. **C10/C11 verified in full** — the `ContextProvider`/`Tool<>` composition itself, and the new
   `invoke_tool()`-pipeline-driven claim, both faithful and correct. `RunCommandTool`'s zero
   `Capabilities<>` ceiling confirmed harmless by direct comparison: the real, shipped `RunShellTool`
   also declares no `Approval<...>`, so BOTH tools skip step 5's human-approval gate by the library's
   own default — the empty ceiling is not a Phase-4-introduced gap, `RunCommandTool`'s real
   authorization genuinely is the `is_bound()` + quota model, as disclosed. The Phase 4 change to
   error-fidelity (passing `outcome.error()` through unchanged) confirmed to introduce no new I3
   exposure — `make_error_result()` (`core/tool_pipeline.hpp`) has always surfaced `error.message` into
   model-visible tool-result content for every tool, pre-existing and unrelated to this port; only the
   `failure_class` TAG changed, never the text reaching the model.
2. **The load-bearing finding (C12, MUST-FIX, empirically confirmed, not merely reasoned about)**: the
   claim above is TRUE but insufficient. `bind_sandbox()` stores `AsyncQuota<RunCost>`/`AsyncQuota
   <BranchCost>`/`AsyncQuota<StorageBytes>` as raw pointers, and those quotas are LEGITIMATELY SHARED
   across multiple independent `SandboxRuntime` instances — an ordinary "one budget for a whole family
   of sibling sessions" pattern this phase's own test itself demonstrates (one quota triple bound
   across six separate sessions). If two sibling sessions' round loops ever run on genuinely different
   OS threads concurrently (the entire reason `AgentSession::session_mutex_` exists per-session at
   all), their two `RunCommandTool` closures' calls into the SAME shared `AsyncQuota` genuinely CONTEND
   on that quota's own internal `AsyncMutex` — no two `MandatorySandboxProvider`/`SandboxRuntime`
   instances need to be touched concurrently for this to fire, only the shared quota. The red-team
   built a minimal, targeted repro against the real, unmodified `rt/async_mutex.hpp`/`rt/task.hpp`,
   reproducing the exact naive-loop pattern under genuine two-thread contention: **5/5 runs** showed
   `AsyncMutex`'s mutual-exclusion guarantee completely defeated — the naive loop's second `resume()`
   call on an already-genuinely-suspended awaiter does not wait, it directly runs `await_resume()`,
   handing back a `Guard` as if the lock were acquired when it is not, and leaves a stale handle in the
   mutex's own waiter queue that a later, real `unlock()` resumes against an ALREADY-DESTROYED
   coroutine frame — a genuine, reproducible use-after-free, the identical hazard class the prove-phase
   original's own `block_on()` (`docs/planning/proofs/common/block_on.hpp`) was built to prevent.
3. **Fix, same day**: the prove-phase original's ASan-hardened `block_on<T>()` mechanism ported for
   real, into a NEW shared production primitive, `include/agentengine/rt/block_on.hpp` — a dedicated
   coroutine driver whose `final_suspend()` performs the cross-thread completion signal as literally
   the last action on its own frame, strictly after every local has already been destroyed, closing the
   exact "second resume() on an already-suspended awaiter" hazard the naive loop hit.
   `mandatory_sandbox_provider.hpp`'s two call sites (the tool closure, the copy-assignment) both now
   use `agentengine::rt::block_on()` instead of the naive loop.
4. **Second, independent verification**: a dedicated positive-control test, `tests/test_rt_block_on.cpp`,
   reproduces the IDENTICAL two-thread contention shape the red-team's own repro used, this time
   against `block_on()` — 5 contention rounds, `max_concurrent_holders` checked never to exceed 1 in
   any round, plus a positive control confirming an uncontended call still returns the correct value.
   All pass. This closes the loop the same way Phase 2/3's own second-round verifications did: the
   fix is not just "plausible," it demonstrably restores the exact property the red-team's own repro
   proved was broken.
5. **A second, related, disclosed-not-fixed finding (SHOULD-FIX)**: `AgentSession::fork_from()`
   (`rt/agent_session.hpp`) is deliberately NOT guarded by `session_mutex_` (unlike `start_run()`/
   `resolve_interaction()`), so nothing structurally prevents a future caller invoking it concurrently
   with an in-flight `run()` on the SAME source session from a different thread — `MandatorySandbox
   Provider`'s own copy-assignment would then contend for `SandboxRuntime::exclusivity_` itself, the
   identical hazard class `block_on()` now protects against for the quota case, but this specific
   vector was not exercised by the red-team's own repro and is not fixed here. Not live today —
   `fork_from()` has no real production caller anywhere in this codebase (confirmed by search;
   `agent.spawn`, the only real spawn path, does not use it) — but real follow-on work before any
   future caller wires `fork_from()` up to run concurrently with an in-flight tool call, named
   explicitly rather than silently repeated (this exact vector was already named, differently, by
   Phase 3's own §19 `merge_into()`/`discard()` finding — the SAME structural gap, `fork_from()`'s own
   lack of session-level serialization, keeps resurfacing through different composition layers).
6. No further findings — the pass concluded that beyond the `block_on()` gap and its own related
   `fork_from()` residual, the port is a faithful translation with no other new defect found after
   genuinely trying (the disclosed "unbound owns no branch" gap re-checked accurate, not worse than
   stated; the zero-`Capabilities<>` design re-confirmed harmless per point 1 above).

## 27. Executed evidence (Phase 4)

- `include/agentengine/sandbox/mandatory_sandbox_provider.hpp`: compiled clean under MSVC 19.51 on the
  first attempt, zero new warnings, linked against the real `agentengine::worktree_store` library.
- `include/agentengine/rt/block_on.hpp`: a NEW shared production primitive (not scoped to this one
  file, unlike every other "drive a task<T>" helper in this codebase, which are all local/duplicated
  per file) — compiled clean, zero new warnings.
- `tests/test_mandatory_sandbox_provider.cpp`: 6 check groups (ported from and condensed relative to
  `docs/planning/proofs/mandatory_sandbox/probe_mandatory_sandbox_real_agent_session.cpp`'s own 10),
  100% passing against a REAL, running Docker daemon — unbound-zero-tools, direct bind+invoke real
  execution, THE headline new claim (a `run_command` call driven through `session.start_run()` →
  the real `invoke_tool()` pipeline, confirmed via `session.history()`'s own `role::tool` message and
  an independent Ledger re-read), real `fork_from()` sibling isolation, real
  `clear_in_process_state()`, and real `would_fork_succeed()` quota reflection. One real
  test-authoring bug found and fixed during bring-up (disclosed for the record, not a defect in the
  port itself): the pipeline-driven check's session was initially `initialize()`d with an unrelated
  `Principal`, not the one the quotas/branch were minted for — since the real pipeline derives
  `ctx.principal` from the session's own `initialize()` call (unlike the direct-accessor checks, which
  bypass that), `IdentityAuthority::adopt()` correctly minted an unrelated identity and `AsyncQuota::
  try_consume()` correctly refused it (`async_quota.unauthorized_spender`) — a real proof the identity
  bridge works correctly, exposed by a test bug, not a port bug.
- `tests/test_rt_block_on.cpp`: 3 checks (5 contention rounds + 1 uncontended positive control), 100%
  passing, proving `block_on()` preserves `AsyncMutex`'s mutual exclusion under the identical
  contention shape the red-team's own repro used to break the naive loop.
- Docker container hygiene confirmed via `docker ps -a` before/after every real-Docker test run in this
  phase: zero leftover containers.
- `python tools/naming_lint.py`: registering Phase 4's own new names surfaced and fixed a SECOND
  instance of the SAME combined-table-row gap Phase 3's own evidence already found and fixed for Phase
  2's rows (`027-Vocabulary-and-Naming.md`'s table-row regex only recognizes the FIRST backtick name
  per row) — this phase's own first `RunCommandArgs`/`RunCommandReply` row, and its own first
  `block_on<T>()` row (which additionally used a fully-qualified `agentengine::rt::block_on<T>()` as
  the leading token, which the same regex captures as the nonsense name "agentengine") both needed
  correcting. A THIRD, separate finding as a byproduct: the file's own top-of-file "Scope: include/
  agentengine/{core,trust,sandbox,plugin,workflow}" comment is inaccurate/stale — the lint's real
  scope is namespace-based (`_in_scope_namespace()`), not directory-based, and genuinely covers
  `agentengine::rt` too (confirmed the hard way: `BlockOnState` in `rt/block_on.hpp` WAS flagged) —
  not fixed in this pass (a documentation-only correction to `tools/naming_lint.py`'s own comment,
  real but out of this ADR's own scope), named here so a future reader does not repeat the same
  mistaken assumption this port's own comments briefly stated before self-correcting.
- Full project rebuild: 376/376 targets, zero errors, run three times across this phase (before the
  `block_on()` fix, immediately after, and again after `test_rt_block_on.cpp` was added).
- Full `ctest` suite: 284/284 passing, zero regressions, run three times total across this phase.

## 28. Per-claim verdicts (Phase 4)

| Claim | Verdict | Evidence |
|---|---|---|
| C10 — faithful ContextProvider/Tool<> composition | **CORRECT** | `static_assert`, checks [1]/[2]/[4]/[5]/[6] all passing against the real, live-Docker-tested composition. |
| C11 — a real tool call driven through the real invoke_tool() pipeline | **CORRECT** | Check [3] — the first time in this design's entire history, prove-phase included. |
| C12 — SandboxRuntime's locking discipline is sound under this composition's real contention pattern | **CORRECT, after a real, found-and-EMPIRICALLY-fixed MUST-FIX** | A targeted repro proved the naive drive loop broken (5/5 corruption); `block_on()` ported for real; a second, independent positive-control test proved the fix holds (5/5 clean). One related SHOULD-FIX (`fork_from()`'s own lack of session-level serialization) disclosed, not fixed — not live today, no real caller. |

## 29. Residual risks (Phase 4)

- **`AgentSession::fork_from()`'s own lack of session-level serialization** (§26 finding 5) — real,
  structurally reachable once a future caller wires `fork_from()` concurrently with an in-flight tool
  call on the source session, not live today. The SAME structural gap Phase 3's own `merge_into()`/
  `discard()` finding (§19) surfaced at the `SandboxRuntime` layer directly — this is its session-level
  echo, not a new independent defect, worth closing once, not twice, when a real caller appears.
- **Every residual already named by Phases 1-3 remains unchanged and unaddressed by this phase**: the
  blob-content-level ACL gap post-merge, `Ledger::merge()`'s missing `AsyncQuota` gate, the Docker
  container leak on a transient `destroy()` failure, durable content storage out of scope, and this
  design overall still awaiting `Judged` sign-off.
- **`block_on<T>()` is a genuinely NEW shared production primitive, not merely a ported file** — unlike
  every other phase's port, this one required inventing (by porting, not designing fresh) a piece of
  infrastructure with codebase-wide applicability beyond this one composition. It has exactly one real
  consumer today (`mandatory_sandbox_provider.hpp`'s two call sites); a future caller reaching for it
  should re-read this file's own top comment for the real hazard class it exists to prevent before
  assuming any naive drive loop elsewhere in this codebase is safe by analogy.
- **The pattern named in Phases 1-3's own residuals held a fourth time, in its most significant shape
  yet**: this phase's own red-team round did not just find a documentation gap or a narrow locking
  omission — it found, and empirically DISPROVED, this port's own central, deliberately-reasoned safety
  argument for skipping a piece of already-proven-necessary prove-phase machinery. The argument was not
  careless (I1 is real, `invoke_tool()`'s sequential dispatch is real) — it was INCOMPLETE, missing that
  a shared resource (the quota) can carry contention across instances the argument only considered in
  isolation. Every phase's own evidence should keep treating "the mechanically faithful parts test
  clean" as no evidence at all about whether a genuinely NEW composition (not just a genuinely faithful
  translation) introduces a defect none of the isolated pieces could show on their own — composition is
  where this whole four-phase effort's real, novel risk has consistently lived.

## 30. The question (Phase 5)

**Stated so it has a wrong answer:** Phase 4 proved `run_command` works through the real `invoke_tool()`
pipeline against a purpose-built test session; nothing before this phase composed it into an actual
production HOST binary a real user runs, alongside the host's own already-shipped tools (`execute_code`,
`mount_skill`) and a real model backend. Does wiring `MandatorySandboxProvider` into `tools/cli_chat.cpp`
— the first genuinely user-reachable composition of this whole four-phase effort — work end to end
against a real model, or does the first real host integration surface a class of defect no isolated test
could show?

## 31. Design

`ToolDeclaringHistoryProvider` (`tools/cli_chat.cpp`) gains a `MandatorySandboxProvider<
DockerExecutionSurface> run_command_provider_` member and a mutable `run_command_provider()` accessor
(mirroring `AgentSession::history_provider()`'s own real pattern one layer up). `on_context()` merges
`run_command_provider_.on_context()`'s tool contribution into its own, deliberately OUTSIDE
`scope_tools_to_mounted_skills()`'s own gating — `run_command` is a session-level sandbox capability,
not a skill-unlocked one, so it must be unconditionally present once bound, the same way
`MandatorySandboxProvider::on_context()` itself already answers "is a sandbox bound at all" internally.
`run_interactive()` mints a real `Ledger<>`/`IdentityAuthority`-adopted owner identity/three
`AsyncQuota<T>`s/a root branch, all in-memory only (matching every prior phase's own disclosed "durable
content storage is out of scope" boundary), and calls `bind_sandbox()` once, before the interactive loop
starts — declared BEFORE `CliSession<Inner> actor` specifically, a real lifetime-ordering requirement
(§32 finding 1), not a stylistic choice.

## 32. The red-team attack (Phase 5)

Two rounds against the real, wired, compiled code — the first is this session's own bring-up (a shell-
wrapper-masked build failure that led directly to a real finding), the second an independent adversarial
pass.

1. **Bring-up finding, MUST-FIX, found and fixed same day**: `cmake --build . --target
   agentengine_cli_chat` initially reported build success only because the invoking shell wrapper's own
   exit code (from a trailing `tail`, not from `ninja` itself) was checked instead of `ninja`'s real one
   — a real process-discipline lesson, not a code defect, but one that let a genuine compile FAILURE go
   unnoticed for one round. The real failure it masked: `include/agentengine/core/ledger.hpp`'s own
   `MergeConflict`/`MergeResult` (Phase 2) silently collided, byte-for-byte on the fully-qualified name,
   with an unrelated, ALREADY-SHIPPED production type in `core/worktree_merge.hpp` (025 §4's own
   `SubWorktree`/`Ref`/`AppendLogStore`-based branch-merge mechanism — a completely different system).
   No build across Phases 2, 3, or 4 — including every full project rebuild and every independent
   red-team round on this exact file — had ever `#include`d both headers in the SAME translation unit,
   so the redefinition compile error stayed silently latent the entire time; Phase 5's `cli_chat.cpp`
   wiring (pulling in `ledger.hpp` transitively via `mandatory_sandbox_provider.hpp`, into a file whose
   existing dependency graph already reached `worktree_merge.hpp`) is what first combined them. Fixed
   by renaming to `LedgerMergeConflict`/`LedgerMergeResult` — distinct names for a genuinely distinct
   concept, matching this whole design's own established discipline (`IdentityHandle` vs. `Principal`,
   `SurfaceRunOutcome` vs. `ExecOutcome`, `SandboxRunOutcome` vs. `a2a::RunOutcome`), this time found the
   hard way by a real compile error rather than caught in review.
2. **A second, purely mechanical build failure, fixed same day**: once the redefinition was gone, MSVC
   reported `C1128: number of sections exceeded object file format limit` — `cli_chat.cpp`'s own
   already-substantial template load (CodeAct/skills machinery) plus the newly-added
   `MandatorySandboxProvider<DockerExecutionSurface>`/`SandboxRuntime`/`Ledger<>` templates pushed one
   `.obj` past MSVC's COFF section-count limit. Fixed with `/bigobj`, scoped to only this one target
   (`CMakeLists.txt`), not project-wide.
3. **A real, live end-to-end smoke test**, run against a genuine OpenRouter-backed model (not a
   scripted/fake client): the model correctly discovered and called `run_command`, a real Docker
   container executed `echo -n phase5-smoke-test-ok > /workspace_proof.txt && cat /workspace_proof.txt`,
   and the real result was independently verified from the raw JSON request/response dump (not merely
   trusted from the model's own narration) — `run_command` present in the real `tools` array sent to the
   model, the real `RunCommandReply` JSON (`"ok":true`, the exact stdout, a real committed tree digest)
   present in the real tool-result content. This is the FIRST time any part of this whole four-phase
   effort has been driven by a genuine, unscripted model rather than a test fixture.
4. **A CONFIRMED (not merely disclosed) real finding from that same smoke test**: after the session
   ended via `exit`, `docker ps -a` showed the real container `run_command` had used still running.
   Traced to `run_interactive()`'s own deliberate `std::_Exit(0)` (a pre-existing, well-justified fix for
   a genuine CPython `Py_Finalize` thread-affinity crash, ADR-034/ADR-037) — skipping all C++ destructors
   on every ordinary exit, including `DockerExecutionSurface::~DockerExecutionSurface()`. This is the
   SAME leak residual Phase 3 already disclosed (there found to trigger on a transient `docker rm -f`
   failure, not only a process crash) — this is a THIRD, now EMPIRICALLY CONFIRMED trigger: an ordinary
   `exit`/`quit` from this CLI, after using `run_command` even once, leaks a real container until
   something else cleans it up. Disclosed inline at the `std::_Exit(0)` call site itself, not fixed —
   doing so needs a real reclaim mechanism (the same "persist instance ids somewhere reclaimable"
   follow-on work Phase 3's own residual already named), not a one-off special case in this one CLI.
   The leaked container from this session's own smoke test was found and removed (`docker rm -f`).
5. **Independent red-team round, SHOULD-FIX**: a fresh pass, reading the real diff plus the actual dump
   files this bring-up left behind, correctly confirmed the `LedgerMergeConflict`/`LedgerMergeResult`
   rename is complete (full-tree grep found no other collision among every Phase 1-4 public name), the
   I2/I3 posture of the new wiring is clean (`run_command`'s reachability and the identity it authorizes
   against are both 100% host-controlled, never model-derived), the lifetime-ordering fix (item 1 above)
   is correct and complete across every early-return path in `run_interactive()`, and the container-leak
   disclosure (finding 4) is accurate, not worse than stated. It also, independently, found a genuinely
   useful gap worth closing on its own merits, even though its own causal story for WHY the gap mattered
   was itself incorrect: reading a stale dump from earlier in this session's own bring-up (captured
   against the pre-fix, pre-rebuild `.exe` still on disk from the masked build failure in finding 1 —
   not a new, separate regression as the round's own report first concluded), it correctly identified
   that the exact composition `cli_chat.cpp` uses (one `ContextProvider` embedding
   `MandatorySandboxProvider` and merging its contribution outside any skill-scoping) had ZERO automated
   test coverage anywhere — `tests/test_mandatory_sandbox_provider.cpp` only ever exercises
   `MandatorySandboxProvider` as the SOLE `HistoryProviderT`, never composed alongside a second provider
   the way `cli_chat.cpp` actually does it, and `cli_chat.cpp` itself is not unit-testable in isolation
   (needs `AGENTENGINE_WITH_HTTPS`/`AGENTENGINE_BUILD_PYTHON_RUNNER` and a live model). Closed with a
   new, Docker-independent test, `tests/test_mandatory_sandbox_provider_composed.cpp` — a minimal
   `ComposedProvider` mirroring `ToolDeclaringHistoryProvider`'s own real shape (a `FakeExecutionSurface`
   stand-in means no Docker daemon is needed to prove the DECLARATION-time composition, only actual
   invocation needs the real one), proving `run_command` is present from the very first `on_context()`
   call after `bind_sandbox()`, alongside the composing provider's own tool, including across a real
   fork.

## 33. Falsifiable claims (Phase 5)

- **C13 (`run_command` wired into the real CLI host works end to end against a genuine, unscripted
  model).** *Disproof: the model cannot discover or successfully invoke `run_command` through the real
  host, or the reported outcome does not match the real, independently-verified result.* — **CORRECT**,
  proved live (§32 finding 3) with independent verification from the raw dump, not the model's own
  claim.
- **C14 (the newly-introduced production composition — `MandatorySandboxProvider` embedded in and
  merged by another `ContextProvider`, outside skill-scoping — is itself correct, from the first call
  after binding, not just eventually).** *Disproof: a real test proving this composition's own
  correctness does not exist, or one that does exist fails.* — **CORRECT**, closed by a new test
  (§32 finding 5) that did not exist before this phase, itself passing.

## 34. Executed evidence (Phase 5)

- `tools/cli_chat.cpp`, `include/agentengine/core/ledger.hpp` (renamed types), `CMakeLists.txt`
  (`/bigobj`, `agentengine::worktree_store` link): all compiled clean under MSVC 19.51 after the two
  real build-failure findings (§32 items 1-2) were fixed — confirmed via the ACTUAL `ninja` exit code
  this time, not a shell-wrapper-masked one (the same lesson finding 1 itself taught, applied to every
  subsequent build in this phase).
- A real, live interactive smoke test against a genuine OpenRouter-backed model: `run_command`
  discovered and called correctly, a real Docker container executed a real command, the result
  independently verified from the raw request/response JSON dump.
- `tests/test_mandatory_sandbox_provider_composed.cpp`: 3 check groups, 100% passing, no Docker daemon
  required (a `FakeExecutionSurface` stand-in proves the declaration-time composition only) — the first
  automated coverage anywhere for the exact "embed-and-merge, outside skill-scoping" composition pattern
  `cli_chat.cpp` uses.
- `docker ps -a` confirmed the one real container leaked during the live smoke test (§32 finding 4,
  the `std::_Exit(0)` container-leak confirmation) and confirmed it was cleaned up (`docker rm -f`)
  after being found.
- Full project rebuild: 306/306 targets (including `agentengine_cli_chat`) plus the full test suite
  (377/377 build targets in total), zero errors, run three times across this phase (once after each
  real fix — the naming collision, `/bigobj`, and the new composed-provider test).
- Full `ctest` suite: 285/285 passing, zero regressions, run twice across this phase.
- `python tools/naming_lint.py`: registering `LedgerMergeConflict`/`LedgerMergeResult` (the renamed
  Phase 2 types) confirmed zero unregistered names remain.

## 35. Per-claim verdicts (Phase 5)

| Claim | Verdict | Evidence |
|---|---|---|
| C13 — `run_command` works end to end against a real, unscripted model | **CORRECT** | Live smoke test, independently verified from the raw dump. |
| C14 — the new embed-and-merge composition is correct from the first call | **CORRECT, after closing a real test-coverage gap** | A new, Docker-independent test now proves it; previously unproven by anything automated. |

## 36. Residual risks (Phase 5)

- **The Docker container leak on `std::_Exit(0)`** (§32 finding 4) is now a CONFIRMED, not merely
  theoretical, third trigger for Phase 3's own already-disclosed residual. Still not fixed — the real
  fix (persisting instance ids somewhere reclaimable) remains real, contained follow-on work, now with
  one more piece of concrete evidence for why it matters in practice, not just in principle.
- **The `LedgerMergeConflict`/`LedgerMergeResult` collision was found and fixed, but the METHOD of
  discovery — a real compile error surfacing only once two previously-uncombined headers finally landed
  in one translation unit — is itself a standing risk pattern this whole effort should budget for
  explicitly going forward**: `tools/naming_lint.py`'s own registration check cannot catch this class of
  bug (it verifies a name is documented, not that it doesn't already exist, unrelated, elsewhere in the
  same namespace), and nothing short of actually combining every header pair in one build would have
  caught it earlier. Real follow-on consideration for whoever next lands a broad, tree-wide `#include`
  audit or a CI job that compiles a "kitchen sink" translation unit including every public header.
- **This Phase 5 slice covers `tools/cli_chat.cpp` wiring plus, additionally, closing `AgentSession::
  fork_from()`'s own lack of session-level serialization** (Phase 3 §22/Phase 4 §29's shared finding —
  see §37 onward below for the real fix, its own red-team round, and the real, empirically-confirmed
  new hazard that round found and this ADR discloses rather than fixes). `ContainerdExecutionSurface`/
  ADR-101 promotion and Windows/Linux parity remain deferred, unauthorized by this ADR, named for
  whichever future session picks them up next.
- **Every residual already named by Phases 1-4 remains unchanged and unaddressed by this phase**: the
  blob-content-level ACL gap post-merge, `Ledger::merge()`'s missing `AsyncQuota` gate, durable content
  storage out of scope, and this design overall still awaiting `Judged` sign-off.
- **The pattern held a fifth time, in its most consequential shape yet**: this is the first phase where
  the "composition surfaces what isolation cannot" lesson (already named after Phases 3 and 4) manifested
  as a genuine BUILD FAILURE from a silent, three-phase-old struct-name collision, not merely a logic or
  concurrency defect caught by review or a targeted repro. The first real host integration of a
  four-phase effort is exactly where a latent, cross-cutting naming collision was always going to surface
  — later, not earlier, is when composition finally happens for real. Whoever attempts Phase 5's
  remaining, deferred items (or any future integration of this design into a second real host) should
  expect the same category of surprise, not assume the four phases' own isolated proofs already ruled it
  out.

## 37. Closing `AgentSession::fork_from()`'s own session-serialization gap

**The question, stated so it has a wrong answer:** Phase 3 §22 and Phase 4 §29 each independently
disclosed, but did not fix, the same real structural gap: `agentengine::rt::AgentSession::fork_from()`
(`include/agentengine/rt/agent_session.hpp`) ran with NO serialization against a concurrent, in-flight
`start_run()`/`resolve_interaction()` on its own `source` argument — unlike every OTHER public entry
point on this class, which all acquire a private `session_mutex_` (`AsyncMutex`, I1's own enforcement
mechanism) for their whole duration. Does closing this gap, on the single most heavily-used class in the
entire codebase, actually work — and does the fix itself introduce a new hazard the disclosure never
named?

**The fix:** `session_mutex_` is now `mutable` (the same rationale `core/ledger.hpp`'s own `mutable
std::mutex mutex_` already established for the identical shape — a real synchronization primitive that
must stay lockable from a conceptually-const access path). A new free function, `agent_session_detail::
acquire_session_mutex(AsyncMutex&)` (a two-line coroutine, `co_return co_await m.lock();`), lets
`fork_from()` acquire `source.session_mutex_` for the whole copy, driven synchronously via
`agentengine::rt::block_on()` (Phase 4's own new primitive, `rt/block_on.hpp`) so `fork_from()` itself
stays a plain, non-coroutine, signature-unchanged function — no call site anywhere in this codebase needs
to change. Deliberately scoped to lock ONLY `source`'s mutex, not `*this`'s: every real call site in this
codebase forks INTO a fresh, not-yet-`start_run()`-able target, so guarding `*this` too would invent new
semantics for a usage pattern nothing else in this codebase exercises.

**Real, dedicated proof, not just "the existing suite still passes":** `tests/test_rt_agent_session_
fork_from_serialization.cpp` races a genuinely slow `ChatClient::chat()` (a real 150ms sleep, holding
`session_mutex_` for a measurable duration on one thread) against a concurrent `fork_from()` call on
another, and asserts the forked target's copied `history()` is the COMPLETE 2-message post-round result,
never a torn snapshot a broken, unserialized `fork_from()` could observe mid-round. This session
EMPIRICALLY VERIFIED the check is a real positive control, not an accidental pass: temporarily
commenting out just the new lock-acquisition line and rebuilding made this exact check fail 3/3 runs,
with everything else in the file still passing — confirming the check exercises precisely the fix, not
some unrelated path. The fix was restored and the full suite re-confirmed green afterward.

## 38. The red-team attack (the `fork_from()` fix)

Given the blast radius (`agent_session.hpp` is the single most-used class in the codebase — dozens of
test files, every real host wiring), a dedicated independent round was run against this specific change,
separate from Phase 5's own broader round. It found one real, empirically-confirmed MUST-FIX and
confirmed the fix's own already-disclosed scoping decision is sound; everything else checked out clean.

1. **MUST-FIX, EMPIRICALLY CONFIRMED, real regression this fix itself introduces**: `AsyncMutex` has no
   reentrancy check (`held_` is a plain bool, no owner-thread tracking, `rt/async_mutex.hpp`). Before
   this fix, `fork_from()` touched no lock at all, so it could never deadlock. Now, calling
   `fork_from(source, ...)` (self-fork included, `source == *this`) from code ALREADY running on the
   same OS thread inside an in-flight `start_run()`/`resolve_interaction()` round on `source` — e.g.
   synchronously, from a tool closure's own body, the exact shape `schedule_wakeup`'s own real, shipped
   closure already routes around via an internal `_impl` bypass for this identical reason — would
   genuinely, reproducibly self-deadlock: `block_on()`'s own busy-wait spins forever, because the only
   thing that could ever call `unlock()` is the very `start_run()`/`resolve_interaction()` Guard already
   parked one frame up on the same stack, waiting for this call to return. The red-team round confirmed
   this with a real, targeted repro (a `ChatClient::chat()` that calls `self->fork_from(*self, ...)` from
   inside a live round, same thread) — a 100%-reproducible hang across every run, not a rare race. NOT
   reachable through any real call site in this codebase today (every `fork_from()` caller is a
   top-level `main()`, never a tool closure or `ChatClient::chat()` body) — but exactly the shape a
   near-future `agent.spawn`-style tool wired to call `fork_from()` directly from its own closure would
   hit, silently (an indefinite CPU-spinning hang, no crash, no diagnostic — a materially worse failure
   mode than a clean, fast error). Disclosed inline, in detail, at the fix's own call site (`agent_
   session.hpp`) — matching this whole four-phase-plus effort's own established "disclosed, not
   reachable today, real follow-on work" pattern for an identically-shaped hazard (`SandboxRuntime::
   spawn_child_branch()`'s own reentrancy caveat, `MandatorySandboxProvider`'s copy-assignment's own).
   Not fixed in this pass: doing so correctly needs either owner-thread tracking on `AsyncMutex` itself
   (a broader change to a low-level primitive several other real call sites also rely on) or a
   `fork_from()`-local reentrancy guard — real, contained follow-on work.
2. **Confirmed sound, not a new finding**: the fix's own "only lock `source`, not `*this`" scoping
   decision — stress-tested against a deliberately-misused scenario (racing `fork_from()` calls against
   a target with its own independent, concurrently-live round) and found to be a real, unguarded
   (by design, disclosed) data race on `*this`'s own field writes if that unsupported usage pattern were
   ever exercised — consistent with, not worse than, the fix's own already-stated scope decision.
3. **Confirmed clean**: `block_on()`'s own mechanism correctly handles `AsyncMutex::Guard` (a move-only,
   RAII-releasing type) as its payload with no double-release or lifetime bug; `mutable` on
   `session_mutex_` introduces no other const-correctness hole anywhere else in the codebase (no other
   const-qualified path touches it); the new test's own timing (150ms sleep / 20ms head start) skews
   toward a false FAILURE under scheduler starvation, never a false pass.

## 39. A real process incident during this same work, disclosed for the record

While the independent red-team round above was investigating, its own cleanup step for temporary scratch
files it had added to `tests/CMakeLists.txt` used a git-level revert of that whole file rather than
undoing only its own added lines — silently discarding every one of this session's own legitimate,
already-verified `add_executable`/`add_test` registrations for Phases 1 through 5 (seven separate blocks:
`test_identity_authority_grant`, two `try_compile()` compile-fail probes, `test_ledger`, `test_sandbox_
runtime`, `test_mandatory_sandbox_provider`, `test_rt_block_on`, `test_mandatory_sandbox_provider_
composed`, and, moments later, `test_rt_agent_session_fork_from_serialization` itself). This was not
caught immediately — a subsequent full rebuild still succeeded (ninja's own build.ninja file still had
cached rules for the already-compiled targets) — and was only discovered when a full `ctest` run's own
total test count silently dropped from 286 to 279. Diagnosed by comparing `git status`/`git diff` against
the actual source files still present on disk (all seven `.cpp` test files were untouched, only their
CMake WIRING was lost), then recovered by manually re-adding all seven registration blocks from this
session's own conversation history, followed by a full reconfigure/rebuild/`ctest` cycle confirming
286/286 passing again with zero further loss. No test SOURCE content was ever lost — only the build-graph
registration, and only for one file, recovered without needing to re-derive or re-verify any test's own
logic. Disclosed here as a real, if narrow, process incident — not a design or code defect in ADR-102's
own subject matter — because it directly affected how this ADR's own Phase 5 evidence was produced and
is the kind of incident a future session re-running this exact verification sequence should watch for.

## 40. Per-claim verdict and residual (the `fork_from()` fix)

| Claim | Verdict | Evidence |
|---|---|---|
| `fork_from()` now genuinely serializes against a concurrent in-flight round on its own `source` | **CORRECT, empirically proven both ways** | A real two-thread race test passes with the fix and fails 3/3 with it forced-reverted; full 286-test suite green afterward. |

**Residual, disclosed not fixed**: the fix itself introduces a real, empirically-confirmed self-deadlock
hazard (§38 finding 1) reachable only by a call shape nothing in this codebase exercises today, but
plausibly exactly what a near-future `agent.spawn`-style tool would reach for. Real follow-on work:
either owner-thread-aware reentrancy detection on `AsyncMutex` itself, or a narrower, `fork_from()`-local
guard — named explicitly here so whoever wires that future caller does not rediscover this the hard way
(a silent, undiagnosed hang, not a clean error).

## 41. The question (Phase 5, `ComposedContextProvider<Ms...>` real production consumer slice)

**Stated so it has a wrong answer:** ADR-102's own §7 named a real, disclosed cost of Phase 4's
"bare `HistoryProviderT`" wiring choice: `agentengine::ComposedContextProvider<Ms...>`
(`core/composed_context_provider.hpp`, ADR-074's consolidation) had, before this slice, **zero real
production consumers anywhere in this codebase** — every use was either a unit test driving
`on_context()`/`on_turn_end()` directly (`tests/test_session_builder.cpp`'s own top comment: "no
`.raw_client_only()` escape hatch... driven DIRECTLY instead") or `docs/planning/proofs/` probe code.
`SandboxToolProvider` (ADR-096, `src/backends/native_jail/sandbox_tool_provider.hpp`) — the ONE real
conformer this codebase ships specifically *for* composing via `ComposedContextProvider<Ms...>` — had
the identical gap: zero real callers beyond its own test. Does composing `SandboxToolProvider` and
`MandatorySandboxProvider<DockerExecutionSurface>` together, through `ComposedContextProvider<Ms...>`,
into ONE real `AgentSession`, actually work when driven through a real `session.start_run()` round for
the first time — or does combining two independently-red-teamed providers, and a composition mechanism
that has never carried a real session through a real tool-calling round, surface a new gap none of the
three, alone, could have shown (the same shape of question Phase 3 asked of its own four
previously-standalone files, and answered "yes, a new gap" — §16-19)?

## 42. Design

Two new files, no changes to any Phase 1-4 file:

- `tests/test_composed_sandbox_providers_live.cpp` — a real, Docker-and-Windows-requiring test proving
  `agentengine::rt::AgentSession<ScriptedChatClient, NoSessionState, ComposedContextProvider<
  SandboxToolProvider, MandatorySandboxProvider<DockerExecutionSurface>>>` end to end: [1] one
  `on_context()` call from the composed provider contributes BOTH tools; [2] a scripted `run_command`
  tool call, driven through the real, unmodified `session.start_run()` -> `invoke_tool()` pipeline,
  genuinely executes in a real Docker container and commits a real `Ledger` checkpoint; [3] a scripted
  `run_shell` tool call, in the SAME session, right after [2], genuinely executes against the real host
  filesystem via `SandboxToolProvider`'s own native jail. Mirrors `tests/test_mandatory_sandbox_
  provider.cpp`'s (Phase 4) `ScriptedChatClient`/`tool_call_message()`/`drive()` fixtures verbatim (no
  shared header exports these — every file that needs them defines its own copy, the established
  convention in this test suite) and `tests/test_sandbox_tool_provider.cpp`'s own digest-based
  scratch-directory verification.
- `tools/sandboxed_shell_chat.cpp` — a new, small, real, user-reachable CLI binary (`agentengine_
  sandboxed_shell_chat`), the first genuinely production host wiring of `ComposedContextProvider<Ms...>`
  anywhere in this codebase. Deliberately NOT a change to `tools/cli_chat.cpp` itself: that file's own
  `ToolDeclaringHistoryProvider` stays a session's BARE `HistoryProviderT` specifically so `AgentSession::
  fork_from()` keeps compiling for the flagship interactive CLI's own session type — wrapping it in
  `ComposedContextProvider<Ms...>` (unconditionally move-only, ADR-074 Finding B) would make `fork_from()`
  a compile error there, an avoidable regression this design does not risk for a tool with no forking
  feature to lose. This new tool's own session type never calls `fork_from()`, so the cost is free here.
  Reuses, rather than re-implements, `quickstart::ComposedQuickstartSessionBuilder<Provider, Store,
  Ms...>` (`core/session_builder.hpp` §2b) for credential/capability/session wiring and `Bundle::ask()`
  (same file) for its REPL loop — both already-shipped, already-tested machinery that, like
  `ComposedContextProvider<Ms...>` itself, had no real production caller before this file. Exits via an
  ordinary `return 0;`, not `cli_chat.cpp`'s own `std::_Exit(0)` (a disclosed, unrelated fix for a real
  CPython thread-affinity crash this tool never triggers, having no Python embed) — so
  `DockerExecutionSurface`'s destructor reclaims its container on every ordinary exit here, closing, for
  THIS tool specifically, the container-leak residual §19/§30 disclosed for `cli_chat.cpp`.

`MandatorySandboxProvider::bind_sandbox()` is called on the LOCAL value in both new files, BEFORE the
provider is moved into `ComposedContextProvider::engage()`/the builder's `.providers()` tuple — a real,
disclosed divergence from `cli_chat.cpp`'s own Phase 5 pattern (which reaches back into an already-
composed provider via a dedicated `run_command_provider()` accessor, AFTER construction): once engaged,
`ComposedContextProvider<Ms...>`'s own descriptor factory (`context_assembly.hpp::make_context_provider_
descriptor()`) type-erases each `Ms` into a `shared_ptr<Ms>` reachable only through its own `on_context`/
`on_turn_end` closures, with no accessor back to the concrete instance — binding first, then composing,
needs no such accessor and is the only order that works for this composition shape.

## 43. Falsifiable claims (Phase 5, `ComposedContextProvider<Ms...>` slice)

- **C10 (structural coexistence).** One `on_context()` call from the composed provider contributes
  BOTH `run_shell` and `run_command`, in the declared order. *Disproof: either tool is missing, or a
  third, phantom tool appears.*
- **C11 (functional coexistence, `run_command`).** A `run_command` tool call, driven through the real
  `invoke_tool()` pipeline in a session where `SandboxToolProvider` is ALSO composed, genuinely executes
  in a real Docker container and commits a real `Ledger` checkpoint — unaffected by the sibling
  provider now sharing its `ContextContribution`. *Disproof: the command does not execute, or the
  checkpoint is missing/wrong, when composed vs. Phase 4's own already-proven bare-provider case.*
- **C12 (functional coexistence, `run_shell`).** A `run_shell` tool call, in the SAME session, right
  after a `run_command` call, genuinely executes against the real host filesystem — the two providers'
  genuinely different resource shapes (a live OS process vs. a content-addressed ledger branch) do not
  interfere with each other. *Disproof: `run_shell` fails, writes to the wrong location, or corrupts/
  is corrupted by `run_command`'s own state.*

## 44. The red-team attack

One independent, fresh-agent adversarial round (2026-08-28), against the actually-landed, compiled,
live-Docker-tested code (not the plan): read this ADR in full including §1-40 for context, read both
new files completely, traced `ComposedContextProvider::engage()` -> `build_contributors()` ->
`make_context_provider_descriptor()` (`context_assembly.hpp`), `MandatorySandboxProvider::
bind_sandbox()`, `BranchHandle::~BranchHandle()`/`maybe_queue_abandon()`,
`ComposedQuickstartSessionBuilder::providers()`/`build()`, `invoke_tool()`'s 10-step pipeline, and
`RunShellTool`/`RunCommandTool`'s own capability declarations. Then went further than static tracing:
did a REAL build with real MSVC, ran the new test against the real Docker daemon on this machine (all
15 checks passing), then additionally built and ran the SAME test under a **clang AddressSanitizer
build** against the same real Docker daemon — the strongest empirical check available for the
lifetime/dangling-pointer claim §42's whole "bind before compose" design rests on — and separately ran
the CLI tool's own early-return path (`OPENAI_API_KEY` unset) to exercise "the temporary `Builder` is
destroyed, with an already-bound `MandatorySandboxProvider` inside it, while `cli_ledger`/quotas are
still alive in the enclosing scope" directly, not just reasoned about.

**No MUST-FIX found.** The round went in assuming a real defect existed (this design's own established
track record, cited to it explicitly) and could not find one after real build + real ASan-instrumented
execution + a full static trace of every claim in §42-43.

**Two real SHOULD-FIX findings, both fixed same day (disclosure-only, no behavior change):**
1. `tools/sandboxed_shell_chat.cpp`'s own container-leak-closed claim overreached: it covered only the
   contrast against `cli_chat.cpp`'s deliberate `std::_Exit(0)`, saying nothing about Ctrl+C — this
   codebase installs no `SetConsoleCtrlHandler`/`SIGINT` handler anywhere (grep-confirmed, zero hits
   including in `cli_chat.cpp`), so Windows' own default console handler calls `ExitProcess()` directly
   on Ctrl+C, never unwinding `main()`'s stack (and therefore never running `DockerExecutionSurface`'s
   destructor) — an entirely ordinary way an interactive user ends a REPL session, not an edge case.
   **Fixed**: the file's own top comment now states this explicitly, scoped correctly to "ordinary
   `return` paths only," with the missing-SIGINT-handler gap named as real, disclosed, not-attempted-
   in-this-pass follow-on work.
2. `Principal const cli_principal{"cli-user", ""}` uses an empty `tenant_id` with no comment warning a
   future reader against copying this literal pattern into a real multi-tenant host — doing so would
   silently reproduce the exact cross-tenant identity-collision class Phase 1's own §4 MUST-FIX fixed
   (`adopt()` keying on `(tenant_id, id)`, not `id` alone). **Fixed**: a comment now states this
   explicitly at the declaration site.

**One SHOULD-FIX-level observation, disclosed rather than changed**: `session_digest_of()`
(`tests/test_composed_sandbox_providers_live.cpp`) is a hand-duplicated copy of `SandboxToolProvider::
ensure_sandbox()`'s own digest computation, not a call into shared code — matching this test suite's
own established "every file defines its own copy" convention (confirmed not a new pattern by the
red-team round itself), but a real, named drift risk: if `ensure_sandbox()`'s own byte-encoding of
`session_id` ever changes, this test's independent copy could silently fall out of sync. Left as-is,
matching the accepted convention, named here for the record rather than left implicit a second time.

**Checked, no finding**:
- **The lifetime/dangling-pointer claim (§42's central claim)**: `ContextProviderDescriptor`
  (`context_assembly.hpp`) exposes only `name`/`budget`/`on_context`/`on_turn_end` — confirmed there is
  genuinely no accessor back to the concrete provider instance once engaged, so "bind before compose"
  is not merely asserted, it is the only order that works. Reverse-declaration-order destruction
  confirmed correct in both files by direct trace AND by a real ASan-instrumented run reaching full
  program teardown (session destruction -> `shared_ptr` refcount to zero -> `SandboxRuntime`/
  `BranchHandle` destruction -> the real `Ledger*`/quota-pointer dereferences this design's whole
  lifetime argument depends on) with zero ASan diagnostics.
- **The CLI tool's early-return-before-`built`-succeeds path**: run live with `OPENAI_API_KEY` unset —
  clean `FATAL:` message, exit 1, no crash, and `docker ps -a` confirmed unchanged before/after (no new
  leaked container from the temporary `Builder`'s own destruction while holding an already-bound
  `MandatorySandboxProvider`).
- **I2/I3, capability confusion between the two composed providers**: traced `invoke_tool()`'s own
  capability-binding step and confirmed it binds ONLY the individual tool's own static ceiling, never
  the session's whole held set — `RunShellTool`'s declared `Capabilities<cap::decl::FsRead<"work">,
  cap::decl::FsWrite<"work">>` exactly matches both new files' own grants (not too broad, not too
  narrow); `RunCommandTool`'s ceiling is genuinely empty (zero policy parameters), matching its
  "authorizes via `IdentityAuthority`/`Grant<T>`/`AsyncQuota<T>` instead" design — no static-capability
  path exists for it to be widened or confused by. Confirmed neither provider's `on_context()` touches
  the `EffectContext` field the other one owns (`ctx.principal` vs. `ctx.sandbox_fs`) — no
  cross-provider interference in either direction.
- **Test rigor**: all three claims' checks (C10/C11/C12) are real positive controls, not vacuous —
  independently confirmed each reads back real, independently-derived state (the real `Ledger` entry,
  a real `std::filesystem::exists()` check on an independently-computed path), never merely the tool's
  own self-reported reply.
- **CMake correctness**: confirmed by nesting-depth trace (not just visual indentation) that the new
  test's registration genuinely sits inside the same `if(WIN32)` block as its sibling
  `test_sandbox_tool_provider`, with an identical, correctly-scoped link line; confirmed the new tool
  target's `AGENTENGINE_WITH_HTTPS AND WIN32` gate is correct for what it actually includes/links
  (genuinely no Python-runner dependency). Both targets re-built a second time and got ninja's own
  "no work to do" — avoiding the exact shell-wrapper-masked-failure trap this ADR's own §32 finding 1
  already named as a real, previously-hit hazard in this same effort.

## 45. Executed evidence

- `tests/test_composed_sandbox_providers_live.cpp`: 15 checks, 100% passing, run against a REAL Docker
  daemon on Windows — three times independently (this session's own first run, the independent
  red-team round's plain-MSVC run, and that same round's clang-ASan-instrumented run), zero failures
  and zero ASan diagnostics across all three.
- `tools/sandboxed_shell_chat.cpp`: compiled clean under MSVC 19.51, zero new warnings. A real, live
  smoke test against a genuine OpenRouter-backed model (not a scripted fixture — this codebase's own
  only reachable real credential) confirmed the REPL/tool-dispatch plumbing works end to end: the
  composed session's `run_shell` tool was genuinely discovered and repeatedly, successfully invoked
  by an unscripted model across a real multi-turn tool-calling loop, with real `ToolResult`s round-
  tripping back into history each time (the specific free-tier model available did not also exercise
  `run_command` in this particular run and looped on `run_shell` past a useful final answer -- model
  behavior, not a defect in the wiring already proven deterministically by the automated test above).
  Independently, the red-team round's own early-return smoke test (`OPENAI_API_KEY` unset) confirmed
  the tool's fail-closed path is clean and leak-free.
- `python tools/naming_lint.py`: `027 naming-lint: OK` — no new public C++ types were introduced by
  this slice (pure composition of already-registered names), so no new vocabulary rows were needed.
- Full project rebuild: clean, zero errors, including both new targets
  (`agentengine_sandboxed_shell_chat`, `test_composed_sandbox_providers_live`).
- Full `ctest` suite: 287/287 passing (286 + the new test), zero regressions, one genuinely clean run
  with no flakes at all (a first for this whole multi-phase effort's own `-j4` parallel-execution
  history).

## 46. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| C10 — structural coexistence (one `on_context()` contributes both tools) | **CORRECT** | Check [1], independently confirmed a real positive control by the red-team round. |
| C11 — functional coexistence, `run_command` unaffected by a composed sibling | **CORRECT** | Check [2]: real Docker execution + independent `Ledger` read-back, unchanged from Phase 4's own already-proven bare-provider behavior. |
| C12 — functional coexistence, `run_shell` unaffected by a composed sibling | **CORRECT** | Check [3]: real host-filesystem read-back, independently verified path computation. |

## 47. Residual risks (Phase 5, `ComposedContextProvider<Ms...>` slice)

- **No `SetConsoleCtrlHandler`/`SIGINT` handling anywhere in this codebase** (not introduced by this
  slice — a pre-existing, whole-codebase gap this slice's own red-team round is the first to name
  explicitly): an interactive user's Ctrl+C on either `tools/cli_chat.cpp` or the new
  `tools/sandboxed_shell_chat.cpp` bypasses all C++ destructors via Windows' own default handler,
  leaking any live `DockerExecutionSurface` container the same way `std::_Exit(0)`/an unhandled crash
  already does. Real, contained follow-on work — a real signal handler that at minimum attempts a
  best-effort `DockerExecutionSurface` teardown before `ExitProcess()`.
- **`session_digest_of()`'s hand-duplicated-vs.-`SandboxToolProvider` drift risk** (§44) — matches an
  already-accepted test-suite convention, not a new pattern, named for the record.
- Every residual already named at the ADR level (§8/§15/§36/§40 — the blob-content-level ACL gap,
  `Ledger::merge()`'s missing `AsyncQuota` gate, `fork_from()`'s self-deadlock hazard, this whole
  design's own pending `Judged` sign-off) is unchanged and out of this slice's own scope; none of them
  are touched or worsened by either new file.
- **Still out of scope, named not dropped** (§7): `ContainerdExecutionSurface`/ADR-101 promotion,
  Windows/Linux parity, and `SandboxToolProvider`'s own `fork_from`-becomes-compile-error negative
  probe (asserted only in a comment, never actually triggered anywhere in this codebase).
