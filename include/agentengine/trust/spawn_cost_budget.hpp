#pragma once
// Implements 026-Agent-Facing-Runtime-Surface.md §9 Q1's corrected `SpawnCostBudget` sketch
// (`OpenQuestions.md` OQ-14's cost-budget half) — a real Quark actor, not a bare copyable value
// type like `SpawnBudget` (spawn_budget.hpp). §9 Q1's own reasoning, restated because it is the
// whole point of this file existing separately from `SpawnBudget`: depth is a CEILING (ordinary
// for two concurrent siblings to each independently compute `parent_depth - 1` off the same
// parent, both correctly getting the same answer — `SpawnBudget::attenuate_for_spawn()` is
// therefore a pure, `const`, immutable-value-type call). Tokens are a CONSUMED POOL, not a
// ceiling: two concurrent `agent.spawn` calls (concurrency this engine already supports --
// `Parallelizable`, 006 §6b; `Concurrent`/map-reduce fan-out, 014 §3) each independently
// attenuating a bare copyable `remaining_tokens_` snapshot could each succeed up to the full
// remaining amount -- a double-spend that defeats the mechanism's entire purpose. Routing
// consumption through a real `quark::Actor<..., quark::Sequential>`'s own `Ask` handler closes
// this by construction: Quark's Sequential dispatch already guarantees at most one handler runs
// at a time per actor instance (the same invariant every other stateful actor in this codebase,
// including `AgentSession` itself, already relies on) -- two concurrent `ConsumeSpawnTokens`
// asks against the SAME `SpawnCostBudgetActor` are serialized by the engine itself, not by any
// lock this file writes.
//
// Scope (matching `decisions/ADR-006-agent-spawn-depth-budget-bound.md`'s own precedent):
// proven standalone, NOT wired to any real `agent.spawn` call path -- none exists yet in this
// codebase (`decisions/ADR-030-session-scoped-codeact-wiring.md`'s own survey; the real
// nested-agent-run invocation mechanism, sub-worktree wiring, and a production `AgentSession`
// tool-call loop hosting a spawned child are all separate, larger, currently-unbuilt
// prerequisites -- ADR-024 §7, `docs/architecture/worktree-sharing-skills-and-subagents.md` §3).
// What IS real here: the actor-serialized consume-without-double-spend primitive itself, proven
// under a real, multi-worker `quark::Engine` (not just `quark::TestKit`, which never actually
// dispatches two handlers concurrently in the first place and so could not exercise the hazard
// this file exists to close).
//
// Wall-clock/deadline budgeting stays explicitly, separately open (026 §9 Q1's own text) -- no
// existing per-run deadline-enforcement mechanism exists anywhere in this codebase to attenuate
// against yet, so this file does not invent one.
//
// Sourcing the amount a child receives is the CALLER's responsibility, not this actor's: 026 §9
// Q1 requires it come only from the target agent's own compiled `AgentMetadata::token_budget`,
// never from anything a model's own output could set (I2/I3) -- `SpawnCostBudgetActor` has no
// visibility into `AgentMetadata` at all, by design, so it cannot itself be the place that rule
// is enforced; whichever future `agent.spawn` call path wires this in must read that field itself
// before ever constructing a `ConsumeSpawnTokens{amount}`.

#include <cstdint>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"

#include "agentengine/core/error.hpp"

namespace agentengine::trust {

// The query: "may I spend `amount` tokens from this pool." No agent/model identity travels with
// it -- attribution of WHO is spending is the caller's own concern (a future `agent.spawn` call
// path would journal/audit that separately, the same "audit is the caller's job, not the
// primitive's" split `invoke_tool`'s own ten-step pipeline already establishes elsewhere).
struct ConsumeSpawnTokens {  // ae-naming-lint: allow ConsumeSpawnTokens — new 026 §9 Q1 vocabulary; 027 has not been updated to list it
    std::uint64_t amount = 0;
};

// Always responded with (never a silent no-reply) -- unlike `AgentSession`'s own fail-closed
// "never call m.respond()" idiom (agent_session.hpp), that idiom is safe there ONLY because every
// caller drives it through `quark::TestKit`, which resolves an unanswered ask synchronously and
// immediately. This actor is specifically proven under a REAL, multi-worker `quark::Engine`
// (`block_on`, off-lane) -- `reply_cell.hpp`'s own comment documents that a genuinely unanswered
// `Ask` there only resolves at actor TEARDOWN/reclaim, which would leave a live caller's
// `block_on` parked indefinitely for the ordinary "budget exhausted" case. Responding explicitly,
// every time, with `result<SpawnTokenGrant>`, sidesteps that hazard entirely rather than relying
// on a convention that was only ever verified safe under `TestKit`.
struct SpawnTokenGrant {  // ae-naming-lint: allow SpawnTokenGrant — new 026 §9 Q1 vocabulary; 027 has not been updated to list it
    std::uint64_t granted = 0;
};

class SpawnCostBudgetActor  // ae-naming-lint: allow SpawnCostBudgetActor — new 026 §9 Q1 vocabulary; 027 has not been updated to list it
    : public quark::Actor<SpawnCostBudgetActor, quark::Sequential> {
public:
    using protocol = quark::Protocol<quark::Ask<ConsumeSpawnTokens, result<SpawnTokenGrant>>>;

    // Configuration-time only, like `AgentSession::initialize()` -- call once, before this actor
    // is registered/reachable, never re-armed mid-lifetime (there is no reset/top-up method here
    // on purpose: a pool that could be refilled by a later call would need its own authority
    // question -- who may top it up, and from where -- 026 §9 Q1 does not ask for that, and this
    // file does not invent it as a drive-by).
    void initialize(std::uint64_t total_tokens) noexcept { remaining_ = total_tokens; }

    // Step 5's whole reason to exist: check-and-decrement as ONE atomic step within this actor's
    // own Sequential handler -- no other code path ever mutates `remaining_`, so there is no
    // window between "check" and "decrement" for a second concurrent ask to observe the
    // pre-decrement value (the exact double-spend 026 §9 Q1 names).
    void handle(quark::Ask<ConsumeSpawnTokens, result<SpawnTokenGrant>> const& m) {
        if (m.query.amount > remaining_) {
            m.respond(std::unexpected(error{failure_class::resource,
                                             "spawn cost budget exhausted",
                                             "spawn_cost_budget.exhausted"}));
            return;
        }
        remaining_ -= m.query.amount;
        m.respond(SpawnTokenGrant{m.query.amount});
    }

    [[nodiscard]] std::uint64_t remaining() const noexcept { return remaining_; }

private:
    std::uint64_t remaining_ = 0;
};

}  // namespace agentengine::trust
