# 001 — Execution Model

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 003, 006, 007 (historical: originally also Quark 001/002/006/015/018 — ADR-037 removed Quark as a dependency, see §1) · **Gate:** §9

## Goal

Define how an agent application *runs*: what a run is, what executes it, what may happen
concurrently, and how cancellation, deadlines, and failure behave. Everything else in this
specification assumes this model.

## 1. The runtime mapping

AgentEngine implements its own runtime substrate, `agentengine::rt::` — plain, host-held C++23
objects and coroutines, no actor engine underneath. (Historical: this section originally mapped
every concept below onto Quark, a distributed actor engine AgentEngine was built directly on top
of; `decisions/ADR-037-remove-quark-as-core-runtime.md`, executed 2026-08-13, removed that
dependency entirely and `third_party/quark` is no longer part of this repository. See
`AgentEngineSpecification.md` §7 for the full current mapping table.)

| Concept | `rt::` realization |
|---|---|
| **`AgentSession`** | A plain templated class instance, one per session, no actor lifecycle; durable via `rt::SessionStore`/`rt::AppendLogStore` (027 §7: the type is `AgentSession`; "session" stays fine in prose) |
| **Run** | `AgentSession::start_run()`, an `rt::task<result<AgentResponse>>` coroutine; `agentengine::stream<T>` for streaming |
| **Turn** | A segment of a run's coroutine between model calls |
| **Agent** | A conforming C++ type + its compiled metadata (002) |
| **Tool invocation** | An ordinary (possibly coroutine) function call through `tool_pipeline.hpp`'s invoke path — no message send |
| **Workflow** | `WorkflowSupervisor`, a plain object driving a superstep loop over a graph of executors via `rt::ThreadPool` (014) |

**I1 (one session, one executor)** is enforced by `rt::AsyncMutex session_mutex_`, acquired for the
whole duration of every public async entry point (`start_run`, `resolve_interaction`) — a
runtime-checked guard, not an actor mailbox's structural exclusivity (historical: this invariant
used to be Quark's own single-executor mailbox guarantee, requiring no locks of AgentEngine's own;
ADR-037's own red-team finding names this as a real, honestly-tracked regression from
structurally-enforced to runtime-checked). A second concurrent run against the same session queues
behind the first on that same lock (see §4 for the alternative the caller may request).

## 2. Run lifecycle

```
Created → Running → { Completed | Failed | Canceled | Rejected }
             ↕
        InputRequired        (human-in-the-loop, MCP MRTR, A2A INPUT_REQUIRED)
             ↕
        AuthRequired         (an effect needs credentials the run does not hold)
             ↕
        Suspended            (durable pause: checkpointed, resumable, no resources held)
```

This state set is chosen to be **exactly A2A v1.0's task lifecycle** (012), so that hosting a run as
an A2A task is a projection rather than a translation. `InputRequired` is the single internal shape
behind three external mechanisms — A2A `INPUT_REQUIRED`, MCP Multi Round-Trip Requests, and the
workflow request/response port (014). Unifying these is deliberate: an agent author writes one
"ask the human/caller something" primitive.

