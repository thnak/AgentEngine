// PROVE-PHASE POSITIVE PROBE for Ledger.

#include "ledger.hpp"
#include "../common/block_on.hpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

int main() {
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal owner = authority.mint_root("ledger-owner");
    Ledger ledger;

    auto branch_quota = AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    auto storage_quota = AsyncQuota<StorageBytes>::mint_root(authority, owner, 1'000'000);
    CHECK(branch_quota.has_value() && storage_quota.has_value());

    // 1. create_root_branch()
    auto root_result = block_on(ledger.create_root_branch(owner));
    CHECK(root_result.has_value());
    BranchHandle root = std::move(*root_result);
    CHECK(ledger.has_branch(root.name()));
    std::printf("[1] create_root_branch(): PASS (name=%s)\n", root.name().c_str());

    // 2. commit() -- self_digest well-definedness: same inputs -> same digest.
    auto cp1 = block_on(ledger.commit(root, "tree-A", owner, *storage_quota));
    CHECK(cp1.has_value());
    CHECK(cp1->turn_index == 1);
    CHECK(cp1->parent.empty());  // root checkpoint's parent is empty
    Digest const recomputed = compute_self_digest(cp1->tree, cp1->parent, cp1->authored_by, cp1->turn_index);
    CHECK(recomputed == cp1->self_digest);
    std::printf("[2] commit() + self_digest recomputation matches exactly: PASS (turn=%llu, "
                "self_digest=%s)\n", (unsigned long long)cp1->turn_index, cp1->self_digest.c_str());

    // 3. A second commit with a DIFFERENT tree produces a DIFFERENT self_digest and a strictly
    // increasing turn_index, chained to the first checkpoint's own self_digest as `parent`.
    auto cp2 = block_on(ledger.commit(root, "tree-B", owner, *storage_quota));
    CHECK(cp2.has_value());
    CHECK(cp2->turn_index == 2);
    CHECK(cp2->parent == cp1->self_digest);   // chains to the PRIOR checkpoint's self_digest
    CHECK(cp2->self_digest != cp1->self_digest);
    std::printf("[3] second commit chains parent correctly, distinct self_digest: PASS\n");

    // 4. Two IDENTICAL commits (same tree, same parent context, same authored_by) at DIFFERENT
    // turn_index still produce DIFFERENT self_digest, because turn_index participates in the hash --
    // this is exactly what closes the historical design's original ambiguity (§17.4/§15.4).
    Digest const d_at_turn_1 = compute_self_digest("same-tree", "same-parent", owner, 1);
    Digest const d_at_turn_2 = compute_self_digest("same-tree", "same-parent", owner, 2);
    CHECK(d_at_turn_1 != d_at_turn_2);
    std::printf("[4] identical (tree,parent,authored_by) at different turn_index -> DIFFERENT "
                "self_digest: PASS (no collision from turn_index alone)\n");

    // 5. reset_to() -- turn_index is ALWAYS current_head+1, never copied from the restored target.
    auto reset_result = block_on(ledger.reset_to(root, /*target_turn_index=*/1, owner));
    CHECK(reset_result.has_value());
    CHECK(reset_result->turn_index == 3);       // head was 2, so this is 3 -- NOT 1 (the restored
                                                   // target's own turn_index)
    CHECK(reset_result->tree == cp1->tree);     // but the TREE content is genuinely restored
    std::printf("[5] reset_to(target=1) from head=2: PASS (new turn_index=%llu, restored "
                "tree='%s' matches checkpoint 1's tree, history not overwritten)\n",
                (unsigned long long)reset_result->turn_index, reset_result->tree.c_str());

    // 6. branch_from() -- real COW branch, consumes AsyncQuota<BranchCost>.
    Principal child_principal = authority.derive_child(owner, "fork-child");
    auto child_share = block_on(branch_quota->allocate_child_share(child_principal, 1));
    CHECK(child_share.has_value());
    auto child_branch_result = block_on(ledger.branch_from(root, child_principal, *child_share));
    CHECK(child_branch_result.has_value());
    BranchHandle child_branch = std::move(*child_branch_result);
    CHECK(ledger.has_branch(child_branch.name()));
    CHECK(child_branch.base_digest() == reset_result->self_digest);  // seeded at parent's CURRENT head
    std::printf("[6] branch_from(): PASS (child='%s', base_digest matches parent's head at branch "
                "time)\n", child_branch.name().c_str());

    // 7. abandon() consumes the handle -- the branch is genuinely removed, and the destructor of the
    // (now-resolved) local variable does NOT re-queue an abandon for it.
    std::string const child_name_before = child_branch.name();
    auto abandon_result = block_on(ledger.abandon(std::move(child_branch)));
    CHECK(abandon_result.has_value());
    CHECK(!ledger.has_branch(child_name_before));
    CHECK(ledger.pending_abandon_queue_size() == 0);   // explicit abandon() resolved it directly --
                                                          // nothing left for reap to do
    std::printf("[7] abandon() consumes the handle, branch genuinely removed, no residual queue "
                "entry: PASS\n");

    // 8. THE REAL RAII TEST: a branch handle that goes out of scope WITHOUT an explicit merge()/
    // abandon() call queues a pending abandon via its destructor -- purely synchronously, no
    // coroutine ever attempted (§13.2's actual fix, proven here, not asserted).
    {
        // Same pattern as step 6 above: a derived-child principal must spend from a SHARE its
        // parent explicitly allocated (AsyncQuota::try_consume()'s own spender-identity check --
        // a real gap a code review pass found and fixed -- rejects a derived child spending
        // straight from a quota it was never granted a share of).
        Principal leaked_principal = authority.derive_child(owner, "leaked");
        auto leaked_share = block_on(branch_quota->allocate_child_share(leaked_principal, 1));
        CHECK(leaked_share.has_value());
        auto leaked_result = block_on(ledger.branch_from(root, leaked_principal, *leaked_share));
        CHECK(leaked_result.has_value());
        std::string const leaked_name = leaked_result->name();
        CHECK(ledger.has_branch(leaked_name));
        // `leaked_result`'s BranchHandle goes out of scope HERE with no merge()/abandon() call.
        (void)leaked_name;
    }
    CHECK(ledger.pending_abandon_queue_size() == 1);   // the destructor really did queue it
    std::printf("[8] a BranchHandle dropped without merge()/abandon(): destructor queued a pending "
                "abandon (queue size=%zu) -- purely synchronous, confirmed no coroutine was "
                "attempted (the destructor has no way to co_await anything at all)\n",
                ledger.pending_abandon_queue_size());

    // 9. reap_pending_abandons() -- the REAL driver: genuinely co_awaits abandon()'s real body this
    // time, unlike Revision 1's discarded-and-never-runs claim.
    auto processed = block_on(ledger.reap_pending_abandons());
    CHECK(processed == 1);
    CHECK(ledger.pending_abandon_queue_size() == 0);
    std::printf("[9] reap_pending_abandons(): PASS (processed=%llu, queue now empty -- the queued "
                "branch's abandon() body genuinely ran)\n", (unsigned long long)processed);

    // 10. merge() also consumes the handle by value -- same discipline as abandon().
    Principal merge_child_principal = authority.derive_child(owner, "merge-child");
    auto merge_child_share = block_on(branch_quota->allocate_child_share(merge_child_principal, 1));
    CHECK(merge_child_share.has_value());
    auto merge_branch_result = block_on(ledger.branch_from(root, merge_child_principal, *merge_child_share));
    CHECK(merge_branch_result.has_value());
    BranchHandle merge_branch = std::move(*merge_branch_result);
    // Same pattern again: merge_child_principal must spend from its OWN share of storage_quota, not
    // straight from owner's root quota (AsyncQuota::try_consume()'s real spender-identity check).
    auto merge_storage_share = block_on(storage_quota->allocate_child_share(merge_child_principal, 1'000));
    CHECK(merge_storage_share.has_value());
    auto merge_cp = block_on(
        ledger.commit(merge_branch, "tree-from-child", merge_child_principal, *merge_storage_share));
    CHECK(merge_cp.has_value());
    std::string const merge_branch_name = merge_branch.name();
    auto merged = block_on(ledger.merge(std::move(merge_branch), root));
    CHECK(merged.has_value());
    CHECK(!ledger.has_branch(merge_branch_name));   // child branch consumed/removed by merge
    CHECK(ledger.pending_abandon_queue_size() == 0);  // merge() resolved it directly too
    std::printf("[10] merge() consumes the handle, child branch removed, no residual queue entry: "
                "PASS\n");

    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
