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
//
// ADR-219 (issue #79) -- A HOST'S RESUMER. A waiter that parks inside a `ScopedResumer` (a host driving
// AgentEngine coroutines from its own executor) is handed to that host's `Resumer` as a
// `ParkedContinuation` instead of being resumed on the releasing thread. The hand-off runs inside the
// trampoline below, outside `m_`: a host resumer that runs the continuation inline (or drops it, which
// resumes it) re-enters `unlock()` as a pending release, not as a nested drain.
//
// decisions/ADR-237 §4.3 (step 5) -- AWAIT CHAINS: LENDING AND REFUSAL. A task forked by a task scope or
// `rt::on_strand` has its own holder id, linked to the task it was forked from (rt/await_chain.hpp). Before
// step 5, such a child asking for a lock its suspended ancestor holds queued behind that ancestor forever (the
// ancestor waits for the child; the session -> workflow -> agent node -> tool -> parent-session cycle). Now a
// CONTENDED lock() classifies the requester against the holder and every lender (a walk of the registry, never
// on the uncontended fast path):
//   - the holder is an ancestor AWAITING the requester's chain, and the entry point is declared LEND-SAFE
//     (`lock(lock_entry::lend_safe)`: read-only ones -- `fork_from_async`, `snapshot_record`, history reads):
//     the lock is LENT. One borrower at a time per lender; further borrowers under the same lender wait in that
//     lender's LOAN QUEUE, served before every outside FIFO waiter. Releasing a loan returns the lock to the
//     LENDER (or to the next borrower in its loan queue), never to the FIFO head -- otherwise a queued mutating
//     entry would take a lock the lender still believes it holds. A borrower's descendant may borrow again:
//     loans form a STACK (`Guard` remembers its depth; releasing a guard that is not the top of the stack is a
//     checked violation). A borrower that finishes with its loan outstanding (the guard moved into background
//     work) is a checked violation, detected where its scope or on_strand records it finished.
//   - a MUTATING entry point (plain `lock()`, or `lock(lock_entry::mutating)`) requested while an awaiting
//     ancestor holds the lock or lends it, or ANY entry point requested while a NON-awaiting ancestor holds it
//     (a `with_scope` owner whose body still runs): REFUSED with `rt.lock_held_by_ancestor` -- a diagnosable error
//     instead of a run parked forever. `lock(entry)` returns it as a value (`std::expected<Guard, lock_refusal>`);
//     plain `lock()` keeps its `Guard` signature and throws `lock_refused` (the case was a silent deadlock before).
//   - everyone else (unrelated tasks, fresh chains -- background jobs) waits in FIFO order, exactly as before.
// `is_held_by_current_thread()` is unchanged (owner == the current holder id; a borrower is the owner while it
// holds its loan), so ADR-123's `fork_from` keeps working for every caller that exists today.

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/rt/await_chain.hpp"
#include "agentengine/rt/resume_home.hpp"

namespace agentengine::rt {

// ADR-237 §4.3: what an entry point declares about itself when it takes a session's lock.
// ae-naming-lint: allow lock_entry — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
enum class lock_entry : std::uint8_t {
    mutating,   // start_run, resolve_interaction, start_background_task, schedule_wakeup, close: never borrows
    lend_safe,  // read-only (fork_from_async, snapshot_record, history reads): may borrow from an awaiting ancestor
};

// ae-naming-lint: allow lock_held_by_ancestor_code — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
inline constexpr char const lock_held_by_ancestor_code[] = "rt.lock_held_by_ancestor";

// ae-naming-lint: allow lock_refusal — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
struct lock_refusal {
    std::string_view code = lock_held_by_ancestor_code;
};

// Thrown by the Guard-returning `AsyncMutex::lock()` on a refusal (file comment).
// ae-naming-lint: allow lock_refused — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class lock_refused : public std::logic_error {
public:
    lock_refused()
        : std::logic_error(std::string(lock_held_by_ancestor_code) +
                           ": a mutating entry point (or any entry point under a non-awaiting ancestor) asked for a "
                           "lock its own ancestor holds (ADR-237 §4.3)") {}
    [[nodiscard]] std::string_view code() const noexcept { return lock_held_by_ancestor_code; }
};

