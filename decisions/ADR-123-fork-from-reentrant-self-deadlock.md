# ADR-123 — Closing `AgentSession::fork_from()`'s reentrant self-deadlock hazard

- **Status:** Proposed — implemented, verified (Windows/MSVC), full rebuild (zero errors) and full
  `ctest` clean (285/286, same pre-existing unrelated matplotlib/pandas gap), `naming_lint.py` clean.
  Not yet independently red-teamed; not yet Linux-verified.
- **Date:** 2026-08-30.
- **Scope:** `include/agentengine/rt/async_mutex.hpp` (one new field, one new public method, two small
  edits to `unlock()`, both purely additive — no behavior change for any existing caller),
  `include/agentengine/rt/agent_session.hpp` (`fork_from()`'s own locking logic, comment rewrite),
  `tests/test_rt_agent_session_fork_from_serialization.cpp` (new section [3]). `decisions/ADR-102-
  identity-native-sandbox-implementation-phase-1.md`, `decisions/ADR-114-task-branch-tools-
  promotion.md` (disclosure corrections pointing here).
- **Related specs:** `decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md` §26 point 5
  / its own Phase 5 summary (the original SHOULD-FIX disclosure this ADR closes — `fork_from()`'s own
  cross-thread serialization gap was fixed there; fixing it introduced THIS narrower, still-open
  hazard, disclosed but not fixed at the time), `tests/test_rt_block_on.cpp` (the established
  "temporarily revert, confirm the new test genuinely fails, restore" methodology this ADR reuses).

## 1. The question

`AgentSession::fork_from()` acquires `source.session_mutex_` (via `agentengine::rt::block_on()`) for
the whole copy, closing a real cross-thread serialization gap (ADR-102 Phase 5). That same red-team
pass found and disclosed, but did not fix, a narrower hazard the fix itself introduced: `AsyncMutex`
has no reentrancy check, so calling `fork_from(source, ...)` from code ALREADY running on the same OS
thread inside an in-flight `start_run()`/`resolve_interaction()` round on `source` — e.g. synchronously
from a tool closure's own body — self-deadlocks 100% of the time (`block_on()`'s own busy-wait spins
forever, since the only thing that could ever call `unlock()` is the very outer round's own Guard,
already parked one stack frame up, waiting for this call to return). Not reachable through any real
call site today (every `fork_from()` caller in this codebase is a top-level `main()`), but named as
"exactly the shape a near-future `agent.spawn`-style tool wired to call `fork_from()` directly from its
own closure would hit." Is this worth closing now, and how, given the two design options the original
disclosure itself named — broader owner-thread tracking on `AsyncMutex` (many other call sites depend
on it) versus a narrower, `fork_from()`-local reentrancy guard?

## 2. Findings

**A third option, cleaner than either originally named, turned out to exist: additive owner-thread
tracking on `AsyncMutex` itself.** The original disclosure worried that touching `AsyncMutex` would be
"a broader change to a low-level primitive several other real call sites also rely on." Reviewing the
primitive's actual structure found this concern doesn't apply to a PURELY ADDITIVE query: `AsyncMutex`
already tracks `held_` (a bool) under its own internal `std::mutex m_`; adding a second field recording
WHICH thread currently holds it, written only at the exact two places `held_` already transitions
(`LockAwaiter::await_resume()` — reached only by whichever coroutine has just become the sole,
exclusive holder, uncontended or freshly handed off via `unlock()`'s own trampoline — and `unlock()`'s
own two `held_ = false` sites), changes nothing for any of `AsyncMutex`'s many other real callers
(`task_branch_mutex_`, `SandboxRuntime::exclusivity_`, `AsyncQuota`'s own internal mutex, etc.) unless
they explicitly opt in by calling the new method. This is a smaller, more contained, more principled
fix than the `fork_from()`-local guard the original disclosure leaned toward — no new locking
discipline invented, just a diagnostic capability (an "am I the current holder" query) many mutex
designs already expose in some form.

**Why `await_resume()` specifically, not `unlock()`'s hand-off path.** `unlock()`'s own trampoline calls
`next.resume()` directly (not via a scheduler hop) when handing the mutex to the next queued waiter —
so the resumed coroutine's remaining body, including its own `await_resume()`, runs SYNCHRONOUSLY on
whichever thread is currently executing `unlock()`'s drain loop, before `next.resume()` returns. This
means `await_resume()` alone, on both the uncontended-fast-path and the resumed-after-hand-off path,
always runs on the thread that is genuinely about to hold the mutex — no separate write needed inside
`unlock()`'s hand-off branch at all, only in its two "mutex is now genuinely free" branches (clearing
the owner to a sentinel).

