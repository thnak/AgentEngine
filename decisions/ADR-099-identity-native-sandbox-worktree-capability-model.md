# ADR-099 — Does the mandatory session/sandbox/checkpoint stack get a clean-slate identity-native `Grant<T>`/`IdentityAuthority`/`Ledger`/`SandboxSession` model, or stay built on `CapabilitySet`/`AppendLogStore`/`WorktreeObjectStore`?

**Status:** Proposed — design, five independent red-team rounds (spread across the historical
document's four revisions plus this document's own Revision-5-equivalent post-prove-phase round),
and an unusually deep prove phase (§20–§35 of the design record, including **two independent
adversarial code-review passes** that each found and fixed real bugs) are all complete. **No
production code has been written or merged from this design** — every primitive
(`IdentityAuthority`/`Principal`/`Grant<T>`, `AsyncQuota<T>`, `Ledger<Store>`/`BranchHandle`,
`SandboxSession`/`MediatedFileSystem`) exists only as standalone, real, compiler-verified C++23
under `docs/planning/proofs/`, deliberately never linked against `include/agentengine/` (the
design's own stated no-reuse-at-design-time discipline). This ADR is a **design-acceptance** record,
not an implementation one — it does not authorize merging any of `docs/planning/proofs/` into the
live engine. Two concrete, load-bearing follow-on decisions (A3, A9 below) are explicitly deferred
to future ADRs of their own, matching this project's own convention (e.g. ADR-012 → ADR-080,
ADR-086 → ADR-087) — do not treat their absence here as an oversight.

