# 014 — Workflow and Orchestration

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Amended 2026-10-01** (§1 switch/case default, ADR-215 Proposed, issue #34; §2 cancellation, ADR-214 Proposed; §4 one interaction per delivery and answers checked before acceptance, issues #155/#157; §4 review points, issue #45) · **Amended 2026-09-04 by ADR-169** (§4 — resolving a request port requires caller admission; holding an `interaction_id` is not authority) · **Amended 2026-09-27 by ADR-152/ADR-157** (§7 — the live view has a fine-grained sibling, the workflow event stream; issue #82) · **Depends on:** 001, 002, 005, 013, 018, 019 · **Gate:** §8

## Goal

Multi-agent structure that is **explicit, typed, checkpointable, and inspectable** — a graph of
executors with typed edges — rather than emergent behaviour from agents calling each other. The
shape is MAF's workflow model (executors, edges, fan-out/fan-in, checkpointing, human-in-the-loop,
time-travel), expressed in the CRTP idiom and driven by `WorkflowSupervisor`, a plain object running
a superstep loop over `rt::ThreadPool` (historical: earlier revisions of this RFC hosted executors on
Quark actors; ADR-037 removed Quark as AgentEngine's runtime — see
`AgentEngineSpecification.md` §7).

## 1. Model

```
Workflow = { executors[], edges[], start, output_selection, policies }
Executor = an agent | a function | a sub-workflow | a request port
Edge     = direct | fan-out | fan-in | switch/case (+ optional default) | multi-selection | chain
```

- **Switch/case selects exactly one case; a default catches "none".** A source's `switch_case` edges
  are matched against the routes its reply names. Exactly one match fires that case. More than one
  match ends the run `routing_failed`, default or not. Zero matches also end it `routing_failed`,
  unless the source declares a **default** (`switch_default`, ADR-215, issue #34): then the default
  edge fires instead. A default carries no label. A source has at most one, and only alongside at
  least one `switch_case` edge (`validate_workflow`). Natively it is `WorkflowBuilder::connect_default`
  (beside `connect_case`); declaratively it is `default: true` on a `to` edge (015 §3).

- **Executors are typed by their input and output message types.** An edge that connects
  incompatible types fails to build — at compile time for the C++ form, at load for the declarative
  form (015), using the same validator (I6).
- **Executors are plain objects**; `WorkflowSupervisor` (`rt/workflow_supervisor.hpp`) owns them and
  drives the superstep loop, dispatching each round's executor calls through `rt::ThreadPool` for
  real concurrent fan-out. Concurrency, ordering, and failure isolation come from the runtime, not
  from a bespoke executor pool (historical: this RFC originally hosted executors as Quark actors,
  with the supervising actor owning them; ADR-037 removed Quark — `AgentEngineSpecification.md` §7).
- **Messages between executors are the content model** (003), so an agent node and a function node
  are interchangeable at an edge.
- **A function executor may be marked `batch` (ADR-235, OQ-20).** Its body must be a
  `BatchableModelCall`: one model call split into build, call and complete. When the host has called
  `enable_batch_coalescing()`, the round's `batch` deliveries are built, admitted by their declared
  `BatchBackend`, grouped by backend and calling context, and sent as vendor batch jobs after the
  round's synchronous wave. Without that opt-in a `batch` node runs synchronously, unchanged. `batch`
  on any other executor kind fails validation. Batching is single-shot: no tool loop runs inside an
  item, so the flag fits independent fan-out calls, not an agent's multi-round turn.

**Worktree scoping across executors, the policy+minting half — resolved by ADR-032
(2026-08-11):** every `Executor` (`workflow/graph.hpp`) declares its own `worktree_mode`
(025 §3's `sharing_mode`), defaulting to `branch` unconditionally (not §3's "sequential defaults to
shared" applied per-node — see the ADR for why that would be unsound for this RFC's cyclic,
dynamically-routed graphs) with `shared`/`readonly`/`scratch` available as an explicit per-executor
opt-in. `workflow/worktree_scoping.hpp`'s `mint_executor_worktrees`/`resume_executor_worktrees` turn
that declaration into a real `SubWorktree` plus a capability-gated `Mount`, reusing 025 §3's
primitive unmodified. **What ADR-032 does NOT close**: wiring the resulting grant into a running
`FunctionExecutor`'s `EffectContext`/`ExecutorBody` (no production host builds a `FunctionExecutor`
fleet today, so there is no real caller to design that wiring against yet); `agent`/`sub_workflow`-
kind executor worktree wiring (those kinds are not built — `check_workflow_executable` still rejects
them); WHEN a `branch` executor's worktree merges back into its parent (025 §4's merge-on-join has
no hook into `WorkflowSupervisor::execute()` yet); and a `readonly` executor's guest-facing `Mount`
(the existing `Mount`/`mount_read` primitive has no pinned-digest read path — a `core/worktree.hpp`
change ADR-032 did not make as a drive-by). Full trace, including the pre-ADR-032 state:
`docs/architecture/worktree-sharing-skills-and-subagents.md` §3.