// ae-naming-lint: allow AsyncMutex — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class AsyncMutex {
public:
    // RAII ownership token, move-only. Held for the duration of a critical section; releasing (on
    // destruction or explicit reset) hands the mutex directly to the next queued waiter, if any -- or, for a
    // LENT guard (ADR-237 §4.3), back to its lender.
    class Guard {
    public:
        Guard() noexcept = default;
        Guard(Guard const&) = delete;
        Guard& operator=(Guard const&) = delete;
        Guard(Guard&& other) noexcept
            : mutex_(std::exchange(other.mutex_, nullptr)), depth_(std::exchange(other.depth_, 0)) {}
        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                release();
                mutex_ = std::exchange(other.mutex_, nullptr);
                depth_ = std::exchange(other.depth_, 0);
            }
            return *this;
        }
        ~Guard() { release(); }

        [[nodiscard]] bool held() const noexcept { return mutex_ != nullptr; }
        // ADR-237 §4.3: 0 for the holder's own guard; n for the n-th nested loan.
        [[nodiscard]] std::size_t loan_depth() const noexcept { return depth_; }

    private:
        friend class AsyncMutex;
        Guard(AsyncMutex* m, std::size_t depth) noexcept : mutex_(m), depth_(depth) {}
        void release() noexcept {
            if (mutex_) {
                AsyncMutex* m = std::exchange(mutex_, nullptr);
                m->unlock(std::exchange(depth_, 0));
            }
        }
        AsyncMutex* mutex_ = nullptr;
        std::size_t depth_ = 0;
    };

    AsyncMutex() noexcept = default;
    AsyncMutex(AsyncMutex const&) = delete;
    AsyncMutex& operator=(AsyncMutex const&) = delete;

    // co_await-able acquisition. `co_await mutex.lock()` yields a `Guard` once this coroutine
    // genuinely owns the mutex -- either immediately (uncontended) or after being queued and later
    // resumed in FIFO order by whichever call released it.
    struct LockAwaiter {
        LockAwaiter(AsyncMutex* m, lock_entry k) noexcept : self(m), kind(k) {}

        AsyncMutex* self;
        lock_entry  kind;
        bool parked = false;
        std::coroutine_handle<> handle_{};

        [[nodiscard]] bool await_ready() noexcept {
            std::lock_guard lock(self->m_);
            return self->decide_locked(current_holder_id(), *this);  // free, lent or refused: no suspension
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) {
            // Before taking m_ and before registering: capturing may allocate a home and throw, which
            // is only safe while nothing refers to `h` yet.
            detail::ParkedResumer record = detail::capture_parked(h);
            std::lock_guard lock(self->m_);
            // Re-check under lock: an unlock() may have raced in between await_ready()'s unlock and
            // this lock (e.g. on a different thread) -- if the mutex is free now, take it without
            // ever actually suspending (and the same for a loan or a refusal that became decidable).
            Classified const c = self->classify_locked(record.holder, kind);
            if (c.decision != Decision::queue_fifo && c.decision != Decision::queue_loan) {
                (void)self->decide_locked(record.holder, *this);
                return false;
            }
            ticket_ = record.ticket;  // ADR-219: null unless a host resumer drives this coroutine
            if (c.decision == Decision::queue_loan) {
                self->loans_[c.frame].queue.push_back(Waiter{std::move(record), this});  // ADR-237 §4.3
            } else {
                self->waiters_.push_back(Waiter{std::move(record), this});
            }
            handle_ = h;
            parked = true;
            return true;  // genuinely suspend -- unlock() posts `h` to its home, or resumes it inline
        }

        [[nodiscard]] Guard await_resume() {
            parked = false;  // reached the ordinary way -- nothing stale for the destructor to remove
            if (refused_) throw lock_refused();  // ADR-237 §4.3
            // ADR-175: ownership was already recorded under m_ by whoever granted the lock. Stamping it
            // here instead -- on whichever thread resumes, whenever that is -- left the RELEASING task
            // reported as owner for as long as a posted successor waited (round 2 finding 1).
            return Guard{self, granted_depth_};
        }

        // See file banner's CANCELLATION SAFETY note: if this awaiter is destroyed while still
        // genuinely parked (the owning task dropped before ever being resumed), remove our own
        // registration so a later unlock() never resumes an already-destroyed frame.
        //
        // ADR-219 self red-team finding 1: if the registration is already gone, the lock was GRANTED to this
        // waiter and its continuation handed to a host resumer, where it may wait indefinitely. Destroying
        // the frame then left the lock held by a dead task forever, and the queued continuation resuming a
        // destroyed frame. If this destruction claims the continuation first, it never runs, and the lock
        // granted to it is released here. (A homed or homeless waiter has no claim: ADR-175 §6's residual.)
        ~LockAwaiter() {
            if (!parked) return;
            bool release_grant = false;
            {
                std::lock_guard lock(self->m_);
                if (!self->erase_waiter_locked(this)) release_grant = detail::abandon_woken(ticket_);
            }
            if (release_grant) self->unlock(granted_depth_);
        }

        std::shared_ptr<detail::ResumerTicket> ticket_{};
        std::size_t granted_depth_ = 0;  // ADR-237 §4.3: written under m_ by whoever granted (0: not a loan)
        bool        refused_       = false;
    };

    // ADR-237 §4.3: `co_await mutex.lock(entry)` -- as `lock()`, but a refusal is a value, not an exception.
    struct CheckedLockAwaiter : LockAwaiter {
        using LockAwaiter::LockAwaiter;
        [[nodiscard]] std::expected<Guard, lock_refusal> await_resume() {
            parked = false;
            if (refused_) return std::unexpected(lock_refusal{});
            return Guard{self, granted_depth_};
        }
    };

    // A MUTATING entry point's acquisition (ADR-237 §4.3): never borrows; throws `lock_refused` when refused.
    [[nodiscard]] LockAwaiter lock() noexcept { return LockAwaiter{this, lock_entry::mutating}; }
    // ADR-237 §4.3: an entry point that declares what it is; a refusal comes back as a value.
    [[nodiscard]] CheckedLockAwaiter lock(lock_entry entry) noexcept { return CheckedLockAwaiter{this, entry}; }

    // Issue #156: non-blocking acquisition from plain (non-coroutine) code -- the lock if it is free right
    // now, else an empty Guard (`held() == false`); never waits, never queues, never borrows. Used by
    // `WorkflowSupervisor::cancel()` to settle a SUSPENDED run on the spot when no entry point is running.
    // The Guard releases exactly like one from `lock()`, handing the mutex to a waiter that queued meanwhile.
    [[nodiscard]] Guard try_lock() noexcept {
        std::lock_guard lock(m_);
        if (held_) return Guard{};
        held_ = true;
        owner_.store(current_holder_id(), std::memory_order_release);
        return Guard{this, 0};
    }

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
    //
    // ADR-237 §4.3 (step 5): a borrower is the owner while its loan is out; the lender, suspended meanwhile,
    // is the owner again once the loan comes back.
    [[nodiscard]] bool is_held_by_current_thread() const noexcept {
        return owner_.load(std::memory_order_acquire) == current_holder_id();
    }

    // ADR-237 §4.3 diagnostics: loans granted, refusals, and loans outstanding now.
    [[nodiscard]] std::uint64_t loans_granted() const noexcept { return loans_granted_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t refusals() const noexcept { return refusals_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t loan_depth() const {
        std::lock_guard lock(m_);
        return loans_.size();
    }
    // Diagnostics: requests parked in the FIFO, and in every loan queue.
    [[nodiscard]] std::size_t queued() const {
        std::lock_guard lock(m_);
        return waiters_.size();
    }
    [[nodiscard]] std::size_t loan_queued() const {
        std::lock_guard lock(m_);
        std::size_t n = 0;
        for (LoanFrame const& f : loans_) n += f.queue.size();
        return n;
    }

private:
    friend struct LockAwaiter;

    // A parked acquisition: its record (who resumes it, under which holder) and its awaiter (where the grant
    // writes the loan depth).
    struct Waiter {
        detail::ParkedResumer r;
        LockAwaiter*          aw = nullptr;
    };

    // ADR-237 §4.3: one entry of the loan stack. `lender` lent the lock to `borrower`; `queue` holds further
    // borrowers under the same lender, served before the FIFO.
    struct LoanFrame {
        std::uint64_t      lender   = 0;
        std::uint64_t      borrower = 0;
        std::deque<Waiter> queue;
    };

    enum class Decision : std::uint8_t { take, lend, refuse, queue_fifo, queue_loan };
    struct Classified {
        Decision    decision = Decision::queue_fifo;
        std::size_t frame    = 0;  // queue_loan: the loan frame whose lender the requester descends from
    };

    // ADR-237 §4.3: what a lock request from holder `me` gets right now (under m_). The current owner (the top
    // borrower, or the holder) first -- a new, nested loan -- then each lender from the top of the stack down -- a
    // place in that lender's loan queue.
    [[nodiscard]] Classified classify_locked(std::uint64_t me, lock_entry kind) const noexcept {
        if (!held_) return {Decision::take, 0};
        using chain_detail::relation;
        auto const&    chains = chain_detail::Registry::instance();
        relation const top    = chains.relate(me, owner_.load(std::memory_order_relaxed));
        if (top == relation::awaiting_ancestor) {
            return {kind == lock_entry::lend_safe ? Decision::lend : Decision::refuse, 0};
        }
        if (top == relation::other_ancestor) return {Decision::refuse, 0};
        for (std::size_t i = loans_.size(); i-- > 0;) {
            relation const r = chains.relate(me, loans_[i].lender);
            if (r == relation::awaiting_ancestor) {
                return {kind == lock_entry::lend_safe ? Decision::queue_loan : Decision::refuse, i};
            }
            if (r == relation::other_ancestor) return {Decision::refuse, 0};
        }
        return {Decision::queue_fifo, 0};
    }

    // Applies a decision that needs no suspension (under m_); returns false when the requester must queue.
    [[nodiscard]] bool decide_locked(std::uint64_t me, LockAwaiter& aw) noexcept {
        Classified const c = classify_locked(me, aw.kind);
        switch (c.decision) {
            case Decision::take:
                held_ = true;
                owner_.store(me, std::memory_order_release);
                aw.granted_depth_ = 0;
                return true;  // uncontended fast path -- no suspension needed
            case Decision::lend:
                loans_.push_back(LoanFrame{owner_.load(std::memory_order_relaxed), me, {}});
                owner_.store(me, std::memory_order_release);
                chain_detail::Registry::instance().add_loan(me, +1);
                aw.granted_depth_ = loans_.size();
                loans_granted_.fetch_add(1, std::memory_order_relaxed);
                return true;
            case Decision::refuse:
                aw.refused_ = true;
                refusals_.fetch_add(1, std::memory_order_relaxed);
                return true;
            case Decision::queue_fifo:
            case Decision::queue_loan:
                return false;
        }
        return false;
    }

    [[nodiscard]] bool erase_waiter_locked(LockAwaiter const* aw) noexcept {
        auto const match = [aw](Waiter const& w) { return w.aw == aw; };
        if (auto it = std::find_if(waiters_.begin(), waiters_.end(), match); it != waiters_.end()) {
            waiters_.erase(it);
            return true;
        }
        for (LoanFrame& f : loans_) {
            if (auto it = std::find_if(f.queue.begin(), f.queue.end(), match); it != f.queue.end()) {
                f.queue.erase(it);
                return true;
            }
        }
        return false;
    }

    // Pops the next waiter and grants it the lock, under m_. The owner is its recorded holder from this
    // instant, whenever it actually resumes -- stored BEFORE posting, so a successor its home resumes at
    // once already sees itself as holder.
    //
    // A homed waiter is posted to its home here and an empty record is returned: nothing is left for the
    // caller to run. A waiter a host resumer drives (ADR-219) is returned for the trampoline to hand over
    // with `detail::wake()` -- never from under `m_`, since that calls host code. A homeless waiter is
    // returned for the caller's trampoline to resume inline -- and so
    // is a homed waiter whose home has CLOSED (its block_on() returned; ADR-175 round 4 finding 1: resuming
    // those inside post() nested each one in the previous one's unlock() and overflowed the stack). Such a
    // waiter parked under a block_on() it did not belong to -- a foreign awaitable's bare handle resume --
    // and recorded that call's holder id; it is granted under a FRESH id instead, so the thread that ran
    // that block_on() is not reported as holder for this critical section (round 4 finding 2).
    [[nodiscard]] detail::ParkedResumer grant_next_locked() noexcept {
        Waiter w = std::move(waiters_.front());
        waiters_.pop_front();
        w.aw->granted_depth_      = 0;
        detail::ParkedResumer next = std::move(w.r);
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

    // ADR-237 §4.3: a released LOAN goes back to its lender -- or to the next borrower queued under that lender --
    // and never to the FIFO. Returns false for the holder's own guard (depth 0), which `unlock` releases normally.
    [[nodiscard]] bool release_loan(std::size_t depth) noexcept {
        detail::ParkedResumer next;
        {
            std::lock_guard lock(m_);
            if (depth != loans_.size()) {
                detail::checked_violation("an AsyncMutex guard was released while a loan of it is outstanding, or "
                                          "out of loan-stack order (§4.3)");
            }
            if (depth == 0) return false;
            LoanFrame& f      = loans_.back();
            auto&      chains = chain_detail::Registry::instance();
            chains.add_loan(f.borrower, -1);
            if (f.queue.empty()) {
                owner_.store(f.lender, std::memory_order_release);  // back to the LENDER, never the FIFO head
                loans_.pop_back();
                return true;
            }
            Waiter w = std::move(f.queue.front());
            f.queue.pop_front();
            f.borrower        = w.r.holder;
            w.aw->granted_depth_ = depth;
            owner_.store(w.r.holder, std::memory_order_release);
            chains.add_loan(w.r.holder, +1);
            loans_granted_.fetch_add(1, std::memory_order_relaxed);
            next = std::move(w.r);
        }
        detail::wake(std::move(next));  // borrowers are strand children: this posts
        return true;
    }

    // Hands ownership directly to the next queued waiter (FIFO), if any, or marks the mutex free.
    // ITERATIVE trampoline, not recursive -- see file banner's second numbered note for why: a naive
    // "resume the next waiter, let its own eventual unlock() recurse into this function again" design
    // grows the call stack by one frame per queued waiter, and a real 200-waiter contention test
    // segfaulted from exactly that. `draining_` marks "a hand-off loop is already running (on some
    // thread, possibly this one several frames up, possibly a call that already returned and whose
    // OWN loop is what's about to notice `pending_release_`)"; a reentrant call arriving while that's
    // set just records the pending release and returns immediately -- one call frame, always.
    void unlock(std::size_t depth) noexcept {
        if (release_loan(depth)) return;  // ADR-237 §4.3
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
            // No home: inline, on behalf of its own holder id -- or, with a host resumer, handed to it.
            detail::wake(std::move(next));
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

    mutable std::mutex m_;
    bool held_ = false;
    bool draining_ = false;
    bool pending_release_ = false;
    std::deque<Waiter> waiters_;
    std::vector<LoanFrame> loans_;  // ADR-237 §4.3: the loan stack (back = the active loan)
    // ADR-175: the holder id the lock is granted to (resume_home.hpp); written under m_, read lock-free by
    // is_held_by_current_thread(). Holder ids start at 1, so 0 (free) never matches a running task.
    std::atomic<std::uint64_t> owner_{0};
    std::atomic<std::uint64_t> loans_granted_{0};
    std::atomic<std::uint64_t> refusals_{0};
};

}  // namespace agentengine::rt
