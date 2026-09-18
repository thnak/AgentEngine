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
//
// EXCEPTIONS: an exception escaping the driven task is captured and rethrown on the CALLING thread by
// `BlockOnState::take()`, the same contract `std::future` offers. This driver used to call
// `std::terminate()` instead -- see `promise_type::unhandled_exception()` below for why that was
// wrong and what it cost.
//
// ADR-175 (issue #78) -- THE CALLING THREAD IS THE DRIVEN TASK'S HOME. When the task parks on an
// `AsyncMutex` or `channel<T>` await, the awaiter records this call as its home (`rt/resume_home.hpp`,
// created lazily on that first park), and the waker POSTS the handle back instead of resuming it on its
// own thread; this thread, waiting in `take()`, resumes it. So the task runs only on the calling thread,
// never inside another thread's `unlock()` trampoline (where a nested `block_on` relock livelocked,
// ADR-175 round 1 finding 2). The wait sleeps on a condition variable once a home exists; a task that never
// parked through a homing awaiter falls back to the original yield loop. Every synchronous driver in `rt::`
// now goes through here -- the five resume-until-done `drive()` loops, `drive_leaf_task()`,
// `ThreadPool::run_job()` and `Bundle::ask()` each mishandled a task that suspends in its own way.
//
// HOLDER. A nested call (a tool closure inside a round that is itself being driven) is a synchronous part
// of the task already running, and keeps its holder id; `AsyncMutex::is_held_by_current_thread()` depends on
// that (`AgentSession::fork_from()`'s reentrant case, ADR-123).

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/task.hpp"

namespace agentengine::rt {

namespace block_on_detail {

// What completion signalling and the wait share, for either `T` or `void`.
class CompletionSignal {
public:
    explicit CompletionSignal(detail::HomeSlot* slot) noexcept : slot_(slot) {}

    // The last touch on the signalling frame (see `SignalTask` below). The home is copied BEFORE the flag
    // is set: once the flag is observed the calling thread may return and destroy `*slot_`.
    void signal_done() noexcept {
        std::shared_ptr<detail::CallerHome> const home = slot_->home;
        flag_.store(true, std::memory_order_release);
        if (home) home->mark_done();
    }