**The sentinel is safe by construction.** `std::thread::id{}` (default-constructed) is the standard's
own "no thread of execution" value, and no real running thread's `std::this_thread::get_id()` can ever
equal it — so `is_held_by_current_thread()` never false-positives against a genuinely unheld mutex.

**Design chosen for `fork_from()` itself, mirroring the codebase's own established idiom**: `AsyncMutex::
Guard source_guard;` (default-constructed, a genuine "holding nothing" state the type already
supports) then conditionally assigned only when `!source.session_mutex_.is_held_by_current_thread()`.
This changes NEITHER of the two already-tested behaviors (`test_rt_agent_session_fork_from_
serialization.cpp`'s own [1]/[2]): a genuinely different-thread caller still correctly blocks (the
query returns false, so the lock path is taken, exactly as before), and an ordinary top-level self-fork
still takes the uncontended fast acquire (nothing holds the mutex at that point either). It only adds a
THIRD path — the previously-deadlocking reentrant case — which now proceeds directly, safe because I1
(one session, one executor) already guarantees no OTHER thread can be touching `source` concurrently
while the CURRENT thread holds its mutex.

**A real fixture-design lesson, found the hard way, not merely anticipated**: the first attempt at the
new test's fixture pre-declared a `using ReentrantSession = AgentSession<ReentrantChatClient>;` alias
BEFORE `ReentrantChatClient`'s own definition (forward-declaring the class, matching an ordinary,
unconstrained self-referential type pattern), and used that alias for a member field type (`Reentrant
Session* self_`) and a member function parameter type inside the class body. This does NOT compile for
`AgentSession`, because it is a CONCEPT-CONSTRAINED template (`requires (ChatClient<ChatClientT> || ...)`)
— naming `AgentSession<ReentrantChatClient>` ANYWHERE in a declarative position (even just to form a
pointer TYPE, not to instantiate the class) requires evaluating `ChatClient<ReentrantChatClient>`
immediately, which needs `ReentrantChatClient` to already be complete; it isn't yet, mid-definition — a
hard compile error (`use of undefined type`), not a graceful SFINAE failure, since a `using` alias
declaration and a member function's own signature are not overload-resolution contexts. Fixed by making
`self_` a `void*` (a type that names nothing constrained) and naming `AgentSession<ReentrantChatClient>`
directly, by its full name, only inside `chat()`'s own BODY — which, per the ordinary C++ rule that an
inline member function's body is compiled as if placed immediately after the class's own closing brace,
sees `ReentrantChatClient` as complete and compiles cleanly. A real, confirmed (not merely reasoned
about) limitation of self-referential fixtures against concept-constrained templates, worth remembering
for the next one.

## 3. What was built

`include/agentengine/rt/async_mutex.hpp`: `#include <atomic>`/`#include <thread>` added; a new private
`std::atomic<std::thread::id> owner_{};` field; `LockAwaiter::await_resume()` now stores
`std::this_thread::get_id()` into it; `unlock()`'s two `held_ = false` sites now also reset it to
`std::thread::id{}`; a new public `[[nodiscard]] bool is_held_by_current_thread() const noexcept`.

