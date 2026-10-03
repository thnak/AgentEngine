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
//
// decisions/ADR-219 (issue #79) -- A HOST'S RESUMER. ADR-175 gave `block_on()` a home; a coroutine driven any
// other way stayed homeless and was resumed inline on the waker's thread. When AgentEngine runs inside
// another runtime, that waker can be a foreign thread (a Quark lane, a UI thread, an IOCP thread) that drops
// a `Guard` or pushes an item, and it then runs an AgentEngine continuation under its own scheduling rules.
// A host whose own executor drives AgentEngine coroutines -- by raw `task<T>::resume()` or by `co_await`ing
// an `rt::task` from its own coroutine type -- opens a `ScopedResumer` around that drive. A coroutine that
// parks inside the scope records the host's `Resumer`, and the waker hands the host a `ParkedContinuation`
// instead of resuming it. The resumer is a third kind of home, chosen by the driver exactly as ADR-175's
// rule says: per drive, not per mutex and not per lock() call. Unused, it adds one pointer to each parked
// record and one null test at park and at wake time (ADR-219 §6 measures about +10% on a chain of homeless
// hand-offs, nothing measurable elsewhere).
//
// decisions/ADR-237 §4.2/§4.3 (step 3) -- A STRAND. The fourth kind of home, and the one engine work uses: a
// lane worker (rt/lanes.hpp) runs each continuation of a strand under an execution context that names the
// strand, and a coroutine that parks during that slice records the strand; its waker posts it back to the
// strand -- never resumes it inline, never on the waker's thread. ADR-219 rejected "home in the promise"
// because leaf awaiters see only a type-erased `coroutine_handle<>`. The strand answers that for engine work
// without RTTI and without templating every awaiter: the only thing that resumes a strand's continuation is a
// lane worker, and it sets the context around exactly that resumption, so for the whole slice every
// coroutine running on that thread IS part of that strand's chain (the continuation and whatever it
// symmetrically transfers into). The context is therefore exact for engine work -- the residual ADR-219 §6
// describes (a host coroutine resumed by the host's own runtime, with no scope) cannot arise, because the
// "host runtime" of engine work is the lane pool. What the `rt::task` promise carries is the other half:
// the strand its AWAITER ran on, so `final_suspend` can tell a same-strand join from a cross-strand one
// (rt/task.hpp). Foreign coroutine types keep the thread-local ScopedResumer fallback, unchanged.
//
// PRECEDENCE when a coroutine parks (`capture_parked`), highest first:
//   1. strand        -- engine work on a lane. A ScopedResumer or raw `task<T>::resume()` opened INSIDE a
//                       strand slice keeps the strand: a continuation of a strand that came back through a
//                       host Resumer would run on a host thread concurrently with the strand's next
//                       continuation, breaking I1. So inside a slice the strand wins.
//   2. block_on home -- ADR-175. Cannot coexist with a strand (block_on is refused on lanes, §4.3); kept above
//                       the host Resumer as ADR-219 already decided ("a block_on() inside the scope still
//                       owns what parks under it").
//   3. host Resumer  -- ADR-219, the fallback for foreign coroutine types.
//   4. homeless      -- refused by reactor and offload wake-ups (§4.2); resumed inline by AsyncMutex/channel
//                       as before ADR-175.

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

class Resumer;

namespace detail {

class CallerHome;

// ADR-237 §4.3: a strand seen as a home -- where a parked engine coroutine is posted when it wakes. The one
// implementation is rt/lanes.hpp's strand core; this base exists so the wake path here needs no include of
// the scheduler (the same type-erasure level as `Resumer`: one indirect call per wake-up). `post` only
// enqueues; it never runs the coroutine on the caller's thread. Shared ownership: a parked record holds a
// strong reference, so a strand outlives every continuation queued for it.
class StrandHome : public std::enable_shared_from_this<StrandHome> {
public:
    virtual ~StrandHome() = default;
    // `priority`: a cancel/approval resume -- picked before other strands, never reordered within this one.
    virtual void post(std::coroutine_handle<> h, std::uint64_t holder, bool priority) noexcept = 0;
    // A process-unique id (diagnostic; 0 is never used).
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

