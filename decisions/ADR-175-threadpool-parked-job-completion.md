# ADR-175 — Who resumes a parked coroutine, and who owns a lock while its holder is parked?

- **Status**: Proposed — revision 3 implemented. Red-teamed in four rounds (§8): round 1 broke revision 1,
  round 2 broke revision 2, round 3 found four must-fix defects in the implementation, round 4 found three
  more (one of them pre-existing in HEAD). All fixed and re-proven. Pending project-owner sign-off.
- **Date**: 2026-09-14
- **Closes**: GitHub issue #78, and the hazard class it belongs to.
- **Revises** ADR-064 §7's `ThreadPool::run_job()` fix and ADR-123's owner-thread check
  (`AsyncMutex::is_held_by_current_thread()`). Reopens nothing in ADR-037.
- **Files**: `include/agentengine/rt/resume_home.hpp` (new); `rt/async_mutex.hpp`; `rt/channel.hpp`;
  `rt/block_on.hpp`; `rt/task.hpp`; `rt/thread_pool.hpp`; `rt/drive_leaf_task.hpp`; the five former
  resume-until-done loops in `rt/agent_workflow_executor.hpp`, `rt/agent_spawn_child_run.hpp`,
  `rt/workflow_as_executor.hpp`, `rt/workflow_as_chat_client.hpp`, `rt/workflow_supervisor.hpp`;
  `core/session_builder.hpp`; comments in `rt/agent_session.hpp`, `rt/agent_spawn.hpp`,
  `rt/bounded_call_fanout.hpp`; `tests/rt/test_rt_parked_task_home.cpp` (new); `tests/rt/test_rt_thread_pool.cpp`
  T6 and `tests/rt/test_rt_drive_leaf_task.cpp` D5 (rewritten); `tests/core/context/test_session_builder.cpp` B30-B31;
  `tests/CMakeLists.txt`; `bench/rt_block_on_handoff.cpp` (new); the marketing site's Builder API and HITL
  pages, which quoted `Bundle::ask()`'s old single-resume code.

---

## 1. The question

Issue #78 reports that `ThreadPool::run_job()` destroys a job another thread is already running. It
resumed a job once and, if it was not `done()`, reported it faulted and destroyed it — arguing that a
parked `AsyncMutex` waiter removes itself from the queue when destroyed. That argument missed a waiter the
releasing thread has already popped and resumed in the gap before the `done()` check.

Reproduced on this machine: the issue's own program, MSVC `/O2 /fsanitize=address`, **3 of 3 runs** crash
within a second (`heap-use-after-free`, `attempting double-free`), stacks matching the issue.

The first fix attempted (§8, revision 1) answered the narrow question — "what should the pool do with a
job that suspends" — and was broken by red-team round 1. The question that has a stable answer is wider.
`rt::` had **eight** places that drive a `task<T>` from non-coroutine code, and **seven** mishandled a
task that suspends:

