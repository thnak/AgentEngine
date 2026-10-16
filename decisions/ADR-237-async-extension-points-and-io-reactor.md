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
  the current config is to be checked. **Checked 2026-10-09 (step 7): `MBEDTLS_THREADING_C` is required and
  is turned on; `MBEDTLS_THREADING_C` alone was not enough — the vendored `MBEDTLS_SELF_TEST` also had to be
  disabled, and the ciphersuite list warmed.**

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
  decision for the owner, **taken 2026-10-09 and done in step 7 below, which found it also had to disable
  `MBEDTLS_SELF_TEST`**. mbedTLS work is serialised process-wide (per stream as of step 7); an alert produced
  during a read waits for the next write; no handshake deadline; TLS buffers not pooled.

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

### Step 7 (2026-10-09): `MBEDTLS_THREADING_C` — mbedTLS made thread-safe, `rt::TlsStream`'s lock becomes per stream

Closes "Open after step 4": the async TLS path serialised every mbedTLS call behind one process-wide lock
(`tls_detail::engine_mutex()`) because mbedTLS 3.6's TLS 1.3/PSA globals are not thread-safe without
`MBEDTLS_THREADING_C`, while the blocking `sandbox::TlsClientSession` — whose BIO blocks inside mbedTLS —
could not take that lock and so raced it anyway. This step moves the locking *into* mbedTLS and deletes the
engine-wide lock.

