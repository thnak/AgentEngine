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
//
// ADR-175 (issue #78) -- WHO RESUMES A WAITER, AND WHO OWNS THE LOCK. Everything above about inline
// hand-off and the trampoline now applies only to HOMELESS waiters: coroutines driven by raw `resume()`
// calls. A waiter that parks while a `block_on()` drives it records that call as its home
// (`rt/resume_home.hpp`), and `unlock()` POSTS it back there instead of resuming it -- the blocked thread
// resumes its own task, and the releasing thread never runs a successor's critical section on its stack.
// Ownership is the HOLDER ID of the task the lock was granted to, recorded under `m_` at the moment of
// granting (fast path, or hand-off pop) -- not the OS thread that happens to resume it, which after a
// posted or inline hand-off can be running somebody else entirely.

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

#include "agentengine/rt/resume_home.hpp"

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
                self->owner_.store(current_holder_id(), std::memory_order_release);
                return true;  // uncontended fast path -- no suspension needed
            }
            return false;
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) {
            // Before taking m_ and before registering: capturing may allocate a home and throw, which
            // is only safe while nothing refers to `h` yet.
            detail::ParkedResumer record = detail::capture_parked(h);
            std::lock_guard lock(self->m_);
            // Re-check under lock: an unlock() may have raced in between await_ready()'s unlock and
            // this lock (e.g. on a different thread) -- if the mutex is free now, take it without
            // ever actually suspending.
            if (!self->held_) {
                self->held_ = true;
                self->owner_.store(record.holder, std::memory_order_release);
                return false;
            }
            self->waiters_.push_back(std::move(record));
            handle_ = h;
            parked = true;
            return true;  // genuinely suspend -- unlock() posts `h` to its home, or resumes it inline
        }

        [[nodiscard]] Guard await_resume() noexcept {
            parked = false;  // reached the ordinary way -- nothing stale for the destructor to remove
            // ADR-175: ownership was already recorded under m_ by whoever granted the lock. Stamping it
            // here instead -- on whichever thread resumes, whenever that is -- left the RELEASING task
            // reported as owner for as long as a posted successor waited (round 2 finding 1).
            return Guard{self};
        }

        // See file banner's CANCELLATION SAFETY note: if this awaiter is destroyed while still
        // genuinely parked (the owning task dropped before ever being resumed), remove our own
        // registration so a later unlock() never resumes an already-destroyed frame.
        ~LockAwaiter() {
            if (!parked) return;
            std::lock_guard lock(self->m_);
            auto& q = self->waiters_;
            auto it = std::find_if(q.begin(), q.end(),
                                   [this](detail::ParkedResumer const& r) { return r.handle == handle_; });
            if (it != q.end()) q.erase(it);
        }
    };

    [[nodiscard]] LockAwaiter lock() noexcept { return LockAwaiter{this}; }

    // ADR-123 -- a real, disclosed reentrant-self-deadlock hazard (AgentSession::fork_from(), agent_
    // session.hpp's own comment) needed a way to ask "does the CALLING thread already own this mutex"
    // without adding a second, incompatible locking discipline (a recursive mutex would silently change
    // this type's own semantics for every existing caller). Nothing reads `owner_` unless it explicitly
    // calls this method.
    //
    // ADR-175 replaced the owner-THREAD comparison ADR-123 §7 documented as a known limitation: the name is
    // kept for source compatibility, but the question answered is "is this mutex held by the logical task
    // running on the calling thread" (`current_holder_id()`, rt/resume_home.hpp). The thread comparison
    // was wrong both ways once a task can park while holding the lock: a false NEGATIVE after the holder
    // resumed on another thread (ADR-123 §7), and a false POSITIVE on the thread that released the lock,
    // or resumed the holder inline, and then ran unrelated code (ADR-175 round 1 finding 3, round 2
    // finding 2) -- the latter letting `AgentSession::fork_from()` skip `session_mutex_` while the real
    // holder was mid-round, an I1 violation. A holder id travels with the task through `block_on()`
    // (inherited by a nested call), through raw `task<T>::resume()` (the task's own id), and through a
    // homeless inline resume (restored from the waiter's record). Residual: a coroutine resumed by a bare
    // `coroutine_handle::resume()` from inside another driver's context runs under THAT driver's id until
    // it next parks -- the same class of wrong answer the thread comparison gave, confined to foreign
    // awaitables (none in tree).
    //
    // Lock-free: `owner_` is written under `m_` at every grant and release, and is 0 whenever the mutex is
    // free. The only task for which it can equal `current_holder_id()` is one the lock was granted to, and
    // that grant happened-before the task could ask; a concurrent grant to or release by ANOTHER task can
    // only move it between values that are not this task's id.
    [[nodiscard]] bool is_held_by_current_thread() const noexcept {
        return owner_.load(std::memory_order_acquire) == current_holder_id();
    }