| Driver | On suspension it… |
|---|---|
| `ThreadPool::run_job()` | resumed once, then destroyed a frame another thread could be running (#78) |
| five `drive()` loops (workflow agent executor, agent spawn child run, workflow-as-executor, workflow-as-chat-client, sub-workflow) | called `resume()` again on a parked handle — a double resume; `block_on.hpp`'s banner records it handing back a `Guard` for a lock not held, then a use-after-free |
| `drive_leaf_task()` | resumed once, returned an error, destroyed the frame — #78's shape |
| `Bundle::ask()` / `ask_stream()` | same as `drive_leaf_task()` |
| `block_on()` | waited correctly |

And every one of them, `block_on()` included, shared a second problem: **a parked coroutine was resumed
inline on whichever thread released the lock or pushed the item.** That is what produced both of round 1's
new hazards once a job could survive parking:

- **Livelock.** A job resumed inside `unlock()`'s trampoline releases the mutex — deferred, because the
  trampoline is draining — then calls `block_on(lock the same mutex)`, which waits on the very thread whose
  trampoline would finish the release.
- **I1 violation.** `owner_` recorded the OS thread that resumed the holder. A holder that parks again
  leaves that thread free to run unrelated code for which `is_held_by_current_thread()` is **true**;
  `AgentSession::fork_from()` then skips `session_mutex_` and reads session state mid-round.

So the question this ADR answers: **when a coroutine parks, who resumes it, on which thread — and who
owns a lock while its holder is parked?**

## 2. Decision

**A coroutine driven by `block_on()` belongs to the thread blocked in that call — its home. When it parks,
the waker hands the handle back to that thread instead of resuming it. `ThreadPool` drives every job with
`block_on()` on its worker. Lock ownership is the holder id of the logical task the lock was granted to,
recorded at the moment of granting.**

## 3. The mechanism

### (a) `rt/resume_home.hpp` (new)

- **`detail::CallerHome`** — a mutex, a condition variable, a queue of posted `{handle, holder}` and a
  done flag. The thread blocked in one `block_on()` call, seen as a place to hand coroutines back to.
- **`detail::HomeSlot`** — owned by one `block_on()` call on its stack; the `CallerHome` is created
  **lazily on the first park**, on that thread. An uncontended `block_on()` allocates nothing extra.
- **Execution context** — two thread-locals: the `HomeSlot*` of the `block_on()` currently driving this
  thread's coroutine (or none), and a holder id. `ScopedExecution` sets both for a scope and restores the
  previous values. The home a parking coroutine records is decided by the thread it parks on.
- **Holder ids** — minted from one atomic counter (never 0). A thread with no driver active has an
  **ambient** id of its own, so `current_holder_id()` always names *some* task; there is no "no
  identity" value to fall back from (round 2 finding 2).
- **Closed homes** — when `block_on()`'s wait ends ("done, nothing queued") the home is closed in the same
  critical section. A coroutine that parked under a `block_on()` it did not belong to — raw-resumed inside a
  pool job, or hopped onto one by a bare `coroutine_handle::resume()` — would otherwise be granted the lock
  after that call returned, posted to a queue nobody drained, and hold the mutex forever (round 3 finding 2).
  `try_post()` reports the closure instead, and `AsyncMutex` hands such a waiter to its own trampoline;
  resuming it inside `post()` nested each one in the previous waiter's `unlock()` and overflowed a 1 MiB
  stack at 3,000 waiters (round 4 finding 1). A waiter granted this way also gets a **fresh holder id**, so
  the thread whose `block_on()` it had adopted is not reported as holding the lock (round 4 finding 2).
  `channel<T>`, which has no trampoline, keeps the inline `post()` path; it has a single consumer.
- **Spin before sleep** — `wait_next()` checks an atomic hint up to 20,000 times (yielding every 1,024)
  before sleeping. A hand-off now wakes the home thread instead of running the successor inline on the
  releasing thread; without the spin a contended short critical section cost 13× HEAD (round 3 finding 5).
- **`detail::capture_parked(h)`** — called by an awaiter's `await_suspend()` before it registers anything:
  records `{h, home (created if needed), current_holder_id()}`.
- **`detail::wake(record)`** — called by a waker after releasing its lock: a homed record is **posted** to
  its home; a homeless one is resumed inline as before, under its own recorded holder id and with no home,
  so it neither adopts the waker's identity nor parks back to the waker's `block_on()`.

### (b) `AsyncMutex`

- Waiters are `ParkedResumer` records.
- `owner_` is a holder id, written under `m_` at the moment the lock is **granted**: the uncontended fast
  path, the `await_suspend()` re-check path, and the hand-off pop in `unlock()`. Not at `await_resume()`:
  a posted successor may not run for a while, and until it does the releasing task would still be
  reported as holder (round 2 finding 1; planted mutant m1 reproduces an I1 overlap, §7).
- `is_held_by_current_thread()` keeps its name for source compatibility and answers "held by the task
  running on this thread": `owner_ == current_holder_id()`, read lock-free (`owner_` is an atomic written
  under `m_`, and 0 whenever the mutex is free).
- `unlock()`: a homed successor is posted and `unlock()` returns — nothing runs on the releasing thread.
  The trampoline remains for homeless successors only; a homed successor met inside it is posted and the
  loop ends.

### (c) `channel<T>`

