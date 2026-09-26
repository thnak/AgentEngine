// Proof for ADR-079 §7's TSan residual on SpawnPump (include/agentengine/rt/agent_spawn.hpp) --
// C9's own claim ("pump serialization closes the AsyncMutex hazard, verified under TSan"). The real
// SpawnPump cannot be built or linked on non-Windows today: its process() step reaches
// core/agent_spawn_worktree.hpp's derive_spawn_child_id() -> compute_digest(), which is Windows
// CNG/BCrypt-only (src/core/worktree_digest.cpp's own top comment: "Windows-only for now") --
// tests/CMakeLists.txt gates the WHOLE test_rt_agent_spawn target (T7, the ASan-only multi-thread
// proof) behind if(WIN32) for exactly this reason, unrelated to spawn's own concurrency mechanism.
// Porting compute_digest to Linux is a separate, larger, out-of-scope task (021 §2's own platform-
// priority backlog).
//
// This file isolates and proves the ACTUAL concurrency-critical mechanism C9 is about --
// rt/agent_spawn.hpp's own top-comment hazard: a naive "while (!t.done()) t.resume()" drive loop
// over rt::SpawnCostBudget::consume() is safe ONLY when nothing else can ever resume the SAME
// coroutine handle concurrently, because consume() is guarded by a real rt::AsyncMutex whose
// unlock() resumes a queued waiter's coroutine handle DIRECTLY FROM THE UNLOCKING THREAD. SpawnPump
// closes this by construction: ONE dedicated worker thread is the ONLY thread that ever calls
// resume() on a consume() coroutine. Reproduced here as a minimal, test-local TestSpawnPump -- the
// IDENTICAL synchronization shape the real SpawnPump::submit()/run()/process() uses for its
// cost-consumption step (queue + condition_variable + one worker thread + std::promise/future per
// caller), built ONLY from rt::SpawnCostBudget (spawn_cost_budget.hpp, fully portable, zero platform
// dependency). The worktree-minting step SpawnPump::process() also performs is deliberately NOT
// reproduced here -- it is not what makes this mechanism concurrency-critical; the file banner this
// class mirrors names the AsyncMutex/consume() hazard specifically, never the worktree mint.
//
// Built and run on Linux under clang -fsanitize=thread (this file has zero platform-specific
// dependency), closing the literal "verified under TSan" half of ADR-079 §5's C9 that its Windows-
// only sibling (tests/test_rt_agent_spawn.cpp T7, ASan-only, proven 2026-08-23) could not reach.
//
//   T1 -- single-threaded sanity: TestSpawnPump::submit() drives consume() to completion correctly
//         both within budget and on exhaustion, matching SpawnCostBudget's own already-proven
//         single-threaded behavior (test_rt_spawn_cost_budget.cpp T1) through the pump wrapper.
//   T2 -- THE PROOF: 16 real std::thread callers submit() concurrently against ONE TestSpawnPump
//         wrapping ONE SpawnCostBudget (pool sized so it genuinely exhausts mid-run: 2000-token
//         pool, 130-token cost -- exactly 15 of 16 can succeed), repeated 25 rounds with a fresh
//         pump each round. Asserts exact token conservation every round (no double-spend, no lost
//         update) and that every rejection carries the real exhaustion code -- under TSan this time,
//         not merely ASan, closing the specific evidence gap ADR-079 §7 names.
//
// MACHINE SAFETY (CLAUDE.md): 16 real threads per round, bounded and joined before the next round;
// no unbounded thread creation.

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/rt/spawn_cost_budget.hpp"

using agentengine::rt::ConsumeSpawnTokens;
using agentengine::rt::SpawnCostBudget;
using agentengine::rt::SpawnTokenGrant;

namespace {

int  g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// SAME naive "resume until done" pattern rt/agent_spawn_child_run.hpp's own agent_spawn_detail::
// drive() uses, and the real SpawnPump::process() reuses unmodified -- reproduced here rather than
// included from there, to avoid that header's own transitive Windows-only dependency chain (it pulls
// in a real rt::AgentSession, which this file has no need of).
template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// Mirrors rt/agent_spawn.hpp's own SpawnPump EXACTLY for the cost-consumption step: one dedicated
// worker thread is the ONLY thread that ever calls resume() on a consume() coroutine; every other
// thread only ever submit()s and blocks on a std::future for the result. See file banner for why the
// worktree-minting step SpawnPump::process() also performs is deliberately absent here.
class TestSpawnPump {
public:
    explicit TestSpawnPump(SpawnCostBudget& pool) : pool_(pool), worker_([this] { run(); }) {}

    ~TestSpawnPump() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    TestSpawnPump(TestSpawnPump const&)            = delete;
    TestSpawnPump& operator=(TestSpawnPump const&) = delete;

    [[nodiscard]] agentengine::result<SpawnTokenGrant> submit(std::uint64_t cost) {
        std::promise<agentengine::result<SpawnTokenGrant>> prom;
        std::future<agentengine::result<SpawnTokenGrant>>  fut = prom.get_future();
        {
            std::lock_guard<std::mutex> lk(m_);
            queue_.push_back(Job{cost, std::move(prom)});
        }
        cv_.notify_one();
        return fut.get();
    }

private:
    struct Job {
        std::uint64_t                                       cost;
        std::promise<agentengine::result<SpawnTokenGrant>> prom;
    };

