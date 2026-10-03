# ADR-237 — Every extension point is asynchronous: the I/O reactor, the threading model, and the seam contract

**Status:** **Draft (2026-10-03). Design, revised after one adversarial red-team round (§11: 6
MUST-FIX, 7 REAL GAP, all answered in the design or listed as open decisions). Not implemented.** Written at the project
owner's direction ("every component can be written to call the network, sandbox and shell included —
everything should be asynchronous"; conversion clean and complete, not piecemeal; the codebase is
pre-production, so shapes may be redone rather than preserved). Owner decisions recorded in §9
(2026-10-03). Next step: a second red-team round against the revised §4 and §9 before any code.

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
resuming the parent). Rule: symmetric transfer only when the homes are equal; otherwise post the awaiter
to its own home.

### 4.3 Lanes, I1 and the session strand

A **lane** is a FIFO of ready continuations served by the lane workers. A parked coroutine is not on any
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

**Holder identity and re-entry (red-team G1).** Holder ids move to the promise with an **ancestor
chain**. `when_all` children are distinct holders whose chain includes the round that spawned them.
`AsyncMutex::lock()` fails immediately with `rt.lock_held_by_ancestor` when the current holder is an
ancestor of the requester — the session → workflow → agent node → tool → back-into-the-parent cycle
(ADR-057, `agent_spawn`, workflow-as-chat-client) becomes a diagnosable error instead of a run parked
forever with no blocked thread to see. A stuck-run watchdog (no progress on a strand past a configured
bound) reports anything the ancestor check cannot see.

`block_on` on a lane worker is refused with `rt.block_on_on_lane` (it would park a worker waiting for work
that may need that worker — the ADR-175 §6 "parked pool job holds its worker" deadlock). Today nothing
refuses it (no check exists in `rt/`); the refusal is new and has a positive-control test. The same
refusal applies on offload workers. `block_on` is not the only way to block a lane (red-team G7):
`future::get` (`workflow_supervisor.cpp:647,825`), condition-variable waits, a `std::mutex` held across a
`co_await` (undefined once the coroutine can resume on another thread), and slow sinks. These are removed
from engine code by the conversion, and a **lane-stall watchdog** (a lane worker that has not returned to
its queue within a bound) reports what review misses. Lanes are FIFO; cancellation and approval resumes
are posted with priority over ordinary continuations so a CPU-heavy tool cannot delay a cancel.

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
- **Completion vs. cancellation race.** Each operation record has a single atomic state
  (`pending → completed | canceled`); whichever transition wins posts the continuation exactly once; the
  loser is a no-op. The record is owned by the awaiting frame and outlives every handler that can reference
  it (§4.5 rule 3).
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
   - counts in-flight operations in the promise; destroying a non-done frame with a non-zero count is a
     checked contract violation (debug: abort with a diagnostic; release: the frame is handed to a
     process-wide reaper scope that cancels and joins it, and the violation is reported) — never a
     silent use-after-free;
   - **revokes drop-to-cancel** for ADR-017 streams and ADR-219 parked coroutines: cancellation is
     `request_stop` + await completion; dropping is legal only after completion. Both ADRs get an
     amendment note;
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
4. **Shutdown order is fixed:** cancel all session scopes → join them (bounded by a shutdown deadline,
   overruns are a reported error, not a detach) → stop lanes → stop the reactor. A reactor destroyed while
   operations are pending is a contract violation caught in debug builds.
5. **Back-pressure is a suspension, not a block — except where the producer is synchronous (revised
   after red-team M6).** `channel::push` gains `co_await producer.push(v)`; `stream<T>` exposes
   `co_await s.next()`; the 5 ms polling drains are deleted. But `EffectContext::report_progress` and the
   other event sinks are synchronous callables (`effect_context.hpp:115`) called from tool bodies,
   including concurrently from parallel-batch children, and the channel is single-producer. So the run-
   event path becomes: sinks **enqueue** into a per-session bounded multi-producer queue (never block,
   never hold `run_event_mutex_` across a push — H-d); one session-scoped task drains it into the
   consumer-facing stream with real `co_await` back-pressure. Overflow policy is explicit: progress and
   delta events coalesce (latest wins per call id); lifecycle events (`ToolCallStarted/Completed`,
   `RunCompleted`, approvals) are never dropped — the queue reserves room for them and, if even that is
   exhausted, the run fails with a `resource` error rather than losing one.

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
`Resolver`. `ChatClient` gains a real `chat()` again (004 §1 lists both) or drops it in favour of
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
- **TLS details (red-team minor):** a write canceled mid-record drops the connection; concurrent TLS 1.3
  handshakes need mbedTLS's PSA state thread-safe (`MBEDTLS_THREADING_C` or one handshake at a time) —
  the current config is to be checked.

