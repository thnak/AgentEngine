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
// Step 5 (§14) adds, report-only, the two watchdogs of §4.3, both off unless a bound is configured:
//   - LANE STALL: a lane worker that has not returned to its queue within `lane_stall_bound` (a body blocking a
//     lane: a `future::get`, a condition-variable wait, a slow sink) -- reported once per slice;
//   - STUCK RUN: a watched strand group (a Runtime root's "session") in which no slice has started or ended for
//     `stuck_run_bound` -- reported once per idle period. It sees what neither lock rule sees (a run parked forever
//     with no blocked thread to look at); a run legitimately parked longer than the bound (an approval) is reported
//     too, which is why it only reports.
// One watchdog thread (joined, never detached) scans both; its callbacks run on it, outside every scheduler lock.
// Task scopes and `when_all` are rt/scope.hpp; holder await chains rt/await_chain.hpp.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "agentengine/rt/await_chain.hpp"
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

// §4.3 watchdog reports (step 5). Report-only: nothing is stopped or resumed because of them.
// ae-naming-lint: allow LaneStall — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
struct LaneStall {
    std::size_t               lane   = 0;  // worker index
    std::uint64_t             strand = 0;  // the strand whose continuation is running there
    std::chrono::milliseconds running_for{};
};
// ae-naming-lint: allow StuckRun — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
struct StuckRun {
    std::uint64_t             group = 0;  // the strand group (Strand::group_id())
    std::chrono::milliseconds idle_for{};
};
// ae-naming-lint: allow WatchdogOptions — ADR-237 §4.3: new runtime vocabulary, 027 §4 row added when the ADR is Judged
struct WatchdogOptions {
    std::optional<std::chrono::milliseconds> lane_stall_bound{};  // unset: no lane-stall watchdog
    std::function<void(LaneStall const&)>    on_lane_stall{};     // called on the watchdog thread; must not throw
    std::optional<std::chrono::milliseconds> stuck_run_bound{};   // unset: no stuck-run watchdog
    std::function<void(StuckRun const&)>     on_stuck_run{};
};

