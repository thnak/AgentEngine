#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §4.2/§4.4 -- the one operation
// record every reactor awaitable parks on (rt/sleep.hpp, rt/tcp.hpp, rt/process.hpp). Factored out of those
// three after step 2 copied it into each (ADR §14): the resume/abandon rules live in exactly one place, so
// the lanes step (home stored in the promise, §4.2) changes them once.
//
//   - on_complete (reactor thread) only enqueues: pending -> woken, then `detail::wake` posts the waiter to
//     its home. A homeless waiter is REFUSED -- never resumed on the reactor thread -- and counted
//     (`Reactor::note_homeless_refusal`).
//   - abandon() (the awaiter's destructor, frame being destroyed): pending -> abandoned, so the completion
//     touches nothing; if already woken, a continuation queued at a host Resumer is claimed so it never runs
//     (ADR-219 ticket).
//   - the backend's status is stored before the CAS, so await_resume reads what really happened (round-1 M1).

#include <atomic>
#include <memory>
#include <utility>

#include "agentengine/pal/reactor.hpp"
#include "agentengine/rt/resume_home.hpp"

namespace agentengine::rt::reactor_detail {

// `Base` is pal::ReactorOp or a family's derived record (pal::TcpOp, pal::Process*Op) whose result fields the
// backend fills before calling on_complete.
template <class Base = pal::ReactorOp>
class AwaitedOp final : public Base {
public:
    AwaitedOp(pal::Reactor& reactor, detail::ParkedResumer parked) noexcept
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

    // True if the operation was still pending (its completion will now do nothing).
    [[nodiscard]] bool abandon() noexcept {
        int expected = kPending;
        if (state_.compare_exchange_strong(expected, kAbandoned, std::memory_order_acq_rel)) return true;
        (void)detail::abandon_woken(ticket_);  // already woken: claim a continuation queued at a host Resumer
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

// The stop_callback body: post a cancel. Never throws out of the callback.
struct Canceler {
    pal::Reactor*                   reactor;
    std::shared_ptr<pal::ReactorOp> op;
    void operator()() const noexcept {
        try {
            reactor->cancel(op);
        } catch (...) {  // NOLINT(bugprone-empty-catch): a failed post leaves the op to finish on its own
        }
    }
};

}  // namespace agentengine::rt::reactor_detail
