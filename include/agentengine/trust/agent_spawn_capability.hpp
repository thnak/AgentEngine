#pragma once
// Implements docs/planning/agent-spawn-runtime-design-draft.md §4.4a (depth-budget wiring, reusing
// trust/spawn_budget.hpp's ADR-006 SpawnBudget unmodified) and §4.5 (child capability minting,
// "Design B" -- the design doc's own recommended, red-teamed-clean alternative, §3) for
// 026-Agent-Facing-Runtime-Surface.md §5's `agent.spawn` -- OpenQuestions.md OQ-14, named the
// project's own "sharpest case".
//
// SCOPE, exactly matching the task that produced this file: items 4 (wire the two already-proven,
// already-standalone budget primitives -- trust::SpawnBudget/ADR-006 and rt::SpawnCostBudget/
// ADR-031 -- into a real call path so agent.spawn fails closed per I8 on either exhaustion) and 5
// (mint_child_spawn_capabilities -- the security-critical function stating exactly what a spawned
// child inherits from its caller, per I2/I3) of the design doc's six-piece breakdown. Items 1/2/3/6
// (the Tool<> surface, the nested-AgentSession drive mechanism, dynamic sub-worktree minting, and
// OQ-16's manifest wired into a real session) are NOT built here -- see the design doc for their own
// file list. rt::SpawnCostBudget itself needs no new type or call site of its own (§4.4b: "no new
// type at all" -- callers drive `SpawnCostBudget::consume()` directly, exactly as its own header and
// test already prove standalone); the orchestration point that submits a `ConsumeSpawnTokens`
// request ahead of `mint_child_spawn_capabilities` below is the design's own SpawnPump (§4.4c),
// which belongs to the full orchestration pipeline (items 1/2/3) this file does not build. This
// header's own test (tests/test_agent_spawn_capability.cpp) exercises both budgets side by side --
// depth via check_and_consume_spawn_depth() below, cost via rt::SpawnCostBudget::consume() called
// directly -- in the same order §2 of the design doc specifies (depth, pure/local, before cost,
// which mutates shared state), proving the two ALREADY-PROVEN primitives compose correctly without
// reimplementing either.
//
// SpawnWorktreeGrant below mirrors workflow/worktree_scoping.hpp's ExecutorWorktreeGrant exactly
// (same four fields, same "write is also nullopt for readonly" caveat) per the design doc's own
// §4.3 spec -- it is a plain data bag with no minting logic of its own. The real dynamic-worktree
// mint (derive_spawn_child_id()/mint_spawn_worktree(), the collision-proof, caller-scoped minting
// this design's §4.3/§9 WT-1/WT-2/WT-3 red-team findings require) is item 3's own
// core/agent_spawn_worktree.hpp, not yet built. The type is declared here, not there, only because
// mint_child_spawn_capabilities()'s own signature (§4.5) needs it to compile and be independently
// testable ahead of item 3 landing -- a future core/agent_spawn_worktree.hpp should reuse this exact
// type rather than defining a second one.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/agent_registry.hpp"  // AgentMetadata
#include "agentengine/core/error.hpp"
#include "agentengine/core/worktree.hpp"        // SubWorktree, Mount
#include "agentengine/trust/capability.hpp"     // CapabilitySet, Capability, cap::AgentCall/FsRead/FsWrite
#include "agentengine/trust/spawn_budget.hpp"   // SpawnBudget (ADR-006)