- **`MBEDTLS_THREADING_C` + `MBEDTLS_THREADING_ALT`** (`src/backends/tls_mbedtls/agentengine_mbedtls_config.h`,
  set as `MBEDTLS_USER_CONFIG_FILE` on all three vendored targets **PUBLIC**, so every consumer compiles one
  configuration). The layer is this project's own `std::mutex` one: `threading_alt.h` defines the type as
  plain C (a handle on a `std::mutex`), `agentengine_mbedtls_threading.hpp` provides the four callbacks and
  an idempotent `ensure_threading()`. `MBEDTLS_THREADING_PTHREAD` was rejected — it is POSIX-only and this
  tree is Windows + Linux. `ensure_threading()` is called from `mbedtls_client::Context` **and** by an
  autostart object in the same header (a test's raw-mbedTLS loopback peer never builds a Context), so every
  mbedTLS user in the process is armed before its first call, before `main`.
- **The finding: `MBEDTLS_THREADING_C` alone was NOT enough — `MBEDTLS_SELF_TEST` had to go.** The vendored
  3.6.7 `mbedtls_config.h` ships `MBEDTLS_SELF_TEST` ENABLED (its own default), which compiles three
  process-global counters into `ecp.c` (`add_count`/`dbl_count`/`mul_count`, `library/ecp.c:89-91`) that every
  EC point operation increments (`INC_MUL_COUNT`, `ecp.c:1041`). `MBEDTLS_THREADING_C` does not guard them, so
  two concurrent TLS 1.3 handshakes race on `mul_count` exactly where the EC key agreement / ECDSA sign runs.
  The user config `#undef`s it (nothing in the tree calls a `*_self_test`; it is also work off the hot path).
  This is the "ECP" race §6.3 named, and it is why THREADING_C by itself was not the whole fix.
- **One more unguarded lazy table, warmed.** `library/ssl_ciphersuites.c`'s `supported_init` /
  `supported_ciphersuites` (built by the first `mbedtls_ssl_config_defaults`) is a plain `static`, also not
  covered by `MBEDTLS_THREADING_C`. `ensure_threading()` builds it once, single-threaded
  (`mbedtls_ssl_list_ciphersuites()`), so concurrent first handshakes cannot race it.
- **`rt::TlsStream`'s lock is now per stream** (`tls_stream_mbedtls.cpp`: one `std::mutex` in `Impl`), so two
  streams no longer serialise against each other; only one stream's concurrent read and write, which share its
  buffers and flags, are serialised. The public `tls_detail::engine_mutex()` is deleted; both rt tests drop
  their peer-side locks (mbedTLS is now safe for the peer itself), and `test_rt_tls` gains **L12** — six
  `TlsStream`s handshake and echo at once, one thread each, every byte intact — the positive control the
  whole change is measured against.
- **Evidence.** Windows (MSVC, HTTPS): `test_rt_tls`, `test_rt_https`, `test_provider_http_client`,
  `test_https_egress` all pass. Linux gcc 15 `-Werror` (HTTPS): builds and both rt tests pass. TSan (gcc 15,
  mbedTLS instrumented): **with `MBEDTLS_SELF_TEST` on, every report is the `mul_count` race — 145 distinct
  `SUMMARY`s in the saved run, all `mbedtls_mpi_mul_mod` (test_rt_https alone counts 777)**; with it off,
  **0**, and `test_rt_tls` + `test_rt_https` pass 3/3 each (they also hung the S4 server-close check before the
  fix, on the same contention). ASan/UBSan + LSan (gcc 15, HTTPS, mbedTLS instrumented): both rt tests pass,
  no leak (the mbedTLS global mutexes live behind global handles). Layering and naming lints clean.
- **Open after step 7 (small):** a Windows HTTPS run of `test_rt_https` (still Linux-only, §14 step 6); the
  blocking `TlsClientSession` now shares mbedTLS's internal locking, but its own `mbedtls_client::Context` is
  still built per connection; `mbedtls_threading_free_alt()` is not called (the global mutexes are
  process-lifetime and reachable from globals — no leak).

### Step 8 (2026-10-09): seam conversion, slice 1 — `ChunkingPolicy::chunk` and `SkillSource::load_skills`

The first slice of §5.1's "seam signatures" phase. The primitives (steps 1–7) are done, so this converts the
two smallest rows and fixes the pattern the remaining ~18 seams copy: a concept or host callable returns
`ae::task<...>`; engine coroutines `co_await` it; a genuinely-synchronous host drives it with `rt::block_on` at
that edge, never inside engine code.

- **`ChunkingPolicy::chunk`** (`core/corpus_source.hpp`) → `task<std::vector<TextChunk>> chunk(std::string) const`
  (text **by value**: a lazy `task` frame must not hold a caller reference across the await). Callers in
  `DiskCorpusSource::mount`/`mount_hybrid` `co_await` it (both were already coroutines). The body never parks;
  it is awaitable but not yet offloaded (a chunker holds no pool — named residual).
- **`SkillSource::load_skills`** (`core/skill_source.hpp`) → `task<result<std::vector<SkillSourceResult>>>`
  (`InlineSkillSource`, `DiskSkillSource`, and `SkillSourceDescriptor::load_skills`'s `std::function`). Disk
  reads stay inline (local, resolve-once, async in name only, owner decision A8).
- **`SkillsProvider`** (`core/skill_provider.hpp`): `resolve_and_mount()` and `ensure_loaded()` are now
  coroutines, `on_context` `co_await`s `ensure_loaded`; the cached `loaded_`/`load_error_` pair keeps a second
  call from re-resolving (it was a *synchronous* entry point before this — that was its whole justification).
- **Sync edges** now call `rt::block_on`: `native_jail::materialize_skill_mounts`
  (`src/backends/native_jail/skill_mount_materializer.hpp`, a configuration-time host helper),
  `cli_chat.cpp`'s `print_skills_banner`, and `examples/11_skill_mount.cpp`.
- **Tests**: the direct `load_skills()`/`ensure_loaded()`/`chunk()` calls in `test_corpus_source`,
  `test_skill_source_inline`, `test_skill_source_disk`, `test_external_skill_discovery`,
  `test_on_demand_skill_mount`, `test_skill_premount`, `test_extract_pdf_text` go through the existing
  `test_support::run_task_sync` driver (still sound: no seam parks yet). No new test file.
- **Docs**: `web/marketing/`'s `apiContent.ts` signature/enum snippets updated to the `task<...>` shapes.
- **Evidence.** Windows (MSVC, full tree `/W4 /WX`): the nine affected tests + `example_11_skill_mount` pass.
  Linux gcc 15 `-Werror`: `test_skill_source_inline` + `test_skill_source_disk` build and pass — the
  rag/provider/materializer tests are `if(WIN32)`-gated in this repo (`tests/core/rag`, `tests/core/skills`),
  so those TUs are compiled only under MSVC. `test_skill_provider_mount` R3 is the positive control — the resolve-once
  cache still holds across the new `co_await` in `ensure_loaded` (a mutant that drops the `loaded_` guard
  re-resolves and fails R3). Layering and naming lints clean. `test_extract_pdf_text` is gated behind
  `AGENTENGINE_WITH_PDF` (no such build exists), so its one-line change is covered by construction, not a run.
- **Open after step 8 (small):** `materialize_skill_mounts`' `rt::block_on` edge goes when the host-side
  drivers move onto the Runtime (D7); `DiskSkillSource` offload when a `SkillSource` gains a pool; the rest of
  §5.1 (tools §5.2, sandbox/egress/secret, stores, deciders, eval, MCP/A2A, `BatchBackend`) is next.

### Step 9 (2026-10-09): tool seam, slice 1 — `ToolDescriptor::invoke` returns `task<...>`

The first slice of §5.2 ("one way to declare a tool"): the **type-erased seam signature** becomes
asynchronous, while the pipeline that drives it is still synchronous (D2's "seam signatures → callers →
deletion" order — this is the first of the three).

- **`ToolDescriptor::InvokeFn`** (`core/tool_descriptor.hpp`) →
  `std::function<task<result<json::Value>>(json::Value const&, EffectContext&)>` — 006 §1's
  `ae::task<result<Reply>>`, type-erased.
- **The two factories** keep their inputs unchanged (still the synchronous `ToolT::invoke` / a
  synchronous `custom_invoke`, so §5.2's shipped tool bodies and the ~30
  `make_tool_descriptor_with_invoke` call sites are untouched here) and lift them into the async seam
  internally.
- **`make_sync_invoke`** (new, transitional): lifts a synchronous json-level closure into the async
  seam, so the hand-built descriptors (`todo_provider`, `plan_execute_mode`, `memory_provider`, both
  RAG providers, `native_providers`, `native_shell_session_provider`, `eval_stub_tool`,
  `mcp_tool_bridge`, `wasm_tool_bridge`) take one wrap rather than a `co_return` rewrite. Deleted once
  those are migrated to `Tool<>`.
- **The three synchronous trampolines** drive the async invoke with `rt::drive_leaf_task` — sound
  because each wrapping coroutine calls a synchronous body and never parks: `tool_pipeline.cpp`'s
  `run_admitted_call` and `background_task`'s detached worker, and `background_job_runner.hpp`'s
  `run_job`.
- **The two descriptor wrappers** (`cli_chat.cpp`'s output-cap wrapper, `test_driver.hpp`'s
  `recording_tool`) are now coroutines that `co_await` the inner invoke.
- **Tests**: every direct `.invoke(...)`/`invoke_tool(...)` call site (24 files) drives the task with
  `test_support::run_task_sync`; hand-built descriptor closures in tests wrap with `make_sync_invoke`.
- **Build**: `agentengine_scenario_runner` now links `agentengine::mediated_shell_runner` (as the test
  driver already does). ADR-208 G6 had it link headers only; the coroutine factory now emits the
  `run_shell` closure eagerly, so the real symbols must resolve.
- **Evidence.** Windows (MSVC, full tree `/W4 /WX`): the 25 affected tests pass, incl.
  `test_tool_pipeline`, `test_agentengine_test_driver`, `test_rt_background_job_runner`,
  `test_native_shell_session_provider`. Linux gcc 15 `-Werror`: full tree builds; the 12 cross-platform
  tool tests pass.
- **Open after step 9 (larger):** §5.2's remaining slices — the pipeline itself async (`invoke_tool`/
  `dispatch_tool_calls`/`background_task` → `task`, callers `co_await`, genuinely-sync edges
  `rt::block_on`); `Tool<>`'s authoring contract → instance `const task<result<Reply>> invoke`;
  `ToolTable::from`; delete `make_tool_descriptor_with_invoke`, `make_sync_invoke`, and the 15 sentinel
  `invoke`s; migrate the ~11 static hand-built descriptors to `Tool<>`; then `drive_leaf_task` /
  `synchronous_leaf` go.

### Step 10 (2026-10-09): tool seam, slice 2 — the pipeline itself is async

The second slice of §5.2 (D2's "seam signatures → callers → deletion" order): now that the seam
signature is `task<...>` (step 9), the **pipeline that drives it** is converted too, so an async
`invoke` is awaited end to end instead of trampolined. Additive for every shipped tool (they still
resolve inline and never park), but a tool that genuinely parks now suspends its caller's own await
chain rather than deadlocking a `block_on`.

- **`run_admitted_call`** (`src/core/tool_pipeline.cpp`) → `task<AdmittedCallOutcome>`: step 8
  `co_await`s `tool.invoke(...)` directly — the transitional `rt::drive_leaf_task` is gone from this
  path. Steps 9 (normalize) and 10 (revoke, unconditional) are unchanged.
- **`invoke_tool`** (`core/tool_pipeline.hpp`/`src/core/tool_pipeline.cpp`) → `task<ToolResult>`:
  `co_await`s `run_admitted_call`; steps 1/4/7/5 (`admit_call`) stay synchronous inside the coroutine
  body, so the audit `finish()` closure and the `{run_id, turn_index, call_index, digest}` idempotency
  derivation are byte-for-byte as before.
- **`background_task`** → `task<result<void>>`: the coroutine body performs only the synchronous
  admission (steps 1, 4/7, 5) and then detaches its worker (step 8), so awaiting it never parks for the
  tool's own duration — same observable behaviour, new signature. The detached worker still drives the
  async invoke with `rt::drive_leaf_task` (a genuinely-sync edge; unchanged here).
- **`AgentSessionCore::dispatch_tool_calls`** (`rt/agent_session_core.hpp`/`src/rt/agent_session_core.cpp`)
  → `task<std::vector<DispatchedCall>>`: the sequential path `co_await`s `invoke_tool`; the fan-out path
  (ADR-160) drives `run_admitted_call` with `rt::block_on` on each `run_jobs_bounded` `std::jthread` — a
  plain ThreadPool-style worker, not a lane, so `block_on` is allowed (`bounded_call_fanout.hpp` marks
  no thread). Its two callers (`run_rounds`, `finish_hook_processed_round`, both already coroutines)
  `co_await` it.
- **Genuinely-sync host edges** drive the async entry points with `rt::block_on`, the one sanctioned
  sync driver (`block_on.hpp`): `invoke_agent_tool` (`core/agent_registry.hpp`), the MCP tool-call and
  background handlers (`protocol/mcp/server.hpp`), the native-jail `bridge_tool_call`
  (`src/backends/native_jail/tool_bridge.hpp`), and `examples/11_skill_mount.cpp`.
  `src/rt/agent_session_core.cpp` and the three host headers each gained a `rt/block_on.hpp` include
  (core L2 → rt L0 is down-layer, already precedented by `session_builder.hpp`).
- **Tests**: every free `invoke_tool(...)`/`background_task(...)` call site (21 files, 78 calls) now
  drives the task with `test_support::run_task_sync<...>` and includes `tests/support/run_task_sync.hpp`
  — the same driver step 9 established. Comment prose naming `invoke_tool()`/`background_task()` is left
  untouched.
- **Evidence.** Windows (MSVC 14.51, full tree `/W4 /WX`, dev preset): builds clean, no warnings. The
  27 targeted tests — every wrapped file plus `test_agent_tool_invocation`, `test_tool_bridge`,
  `test_rt_background_job_runner`, `example_11_skill_mount`, and the two `sandbox_tool_provider`
  compile-fail controls — pass 27/27. Full suite `ctest -LE live-network`: 401/415, the failures being
  the 13 Docker-daemon sandbox tests (the unchanged step-1 baseline) and
  `test_external_skill_discovery`'s **D6**, an explicitly *opportunistic* check against this machine's
  real `~/.claude/skills` (which currently holds only `synced/` — no symlinked `firecrawl-*` skill dirs),
  a host-state difference rather than a regression: nothing in this change touches skill discovery.
- **Open after step 10 (smaller, but sharp):** the last two trampolines on this seam are
  `background_task`'s detached `std::thread` and `background_job_runner.hpp`'s `run_job` — both go when
  `AgentSession::start_background_task` moves onto `rt::BackgroundJobRunner` (ADR-181 phase 5) and
  `drive_leaf_task`/`synchronous_leaf` are deleted. The four host `rt::block_on` edges
  (`invoke_agent_tool`, MCP, the native-jail bridge, the example) go when those callers move onto the
  Runtime/strands (§5.2 callers). Then the rest of §5.2: `Tool<>`'s instance
  `const task<result<Reply>> invoke`, `ToolTable::from`, deletion of `make_tool_descriptor_with_invoke`/
  `make_sync_invoke`/the sentinel `invoke`s, and migrating the ~11 hand-built descriptors.

### Step 11 (2026-10-09): §5.2 "one way to declare a tool", slice 1 — the instance contract enters

The first, additive move of §5.2's authoring-contract change: introduce the target shape and its
registration path without yet forcing the ~130 existing conformers over.

- **Contract (`core/tool.hpp`)**: the §5.2 target form is a non-static **`const`**
  `agentengine::task<result<Reply>> invoke(Args, EffectContext&)` — a stateless tool is an empty object,
  a stateful one carries injected state — documented at the contract site; `core/task.hpp` is now
  included by `tool.hpp`. The older `static result<Reply> invoke` is recorded as accepted
  **transitionally** and deleted once every conformer is converted.
- **Mechanism (`core/tool_descriptor.hpp`)**: a second, value-taking
  `make_tool_descriptor<ToolT>(ToolT tool)` moves the tool into a descriptor-owned `std::shared_ptr` and
  the seam calls its instance `invoke`; it carries the `Deterministic`/`EnforcesGrantedCaps` claims from
  the declaration (the instance invoke **is** the declared code, unlike `make_tool_descriptor_with_invoke`).
  `ToolTable::from(ToolT{...}, ...)` registers instances; `from()` is an empty table. Neither existing
  path changes, so nothing regresses.
- **Policy declaration (`core/tool.hpp`)**: `CapturesSessionState` — the explicit tag §5.2 requires
  (red-team G2) — with `declared_captures_session_state()`, read by both factories into
  `ToolDescriptor::captures_session_state`. This is what lets a migrated stateful descriptor keep the
  `true` that `make_tool_descriptor_with_invoke` set implicitly, instead of defaulting `false` (which
  would silently make a configured tool backgroundable and rerun-comparable).
- **First conformers converted**: `tools::ReadSandboxFile` and `tools::ReadContent` — both stateless, so
  `captures_session_state` stays `false`, exactly as their old `make_tool_descriptor<T>()` default (their
  `invoke_via`/`invoke_from_sandbox` helpers stay static). `tools/test_driver/test_driver.hpp` now
  registers `ReadSandboxFile` through the value-taking factory.
- **Tests**: `test_read_content` and `test_effect_context_sandbox_fs` drive the async invoke with
  `test_support::run_task_sync`.
- **Evidence.** Windows (MSVC 14.51, dev preset `/W4 /WX`): full tree builds clean; `test_read_content`,
  `test_effect_context_sandbox_fs`, `test_agentengine_test_driver` pass; full suite `ctest -LE
  live-network` 401/415 — the identical 14 (13 Docker + `test_external_skill_discovery`'s opportunistic
  D6) as step 10, i.e. no new failures from a change that touches `tool.hpp` for every consumer.
- **Open (the bulk of §5.2):** the remaining `Tool<>` conformers (mostly tests) and the ~30
  `make_tool_descriptor_with_invoke` / ~20 `make_sync_invoke` sites still use the transitional paths. With
  `CapturesSessionState` now available, the ~11 hand-built **stateful** descriptors can migrate to
  `Tool<>` instances (declaring the tag where they held session state), then the remaining conformers and
  test drivers, then the deletion of the static `result<Reply> invoke` form /
  `make_tool_descriptor_with_invoke` / `make_sync_invoke` / the sentinel `invoke`s — the next slices.

### Step 12 (2026-10-10): §5.2 slice 2 — the hand-built stateful descriptors start moving

`CapturesSessionState` (step 11) unblocks the stateful half of the migration. This step converts the
hand-built descriptors whose state is a plain shared block, leaving the two that are still entangled
with an unmigrated seam (below).

- **`TodoProvider`** (`core/todo_provider.hpp`, 5 tools): `todos_add`/`todos_complete`/`todos_remove`/
  `todos_get_remaining`/`todos_get_all` become `todo_detail::Todo*Tool` `Tool<>` conformers holding the
  shared `TodoState`; the two read-only ones carry `EffectClass<pure>`; all five declare
  `CapturesSessionState`, so their `captures_session_state` stays `true` exactly as the old
  `make_sync_invoke([this]...)` closures set it. The bounds and `render()` moved into `todo_detail`
  (`TodoProvider::kMaxItems`/`kMaxTitleLength` remain public aliases for existing callers).
- **`PlanExecuteMode`** (`core/plan_execute_mode.hpp`, `plan_ready`): a `plan_execute_detail::PlanReadyTool`
  `Tool<>` holding the shared read-only `TodoState` and the shared `GateState`, declaring
  `CapturesSessionState`.
- **`MemoryProvider`** (`core/memory_provider.hpp`, `recall`): a `memory_detail`-adjacent
  `RecallTool<OS,RS>` `Tool<>` that holds the provider's read-only config by value and is therefore
  **not** `CapturesSessionState` (matching the old copy-capturing closure). Its ceiling is known only at
  provider construction, so `make_recall_tool_descriptor()` still sets `capability_ceiling` on the
  returned descriptor (ADR-153's must-fix, preserved verbatim) — the `Tool<>` declares `Capabilities<>`
  empty. §9 D3 replaces that post-hoc set with a per-call requirement.
- **Deferred (still hand-built, with reasons):** the two RAG providers' `recall` tools
  (`vector_rag_context_provider.hpp`, `hybrid_rag_context_provider.hpp`) drive their `Embedder`/index
  through `rt::drive_leaf_task` behind `synchronous_leaf` gates; converting them to an async `invoke`
  removes those gates and is entangled with §5.1's async embedder/index seams, so it lands with that
  work. The two native-process providers must move together with §9 **D3** (their empty ceiling plus
  in-body grant matching is exactly what D3 replaces with a per-call requirement).
- **Evidence.** Windows (MSVC 14.51, dev preset `/W4 /WX`): full tree builds clean; `test_todo_provider`,
  `test_plan_execute_mode`, `test_harness_session`, `test_memory_provider`, `test_memory_codeact_bridging`,
  `test_memory_no_authority_laundering` and the rest of the memory/eval/RAG set pass; `tools/naming_lint.py`
  OK; full suite `ctest -LE live-network` 401/415 — the identical 14 as steps 10/11, no new failures.

### Step 12 open items (unchanged from step 11): the RAG `recall` tools (with §5.1), the native-process
providers (with D3), the remaining test `Tool<>` conformers, and the deletion of the static `invoke`/
`make_tool_descriptor_with_invoke`/`make_sync_invoke`/sentinels.

### Step 12a (2026-10-10): code-review pass over the §5.2 tool-contract changes

A focused review (no BLOCKER/HIGH; behavior-preserving, I2 intact) produced these fixes:

- **Include hygiene.** `core/tool.hpp` no longer includes `core/task.hpp` — it never names `task`, and
  `core/tool_descriptor.hpp` (plus the two conformers that spell `task<...>`, `tools/read_content.hpp`
  and `tools/read_sandbox_file.hpp`) include it directly. Stops `rt/task.hpp`'s `<thread>`/`<mutex>`/
  `<condition_variable>` fan-out through every `tool.hpp`-only includer.
- **`ToolTable::from(...)` coverage.** It had zero call sites/tests; `test_tool_table_scoping.cpp` now has
  R5 — `from()` (empty), `from(LiveTool{...})`, field-equality against `make_tool_descriptor(T{...})`, and
  a driven call proving the value-taking factory keeps the live instance alive.
- **N1 (per-turn allocations) resolved.** The five `TodoProvider` tools, `PlanExecuteMode`'s `plan_ready`,
  and `MemoryProvider`'s `recall` are now built **once in the owning provider's constructor** and cached
  (`TodoProvider::tools_`, `PlanExecuteMode::plan_ready_tool_`, `MemoryProvider::recall_tool_`), instead
  of `make_shared<ToolT>` + schema re-parse on every `on_context()`/`make_recall_tool_descriptor()`.
  `on_context()` copies the cached descriptors into its contribution.
- **M1 documented.** `RecallTool` holds raw `OS*`/`RS*` (the provider's config by value); the
  provider-outlives-descriptor precondition is now stated explicitly on the type (ownership change is out
  of scope — the host owns the stores).
- **Prose.** Refreshed the stale file-top comments in `todo_provider.hpp`/`tool_descriptor.hpp`, merged
  the duplicate `plan_execute_detail` block, and fixed the "invoke closure" wording in
  `plan_execute_mode.hpp`.
- **Evidence.** Windows (MSVC 14.51, `/W4 /WX`): full tree clean; naming-lint OK; layering-lint OK; the
  affected provider/context/memory/tools tests pass; full suite `ctest -LE live-network` 401/415, the
  identical 14 baseline failures.

### Step 13 (2026-10-10): §5.2 stateful-conformer migration begins — `ToolOptimizerProvider`

The first production `make_tool_descriptor_with_invoke<ToolT>` site converted to §5.2's instance form,
establishing the pattern the remaining stateful descriptors copy: a shared, session-scoped state object
behind one `shared_ptr` (the `TodoProvider`/`TodoState` precedent), held by value by each tool instance.

- `search_tools`/`mount_tool`/`unmount_tool` are now `Tool<..., EffectClass<pure>, CapturesSessionState>`
  conformers with a non-static `const task<result<Reply>> invoke`; the dead static poison `invoke()` stubs
  are gone -- the instance invoke IS the one real path.
- New `ToolOptimizerState` (`agent_tools` + both source fetches + `always_on` + the mutable `mounted` set),
  owned by `ToolOptimizerProvider` and handed to each tool, so a tool's `invoke()` mutates the SAME mount
  set the next turn's `on_context()` scopes from -- across every copy a `ComposedContextProvider` makes --
  and no closure captures the provider's `this` (the ADR-028 dangling-capture hazard).
- The real logic moved to free functions over `ToolOptimizerState` (`build_universe`,
  `real_search_tools`/`real_mount_tool`/`real_unmount_tool`), reached by both `on_context()` and the tools
  -- one implementation, no per-copy state. Public API unchanged (ctor, `on_context`, `on_turn_end`,
  `currently_scoped_tools`).
- Evidence: `test_tool_optimizer_provider` R1-R6 pass (incl. R6, the real `AgentSession` declare/invoke
  cadence over the composed 3-provider stack); full tree `/W4 /WX` clean; naming/layering lint OK; full
  suite `ctest -LE live-network` 401/415, the identical 14 baseline failures.

The same slice migrated `trust/secret_quarantine.hpp`'s `QuarantineSecretTool` (ADR-068 §2e) the same
way -- an instance-form `Tool<..., EffectClass<pure>, CapturesSessionState>` holding the quarantining
store by pointer (the provider owns it and outlives the descriptor), with its dead static `invoke()`
poison stub removed. `test_secret_quarantine`'s old "dead static invoke fails safely" case is replaced
by one asserting the descriptor declares `CapturesSessionState` and keeps `EffectClass<pure>` -- the
§5.2 replacement for the old factory's implicit `true`. `test_secret_quarantine` and
`test_rt_agent_session_quarantine_tool` pass; full tree clean, lints OK, suite 401/415.

### Step 14 open: the remaining `make_tool_descriptor_with_invoke` production sites follow step 13's
pattern -- `sandbox/mandatory_sandbox_provider.hpp`
(RunCommand + the four task-branch tools),
`native_jail/session_shell_wiring.hpp` (RunShellTool),
`tools/cli_chat.cpp` (ExecuteCodeTool/MountSkillTool). The
dynamic-schema bridges (`mcp_tool_bridge.hpp`, `wasm_tool_bridge.hpp`, `native_providers.hpp`,
`native_shell_session_provider.hpp`, the two RAG providers, `eval_stub_tool.hpp`) can never be `Tool<>`
conformers -- their Args/Reply are runtime-derived -- so they lose `make_sync_invoke` and get a direct
async `invoke` lambda instead.

### Step 15 (2026-10-10): dynamic-schema bridges begin losing `make_sync_invoke`

`eval/eval_stub_tool.hpp` and `protocol/mcp/mcp_tool_bridge.hpp` now build their `invoke` as a direct
coroutine lambda (`-> task<result<json::Value>>` with `co_return`) instead of wrapping a synchronous body
in `make_sync_invoke` -- same behavior, one fewer transitional hop, and the tools keep their existing
`captures_session_state` (false for the eval stub; the WASM bridge below keeps `true`).
Evidence: test_eval_stub_tool, test_eval_trial_driver/_follow_rate_screen/_gross_harm_screen/
_tier1_screen and test_codeact_tool_union pass; full tree `/W4 /WX` clean; naming/layering lint OK.

`src/backends/wasm/wasm_tool_bridge.hpp` is the same one-line change but compiles only under
`AGENTENGINE_WITH_WASM` (OFF in every configured build tree here; enabling it fetches the vendored
wasmtime release), so it is deliberately LEFT on `make_sync_invoke` until a WASM-enabled build can verify
it -- not converted unverified. The remaining dynamic-schema sites (`native_providers.hpp`,
`native_shell_session_provider.hpp`, the two RAG providers) move with D3/§5.1.

### Step 16 (2026-10-10): `ScheduleWakeupTool` to instance form (first big-owner conversion)

`ScheduleWakeupTool` (rt/agent_session_core.hpp) was the one place in the codebase whose tool closure
reached back into its owning session by capturing `this` (ADR-028 §1's own exhaustive check). It is now
an instance-form `Tool<ScheduleWakeupTool, CapturesSessionState>` holding an explicit `AgentSessionCore*`;
its `invoke` (defined in agent_session_core.cpp, where the class is complete) calls the new private
`AgentSessionCore::dispatch_schedule_wakeup()` -- the old closure's body, moved verbatim, reached through
a `friend` declaration. `AgentSessionCore` gained a forward declaration above the tool; the former static
poison `invoke()` sentinel is gone, and `schedule_wakeup_tool()` now returns
`make_tool_descriptor(ScheduleWakeupTool{*this})`.

- `captures_session_state` is preserved (declared `CapturesSessionState`), so ADR-061 §20.5/§20.6's
  unlocked-direct-dispatch discipline and the background/parallel refusals are unchanged.
- This is the template for the rest of the big-owner sites (mandatory_sandbox_provider, agent_spawn,
  cli_chat, session_shell_wiring): tool holds an explicit owner handle, invoke defined where the owner is
  complete, dispatch body moved to a member, `CapturesSessionState` declared.
- Evidence: test_rt_agent_session_schedule_wakeup_tool, test_rt_agent_session_tooling_and_delegation,
  test_tool_deterministic and test_tool_batch_partition (plus the deterministic compile-fail control) pass;
  full tree (469 targets) `/W4 /WX` clean; naming/layering lint OK; full suite `ctest -LE live-network`
  401/415, the identical 14 baseline failures.

### Step 17 (2026-10-10): `ShellExecTool` to instance form (first templated-provider conversion)

`sandbox/live_shell_sandbox_provider.hpp`'s `ShellExecTool` follows Step 16's template, with one twist:
`LiveShellSandboxProvider` is a class template, so `ShellExecTool` becomes `template <class ProviderT>` --
it holds `ProviderT*`, and its out-of-line `invoke()` (defined after the provider, where it is complete)
reaches the provider's private `shell_exec()` through a `template <class> friend struct ShellExecTool;`
declaration. It keeps `Capabilities<cap::decl::RunCommand>` and adds `CapturesSessionState`; the static
poison sentinel is gone and `on_context()` now pushes
`make_tool_descriptor(ShellExecTool<LiveShellSandboxProvider>{*this})`.

Evidence: test_live_shell_sandbox_provider passes (test_live_shell_sandbox_provider_live is one of the 14
Docker baseline failures); full tree `/W4 /WX` clean; naming/layering lint OK; full suite 401/415.

### Step 18 (2026-10-10): `AgentSpawnTool` to instance form (second templated-provider conversion)

`rt/agent_spawn.hpp`'s `AgentSpawnTool` follows Step 17: `AgentSpawnToolProvider` is
`template <AppendLogStore StoreT>`, so the tool becomes `template <AppendLogStore StoreT>`, holds
`AgentSpawnToolProvider<StoreT>*` (the provider itself forward-declared above it), and its out-of-line
`invoke()` (defined at the end of the header, where the provider is complete) calls the provider's new
private `dispatch_agent_spawn()` -- the old closure's body, calling the same free
`perform_agent_spawn()` -- through a `template <AppendLogStore> friend struct AgentSpawnTool;`
declaration. It declares `CapturesSessionState`; the static poison sentinel is gone and `on_context()`
pushes `make_tool_descriptor(AgentSpawnTool<StoreT>{*this})`.

Evidence: test_rt_agent_spawn, test_rt_agent_spawn_child_run, test_spawn_budget, test_agent_spawn_capability,
test_agent_spawn_worktree, test_delegation_provenance, test_rt_spawn_cost_budget, test_rt_spawn_pump_concurrency
and test_rt_agent_session_tooling_and_delegation pass; full tree `/W4 /WX` clean; naming/layering lint OK;
full suite 401/415.

### Step 19 (2026-10-10): test-side `make_sync_invoke` sweep begins

Four test-side authors that lifted a synchronous closure with `make_sync_invoke` now build the coroutine
`invoke` directly (no behavior change), removing four call sites of the transitional factory ahead of its
deletion: `tools/test_driver/test_driver.hpp` (the ADR-208 replay double),
`tests/core/tools/test_tool_registry.cpp`, `tests/core/context/test_turn_middleware.cpp`, and
`tests/rt/test_rt_background_job_runner.cpp` (its `tool()` helper keeps its synchronous-callable API and
wraps it in the coroutine inline).

Evidence: test_tool_registry, test_turn_middleware (+ test_rt_agent_session_turn_middleware),
test_rt_background_job_runner and test_agentengine_test_driver pass; full tree `/W4 /WX` clean;
naming/layering lint OK; full suite 401/415 (a single transient `test_rt_workflow_batch_coalescing`
failure under `-j4` re-ran clean -- a pre-existing flake, not a regression). Remaining test-side sites:
`tests/testing/test_agentengine_test_driver.cpp`, `tests/core/tools/test_approval_resume.cpp`,
`tests/core/context/test_tool_optimizer_provider.cpp`, `tests/core/agent/test_agent_yaml_compiler.cpp`,
`tests/python/test_agent_tools_codegen.cpp`.

### Step 20 (2026-10-10): test-side `make_sync_invoke` sweep complete

The remaining five test-side call sites (`test_agentengine_test_driver.cpp`, `test_approval_resume.cpp`,
`test_tool_optimizer_provider.cpp`, `test_agent_yaml_compiler.cpp`'s two, `test_agent_tools_codegen.cpp`)
now build their coroutine `invoke` directly. `make_sync_invoke` now appears ONLY at its definition
(`tool_descriptor.hpp`), inside comments, and at the explicitly-deferred sites: the two RAG providers
(with §5.1), the two native-process providers (with D3), and the WASM bridge (needs a WASM-enabled build).
Every other transitional site is gone.

Evidence: test_agentengine_test_driver, test_approval_resume, test_agent_yaml_compiler,
test_agent_tools_codegen, test_tool_optimizer_provider and test_codeact_tool_union pass; full tree
`/W4 /WX` clean; naming/layering lint OK; full suite 401/415.

### Step 21 (2026-10-10): `mandatory_sandbox_provider` -- five tools to instance form

All five tools this provider contributes (`run_command` plus the four task-branch verbs) are now
instance-form `Tool<>` conformers, templated on the owning provider type and holding it explicitly (the
provider is a class template). Each has a non-static `invoke()` defined below the class, reaching a new
private `dispatch_*()` through a `template <class> friend struct ...;` declaration; `on_context()` now
registers `make_tool_descriptor(RunCommandTool<MandatorySandboxProvider>{*this})` and its four siblings.
Each `dispatch_*()` body is the old `on_context()` closure moved verbatim and still wrapped in
`with_image_provenance()`, so ADR-176 §14's post-body stamping lands in exactly the same place and order.
(The wrapper's parameters are named `request`/`context` rather than `args`/`ctx` so the moved body's own
`args`/`ctx` do not shadow an outer parameter, a level-4 warning under `/WX`.) The five static poison
`invoke()` sentinels are gone.

Field-for-field equivalence with the old `make_tool_descriptor_with_invoke<T>()` factory, checked against
that factory's own body: same name/description/capability ceiling/approval/backgroundable/effect_class/
exclusivity_group/parallelizable, `deterministic` false in both (neither tool declares `Deterministic`),
and `captures_session_state` true in both -- the factory hardcoded it, and the new tools declare
`CapturesSessionState`, which `make_tool_descriptor` reads through `declared_captures_session_state()`.

Evidence: test_mandatory_sandbox_provider_composed (drives `on_context()` and asserts the contributed set:
`run_command` absent before `bind_sandbox()`, present on the very first call after it, exactly two tools
once bound, still correct on a forked composed provider), test_run_command_commit_failure and
test_task_branch_durability_recovery pass; full tree `/W4 /WX` clean; naming/layering lint OK; full suite
401/415. The six Docker-gated tests in this area (test_mandatory_sandbox_provider, test_task_branch_tools,
test_task_branch_concurrent_dispatch, test_sandbox_runtime, test_execution_surface_image_identity,
test_live_shell_sandbox_provider_live) fail identically to baseline with "is a Docker daemon
reachable?"/"failed to connect to the docker API" -- compile-verification only in this preset, as before;
no daemon is available here to exercise the migrated dispatch bodies end to end.

### Step 22 (2026-10-10): `cli_chat`'s two tools to instance form, and the declaration-only universe closed

`tools/cli_chat.cpp`'s `ExecuteCodeTool` and `MountSkillTool` are now instance-form `Tool<>` conformers
templated on `ToolDeclaringHistoryProvider`, holding it explicitly and reaching its private
`real_execute_code()`/`real_mount_skill()` through `template <class> friend struct ...;`.
`CapturesSessionState` preserves the factory's hardcoded `captures_session_state = true`; for
`MountSkillTool` that is the `pure` + `CapturesSessionState` combination `QuarantineSecretTool` already
uses (no external effect, state that belongs to the session). The two static poison `invoke()` sentinels
are gone.

The second half is what made this site non-mechanical. `print_skills_banner()` is a free function with no
host to hand the tools to, and it built `ToolTable::from_tools<ExecuteCodeTool, MountSkillTool>()` purely
for the two DECLARATIONS -- to print what is invocable at startup. A zero-argument static table does not
exist once the tools are instance-form, so the provider gained a public `declared_universe()` that builds
both descriptors from live instances; `on_context()` now scopes from that same source and the banner takes
it as a parameter. Declared and invocable therefore come from one place instead of two.

Evidence: this file is outside build-dev (that tree builds `test_cli_chat_plan` only -- the decision half,
`tools/cli_chat_plan.hpp`), so it was compile-verified where the target actually exists:
`cmake --build build --target agentengine_cli_chat`, in the tree configured with
`AGENTENGINE_WITH_HTTPS=ON` and `AGENTENGINE_BUILD_PYTHON_RUNNER=ON` (the two conditions CMakeLists
requires for the executable), compiles `tools/cli_chat.cpp` and links `agentengine_cli_chat.exe` with no
errors. build-dev rebuild clean; naming-lint OK; layering-lint OK; full suite 401/415, the identical 14
baseline failures.

Remaining §5.2 production site: `session_shell_wiring.hpp`'s `RunShellTool`, still blocked on
`tools/test_driver/test_driver.hpp`'s static-shape use.

### Step 23 (2026-10-10): `RunShellTool` to instance form -- no production `with_invoke` site left

`src/backends/native_jail/session_shell_wiring.hpp`'s `RunShellTool` is an instance-form `Tool<>`
conformer holding the owning `SessionShellSandbox` explicitly. It is deliberately NOT templated on that
owner: the owner is a concrete class, so the name `RunShellTool` is unchanged (027's row and every doc
reference keep working) and the ADR-208 test driver keeps naming `RunShellTool::Reply`. It declares
`CapturesSessionState`, preserving what the old factory closure set.

The poison sentinel became a real, used failure path rather than being deleted. The ADR-208 test driver
records from a *declared shape* and deliberately constructs no sandbox (`real_tool_standins()`, ADR-208
G6), so `RunShellTool` gained a default constructor with a null owner, and its `invoke()` -- out-of-line,
where `SessionShellSandbox` is complete, reaching the private `run()` through `friend struct RunShellTool;`
-- fails loudly when the owner is null: the same loud failure the old stub produced, not a silent run
against no sandbox. `tool_descriptor()` now returns `make_tool_descriptor(RunShellTool{*this})`, and the
test driver's stand-in is `make_tool_descriptor(RunShellTool{})`.

**Milestone: no production `make_tool_descriptor_with_invoke` call site remains.** A grep over every
`.cpp`/`.hpp` in the tree leaves only the factory's own definition (`tool_descriptor.hpp:229`), prose in
comments, the deliberately-unbuilt `docs/planning/proofs/`, and test files that exist to exercise the
factory itself. Every production tool that ever used it is now a `Tool<>` conformer, and no production
static poison `invoke()` sentinel is left (the remaining `*_invoke_unreachable`/`dead_static_invoke_path`
strings are test-local tools plus `RunShellTool`'s null-owner path, which is the live stand-in contract).

Evidence: test_session_shell_wiring, test_agentengine_test_driver and scenario_scripted_shell_roundtrip
pass (the last two drive the stand-in through the real recording/replay path), as do the other 23
shell/mediated/native_jail tests in that selection; the only two failures there are the baseline
Docker-gated `test_docker_persistent_shell_surface`/`test_live_shell_sandbox_provider_live`. build-dev
rebuild clean; naming-lint OK; layering-lint OK; full suite 401/415, the identical 14.

Next §5.2 step: delete `make_tool_descriptor_with_invoke` itself and rewrite the ~10 test files that
exercise it.

### Step 24 (2026-10-10): test-side `make_tool_descriptor_with_invoke` sweep, batch 1

The factory has zero production call sites (step 23) but 15 test-local ones, and deleting it requires
converting them. This batch takes the first three files (four sites):

- `tests/core/skills/test_skill_premount.cpp`: `MountSkillTool` becomes instance form holding the
  `MountedSkillsState` it mutates and declares `CapturesSessionState`; the poison static `invoke()` is
  gone and `PremountProvider` registers `make_tool_descriptor(MountSkillTool{mounted_})`.
- `tests/core/tools/test_tool_batch_partition.cpp`: `StatefulTool` declares `Parallelizable` +
  `CapturesSessionState` explicitly -- the same `captures_session_state && parallelizable` combination the
  factory used to manufacture for ADR-160 §5's MUST-FIX 1 check, now coming from the tool's own
  declaration. That check is the regression proof the conversion had to preserve.
- `tests/core/tools/test_approval_resume.cpp`: `AskTool`/`FreeAskTool` become instance form with their
  real bodies (they read `ctx.codeact_preseeded_answers`), the two helpers they call are declared before
  them, and both providers register by value.

Evidence: build-dev rebuild clean; `test_skill_premount`, `test_tool_batch_partition`,
`test_approval_resume` and `test_tool_deterministic` (plus its compile-fail control) all pass, 5/5.

Remaining: 11 more call sites in 7 files, then the factory itself goes.

### Step 25 (2026-10-10): test-side `make_tool_descriptor_with_invoke` sweep, batch 2 (owner-free half)

Three more sites convert -- the fixtures whose real body needs no provider at all:

- `tests/rt/agent_session/test_rt_agent_session_tooling_and_delegation.cpp`: `CounterTool` holds the
  provider's own session-scoped counter (`int*`) and declares `CapturesSessionState`. Its old static
  `invoke()` returned the sentinel `-999` purely to prove the factory ran the CALLER's callable -- a
  proof the instance form makes unnecessary, since the instance invoke *is* the real code.
  `BackgroundableStatefulTool` declares `Backgroundable` + `CapturesSessionState` itself, so S3's
  "state-capturing AND backgroundable is refused" check now reads the tool's own declaration.
  **S4 had to be repointed, not just edited**: it existed to pin the zero-argument
  `make_tool_descriptor<ToolT>()`'s default (`captures_session_state = false`) and was using
  `BackgroundableStatefulTool` to do it -- impossible once that type is instance-form. It now uses a new,
  deliberately-static `StatelessBackgroundableTool`, which dies with the static form.
- `tests/rt/agent_session/test_rt_agent_session_codeact_ask_max_turns.cpp`: `AskPendingTool`'s
  ask-pending body becomes its instance `invoke`.

Evidence: build-dev rebuild clean; `test_rt_agent_session_tooling_and_delegation` and
`test_rt_agent_session_codeact_ask_max_turns` pass. (The first build of this batch FAILED -- S4 was still
resolving the zero-arg factory for a now-instance-form tool, `tool_descriptor.hpp:211`; that failure is
what forced the repoint above rather than a silent behaviour change.) naming-lint OK; layering-lint OK;
full suite 401/415, the identical 14.

Remaining: 7 sites, all owner-holding -- the tool calls a PRIVATE provider method, so it needs a forward
declaration, `friend`, and an out-of-line `invoke` defined where the owner is complete:
`MountTool`/`UseRunnerTool` (`tooling_and_delegation`), `SpawnTool` (`spawn_child_run`),
`ExecuteCodeTool` (`suspend_codeact_ask`), `MountSkillTool`/`ExecuteCodeTool` (`farm_ops`),
`MountSkillTool` (`farm_advisory`).

### Step 26 (2026-10-10): `make_tool_descriptor_with_invoke` DELETED

Slices 14-15 converted the factory's call sites; this step removes the factory itself, which §5.2 names
explicitly ("`make_tool_descriptor_with_invoke` and every sentinel `invoke` (16 tools) are deleted"):

- `include/agentengine/core/tool_descriptor.hpp`: the function and its ADR-028 doc block are gone, with a
  short note recording where the call sites went. The `captures_session_state` field comment and
  `ToolTable::from()`'s comment no longer describe the factory in the present tense, and the
  `args_schema_value` caching comment now names the two surviving factories.
- `core/tool.hpp`: the `EnforcesGrantedCaps` and `CapturesSessionState` tag comments no longer reference
  the deleted custom-invoke path.
- `tests/core/tools/test_tool_deterministic.cpp`: its D2 case -- which asserted the factory dropped
  `Deterministic` -- is deleted with the factory. Nothing replaces it: the behaviour it pinned (a
  factory-supplied closure is not the code the tag describes) no longer has a path to exist.

**Sentinel `invoke()`s are gone from compiled code.** A grep for
`dead_static_invoke_path`/`invoke_unreachable`/`must never run` over every `.cpp`/`.hpp` now finds only
(a) the deliberately-unbuilt `docs/planning/proofs/`, (b) `RunShellTool`'s null-owner failure path, which
is the live owner-less stand-in contract rather than a poison stub, and (c) one explanatory comment.

Evidence: `cmake --build build-dev` AND `cmake --build build --target agentengine_cli_chat` both clean --
the second because `tool_descriptor.hpp` is shared and `agentengine_cli_chat` lives in the
HTTPS + Python-runner tree, so a header change there has to be re-verified there. naming-lint OK;
layering-lint OK; full suite `ctest -LE live-network` 401/415, the identical 14. `test_tool_deterministic`
(+ its compile-fail control), `test_tool_batch_partition` and `test_approval_resume` pass.

§5.2 is NOT closed by this. Still open: the transitional zero-argument `make_tool_descriptor<ToolT>()` /
`from_tools<Ts...>()` static-invoke form (its remaining conformers are mostly tests); `make_sync_invoke`'s
five production sites (two RAG providers, with §5.1's embedder/index seams; two native-process providers,
with §9 D3; the WASM bridge, which needs a WASM-enabled build to verify).

### Step 27 (2026-10-10): §5.1 seam conversion, slice 2 -- `ToolSourceFetch` is async

The first §5.1 seam after step 8, chosen because the inventory shows it is the smallest: one typedef, two
`ToolOptimizerState` fields, one test helper, and **no production supplier at all** -- the only producers
are still `no_tool_source()` and a test's `fetch_one()`.

- `core/tool_optimizer_provider.hpp`: `ToolSourceFetch` becomes
  `std::function<task<result<std::vector<ToolDescriptor>>>(EffectContext&)>` -- a source may do real I/O
  (a remote MCP server, a plugin host), which is the §5.1 reason for the seam. `no_tool_source()` returns
  a coroutine lambda.
- The consumers follow rather than being left half-async: `build_universe()` becomes
  `task<result<ToolTable>>` and `co_await`s both fetches; `real_search_tools`/`real_mount_tool` become
  coroutines (`real_unmount_tool` fetches nothing and stays a plain function); the two tools that call
  them `co_return co_await ...`; `on_context()` `co_await`s `build_universe`; and
  `currently_scoped_tools()` -- a documented public accessor with **zero call sites in the tree** --
  becomes `task<result<ToolTable>>` rather than keeping a synchronous facade over an async seam.
- `tests/core/context/test_tool_optimizer_provider.cpp`: `fetch_one()`'s lambda returns a task.

Evidence: build-dev rebuild clean; `test_tool_optimizer_provider` (incl. R6, the real `AgentSession`
declare/invoke cadence over the composed three-provider stack), `test_agent_yaml_compiler` and
`test_agent_tools_codegen` pass; naming-lint OK; layering-lint OK; full suite `ctest -LE live-network`
401/415, the identical 14. The `agentengine_cli_chat` target does not include this header and was already
up to date.

### Step 28 (2026-10-10): §5.1 seam conversion, slice 3 -- the A2A client boundary is async

The §5.1 inventory names "A2A `CardFetcher`/`RunStarter`/client calls". This slice takes the client half,
and takes it as a WHOLE rather than seam-by-seam: `A2aClient` has **no production consumer at all** (only
`tests/protocol/a2a/test_a2a_client.cpp`), and converting `CardFetcher` alone would have left
`send_message`/`get_task`/`cancel_task` synchronous over an async transport -- an inconsistency with no
justification, given that the whole reason these are seams is that reaching a remote peer is a network
round trip.

- `protocol/a2a/client.hpp`: `RemoteAgentTransport`'s three callables and `CardFetcher` all become
  `std::function<task<result<...>>(...)>`, and `A2aClient`'s four verbs become coroutines that `co_await`
  them. Adds the `rt/task.hpp` include -- the `protocol/a2a -> rt` layer edge was already established by
  `server.hpp`'s own `rt/agent_session.hpp`.
- `tests/protocol/a2a/test_a2a_client.cpp`: the transport stand-ins and the four hand-rolled
  `CardFetcher`s become coroutines, and every client verb is driven through the test's existing local
  `drive()` helper. All seven D4 assertions are unchanged in substance -- the real-server passthrough
  (D4-1/2/3) and the digest-pinned card cases (D4-4/5/6/7, including the rug-pull detection and the
  fetcher-failure propagation), which is the point: making the seam async must not move any behaviour.

Deliberately NOT in this slice: `A2aServer`'s own `RunStarter`, also named in the inventory. The server is
the other role of the same protocol and is a self-contained synchronous component whose verbs drive a
session run to completion in place, so converting it is a separate change with its own two test files
(`test_a2a_server.cpp`, `test_task_principal_binding.cpp`) rather than a rider on this one.

Evidence: build-dev rebuild clean; all 7 A2A tests pass (`test_a2a_client`, `test_a2a_server`,
`test_task_principal_binding`, `test_a2a_types`, `test_a2a_agent_card`, `test_a2a_mapping`,
`test_a2a_streaming`); naming-lint OK; layering-lint OK; full suite `ctest -LE live-network` 401/415, the
identical 14.

### Step 29 (2026-10-10): §5.1 seam conversion, slice 4 -- the A2A server half

The other half of the inventory's "A2A `CardFetcher`/`RunStarter`/client calls" (step 28 took the client).

- `protocol/a2a/server.hpp`: `RunStarter` becomes `std::function<task<result<RunOutcome>>(rt::StartRun)>`
  and `A2aServer::send_message()` becomes a coroutine that `co_await`s it. `get_task()`/`cancel_task()`
  stay synchronous **on purpose**: they are pure in-memory map lookups with no I/O behind them, so there is
  no seam there to make async -- converting them would be ceremony, not the removal of a blocking call.
- The one thing worth checking by hand rather than mechanically: `send_message()` takes a
  `std::lock_guard<std::mutex>` over its task store. `co_await starter_(...)` is the ONLY suspension point
  and sits deliberately BEFORE the guard, so no mutex is ever held across a suspension. A comment at that
  line now says so -- the next person to add a `co_await` below the guard would otherwise hold the lock
  across a resume.
- Test updates: the `RunStarter` stand-ins (`Harness::starter()` plus the two inline lambdas in
  `test_a2a_server.cpp`, and the ones in `test_task_principal_binding.cpp` and `test_a2a_client.cpp`)
  become coroutines, and every `send_message()` call site is driven locally. `test_task_principal_binding.cpp`
  needed a local `drive()` helper ADDED -- it had none, because it had never driven a task before.
- Deliberately NOT treated as the risky case it looked like: D3-11's mid-run cancel
  (`test_a2a_server.cpp`). The cancel lands from `set_run_event_tap` on the SAME thread, inside the run, so
  there is no cross-thread timing assumption for the async conversion to disturb. Read and verified.

Evidence: build-dev rebuild clean. An earlier attempt failed to compile -- `test_task_principal_binding.cpp`
has no `art` alias and no `drive()` helper, both of which the new call sites assumed -- and was fixed
rather than worked around. All 7 A2A tests pass on FRESHLY built binaries: the first run printed "passed"
against stale binaries, the exact trap recorded in step 24-25's finding, caught by reading the build
output rather than the test output. naming-lint OK; layering-lint OK; full suite `ctest -LE live-network`
401/415, the identical 14.

### Step 30 (2026-10-10): §5.1 seam conversion, slice 5 -- `GraderFn` is async

Chosen over `OutputValidator` after sizing both, and the sizing is the finding: `OutputValidator` is NOT
the small change the inventory implied. The validator is *stored* into the session
(`AgentSession::set_output_schema`), so converting it reaches `AgentSession`'s core plus six test call
sites. `GraderFn` is the contained one -- it lives entirely in `eval/`.

- `eval/eval_grader.hpp`: `GraderFn` becomes `std::function<task<grade_outcome>(TrialResult const&)>`, and
  `make_tool_argument_grader()`'s lambda becomes a coroutine. Adds the `rt/task.hpp` include.
- `eval/eval_screen_common.hpp`: `grade_trial()` becomes a coroutine. **The §3.5 rule is preserved, not
  re-derived:** a throwing grader is still `ungraded`, because `rt::task` contains a body exception in
  `unhandled_exception()` and rethrows it at the awaiting `co_await`, so `grade_trial`'s `try/catch`
  around the `co_await` still sees it. That is exactly what S13 in `test_eval_gross_harm_screen` and
  `throwing_on_boom_grader` in `test_eval_follow_rate_screen` pin, and both pass unchanged.
- The two screens needed **one line each**: both `run_*_screen` internals were ALREADY coroutines
  (`co_await detail::run_trial_with_retry(...)` on the line above), so `detail::grade_trial(...)` becomes
  `co_await detail::grade_trial(...)` and nothing else moves -- no new cascade to chase.
- Tests: the three hand-rolled grader factories (`task_grader`, `throwing_on_boom_grader`,
  `final_value_grader`) become coroutines, and the five direct `grade_trial` calls become `drive(...)`.
  Both screen tests already had a local `drive()` helper, so nothing new was needed there.

Evidence: build-dev rebuild clean; all 11 eval tests pass, including `test_eval_tier1_screen`,
`test_eval_gross_harm_screen` and `test_eval_follow_rate_screen`; naming-lint OK; layering-lint OK; full
suite `ctest -LE live-network` 401/415, the identical 14.

Next: MCP. **The three typedefs are NOT separable the way this step first assumed -- do not plan the split
along typedef lines.** `InputRequestHandler` is invoked inside `call_tool`'s own MRTR round loop
(`client.hpp:462`), so converting it makes `call_tool` a coroutine and drags the sender typedefs and
`mcp_tool_bridge.hpp` along with it: `McpClient` is one unit. It also has a real production consumer
(`tools/mcp_conformance_client.cpp`, a `RequestSenderWithHeaders` and a real input handler), unlike the A2A
and `ToolSourceFetch` slices, which had none. Then `OutputValidator`, which reaches
`AgentSession::set_output_schema` and six test call sites.

**MCP resume inventory (recorded here so the next session starts cold -- no re-exploration needed).** The
conversion is atomic: the three typedefs and every `McpClient` verb that invokes them must move in one
commit, because `McpClient` has no sync/async split to hide behind.

- `protocol/mcp/client.hpp` -- `RequestSender` (:49), `InputRequestHandler` (:77), `RequestSenderWithHeaders`
  (:317) go `task`-shaped. Every invocation is inside `McpClient`: the transport picker at `:587` (prefers
  `sender_with_headers_`, falls back to `sender_`) and the MRTR round loop at `:462` (`input_handler_`).
- Verbs that become coroutines with them: `list_tools` (:344), `call_tool` (:411), and the task-extension
  pair around `:520` (`call_tool_as_task` and its poll path). `get`/`cancel`-style pure lookups, if any, stay
  sync -- the A2A precedent.
- `protocol/mcp/mcp_tool_bridge.hpp` -- `mcp_tools_as_descriptors_from` (its descriptor lambda at `:40` calls
  `client->call_tool`) and `mcp_tools_as_descriptors` (:75, calls `list_tools`) both become coroutines; the
  `ToolDescriptor` lambdas they hand out already return tasks, so that side needs nothing.
- Consumers: `tools/mcp_conformance_client.cpp` (`:290` ctor, `:329` `list_tools`, `:429` `call_tool`, plus a
  real `RequestSenderWithHeaders` at `:193` and a real input handler at `:297`). **Open question to settle
  first, and it is the one thing that could make this slice bigger than it looks: how does that tool run a
  task?** It is a `tools/` binary, not a test, so `tests/support/run_task_sync.hpp` may not be reachable --
  if its `main` has no drive loop today, the slice has to add one. Then
  `tests/protocol/mcp/test_mcp_client.cpp` (~15 call sites; needs a local `drive()`),
  `tests/protocol/mcp/test_mcp_tasks_extension.cpp` (`:92` sender lambda, task verbs) and
  `tests/protocol/mcp/test_codeact_tool_union.cpp` (`:187`-`:219`).

### Step 31 (2026-10-11): §5.1 seam conversion, slice 6 -- the MCP client boundary is async (Phase A)

The first of §14.1's named phases, and the only seam that was fully inventoried: `McpClient` (client.hpp)
and its bridge (mcp_tool_bridge.hpp). Converted atomically, exactly as the inventory above requires --
`McpClient` has no sync/async split to hide behind, so the three typedefs and every verb that invokes one
move together.

- **`protocol/mcp/client.hpp`** -- `RequestSender` (`:49`), `RequestSenderWithHeaders` (`:317`) and
  `InputRequestHandler` (`:77`) are now `std::function<task<...>>`, and the private `send` funnel (the one
  place both sender shapes are invoked) is a coroutine too. Every verb that reaches the sender became a
  coroutine: `list_tools`, `call_tool` (its MRTR round loop `co_await`s `InputRequestHandler` per key),
  `call_tool_as_task`, `get_task`, `cancel_task`. The A2A precedent's "pure lookups stay sync" applies to
  **none** of them -- each invokes the sender, so each awaits it. Adds `core/task.hpp`; the body of each verb
  is unchanged (same cache key, same digest/rug-pull check, same `isError` surfacing, same MRTR id/state
  rules), only `return` -> `co_return` and the one `send` call per verb gained a `co_await`.
- **`protocol/mcp/mcp_tool_bridge.hpp`** -- `mcp_tools_as_descriptors_from` and
  `mcp_tools_as_descriptors` both return `task<...>`; the descriptor lambda it hands out (already a
  `task<result<json::Value>>`) now `co_await`s `client->call_tool`. The consumer seam is unchanged: a
  caller wraps this in a `ToolSourceFetch` lambda, which has been `task`-shaped since step 27, so no
  production caller needed editing.
- **`tools/mcp_conformance_client.cpp`** -- the drive-loop question settled first, as §14.1 required.
  The binary is a `tools/` target whose `main` is synchronous and cannot reach
  `tests/support/run_task_sync.hpp`, but it links `agentengine::core`, an INTERFACE library that already
  pulls in the rt runtime (`agentengine_rt_session` etc.), so `rt::block_on` -- the one sanctioned outer
  driver (`block_on.hpp`) -- is reachable with **no CMake change**. Its `RequestSenderWithHeaders` and its
  real `InputRequestHandler` become coroutines (their bodies call the blocking `perform_http_exchange`, so
  they never park), and `list_tools`/`call_tool` are driven with `rt::block_on`.
- **Tests** -- `test_mcp_client.cpp` and `test_mcp_tasks_extension.cpp` gain the local `drive()` helper the
  A2A client test uses (a resume-until-done loop, sound here because the sender never parks; the ADR
  flags `run_task_sync`/the naive loop as wrong only once a task can genuinely suspend). The server/mock
  sender stand-ins become coroutines. `test_codeact_tool_union.cpp` reuses its existing `run_task_sync` for
  `mcp_tools_as_descriptors`. `web/marketing`'s `apiContent.ts` snippet still compiles as written (a
  non-coroutine lambda returning a prvalue `task` is exactly what `ToolSourceFetch` wants).
- **Behaviour did not move** -- the standing rule of this ADR. All 9 `protocol/mcp` tests pass unchanged
  in substance: C3-1..C3-8 (real-server happy path, caching, ttl=0, pagination cache keys, rug-pull
  detection), C4-1..C4-10 (the tasks extension, incl. `Background<1>` capacity and sticky cancel) and
  U5/U6 (the bridged descriptor round-tripping through the real `McpClient`/`McpServer` pair).

Evidence: full tree `build-dev` clean (MSVC 14.51, `/W4 /WX`); `agentengine_mcp_conformance_client` builds;
`ctest -L protocol/mcp` **9/9**; naming-lint OK; layering-lint OK; full suite `ctest -LE live-network`
**401/415**, the identical 14 baseline failures (13 Docker-dependent sandbox tests + `test_external_skill
_discovery`'s opportunistic host-state check).

Open after step 31: the tool's `rt::block_on` edge goes when the host-side drivers move onto the Runtime
(D7), same as step 8's `materialize_skill_mounts` edge; the remaining §5.1 seams are §14.1's Phases B-F.

### Step 32 (2026-10-11): §5.1 seam conversion, slice 7 -- `OutputValidator` is async (Phase B, slice 1)

Phase B's first seam, taken with the test-first discipline §14.1 requires -- and a correction to that
phase's premise, recorded rather than absorbed (this ADR's standing rule).

- **The premise was too strong.** §14.1 recorded `OutputValidator` as having "zero test coverage today
  (only the build verifies it)". The seam's *behaviour* is in fact already pinned end to end:
  `test_agent_session_bridge` **P9** (`make_agent_session<SchemaAgent>`: a matching reply yields structured
  output, an invalid one fails `run.output_schema_validation_failed`, a metadata-only bind refuses),
  `test_agent_session_output_schema` **O1/O2/O5** (a hand-written validator through `set_output_schema`),
  and `test_rt_workflow_chat_client_outer_session` **O4** (the workflow outer session). What genuinely had
  no *direct* coverage was `output_validator_for<A>()` itself -- and P9's single `"not json at all"` reply
  cannot tell a **parse** error from a **schema mismatch**. So **P9b was ADDED to
  `test_agent_session_bridge`** (written and run green against the *sync* seam first, then re-run after the
  conversion): valid schema-shaped JSON passes; malformed JSON is rejected at the parse layer; JSON missing
  a required field is rejected at the `from_json` layer; an agent declaring no `OutputSchema<T>` produces no
  validator at all.
- **`core/agent_session_bridge.hpp`** -- `agent_bind_detail::OutputValidator` becomes
  `std::function<task<result<void>>(std::string_view)>`, and `output_validator_for<A>()`'s lambda is a
  coroutine (`co_return`s at its three exits; its body never parks). Adds `core/task.hpp`. `harness.hpp`
  threads the same alias through unchanged, so the harness path follows automatically.
- **`rt/agent_session_core.hpp`** -- `set_output_schema`'s parameter and the `output_schema_validate_`
  member both spell the `task`-returning `std::function` inline; the session deliberately does not name the
  bridge's alias (rt stays below core). **`src/rt/agent_session_core.cpp`** -- the one invocation site, in
  the already-coroutine round loop, is now `co_await output_schema_validate_(text_content)`; the
  fail-closed branch (the `run_failed` event + `run.output_schema_validation_failed`) is byte-for-byte
  unchanged.
- **Test call sites** (the six §14.1 named, plus P9b): `test_agent_session_output_schema`'s
  `make_demo_validator` and `test_rt_workflow_chat_client_outer_session`'s `validate` lambda became
  coroutines; P9b's direct calls go through `rt::block_on`.

Evidence: full tree `build-dev` clean (MSVC 14.51, `/W4 /WX`); `test_agent_session_bridge`,
`test_agent_session_output_schema` and `test_rt_workflow_chat_client_outer_session` pass; naming-lint OK;
layering-lint OK; full suite `ctest -LE live-network` **401/415**, the identical 14.

Open after step 32: the rest of Phase B -- `ChatClient`, `Resolver`, `SurfaceFactory`.

### Step 33 (2026-10-11): §4.5 rule 2 -- `rt::Runtime::spawn` / `rt::RootHandle` (the owned-root primitive Phase B needs)

Phase B's `ChatClient` slice must start a `chat_stream()` producer on the reactor with no per-call thread,
and -- by the owner's 2026-10-11 decision (the caller-passed-scope of §4.5 rule 3a was found impractical:
a `task_scope` is constructible only on a strand, while ~90 call sites call `chat_stream()` off any strand)
-- the CLIENT, not the caller, owns that producer. `rt::Runtime` had only `run` (blocking) and `enter`
(which must be `co_await`ed), so this step adds the missing primitive: a non-blocking OWNED root.

- `include/agentengine/rt/runtime.hpp` -- `Runtime::spawn(StrandGroup const&, task<T>)` (plus a
  `spawn(task<T>)` overload on a fresh group) posts the root to a fresh strand of the group and returns a
  `RootHandle` **immediately**. The root is tracked exactly like a `run`/`enter` root: `admit()` registers it
  with its own stop source, an await-chain link is added, `watch_root` arms the stuck-run watchdog, and
  shutdown requests stop on it and joins it. `RootHandle` exposes `done()`, `request_stop()`, `fault()` and
  `join()`; `join()` is refused on a lane/offload worker (`rt.block_on_on_lane`, §4.3). `RootHandle`
  deliberately does **not** join in its destructor -- the Runtime joins every live root at shutdown, so an
  abandoned handle cannot leak a thread, it only loses the ability to stop/join/observe.
- The frame (`runtime_detail::spawn_root`) mirrors `enter_root`: it owns the engine task and finalizes itself
  through `finish_spawn` (defined after `Runtime`, mirroring `finish_enter`), which removes the chain link,
  unwatches the group, marks the handle done, wakes a joiner, then `release_root` -- the last touch on the
  Runtime. A body exception is recorded on the handle, never `std::terminate`.
- Scope deviation, recorded: this is the owner's chosen alternative to §4.5 rule 3a's caller-passed scope
  (producer owned by the client, not the caller). The `chat_stream()` signature is UNCHANGED, so the ~190-site
  concept churn the original Phase B plan anticipated does not happen; the ADR's §4.5 rule 3a text is to be
  amended when this is Judged.

Evidence: `tests/rt/test_rt_spawn.cpp` S1-S6 -- the body runs on a lane (a strand is current; `block_on` is
refused there) and never on the host thread; `spawn()` is non-blocking (returns before the body finishes); a
root parks on the reactor (`rt::sleep_for`) and resumes on a lane; `request_stop()` cancels a 10 s park
promptly; a throwing root records `fault()` without terminating; destroying the Runtime stops + joins a
parked root promptly; `join()` on a lane is refused. `build-dev` full tree clean (MSVC 14.51, `/W4 /WX`);
`ctest -L rt` **80/80**; `ctest -LE live-network` **402/416** (`test_rt_spawn` added), the identical 14
baseline failures; naming-lint OK; layering-lint OK.

Open: not yet used -- the provider `ChatClient`'s `chat_stream` producer is the first consumer (Phase B,
next). The plan for that slice (client-owned scope, the reactor HTTP rewrite replacing `Resolver`, dropping
`chat()`) is written up at `~/.commandcode/plans/adr237-phase-b-chatclient-switchover.md`.

### Step 34 (2026-10-11): §5.1 Phase B -- the provider `chat_stream` on the reactor (the ChatClient switchover, slice 1)

The first half of the `ChatClient`/`Resolver` switchover. The vendor backends' `chat_stream` can now run its
producer as an OWNED reactor root (`rt::Runtime::spawn`, step 33) instead of a detached `std::thread`. The
change is additive and opt-in so the tree stays green throughout: a trailing `ProviderReactorOptions` on both
provider ctors, defaulted to the blocking path (null `runtime`), so all ~30 existing construction sites are
unchanged.

- `protocol/provider_chat_wire.hpp` -- `ProviderReactorOptions{runtime, policy, resolver, options}` (the
  reactor path's inputs) and `http_error_to_error` (step 6's named leftover: the reactor's `http_error` onto
  `failure_class`, reusing `rt::is_resource_error` for the resource class).
- `src/protocol/sse_stream_pump.hpp` -- `pump_sse_stream_async`, a COROUTINE twin of the blocking
  `pump_sse_stream` that suspends on `rt::http_stream` / `rt::sse_events`. All six terminal rules are
  preserved (transport error forwarded, non-2xx via `http_status_error`, mid-stream cut and "body never
  began" both `net.stream_truncated`, complete stream closes). rt's framing/SSE decoders do the chunk work,
  so the accumulators are fed `chunked=false`; each decoded event is re-serialized for the vendor accumulator
  with its `event:` NAME preserved when the wire carried one (Anthropic's named-event framing), so both wire
  dialects parse identically. `make_http_context` lives here (src/, HTTPS-only) so the TLS connector does not
  leak into the shared header; `idle_timeout` is `sandbox::io_timeout_ms()`.
- `protocol/openai/chat_client.hpp` + `src/protocol/openai/chat_client.cpp` (and the identical Anthropic
  pair) -- a trailing `ProviderReactorOptions` ctor param and `run_stream_worker_async`, a plain function that
  builds the same wire request and calls `reactor.runtime->spawn(pump_sse_stream_async<Accumulator>(...))`.
  `chat_stream` branches to it when `reactor_.runtime != nullptr`.
- `CMakeLists.txt` -- `agentengine_provider_http_client` now links `agentengine::rt_tls` PUBLIC (the library
  references `rt::TlsStream`); every consumer inherits it.

Evidence: the HTTPS tree (`build/`, VS generator, `AGENTENGINE_WITH_HTTPS=ON`) builds the provider library and
its four protocol test targets; the two loopback live tests (`test_openai_chat_client_live`,
`test_anthropic_chat_client_live`) pass unchanged (the blocking path). NEW cases in BOTH loopback tests
construct the client with a `rt::Runtime` + a loopback `HostResolver` and assert the same canned chunked-SSE
response (OpenAI's `data:`-only shape; Anthropic's NAMED-`event:` shape) decodes and delivers through
`ae::stream<T>` in order with 0 loss and reaches the success terminal -- driven by an owned root on the
reactor, no detached thread. `build-dev` is untouched (every change is inside `AGENTENGINE_WITH_HTTPS`), so its
402/416 is unchanged. A LIVE end-to-end run against the real OpenRouter API
(`test_openai_chat_client_openrouter_live_e2e`, a real key) passes -- real token usage, a real streamed reply
("Ping!") over `ae::stream<T>`, a wrong-key 401 classified `policy`, and the I2 capability denial -- so the
shared provider plumbing still talks to a real API. That live run drives the blocking path; a REACTOR-path live
run (which would also exercise reactor DNS) needs a live test that constructs the client with a Runtime, still
open.

Still open after step 34 (partly superseded by step 35, which made `chat()` non-blocking on the reactor):
hosts do not yet construct the provider clients with a Runtime (the switchover is opt-in, and the `Resolver`
seam remains for the blocking fallback); `io_timeout_ms` is threaded but the step-6 timer-stop-on-strand move
is not.

### Step 35 (2026-10-11): §5.1 Phase B -- `chat()` on the reactor (the ChatClient switchover, slice 2)

The second half of the ChatClient switchover, taken as the CHEAPER of §5.1's two options for `chat()`
("gains a real `chat()` again ... or drops it"): the non-streaming method stays in the API, but its body is
no longer blocking where a Runtime is set. Dropping `chat()` was measured FIRST and rejected as
disproportionate (see the §14.1 sizing note): ~50 differently-shaped test fixtures stub `chat_stream` and
rely on the unary path, so removing it is a ~40-file rewrite that also erases `AgentSession`'s non-streaming
mode.

- `protocol/openai/chat_client.hpp` + `src/protocol/openai/chat_client.cpp` (and the identical Anthropic pair)
  -- a new `detail::exchange_json_async(...)`, a COROUTINE that runs the same buffered exchange on
  `rt::http_request` (rt/http.hpp) with `exchange_json`'s exact contract (transport failure via
  `http_error_to_error`, non-2xx via `http_status_error`, 2xx body parsed as JSON). `chat()` branches to it
  when `reactor_.runtime != nullptr`, and falls back to the blocking `exchange_json` otherwise -- so all ~30
  construction sites and every fixture are unchanged. The step-34 `ProviderReactorOptions` /
  `make_http_context` / `http_error_to_error` are reused verbatim; cancellation passes `ctx.cancellation` as
  the request's stop token.

Evidence: the HTTPS tree builds; `test_openai_chat_client_live` / `test_anthropic_chat_client_live` gain a
`chat()+reactor` case (a `rt::Runtime` + loopback `HostResolver`, driven by `rt::block_on`) that performs a
real TLS round-trip on `rt::http` and parses the buffered body -- both pass, and the existing blocking
`chat()` cases still pass; the live OpenRouter e2e (`test_openai_chat_client_openrouter_live_e2e`, real key)
still passes. `build-dev` is untouched (every change is inside `AGENTENGINE_WITH_HTTPS`), so its 402/416
stands.

Residuals: the switchover stays opt-in -- hosts do not yet construct the provider clients with a Runtime, so
`chat()` is non-blocking only when one is supplied (the blocking `exchange_json` remains for hosts with
none); the `Resolver` seam is therefore still present.

### Step 36 (2026-10-11): §5.1 Phase B -- the reactor path verified LIVE against a real provider

Closes the "reactor's real DNS / a live reactor-path run" residual that steps 34 and 35 both named.

- `tests/protocol/openai/test_openai_chat_client_openrouter_live_e2e.cpp` gains **OC-5**: the same class,
  host and model, but constructed with a `rt::Runtime` and NO injected resolver, so DNS resolves through
  `rt::Runtime::dns_pool()`. Both reactor halves run against the real endpoint -- `chat()` through
  `rt::http_request` (driven by `rt::block_on`) and `chat_stream()` through the owned root
  (`rt::Runtime::spawn`, no detached thread).
- Evidence (live, real key, 2026-10-11): OC-5 passes -- a real round-trip on `rt::http_request` with
  nonzero input/output usage, and the owned-root stream reaches `stream_terminal::closed` with non-empty
  text. OC-1..OC-4 (the blocking path) still pass, as does the loopback suite on the HTTPS tree.

Sizing of the remaining residual (making the switchover non-opt-in, then deleting the blocking
`exchange_json` + `Resolver` seam): **no product/host code constructs a `rt::Runtime` today** -- the type
appears only in `rt/` headers, the provider seam, and tests. So "thread a Runtime through the hosts" is in
fact "give every host a Runtime": `tools/cli_chat.cpp`, `tools/test_driver/live_backend.hpp`,
`examples/16_group_chat_live.cpp`, `examples/17_planner_live.cpp`, `devsim/*/main.cpp`, and
`core/session_builder.hpp`'s `QuickstartSessionBuilder` (the canonical product factory, whose `build()`
constructs the real `Primary` client through one uniform positional call). Only after that can the blocking
fallback go -- and every no-Runtime test site must then be given a Runtime or kept on a named fallback,
the same per-file cost the drop-`chat()` measurement (§14.1) found. Recommended as its own slice; not
attempted here.

### Step 37 (2026-10-11): §5.1 Phase B -- `SurfaceFactory` async (first of Phase B's last two seams)

- `sandbox/live_shell_sandbox_provider.hpp`: `SurfaceFactory` is now `std::function<rt::task<Surface>()>` -- a
  `Surface` may be a possibly-REMOTE environment (ADR-209: a container/daemon handshake), so constructing one
  is allowed to suspend. `bind_sandbox` is now `task<result<void>>` and awaits the factory; its refusal
  ordering and result contract are otherwise unchanged. The copy-assign constructs its surface through
  `rt::block_on(make_surface_())` -- the same idiom that operator already used two lines up for
  `spawn_child_branch` (l.187), so no new pattern was introduced.
- Conformers: `tests/sandbox/test_live_shell_sandbox_provider.cpp`'s `World::bind` helper and
  `tests/sandbox/execution_surface/test_live_shell_sandbox_provider_live.cpp`'s `bind` lambda keep their
  synchronous `result<void>` shape for their call sites and drive the task with `rt::block_on`; the surface
  lambdas became coroutines (`co_return Surface{...}`). No production caller outside the class:
  `MandatorySandboxProvider::bind_sandbox` is a different signature and is untouched, and every
  `tools/*.cpp` `bind_sandbox` is that one.
- Evidence: `build-dev` clean; `test_live_shell_sandbox_provider` passes (bind / re-bind / copy-fork);
  the Docker-daemon live test gets THROUGH `ok: setup: bind the live provider`, and its only failures are
  `docker run failed: failed to connect to the docker API` -- no daemon on this machine, not this change.
  `ctest -LE live-network` shows the identical 14 known failures of §14.1's baseline; the extra 20
  `compile_fail` failures in that run are an ARTIFACT of running ctest without the MSVC environment
  (`fatal error C1083: Cannot open include file: 'cstdint'`), not a code regression.

**Next -- the last Phase B seam, `Resolver` (sized, not started).** The sync seam is
`std::function<result<VerifiedEndpoint>(std::string_view, std::uint16_t)>` (`protocol/provider_chat_wire.hpp:26`),
i.e. the blocking client's resolve+SSRF-filter step. The reactor path already SUBSUMES it -- `rt::Runtime`
supplies DNS via `dns_pool()` and the address policy via `ProviderReactorOptions.policy` -- so "converting"
it means DELETING it together with the blocking `exchange_json`. Measured shape of that slice:
(1) the seam is a POSITIONAL ctor argument at ~101 construction sites across ~30 test files plus the hosts,
so the clients should OWN a `rt::Runtime` rather than require one from each site (that keeps every
construction site and every fixture compiling unchanged); (2) `run_task_sync` (the driver those ~30 files
use) supplies NO home, and `reactor_await.hpp` REFUSES a homeless waiter, so the task driver must become
`rt::block_on`-based (`block_on.hpp`'s own contract: "the calling thread is the driven task's home") -- a
harness change, not a per-file one; (3) only then can `exchange_json` + the `Resolver` parameter go. That
is the next slice, and it should be taken as one deliberate pass with a green build between each of the
three steps. The qdrant `Resolver` (same shape, another file) belongs to Phase C.

### Step 38 (2026-10-11): §5.1 Phase B -- the test driver supplies a home (the `Resolver` slice, step 1 of 3)

Prerequisite for deleting the last Phase B seam (`Resolver`) and the blocking `exchange_json` behind it:
the harness the provider tests drive explicit-path `chat()` with had no way to run a task that genuinely
parks.

- `tests/support/run_task_sync.hpp` now delegates to `rt::block_on` instead of its own manual-resume
  `Driver`. `block_on` makes the CALLING THREAD the driven task's home (its own ADR-175 contract), which
  is required because `rt/reactor_await.hpp`'s `AwaitedOp::on_complete` REFUSES a homeless waiter
  (`Reactor::note_homeless_refusal`) rather than resuming it on the reactor thread -- so a reactor-awaiting
  task driven by the old loop would never resume at all. For the non-parking `ChatClient` conformers this
  harness was written for, behaviour is unchanged (the chain still runs inline on this thread); the
  hand-written driver's ASan-confirmed stack-use-after-scope workaround goes away with it, since
  `rt::block_on` already carries that fix.
- Evidence: `build-dev` clean, `ctest -L rt` 80/80; the HTTPS tree builds and
  `test_openai_chat_client_live` / `test_anthropic_chat_client_live` run ALL PASS directly. (Under
  `ctest --test-dir build` these two report "Test command: NOT_AVAILABLE ... Not Run" in 0.00s -- a
  registration/build-config artifact of the multi-config `build` tree, independent of any source change,
  since the same binaries pass when invoked directly.)

Remaining steps of the `Resolver` slice (unchanged in intent, see step 37's plan): (2) make the two
provider clients OWN a `rt::Runtime` so the reactor path becomes the only path without touching the ~101
construction sites; (3) delete `exchange_json` + the `Resolver` ctor parameter and the detached-thread
worker. Step 2 was subsequently taken in the same session (step 39, below); step 3 was not, because its
parameter removal looked like the ~101-site change class -- step 39 records the cheaper shape it can take
instead.

### Step 39 (2026-10-11): §5.1 Phase B -- the provider clients own their Runtime (`Resolver` slice, step 2 of 3)

The reactor path is no longer opt-in. Each provider client holds `std::unique_ptr<rt::Runtime>
owned_runtime_`, created in the constructor when the caller supplied no `ProviderReactorOptions.runtime`,
and `reactor_.runtime` is pointed at it -- so `chat()` and `chat_stream()` always take the reactor branch,
with NO change to any of the ~101 construction sites (they keep their existing positional arguments,
including the trailing options default).

- Held by `unique_ptr`, not by value, for two reasons: the client stays move-constructible, and the heap
  object's address is stable, so `reactor_.runtime` (a raw pointer copied into the coroutine's context)
  cannot dangle when a client is moved -- e.g. into `ModelCallGateway`. Declared AFTER `reactor_` so it is
  destroyed first.
- Evidence (the "an async seam must not move behaviour" check for this step): the HTTPS tree builds;
  `test_openai_chat_client_live` and `test_anthropic_chat_client_live` run ALL PASS invoked directly,
  including every PRE-EXISTING blocking-path case (attribution, structured, metadata, tool-call
  streaming) as well as the reactor cases. The same assertions now hold with the exchange running on
  `rt::http` instead of a detached thread. (`ctest --test-dir build` cannot run either binary --
  "Test command: NOT_AVAILABLE" -- a multi-config registration artifact, see step 38.)

Remaining -- step 3, the last of Phase B: delete `exchange_json`, the `if (runtime == nullptr)` branches in
both clients, and the detached-thread `run_stream_worker`. CHEAPER SHAPE found while doing step 2: the
`Resolver` ctor PARAMETER need NOT be removed (which would have forced the ~101-site edit). It can instead
be WIRED INTO the reactor -- converted to the reactor's own `HostResolver` and left in
`ProviderReactorOptions.resolver` -- so production's `sandbox::resolve_host` still governs name resolution,
now executed on the runtime's DNS pool rather than on a blocking thread. Every construction site keeps
compiling, and the parameter stays meaningful instead of dead.

### Step 40 (2026-10-11): §5.1 Phase B -- the `Resolver` parameter becomes the reactor's DNS seam

The step 39 note's "cheaper shape", taken first (before any deletion) because the deletions are only safe
once name resolution has somewhere to go.

- `protocol/provider_chat_wire.hpp` gains `host_resolver_from(Resolver, std::uint16_t)`: it wraps the
  provider `Resolver` (`result<sandbox::VerifiedEndpoint>`) as the reactor's `rt::HostResolver`
  (`pal::ResolveOutcome`), mapping the verified endpoint's host-order IPv4 onto `pal::IpAddress::v4` and a
  failed resolve onto `pal::resolve_error::not_found`. `rt` runs a `HostResolver` on its DNS pool
  (rt/dns.hpp's own words: "a blocking resolver, run on the DNS pool"), so production's
  `sandbox::resolve_host` keeps governing name resolution on the reactor path -- off the reactor thread --
  instead of being called inline by the blocking exchange.
- Both clients' constructors install it when the caller named no reactor resolver:
  `if (!reactor_.resolver) reactor_.resolver = host_resolver_from(resolver_, port_);`. So the `Resolver`
  constructor parameter -- which every one of the ~101 construction sites still passes -- stays
  LOAD-BEARING rather than becoming vestigial, and no call site changes.
- Evidence: HTTPS tree builds; `test_openai_chat_client_live` and `test_anthropic_chat_client_live` both
  run ALL PASS with resolution now flowing through the wrapped seam. HONEST SCOPE of that evidence: those
  loopback suites use the host name "localhost", which real DNS also resolves, so they prove the wiring
  does not BREAK anything rather than proving the wrapper is individually load-bearing -- a case that
  isolates it (a name only the test resolver can answer) is a worthwhile follow-up.

Remaining -- the last piece of Phase B: with resolution moved, delete `exchange_json`, the
`if (runtime == nullptr)` branches in both clients, and the detached-thread `run_stream_worker`.

### Step 41 (2026-10-11): §5.1 Phase B -- the blocking provider path is gone from both clients

With resolution moved (step 40), both backends' model path is now reactor-only:

- `chat()`: the `if (reactor_.runtime != nullptr) { ... } else { provider_wire::exchange_json(...) }` branch
  is replaced by an unconditional `co_await detail::exchange_json_async(...)`. No blocking exchange remains
  in the method.
- `chat_stream()`: the `std::thread(&detail::run_stream_worker, ...).detach()` branch is deleted; the owned
  producer root (`rt::Runtime::spawn` via `run_stream_worker_async`) is the only way a stream is produced.
  This is the charter item -- no detached thread on the model path.
- Evidence: HTTPS tree builds; `test_openai_chat_client_live` and `test_anthropic_chat_client_live` both run
  ALL PASS. Since steps 39/40 already made every construction site take the reactor branch, those two
  suites' PREVIOUSLY-BLOCKING cases (attribution, structured, metadata, tool-call streaming, and the
  intentional-error cases) are now genuinely Reactor-path runs -- a real "the seam moved, the behaviour did
  not" check, not a re-run of the same code.

Leftover, mechanically dead but still compiled: `detail::run_stream_worker`'s and
`provider_wire::exchange_json`'s declarations in the two client headers and their definitions in
`src/protocol/{openai,anthropic}/chat_client.cpp` / `provider_chat_wire.hpp`, plus the (now unused)
`pump_sse_stream` twin of `pump_sse_stream_async`. Deleting them is pure subtraction with no call sites to
update -- the last cleanup of Phase B, left to a following pass rather than started without room to
re-verify.

### Step 42 (2026-10-11): §5.1 Phase B -- the dead detached-thread worker is deleted

- `detail::run_stream_worker` is gone from BOTH backends: its declaration in
  `protocol/{openai,anthropic}/chat_client.hpp` and its definition in `src/protocol/{openai,anthropic}/
  chat_client.cpp`. Nothing called it after step 41, and its `pump_sse_stream<...>` instantiation goes with
  it (the blocking `pump_sse_stream` template itself stays -- uninstantiated, it costs nothing, and it is
  the pair `pump_sse_stream_async` is documented against).
- **`provider_wire::exchange_json` was NOT deleted, and the grep is the reason**: it is still called by
  `protocol/openai/openrouter_batch_backend.hpp:139`, so the batch backend is a second client of the
  blocking exchange. It therefore outlives this slice; that backend's own conversion is the follow-up, not
  a deletion.
- Evidence: HTTPS tree builds; `test_openai_chat_client_live` and `test_anthropic_chat_client_live` both
  ALL PASS.
- Stale-comment residual (cosmetic, not compiled): the "detached background worker" comment block above the
  removed declarations and `rt/http.hpp`'s mention of `pump_sse_stream` still describe the old shape.

### Step 43 (2026-10-14): §5.1 -- `BatchBackend` is async; the blocking `exchange_json` is deleted

Taken out of Phase D's group because it is the batch backend that kept `exchange_json` alive (step 42), and it
needs none of the sandbox plumbing the rest of Phase D shares.

- **The seam.** `BatchBackend::submit/poll/cancel/release` return `task<result<...>>`
  (`core/batch_backend.hpp`). `group_key`, `limits` and `admit` stay synchronous: pure, no I/O, and `admit`
  runs per candidate inside the supervisor's gather loop. The caller keeps the reference arguments alive until
  the task completes -- stated on the seam.
- **The OpenRouter conformer** calls `openai::detail::exchange_json_async` (the exchange `chat()` uses) and
  takes a trailing `ProviderReactorOptions` (appended last, the chat clients' convention). With no Runtime it
  owns one; the ctor `Resolver` becomes the reactor's DNS seam unless the host named one -- the same rules as
  `OpenAIChatClient` (steps 39-40). `ctx.cancellation` is passed through, which the blocking path never had.
- **Wrappers.** `ScriptedBatchBackend` and the replayer complete without suspending. The recorder's
  `record()` is split into begin / record-thrown / record-outcome steps shared by the sync `admit` and a new
  coroutine `record_async`; the thrown-call path keeps its rethrow OUTSIDE the handler (the clang-cl ASan EH
  constraint recorded there).
- **Supervisor.** `submit_batch_chunks`, `fold_batch_items` (which releases consumed jobs) and
  `cancel_abandoned_batches` become coroutines; every caller was already one (`execute()`, `poll_batches*`).
  `guarded_async` is `guarded`'s twin: a throw from the call itself or from its body (rethrown at the
  `co_await`) is the same classified `rt.batch.threw` transient failure. C20's throwing backend now throws both
  ways (submit/poll from the body, cancel before returning a task).
- **Lock audit** (Phase D's standing instruction): the supervisor suspends on a vendor call only under its
  `AsyncMutex` run lock; the scripted backend, the recorder's sink lock and the replayer's state lock are
  `std::mutex` held only in synchronous sections, never across a `co_await`.
- **Deleted:** `provider_wire::exchange_json` (declaration and definition). No blocking provider exchange
  remains; `tools/batch_infer.cpp` drives the backend with `rt::block_on` from `main`.
- **Evidence.** `test_openrouter_batch_backend` gains R9 (a `ctx.cancellation` stop ends an in-flight poll as
  `canceled` within 5 s, against a server that accepts and never answers) and R10 (a host-supplied
  `ProviderReactorOptions` is used: its resolver is the only route to a `.invalid` host). Mutants: passing an
  empty stop token fails R9 (still in flight 10 s after the stop); ignoring the ctor's options fails both R10
  checks. `ctest -LE live-network`: 406/419 (440 registered incl. live-network), the 13 failures being the Docker-daemon tests. naming, layering
  and milestone lints OK. Not run: `test_openrouter_batch_live_e2e` (needs
  `AGENTENGINE_OPENROUTER_BATCH_API_KEY`; skipped when unset).
### Step 44 (2026-10-14): Phase C slice 1 -- one `task`-shaped index seam; the RAG `recall` tools await (#172)

§5.1 said the index concepts merge into the already-async `RemoteVectorIndex` shape, because the local/remote
split existed only because one side was synchronous. Done that way, and together with the RAG `recall`
conversion, which §14.1 had scheduled last in Phase C: once local indexes are `task`-shaped, a synchronous
`recall` could only reach them by a temporary `synchronous_leaf` on every local conformer.

- **Seams.** `VectorIndex` (`core/vector_index.hpp`) and `SparseIndex` (`core/sparse_index.hpp`):
  `add_batch(..., ctx)`, `search(..., ctx)`, `contains(id, ctx)`, each `task<result<...>>`.
  `core/remote_vector_index.hpp` (`RemoteVectorIndex`, `AnyVectorIndex`, ADR-180 §2.1's R1 disjointness rule)
  is deleted. `PersistentVectorIndex::snapshot/restore` stay synchronous for now: they are store calls and
  move with the store seams.
- **Conformers.** `BruteForceCosineIndex`, `BM25Index`, `VulkanCosineIndex`: the existing synchronous bodies
  are kept byte-for-byte as private `*_now` helpers, and the public seam methods are coroutines over them that
  complete without suspending (`ctx` unused). `QdrantVectorIndex` was already `task`-shaped; it drops
  `synchronous_leaf`.
- **Callers.** `DiskCorpusSource::mount`/`mount_hybrid` and both providers' `on_context` simply `co_await`;
  every `if constexpr (RemoteVectorIndex<...>)` dispatch is gone. A failed `contains` during `mount` now aborts
  the mount (it was a `bool` that could not fail) -- the same pre-commit abort an embed failure already gets.
- **`recall`.** Both RAG providers' `recall` tools are a plain lambda handing off to a member coroutine
  (`recall_call`, args by value -- the MCP bridge's shape), which awaits the embedder and the index. Deleted
  with it: both `make_sync_invoke` sites (2 of §5.2's five), both `rt::drive_leaf_task` uses, and the four
  `recall_tool_requires_synchronous_leaf_{embedder,index}` contract errors. `synchronous_leaf` is no longer
  required by `Embedder`; `OpenAIEmbedder`/`QdrantVectorIndex` keep a comment stating their bodies still block
  ("async in name only", decision A8 -- moving them onto `rt::http` is the next slice).
- **Finding, recorded rather than absorbed.** `test_vector_rag_context_provider` R16 pinned the fail-closed
  `recall_tool_requires_synchronous_leaf_embedder` error for a `synchronous_leaf = false` embedder. The same
  conformer's `recall` now succeeds; R16 asserts that instead, with the reason in the test.
- **Finding: the old `recall` could not run on a runtime lane.** Since ADR-175 `drive_leaf_task` is
  `block_on`, and a lane refuses `block_on` (`rt.block_on_on_lane`, §4.3). The pre-#172 `recall` therefore
  returned `rt.leaf_task_faulted` whenever it was invoked on a lane -- latent only because session callers are
  not on the runtime yet (#179). The converted `recall` awaits, so it works there.
- **Evidence.** `test_remote_vector_index` is rewritten (historical name) to drive conformers that genuinely
  suspend -- each parks on a reactor timer -- through every caller ON A RUNTIME LANE: S1 `on_context`, S2
  `recall`, S3 hybrid `on_context` + `recall` with both legs suspending, S4 `mount` + `mount_hybrid`; each
  check also asserts the mocks really parked. Mutant: vector `recall` driving its embedder through
  `drive_leaf_task` (the old shape) fails S2 with `rt.leaf_task_faulted`. Index tests call the seam through
  `tests/support/index_sync.hpp` (concept-constrained `sync_*` drivers, so braced-init arguments still work).
  Vulkan on, real GPU: `test_vulkan_cosine_index` and its fault-injection twin pass (Vulkan is not built in
  CI). Full `ctest -LE live-network`: 405/419 -- the 13 Docker-daemon tests, plus
  `test_native_jail_backend_windows`, which passed 3/3 when rerun alone (timing under `-j6` load). naming,
  layering and milestone lints OK.

### Step 45 (2026-10-14): Phase C slice 2 -- `QdrantVectorIndex` and `OpenAIEmbedder` on `rt::http` (#172)

The two "async in name only" RAG backends (decision A8): `task`-shaped since ADR-063/180, but each body was a
blocking `sandbox::perform_provider_https_exchange` that took no stop token.

- **One provider-plane exchange.** `provider_wire::exchange_async` (`provider_chat_wire.hpp`, defined in
  `provider_chat_wire.cpp` beside the TLS connector): a buffered HTTP/1.1 exchange on `rt::http`, `stop` ending
  it in flight, a transport failure mapped through `http_error_to_error`, and EVERY status returned -- each
  client classifies its own. Both chat clients' `exchange_json_async` (two copies of the same body) are now
  that call plus `http_status_error`.
- **The two backends** take a trailing `ProviderReactorOptions` (the chat clients' "append last" convention),
  own an `rt::Runtime` when none is supplied, turn the ctor `Resolver` into the reactor's DNS seam unless the
  host named one (qdrant's half of `Resolver`, §14.1) -- the rules of steps 39-40 -- and `co_await` the
  exchange with `ctx.cancellation`. Their own status mapping (`map_http_status_error`, Qdrant's 404-is-`false`)
  is unchanged.
- **Residual, stated:** `perform_provider_https_exchange` has no production caller left; six test files still
  use it (including its own test), so deleting it belongs with #180/#182.
- **Evidence.** New `test_rag_backends_on_reactor` against a loopback server: W1 Qdrant wire (PUT upsert with
  `api-key`, search parse, 404/200 `contains`, 500 transient with Qdrant's message), W2 the embedder's wire
  (bearer key, vectors by index), C1 a stop ends an in-flight embed and an in-flight Qdrant search as
  `canceled` promptly, against a server that never answers, H1 a host-supplied `ProviderReactorOptions`
  resolver is the only route to a `.invalid` host for both. Mutants: an empty stop token in the embedder's or
  Qdrant's exchange leaves the call in flight 10 s after the stop (each run separately); ignoring the ctor's
  options fails all three H1 checks. `ctest -LE live-network`: 407/420, the 13 Docker-daemon tests. Not run: the
  embedder's and Qdrant's live tests (no `AGENTENGINE_OPENROUTER_API_KEY` / Qdrant host here; they skip).

### Step 46 (2026-10-14): Phase C slice 3 -- `SecretStore::resolve` is `task`-shaped (#172)

`SecretStore` was "kept synchronous" on the grounds that resolution has no I/O (env, memory, a small file). That
holds for the stores in the tree, not for the seam: a vault- or keystore-backed store resolves over the network,
and decision A8 makes the conversion total.

- **The seam.** `resolve(ref, ctx) -> task<result<SecretLease>>`; the three conformers (`AgentEngineSecretStore`,
  `InMemorySecretStore`, `QuarantineSecretStore`) keep their bodies and complete without suspending. They take
  the `SecretRef` BY VALUE, so a task held past the call never reads a dead temporary. `SecretSource::get()`
  stays synchronous: it is the env/file adapter underneath, not the seam.
- **Callers inside coroutines** `co_await` it: both chat clients' `chat()`, the embedder, Qdrant (x3), the batch
  backend (x3).
- **`chat_stream()` resolves on its producer root.** `chat_stream()` returns synchronously, so it cannot await.
  Both real clients now spawn an owned root (`stream_after_key`) whose first step awaits the key, failing the
  stream with the store's own error on a denial, and which then starts the pump (step 34's root). New, documented
  on the `ChatClient` concept: the `EffectContext` (and the `CapabilitySet` it borrows), the client, and its store
  must outlive the returned stream. Every caller already drains the stream within the turn that owns `ctx`.
  Rejected: resolving inline with `rt::block_on`, which is refused on a lane (`rt.block_on_on_lane`) -- exactly
  where a session calls `chat_stream()`.
- **Evidence.** New `test_chat_stream_key_resolution`, both vendors, loopback server: K1 a denied grant fails the
  stream with `secret.not_granted`/`policy` and no connection is made; K2 a granted key reaches the wire
  (`Authorization: Bearer` / `x-api-key`) and the server's 401 fails the stream; K3 `chat_stream()` called on a
  runtime lane against a store that genuinely suspends on the reactor reaches the wire. Mutant: resolving inline
  with `rt::block_on` in the OpenAI client fails K3 with `rt.block_on_on_lane`. Test callers of `resolve` drive
  it with `rt::block_on` off-lane. `ctest -LE live-network`: 408/421, the 13 Docker-daemon tests. Not run: the chat clients' live tests (no provider key here; they skip).

### Step 47 (2026-10-15): Phase C slice 4 -- `SessionStore` is `task`-shaped (#172)

The first of the three stores, taken first because its core callers already `co_return` from coroutines.

- **The seam.** `save`/`load`/`exists`/`remove` all return `task<...>` (`exists` is `task<bool>`); ids are taken
  BY VALUE so a held task never reads a dead temporary. `InMemorySessionStore` and `FileSessionStore` keep their
  bodies and complete without suspending -- the `rt::offload` move §6 names for file stores is NOT done here (same
  stance as step 46's conformers); a file store that blocks a lane is the remaining residual of this seam.
- **Callers.** `save_agent_session_snapshot`, `save_turn_delta`, `delete_session`, `save_workflow_checkpoint`
  `co_await`; the loaders (`load_agent_session_snapshot`, `load_turn_delta`, `load_project_snapshot`,
  `load_workflow_checkpoint`) and `save_project_snapshot`, `WorkflowCheckpointManager::resume_or_start` become
  `task`-shaped. `ProjectSupervisor` needed no change.
- **`WorkflowSupervisor::CheckpointHook` is now `std::function<task<void>(std::uint32_t, RunStateRecord)>`** (record
  by value) and `execute()` awaits it at the superstep boundary. This was the one decision the seam forced: the
  manager's hook is what persists the checkpoint, and `rt::block_on` is refused on a lane. Every hook in tests is a
  trivial coroutine lambda; no test assertion changed, only call shapes (`block_on` off-lane).
- **Evidence.** `test_rt_workflow_checkpoint_manager` C6 attaches the manager to a store that genuinely parks on the
  reactor and drives the workflow on a runtime lane. Mutant: `block_on` inside the hook terminates the test
  (`rt.block_on_on_lane`, exit 3). `ctest -LE live-network`: the 13 Docker-daemon tests fail as in step 46;
  `test_rt_lanes` failed once under `-j8` and passes 3/3 alone (load-sensitive, unrelated to this change).
  naming-lint and layering-lint OK. `agentengine_test_driver.exe` could not relink (held open by a running MCP
  server) -- unrelated, not rebuilt.

### Step 48 (2026-10-15): Phase C slice 5 -- `AppendLogStore` is `task`-shaped, and with it every ref reader/writer (#172)

`append`/`read_from`/`last_seq` return `task<...>`; ids by value. `InMemoryAppendLogStore` keeps its body.
`FileAppendLogStore`'s out-of-line bodies are now `append_blocking`/`read_from_blocking`/`last_seq_blocking`
(format, locking and truncation untouched) behind inline `task` wrappers that complete without suspending; the
`rt::offload` move §6 names is NOT done here -- the same named residual as `FileSessionStore`'s.

- **The cascade was the finding.** `AppendLogStore` is the ref store, so `read_ref`/`commit_ref` sit under the
  whole worktree layer. Everything that touches a ref is now a `task`: `commit_ref`/`commit_turn`/`turn_digest_at`/
  `rewind_to_turn`; `mount_read`/`mount_write(_with_blob)`/`materialize_merge_conflicts`;
  `merge_branch_into_parent` + retry; `create/read/write_sub_worktree`; `check_shared_staleness`; the memory
  helpers (`ensure_*`, `write/read/list_memory_item(s)`, `rank_memory_items`); corpus-chunk read/write,
  `render_scored_chunk`, `mint_spawn_worktree`, `mint/resume_executor_worktrees`; the effect journal, project
  archive and workflow time-travel; `Tier1AttemptLog` and `EvalStore::make`; and the native-jail materializers
  (`materialize_mount`, `harvest_mount`, `materialize_memory_mount`, `harvest_memory_notes`,
  `materialize_skill_mounts`). Object-store-only helpers (`set_entry_at_path`, `merge_subtrees`, `diff_*`) are
  unchanged: they wait for `WorktreeObjectStore` (next).
- **`WorkflowSupervisor::MergeOnJoinHook` is now `std::function<task<result<void>>(std::string)>`**, awaited in
  `execute()` and `fold_batch_items()` (both already coroutines); `make_merge_on_join_hook` returns it.
- **`SpawnMinter` drives `mint_spawn_worktree` with `agent_spawn_detail::drive`**, the same naive resume loop it
  already uses for the cost pool on its dedicated worker thread. Sound while the ref store completes without
  suspending; a store that truly parks would spin that thread -- named, to fix with Phase D.
- **Evidence.** `test_worktree_ref_store` A2-L1 drives `commit_ref -> mount_write -> mount_read` on a runtime lane
  against a log that parks on the reactor (both appends park). Mutant: `block_on` in `commit_ref_impl` ends the
  test with exit 3 (`rt.block_on_on_lane`). Test fakes that wrapped a store (`FlakyStore`, `LosingStore`,
  `SameSeqStore`, `CountingLogStore`) became `task`-shaped; ~600 call sites in tests changed shape only
  (`block_on` off-lane), no assertion changed. `ctest -LE live-network`: 408/421, the 13 Docker-daemon tests --
  among them `test_sandbox_runtime`, `test_mandatory_sandbox_provider` and `test_task_branch_*`, which reach this
  code and could NOT run here, so the sandbox-side wiring is verified by compilation only.
  naming-lint and layering-lint OK.

### Step 49 (2026-10-15): Phase C slice 6 -- `WorktreeObjectStore` is `task`-shaped, including `Ledger` (#172)

`put_blob`/`get_blob`/`put_tree`/`get_tree` (and the optional `blob_size`) return `task<result<...>>`; ids by value;
`put_blob` keeps its `std::span`, which must outlive the returned task. `InMemoryWorktreeObjectStore`'s methods are
coroutines with the same bodies. `FileWorktreeObjectStore`'s bodies are now `*_blocking` behind inline `task` wrappers
-- the `rt::offload` move is the same named residual as steps 47-48.

- **`Ledger` is the point of this slice, and it needed its own design: `decisions/ADR-238`.** `Ledger` held a
  `std::mutex` across every store call, which cannot survive a store that suspends. ADR-238 (Proposed, red-teamed twice
  before any code) splits `mutex_` into a state lock that is never held across a `co_await` and a new `op_mutex_`
  (`rt::AsyncMutex`) that serialises `create_root_branch`/`commit`/`reset_to`/`merge`/`abandon`/
  `abandon_orphaned_branch` across their store I/O, in phases (authorize and copy locals under `mutex_`; store I/O with
  no lock; re-find by name and apply under `mutex_`). `get_blob_safe`, `get_tree_safe`, `put_blob_safe` and
  `BlobWriteBatch::put` are `task`s. `RealIoFileSystem::materialize` reads every blob before taking its `sync_mutex_`.
- **Everything over an object store followed:** `ensure_empty_tree`, `resolve_entry_at_path`, `set_entry_at_path`,
  `resolve_subtree_digest`, `blob_size_of`, `subtree_usage`, `read_blob_within_size_cap`; `merge_subtrees` and
  `merge_trees(store, ...)`; `collect_leaves_as`, `diff_subtrees`, `diff_trees`, `check_shared_staleness`;
  `collect_blob_paths`, `assemble_tree`, `resolve_and_mount`, corpus `mount`/`mount_hybrid`, `render_scored_chunk`;
  the native-jail materializers; and `PersistentVectorIndex::snapshot`/`restore` (which write to the store).
- **Evidence.** `test_ledger_async_locking` (44 checks) forces each interleaving with a gated store on one thread:
  T1 concurrent commits chain 1..N with `put_tree` parked; T2 a merge waits for a parked commit to its parent and
  keeps the commit's file; T3 an abandon waits behind an in-flight merge; T4 an unauthorized read never reaches the
  store; T5 a throwing `put_tree` becomes `ledger.store_faulted`, refunds, and releases the lock; T6
  `abandon_orphaned_branch`/`reap_pending_abandons` do not self-deadlock; T7 a merge task destroyed while parked
  leaves its child reclaimable; T8 a nested operation from a `when_all` child of the lock holder is refused with
  `ledger.op_lock_refused` and throws nothing (real one-lane `Runtime`); T9 `materialize` holds no `std::mutex` while
  parked. **All seven planted mutants are killed** (no op lock on commit -> T2; none on abandon -> T3; nested public
  `abandon` -> hang; no `InFlightChild` guard -> T7; plain `lock()` -> T8; `put_tree` under `mutex_` -> crash;
  `materialize` locking before its reads -> hang). Call sites in 43 test files changed shape only; the fakes that
  wrapped a store became `task`-shaped. `ctest -LE live-network`: all green except the same 13 Docker-daemon tests,
  among them `test_sandbox_runtime`, `test_mandatory_sandbox_provider` and `test_task_branch_*`, which reach this code
  and could NOT run here, so the sandbox-side wiring (including `materialize`) is verified by compilation and T9 only.
  naming-lint and layering-lint OK.
- **A refusal is not what a nested plain `co_await` produces.** Writing T8 showed `ledger.op_lock_refused` only
  arises for a task *forked* by a scope (`when_all`/`on_strand`) from the holder; a plain awaited child shares the
  holder's id and would wait on itself. The code therefore never nests a public locker (ADR-238 §3.5), and T6 proves it.

### Step 50 (2026-10-15): Phase D -- the execution and sandbox seams are `task`-shaped (#173)

Five slices, one commit each, in the order smallest-first: `NetEgressBackend`, `Runner`, `SandboxBackend`,
`ExecutionSurface`, `PersistentShellSurface`. The same shape throughout, Phase C's: every conformer keeps its
body byte-for-byte as a public `*_blocking` member and the seam method is a coroutine over it that completes
without suspending. Moving those bodies onto `rt::process` / `rt::http` is #180's, not this phase's; until then
a conformer that blocks a lane is the named residual, as for the file stores.

- **Seams.** `NetEgressBackend::fetch` (`sandbox/net_egress_proxy.hpp`) -> `task<result<NetEgressResponse>>`;
  `Runner::run` (`sandbox/runner.hpp`) -> `task<result<ExecOutcome>>`; `SandboxBackend::create/exec`
  (`sandbox/sandbox.hpp`) -> `task<result<...>>` and `destroy` -> `task<void>`; `ExecutionSurface::reset/run/
  drain_to` (`sandbox/execution_surface.hpp`) and `PersistentShellSurface::open/exec/drain_to/close`
  (`sandbox/persistent_shell.hpp`) -> `task<result<...>>`. `is_live()` and `traits` stay synchronous (§5.3).
  Value arguments are taken by value in every in-tree conformer's wrapper; handles, `ExecState` and `ctx` are
  borrowed and must outlive the task -- stated on each concept.
- **Conformers.** `HostEgressProxy` (`fetch_blocking`); `ShellRunner`, `PythonRunner`, `MediatedShellRunner`,
  `MediatedPythonRunner` (`run_blocking`); `NativeJailBackend`, `LinuxNativeJailBackend`, `KataBackend`,
  `WasmBackend` (`create/exec/destroy_blocking`); `DockerExecutionSurface`, `ContainerdExecutionSurface`
  (`reset/run/drain_to_blocking`); `DockerPersistentShellSurface` (`open/exec/drain_to/close_blocking`). Every
  wrapper is `[[nodiscard]]`, so an un-awaited `destroy(h);` is a compile error under `/WX`.
- **A hazard the rename had to cover, found by reading every body.** `DockerPersistentShellSurface`'s destructor
  and `open()`'s failure lambda both called `(void)close();`. Left alone, that is a discarded task: the `(void)`
  silences `[[nodiscard]]` and the container is never removed. Both call `close_blocking()`. No other conformer
  calls its own seam method internally (checked per file).
- **Callers that await.** `read_content`'s `invoke_via`, the four PDF tools (`fetch_via_url`, `invoke_worker`,
  `invoke`/`invoke_via` -- they move to the §5.2 instance form); `SessionShellSandbox::run` (`run_shell`) and
  `cli_chat`'s `real_execute_code`; the PDF worker's create/exec/destroy, each still inside its own ADR-170
  event bracket (two small coroutines replace the immediately-invoked lambdas, so a scope still closes before
  the next stage starts); `SandboxRuntime::run` and `run_live`; `LiveShellSandboxProvider::shell_exec`,
  `check_lifetime`, `release` and `bind_sandbox`'s release of the old environment.
- **Finding: `shell_exec` could not run on a runtime lane.** It drove `run_live()` with `rt::block_on`, which a
  lane refuses (`rt.block_on_on_lane`, §4.3) -- latent only because sessions are not on the runtime yet (#179),
  the same shape as step 44's `recall`. It awaits now; L1 below proves it on a lane.
- **Callers that cannot await, each stated.** The WASM host import `http-request` calls
  `HostEgressProxy::fetch_blocking` (the guest is blocked inside the import; there is nothing to suspend to).
  `MediatedPythonRunner`'s destructor calls `NativeJailBackend::destroy_blocking`. `SandboxBackendRegistry`'s
  type-erased entry returns the seam's tasks. `RegisteredRunner` -- the shell's runner-composition typedef --
  is still synchronous (it is one of #174's untriaged typedefs); compositions call `run_blocking`, or drive
  `run` with `rt::block_on` in tests. **`LiveShellSandboxProvider`'s destructor and assignment operators**
  release with a guarded `rt::block_on(surface_->close())`: complete for every in-tree surface; on a lane a
  refusal is counted as a failed release (the accounting a failed close already gets, with ADR-139's orphan
  reaping as the last net) and never thrown out of a destructor. §4.5 rule 3a (owners close asynchronously) is
  the real answer and is not built.
- **Lock audit** (the phase's standing instruction). No `std::mutex` is held across a `co_await` in anything
  this phase touched: the backend and surface `.cpp` files contain no `co_await` at all, so every lock in them
  (`instances_mutex_`, `call_mutex`, `disk_quota_setup_mutex_`, the profile/grant statics) sits in a
  synchronous body; the coroutine callers hold none (`SandboxRuntime` already serialises on an `AsyncMutex`).
- **Also fixed: the `AGENTENGINE_BUILD_PYTHON_RUNNER` build was broken on `main`.** Step 48 made
  `materialize_skill_mounts` a task, but `cli_chat`'s `main()` and `test_mediated_python_runner_skill_mounts`
  still used it synchronously. Neither builds unless that option is ON, which no CI leg sets, so it went unseen.
  Both now drive it with `rt::block_on` (separate commit).
- **Evidence.** New `test_execution_seams_on_lane`: each caller driven ON A RUNTIME LANE against a conformer
  that parks on a reactor timer, every check also asserting the conformer really suspended. E1/E2
  `read_content` through a suspending egress backend (success, and the non-2xx mapping unchanged); R1 a
  suspending `Runner` still shares `ExecState` by reference across the suspension, R2 `run_shell` through the
  real pipeline; S1/S2 a suspending backend through the registry's type-erased entry, per-instance state kept;
  X1/X2 `SandboxRuntime::run` with a suspending surface (committed checkpoint; a refused command still
  refunds `RunCost`); L1/L2 `LiveShellSandboxProvider` bound, `shell_exec` invoked through its descriptor and
  the PerRun close, all on the lane. **Five planted mutants, each killed** (the test terminates, 0xC0000409, on
  the refused `block_on`): `rt::block_on` around the fetch in `invoke_via` (E1), around `shell_.run` in
  `SessionShellSandbox::run` (R2), in the registry's `exec` entry (S1), around `surface.run` in
  `SandboxRuntime::run` (X1), and the pre-#173 `block_on(run_live(...))` in `shell_exec` (L1).
- **Verification.** Windows `build-dev`: clean; `ctest -LE live-network` 417/418 -- **with the Docker-daemon
  tests running this time** (Docker Desktop started locally): `test_sandbox_runtime`,
  `test_mandatory_sandbox_provider`, `test_task_branch_tools`, `test_task_branch_concurrent_dispatch`,
  `test_docker_*`, `test_execution_surface_image_identity`, `test_composed_sandbox_providers_live`,
  `test_docker_persistent_shell_surface` and `test_live_shell_sandbox_provider_live` all pass, which closes the
  "verified by compilation only" gap steps 48-49 recorded for the sandbox-side wiring. The one failure is
  `test_external_skill_discovery` D6, which needs symlinked skills in this machine's `~/.claude/skills`
  (absent, as step 11 already noted). A second Windows tree with `AGENTENGINE_BUILD_PYTHON_RUNNER`,
  `AGENTENGINE_WITH_PDF`, `AGENTENGINE_WITH_WASM`, `AGENTENGINE_WITH_NATIVE_PROCESS` and HTTPS ON (none of which
  CI builds): clean; the PDF tools' tests (real sandboxed worker), the native-process, CodeAct and Python
  runner tests pass; `test_wasm_backend`/`test_wasm_tool_bridge` skip (no guest component built);
  `test_reference_agent_task_corpus` H1-T3 fails inside the jailed Python worker (`pandas` imported without
  `read_csv`) after the seam returned the worker's traceback correctly -- an interpreter-environment fault in
  the worker process, not this change, though it was not re-run on `main` to confirm. Linux (WSL, `-Werror`,
  HTTPS on): gcc and clang both build the full tree, `LinuxNativeJailBackend`, `KataBackend` and the containerd
  surface included; gcc's `ctest` is 384/400, the 16 failures being the Docker/containerd-daemon tests (WSL
  has no daemon) and `test_json_dump_escape`'s RLIMIT_DATA self-check, all expected there. naming-lint,
  layering-lint and milestone lint OK.

### Step 51 (2026-10-16): Phase E slice 1 -- one way to declare a tool; `drive_leaf_task` deleted (#175)

- **The static-invoke form is gone.** All **167** `Tool<>` conformers that still declared
  `static result<Reply> invoke(Args, EffectContext&)` (148 in tests, 11 in `tools/`, 8 in `examples/`; none left
  in `include/` or `src/`) now declare §5.2's `task<result<Reply>> invoke(Args, EffectContext&) const`. The
  rewrite was mechanical (signature, `const`, `return` -> `co_return` in the brace-matched body) and compiler-checked:
  a nested lambda's `return` turned into `co_return` would have failed to compile, and none did. No conformer took
  `Args` by reference, so no coroutine holds a reference into its caller's frame.
- **Deleted:** the zero-argument `make_tool_descriptor<ToolT>()` and `ToolTable::from_tools<Ts...>()`. Their 160
  call sites became `make_tool_descriptor(ToolT{})` / `ToolTable::from(Ts{}...)`, including the declarative
  `Tools<Ts...>` policy (`core/agent_registry.hpp`), which now default-constructs each tool. That was already the
  only way a static-invoke tool could be stateless.
- **Deleted:** `rt/drive_leaf_task.hpp` and `test_rt_drive_leaf_task` (`synchronous_leaf` went in step 44). Its two
  callers were the two detached background workers, and both now drive `invoke` with `rt::block_on`.
  - `src/core/tool_pipeline.cpp` (`background_task`'s worker) has no outer `catch`, so it keeps
    `drive_leaf_task`'s fault translation inline: the same class, message and code (`rt.leaf_task_faulted`,
    transient). Behaviour unchanged.
  - `rt/background_job_runner.hpp` already wraps the drive in a `catch`. A coroutine that throws now reaches it
    and reports `background_job.tool_threw` (fatal), with the tool's own message. Before, it was
    `rt.leaf_task_faulted` (transient). **This moves behaviour, and is recorded as a finding:** it restores what a
    throwing tool reported before §5.2 made `invoke` async, and the transient class was wrong for a tool that
    throws (a retry would throw again). R14 in `test_rt_background_job_runner` now pins the message. Against the
    old code it fails, because the message was "leaf task faulted".
- **Not done, and why:** `make_sync_invoke` stays, with its **three** production sites (the RAG pair went in
  step 44). The two native-process providers wait on **D3** (#176, open). The WASM bridge needs a WASM-enabled
  build to verify, which `build-dev` does not produce.
- **Stale user-facing docs, carried to Phase F:** `web/marketing/src` (`data/apiContent.ts`,
  `data/content.ts`, `components/Api*Reference.tsx`) still shows the static form, which no longer compiles.
  `docs/architecture/tool-and-skill-context-and-invocation.md` is updated.
- **Gate.** `build-dev` (MSVC) builds clean. `ctest -LE live-network`: **403/417**. The 14 failures are
  environmental: 13 fail with "docker API ... daemon is not running", and `test_external_skill_discovery` D6
  needs real symlinked skills installed on the machine. That matches §14.1's count of 14. (Run without the MSVC
  developer environment, the 15 `compile_fail` tests also fail, because the compiler cannot find its headers;
  with `vcvars64` they pass.) naming-lint, layering-lint and milestone-status-lint are OK.

### §14.1 What is left, and the phases to follow

**Exact count (2026-10-16, #174 triage, §14.2):** §5.1 names **31** seams. **25 are `task`-shaped**, **5 are
synchronous and must still convert** (`ApprovalDecider`, `PolicyDecider`, `ExecutorBody`, `ChildRunner`, the batch
`Call`), and **1 is reclassified to stay synchronous** (`SessionFactory`, with the batch `Build`/`Complete`
narrowed out of the same row). The triage adds **3 seams §5.1 never named** that must also convert
(`ReplayChatClient::SleepFn`, `RegisteredRunner::invoke`, `EffectContext::blob_sink`): **8 conversions left**.
The "~27" figure and the step-46-era paragraph below are kept as history; §14.2 supersedes their count.

Recorded at session end so the work resumes cold. **Status: §5.1 is 14 of ~27 seams converted**
(`SkillSource`/`ChunkingPolicy`, `ToolSourceFetch`, the A2A client, the A2A server, `GraderFn`, MCP --
§14 step 31, `OutputValidator` -- §14 step 32, `ChatClient` -- §14 steps 33-36, `SurfaceFactory` --
§14 step 37, `Resolver` -- §14 steps 38-41, `BatchBackend` -- §14 step 43, `VectorIndex`/`SparseIndex` -- §14 step 44, qdrant's `Resolver` -- §14 step 45, and `SecretStore` -- §14 step 46). **PHASE B IS COMPLETE** on the conversion side: every one
of its seams has a reactor/task form and the provider model path has no blocking body and no detached
thread. Its one outstanding item is DEAD-CODE SUBTRACTION, not conversion: delete the now-uncalled
`run_stream_worker`, `provider_wire::exchange_json` and `pump_sse_stream` (step 41's own note). §5.2's
tool contract is complete on the production side, but its transitional sync bridge is NOT removed.
Everything listed below is **not done**, and nothing in this ADR or PR #169 claims otherwise.

Sizing of the last two Phase B seams (2026-10-11, measured -- both are SMALL if taken as separate slices):
**`SurfaceFactory`** is one typedef, `std::function<Surface()>`, in `sandbox/live_shell_sandbox_provider.hpp:133`,
used at exactly TWO sites -- `bind_sandbox` (l.171, so `bind_sandbox` becomes `task<result<void>>`) and the
copy-assign (l.202, which keeps `rt::block_on(make_surface_())`; the SAME operator already does
`rt::block_on(spawn_child_branch(...))` at l.187, so the idiom is precedented in place). Conformers are tiny
coroutines. **`Resolver`** is `std::function<result<VerifiedEndpoint>(std::string_view, std::uint16_t)>`, and
it must be split: the provider half (`protocol/provider_chat_wire.hpp:26`, aliased by both backends) is the
step-36 residual -- give hosts a Runtime, then delete the blocking `exchange_json` + the parameter, multi-host
and multi-test-site; the qdrant half (`protocol/qdrant/vector_index.hpp:90`, same shape) belongs in Phase C
with the index seams. `rt/dns.hpp`'s `HostResolver` is NOT a seam to convert -- it is already the
reactor-vocabulary input (a blocking resolver run on the DNS pool). Order taken: `SurfaceFactory` first (small,
closes the small end of Phase B), then Phase C with the qdrant `Resolver` folded in.

**Remaining §5.1 seams (sync -> `task`):** ~~`Runner`; `SandboxBackend`; `ExecutionSurface`;
`PersistentShellSurface`; `NetEgressBackend`~~ (Phase D, §14 step 50); ~~`SecretStore`; the three stores~~ (Phase
C, steps 46-49); ~~plus ~15 further `std::function` typedefs that have not been triaged individually yet~~
(triaged, #174, §14.2). What is left after the triage: `ApprovalDecider`, `PolicyDecider`, `ExecutorBody` (with
`AgentExecutorBodyTag::Impl`), `ChildRunner`, `BatchableModelCall::Call` and `EffectContext::blob_sink` (**Phase
G**, §14.2), plus `ReplayChatClient::SleepFn` and the shell's `RegisteredRunner::invoke` (with #180, the blocking
conformer bodies).

**Remaining §5.2 cleanup:** delete `make_sync_invoke`'s **five** production call sites -- counted by grep,
not recollection: the two RAG tools (`core/hybrid_rag_context_provider.hpp:211`,
`core/vector_rag_context_provider.hpp:367`), the two native-process providers
(`native_process/native_providers.hpp:230`, `native_process/native_shell_session_provider.hpp:324`, which
wait on **D3**), and the WASM bridge (`wasm/wasm_tool_bridge.hpp:135`, which needs a WASM-enabled build to
verify). Then remove the zero-arg `make_tool_descriptor<ToolT>()` / `ToolTable::from_tools<Ts...>()`
static-invoke form (the remaining conformers are mostly tests).

**Carried but unstarted:** D3, D4, D7; the §10 spec + 027 vocabulary + `web/marketing` pass; the §8 gates.

**Phase A -- MCP, as one atomic slice.** DONE (§14 step 31, 2026-10-11). The inventory above was the whole
spec; it went first because it was the only seam fully inventoried and self-contained. The
`mcp_conformance_client.cpp` drive-loop question was settled before editing: that binary reaches
`rt::block_on` through `agentengine::core` (see step 31), so no CMake change was needed.

**Phase B -- core seams.** `OutputValidator` DONE (§14 step 32, 2026-10-11). The phase's "zero test
coverage" premise was too strong and is corrected in step 32: the seam's behaviour was already pinned
end-to-end (bridge P9, session O1/O2/O5, workflow O4), but the typed producer `output_validator_for<A>()`
had no *direct* test and P9 could not separate a parse error from a schema mismatch -- so P9b was ADDED to
`test_agent_session_bridge` before the conversion (run green against the sync seam, then re-run after).
Remaining: `Resolver` (`SurfaceFactory` landed as §14 step 37). The `ChatClient` producer-ownership question is now
settled (owner decision 2026-10-11: **client-owned**, NOT §4.5 rule 3a's caller-passed scope -- a
`task_scope` needs a strand, and ~90 call sites have none), and its prerequisite landed as **step 33**
(`rt::Runtime::spawn` / `rt::RootHandle`, the non-blocking owned-root primitive). The provider `chat_stream`
rewrite onto `rt::http` and dropping `chat()` are the next slices
(`~/.commandcode/plans/adr237-phase-b-chatclient-switchover.md`).

Sizing note for the drop-`chat()` slice (2026-10-11, measured): it is much larger than the plan assumed.
`AgentSession` picks the unary path whenever `stream_model_calls_` is false -- which is the DEFAULT in the
tests -- so ~50 fixtures across ~35 files stub `chat_stream` as `return {};` (each with its own `State` shape;
some have no `state_` at all). Deleting the session's `chat()` branch flips every one onto the streaming path,
so each stub must first become a real `chat_stream` (one delta per scripted item, usage on the final update --
see the `response_stream`-style helper a landed pass should add). Verified by experiment: deleting the branch
without that conversion segfaults ~40 session tests. The conversion must precede the deletion, or the tree
breaks. Ordering: (1) add the helper, (2) convert every stub (green throughout, since `chat()` still exists),
(3) delete `chat()` + `LegacyChatClient` + the branch + the providers' `chat()` and its call sites.

DECISION (2026-10-11): given that measurement, §5.1's OTHER option was taken instead -- `chat()` KEPT, made
non-blocking on the reactor (**step 35**). It reaches the same charter goal (no blocking body, no detached
thread on the model path) at ~1 file of change, and keeps `LegacyChatClient`, `AgentSession`'s non-streaming
mode, and every fixture intact. The drop-`chat()` ordering above is recorded for the event someone still
wants it; it is not currently planned.

**Phase C -- index and storage seams.** ~~`VectorIndex`/`SparseIndex`~~ and ~~the RAG `recall`
conversion~~ done together (§14 step 44); ~~the "async in name only" bodies (`QdrantVectorIndex`,
`OpenAIEmbedder`) onto `rt::http` with qdrant's `Resolver`~~ (§14 step 45); ~~`SecretStore`~~ (§14 step 46); ~~`SessionStore`~~ (§14 step 47); ~~`AppendLogStore`~~ (§14 step 48); ~~`WorktreeObjectStore` incl. `Ledger`~~ (§14 step 49, ADR-238). The RAG `recall`
conversion was planned to belong here rather than earlier: `recall` deletes two
`synchronous_leaf` contract-error paths that tests pin deliberately, and it depends on the index seams.
Gate: those tests are updated with the reason recorded -- never deleted to make a build pass.
**PHASE C IS COMPLETE (2026-10-15, #172):** every seam in its checklist has a `task` form (steps 44-49), and the RAG `recall` tools landed with the index seams in step 44, so no `synchronous_leaf` path remains in `include/` or `src/`. Not done and not claimed: Phase D (execution/sandbox surfaces) and Phase E (§5.2 cleanup).

**Phase D -- execution and sandbox surfaces.** `Runner`, `SandboxBackend`, `ExecutionSurface`,
`PersistentShellSurface`, `NetEgressBackend` (`BatchBackend` was taken early, §14 step 43). Grouped because they share the
`EffectContext`/capability plumbing. Expect the mutex-across-suspension hazard from the A2A server
(`protocol/a2a/server.hpp:141`) to recur -- audit every lock when one of these goes async.
**PHASE D IS COMPLETE (2026-10-15, #173):** all five seams are `task`-shaped (§14 step 50, one slice each), every
production caller awaits them except the stated synchronous edges (the WASM host import, two destructors, the
`RegisteredRunner` typedef), and the lock audit found no `std::mutex` held across a `co_await`. Not done and not
claimed: the blocking conformer bodies moving onto `rt::` (#180), §4.5 rule 3a's asynchronous close for the
providers whose destructors still `block_on` a surface close, and Phase E (§5.2 cleanup).

**Phase E -- §5.2 cleanup, only after C and D.** The five `make_sync_invoke` sites and the static
`make_tool_descriptor`/`from_tools` form can only be deleted once nothing synchronous remains downstream of
them. The WASM site additionally needs a WASM-enabled build to verify, which the current local build does
not produce.
**Slice 1 DONE (§14 step 51, 2026-10-16, #175):** the static-invoke form (167 conformers, `make_tool_descriptor<T>()`,
`from_tools`) and `drive_leaf_task` are deleted. Left: `make_sync_invoke` and its three production sites
(native-process x2 after D3/#176; WASM with a WASM-enabled build).

**Phase F -- the carried threads.** D3/D4/D7, the §10 + 027 + `web/marketing` pass, the §8 gates.

**Phase G -- the orchestration and decider seams (found by the #174 triage, §14.2).** `ApprovalDecider`,
`PolicyDecider`, `ExecutorBody` + `AgentExecutorBodyTag::Impl`, `ChildRunner`, `BatchableModelCall::Call`,
`EffectContext::blob_sink`. §5.1 named five of these from the start; §14.1's remaining list dropped them, and
nothing converted them. The bodies are where engine code `rt::block_on`s from inside a task today
(`agent_spawn_child_run.hpp:139`, `agent_workflow_executor.hpp:87`, `workflow_supervisor.hpp:1160`), so this
phase removes those nested drive loops. Order: the two deciders first (`src/core/tool_pipeline.cpp:131,136` for
the policy decider, `:213,479` for the approval decider; ADR-070's "unset = today's behaviour" kept as an
immediately-ready task), then
`ExecutorBody` and `Impl` together (one signature), then `ChildRunner`, then `Call`, then `blob_sink`.

**Gate for every phase, unchanged:** build-dev clean; the named tests for the touched area pass; naming-lint
OK; layering-lint OK; `ctest -LE live-network` -> 401/415 with the identical 14. Never mark §5.1 or §5.2 done
on anything less. And the standing behavioural rule: **a seam going async must not move behaviour** -- the
A2A D4 assertions are the exemplar. If a test has to change, that change is the finding, and it gets
recorded rather than absorbed.

### §14.2 The `std::function` seam triage (2026-10-16, #174)

**Method.** Every `std::function` / `std::move_only_function` **type alias** in `include/` and `src/`, found by
grep (`using X = std::function<...>`, single-line and wrapped), not taken from memory: **49 aliases**. Each one was
read at its definition and at its call sites, then given one of three dispositions:

- **async** -- already returns `task<...>`. Either converted by a §14 step or written async from the start.
- **stays** -- synchronous by design. The reason is a §5.3 category (pure, accessor, sink, clock) or is
  stated in the row.
- **convert** -- synchronous today but does I/O or blocks (directly, or by `rt::block_on` on a nested task
  inside an engine task). The row names the phase it joins.

Result: **20 async, 22 stay, 7 convert**. Two triage decisions narrow §5.1's list, and the evidence is in their
rows: `SessionFactory` stays, and of the batch callables only `Call` converts.

| # | Alias | Defined at | Disposition | Reason / phase |
|---|---|---|---|---|
| 1 | `OutputValidator` | `core/agent_session_bridge.hpp:250` | async | §14 step 32 |
| 2 | `ContextProviderDescriptor::OnContextFn` | `core/context_assembly.hpp:128` | async | Born `task`-shaped |
| 3 | `ContextProviderDescriptor::OnTurnEndFn` | `core/context_assembly.hpp:135` | async | Born `task`-shaped |
| 4 | `ContextProviderDescriptor::OnRunEndFn` | `core/context_assembly.hpp:145` | async | Born `task`-shaped |
| 5 | `ReflectionEvaluator` | `core/harness.hpp:223` | async | Born `task`-shaped (ADR-234) |
| 6 | `ToolCallHook` | `core/tool_call_hook.hpp:79` | async | Born `task`-shaped |
| 7 | `ToolDescriptor::InvokeFn` | `core/tool_descriptor.hpp:64` | async | §14 step 9 |
| 8 | `ToolSourceFetch` | `core/tool_optimizer_provider.hpp:113` | async | §14 step 27 |
| 9 | `TurnMiddlewareHook` | `core/turn_middleware.hpp:271` | async | Born `task`-shaped |
| 10 | `GraderFn` | `eval/eval_grader.hpp:42` | async | §14 step 30 |
| 11 | `a2a::CardFetcher` | `protocol/a2a/client.hpp:59` | async | §14 step 28 |
| 12 | `a2a::RunStarter` | `protocol/a2a/server.hpp:89` | async | §14 step 29 |
| 13 | `mcp::RequestSender` | `protocol/mcp/client.hpp:61` | async | §14 step 31 |
| 14 | `mcp::InputRequestHandler` | `protocol/mcp/client.hpp:92` | async | §14 step 31 |
| 15 | `mcp::RequestSenderWithHeaders` | `protocol/mcp/client.hpp:332` | async | §14 step 31 |
| 16 | `rt::HttpTlsConnector` | `rt/http.hpp:237` | async | Born `task`-shaped (§14 step 6) |
| 17 | `ProjectSupervisor` `CheckpointHook` | `rt/project_supervisor.hpp:69` | async | Born `task`-shaped |
| 18 | `WorkflowSupervisor::CheckpointHook` | `rt/workflow_supervisor.hpp:891` | async | Born `task`-shaped |
| 19 | `WorkflowSupervisor::MergeOnJoinHook` | `rt/workflow_supervisor.hpp:898` | async | §14 step 48 |
| 20 | `LiveShellSandboxProvider::SurfaceFactory` | `sandbox/live_shell_sandbox_provider.hpp:137` | async | §14 step 37 |
| 21 | `DescriptorFilter` | `core/agent_session_bridge.hpp:256` | stays | §5.3: pure, edits an in-memory vector |
| 22 | `BatchRecorder::Sink` | `core/batch_recording.hpp:123` | stays | §5.3 sink: must not block |
| 23 | `BatchReplayer::DivergenceHook` | `core/batch_recording.hpp:145` | stays | §5.3 sink |
| 24 | `ContentReplayTrigger` | `core/content_replay_gateway.hpp:77` | stays | §5.3: pure over a `ChatResponse` |
| 25 | `ContentReplayTraceHook` | `core/content_replay_gateway.hpp:90` | stays | §5.3 `*TraceHook` |
| 26 | `MiddlewareTraceHook` | `core/middleware.hpp:99` | stays | §5.3 `*TraceHook` |
| 27 | `ModelCallGateway::JitterSource` | `core/model_call_gateway.hpp:166` | stays | §5.3: pure |
| 28 | `RecordingChatClient::RecordingSink` | `core/recording_chat_client.hpp:129` | stays | §5.3 sink |
| 29 | `BackgroundTaskCompletion` | `core/tool_pipeline.hpp:431` | stays | A completion sink. It fires once from the background worker, and its one production wiring is a self-`tell()` (an enqueue). §5.3's non-blocking rule applies |
| 30 | `rt::EmitFn` | `rt/agent_session_trust.hpp:31` | stays | §5.3 sink (named there) |
| 31 | `rt::BackgroundJobSink` | `rt/background_job_runner.hpp:148` | stays | §5.3 sink (named there) |
| 32 | `rt::HostResolver` | `rt/dns.hpp:84` | stays | The reactor's blocking-resolver **input**. It runs on the DNS pool, never on a strand (§14.1 sizing note) |
| 33 | `rt::AddressPolicy` | `rt/dns.hpp:99` | stays | Pure predicate over an `IpAddress` (I3: host code) |
| 34 | `provider_wire::Resolver` | `protocol/provider_chat_wire.hpp:28` | stays | Converted in **placement**, not shape (§14 step 40). `host_resolver_from` runs it on the DNS pool as a `HostResolver` |
| 35 | `qdrant::detail::Resolver` | `protocol/qdrant/vector_index.hpp:93` | stays | Same as #34 (§14 step 45) |
| 36 | `LiveShellClock` | `sandbox/live_shell_lifetime.hpp:80` | stays | §5.3 clock (named there) |
| 37 | `SandboxBackendResolutionAuditHook` | `sandbox/sandbox_backend_registry.hpp:63` | stays | §5.3 `*AuditHook` |
| 38 | `QuarantineAuditHook` | `trust/secret_quarantine.hpp:111` | stays | §5.3 `*AuditHook` |
| 39 | `worker::QueryFn` | `src/backends/native_jail/python_worker_mediation.hpp:96` | stays | Runs in the **jailed Python worker process**, not the engine. The worker is single-threaded CPython with no reactor, and the blocking round trip over its framed pipe *is* the protocol. The engine side of that pipe is the `Runner` (Phase D, async) |
| 40 | `rt::SessionFactory<...>` | `rt/multi_agent.hpp:90` | stays (**narrowed from §5.1**) | It only constructs a fresh `unique_ptr<AgentSession>`. Every in-tree conformer builds an object and does no I/O. Anything that would need I/O (a resume from `SessionStore`, a state load) is a call on the session afterwards, and those are already `task`-shaped (§14 step 47). Making construction async would buy nothing |
| 41 | `BatchableModelCall::Build` | `rt/workflow_supervisor.hpp:291` | stays (**narrowed from §5.1**) | Pure by contract: "`build` must be deterministic in its input" (ADR-235 §3.1). A failed batch item is rebuilt, and that is only sound because building does nothing |
| 42 | `BatchableModelCall::Complete` | `rt/workflow_supervisor.hpp:295` | stays (**narrowed from §5.1**) | Pure: `(Message, ChatResponse) -> ExecutorOutcome`, with no `EffectContext` |
| 43 | `ApprovalDecider` | `core/tool_pipeline.hpp:149` | **convert** -- Phase G | Named in §5.1, never converted. It is called inline from the async pipeline (`src/core/tool_pipeline.cpp:213,479`), so a host decider that asks a policy service blocks the strand. A human wait stays on the session's suspend-for-approval path, which is unchanged |
| 44 | `PolicyDecider` | `core/tool_pipeline.hpp:170` | **convert** -- Phase G | Named in §5.1. Same reason as #43 (`tool_pipeline.cpp:131,136`). ADR-070's graduated host logic is exactly what tends to be remote |
| 45 | `ExecutorBody` | `rt/workflow_supervisor.hpp:248` | **convert** -- Phase G | Named in §5.1. The agent-backed body drives a whole `AgentSession` run with `rt::block_on` (`agent_workflow_executor.hpp:87`) from inside the supervisor's `run_executor_job` task, and `sub_workflow` does the same (`workflow_supervisor.hpp:1160`) |
| 46 | `AgentExecutorBodyTag::Impl` | `rt/workflow_supervisor.hpp:264` | **convert** -- Phase G, with #45 | Same signature as `ExecutorBody`, so it changes in the same slice |
| 47 | `rt::ChildRunner` | `rt/agent_spawn.hpp:160` | **convert** -- Phase G | Named in §5.1. `agent.spawn` is a `task`-shaped tool, and it calls `run_child` synchronously (`agent_spawn.hpp:552`). The conformer then `rt::block_on`s the child's run (`agent_spawn_child_run.hpp:139`) |
| 48 | `BatchableModelCall::Call` | `rt/workflow_supervisor.hpp:293` | **convert** -- Phase G | Named in §5.1. It is the synchronous model call, and `ChatClient` is `task`-shaped since §14 steps 33-35 |
| 49 | `ReplayChatClient::SleepFn` | `core/replay_chat_client.hpp:144` | **convert** -- with #180 | Not named in §5.1. The default `real_sleep` is `std::this_thread::sleep_for` on a detached worker thread (`replay_chat_client.hpp:229`), the shape Phase B removed from the live provider path. It becomes `task<void>(milliseconds)` over `rt::sleep_until` when the replay worker moves onto `rt::` |

**Seams that are not aliases, found by the same sweep.** The issue named `RegisteredRunner` a "typedef", but
it is a struct with a `std::function` member. The grep for non-alias `std::function` members (89 lines) turned up
three more seams. Everything else it found is a §5.3 category: `EffectContext`'s event sinks, `on_lane_stall` /
`on_stuck_run` / `on_overrun` watchdog hooks, `stop_callback` payloads, `now_ns` / `clock()` clocks,
`is_planning_safe` predicates, and `on_body` chunk sinks.

| Member | Defined at | Disposition | Reason / phase |
|---|---|---|---|
| `RegisteredRunner::invoke` (both registries) | `src/backends/native_jail/command_registry.hpp:46`, `mediated_command_registry.hpp:36` | **convert** -- with #180 | Runs a whole `Runner` (`shell_dispatch.cpp:270`), which is `task`-shaped since §14 step 50. Compositions call `run_blocking` today. It converts when the shell dispatcher itself goes async, together with the blocking conformer bodies |
| `RegisteredTool::invoke` (both registries) | `command_registry.hpp:59`, `mediated_command_registry.hpp:46` | stays | A test stub by its own header comment (ADR-052, gap-audit finding 13), with zero production call sites. A real integration goes through `invoke_tool()`, which is async |
| `EffectContext::blob_sink` | `core/effect_context.hpp:237` | **convert** -- Phase G | Step 9's oversized-result promotion. Its documented conformer is `WorktreeObjectStore::put_blob`, which is `task`-shaped since §14 step 49, so a real wiring would have to `block_on` inside the pipeline. Only tests wire it today, which makes it cheap to convert now |

Not re-triaged here: `sandbox/provider_http_client.hpp`'s and `net_egress_proxy.hpp`'s `resolver` parameters
(the same shape as #34). They feed the blocking conformer bodies #180 already owns.

**What this changes.** Nothing in code. This is triage, and the gate is that every row cites the line it was read
from. §14.1's count is now exact (its header paragraph), and the "~15 untriaged" item there is closed. The two
**narrowings** (#40 and #41/#42) are triage decisions under this ADR's own §5.3 test ("pure computations over
in-memory values that no conformer can make remote"). They are proposed for the owner's sign-off as **ADR-239**,
which amends §5.1's list and ADR-235 §3.1, and adds the contract terms the narrowings need.

