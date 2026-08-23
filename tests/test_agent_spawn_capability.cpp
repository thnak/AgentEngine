// Proves docs/planning/agent-spawn-runtime-design-draft.md's items 4 and 5 (trust/
// agent_spawn_capability.hpp) -- OpenQuestions.md OQ-14's agent.spawn "sharpest case", the budget
// wiring and child-capability-minting half only (items 1/2/3/6 -- the Tool<> surface, the nested
// AgentSession drive, dynamic worktree minting, and OQ-16's session wiring -- are NOT built or
// tested here; see the design doc for their own scope).
//
//   T1 -- trust::check_and_consume_spawn_depth() (ADR-006's SpawnBudget, wired): a caller holding no
//         cap::AgentCall grant for the target id at all fails closed with "agent_spawn.not_available"
//         (§4.4a); a caller holding a grant with depth remaining succeeds and returns a budget one
//         level shallower; a caller whose held grant is ALREADY exhausted (remaining_depth == 0)
//         fails closed with SpawnBudget's own "spawn_budget.depth_exhausted" (ADR-006, unchanged) --
//         the depth-exhaustion-fails-closed requirement.
//   T2 -- rt::SpawnCostBudget (ADR-031, already-proven, driven directly): consuming within budget
//         succeeds and decrements; consuming beyond what remains is denied with
//         "spawn_cost_budget.exhausted" and does NOT decrement -- the cost-exhaustion-fails-closed
//         requirement, exercised the same way test_rt_spawn_cost_budget.cpp's own T1 already proves
//         the primitive standalone, here composed with the depth check.
//   T3 -- the two budgets WIRED TOGETHER in §2's own order (depth before cost, since depth is
//         pure/local and cost mutates shared state): a local helper composes
//         check_and_consume_spawn_depth() then SpawnCostBudget::consume(), and proves a depth
//         failure never touches the cost pool (no side effect from a check that was going to fail
//         anyway), while a depth success still lets a subsequent cost exhaustion fail the whole
//         attempt closed.
//   T4 -- trust::mint_child_spawn_capabilities() (Design B, §4.5), POSITIVE control: a caller
//         holding a superset of what the target declares (including a wider chain depth than the
//         target's own declared per-id ceiling) mints a child CapabilitySet that is (a) never wider
//         than the caller's held set, (b) bounded to exactly the target's declared ceiling plus the
//         worktree grant, and (c) has its AgentCall entry re-rooted to the TIGHTER of the caller's
//         live chain depth and the target's own declared depth (mirrors the design doc's C5).
//   T5 -- mint_child_spawn_capabilities(), NEGATIVE controls: (a) a caller missing an entry the
//         target's declared ceiling needs is rejected with "capability.attenuation_not_subsumed"
//         before anything is minted; (b) a caller holding a NUMERICALLY CAPPED grant (a real quota)
//         cannot satisfy a target whose (uncapped-by-construction) declared ceiling asks for that
//         same kind unrestricted -- the "never wider than the parent's held set even when the
//         request asks for more" property, exercised directly (mirrors ADR-059 R3's own widening
//         check, extended to this call path).
//   T6 -- multi-id independence (design doc's C5b): a target declaring AgentCall for TWO different
//         agent_ids, spawned by a caller with different live depth for each, produces two
//         independently tightened entries -- neither borrows the other's headroom (§9 I2-4).

#include <cstdio>
#include <string>

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/rt/spawn_cost_budget.hpp"
#include "agentengine/trust/agent_spawn_capability.hpp"

using agentengine::AgentMetadata;
using agentengine::Capability;
using agentengine::CapabilitySet;
using agentengine::Mount;
using agentengine::SpawnWorktreeGrant;
using agentengine::SubWorktree;
using agentengine::cap::AgentCall;
using agentengine::cap::Entropy;
using agentengine::cap::FsRead;
using agentengine::cap::FsWrite;
using agentengine::cap::NetOut;
using agentengine::rt::ConsumeSpawnTokens;
using agentengine::rt::SpawnCostBudget;
using agentengine::rt::SpawnTokenGrant;
using agentengine::trust::check_and_consume_spawn_depth;
using agentengine::trust::ChildSpawnGrant;
using agentengine::trust::mint_child_spawn_capabilities;

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

