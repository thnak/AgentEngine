#pragma once
// ADR-037 Phase 2: `agentengine::rt::AsyncMutex`, the local replacement for what Quark's actor
// mailbox (Sequential dispatch) guaranteed structurally: I1, "one session, one executor." A
// session-like type (AgentSession, eventually WorkflowSupervisor) embeds ONE `AsyncMutex` and wraps
// every public async entry point through `co_await mutex.lock()`, holding the returned `Guard` for
// the duration of the call. Unlike Quark's mailbox, which REJECTS interleaving unless a handler's own
// business logic explicitly rejects it (ADR-029's "reject a fresh StartRun while a round is
// suspended" is a business rule layered ON TOP of the mailbox's own FIFO queueing, not a replacement
// for it), this type reproduces the mailbox's actual behavior: a second concurrent caller QUEUES
// (suspends, without blocking any OS thread) rather than being refused, and is woken in FIFO order
// once the current holder releases -- matching what "Sequential dispatch" has always actually meant
// in this codebase's own existing tests.
//
// NOT built by composing `rt::channel<T>` as a single-token semaphore -- that was the first design
// considered, and an independent red-team pass (before this file was written) found it structurally
// broken for this job: `channel_consumer` is deliberately single-consumer (channel.hpp's own file
// banner), so two callers concurrently reaching `co_await consumer.next_async()` would have the
// second registration silently clobber the first's, leaving the first caller parked forever with no
// way to ever be woken -- a guaranteed deadlock under this type's core use case (concurrent lock()
// callers), not an edge case. This file is instead its own small, directly-auditable FIFO waiter
// queue (per ADR-037 §5's own call for I1's replacement to "live in one place") -- built the same
// no-condition-variable-on-the-coroutine-path way `channel<T>`'s own async surface is (a suspended
// coroutine's handle is recorded under a mutex and resumed directly, later, by whichever thread next
// makes progress possible; see that file's banner for the fuller rationale, reused here without
// re-deriving it).
//
// CANCELLATION SAFETY: a `lock()` awaiter destroyed while genuinely parked (never resumed) removes
// its own registration from the waiter queue -- the same fix `channel<T>`'s `next_awaiter` needed
// (found and closed during this same Phase 2 red-team pass, see channel.hpp's own comment) for the
// identical reason: ADR-017 already establishes "drop the handle = cancel" as a deliberate house
// idiom, so a queued lock() being abandoned (a caller's own cancellation/timeout path) is a real,
// not hypothetical, scenario that must not corrupt this type's internal state.
//
// ONE NARROW, NAMED RESIDUAL, not silently claimed safe: `unlock()` pops the next waiter's handle
// under the internal mutex, releases the mutex, THEN calls `.resume()` on it (resuming while still
// holding the mutex would self-deadlock the instant the resumed coroutine's own continuation called
// `unlock()` again through symmetric transfer, since `std::mutex` is not recursive). In the
// vanishingly narrow window between "popped from the queue" and "resume() actually runs," if that
// SAME coroutine were somehow torn down by another thread, the destructor's self-removal check would
// no longer find it in the queue (already popped) and would not prevent the resume() call landing on
// a by-then-destroyed frame. `channel<T>`'s own producer-side hand-off has the structurally identical
// window and the same residual -- both are accepted here for the same reason: closing it needs
// holding a lock across a coroutine resume, which trades this narrow race for a real, broader
// self-deadlock risk. Named, not fixed, matching this project's own "residuals named, not fixed"
// convention (see e.g. ADR-028 §6).
//
// A SECOND issue, found the hard way (a real, 100%-reproducible SEGFAULT while first testing this
// type under 200 genuinely contended waiters) and FIXED, not just named: calling `next.resume()`
// directly from inside `unlock()` is an ordinary function call, not symmetric transfer -- if the
// resumed coroutine's own critical section is short (acquire, do a little work, release), its Guard
// destructor calls `unlock()` AGAIN before `next.resume()` ever returns to the FIRST `unlock()` call
// -- and if waiter #2 hands off to waiter #3 the same way, and so on, EVERY hand-off in the chain
// nests one call frame deeper than the last. With N genuinely contended waiters this is O(N) stack
// depth, not O(1) -- 200 waiters overflowed the stack outright. Fixed with a standard trampoline: a
// `draining_` flag (guarded by `m_`) marks "some thread is already inside this function's hand-off
// loop"; a reentrant `unlock()` call arriving while that flag is set does NOT recurse -- it just
// records `pending_release_ = true` and returns immediately, letting the ALREADY-RUNNING loop (still
// on its own original stack frame, one level deep, never growing) notice the pending flag right after
// its own `next.resume()` call returns and continue iterating. This is safe even when the eventual
// releasing thread differs from the one that started the drain (a lock held across real cross-thread
// async work, not just this file's own synchronous test workload): AsyncMutex's own exclusivity
// guarantees at most one logical "holder" exists at a time, so at most one unlock() call is ever
// legitimately in flight for a given instance -- a boolean is sufficient, no counter needed. See
// `unlock()`'s own body for the loop.

#include <algorithm>
#include <coroutine>
#include <deque>
#include <mutex>
#include <utility>

