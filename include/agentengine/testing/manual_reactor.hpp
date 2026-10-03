#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §8.1 ("a test reactor:
// single-threaded, manual run, virtual clock") and §4.7 (a virtual timer queue so unit tests run without wall
// time) -- `testing::ManualReactor`, a `pal::Reactor` that does nothing until the test drives it.
//
// THE MODEL. Every call that the real reactor would post to its reactor thread (`start_timer`, `cancel`) is
// only QUEUED here, from any thread. Nothing happens until the test calls, on one thread at a time:
//   - `run_ready()`   -- processes the queued starts and cancels in FIFO order, then completes every started
//                        timer whose deadline is <= `now()`;
//   - `advance(d)` / `advance_to(t)` -- `run_ready()`, then moves the VIRTUAL clock forward, completing each
//                        timer that falls due in deadline order (ties in start order) with `now()` set to its
//                        deadline, processing whatever those completions queued as it goes.
// For the duration of each of these calls the calling thread IS the reactor thread: `on_reactor_thread()`
// is true on it (and false everywhere else, always), and every `ReactorOp::on_complete` runs on it -- so the
// "the reactor never runs engine code" rule (§4.2) is testable without threads.
//
// Same contract as the production backend (pal/reactor.hpp): a start whose op already carries the sticky
// cancel flag completes `canceled` immediately (round-2 M2); a cancel of a pending timer completes it
// `canceled`; a cancel of an op that already completed does nothing; each started op completes exactly once.
// Destroying the reactor is shutdown (§4.5 rule 4): queued starts and every pending timer complete `canceled`
// on the destroying thread before the destructor returns.
//
// LIMITS -- read before relying on it:
//   - Single-threaded by construction, so it cannot show strand races or cancel/complete races between
//     threads (§8.1). Every concurrency claim must ALSO run against `pal::make_default_reactor()` (and under
//     TSan); this reactor proves ordering and logic, not thread safety.
//   - `rt::sleep_until`'s await_ready shortcut compares the deadline with the REAL `steady_clock::now()`, not
//     with this reactor's virtual `now()`. The virtual clock therefore starts at the real `now()` at
//     construction (unless told otherwise) and only moves forward, so a deadline written as
//     `reactor.now() + d` (d > 0) is in the real future too and the shortcut never fires early. Use
//     `sleep_until(reactor, reactor.now() + d)`, not `sleep_for(reactor, d)`: `sleep_for` adds `d` to the
//     real clock, which drifts from the virtual one as soon as the test advances it.
//   - One driver at a time: `run_ready`/`advance*` must not be called concurrently or re-entered from an
//     `on_complete` (checked: it throws `std::logic_error`).
//
// Not a production type: lives under `testing/`; nothing under `core/` or `rt/` includes it.

#include <atomic>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/pal/reactor.hpp"

namespace agentengine::testing {

// ae-naming-lint: allow ManualReactor — ADR-237 §8.1: testing vocabulary
class ManualReactor final : public pal::Reactor {
public:
    using time_point = clock::time_point;
    using duration   = clock::duration;

    ManualReactor() : ManualReactor(clock::now()) {}
    explicit ManualReactor(time_point start) : now_(start) {}

    // Shutdown: every queued start and every pending timer completes `canceled`, on this thread.
    ~ManualReactor() override {
        DriveScope const drive(*this);
        shutting_down_ = true;
        process_posted();
        while (!timers_.empty()) {
            auto node = timers_.extract(timers_.begin());
            node.mapped()->on_complete(pal::op_status::canceled);
            process_posted();  // a completion may queue more; they are canceled too
        }
    }

    ManualReactor(ManualReactor const&)            = delete;
    ManualReactor& operator=(ManualReactor const&) = delete;

    // Thread-safe: queues the start. It takes effect at the next run_ready()/advance*().
    void start_timer(std::shared_ptr<pal::ReactorOp> op, time_point deadline) override {
        std::lock_guard lock(m_);
        posted_.push_back(Posted{std::move(op), deadline, false});
    }

    [[nodiscard]] bool on_reactor_thread() const noexcept override {
        return driver_.load(std::memory_order_acquire) == std::this_thread::get_id();
    }

    // The virtual clock.
    [[nodiscard]] time_point now() const noexcept { return now_.load(std::memory_order_acquire); }