// Safe here for the identical reason test_rt_spawn_cost_budget.cpp's own `drive()` names: consume()'s
// only suspension point is its internal AsyncMutex lock(), uncontended in every case this file drives
// (one caller, no concurrent contender), so the fast path never genuinely suspends and one resume()
// call resolves it fully.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// §2's own ordering, composed locally: depth first (pure/local), cost second (mutates shared state)
// -- see this file's own top comment (T3) and the design doc's §2 "why this order" prose. This is
// NOT the design's own SpawnPump (§4.4c, item 2/3's orchestration, not built here) -- just enough
// composition to prove the two ALREADY-PROVEN primitives this file wires actually compose in the
// specified order, without reimplementing either.
agentengine::result<agentengine::trust::SpawnBudget> check_and_consume_both(
    CapabilitySet const& caller_held, std::string const& target_agent_id, SpawnCostBudget& cost_pool,
    std::uint64_t spawn_cost) {
    auto depth = check_and_consume_spawn_depth(caller_held, target_agent_id);
    if (!depth) return depth;  // fails closed BEFORE the cost pool is ever touched
    auto cost = drive(cost_pool.consume(ConsumeSpawnTokens{spawn_cost}));
    if (!cost) return std::unexpected(cost.error());
    return depth;
}

}  // namespace