namespace lanes_detail {

[[nodiscard]] inline std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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
    friend class ::agentengine::rt::Strand;
    friend class ::agentengine::rt::LanePool;
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
    explicit GroupCore(std::shared_ptr<Scheduler> sched) noexcept : sched_(std::move(sched)), id_(mint_group_id()) {}
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

private:
    [[nodiscard]] static std::uint64_t mint_group_id() noexcept {
        static std::atomic<std::uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    friend class Scheduler;
    friend class StrandCore;
    friend class ::agentengine::rt::StrandGroup;
    friend class ::agentengine::rt::Strand;
    std::shared_ptr<Scheduler>               sched_;
    std::uint64_t                            id_;
    std::deque<std::shared_ptr<StrandCore>> ready_;  // ready strands of this group, round-robin
    bool                                     in_ready_ = false;  // in the scheduler's ready_groups_
    // Stuck-run watchdog (§4.3, step 5). `last_progress_ns_`: when a slice of this group last started or ended
    // (stamped only while a watchdog is on). `watched_`/`listed_`: under the scheduler's mutex. `reported_for_`:
    // the watchdog thread's own, so that one idle period is reported once.
    std::atomic<std::int64_t> last_progress_ns_{0};
    std::size_t               watched_      = 0;
    bool                      listed_       = false;
    std::int64_t              reported_for_ = 0;
};

// The shared queue state of one lane pool. Outlives the pool's workers: a strand or a parked record holding a
// strand may outlive the pool, and a post after the workers stopped is dropped and counted, never run inline.
class Scheduler {
public:
    // `lanes`: worker count (one watchdog slot each). `timed`: stamp slice start/end times for the watchdogs.
    Scheduler(std::size_t lanes, bool timed)
        : slots_(std::make_unique<WorkerSlot[]>(lanes)), lanes_(lanes), timed_(timed) {}

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
    void run_worker(std::size_t index) {
        ScopedBlockOnRefusal const no_block_on(kRefusalCode);  // §4.3
        WorkerSlot&                slot = slots_[index];        // §4.3 lane-stall watchdog
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
            if (timed_) {  // §4.3 watchdogs: the slice starts
                std::int64_t const t = now_ns();
                slot.seq.fetch_add(1, std::memory_order_relaxed);
                slot.strand.store(s->id(), std::memory_order_relaxed);
                slot.start_ns.store(t, std::memory_order_release);
                s->group_->last_progress_ns_.store(t, std::memory_order_relaxed);
            }
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
            if (timed_) {  // the slice ended: the worker is back at its queue
                std::int64_t const t = now_ns();
                slot.start_ns.store(0, std::memory_order_release);
                s->group_->last_progress_ns_.store(t, std::memory_order_relaxed);
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

    // Stuck-run watchdog: a group with a root in flight is watched (counted: one group may carry several roots).
    void watch(std::shared_ptr<GroupCore> const& g) {
        std::lock_guard lock(m_);
        if (g->watched_++ == 0) g->last_progress_ns_.store(now_ns(), std::memory_order_relaxed);
        if (!g->listed_) {
            g->listed_ = true;
            watched_.push_back(g);
        }
    }
    void unwatch(GroupCore& g) noexcept {
        std::lock_guard lock(m_);
        if (g.watched_ > 0) --g.watched_;
    }

    // One watchdog pass (the watchdog thread only). Reports go to the callbacks after every lock is released.
    void scan(WatchdogOptions const& opts) {
        std::int64_t const     now = now_ns();
        std::vector<LaneStall> stalls;
        std::vector<StuckRun>  stuck;
        if (opts.lane_stall_bound) {
            std::int64_t const bound =
                std::chrono::duration_cast<std::chrono::nanoseconds>(*opts.lane_stall_bound).count();
            for (std::size_t i = 0; i < lanes_; ++i) {
                WorkerSlot&        w     = slots_[i];
                std::int64_t const start = w.start_ns.load(std::memory_order_acquire);
                if (start == 0 || now - start <= bound) continue;
                std::uint64_t const seq = w.seq.load(std::memory_order_relaxed);
                if (seq == w.reported_seq) continue;  // this slice was reported already
                w.reported_seq = seq;
                stalls.push_back(LaneStall{i, w.strand.load(std::memory_order_relaxed),
                                           std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::nanoseconds(now - start))});
            }
        }
        if (opts.stuck_run_bound) {
            std::int64_t const bound =
                std::chrono::duration_cast<std::chrono::nanoseconds>(*opts.stuck_run_bound).count();
            std::lock_guard lock(m_);
            for (auto it = watched_.begin(); it != watched_.end();) {
                GroupCore& g = **it;
                if (g.watched_ == 0) {
                    g.listed_ = false;
                    it        = watched_.erase(it);
                    continue;
                }
                std::int64_t const last = g.last_progress_ns_.load(std::memory_order_relaxed);
                if (now - last > bound && g.reported_for_ != last) {
                    g.reported_for_ = last;  // once per idle period
                    stuck.push_back(StuckRun{g.id_, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                        std::chrono::nanoseconds(now - last))});
                }
                ++it;
            }
        }
        for (LaneStall const& r : stalls) {
            lane_stalls_.fetch_add(1, std::memory_order_relaxed);
            if (opts.on_lane_stall) {
                try {
                    opts.on_lane_stall(r);
                } catch (...) {  // NOLINT(bugprone-empty-catch): a reporter must not stop the watchdog
                }
            }
        }
        for (StuckRun const& r : stuck) {
            stuck_runs_.fetch_add(1, std::memory_order_relaxed);
            if (opts.on_stuck_run) {
                try {
                    opts.on_stuck_run(r);
                } catch (...) {  // NOLINT(bugprone-empty-catch): a reporter must not stop the watchdog
                }
            }
        }
    }

