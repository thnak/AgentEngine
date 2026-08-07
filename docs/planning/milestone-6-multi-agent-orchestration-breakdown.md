# Milestone 6 — Multi-agent orchestration — work breakdown and kick-off

**Status:** Work breakdown (stage 4 of [the review-signoff workflow](v1-review-signoff-workflow.md)),
written just-in-time as this milestone starts, per that doc's §4. Scoped to
[the roadmap's](v1-implementation-roadmap.md) Milestone 6 exit criterion: *"each 014 §3 pattern runs
correctly under injected executor failure (014 §8 G1), and pausing one Project has zero observable
effect on N-1 others (030 §7 G1)."*

**RFCs:** 014 (Workflow and Orchestration), 030 (Project: Workspace Grouping and Directed Lifecycle).
Both Reviewed (2026-08-05). 014 depends on 001, 002, 005 (all real since M1/M4), **013 (M7)** and
**019** (M4, real). 030 depends on 001, 005, 007, **014** (this milestone), **020 (M9)**, 025 (M4,
real), and Quark 012/ADR-034 (real). This milestone therefore builds the slice of each that its
own unbuilt dependencies do not gate, and defers the rest explicitly — the same discipline M5
decision 1 applied to 018's dependency on 011/012.

Build order is forced by 030's own dependency on 014: the workflow graph and its superstep engine
first (014 §1-§2), then patterns and failure (§3, §6), then the request port (§4), then checkpoint
and time-travel (§5), then introspection (§7); only then the Project record and its directed
lifecycle (030), whose §4 pause sequence must reach *workflow-supervising actors* that do not exist
until 014 is real.

## Current state (verified 2026-08-07, after M5)

| Item | State |
|---|---|
| `include/agentengine/workflow/` | **README only, 10 lines**, explicitly "declares shape only". No headers, nothing compiled. 014 is genuinely greenfield — unlike every prior milestone, there is no stale M0 scaffolding here to reconcile against |
| Any `Project` type, `project_id`, or manifest | **Does not exist anywhere** in `include/`, `src/`, or `tests/`. 030 is greenfield too |
| `EmbeddedHost` (030 §6's named host surface) | **Does not exist.** It belongs to 020 (Configuration and Hosting), scheduled **M9**. 030 §6's "gains four calls" has no type to add them to yet — see decision 2 |
| `quark::Engine`, real, in this repo's tests | **Real precedent since M4 Phase E2.** `tests/test_agent_session_suspend_resume.cpp` is "the first AgentEngine test anywhere to stand up a REAL `quark::Engine`", mirroring Quark's own `engine_passivate_test.cpp` (locally-owned `Activation` registered via `register_activation`, deliberately not `Engine::spawn<A>()` — which returns only an `ActorId`, no mutable reference an actor's own `initialize()` could use). `test_agent_session_timer_wake.cpp` is the second. **24 test files still use `quark::TestKit`** |
| `quark::TestKit` | Real, and the wrong tool for this milestone: it **has no async carrier**. `agent_session.hpp:243-248` states the boundary precisely — "only a `ChatClientT` that itself awaits a genuine cross-actor primitive would need a real `Engine`, not `TestKit`". A workflow supervisor awaiting its executor actors is exactly that await. See decision 3 |
| `ActorRef<A>::passivate()` (030 §4's primitive) | **Real and Accepted** (`third_party/quark/decisions/ADR-034-on-demand-actor-passivation.md`; code at `actor_ref.hpp:398-415`, banner citing "ADR-028 Phase 8; 006 §passivate"). Fire-and-forget, posts the same `Deactivate` control descriptor the idle wheel posts, drains in-flight work, never a mid-handler interrupt. **Sequential-only, enforced by `static_assert(max_concurrency_of<A>() == 1)` — a COMPILE ERROR on a `Reentrant`/`MaxConcurrency<N>` actor, never a silent no-op.** This is load-bearing for 014's design — see decision 4 |
| Quark supervision (014 §6's "Supervision is Quark 007") | **Real.** `SupervisionDirective{Resume, Restart, Stop, Escalate}` and `SupervisionPolicy` (`activation.hpp:97-130`), compile-time `OnFailure<Decision, MaxRestarts<N, Within<…>>>`, an `EscalationSink` seam with an engine-side `EscalationGuard` storm limiter. Restart-with-bound is itself noted Sequential-only (`activation.hpp:127`) |
| Quark `AskFuture<R>` (the fan-out mechanism) | **Real** (`actor_ref.hpp:68-88`), move-only, `co_await`-able. There is **no `when_all`/`join`/`gather` primitive** anywhere in Quark — verified. Fan-out concurrency therefore comes from *issuing all N asks before awaiting any*, then collecting in a fixed order; see decision 5, which turns this from a limitation into 014 §8 G3's proof |
| `ae::task<T>` / `quark::task<T>` | Real for non-void `T` (ADR-047, wired M5 Phase B4a). `AgentSession::handle` is already Quark's async form and `co_await`s across suspend points — the precedent a supervising actor's own handler follows |
| `ae::stream<T>` (`core/stream.hpp`) | Real (M5 Phase B4b, ADR-017/ADR-019): credit-controlled ring, consumer-cancellation `stop_token`, genuinely incremental. This is what 014 §7's "live view of executor states, in-flight messages, and round number" would stream over |
| `Interaction` (`core/interaction.hpp`) | **Real since M4 Phase E1** — `{interaction_id, run_id, reason ∈ {input, auth}, opened_at, expires_at?}`, all-scalar, `QUARK_SERIALIZE`-able, already embedded in `AgentSessionRecord.open_interactions`. This is exactly what 014 §4's request port emits; **the type exists, no request port does**. 014 §4's "multiple concurrent `Interaction` records on the same run" (resolving OQ-4) has never been exercised — every existing use opens at most one |
| Session checkpoint/persistence (005 §2 seam) | **Real** — `save_agent_session_snapshot`/`load_agent_session_snapshot` over Quark 012's `Store` via `quark::snapshot_sequential`/`recover_snapshot`, with a tombstone-based delete. 014 §5's "backed by the same store as sessions" has a real seam to reuse, not to invent |
| `EffectJournal` + `IdempotencyKey` (`core/effect_journal.hpp`) | **Real** (M4). 014 §5's "effects are not rewound… idempotency keys (019) are the mechanism that keeps that from double-charging" has its mechanism already built — time-travel must *use* it, not add one |
| `Worktree` (`core/worktree.hpp`) + tree digests | **Real** (M3/M4), including `TreeDiff`. 030 §2's "N independent worktree refs, a diff over N tree digests, never a merge of N trees" is expressible with what exists; 025 §4's merge machinery is deliberately not reused |
| 013 (UI and Streaming Surfaces) | Reviewed, **not built, M7**. `protocol/agui/` is README-only. 014 §4's "one shape, four surfaces (013 §5)" can build the *shape* but binds no surface this milestone |
| 020 (Configuration and Hosting) | Reviewed, **not built, M9**. Owns `EmbeddedHost` (030 §6) and the host-configured N in 030 §7 G1 |
| Machine-safety envelope | Unchanged and binding: `-j4` max, tests pinned ≤ 4 cores, never `hardware_concurrency()`. 014 §8 G3 asks for 10³ seeds and 030 §7 G1 for N ≥ 100 Projects — both are *actor* counts, not thread counts, so both fit, but the shuffle test's seed loop is the one place this milestone could accidentally saturate the box. See decision 8 |

## Design decisions made while breaking this down

1. **014 is scoped to the engine half of §4; 013's four surfaces are deferred to M7.** §4's request
   port is "the same mechanism as tool approval and A2A `INPUT_REQUIRED` — one shape, four surfaces
   (013 §5)". The *shape* — emit `InputRequired`, open an `Interaction`, suspend holding no
   resources, resume on response — depends only on 001 §2 and 019, both real. The four surfaces need
   013. Building the shape now and binding surfaces in M7 is what §4's own "one shape, four surfaces"
   framing invites; building a surface now would mean inventing 013's vocabulary a milestone early.
   **What this milestone can still fully prove:** 014 §8 G5 (a suspended workflow holds no
   activation/sandbox/connection, measured, and resumes after a process restart) — it is measured by
   census against a real `Engine`, exactly as M4 Phase E2 already did for a session.

2. **030 §6's `EmbeddedHost` facade is deferred to M9; its four verbs are built as engine-level
   operations now.** No `EmbeddedHost` type exists and it belongs to 020 (M9). But §6 is explicit
   that these are not a new protocol surface — `create_project`/`list_projects` are "ordinary `ask`s
   against a small Project-registry actor", and `pause_project`/`restore_project` deliberately do
   **not** route through that registry, acting directly on the named Project's own supervising actor.
   That division is the load-bearing part (it is *why* §7 G1's "zero observable effect on the other
   N-1" holds), and it is buildable today. M9 adds a facade over it, not a redesign.

3. **This milestone runs on a real `quark::Engine`, not `TestKit`, and that is forced rather than
   preferred.** `TestKit` has no async carrier; a workflow supervisor's whole job is awaiting
   cross-actor asks. The precedent and the exact construction shape already exist
   (`test_agent_session_suspend_resume.cpp`, M4 Phase E2). Consequence to plan for: every M6 test is
   an Engine test, so they are slower and more ordering-sensitive than the 24 `TestKit` files —
   budget for that rather than discovering it in Phase B.

4. **The workflow-supervising actor MUST be `quark::Sequential`. This is forced by a constraint
   neither RFC states.** 030 §4/§8 Q4 requires `pause_project` to `.passivate()` any
   workflow-supervising actor, and 030 §7 G1 *measures* its activation count dropping to zero. But
   `ActorRef<A>::passivate()` `static_assert`s `max_concurrency_of<A>() == 1` — on a `Reentrant` or
   `MaxConcurrency<N>` supervisor the pause sequence would not merely misbehave, **it would not
   compile**, and G1 would be unprovable. Fortunately this is also the right shape on 014's own
   terms: §2's superstep model processes one round at a time, so a supervisor that handled rounds
   concurrently would be violating §2 anyway. Recorded here because the two RFCs agree only by
   coincidence, and a future author who "optimises" the supervisor to `Reentrant` needs to find this
   note before they find the compile error.

5. **Fan-out concurrency comes from issuing all asks before awaiting any — and that choice is also
   014 §8 G3's proof, not merely compatible with it.** Quark has no `when_all`/`join`/`gather`
   (verified). The supervisor therefore issues N `ask`s (each executor begins work immediately, on
   its own actor), collects N move-only `AskFuture<R>`s, then `co_await`s them **in a fixed index
   order**. Concurrency is real; the *collection* order is deterministic regardless of completion
   order. §8 G3 asks that shuffling intra-round scheduling across 10³ seeds produce identical output
   — with this shape the round's result assembly cannot depend on completion order **by
   construction**, which is a far stronger position than testing for it and hoping. G3's test then
   becomes a genuine positive control (it must still catch a deliberately order-dependent executor),
   not the only thing standing between the design and a heisenbug.

5b. **Executor actor-key choice belongs to the workflow layer, because Quark's placement does not
   spread consecutive keys — found by measurement in Phase B, not by reading.** Every executor is the
   same actor type (`FunctionExecutor`, so the graph stays data — Phase A), so placement is decided
   entirely by instance key: `hash_combine(TypeKey, key) & (shard_count - 1)`. Registering nodes
   under the obvious `1, 2, 3, …` put **keys 1/2/3/4 on shards 3/1/1/1** of a 4-shard engine. Three
   executors sharing a shard are drained by one worker in sequence, so **014 §3's Concurrent pattern
   silently degrades to Sequential while producing byte-identical output** — Phase B's own fan-out
   case measured 184 ms for three 60 ms nodes, exactly their serial sum. Nothing failed; the
   execution model was just no longer the one 014 §1 promises ("concurrency … comes from the
   runtime"). `workflow/placement.hpp`'s `spread_executor_keys` is the fix, and it lives in the
   workflow layer rather than in host wiring precisely because the naive host choice is the broken
   one. The search is **bounded** — a degenerate hash costs a worse spread, never a hang. After the
   fix the same case measures **75 ms** on keys 1/2/5/12 → shards 3/1/0/2.
   *Consequence for later phases:* 014 §8 G1 (patterns under injected failure and delay) and G3
   (10³-seed shuffle) are both meaningless if a round is accidentally serialized, so Phase J must
   assert the spread as a precondition rather than trusting it.

6. **Typed edges are checked at compile time for the C++ form; the declarative form's loader is
   deferred to M7 with 015.** 014 §1 requires an incompatible edge to fail "at compile time for the
   C++ form, at load for the declarative form (015), using the same validator (I6)". 015 is M7. This
   milestone builds the validator as a *shared, non-template-only* predicate over the graph
   description so the M7 loader calls the same code rather than a parallel reimplementation — I6's
   equivalence is a property of sharing the validator, and retrofitting that sharing later is exactly
   how the two surfaces drift. §8 G4's "in both the C++ and the declarative form" is therefore
   **half-provable this milestone**, and is recorded as such rather than claimed complete.

7. **014 §8 G6 (cross-node checkpoint consistency, ≥3 cluster nodes) is deferred, named, not
   silently dropped.** Quark has real `cluster.hpp`/`membership.hpp`/`placement.hpp`, so this is not
   impossible — but standing up a ≥3-node cluster with an injected node failure between
   checkpoint-pending and checkpoint-committed is a distinct piece of infrastructure this repo has
   never built, and the roadmap's own M6 exit criterion does not include G6. The **two-phase
   pending→committed discipline G6 tests is still built and proven single-node** in Phase F (a
   failure injected between the two phases must leave resume falling back to the prior committed
   checkpoint); what defers is the multi-node execution of it. This mirrors M5's own treatment of
   10⁴-scale gates.

8. **G3's 10³ seeds and G1's N ≥ 100 Projects are actor counts, and the seed loop is sequential.**
   Both fit the machine-safety envelope: 100 Projects are 100 lazily-declared actors, not 100
   threads, and the shuffle test runs its 10³ seeds one after another on a fixed ≤4-worker Engine.
   The failure mode to avoid is spawning an Engine per seed; the test builds one Engine and re-runs
   the graph, which is also what makes the seeds comparable.

9. **Time-travel's audit obligation is built with the rewind, not after it.** 014 §5 says "every
   rewind is audited, because rewinding a workflow that already had external effects is a correctness
   hazard the operator must own", and "effects are not rewound". `EffectJournal`/`IdempotencyKey`
   already exist (M4), so the honest implementation is available now. A rewind API that ships without
   the audit record would be the footgun §5 explicitly warns against, so the two land in the same
   phase or neither does.

## Phases

Each phase names the RFC text it implements and the check that would falsify it. Phases A-G are 014;
H-I are 030; J is the milestone's own exit-criterion proof.

- **Phase A — the graph as data (014 §1).** `Executor`/`Edge`/`Workflow` description types; the
  shared edge-type-compatibility validator (decision 6); `output_selection` and the required
  termination bound (§2: "an unbounded workflow does not run"). No execution. *Falsifiable:* a
  type-mismatched edge builds; an unbounded workflow constructs.
- **Phase B — the superstep engine (014 §2).** The `Sequential` supervising actor (decision 4),
  round-at-a-time delivery, fan-out via issue-all-then-collect (decision 5), termination by output
  selection / terminal executor / `MaxRounds` / deadline / budget. Real `Engine` (decision 3).
  *Falsifiable:* messages from round *n+1* observed by a round-*n* executor; a workflow exceeding its
  bound.
- **Phase C — the §3 patterns.** All eight rows as graph configurations, each a runnable sample:
  Sequential, Concurrent, Handoff, Group chat/debate, Planner (Magentic), Map-reduce, Router,
  Reflection/critic. *Falsifiable:* a pattern needing an engine primitive §1-§2 does not provide —
  which would contradict §3's "configurations of the graph, not separate subsystems".
  **Outcome: §3's claim holds, with two §1 primitives completed rather than added.** Phase B fired
  every edge unconditionally and produced one delivery per inbound edge, so two of §1's six edge
  kinds were inert: **switch/case + multi-selection routing** (without which Handoff and Router are
  unbuildable) and **fan-in aggregation** (§2 states outright that the superstep model "makes fan-in
  well-defined"; without merging, an aggregator ran once *per inbound edge* instead of once with
  every input — a different program that still produces a plausible-looking answer, so it is checked
  on the aggregator's input cardinality, not its output text). Both were already declared in §1 and
  already validated by Phase A; neither is a subsystem. Group chat and Planner are built from **one
  graph, twice**, differing only in the moderator's body — §3 says they share a shape, so building
  them as two shapes would have contradicted the RFC.
  **I3 boundary, tested:** routing selects among edges *the graph declares*. An executor — in
  production, one fed by a model — that returns a label no edge carries reaches nothing, and the run
  fails `routing_failed` rather than completing with an empty output the caller cannot distinguish
  from success. This is what keeps §3's Router row ("switch/case on a **classifier's** typed
  output") inside I3 instead of in tension with it: the choice is among pre-authorized,
  already-type-checked options, never an authority the executor holds.
- **Phase D — failure (014 §6).** Per-edge declared policy (propagate/retry/fallback-branch/fail),
  Quark `OnFailure` supervision wiring, preserved partial results. *Falsifiable:* a failed workflow
  discarding completed executor outputs; an executor failure taking the workflow down under a policy
  that says otherwise.
- **Phase E — the request port (014 §4), engine half (decision 1).** `InputRequired` emission,
  concurrent `Interaction` records on one run (OQ-4's case, never yet exercised), suspend holding no
  resources, resume on response. *Falsifiable (G5):* a census showing a live activation while
  suspended.
- **Phase F — checkpoint, resume, time-travel (014 §5).** Superstep-boundary checkpoints over the
  existing session `Store` seam; two-phase pending→committed (decision 7); rewind + re-run-forward
  with the audit record and `EffectJournal` interaction (decision 9). *Falsifiable (G2):* a kill at
  any of a 20-node workflow's superstep boundaries resuming to output differing from the
  uninterrupted control.
- **Phase G — introspection (014 §7).** Mermaid/DOT render, graph diff across versions, live view of
  executor states/in-flight messages/round number over `ae::stream<T>`. *Falsifiable:* a valid
  workflow that cannot be drawn.
- **Phase H — the Project record and registry (030 §2/§3/§6).** Manifest schema (active/archived
  member split, §8 Q1), always-snapshot-mode over the same `Store` seam, the registry actor,
  `create_project`/`list_projects`. *Falsifiable (G4):* a manifest write proportional to
  active-member count or archived-tail size.
- **Phase I — directed lifecycle (030 §4).** `pause_project` (member sessions, then workflow
  supervisors per §8 Q4, then the Project's own actor, last), `restore_project` (manifest only, no
  eager reactivation), resume, archive. *Falsifiable (G3):* `list_projects` returning another
  principal's Project under concurrent multi-principal load.
- **Phase J — exit-criterion proof.** 014 §8 G1 (each §3 pattern correct under *injected executor
  failure and delay*) and 030 §7 G1 (N ≥ 100 Projects; pausing one drops its sessions' and workflow
  supervisors' activations and sandboxes to zero with zero observable effect — latency and event
  ordering — on the other N-1).

## What this milestone will not close, stated up front

- **014 §8 G4's declarative half** — needs 015 (M7). The validator is shared so M7 wires a loader to
  it rather than reimplementing (decision 6).
- **014 §8 G6's multi-node execution** — needs ≥3-node cluster test infrastructure this repo has
  never built; the two-phase discipline it tests is proven single-node in Phase F (decision 7).
- **014 §4's four surfaces** — need 013 (M7); the shape is built, no surface is bound (decision 1).
- **030 §6's `EmbeddedHost` facade** — needs 020 (M9); the four verbs are built beneath it
  (decision 2).
- **Data-driven fan-out CARDINALITY for Map-reduce (§3 row 6)** — found in Phase C. §3's Map-reduce
  is built over a *fixed* mapper set, each mapper taking a slice, which exercises the graph shape
  fully. What is not built is *K mappers for K items, K known only at runtime*. 014 §9 Q3 already
  resolved this as separable — "already a runtime instance count, not a change to 'the graph' as §1
  defines it (executors/edges are typed *kinds*; how many parallel instances a fan-out spawns is
  orthogonal to which kinds exist)" — so it is per-node instance multiplicity, not a graph feature.
  It is named rather than faked because the fake is tempting and wrong: K deliveries to one mapper
  actor would **serialize on that actor** (one actor drains its mailbox sequentially) and quietly
  stop being a map, the same class of silent degradation decision 5b already caught once.
- **A cost/latency budget for orchestration** — 023 baselining stays `TBD-baselined` project-wide
  until M8, unchanged from every prior milestone.

## Handover & kick-off

Written 2026-08-07, immediately following M5's close (ADR-020, commit `53a0f5c`). Milestones 0-5 are
complete. Nothing in M6 is blocked: every primitive it needs — real `Engine`, `passivate()`,
supervision, `AskFuture`, `task<T>`, `ae::stream<T>`, `Interaction`, the `Store` snapshot seam,
`EffectJournal`, `Worktree` — is verified real above. The two genuinely new things are the graph
vocabulary itself and the Project record; both are greenfield, with no stale scaffolding to
reconcile, which is the first time that has been true since M0.
