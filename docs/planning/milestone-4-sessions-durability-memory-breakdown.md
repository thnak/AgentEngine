# Milestone 4 — Sessions, durability, memory — work breakdown and kick-off

**Status:** Work breakdown (stage 4 of [the review-signoff workflow](v1-review-signoff-workflow.md)),
written just-in-time as this milestone starts, per that doc's §4. Scoped to
[the roadmap's](v1-implementation-roadmap.md) Milestone 4 exit criterion: *"a session survives
process restart with an identical resumed run (001 §9 G2 / 019 §7 G1), `Suspended` holds zero
resources (019 §7 G3), and the default (non-vector) memory retrieval path is deterministic and
replayable (029 §9 G1)."*

**RFCs:** 005 (Sessions, State and Memory), 019 (Durability and Long-Running Agents), 029 (Memory
System). All three Reviewed (2026-08-05). 001 (Execution Model) is not itself in scope for a fresh
review pass — it's already Reviewed and M1 built its walking-skeleton slice of it — but two of its
own gaps (real `session_id` actor-keying, real `Run`/`Turn`/`Interaction` identity) turn out to be
prerequisites 019 needs and M1 explicitly deferred to "the milestone that owns them" (see decision 2
below); closing them here is finishing 001's own already-reviewed text, not reopening design.

Build order follows the roadmap's own dependency chain: 005 (the session/state/context model) first
— everything else sits on it — then 019 (durability, which checkpoints *through* 005's persistence
seam) on top, then 029 (memory, which attaches through 005's `ContextProvider` seam and reuses 025's
worktree, both already real) last.

## Current state (verified 2026-08-06, after M3)

| Item | State |
|---|---|
| `AgentSession<ChatClientT>` (`core/agent_session.hpp`) | Real **Quark actor** (`quark::Actor<AgentSession<ChatClientT>, quark::Sequential>`), real `handle(Ask<StartRun, AgentResponse>)` turn loop (append input, call `ChatClientT::chat()`, append response, reply) — proven end-to-end by `tests/test_m1_walking_skeleton.cpp` via `quark::TestKit`. **`session_id_` is declared but never assigned or derived into a Quark `ActorId` anywhere** — the "one Quark actor instance, key = `session_id`" claim (001 §1, restated in this header's own top comment) is not actually wired. No `state`/`metadata` fields (005 §1) exist on the type at all |
| `Run`/`Turn`/`Interaction` (001 §1-§2) | **Prose only.** No C++ type exists under `include/` for any of the three — `StartRun`/`AgentResponse` (a message pair) is the only concrete artifact. 001 §9 G1/G2 (10⁴ concurrent sessions; checkpoint/restart with byte-identical resume) have zero test coverage — `test_m1_walking_skeleton.cpp`'s own top comment states plainly these gates "stay with Milestone 4" |
| Quark's `Store`/`EventLog` seam (`persistence.hpp`) | Real and already reused directly by this project: `Store` concept (fencing via `FenceToken`/`acquire_fence`, strict-seq-monotonicity), `InMemoryStore` (reference adapter), `FileStore` (crash-durable, std-only, shipped+verified per Quark's own README), `EventLog::stage/commit`, `recover_event_sourced`. `core/worktree.hpp`'s `Ref`/`commit_ref`/`commit_turn` (M3 Phase A2/D1) already ride this exact seam — session persistence (005 §2) and checkpoint identity (019 §1) have a proven precedent to copy, not a design question to answer fresh |
| `ContextProvider` (`core/context_provider.hpp`) | **Vocabulary only** — the concept (`on_context`/`on_turn_end`) exists and matches 005 §5's shape, but `SessionContext` is forward-declared with "not yet modeled," `ContextContribution.tools` is commented out pending `ToolDecl` (which now exists for real, 006, since M2 — this elision is stale), no concrete conformer exists, nothing calls it, no test references it |
| Quark's durable reminders (`reminder_service.hpp`, ADR-017) | Real, Accepted, proven at scale: 10⁶ simultaneous-due reminders flatten to `peak == N/spread`, 42 crash/SIGKILL trials with zero loss and idempotent re-fire. Named residual: physical cross-node handoff of a durable row is unproven on real hardware. This is 019 §2's "Timer/schedule" wake condition — pure reuse, no new mechanism needed |
| Quark's placement/fencing (010-Distribution.md) and passivation (ADR-028/034) | Real. Fencing on node-loss split-brain is explicit and proven ("the store accepts only the higher fencing token at commit, so the zombie activation's commit is rejected") — backed by the same `Store::acquire_fence` sessions will use. Passivation (`ActorRef<A>::passivate()` + idle-timeout eviction) is Accepted. Caveat: physically moving a session's durable state to a new owner node on re-placement is not proven on real hardware (same gap as the reminder service's residual) |
| Worktree owner-string parameterization (`core/worktree.hpp`) | Confirmed generic already — `Ref{name, tree_digest}`'s `name` is a plain string with the file's own comment giving `"session:s-42"` and `"principal:p-7"` as interchangeable examples; `ref_actor_id()` hashes whatever string is passed with no session-specific branching anywhere. 029 §2's "identical model, scoped one level up" needs zero worktree rework — just a new caller using a different prefix |
| `agentengine::Principal` (`trust/principal.hpp`) | Real, distinct from `quark::Principal` (a capability bitset — unrelated type, same name by coincidence). A bare `{id, tenant_id}` identity struct, wired into `EffectContext`, `AgentSession`, `MemoryOrigin` already. No registry, no principal→session/worktree ownership lookup, no enforcement beyond attribution — that layer doesn't exist yet |
| `MemoryItem`/`MemoryOrigin`/`memory_kind`/`memory_source` (`core/memory.hpp`) | Vocabulary only, matches 029 §3 exactly, compile-checked in `smoke_vocabulary.cpp`. No storage-as-worktree-blob wiring, no writer, no reader, no retrieval ranking |
| `Backgroundable`/`StandingEffect`/`background_task` (006 §6b) | **Does not exist anywhere.** 019 §2's "Local background task completion" wake condition depends on this, and it was never built in M2 either — M2's own scope was narrower than 006's full text, same pattern as every milestone's own deferred residuals |
| `InputRequired`/`Interaction`/`interaction_id` (001 §2) | **Does not exist anywhere.** 019 §2's "Human/caller input" wake condition depends on this |
| Quark's transactional outbox (017-Delivery-Guarantees.md) | Real, **Accepted** (x86-64) per Quark's own RFC index — at-most/at-least/effectively-once delivery with partition proof. 019 §3's own text names this as the backing mechanism for exactly-once effects ("this RFC does not invent a delivery mechanism") — confirmed real, ready to reuse |
| 004 (`ChatClient` Plane) real providers | Not built — M5's job. Every session/memory-extraction test in M4, like every test in M1-M3, runs against a mock `ChatClient` (the established project-wide precedent, not a new exception) |

## Design decisions made while breaking this down

1. **`AgentSession`'s `session_id_` gets a real `session_actor_id()` helper, mirroring
   `core/worktree.hpp`'s `ref_actor_id()` exactly** — same bridge (`quark::ActorId{durable_type_key,
   hash(name)}`) for the same reason (Quark's `ActorId` key is a `uint64_t`, not a string). No new
   design question here; it's applying an already-proven pattern from M3 to a type M1 left
   unwired.
2. **`Run`/`Turn`/`Interaction` become real C++ types under this milestone, not a reopening of
   001's own review.** 001 §1/§2 already specify their shape in prose; M1's own breakdown doc
   explicitly named this gap and assigned it here ("001 §9 G1's 10⁴-session gate... need RFCs not
   yet in scope... stay with the milestones that own them" — this is that milestone). Building them
   is implementing already-Reviewed text, the same relationship M3's Phase E had to 010's
   already-Reviewed `Runner` concept.
3. **Checkpoint boundaries, this milestone, are turn boundaries only (001 §3) — workflow superstep
   boundaries (014 §5) are out of scope and explicitly deferred to M6.** 019 §1 names both kinds in
   the same sentence, but 014 (Workflow and Orchestration) is M6's RFC and doesn't exist yet. This
   is the same "narrower than the RFC's own promotion bar, named not silently dropped" discipline
   M1-M3 each already used (most recently 025's own D3 deferring 019-dependent work to "M4," which
   is this milestone, one layer up).
4. **019 §2's suspension wake-condition table has six rows; only two are buildable inside M4's own
   RFC scope.** "Timer/schedule" is pure reuse of Quark's already-Accepted durable reminders.
   "Human/caller input" needs the `Interaction`/`InputRequired` type from decision 2, which this
   milestone builds anyway. The other four each need an RFC this milestone doesn't own:
   "External event" and "Remote task completion" need 012 (A2A, M7); "Local background task
   completion" needs `Backgroundable`/`StandingEffect` (006 §6b), which — confirmed by direct
   inspection — was never built even though 006 itself has been real since M2 (M2's own scope was
   narrower than 006's full text, the same pattern as every prior milestone's residuals). Named as a
   real, not hypothetical, gap; deferred to whichever milestone builds its owning RFC.
5. **Exactly-once effects (019 §3) ride Quark's `017-Delivery-Guarantees.md` transactional outbox
   directly, per 019's own text** — confirmed real and Accepted, not merely asserted available.
   AgentEngine builds the idempotency-key derivation and the intent/outcome journaling calls against
   that seam; it does not build a delivery mechanism of its own, matching 005/025's own "no second
   storage engine" discipline extended to messaging.
6. **`ContextContribution.tools` gets un-elided.** `context_provider.hpp`'s comment says it's
   pending "006's `ToolDecl`" — 006 has been real since M2, so this is a stale placeholder, not a
   design decision to make. Fixing it is a precondition for Phase B's `ContextProvider` conformers
   (memory's `recall` tool, 029 §5) to be buildable at all, not new architecture.
7. **029 (Memory System) is built last (Phase G), after 005's `ContextProvider` is real (Phase B) —
   not in parallel.** 029 §2/§4/§5 all attach through `ContextProvider` and the worktree; building
   memory before that seam is real would mean building against vocabulary that might still change
   shape, the same ordering risk M3's own roadmap language explicitly avoided for worktree-then-
   interpreter-then-`agent.*`.
8. **029 §4's extraction ("a memory-writing provider's `on_turn_end` may call a declared
   `ChatClient` to extract candidate `MemoryItem`s") is tested against a mock `ChatClient`, not
   deferred to M5.** This is not a new exception — it is the same precedent every session/turn-loop
   test in this project already uses (004's real providers are M5's job regardless of what calls
   `ChatClient::chat()`); a mock extraction response is exactly as valid a fixture as
   `test_m1_walking_skeleton.cpp`'s mock turn response already is.

## Tasks, in dependency order

### Phase A — Session core made real (005 §1, closing 001 §1's wiring gap)

- **A1.** `session_actor_id()` (decision 1) + real per-session isolation. A test with N sessions
  (machine-safe bounded count, per CLAUDE.md's machine-safety section — not the real 10⁴, named as
  001 §9 G1 "in miniature" the same way M1's own walking-skeleton test named itself) proves each
  session's turn history is invisible to and unaffected by every other session's — the real-identity
  version of the isolation claim `test_m1_walking_skeleton.cpp` never actually tested (it drove one
  `AgentSession` via `TestKit`, never two).
- **A2.** `state` (typed per-agent, 005 §8 Q1's resolution) and `metadata` fields on `AgentSession`.
- **A3.** Real `Run`/`Turn` identity (decision 2) — `run_id`, `turn_index` threaded through
  `EffectContext`/`AgentSession`, the identity Phase F's idempotency keys (019 §3) and Phase D's
  checkpoint records (019 §1) both need.
- **A4.** Session persistence via `Store` (005 §2) — Snapshot mode first (the simpler shape),
  Event-sourced mode second, reusing the worktree's `Ref`/`EventLog` pattern directly (M3 decision 1
  applies unchanged: a session's durable pointer is exactly the shape `Store` already fits).

### Phase B — Context assembly, `ContextProvider`, compaction (005 §3-§5)

- **B1.** Un-elide `ContextContribution.tools` (decision 6); model `SessionContext` for real.
- **B2.** `HistoryProvider` — the first real `ContextProvider` conformer, replacing `AgentSession`'s
  current "the full history, trivially" shortcut (M1's own named scope limit) with a real windowed
  assembly path.
- **B3.** Per-contributor token budgets + deterministic drop order, drops recorded in the trace
  (005 §3's stated rules).
- **B4.** Compaction: `Window<N>` first (the exact-replay case 005 §9 G3 names explicitly),
  `Summarize<N>` second (bounded-divergence case, also named in G3). `Salience`/`Hierarchical` are
  not named by any gate text and are candidates to defer if this phase runs long (see deferred list).

### Phase C — Fork, redact, delete (005 §6)

- **C1.** Fork — copy-on-write new `session_id` from a history prefix, reusing A1/A4 directly.
- **C2.** Redact — in-place tombstone with reason/actor, propagated to checkpoints (needs Phase D).
- **C3.** Delete — hard removal of session state and derived artifacts, with a completion receipt
  (005 G4's own falsifiable claim: "a search over the store finds no residue").

### Phase D — Checkpoints (019 §1, turn-boundary only per decision 3)

- **D1.** Checkpoint content shape: run position, session delta, capability set recorded as
  *references* (never live handles — 019 §1's explicit anti-forgery rule), pending
  approvals/input-requests.
- **D2.** Checkpoint cadence policy (incremental deltas + periodic full checkpoint).

### Phase E — Suspension and recovery (019 §2/§4, scoped per decision 4)

- **E1.** `Interaction`/`InputRequired` real type (decision 2/4) — the "Human/caller input" wake row.
- **E2.** `Suspended` holds zero resources — no activation, no sandbox, no connection, no thread.
  This is one of the roadmap's own three milestone-defining claims (019 §7 G3); proven by census,
  not asserted, per that gate's own wording.
- **E3.** Timer/schedule wake via Quark's durable reminders — reuse, not new design (decision 4).
- **E4.** Recovery: process restart (the other milestone-defining claim, 001 §9 G2 / 019 §7 G1);
  node-loss fencing (reuse of Quark's already-proven placement fencing); poison-run quarantine after
  a bounded retry count with state preserved (019 §7 G5).

### Phase F — Exactly-once effects (019 §3, decision 5)

- **F1.** Idempotency key derivation `{run_id, turn_index, call_index, argument_digest}`, wired into
  the existing (real, M2) `tool_pipeline`.
- **F2.** Effect journaling (intent journaled before execution, outcome after) atop Quark's 017
  outbox.
- **F3.** `at-most-once` ambiguity surfaces as *indeterminate*, never guessed (019 §7 G6) — the
  gate's own 10⁴-trial fault-injection bar is the strictest correctness claim in this milestone;
  flagged as a candidate for the design→red-team→prove→judge/ADR track given CLAUDE.md's own
  "contested, hot-path, or security-critical" bar (getting this wrong is literally "a payment gets
  made twice," 019 §3's own example) — recommendation, not yet confirmed with the project owner.
- **F4.** 019 §6's rewind-then-reexecute rule (`at-most-once` requires explicit operator
  acknowledgement) — the *rule* is provable now against a manually-triggered turn re-execution; full
  integration with workflow rewind (014 §5) waits for M6, named explicitly, same as decision 3.

### Phase G — Memory system (029, decision 7/8)

- **G1.** Memory worktree per principal (`"principal:p-7"` `Ref`) — zero new worktree code, a new
  caller using the owner-string genericity M3 already built.
- **G2.** `MemoryItem` storage as blobs under a `{kind, id}` path (029 §3).
- **G3.** Writing memory: the `agent.notes` path (026, real since M3) for agent-authored writes, and
  a `ContextProvider.on_turn_end` extraction hook against a mock `ChatClient` (decision 8) for
  host-side extraction.
- **G4.** `recall(query)` tool contributed via `ContextContribution.tools` (needs B1).
- **G5.** Cross-principal isolation proof (029 §9 G5) and no-authority-laundering hostile-corpus
  check (029 §9 G3) — both reuse already-Judged mechanisms (capability enforcement, ADR-009; taint,
  003 §2) rather than needing a fresh ADR, the same "ordinary task over Judged primitives" relationship
  M3's mount-capability work (Phase C1) had to ADR-009.
- **Vector-based `ae:memory` plugin** (029 §5/§9 G6's upgrade path) is **not** built this phase —
  the roadmap's own M4 exit criterion only requires the default, non-vector retrieval path; the
  vector upgrade is a 009 WASM plugin (real infrastructure since M2) that can land in any later
  milestone once an operator actually wants it, named explicitly in the deferred list below rather
  than silently assumed in scope.

### Phase H — The milestone's central falsifiable claims (roadmap exit criterion)

Mirrors M3's own Phase H pattern: a closing phase that proves the roadmap's exit-criterion claims
directly, even though most of the supporting mechanics were already built and tested in Phases A-G.

- **H1.** A session survives process restart with an identical resumed run (001 §9 G2 / 019 §7 G1) —
  kill `-9` at a checkpoint boundary, restart, resume, byte-identical output vs. the uninterrupted
  control.
- **H2.** `Suspended` holds zero resources (019 §7 G3) — the census-based proof from E2, exercised
  end-to-end through a real suspend/resume cycle rather than in isolation.
- **H3.** Default (non-vector) memory retrieval is deterministic and replayable (029 §9 G1) —
  byte-identical `ContextContribution` across repeated runs against a fixed memory-worktree tree
  digest and a fixed turn, with no network call occurring.

## What's explicitly deferred past M4

- **Workflow-superstep checkpoint boundaries** (019 §1's other half) — needs 014, M6's job.
- **External event / remote task completion / local background task wake conditions** (019 §2 rows
  3-5) — need 012 (M7) and a real `Backgroundable`/`StandingEffect` build-out (006 §6b, never done in
  M2 either).
- **019 §6's workflow-rewind integration** for `at-most-once` re-execution acknowledgement — needs
  014, M6's job (the underlying rule is proven narrower in Phase F4 against a turn, not a workflow).
- **Vector-based `ae:memory` plugin** (029 §5, §9 G6's upgrade-path gate) — optional; not required by
  the roadmap's own M4 exit criterion.
- **`Salience`/`Hierarchical` compaction strategies** (005 §4) beyond `Window`/`Summarize` — not
  named by 005 §9 G3's gate text specifically; lower priority, deferred if Phase B runs long.
- **10⁴/10⁶-scale gates** (001 §9 G1, 005 §9 G1, 019 §7 G4) — 023 stays `TBD-baselined`
  project-wide until M8, the same status quo every earlier milestone already established; this
  milestone builds machine-safe bounded versions of each isolation/scale proof instead, per
  CLAUDE.md's machine-safety section (same treatment M3 decision 7 gave its own scale gates).
- **Redaction reaching the memory worktree fully** (029 §9 G4) depends on 025 §6's GC policy, which
  M3's own Phase D3 already deferred to "M4" (this milestone) — if GC lands here, redaction rides it
  (C2/G5); if not, redaction proves the tombstone/delete-request path only, named narrower rather
  than silently assumed complete.
- **Cross-node physical state handoff on session re-placement** — named as an existing, unproven-on-
  real-hardware residual in both Quark's reminder-service ADR-017 and its placement/fencing design;
  this milestone's node-loss proof (E4) exercises the fencing-rejects-zombie-write half only, matching
  what's actually proven upstream today.

## Handover & kick-off

Started 2026-08-06, immediately following M3's close-out (see that milestone's breakdown doc, final
section). No deviation from the roadmap's assumed order. Phase F3's ADR-track recommendation
(exactly-once `at-most-once` handling) is a recommendation pending project-owner confirmation, not
yet decided — the one open item carried forward from this breakdown into Phase F's own kick-off.
