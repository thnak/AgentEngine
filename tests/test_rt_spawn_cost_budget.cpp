// Proof for ADR-037: agentengine::rt::SpawnCostBudget (include/agentengine/rt/spawn_cost_budget.hpp),
// the Quark-actor-free replacement for agentengine::trust::SpawnCostBudgetActor. Mirrors the
// original's own test_spawn_cost_budget.cpp T1/T2 shape closely, so the two can be compared side by
// side:
//   T1 -- single-threaded correctness: consuming within budget succeeds and decrements; consuming
//         beyond what remains is denied and does NOT decrement; the pool never goes negative.
//   T2 -- the actual point of this file, proven with REAL concurrent std::threads (not just driven
//         via a single-threaded resume() loop, which could never produce the race this exists to
//         close): N concurrent callers, each consuming from the SAME budget instance concurrently.
//         Asserts the sum of every granted amount never exceeds the initial pool -- the exact
//         double-spend 026 Sec9 Q1 names as the reason a bare copyable value type would be actively
//         wrong for a consumed pool. Each thread calls resume() exactly once, as in
//         test_rt_async_mutex.cpp's own M3, and results are read only after every thread has joined
//         (see the comment at the join for why reading them any earlier is UB) -- rt::AsyncMutex is what
//         actually serializes the concurrent consume() calls underneath, so this is exercising
//         AsyncMutex's own already-proven FIFO hand-off, not inventing a new concurrency mechanism.
//
// MACHINE SAFETY (CLAUDE.md): 8 real threads, bounded, matching the original's own cap.

#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "agentengine/rt/spawn_cost_budget.hpp"

using agentengine::rt::ConsumeSpawnTokens;
using agentengine::rt::SpawnCostBudget;
using agentengine::rt::SpawnTokenGrant;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// Safe here: consume()'s only suspension point is mutex_'s lock() -- uncontended in T1 (single
// caller), so the fast path never genuinely suspends and one resume() call resolves it fully.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace

int main() {
    // ---- T1: single-threaded correctness ---------------------------------------------------------
    {
        SpawnCostBudget budget;
        budget.initialize(100);

        auto r1 = drive(budget.consume(ConsumeSpawnTokens{30}));
        check(r1.has_value() && r1->granted == 30,
              "T1: consuming 30 from a pool of 100 succeeds and grants exactly 30");
        check(budget.remaining() == 70, "T1: remaining() reflects the decrement (70)");

        auto r2 = drive(budget.consume(ConsumeSpawnTokens{80}));
        check(!r2.has_value(),
              "T1: consuming 80 when only 70 remains is denied (not a crash, not a silent partial "
              "grant)");
        if (!r2.has_value()) {
            check(r2.error().code == "spawn_cost_budget.exhausted", "T1: the denial carries the specific error code");
        }
        check(budget.remaining() == 70,
              "T1: a DENIED consume does not decrement -- remaining() is still 70, not 70 minus some "
              "partial amount");

        auto r3 = drive(budget.consume(ConsumeSpawnTokens{70}));
        check(r3.has_value() && r3->granted == 70, "T1: consuming exactly the remaining 70 succeeds");
        check(budget.remaining() == 0, "T1: the pool is now exactly exhausted (0)");

        auto r4 = drive(budget.consume(ConsumeSpawnTokens{1}));
        check(!r4.has_value(), "T1: consuming even 1 more token from an exhausted pool is denied");
    }

    // ---- T2: real concurrency proof -----------------------------------------------------------
    {
        constexpr std::uint64_t kInitialPool = 1000;
        constexpr std::uint64_t kPerCallerAmount = 130;  // 8 * 130 = 1040 > 1000: guarantees at
                                                          // least one caller is denied, so BOTH
                                                          // outcomes are exercised under real
                                                          // concurrency, not just "everyone fits."
        constexpr int kCallerCount = 8;                  // CLAUDE.md's machine-safety cap.

        SpawnCostBudget budget;
        budget.initialize(kInitialPool);

        std::vector<agentengine::rt::task<agentengine::result<SpawnTokenGrant>>> jobs;
        jobs.reserve(kCallerCount);
        for (int i = 0; i < kCallerCount; ++i) jobs.push_back(budget.consume(ConsumeSpawnTokens{kPerCallerAmount}));

        {
            std::vector<std::thread> callers;
            callers.reserve(kCallerCount);
            for (auto& j : jobs) {
                // Exactly one resume() per job, from its own real thread, and NOTHING else. A job that
                // finds the mutex held is not finished when this returns: it is parked in lock(), and
                // AsyncMutex::unlock()'s trampoline resumes it later, on whichever thread releases.
                // Reading its value here -- as this test once did, right after resume() -- reads the
                // task's result storage before anything was constructed in it (UBSan: "load of value
                // 190, which is not a valid value for type 'bool'"; on MSVC Debug it failed the
                // remaining() check below in 45 of 300 runs).
                callers.emplace_back([&j] { j.resume(); });
            }
            for (auto& t : callers) t.join();
        }

        // consume() has one suspension point, and every resume of a parked job runs inside some
        // caller's resume() -> ... -> unlock() chain, so once every caller thread has joined, every
        // job has run to completion and join() is the synchronization edge for reading it here.
        std::uint64_t total_granted = 0;
        int denied_count = 0;
        int finished = 0;
        for (auto& j : jobs) {
            if (!j.done()) continue;  // counted as unfinished below, never read
            ++finished;
            agentengine::result<SpawnTokenGrant> r = j.take_value();
            if (r.has_value()) {
                total_granted += r->granted;
            } else {
                ++denied_count;
            }
        }
        check(finished == kCallerCount,
              "T2 setup: all 8 concurrent consume() calls actually completed");

        check(total_granted <= kInitialPool,
              "T2: the sum of every granted amount across 8 REAL concurrent callers never exceeds "
              "the initial pool -- no double-spend under genuine concurrent access, the exact "
              "hazard a bare copyable value type would have");
        check(denied_count > 0,
              "T2: at least one caller was genuinely denied (8*130=1040 > 1000 guarantees this) -- "
              "proving this test exercises the contended case, not just a lucky everyone-fits run");
        check(budget.remaining() == kInitialPool - total_granted,
              "T2: the budget's own final remaining() is EXACTLY consistent with the sum of grants "
              "-- no lost or double-counted decrement across the concurrent run");
    }

    if (g_failures == 0) {
        std::printf("test_rt_spawn_cost_budget: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_spawn_cost_budget: %d failure(s)\n", g_failures);
    return 1;
}