The parked consumer is a `ParkedResumer`; `push()` and `finish_terminal()` wake it the same way. The
awaiter now holds its own `shared_ptr` to the channel state instead of a pointer to the consumer: a posted
resumption can run after the consumer that parked it has been destroyed, since its destructor cancels and
that cancel is what wakes it (round 2 finding 6; mutant m4 is an ASan use-after-free).

### (d) `block_on()`

- Creates a `HomeSlot`, drives the task under `{slot, current_holder_id()}` — a nested call (a tool
  closure inside a round) **inherits** the running task's id, a top-level call uses the thread's ambient
  id — then waits.
- Waits on the home's condition variable once a home exists, resuming posted handles on the calling
  thread; if the task never parked through a homing awaiter it falls back to the original yield loop
  (the only way to wait on a foreign awaitable that signals through the flag).
- Completion signal: the home is copied **before** the flag is set, because once the flag is observed the
  calling thread may return and destroy the slot.
- Gains a `task<void>` overload.

### (e) The drivers

- **`ThreadPool::run_job()`** → `block_on(job)` on the worker, with an explicit `done()` guard for a
  default-constructed or already-finished job. A parked job holds its worker until it completes; T4's
  shutdown semantics are unchanged.
- The five `drive()` loops, `drive_leaf_task()`, `Bundle::ask()` and `ask_stream()` → `block_on()`.
  `ask_stream()`'s driver is a `std::jthread`, and `block_on()` rethrows a round that threw: it now catches
  that, joins the relay (the producer's only other writer), and fails the stream with
  `quickstart_bundle.ask_stream_run_threw`. Uncaught, it was `std::terminate` (round 3 finding 1).
- **Raw `task<T>::resume()`** — the drive entry point of tests, examples and hand-written loops — now runs
  the task homeless under the task's own holder id, minted on first resume and kept in its promise. It
  used to inherit the calling thread's context, which gave a raw-resumed task somebody else's home (finding
  2) and let a homeless task that parked on one thread and held a lock on another make the thread it parked
  from look like the holder — the I1 false positive again, for raw drivers (round 3 finding 3).

## 4. Behaviour changes

- **A pool job that suspends completes** instead of being reported faulted (`test_rt_thread_pool` T6
  rewritten). It runs start to finish on one worker.
- **A parked job holds its worker.** A pool whose every worker is parked waiting on work still queued
  behind them deadlocks — e.g. a one-worker pool whose job waits on a channel fed by the next queued job.
  This is the cost of revision 3 over revision 2 (§5), accepted because revision 2's mirror-image
  deadlock is at least as reachable and no `rt::` caller submits jobs that wait on each other.
- **`drive_leaf_task()`** no longer returns `rt.leaf_task_contract_violation`; it waits. A leaf that parks
  on a lock held by the *same* thread now blocks forever instead of erroring — a caller-composed deadlock
  the old code "detected" by destroying a frame that could be running elsewhere. `synchronous_leaf` still
  documents that a conformer does not block its caller; it no longer guards memory safety
  (`test_rt_drive_leaf_task` D5 rewritten to a cross-thread holder).
- **`Bundle::ask()` / `ask_stream()`** wait for a contended `session_mutex_` instead of failing with
  `quickstart_bundle.ask_would_block` / `ask_stream_would_block`. A tool inside a round started through
  the raw `session()` accessor that calls `ask()` on the same `Bundle` now blocks forever where it used to
  fail fast; `~Bundle` and the next `ask_stream()` join a driver that may be waiting.
- **A pool job that awaits a lock held by the thread waiting on its future** used to be faulted at once
  (safely: the waiter was still queued) and now waits forever — e.g. a job body awaiting a
  `WorkflowSupervisor` entry point while `execute()` holds `run_mutex_` inside `future::get()`. No in-tree
  job does this; it is the same caller-composed deadlock as the queued-work case above (round 3 finding 6).
- **`is_held_by_current_thread()`** answers for the logical task, not the thread. A guard obtained through
  a top-level `block_on()` belongs to the calling thread (its ambient id), as before.
- **`ask_stream()` over a round that throws** ends its stream failed (`ask_stream_run_threw`) instead of
  closed, and a successful `ask_stream()` now actually delivers its text: the relay used to exit on the stop
  request issued the moment the run returned — usually mid-sleep, before relaying anything — so the stream
  ended closed and empty. It drains once more after the run completes (round 4 finding 4; pre-existing in
  HEAD, fixed here because this change edits that lambda).
- **A sub-task raw-resumed inside a round is no longer the round's lock holder** — a deliberate choice
  (round 4 finding 3). Every raw `task::resume()` carries the task's own holder id; inheriting the caller's
  would restore HEAD's answer for this shape but reopen the I1 false positive of round 3 finding 3 for a
  task raw-started and left running inside a driver. A visible self-deadlock beats silently skipping a lock.
  No in-tree caller does this: every raw drive loop in `examples/` is a top-level `main()`. Pinned by H17.
- **`block_on()`** spins briefly and then sleeps once its task has parked, instead of spinning with
  `yield()` until done.
- **Costs** (MSVC `/O2`, no sanitizer, `bench/rt_block_on_handoff.cpp`, HEAD → ADR-175): uncontended
  `block_on` 137 → 148 ns; uncontended lock inside `block_on` 193 → 195 ns; `co_await` lock/unlock inside
  one `block_on` 39 → 27 ns; `is_held_by_current_thread()` 2.5 → 2.0 ns; **four threads contending one lock
  564 → ~850 ns per acquisition** (7,280 ns before the spin); **capacity-1 channel with a producer thread 67
  → ~750 ns per item**; `ThreadPool` throughput unchanged. The two regressions are the price of not
  running a successor inline on the releasing thread, which is what HEAD's speed came from and what
  produced round 1's livelock and I1 false positive. `AsyncMutex` guards session rounds and quota debits,
  whose own costs are milliseconds to seconds.

## 5. Rejected alternatives

- **Revision 1 — pool driver only; a parked job continues on whichever thread wakes it.** Opens round 1's
  livelock and I1 false positive for every job that parks (§8).
- **Revision 2 — the pool is a home too; a parked job's resumption is posted to the pool queue.** Round 2
  showed a regression where today's code merely faulted: one worker, job A holds L1 and parks on L2, job B
  blocks in `block_on(L1)` — A's resumption is posted to a queue whose only worker is inside B (H10 guards
  it). Also needed a self-freeing job driver, a live-job counter and a destructor that waits on it.