    StrandHome(StrandHome const&)            = delete;
    StrandHome& operator=(StrandHome const&) = delete;

protected:
    StrandHome() noexcept : id_(mint_strand_id()) {}

private:
    [[nodiscard]] static std::uint64_t mint_strand_id() noexcept {
        static std::atomic<std::uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint64_t id_;
};

// ADR-219: what one park inside a ScopedResumer records -- allocated per park, only on that path, and shared
// by the parked record, the awaiter and the continuation. One allocation carries both:
//   - the host resumer the coroutine belongs to (owning: it outlives everything handed to it);
//   - self red-team finding 1's claim: who gets the continuation -- the resumer that runs it, or the
//     destruction of its coroutine frame while it waits in the host's queue (a host cancelling by dropping
//     the coroutine, ADR-017's house idiom). Exactly one wins.
struct ResumerTicket {
    static constexpr int kPending   = 0;
    static constexpr int kResumed   = 1;
    static constexpr int kAbandoned = 2;
    explicit ResumerTicket(std::shared_ptr<Resumer> r) noexcept : resumer(std::move(r)) {}
    std::shared_ptr<Resumer> const resumer;
    std::atomic<int>               state{kPending};
    [[nodiscard]] bool try_take(int to) noexcept {
        int expected = kPending;
        return state.compare_exchange_strong(expected, to, std::memory_order_acq_rel);
    }
};

// Owned by one `block_on()` call, on its stack; `home` is created on the first park, by the thread that
// owns the call. Read from another thread only by a completion signal that came through a foreign
// (non-homing) awaitable -- after that thread's own write, which the hand-off orders before it.
struct HomeSlot {
    std::shared_ptr<CallerHome> home;
};

struct ExecutionContext {
    HomeSlot*     slot   = nullptr;  // the block_on() currently driving this thread's coroutine, if any
    std::uint64_t holder = 0;        // 0: use the thread's ambient id
    // ADR-219: the host resumer driving this thread's coroutine, if any. Points at a shared_ptr owned by
    // the enclosing ScopedResumer or ParkedContinuation::resume() frame, which outlives every use. Ignored
    // while `slot` is set: a block_on() inside a host drive owns what parks under it.
    std::shared_ptr<Resumer> const* resumer = nullptr;
    // ADR-237 §4.3: the strand whose continuation this thread is running -- set only by a lane worker, and
    // kept by scopes nested inside its slice. Highest precedence (file comment). The lane worker holds a
    // strong reference for the duration of the slice.
    StrandHome* strand = nullptr;
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
// (nullptr: homeless, or to `resumer` if one is given -- ADR-219; to `strand`, above both -- ADR-237 §4.3).
// The previous context is restored on destruction.
// ae-naming-lint: allow ScopedExecution — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class ScopedExecution {
public:
    ScopedExecution(detail::HomeSlot* slot, std::uint64_t holder,
                    std::shared_ptr<Resumer> const* resumer = nullptr,
                    detail::StrandHome* strand = nullptr) noexcept
        : saved_(std::exchange(detail::current_execution(),
                               detail::ExecutionContext{slot, holder, resumer, strand})) {}
    ~ScopedExecution() { detail::current_execution() = saved_; }
    ScopedExecution(ScopedExecution const&) = delete;
    ScopedExecution& operator=(ScopedExecution const&) = delete;

private:
    detail::ExecutionContext saved_;
};

namespace detail {
struct ParkedResumer;
inline void hand_to_resumer(ParkedResumer r) noexcept;
}  // namespace detail

// ADR-219: a parked AgentEngine coroutine handed to a host `Resumer` -- the continuation of a lock grant or
// a channel wake-up. Move-only, and resumed AT MOST ONCE by construction -- exactly once unless its
// coroutine is destroyed first:
//   - `resume()` runs it on the calling thread, under the holder id it parked with and with the same
//     resumer, so a coroutine that parks again is handed back to the same host resumer;
//   - destroying a continuation that was never resumed resumes it inline, on the destroying thread. A
//     continuation a host drops (shutdown, a full queue, an exception while queuing) is therefore never
//     lost: an AsyncMutex waiter has already been granted the lock, and the lock can only be released by
//     running it. Running it late on an unexpected thread is the fallback; holding the lock forever is not.
//   - if the coroutine's frame is destroyed while its continuation waits in the host's queue (the host
//     cancelled by dropping the coroutine), the parked awaiter's destructor claims the continuation first:
//     a lock already granted to it is released there, and `resume()` becomes a no-op that never touches
//     the destroyed frame. Destroying the frame CONCURRENTLY with `resume()` running is still undefined,
//     as destroying any coroutine while another thread resumes it is.
// Not thread-safe: one owner at a time, like any move-only handle.
class ParkedContinuation {
public:
    ParkedContinuation() noexcept = default;
    ParkedContinuation(ParkedContinuation&& o) noexcept
        : handle_(std::exchange(o.handle_, {})), holder_(o.holder_), ticket_(std::move(o.ticket_)) {}
    ParkedContinuation& operator=(ParkedContinuation&& o) noexcept {
        if (this != &o) {
            resume();  // never silently overwrite a pending continuation
            handle_ = std::exchange(o.handle_, {});
            holder_ = o.holder_;
            ticket_ = std::move(o.ticket_);
        }
        return *this;
    }
    ParkedContinuation(ParkedContinuation const&)            = delete;
    ParkedContinuation& operator=(ParkedContinuation const&) = delete;
    ~ParkedContinuation() { resume(); }

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(handle_); }
    // The holder id the coroutine runs under when resumed (diagnostic).
    [[nodiscard]] std::uint64_t holder() const noexcept { return holder_; }

