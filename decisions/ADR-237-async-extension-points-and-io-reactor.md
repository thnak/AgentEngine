# ADR-237 — Every extension point is asynchronous: the I/O reactor, the threading model, and the seam contract

**Status:** **Draft (2026-10-03). Design, revised after three adversarial red-team rounds (§11: round 1,
6 MUST-FIX + 7 REAL GAP; §12: round 2, 5 MUST-FIX + 7 REAL GAP; §13: round 3, targeted, 3 MUST-FIX + 5
REAL GAP; all answered in the design). Not implemented.** Written at the project
owner's direction ("every component can be written to call the network, sandbox and shell included —
everything should be asynchronous"; conversion clean and complete, not piecemeal; the codebase is
pre-production, so shapes may be redone rather than preserved). Owner decisions recorded in §9
(2026-10-03). Next step: owner review of the round-3 amendments (§13), then implementation on the
integration branch (D2).

**Relates to:** 001 §1/§5/§8, 004 §1, 006 §1/§3/§5/§6b, 008 §2, 018 §4, 020 §3a, 023 (numeric budgets),
027 §4/§5, CONVENTIONS. ADR-037 (the `rt` substrate; named "the minimal executor" as its largest
undesigned risk), ADR-064 (Design A "async `invoke`" and Design D "a real reactor", both deferred — this
ADR is A+D), ADR-160 (parallel tool batch), ADR-175 (resume home), ADR-219 (host resumer), ADR-209
(persistent shell; "cancellation cannot interrupt a running command"), ADR-017 (shared stream
`stop_source`), ADR-178 (session cancel), ADR-181 (background jobs), ADR-070 (delegated decision seam),
ADR-013 (mbedTLS), ADR-011 (resolve once, connect to the verified address), ADR-153 (the `recall`
ceiling, applied to RAG on 2026-10-03).

**Research:** `docs/research/2026-10-03-maf-tool-invocation-async.md` (MAF's tool contract; the seam
inventory §2b/§7; open questions §6), `docs/research/2026-10-03-async-reactor-options.md` (reactor
options, sourced).

---

## 1. The question

Stated so it has a wrong answer:

> How does every seam a conformer implements become `ae::task<…>`, backed by I/O that genuinely
> suspends (sockets, TLS, DNS, child processes, pipes, timers), such that **a suspended run holds no OS
> thread**, **cancellation reaches the in-flight operation in bounded time**, **no engine continuation
> ever runs on a thread that does not own it**, and I1/I2/I3/I5/I8 hold exactly as before?

