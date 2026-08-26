#pragma once
// PROVE-PHASE PROBE for SandboxSession's exclusivity fix (§19.3): every method takes the SAME
// AsyncMutex, so release_branch() cannot run concurrently with a suspended materialize()/
// harvest_and_checkpoint() call -- closing round 4's TOCTOU finding for real, not by asserting the
// rvalue-qualification on release_branch() alone provides it (round 4 proved it doesn't).
//
// Held behind unique_ptr (§21.1's finding, applied here BEFORE this type was even probed): AsyncMutex
// is non-copyable/non-movable, so a by-value member would make this type unreturnable from a
// task<result<SandboxSession>>-returning factory.

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"

#include "../common/result.hpp"

namespace probe {

// A minimal stand-in for whatever "do real sandbox work" means -- this probe exists to test the
// EXCLUSIVITY/lifecycle mechanism, not sandbox mechanics, so materialize()/harvest_and_checkpoint()
// just record observable events (with an artificial delay INSIDE the critical section, simulating
// real async work) rather than touching a real filesystem/Ledger.
struct SandboxEventLog {
    std::mutex log_mutex;   // ordinary std::mutex, purely for this TEST's own bookkeeping -- not part
                              // of the design being probed
    std::vector<std::string> events;

    void record(std::string event) {
        std::lock_guard<std::mutex> g(log_mutex);
        events.push_back(std::move(event));
    }
};

class SandboxSession {
public:
    explicit SandboxSession(SandboxEventLog* log)
        : exclusivity_(std::make_unique<agentengine::rt::AsyncMutex>()), log_(log) {}

    SandboxSession(SandboxSession const&) = delete;
    SandboxSession& operator=(SandboxSession const&) = delete;
    SandboxSession(SandboxSession&&) = default;
    SandboxSession& operator=(SandboxSession&&) = default;

    [[nodiscard]] agentengine::rt::task<result<void>> materialize(int hold_millis) {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        if (released_) {
            co_return std::unexpected(error{"SandboxSession used after release_branch()",
                                              "sandbox_session.already_released"});
        }
        log_->record("materialize:enter");
        // Real, synchronous work WHILE HOLDING THE GUARD -- simulates real session lifecycle work
        // (e.g. a real materialize_mount() call) that takes measurable time. The guard is a plain
        // local object; holding it across ordinary synchronous statements (not a further co_await)
        // is exactly how a real critical section here would look.
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_millis));
        log_->record("materialize:exit");
        co_return result<void>{};
    }

    // Rvalue-qualified (matching §19.3's literal signature) AND now genuinely serialized via the same
    // exclusivity_ lock every other method takes -- closing round 4's TOCTOU finding: this can no
    // longer run concurrently with a suspended materialize() call.
    [[nodiscard]] agentengine::rt::task<result<void>> release_branch() && {
        agentengine::rt::AsyncMutex::Guard guard = co_await exclusivity_->lock();
        log_->record("release_branch:enter");
        released_ = true;
        log_->record("release_branch:exit");
        co_return result<void>{};
    }

private:
    std::unique_ptr<agentengine::rt::AsyncMutex> exclusivity_;
    bool released_ = false;
    SandboxEventLog* log_;
};

}  // namespace probe