    // Resumes the coroutine on the calling thread; a no-op on an empty or already-resumed continuation, or
    // one whose coroutine was destroyed while it waited.
    void resume() noexcept {
        if (!handle_) return;
        std::coroutine_handle<> const               h      = std::exchange(handle_, {});
        std::shared_ptr<detail::ResumerTicket> const ticket = std::move(ticket_);  // alive while it may re-park
        if (!ticket->try_take(detail::ResumerTicket::kResumed)) return;            // its frame was destroyed
        ScopedExecution const context(nullptr, holder_, &ticket->resumer);
        h.resume();
    }

private:
    friend void detail::hand_to_resumer(detail::ParkedResumer r) noexcept;
    ParkedContinuation(std::coroutine_handle<> h, std::uint64_t holder,
                       std::shared_ptr<detail::ResumerTicket> ticket) noexcept
        : handle_(h), holder_(holder), ticket_(std::move(ticket)) {}

    std::coroutine_handle<>                handle_{};
    std::uint64_t                          holder_ = 0;
    std::shared_ptr<detail::ResumerTicket> ticket_{};  // never null while handle_ is set
};

// ADR-219: the host's side of the hand-off -- where a parked AgentEngine coroutine goes when the thing it
// waited for happens. Implemented by a host that drives AgentEngine coroutines from its own executor.
//
// CONTRACT for `post()`:
//   - It is called on the WAKER's thread (whoever dropped the Guard, pushed the item, closed or cancelled
//     the channel), with the waking primitive's own lock released -- though the waker may hold locks of
//     its own. It should only enqueue: anything it runs inline runs on the waker's thread, which is exactly
//     what this type exists to avoid. It must not block waiting for its executor to drain: the waker may
//     BE that executor (a continuation that releases a lock wakes the next waiter from inside resume()).
//   - It takes ownership of `c` and should arrange for `c.resume()` to be called once, on a thread of the
//     host's choosing. If it cannot (shutting down, queue full), it drops `c`, which resumes it inline on
//     the thread that drops it -- see ParkedContinuation. If it throws, the waker swallows the exception
//     and the continuation, wherever it ended up, is still resumed when destroyed.
//   - Until `c` is resumed, a granted AsyncMutex stays held. A resumer that sits on a continuation stalls
//     every later waiter on that mutex, as any holder that does not release does.
//   - A host may destroy a parked coroutine (drop it to cancel) while its continuation is queued: the
//     continuation is then claimed by the destruction and its resume() does nothing. It must not destroy
//     the objects a parked coroutine refers to (the AsyncMutex, the session, the channel) first, the same
//     order it needs for any pending work.
// Lifetime: shared ownership. Every parked record and every continuation holds a shared_ptr, so the
// resumer object outlives everything handed to it; what it forwards to (a host executor) is the host's
// to keep alive, or to detect gone and drop.
class Resumer {
public:
    virtual ~Resumer() = default;
    virtual void post(ParkedContinuation c) = 0;

protected:
    Resumer() noexcept                 = default;
    Resumer(Resumer const&)            = default;
    Resumer& operator=(Resumer const&) = default;
};

// ADR-219: RAII -- AgentEngine coroutines resumed on this thread until destruction (raw `task<T>::resume()`,
// or an `rt::task` co_awaited from a host coroutine) are driven by `resumer`: a coroutine that parks on an
// AsyncMutex or a channel inside this scope is handed to `resumer->post()` when woken, never resumed on
// the waker's thread. Runs under a FRESH holder id by default -- one scope per drive of one logical task
// (a scope shared by unrelated coroutines would make them one holder to `is_held_by_current_thread()`).
// A `block_on()` inside the scope still owns what parks under it. A null `resumer` is a homeless scope.
//
// The resumer lives in the THREAD's context, not the coroutine (ADR-175 §6's foreign-awaitable residual): a
// host coroutine that the host's own runtime resumes after a host awaitable runs with no scope, and parks
// homeless again. A host executor closes that by opening the scope around every resumption it performs,
// passing a holder id it keeps per host task (`mint_holder_id()` once, then the two-argument form). Inside
// an engine round (a tool closure), pass `current_holder_id()` to stay the round's holder.
class ScopedResumer {
public:
    explicit ScopedResumer(std::shared_ptr<Resumer> resumer)
        : ScopedResumer(std::move(resumer), mint_holder_id()) {}
    // ADR-237 §4.3: opened inside a strand slice, the strand is kept and takes precedence (file comment).
    ScopedResumer(std::shared_ptr<Resumer> resumer, std::uint64_t holder) noexcept
        : resumer_(std::move(resumer)), scope_(nullptr, holder, &resumer_, detail::current_execution().strand) {}
    ScopedResumer(ScopedResumer const&)            = delete;
    ScopedResumer& operator=(ScopedResumer const&) = delete;

private:
    std::shared_ptr<Resumer> resumer_;  // declared first: scope_ points at it
    ScopedExecution          scope_;
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
    std::shared_ptr<ResumerTicket> ticket{};  // ADR-219: set only when `home` is not
    std::shared_ptr<StrandHome>    strand{};  // ADR-237 §4.3: when set, the only home set

