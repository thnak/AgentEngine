// PROVE-PHASE POSITIVE PROBE for GrantSet + IdentityAuthority::adopt() (the real Principal-bridging
// answer to §24.3's open question).

#include "grant_set.hpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace probe {
struct RollbackAuthority { std::uint32_t max_turns_back = 0; };
struct BranchCost { std::uint64_t cost = 1; };
}  // namespace probe

int main() {
    using namespace probe;
    IdentityAuthority& authority = IdentityAuthority::bootstrap();

    // ============================================================================================
    // Part A: GrantSet -- heterogeneous storage, multi-hop lookup, multi-grant, session isolation
    // ============================================================================================

    Principal owner = authority.mint_root("gs-owner");
    Principal child = authority.derive_child(owner, "gs-child");
    Principal grandchild = authority.derive_child(child, "gs-grandchild");
    Principal stranger = authority.mint_root("gs-stranger");

    GrantSet session_grants;
    session_grants.insert(authority.mint_grant(RollbackAuthority{5}, owner, owner));
    session_grants.insert(authority.mint_grant(BranchCost{1}, owner, owner));

    // 1. Heterogeneous storage -- two different Payload kinds in ONE GrantSet, no cross-contamination.
    CHECK(session_grants.count<RollbackAuthority>() == 1);
    CHECK(session_grants.count<BranchCost>() == 1);
    auto rollback = session_grants.find<RollbackAuthority>(owner);
    auto branch_cost = session_grants.find<BranchCost>(owner);
    CHECK(rollback.has_value() && rollback->payload().max_turns_back == 5);
    CHECK(branch_cost.has_value() && branch_cost->payload().cost == 1);
    std::printf("[1] heterogeneous storage (RollbackAuthority + BranchCost in one GrantSet): PASS\n");

    // 2. Multi-hop lookup through GrantSet -- a GRANDCHILD can find a grant issued to the ROOT,
    // through GrantSet::find(), not just via authorized() called directly (round 1 Finding 6's
    // missing enumeration surface, now proven end to end).
    auto rollback_for_grandchild = session_grants.find<RollbackAuthority>(grandchild);
    CHECK(rollback_for_grandchild.has_value());
    CHECK(rollback_for_grandchild->payload().max_turns_back == 5);
    std::printf("[2] GrantSet::find() resolves multi-hop (grandchild finds a root-issued grant): "
                "PASS\n");

    // 3. An unrelated principal finds nothing.
    CHECK(!session_grants.find<RollbackAuthority>(stranger).has_value());
    std::printf("[3] GrantSet::find() correctly returns nothing for an unrelated principal: PASS\n");

    // 4. find_all() -- a caller holding TWO independent grants of the same kind gets both.
    GrantSet multi_grant_set;
    multi_grant_set.insert(authority.mint_grant(RollbackAuthority{3}, owner, owner));
    multi_grant_set.insert(authority.mint_grant(RollbackAuthority{10}, owner, owner));
    auto all_rollback = multi_grant_set.find_all<RollbackAuthority>(owner);
    CHECK(all_rollback.size() == 2);
    std::printf("[4] find_all() returns ALL matching grants (%zu), not just the first: PASS\n",
                all_rollback.size());

    // 5. Session isolation -- a SECOND, independent GrantSet for a different owner has NO visibility
    // into the first session's grants, even though both share the same process-wide IdentityAuthority.
    Principal other_owner = authority.mint_root("gs-other-owner");
    GrantSet other_session_grants;
    other_session_grants.insert(authority.mint_grant(RollbackAuthority{99}, other_owner, other_owner));
    CHECK(!other_session_grants.find<RollbackAuthority>(owner).has_value());
    CHECK(!session_grants.find<RollbackAuthority>(other_owner).has_value());
    std::printf("[5] two independent GrantSets (simulating two sessions) do NOT leak into each "
                "other: PASS\n");

    // ============================================================================================
    // Part B: IdentityAuthority::adopt() -- bridging the real agentengine::Principal (string-keyed)
    // ============================================================================================

    // 6. Idempotent adoption: adopting the SAME real principal id twice returns the SAME internal id.
    Principal adopted_1 = authority.adopt("real-user-42", "");
    Principal adopted_2 = authority.adopt("real-user-42", "");
    CHECK(adopted_1.id() == adopted_2.id());
    std::printf("[6] IdentityAuthority::adopt() is idempotent: PASS (same real id -> same internal "
                "Principal, id=%llu both times)\n", (unsigned long long)adopted_1.id());

    // 7. Real ancestry preserved WHEN the parent was adopted first (matching a real
    // agentengine::Principal::derive_on_behalf_of() chain: parent adopted, then child).
    Principal real_parent = authority.adopt("real-parent-1", "");
    Principal real_child = authority.adopt("real-child-1", "real-parent-1");
    CHECK(authority.is_ancestor_of(real_parent.id(), real_child.id()));
    std::printf("[7] adopt() preserves real on_behalf_of ancestry when the parent was adopted "
                "first: PASS\n");

    // 8. DISCLOSED LIMITATION, proven not just asserted: if the CHILD is adopted BEFORE its real
    // parent has ever been adopted, the bridge cannot know about the relationship -- it registers
    // the child as an independent root. This is exactly the honest residual named in the design
    // doc's §24.3, now demonstrated for real rather than left as an assumption.
    Principal orphan_child = authority.adopt("real-child-2", "real-parent-2-never-adopted");
    Principal late_parent = authority.adopt("real-parent-2-never-adopted", "");
    CHECK(!authority.is_ancestor_of(late_parent.id(), orphan_child.id()));
    std::printf("[8] DISCLOSED LIMITATION confirmed: adopting a child before its real parent has "
                "ever been adopted registers it as an independent root -- ancestry is NOT "
                "retroactively established when the parent is adopted later. This is the exact "
                "residual §24.3 named, now proven rather than assumed.\n");

    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
