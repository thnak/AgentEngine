#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §4.1 ("Lane worker"), §4.3 (lanes, I1
// and the session strand; scheduling) and §4.2 (a strand is the home of engine work) -- step 3 of the ADR's
// implementation order (§14).
//
//   LANE POOL. A small, fixed set of `std::jthread` workers -- joined, never detached (§4.5 rule 1) -- that run
//   READY STRANDS. A parked coroutine is on no lane: it costs its frame and its operation record (023). Each
//   worker carries `ScopedBlockOnRefusal("rt.block_on_on_lane")` for its whole life (§4.3): `block_on()` on a
//   lane would park a worker waiting for work that may need that worker.
//
//   STRAND. A serial view of the lanes: at most one continuation of a strand is runnable at a time, in FIFO
//   order. A worker takes ONE continuation of a strand, runs it until it parks or finishes (a body cannot be
//   preempted), and only then may the strand's next continuation run -- on whichever worker picks it up
//   (nothing may depend on thread identity; holder ids are per logical task, ADR-175). While it runs, the
//   worker's execution context names the strand, so whatever parks in that slice records the strand as its
//   home and is posted back to it (rt/resume_home.hpp). This makes I1 structural for continuations; the
//   session's AsyncMutex stays the authority for entry points (§4.3).
//
//   STRAND GROUP. The scheduling unit "session" (§4.3): a session's strand and its children's strands (a
//   parallel tool batch's `when_all` children) belong to one group. Scheduling, revised after rounds 2 and 3:
//     - round-robin across GROUPS first, then across the group's ready strands -- a 50-child batch gets its
//       session's share, not 50 shares (R3-8);
//     - strictly FIFO within a strand;
//     - a PRIORITY post (a cancel or approval resume) makes its strand picked before every other ready strand,
//       across groups, but is queued behind the strand's earlier continuations -- it never reorders a strand
//       (that would break I1's ordering).
//
// One mutex guards the whole scheduler. That is the simple, obviously-correct first version this step's
// claims are tested on; a sharded or lock-free queue is a later, benchmarked change (§8.2), not this one.
//
// Not here yet (later steps): the lane-stall and stuck-run watchdogs (§4.3), task scopes and `when_all`
// (§4.5), holder await chains (§4.3).

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/task.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow LanePool — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class LanePool;
// ae-naming-lint: allow StrandGroup — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class StrandGroup;
// ae-naming-lint: allow Strand — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class Strand;

namespace lanes_detail {

inline constexpr char const kRefusalCode[] = "rt.block_on_on_lane";

class Scheduler;
class GroupCore;

// One continuation waiting on a strand.
struct Item {
    std::coroutine_handle<> handle{};
    std::uint64_t           holder   = 0;
    bool                    priority = false;
};

// A strand. Its fields are guarded by the scheduler's mutex.
class StrandCore final : public detail::StrandHome {
public:
    StrandCore(std::shared_ptr<Scheduler> sched, std::shared_ptr<GroupCore> group) noexcept
        : sched_(std::move(sched)), group_(std::move(group)) {}

    void post(std::coroutine_handle<> h, std::uint64_t holder, bool priority) noexcept override;

private:
    [[nodiscard]] std::shared_ptr<StrandCore> shared_from_this_core() {
        return std::static_pointer_cast<StrandCore>(shared_from_this());  // no RTTI: the one derived type
    }