    // Resumes posted handles on this thread until the driven task is done. `holder` is the id this call
    // drives under; a posted record carries the same id.
    void wait(std::uint64_t holder) {
        std::shared_ptr<detail::CallerHome> const home = slot_->home;
        if (!home) {
            // Nothing parked through a homing awaiter: either already done, or suspended on a foreign
            // awaitable that resumes it elsewhere and signals through the flag.
            while (!flag_.load(std::memory_order_acquire)) std::this_thread::yield();
            return;
        }
        for (;;) {
            detail::CallerHome::Posted const next = home->wait_next();
            if (!next.handle) return;
            ScopedExecution const context(slot_, next.holder != 0 ? next.holder : holder);
            next.handle.resume();
        }
    }

private:
    detail::HomeSlot* slot_;
    std::atomic<bool> flag_{false};
};

}  // namespace block_on_detail

template <class T>
class BlockOnState {
public:
    explicit BlockOnState(detail::HomeSlot* slot) noexcept : signal_(slot) {}
    // Stores the delivered value -- safe to call from the coroutine body BEFORE final_suspend, since
    // nothing reads `value_` until the completion signal has been observed.
    void set_value(T value) { value_.emplace(std::move(value)); }
    // Stores an escaping exception under the SAME timing rule as `set_value()`, and for the same
    // reason. A coroutine whose `unhandled_exception()` returns normally still runs `final_suspend()`,
    // so recording the fault here does not cost the completion signal.
    void set_fault(std::exception_ptr fault) noexcept { fault_ = std::move(fault); }
    // The actual signal -- must be called ONLY from a point that is guaranteed to be the last touch on
    // the signaling frame (see `block_on_detail::SignalTask` below).
    void signal_done() noexcept { signal_.signal_done(); }
    [[nodiscard]] T take(std::uint64_t holder) {
        signal_.wait(holder);
        // Before `value_`, necessarily: a driven task that threw never reached `set_value()`, so
        // dereferencing the optional first would be undefined behaviour on exactly the path this
        // exists to handle.
        if (fault_) std::rethrow_exception(fault_);
        return std::move(*value_);
    }

private:
    block_on_detail::CompletionSignal signal_;
    std::optional<T> value_;
    std::exception_ptr fault_;
};

template <>
class BlockOnState<void> {
public:
    explicit BlockOnState(detail::HomeSlot* slot) noexcept : signal_(slot) {}
    void set_value() noexcept {}
    void set_fault(std::exception_ptr fault) noexcept { fault_ = std::move(fault); }
    void signal_done() noexcept { signal_.signal_done(); }
    void take(std::uint64_t holder) {
        signal_.wait(holder);
        if (fault_) std::rethrow_exception(fault_);
    }

private:
    block_on_detail::CompletionSignal signal_;
    std::exception_ptr fault_;
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
        // This was `std::terminate()`, carried over from the prove-phase original's own
        // no-exception-handling posture. That posture was wrong once this driver reached production
        // callers. `agentengine::rt::task<T>` deliberately CATCHES an exception into its promise and
        // rethrows it at the awaiting `co_await` -- which is the `co_await inner` in
        // `drive_and_signal()` below. So every throw anywhere inside any task driven through
        // `block_on()` landed here and killed the process, turning a failure a caller could have
        // handled into one nobody can. `sandbox/ustar_writer.hpp`'s own comment records a red-team
        // reproducing exactly that, via a `filesystem_error` from an unreadable subdirectory.
        //
        // The fault is now carried out to the calling thread and rethrown there by `take()`, which
        // is what a synchronous driver is supposed to do (`std::future` has the same contract). No
        // current caller drives `block_on()` from a destructor or a `noexcept` function, so nothing
        // trades a terminate here for a terminate somewhere worse; checked before making the change.
        void unhandled_exception() noexcept { state->set_fault(std::current_exception()); }
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

// `task<void>` (ADR-175: `ThreadPool::run_job()` drives its jobs through here).
inline SignalTask<void> drive_and_signal(agentengine::rt::task<void> inner, BlockOnState<void>* state) {
    co_await inner;
    state->set_value();
    co_return;
}

// Runs `driver` on the calling thread under the current task's holder id (a fresh one at top level is
// the thread's ambient id), then waits in `state` for completion.
template <class T>
decltype(auto) run_to_completion(SignalTask<T>& driver, BlockOnState<T>& state, detail::HomeSlot& slot) {
    std::uint64_t const holder = current_holder_id();
    {
        ScopedExecution const context(&slot, holder);
        driver.resume();
    }
    return state.take(holder);
}

}  // namespace block_on_detail

// Blocks the CALLING thread until `t` completes. If `t` parks on a contended `AsyncMutex`/`channel<T>`
// await, the waker hands it back to this thread, which resumes it (ADR-175); this is the correct,
// general-purpose way to synchronously drive an `agentengine::rt::task<T>` from a plain, non-coroutine
// call site when the task MAY genuinely suspend -- see this file's own top comment for the real hazard a
// naive "resume until done" loop has here that this mechanism avoids.
template <class T>
[[nodiscard]] T block_on(agentengine::rt::task<T> t) {
    detail::HomeSlot slot;
    BlockOnState<T> state(&slot);
    block_on_detail::SignalTask<T> driver = block_on_detail::drive_and_signal(std::move(t), &state);
    return block_on_detail::run_to_completion(driver, state, slot);
}

inline void block_on(agentengine::rt::task<void> t) {
    detail::HomeSlot slot;
    BlockOnState<void> state(&slot);
    block_on_detail::SignalTask<void> driver = block_on_detail::drive_and_signal(std::move(t), &state);
    block_on_detail::run_to_completion(driver, state, slot);
}

}  // namespace agentengine::rt
