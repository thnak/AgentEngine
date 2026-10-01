# ADR-219 — Who runs a woken AgentEngine coroutine when a host's own executor drives it?

- **Status**: **Proposed** (2026-10-01). Implemented, one adversarial self red-team pass (§8) whose
  must-fix findings are fixed and proven; awaiting Judge.
- **Date**: 2026-10-01
- **Closes**: GitHub issue #79 (`AsyncMutex` resumes waiters inline on the releasing thread; no executor
  hand-off when AgentEngine is hosted inside another runtime).
- **Extends** ADR-175 (a parked coroutine is resumed by the driver it belongs to). Reopens nothing in it.
- **Files**: `include/agentengine/rt/resume_home.hpp` (`Resumer`, `ParkedContinuation`, `ScopedResumer`,
  `detail::ResumerTicket`); `rt/task.hpp` (raw `resume()` keeps a host resumer); `rt/async_mutex.hpp` and
  `rt/channel.hpp` (awaiter destructors, comments); `tests/rt/test_rt_host_resumer.cpp` (new) and
  `tests/rt/CMakeLists.txt`; `bench/rt_block_on_handoff.cpp` (two new lines); `020-Configuration-and-Hosting.md`
  §3a and `001-Execution-Model.md` §8 (the host rule); `027-Vocabulary-and-Naming.md` §4 (three names).

---

## 1. The question

Issue #79 was filed while proving that QuarkCpp and `agentengine::rt` can share one process (AeroCoWorker
ADR-001): a thread parks in `co_await m.lock()`, the holder's `Guard` is moved into a Quark actor and dropped
there, and the waiter's continuation runs **on the Quark worker lane**, where its `quark::block_on()` is
refused. The issue proposes a per-`lock()` hook, `lock(Resumer)` with `Resumer = void(*)(coroutine_handle<>,
void*)`, defaulting to inline resume.

**Checked against `main` first** (6c584b7). ADR-175 (issue #78, after #79 was filed) already changed half of
this: a waiter driven by `block_on()` is now *posted back to the thread blocked in that `block_on()`*, never
run on the releaser. What it left is every coroutine **not** driven by `block_on()` — a raw `task<T>::resume()`,
or an `rt::task` `co_await`ed from a host's own coroutine type. Those park **homeless** and are resumed
inline by the waker (`resume_home.hpp`'s `detail::wake()`, homeless branch). That is exactly how a host
runtime drives AgentEngine without blocking its own lanes (it cannot use `block_on()` on a lane: that is
the very refusal the issue hit). Reproduced as `R1c` and `R2c` below: without a hook, the critical section
runs on the releasing thread, and a channel consumer on the producer's thread.

So the question: **how does a host that drives AgentEngine coroutines from its own executor get their
continuations back, without changing anything for callers that do not ask?**

### Every `rt` awaitable that resumes on a completer's thread

`await_suspend` appears in exactly five places under `include/` and `src/`:

| Awaitable | Resumes a parked coroutine on the completer's thread? | Here |
|---|---|---|
| `AsyncMutex::LockAwaiter` | yes, homeless waiters, in `unlock()`'s trampoline | **fixed** (same `detail::wake()`) |
| `channel_consumer::next_awaiter` | yes, homeless consumers, in `push()`/`close()`/`fail()`/`cancel()` | **fixed** (same `detail::wake()`) |
| `AsyncQuota` (`rt/async_quota.hpp`) | only through its `AsyncMutex` | inherits the fix |
| `core/stream.hpp` `stream<T>` | poll-only wrapper over `channel<T>`; no awaiter of its own | inherits the fix |
| `task<T>` `await_suspend` / `FinalAwaiter` | symmetric transfer to the *awaiting* coroutine on the thread that finished the child — same chain, same driver | **left**: not a completer from another context. A child finishes on a foreign thread only if it was itself resumed there by one of the two rows above, which this ADR fixes |
| `block_on`'s `SignalTask` final awaiter | sets a flag; resumes nothing | n/a |
| `ThreadPool::submit` | returns `std::future`, no awaiter | n/a |

## 2. Decision

**A host executor is a third kind of home. A host opens an `rt::ScopedResumer` around each drive of an
AgentEngine coroutine; a coroutine that parks inside the scope records the host's `rt::Resumer`, and when it
is woken the waker calls `Resumer::post()` with a move-only `rt::ParkedContinuation` instead of resuming it.
Outside a scope nothing changes.**

## 3. The mechanism

- **`rt::Resumer`** — an interface with one virtual, `post(ParkedContinuation)`. Held by `shared_ptr`
  everywhere it is recorded. Its contract (on the class) — called on the waker's thread with the waking
  primitive's lock released; should only enqueue; must not block on its own executor (the waker may *be*
  that executor); takes ownership of the continuation.
- **`rt::ParkedContinuation`** — `{handle, holder id, ticket}`, move-only. `resume()` runs the coroutine on
  the calling thread under the holder id it parked with and **with the same resumer in context**, so a
  coroutine that parks again goes back to the same resumer (R4). Resumed **at most once by construction**:
  `resume()` empties it; destroying an un-resumed one resumes it inline on the destroying thread (R6), so a
  dropped continuation — host shutdown, full queue, `post()` throwing (R7) — is never lost; and if its
  coroutine frame was destroyed while it waited, it does nothing (§8 finding 1).
- **`rt::ScopedResumer(resumer[, holder])`** — sets the thread's execution context (ADR-175's thread-local)
  to `{no block_on slot, holder, resumer}` for its scope. The holder defaults to a **fresh** id (§8 finding 3).
