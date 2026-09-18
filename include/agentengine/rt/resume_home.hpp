#pragma once
// decisions/ADR-175 -- who resumes a parked coroutine, and who owns a lock while it is parked.
//
// Before ADR-175, `AsyncMutex::unlock()` and `channel<T>::push()` resumed a parked coroutine INLINE, on
// whichever thread released the lock or pushed the item. Every synchronous driver in `rt::` then had to
// cope with a task that might finish on a thread it had never seen, and most did not (issue #78: the
// pool destroyed a job another thread was running; five resume-until-done loops resumed a parked handle a
// second time). This header gives the answer those drivers share:
//
//   HOME. A coroutine driven by `block_on()` belongs to the thread blocked in that call. When it parks,
//   the awaiter records that home; the waker POSTS the handle back to it instead of resuming it, and the
//   blocked thread resumes it itself. So a task `block_on()` drives never runs inside a waker's call
//   stack (where a nested relock livelocked, ADR-175 round 1), and resumes on the thread that drives it.
//
//   Which home a parking coroutine records is decided by the THREAD it parks on -- the execution context
//   below, set by whichever driver resumed it. A coroutine resumed by raw `task<T>::resume()` runs with no
//   home (`task.hpp` clears it), and parks homeless: the pre-ADR-175 inline resume. A coroutine resumed by
//   a bare `coroutine_handle::resume()` from inside some OTHER driver's context -- an awaitable that hops
//   to a pool job, say -- adopts that driver's home and holder until it next parks; if that driver has
//   already finished when the lock is granted, its home is CLOSED and the waker resumes inline instead
//   (ADR-175 round 3 finding 2: before closing, such a grant was posted to a queue nobody drained, and the
//   mutex stayed held forever).
//
//   HOLDER. Every coroutine runs on behalf of a logical task, named by a holder id. `AsyncMutex` records
//   the holder id of whoever it grants the lock to, and `is_held_by_current_thread()` compares against the
//   running task's id -- not an OS thread, which after a hand-off may be running somebody else entirely
//   (ADR-175 round 1 finding 3, round 2 findings 1-2). A `block_on()` runs under the holder already current
//   (a tool closure inside a round is part of that round); a raw `task<T>::resume()` under the task's own
//   id, minted on first resume (round 3 finding 3: a homeless task must not lend its identity to the
//   thread it parked from); a thread running neither has an AMBIENT id of its own.
//
// Lazy: `block_on()` creates its home on the first park, on its own thread, so an uncontended call pays no
// allocation (ADR-175 round 2 finding 8 measured an eager home at 3.7x an uncontended call).

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace agentengine::rt {

[[nodiscard]] inline std::uint64_t mint_holder_id() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

namespace detail {

class CallerHome;

// Owned by one `block_on()` call, on its stack; `home` is created on the first park, by the thread that
// owns the call. Read from another thread only by a completion signal that came through a foreign
// (non-homing) awaitable -- after that thread's own write, which the hand-off orders before it.
struct HomeSlot {
    std::shared_ptr<CallerHome> home;
};

struct ExecutionContext {
    HomeSlot*     slot   = nullptr;  // the block_on() currently driving this thread's coroutine, if any
    std::uint64_t holder = 0;        // 0: use the thread's ambient id
};

[[nodiscard]] inline ExecutionContext& current_execution() noexcept {
    thread_local ExecutionContext ctx;
    return ctx;
}

[[nodiscard]] inline std::uint64_t ambient_holder_id() noexcept {
    thread_local std::uint64_t const id = mint_holder_id();
    return id;
}

}  // namespace detail

// The logical task the current thread is running: set by the driver that resumed it, else the thread's own.
[[nodiscard]] inline std::uint64_t current_holder_id() noexcept {
    std::uint64_t const h = detail::current_execution().holder;
    return h != 0 ? h : detail::ambient_holder_id();
}

// RAII: coroutines resumed on this thread until destruction belong to `holder` and park back to `slot`
// (nullptr: homeless). The previous context is restored on destruction.
// ae-naming-lint: allow ScopedExecution — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class ScopedExecution {
public:
    ScopedExecution(detail::HomeSlot* slot, std::uint64_t holder) noexcept
        : saved_(std::exchange(detail::current_execution(), detail::ExecutionContext{slot, holder})) {}
    ~ScopedExecution() { detail::current_execution() = saved_; }
    ScopedExecution(ScopedExecution const&) = delete;
    ScopedExecution& operator=(ScopedExecution const&) = delete;

private:
    detail::ExecutionContext saved_;
};