- **Issue #78's option B — leak the frame, report faulted.** Reports a completed job as failed.
- **Fail closed at the suspension point** (a thread-local "no-park zone" making awaiters throw). Sound for
  the pool, but turns ordinary contention on a shared `AsyncQuota` into random transient failures in
  exactly the workflow paths where the resume-until-done loops were reachable.
- **Fix `is_held_by_current_thread()` alone.** Needs task identity, which only exists once drivers carry
  it — that is §3(a).

## 6. Residuals

- **Homeless coroutines keep inline semantics** — the trampoline, and with it round 1's livelock for a
  homeless tail that releases and then `block_on`-relocks the same mutex (round 2 finding 4, still true).
  Every `rt::`/`core::` driver is homed; raw `resume()` loops remain in tests and examples. A raw-driven
  task carries its own holder id (§3e), so it neither lends its identity to nor borrows it from the thread
  that resumes it.
- **Destroying a parked coroutine concurrently with its wake-up** is a use-after-free: the waker pops the
  record and posts a handle the destroyer frees. Before ADR-175 the window was pop-to-`resume()` inside one
  call; for a homed record it is now until the home resumes it. No in-tree code destroys a parked
  coroutine (every driver that did is replaced here); a cancel token in the record would close it.
- **`post()` and `wake()` are `noexcept`**; a queue allocation failure inside them terminates.
  `capture_parked()` may throw on the first park's home allocation, before anything is registered.
- **A parked pool job holds its worker** (§4).
- **`block_on()` inside a pool job blocks that worker** (unchanged from before).
- **Foreign awaitables.** A coroutine resumed by a bare `coroutine_handle::resume()` from inside another
  driver's context — an awaitable that hops to a pool job — runs under that driver's home and holder id
  until it next parks. If that driver is still running when the lock is granted, the coroutine resumes on
  that driver's thread; if it has returned, inline on the waker's (closed home). Either way it completes,
  but `is_held_by_current_thread()` can answer for the wrong task in that window: the same class of wrong
  answer HEAD's thread comparison gave, confined to awaitables nobody in tree writes. Carrying the home
  and holder in the awaiting coroutine's promise instead of a thread-local would close it; that needs every
  coroutine type in the chain to cooperate.