Wrong answers this rules out: `task<>` signatures over blocking bodies (today's `ChatClient::chat()`);
a thread per in-flight call (today's SSE readers, background tools, parallel batches); cancellation that
is only polled between blocking waits (today's 90 s `select()`); a reactor that resumes coroutines on its
own thread (the ADR-219 hazard, generalised).

## 2. Where the code actually is (verified 2026-10-03, `main` at `beb47b3`)

The specs already describe an async engine; the code never built it. Every row below is a contradiction
this ADR closes, not a new direction.

| Spec says | Code does |
|---|---|
| 001 §8: provider I/O is "async coroutine over the PAL event loop… no thread per run" | No event loop exists (`include/agentengine/pal/` has `console`, `env`, `net` only). `chat()` blocks inside its coroutine body (`protocol/openai/chat_client.hpp:474-497`); `chat_stream()` spawns a **detached `std::thread` per call** (`:514-518`, anthropic `:536-540`). |
| 001 §5: "Cancellation is plain `std::stop_token`, propagated into the provider call, the sandbox execution, and every outbound protocol request. There is no cooperative-polling-only path." | `exchange_json` passes an empty token (`src/protocol/provider_chat_wire.cpp:29`); `HostEgressProxy::fetch` passes none (`net_egress_proxy.cpp:782,784`); a token, when present, is checked only between `select()` waits of up to 90 s; DNS (`getaddrinfo`) and the TLS handshake are uncancellable. |
| 006 §1: `static ae::task<result<Reply>> invoke(…)` | `static result<Reply> invoke(…)` (`core/tool.hpp:200-202`), 16 tools with an always-failing sentinel `invoke` plus a closure (§5.2). |
| 008 §2: `create/exec/destroy` return `ae::task<…>`; "a `stop_token` terminates the guest within a bounded, measured time" | All synchronous (`sandbox/sandbox.hpp:373-378`); ADR-209: "cancellation cannot interrupt a running command". |
| 018 §4: `ae::task<result<SecretLease>> resolve(…)` | Synchronous (`trust/secret.hpp:287-288`). |
| 023: a suspended run holds "zero threads"; per-chunk ≤10 µs, 0 allocations | Every parked `ThreadPool` job pins its worker (`rt/thread_pool.hpp:22-24`); stream drains busy-poll with `sleep_for(5ms)` (`rt/agent_session_trust.hpp:190`, `core/chat_stream_drain.hpp:140`, `core/model_call_gateway.hpp:512`, `core/session_builder.hpp:597`). |

The threading hazards that exist **today** and that this ADR must not carry forward (each cited in the
research agents' reports, re-checked):

- **H-a Detached threads that outlive their owner.** `ModelCallGateway::call_stream` captures `this` on a
  detached thread (`model_call_gateway.hpp:218-274`, its own comment names the use-after-free);
  `background_task` captures a raw `ToolDescriptor const*` into the caller's table
  (`src/core/tool_pipeline.cpp:377,483`); `BackgroundJobRunner::shutdown` detaches unfinished workers
  (`rt/background_job_runner.hpp:583`); recording/replay clients and `workflow_as_chat_client` also detach.
- **H-b Homeless inline resume.** A raw-`resume()`d coroutine that parks is resumed by whoever wakes it
  (`rt/resume_home.hpp:386-399`); a dropped `ParkedContinuation` resumes inline in its destructor
  (`:176`); `channel::next_async` runs the consumer on the producer's (detached SSE) thread.
- **H-c Blocking inside coroutines.** HTTP in `chat()`; `sleep_for` retry backoff (`model_call_gateway.hpp:402,556`);
  `future::get()` inside `WorkflowSupervisor::execute` (`src/rt/workflow_supervisor.cpp:647,825`);
  `run_jobs_bounded` joining inside a round (`agent_session_core.cpp:1114-1131`).
- **H-d Back-pressure blocks the session thread.** `emit_run_event_for` holds `run_event_mutex_` while
  `channel::push` can block on a full channel (`agent_session_core.cpp:951-958`, `rt/channel.hpp:200-214`).
- **H-e Pipes drained after exit.** Native jail (Windows and Linux) waits for the child, then reads its
  pipes (`native_jail_backend.cpp:376-377`, `linux_native_jail_backend.cpp:467-468`): a child writing
  more than the pipe buffer deadlocks until the wall timeout.
- **H-f Unsafe test drivers.** `tests/support/run_task_sync.hpp:80-84` destroys the frame after one
  resume; ~130 test files use `while(!t.done()) t.resume();` — both are wrong the moment a task parks,
  i.e. the moment I/O is genuinely asynchronous.
- **H-g `select()` with `fd ≥ FD_SETSIZE`** is undefined on Linux (`net_egress_proxy.cpp:73-84`).

## 3. Decision (summary)

1. **One rule for seams.** Every concept or host-supplied callable a conformer implements returns
   `ae::task<…>` (or `ae::stream<…>` for incremental results). Exceptions are enumerated in §5.3 with a
   reason each; "deferred" is not a reason.
2. **A reactor is a PAL seam**, `pal::Reactor`, std-only in its interface, with one backend. It wakes
   coroutines; it never runs them (§4.2).
3. **Engine-owned execution is lanes, not blocked threads.** A parked coroutine releases its thread; its
   completion is posted to its home (§4.3). `block_on` survives only at the outer edge of a synchronous
   host and is **refused** on an engine lane.
4. **Structured concurrency.** No detached threads, no detached tasks: every child operation is owned and
   awaited by a parent scope; a session owns a task scope that is joined before it is destroyed (§4.5).
5. **Cancellation is per-operation.** The `stop_token` already in `EffectContext` (`effect_context.hpp:214`)
   is wired into every reactor operation; deadlines are reactor timers, not checks (§4.4).
6. **Truly blocking work is explicit.** `co_await rt::offload(fn)` runs it on a small bounded pool, and its
   uncancellability is a stated residual (§4.6).
7. **One way to declare a tool.** `invoke` becomes an instance member returning `ae::task`; the
   sentinel-plus-closure path and hand-built descriptors for static tools disappear (§5.2).
8. **Conversion is total**, including the "async in name only" seams (`ChatClient`, `Embedder`,
   `RemoteVectorIndex`) — owner decision A8, 2026-10-03.

## 4. System design: threads, homes, cancellation

### 4.1 Thread roles

| Role | Count | Runs | Never does |
|---|---|---|---|
| **Host thread** | host's | `block_on(task)` at the outer edge of a synchronous host (CLI `main`, a test, an embedding that is itself synchronous) — or nothing, if the host drives engine tasks from its own executor via an ADR-219 `Resumer` | — |
| **Lane worker** | small, fixed (default 2, configurable; never `hardware_concurrency()`, CONVENTIONS) | engine coroutines: session rounds, tool bodies, workflow nodes | block on I/O; call `block_on` (refused, §4.3) |
| **Reactor thread** | 1 (backend may use more internally, e.g. IOCP) | completion handlers that **post** a parked continuation to its home | resume a coroutine; take an engine lock; allocate on the hot path beyond the op record |
| **Offload worker** | small, bounded (default 2) | `rt::offload` bodies: `getaddrinfo` fallback, file APIs without async support, foreign C libraries, CPU-heavy pure functions | touch session state except through its own return value |

The CPython interpreter (010) and native-jail worker processes are *processes*, not threads in this table:
they are driven through async pipes (§6.3).

### 4.2 The reactor never runs engine code

The reactor backend completes an operation by **posting** the parked continuation to the home captured
when it parked (ADR-175 `block_on` home, ADR-219 host `Resumer`, or — new — the lane it was running on).
It never resumes inline, even when the operation completes immediately: an immediate completion is posted
too. This closes H-b for every reactor-backed awaitable and removes unbounded symmetric-transfer recursion
on chains of immediate completions.

**The reactor only ever enqueues (revised after red-team M5).** Three existing fallbacks run engine
code inline on whatever thread wakes it, and each would put it on the reactor thread: a closed
`block_on` home resumes inline (`rt/resume_home.hpp:277-279`); a dropped `ParkedContinuation` resumes
inline in its destructor (`:176`); and `request_stop()` runs every registered `stop_callback` inline —
existing callbacks do real work (`child.cancel()` → `bound_cancel_client_interactions()`,
`agent_spawn_child_run.hpp:210`; `WorkflowSupervisor::cancel` settles a suspended run,
`workflow_supervisor.cpp:68`; job-runner cascades). All three are redirected: a closed home or a dropped
continuation is posted to a lane; timer-driven stops are posted to the owning strand (§4.4); and every
existing `stop_callback` body is audited and reduced to "signal / enqueue" (an inventory is part of the
implementation's evidence).

**Engine-started work always has a home; foreign coroutines keep the ADR-219 fallback (revised after
red-team M4).** Every coroutine the engine starts runs on a lane under a strand. Every public entry point
(`start_run`, `resolve_interaction`, `close`, …) begins by hopping onto the session's strand, so a host
that `co_await`s `start_run` from its own coroutine type (020 §3a's documented embedding) does not run
the engine body on the host thread. The home travels in the `rt::task` promise. ADR-219 rejected
"home in the promise" because every coroutine type in a chain must cooperate and leaf awaiters see only a
type-erased `coroutine_handle<>`; that objection holds for **foreign** coroutine types, so the
thread-local `ScopedResumer` stays as the fallback for them and is not removed. Where an engine task
completes back into a foreign continuation, that continuation runs where the host said — its own
`Resumer`, or an engine `CompletionThread` the host created by name — never on a lane (host code may
block and would starve the lanes) and never on the reactor. Decided by D7 (§9): no hidden thread — the host names its choice
through `runtime.enter(task, resumer)`, and a bare `co_await` from a foreign coroutine type does not
compile.

**`final_suspend` posts across homes.** Today `FinalAwaiter` transfers directly to the awaiting coroutine
(`rt/task.hpp:66,157`). Under strands that is wrong whenever the child and the awaiter have different
homes (a child session's `start_run` finishing into the parent's tool body; the last `when_all` child
resuming the parent). Rule: symmetric transfer only when child and awaiter run on the **same strand**
(not merely the same lane pool — sibling and parent strands share lanes, and transferring across strands
would run the parent while its strand may be running something else, breaking I1); otherwise post the
awaiter to its own strand and return `noop_coroutine()`. Recursion stays bounded: at most one post per
cross-strand join (red-team round 2, G-a).

**A foreign continuation whose home is gone** (its `Resumer` destroyed before the engine task
completed) is never resumed and is reported; the engine chain it was awaiting is **adopted** by the
runtime (§4.5 rule 3) and finishes normally. A `CompletionThread` cannot vanish this way: its destructor
joins, and destroying it with work outstanding is a checked violation (round 3). "A closed home is posted to a lane" above applies to engine
homes only.

### 4.3 Lanes, I1 and the session strand

A **lane** is a queue of ready strands served by the lane workers (round-robin across strands, FIFO within one — see "Scheduling" below). A parked coroutine is not on any
lane; it costs its frame and its operation record, nothing else (023: zero threads per suspended run).

I1 today is a runtime `AsyncMutex` (`session_mutex_`, `agent_session_core.hpp:1430`) held across every
public entry point. That remains the authority. In addition, each session gets a **strand**: a serial view
of the lanes — at most one of the session's continuations is runnable at a time, in FIFO order. The
strand makes "one session, one executor" structural for continuations; the mutex keeps guarding entry
points (a host may call `start_run` from anywhere). A session's continuation may resume on a different
lane worker than it parked on; nothing in the session may depend on thread identity (holder ids are
already per logical task, ADR-175 — and move to the promise, §4.2).

Parallel tool batches (ADR-160) become **child strands** under the session's task scope:
`co_await rt::when_all(children)`; each child gets its own `EffectContext` copy exactly as ADR-160 §5
builds it; results are appended in emitted order after the join (006 §5, I5). The bounded `jthread`
fan-out (`rt/bounded_call_fanout.hpp`) is deleted; the concurrency cap becomes a semaphore on the scope.

**Holder identity and re-entry (red-team G1, revised after round 2 M4).** Holder ids move to the
promise with an **await chain**: a link exists only along an *awaited* (joined) edge — a coroutine that
`co_await`s another, or a `when_all`/`with_scope` child of the scope's owner. Background jobs and tasks
spawned into a job runner's scope start a **fresh** chain (they are not awaited by the round that started
them, so they wait for the lock like any other caller). Lock rules on `AsyncMutex`:
- **Lending, read-only entry points only (revised after round 3, M1).** When the holder is suspended
  *awaiting* the requester's chain, it lends the lock to that descendant **only if the entry point is
  declared lend-safe** — read-only ones: `fork_from_async` (today's deliberately re-entrant `fork_from`,
  `rt/agent_session.hpp:351-356`, ADR-123/175, only copies), `snapshot_record`, history reads. A mutating
  entry point (`start_run`, `resolve_interaction`, `start_background_task`, `schedule_wakeup`, `close`)
  never borrows: a nested round inside the outer round's pending tool call would corrupt the
  tool_call/tool_result pairing and break I1.
- **Loan rules (round 3, gap 4).** One borrower at a time; further borrowers under the *same* lender wait
  in a **loan queue** served before outside FIFO waiters (so `when_all` siblings that each fork queue,
  rather than one failing at random). Releasing a loan returns the lock **to the lender**, never to the
  FIFO head — otherwise a queued `resolve_interaction` would take a lock the lender still believes it
  holds (mutant-tested). A lent guard is bound to the borrower's frame: reaching `final_suspend` with the
  loan outstanding (e.g. the guard moved into a background scope) is a checked violation. Nested loans
  form a stack.
- **Refusal.** A *mutating* entry point requested while an awaiting ancestor holds the lock, or any
  entry point requested while a non-awaiting ancestor holds it, fails with `rt.lock_held_by_ancestor` —
  the session → workflow → agent node → tool → back-into-the-parent cycle becomes a diagnosable error,
  not a run parked forever with no blocked thread to see.
- Everyone else waits, as today. A **stuck-run watchdog** (no progress on a strand past a configured
  bound) reports what neither rule sees.

`block_on` on a lane worker is refused with `rt.block_on_on_lane` (it would park a worker waiting for work
that may need that worker — the ADR-175 §6 "parked pool job holds its worker" deadlock). Today nothing
refuses it (no check exists in `rt/`); the refusal is new and has a positive-control test. The same
refusal applies on offload workers. `block_on` is not the only way to block a lane (red-team G7):
`future::get` (`workflow_supervisor.cpp:647,825`), condition-variable waits, a `std::mutex` held across a
`co_await` (undefined once the coroutine can resume on another thread), and slow sinks. These are removed
from engine code by the conversion, and a **lane-stall watchdog** (a lane worker that has not returned to
its queue within a bound) reports what review misses.

**Scheduling (revised after rounds 2 and 3).** Each lane serves **sessions round-robin first, then the
session's strands round-robin** (a session's `when_all` children are strands of that session), not one
global FIFO — so a 50-child batch gets its session's share, not 50 shares, and cannot starve other
sessions. Within a strand, order is strictly
FIFO — priority never reorders a strand's own continuations (that would break I1's ordering). Cancellation
and approval resumes get priority **across** strands only: the lane picks a strand with a pending
cancel/approval first. A body that is already running cannot be preempted; a cancel waits for its next
suspension point, which is why CPU-heavy work belongs in `rt::offload` (§4.6) and why §8.2 gate 3 is
measured under load.

### 4.4 Cancellation and deadlines

- **Per operation, two-phase (revised after red-team M1).** Every reactor awaitable takes a stop token
  and registers a `std::stop_callback` whose body only **posts a cancel request to the reactor thread**
  (never calls the backend from the requesting thread — Asio objects are not thread-safe, and the
  requester is often a host thread). The reactor issues the cancel (IOCP: `CancelIoEx`; epoll:
  deregister). **The continuation is posted only when the backend's own completion arrives** — never
  when the cancel is requested — because until then the kernel still owns the `OVERLAPPED` and the
  buffer. The outcome is the backend's status: an operation that finished before the cancel landed
  reports its real result (bytes read, bytes written, effect done), so I4 audit records stay true.
  The operation record is shared between the frame and the backend (intrusive refcount, pooled for 023)
  and is freed by whichever releases last. A socket or TLS connection whose operation was canceled is
  unusable afterwards (partial record/frame) and is closed.
- **Per-operation stop sources (red-team M5).** A tool-call or operation deadline must not cancel the
  whole run: each operation gets its own `std::stop_source` linked to the caller's token; the deadline
  timer stops the *operation's* source, and that request is **posted to the owning strand**, never run on
  the reactor thread. Cost: one linked source per operation, pooled, counted in §8.2 benches.
- The awaiter then resumes with an `error` value — errors are values (001 §6). Which `failure_class` a cancellation
  carries: `failure_class::canceled` for a requested stop, `resource` + `deadline_exceeded` for a
  deadline (§9 D6).
- **One serial context per I/O object (revised after round 2, M2).** *Initiation*, completion
  handling and cancellation of every operation on a given socket, pipe, process or timer run on the same
  serial context — an Asio strand per I/O object (or the reactor thread). A lane never calls into an I/O
  object directly: `await_suspend` posts the initiation to the object's strand. This removes the race of
  a lane starting `async_write` on a TLS socket while the reactor thread handles that socket's read
  completion or cancels it (Asio: shared I/O objects are unsafe).
- **Sticky cancel.** "Cancel requested" is a flag in the operation record, set by the posted cancel and
  checked at initiation: a stop that lands after the `stop_callback` is registered but before the backend
  holds the operation still cancels it (immediately, as `canceled`), instead of being lost and letting the
  operation run to its natural end.
- **Exactly-once resume.** The continuation is posted exactly once: from the backend completion when
  the backend held the operation, or from the initiation path when a sticky cancel means the operation
  was never started. The cancel request itself never posts it. The record is shared between frame and backend (refcount) and
  freed by whichever releases last.
- **Deadlines are timers.** `EffectContext::deadline` (`effect_context.hpp:54`) is enforced by racing the
  operation against a reactor timer that fires the same cancellation, not by a check before the call.
  `ModelCallGateway` backoff becomes `co_await rt::sleep_until(t, stop)`.
- **Process cancellation** kills the job/process group and closes the pipes; the awaiting read completes
  with `canceled`; the exit wait completes when the process is gone. Bounded time is a measured gate (§8).
- **Approval waits** keep the `InputRequired` suspend-and-resume shape (ADR-029/057): a human approval is
  not an awaited operation holding `session_mutex_`.

### 4.5 Structured concurrency — the lifetime rules

1. **No detached threads, anywhere** under `include/` and `src/`. Enforced by a lint (`std::thread(...)
   .detach()`, `std::thread` construction outside `pal/` backends and `rt/` lanes/offload) and by H-a's
   sites being rewritten.
2. **No detached tasks.** A child task is started only inside a `rt::task_scope`, and the scope's owner
   `co_await`s `scope.join()` before it is destroyed. A session owns a scope for background tools
   (006 §6b), standing effects and its parallel batches; `AgentSession` shutdown cancels the scope and
   joins it. ADR-181's "one detached thread per background task; nothing joins them" becomes a scope.
3. **A frame with an in-flight operation is never destroyed — enforced, not assumed (revised after
   red-team M2).** `~task()` destroys frames unconditionally today (`rt/task.hpp:95,205`), and two
   contracts *rely* on drop-to-cancel (ADR-017: dropping a `stream` cancels it; ADR-219: a host may drop
   a parked coroutine). The design therefore:
   - counts in-flight operations **along the whole awaited chain** (a leaf's operation increments every
     frame up to the root that is awaiting it), because `~task()` runs on the *outer* frame while the
     operation usually sits several awaits deep (round 2, M1). Destroying a non-done frame whose chain
     count is non-zero is handled at the **root**, not the leaf (revised after round 3, M2):
     - **The host drops its awaiter** (`runtime.enter` / `runtime.run`, or a dead `Resumer`): the
       runtime **adopts the whole engine chain** into its own scope, requests stop, and lets it finish
       normally — every referent is still alive because the chain is intact, so locks
       (`session_mutex_`, taken at `agent_session_core.cpp:102`), quota tickets, sandbox refunds and
       shell slots are released by their ordinary destructors. Adopted chains are joined at shutdown
       and reported. Leaking a frame that holds `session_mutex_` would block every later entry point on
       that session forever, which is why leaking is not the answer here.
     - **A non-root frame destroyed by its owner** while its chain has operations in flight can only
       happen by bypassing `with_scope`/`when_all` — a bug, so it aborts (every build) with the frame's
       origin. A reaper that "cancels and joins" a detached subframe was rejected (round 2): it resumes
       or destroys a frame whose referents are gone;
   - **changes drop-to-cancel for parked engine coroutines** (ADR-219): dropping a parked root task is
     adoption (above) — the work is stopped and finished cleanly, not torn down. **Dropping a `stream` consumer
     stays legal** and remains a stop request (ADR-017, `core/stream.hpp:163`), because under rule 3a
     the consumer handle owns no in-flight operation — the producer lives in a scope the caller passed in
     and is joined there (round 2, G-e). ADR-219 gets an amendment note;
   - offers scopes only as `co_await rt::with_scope([&](rt::task_scope& s) -> task<…> {…})`, which joins
     every child before returning *or rethrowing* (C++ has no async destructor, so a scope object that
     could be unwound over live children is not offered);
   - specifies `when_all`: on the first failure, request stop on every sibling, **join all of them**,
     then propagate.
3a. **Owners of async resources close asynchronously (red-team M3).** Anything that owns in-flight work
   or a reactor resource exposes `task<void> close()`; its destructor on a not-closed object with live
   work is the same checked violation as rule 3. This covers `AgentSession` (including child sessions
   built and destroyed inside tool bodies — `rt/agent_spawn_child_run.hpp`, `rt/agent_workflow_executor.hpp`),
   streams and their producers (a `chat_stream` producer is spawned into a scope **the caller passes
   in**, never one hidden inside the stream), sandbox providers (today `MandatorySandboxProvider::operator=`
   calls `block_on`, `mandatory_sandbox_provider.hpp:748`, and refunds via `block_on` at `:618-619`),
   and `fork_from` (synchronous `block_on`, `rt/agent_session.hpp:356`), which becomes `fork_from_async`.
   So that early returns, `break`s and exceptions in tool bodies do not each become a violation, owners
   are used through scoped helpers shaped like `with_scope` — `co_await rt::with_session(factory, body)`,
   `co_await rt::with_stream(…)` — which `close()` on every exit path before returning or rethrowing
   (round 2, G-e).
4. **Shutdown order is fixed:** cancel all session scopes → join them (bounded by a shutdown deadline,
   overruns are a reported error, not a detach) → stop lanes → stop the reactor. A reactor destroyed while
   operations are pending is a contract violation caught in debug builds.
5. **Back-pressure is a suspension, not a block — except where the producer is synchronous (revised
   after red-team M6).** `channel::push` gains `co_await producer.push(v)`; `stream<T>` exposes
   `co_await s.next()`; the 5 ms polling drains are deleted. But `EffectContext::report_progress` and the
   other event sinks are synchronous callables (`effect_context.hpp:115`) called from tool bodies,
   including concurrently from parallel-batch children, and the channel is single-producer. So the run-
   event path is split by who produces the event (revised after round 2, M3):
   - **Lifecycle events and model/tool deltas** are produced by the pipeline and the model-call path,
     which are coroutines: they `co_await` a real push into the per-session event stream. Back-pressure
     suspends the run (no thread is held — H-d gone); nothing is coalesced or dropped, because a
     `model_delta` is a *fragment* of the transcript (it becomes AG-UI text content,
     `protocol/agui/projection.hpp`), not a snapshot. A slow consumer pauses the run, with three
     guarantees so that a paused consumer never makes cancellation impossible (round 3, M3):
     1. the awaited push **races the run's stop token** — a stop resumes the pusher;
     2. one slot is **reserved for the terminal event** (`run_completed`/`run_failed`/`run_canceled`), so
        the run can always end; after a stop, non-terminal events go to the recording only;
     3. a **consumer-stall deadline** (configurable) fails **the stream** with `resource`, not the run;
        the run continues without that consumer and its events are still recorded.
     Positive control: cancel a run whose consumer never reads; the run ends within the cancellation
     bound. This also covers `SessionBuilder::ask_stream`'s session-wide stream
     (`core/session_builder.hpp:520-531`) and hosts that drain only after the run returns
     (`tests/core/chat/test_stream_retry_real_transport.cpp:232-236`).
   - **Synchronous sinks called from tool bodies** (`report_progress`, the delegated sinks) enqueue into a
     per-session bounded multi-producer queue drained by one session task into the same stream. Only
     *idempotent snapshot* events (progress percentage, status text) are allowed through this path, and
     they coalesce latest-wins per call id on overflow. A sink that would need lossless delivery is
     converted to an awaitable by this ADR.
   - **I5 placement:** recording taps lifecycle events **before** either queue, in pipeline order, so what
     is recorded never depends on how fast the consumer drains. Progress snapshots from concurrent sync
     sinks have no deterministic order and are not an I5 seam. Before pushing a call's
     `tool_call_finished`, the pipeline flushes or drops that call's pending snapshots, so a snapshot is
     never delivered after its call finished (round 3, minor).

### 4.6 Truly blocking work: `rt::offload`

`co_await rt::offload(fn, stop)` runs `fn` on an offload worker and posts the result to the caller's home.
Used for: `getaddrinfo` where no async resolver exists on the platform, file APIs (Windows file I/O can be
overlapped and later moved to the reactor; POSIX regular-file I/O cannot be made non-blocking with epoll),
CPython/PDFium calls that block, and CPU-heavy pure work that would starve a lane.

**Residual (stated, as MAF states it for `asyncio.to_thread`):** an offloaded body that is already running
cannot be interrupted; cancellation resumes the *caller* with `canceled` and the body's result is
discarded on completion, but its side effect may still happen. Rules: an offload body never holds a
capability beyond its bound one (I2), its completion is attributed in the audit record even when discarded
(I4), and tools whose effect class is `at_most_once` must not perform their effect inside an offload body
they cannot cancel — they use reactor-backed I/O instead.

**Offload lifetime rules (red-team G3).** A canceled-but-still-running body is not a detached task: it is
owned by the offload pool's own scope, counted, and joined (with a reported overrun) at shutdown.
Offload bodies take every input **by value** — the API accepts a callable plus moved arguments and
rejects lambdas capturing by reference at compile time where detectable — because the caller may resume
with `canceled` and unwind before the body finishes. DNS gets its **own** bounded pool, separate from file
I/O and foreign calls, so a burst of slow lookups (glibc `getaddrinfo` can take ~30 s) cannot stall the
file stores of every session.

### 4.7 Determinism (I5)

Timer expiry and I/O completion order are nondeterministic. Recording sits **above** the reactor, at the
existing seams (model calls, tool results, clocks): replay never starts a reactor operation. A virtual
`pal::Clock`/timer queue is part of the test reactor (§8.1) so replay and unit tests run without wall time.

## 5. The seam contract

### 5.1 Signatures

Every row of research §7a's "must become async" tables changes to return `ae::task<result<T>>`
(or `ae::task<T>` where it cannot fail, `ae::stream<T>` where it is incremental). In particular:
`Runner::run`, `SandboxBackend::create/exec/destroy`, `ExecutionSurface`, `PersistentShellSurface`,
`NetEgressBackend::fetch`, `SecretStore::resolve`, `WorktreeObjectStore`, `AppendLogStore`,
`SessionStore`, the index concepts (`VectorIndex`/`SparseIndex` merge into the already-async
`RemoteVectorIndex` shape — the local/remote split only existed because one side was synchronous),
`SkillSource::load_skills`, `ChunkingPolicy::chunk`, `ApprovalDecider`/`PolicyDecider`, `ExecutorBody`,
`MergeOnJoinHook`, MCP `RequestSender`/`InputRequestHandler`, A2A `CardFetcher`/`RunStarter`/client calls,
`ChildRunner`, `SessionFactory`, `ToolSourceFetch`, `GraderFn`, `OutputValidator`, `SurfaceFactory`,
`Resolver`. Added by `main` after this ADR was drafted (ADR-235/236, merged into the branch 2026-10-03):
`BatchBackend::submit/poll/cancel/release` (`core/batch_backend.hpp:91-98`, a `virtual` class whose
methods call the vendor's batch API synchronously) and `WorkflowSupervisor`'s batch callables
`Build`/`Call`/`Complete` (`rt/workflow_supervisor.hpp:291-295`). `BatchBackend::limits()`/`admit()`
are local checks and stay synchronous (§5.3). ADR-236's batch recording sink (`Sink`, `DivergenceHook`)
is an event sink under §5.3's non-blocking rule. `ChatClient` gains a real `chat()` again (004 §1 lists both) or drops it in favour of
`chat_stream` only — decided in the implementation, not left as a blocking body.

ADR-070's property "unset = today's behaviour" is preserved for the deciders: an unset async decider is
the same fail-closed default, now returned by an immediately-ready task.

### 5.2 One way to declare a tool

```cpp
struct RunCommandTool : Tool<RunCommandTool, Capabilities<cap::decl::RunCommand>> {
    static constexpr std::string_view name = "run_command";
    static constexpr std::string_view description = "...";
    using Args = RunCommandArgs; using Reply = RunCommandReply;

    explicit RunCommandTool(MandatorySandboxProvider& p) : provider_(&p) {}   // state, if any
    ae::task<result<Reply>> invoke(Args, EffectContext&);                    // instance member
private:
    MandatorySandboxProvider* provider_;
};
// registration: ToolTable::from(RunCommandTool{provider}, WordCountTool{}, ...)
```

- `invoke` is a non-static **`const`** member returning `ae::task<result<Reply>>`; a stateless tool is an
  empty object. `const` because a `Parallelizable` tool called twice in one batch runs two `invoke`s on
  the same object concurrently (red-team G2); mutable per-call state lives in the frame, shared state
  behind the tool's own synchronisation. This amends 006 §1 (`static`).
- `make_tool_descriptor_with_invoke` and every sentinel `invoke` (16 tools, research §5) are deleted.
- `captures_session_state` stays an **explicit declaration** (a policy tag), not derived from "the type
  is non-empty": deriving it would make every configured tool unbackgroundable
  (`background_job_runner.hpp:377`) and not re-run comparable (`tool_descriptor.hpp:169`) — including this
  section's own example (red-team G2).
- Tool instances are held by **shared ownership**. ADR-181 made background jobs outlive sessions by
  construction (`ADR-181…md:88`) and recorded borrowed pointers becoming use-after-free (C6, `:374`); a
  tool instance borrowed from a session's `ToolTable` would reintroduce that. The background scope of
  §4.5 rule 2 is therefore the **job runner's** scope (ADR-181), not the session's.
- **I2 for injected state:** a handle injected through a tool's constructor may expose effect methods
  only if they take a bound capability from the call's `EffectContext`; a constructor-captured handle
  that performs an effect without one is the ADR-153 bypass in a new shape. Reviewed per tool, with the
  RAG `recall` fix (2026-10-03) as the reference.
- Hand-built `ToolDescriptor`s remain only for tools whose schema is known at run time (MCP bridge, WASM
  bridge, eval stub). That path takes every policy field explicitly (no silent `never_require`/empty
  ceiling), closing the ADR-028 warning the RAG `recall` tools fell into.
- The ~11 static tools built by hand today (`todo_provider` ×5, `plan_execute_mode`, `memory_provider`,
  the two RAG providers, the two native-process providers) move to `Tool<>`.
- The native-process providers' empty ceiling plus in-body grant matching
  (`native_providers.hpp:158-181,225-230`) is replaced by a per-call requirement derived from the
  arguments and bound by the pipeline (§9 D3).

### 5.3 Seams that stay synchronous, each with its reason

- Pure computations over in-memory values that no conformer can make remote: `ContentReplayTrigger`,
  `DescriptorFilter`, `JitterSource`, capability binding, JSON/schema.
- Trivial accessors: `capabilities()`, `traits`, `origin_id()`, `is_live()`.
- Event and audit sinks returning `void` (`RecordingSink`, `BackgroundJobSink`, `EmitFn`, `*TraceHook`,
  `*AuditHook`): stay synchronous **and must not block** — contractually an enqueue; a sink that writes to
  the network owns its own queue and drains it in its own task. A sink that blocks is a defect caught by a
  test that asserts round latency with a deliberately slow sink.
- Clocks (`LiveShellClock`, `pal::Clock::now`) — reads, not waits.

## 6. The reactor and the I/O stack

### 6.1 `pal::Reactor` (std-only interface)

A PAL seam (CONVENTIONS: OS specifics only behind `pal`). Operations: socket connect/read/write, pipe
read/write, process exit wait, timer, `post`. Each operation is an awaitable taking a `stop_token`. No
backend type appears in an `include/agentengine/` header outside `pal/`.

### 6.2 Backend: standalone Asio, behind the seam (proposed; owner decision)

Research recommends **standalone Asio 1.38.2** (BSL-1.0; IOCP on Windows, epoll on Linux), vendored and
pinned like mbedTLS, compiled once, confined to `src/backends/reactor_asio/`. Its handlers are written
against our own completion token that posts to the parked continuation's home (§4.2); `rt::task<T>` stays
the only coroutine type. **io_uring is excluded** (research §: disabled at Google after kCTF findings;
blocked by Docker/containerd default seccomp — the engine must run inside our own containers).

The alternative is a hand-written IOCP/epoll reactor in `pal/`. It satisfies CONVENTIONS' "core: std only,
zero third-party dependency, ever" without a backend dependency, at the cost of rebuilding what Asio
already proves on Windows (overlapped connect, cancellation, timer queues). libuv is rejected (on Windows
its first spawn places the *calling process* in a Job Object and aborts on failure — unacceptable for an
embedded library); `std::execution` is rejected for now (C++26 without shipped implementations; networking
deferred to C++29). **Decided: Asio (§9 D1).**

### 6.3 What sits on the reactor

- **TLS:** keep mbedTLS (ADR-013). Its non-blocking BIO callbacks (`want_read`/`want_write`) are driven by
  reactor socket readiness instead of the current internal `select()` (`tls_client.cpp:47-94`). Handshake,
  read and write are all cancellable.
- **HTTP/1.1 + SSE:** keep the in-house client (`net_egress_proxy.cpp`, `sse_stream_pump.hpp`), rewritten
  as coroutines. `select()` disappears (H-g). Connection reuse (today: `Connection: close`, a full
  handshake and CA-bundle parse per request) is in scope as a performance follow-on, not a correctness
  requirement of this ADR.
- **DNS:** platform async resolver where available (`GetAddrInfoExW` with overlapped completion on
  Windows); `rt::offload(getaddrinfo)` elsewhere, with the §4.6 residual. ADR-011's resolve-once/connect-to-
  verified-address rule is unchanged.
- **Processes:** a thin engine layer on the reactor's pipe and handle primitives. Windows: `CreateProcessW`
  with `PROC_THREAD_ATTRIBUTE_JOB_LIST` (child is in its Job at creation), overlapped named pipes, async
  wait on the process handle (Job completion-port messages are not guaranteed — research). Linux:
  `pidfd_open` + pipes. **Pipes are read concurrently with the wait** — closes H-e.
  **Handle hygiene becomes mandatory (red-team G4, pre-existing bugs that concurrency exposes):** every
  Windows spawn passes `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` with exactly the child's own pipe ends (today
  `native_process_spawn.cpp:209` and `docker_execution_surface.cpp:75,259` inherit with
  `bInheritHandles=TRUE` and no list, so a sibling spawned at the same moment inherits another child's
  write end and "read until EOF" waits for the sibling); every POSIX pipe is created `O_CLOEXEC`
  (`pipe2`; today `docker_execution_surface.cpp:339` uses `pipe()`). The Linux jail child must be
  async-signal-safe between `clone()` and `execve` — today `setup_jail` allocates `std::string`s there
  (`linux_native_jail_backend.cpp:143-206`), which becomes a likely malloc-lock deadlock once the process
  permanently runs reactor, lane and offload threads; the setup data is prepared before `clone()`. Pipe
  writes ignore `SIGPIPE` per call (`MSG_NOSIGNAL` already covers sockets, `pal/net.hpp:301`; pipes get
  `EPIPE` handling with `SIGPIPE` blocked on the writing thread). Exit status on Linux comes from the
  `pidfd` (`waitid(P_PIDFD)`), robust to a host that sets `SIGCHLD` to `SIG_IGN` — to verify on the
  supported kernels. Applies to the native
  jail (both OSes), native-process providers, docker/containerd/kata CLIs, the CPython worker's
  `FramedChannel`, the PDF worker, and ADR-209's held shell (whose file-polling IPC becomes pipe/handle
  waits).
- **Files:** stores (`FileAppendLogStore`, `FileSessionStore`, `FileWorktreeObjectStore`) go through
  `rt::offload` first; overlapped file I/O on Windows is a later optimisation behind the same seam.
- **Memory per parked read (red-team G6).** With IOCP every pending read commits its buffer up front;
  10 000 parked SSE reads at 64 KiB is ~640 MB. Parked stream reads use **zero-byte reads / readiness**
  (IOCP zero-byte `WSARecv`, epoll readiness) and take a pooled buffer only when data is ready. §8.2's
  "zero threads per parked run" gate gets a memory twin.
- **WASM plugins (red-team G5).** The plugin ABI (WASI 0.3, locked) is out of this ADR's I/O path for
  now: guest calls are CPU-bound and run through `rt::offload` (bounded, cancellable only at epoch
  boundaries via the existing epoch interruption); host imports that do I/O inside a guest call cannot
  suspend the guest without Wasmtime's async support (C-API support unverified) and are a named residual.
  The Wasmtime epoch ticker (`wasm_backend.cpp:78`) becomes a reactor timer.
- **Operation buffers are owned by the operation record**, never by the frame (round 3, gap 6): the
  kernel may still own an `OVERLAPPED`'s buffer after the reactor fails, so on reactor failure the
  records are leaked (not freed) and the frames are resumed with `fatal`.
- **Streamed reads keep one read outstanding (round 3, gap 7).** The reactor re-arms the next read on the
  I/O object's strand into a pooled ring buffer, and the session strand is woken once per batch of
  ready data, not once per chunk — two thread hops per chunk would not meet 023's ≤10 µs / 0-allocation
  per-chunk budget. Bench in §8.2 gate 8.
- **TLS context ownership (round 3, minor).** The mbedTLS context is touched only by the awaiting
  coroutine's operations on its I/O-object strand; "close on cancel" runs after that operation
  completes, never while another thread is inside `mbedtls_ssl_read`.
- **Reactor failure (round 2, G-f).** If the backend fails (IOCP port error, an exception escaping a
  handler on the reactor thread), every pending operation completes with a `fatal` error, the runtime
  enters an unhealthy state that refuses new operations, and the watchdog checks reactor liveness — no
  parked run may wait forever on a reactor that is gone.
- **Descriptor limits (round 2, G-g).** Linux's default soft `RLIMIT_NOFILE` (commonly 1024) is reached
  long before 10 000 parked sessions; `EMFILE`/`ENFILE` and Windows handle exhaustion are `resource`
  errors. The engine never raises the limit itself (a library must not); §8.2 gate 2 states the limit it
  runs with.
- **TLS details (red-team minor):** a write canceled mid-record drops the connection; concurrent TLS 1.3
  handshakes need mbedTLS's PSA state thread-safe (`MBEDTLS_THREADING_C` or one handshake at a time) —
  the current config is to be checked.

## 7. New problems this design creates (to be attacked by red-team)

Named here so the red-team pass starts from them, not from scratch:

1. **Thread migration.** A session continuation may resume on another lane worker. Any remaining
   thread-local state (holder ids today, any `thread_local` caches, Windows per-module TLS — ADR-175 §6)
   breaks silently. Mitigation: holder/home in the promise; a lint for `thread_local` in `rt/`/`core/`.
2. **Strand + mutex double-serialisation.** Can a continuation waiting on `session_mutex_` and the strand
   deadlock with a child strand that needs the parent's mutex? Answered by §4.3: child strands take
   `session_mutex_` only through lend-safe (read-only) entry points, by loan from the awaiting round;
   mutating entry points are refused with `rt.lock_held_by_ancestor`.
3. **Lane starvation.** A CPU-heavy tool body that never suspends occupies a lane worker; with 2 workers,
   two such tools stall every other session. Mitigation: per-call CPU budget measured; `rt::offload` for
   known-heavy code; open question whether lanes should be per-session-count adaptive.
4. **Cancel/complete and cancel/destroy races** (§4.4/§4.5) — the hardest correctness surface; requires
   TSan runs (CONVENTIONS) and a stress test.
5. **Reentrancy.** A handler that posts while holding a backend lock, or a `stop_callback` that runs
   synchronously inside `request_stop()` on a lane while the same strand holds state. `stop_callback`
   bodies must only signal the backend, never touch session state.
6. **Allocation on the hot path (023).** Each operation allocates a record and each `task` a frame; the
   per-chunk 0-allocation budget needs a recycling allocator for stream reads, measured by the existing
   bench gate.
7. **Host integration.** A host that owns its own loop (020 §3a) must either give the engine its lanes or
   run them itself; a UI-pump host must never be a lane. `EmbeddedHost`'s fail-fast rule (020 §8 Q5)
   carries over.
8. **Offload residual** (§4.6) for `at_most_once` effects.
9. **Two Asio copies** if a host links its own Asio (mitigated by 1.38's versioned namespaces; to verify).
10. **Shutdown under load**: scopes that do not join within the shutdown deadline — reported, never
    detached; what the host sees is part of the contract.

## 8. Evidence this ADR will require (gates)

### 8.1 Test infrastructure first

- A **test reactor**: single-threaded, manual `run_until_idle()`, virtual clock. Replaces
  `run_task_sync` and the ~130 naive `drive()` loops (H-f); those helpers are deleted, not kept as an
  alternative. A single-threaded reactor cannot show strand races, so every concurrency claim is also
  run on the real multi-lane runtime under TSan in a stress configuration.

### 8.2 Positive controls (each must fail against the pre-change code or a mutant)

1. **Resumption thread:** every reactor-completed awaitable resumes on its home, never the reactor thread
   (assert thread identity; mutant: inline resume) — including the three fallback paths of §4.2, a
   timer-driven stop, and `final_suspend` across homes.
1a. **Late completion after cancel:** cancel an IOCP read and destroy nothing until its completion
   arrives; a mutant that posts on cancel-request must be caught (ASan/page-guarded buffer).
1b. **Drop of a frame with an in-flight op** is the checked violation of §4.5 rule 3, not a UAF.
1c. **Ancestor lock:** a tool that calls a *mutating* entry point (`start_run`, `resolve_interaction`) on
   its own parent session fails with `rt.lock_held_by_ancestor`; the same tool's `fork_from_async` of that
   session succeeds by loan; two parallel siblings that both fork queue and both succeed; a loan released
   returns to the lender, not to a queued outside waiter (mutant).
1e. **Dropped host awaiter:** a host drops `runtime.enter(start_run(…))` mid-read; the chain is adopted,
   finishes, and a later `resolve_interaction` on the same session acquires the lock.
1f. **Paused consumer:** cancel a run whose event consumer never reads — it ends within the cancellation
   bound; a stalled stream fails with `resource` while the run completes.
1d. **Concurrent spawns:** 64 children spawned concurrently each reach end-of-stream at their own exit
   (handle list / `O_CLOEXEC`).
2. **Zero threads per parked run:** 10 000 sessions parked on I/O with 2 lane workers; OS thread count
   stays flat (023), and committed memory stays under a stated per-run bound (zero-byte reads, §6.3).
3. **Cancellation latency:** cancel during DNS, TLS handshake, SSE read, child-process run, offload — the
   caller resumes within a measured bound; no orphaned process/handle/capability (001 §9 G3). Measured
   **under load** (lanes saturated by CPU-heavy tools), not on an idle runtime.
4. **Pipe deadlock gone:** a child writing 16 MiB to stdout before exiting completes normally (H-e).
5. **`block_on` refused on a lane** and **homeless wake on reactor refused** (§4.2/§4.3).
6. **No detached threads:** lint over `include/` and `src/`; a shutdown test that destroys a session with
   background tools and parallel batches in flight and joins cleanly under TSan.
7. **Ordered append preserved** for parallel batches (006 §5, I5) under randomised completion order.
8. **Benches (023):** turn overhead, per-chunk latency/allocations, tool dispatch — before/after.
9. **Conformance (I7):** MCP and CodeAct bridges re-run against the async pipeline; no `block_on` at
   those edges.

## 9. Owner decisions (2026-10-03)

Answers given by the project owner on 2026-10-03; where the owner delegated the choice ("help me" /
"I don't know which"), the recommendation below is this ADR's and is marked as such.

- **D1 — Reactor backend: a third-party library (owner).** Standalone Asio (research
  `2026-10-03-async-reactor-options.md`), vendored and hash-pinned like mbedTLS, confined to
  `src/backends/reactor_asio/` behind `pal::Reactor` (§6.1–§6.2). Not an optional backend: every build
  links it. CONVENTIONS gets one sentence: the reactor backend is the one mandatory backend dependency;
  core headers stay std-only.

- **D2 — Merge strategy (delegated; recommendation).** **One integration branch, merged into `main`
  once**, when every seam in §5.1 is converted, the old paths are deleted and §8's gates are green.
  Inside the branch the work proceeds in dependency order (reactor + async HTTPS/process primitives →
  seam signatures → callers → deletion of `block_on`-at-the-edge, `drive_leaf_task`,
  `synchronous_leaf`, detached threads), each step a commit that builds and passes the suite on the
  branch. Reason: the owner asked for a clean, complete conversion and the codebase is pre-production,
  so the cost a long branch usually carries (other work blocked or conflicting) is lower than the cost
  of `main` living with half-sync, half-async seams; and the old shapes are deleted rather than kept
  alongside, which only works if nothing on `main` still depends on them. `main` is merged *into* the
  branch regularly so the final merge is not a surprise. **`main` is frozen while the branch is implemented (owner, 2026-10-03)**, so the
  regular `main`-into-branch merges are expected to be empty; the last one (ADR-235/236, PCH, object-store
  lock) was taken on 2026-10-03. If the freeze is lifted, these process rules apply (round
  2): no new synchronous seams land on `main` — enforced by a CI lint on concept/callable signatures, not
  only by review (round 3) (they would have to be converted twice); the seam renames
  (`chat` → `get_response`) are done early on the branch so later `main` merges conflict once, not
  repeatedly; ADR numbers used on the branch are reserved in `decisions/README.md` on `main` up front;
  the I7 conformance gates (MCP, CodeAct, A2A, AG-UI) run on the branch before the single merge.

- **D3 — Argument-dependent capability requirements (delegated; recommendation).** The native-process
  providers do not need "any one of N grants" (an `AnyOf<…>` ceiling); they need a requirement that
  depends on the call's **arguments**: running `python3` requires a `NativeExec` grant covering
  `python3` on the provider's mount. Today the tool declares an empty ceiling and matches grants inside
  its own body (`src/backends/native_process/native_providers.hpp:158-181`), outside the pipeline's
  steps 4/7, so the audit record shows no bound capability for the call. Recommendation (revised after round 2, M5): a tool may declare a **per-call requirement**:
  - a static **requirement kind** — e.g. `RequirementKind<cap::decl::NativeExec<…mount…>>` — that bounds
    what the hook may ask for (kind and mount) without binding anything; the AND-of-all static ceiling
    cannot express this bound and is not reused for it;
  - `std::vector<Capability> required_capabilities(Args const&) const`.

  Rules, each closing a round-2 finding:
  1. **Parse once.** The pipeline parses `Args` from the JSON once (today step 2 defers shape checks to
     `invoke`'s own `from_json`, `src/core/tool_pipeline.cpp:159-165,233`) and passes the *same* object
     to `required_capabilities` and to `invoke`, so the requirement and the effect cannot see different
     arguments.
  2. **Bound set only — with a dynamic form for authority found at run time (revised after round 3).**
     A tool with a static hook sees only `ctx.bound_capabilities`, never the full held set
     (`ctx.capabilities`). Some tools cannot know their requirement from the arguments: the mediated
     shell looks up `FsRead`/`FsWrite` for paths a script reaches while running
     (`mediated_shell_dispatch.cpp:62,88,111`, e.g. `for f in *; do cat $f`). Those declare the
     **dynamic** form instead: `invoke` sees the held set narrowed to the declared requirement kinds and
     mounts, and every lookup goes through `co_await ctx.authorize(cap)`, which binds and records the
     binding in the audit (I4) exactly as steps 4/7 would. Neither form gives the body unaudited access
     to the full held set; the ~31 sites that read `ctx.capabilities` are inventoried and moved to one of
     the two forms. A tool may declare a **list** of requirement kinds (e.g. a secret and an exec grant).
     On resume after approval, the requirement is recomputed from the same parsed args and the call is
     refused if it differs from what the human approved.
  3. **Fail closed.** An empty result, or one outside the declared requirement kind, is a `contract`
     error — never "nothing to bind, proceed".
  4. **Bound like a ceiling.** The result is bound at steps 4/7 (`bind`/`bind_clamped`, ADR-217); a
     requirement no held grant covers is denied there, as today.
  5. **Approval shows what is bound.** The approver today sees `(caller, tool_name, canonical_args)`
     (`tool_pipeline.cpp:212-213`); for a tool with the hook the approval request also carries the
     requirement computed from those same parsed args, so the human approves exactly what will be bound.

  I2/I3: the model's argument selects *which* already-held grant must cover the call — the same as
  path-scoped grants today; it narrows within held authority and never mints or widens it (the ADR-070
  boundary). This replaces the "empty ceiling
  + in-body check" exception rather than naming it, and applies equally to the held-shell provider
  (`native_shell_session_provider.hpp:325-326`).

- **D4 — Real asynchronous model calls, renamed to match (owner).** `ChatClient` gets a genuinely
  asynchronous non-streaming call again (004 §1 lists both), and the method names are refactored to the
  MAF/Microsoft.Extensions.AI shape the project models itself on: `IChatClient.GetResponseAsync` /
  `GetStreamingResponseAsync` (.NET), `get_response(…, stream=…)` (Python). Proposed C++ names:
  `get_response(ChatRequest const&, EffectContext&) -> task<result<ChatResponse>>` and
  `get_streaming_response(ChatRequest, EffectContext&) -> stream<ChatResponseUpdate>`. The exact names
  are a 027 §4 vocabulary change and are confirmed with the owner before the refactor; every conformer
  (OpenAI, Anthropic, recording, replay, gateway, workflow-as-chat-client, test mocks) is renamed in the
  same branch — no aliases kept.

- **D5 — Lane workers default to 50% of the device, configurable (owner).** Default lane count =
  `max(2, usable_cpus() / 2)`, where `usable_cpus()` honours the process's CPU affinity and, on Linux,
  the cgroup CPU quota — `hardware_concurrency()` alone reports host cores inside a `--cpus=1` container
  (round 2, G-d; the libstdc++ behaviour is to be verified on the supported toolchains). The floor of 2
  keeps one CPU-heavy tool from stalling every session on a 2-core machine. `RuntimeConfig::lanes`
  overrides it. This amends CONVENTIONS'
  "never spawn `hardware_concurrency()` threads": the engine never takes the whole machine by default,
  and the host can always set the number. Offload and DNS pools are separate, small and configurable
  (defaults: 2 each), because they hold threads that block and must not grow with the core count.

- **D6 — A dedicated cancellation class (owner: yes, if it works well).** Add `failure_class::canceled`:
  "stopped on request — by the host, a parent, or a sibling's failure". It is not retryable, not a
  policy denial, and not a fault. Deadlines stay what 001 §5 already says: a budget, i.e.
  `failure_class::resource` with code `deadline_exceeded` (I8) — so "the host canceled" and "this call
  ran out of time" are distinguishable by class, not by parsing a code. A canceled run ends `canceled` +
  `run.canceled` (amends ADR-178, which uses `fatal`). A canceled *operation* inside a live run (one
  sibling canceled by `when_all`) carries `canceled` up to whoever awaits it; it ends the run only if
  that caller lets it. "Compiler-checked" is not enough (round 2, G-c): there is one `switch` over
  `failure_class` (`rt/workflow_supervisor.hpp:305`) and everything else is `==` predicates. The
  implementation carries a grep inventory of every classification site as evidence, with these rules:
  - `canceled` never triggers retry, fallback or propagate (`is_retryable`, `rt/multi_agent.hpp:67`,
    `rt/workflow_supervisor.hpp:1074`; workflow `propagate`/`fallback`, `workflow/graph.hpp:156-164`) —
    a sibling canceled by `when_all` must not be rerouted to a fallback executor that keeps working.
  - The deadline class is reconciled everywhere: `deadline_exceeded` is `resource` (today the replay
    client maps it to `transient`, `core/replay_chat_client.hpp:73`, and "cancelled" to `fatal`, `:82`);
    whether a `resource` deadline is retryable at all is decided once, not per site.
  - String-keyed special cases move to the class: `net.cancelled` (`transient` today,
    `src/sandbox/net_egress_proxy.cpp:98`, special-cased at `src/rt/agent_session_core.cpp:1154`), the
    eval screen's `"run.canceled"` key (`eval/eval_screen_common.hpp:59`), background "canceled before
    start" (`policy` today, `rt/background_job_runner.hpp:312`).
  - Protocol mapping (I7): A2A maps `canceled` to its `canceled` task state (today every failure is
    `failed`, `protocol/a2a/server.hpp:142-151`); MCP and AG-UI mappings are checked the same way.

- **D7 — An explicit, short host API (owner).** No hidden host-completion thread: the developer states
  where their continuations run, in one call. Sketch:

  ```cpp
  ae::rt::Runtime runtime{{.lanes = ae::rt::half_of_hardware}};   // D5; owns reactor + lanes

  // 1. A synchronous host (CLI main, tests): block this thread until the task finishes.
  auto r = runtime.run(session.start_run(request));

  // 2. A host with its own coroutine type / event loop: say how to get back onto it.
  auto r = co_await runtime.enter(session.start_run(request), my_loop_resumer);   // ADR-219 Resumer

  // 3. A host with neither: ask the engine for a dedicated completion thread, by name.
  ae::rt::CompletionThread completions{runtime};
  auto r = co_await runtime.enter(session.start_run(request), completions);
  ```

  `runtime.enter` is the only way to `co_await` an engine awaitable from a foreign coroutine type; a bare
  `co_await session.start_run(…)` from a non-`ae::task` coroutine is a compile error, so the choice
  cannot be forgotten. Implementation (round 2, G-b): `await_suspend` is templated on
  `coroutine_handle<P>` with an `is_engine_promise<P>` constraint (today it takes a type-erased
  `coroutine_handle<>`, `rt/task.hpp`). The restriction covers **every** engine awaitable, not only
  `task`: `stream<T>::next()`, `AsyncMutex::lock()`, `channel::next_async` — hosts consume the run-event
  and `get_streaming_response` streams through `runtime.enter` as well. The direct-driving API
  (`start()`/`resume()`/`take_value()`) becomes private to `Runtime`/`block_on` (`SignalTask`,
  `rt/block_on.hpp:176`, is an engine promise). The ADR-219 tests that drive engine tasks from foreign
  coroutine types (`tests/rt/test_rt_host_resumer.cpp:248`, `tests/rt/test_rt_parked_task_home.cpp:297`)
  are rewritten to `runtime.enter`, and 020 §3a's documented `co_await start_run` embedding is amended
  (§10). Scope, sized (round 3): ~131 test files drive tasks with `while (!done()) resume()` and ~132 use
  `take_value()`; library code drives through `block_on` in `agent_spawn_child_run.hpp:139`,
  `agent_workflow_executor.hpp:87`, `workflow_as_chat_client.hpp:161`, `workflow_as_executor.hpp:98`,
  `workflow_supervisor.hpp:960`; `rt::ThreadPool` (`block_on` per job, `thread_pool.hpp:278`) and
  `multi_agent::parallel` (`future::get`, `multi_agent.hpp:314-333`) are **deleted or converted**, and the
  ~15 `drive_leaf_task` users go with ADR-064's stopgap. This is the integration branch's largest single
  cost and is planned as its own step in D2's order. `runtime.run` is
  `block_on` at the edge and is refused on lanes/offload workers (§4.3). Names (`Runtime`, `enter`,
  `CompletionThread`) are 027 §4 additions, confirmed with the owner before implementation.

## 10. Spec changes this ADR will carry (when Judged)

- **001 §5/§8:** define the reactor, lanes, strand and `offload`; replace the undefined `Fast` trait and
  "drain-budget test (023)" with the lane-starvation rule (§7.3) and its test; resolve C1 (020 §3a vs
  ADR-219 on who resumes a stream consumer) in favour of the home rule.
- **006 §1:** `invoke` is an instance member; §5 references `when_all` child strands; §6b background via
  the session task scope.
- **008 §2, 018 §4:** unchanged in signature (already async); add the measured cancellation bound.
- **020 §3a:** host modes (engine-owned lanes / host-run lanes / `Resumer`), the reactor thread rule;
  the documented `co_await start_run` embedding becomes `co_await runtime.enter(start_run(…), resumer)`
  (D7).
- **ADR-219:** amendment note — dropping a parked engine coroutine is no longer a cancel (§4.5 rule 3).
- **027 §4:** `Reactor`, `lane`, `strand`, `task_scope`, `offload` (none may be named `Executor` — 027 §5
  gives that to the workflow node); remove the stale ambient `MessageContext` row (conflicts with
  CONVENTIONS' "EffectContext is a parameter, not a thread-local").
- **CONVENTIONS:** no detached threads; no `thread_local` in `rt/`/`core/` without an ADR; the reactor
  backend is the one mandatory backend dependency (D1); lane default 50% of hardware threads, always
  configurable (D5).
- **004 §1:** `get_response` / `get_streaming_response` (D4). **006 §3:** per-call capability
  requirement bound at steps 4/7 (D3). **ADR-178:** canceled run is `failure_class::canceled` (D6).
- **027 §4:** also `Runtime`, `enter`, `CompletionThread`, `canceled` (D4/D6/D7).

## 11. Red-team, round 1 (2026-10-03, independent agent, no prior context, against the draft)

Verified against `main` at `beb47b3`. Dispositions refer to the revised sections above.

| # | Severity | Finding (one line) | Disposition |
|---|---|---|---|
| M1 | MUST-FIX | "Cancel wins and posts" lets the kernel/Asio complete into a freed frame and buffer; a finished I/O is misreported as canceled; cancel called from a foreign thread races Asio | §4.4 two-phase cancel: post the cancel to the reactor, resume only on the backend completion, refcounted op record, real outcome reported, canceled connections closed |
| M2 | MUST-FIX | "Never destroy a frame with an in-flight op" is unenforceable: `~task` destroys unconditionally; ADR-017/219 rely on drop-to-cancel; unwinding over a scope; `when_all` propagating before siblings finish | §4.5 rule 3: in-flight count + checked violation (reaper in release); drop-to-cancel revoked (amendments to ADR-017/219); `with_scope` only; `when_all` cancels + joins all then propagates |
| M3 | MUST-FIX | Async cleanup has no owner: stream producers, `AgentSession` destructors on a lane, `operator=`/refund/`fork_from` via `block_on` | §4.5 rule 3a: `task<void> close()` for every owner; producers spawned into a caller-passed scope; `fork_from_async` |
| M4 | MUST-FIX | `final_suspend` symmetric transfer bypasses homes/strands; 020 §3a host `co_await start_run` runs engine code on the host thread and then hits the fatal homeless refusal; ADR-219's objection to home-in-promise unanswered | §4.2: entry points hop to the strand; `final_suspend` posts across homes; `ScopedResumer` kept for foreign coroutine types; host-completion thread or mandatory `Resumer` (D7) |
| M5 | MUST-FIX | The reactor still runs engine code via the closed-home and dropped-continuation fallbacks and via `request_stop()` from timers; existing `stop_callback`s do real work; a per-call deadline would cancel the whole run | §4.2 "the reactor only ever enqueues"; stop-callback audit; §4.4 per-operation linked stop sources, timer stops posted to the strand |
| M6 | MUST-FIX | Synchronous sinks (`report_progress`) cannot apply async back-pressure; the channel is single-producer while parallel children emit concurrently | §4.5 rule 5: bounded MPSC enqueue + one draining task; explicit overflow policy (coalesce progress, never drop lifecycle, else `resource` error) |
| G1 | REAL GAP | Holder identity of child strands undefined; parent/child re-entry becomes an invisible permanent park | §4.3 ancestor chain in the promise, `rt.lock_held_by_ancestor`, stuck-run watchdog |
| G2 | REAL GAP | Instance tools vs. ADR-181 (jobs outlive sessions), derived `captures_session_state` disables background/rerun, concurrent `invoke` on one object, I2 via injected handles | §5.2: `const invoke`, explicit `captures_session_state`, shared ownership, background scope = job runner's, injected-handle rule |
| G3 | REAL GAP | Offload: discarded bodies are detached tasks, by-reference captures, DNS exhausting a shared 2-worker pool, `block_on` inside offload | §4.6 offload lifetime rules, by-value inputs, separate DNS pool; `block_on` refused on offload workers (§4.3) |
| G4 | REAL GAP | Spawns without `HANDLE_LIST`/`O_CLOEXEC` leak write ends to concurrent siblings; Linux jail allocates between `clone` and `execve`; `SIGPIPE`; `SIGCHLD=SIG_IGN` (pre-existing) | §6.3 handle hygiene; gate 1d |
| G5 | REAL GAP | WASM seams not covered; epoch ticker is a `jthread` | §6.3 WASM residual; ticker becomes a reactor timer |
| G6 | REAL GAP | IOCP commits a buffer per parked read: ~640 MB for 10 000 parked SSE reads | §6.3 zero-byte/readiness reads; memory twin of gate 2 |
| G7 | REAL GAP | Lane blocking is not only `block_on`; FIFO lanes delay cancellation behind CPU work | §4.3 lane-stall watchdog, priority for cancel/approval resumes; gate 3 under load |
| — | MINOR | TLS write canceled mid-record; mbedTLS PSA threading; ADR-209 cancel kills the held environment; "fatal, logged" undefined for a library; single-threaded test reactor hides races; `thread_local` jitter RNG (`retry_policy.hpp:63`, harmless); read `GetLastError` before suspending; Asio hidden threads / `signal_set` | §6.3 TLS details; §8.1 TSan stress; the remaining items go to the implementation checklist |

Checked, no issue: the only engine-relevant `thread_local` is `ExecutionContext` / the ambient holder id
(`resume_home.hpp:106,111`); no `std::this_thread::get_id` use; CPython runs in a worker process, so GIL
thread affinity is not affected; each TLS session has its own DRBG (`tls_client.cpp:172`).

Pre-existing defects the round surfaced (independent of this ADR, worth fixing even if it is rejected):
G4's inherited pipe handles and allocation-after-`clone`, H-e's drain-after-exit pipe deadlock, and the
`run_task_sync` frame destruction (H-f).

## 12. Red-team, round 2 (2026-10-03, independent agent, against the revised design)

Verified on branch `adr237-async-design`. The round confirmed four round-1 fixes (two-phase cancel with a
refcounted record; `when_all` cancel + join; `with_scope` only; a tractable `stop_callback` audit, ~10
sites) and found that two did not hold (the release-mode reaper; Asio objects touched from two threads).

| # | Severity | Finding (one line) | Disposition |
|---|---|---|---|
| R2-M1 | MUST-FIX | The release-mode reaper resumes or destroys a frame whose referents (parent `EffectContext`, tool, buffers, `continuation_`) are gone; the in-flight count sits in the leaf while `~task` runs on the outer frame | §4.5 rule 3: count along the awaited chain; orphan = never resumed, continuation cleared, op canceled, frame **leaked** after the backend completes, reported; debug aborts |
| R2-M2 | MUST-FIX | Initiation on a lane races the reactor's completion/cancel on the same Asio object; a stop landing before the backend holds the op is lost | §4.4: one serial context (Asio strand) per I/O object for initiation, completion and cancel; sticky cancel flag checked at initiation |
| R2-M3 | MUST-FIX | Coalescing `model_delta`s garbles the transcript; reserved-room exhaustion fails a healthy run on a slow consumer; I5 recording placement unstated | §4.5 rule 5: lifecycle + deltas use a real `co_await push` (never coalesced, never failing the run); only idempotent snapshots from sync sinks coalesce; recording taps before the queues |
| R2-M4 | MUST-FIX | The ancestor-lock refusal breaks the deliberately re-entrant `fork_from` of one's own session and background jobs that should simply wait | §4.3: chains follow awaited edges only; exclusive lending to an awaited descendant; refusal only when the ancestor is not awaiting the requester |
| R2-M5 | MUST-FIX | D3: args parsed twice; `invoke` still sees every held grant; empty requirement fails open; "ceiling stays the upper bound" contradicts the AND ceiling | §9 D3 rewritten: requirement kind, parse once, bound set only, fail closed, approval shows the requirement |
| G-a | REAL GAP | "Homes equal" must mean strand-equal; a foreign continuation whose home is gone is unspecified | §4.2: same-strand symmetric transfer only; dead foreign home → never resumed, reported, orphan rule |
| G-b | REAL GAP | D7's compile-time guard leaks through streams, `AsyncMutex`, `channel`, the public direct-driving API; ADR-219 tests and 020 §3a's embedding | §9 D7: restriction on every engine awaitable; direct driving private to `Runtime`; tests rewritten; 020 §3a amended |
| G-c | REAL GAP | D6 "compiler-checked" is hollow (one `switch`); retry/fallback/propagate, replay mapping, string-keyed codes, A2A state mapping would misclassify | §9 D6: grep inventory as evidence; `canceled` never retries/falls back/propagates; deadline class reconciled; protocol mapping |
| G-d | REAL GAP | `hardware_concurrency()` ignores cgroup quotas; 2-core box → 1 lane; priority vs strand FIFO; no cross-session fairness | §9 D5 `max(2, usable_cpus()/2)`; §4.3 round-robin across strands, priority across strands only |
| G-e | REAL GAP | Async `close()` turns every early return/exception into a violation; stream drop-to-cancel is a working contract | §4.5: consumer drop stays a stop request; `with_session`/`with_stream` helpers |
| G-f | REAL GAP | A failed reactor leaves every parked run waiting forever | §6.3: fail pending ops `fatal`, unhealthy runtime, watchdog |
| G-g | REAL GAP | `RLIMIT_NOFILE` breaks the 10 000-session gate | §6.3: `resource` classification; gate states its limit; the library never raises it |
| — | MINOR | Asio must not associate an `immediate_executor` (two queue hops on immediate completion — bench it); grant revocation during long async calls (pre-existing, now longer windows); D2 process risks | Implementation checklist; D2 process rules added to §9 |

## 13. Red-team, round 3 (2026-10-03, targeted at the round-2 amendments)

Held: strand-equal `final_suspend` (G-a); one serial context per I/O object + sticky cancel (R2-M2) — an
I/O-object strand never waits on a session strand, so the two layers cannot deadlock; D5, D6, G-g, D2;
cancel while a lender is suspended is safe (the lender cannot resume before `when_all` joins).

| # | Severity | Finding (one line) | Disposition |
|---|---|---|---|
| R3-1 | MUST-FIX | Lending to *any* awaited descendant lets a tool run `start_run`/`resolve_interaction` on its parent mid-round, corrupting tool_call/tool_result pairing; contradicts gate 1c and §7.2 | §4.3: lend-safe (read-only) entry points only; mutating ones refused; gate 1c and §7.2 reconciled |
| R3-2 | MUST-FIX | Leaking an orphan frame strands `session_mutex_`, quota tickets, refunds, shell slots forever; unbounded memory under repeated drops | §4.5 rule 3: root adoption (chain intact, finishes normally, joined at shutdown); non-root destruction bypassing scopes aborts; `CompletionThread` joins |
| R3-3 | MUST-FIX | Awaited push with a paused consumer makes cancel impossible (no room for `run_canceled`); `ask_stream`'s session-wide stream; drain-after-run hosts deadlock | §4.5 rule 5: push races the stop token; reserved terminal slot; consumer-stall deadline fails the stream, not the run; gate 1f |
| R3-4 | REAL GAP | Loan details: siblings refused at random; released loan could go to the FIFO head; borrowed guard can escape; nesting | §4.3 loan rules: loan queue, return to lender (mutant-tested), frame-bound guards, stack |
| R3-5 | REAL GAP | "Bound set only" breaks tools whose authority is found at run time (mediated shell) | §9 D3 dynamic form via `ctx.authorize(cap)` (audited); list of requirement kinds; approval recheck on resume |
| R3-6 | REAL GAP | Reactor failure resumes frames whose buffers the kernel may still own | §6.3: buffers owned by the op record; records leaked on reactor failure |
| R3-7 | REAL GAP | Two thread hops per streamed chunk vs 023's per-chunk budget | §6.3: one read kept outstanding, pooled ring, one wake per batch; bench |
| R3-8 | REAL GAP | Fairness per strand gives a 50-child session 50 shares | §4.3: round-robin across sessions first, then strands |
| — | MINOR | Snapshot after `tool_call_finished`; nondeterministic snapshot order vs I5; exactly-once wording vs sticky cancel; mbedTLS context ownership; D7 scope understated (~131/~132 test files, library `block_on` sites, `ThreadPool`, `multi_agent::parallel`); D2 rule needs a CI lint | All folded into §4.4, §4.5, §6.3, §9 D2/D7 |

Round 3's verdict was "ready once the six listed items are folded in"; they are, above. The remaining
items are implementation-checklist work with named gates.

## 14. Implementation record

### Step 1 (2026-10-03): the reactor seam, its Asio backend, `rt::sleep_until`

- `include/agentengine/pal/reactor.hpp` — `pal::Reactor` / `pal::ReactorOp` (std-only): timers, sticky
  two-phase cancel, `on_reactor_thread()`, homeless-refusal counter.
- `src/backends/reactor_asio/reactor_asio.cpp` — standalone Asio 1.38.2 (fetched, SHA256-pinned,
  `AGENTENGINE_ASIO_ROOT` for offline builds), one io_context on one reactor thread that is joined, never
  detached; every operation is initiated, completed and cancelled on that thread; shutdown cancels and
  delivers every pending completion before joining. Layer L0 (`tools/layers.toml`).
- `include/agentengine/rt/sleep.hpp` — `rt::sleep_until` / `sleep_for`: resumes on the waiter's home
  (ADR-175 `block_on` home or ADR-219 `Resumer`), refuses homeless wake-ups, stop_callback registered before
  the start, frame destruction abandons the operation.
- `tests/rt/test_rt_reactor_timer.cpp` — 26 checks (R1–R10, R4b), all passing. Mutants, each killed:
  A — drop the homeless refusal (R6 fails); B — resume inline on the reactor thread (R1, R2, R5, R10 fail);
  C — ignore the sticky flag at start (R4b fails; the racing R4 alone did **not** kill it, which is why R4b
  exists).
- Regression: full suite (`ctest -LE live-network`, Windows/clang) — every failure is a Docker-dependent
  test on a machine without Docker running (13), unchanged from before this step; `rt` label 69/69.
  Layering and naming lints clean.
- Linux (WSL, gcc 15, `-Werror`): builds; the test's first version **segfaulted** there (R5) and passed on
  Windows by luck — immediately-invoked lambda coroutines read their captures through a closure destroyed
  before the lazily started body ran. A test bug, not an engine bug, but exactly the lifetime class §4.5
  is about; the test now uses free coroutines. Then 26/26, three runs.
- TSan (gcc 15, the test plus the backend): one data race, again in the test (R8's waiter read the
  `unique_ptr` main was resetting); fixed; 5/5 runs clean.

### Step 2 (2026-10-03): offload, TCP, processes — three parallel slices, merged into the integration branch

Built in parallel (separate worktrees), each on the step-1 recipe (`src/backends/reactor_asio/asio_reactor.hpp`
top comment), then merged; the merged tree passes `rt` 73/73 on Windows/clang and the five reactor tests on
Linux/gcc 15 `-Werror`. Lints clean.

- **2a `rt::offload`** — `include/agentengine/rt/offload.hpp`: `OffloadPool` (fixed workers, joined, never
  detached), `co_await rt::offload(pool, stop, fn, args...)` → `offload_result<T>` {completed, canceled,
  faulted}. By-value inputs (`std::ref` and reference returns rejected at compile time). One CAS state
  machine per job; cancel resumes the caller directly (there is no backend completion to wait for) while the
  running body stays owned by the pool and its result is discarded and counted. Shutdown resumes queued
  callers `canceled`, always joins running bodies, and only *reports* a `shutdown_deadline` overrun.
  `block_on.hpp` gained `ScopedBlockOnRefusal` / `block_on_refused` (§4.3): both overloads throw on a marked
  thread, nothing else changes. Also `include/agentengine/testing/manual_reactor.hpp` (virtual time,
  single-threaded; with `sleep_until` use deadlines from `r.now()`, because `await_ready` reads the real
  clock). Tests: offload 51 checks (O1–O11), ManualReactor 25 (M1–M7); 24 mutants, all killed (two
  survived at first — `block_on(task<void>)` ignoring the marker, and a setup-time cancel posting — and
  led to a stronger O8 and to `await_ready` always returning false). TSan 5/5, ASan/UBSan clean. Linux
  found one more test-only lifetime bug (a result vector declared after the reactor).
- **2b TCP** — `pal/reactor_tcp.hpp`, `reactor_tcp_asio.cpp`, `rt/tcp.hpp`: connect to an already-resolved
  numeric address (no DNS in the seam), read_some / write_all / wait_readable (zero-byte readiness, §6.3 G-6).
  Buffers are owned by the op record, never the frame. Per-op cancellation slots; an in-flight op ending
  `canceled` closes the stream (§4.4), a pre-start sticky cancel touches nothing; a second op in the same
  direction is `busy`. EMFILE/ENFILE/WSAEMFILE/WSAENOBUFS → `too_many_open_files`, a `resource` error
  (§6.3 G-g). Streams may outlive their reactor (an Asio service tears sockets down at shutdown). Tests: 53
  checks on Linux (52 on Windows, no `RLIMIT_NOFILE`), T1–T15; 12 mutants killed; ASan positive control for
  buffer ownership (frame-owned buffer → heap-use-after-free inside `recv`). TSan 5/5, ASan 3/3.
- **2c Processes** — `pal/reactor_process.hpp`, `reactor_process_asio.cpp`, `rt/process.hpp`
  (`rt::run_process`), helper `tests/rt/process_child_helper.cpp`. Windows: Job at creation
  (`PROC_THREAD_ATTRIBUTE_JOB_LIST`), exact `HANDLE_LIST`, overlapped named pipes, `object_handle` exit wait.
  Linux: `pidfd_spawn` (glibc ≥ 2.39) or `posix_spawn` + `pidfd_open`, `O_CLOEXEC` plus close-above-2,
  own process group, `WNOWAIT` so the pgid is not reused while a group kill may target it, SIGPIPE blocked on
  the reactor thread; no pidfd → `unsupported`, never a pid-wait fallback. Pipes are read concurrently with
  the exit wait (closes H-e); "finished" = exit + both EOFs, with a grace period then a group kill when a
  grandchild holds a pipe (`output_incomplete`). `run_process` uses a small internal join (no `when_all` yet)
  whose decisions run on the home thread. Tests: 65 checks (P1–P19) on both OSes, incl. gate 4 (16 MiB before
  exit) and gate 1d (64 concurrent spawns); 12 mutants — all killed except "no `O_CLOEXEC` alone", which
  close-above-2 masks (dropping close-above-2 is killed). TSan 5/5.
- **Integration fix**: TCP found the Asio reactor with `dynamic_cast`; CONVENTIONS forbids RTTI, so it now
  uses `AsioReactor::from(Reactor&)` (a live-reactor registry added by 2c).
- **Refactor after step 2**: the op state machine copied into sleep, tcp and process is now one record,
  `rt/reactor_await.hpp` (`reactor_detail::AwaitedOp<Base>`, `Canceler`). Mutant (drop its homeless
  refusal) fails the timer, TCP and process tests together.
- **Open after step 2**: read buffers are not pooled (§6.3 round-3 gap 7); a stream/process handle must
  outlive its awaiters (raw pointer, documented, not enforced); `run_process` must finish before its reactor
  is destroyed (§4.5 rule 4 ordering, documented, not checked); SIGCHLD=`SIG_IGN` hosts get `known = false`
  exit status (untested); a `setsid` grandchild escapes the process group (the jail's job, cgroups);
  compile-time rejections in offload and the foreign-reactor paths have no tests.

### Step 3 (2026-10-03): lanes, strands, the strand as home, `rt::Runtime`

Additive: every existing caller and hand-driven test is unchanged (`rt` 75/75 Windows/clang, 72/72 Linux/gcc 15
`-Werror`; full Windows suite `ctest -LE live-network` 397/410, the 13 failures the same Docker-daemon tests as
step 1). The D7 compile-time restriction, AsyncMutex lending, seam conversion and deleting
`ThreadPool`/`block_on`-at-the-edge are later steps.

- `include/agentengine/rt/lanes.hpp` — `LanePool` (fixed `std::jthread` workers, joined; each carries
  `ScopedBlockOnRefusal("rt.block_on_on_lane")`), `StrandGroup` (a "session" for scheduling), `Strand` (serial: a
  worker takes one continuation, runs it until it parks or ends, only then may the strand's next run, on any
  worker; FIFO), `rt::reschedule()`, `rt::on_strand(strand, task)`, `rt::current_strand_id()`. Scheduling as
  §4.3: round-robin across groups, then across the group's strands; a priority post picks its strand first across
  strands and is queued behind the strand's own items. One scheduler mutex (a sharded queue is a benchmarked later
  change). A post after the workers stopped is dropped and counted, never run on the posting thread.
- `include/agentengine/rt/resume_home.hpp` — `detail::StrandHome` (the fourth kind of home; one virtual `post`,
  the type-erasure level of `Resumer`), `ExecutionContext::strand`, `ParkedResumer::strand` and `homed()`;
  `capture_parked`/`wake` post a strand-homed coroutine to its strand. `reactor_await.hpp`, `offload.hpp`,
  `process.hpp` refuse on `!homed()` instead of `!home && !ticket`.
- `include/agentengine/rt/task.hpp` — `detail::TaskLink` in both promises (the awaiter's strand and holder, or a
  root-completion callback), `detail::complete_to` (final_suspend: transfer iff same strand, else post the awaiter
  to its strand and return `noop_coroutine`), `detail::TaskAccess`; raw `resume()` keeps the current strand.
- `include/agentengine/rt/runtime.hpp` — `RuntimeConfig{lanes, offload_workers, dns_workers, shutdown_deadline,
  on_shutdown_overrun}` (`half_of_hardware` = the D5 default), `Runtime` (one `make_default_reactor()`, the lanes,
  general and DNS `OffloadPool`s; `run(task)`, `run(strand, task)`, `enter(task, Resumer&|shared_ptr<Resumer>|
  CompletionThread&)`, `new_strand_group()`), `CompletionThread`, `usable_cpus()`, `default_lane_count()`.
- `include/agentengine/pal/cpu.hpp`, `src/pal/usable_cpus.cpp` (new static lib `agentengine_pal_cpu`, layer L0
  in `tools/layers.toml`) — `pal::usable_cpus()`: Windows process affinity (whole system mask → every active
  processor across groups); Linux `sched_getaffinity` capped by cgroup v2 `cpu.max` along the cgroup path (tightest
  wins) or v1 `cfs_quota/period`, rounded up, at least 1. Parsers in the header, tested on every OS.

Decisions:

- **How the home reaches a type-erased awaiter.** Not a templated `capture_parked<P>`: the strand is a field of
  the thread's execution context that **only a lane worker sets, around exactly one continuation of that strand**.
  So for that slice every coroutine on the thread is part of that strand's chain (the continuation and whatever it
  symmetrically transferred into). ADR-219's objection — leaf awaiters see `coroutine_handle<>`, every coroutine
  type in a chain must cooperate — was about a context set by *some* driver that a host runtime could bypass; for
  engine work the only resumer is the lane pool, so the context is exact, and no awaiter changes (sleep, TCP,
  process, offload, AsyncMutex, channel all go through `capture_parked`/`wake` unchanged). What the promise
  carries is the other half, the **awaiter's** strand (read from the same context at `co_await`), which
  `final_suspend` compares with the strand it finishes on. Foreign coroutine types keep the ScopedResumer fallback.
- **Precedence: strand > block_on home > host Resumer > homeless.** Differs from the order suggested for this step
  (Resumer above block_on): ADR-219 already put `block_on` above the Resumer ("a block_on() inside the scope still
  owns what parks under it") and nothing in ADR-237 reverses it; strand and block_on cannot coexist (block_on is
  refused on lanes). Strand above Resumer because a strand continuation handed to a host Resumer would run on a host
  thread concurrently with the strand's next continuation (I1); so a `ScopedResumer` or raw `task::resume()` opened
  inside a strand slice inherits the strand.
- `on_strand` from a non-strand coroutine (block_on or Resumer home) captures that home and wakes it from the root's
  completion; from a homeless coroutine it is refused before suspending (`rt.on_strand_homeless`) — resuming it
  inline would run it on the lane. Each root (`run`, `enter`, `on_strand`) runs under a fresh holder id (await
  chains are a later step).
- `enter`'s continuation is handed to the Resumer through an ADR-219 ticket; the `CompletionThread` is a Resumer
  (what its continuations park on later comes back to it). Destroying a `CompletionThread` with an outstanding
  `enter`, destroying a `Runtime` from its own lane or while a `CompletionThread` is alive, and destroying a
  coroutine awaiting `enter` while its root still runs (interim, until §4.5 rule 3 adoption) are checked violations:
  abort with a message, every build.
- Shutdown (§4.5 rule 4): stop accepting (`rt.runtime_closed`) → wait for every `run`/`enter` root (the optional
  deadline only reports) → DNS and offload pools → lanes (drain, join) → reactor (completions delivered; posts to
  the stopped lanes dropped and counted).

Tests (`tests/rt/test_rt_lanes.cpp` L1–L11, 33 checks; `tests/rt/test_rt_runtime.cpp` R1–R10, 43 checks; same
counts on both OSes): I1 per strand under 6 lanes with wake-ups also arriving from the reactor thread (max in-flight
1, the strand moves between workers); FIFO; two strands in parallel; fairness — a 1-strand session against a
50-strand session gets 50.0% of slices on 1 lane and 33% on 2 (thresholds 40%/25%; per-strand round-robin gives
2%); priority order C1, C2, A1, B1; block_on refused on lanes; precedence strand > Resumer and block_on > Resumer;
cross-strand final_suspend; on_strand from block_on and homeless; same-strand await; drain and drop-after-stop;
`run` on a lane, value and exception; reactor and offload wake-ups back on the strand (not the reactor, host or
offload thread); `run`/`block_on` refused on a lane; `enter` via Resumer and via CompletionThread (never on a lane);
the CompletionThread violation in a child process; D5 config; `usable_cpus` under a 1- and 2-CPU affinity mask
(Linux `sched_setaffinity`, Windows `SetProcessAffinityMask`) plus the cgroup parsers; shutdown waiting for a sleeping
`enter` root and still delivering it; thread count before/after a Runtime (Linux exact 1/9/1; Windows 4/12/4, checked
as after ≤ before because OS loader threads may exit).

Mutants (19, each killed on Linux/gcc 15 and Windows/clang): M1 a running strand made ready again (L1 max in-flight
5–6, L8); M2 LIFO within a strand (L2, L5); M3 one strand at a time pool-wide (L3); M4 every strand its own session
(L4 2%); M5a priority ignored, M5b priority jumps its own strand (L5); M6 no lane marker (L6, R1, R2, R3, R4, R5,
R6); M7 Resumer checked before strand (L7); M8 final_suspend always transfers (L8: parent on S2, in-flight 2); M9 a
strand wake resumes inline (R2/R3 on the reactor/offload thread; L1 crash); M10 `enter` resumes the foreign coroutine
inline on the lane (R5, R6); M11 no CompletionThread check (R6 child exits 0); M12 `usable_cpus` ignores affinity
(R8 reports 12); M13 shutdown skips waiting for roots (R9); M14 lanes detached (R10 on Linux, L11); M15 homeless
`on_strand` not refused (L9); M16 a post after stop runs inline (L11); M17 ScopedResumer drops the strand, M18 raw
`resume()` drops the strand (L7); M19 the reactor refuses strand-homed waiters (L1, L7, R hangs). The first M1 (only
"ready while running") crashed instead of failing a check; it was narrowed to a mutant that lets the strand run twice
without corrupting the queue, and L1 then reports in-flight 5–6.

Linux (WSL, gcc 15, `-Werror`, Release): `rt` 72/72, the two tests 3 runs each. TSan (gcc 15, the two tests plus the
three reactor backends and `usable_cpus.cpp`): 5/5 runs each, 0 warnings — after two **test** fixes TSan found: the
host destroyed a foreign coroutine's frame right after the `done` flag its body set, while the body was still
finishing on the Resumer's thread (now self-destroying); and gcc's ramp function writes to the frame after a
`suspend_never`-initial coroutine first suspends, which races once the body has been resumed elsewhere (test
coroutines now start lazily). Engine tasks are lazy, so the second does not reach `rt::task`, but a host coroutine
type with an eager start awaiting `runtime.enter` has the same race — worth a sentence in 020 §3a. Also the thread
count now starts after one thread creation (TSan's own helper thread). Lints clean (`Canceler`, from the step-2
refactor, had no naming-lint allow; added).

Open after step 3:
- A strand-homed continuation already posted when its frame is destroyed is still resumed (ADR-175 §6's residual,
  now for strands too; no claim ticket). Only reachable by destroying a running engine chain, which root adoption
  (§4.5 rule 3) will own.
- A host `Resumer` that drops an `enter` continuation resumes it inline on the lane (ADR-219's drop fallback); §4.2
  "reported, never resumed" for a gone foreign home is not implemented.
- Not built: lane-stall and stuck-run watchdogs, task scopes / `when_all`, holder await chains, the D7 compile-time
  restriction, `stop_callback` redirection to strands; `run` takes no stop token, so shutdown can only wait.
- `usable_cpus`: Windows Job-object CPU rate caps are not read; the cgroup quota path is tested only through its
  parsers (no in-test cgroup on WSL).
- Scheduler: one mutex and linear removal from ready lists — fine for the tests, unmeasured (§8.2 benches).

### Step 4 (2026-10-03): DNS and TLS on the reactor (built in parallel with step 3, merged after it)

- **DNS** — `pal/resolver.hpp` + `src/backends/reactor_asio/resolver_getaddrinfo.cpp` (one blocking
  `getaddrinfo`, numeric `IpAddress` values, no policy); `rt/dns.hpp`: `rt::resolve` runs it on the dedicated
  DNS `OffloadPool` (§4.6), a numeric host skips DNS, a stop resumes the caller `canceled` while the lookup
  finishes in the pool and is discarded and counted. `rt::connect_resolved` is ADR-011's rule: resolve once,
  ask a caller `AddressPolicy` about every address, fail closed (no policy → `no_policy` before any lookup; a
  throwing policy rejects; none accepted → nothing connected), connect to accepted addresses in order with a
  per-attempt deadline. No happy-eyeballs; Windows `GetAddrInfoExW` not done (the pool on both OSes).
  Recorded deviation from M5: the per-attempt deadline requests stop on the reactor thread (it was built
  before strands landed) — safe because that stop source is private and its only callback posts a cancel;
  move it onto the strand with the other timer-driven stops.
- **TLS** — `rt/tls.hpp`, `src/backends/tls_mbedtls/` (`agentengine::rt_tls`, inside `AGENTENGINE_WITH_HTTPS`):
  `rt::TlsStream` on mbedTLS (D1). The BIO never touches a socket: send appends to an op-owned outgoing
  buffer, receive serves bytes already read or returns `WANT_READ`; the coroutine flushes and reads through
  `rt/tcp.hpp`, so mbedTLS runs only on the awaiting coroutine's home. ADR-013's client configuration
  (vendored CA bundle, verification required, TLS 1.2 floor, hostname check) moved verbatim into
  `mbedtls_client_config.{hpp,cpp}`, shared with the blocking `tls_client.cpp`, whose behaviour and error
  codes are unchanged. One op per direction (`busy`); a read and a write may run concurrently; a stop
  mid-operation cancels the TCP op (two-phase) and closes the stream (not resumable).
  **§6.3's "config to be checked" answered:** `MBEDTLS_THREADING_C` is off in the vendored build and TSan shows
  mbedTLS global state (ciphersuite list, ECP, PSA) racing between threads, so every mbedTLS call runs under
  one process-wide lock, held only for the synchronous call and never across a `co_await`.
- **Tests** — `test_rt_dns` (D1–D11; 42 checks on Windows, 43 on Linux; lookups beyond `localhost` via a test
  seam) and `test_rt_tls` (L1–L11, HTTPS only, loopback mbedTLS peer with a CA generated at start-up; 49 on
  both OSes — the Windows HTTPS build with clang-cl/llvm-mt). 16 mutants, all killed; "no deadline timer"
  first survived because Windows' ~21 s OS connect timeout satisfied D10's fallback branch — D10 now bounds
  every path. "Engine lock per thread" is killed by TSan (~1,170 reports). TSan 5/5, ASan 3/3 with mbedTLS
  instrumented.
- **Open after step 4** — the blocking `TlsClientSession` does not take the engine lock (its socket wait
  blocks inside mbedTLS), so it races any concurrent mbedTLS user, a pre-existing hazard that the async
  path makes reachable; the fix is `MBEDTLS_THREADING_C` (custom threading layer on Windows) — a build
  decision for the owner. mbedTLS work is serialised process-wide; an alert produced during a read waits for
  the next write; no handshake deadline; TLS buffers not pooled.

**Integration after steps 3 + 4** (merged tree): Windows `rt` 76/76, full suite 401/414 (the 13 failures
are the Docker-dependent tests, unchanged since step 1); Linux gcc 15 `-Werror` with HTTPS: full tree builds,
`rt` 77/77 (incl. `test_rt_tls`), `test_https_egress` / `test_provider_http_client` pass; naming and layering
lints clean.

### D6 (2026-10-03): `failure_class::canceled` and the classification sites (built in parallel with steps 5+)

Inventory first, as D6 required: `docs/research/2026-10-03-failure-class-canceled-inventory.md` — 62 sites in
nine kinds (one spelling and its copies 7, retry predicates 7, fallback/fail-over/propagate 3, other class
predicates 6, cancellation producers 13, deadline producers 11, protocol mappings 5, `run_canceled` event-kind
consumers 4, step 1-4 I/O results 6); 33 changed. Branch `adr237-canceled`.

- `include/agentengine/core/error.hpp` — `failure_class::canceled`; `failure_class_to_string` /
  `failure_class_from_string`, the one spelling (`canceled`, one l; `cancelled` is not a class name, only a
  legacy `stream_terminal`/`workflow_status` tag); `retry_budget {shared, fresh}` and
  `is_retryable(failure_class, retry_budget)`, **the one retry decision**. The naming-lint suppression on
  `failure_class` is gone: 027 §4 now lists `failure_class` and `retry_budget`.
- Retry sites now call it: the gateway (`shared`), the session's ADR-177 stream retry (`shared`; its
  `code != "net.cancelled"` exception removed), the eval re-draw (`shared`), `multi_agent::spawn_with_retry`
  and workflow edge `retry` (`fresh`).
- Never rerouted: `WorkflowSupervisor::route_from` ends the run at a `canceled` step before the edge-policy
  `switch` (no marker, no fallback executor); the gateway does not fail a `canceled` tier over to the next
  (`fails_over`, `call()` and `call_stream()` — not named in D6, found by the inventory).
- Producers: `run.canceled` (session `finish_canceled` and the drain), `net.cancelled`, replayed/recorded
  stream cancels, the eval summarizer's cancel, background jobs canceled before start / for a canceled parent
  / at shutdown, a vendor batch item `canceled`, the test driver's `test.real_tool_canceled`, a wrapped
  workflow that ended `cancelled`, and the drain's `run.stream_incomplete` when the inner stream ended
  `canceled` (found by test K8 — a re-classifying wrapper the grep could not see).
- Deadlines are `resource`: replayed `deadline_exceeded` (was `transient`), batch `max_wait` and vendor
  `expired` (were `transient`), a wrapped workflow's `bound_deadline` (was `contract`).
- Spec: 001 §5 (canceled vs out-of-time) and §6 (the class, the refined retry rule), 004 §4, 014 §6, 027 §4;
  ADR-178's "How does a run end?" row amended with a dated note; `decisions/README.md` rows 177/178.

Decisions:

- **Is a `resource` deadline retryable? Decided once:** never under the budget it exhausted (`shared`), yes
  by a fresh attempt with its own budget (`fresh`), bounded by the caller's attempt count. This keeps every
  pre-D6 behaviour — the gateway was transient-only, `multi_agent` and the supervisor transient+resource, a
  divergence `multi_agent.hpp` had flagged — and states why instead of repeating it per site.
- **Whose cancel ends the run.** The session ends `run_canceled` iff its own token fired (the old
  `code == "run.canceled"` test was redundant: the drain raises it only then). A `canceled` failure the run
  did not ask for ends `run_failed` with `canceled` carried to the caller — D6's "ends the run only if that
  caller lets it".
- **Wire values (I7).** A2A: a `canceled` run is `TASK_STATE_CANCELED` (was `FAILED`; A2A v1.0 terminal
  state, `docs/research/2026-a2a-and-agui-detail.md`), matching what `streaming.hpp` already did for the
  `run_canceled` event. AG-UI: unchanged — `RUN_ERROR{code: "run.canceled"}`, since AG-UI has no cancel event
  and `RUN_ERROR` is its sole error shape. MCP: unchanged — no class reaches the wire; a stopped tool is
  `completed` + `isError`, task `cancelled` only via `tasks/cancel`, `notifications/cancelled` is not for
  tasks (`docs/research/2026-mcp-protocol-detail.md`). OTel `error.type`: no exporter exists; open.
- `tool.canceled_no_effect` stays a code check in the background runner: it says "and no effect", which the
  class does not.
- Socket I/O timeouts (`net.connect_failed`) stay `transient`: a network condition, not the caller's budget.

Tests (positive controls, each with its control): `test_failure_class` (new; F1 the class x budget table,
F2 the gateway column, F3 spellings, F4 fail-over; three `static_assert`s); `test_model_call_gateway` G16
(canceled: one attempt, no fallback tier, `call()` and `call_stream()`; resource not retried in a shared
budget); `test_replay_chat_client` (6 D6) class per terminal and (10) every class round-trips through the
recording JSON; `test_rt_agent_session_cancel` K1/K2/K3/K5b assert class `canceled`, K8 the not-our-cancel
case; `test_rt_agent_session_stream_retry` P5 (class, not code); `test_rt_multi_agent` R3;
`test_rt_workflow_supervisor_failure_policies` D5a (no fallback), D5b (no propagate), D5c (no retry), D5d
(inner-run classes); `test_a2a_server` D3-11; `test_rt_background_job_runner` R1; `test_eval_gross_harm_screen`
S5g by class; `test_provider_http_client` P2 (`net.cancelled` class, HTTPS builds).

Mutants (Windows/clang, each applied alone, rebuilt, restored): M1 `is_retryable(canceled) = true` → killed
(`test_failure_class` does not compile — `static_assert`; G16, K8, P5, R3, D5c fail); M2 `route_from` canceled
check removed → D5a, D5b; M3 replay `deadline_exceeded` → `transient` → (6 D6) ×2; M4 replay `cancelled` →
`fatal` → (6 D6); M5 `finish_canceled` → `fatal` → K1, K2, K3, D3-11; M6 the drain's own `run.canceled` →
`fatal` → **survives, equivalent**: the session re-classes through `finish_canceled` whenever its token fired,
which is the only time the drain raises it; M7 session ends `run_canceled` on any `canceled` class → K8;
M8 A2A always `FAILED` → D3-11; M9 `fails_over` always true → F4, G16 ×3; M10 `canceled` spelled `"fatal"` →
`static_assert`, (10); M11 eval drops `canceled` from measurement faults → S5g, S10; M12 inner `cancelled` →
`contract` → D5d; M13 `run.stream_incomplete` always `transient` → K8; M14 `parent_canceled` → `policy` → R1.
On Linux (gcc 15): M15 `net.cancelled` back to `transient` → P2 in `test_provider_http_client` (HTTPS). Of the 15
mutants, 14 are killed and M6 is equivalent.

Results: Windows (clang, Debug) full suite `ctest -LE live-network` 402/415 — the 13 failures are the
Docker-daemon tests, unchanged since step 1. Linux (WSL, gcc 15, `-Werror`, Release, HTTPS on, own dir
`~/ae-canceled`): full tree builds clean; 350/366 — 15 Docker/containerd sandbox tests (no daemon) and
`test_json_dump_escape`'s M0 environment check ("an allocation of 1024 MiB is refused" — the memory cap is not
in force under this WSL; untouched by this change). Naming, layering and milestone-status lints clean (one
fewer naming-lint suppression).

Open after D6:
- A canceled attempt still counts as a circuit-breaker failure in the gateway (not the provider's fault;
  changing it touches half-open probe bookkeeping).
- `route_from` handles a round's replies in executor order, so when `when_all` cancels siblings after one
  fails, a canceled sibling can be named `failed_executor` instead of the cause. No in-round sibling cancel
  exists yet; the structured-concurrency step should route the originating failure first.
- Not yet mapped to a class: the step 1-4 I/O results (`sleep_status`, `offload_status`, `tcp_error`,
  `dns_error`/`connect_error` + `timed_out`, `tls_error`, process) — the rule for the seam conversion is a stop →
  `canceled`, a deadline (incl. `connect_error`'s `timed_out`) → `resource`; the parallel `rt/http*` client and
  `when_all` must follow it.
- Re-classifying wrappers (ledger, sandbox runtime, kata) fold inner errors into `fatal`/`policy`; none can be
  canceled today. OTel `error.type` has no exporter. A canceled tool call raises `RuntimeError` in the Python
  guest (code-keyed).

### Step 5 (2026-10-03): structured concurrency, await chains, root adoption, stop callbacks on strands, watchdogs

Additive: no engine caller is converted yet (`bounded_call_fanout`, `ThreadPool`, `fork_from_async`, the D7 compile
restriction are later steps). Windows/clang: full tree builds; `rt` 78/78 (incl. the `rt/agent_session`,
`rt/agent_spawn`, `rt/project` sub-labels); the other AsyncMutex users (`test_agent_session_bridge`,
`test_session_builder`, `test_composed_context_provider`, `test_approval_resume`, `test_sandbox_tool_provider`,
`test_tool_batch_parallel_dispatch`, `test_agentengine_test_driver`) pass; `test_mandatory_sandbox_provider`,
`test_task_branch_tools`, `test_task_branch_concurrent_dispatch` fail only on their real-container checks (Docker
daemon not running -- the step-1 baseline set).

- `include/agentengine/rt/scope.hpp` -- `rt::task_scope`, `co_await rt::with_scope(fn, ScopeOptions)`, `co_await
  rt::when_all(tasks...)` (tuple, void -> monostate) and `rt::when_all(std::vector<task<T>>, ScopeOptions)`. A scope
  opens only on a strand (`rt.scope_off_strand`); each child is a fresh strand of the owner's strand GROUP
  (`Strand::sibling()`), run in a self-freeing wrapper frame that destroys the child's task -- releasing every guard it
  held -- before reporting completion; the last child posts the owner back to ITS strand (one post per join). The first
  exception (or a throwing body) requests stop on the scope's `std::stop_source`; all children are joined, then it is
  rethrown. `when_all` writes each result into the slot of its position (I5). `max_concurrency` is a semaphore:
  over-cap children wait unstarted, in spawn order. A scope follows its owner's scope stop token and
  `ScopeOptions::stop`. Destroying a scope with live children aborts (every build, like step 3's violations).
- `include/agentengine/rt/await_chain.hpp` -- **how holder chains travel.** Holder ids stay integers in the execution
  context, strand items and parked records (ADR-175). A plain `co_await` keeps the awaiter's id: callee and awaiter are
  one logical task (already true: the callee runs in the awaiter's context and parks with its id). A *fork* mints an
  id and registers `{parent, awaited flag, stop token, loans}` in a process registry: `when_all` and `on_strand`
  children are always-awaited edges; a `with_scope` child's edge is awaited only while the owner is suspended in its
  join; `fresh_chains` scopes (job runner / background) and every Runtime root have no parent. Entries live exactly as
  long as the forked task (structured concurrency keeps every ancestor registered while a descendant can ask). The
  registry is consulted only on a contended `lock()` and at forks, and skipped when empty. `rt::scope_stop_token()`
  returns the current task's scope (or root) token -- how a lazily built `when_all` child sees its siblings' cancel.
- `include/agentengine/rt/async_mutex.hpp` -- the §4.3 lock rules. `lock(lock_entry::lend_safe | mutating)` returns
  `std::expected<Guard, lock_refusal>`; plain `lock()` is a mutating entry and throws `lock_refused` on refusal (that
  case was a silent deadlock before). A contended request is classified against the owner, then each lender top-down:
  awaiting ancestor + lend-safe -> LOAN (a stack; `Guard` carries its depth); descendant of a lender -> that lender's
  LOAN QUEUE, served before the FIFO; mutating under an awaiting ancestor, or anything under a non-awaiting ancestor ->
  `rt.lock_held_by_ancestor`; otherwise FIFO as before. Releasing a loan hands the lock to the next loan-queued
  borrower or back to the LENDER, never to the FIFO head; releasing a guard out of stack order aborts; a borrower that
  finishes with a loan outstanding aborts (checked where its scope / `on_strand` records it finished).
  `is_held_by_current_thread()` is unchanged (a borrower is the owner while it holds the loan), so ADR-123's
  `fork_from` and its tests are untouched; declaring `fork_from_async` lend-safe is the seam conversion's job.
- `include/agentengine/rt/runtime.hpp` -- **how adoption works.** An `enter` root now runs inside a self-freeing
  `RootFrame` that owns the engine task and a shared `EnterState`; a CAS on `phase` decides once who owns the end. If
  the root finishes first, the foreign continuation is handed to the Resumer as before. If the host destroys the
  awaiting coroutine first, `~EnterAwaiter` wins the CAS, counts the adoption and requests stop on the root's own stop
  source (its `scope_stop_token()`); the chain is intact, keeps running, finishes normally (locks and tickets released
  by their destructors), and is joined at shutdown like any root (`adopted_roots()`, `adopted_roots_finished()`). The
  destructor never touches the Runtime (shared counters), since a finished root may already have released it. The
  step-3 interim abort is gone. An `enter` continuation that a host Resumer DROPS is no longer resumed inline on the
  lane: its ticket carries a counter and it is never resumed (`dropped_continuations()`, §4.2). Every `run`/`enter`
  root is a fresh chain with its own stop source; shutdown now requests stop on every root before joining (§4.5 rule
  4). `RuntimeConfig::{lane_stall_bound, on_lane_stall, stuck_run_bound, on_stuck_run}`.
- `include/agentengine/rt/lanes.hpp` -- report-only watchdogs on one joined thread: LANE STALL (a worker inside one
  slice longer than the bound; once per slice, naming the strand) and STUCK RUN (a root's strand group with no slice
  started or ended within the bound; once per idle period). Slice timestamps are taken only when a bound is set.
  `Strand::sibling()`, `Strand::current()`, `Strand::group_id()`. `rt::on_strand` children are registered forks; an
  `on_strand` awaiter destroyed before it was resumed aborts (step 3's "frame destroyed after its wake was posted"
  gap, for strand awaiters).
- `include/agentengine/rt/strand_stop_callback.hpp` -- `rt::StrandStopCallback`: a `std::stop_callback` whose body is
  posted (`detail::HomedCall`, a one-shot self-freeing frame armed on the registering home) to the registering strand
  (or block_on home / Resumer), under the registering holder; a callback destroyed before its posted body ran never
  runs it. `rt/dns.hpp`: the per-attempt connect deadline (step 4's recorded deviation) now posts its stop to the
  attempt's home through `HomedCall` -- a few lines.
- `include/agentengine/rt/resume_home.hpp` -- `ResumerTicket::dropped_counter` and `ParkedContinuation`'s drop policy.

Tests: `tests/rt/test_rt_scope.cpp` S1-S9 (29 checks), `tests/rt/test_rt_lock_chain.cpp` C1-C10 (28 checks),
`tests/rt/test_rt_runtime.cpp` extended with R11 adoption, R12 dropped continuation, R13 watchdogs, R14 shutdown
cancels roots (59 checks, was 43); same counts on Windows and Linux. Both new tests also check that every registry
entry was removed.

Mutants (20, each killed on Windows/clang): M1 owner resumed when the first child finishes (S1; the owner then leaves
with live children and the scope violation fires); M2 a child failure does not stop its siblings (S3: 5012 ms, 0
canceled); M3 range results out of order (S4); M4 no live-children check (S6: child exits 0, no message); M5 cap
ignored (S5: max 8); M6 join resumes the owner inline on the last child's slice (S1: wrong strand); M7 a scope does
not follow its owner's token (S7: 5007 ms); M8 a stop callback runs inline (S9a/b/c: host thread, reactor thread,
disarm lost); M9 a released loan goes to the FIFO head (C2, C4 x2, C6); M10 a mutating entry may borrow (C1); M11 a
non-awaiting ancestor is not refused (C5); M12 nested loans do not stack (C6); M13 no loan-outstanding check (C7);
M14 adoption disabled (R11: the process aborts); M15 a dropped enter continuation resumed inline (R12); M16 no progress
stamp (R13: the progressing run reported stuck); M17 a stall reported on every scan (R13: 17 reports); M18 shutdown
joins without stopping roots (R14); M19 lane-stall watchdog silent (R13); M20 the moved DNS deadline never posts its
stop (`test_rt_dns` D10: 21 s).

Linux (WSL, gcc 15, `-Werror`, Release, own dir `~/ae-scopes`, HTTPS off): full tree builds; `rt` 75/75; the three
step-5 tests 29/28/59; the AsyncMutex users above (non-Docker) pass. TSan (gcc 15, each test standalone with the
reactor backends and `usable_cpus.cpp`): `test_rt_scope`, `test_rt_lock_chain`, `test_rt_runtime`, `test_rt_lanes`,
`test_rt_dns` 5/5 runs each, 0 warnings. Naming and layering lints clean (new names carry `ae-naming-lint: allow`).

Open after step 5:
- In-flight operation counts along the awaited chain (§4.5 rule 3's enforcement for non-root frames destroyed by
  `~task()`) are not built: adoption covers the root (`enter`), scope and `on_strand` awaiters abort when destroyed
  early, but a plain `rt::task` frame destroyed mid-flight by its owner is still destroyed, as before.
- Existing `std::stop_callback` bodies in engine code (`agent_spawn_child_run.hpp:210`, `WorkflowSupervisor::cancel`,
  job-runner cascades) are not yet moved onto `StrandStopCallback`; that is the inventory step of the conversion. A
  `StrandStopCallback` destroyed off its strand does not wait for a body already running (unlike
  `std::stop_callback`); a post after the lanes stopped is dropped and its small frame leaks.
- Only an exception cancels siblings; `result<T>` errors are values and cancel nothing (ADR-160 keeps per-call
  results). With D6 merged: the parent always sees the FIRST (originating) exception -- a sibling that throws after
  being stopped never replaces it (`first_fault` is set once); the siblings' own outcome is whatever their code
  reports on a stop, which for converted seams is `failure_class::canceled` (deadlines `resource`) -- `rt/` itself does
  not construct failure classes. Not touched: `WorkflowSupervisor::route_from` processes replies in executor order,
  so once batch nodes run as `when_all` siblings it could blame a canceled sibling instead of the executor that
  failed; it must route the originating failure first when that path is converted.
- The chain registry is one process-wide mutex, taken at forks and on contended locks; unmeasured (§8.2 benches).
- No engine entry point declares `lock_entry` yet; `session_mutex_` callers use plain `lock()` (mutating).
- Adoption covers `enter` (`run` cannot be dropped); an adopted root's result is discarded, reported by counters only.

### Step 6 (2026-10-03): HTTP/1.1 + SSE on the reactor (built in parallel with step 5 and D6, merged after them)

- `rt/http_wire.hpp` (no I/O: request building, strict head parser, RFC 9112 framing, capped chunked decoder,
  WHATWG SSE decoder), `rt/http.hpp` (`rt::http_request`, `rt::http_stream` → `HttpBodyReader::next`,
  `rt::sse_events` → `SseStream::next`), `rt/http_tls.hpp` (`rt::tls_connector()` over `rt::TlsStream`, HTTPS
  builds only). Connection only via `connect_resolved` with the caller's `AddressPolicy` (fail closed); no
  redirect followed (ADR-011 C10); `Connection: close` (pooling deferred); body cap min(limit, 16 MiB) on wire
  bytes, enforced during the read. **Stricter than the blocking client:** CR/LF/NUL/CTL rejected at build time
  on every path; caller-set Host/Content-Length/Transfer-Encoding/Connection refused; CRLF-only heads, no
  obs-fold, header byte/count caps; CL+TE, conflicting CLs and non-`chunked` TE refused (smuggling class);
  truncation reported, incl. HTTPS bodies ended without close_notify.
- Deadlines are timers: per-operation stop sources stopped by the caller, an overall timer and per-operation
  idle timers (precedence canceled > deadline_exceeded > idle_timeout); the TLS handshake is idle-bounded
  (closes step 4's "no handshake deadline" for this client). Consumer-stall deadline (§4.5): a timer after the
  head and each chunk/event closes the stream; `consumer_stalled` (a resource error), sticky. One read
  outstanding (`busy`), op-owned buffers.
- SSE decoder differs from the existing provider framing on multi-line `data:` (joined into one event, per
  WHATWG); a 400-seed differential against `SseEventFramer` + the Anthropic/OpenAI scans shows identical
  events on single-data-line streams, which is every real provider stream.
- Tests: `test_rt_http` (H1–H17, 81 checks, both OSes), `test_rt_https` (S1–S6, 16 checks, Linux HTTPS only).
  29 mutants: 28 killed on both OSes; "no CR/LF/NUL check on header values" alone is equivalent because the CTL
  check rejects the same bytes (dropping the CTL check alone, or both, is killed). TSan 5/5, ASan/UBSan 3/3
  (mbedTLS instrumented; sanitizer trees need `-Wno-error=maybe-uninitialized`, a gcc 15 + TSan false positive
  on a `stop_token` in a coroutine frame).
- **Open:** the timer stops are requested on the reactor thread (built before step 5's `StrandStopCallback`
  existed; the step-4 DNS deadline already moved) — move them onto it; pooled read-ahead (§6.3 gap 7);
  keep-alive; a Windows HTTPS run of `test_rt_https`; `failure_class` mapping of `http_error` (stop →
  `canceled`, deadline / stall → `resource`) with the seam conversion; the callers' switch-over (pass the
  sandbox `io_timeout_ms()`); one 2.5 s H11 run under `ctest -j2` on Linux, in the same run as failures of
  `test_rt_reactor_process` P8 and `test_rt_agent_session_background_task` — looks like machine load, not
  reproduced in 19 further runs; H11 now prints the phase if it recurs.