namespace detail {

// The thread blocked in one `block_on()` call, seen as a place parked coroutines are handed back to.
class CallerHome {
public:
    struct Posted {
        std::coroutine_handle<> handle{};
        std::uint64_t           holder = 0;
    };

    // Called by a waker after releasing its primitive's lock; the waker's own shared_ptr keeps this object
    // alive through the unlock. A home whose block_on() has already returned is closed: nobody will drain
    // it, so the handle is resumed here, inline and homeless, exactly as before ADR-175.
    void post(std::coroutine_handle<> h, std::uint64_t holder) noexcept {
        if (try_post(h, holder)) return;
        ScopedExecution const homeless(nullptr, holder);
        h.resume();
    }

    // Queues `h` and returns true, or returns false -- queuing nothing -- if the home has closed. A waker
    // that can run a trampoline (AsyncMutex::unlock()) uses this to resume a closed-home waiter in its loop
    // instead of nesting it on the stack (ADR-175 round 4 finding 1: a chain of closed-home waiters each
    // resumed inside the previous one's unlock() overflowed the stack at 3,000 waiters).
    [[nodiscard]] bool try_post(std::coroutine_handle<> h, std::uint64_t holder) noexcept {
        std::lock_guard<std::mutex> lock(m_);
        if (closed_) return false;
        posted_.push_back(Posted{h, holder});
        ready_.store(true, std::memory_order_relaxed);
        cv_.notify_one();
        return true;
    }

    void mark_done() noexcept {
        std::lock_guard<std::mutex> lock(m_);
        done_ = true;
        ready_.store(true, std::memory_order_relaxed);
        cv_.notify_one();
    }

    // Blocks until a handle is posted (returned) or the driven task is done and nothing is left, in which
    // case the home is closed in the same critical section -- so no post can slip in unseen -- and an
    // empty record is returned.
    //
    // Spins briefly before sleeping. A hand-off to a parked task must now wake its home thread rather than
    // run the task inline on the releasing thread, and under contention on short critical sections that
    // wake is most of the cost (ADR-175 round 3 finding 5 measured a 13x contended slowdown before this).
    // `ready_` is only a hint read without the lock; the decision is always re-made under it.
    [[nodiscard]] Posted wait_next() {
        for (int i = 0; i < kSpinIterations && !ready_.load(std::memory_order_relaxed); ++i) {
            if ((i & 1023) == 1023) std::this_thread::yield();
        }
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [this] { return done_ || !posted_.empty(); });
        if (posted_.empty()) {
            closed_ = true;
            return Posted{};
        }
        Posted next = posted_.front();
        posted_.pop_front();
        ready_.store(done_ || !posted_.empty(), std::memory_order_relaxed);
        return next;
    }

private:
    static constexpr int kSpinIterations = 20000;

    std::mutex              m_;
    std::condition_variable cv_;
    std::deque<Posted>      posted_;
    std::atomic<bool>       ready_{false};  // hint: something to take (under m_ for writes)
    bool                    done_   = false;
    bool                    closed_ = false;
};

// What a parked awaiter remembers about who resumes it and on whose behalf.
struct ParkedResumer {
    std::coroutine_handle<>     handle{};
    std::shared_ptr<CallerHome> home{};
    std::uint64_t               holder = 0;
};

// Called by an awaiter's await_suspend() BEFORE it registers anything: the allocation of a first-park home
// may throw, and an exception from await_suspend() is only safe while nothing yet refers to the handle.
// Runs on the thread executing the coroutine, which is the thread that owns `slot`.
[[nodiscard]] inline ParkedResumer capture_parked(std::coroutine_handle<> h) {
    ExecutionContext const& ctx = current_execution();
    std::shared_ptr<CallerHome> home;
    if (ctx.slot != nullptr) {
        if (!ctx.slot->home) ctx.slot->home = std::make_shared<CallerHome>();
        home = ctx.slot->home;
    }
    return ParkedResumer{h, std::move(home), current_holder_id()};
}

// Called by a waker AFTER releasing its primitive's lock. A homed coroutine is posted to its home (which
// resumes it inline if it has closed); a homeless one is resumed inline as before ADR-175, on behalf of its
// own recorded holder and with no home, so it neither adopts the waker's identity nor parks back to the
// waker's block_on().
inline void wake(ParkedResumer r) noexcept {
    if (!r.handle) return;
    if (r.home) {
        std::shared_ptr<CallerHome> const home = std::move(r.home);
        home->post(r.handle, r.holder);
        return;
    }
    ScopedExecution const homeless(nullptr, r.holder);
    r.handle.resume();
}

}  // namespace detail

}  // namespace agentengine::rt