private:
    friend struct LockAwaiter;

    // Pops the next waiter and grants it the lock, under m_. The owner is its recorded holder from this
    // instant, whenever it actually resumes -- stored BEFORE posting, so a successor its home resumes at
    // once already sees itself as holder.
    //
    // A homed waiter is posted to its home here and an empty record is returned: nothing is left for the
    // caller to run. A homeless waiter is returned for the caller's trampoline to resume inline -- and so
    // is a homed waiter whose home has CLOSED (its block_on() returned; ADR-175 round 4 finding 1: resuming
    // those inside post() nested each one in the previous one's unlock() and overflowed the stack). Such a
    // waiter parked under a block_on() it did not belong to -- a foreign awaitable's bare handle resume --
    // and recorded that call's holder id; it is granted under a FRESH id instead, so the thread that ran
    // that block_on() is not reported as holder for this critical section (round 4 finding 2).
    [[nodiscard]] detail::ParkedResumer grant_next_locked() noexcept {
        detail::ParkedResumer next = std::move(waiters_.front());
        waiters_.pop_front();
        owner_.store(next.holder, std::memory_order_release);
        if (next.home) {
            if (next.home->try_post(next.handle, next.holder)) return detail::ParkedResumer{};
            next.home.reset();
            next.holder = mint_holder_id();
            owner_.store(next.holder, std::memory_order_release);
        }
        return next;
    }

    void release_locked() noexcept {
        held_ = false;
        owner_.store(0, std::memory_order_release);
    }

    // Hands ownership directly to the next queued waiter (FIFO), if any, or marks the mutex free.
    // ITERATIVE trampoline, not recursive -- see file banner's second numbered note for why: a naive
    // "resume the next waiter, let its own eventual unlock() recurse into this function again" design
    // grows the call stack by one frame per queued waiter, and a real 200-waiter contention test
    // segfaulted from exactly that. `draining_` marks "a hand-off loop is already running (on some
    // thread, possibly this one several frames up, possibly a call that already returned and whose
    // OWN loop is what's about to notice `pending_release_`)"; a reentrant call arriving while that's
    // set just records the pending release and returns immediately -- one call frame, always.
    void unlock() noexcept {
        detail::ParkedResumer next;
        {
            std::lock_guard lock(m_);
            if (draining_) {
                // Someone (possibly ourselves, several logical hand-offs up the SAME already-running
                // loop -- see the loop body below) is already draining. Don't resume anything here;
                // just flag that another release happened and let that loop pick it up next.
                pending_release_ = true;
                return;
            }
            if (waiters_.empty()) {
                release_locked();
                return;
            }
            next = grant_next_locked();
            if (!next.handle) return;  // ADR-175: posted to its home -- nothing runs on this thread
            draining_ = true;
        }

        // The trampoline, for waiters resumed inline (homeless, or homed to a closed home): resume the
        // current candidate, then check whether that resume() call (or, in principle, a concurrent release
        // on a genuinely different thread -- exclusivity means at most one is ever actually pending, see
        // file banner) queued up another hand-off while we were inside it. Every iteration reuses THIS one
        // stack frame. A successor posted to its home ends the loop.
        for (;;) {
            detail::wake(std::move(next));  // no home: inline, on behalf of its own holder id
            std::lock_guard lock(m_);
            if (!pending_release_) {
                draining_ = false;
                return;
            }
            pending_release_ = false;
            if (waiters_.empty()) {
                release_locked();
                draining_ = false;
                return;
            }
            next = grant_next_locked();
            if (!next.handle) {
                draining_ = false;
                return;
            }
        }
    }

    std::mutex m_;
    bool held_ = false;
    bool draining_ = false;
    bool pending_release_ = false;
    std::deque<detail::ParkedResumer> waiters_;
    // ADR-175: the holder id the lock is granted to (resume_home.hpp); written under m_, read lock-free by
    // is_held_by_current_thread(). Holder ids start at 1, so 0 (free) never matches a running task.
    std::atomic<std::uint64_t> owner_{0};
};

}  // namespace agentengine::rt
