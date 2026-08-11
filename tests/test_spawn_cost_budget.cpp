// Proves `SpawnCostBudgetActor` (trust/spawn_cost_budget.hpp) — the "prove" half of 026 §9 Q1's
// design → red-team → prove record (the design/red-team half is already written down in that
// spec section and in OpenQuestions.md's OQ-14; this file is what turns "designed and
// red-teamed, no code" into real, sanitizer-buildable evidence).
//
//   T1 — basic correctness (quark::TestKit, single-threaded): consuming within budget succeeds
//        and decrements; consuming beyond what remains is denied and does NOT decrement; the
//        pool never goes negative.
//   T2 — the actual point of this file, proven under a REAL, multi-worker quark::Engine (never
//        exercisable through quark::TestKit, which drains one ask fully before the next can even
//        be issued and so cannot produce a genuine race): N concurrent std::thread callers, each
//        issuing a ConsumeSpawnTokens ask against the SAME actor instance from the SAME pool,
//        concurrently. Asserts the sum of every granted amount never exceeds the initial pool —
//        the exact double-spend 026 §9 Q1 names as the reason a bare copyable value type (like
//        SpawnBudget's own depth-ceiling shape) would be actively wrong for a consumed pool.

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/testkit.hpp"

#include "agentengine/trust/spawn_cost_budget.hpp"

using namespace quark;
namespace trust = agentengine::trust;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::fprintf(stderr, "FAIL: %s (" #cond ") at %s:%d\n", label, __FILE__, __LINE__);   \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::printf("  ok: %s\n", label);                                                     \
        }                                                                                          \
    } while (0)

}  // namespace

int main() {
    // ---- T1: single-threaded correctness (TestKit) ---------------------------------------------
    {
        quark::TestKit<trust::SpawnCostBudgetActor> kit;
        kit.actor().initialize(100);

        auto r1 = kit.ask<agentengine::result<trust::SpawnTokenGrant>>(trust::ConsumeSpawnTokens{30});
        AE_CHECK(r1.has_value() && r1->has_value() && (*r1)->granted == 30,
                 "T1: consuming 30 from a pool of 100 succeeds and grants exactly 30");
        AE_CHECK(kit.actor().remaining() == 70, "T1: remaining() reflects the decrement (70)");

        auto r2 = kit.ask<agentengine::result<trust::SpawnTokenGrant>>(trust::ConsumeSpawnTokens{80});
        AE_CHECK(r2.has_value() && !r2->has_value(),
                 "T1: consuming 80 when only 70 remains is denied (not a crash, not a silent "
                 "partial grant)");
        if (r2.has_value() && !r2->has_value()) {
            AE_CHECK(r2->error().code == "spawn_cost_budget.exhausted",
                     "T1: the denial carries the specific error code");
        }
        AE_CHECK(kit.actor().remaining() == 70,
                 "T1: a DENIED consume does not decrement -- remaining() is still 70, not 70 minus "
                 "some partial amount");

        auto r3 = kit.ask<agentengine::result<trust::SpawnTokenGrant>>(trust::ConsumeSpawnTokens{70});
        AE_CHECK(r3.has_value() && r3->has_value() && (*r3)->granted == 70,
                 "T1: consuming exactly the remaining 70 succeeds");
        AE_CHECK(kit.actor().remaining() == 0, "T1: the pool is now exactly exhausted (0)");

        auto r4 = kit.ask<agentengine::result<trust::SpawnTokenGrant>>(trust::ConsumeSpawnTokens{1});
        AE_CHECK(r4.has_value() && !r4->has_value(),
                 "T1: consuming even 1 more token from an exhausted pool is denied");
    }

    // ---- T2: real concurrency proof (live, multi-worker quark::Engine) --------------------------
    {
        auto built = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
        if (!built) {
            std::fprintf(stderr, "FAIL: T2 setup: ConfigBuilder failed to produce a valid EngineConfig\n");
            ++g_failures;
        } else {
            constexpr std::uint64_t kInitialPool = 1000;
            constexpr std::uint64_t kPerCallerAmount = 130;  // 8 * 130 = 1040 > 1000: guarantees at
                                                              // least one caller is denied, so this
                                                              // test exercises BOTH outcomes under
                                                              // real concurrency, not just "everyone
                                                              // happens to fit."
            constexpr int kCallerCount = 8;                  // capped at 4 real workers (CLAUDE.md's
                                                              // machine-safety rule) -- 8 CONCURRENT
                                                              // callers still contend for only 4
                                                              // worker lanes, which is what actually
                                                              // matters for exercising the race.

            Engine<> eng(*built);
            detail::MessagePool pool(64);
            trust::SpawnCostBudgetActor actor;
            actor.initialize(kInitialPool);
            Activation act{&actor, trust::SpawnCostBudgetActor::dispatch_table(), pool.sink()};
            eng.register_activation(actor_id_of<trust::SpawnCostBudgetActor>(1), act);

            LocalRouter router(eng.post_courier(), pool);
            ActorRef<trust::SpawnCostBudgetActor> ref = router.get<trust::SpawnCostBudgetActor>(1);
            eng.start();

            std::atomic<std::uint64_t> total_granted{0};
            std::atomic<int> denied_count{0};
            std::vector<std::thread> callers;
            callers.reserve(kCallerCount);
            for (int i = 0; i < kCallerCount; ++i) {
                callers.emplace_back([&] {
                    auto r = block_on(ref.ask<agentengine::result<trust::SpawnTokenGrant>>(
                        trust::ConsumeSpawnTokens{kPerCallerAmount}));
                    if (r.has_value() && r->has_value()) {
                        total_granted.fetch_add((*r)->granted, std::memory_order_relaxed);
                    } else {
                        denied_count.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
            for (auto& t : callers) t.join();
            eng.stop();

            AE_CHECK(total_granted.load() <= kInitialPool,
                     "T2: the sum of every granted amount across 8 REAL concurrent callers never "
                     "exceeds the initial pool -- no double-spend under genuine concurrent "
                     "dispatch (the exact hazard a bare copyable value type would have)");
            AE_CHECK(denied_count.load() > 0,
                     "T2: at least one caller was genuinely denied (8*130=1040 > 1000 guarantees "
                     "this) -- proving this test exercises the contended case, not just a lucky "
                     "everyone-fits run");
            AE_CHECK(actor.remaining() == kInitialPool - total_granted.load(),
                     "T2: the actor's own final remaining() is EXACTLY consistent with the sum of "
                     "grants -- no lost or double-counted decrement across the concurrent run");
        }
    }

    std::printf(g_failures == 0 ? "test_spawn_cost_budget: OK\n" : "test_spawn_cost_budget: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