namespace agentengine::rt {

// ae-naming-lint: allow AsyncMutex — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class AsyncMutex {
public:
    // RAII ownership token, move-only. Held for the duration of a critical section; releasing (on
    // destruction or explicit reset) hands the mutex directly to the next queued waiter, if any.
    class Guard {
    public:
        Guard() noexcept = default;
        Guard(Guard const&) = delete;
        Guard& operator=(Guard const&) = delete;
        Guard(Guard&& other) noexcept : mutex_(std::exchange(other.mutex_, nullptr)) {}
        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                release();
                mutex_ = std::exchange(other.mutex_, nullptr);
            }
            return *this;
        }
        ~Guard() { release(); }

        [[nodiscard]] bool held() const noexcept { return mutex_ != nullptr; }

    private:
        friend class AsyncMutex;
        explicit Guard(AsyncMutex* m) noexcept : mutex_(m) {}
        void release() noexcept {
            if (mutex_) {
                AsyncMutex* m = std::exchange(mutex_, nullptr);
                m->unlock();
            }
        }
        AsyncMutex* mutex_ = nullptr;
    };

    AsyncMutex() noexcept = default;
    AsyncMutex(AsyncMutex const&) = delete;
    AsyncMutex& operator=(AsyncMutex const&) = delete;

    // co_await-able acquisition. `co_await mutex.lock()` yields a `Guard` once this coroutine
    // genuinely owns the mutex -- either immediately (uncontended) or after being queued and later
    // resumed in FIFO order by whichever call released it.
    struct LockAwaiter {
        AsyncMutex* self;
        bool parked = false;
        std::coroutine_handle<> handle_{};

        [[nodiscard]] bool await_ready() noexcept {
            std::lock_guard lock(self->m_);
            if (!self->held_) {
                self->held_ = true;
                return true;  // uncontended fast path -- no suspension needed
            }
            return false;
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) {
            std::lock_guard lock(self->m_);
            // Re-check under lock: an unlock() may have raced in between await_ready()'s unlock and
            // this lock (e.g. on a different thread) -- if the mutex is free now, take it without
            // ever actually suspending.
            if (!self->held_) {
                self->held_ = true;
                return false;
            }
            handle_ = h;
            parked = true;
            self->waiters_.push_back(h);
            return true;  // genuinely suspend -- some future unlock() resumes `h` directly
        }

        [[nodiscard]] Guard await_resume() noexcept {
            parked = false;  // reached the ordinary way -- nothing stale for the destructor to remove
            return Guard{self};
        }

        // See file banner's CANCELLATION SAFETY note: if this awaiter is destroyed while still
        // genuinely parked (the owning task dropped before ever being resumed), remove our own
        // registration so a later unlock() never resumes an already-destroyed frame.
        ~LockAwaiter() {
            if (!parked) return;
            std::lock_guard lock(self->m_);
            auto& q = self->waiters_;
            auto it = std::find(q.begin(), q.end(), handle_);
            if (it != q.end()) q.erase(it);
        }
    };

    [[nodiscard]] LockAwaiter lock() noexcept { return LockAwaiter{this}; }

private:
    friend struct LockAwaiter;

    // Hands ownership directly to the next queued waiter (FIFO), if any, or marks the mutex free.
    // ITERATIVE trampoline, not recursive -- see file banner's second numbered note for why: a naive
    // "resume the next waiter, let its own eventual unlock() recurse into this function again" design
    // grows the call stack by one frame per queued waiter, and a real 200-waiter contention test
    // segfaulted from exactly that. `draining_` marks "a hand-off loop is already running (on some
    // thread, possibly this one several frames up, possibly a call that already returned and whose
    // OWN loop is what's about to notice `pending_release_`)"; a reentrant call arriving while that's
    // set just records the pending release and returns immediately -- one call frame, always.
    void unlock() noexcept {
        std::coroutine_handle<> next;
        {
            std::lock_guard lock(m_);
            if (draining_) {
                // Someone (possibly ourselves, several logical hand-offs up the SAME already-running
                // loop -- see the loop body below) is already draining. Don't resume anything here;
                // just flag that another release happened and let that loop pick it up next.
                pending_release_ = true;
                return;
            }
            draining_ = true;
            if (!waiters_.empty()) {
                next = waiters_.front();
                waiters_.pop_front();
            } else {
                held_ = false;
            }
        }

        // The trampoline: resume the current candidate (if any), then check whether that resume()
        // call (or, in principle, a concurrent release on a genuinely different thread -- exclusivity
        // means at most one is ever actually pending, see file banner) queued up another hand-off
        // while we were inside it. Loop until there is nothing left to do; every iteration reuses THIS
        // one stack frame, never nests a new one.
        for (;;) {
            if (next) next.resume();
            std::lock_guard lock(m_);
            if (!pending_release_) {
                draining_ = false;
                return;
            }
            pending_release_ = false;
            if (!waiters_.empty()) {
                next = waiters_.front();
                waiters_.pop_front();
            } else {
                held_ = false;
                next = {};
                draining_ = false;
                return;
            }
        }
    }

    std::mutex m_;
    bool held_ = false;
    bool draining_ = false;
    bool pending_release_ = false;
    std::deque<std::coroutine_handle<>> waiters_;
};

}  // namespace agentengine::rt