## 2. Execution semantics

- **Superstep model.** Execution proceeds in rounds: all messages delivered in round *n* are
  processed before round *n+1* begins. This makes fan-in well-defined, checkpoints natural
  (round boundaries), and replay deterministic given recorded nondeterminism (I5).
- **Determinism obligations.** Within a round, executor scheduling order must not affect the
  workflow's output. An executor whose output depends on intra-round ordering is a defect, caught
  by the shuffle test (§8 G3).
- **Termination** is by output selection, by an explicit terminal executor, or by bound
  (`MaxRounds`, deadline, budget). An unbounded workflow does not run — the bound is required.
- **Cancellation** (`ADR-159`, amended by `ADR-214`, issue #156) ends a run `cancelled` wherever it
  lands. In a round, the in-flight steps finish cooperatively (an agent step's session sees the
  cancel and ends its own run), no failed step is retried, and the round ends `cancelled` before
  routing — a step failure the cancel caused is reported as the cancel, not as an executor failure.
  While suspended, the run ends `cancelled` at once and its open interactions (including a nested
  sub-workflow's) are closed; a later answer or continue is refused. A cancel applies to the run in
  progress, not to the supervisor: the next run starts with a fresh cancellation, and a caller that is
  itself cancellable links its own token into the run it starts.

## 3. Patterns

The named patterns are configurations of the graph, not separate subsystems:

| Pattern | Graph shape |
|---|---|
| **Sequential** | chain |
| **Concurrent** | fan-out + fan-in with an aggregator |
| **Handoff** | switch/case on a routing decision, control transfer |
| **Group chat / debate** | a moderator executor cycling among participants with a caller-set round bound as the primary termination contract |
| **Planner (Magentic)** | the same cyclic-moderator shape as Group chat/debate, but the moderator maintains a task/progress ledger, picks the next participant and decides completion itself, and self-triggers a replan on stall detection — round/stall/reset bounds are a safety valve, not the termination contract |
| **Map-reduce** | fan-out over a collection + fan-in reducer |
| **Router** | switch/case on a classifier's typed output |
| **Reflection / critic** | a cycle with an explicit iteration bound |

**Guidance:** an agent-to-agent interaction with structure belongs here, not in a handoff chain
(002 §4). Handoff is for one hop; a graph is for a process. **Group chat/debate vs. Planner:** pick
Group chat/debate when the caller wants to author the routing and stopping condition explicitly;
pick Planner when the caller wants to hand over a goal and let the moderator own routing, completion,
and recovery from a stalled round — "send it a goal and it finishes" is Planner's defining case, not
Group chat's (see `docs/research/2026-maf-orchestration-patterns.md`).

## 4. Human-in-the-loop

A **request port** is an executor that emits `InputRequired` (001 §2) and suspends the workflow
until a response arrives. It is the same mechanism as tool approval and A2A `INPUT_REQUIRED` — one
shape, four surfaces (013 §5). Multiple request ports open concurrently in different branches
produce multiple concurrent `Interaction` records (001 §2) on the same run — the case that makes
`interaction_id` a set rather than a singleton, resolving OQ-4. The set has one member per
*delivery*, not per port: two messages reaching the same request port in the same round (a fan-out
whose branches both lead into one review port) open two interactions with two distinct ids, each
answered on its own (issue #157). An id is `<run>:port:<port id>:<round>`; a second same-round
delivery to that port takes the first free `:<k>` suffix (`k ≥ 1`), so a port reached once per round
keeps the unsuffixed id. No two interactions that are open at the same time share an id.

**An answer is checked before it is accepted** (issue #155). The routes a response names are validated
against the port's outgoing edges *before* the port is consumed: every label must be the case label
of a `switch_case`/`multi_selection` edge out of that port (an undeclared label is refused whether it
is alone or mixed with valid ones, and on a port with no labelled edges any label is undeclared), and
when the port has `switch_case` edges the routes must select exactly one of them (or none, if the port
has a switch default — ADR-215; a default answers "no case chosen", it never absorbs an undeclared
label). A refused answer —
like an unknown or already-answered id, or a caller the admission gate denies — changes nothing: the
port stays open under the same id, the run stays suspended, and the refusal is reported as itself
(`invalid_routes`; on the event stream, `request_port_rejected`, never a run-failed event). A typo in
a route is therefore corrected by answering again, not by losing the run.

**Review points: the builder form of "pause after this step"** (issue #45; MAF's
`SequentialBuilder`/`ConcurrentBuilder.with_request_info()`). A review point is not a new kind of node:
it is a request port spliced in after a step, plus ordinary edges, by one shared function over the
graph (`insert_review_points()`, `workflow/review_points.hpp`). For a review point after step *S*: a
`request_port` *S*`.review` typed with *S*'s output type is appended to `executors`; every edge out of
*S* now leaves from the port (same target, kind and label, default failure policy); a direct edge
*S* → *S*`.review` carrying *S*'s own failure policy (§6) takes the place of *S*'s first edge; and if
*S* was selected as output, the port replaces it. A **revisable** point also turns *S*'s one forward
edge into the port's switch default (§1) and adds a `switch_case` edge back to *S* labelled
`revise`. Answering is the ordinary resume: no routes approves — the answer is what flows on, so
answering with the ask passes *S*'s output through and an edited message amends it; `revise` sends the
answer back to *S* for another pass (so *S* must take its own output type), bounded by the round bound
(§2). Revision needs exactly one direct/chain successor: a step that feeds a fan-in (a fan-in edge
always fires, so it cannot be a default) or has no successor gets an approve/amend point. A step whose
edges route (switch/case, multi-selection) cannot be reviewed — the reviewer's routes would replace its
own — and neither can a request port, an unknown step, or one selected twice; these are build
errors, never a silently different graph.

The C++ surface is `SequentialWorkflowBuilder<Msg>` (participants joined by `chain` edges;
`.with_request_info({.only, .allow_revision})` — every participant when `only` is empty; the last
participant's point is approve/amend) and `ConcurrentWorkflowBuilder<In, Out, Result>` (dispatcher
fan-out, participants, fan-in aggregator; `.with_request_info({ids})` reviews the selected
participants *before* aggregation, so the fan-in barrier counts the review port as the source and the
aggregator runs once, after every review is answered). The declarative surface is 015 §3's
`review_points`, expanded by the same function (I6). Because the expansion is exactly the hand-wired
graph (proved by structural equality in `tests/workflow/test_workflow_review_points.cpp`), everything
above about request ports — one interaction per delivery, answers checked first, admission,
cancellation, checkpoints — holds for review points with no further engine code. Not built: a
revisable review in front of a fan-in, which MAF's nested approval workflow allows; it needs a fan-in
edge gated by a route, which the graph does not have.

A suspended workflow **holds no resources**: it is checkpointed, its activations passivate, and it
resumes on the response, on a durable reminder (the runtime's durable reminders — formerly Quark's;
carried over intact by ADR-037's `rt::` migration), or never — an abandoned workflow is
garbage-collected by policy, not leaked. The request port's `InputRequired` carries the same
`request_id`-shaped correlation token defined in 001 §2; a checkpoint taken while suspended stores
the pending request indexed by that token, matching MAF's own checkpoint/request-info coupling
(`docs/research/2026-08-03-maf-workflow-and-hitl-model.md` §2).

**Holding a correlation token is not authority to resolve it** (added 2026-09-04 by `ADR-169`,
GitHub issue #65 — this section was previously silent on *who* may answer, and that silence was the
gap). Resolving a request port injects a `Message` into a suspended run **and** may name `routes`,
which on a `switch_case`/`multi_selection` edge decides where the run goes next; that is authority
over the run, spending effects and budget belonging to the run's owner. An `interaction_id` is not a
secret — it crosses the run-result surface to the driving host, and per `ADR-061` that host is
exactly the layer relaying a possibly-untrusted caller. So every entry point that starts, resumes, or
continues a run admits its caller against the run's owning principal first, using the same 018 §2
predicate every other actor boundary uses (I2), and a refusal is recorded as itself — distinct from
an invalid token, and carrying no other interaction's id back to the refused caller (I4). The unit of
admission is the run, not the individual port: a workflow whose concurrent ports genuinely belong to
*different* principals needs per-interaction ownership, which `ADR-169` §9 names as unbuilt.

**A pending vendor batch item is a third way a run waits on the outside (ADR-235 §3.0),** beside
an unanswered port and a pending sub-workflow. One predicate, `awaiting_outside()`, covers all three:
no entry point drives the superstep loop while it holds, so answering the last port while batch items
are pending keeps the run suspended. A batch item is not an `Interaction`: it is never listed by
`open_interactions()` and never answered through `resume_workflow()`. It is resolved only by
`poll_batches(PollBatches{caller})`, which admits its caller like every other entry point. A run that
ends in any state but `suspended` abandons its pending items; their jobs are cancelled on a later poll.

## 5. Checkpointing, resume, and time-travel

- **Checkpoint at superstep boundaries**: executor states, in-flight messages, workflow state,
  and the run's position. Backed by the same store as sessions (005 §2).
- **Resume** restores exactly, on the same node (historical: this RFC originally described resume on
  any node in a cluster, with Quark placement deciding where; ADR-037 removed Quark and AgentEngine
  has no multi-node cluster story at all — this is a real, permanent narrowing, not a renamed
  mechanism, per `AgentEngineSpecification.md` §7).
- **Time-travel**: rewind to any retained checkpoint and re-run forward, optionally with modified
  state — the debugging feature that makes multi-agent systems tractable. Retention is policy;
  every rewind is audited, because rewinding a workflow that already had external effects is a
  correctness hazard the operator must own.
- **Pending vendor batch items are checkpointed; their results are not (ADR-235 §3.4).** A checkpoint
  carries each pending item's executor, input, group key, job id and custom id, plus abandoned jobs
  still owed a cancel. A restored run re-polls; a job id goes back only to a backend with the same
  group key, and fails closed otherwise.
- **Effects are not rewound.** Re-running forward re-executes tools; idempotency keys (019) are the
  mechanism that keeps that from double-charging someone. The spec states this loudly because the
  alternative — pretending rewind is safe — is how time-travel becomes a footgun.

## 6. Failure

- An executor failure is classified (001 §6) and handled by the edge's declared policy: propagate,
  retry, route to a fallback branch, or fail the workflow.
- **Supervision** is per-round fault containment: `rt::ThreadPool::submit()`'s `JobOutcome{faulted,
  fault_ptr}` stops a throwing executor's job from crashing the process or hanging the collector, the
  faulted job is classified `failure_class::transient`, and the workflow's own edge-level retry
  policy (this section, first bullet) handles it — without taking the workflow down (historical: this
  RFC originally specified Quark 007 actor supervision, restarting a faulting executor's actor,
  bounded, with escalation; ADR-037 removed Quark and, with it, the actor-restart mechanism. There is
  no restart-budget mechanism today because `ExecutorBody` is a plain function call with no
  persistent per-executor state a restart would recover — a real, deliberate narrowing, not a
  same-shape replacement. See `rt/workflow_supervisor.hpp`'s file banner and
  `AgentEngineSpecification.md` §7).
- **Partial results are preserved** — a failed workflow's completed executor outputs are available
  in its final state, not discarded.

## 7. Visualization and introspection

The graph is data: it renders (Mermaid/DOT), it validates, it diffs across versions, and a running
workflow exposes a live view of executor states, in-flight messages, and round number. A workflow
that cannot be drawn is a workflow nobody can review.

A running workflow is visible at two grains:

- **The live view** (`enable_live_view()`) is one summary per superstep: which executors ran, what
  was in flight, the round number.
- **The event stream** (`enable_event_stream()`, `ADR-152`) reports each routing, fan-out/fan-in,
  request-port, checkpoint and merge decision as the supervisor makes it. It also forwards each
  node's own streaming output live (an agent node's per-token `RunEvent`s, a moderator's deltas),
  including from inside `sub_workflow` nodes (`ADR-157`), tagged with the path of nested node ids.
  The vocabulary, its two delivery mechanisms and what a consumer may rely on are in 013 §1.1.

Both are observation only. Nothing a consumer reads from either may feed a routing or permission
decision (I2/I3).

## 8. Promotion gate

- **G1** — each §3 pattern is a runnable sample producing correct results under injected executor
  failures and delays.
- **G2** — checkpoint/resume: kill at each superstep boundary of a 20-node workflow; every resume
  completes with output identical to the uninterrupted control.
- **G3 (determinism)** — shuffling intra-round executor scheduling across 10³ seeds produces
  identical workflow output; an intentionally order-dependent executor is detected by the same test.
- **G4** — type-mismatched edges fail to build with a specific diagnostic, in both the C++ and the
  declarative form.
- **G5** — a workflow suspended at a request port holds no activation, no sandbox, and no connection
  (measured), and resumes correctly after a process restart.
- **G6 (§9 Q4, cross-node checkpoint consistency)** — a workflow with executors placed on ≥3 distinct
  cluster nodes, killed at a superstep boundary via a node failure injected between checkpoint-pending
  and checkpoint-committed, resumes with output identical to the uninterrupted control; a failure
  injected strictly before checkpoint-pending is observed leaves no partial/ambiguous checkpoint —
  resume falls back to the prior committed one. (Historical: this gate assumed the multi-node cluster
  placement this RFC originally specified over Quark. ADR-037 removed Quark, and AgentEngine has no
  multi-node cluster story at all — a real, permanent gap, not a renamed mechanism, per
  `AgentEngineSpecification.md` §7. This gate is currently moot/unattainable as written; it is left
  here as a record of the cross-node consistency reasoning in §9 Q4, not as a live promotion
  criterion.)

## 9. Open questions

- ~~**Q1** — Should the single-agent turn loop *be* a workflow (001 Q1)? Uniformity versus
  overhead.~~ **Resolved, No (OQ-2, 2026-08-04):** see 001 §10 Q1 for the full reasoning — merging
  would invert the dependency direction between the two RFCs (§1's `Executor = an agent | ...`
  already builds workflows *from* agents) and would route the dominant single-agent path through
  supervising-actor and typed-edge machinery it cannot exercise. The uniformity this question wanted
  — one checkpoint story, one replay mechanism — is achieved at the 019/013 layer instead (turn
  boundaries and superstep boundaries are peer checkpoint-boundary kinds), without collapsing the
  execution models themselves. **Same reasoning applied directly, not just by analogy (issue #54,
  decisions/ADR-167-plan-execute-mode.md, 2026-09-03):** single-agent Plan/Execute mode (MAF's
  Harness "Agent modes" row) does NOT route through §3's Planner (Magentic) pattern as a degenerate
  one-node case — it's built at the 002/005 layer instead (`ContextProvider` + `PolicyDecider`
  composition, `core/plan_execute_mode.hpp`), the identical "don't pay supervising-actor/typed-edge
  cost for what a single agent doesn't need" logic this Q1 answer already established.
- ~~**Q2** — Cyclic graphs: required for reflection patterns, and the hard part of static validation.
  Current position is "cycles allowed with a mandatory bound"; the validation story is incomplete.~~
  **Resolved — the story was already complete, just not stated for the cyclic case (2026-08-04):**
  no new validation category is needed. **Type-checking** is already local and pairwise (§1: "an
  edge that connects incompatible types fails to build") and doesn't care whether the edge it's
  checking happens to close a cycle — the loop-closing edge is validated exactly like any other.
  **Bound enforcement** is already global, not per-cycle: §2's round counter is a whole-workflow
  clock ("all messages delivered in round *n* are processed before round *n+1* begins"), so
  `MaxRounds` transitively bounds every cycle's iteration count by construction — a cycle cannot
  iterate more times than there are total rounds, so there is no graph shape that structurally
  bypasses the bound. The "incomplete" feeling came from these two facts never having been stated
  together for the cyclic case specifically; nothing about them changes for it.
- ~~**Q3** — Dynamic graph mutation at runtime (adding an executor mid-run) — powerful, and it breaks
  the "snapshot the graph per run" property that makes replay sound.~~ **Resolved, No — the graph
  stays fixed per run (2026-08-04):** true topology mutation would turn "the graph" into versioned,
  replayable state in its own right (what did it look like at checkpoint *k*?) — a materially bigger
  feature than anything else here, for needs that turn out to already be served without it. The two
  real motivations dissolve on inspection: **data-driven fan-out cardinality** (Map-reduce, §3) is
  already a runtime instance count, not a change to "the graph" as §1 defines it (executors/edges are
  typed *kinds*; how many parallel instances a fan-out spawns is orthogonal to which kinds exist).
  **Open-ended agent invocation** ("call an agent kind the graph wasn't wired to") is already
  `AgentCall` (007 §3) — an ordinary, capability-gated tool call from inside an executor, not a
  graph-structural change. An author who wants the topology itself to differ builds two workflows and
  routes between them from an outer decision; "the graph" stays the one stable, checkpointable,
  drawable (§7) thing this RFC promises. This also resolves Q6 below, which was explicitly deferred
  to this answer.
- ~~**Q4** — Distributed workflows spanning nodes: Quark makes it possible; the checkpoint consistency
  model across nodes is unspecified.~~ **Resolved — the superstep barrier already is the
  distributed-consistency mechanism (2026-08-04):** a round boundary already requires the supervising
  actor to have observed every round-*n* executor's completion, regardless of which node hosts it —
  that's a precondition §2's fan-in semantics already needs to be well-defined, not a new
  requirement. Checkpointing at that boundary (§5) is therefore checkpointing at a point already
  known to be globally quiescent; no distributed-snapshot protocol (Chandy-Lamport or similar) needs
  inventing. What's added, concretely: the checkpoint commits in two phases from the supervising
  actor's view — mark checkpoint-pending once every round-*n* completion is observed, wait for each
  executor's own per-actor persistence (node-local, already durable regardless of placement) to
  confirm, then mark the checkpoint committed — the same intent-then-confirm discipline 019 §3 already
  applies to effects, applied here to the checkpoint itself. A node failure between "pending" and
  "committed" leaves an incomplete checkpoint that resume treats as "hadn't happened," never as
  ambiguous partial state. This is the same question as 019 §8 Q3 (now resolved there too, pointing
  back here) — one resolution, not two. (Historical: this reasoning, and the "distinct cluster nodes"
  premise it and G6 above depend on, assumed Quark's multi-node placement and the Quark 012 `Store`
  seam it persisted through. ADR-037 removed Quark; AgentEngine has no multi-node cluster story today
  — the resolution's *logic* still holds for a hypothetical future cluster, but nothing here currently
  executes across nodes, per `AgentEngineSpecification.md` §7.)
- ~~**Q5** — §3's pattern table has no row for MAF's fifth named orchestration pattern, Magentic.~~
  **Resolved:** given its own row, **Planner (Magentic)**, distinct from Group chat/debate — see §3.
  Grounded in `docs/research/2026-maf-orchestration-patterns.md` (2026-08-04): the graph shape is the
  same cyclic moderator as Group chat/debate (Microsoft's own docs say so directly), but the defining
  property — the moderator owns its own completion decision and self-triggers a replan on stall,
  bounded only by safety-valve counters rather than a caller-authored round contract — is exactly the
  "hand it a goal and it finishes without babysitting" experience §3's other rows don't offer. No new
  engine primitive: cyclic graphs with a mandatory bound (§2) and a request port for optional human
  plan review (§4) already express it, consistent with §3's "patterns are graph configurations, not
  separate subsystems." Naming it earns the row anyway, per 027 §1's rule to adopt MAF's name wherever
  the concept is genuinely the same.
- ~~**Q6** — Mid-run amendment of a running executor's goal/instructions (as opposed to routing a new
  *message* to it, already supported).~~ **Resolved, queue and apply at the next superstep boundary,
  never interrupt (2026-08-04):** once separated from Q3 (a goal update is a *message* to an existing
  executor, not a topology change, so Q3's "no mutation" answer doesn't actually block it), the
  tension collapses on its own terms: interrupting a turn already in flight would violate 001 §3's
  "every iteration is a checkpoint boundary" at the agent level, the identical reasoning that closed
  Q3 at the graph level. §2's superstep model already delivers typed messages to executors as
  ordinary graph edges landing at the next round — a goal update needs no new primitive, it is that
  mechanism carrying updated goal content, applied when the executor's own turn loop next reads it.
  No MAF precedent supports live-editing either (neither SDK live-edits a running executor's
  instructions), consistent with not inventing one here.