**Correlation identity (resolves OQ-4).** Entering `InputRequired` or `AuthRequired` mints one or
more durable `Interaction{interaction_id, run_id, reason ∈ {input, auth}, opened_at, expires_at?}`
records — one per outstanding "the agent needs something" point; usually one, but a workflow with
multiple concurrent request ports open in different branches (014 §4) can hold several at once. The
`reason` tag is what lets one mechanism serve both states (§10 Q3) without a UI or protocol adapter
needing to guess from context which kind of "waiting" it's looking at. `interaction_id` is the *one* internal
correlation identity; MCP, A2A, and AG-UI each need a differently-shaped external identity to carry
it (013 §2.2, 012 §5a), and each protocol adapter is responsible for the mapping — the run itself
only ever knows `Interaction` records, never a protocol's id shape. A run does not leave
`InputRequired`/`Suspended` for its "waiting" reason until every `Interaction` a given resolution
call names is resolved; whether *all* open interactions on a run must resolve together is an
adapter-level policy (AG-UI's structural "no partial resumes" is the strict case, 013 §2.2), not a
property this state machine enforces uniformly. **Before signaling any pause externally, a run
finalizes and emits pending state** — this is what AG-UI's pre-`RUN_FINISHED` state-emission
ordering (013 §2.2) actually depends on, generalized so every adapter gets the same guarantee, not
only the one protocol whose spec says so explicitly.

**Terminal states are terminal.** A run in `Completed`/`Failed`/`Canceled`/`Rejected` rejects
further input; a continuation is a *new run* on the same session.

## 3. The turn loop

A run's core is a coroutine on the session actor:

```
loop:
  1. assemble context      (instructions, session history, memory, middleware pre-hooks)
  2. call the provider     (004) — the model call, streamed or not
  3. if the response contains tool calls:
        a. resolve policy + capabilities for each call            (006, 007)
        b. request approval if policy demands it — may → InputRequired
        c. invoke, concurrently where the tools declare it safe   (§4)
        d. append results to the session
        e. continue loop
  4. otherwise: finalize, run middleware post-hooks, emit the response
guards: max_turns, deadline, token budget, cancellation
```

**Rules:**

- **The loop is host-driven, not model-driven.** Steps (a) and (b) are host decisions taken against
  pre-registered policy. Nothing in the model's output selects its own permissions (**I3**).
- **Every iteration is a checkpoint boundary** (019). A run may be suspended between turns without
  losing work and without holding a sandbox, connection, or activation.
- **Streaming does not change the loop**, only the emission: the response is emitted incrementally
  over `agentengine::stream<T>`, a credit-controlled producer/consumer pair over `rt::channel<T,E>`
  (historical: originally Quark's own credit-controlled reply ring, ADR-018; ADR-037 replaced the
  backend, the credit-controlled contract itself is unchanged), so a slow consumer applies
  backpressure to the provider read instead of buffering unboundedly.

## 4. Concurrency

Three distinct concurrency axes, with different answers:

1. **Across sessions** — fully parallel. This is the scaling axis; `rt::ThreadPool` (historical:
   Quark's scheduler before ADR-037) owns it.
2. **Within a run, across tool calls** — parallel *only* when every tool in the batch declares
   `Parallelizable` (006). A batch containing one non-parallelizable tool runs the whole batch
   sequentially in the model's emitted order. Default is sequential: a tool that mutates external
   state is the common case, and silently interleaving those is how agent frameworks corrupt data.
3. **Across runs on one session** — serialized by I1 by default. A caller that genuinely wants
   concurrent runs against shared history must either (a) accept FIFO queueing, or (b) fork the
   session (`fork_session`, producing a new session id with copy-on-write history) into two
   independent timelines. There is no third option: concurrent mutation of one history is not
   offered. A fork is fork-only — there is no merge back into one history (§10 Q2).

**Sub-agents** (an agent invoked as another agent's tool) run as *separate runs on separate
sessions*, linked by `parent_run_id`. They inherit the parent's deadline and a **subset** of its
capability set, never a superset (007).

## 5. Cancellation and deadlines

- Cancellation is plain `std::stop_token`, propagated into the provider call, the sandbox
  execution, and every outbound protocol request. There is no cooperative-polling-only path
  (unchanged by ADR-037 — this was never actor-specific).
- Deadlines are **monotonic and propagated as remaining duration**, including into MCP/A2A calls, so
  a cross-process agent call inherits the caller's budget instead of restarting the clock (there is
  no multi-node cluster in `rt::` land — see `AgentEngineSpecification.md` §7 — so cross-*node*
  propagation is out of scope post-ADR-037).
- **A canceled run must still be attributable and finalizable**: partial results, token usage, and
  emitted effects are recorded. Cancellation is a state, not an abort.
- Deadline exhaustion inside a turn terminates the run at the next checkpoint boundary with
  `Failed{deadline_exceeded}`, after cancelling in-flight effects.

## 6. Failure model

- **Errors are values.** `ae::result<T>` throughout; no exceptions for control flow.
- **Failure is classified before it is handled**, into: `Transient` (retryable — provider 5xx,
  network), `Policy` (denied — capability, approval, content policy), `Contract` (invalid tool args,
  schema violation), `Resource` (limit exceeded — tokens, time, memory), `Fatal` (invariant
  violation). Retry applies to `Transient` only; the others are never retried automatically because
  retrying a `Policy` denial is how a loop becomes an attack.
- **A tool failure is not a run failure by default** — it is a tool result the model observes and
  may recover from, bounded by `max_consecutive_tool_failures`.
- **Supervision**: a run that violates an invariant fails the run and returns a
  `JobOutcome{faulted, fault_ptr}` (`rt::ThreadPool::submit()`'s own containment) to the caller,
  without taking down the process (historical: this used to be Quark's actor-restart supervision,
  §7's policy-driven restart/stop; ADR-037's own containment mechanism is per-unit-of-work, not
  actor-restart-shaped — a named, permanent narrowing, not a like-for-like port).
- **A sandbox failure (timeout, OOM, crash, escape attempt) is always a structured result**, never
  a host crash and never an exception crossing the seam (008).

## 7. Determinism and replay (I5)

Every nondeterministic input crosses a recorded seam:

| Seam | Recorded | Replayed by |
|---|---|---|
| Provider call | request + response (or stream chunks) | serving from the recording |
| Sandbox execution | inputs, outputs, exit status, artifact digests | replaying outputs |
| Tool invocation (remote) | request + response | replaying the response |
| Clock | monotonic reads at turn boundaries | virtual clock (historical: Quark 014's mechanism before ADR-037; the recorded-seam contract itself is unchanged) |
| RNG | seed | reseeding |

A recorded run replays with **no external service contacted**. This is the substrate for the
testing model (022) and for post-incident analysis; it is also why the content-capture privacy
controls in 016 apply to recordings as strictly as to telemetry.

## 8. What executes where

| Work | Executes on | Why |
|---|---|---|
| Turn loop, context assembly, middleware | The calling coroutine directly (`rt::AsyncMutex`-guarded, no per-session worker) | cheap, non-blocking, ordered |
| Provider HTTP/stream I/O | Async coroutine over the PAL event loop | no thread per run |
| Native in-process tool | Inline if declared `Fast`; `rt::ThreadPool` otherwise | avoid blocking the drain path |
| WASM plugin call | Plugin host, on a pooled instance | µs instantiation, no thread |
| Sandbox execution (interpreter) | Out-of-process backend, awaited asynchronously | isolation requires a boundary |
| Blocking/foreign C tool | `rt::ThreadPool::submit()`, per invocation | the one axis stackless cannot serve |

**Rule:** nothing that can block for more than a bounded budget runs inline on the session's own
call stack. A tool that lies about being `Fast` is a defect caught by the drain-budget test (023).
(Historical: this section originally described "session activation"/"pool actor" as Quark's own
per-actor scheduling primitives; ADR-037 removed Quark, and `rt::` has no per-actor scheduling — see
`AgentEngineSpecification.md` §7.)

## 9. Promotion gate

This RFC moves Draft → Proven when an executed ADR demonstrates, on Windows and Linux:

- **G1** — 10⁴ concurrent sessions each running a scripted multi-turn run against a mock provider:
  zero cross-session interference, per-session turn order preserved, and per-session footprint
  within the 023 budget.
- **G2** — a run suspended at a checkpoint boundary, with the process restarted, resumes and
  completes with byte-identical output to the uninterrupted control.
- **G3** — cancellation at each of {context assembly, mid-stream, mid-tool-call, mid-sandbox}
  leaves no orphaned sandbox, no leaked capability handle, no unattributed effect.
- **G4** — a recorded run replays with the network disabled and produces an identical event stream.

## 10. Open questions

- ~~**Q1** — Should a run's turn loop itself be expressible as a workflow graph (014), making
  "agent" a special case of "workflow"? Attractive for uniformity; risks paying graph overhead on
  the common single-agent path.~~ **Resolved, No (OQ-2, 2026-08-04):** kept as two distinct
  execution models, not merged. Three reasons, each independently sufficient:
  1. **It would invert or merge the 001↔014 dependency direction.** 014 §1 already defines
     `Executor = an agent | a function | a sub-workflow | a request port` — an agent is one kind of
     thing a workflow is built *from*. Making the turn loop *itself* a workflow graph reverses that:
     001 would need to depend on 014's graph machinery to define what an agent's own run is, while
     014 still depends on 001 to know what an agent *is* as an executor kind. That is a cycle, not a
     layering, unless the turn loop is special-cased as "the graph-of-one that doesn't go through
     the graph machinery" — which is exactly the non-uniform carve-out this question was trying to
     avoid, arrived at anyway.
  2. **The turn loop's placement is deliberately cheap** (§8: "Turn loop, context assembly,
     middleware" on the session activation directly — no supervising actor). 014 §1 builds a
     workflow as "a supervising actor owning [executors]" with typed-edge validation (§1) and
     per-round scheduling obligations (§2). Routing every single-agent run through that machinery
     puts graph bookkeeping on the dominant hot path for a benefit — one visualization, one
     time-travel story — that a one-node graph cannot actually exercise (there is nothing to
     fan-out, fan-in, or route).
  3. **This is also where the developer model AgentEngine tracks already lands.** MAF's own source
     (`docs/research/2026-08-03-maf-workflow-and-hitl-model.md` §1) has a plain agent run never touch
     `Workflow`/`Executor` machinery, with the dependency running the same direction as reason 1:
     `AgentExecutor` wraps `agent.run()` to let an agent become *one node* in a graph, opt-in via an
     explicit builder, not the reverse. 014 §1's `Executor = an agent | a function | a sub-workflow |
     a request port` is already the right shape for that opt-in case and needs no change.

  **What *is* unified, at the correct layer instead:** turn boundaries and workflow superstep
  boundaries are already peer checkpoint-boundary kinds sharing one `Store` and one replay mechanism
  (019 §1), and both project onto the same internal event stream (013 §1). The uniformity the
  candidate resolution was chasing — one checkpointing story, one replay mechanism, one observable
  shape — is achieved there without collapsing the two execution models that produce the
  checkpoints. A multi-agent author who wants graph properties (typed edges, fan-out, visualization,
  time-travel) opts into 014 explicitly by building a `Workflow`; the default single-agent path
  never pays for what it did not ask for. (No implementation exists yet to measure the graph-of-one
  overhead the original candidate resolution proposed benchmarking — this decision rests on the
  layering and source-grounded arguments above, neither of which depends on a measurement; §1's
  overhead concern becomes moot rather than unresolved, since the models are not being merged.)
- ~~**Q2** — Fork-and-merge semantics for sessions (§4.3) need a defined merge policy, or must be
  restricted to fork-only.~~ **Resolved, fork-only, no merge primitive (2026-08-04):** a session's
  two forked branches are alternative *continuations* of a conversation — each assistant turn after
  the fork point was generated conditioned on that branch's own history, not the other's. That has no
  principled three-way-merge semantics the way 025's worktree files do: a worktree conflict is two
  edits to shared mutable state, reconcilable by taking disjoint changes and surfacing real
  collisions (025 §4, OQ-13); a session fork is two equally-valid *futures*, and splicing branch B's
  later messages after branch A's would produce a transcript the model never actually saw when
  generating what comes next in either branch — not a merge, a fabrication. What the engine offers
  instead, and what actually has coherent semantics: the two branches can be **compared** (a diff
  view over the message list from the fork point) so a human or agent can **select** one branch to
  continue as the session's history going forward, or **cherry-pick** specific messages/artifacts
  from one branch as new appended content on the other — an ordinary append, explicitly not a merge.
  No merge operation is offered because none would mean anything.
- ~~**Q3** — Should `AuthRequired` be a distinct state, or an `InputRequired` variant? A2A separates
  them; unifying loses a useful distinction for UIs.~~ **Resolved, distinct state, confirming what §2's
  lifecycle diagram already assumed (2026-08-04):** the two differ in both **UI treatment** (render a
  text input vs. render a credential/OAuth-consent affordance) and **resume trigger** (an ordinary
  content answer vs. a credential becoming resolvable — a `SecretRef` resolving or an operator
  provisioning one), so collapsing them would force `InputRequired`'s generic answer payload to also
  carry credential-resolution semantics it doesn't otherwise need. A2A already keeps `AUTH_REQUIRED`
  and `INPUT_REQUIRED` separate (018 §5), so distinguishing them costs nothing at that mapping and
  keeping them merged would have required *inventing* a translation A2A doesn't need. This isn't two
  state machines: both states mint the same `Interaction` record (§2), now tagged
  `reason ∈ {input, auth}` — one mechanism, one correlation identity, one suspension/resume path
  (019 §2), with the tag carrying exactly the distinction UIs and protocol adapters need.
