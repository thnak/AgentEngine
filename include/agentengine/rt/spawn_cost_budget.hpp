#pragma once
// ADR-037: agentengine::rt::SpawnCostBudget, the Quark-actor-free replacement for
// agentengine::trust::SpawnCostBudgetActor (trust/spawn_cost_budget.hpp). Implements 026-Agent-
// Facing-Runtime-Surface.md Sec9 Q1's corrected SpawnCostBudget sketch -- read that file's own banner
// for the full "why a real serialized actor, not a bare copyable value type like SpawnBudget's own
// depth-ceiling shape" reasoning; it is unchanged here, just re-expressed over rt::AsyncMutex instead
// of quark::Actor<Sequential>. The double-spend hazard the original names (two concurrent
// agent.spawn calls each independently attenuating a bare copyable remaining_ snapshot could each
// succeed up to the full remaining amount) is closed the SAME way -- check-and-decrement as ONE
// atomic step -- just serialized by rt::AsyncMutex (already proven for AgentSession's/
// WorkflowSupervisor's own I1) instead of Quark's Sequential dispatch.
//
// SCOPE (matching the original's own precedent): proven standalone, NOT wired to any real
// agent.spawn call path -- none exists yet in this codebase (see the original's own banner for why).
// Sourcing the amount a child receives is still the CALLER's responsibility, never this type's own --
// it has no visibility into AgentMetadata, by design, matching the original exactly.
//
// A real narrowing named, not silently carried over unchanged: the original's own comment explains
// why every consume() ask is answered explicitly rather than left as a "never respond" fail-closed
// idiom -- "a genuinely unanswered Ask only resolves at actor teardown/reclaim" under a real
// multi-worker Engine, which would park a live caller's block_on() indefinitely. That specific hazard
// does not apply here at all: consume() is an ordinary rt::task<T> a caller co_awaits directly, with
// no separate reply-cell/teardown-resolution mechanism to leave dangling -- but the "always respond,
// never silently fail closed by not replying" DISCIPLINE itself is preserved anyway, unchanged, since
// it is still the right contract for a caller regardless of which mechanism would have made getting
// it wrong observable.

#include <cstdint>

#include "agentengine/core/error.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"

namespace agentengine::rt {

// Same shapes as the original -- no identity travels with the query; attribution of WHO is spending
// is still the caller's own concern.
struct ConsumeSpawnTokens {
    std::uint64_t amount = 0;
};
struct SpawnTokenGrant {
    std::uint64_t granted = 0;
};

class SpawnCostBudget {
public:
    // Configuration-time only, like the original -- call once, before any consume() call, never
    // re-armed mid-lifetime. Same "no reset/top-up method, on purpose" rationale: a refillable pool
    // needs its own authority question this type does not answer.
    void initialize(std::uint64_t total_tokens) noexcept { remaining_ = total_tokens; }

    // Check-and-decrement as ONE atomic step under mutex_ -- no other code path ever mutates
    // remaining_, so there is no window between "check" and "decrement" for a second concurrent
    // consume() to observe the pre-decrement value.
    [[nodiscard]] task<agentengine::result<SpawnTokenGrant>> consume(ConsumeSpawnTokens request) {
        AsyncMutex::Guard guard = co_await mutex_.lock();
        if (request.amount > remaining_) {
            co_return std::unexpected(agentengine::error{agentengine::failure_class::resource,
                                                           "spawn cost budget exhausted",
                                                           "spawn_cost_budget.exhausted"});
        }
        remaining_ -= request.amount;
        co_return SpawnTokenGrant{request.amount};
    }

    // Unlocked, matching the original's own plain accessor -- callers reading this while a consume()
    // is genuinely in flight on another thread carry the same responsibility the original placed on
    // them (this codebase's tests only ever read it once nothing is concurrently consuming, the same
    // pattern this type's own test follows).
    [[nodiscard]] std::uint64_t remaining() const noexcept { return remaining_; }

private:
    std::uint64_t remaining_ = 0;
    AsyncMutex mutex_;
};

}  // namespace agentengine::rt
