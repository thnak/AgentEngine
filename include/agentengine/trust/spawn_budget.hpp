#pragma once
// Implements 007-Capability-and-Trust-Model.md §3's `AgentCall<agent>` capability parameter
// ("agent id, depth budget") and resolves the depth-bound half of 026-Agent-Facing-Runtime-
// Surface.md §9 Q1 / OQ-14 (OpenQuestions.md) — whether depth and budget bounds are sufficient
// against agent.spawn's recursion-and-cost hazard. See
// decisions/ADR-006-agent-spawn-depth-budget-bound.md for the red-team pass this small prove ran.
//
// Deliberately NOT a cryptographic token like trust/capability_token.hpp's cross-process bearer
// token (ADR-005) -- agent.spawn creates a sibling Quark actor within the same engine/cluster, never
// crossing an OS process boundary to an untrusted holder, so the threat model is different and
// lighter: unforgeability here comes from the same place trust/capability.hpp §3 rule 4 already
// names for in-process capabilities -- private construction plus the type system -- not from HMAC.
// The security property this type provides depends on an architectural fact this RFC already
// commits to elsewhere (I2/I3, 026 §5): model-generated code never calls agent.spawn() by directly
// invoking engine internals -- it triggers an *effect* the host mediates, and only the host's
// mediation code (never guest Python) is positioned to call mint_root()/attenuate_for_spawn(). This
// header proves the counter itself cannot be defeated once that mediation boundary holds; it does
// not re-prove the mediation boundary itself (that is 006 §3's tool-pipeline claim, already scoped
// elsewhere).

#include <cstdint>

#include "agentengine/core/error.hpp"

namespace agentengine::trust {

// A depth budget for a chain of agent.spawn calls. Strictly decreasing (007 §3 rule 2: attenuation
// only, never a superset) -- every attenuation consumes exactly one level, and the type has no
// operation that increases remaining_depth() or resets it.
// ae-naming-lint: allow SpawnBudget — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class SpawnBudget {
public:
    // Only the host calls this -- typically once per top-level run, seeded from policy (023
    // budgets), never from anything a model's own output could set (I3). There is no default
    // constructor and no public constructor taking an arbitrary depth: mint_root is the only entry
    // point that creates a budget from nothing.
    static SpawnBudget mint_root(std::uint32_t max_depth) { return SpawnBudget(max_depth); }

    // The one operation a spawn attempt performs: consumes one level and returns the child's
    // budget, or fails closed if none remains. Combines "check" and "narrow" into one call
    // precisely so there is no check-then-act gap between deciding a spawn is allowed and actually
    // reducing the budget the child receives.
    result<SpawnBudget> attenuate_for_spawn() const {
        if (remaining_depth_ == 0) {
            return std::unexpected(ae::error{failure_class::policy,
                                              "agent.spawn depth budget exhausted",
                                              "spawn_budget.depth_exhausted"});
        }
        return SpawnBudget(remaining_depth_ - 1);
    }

    std::uint32_t remaining_depth() const { return remaining_depth_; }

private:
    explicit SpawnBudget(std::uint32_t remaining_depth) : remaining_depth_(remaining_depth) {}
    std::uint32_t remaining_depth_;
};

} // namespace agentengine::trust