- **`channel<T>` resumes a closed-home waiter inline** (it has no trampoline to hand it to), so a chain of
  such consumers would nest stack frames. `AsyncMutex`, where a chain is actually reachable, hands them to
  its trampoline instead (§3a); a channel has one consumer.
- **Header-only statics and thread-locals are per module on Windows.** A consumer that splits the engine
  across DLLs gets separate holder counters and execution contexts per DLL; HEAD's `std::thread::id` did
  not have that problem. No in-tree DLL.

## 7. Evidence

**`tests/rt/test_rt_parked_task_home.cpp`** — 20 checks, no daemon, memory-capped, self-watchdogged:

| Check | Property |
|---|---|
| H1 | issue #78's program: 19,200 contending jobs on 4 workers, none faulted, never two inside |
| H2 | a job that throws after parking reports its original exception |
| H3 | a pool job parked on `next_async()` completes with the value, on the worker it parked on |
| H4 | a job's parameters are destroyed before `future::get()` returns (destructor sleeps 50 ms) |
| H5 | round 1 finding 2: release then `block_on` relock inside a woken job completes |
| H6 | round 2 finding 1: after a hand-off, the releasing task is never the holder — 300 attempts |
| H7 | round 1 finding 3 / round 2 finding 2: a homeless holder resumed on a worker does not make an unrelated job there the holder of either lock it takes |
| H8 | ADR-123's reentrant case: a nested `block_on` inside a round is the holder; a top-level guard belongs to the caller |
| H9 | `block_on` resumes a parked task on the calling thread, not the releasing thread |
| H10 | round 2 finding 3: one worker, parked holder of L1 plus a job blocking on L1 — completes |
| H11 | round 2 finding 6: awaiter woken by its consumer's destruction does not reach through it |
| H12 | `block_on` threads and pool jobs on one mutex: count exact, never two inside |
| H13 | round 3 finding 2: a task raw-resumed inside a pool job / a `block_on` that then returns completes when the lock is granted later, and the lock is free afterwards (two variants) |
| H14 | round 3 finding 2: a task that hops to a pool job by bare handle resume and then parks still completes |
| H15 | round 3 finding 3: the thread a homeless round parked from is not its holder while the round holds the lock elsewhere |
| H16 | round 4 findings 1-2: 5,000 bare-resumed waiters granted into closed homes complete on one 1 MiB stack, and such a waiter holds the lock under a fresh id |
| H17 | round 4 finding 3, pinned: a sub-task raw-resumed inside a round is not the round's holder |

**`tests/core/context/test_session_builder.cpp` B30-B31** — round 3 finding 1: `ask_stream()` over a provider that
throws ends its stream failed, and the process survives. Round 4 finding 4: five consecutive `ask_stream()`
calls each deliver the full reply text.

**Control** — the same test built against HEAD's headers (a separate include directory; plus a
`task<void>` `block_on` shim H12's harness needs, which HEAD lacks): H1 **ASan heap-use-after-free**; H2,
H3, H4, H5, H6, H7, H9, H10 fail; H12 fails, count 14,999 of 15,000. **Corrected (round 3 finding 4):** an
earlier draft said that shortfall meant HEAD granted the mutex to two tasks at once. It does not — "most
inside at once" stayed 1 in every run; the missing increments are jobs HEAD faulted and dropped (the
harness ignores `JobOutcome`), and without ASan HEAD crashed in 5 of 6 H12 runs. H8, H11, H13, H14 and
H15 pass at HEAD by design: they guard regressions found in intermediate designs and the first
implementation, not HEAD bugs.

**Planted mutants**, each built from the implemented headers with one change, each run under the memory
cap and an external watchdog:

| Mutant | Caught by |
|---|---|
| m1 owner recorded at `await_resume` instead of at grant | H6: 86 of 300 attempts claimed, **two tasks inside at once** |
| m2 homeless inline resume clears the holder id | H7: Q reported held by the unrelated job |
| m3 homed waiter resumed inline instead of posted | H9 |
| m3b revision 1's trampoline for homed waiters | H5: hangs, killed by the watchdog |
| m4 awaiter reaches state through the consumer | H11: ASan heap-use-after-free |
| m5 nested `block_on` mints a new holder id | H8, both checks |
| m6 a finished home never closes | H14: hangs, killed by the watchdog |
| m7 raw `task::resume()` inherits the thread's context | H15: the origin thread reported as holder (top level and inside `block_on`) |
| m6 + m7 together (the first implementation) | H13: both variants fail |
| m8 completion flag set before the home is copied | **not caught** — the use-after-free is a read of the returned `block_on`'s stack slot, which MSVC ASan does not detect without stack-use-after-return instrumentation; the ordering is argued in `block_on.hpp`, not proven by a test |
| B30's fix removed (no try/catch in `ask_stream()`'s driver) | B30 fails, and the process hangs in the CRT's abort path until killed |
| m9 a closed home resumes the waiter inline again (round 4 finding 1 restored) | H16: aborts before printing a check — the stack overflow |
| m10 a closed-home grant keeps the adopted holder id | H16: the adopting thread reported as holder |
| B31's fix removed (relay exits on the stop request) | B31 fails: the stream ends closed and empty |

Two first-draft checks did **not** catch their mutant and were strengthened, not argued away: H6 (one
attempt — the successor's thread usually woke first; now 300) and H7 (asked about the first lock, whose
owner is set correctly either way; now also the second). H13 alone does not catch m6 or m7 — each fix
masks the other's absence — which is why H14 and H15 exist.

