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

10. **A declared `OnFailure<…>` is INERT on the registration path this milestone uses — found by
   measurement in Phase D, and it is decision 5b's shape exactly.** `Engine::spawn<A>()` resolves
   `supervision_of<A>()` and passes it to the `Activation` constructor. `Engine::register_activation()`
   — the path M4 Phase E2 established and every M6 test uses, because `spawn<A>()` returns only an
   `ActorId` while these actors need their own `initialize()` called — takes an already-constructed
   `Activation` and **never touches supervision**. So the obvious host wiring
   (`Activation(node, A::dispatch_table(), pool.sink())`) silently yields Quark's default policy:
   Restart, *unbounded*. `FunctionExecutor`'s declared `OnFailure<Restart, MaxRestarts<3,
   Within<1000>>>` was in force nowhere, every test passed, and 014 §6's "subject to a **bounded**
   escalation" was simply not implemented. Measured: a throwing executor driven past the declared
   budget of 3 entered its handler **6** times. `workflow/executor.hpp`'s `make_workflow_activation`
   is the fix, and it lives beside the policy *declaration* — as with 5b, the naive host choice is
   the broken one, so the correct construction has to be the one that takes *less* code to write.
   After the fix the same case measures **4** entries: 3 charged restarts, then `do_escalate` →
   `do_stop`, after which the stopped actor's asks dead-letter without entering the handler.
   *Consequence:* the workflow's per-edge retry budget (014 §6) and the actor's restart budget
   (Quark 007) compose, and the **tighter one wins** — an edge cannot buy itself more executor
   attempts than the executor's own supervision policy allows. *Consequence for later phases:* any
   future workflow actor must be built through this helper, and 030's Project-supervising actors
   (Phases H-I) inherit the same trap.

11. **A suspended run RETURNS; it does not park inside a handler — forced by `passivate()`, the same
   way decision 4 is forced, one layer up.** 014 §4 requires a suspended workflow to hold no
   resources: "it is checkpointed, its activations passivate", and §8 G5 *measures* it. The obvious
   implementation of "suspends the workflow until a response arrives" is to `co_await` the response
   inside the running handler — but `ActorRef::passivate()` drains in-flight work and is explicitly
   never a mid-handler interrupt (Quark ADR-034), so a run parked mid-handler keeps its activation
   alive forever and G5 becomes unprovable. Unlike decision 4 this one does **not** announce itself
   with a compile error: the parked design compiles, runs, and passes every correctness assertion
   about suspend and resume. Only the census fails. So the run's state lives in the ACTOR
   (`RunState`), `RunWorkflow` replies `suspended` carrying the open `Interaction`s, and
   `ResumeWorkflow` continues the same run from exactly where it stopped.
   *Consequence:* §2's deadline accumulates over **running** time only. Charging human latency to a
   bound the author set to limit *computation* would kill every human-in-the-loop workflow that
   declares one, and would make the bound mean something other than what §2 says.
   *Consequence for Phase F:* the checkpoint boundary and the suspension boundary are now the same
   boundary — `RunState` is already the thing that has to be durable.

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
  **Outcome: §6's three bullets are built, and the phase turned on there being TWO failure channels**
  — a body that *returns* an error (the actor is healthy; the edge's declared policy decides) and a
  body that *throws* (Quark 007 runs the executor's `OnFailure` and dead-letters the pending ask).
  Phase B collapsed both into one `round_failed` flag. A suite that only exercised `ok = false` would
  have proven half of §6 and would never have discovered whether a faulting executor **stalls** the
  supervisor — so the throw tests assert on a *completed* run, not merely on a non-crash.
  **Classification does work rather than decorating** (§6's "is classified (001 §6)"): retry applies
  to `transient`/`resource` only. A `contract` failure is deterministic, so retrying re-runs the same
  computation to learn nothing; a `policy` failure is a **denial**, and re-asking a denied request
  until the answer changes is an I2 concern, not a retry. Measured: 1 invocation under a budget of 3
  and of 5 respectively, with the retried `transient` case as the positive control.
  **Policy is declared on the edge (§6's word) but must AGREE across a node's outgoing edges** — the
  thing it decides is a property of the source, and resolving a disagreement by precedence
  ("most severe wins") would be a rule no reader could predict, on the code path that runs when
  something has already gone wrong. Only the fallback *target* may differ. A fallback branch is
  type-checked like any other edge out of its source, so it is not the one edge in a graph exempt
  from 014 §1 — the one that only ever runs after a failure.
  **Partial results are last-write-wins per executor**, bounded by node count. An append-only log
  would grow with rounds, and a deadline-bounded cyclic graph has no bound on those; the full
  per-round history is 014 §5's checkpoint record (Phase F), which is the thing designed to be
  durable. See decision 10 for what Phase D measured about the supervision wiring itself.
- **Phase E — the request port (014 §4), engine half (decision 1).** `InputRequired` emission,
  concurrent `Interaction` records on one run (OQ-4's case, never yet exercised), suspend holding no
  resources, resume on response. *Falsifiable (G5):* a census showing a live activation while
  suspended.
  **Outcome: built, and its structural decision is forced rather than stylistic** — see decision 11.
  A suspended run **returns**; it does not `co_await` the human's response inside the running
  handler. `ActorRef::passivate()` drains in-flight work and is never a mid-handler interrupt, so a
  parked run would hold its activation and G5 would be false *while every other assertion still
  passed*. The census is the only thing that distinguishes the two designs, which is why §4's "holds
  no resources" needed measuring and not asserting.
  **OQ-4's case is now exercised for the first time anywhere in this codebase:** two ports in
  different fan-out branches open two concurrent `Interaction` records on one run, and §2's superstep
  barrier is what makes it reachable — it keeps the branches in step so both reach their ports in the
  same round. Answering one leaves the run suspended on the other (001 §2's "does not leave
  Suspended until every Interaction is resolved", which `AgentSession` already implements verbatim).
  **Interaction ids are derived, not random** — `<run>:port:<executor>:<round>`. §4 wants a
  checkpoint indexed by the token and I5 wants nondeterminism to cross a recorded seam; a UUID would
  be an unrecorded nondeterministic input on the one identifier a resume must match. The round is in
  it because a port inside a cycle opens more than once, and two openings are different requests.
  **A human's answer can route** (`ResumeWorkflow::routes`), because approve/reject is the canonical
  HITL shape and would otherwise be unbuildable. It reuses `ExecuteReply::routes` verbatim, so the I3
  boundary is the *same* one a classifier gets: a label selects among edges the graph declares, and a
  human can no more name an unwired node than a model can — tested with an `admin_override` label
  that reaches nothing.
  **A second predicate was needed:** `check_workflow_executable`, kept separate from
  `validate_workflow`. The supervisor asks every non-port node through `FunctionExecutor`, so an
  `agent` or `sub_workflow` node would be *run as a function* — plausible output from a graph whose
  behaviour differs from what its author declared. That refusal cannot live in the shared validator,
  whose answer must not depend on how much of 014 is implemented: §7's rendering, §7's diffing, and
  the 015 loader all run over graphs no build can execute.
  *G5 is HALF proven and recorded as such:* the activation half is measured by census; "no sandbox,
  no connection" have nothing to hold yet (both arrive with the agent-kind executor), and "resumes
  after a **process restart**" needs 014 §5's checkpoint — Phase F. What is proven is resume across a
  Dormant round-trip in one process, which is strictly less.
- **Phase F — checkpoint, resume, time-travel (014 §5).** Superstep-boundary checkpoints over the
  existing session `Store` seam; two-phase pending→committed (decision 7); rewind + re-run-forward
  with the audit record and `EffectJournal` interaction (decision 9). *Falsifiable (G2):* a kill at
  any of a 20-node workflow's superstep boundaries resuming to output differing from the
  uninterrupted control.
  **Outcome: built, and it turned on a real prerequisite this phase's own scope note hadn't named
  yet.** §5's "resume restores exactly" is false the moment a checkpoint drops the run's actual
  `Message` payloads (`pending`/`partial`/`selected_output`) -- and `Message`/`ContentItem` had no
  `QUARK_SERIALIZE` at all (the gap `AgentSessionRecord`'s own comment names, agent_session.hpp:
  154-158). Closing it turned out to be a real design fork, not a fill-in-the-tags exercise: a
  hand-written, branching `quark_describe(Ar&, ContentItem&)` over the 9-way variant was tried
  first and REJECTED -- `FingerprintFolder` computes a type's fingerprint from ONE
  default-constructed sample (always arm 0, `Text`), so a conditional describe body would fold the
  SAME fingerprint for two versions that differ only in how they encode `Media`/`ToolCall`/etc., a
  new and undetected variant of the non-uniqueness gap `third_party/quark/OpenQuestions.md` item 2
  already names -- and the write/size passes' `const_cast`-and-trust-it's-read-only convention
  (wire.hpp) makes a body that reassigns the variant on every pass real undefined behaviour on a
  genuinely `const` caller object. The design that shipped instead
  (`include/agentengine/core/content_record.hpp`) is a FLAT record family -- one always-present
  field per variant arm, materialised from whichever is active, defaulted otherwise -- so every
  `quark_describe` body is a straight-line `QUARK_SERIALIZE` list with zero branching, the same
  shape every other durable record in this codebase already uses. `RunStateRecord`
  (`workflow/checkpoint.hpp`) is `WorkflowSupervisor`'s own durable projection built on top of it,
  crossed with the live actor at exactly two points (`to_record()`/`restore_from_record()`,
  mirroring `AgentSession`'s own pair).
  **The two-phase pending→committed discipline (decision 7) is real, not a restatement of
  `EventLog::commit()`'s own atomicity.** `Store`'s snapshot slot is latest-only by construction
  (`InMemoryStore::save_snapshot` overwrites), so "rewind to ANY retained checkpoint" needed the
  EventSourced model instead -- `effect_journal.hpp`'s own intent/outcome idiom, reused verbatim for
  a checkpoint's pending/committed pair. A single atomic batch would already make "crash before
  either phase" safe for free (nothing durable, indistinguishable from "never attempted") -- the
  two-phase split earns its keep specifically for a crash BETWEEN the two commits, which
  `stage_pending_checkpoint`/`commit_checkpoint` are exposed as separate calls specifically so a
  test can inject.
  **`ContinueWorkflow` is a new ask this phase added, not reused from Phase E.** `RunWorkflow`
  always starts a fresh run and `ResumeWorkflow` requires an open port matching the response's
  interaction id -- neither fits "continue a restored, NOT-suspended run", which is the common case
  (most superstep boundaries are not a request port). `execute()` itself needed no change: it does
  not care how `state_.pending` came to be populated, the same sharing property Phase E's own
  header note already relies on for `RunWorkflow`/`ResumeWorkflow`.
  **The checkpoint hook is an explicitly-injected callback, not a `Store` the actor holds** --
  `std::function<void(std::uint32_t, RunStateRecord const&)>`, nullptr by default. `Store` is a
  compile-time concept, not a runtime-typed interface, so holding one directly would have forced
  `WorkflowSupervisor` to become a template over a `Store` type, and every Phase B-E test to grow a
  template parameter it has no use for. The hook keeps the actor `Store`-agnostic (I2 -- an
  explicitly passed capability, never ambient) while still letting `execute()`'s own round loop
  call out at the ONE place that knows where a superstep boundary actually is.
  **Time-travel's audit log is a SEPARATE `EventLog`/`ActorId` from the checkpoint log, same run**
  (`workflow/time_travel.hpp`) -- found before it became a bug, not after: Quark's durable encoding
  prefixes every event with its OWN type's fingerprint header, and `replay_tail<Event,...>` is
  templated on exactly one `Event` type, so mixing `WorkflowCheckpointRecord` and
  `RewindAuditRecord` entries in one per-actor log would make replay fail to decode the first
  audit entry it hit. **Nothing new was built for "effects are not rewound / idempotency keys keep
  it from double-charging"** -- `IdempotencyKey`/`EffectJournal` (M4 Phase F1/F2) already derive a
  key from `{run_id, turn_index, call_index, argument_digest}`, so an executor authored against
  that machinery re-derives the identical key on an unmodified-state re-run and a different one the
  moment modified state changes an argument, both correct by construction; decision 9's "the honest
  implementation is available now" meant USE it, not re-implement it at the workflow layer.
  **G2 passed 20/20** (`test_workflow_checkpoint_g2.cpp`): a 20-node chain, one checkpoint captured
  per superstep boundary from an uninterrupted control run, then EVERY one of those 20 checkpoints
  individually restored onto a genuinely new actor instance and driven to completion via
  `ContinueWorkflow` -- not just the first or last boundary, which is the failure mode a narrower
  test would have missed. `test_workflow_checkpoint.cpp`'s own F2b additionally proves the
  HITL-suspended case (a real `ResumeWorkflow` against a restored instance that never itself ran a
  single round) -- Phase E's own G5 was only ever "half proven" (activation census; "resumes after a
  process restart" had nothing to restore FROM before this phase); it is now proven for real.
- **Phase G — introspection (014 §7).** Mermaid/DOT render, graph diff across versions, live view of
  executor states/in-flight messages/round number over `ae::stream<T>`. *Falsifiable:* a valid
  workflow that cannot be drawn.
  **Outcome: built, all three bullets.** `render_mermaid`/`render_dot`
  (`workflow/introspection.hpp`) are TOTAL functions over any `Workflow` -- the only way to
  actually guarantee "a valid workflow that cannot be drawn" never happens. The one real design
  decision: node identifiers on the wire are `n<index>`, never a sanitized version of the author's
  own id. Sanitizing (stripping/replacing special characters) can COLLIDE two distinct ids onto the
  same token and silently merge two different nodes in the rendered graph -- worse than an ugly
  label, and a failure mode this project's own I4 discipline (every effect attributable) would call
  out anywhere else. The author's real id is always the LABEL text instead (quoted, one escape),
  proven against a graph whose own id AND one executor's id both contain embedded quotes.
  `diff_workflows` is a plain structural comparison, no version history or three-way merge (025's
  merge machinery is deliberately not reused -- a workflow graph is authored data, not a
  worktree) -- `operator==` added to `Executor`/`Edge`/`EdgeFailurePolicy`/`TerminationBound`/
  `Workflow` (graph.hpp) is what keeps it a few lines rather than a hand-rolled field walk.
  **`enable_live_view()` fires from the EXACT SAME call site `checkpoint_hook_` (Phase F) already
  uses** -- that is the one place `execute()`'s round loop actually knows a superstep just
  finished and what happened in it. `WorkflowLiveEvent` is deliberately its own type, not
  `RunStateRecord` reused: a checkpoint is a durability record (needs the full state to resume
  exactly); a live event is a UI-shaped summary (which executors ran/failed/opened a port THIS
  round, how many messages are in flight) -- carrying `Message` payloads into a live-view event
  would answer a question nobody watching a dashboard asked. A round that ends the run via a
  `fail`-policy failure produces no live event, the same `broke` branch the checkpoint hook already
  skips -- proven with a real wait (not an unchecked absence) that nothing is buffered afterward.
  Resuming a suspended run continues pushing to the SAME producer, proven across a real
  suspend/resume boundary, not just a single uninterrupted run.
- **Phase H — the Project record and registry (030 §2/§3/§6).** Manifest schema (active/archived
  member split, §8 Q1), always-snapshot-mode over the same `Store` seam, the registry actor,
  `create_project`/`list_projects`. *Falsifiable (G4):* a manifest write proportional to
  active-member count or archived-tail size.
  **Outcome: built, and §3's own "always snapshot-mode, never event-sourced" sentence had to be
  read narrowly to make G4 provable at all.** §8 Q1 resolved that the archived tail "can grow
  unboundedly over a Project's long lifetime," and G4 requires a member moving to that tail to
  never cost a write proportional to its size. A single Snapshot record holding BOTH
  `active_members` and `archived_members` cannot satisfy that once the tail grows past trivial
  size: `Store::save_snapshot` (persistence.hpp) is an overwrite-the-whole-blob write, O(current
  blob size) on every call, by construction -- not a bug to route around, the actual contract of
  the model. `project.hpp` therefore splits the two: `ProjectRecord` (project_id/principal/status/
  `active_members`, kept small because members move OUT once their role completes) stays Snapshot,
  matching §3's text; the archived tail moved to its own `EventLog<ProjectMember,S>` under a
  SEPARATE `ActorId`, whose `commit()` cost is a function of the batch just staged, never of prior
  history (ADR-009 C7) -- the exact property G4 asks for, and the identical lesson Phase F already
  learned for `WorkflowSupervisor`'s own checkpoint log. G4 is proven by measurement, not asserted:
  growing the archived tail to 2000 entries leaves a 5-active-member manifest's own
  `tagged_object_size` byte-for-byte unchanged, and the per-member marginal cost of active-member
  growth (5 -> 50) stays small and roughly constant.
  **The registry actor never self-persists**, matching `AgentSession`/`WorkflowSupervisor`'s own
  consistent I2 discipline across this codebase (found by grepping, not assuming: neither type's
  own handler ever calls a `Store` function itself -- `journal_effect_intent`/`journal_effect_outcome`
  aren't called from inside `AgentSession::handle()` either). `ProjectRegistry::handle(CreateProject)`
  mutates only its own in-memory index; a host separately calls `append_project_registered`
  (registry.hpp) and `save_project_snapshot` (project.hpp), exactly mirroring
  `AgentSession::to_record()` + the external `save_agent_session_snapshot()` a host already drives.
  **`CreateProject`/`CreateProjectResult` measured over Quark's 192-byte inline message-pool cell
  (`detail::MessagePool::kMaxPayload`) before landing on their final shape.** The first draft
  carried a full `Principal` (its own `kind`/`on_behalf_of`/`delegation_depth` fields this actor
  never needs) plus `root_session_id`/`title`/`host_metadata`, and a full `ProjectRecord` echoed
  back in the reply -- both well over budget, a real compile-time `static_assert` failure caught
  before it could become a runtime surprise. The shipped shape carries only `project_id`/
  `principal_id`/`principal_tenant_id` in the request and a bare `{ok, error_code}` in the reply;
  a caller who wants `root_session_id`/`title`/`host_metadata` on the record already has every
  input needed to build the rest of `ProjectRecord` itself before calling `save_project_snapshot` --
  the registry's own job stays exactly what §1 says a Project already is one layer down: "an index,
  not a new execution unit."
  **`project_id` is caller-supplied, never minted here** -- matching `session_id`'s own provenance
  (`AgentSession::initialize`), since this project has no wired random source anywhere and I5
  forbids inventing one to mint ids with.
- **Phase I — directed lifecycle (030 §4).** `pause_project` (member sessions, then workflow
  supervisors per §8 Q4, then the Project's own actor, last), `restore_project` (manifest only, no
  eager reactivation), resume, archive. *Falsifiable (G3):* `list_projects` returning another
  principal's Project under concurrent multi-principal load.
  **Outcome: built, and it surfaced a real gap §4's own prose glosses over.** §4 says "call
  `.passivate()` on every member session's `ActorRef<AgentSession>`" as if that named one type. It
  does not: `AgentSession<ChatClientT, StateT, HistoryProviderT>` (agent_session.hpp) is a
  three-parameter template, so `ActorRef<AgentSession<C1,S1,H1>>` and
  `ActorRef<AgentSession<C2,S2,H2>>` are different C++ types the moment two member sessions use
  different backends -- which 030 §2's own model allows (members are just session ids, nothing
  constrains them to one instantiation). `ActorRef<A>::passivate()` needs `A` concretely known at
  the call site (`static_assert(max_concurrency_of<A>() == 1, ...)`, actor_ref.hpp) and this repo
  has never had any type-erased or polymorphic way to call it (verified by search: zero hits for
  any `PassivatableRef`/`std::variant<ActorRef<...>>`/`std::any`-over-actors pattern anywhere in
  Quark or AgentEngine) -- genuinely greenfield, not an oversight to route around quietly.
  **`PassivatableHandle` (lifecycle.hpp) closes the gap without weakening the safety the
  `static_assert` exists for.** The tempting shortcut -- build on `LocalRouter::
  request_passivate(ActorId)`, the untyped primitive underneath `ActorRef<A>::passivate()` -- was
  rejected: that call has NO type gate at all, so a wrapper built on it directly could silently
  passivate a Reentrant/`MaxConcurrency<N>` actor, whose close-out is, per the assert's own
  wording, "out of scope" / unproven (ADR-028 Phase 2 only proves it for Sequential). Doing that
  would have reintroduced, by omission, exactly the hazard class the M6 breakdown's own decision 4
  already flagged for `WorkflowSupervisor` itself. `PassivatableHandle` instead closes over a call
  to the REAL `ActorRef<A>::passivate()` inside a `std::function<bool()>`, constructed from a
  template constructor -- the static_assert still fires, at construction, on the concrete `A` the
  caller already has in hand. Type erasure happens to the closure, never to the type-level check.
  Proven with REAL actors, not a synthetic pair of dummy types: one `ProjectSupervisor` holds a
  genuine `AgentSession<CannedChatClient>` (a member session) AND a genuine `WorkflowSupervisor` (a
  workflow-supervising actor, 030 §8 Q4's own named case) and passivates both uniformly -- the
  first time this codebase has ever held two different concrete actor types in one caller-side
  list. Census confirms all three activations (session, workflow, the Project's own actor,
  passivated LAST per the breakdown's own Phase I ordering) reach Dormant; restore never touches
  any of them; a real `Run` issued against the member session after restore continues its EXACT
  run-id sequence untouched (`s-member-1:run:2`, not a reminted one) -- 030 §4's own "the pause/
  restore cycle is invisible to the run itself," now proven for a session reached through a
  Project rather than directly.
  **No per-Project actor existed before this phase.** §6's "the Project's own supervising actor"
  (the thing `pause_project`/`restore_project` act on directly, not through the registry) had no
  type in Phase H -- `registry.hpp`'s `ProjectRegistry` is the shared INDEX, one per deployment,
  never one per Project. `ProjectSupervisor` (lifecycle.hpp) is new this phase: non-templated,
  matching `WorkflowSupervisor`'s own shape, since `PassivatableHandle` absorbs all the
  heterogeneity so the supervisor itself doesn't have to.
  **`archive_project` is deliberately just `pause_project` plus a status flip, nothing else** -- no
  retention/GC call anywhere, consistent with 030 §8 Q2's already-resolved "archived means hidden,
  not shrunk," and consistent with no retention/GC mechanism existing in this codebase yet to call
  even if this phase wanted to.
  **G3's own "concurrent multi-principal load" stress framing is not built THIS phase** -- Phase
  H's own H2 already proves the underlying cross-principal/cross-tenant isolation property
  sequentially (registry.hpp's index is scoped on `principal_id` AND `principal_tenant_id`
  together, structurally, not by a filter that could be bypassed); a genuinely concurrent
  multi-principal stress version of the same claim is left to Phase J, which is where 030 §7's
  gates are proven as a set against N ≥ 100 Projects.
- **Phase J — exit-criterion proof.** 014 §8 G1 (each §3 pattern correct under *injected executor
  failure and delay*) and 030 §7 G1 (N ≥ 100 Projects; pausing one drops its sessions' and workflow
  supervisors' activations and sandboxes to zero with zero observable effect — latency and event
  ordering — on the other N-1).
  **Outcome: both halves built and passing -- this is the milestone's own exit criterion, and it
  now holds.**
  **014 §8 G1** (`test_workflow_patterns_resilience.cpp`) reuses Phase C's own eight §3 graph
  constructions verbatim -- this file does not re-derive correctness, it re-proves it under
  fault. Injecting failure into a node's INCOMING edge instead of its OWN outgoing edge was a real
  bug caught while writing this, not a hypothetical: `policy_for()` (supervisor.hpp) reads a
  node's OWN outgoing edges to decide what happens when THAT node fails -- an edge INTO the
  flaky node governs what happens if its SOURCE fails, a different node entirely. Concretely, this
  meant a terminal node (`billing`/`tech` in Router, no outgoing edges of their own since they are
  only output-selected) can never retry via edge-declared policy no matter what its incoming edge
  says -- so Router's own failure injection moved to the CLASSIFIER itself (both its outgoing
  switch_case edges, consistently), which incidentally makes the more interesting claim anyway: the
  routing decision-maker recovering from a transient fault still routes correctly, not just a
  downstream leaf recovering. Both failure (retry-recovered, five patterns) and delay (a real,
  short, bounded sleep inside an executor body -- Phase B's own B2 precedent for simulating
  latency, not a new technique, three patterns) are exercised across the eight, not one kind
  repeated eight times.
  **030 §7 G1** (`test_project_scale_isolation.cpp`) stands up 100 real `AgentSession` actors and
  100 real `ProjectSupervisor` actors (200 activations -- decision 8's own reasoning again: an
  actor count, never a thread count), gives every one a real first `Run`, pauses Project #50 (an
  interior index, not a boundary one), and then drives the other 99 through a real second `Run`
  each. All three claims are measured: the paused Project's two activations census to Dormant; all
  99 others complete with their OWN run-id sequence uninterrupted (`run:2`, or `run:3` for a
  sampled subset an earlier timing pass already advanced -- tracked per-session, not assumed
  uniform, a second real bug the test's own first run caught before this became a false green);
  and a timed sample shows no structural latency regression (a generous 3x bound, sized to catch an
  accidental shared lock, not machine noise). **Sandboxes are explicitly NOT asserted** -- nothing
  in this milestone wires a real sandbox allocation into `AgentSession`/`WorkflowSupervisor` (008's
  backend is a separate M2 subsystem this milestone's own actors never touch), so "sandbox count
  drops to zero" would be vacuously true rather than a real measurement; named as a scope
  limitation here rather than silently claimed proven.

## Milestone 6 is complete (Phases A-J)

Every phase this breakdown planned is built and tested: the graph as data (A), the superstep engine
(B), all eight §3 patterns (C), failure handling (D), the request port (E), checkpoint/resume/
time-travel (F), rendering/diffing/live-view (G), the Project manifest and registry (H), directed
lifecycle (I), and this phase's own exit-criterion proof (J). The roadmap's own Milestone 6 exit
criterion -- "each 014 §3 pattern runs correctly under injected executor failure (014 §8 G1), and
pausing one Project has zero observable effect on N-1 others (030 §7 G1)" -- is the pair Phase J
proves. What Milestone 6 deliberately does NOT close is listed below, unchanged by Phase J and
carried forward to the milestones already named for each: M7 (015's declarative loader, 013's four
HITL surfaces, the ≥3-node cluster claim), M9 (020's `EmbeddedHost` facade).

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
