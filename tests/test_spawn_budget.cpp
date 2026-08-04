// Proof + red-team for decisions/ADR-006-agent-spawn-depth-budget-bound.md
// (trust/spawn_budget.hpp), resolving the depth-bound half of 026 §9 Q1 / OQ-14. S-C4 and the
// static_asserts below are positive controls (022 §5): they prove the boundary actually fires and
// that the type system actually blocks the constructions it claims to, not just that every "happy"
// case in this file happens to pass.

#include <cstdint>
#include <iostream>
#include <type_traits>

#include "agentengine/trust/spawn_budget.hpp"

using namespace agentengine;
using namespace agentengine::trust;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " at " << __FILE__ << ":" << __LINE__ << "\n";     \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::cout << "  ok: " << (label) << "\n";                                              \
        }                                                                                           \
    } while (0)

// Compile-time proof (fails the BUILD if the guard is gone, not just a runtime check): SpawnBudget
// cannot be default-constructed and cannot be constructed from an arbitrary depth by anyone outside
// the class -- mint_root() is the only way to bring one into existence with a caller-chosen depth.
static_assert(!std::is_default_constructible_v<SpawnBudget>,
              "SpawnBudget must not be default-constructible (007 §3 rule 4: unforgeable in-process)");
static_assert(!std::is_constructible_v<SpawnBudget, std::uint32_t>,
              "SpawnBudget must not be constructible from an arbitrary depth outside mint_root()");

} // namespace

int main() {
    // S-C1: mint_root(N) starts at exactly N.
    {
        auto b = SpawnBudget::mint_root(3);
        AE_CHECK(b.remaining_depth() == 3, "S-C1: mint_root(3).remaining_depth() == 3");
    }

    // S-C2: N attenuations succeed, decrementing by exactly 1 each time.
    {
        auto b = SpawnBudget::mint_root(3);
        auto b1 = b.attenuate_for_spawn();
        AE_CHECK(b1.has_value() && b1->remaining_depth() == 2, "S-C2: 1st attenuation -> depth 2");
        auto b2 = b1->attenuate_for_spawn();
        AE_CHECK(b2.has_value() && b2->remaining_depth() == 1, "S-C2: 2nd attenuation -> depth 1");
        auto b3 = b2->attenuate_for_spawn();
        AE_CHECK(b3.has_value() && b3->remaining_depth() == 0, "S-C2: 3rd attenuation -> depth 0");

        // S-C3: the (N+1)th attenuation, now that the budget is exhausted, fails closed.
        auto b4 = b3->attenuate_for_spawn();
        AE_CHECK(!b4.has_value() && b4.error().code == "spawn_budget.depth_exhausted",
                 "S-C3: (N+1)th attenuation fails closed with depth_exhausted");
    }

    // S-C4 (positive control): a zero-depth budget fails on its very first attenuation attempt --
    // proves the boundary fires exactly at zero, not "eventually" or "never".
    {
        auto zero = SpawnBudget::mint_root(0);
        auto first = zero.attenuate_for_spawn();
        AE_CHECK(!first.has_value() && first.error().code == "spawn_budget.depth_exhausted",
                 "S-C4 (positive control): mint_root(0) fails on the first attenuation attempt");
    }

    // S-R1 (red-team): once exhausted, EVERY subsequent attempt fails -- not just the first one
    // past the boundary. Guards against an off-by-one that only catches the exact exhaustion call.
    {
        auto b = SpawnBudget::mint_root(1);
        auto exhausted = *b.attenuate_for_spawn(); // depth 0, one legitimate attenuation used
        for (int attempt = 0; attempt < 10; ++attempt) {
            auto denied = exhausted.attenuate_for_spawn();
            AE_CHECK(!denied.has_value(),
                     "S-R1: repeated attenuation attempt " + std::to_string(attempt) +
                         " on an exhausted budget still fails");
        }
    }

    // S-R2 (red-team, exhaustive over a range): for every max_depth in [0, 50], walking the chain
    // to exhaustion succeeds EXACTLY max_depth times, each step strictly decrementing by 1, and the
    // (max_depth+1)th call fails -- no off-by-one for any starting depth, not just the ones spot-
    // checked above.
    {
        bool all_ok = true;
        for (std::uint32_t max_depth = 0; max_depth <= 50 && all_ok; ++max_depth) {
            auto current = SpawnBudget::mint_root(max_depth);
            for (std::uint32_t step = 0; step < max_depth; ++step) {
                auto next = current.attenuate_for_spawn();
                if (!next.has_value() || next->remaining_depth() != max_depth - step - 1) {
                    all_ok = false;
                    break;
                }
                current = *next;
            }
            if (all_ok) {
                auto past_the_end = current.attenuate_for_spawn();
                if (past_the_end.has_value()) {
                    all_ok = false;
                }
            }
        }
        AE_CHECK(all_ok, "S-R2: max_depth in [0,50] all walk to exhaustion with no off-by-one");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All spawn_budget proof/red-team checks passed.\n";
    return 0;
}