    // Processes queued starts and cancels, then completes the timers already due. Returns the number of
    // completions delivered.
    std::size_t run_ready() {
        DriveScope const drive(*this);
        std::size_t      n = process_posted();
        n += fire_due(now());
        return n;
    }

    // Moves the virtual clock to `target` (never backwards), completing each timer that falls due on the way
    // in deadline order. Returns the number of completions delivered.
    std::size_t advance_to(time_point target) {
        DriveScope const drive(*this);
        std::size_t      n = process_posted();
        n += fire_due(now());
        if (target > now()) {
            n += fire_due(target);
            now_.store(target, std::memory_order_release);
        }
        return n;
    }
    std::size_t advance(duration d) { return advance_to(now() + d); }

    // Started timers not yet completed (queued starts are not counted until processed).
    [[nodiscard]] std::size_t pending() const noexcept { return pending_.load(std::memory_order_acquire); }
    // Starts and cancels queued and not yet processed.
    [[nodiscard]] std::size_t queued() const {
        std::lock_guard lock(m_);
        return posted_.size();
    }

protected:
    void post_cancel(std::shared_ptr<pal::ReactorOp> const& op) override {
        std::lock_guard lock(m_);
        posted_.push_back(Posted{op, time_point{}, true});
    }

private:
    struct Posted {
        std::shared_ptr<pal::ReactorOp> op;
        time_point                      deadline;
        bool                            cancel = false;
    };

    // Marks the calling thread as the reactor thread for one driving call; refuses concurrent or re-entrant
    // driving.
    class DriveScope {
    public:
        explicit DriveScope(ManualReactor& r) : r_(r) {
            std::thread::id expected{};
            if (!r_.driver_.compare_exchange_strong(expected, std::this_thread::get_id(),
                                                    std::memory_order_acq_rel)) {
                throw std::logic_error("testing::ManualReactor: driven concurrently or re-entrantly");
            }
        }
        ~DriveScope() { r_.driver_.store(std::thread::id{}, std::memory_order_release); }
        DriveScope(DriveScope const&)            = delete;
        DriveScope& operator=(DriveScope const&) = delete;

    private:
        ManualReactor& r_;
    };

    // Driver thread only. Completions run with the queue lock released (they may queue more work).
    std::size_t process_posted() {
        std::size_t n = 0;
        for (;;) {
            Posted p;
            {
                std::lock_guard lock(m_);
                if (posted_.empty()) return n;
                p = std::move(posted_.front());
                posted_.pop_front();
            }
            if (p.cancel) {
                // Pending: complete it `canceled`. Not started yet: the sticky flag stops it at start.
                // Already completed: nothing.
                for (auto it = timers_.begin(); it != timers_.end(); ++it) {
                    if (it->second == p.op) {
                        auto node = timers_.extract(it);
                        pending_.store(timers_.size(), std::memory_order_release);
                        node.mapped()->on_complete(pal::op_status::canceled);
                        ++n;
                        break;
                    }
                }
                continue;
            }
            if (p.op->cancel_requested() || shutting_down_) {
                p.op->on_complete(pal::op_status::canceled);  // sticky cancel (round-2 M2), or shutdown
                ++n;
                continue;
            }
            timers_.emplace(p.deadline, std::move(p.op));  // multimap: equal deadlines keep start order
            pending_.store(timers_.size(), std::memory_order_release);
        }
    }

    // Driver thread only. Completes, in deadline order, every timer due by `limit`, with now() at each one's
    // deadline (never moved backwards).
    std::size_t fire_due(time_point limit) {
        std::size_t n = 0;
        while (!timers_.empty() && timers_.begin()->first <= limit) {
            auto node = timers_.extract(timers_.begin());
            pending_.store(timers_.size(), std::memory_order_release);
            if (node.key() > now()) now_.store(node.key(), std::memory_order_release);
            node.mapped()->on_complete(pal::op_status::completed);
            ++n;
            n += process_posted();  // starts/cancels queued by that completion, before the next deadline
        }
        return n;
    }

    mutable std::mutex                                        m_;
    std::deque<Posted>                                        posted_;  // guarded by m_
    std::multimap<time_point, std::shared_ptr<pal::ReactorOp>> timers_;  // driver thread only
    std::atomic<time_point>                                   now_;
    std::atomic<std::size_t>                                  pending_{0};
    std::atomic<std::thread::id>                              driver_{};
    bool                                                      shutting_down_ = false;  // driver thread only
};

}  // namespace agentengine::testing