    friend class Scheduler;
    std::shared_ptr<Scheduler> sched_;
    std::shared_ptr<GroupCore> group_;
    std::deque<Item>           queue_;
    std::size_t                priority_items_ = 0;  // priority items still queued
    bool                       running_        = false;
    bool                       in_ready_       = false;  // in group_->ready_
    bool                       in_priority_    = false;  // in the scheduler's priority list
};

// A strand group ("session"). Its fields are guarded by the scheduler's mutex.
class GroupCore {
public:
    explicit GroupCore(std::shared_ptr<Scheduler> sched) noexcept : sched_(std::move(sched)) {}

private:
    friend class Scheduler;
    friend class StrandCore;
    friend class ::agentengine::rt::StrandGroup;
    std::shared_ptr<Scheduler>               sched_;
    std::deque<std::shared_ptr<StrandCore>> ready_;  // ready strands of this group, round-robin
    bool                                     in_ready_ = false;  // in the scheduler's ready_groups_
};

// The shared queue state of one lane pool. Outlives the pool's workers: a strand or a parked record holding a
// strand may outlive the pool, and a post after the workers stopped is dropped and counted, never run inline.
class Scheduler {
public:
    // Enqueues `item` on `s`; makes `s` ready if it is neither running nor ready.
    void post(StrandCore& s, Item item) noexcept {
        {
            std::lock_guard lock(m_);
            if (stopped_) {
                // The workers are gone: nobody will run it, and running it here would run engine code on the
                // posting thread (possibly the reactor). Dropped -- the frame is never resumed -- and counted.
                dropped_after_stop_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (item.priority) ++s.priority_items_;
            s.queue_.push_back(item);
            if (s.running_) return;  // re-readied when its running continuation returns
            if (!s.in_ready_) {
                make_ready_locked(s);
            } else if (s.priority_items_ > 0 && !s.in_priority_) {
                priority_.push_back(s.shared_from_this_core());
                s.in_priority_ = true;
            }
        }
        work_cv_.notify_one();
    }

    // Worker loop. Runs until stop() and no work is left.
    void run_worker() {
        ScopedBlockOnRefusal const no_block_on(kRefusalCode);  // §4.3
        std::unique_lock           lock(m_);
        for (;;) {
            std::shared_ptr<StrandCore> s = pick_locked();
            if (!s) {
                if (stopped_) return;
                if (stopping_ && running_ == 0) {
                    // Nothing ready and nothing running that could post more: from now on posts are dropped.
                    stopped_ = true;
                    work_cv_.notify_all();
                    return;
                }
                work_cv_.wait(lock);
                continue;
            }
            Item const item = s->queue_.front();
            s->queue_.pop_front();
            s->running_ = true;
            ++running_;
            lock.unlock();
            {
                // The slice: everything that parks while it runs records `s` as its home (rt/resume_home.hpp).
                ScopedExecution const slice(nullptr, item.holder, nullptr, s.get());
                try {
                    item.handle.resume();
                } catch (...) {
                    // An rt::task never throws out of resume() (its promise catches); a foreign coroutine type
                    // that does has left its frame in an unknown state. Not survivable.
                    std::fputs("agentengine: an exception escaped a coroutine resumed on a lane (ADR-237 §4.3)\n",
                               stderr);
                    std::terminate();
                }
            }
            lock.lock();
            --running_;
            slices_.fetch_add(1, std::memory_order_relaxed);
            s->running_ = false;
            if (item.priority) --s->priority_items_;
            if (!s->queue_.empty()) make_ready_locked(*s);
            if (stopping_ && running_ == 0) work_cv_.notify_all();  // let idle workers re-check for exit
        }
    }

    // Workers finish every queued continuation (including ones posted while draining), then exit.
    void begin_stop() noexcept {
        {
            std::lock_guard lock(m_);
            stopping_ = true;
        }
        work_cv_.notify_all();
    }
    // After the workers are joined: later posts are dropped (the last worker already set it; idempotent).
    void mark_stopped() noexcept {
        std::lock_guard lock(m_);
        stopped_ = true;
    }

    [[nodiscard]] std::uint64_t slices() const noexcept { return slices_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t dropped_after_stop() const noexcept {
        return dropped_after_stop_.load(std::memory_order_relaxed);
    }

private:
    void make_ready_locked(StrandCore& s) {
        std::shared_ptr<StrandCore> sp = s.shared_from_this_core();
        GroupCore&                  g  = *s.group_;
        s.in_ready_                    = true;
        g.ready_.push_back(sp);
        if (!g.in_ready_) {
            g.in_ready_ = true;
            ready_groups_.push_back(s.group_);
        }
        if (s.priority_items_ > 0 && !s.in_priority_) {
            s.in_priority_ = true;
            priority_.push_back(std::move(sp));
        }
    }

    // Removes `s` from its group's ready list (and the group from the ready groups if it empties).
    void unready_locked(StrandCore& s) {
        GroupCore& g = *s.group_;
        auto const it = std::find_if(g.ready_.begin(), g.ready_.end(),
                                     [&](std::shared_ptr<StrandCore> const& p) { return p.get() == &s; });
        if (it != g.ready_.end()) g.ready_.erase(it);
        s.in_ready_ = false;
        if (g.ready_.empty() && g.in_ready_) {
            g.in_ready_   = false;
            auto const gi = std::find_if(ready_groups_.begin(), ready_groups_.end(),
                                         [&](std::shared_ptr<GroupCore> const& p) { return p.get() == &g; });
            if (gi != ready_groups_.end()) ready_groups_.erase(gi);
        }
    }

    // §4.3: a strand with a pending priority item first; otherwise the next group round-robin, then that group's
    // next strand round-robin.
    [[nodiscard]] std::shared_ptr<StrandCore> pick_locked() {
        while (!priority_.empty()) {
            std::shared_ptr<StrandCore> s = std::move(priority_.front());
            priority_.pop_front();
            s->in_priority_ = false;
            if (!s->in_ready_ || s->priority_items_ == 0) continue;  // running, or already served
            unready_locked(*s);
            return s;
        }
        if (ready_groups_.empty()) return {};
        std::shared_ptr<GroupCore> g = std::move(ready_groups_.front());
        ready_groups_.pop_front();
        std::shared_ptr<StrandCore> s = std::move(g->ready_.front());
        g->ready_.pop_front();
        s->in_ready_ = false;
        if (!g->ready_.empty()) {
            ready_groups_.push_back(std::move(g));  // still ready: back of the round-robin
        } else {
            g->in_ready_ = false;
        }
        return s;
    }

    std::mutex                               m_;
    std::condition_variable                  work_cv_;
    std::deque<std::shared_ptr<GroupCore>>   ready_groups_;
    std::deque<std::shared_ptr<StrandCore>>  priority_;
    std::size_t                              running_  = 0;
    bool                                     stopping_ = false;
    bool                                     stopped_  = false;
    std::atomic<std::uint64_t>               slices_{0};
    std::atomic<std::uint64_t>               dropped_after_stop_{0};
};

inline void StrandCore::post(std::coroutine_handle<> h, std::uint64_t holder, bool priority) noexcept {
    sched_->post(*this, Item{h, holder != 0 ? holder : mint_holder_id(), priority});
}

}  // namespace lanes_detail

// A strand: the serial home of one logical line of engine work (a session's, or one parallel child's). A
// copyable handle; the strand lives while any handle, ready entry or parked continuation refers to it.
// ae-naming-lint: allow Strand — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class Strand {
public:
    Strand() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(core_); }
    [[nodiscard]] std::uint64_t id() const noexcept { return core_ ? core_->id() : 0; }
    // True while the calling thread is running one of this strand's continuations.
    [[nodiscard]] bool is_current() const noexcept {
        return core_ && detail::current_execution().strand == core_.get();
    }

