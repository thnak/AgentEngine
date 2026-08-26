#pragma once
// PROVE-PHASE shared utility: block_on<T>() synchronously drives a real agentengine::rt::task<T> to
// completion from the calling thread. This is not a test-harness convenience only -- it is a real
// implementation of the exact mechanism §13.5/§15.3's "MediatedFileSystem sync facade... blocks on its
// own async core operation completing" needs. Built here specifically so the deadlock hazard round
// 2/3 raised against that claim can be tested empirically, against the REAL rt::task<T>/rt::AsyncMutex
// (included directly from the real tree -- these are pre-existing coroutine SUBSTRATE this design
// runs on top of, not part of the Capability/Worktree/Sandbox machinery this design's own no-reuse
// framing is about; see identity_authority.hpp's own banner for the same distinction applied there).
//
// REAL, ASAN-PROVEN BUG FOUND AND FIXED (§34, a dedicated investigation round): the original
// `drive_and_signal()` returned a real `agentengine::rt::task<void>` and set the completion flag as
// an ordinary BODY STATEMENT one step before its own `co_return` -- e.g. `state->set(std::move(v));
// co_return;`. Under genuine cross-thread contention (many real OS threads, each independently
// calling `block_on()`, contending on a SHARED `agentengine::rt::AsyncMutex` -- exactly
// `async_quota/probe_positive.cpp`'s own "Part B" stress test), a coroutine suspended waiting on
// that mutex gets RESUMED by a DIFFERENT thread's `AsyncMutex::unlock()` (a deliberate, documented,
// CORRECT property of `AsyncMutex` -- see that file's own banner). That means the thread that set
// the flag is NOT necessarily the same thread `block_on()` was originally called from: the polling
// thread observes the flag, and IMMEDIATELY destroys the `task<void> driver` object -- but the
// resuming thread is, at that exact moment, still physically executing inside that same coroutine
// frame (its own `co_return` -> local-variable teardown -> `final_suspend()` transition hasn't
// finished yet). This is a genuine, real, ASan-confirmed cross-thread USE-AFTER-FREE on the driver
// coroutine's own frame -- confirmed to crash the unmodified original 17-25 times out of 30 runs
// under real ASan/repeated stress, not a rare theoretical edge case; the much lower "5-7%" rate an
// earlier, non-ASan sweep observed for the specific SILENT symptom (a wrong `remaining()` value
// instead of a crash) was the rare SURVIVABLE subset of a much more frequently occurring corruption.
//
// A dedicated instrumented probe confirmed `AsyncMutex`'s OWN mutual-exclusion invariant is never
// violated (a "believed-holders" counter never exceeded 1 across every completed run) -- the bug is
// entirely in THIS file's own driving pattern, not in `agentengine::rt::AsyncMutex` or
// `agentengine::rt::task<T>` themselves, both confirmed intact.
//
// FIX: the signal to the polling thread must be the ABSOLUTE LAST action ever taken on the driver
// coroutine's frame -- nothing may touch that frame's state afterward except the eventual, explicit
// `.destroy()` call, which must only happen once the signal has already been observed. This requires
// a dedicated, LOCAL (to this file) coroutine type whose `final_suspend()` awaiter's
// `await_suspend()` IS the signal -- final_suspend runs strictly after every local variable in the
// coroutine body (including the by-value `inner` parameter and the locally-`co_await`ed result) has
// already been destroyed, matching the standard "cppcoro sync_wait"-shaped pattern for exactly this
// problem. `agentengine::rt::task<T>`'s own `FinalAwaiter` (real production code, not modified here)
// is NOT reused for the driver itself for this reason -- its own final_suspend has a different job
// (resuming a `continuation_`, or none), and layering the signal as an ordinary body statement
// BEFORE reaching it is exactly the bug.

#include <atomic>
#include <coroutine>
#include <optional>
#include <thread>
#include <utility>

#include "agentengine/rt/task.hpp"

namespace probe {

template <class T>
class BlockOnState {
public:
    // Stores the delivered value -- safe to call from the coroutine body BEFORE final_suspend, since
    // nothing reads `value_` until `ready()` observes the flag this does NOT set.
    void set_value(T value) { value_.emplace(std::move(value)); }
    // The actual cross-thread signal -- must be called ONLY from a point that is guaranteed to be
    // the last touch on the signaling frame (see SignalTask::FinalAwaiter below).
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

namespace detail {

// A minimal, purpose-built coroutine type -- deliberately NOT agentengine::rt::task<void> -- whose
// entire reason to exist is a `final_suspend()` awaiter that performs the cross-thread completion
// signal as literally the last instruction ever executed on this frame. Never awaited by anything
// (block_on() drives it directly via resume()), so its FinalAwaiter never needs to resume a
// continuation -- it just signals and then leaves the frame permanently parked (suspended, not
// running anywhere) until block_on()'s own explicit destroy(), by which point the signal guarantees
// no other thread is still inside this frame.
template <class T>
struct SignalTask {
    struct promise_type {
        BlockOnState<T>* state = nullptr;

        // Matches drive_and_signal()'s own parameter list exactly (C++20's "promise constructor
        // parameter matching" -- the compiler passes the coroutine function's own arguments through
        // to a matching promise_type constructor, if one exists, instead of default-constructing
        // it). This is how `state` gets into the promise before the coroutine body ever runs, with
        // no separate "hook it up after the fact" step needed.
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
            // resume nothing else" -- the frame sits parked here, inert, until block_on()'s own
            // later, safe destroy() call.
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                h.promise().state->signal_done();
            }
            void await_resume() noexcept {}
        };
        [[nodiscard]] FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }  // matches this file's own prior
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
    // `state` here is the coroutine's own ordinary parameter (still directly usable in the body,
    // same as any coroutine parameter) -- used only to STORE the value, which is safe to do before
    // final_suspend since nothing reads it until the flag (set separately, see below) is observed.
    T v = co_await inner;
    state->set_value(std::move(v));
    co_return;
    // `inner` and `v` are destroyed by the compiler-generated code between here and
    // final_suspend(). The actual cross-thread SIGNAL happens in
    // SignalTask::promise_type::FinalAwaiter::await_suspend() (via promise_type::state, captured by
    // its own matching constructor above) -- strictly after that teardown, never here.
}

}  // namespace detail

// Blocks (busy-waits) the CALLING thread until `t` completes, wherever/whenever that completion
// actually happens (immediately on this call stack if uncontended, or later via a DIFFERENT thread's
// AsyncMutex::unlock() trampoline symmetric-transferring all the way through to completion).
template <class T>
[[nodiscard]] T block_on(agentengine::rt::task<T> t) {
    BlockOnState<T> state;
    detail::SignalTask<T> driver = detail::drive_and_signal(std::move(t), &state);
    driver.resume();
    return state.take();
}

// Bounded variant for deliberately provoking (and PROVING, not hanging on) a real deadlock: spins at
// most `max_spins` times, returns false (never completed) rather than looping forever. Used only by
// the deadlock-demonstration probe -- every other probe uses the unbounded block_on() above, matching
// what a real implementation would actually do.
template <class T>
[[nodiscard]] bool try_block_on_bounded(agentengine::rt::task<T> t, std::size_t max_spins, T* out) {
    BlockOnState<T> state;
    detail::SignalTask<T> driver = detail::drive_and_signal(std::move(t), &state);
    driver.resume();
    for (std::size_t i = 0; i < max_spins; ++i) {
        if (state.ready()) {
            *out = state.take();
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

}  // namespace probe