    [[nodiscard]] std::uint64_t lane_stalls() const noexcept { return lane_stalls_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t stuck_runs() const noexcept { return stuck_runs_.load(std::memory_order_relaxed); }

private:
    // One per worker: what the lane-stall watchdog reads. `reported_seq` is the watchdog thread's own.
    struct WorkerSlot {
        std::atomic<std::int64_t>  start_ns{0};  // 0: the worker is at its queue, not in a slice
        std::atomic<std::uint64_t> seq{0};       // slices started by this worker
        std::atomic<std::uint64_t> strand{0};
        std::uint64_t              reported_seq = 0;
    };

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
    std::unique_ptr<WorkerSlot[]>            slots_;  // NOLINT(cppcoreguidelines-avoid-c-arrays): fixed at start
    std::size_t                              lanes_;
    bool                                     timed_;
    std::vector<std::shared_ptr<GroupCore>>  watched_;  // groups with a root in flight (pruned by scan)
    std::atomic<std::uint64_t>               lane_stalls_{0};
    std::atomic<std::uint64_t>               stuck_runs_{0};
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

    // ADR-237 §4.3 (step 5): a new strand in this strand's group -- a child strand of the same "session" for
    // scheduling (task scopes, rt/scope.hpp).
    [[nodiscard]] Strand sibling() const {
        return Strand(std::make_shared<lanes_detail::StrandCore>(core_->sched_, core_->group_));
    }
    // The id of this strand's group (StuckRun::group).
    [[nodiscard]] std::uint64_t group_id() const noexcept { return core_ ? core_->group_->id() : 0; }

    // The strand whose continuation the calling thread is running, or an empty handle.
    [[nodiscard]] static Strand current() {
        detail::StrandHome* const s = detail::current_execution().strand;
        if (s == nullptr) return Strand{};
        // No RTTI: StrandCore is the one type derived from StrandHome.
        return Strand(std::static_pointer_cast<lanes_detail::StrandCore>(s->shared_from_this()));
    }

private:
    friend class StrandGroup;
    friend class LanePool;
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
    explicit LanePool(std::size_t lanes, WatchdogOptions watchdog = {})
        : sched_(make_scheduler(lanes, watchdog)), watchdog_opts_(std::move(watchdog)) {
        workers_.reserve(lanes);
        try {
            for (std::size_t i = 0; i < lanes; ++i) {
                workers_.emplace_back([s = sched_, i] { s->run_worker(i); });
            }
            if (watchdog_opts_.lane_stall_bound || watchdog_opts_.stuck_run_bound) {
                watchdog_ = std::jthread([this](std::stop_token const& st) { watch_loop(st); });
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
        if (watchdog_.joinable()) {
            watchdog_.request_stop();
            watchdog_.join();  // never detached (§4.5 rule 1)
        }
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

    // §4.3 watchdogs (step 5): `strand`'s group is watched for a stuck run while a root runs in it.
    void watch_root(Strand const& strand) { sched_->watch(strand.core_->group_); }
    void unwatch_root(Strand const& strand) noexcept { sched_->unwatch(*strand.core_->group_); }
    // Reports so far.
    [[nodiscard]] std::uint64_t lane_stalls() const noexcept { return sched_->lane_stalls(); }
    [[nodiscard]] std::uint64_t stuck_runs() const noexcept { return sched_->stuck_runs(); }

private:
    [[nodiscard]] static std::shared_ptr<lanes_detail::Scheduler> make_scheduler(std::size_t lanes,
                                                                                WatchdogOptions const& w) {
        if (lanes == 0) throw std::invalid_argument("rt::LanePool: lanes must be at least 1");
        return std::make_shared<lanes_detail::Scheduler>(lanes, w.lane_stall_bound || w.stuck_run_bound);
    }

    // The scan period: a quarter of the tightest bound, between 1 ms and 50 ms.
    void watch_loop(std::stop_token const& st) {
        using std::chrono::milliseconds;
        milliseconds bound{1000};
        if (watchdog_opts_.lane_stall_bound) bound = std::min(bound, *watchdog_opts_.lane_stall_bound);
        if (watchdog_opts_.stuck_run_bound) bound = std::min(bound, *watchdog_opts_.stuck_run_bound);
        milliseconds const          period = std::clamp(bound / 4, milliseconds{1}, milliseconds{50});
        std::mutex                  m;
        std::condition_variable_any cv;
        std::unique_lock            lock(m);
        while (!st.stop_requested()) {
            (void)cv.wait_for(lock, st, period, [] { return false; });  // wakes on stop or after `period`
            if (st.stop_requested()) break;
            sched_->scan(watchdog_opts_);
        }
    }

    std::shared_ptr<lanes_detail::Scheduler> sched_;
    WatchdogOptions                          watchdog_opts_;
    std::vector<std::jthread>                workers_;
    std::jthread                             watchdog_;  // only when a watchdog bound is set
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
//
// Step 5 (§4.3, §4.5 rule 3): the child is a FORK of the awaiter's logical task -- a fresh holder id registered in
// the await chain with an always-awaited edge to the awaiter (rt/await_chain.hpp), and the awaiter's scope stop
// token. Destroying the awaiting frame before it was resumed (the child still running, or its wake already
// posted to a strand) is a checked violation: that is a non-root frame destroyed by its owner, which only
// bypassing structured concurrency can do.
template <class T>
class OnStrandAwaiter {
public:
    OnStrandAwaiter(Strand target, task<T> t) noexcept : target_(std::move(target)), task_(std::move(t)) {}
    OnStrandAwaiter(OnStrandAwaiter const&)            = delete;
    OnStrandAwaiter& operator=(OnStrandAwaiter const&) = delete;
    ~OnStrandAwaiter() {
        if (!started_ || resumed_) return;
        if (awaiter_strand_ || !finished_.load(std::memory_order_acquire)) {
            detail::checked_violation("a coroutine awaiting rt::on_strand was destroyed before its child joined "
                                      "(§4.5 rule 3)");
        }
        // A block_on/Resumer home whose root already finished: claim a continuation queued at a host Resumer.
        (void)detail::abandon_woken(ticket_keep_);
        (void)chain_detail::Registry::instance().remove(child_holder_);
    }

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
            ticket_keep_     = home_.ticket;
            link.on_done     = &OnStrandAwaiter::root_done;
            link.on_done_ctx = this;
        }
        std::uint64_t const parent = current_holder_id();
        child_holder_              = mint_holder_id();  // a fresh logical task on the target strand ...
        auto& chains               = chain_detail::Registry::instance();
        chains.add(child_holder_,  // ... forked from this one along an awaited edge (step 5)
                   chain_detail::Link{parent, &chain_detail::kAlwaysAwaited, chains.stop_of(parent), 0});
        started_ = true;
        target_.post(child, child_holder_);
    }

    T await_resume() {
        resumed_ = true;
        if (chain_detail::Registry::instance().remove(child_holder_) != 0) {
            detail::checked_violation("an AsyncMutex loan was still outstanding when its borrower (an rt::on_strand "
                                      "child) finished (§4.3)");
        }
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
        self->finished_.store(true, std::memory_order_release);
        detail::wake(std::move(r));  // `self` may be destroyed from here on
    }

    Strand                                 target_;
    task<T>                                task_;
    std::shared_ptr<detail::StrandHome>    awaiter_strand_;
    detail::ParkedResumer                  home_;
    std::shared_ptr<detail::ResumerTicket> ticket_keep_;
    std::uint64_t                          child_holder_ = 0;
    std::atomic<bool>                      finished_{false};  // home path: the root completion ran
    bool                                   started_ = false;
    bool                                   resumed_ = false;
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