- **`detail::ResumerTicket`** — one allocation per park inside a scope, shared by the parked record, the
  awaiter and the continuation: the owning `shared_ptr<Resumer>` plus the claim of §8 finding 1. One pointer
  in `ParkedResumer` instead of two (§6, §8 finding 6).
- **`capture_parked()`** — a `block_on()` slot still wins (a `block_on()` inside a scope owns what parks
  under it: R8); else a non-null resumer in context records a ticket; else homeless as before.
- **`detail::wake()`** — homed → posted to its home (unchanged); ticket → `hand_to_resumer()`; else inline
  (unchanged). `hand_to_resumer()` keeps the `Resumer` alive across `post()` and swallows an exception from
  it (the continuation's destructor has already run it).
- **`task<T>::resume()`** — ADR-175 made a raw resume run homeless, clearing the context so a task
  raw-resumed inside a pool job could not adopt a `block_on()` home that closes. It now keeps a host
  resumer (a resumer never closes) while still clearing the home (§8 finding 4).
- **`AsyncMutex`** — no new state. A ticket record is returned by `grant_next_locked()` and handed over by
  `detail::wake()` **inside `unlock()`'s trampoline, outside `m_`** (§8 finding 2). A resumer that runs the
  continuation inline in `post()`, or drops it, re-enters `unlock()` as a pending release, not a nested drain:
  5,000 chained waiters on one default stack (R10).

### Why per-drive, not per-mutex or per-`lock()` (the issue's two options)

- **Per-`lock()`**: the locks that matter are taken *inside* the engine — `AgentSession`'s `session_mutex_`,
  `AsyncQuota`'s mutex, `WorkflowSupervisor`'s `run_mutex_`. A host cannot reach those `lock()` calls to pass
  a resumer, so the hook would cover only locks the host takes itself, which it could already avoid.
- **Per-mutex**: the mutex is shared by waiters with different drivers — a host lane's coroutine and an
  engine `ThreadPool` job's `block_on()` wait on the same `session_mutex_`. A per-mutex hook would send the
  `block_on()` waiter to the host's executor while its own thread sits blocked waiting for it (ADR-175
  round 2's mirror-image deadlock), and two hosts' waiters on one mutex need different executors.
- **Per-drive** is ADR-175's own rule — *a parked coroutine is resumed by the driver it belongs to* — with the
  host executor as one more driver. It needs no change to any engine call site.

### Why an interface and a move-only continuation, not `void(*)(coroutine_handle<>, void*)`

A raw function pointer and `void*` give no lifetime (the context can be freed while a waiter that recorded
it is still parked), no exactly-once (a host that drops the handle leaves a granted lock held forever; one
that resumes it twice double-resumes a frame), no way to carry the holder id that ADR-175's I1 ownership
needs, and no way to re-establish the resumer on resumption (R4). A host that wants a function pointer
writes a three-line `Resumer` around it.

## 4. Behaviour changes

- **None outside a `ScopedResumer`.** R11, R1c, R2c, and the existing suites (§7) pin that.
- Inside one: woken lock waiters and channel consumers go to `post()`; a coroutine the host destroys while its
  continuation is queued releases the lock it was granted (R12) and its channel item stays queued (R13).
- `task<T>::resume()` keeps a host resumer in context (it still clears a `block_on()` home and still runs under
  the task's own holder id).

## 5. Rejected alternatives

- **Per-`lock()` hook, per-mutex hook, raw function pointer** — §3.
- **Make `CallerHome` a subclass of one virtual "home" interface and the resumer another subclass.** Puts a
  virtual call on `block_on()`'s hand-off hot path and mixes a home that *closes* (its `block_on()` returns)
  with one that never does; the closed-home trampoline path (ADR-175 round 4) depends on that distinction.
- **Carry the resumer in the coroutine's promise instead of the thread's context.** Survives hops through
  foreign awaitables (§6), but needs every coroutine type in the chain — the host's own included — to
  cooperate; the same reason ADR-175 §6 left it.
- **Leak a dropped continuation (lock held) instead of resuming it on the dropping thread.** A silent
  deadlock of every later waiter; running late on an unexpected thread is the lesser failure, and a host
  that wants neither keeps its continuations.

## 6. Residuals and costs

- **Cost when unused** (MSVC 14.51 `/O2`, `bench/rt_block_on_handoff.cpp`; HEAD = 6c584b7's headers built
  with `/D AE_BENCH_PRE_ADR219`; reps 1-2 of three alternating HEAD/new runs, plus two earlier alternating
  runs; this machine was shared with other agents' builds, so single numbers are noisy): homeless hand-off
  chain (200,000 raw-started waiters drained by one release) **HEAD 86-107 ns (median ~93) → 98-114 ns
  (median ~102)** per hand-off, about +10%: the record grew by one `shared_ptr`, and raw `resume()` reads one
  more thread-local field. Everything else is at or inside run-to-run noise: `block_on(trivial)` 130-148 →
  116-122 ns; `block_on(lock)` 160-168 → 152-162 ns (HEAD's outliers to 312 excluded); in-`block_on`
  lock/unlock 26.8-28.4 → 27.3-28.0 ns; `is_held_by_current_thread()` 1.52-1.80 → 1.51-1.67 ns; four-thread
  contention 518-621 → 560-673 ns; channel cap 1 649-743 → 573-975 ns (that line spans 450-950 ns across
  runs of the *same* binary). The first implementation (two `shared_ptr`s in the record) measured +20-25% on
  the homeless chain; merging them into one ticket (§8 finding 6) is what brought it down.
- **Cost when used**: resumer chain with an inline `post()` ~170-190 ns per hand-off (one ticket allocation
  per park, plus the virtual call); with a `std::thread` executor ~310-390 ns (the thread hop).
- **Foreign-awaitable hops drop the resumer.** The resumer lives in the thread's execution context, as
  ADR-175's home does. A host coroutine resumed by its own runtime (after `co_await`ing a host awaitable)
  runs with whatever context that thread has — none — and its next AgentEngine park is homeless again. A host
  executor closes this by opening the scope around *every* resumption it performs, passing a holder id it
  keeps per host task (`ScopedResumer(r, holder)`, `rt::mint_holder_id()`), so the task keeps one identity
  across resumptions. Carrying it in the promise would close it in the engine (§5).
- **A scope opened inside an engine round** (a tool closure) runs under a fresh holder by default, so a
  reentrant `AgentSession::fork_from()` inside it no longer sees itself as the round's holder and waits on
  `session_mutex_` — a visible self-deadlock, not an I1 violation. Pass `current_holder_id()` to inherit.
- **Destroying a coroutine concurrently with `ParkedContinuation::resume()` running it** is undefined, as it
  is for any coroutine resumed on one thread and destroyed on another. Destroying it *before* the resumer
  gets to it is safe (§8 finding 1).
- **A continuation dropped at host shutdown after the engine objects it refers to are gone** resumes into
  freed state. The host must drain or drop its queue before destroying the sessions, mutexes and channels its
  parked coroutines wait on — the order it needs for any pending work. Stated on `Resumer`.
- **`post()` runs inside `AsyncMutex::unlock()`'s trampoline on the waker's thread**: a slow `post()` delays
  the waker (not other waiters — the lock is already granted). The contract says enqueue only.
- **Homed (`block_on`) and homeless waiters keep ADR-175 §6's destroy-during-wake residual** — the claim is
  allocated only for resumer-driven parks, which are the ones that can wait indefinitely in a host queue.
- **Per-DLL thread-locals** (ADR-175 §6) apply equally: a `ScopedResumer` opened in a module with its own copy
  of the header-only thread-local does not reach awaiters compiled into another module.
- **Not run**: TSan and Linux (WSL was not reachable from this session); clang-cl. MSVC ASan `/O2` and `/Od`
  were run (§7).

## 7. Evidence

**`tests/rt/test_rt_host_resumer.cpp`** — 32 checks, no daemon, memory-capped, self-watchdogged. The fake
host executor is a plain `std::thread` draining a queue of `ParkedContinuation`s (no QuarkCpp).

| Check | Property |
|---|---|
| R1 | lock waiter started in a scope runs its critical section on the executor's thread, not the releaser's; one post; it is the holder there |
| R1c | **positive control**: same program without the scope runs it on the releasing thread (issue #79) |
| R2 | channel consumer in a scope resumes on the executor's thread after a producer thread pushes |
| R2c | **positive control**: without the scope, on the producer's thread |
| R3 | `close()`'s terminal wake-up is handed to the resumer too |
| R4 | a resumed coroutine that parks again is handed back to the same resumer (two posts) |
| R5 | a host coroutine type awaiting an `rt::task` in a scope: the lock continuation and the host code after `co_await` run on the executor; while queued, the scope's opener is not the holder; inside, the coroutine is |
| R6 | a resumer that drops every continuation: resumed inline by the drop; the lock is released |
| R7 | a resumer whose `post()` throws: waker survives, continuation still runs, lock released |
| R8 | `block_on()` inside a scope still owns its task (resumed on its own thread, nothing posted) |
| R9 | 2,000 waiters in one scope: exact count, never two inside, all on the executor |
| R10 | a resumer that runs continuations inline in `post()`: 5,000 chained waiters complete on a default stack |
| R11 | outside a scope: nothing posted, releaser runs the waiter as before |
| R12 | §8 finding 1: destroying a waiter whose lock grant is queued releases the lock; the queued continuation does nothing |
| R13 | §8 finding 1, channel: never resumed; the item stays in the channel |

Results: 32/32 under MSVC `/O2`, MSVC `/O2 /fsanitize=address`, MSVC `/Od /fsanitize=address`, and the CMake
Debug build (ctest; 20/20 with `--repeat until-fail:20`). Existing suites with the change (CMake Debug,
Windows): every `rt`-labelled test, **67/67** (including ADR-175's `test_rt_parked_task_home`, the
`AsyncMutex`, channel, block_on, thread-pool, agent-session and agent-spawn suites), plus
`test_rt_workflow_supervisor`, `_request_port`, `_patterns`, `_scheduling_shuffle`, `_merge_on_join` and
`test_mandatory_sandbox_provider`, **6/6**. The full suite was not run (this session built only the targets
above).

**Planted mutants**, each built from the implemented headers with one change (`/O2`; m8-m10 under `/Od` ASan):

| Mutant | Caught by |
|---|---|
| m1 `capture_parked()` ignores the resumer | R1-R5 fail, then R6 hangs (timeout) |
| m2 raw `task::resume()` clears the resumer (ADR-175's original line) | R1-R4, R6, R7 fail, then hang |
| m3 `ScopedResumer` inherits `current_holder_id()` | R5: the scope's opener reported as holder (I1 false positive) |
| m4 a dropped continuation is leaked | R6 fails, then the lock is held forever (timeout) |
| m5 `ParkedContinuation::resume()` does not re-establish the resumer | R4: the second park runs on the producer's thread |
| m6 `AsyncMutex` hands a ticket record over outside the trampoline | R10: `0xC00000FD` stack overflow |
| m7 the waker does not catch `post()`'s exception | R7: `std::terminate` (exit 3) |
| m8 the lock awaiter's destructor does not claim a woken continuation | R12 fails (lock never released) and ASan heap-use-after-free |
| m9 `resume()` ignores the claim | R12: ASan heap-use-after-free |
| m10 the channel awaiter's destructor does not claim | R13: ASan heap-use-after-free |

## 8. Self red-team (one adversarial pass against the first implementation)

1. **Must-fix, fixed — a host cancelling a parked coroutine held a lock forever and resumed a freed frame.**
   ADR-017's "drop the handle = cancel" is the house idiom, and a host runtime cancels the same way. The first
   implementation kept ADR-175's destructor: remove the registration if still queued, else nothing. But a
   resumer-driven waiter, once granted, can sit in a host queue indefinitely (ADR-175's homed window is bounded
   by one `block_on()`); destroying it there left the mutex owned by a dead task and the queued continuation
   resuming freed memory. Fix: the per-park ticket carries a claim; the awaiter's destructor, finding its record
   already popped, claims the continuation and (for `AsyncMutex`) releases the grant; `resume()` claims first
   or does nothing. R12/R13; m8, m9, m10.
2. **Must-fix, fixed at design time — where the hand-off runs.** The obvious place mirrors `CallerHome::try_post()`
   inside `grant_next_locked()`, i.e. **under `m_`**: host code under an engine lock, and a host that resumes
   inline would re-enter `unlock()` and self-deadlock on `m_`. The next-obvious place — after releasing `m_` but
   outside the trampoline — nests each inline-resumed waiter in the previous one's `unlock()` (ADR-175 round 4's
   overflow). It runs inside the trampoline instead. R10; m6 overflows the stack.
3. **Should-fix, fixed — I1 false positive through the scope's holder.** A scope that inherited the opening
   thread's `current_holder_id()` would let a coroutine that parked inside it and later holds the lock on the
   executor make the *opening* thread report `is_held_by_current_thread()` true — ADR-175 round 3 finding 3's
   shape, the one that lets `fork_from()` skip `session_mutex_`. Fresh id by default. R5; m3.
4. **Should-fix, fixed — the hook was dead on the main host path.** ADR-175's raw `task::resume()` replaces the
   whole execution context, which erased the scope: a host driving with `t.start()` inside a scope got
   today's inline behaviour silently. It now keeps a resumer (never a home). R1-R4; m2.
5. **Should-fix, fixed — the contract overclaimed.** The first text said `post()` runs "with no AgentEngine lock
   held"; the waker can hold its own locks (any engine code that drops a `Guard` inside a `std::lock_guard`
   scope). Now: the waking primitive's lock is released. Also added: `post()` must not block on its own
   executor, since the waker may be that executor.
6. **Should-fix, fixed — cost when unused.** The first implementation added two `shared_ptr`s (resumer, claim)
   to every parked record and measured +20-25% on the homeless chain. Merged into one ticket: about +10% (§6).
7. **Residual, disclosed — foreign-awaitable hops** (§6), with the per-resumption-scope guidance.
8. **Residual, disclosed — a scope inside an engine round** (§6).
9. **Cleared**: double resume (move-only, `resume()` empties; concurrent `resume()` and drop race only on one
   owner, which the type forbids); resumer lifetime (every record and continuation owns it; `hand_to_resumer()`
   holds a reference across `post()` because `post()` may drop the last continuation — and with it the ticket —
   before it returns); `capture_parked()` throwing on the ticket allocation (before registration, as ADR-175's
   home allocation); a `post()` that resumes inline inside the trampoline; a `block_on()` nested inside a scope;
   a re-park after resumption getting a fresh ticket of its own; `await_suspend()`'s re-check path (no ticket
   is stored unless the coroutine actually parks); `is_held_by_current_thread()` on the executor for the
   resumed coroutine (R1, R5); I2/I3 — no capability or authority flows through any of this: the `Resumer` is
   host code the host opts into (ADR-070's seam shape: opt-in, unset = today's behaviour, never model output),
   and it only chooses *where* an already-running task continues.
