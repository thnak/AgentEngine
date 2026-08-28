#pragma once
// Implements ADR-102 Phase 4 (identity-native sandbox/worktree design) -- `agentengine::rt::
// block_on<T>()`, a synchronous driver for `agentengine::rt::task<T>` that is CORRECT under genuine
// cross-thread `AsyncMutex` contention, unlike the naive "while (!t.done()) t.resume();" loop this
// codebase otherwise uses everywhere (`rt/agent_workflow_executor.hpp`'s own `agent_executor_detail::
// drive()`, every `tests/test_rt_agent_session*.cpp` file's local `drive<T>()`, and this file's own
// first version of `mandatory_sandbox_provider.hpp`'s local driver).
//
// WHY THIS FILE EXISTS, A REAL FINDING NOT ANTICIPATED AT DESIGN TIME: an independent red-team pass
// on `sandbox/mandatory_sandbox_provider.hpp` (2026-08-28) found, and EMPIRICALLY PROVED with a
// targeted repro against the real, unmodified `AsyncMutex`/`task<T>`, that the naive drive loop that
// file's first version used is NOT safe in this specific composition, despite that version's own
// "I1 (one session, one executor) plus invoke_tool()'s sequential dispatch means nothing here ever
// contends" argument. The argument's flaw: `AsyncQuota<RunCost>`/`AsyncQuota<BranchCost>`/
// `AsyncQuota<StorageBytes>` are stored as raw pointers by `MandatorySandboxProvider::bind_sandbox()`
// and are LEGITIMATELY SHARED across multiple independent `SandboxRuntime` instances (an ordinary "one
// budget for a whole family of sibling sessions" pattern -- this phase's own test binds one quota
// triple across six separate sessions). If two sibling sessions' round loops ever run on genuinely
// different OS threads concurrently (the entire reason `AgentSession::session_mutex_` exists
// per-session in the first place), their two `RunCommandTool` closures' calls into the SAME shared
// `AsyncQuota` genuinely CONTEND on that quota's own internal `AsyncMutex` -- no two
// `MandatorySandboxProvider`/`SandboxRuntime` instances need to be touched concurrently for this to
// fire, only the shared quota. The naive loop's failure mode under that contention, confirmed by the
// red-team's own repro (5/5 runs): the busy-loop's SECOND `resume()` call on an awaiter that has
// already genuinely suspended (`LockAwaiter::await_suspend()` returned `true`, registering the handle
// in `waiters_`) does not "wait" -- it directly runs `await_resume()`, handing back a `Guard` as if the
// lock were acquired even though it is not, defeating `AsyncMutex`'s mutual-exclusion guarantee
// outright and leaving a STALE handle in `waiters_` that a later, real `unlock()` resumes against an
// ALREADY-DESTROYED coroutine frame -- a genuine use-after-free, not a theoretical one.
//
// Ported from docs/planning/proofs/common/block_on.hpp (ADR-099's own standalone original -- kept
// as-is, this is a new file). That file's own header records the SAME hazard class being found,
// ASan-confirmed (17-25 crashes out of 30 runs under real thread contention), and fixed there first --
// this port carries that fix into production, verbatim in mechanism, rather than re-deriving it. Real
// change made during the port: `namespace probe` -> `namespace agentengine::rt` (this file's own real
// home; `agentengine::rt::task<T>` was already used unqualified by the original, no other type
// translation needed). NOT ported: `try_block_on_bounded()` (the original's own bounded variant, used
// only by a deadlock-DEMONSTRATION probe with no real caller in this phase) -- named here as
// deliberately out of scope, not silently dropped.
//
// THE MECHANISM, briefly: a dedicated, LOCAL coroutine type (`detail::SignalTask<T>`) whose
// `final_suspend()` awaiter performs the cross-thread completion signal as the LAST action ever taken
// on its own frame -- strictly after every local (the driven `task<T>`, its `co_await`ed result) has
// already been destroyed. `agentengine::rt::task<T>`'s own `FinalAwaiter` is deliberately NOT reused
// for this driver (it has a different job -- resuming a `continuation_`, or none) -- signaling as an
// ordinary body statement BEFORE reaching final_suspend is exactly the bug this file exists to avoid.

#include <atomic>
#include <coroutine>
#include <optional>
#include <thread>
#include <utility>

#include "agentengine/rt/task.hpp"