    // Queues `h` to run on this strand, after every continuation already queued on it. `priority`: a cancel or
    // approval resume -- this strand is picked before other ready strands (§4.3). Only enqueues. `holder`: the
    // holder id it runs under (0: a fresh one).
    void post(std::coroutine_handle<> h, std::uint64_t holder = 0, bool priority = false) const noexcept {
        core_->post(h, holder, priority);
    }

    // The underlying home (rt::Runtime, rt::on_strand).
    [[nodiscard]] std::shared_ptr<detail::StrandHome> home() const noexcept { return core_; }

private:
    friend class StrandGroup;
    explicit Strand(std::shared_ptr<lanes_detail::StrandCore> core) noexcept : core_(std::move(core)) {}
    std::shared_ptr<lanes_detail::StrandCore> core_;
};

// A strand group: one "session" for round-robin scheduling (§4.3). Strands made from it share its share.
// ae-naming-lint: allow StrandGroup — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class StrandGroup {
public:
    StrandGroup() noexcept = default;
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(core_); }
    [[nodiscard]] Strand new_strand() const {
        return Strand(std::make_shared<lanes_detail::StrandCore>(core_->sched_, core_));
    }

private:
    friend class LanePool;
    explicit StrandGroup(std::shared_ptr<lanes_detail::GroupCore> core) noexcept : core_(std::move(core)) {}
    std::shared_ptr<lanes_detail::GroupCore> core_;
};

// The lane workers (§4.1). Destruction drains every queued continuation and joins every worker.
// ae-naming-lint: allow LanePool — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
class LanePool {
public:
    explicit LanePool(std::size_t lanes) : sched_(std::make_shared<lanes_detail::Scheduler>()) {
        if (lanes == 0) throw std::invalid_argument("rt::LanePool: lanes must be at least 1");
        workers_.reserve(lanes);
        try {
            for (std::size_t i = 0; i < lanes; ++i) {
                workers_.emplace_back([s = sched_] { s->run_worker(); });
            }
        } catch (...) {
            stop();
            throw;
        }
    }
    ~LanePool() { stop(); }
    LanePool(LanePool const&)            = delete;
    LanePool& operator=(LanePool const&) = delete;

