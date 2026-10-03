#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §4.2/§4.4 -- `rt::sleep_until` /
// `rt::sleep_for`: a coroutine timer that genuinely suspends on the I/O reactor (pal/reactor.hpp) instead of
// blocking its thread (today's `std::this_thread::sleep_for` in retry backoff, model_call_gateway.hpp). It is
// the first engine awaitable on the reactor and the reference for the rules every later one follows:
//
//   - Where it resumes: the waiter is posted to the home it parked from (ADR-175 `block_on` home, or an
//     ADR-219 host `Resumer`) -- never resumed on the reactor thread. A waiter with no home (resumed raw,
//     outside both) is REFUSED: it is not resumed at all, and `Reactor::homeless_refusals()` counts it
//     (§4.2). Lanes (a later step of the ADR) give every engine coroutine a home, and D7's explicit host API
//     makes the homeless case unreachable from host code; until then the refusal is the loud failure.
//   - Cancellation: a `std::stop_token` whose stop is requested posts a cancel to the reactor; the waiter is
//     resumed from the timer's own completion, reporting what actually happened (`expired` if the timer had
//     already fired). Two-phase, so nothing resumes while the reactor still references the operation
//     (round-1 M1); sticky, so a stop requested just before the timer starts is not lost (round-2 M2).
//   - Ordering inside await_suspend: the stop_callback is registered BEFORE the timer is started, and nothing
//     in the frame is touched after the start -- a host Resumer may run the continuation on another thread
//     the moment the completion is posted.
//   - Frame destroyed while waiting (a host dropping a parked task): the awaiter's destructor abandons the
//     operation, so its completion touches nothing in the dead frame, and cancels the timer early. A
//     continuation already queued at a host Resumer is claimed so it never runs (ADR-219 ticket).
//
// No `failure_class` is involved: the result is a plain `sleep_status`. (`failure_class::canceled`, D6,
// arrives with the seam conversion.)

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

#include "agentengine/pal/reactor.hpp"
#include "agentengine/rt/resume_home.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow sleep_status — ADR-237 §4.4: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class sleep_status : std::uint8_t {
    expired,   // the deadline passed
    canceled,  // the stop token was requested (or the reactor shut down) before it did
};

namespace sleep_detail {

class SleepOp final : public pal::ReactorOp {
public:
    SleepOp(pal::Reactor& reactor, detail::ParkedResumer parked) noexcept
        : reactor_(&reactor), ticket_(parked.ticket), parked_(std::move(parked)) {}

    // Reactor thread. Only enqueues (ADR-237 §4.2).
    void on_complete(pal::op_status status) noexcept override {
        status_.store(status, std::memory_order_relaxed);
        int expected = kPending;
        if (!state_.compare_exchange_strong(expected, kWoken, std::memory_order_acq_rel)) return;  // abandoned
        if (!parked_.home && !parked_.ticket) {
            // Homeless: resuming here would run the coroutine on the reactor thread. Refused, never resumed.
            reactor_->note_homeless_refusal();
            return;
        }
        detail::wake(std::move(parked_));
    }

    // Awaiter destructor while the frame is being destroyed. True if the operation was still pending (its
    // completion will now do nothing).
    [[nodiscard]] bool abandon() noexcept {
        int expected = kPending;
        if (state_.compare_exchange_strong(expected, kAbandoned, std::memory_order_acq_rel)) return true;
        // Already woken: a continuation queued at a host Resumer is claimed so it never runs (ADR-219).
        (void)detail::abandon_woken(ticket_);
        return false;
    }

    [[nodiscard]] pal::op_status status() const noexcept { return status_.load(std::memory_order_relaxed); }

private:
    static constexpr int kPending   = 0;
    static constexpr int kWoken     = 1;
    static constexpr int kAbandoned = 2;

    pal::Reactor*                          reactor_;
    std::shared_ptr<detail::ResumerTicket> ticket_;  // kept for abandon(); parked_ is moved out on wake
    detail::ParkedResumer                  parked_;
    std::atomic<int>                       state_{kPending};
    std::atomic<pal::op_status>            status_{pal::op_status::canceled};
};

struct Canceler {
    pal::Reactor*                   reactor;
    std::shared_ptr<pal::ReactorOp> op;
    void operator()() const noexcept {
        try {
            reactor->cancel(op);
        } catch (...) {  // NOLINT(bugprone-empty-catch): a failed post leaves the timer to expire on its own
        }
    }
};

class SleepAwaiter {
public:
    SleepAwaiter(pal::Reactor& reactor, pal::Reactor::clock::time_point deadline, std::stop_token stop) noexcept
        : reactor_(&reactor), deadline_(deadline), stop_(std::move(stop)) {}

    SleepAwaiter(SleepAwaiter const&)            = delete;
    SleepAwaiter& operator=(SleepAwaiter const&) = delete;

    ~SleepAwaiter() {
        if (!op_) return;
        cb_.reset();
        if (op_->abandon()) Canceler{reactor_, op_}();  // frame destroyed while waiting: free the timer early
    }

    [[nodiscard]] bool await_ready() noexcept {
        if (stop_.stop_requested()) {
            ready_ = sleep_status::canceled;
            return true;
        }
        if (pal::Reactor::clock::now() >= deadline_) {
            ready_ = sleep_status::expired;
            return true;
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        // Everything that can throw happens before the timer starts, while nothing refers to `h` yet.
        auto op = std::make_shared<SleepOp>(*reactor_, detail::capture_parked(h));
        if (stop_.stop_possible()) cb_.emplace(stop_, Canceler{reactor_, op});
        op_ = op;
        try {
            reactor_->start_timer(std::move(op), deadline_);
        } catch (...) {
            cb_.reset();
            op_.reset();
            throw;
        }
        // Nothing in this frame may be touched from here on: the continuation may already be running.
    }

    [[nodiscard]] sleep_status await_resume() noexcept {
        if (!op_) return ready_;
        cb_.reset();
        sleep_status const s =
            op_->status() == pal::op_status::completed ? sleep_status::expired : sleep_status::canceled;
        op_.reset();
        return s;
    }

private:
    pal::Reactor*                                   reactor_;
    pal::Reactor::clock::time_point                 deadline_;
    std::stop_token                                 stop_;
    std::shared_ptr<SleepOp>                        op_;
    std::optional<std::stop_callback<Canceler>>     cb_;
    sleep_status                                    ready_ = sleep_status::expired;
};

}  // namespace sleep_detail

// Suspends until `deadline` or until `stop` is requested, without holding a thread.
[[nodiscard]] inline sleep_detail::SleepAwaiter sleep_until(pal::Reactor& reactor,
                                                           pal::Reactor::clock::time_point deadline,
                                                           std::stop_token stop = {}) noexcept {
    return sleep_detail::SleepAwaiter(reactor, deadline, std::move(stop));
}

template <class Rep, class Period>
[[nodiscard]] sleep_detail::SleepAwaiter sleep_for(pal::Reactor& reactor, std::chrono::duration<Rep, Period> d,
                                                   std::stop_token stop = {}) noexcept {
    return sleep_until(reactor,
                       pal::Reactor::clock::now() + std::chrono::duration_cast<pal::Reactor::clock::duration>(d),
                       std::move(stop));
}

}  // namespace agentengine::rt