**Full design record** (the actual "the full record — N revisions, N red-team rounds" this ADR
summarizes, not duplicates): `docs/planning/identity-native-sandbox-worktree-design.md` (grown past
§35 to §39 as of 2026-08-27 — A3/A9 execution-surface and mandatory-sandbox-binding work, external
CVE/incident validation, and A10's task-branch tool surface, all post-dating this ADR's own last
edit; check the design doc's own section list rather than trusting a stale line count here) and its
superseded predecessor `docs/planning/mandatory-session-worktree-design.md` (4 revisions, 4
red-team rounds, kept as-is per that document's own header — Design A below).

**Relates to:** `007-Capability-and-Trust-Model.md` (`CapabilitySet`, I2/I3 — the model Design A
retrofits onto and Design B replaces), `008-Sandbox-and-Isolation.md` (the mandatory-sandbox
requirement itself; §2a's `SandboxBackend` concept), `decisions/ADR-012-sandbox-profile-template-
parameter-kind.md`, `decisions/ADR-080-sandbox-backend-registry.md`, `decisions/ADR-087-sandbox-
spec-capability-enforcement.md`, `decisions/ADR-096-session-sandbox-lifecycle-context-provider-
wiring.md`, `decisions/ADR-098-default-sandbox-backend-registry-wiring.md` — **all four of these are
real, shipped or implemented-and-tested code built on `CapabilitySet`/`SandboxSpec`/
`SandboxBackendRegistry`, none of it built on or aware of this design's `Grant<T>`/
`IdentityAuthority`/`Ledger`. This ADR does not reconcile with them — see §7's "Explicitly out of
scope" and §8's residual.** `decisions/ADR-071-native-unsandboxed-process-execution-providers.md`
(the Delegated Decision Seam precedent this design's own feature-vs-safety framing follows).

## 1. The question

**Stated so it has a wrong answer:** `008-Sandbox-and-Isolation.md`'s mandatory requirement — every
session gets exactly one sandbox and one rewindable checkpoint history, no exceptions — needs an
identity/authorization/durability model underneath it. Does that model get built by **retrofitting**
the existing, already-Judged-track `CapabilitySet`/`AppendLogStore`/`WorktreeObjectStore`/
`SandboxBackendRegistry` primitives (Design A), or by building a **clean-slate** identity-native
stack — `Grant<T>` keyed to a real `Principal`, a durable multi-hop `IdentityAuthority` ancestry
table, one coroutine-native `AsyncQuota<T>` reused for every budget kind, a content-addressed
`Ledger` with real branch/merge — that shares nothing with the old model except
`WorktreeObjectStore`'s pure content-addressing primitive (Design B)?

## 2. The competing designs

### Design A (rejected, after four revisions and four independent red-team rounds) — retrofit onto `CapabilitySet`/`AppendLogStore`/`WorktreeObjectStore`

`docs/planning/mandatory-session-worktree-design.md`. Makes `Sandbox` an `AgentSession` structural
member (not a `ContextProvider`), reuses `CapabilitySet` for authorization
(`cap::SandboxReset`/`cap::FsRead`/`cap::FsWrite`), reuses `AppendLogStore`/`WorktreeObjectStore` for
the checkpoint ledger, and `SpawnCostBudget` (already real, `rt/spawn_cost_budget.hpp`) for
per-branch cost accounting.

**Steelman.** Every primitive it reuses is already real, already-Judged-track, already-tested
production code — zero new authority model for a reviewer to learn, and a direct integration story
with whatever eventually consumes `SandboxBackendRegistry`. The smallest possible diff from today's
actual codebase.

**Rejected because — four revisions, four independent red-team rounds, a genuine structural gap
found every single time, not a diminishing-severity pattern:**

- **Round 1/2 (fork/engage lifecycle):** the fork/engage story was found broken, fixed, then found
  broken again in a different way on the next pass (`mandatory-session-worktree-design.md` §12).
- **Round 3:** confirmed the Revision-3 fixes were honestly stated (no overclaim), but did not
  confirm they were sufficient.
- **Round 4, the load-bearing one — three independent, structural problems, none fixable by editing
  the same design further:**
  1. **No identity field on `CapabilitySet` grants at all.** `find_fs_read`/`find_fs_write`/
     `contains` check grant SHAPE only, never WHO holds it. The proposed `abandon_branch()`
     "authorized by whoever could have spawned the branch" is, in the REAL `CapabilitySet` type, "any
     caller holding an equivalent `FsRead`/`FsWrite` grant on the same `mount_id`" — a sibling agent,
     or the branch's own child, can permanently destroy a DIFFERENT principal's still-useful branch.
     This is I2-shaped and structural: no amount of additional design prose closes it without adding
     an identity field `CapabilitySet` was never built to carry.
  2. **A closed-variant extension tax.** `Capability` is `std::variant<cap::FsRead, cap::FsWrite,
     ...>` with `capability_kind_of()`/`subsumes_payload()` exhaustive over every alternative (a
     `static_assert(sizeof(T)==0)` fallback hard-fails to compile for anything unhandled). Adding
     `cap::SandboxReset` means touching the variant, the kind-switch, AND the subsumes overload set —
     and even then, `tool_pipeline.hpp`'s capability-ceiling binding is AND, not OR, so a host would
     still need to grant ordinary `FsRead`/`FsWrite` just to unlock a tool the design claims is "no
     longer" gated by them.
  3. **A sync/async mismatch and an ownership-shape conflict**, both in the proposed `SpawnCostBudget`
     reuse: `consume()` is a real coroutine, `fork_from()` is a plain synchronous method with no safe
     driving idiom for it; and `SpawnPump`'s real constructor takes ONE shared `SpawnCostBudget&`
     instance, not "the session's own," so the design asks one mechanism to serve two ownership
     shapes it was never built to reconcile.

  **Verdict, unchallenged since**: every mechanism the fourth revision proposed to close the third
  round's findings had its own new, real, independently-found gap — not restatements of old
  problems. A fifth revision was not attempted; the document was frozen as historical record instead.

### Design B (accepted — this document) — clean-slate `Grant<T>`/`IdentityAuthority`/`Ledger`/`SandboxSession`

`docs/planning/identity-native-sandbox-worktree-design.md`, §0's own words: *"does not carry any of
that lineage forward and does not cite what it replaces — the design brief is to build the right
thing on its own merits, first-principles, at whatever size that takes."* Every authority object
(`Grant<Payload>`, every `AsyncQuota<T>` share, every `Ledger` ACL entry) is scoped to a real
`Principal`, minted and tracked by a single `IdentityAuthority` with a durable, multi-hop ancestry
table (`is_ancestor_of()`) — closing Design A's round-4 identity gap structurally, by construction,
not by convention. `Grant<Payload>` is a template, not a closed variant — a new authority kind needs
no exhaustive-switch edit. One `AsyncQuota<T>` (a single, coroutine-native primitive, instantiated
three times: `BranchCost`, `StorageBytes`, a `SpawnDepth`-shaped kind) replaces three separate ad hoc
budgets each with their own sync/async story — the exact class of mismatch Design A's round 4 found.
`Ledger<Store>` reuses ONLY `WorktreeObjectStore`'s pure content-addressing (§23's own explicit
note — no capability-system entanglement in that primitive at all), never `AppendLogStore` or
`CapabilitySet`.

**Steelman.** Structurally closes every one of Design A's four independently-found gaps rather than
patching around them, at the acknowledged cost of being a second, parallel authority model
(`Grant<T>`/`IdentityAuthority`) alongside the real, shipped `CapabilitySet` — a real, disclosed
cost, not hidden (see §7/§8). Backed by an unusually deep prove phase: every primitive implemented
and run as real, compiler-verified C++23 (not pseudocode), a real adversarial internal-attack
simulation, a real Docker OS-level-isolation integration, and **two independent, adversarial code-
review passes of the prove-phase code itself** (§32, §35) that each found and fixed real bugs
(§32: two Ledger implementations silently diverging, so the "full stack" demo and the "attacks are
fixed" demo were claims about two different artifacts; §35: 10 findings including a spender-identity
check that had been silently no-op'd, a real path-traversal gap on rollback, a merge-branch leak, a
quota-exhaustion DoS, and a shell-injection vulnerability in the Docker probe — all fixed, all
re-verified by recompiling and re-running the actual affected probes).

## 3. Falsifiable claims (Design B) — the load-bearing subset; the full list is §0–§35 of the design record

- **Non-copyability/move-only properties are compiler-enforced, not documented.** `IdentityAuthority`
  is non-copyable/non-movable (deleted special members); `BranchHandle` is move-only with RAII
  abandon-on-drop. *Disproof: any of these types compiles when copied, or a moved-from handle's
  destructor fails to queue an abandon.* — **CORRECT**, proven by dedicated negative
  (must-fail-to-compile) probes, §20.
- **`Grant<T>`/authority minting fails closed against an unminted principal.** `AsyncQuota::mint_root`
  and `mint_grant` both reject a `Principal` the calling `IdentityAuthority` never actually minted.
  *Disproof: a caller mints its own `Principal`-shaped value and successfully obtains a quota/grant
  without ever having been minted by the real authority.* — **CORRECT**, §20–§21; this document's own
  §17.2 records this as a fix for a round-3 bypass in an earlier revision.
- **`AsyncQuota<T>::try_consume()` is identity-scoped — a spender must be the quota's owner or a
  principal it explicitly split a share to.** *Disproof: a `Principal` with no relationship to a
  quota successfully consumes from it.* — **CORRECT, after a real regression found and fixed this
  pass** (§35 finding 1): the check had been silently discarded (`(void)spender;`); fixed, and the
  fix's own probe-suite fallout (two other probes had been unknowingly relying on the missing check)
  was found and fixed in the same pass, not left for a later one.
- **`Ledger`'s content-addressed ACL is identity-scoped and fails closed on cross-session read.**
  *Disproof: a principal holding a known digest but no ACL relationship to it reads the content
  anyway.* — **CORRECT**, §29 (a real internal-attack simulation against the running stack, not a
  synthetic test), re-confirmed live in §35.3's re-verification.
- **`merge()`/`branch_from()` require possession of a real `BranchHandle`, never a bare name.**
  *Disproof: a caller merges into or abandons a branch it never created or received a handle for, by
  name alone.* — **CORRECT, after a real found-and-fixed vulnerability** (§34.7/B1): the original
  `merge()` took a bare `std::string const& parent_name`; an attacker possessing only their own,
  unrelated branch could merge it into a victim's freshly-created (still-empty) branch by guessing
  the deterministic name scheme. Fixed by requiring the parent's own `BranchHandle`.
- **A crashed/exited process's branches are recoverable, never silently lost, but never
  auto-reclaimed either.** *Disproof: a branch orphaned by a real process exit either vanishes
  permanently or is silently reclaimed with no explicit host decision.* — **CORRECT**, §34.6/A7, and
  independently re-confirmed this pass (§35) for the merge-rejection path specifically, which had its
  own real leak (finding 3) closed by reusing the same orphan-reclaim mechanism.
- **Durable identity never re-issues an already-live id across a real process restart.** *Disproof: a
  genuinely separate OS process restart causes two different real principals to receive the same
  internal id.* — **CORRECT**, §33/§34.2 (A1), the fix for a real, twice-reproduced leak; re-verified
  this pass after a further durability defect (§35 finding 6: non-atomic persistence) was found and
  fixed.
- **The design composes with a real, kernel-enforced OS isolation boundary its own in-process
  primitives cannot provide.** *Disproof: a real Docker container bridged into this stack fails to
  contain a genuinely unreachable host secret.* — **CORRECT**, §31, confirmed twice: once during the
  original prove phase and again this pass after a real shell-injection vulnerability in the bridging
  probe itself (§35 finding 9) was found and fixed, re-verified against a live Docker daemon.

## 4. The red-team attack

Design A: four independent rounds across four revisions, §2 above — each found a genuine structural
gap, the fourth fatal to the whole approach.

Design B: at minimum five independent adversarial passes, spread across this document's own history,
each finding something real, not a formality:

1. **Per-revision red-team rounds during the original design phase** (§0–§19 of the design record) —
   found and fixed the historical issues this document's own early sections narrate (a round-3
   compile-error/round-4-fix-introduces-new-compile-error cycle typical of this project's own
   discipline).
2. **§32 — an independent adversarial code review of the prove phase itself**, prompted by an
   explicit instruction to stop expanding scope and review what already existed. Found the central,
   previously-undisclosed defect of the whole prove phase: two different `Ledger` implementations
   existed in the tree, silently diverging, meaning the "full stack is safe" demo and the "attacks
   are fixed" demo were claims about two non-overlapping artifacts. Fixed by real unification, not a
   documentation patch — re-verified with a real capstone attack check added to the composed
   artifact itself.
3. **§33 — a fifth red-team round, post-prove-phase**, specifically hunting for identity-durability
   vs. durable-ACL incompatibility. Found and reproduced (twice, in genuinely separate OS processes)
   a real cross-restart id-recycling leak.
4. **§34.7/B1 — the first-ever adversarial pass against `branch_from()`/`merge()`**, the newest
   surface in the document at the time. Found and fixed the merge-by-name vulnerability described in
   §2 above.
5. **§35 — a second, independent code-review pass of the entire prove phase**, after all of §20–§34's
   fixes had landed. Found 10 more real defects (§3 above lists the most severe); all fixed, all
   re-verified by recompiling and re-running the actual affected probes (28 positive + 5 negative,
   zero regressions), not merely re-read.

**The pattern across all five, stated as a residual, not smoothed over:** every single independent
pass found something real. None came back clean on a first attempt. §35.4's own honest closer:
*"the honest expectation is that a fifth pass would likely find an eleventh."*

## 5. Executed evidence

The complete prove phase, §20–§35 of the design record: every primitive implemented as real,
standalone C++23 under `docs/planning/proofs/`, compiled with `clang 22.1.5`,
`-std=c++23`, target `x86_64-pc-windows-msvc`, real `agentengine::compute_digest` (SHA-256 via
Windows CNG/BCrypt, `src/core/worktree_digest.cpp` linked as a real second translation unit) — not a
placeholder digest scheme. Includes: real multi-thread contention runs (10 threads × 2000 iterations
and larger), real multi-process restart proofs (a genuinely separate OS process reopening a durable
directory the first process wrote, for identity, for the ledger, and for crash-orphan reclaim,
three separate two-process pairs), a real adversarial internal-attack simulation against the live
composed stack (§29, five distinct attacks, each with a stated outcome — blocked, structurally
blocked, or an honestly disclosed residual), and a real Docker container integration (§31) proving
composition with genuine OS-level isolation.

**This pass's own re-verification** (§35.3): after fixing all 10 newly-found defects, every touched
probe was recompiled and re-run to completion, including a real, live Docker daemon run (not
compile-only) confirming both the probe's own end-to-end behavior and a dedicated adversarial check
that the shell-injection fix actually blocks a real breakout attempt (a crafted image name and
`exec()` command, each verified to leave no trace on the host filesystem). Zero regressions across
the full suite (28 positive + 5 negative-must-fail-to-compile probes).

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| Design A: identity-scoped authorization on `CapabilitySet` | **WRONG** | Round 4, `mandatory-session-worktree-design.md` §16 — no identity field exists on the real type. |
| Design A: `SandboxReset` fits the closed `Capability` variant cleanly | **WRONG** | Round 4 — exhaustive-switch touch required, AND-composition defeats the stated goal even after. |
| Design A: `SpawnCostBudget` reuse fits `fork_from()`'s sync call shape | **WRONG** | Round 4 — real sync/async and ownership-shape mismatches, no safe driving idiom. |
| Design B: non-copyable/move-only types are compiler-enforced | **CORRECT** | §20, negative compile probes. |
| Design B: minting fails closed against an unminted principal | **CORRECT** | §20–§21. |
| Design B: `try_consume()` is identity-scoped | **CORRECT, after a real regression found and fixed this pass** | §35 finding 1, re-verified via the full probe suite. |
| Design B: `Ledger` ACL fails closed cross-session | **CORRECT** | §29, re-confirmed §35.3. |
| Design B: `merge()`/`branch_from()` require real possession | **CORRECT, after a real found-and-fixed vulnerability** | §34.7/B1. |
| Design B: crashed branches are recoverable, never auto-reclaimed | **CORRECT** | §34.6/A7, §35 finding 3 (a second real leak on the merge-rejection path, fixed and proven reclaimable). |
| Design B: durable identity never re-issues a live id across a restart | **CORRECT** | §33/§34.2, §35 finding 6 (non-atomic persistence, fixed). |
| Design B: composes with real OS-level isolation | **CORRECT** | §31, §35 finding 9 (shell injection in the bridging probe, fixed, re-verified live). |

## 7. The decision

**Design B is accepted as the identity/authorization/checkpoint-durability model** for the mandatory
per-session sandbox + checkpoint-ledger requirement (`008-Sandbox-and-Isolation.md`). This ADR
**authorizes the design**, not an implementation merge — no file under `include/agentengine/` or
`src/` changes as a result of this ADR by itself.

**Binds, once a future implementation ADR (or ADRs) actually wires this in:**
- `Grant<Payload>`/`IdentityAuthority`/`Principal` as the authority model for whatever this design
  eventually authorizes — extensible-by-template, identity-scoped, durable, per §20/§33/§34.2.
- One `AsyncQuota<T>` primitive, instantiated per budget kind, coroutine-native throughout, with the
  real spender-identity check (§35 finding 1) and refund-on-failure (§35 finding 4) both load-bearing
  parts of its contract, not optional hardening.
- `Ledger<Store>` templated on its object store (default in-memory, `FileWorktreeObjectStore` for
  durability) as the ONE checkpoint/branch/merge implementation — never a second, parallel Ledger
  type, per §32's own central lesson.

**Explicitly out of scope — named, not silently dropped, each its own future design → red-team →
prove → judge cycle:**

- **A3 — the concrete execution-surface technology.** This document's own "single largest remaining
  engineering unknown" (§11/§34.10). **Update, §36 of the design record**: a fresh `ExecutionSurface`
  concept + a real `DockerExecutionSurface` conformer + `SandboxRuntime::run()` (composing
  `Ledger`+`RealIoFileSystem`+the surface into one real verb) have now been built and proven live
  against a real Docker daemon, through three rounds of independent adversarial review that found and
  fixed real bugs at every round (a quota-bypass letting a caller run commands for free, container
  move-semantics leaks, a reintroduced ACL-batch-persist bug) before converging clean. This is
  deliberately NOT built as a conforming `SandboxBackend` (`sandbox.hpp`, 008 §2a) or wired to any of
  the four real backends — per explicit project-owner direction, designed fresh on this design's own
  primitives rather than around reuse. **Update, §36.5 (2026-08-27)**: the "does the three-verb shape
  generalize past Docker" question above now has a real, fresh, OCI-standard SECOND conformer designed
  (not built — design document only, per explicit user direction) — a `ctr`/containerd-based
  `ContainerdExecutionSurface` using a bind mount instead of Docker's copy-in/copy-out, closing this
  codebase's own gap where the A3 prove-phase work never drew on `KataBackend`'s already-real,
  already-Judged-track OCI/`ctr` experience. One design-only red-team round found a real, unresolved
  ordering question (`SandboxRuntime::run()`'s fixed `materialize()`-before-`reset()` sequence against
  a bind mount whose previous container may still be live) — reasoned through as plausibly benign but
  explicitly not proven, named as the first thing a real implementation must verify empirically. No
  code, no environment provisioning, no live proof yet — full record in
  `docs/planning/oci-execution-surface-design-draft.md`. How/whether either conformer composes with
  `full_stack::SandboxSession` (a separate, pre-existing type this work did not modify) remains an A9/
  implementation-time question, not decided here.
- **A9 — real integration into `AgentSession`/`ContextProvider`/`Tool<>`.** **Update, §37 of the
  design record**: the core mechanical question — how does a mandatory per-session sandbox binding
  actually work against the REAL, unmodified `AgentSession<ChatClientT, StateT, HistoryProviderT>`
  class's own fixed `fork_from()`/`clear_in_process_state()` statements — is now designed, built, and
  proven live: `MandatorySandboxProvider`, composed as `HistoryProviderT`, went through three rounds
  of independent adversarial review (the first finding the initial design was structurally unsound —
  a shared, mutable "prepared fork" slot any incidental copy through `AgentSession::history_provider(
  )`'s real mutable accessor could silently corrupt — closed by a full redesign making every copy-
  assignment self-contained; the second finding one new gap the redesign itself introduced, a
  self-copy failure path that could wipe an already-bound session, now fixed and proven). Real,
  compiler-verified, live-Docker-tested code, not a sketch. Still real, still deferred, still
  reconciliation with THREE real, already-shipped, uncoordinated mechanisms this design's own §34.10
  named only in the abstract at the time it was written, and which have since become concrete, real
  code:**
  1. **`SandboxToolProvider`** (`ADR-096`, implemented and tested 2026-08-25) — a real `ContextProvider`
     giving Shell a per-session sandbox lifecycle, authorized via `CapabilitySet`, with **zero**
     connection to `SandboxBackendRegistry` (confirmed by that ADR's own C7) and **zero** connection
     to any checkpoint/ledger system.
  2. **`CodeActRunnerBinding`** (`ADR-030`) — Python's own, separate, bind-once-forever session-
     lifecycle mechanism, explicitly not reconciled with `SandboxToolProvider`'s shape (ADR-096 §7
     names a combined provider as real, buildable, future work — not designed there, and not designed
     here either).
  3. **`SandboxBackendRegistry`** (`ADR-080`/`ADR-098`) — real, tested backend-selection
     infrastructure with, as of both of those ADRs' own writing, **zero real production consumers**.

  **Update (`ADR-100`, 2026-08-27, per explicit project-owner direction to reconcile #1 and #3
  before any further implementation here): they were never actually uncoordinated.** `ADR-080`'s own
  Finding O already, correctly, decided neither Python nor Shell routes through
  `SandboxBackendRegistry` — both `ADR-096` and `ADR-098` already cited this. `ADR-100`'s first draft
  claimed otherwise (that Python reaches its real OS jail via the registry) and three independent
  red-team rounds found that claim false against the real code before it shipped as an ADR — recorded
  honestly in `ADR-100` §2 rather than smoothed over. The real, corrected finding: `run_shell` has
  zero OS-level containment where `execute_code` has one (via a direct `NativeJailBackend&`
  dependency, never the registry), plus a live, unmitigated wall-clock/iteration DoS gap in the
  mediated-shell evaluation loop — named as concrete future work (an immediate in-process mitigation,
  and a deferred `create_shell_worker()` mirroring `create_python_worker()`), not built by `ADR-100`
  itself. Relationship #2 below (`CodeActRunnerBinding` vs. `SandboxToolProvider`) and this design's
  own eventual `Grant<T>`/`Ledger` integration remain untouched by that reconciliation.

  This design, if implemented naively without ever being aware of them, would become a **fourth**
  disconnected mechanism in the same space, not a unification of the first three. **Per explicit
  project-owner direction, this is not a design-time obligation**: A3/A9's own future design →
  red-team → prove passes build `SandboxSession`'s real execution-surface and engine-integration
  shape fresh, on this design's own primitives, without designing around reuse of
  `SandboxToolProvider`/`CapabilitySet`/`CodeActRunnerBinding` — matching this whole document's own
  standing rule that reuse-of-existing-machinery is an *implementation-time* question, decided once
  A3/A9 are ready to actually be wired into the live engine, never a constraint on the design/prove
  work itself. The three existing mechanisms are named here so the eventual implementer knows what
  is already occupying this space and can make an informed reuse-or-replace call then — not so that
  a future design pass feels obligated to design a compatibility layer now.
- **The confused-deputy residual (§29.4)** — host code itself tricked by tool/model output into
  targeting the wrong real object via a privileged call — is explicitly inherited by A9, not solved
  by anything built so far. An I3-shaped concern for whatever integration layer eventually drives real
  `abandon()`/`merge()`/`reset_to()` calls.
- **§30's disclosed disconnect** stands unresolved: today's real Shell and Python production code
  paths are not wired to any worktree/checkpoint system, old or new. A9 must decide whether closing
  this is in scope for that future work or remains a named, accepted gap.

## 8. Residual risks

- **This design has never been Judged, and — separately — neither has `ADR-096` nor `ADR-098`,
  the two most relevant already-shipped pieces it will eventually need to reconcile with.** There
  are, as of this ADR, **two independent, unjudged, security-critical sandbox-lifecycle designs**
  sitting in this repository occupying overlapping conceptual territory, built on incompatible
  authority models, with no ADR yet reconciling them. Whoever judges this ADR should weigh that
  explicitly — accepting Design B here does not retroactively make `SandboxToolProvider`'s
  `CapabilitySet`-based approach wrong, and does not by itself decide which one (or what combination)
  production code should actually use going forward.
- **A8's bound (`kMaxAclRootsPerDigest = 64`)**: originally disclosed here as merely "not a tuned
  production value" (§29.6/§34) — **update, §40.1 (2026-08-27)**: that framing understated the real
  failure mode. Tracing every ACL-insertion call site against A10's own real calling pattern found the
  cap is a PERMANENT, non-evictable ceiling with no escape hatch — once 64 distinct, non-descendant
  principals ever touch one digest (the realistic driver: many sessions forking from a common,
  differently-owned shared base), the 65th legitimate session is denied forever. Closed for real, not
  just re-disclosed: the cap is now a per-instance constructor parameter, and `mark_digest_shared()`
  gives an already-authorized principal a real, permanently-ratcheted escape hatch that also closes
  the underlying ACL-growth vector, not just the denial. Independently red-teamed (security/I2-I3/
  concurrency), zero fatal findings; one dormant, disclosed residual (`mark_digest_shared()`'s
  `Digest` parameter lacks `HostSandboxSelection`-style structural I3 defense-in-depth — moot today,
  no production caller exists). Real usage data for what the DEFAULT should be is still not available.
- **Docker-as-execution-surface (§31) does NOT establish**: resource-limit enforcement
  (`--memory`/`--cpus`/`--pids-limit`), network isolation, a live bind-mount round trip (this
  environment's Docker Desktop configuration prevented testing it), or whether
  `scan_and_drain_into_tree()`'s full-directory-scan drain performs adequately at real working-tree
  scale. None of these block the A3 decision, but none should be assumed solved either.
- **Four independent review/red-team passes beyond the original per-revision rounds (§32, §33,
  §34.7/B1, §35) each found a new, real, previously-undisclosed defect — none came back clean on a
  first attempt.** The honest expectation, stated plainly in the design record itself (§35.4), is
  that a further pass would likely find more. Implementation planning should budget for continued
  adversarial review as a standing cost of this design, not a one-time gate already cleared.
- **§35's own re-verification found that fixing one primitive (the `AsyncQuota` spender-identity
  check) silently broke two other, previously-passing probes** that had unknowingly depended on the
  bug — a real, concrete instance of this document's own repeatedly-observed "independently-plausible
  pieces quietly failing to converge" risk (the original motivating concern for the entire 9-step
  closure pass that produced §34). Any future change to a shared primitive in this stack should
  re-run the FULL probe suite, not just the probes it appears to touch, as a matter of course.
- **A10 (§39, 2026-08-27)**: a real-world-use-case research pass (`docs/research/2026-08-27-real-
  world-agent-use-case-coverage.md`, separate from §38's attack-focused external validation) found
  this design's central gap wasn't a security defect but a missing tool-facing surface: every
  actively-developed coding agent surveyed ships git-worktree-per-task isolation as its primary
  mechanism, and this design had the underlying primitives proven but zero agent-callable path to
  them. `TaskBranchSandbox` (start/run/commit/discard, `docs/planning/proofs/task_branch_tool/`)
  closes this at the same standalone-prove-phase bar as everything else in this document — three
  independent red-team rounds, one fatal finding (an unsynchronized handle table) corroborated by
  all three, all fixed and re-proven (13/13 checks against live Docker). **Update, §40.2 (2026-08-27)**:
  the one usability gap that round left disclosed-not-fixed — a rejected commit's real work reachable
  only via the lower-level A7 API, not through this tool's own handle — is now closed for real, using
  that same A7 `reclaim_orphaned_branch()` API: `commit_task_branch()` reclaims and re-surfaces the
  branch under its original `handle_id` on any merge rejection (14/14 checks). Independently
  red-teamed (correctness/concurrency), zero fatal findings. Still governed by every residual above:
  no production wiring, no capability-decl design, not Judged.