`include/agentengine/rt/agent_session.hpp`: `fork_from()`'s locking logic changed from an
unconditional `block_on(acquire_session_mutex(source.session_mutex_))` to a conditional acquire guarded
by `!source.session_mutex_.is_held_by_current_thread()`. The extensive disclosure comment immediately
above it is rewritten to describe the fix (rather than merely naming the hazard) and point here.

`tests/test_rt_agent_session_fork_from_serialization.cpp`: a new `ReentrantChatClient` fixture (see §2's
own design-lesson) whose `chat()` — called from INSIDE an in-flight `start_run()` round, holding
`session_mutex_` on the calling thread — calls `fork_from()` reentrantly on the very session running
that round, forking FROM it INTO a separate, freshly-constructed target (deliberately not self-into-
self; `fork_from()`'s own comment already scopes self-into-self-while-live as an unsupported operation
this fix does not attempt to give defined semantics to — only "does it deadlock" is under test here). A
new section [3] drives this through a real `start_run()` round on a dedicated `std::thread`, polling a
bounded wait (up to ~5 seconds, matching this codebase's own "prove it, don't just reason about it"
discipline for concurrency claims) rather than an unconditional `join()` — a genuine regression here
must FAIL this check, not hang the entire test binary; on timeout the round thread is deliberately
`detach()`-ed (never joined), since the process is about to return failure anyway and the OS reclaims
even a permanently-spinning thread on process exit.

## 4. Verification

Built and ran `test_rt_agent_session_fork_from_serialization`: **ALL CHECKS PASSED**, including the new
section [3] (completed well within the 5-second bound) and both pre-existing sections [1]/[2]
unchanged. Sanity-checked the same way this design line always does: temporarily reverted `fork_from()`
back to its unconditional-lock form, rebuilt, and reran — section [3] genuinely **FAILED** (timed out,
confirming the pre-fix code really does deadlock, not merely in theory) while the process still exited
cleanly (the detach path working as designed, no hung test run blocking the suite) — then restored the
fix and reran, confirming a full pass again. `git diff --stat` on both core headers confirmed the
restored state matched the intended fix exactly, no leftover sanity-check artifacts.

Full project rebuild (foundational headers touched — `async_mutex.hpp`/`agent_session.hpp` are used
essentially everywhere): zero errors. Full `ctest`: **285/286**, the one failure being the same,
unrelated, pre-existing matplotlib/pandas gap this whole design line has repeatedly confirmed —
nothing newly broken by touching either header. `python tools/naming_lint.py`: clean, no new exported
vocabulary (the new `AsyncMutex` method is not itself an exported "concept" this lint tracks).

## 5. What was NOT done

- **No independent red-team pass yet.** This is a real, if narrow, change to the single most
  heavily-shared low-level concurrency primitive in the codebase (`AsyncMutex`) plus the single most
  heavily-used class (`AgentSession`) — a red-team round is the expected next step before this is
  considered fully closed, matching every other concurrency-adjacent change in this design line.
- **No Linux verification.**
- **The hazard remains, by design, unreachable by any real caller today** — this ADR closes the
  disclosed gap before a future caller can hit it, not in response to a live incident.
- **`is_held_by_current_thread()` was not adopted by any OTHER `AsyncMutex` call site** — this ADR's
  own scope is closing `fork_from()`'s specific hazard, not auditing every other lock acquisition in
  the codebase for a similar opportunity.

## 6. Residuals

- Everything named in §5 not otherwise closed.
- `is_held_by_current_thread()` is now a real, generically-useful capability on `AsyncMutex` — any
  FUTURE reentrant-call hazard against a different `AsyncMutex`-guarded critical section (e.g.
  `task_branch_mutex_`, `SandboxRuntime::exclusivity_`) has a ready-made, already-proven tool to close
  it with, rather than needing to re-derive this same design question from scratch.