## 7. New problems this design creates (to be attacked by red-team)

Named here so the red-team pass starts from them, not from scratch:

1. **Thread migration.** A session continuation may resume on another lane worker. Any remaining
   thread-local state (holder ids today, any `thread_local` caches, Windows per-module TLS — ADR-175 §6)
   breaks silently. Mitigation: holder/home in the promise; a lint for `thread_local` in `rt/`/`core/`.
2. **Strand + mutex double-serialisation.** Can a continuation waiting on `session_mutex_` and the strand
   deadlock with a child strand that needs the parent's mutex? Rule proposed: child strands never take
   `session_mutex_`; they communicate only through their return values and the sinks of ADR-160 §5.
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
1c. **Ancestor lock:** a tool that re-enters its own parent session fails with `rt.lock_held_by_ancestor`.
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
  branch regularly so the final merge is not a surprise.

- **D3 — Argument-dependent capability requirements (delegated; recommendation).** The native-process
  providers do not need "any one of N grants" (an `AnyOf<…>` ceiling); they need a requirement that
  depends on the call's **arguments**: running `python3` requires a `NativeExec` grant covering
  `python3` on the provider's mount. Today the tool declares an empty ceiling and matches grants inside
  its own body (`src/backends/native_process/native_providers.hpp:158-181`), outside the pipeline's
  steps 4/7, so the audit record shows no bound capability for the call. Recommendation: a tool may
  declare, in addition to its static ceiling, a **per-call requirement** —
  `std::vector<Capability> required_capabilities(Args const&) const` — evaluated by the pipeline after
  step 2 (validated, typed arguments), and bound at step 4/7 exactly like the static ceiling
  (`bind`/`bind_clamped`, ADR-217), so the decision, the binding and the audit are the pipeline's again.
  I2/I3 hold: the model's argument chooses *which* already-held grant must cover the call; it can only
  narrow within held authority, never mint or widen it (the ADR-070 boundary), and a requirement no held
  grant covers is denied at step 4 as today. The static ceiling stays the upper bound — a per-call
  requirement outside the declared ceiling's kind is a contract error. This replaces the "empty ceiling
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
  `max(1, hardware_concurrency() / 2)`; `RuntimeConfig::lanes` overrides it. This amends CONVENTIONS'
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
  that caller lets it. Every `switch` over `failure_class` gains the case (compiler-checked).

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

  `runtime.enter` is the only way to `co_await` an engine task from a foreign coroutine type; a bare
  `co_await session.start_run(…)` from a non-`ae::task` coroutine is a compile error (the `task`
  awaiter is restricted to `ae::task` promises), so the choice cannot be forgotten. `runtime.run` is
  `block_on` at the edge and is refused on lanes/offload workers (§4.3). Names (`Runtime`, `enter`,
  `CompletionThread`) are 027 §4 additions, confirmed with the owner before implementation.

## 10. Spec changes this ADR will carry (when Judged)

- **001 §5/§8:** define the reactor, lanes, strand and `offload`; replace the undefined `Fast` trait and
  "drain-budget test (023)" with the lane-starvation rule (§7.3) and its test; resolve C1 (020 §3a vs
  ADR-219 on who resumes a stream consumer) in favour of the home rule.
- **006 §1:** `invoke` is an instance member; §5 references `when_all` child strands; §6b background via
  the session task scope.
- **008 §2, 018 §4:** unchanged in signature (already async); add the measured cancellation bound.
- **020 §3a:** host modes (engine-owned lanes / host-run lanes / `Resumer`), the reactor thread rule.
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
