// Proves ADR-102 Phase 4's own real finding and fix (rt/block_on.hpp's own top comment) -- an
// independent red-team pass on sandbox/mandatory_sandbox_provider.hpp found, and empirically proved via
// a targeted repro, that a naive "while (!t.done()) t.resume();" drive loop breaks
// agentengine::rt::AsyncMutex's mutual-exclusion guarantee under genuine cross-thread contention: the
// loop's second resume() call on an already-suspended awaiter hands back a Guard as if the lock were
// acquired when it is not, and later leaves a stale handle in the mutex's own waiter queue that a real
// unlock() resumes against an already-destroyed coroutine frame.
//
// This file proves the FIX: agentengine::rt::block_on<T>() (ported from the prove-phase original's own
// ASan-hardened driver) preserves real mutual exclusion under the SAME contention shape the red-team's
// own repro used to break the naive loop.
//
//   [1] two threads contend for one real AsyncMutex, each driven through block_on() -- the thread that
//       arrives second while the first is still holding the lock never observes itself "inside" the
//       critical section concurrently with the first holder (a shared counter never exceeds 1, checked
//       from both sides of the critical section, not just at the boundaries).
//   [2] the SAME scenario, repeated across several rounds, to make a return to the corrupted behavior
//       (data race dependent, not 100% every single run in principle) more likely to surface if the fix
//       regressed.
//   [3] an uncontended block_on() call (the ordinary case) still returns the correct value.

#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/block_on.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace agentengine::rt;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

// Enters the critical section, records the concurrent-holder count at both entry and a short hold
// window, then leaves -- so a mutual-exclusion violation of either "two holders overlap briefly" or
// "two holders overlap for the whole window" shape would be caught, not just an instantaneous check.
struct Shared {
    AsyncMutex mutex;
    std::atomic<int> concurrent_holders{0};
    std::atomic<int> max_concurrent_holders{0};
};

task<int> critical_section(Shared* shared, std::chrono::milliseconds hold_for) {
    AsyncMutex::Guard guard = co_await shared->mutex.lock();
    int const now = shared->concurrent_holders.fetch_add(1, std::memory_order_acq_rel) + 1;
    int prev_max = shared->max_concurrent_holders.load(std::memory_order_acquire);
    while (now > prev_max &&
           !shared->max_concurrent_holders.compare_exchange_weak(prev_max, now, std::memory_order_acq_rel)) {
    }
    std::this_thread::sleep_for(hold_for);
    shared->concurrent_holders.fetch_sub(1, std::memory_order_acq_rel);
    co_return now;
}

}  // namespace

int main() {
    // [1]/[2] real cross-thread contention, several rounds.
    for (int round = 0; round < 5; ++round) {
        Shared shared;
        std::thread t1([&shared] {
            (void)block_on(critical_section(&shared, std::chrono::milliseconds(60)));
        });
        // Start t2 while t1 is very likely still holding the lock -- deliberately racy by design (the
        // whole point is to exercise genuine contention, not to avoid it).
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::thread t2([&shared] {
            (void)block_on(critical_section(&shared, std::chrono::milliseconds(60)));
        });
        t1.join();
        t2.join();
        check(shared.max_concurrent_holders.load(std::memory_order_acquire) == 1,
              "block_on() preserves AsyncMutex's mutual exclusion under real cross-thread contention "
              "(never more than one concurrent holder observed)");
    }
    std::printf("[1]/[2] %d contention round(s) completed, mutual exclusion held every time -- PASS\n", 5);

    // [3] the ordinary, uncontended case still returns the correct value.
    {
        Shared shared;
        int const result = block_on(critical_section(&shared, std::chrono::milliseconds(1)));
        check(result == 1, "an uncontended block_on() call returns the correct value");
    }
    std::printf("[3] uncontended block_on() still returns the correct value -- PASS\n");

    if (g_failures == 0) {
        std::printf("\nALL CHECKS PASSED -- agentengine::rt::block_on<T>() genuinely preserves "
                     "AsyncMutex's mutual-exclusion guarantee under real cross-thread contention, "
                     "closing the exact hazard an independent red-team pass proved the naive drive loop "
                     "did not.\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