    // False for a homeless record, which a waker that must not run it inline (the reactor thread, an offload
    // worker) refuses (ADR-237 §4.2).
    [[nodiscard]] bool homed() const noexcept { return strand || home || ticket; }
};

// Called by an awaiter's await_suspend() BEFORE it registers anything: the allocation of a first-park home
// (or of an ADR-219 ticket) may throw, and an exception from await_suspend() is only safe while nothing yet
// refers to the handle. Runs on the thread executing the coroutine, which is the thread that owns `slot`.
// Precedence (file comment): strand > block_on home > host Resumer > homeless.
[[nodiscard]] inline ParkedResumer capture_parked(std::coroutine_handle<> h) {
    ExecutionContext const& ctx = current_execution();
    ParkedResumer r{h, {}, current_holder_id(), {}, {}};
    if (ctx.strand != nullptr) {
        r.strand = ctx.strand->shared_from_this();  // ADR-237 §4.3: engine work goes back to its strand
    } else if (ctx.slot != nullptr) {
        if (!ctx.slot->home) ctx.slot->home = std::make_shared<CallerHome>();
        r.home = ctx.slot->home;
    } else if (ctx.resumer != nullptr && *ctx.resumer) {
        r.ticket = std::make_shared<ResumerTicket>(*ctx.resumer);  // ADR-219: the host drive it belongs to
    }
    return r;
}

// ADR-219, for an awaiter's destructor running while it is still parked (its coroutine destroyed) and its
// record is no longer in the primitive's queue -- i.e. already woken, its continuation on its way to or in
// the host's queue. True if the destruction claimed the continuation (it will never run), so the awaiter
// must undo what the wake-up granted; false if there is no claim (a homed or homeless record, ADR-175 §6's
// residual) or the continuation already started running.
[[nodiscard]] inline bool abandon_woken(std::shared_ptr<ResumerTicket> const& ticket) noexcept {
    return ticket && ticket->try_take(ResumerTicket::kAbandoned);
}

// ADR-219: hands a woken record to its host resumer, outside every engine lock. The local shared_ptr keeps
// the resumer alive through post() -- which may drop the continuation, and with it the ticket, before it
// returns. Whatever post() does with the continuation -- queues it, drops it, throws -- it is resumed at
// most once, and exactly once unless its frame is destroyed first (ParkedContinuation).
inline void hand_to_resumer(ParkedResumer r) noexcept {
    std::shared_ptr<Resumer> const target = r.ticket->resumer;
    ParkedContinuation             c(r.handle, r.holder, std::move(r.ticket));
    try {
        target->post(std::move(c));
    } catch (...) {  // NOLINT(bugprone-empty-catch): the continuation's destructor already resumed it
    }
}

// Called by a waker AFTER releasing its primitive's lock. A strand-homed coroutine is posted to its strand
// (ADR-237 §4.3; only enqueues); a homed coroutine is posted to its home (which resumes it inline if it has
// closed); one a host resumer drives is handed to that resumer (ADR-219); a homeless one is resumed inline as
// before ADR-175, on behalf of its own recorded holder and with no home, so it neither adopts the waker's
// identity nor parks back to the waker's block_on().
inline void wake(ParkedResumer r) noexcept {
    if (!r.handle) return;
    if (r.strand) {
        std::shared_ptr<StrandHome> const strand = std::move(r.strand);
        strand->post(r.handle, r.holder, false);
        return;
    }
    if (r.home) {
        std::shared_ptr<CallerHome> const home = std::move(r.home);
        home->post(r.handle, r.holder);
        return;
    }
    if (r.ticket) {
        hand_to_resumer(std::move(r));
        return;
    }
    ScopedExecution const homeless(nullptr, r.holder);
    r.handle.resume();
}

}  // namespace detail

}  // namespace agentengine::rt
