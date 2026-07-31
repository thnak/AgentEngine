# 001 — Execution Model

**Status:** Draft · **Depends on:** Quark 001/002/006/015/018 · **Gate:** §9

## Goal

Define how an agent application *runs*: what a run is, what executes it, what may happen
concurrently, and how cancellation, deadlines, and failure behave. Everything else in this
specification assumes this model.

## 1. The actor mapping

AgentEngine does not implement a scheduler. It maps its concepts onto Quark's and inherits Quark's
proven invariants.

| Concept | Quark realization |
|---|---|
| **`AgentSession`** | One actor instance, key = `session_id`, `Sequential`, durable via Quark 012 (027 §7: the type is `AgentSession`; "session" stays fine in prose) |
| **Run** | An `Ask<StartRun, RunResponse>` to the session actor; `ask_stream` for streaming |
| **Turn** | A segment of a run's coroutine between model calls |
| **Agent** | An actor *type* + its compiled metadata (002); stateless agents may be `Stateless<N>` pools |
| **Tool invocation** | A message to a tool executor — pool actor, plugin host, or sandbox |
| **Workflow** | A supervising actor owning a graph of executor actors (014) |

**I1 (one session, one executor)** is therefore not something this engine enforces — it is Quark's
single-executor invariant, and AgentEngine adds no locks, no session mutex, and no re-entrancy of
its own. A second concurrent run against the same session queues behind the first in mailbox FIFO
order (see §4 for the alternative the caller may request).

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
- **Streaming does not change the loop**, only the emission: with `ask_stream` the response is
  emitted incrementally over Quark's credit-controlled reply ring (Quark 006/024, ADR-018), so a
  slow consumer applies backpressure to the provider read instead of buffering unboundedly.

## 4. Concurrency

Three distinct concurrency axes, with different answers:

1. **Across sessions** — fully parallel. This is the scaling axis; Quark's scheduler owns it.
2. **Within a run, across tool calls** — parallel *only* when every tool in the batch declares
   `Parallelizable` (006). A batch containing one non-parallelizable tool runs the whole batch
   sequentially in the model's emitted order. Default is sequential: a tool that mutates external
   state is the common case, and silently interleaving those is how agent frameworks corrupt data.
3. **Across runs on one session** — serialized by I1 by default. A caller that genuinely wants
   concurrent runs against shared history must either (a) accept FIFO queueing, or (b) fork the
   session (`fork_session`, producing a new session id with copy-on-write history) and merge
   explicitly. There is no third option: concurrent mutation of one history is not offered.

**Sub-agents** (an agent invoked as another agent's tool) run as *separate runs on separate
sessions*, linked by `parent_run_id`. They inherit the parent's deadline and a **subset** of its
capability set, never a superset (007).

## 5. Cancellation and deadlines

- Cancellation is Quark's `std::stop_token`, propagated into the provider call, the sandbox
  execution, and every outbound protocol request. There is no cooperative-polling-only path.
- Deadlines are **monotonic and propagated as remaining duration** (Quark 018), including across
  nodes and into MCP/A2A calls, so a cross-process agent call inherits the caller's budget instead
  of restarting the clock.
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
- **Supervision** uses Quark 007: a run that violates an invariant fails the run and, per policy,
  restarts or stops the session actor without taking down the node.
- **A sandbox failure (timeout, OOM, crash, escape attempt) is always a structured result**, never
  a host crash and never an exception crossing the seam (008).

## 7. Determinism and replay (I5)

Every nondeterministic input crosses a recorded seam:

| Seam | Recorded | Replayed by |
|---|---|---|
| Provider call | request + response (or stream chunks) | serving from the recording |
| Sandbox execution | inputs, outputs, exit status, artifact digests | replaying outputs |
| Tool invocation (remote) | request + response | replaying the response |
| Clock | monotonic reads at turn boundaries | virtual clock (Quark 014) |
| RNG | seed | reseeding |

A recorded run replays with **no external service contacted**. This is the substrate for the
testing model (022) and for post-incident analysis; it is also why the content-capture privacy
controls in 016 apply to recordings as strictly as to telemetry.

## 8. What executes where

| Work | Executes on | Why |
|---|---|---|
| Turn loop, context assembly, middleware | Session activation (Quark worker) | cheap, non-blocking, ordered |
| Provider HTTP/stream I/O | Async coroutine over the PAL event loop | no thread per run |
| Native in-process tool | Session activation if declared `Fast`; pool actor otherwise | avoid blocking the drain path |
| WASM plugin call | Plugin host, on a pooled instance | µs instantiation, no thread |
| Sandbox execution (interpreter) | Out-of-process backend, awaited asynchronously | isolation requires a boundary |
| Blocking/foreign C tool | Quark `BlockingHandler` (ADR-015), per invocation | the one axis stackless cannot serve |

**Rule:** nothing that can block for more than a bounded budget runs on a session activation. A
tool that lies about being `Fast` is a defect caught by the drain-budget test (023).

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

- **Q1** — Should a run's turn loop itself be expressible as a workflow graph (014), making
  "agent" a special case of "workflow"? Attractive for uniformity; risks paying graph overhead on
  the common single-agent path.
- **Q2** — Fork-and-merge semantics for sessions (§4.3) need a defined merge policy, or must be
  restricted to fork-only.
- **Q3** — Should `AuthRequired` be a distinct state, or an `InputRequired` variant? A2A separates
  them; unifying loses a useful distinction for UIs.