    // Runs every queued continuation (and whatever they post while draining), then joins the workers. A post
    // after that is dropped and counted (`dropped_after_stop`). Idempotent; must not be called from a lane.
    void stop() noexcept {
        sched_->begin_stop();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();  // never detached (§4.5 rule 1)
        }
        sched_->mark_stopped();
    }

    [[nodiscard]] StrandGroup new_group() const {
        return StrandGroup(std::make_shared<lanes_detail::GroupCore>(sched_));
    }
    [[nodiscard]] std::size_t lanes() const noexcept { return workers_.size(); }
    // Diagnostics: continuations run; posts dropped because the pool had stopped.
    [[nodiscard]] std::uint64_t slices() const noexcept { return sched_->slices(); }
    [[nodiscard]] std::uint64_t dropped_after_stop() const noexcept { return sched_->dropped_after_stop(); }

private:
    std::shared_ptr<lanes_detail::Scheduler> sched_;
    std::vector<std::jthread>                workers_;
};

// The id of the strand whose continuation the calling thread is running, or 0.
[[nodiscard]] inline std::uint64_t current_strand_id() noexcept {
    detail::StrandHome const* s = detail::current_execution().strand;
    return s != nullptr ? s->id() : 0;
}

namespace lanes_detail {

// `co_await rt::reschedule()`: park and be posted straight back to the current home -- behind every
// continuation already queued on the strand. On a homeless thread it does not suspend.
struct RescheduleAwaiter {
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) {
        detail::ParkedResumer r = detail::capture_parked(h);
        if (!r.homed()) return false;
        detail::wake(std::move(r));  // only enqueues for a strand or block_on home
        return true;
    }
    void await_resume() const noexcept {}
};

// `co_await rt::on_strand(strand, task)`: runs `task` as a continuation of `strand` and resumes the awaiter on
// ITS home. From a strand, the child's final_suspend sees two different strands and posts (rt/task.hpp); from
// a block_on() or a host Resumer, the awaiter's captured home is woken by the root completion.
template <class T>
class OnStrandAwaiter {
public:
    OnStrandAwaiter(Strand target, task<T> t) noexcept : target_(std::move(target)), task_(std::move(t)) {}
    OnStrandAwaiter(OnStrandAwaiter const&)            = delete;
    OnStrandAwaiter& operator=(OnStrandAwaiter const&) = delete;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        auto  child = detail::TaskAccess::handle(task_);
        auto& link  = child.promise().link_;
        detail::StrandHome* const here = detail::current_execution().strand;
        if (here != nullptr) {
            // The awaiter is a continuation of `here`: the child's final_suspend posts it back there.
            awaiter_strand_ = here->shared_from_this();  // keeps it alive until the join
            child.promise().continuation_ = h;
            link.awaiter_strand           = here;
            link.awaiter_holder           = current_holder_id();
        } else {
            home_ = detail::capture_parked(h);  // block_on home or host Resumer (precedence: resume_home.hpp)
            if (!home_.homed()) {
                // Resuming it inline from a lane would run it on the lane: refused before suspending.
                throw std::logic_error("rt.on_strand_homeless: co_await rt::on_strand from a coroutine with no "
                                       "home (ADR-237 §4.2)");
            }
            link.on_done     = &OnStrandAwaiter::root_done;
            link.on_done_ctx = this;
        }
        target_.post(child, mint_holder_id());  // a fresh logical task on the target strand
    }

    T await_resume() {
        auto child = detail::TaskAccess::handle(task_);
        if (child.promise().fault_) std::rethrow_exception(child.promise().fault_);
        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return std::move(*child.promise().value_ptr());
        }
    }

private:
    static void root_done(void* ctx) noexcept {
        auto*                 self = static_cast<OnStrandAwaiter*>(ctx);
        detail::ParkedResumer r    = std::move(self->home_);
        detail::wake(std::move(r));  // `self` may be destroyed from here on
    }

    Strand                              target_;
    task<T>                             task_;
    std::shared_ptr<detail::StrandHome> awaiter_strand_;
    detail::ParkedResumer               home_;
};

}  // namespace lanes_detail

// ae-naming-lint: allow reschedule — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
[[nodiscard]] inline lanes_detail::RescheduleAwaiter reschedule() noexcept { return {}; }

// ae-naming-lint: allow on_strand — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
template <class T>
[[nodiscard]] lanes_detail::OnStrandAwaiter<T> on_strand(Strand const& target, task<T> t) noexcept {
    return lanes_detail::OnStrandAwaiter<T>(target, std::move(t));
}

}  // namespace agentengine::rt