int main() {
    // ---- T1: depth budget wiring (ADR-006, via CapabilitySet::find_agent_call()) -----------------
    {
        // T1a: no grant for this agent_id at all -- fails closed, agent_spawn.not_available.
        CapabilitySet const nothing_held = CapabilitySet::grant_root({});
        auto r = check_and_consume_spawn_depth(nothing_held, "helper");
        check(!r.has_value(), "T1a: no cap::AgentCall grant for the target id fails closed");
        if (!r.has_value()) {
            check(r.error().code == "agent_spawn.not_available",
                  "T1a: fails with agent_spawn.not_available, the collapsed unknown/ungranted code");
        }

        // T1b: a grant WITH remaining depth succeeds and narrows by exactly one level.
        CapabilitySet const has_depth = CapabilitySet::grant_root(
            {AgentCall{"helper", agentengine::trust::SpawnBudget::mint_root(2)}});
        auto r2 = check_and_consume_spawn_depth(has_depth, "helper");
        check(r2.has_value(), "T1b: a grant with remaining depth (2) succeeds");
        if (r2.has_value()) {
            check(r2->remaining_depth() == 1, "T1b: the returned budget is narrowed by exactly one level");
        }

        // T1c: a grant that is ALREADY exhausted (mint_root(0)) fails closed --
        // spawn_budget.depth_exhausted, ADR-006's own code, propagated unmodified.
        CapabilitySet const exhausted =
            CapabilitySet::grant_root({AgentCall{"helper", agentengine::trust::SpawnBudget::mint_root(0)}});
        auto r3 = check_and_consume_spawn_depth(exhausted, "helper");
        check(!r3.has_value(), "T1c: an exhausted depth grant fails closed -- I8");
        if (!r3.has_value()) {
            check(r3.error().code == "spawn_budget.depth_exhausted",
                  "T1c: fails with ADR-006's own spawn_budget.depth_exhausted code, unmodified");
        }

        // T1d: a grant for a DIFFERENT agent_id never authorizes this one (I2 -- no cross-id bleed).
        CapabilitySet const wrong_id =
            CapabilitySet::grant_root({AgentCall{"other-agent", agentengine::trust::SpawnBudget::mint_root(5)}});
        auto r4 = check_and_consume_spawn_depth(wrong_id, "helper");
        check(!r4.has_value(), "T1d (I2): a grant for a different agent_id does not authorize this spawn");
        if (!r4.has_value()) {
            check(r4.error().code == "agent_spawn.not_available", "T1d: same collapsed error code as T1a");
        }
    }

    // ---- T2: cost budget wiring (ADR-031, rt::SpawnCostBudget driven directly) --------------------
    {
        SpawnCostBudget pool;
        pool.initialize(1);

        auto ok = drive(pool.consume(ConsumeSpawnTokens{1}));
        check(ok.has_value() && ok->granted == 1, "T2: consuming exactly the remaining pool succeeds");
        check(pool.remaining() == 0, "T2: the pool is now exhausted");

        auto denied = drive(pool.consume(ConsumeSpawnTokens{1}));
        check(!denied.has_value(), "T2: cost exhaustion fails closed -- I8");
        if (!denied.has_value()) {
            check(denied.error().code == "spawn_cost_budget.exhausted",
                  "T2: fails with ADR-031's own spawn_cost_budget.exhausted code, unmodified");
        }
        check(pool.remaining() == 0, "T2: a DENIED consume does not further decrement an exhausted pool");
    }

    // ---- T3: the two budgets wired together, in §2's own order -------------------------------------
    {
        // T3a: a depth failure must never touch the cost pool at all (no wasted spend on a request
        // that was always going to fail the cheaper, earlier check).
        SpawnCostBudget pool;
        pool.initialize(10);
        CapabilitySet const no_grant = CapabilitySet::grant_root({});
        auto r = check_and_consume_both(no_grant, "helper", pool, 3);
        check(!r.has_value(), "T3a: depth failure fails the combined attempt closed");
        check(pool.remaining() == 10,
              "T3a: the cost pool is UNTOUCHED by a request that fails depth first -- no side effect "
              "for a request that was always going to be rejected");

        // T3b: depth succeeds but cost is then exhausted -- the whole attempt still fails closed,
        // and the (non-refundable, ADR-031 §7) token IS spent, matching the design's own named
        // residual (§8/§9 RC-3) rather than silently refunding.
        SpawnCostBudget tiny_pool;
        tiny_pool.initialize(1);
        CapabilitySet const has_grant =
            CapabilitySet::grant_root({AgentCall{"helper", agentengine::trust::SpawnBudget::mint_root(2)}});
        auto r2 = check_and_consume_both(has_grant, "helper", tiny_pool, 1);
        check(r2.has_value(), "T3b: depth AND cost both succeed within bounds -- a positive control");
        check(tiny_pool.remaining() == 0, "T3b: the single available token was actually spent");

        auto r3 = check_and_consume_both(has_grant, "helper", tiny_pool, 1);
        check(!r3.has_value(), "T3b: a second spawn attempt against the now-exhausted pool fails closed");
        if (!r3.has_value()) {
            check(r3.error().code == "spawn_cost_budget.exhausted",
                  "T3b: fails with the cost pool's own exhaustion code, even though depth still had "
                  "headroom -- BOTH primitives gate independently, either failing closes the spawn");
        }
    }

    // ---- T4: mint_child_spawn_capabilities(), positive control (Design B, §4.5) --------------------
    {
        CapabilitySet const caller_held = CapabilitySet::grant_root(
            {Entropy{}, AgentCall{"helper", agentengine::trust::SpawnBudget::mint_root(5)},
             NetOut{{"unrelated.example:443:https"}, std::nullopt, {}}});  // extra, unrelated grant --
                                                                            // must not leak through

        AgentMetadata target;
        target.capability_ceiling = {Entropy{}, AgentCall{"helper", agentengine::trust::SpawnBudget::mint_root(3)}};

        SpawnWorktreeGrant const worktree_grant{
            SubWorktree{"child-1", "spawn-ref", agentengine::sharing_mode::branch, {}, {}},
            Mount{"spawn-mount", "spawn-ref", ""},
            FsRead{"spawn-mount", "/workspace", std::nullopt},
            FsWrite{"spawn-mount", "/workspace", std::nullopt, std::nullopt}};

        // child_depth_budget simulates check_and_consume_spawn_depth()'s own output for THIS spawn --
        // live chain depth 2, tighter than the target's own declared depth (3).
        auto minted = mint_child_spawn_capabilities(
            caller_held, target, worktree_grant, agentengine::trust::SpawnBudget::mint_root(2));
        check(minted.has_value(), "T4: a caller covering the target's declared ceiling mints successfully");
        if (minted.has_value()) {
            // Never wider than the caller's held set: caller held 3 entries (Entropy, AgentCall,
            // extra NetOut); the child gets Entropy + AgentCall + 2 worktree grants = 4, and the
            // unrelated NetOut never appears.
            check(minted->capabilities.size() == 4,
                  "T4: the minted set is exactly {Entropy, AgentCall(re-rooted), FsRead, FsWrite} -- "
                  "bounded to the target's declared ceiling plus the worktree grant, not the caller's "
                  "larger held set");
            check(!minted->capabilities.contains_kind(agentengine::capability_kind::net_out),
                  "T4: the caller's extra, unrelated NetOut grant never leaks into the child");

            auto const& child_agent_call = minted->capabilities.find_agent_call("helper");
            check(child_agent_call.has_value(), "T4: the child holds a re-rooted AgentCall for \"helper\"");
            if (child_agent_call.has_value()) {
                check(child_agent_call->budget.remaining_depth() == 2,
                      "T4: re-rooted to the TIGHTER of the caller's live depth (2) and the target's "
                      "own declared depth (3) -- never the wider of the two (mirrors design C5)");
            }

            check(minted->capabilities.find_fs_read("spawn-mount", "/workspace").has_value(),
                  "T4: the freshly-minted worktree read grant is present on the new mount");
            check(minted->capabilities.find_fs_write("spawn-mount", "/workspace").has_value(),
                  "T4: the freshly-minted worktree write grant is present on the new mount");
        }
    }

    // ---- T5: mint_child_spawn_capabilities(), negative controls ------------------------------------
    {
        SpawnWorktreeGrant const empty_worktree_grant{};

        // T5a: caller missing an entry the target's declared ceiling needs -- rejected before
        // anything is minted.
        CapabilitySet const bare_caller = CapabilitySet::grant_root({Entropy{}});
        AgentMetadata target_needs_more;
        target_needs_more.capability_ceiling = {Entropy{},
                                                  NetOut{{"api.example:443:https"}, std::nullopt, {}}};
        auto rejected = mint_child_spawn_capabilities(bare_caller, target_needs_more,
                                                        empty_worktree_grant,
                                                        agentengine::trust::SpawnBudget::mint_root(1));
        check(!rejected.has_value(),
              "T5a: a caller missing an entry the target's ceiling needs is rejected -- the "
              "should-be-rejected control");
        if (!rejected.has_value()) {
            check(rejected.error().code == "capability.attenuation_not_subsumed",
                  "T5a: rejected with ADR-009's own attenuation-not-subsumed code");
        }

        // T5b (ADR-059 R3 shape, extended here): a caller holding a NUMERICALLY CAPPED FsWrite grant
        // (a real, host-hand-narrowed quota) cannot satisfy a target whose declared ceiling asks for
        // that same mount UNRESTRICTED (uncapped-by-construction, per every to_capability() overload
        // -- ADR-059 §4 R3's own named residual). This is the "never wider than the parent's held set
        // even when the request asks for more" property, exercised directly and NOT worked around.
        CapabilitySet const capped_caller =
            CapabilitySet::grant_root({FsWrite{"work", "", std::uint64_t{100}, std::nullopt}});
        AgentMetadata target_wants_uncapped;
        target_wants_uncapped.capability_ceiling = {FsWrite{"work", "", std::nullopt, std::nullopt}};
        auto rejected2 = mint_child_spawn_capabilities(capped_caller, target_wants_uncapped,
                                                         empty_worktree_grant,
                                                         agentengine::trust::SpawnBudget::mint_root(1));
        check(!rejected2.has_value(),
              "T5b: a caller holding only a CAPPED grant cannot mint a child an UNCAPPED one for the "
              "same kind+mount, even though the target's own declared ceiling asks for it -- the "
              "minted set can never be wider than the caller's own held set");
    }

    // ---- T6: multi-id independence (design doc C5b, §9 I2-4) ---------------------------------------
    {
        // Caller holds a live chain depth of 6 for EACH id (a real grant, generous enough to cover
        // whatever the target below declares) -- the point of this case is that a SINGLE shared
        // child_depth_budget (5, below) produces two DIFFERENT tightened outputs depending only on
        // each AgentCall entry's OWN ac->budget (the target's own declared per-id ceiling), never a
        // value borrowed from the other entry.
        CapabilitySet const caller_held = CapabilitySet::grant_root(
            {AgentCall{"cheap_helper", agentengine::trust::SpawnBudget::mint_root(6)},
             AgentCall{"privileged_ops", agentengine::trust::SpawnBudget::mint_root(6)}});

        AgentMetadata target;
        target.capability_ceiling = {AgentCall{"cheap_helper", agentengine::trust::SpawnBudget::mint_root(5)},
                                      AgentCall{"privileged_ops", agentengine::trust::SpawnBudget::mint_root(1)}};

        SpawnWorktreeGrant const empty_worktree_grant{};

        // child_depth_budget (2) is the SAME shared value fed to every loop iteration (matching
        // §4.5's own literal algorithm -- one child_depth_budget parameter, not one per target id).
        // cheap_helper's own declared depth (5) is WIDER than 2, so tighter = child_depth_budget = 2;
        // privileged_ops' own declared depth (1) is NARROWER than 2, so tighter = ac->budget = 1.
        // Two different outputs from the SAME shared input, driven entirely by each entry's own
        // ac->budget -- proving the computation is keyed per agent_id, not a single scalar decision
        // applied uniformly to every entry.
        auto minted = mint_child_spawn_capabilities(
            caller_held, target, empty_worktree_grant, agentengine::trust::SpawnBudget::mint_root(2));
        check(minted.has_value(), "T6: caller covers both declared AgentCall entries");
        if (minted.has_value()) {
            auto cheap = minted->capabilities.find_agent_call("cheap_helper");
            auto priv = minted->capabilities.find_agent_call("privileged_ops");
            check(cheap.has_value() && priv.has_value(), "T6: both re-rooted entries are present");
            if (cheap.has_value()) {
                check(cheap->budget.remaining_depth() == 2,
                      "T6: cheap_helper tightened to the shared child_depth_budget (2), tighter than "
                      "its own target-declared depth (5)");
            }
            if (priv.has_value()) {
                check(priv->budget.remaining_depth() == 1,
                      "T6: privileged_ops independently tightened to its OWN target-declared depth "
                      "(1), tighter than the SAME shared child_depth_budget (2) cheap_helper just "
                      "used -- neither entry's result was influenced by the other's ac->budget");
            }
        }
    }

    if (g_failures == 0) {
        std::printf("test_agent_spawn_capability: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_spawn_capability: %d failure(s)\n", g_failures);
    return 1;
}