namespace agentengine::rt {

template <class T>
class BlockOnState {
public:
    // Stores the delivered value -- safe to call from the coroutine body BEFORE final_suspend, since
    // nothing reads `value_` until `ready()` observes the flag this does NOT set.
    void set_value(T value) { value_.emplace(std::move(value)); }
    // The actual cross-thread signal -- must be called ONLY from a point that is guaranteed to be the
    // last touch on the signaling frame (see `block_on_detail::SignalTask` below).
    void signal_done() { flag_.store(true, std::memory_order_release); }
    [[nodiscard]] bool ready() const noexcept { return flag_.load(std::memory_order_acquire); }
    [[nodiscard]] T take() {
        while (!ready()) std::this_thread::yield();
        return std::move(*value_);
    }

private:
    std::atomic<bool> flag_{false};
    std::optional<T> value_;
};

namespace block_on_detail {

// A minimal, purpose-built coroutine type -- deliberately NOT `agentengine::rt::task<void>` -- whose
// entire reason to exist is a `final_suspend()` awaiter that performs the cross-thread completion
// signal as literally the last instruction ever executed on this frame. Never awaited by anything
// (`block_on()` drives it directly via `resume()`), so its `FinalAwaiter` never needs to resume a
// continuation -- it just signals and then leaves the frame permanently parked (suspended, not
// running anywhere) until `block_on()`'s own explicit `destroy()`, by which point the signal
// guarantees no other thread is still inside this frame.
template <class T>
struct SignalTask {
    struct promise_type {
        BlockOnState<T>* state = nullptr;

        // Matches `drive_and_signal()`'s own parameter list exactly (C++20's "promise constructor
        // parameter matching" -- the compiler passes the coroutine function's own arguments through to
        // a matching promise_type constructor, if one exists, instead of default-constructing it).
        // This is how `state` gets into the promise before the coroutine body ever runs, with no
        // separate "hook it up after the fact" step needed.
        promise_type(agentengine::rt::task<T>&, BlockOnState<T>* s) noexcept : state(s) {}

        [[nodiscard]] SignalTask get_return_object() {
            return SignalTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            [[nodiscard]] bool await_ready() noexcept { return false; }
            // Runs strictly after every local in the coroutine body (the by-value `inner` task<T>
            // parameter, the locally co_await-ed `T v`) has already been destroyed -- the true last
            // touch on this frame. Returning void (not a coroutine_handle<>) means "just suspend,
            // resume nothing else" -- the frame sits parked here, inert, until block_on()'s own later,
            // safe `destroy()` call.
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                h.promise().state->signal_done();
            }
            void await_resume() noexcept {}
        };
        [[nodiscard]] FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }  // matches the prove-phase original's own
                                                              // no-exception-handling posture
    };

    explicit SignalTask(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
    SignalTask(SignalTask const&) = delete;
    SignalTask& operator=(SignalTask const&) = delete;
    SignalTask(SignalTask&& other) noexcept : h_(std::exchange(other.h_, {})) {}
    SignalTask& operator=(SignalTask&&) = delete;
    ~SignalTask() {
        if (h_) h_.destroy();
    }

    void resume() { h_.resume(); }

    std::coroutine_handle<promise_type> h_;
};

template <class T>
SignalTask<T> drive_and_signal(agentengine::rt::task<T> inner, BlockOnState<T>* state) {
    // `state` here is the coroutine's own ordinary parameter (still directly usable in the body, same
    // as any coroutine parameter) -- used only to STORE the value, which is safe to do before
    // final_suspend since nothing reads it until the flag (set separately, see below) is observed.
    T v = co_await inner;
    state->set_value(std::move(v));
    co_return;
    // `inner` and `v` are destroyed by the compiler-generated code between here and final_suspend().
    // The actual cross-thread SIGNAL happens in `SignalTask::promise_type::FinalAwaiter::
    // await_suspend()` (via `promise_type::state`, captured by its own matching constructor above) --
    // strictly after that teardown, never here.
}

}  // namespace block_on_detail

// Blocks (busy-waits) the CALLING thread until `t` completes, wherever/whenever that completion
// actually happens (immediately on this call stack if uncontended, or later via a DIFFERENT thread's
// `AsyncMutex::unlock()` trampoline symmetric-transferring all the way through to completion). This is
// the correct, general-purpose way to synchronously drive an `agentengine::rt::task<T>` from a plain,
// non-coroutine call site when the task MAY genuinely suspend on a contended `AsyncMutex`/`AsyncQuota`
// -- see this file's own top comment for the real hazard a naive "resume until done" loop has here
// that this mechanism avoids.
template <class T>
[[nodiscard]] T block_on(agentengine::rt::task<T> t) {
    BlockOnState<T> state;
    block_on_detail::SignalTask<T> driver = block_on_detail::drive_and_signal(std::move(t), &state);
    driver.resume();
    return state.take();
}

}  // namespace agentengine::rt
