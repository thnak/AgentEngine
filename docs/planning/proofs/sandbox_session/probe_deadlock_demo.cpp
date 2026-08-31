// PROVE-PHASE DEMONSTRATION PROBE. This deliberately builds the ANTI-PATTERN round 2/3's red-team
// warned about -- a "synchronous facade" that blocks (via block_on) on an async operation needing the
// SAME AsyncMutex the calling thread already holds -- and proves, empirically, that it self-deadlocks
// (bounded-spin: the operation never completes within a generous iteration budget). This is the
// negative control for probe_two_lock_safe.cpp's positive demonstration of §15.3's actual fix (two
// SEPARATE locks that never meet).

#include "../common/block_on.hpp"
#include "../common/result.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace probe {

class SharedLockFacade {
public:
    SharedLockFacade() : mutex_(std::make_unique<agentengine::rt::AsyncMutex>()) {}

    // Simulates "the async core's own operation" -- ordinary, needs the mutex.
    [[nodiscard]] agentengine::rt::task<result<void>> simple_op() {
        agentengine::rt::AsyncMutex::Guard guard = co_await mutex_->lock();
        co_return result<void>{};
    }

    // Simulates a coroutine that holds the lock WHILE synchronously calling back into code that
    // tries to synchronously perform another operation needing the SAME lock -- exactly the shape
    // round 2/3 warned a naive "sync facade blocks on its own async core" design could produce IF
    // the sync path and the async path shared one mutex (the bug §15.3's two-lock split exists to
    // rule out by construction).
    [[nodiscard]] agentengine::rt::task<result<void>> hold_and_call_back(std::function<void()> while_held) {
        agentengine::rt::AsyncMutex::Guard guard = co_await mutex_->lock();
        while_held();
        co_return result<void>{};
    }

private:
    std::unique_ptr<agentengine::rt::AsyncMutex> mutex_;
};

}  // namespace probe

int main() {
    using namespace probe;

    SharedLockFacade facade;
    bool inner_completed = false;

    auto outer = block_on(facade.hold_and_call_back([&]() {
        // We are now INSIDE the outer critical section (mutex_ is held). Try to synchronously
        // drive a SECOND operation that needs the SAME mutex, bounded so this probe doesn't hang
        // the test run forever if the prediction is right.
        result<void> inner_result;
        inner_completed = try_block_on_bounded(facade.simple_op(), 5'000'000, &inner_result);
    }));

    CHECK(outer.has_value());          // the OUTER operation itself completes fine (nothing wrong
                                         // with holding a lock and doing synchronous work)
    CHECK(!inner_completed);            // but the INNER, same-mutex operation NEVER completes --
                                         // empirically reproduced self-deadlock, not a hang we're
                                         // just guessing would happen

    std::printf("[1] Sharing ONE AsyncMutex between a sync-blocking wait and a critical section the\n"
                "    SAME call chain already holds: CONFIRMED SELF-DEADLOCK (inner operation did not\n"
                "    complete within 5,000,000 spins). This is the exact hazard round 2/3's red-team\n"
                "    warned about -- reproduced here for real, not argued on paper. See\n"
                "    probe_two_lock_safe.cpp for the actual fix (two separate locks, never shared).\n");

    std::printf("\nALL CHECKS PASSED (the deadlock IS the expected, confirmed result)\n");
    return 0;
}