**Sanitizer matrix** for the new test: MSVC `/O2` ASan 3/3; clang-cl `/Od` ASan 2/2 and `/O2` ASan 2/2
(`/Od` is where round 1's fatal finding hid).

**Linux, g++-14** (`-O1`): ASan clean on `test_rt_parked_task_home`, `test_rt_thread_pool`,
`test_rt_async_mutex`, `test_rt_channel`, `test_rt_drive_leaf_task`; TSan clean on all five. TSan's first
run reported three data races — all in H7's own harness, which polled `homeless.done()` on the main thread
while the coroutine could still be finishing on another; fixed by joining the thread that finishes it
(`ExternalHolder::join()`), then TSan clean 3/3.

**Suites**: Windows full suite **365/365**. Linux full suite **237/240** — the three failures are the live
containerd tests (`test_containerd_execution_surface`, `test_containerd_isolation`,
`test_composed_containerd_providers_live`), refused `/run/containerd/containerd.sock` (root-only), which
CI excludes for the same reason; unrelated to this change.

## 8. Red-team rounds

**Round 1 (against revision 1).** Fatal: the self-freeing job driver started eagerly
(`initial_suspend` → `suspend_never`) is used after free under clang-cl `/Od` (3/3), invisible to MSVC,
clang-cl `/O1`/`/O2` and g++ ASan/TSan — and to CI's sanitizer legs. Must-fix: `block_on` relock livelock
(the revised pool hangs where HEAD faulted); owner-thread false positive letting `fork_from` skip
`session_mutex_` (I1, demonstrated); reachability premise wrong — no production path to #78, while the
resume-until-done loops were the reachable hazard (argued). Should-fix: CPython thread affinity and
borrowed threads; an executor-aware alternative rejected on a false premise; proof-plan gaps. Nits:
`drive_leaf_task` has #78's shape; borrowed threads exceed the worker budget. Cleared: destroying the job
frame from the driver after symmetric transfer; #78 against the prototype on three compilers; the
live-job counter lifetime and `submit()` rollback; parameter-destruction order; the trampoline's
pending-release hand-off.

**Round 2 (against revision 2, by an executed model of the design).** Must-fix: stale owner after a
hand-off to a posted successor (I1, demonstrated: two holders); the holder-0 thread-id fallback bringing
round 1's false positive back (demonstrated); posting to a pool home deadlocks when its worker is blocked
in `block_on` — a regression where HEAD faulted (demonstrated). Should-fix: homeless-trampoline livelock
survives (demonstrated, §6); §4 understated `~ThreadPool`, `Bundle` and `drive_leaf_task` changes; channel
awaiter through a destroyed consumer; destroy-during-wake window unbounded. Nits: uncontended `block_on`
3.7× slower with an eager home (measured); `block_on` lacked `task<void>`. Its proposed simplification —
`block_on` as the only home, the pool driving jobs through it — is revision 3. Cleared: holder-id
inheritance on homed paths; a mixed homed/homeless trampoline under load (36,000 acquisitions, 3 runs);
nested `block_on`.

**Round 3 (against the implementation, probes against both the working tree and HEAD's headers).**
Must-fix, all demonstrated and all fixed (§3): (1) `ask_stream()`'s driver `std::jthread` let `block_on()`'s
rethrow of a throwing round escape — `std::terminate` (`0xC0000409`), where HEAD survived; (2) a coroutine
raw-resumed inside a pool job or `block_on()`, or hopped onto a pool job by a bare handle resume, adopted
that driver's home and, once it returned, was granted the lock into a queue nobody drained — the mutex
held forever, or the outer `block_on()` spinning at 100% CPU, where HEAD completed; (3) a homeless round
that parked on thread X and held the lock on thread Y left X reported as holder — the `fork_from` mirror
skipped the lock and two tasks were inside at once, where HEAD said no; (4) this ADR's claim that HEAD's H12
shortfall meant two holders was false (§7, corrected). Should-fix: a 13.5× contended-acquisition regression
and 13× channel regression with no bench (spin added, `bench/rt_block_on_handoff.cpp` added, remaining cost
disclosed in §4); `is_held_by_current_thread()` 6× slower and taking a mutex in a `noexcept` function (now
lock-free, faster than HEAD); a job awaiting a lock held by the thread waiting on its future now hangs
where it faulted (§4). Nits: stale comments in `agent_workflow_executor.hpp` and `resume_home.hpp` (fixed);
per-DLL thread-locals (§6); no test for findings 1-3 or the signal ordering (B30, H13-H15 added; the
signal ordering remains untested, §7). Cleared: TSan on every H-check; `signal_done` versus
`capture_parked` ordering under a foreign awaitable; wake ordering; `capture_parked` throwing; the mixed
homed/homeless trampoline; holder-id inheritance on homed paths including thread-id reuse; no new hang in
`WorkflowSupervisor` parallel nodes on a shared `AsyncQuota`, nested sub-workflows, `SpawnPump`,
`cli_chat`'s one-worker pool, or `drive_leaf_task`'s callers.

**Round 4 (against round 3's fixes).** No fatal findings. Should-fix, all demonstrated and all fixed:
(1) a post to a closed home resumed inline with no trampoline, so 3,000 chained waiters overflowed a 1 MiB
stack (now handed to `AsyncMutex`'s trampoline; H16 covers 5,000); (2) such a waiter kept the holder id of
the `block_on()` it had adopted for its whole critical section, so that call's thread was reported as
holder — the I1 false positive again, for foreign awaitables (now a fresh id at grant). Must-fix but
**pre-existing in HEAD**: (3) `ask_stream()` relayed no text at all with a fast provider (`text=""` 5/5),
because the relay exited on the stop request issued as soon as the run returned; it now drains once more
after completion (B31). Disclosed rather than changed: (4) a sub-task raw-resumed inside a round no longer
inherits the round's holder id — deliberate, §4, pinned by H17. Also fixed: ADR text that had drifted from
the code. Cleared by execution: no lost post or stuck `ready_` hint in the spin/wait path; a home cannot
close while its own chain's resumption is outstanding; reentrancy into `unlock()` from an inline
closed-home resume; the lock-free `owner_` read against a `Guard` handed to another task or thread; a mixed
stress load of 44,000 acquisitions across `block_on` threads, homeless raw-resumed tasks and pool jobs with
an observer polling `is_held` (exact count, never two inside, observer never true) under MSVC `/O2` ASan
and clang-cl `/Od` ASan; and ADR-123's reentrant `fork_from` still holding.
