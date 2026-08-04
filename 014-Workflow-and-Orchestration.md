# 014 — Workflow and Orchestration

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 001, 002, 005, 013, 019 · **Gate:** §8

## Goal

Multi-agent structure that is **explicit, typed, checkpointable, and inspectable** — a graph of
executors with typed edges — rather than emergent behaviour from agents calling each other. The
shape is MAF's workflow model (executors, edges, fan-out/fan-in, checkpointing, human-in-the-loop,
time-travel), expressed in the CRTP idiom and hosted on Quark actors.

## 1. Model

```
Workflow = { executors[], edges[], start, output_selection, policies }
Executor = an agent | a function | a sub-workflow | a request port
Edge     = direct | fan-out | fan-in | switch/case | multi-selection | chain
```

- **Executors are typed by their input and output message types.** An edge that connects
  incompatible types fails to build — at compile time for the C++ form, at load for the declarative
  form (015), using the same validator (I6).
- **Executors are Quark actors**; the workflow is a supervising actor owning them. Concurrency,
  ordering, and failure isolation come from the runtime, not from a bespoke executor pool.
- **Messages between executors are the content model** (003), so an agent node and a function node
  are interchangeable at an edge.

## 2. Execution semantics

- **Superstep model.** Execution proceeds in rounds: all messages delivered in round *n* are
  processed before round *n+1* begins. This makes fan-in well-defined, checkpoints natural
  (round boundaries), and replay deterministic given recorded nondeterminism (I5).
- **Determinism obligations.** Within a round, executor scheduling order must not affect the
  workflow's output. An executor whose output depends on intra-round ordering is a defect, caught
  by the shuffle test (§8 G3).
- **Termination** is by output selection, by an explicit terminal executor, or by bound
  (`MaxRounds`, deadline, budget). An unbounded workflow does not run — the bound is required.

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
`interaction_id` a set rather than a singleton, resolving OQ-4.

A suspended workflow **holds no resources**: it is checkpointed, its activations passivate, and it
resumes on the response, on a durable reminder (Quark 027), or never — an abandoned workflow is
garbage-collected by policy, not leaked. The request port's `InputRequired` carries the same
`request_id`-shaped correlation token defined in 001 §2; a checkpoint taken while suspended stores
the pending request indexed by that token, matching MAF's own checkpoint/request-info coupling
(`docs/research/2026-08-03-maf-workflow-and-hitl-model.md` §2).

## 5. Checkpointing, resume, and time-travel

- **Checkpoint at superstep boundaries**: executor states, in-flight messages, workflow state,
  and the run's position. Backed by the same store as sessions (005 §2).
- **Resume** restores exactly, on any node in a cluster (Quark placement decides where).
- **Time-travel**: rewind to any retained checkpoint and re-run forward, optionally with modified
  state — the debugging feature that makes multi-agent systems tractable. Retention is policy;
  every rewind is audited, because rewinding a workflow that already had external effects is a
  correctness hazard the operator must own.
- **Effects are not rewound.** Re-running forward re-executes tools; idempotency keys (019) are the
  mechanism that keeps that from double-charging someone. The spec states this loudly because the
  alternative — pretending rewind is safe — is how time-travel becomes a footgun.

## 6. Failure

- An executor failure is classified (001 §6) and handled by the edge's declared policy: propagate,
  retry, route to a fallback branch, or fail the workflow.
- **Supervision** is Quark 007: an executor that violates an invariant is restarted or stopped
  without taking the workflow down, subject to a bounded escalation.
- **Partial results are preserved** — a failed workflow's completed executor outputs are available
  in its final state, not discarded.

## 7. Visualization and introspection

The graph is data: it renders (Mermaid/DOT), it validates, it diffs across versions, and a running
workflow exposes a live view of executor states, in-flight messages, and round number. A workflow
that cannot be drawn is a workflow nobody can review.

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
  resume falls back to the prior committed one.

## 9. Open questions

- ~~**Q1** — Should the single-agent turn loop *be* a workflow (001 Q1)? Uniformity versus
  overhead.~~ **Resolved, No (OQ-2, 2026-08-04):** see 001 §10 Q1 for the full reasoning — merging
  would invert the dependency direction between the two RFCs (§1's `Executor = an agent | ...`
  already builds workflows *from* agents) and would route the dominant single-agent path through
  supervising-actor and typed-edge machinery it cannot exercise. The uniformity this question wanted
  — one checkpoint story, one replay mechanism — is achieved at the 019/013 layer instead (turn
  boundaries and superstep boundaries are peer checkpoint-boundary kinds), without collapsing the
  execution models themselves.
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
  executor's own per-actor persistence (Quark 012's `Store`, node-local, already durable regardless
  of placement) to confirm, then mark the checkpoint committed — the same intent-then-confirm
  discipline 019 §3 already applies to effects, applied here to the checkpoint itself. A node failure
  between "pending" and "committed" leaves an incomplete checkpoint that resume treats as "hadn't
  happened," never as ambiguous partial state. This is the same question as 019 §8 Q3 (now resolved
  there too, pointing back here) — one resolution, not two.
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