namespace agentengine {

// §4.3's data shape -- see this file's own top comment for why it lives here rather than in item
// 3's not-yet-built header. Never constructed from anything a model's own tool-call output could
// influence (I3); a real mint (item 3) is host/engine policy, never guest-derived.
struct SpawnWorktreeGrant {
    SubWorktree sub;
    std::optional<Mount> mount;
    std::optional<cap::FsRead> read;
    std::optional<cap::FsWrite> write;
};

namespace trust {

// §2 step [2] / §4.4a. The ONLY entry point this file offers for the depth half of item 4: looks up
// the CALLER's own held cap::AgentCall grant for this exact target_agent_id (CapabilitySet::
// find_agent_call(), added alongside this file) and attenuates it one level
// (SpawnBudget::attenuate_for_spawn(), ADR-006, unmodified). Fails closed with
// "agent_spawn.not_available" if the caller holds no grant for this id at all -- deliberately the
// SAME code an unknown-registry-id case would use (§4.1/§9 I3-3's enumeration-oracle fix; this
// function alone cannot distinguish "unknown to the registry" from "known but ungranted", so it only
// ever reports the ungranted half, and a caller composing this with registry lookup must collapse
// both onto this one code, not invent a second one). Once a grant IS found but its own remaining
// depth is exhausted, SpawnBudget::attenuate_for_spawn() itself returns
// "spawn_budget.depth_exhausted" (ADR-006's own code, unchanged) -- this function does not
// re-wrap or rename that. Pure -- no shared/durable state touched, matching §2's own ordering
// rationale (depth, cheap and local, is checked before cost, which mutates a shared pool).
[[nodiscard]] inline result<SpawnBudget> check_and_consume_spawn_depth(
    CapabilitySet const& caller_held, std::string const& target_agent_id) {
    std::optional<cap::AgentCall> const grant = caller_held.find_agent_call(target_agent_id);
    if (!grant) {
        return std::unexpected(error{failure_class::policy,
                                      "caller holds no agent.spawn grant for this agent id",
                                      "agent_spawn.not_available"});
    }
    // attenuate_for_spawn() already returns spawn_budget.depth_exhausted on exhaustion (ADR-006) --
    // propagated verbatim, not re-derived.
    return grant->budget.attenuate_for_spawn();
}

// §4.5's ChildSpawnGrant. `child_depth_budget` is carried PURELY as caller-side bookkeeping/
// observability (e.g. logging how much depth headroom this spawn consumed) -- per §9 I2-2, it is
// NEVER separately merged into `capabilities` outside the re-rooting loop mint_child_spawn_
// capabilities() performs below. A target whose declared capability_ceiling contains no
// cap::AgentCall entry for itself grants the child ZERO further-spawn authority, full stop,
// regardless of how much headroom this field still has -- self-recursion is opt-in by the TARGET's
// own author, declared in its own ceiling, never ambient because a caller's live chain happened to
// have depth left.
struct ChildSpawnGrant {
    CapabilitySet capabilities;
    SpawnBudget child_depth_budget;
};

// §2 steps [3]+[6], item 5's central function -- Design B (§3 of the design doc), the only
// design of the three red-teamed alternatives that was NOT rejected. Implements ADR-059's
// already-Judged discipline ("never grant the target's declared ceiling directly; attenuate the
// CALLER's own HELD set down to it... bounded on both sides at once") extended with the
// recursion-specific AgentCall re-rooting step ADR-059 itself never needed (a single dispatched
// tool call has no notion of "further recursion").
//
// Fails closed with "capability.attenuation_not_subsumed" (ADR-009's own existing code, reused
// unmodified, matching ADR-059's own error shape for the identical failure) the moment ANY entry
// in target_metadata.capability_ceiling is not contains()-covered by caller_held -- before anything
// below is assembled, so a request that would end up rejected never partially mutates anything.
//
// What this function NEVER does, restated for a red-teamer (§5 of the design doc, I2/I3):
//   - never grants target_metadata.capability_ceiling directly via grant_root() (Design A, rejected
//     outright -- the exact bug ADR-059 already found and fixed for invoke_agent_tool());
//   - never copies caller_held's raw contents into the child (only entries BOTH the caller holds
//     AND the target declares wanting ever appear in the result, narrower on both sides at once);
//   - never lets `child_depth_budget`'s headroom manufacture a cap::AgentCall entry the target's own
//     declared ceiling doesn't already literally contain (§9 I2-2) -- an empty declared ceiling
//     mints zero further-spawn authority no matter how much live chain depth remains;
//   - never trusts worktree_grant as exempt from the caller-coverage discipline "because the mount
//     is new" -- by the time this function sees it, a real mint_spawn_worktree() (item 3, not built
//     here) is responsible for having already intersected branch-mode grants with what caller_held
//     covers on the caller's OWN mount (§9 I2-1); this function appends worktree_grant's read/write
//     entries verbatim, exactly as designed, and does not itself re-derive that intersection.
[[nodiscard]] inline result<ChildSpawnGrant> mint_child_spawn_capabilities(
    CapabilitySet const& caller_held, AgentMetadata const& target_metadata,
    SpawnWorktreeGrant const& worktree_grant, SpawnBudget const& child_depth_budget) {
    // Verify coverage FIRST -- attenuate()'s own all-or-nothing rule, reused unmodified (ADR-009).
    // The returned CapabilitySet is discarded; only the success/fail verdict is needed, since a
    // successful attenuate() already proves final_grants below (copied straight from
    // target_metadata.capability_ceiling) is fully caller-covered, entry for entry.
    result<CapabilitySet> const covered = caller_held.attenuate(target_metadata.capability_ceiling);
    if (!covered) {
        return std::unexpected(covered.error());
    }

    std::vector<Capability> final_grants;
    final_grants.reserve(target_metadata.capability_ceiling.size() + 2);
    for (Capability const& c : target_metadata.capability_ceiling) {
        if (auto const* ac = std::get_if<cap::AgentCall>(&c)) {
            // Re-root to the TIGHTER of: the live chain depth inherited from the caller
            // (child_depth_budget, already decremented by check_and_consume_spawn_depth()), and the
            // target's own declared per-id ceiling (ac->budget, an upper bound stated by the target
            // agent's OWN author, independent of any live chain). Narrowing an already-covered entry
            // further can never violate the coverage attenuate() just proved.
            //
            // INVARIANT (§9 I2-4, stated explicitly, not merely emergent from this formula): this
            // computation is keyed by `ac->agent_id`, INDEPENDENTLY, once per loop iteration -- never
            // a single scalar shared across every AgentCall entry in the ceiling. A target declaring
            // further-spawn authority for TWO different agent_ids gets two independently tightened
            // entries; neither can ever borrow the other's headroom (proven by this file's own
            // multi-id test, mirroring the design doc's C5b).
            SpawnBudget const tighter =
                (child_depth_budget.remaining_depth() <= ac->budget.remaining_depth())
                    ? child_depth_budget
                    : ac->budget;
            final_grants.push_back(cap::AgentCall{ac->agent_id, tighter});
        } else {
            final_grants.push_back(c);  // verbatim -- already proven covered above
        }
    }

    // Freshly host-minted authority for a mount that never existed before this call -- see this
    // function's own top comment for why it is trusted verbatim here rather than re-intersected.
    if (worktree_grant.read) {
        final_grants.push_back(*worktree_grant.read);
    }
    if (worktree_grant.write) {
        final_grants.push_back(*worktree_grant.write);
    }

    return ChildSpawnGrant{CapabilitySet::grant_root(std::move(final_grants)), child_depth_budget};
}

}  // namespace trust
}  // namespace agentengine