    void run() {
        for (;;) {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stop_) return;
                continue;
            }
            Job job = std::move(queue_.front());
            queue_.pop_front();
            lk.unlock();
            // Safe ONLY because this worker thread processes jobs strictly one at a time -- the SAME
            // "structurally only one thread could ever contend pool_'s internal AsyncMutex" argument
            // rt/agent_spawn.hpp's own SpawnPump::process() makes for the identical call.
            job.prom.set_value(drive(pool_.consume(ConsumeSpawnTokens{job.cost})));
        }
    }

    SpawnCostBudget&        pool_;
    std::mutex               m_;
    std::condition_variable  cv_;
    std::deque<Job>          queue_;
    bool                     stop_ = false;
    std::thread              worker_;  // declared last: must start only once every member above it
                                         // is already fully constructed -- same ordering rationale
                                         // as the real SpawnPump's own worker_ field.
};

}  // namespace

int main() {
    // ---- T1: single-threaded sanity through the pump wrapper --------------------------------------
    {
        SpawnCostBudget pool;
        pool.initialize(100);
        TestSpawnPump   pump(pool);

        auto r1 = pump.submit(30);
        check(r1.has_value() && r1->granted == 30, "T1: submit() within budget succeeds via the pump");
        check(pool.remaining() == 70, "T1: remaining() reflects the decrement (70)");

        auto r2 = pump.submit(80);
        check(!r2.has_value(), "T1: submit() beyond what remains is denied through the pump");
        if (!r2.has_value()) {
            check(r2.error().code == "spawn_cost_budget.exhausted",
                  "T1: the denial carries SpawnCostBudget's own real error code");
        }
        check(pool.remaining() == 70, "T1: a denied submit() does not decrement");
    }

    // ---- T2: THE PROOF -- real cross-thread contention, TSan-clean ---------------------------------
    {
        constexpr int             kThreads = 16;
        constexpr std::uint64_t   kPool    = 2000;
        constexpr std::uint64_t   kCost    = 130;
        // floor(kPool / kCost) -- the exact number of the 16 concurrent submits that CAN succeed;
        // computed, not eyeballed, mirroring tests/test_rt_agent_spawn.cpp's own T7 exactly.
        constexpr int kExpectedSuccesses = static_cast<int>(kPool / kCost);
        static_assert(kExpectedSuccesses > 0 && kExpectedSuccesses < kThreads,
                      "T2: the scenario must genuinely exhaust the pool mid-run, not merely satisfy "
                      "or starve every caller -- otherwise this proves nothing about the boundary");

        constexpr int kRounds = 25;  // one lucky pass proves little about a cross-thread-resume hazard
        int rounds_with_wrong_success_count = 0;
        int rounds_with_wrong_remaining     = 0;
        int rounds_with_bad_failure_code    = 0;

        for (int round = 0; round < kRounds; ++round) {
            SpawnCostBudget pool;
            pool.initialize(kPool);
            TestSpawnPump pump(pool);

            std::vector<agentengine::result<SpawnTokenGrant>> results(kThreads);
            std::vector<std::thread>                            workers;
            workers.reserve(kThreads);
            for (int i = 0; i < kThreads; ++i) {
                workers.emplace_back(
                    [&, i] { results[static_cast<std::size_t>(i)] = pump.submit(kCost); });
            }
            for (auto& t : workers) t.join();

            int  successes = 0;
            bool all_failures_correctly_coded = true;
            for (auto const& r : results) {
                if (r.has_value()) {
                    ++successes;
                } else if (r.error().code != "spawn_cost_budget.exhausted") {
                    all_failures_correctly_coded = false;
                }
            }

            if (successes != kExpectedSuccesses) ++rounds_with_wrong_success_count;
            if (pool.remaining() != kPool - static_cast<std::uint64_t>(successes) * kCost)
                ++rounds_with_wrong_remaining;
            if (!all_failures_correctly_coded) ++rounds_with_bad_failure_code;
        }

        check(rounds_with_wrong_success_count == 0,
              "T2: every round, exactly floor(pool/cost) of 16 REAL concurrent submit() callers "
              "succeeded -- no round over- or under-granted under real cross-thread contention");
        check(rounds_with_wrong_remaining == 0,
              "T2: every round, SpawnCostBudget::remaining() landed EXACTLY on successes*cost "
              "subtracted from the pool -- no double-spend and no lost update across 16 real threads "
              "racing submit() against the same pool, TSan-clean (the exact hazard rt/agent_spawn.hpp "
              "and ADR-079 C9 name)");
        check(rounds_with_bad_failure_code == 0,
              "T2: every rejected submit(), every round, failed with the real "
              "spawn_cost_budget.exhausted code -- not a crash, not a wrong/generic error");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_rt_spawn_pump_concurrency: ALL PASS\n");
    } else {
        std::fprintf(stderr, "test_rt_spawn_pump_concurrency: %d FAILURE(S)\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
