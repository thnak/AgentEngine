# 014 — Workflow and Orchestration

**Status:** Draft · **Depends on:** 001, 002, 005, 013, 019 · **Gate:** §8

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
| **Group chat / debate** | a moderator executor cycling among participants with a bound |
| **Map-reduce** | fan-out over a collection + fan-in reducer |
| **Router** | switch/case on a classifier's typed output |
| **Reflection / critic** | a cycle with an explicit iteration bound |

**Guidance:** an agent-to-agent interaction with structure belongs here, not in a handoff chain
(002 §4). Handoff is for one hop; a graph is for a process.

## 4. Human-in-the-loop

A **request port** is an executor that emits `InputRequired` (001 §2) and suspends the workflow
until a response arrives. It is the same mechanism as tool approval and A2A `INPUT_REQUIRED` — one
shape, four surfaces (013 §5).

A suspended workflow **holds no resources**: it is checkpointed, its activations passivate, and it
resumes on the response, on a durable reminder (Quark 027), or never — an abandoned workflow is
garbage-collected by policy, not leaked.

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

## 9. Open questions

- **Q1** — Should the single-agent turn loop *be* a workflow (001 Q1)? Uniformity versus overhead.
- **Q2** — Cyclic graphs: required for reflection patterns, and the hard part of static validation.
  Current position is "cycles allowed with a mandatory bound"; the validation story is incomplete.
- **Q3** — Dynamic graph mutation at runtime (adding an executor mid-run) — powerful, and it breaks
  the "snapshot the graph per run" property that makes replay sound.
- **Q4** — Distributed workflows spanning nodes: Quark makes it possible; the checkpoint consistency
  model across nodes is unspecified.
- **Q5** — §3's pattern table has no row for MAF's fifth named orchestration pattern, **Magentic**:
  a manager that maintains a task/progress ledger, dynamically assigns work, and can trigger
  replanning — meaningfully more than "a moderator executor cycling among participants with a
  bound" (the closest existing row, Group chat/debate). Undecided whether ledger/replan behavior is
  meant to be business logic inside a moderator executor — consistent with §3's stated philosophy
  that patterns are graph configurations, not separate subsystems, and needs no new primitive — or
  whether it's common enough to deserve its own named row so every application doesn't reinvent
  ledger/replan semantics independently.
- **Q6** — Mid-run amendment of a running executor's goal/instructions (as opposed to routing a new
  *message* to it, already supported). No MAF precedent: neither its Python nor .NET SDK supports
  live-editing a running executor's instructions — both only steer through new messages fixed at
  builder-configuration time. §2's superstep model already delivers typed messages to executors as
  ordinary graph edges, so a goal update that **queues and applies at the next superstep boundary**
  needs no new primitive — same discipline as everything else here. What's open is the same tension
  Q3 already names for dynamic graph mutation: whether a goal update should be able to **interrupt**
  a turn already in flight (powerful, but breaks the snapshot-the-graph-per-run property that makes
  replay sound) versus always waiting for the next boundary (safe, replay-sound, but not "mid-turn"
  in the sense a caller invoking this might expect). Treat as a specific instance of Q3, not a
  separate mechanism, until Q3 itself is resolved.
