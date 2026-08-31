// PROVE-PHASE PROBE (A4/§34): Ledger::merge() now actually calls the real three-way merge_trees()
// (§28.3) instead of the original stub's "parent wholesale-adopts the child's head." Exercises both
// a clean, real merge (two divergent, non-overlapping changes combined) and a real conflict (both
// sides changing the SAME path differently), through the Ledger API itself -- not the standalone
// merge_trees() pure function §28.3's own probe already covers.

#include "worktree_ledger.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace {
template <class T>
T run(agentengine::rt::task<T> t) { t.resume(); return t.take_value(); }

std::vector<std::byte> to_bytes(std::string const& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}
}  // namespace

int main() {
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal owner = authority.mint_root("merge-probe-owner");
    Ledger<> ledger;
    auto quota = AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
    CHECK(quota.has_value());
    auto branch_cost_quota = AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    CHECK(branch_cost_quota.has_value());

    auto root_r = run(ledger.create_root_branch(owner));
    CHECK(root_r.has_value());
    BranchHandle<> root = std::move(*root_r);

    auto a1 = ledger.put_blob_safe(to_bytes("A-original"), owner);
    CHECK(a1.has_value());
    agentengine::Tree t1;
    t1.entries.push_back(agentengine::TreeEntry{"a.txt", *a1, false});
    auto cp1 = run(ledger.commit(root, t1, owner, *quota));
    CHECK(cp1.has_value());
    std::printf("[1] root branch, turn 1: a.txt=\"A-original\" committed\n");

    // === Scenario 1: clean merge -- parent adds b.txt, child modifies a.txt. Different paths, ====
    // === both real changes relative to the shared base -- must merge cleanly, no conflict. =======
    {
        auto branch_r = run(ledger.branch_from(root, owner, *branch_cost_quota));
        CHECK(branch_r.has_value());
        BranchHandle<> child = std::move(*branch_r);

        // Parent: add b.txt (a.txt unchanged).
        auto b1 = ledger.put_blob_safe(to_bytes("B-from-parent"), owner);
        CHECK(b1.has_value());
        agentengine::Tree t_parent;
        t_parent.entries.push_back(agentengine::TreeEntry{"a.txt", *a1, false});
        t_parent.entries.push_back(agentengine::TreeEntry{"b.txt", *b1, false});
        auto cp_parent = run(ledger.commit(root, t_parent, owner, *quota));
        CHECK(cp_parent.has_value());

        // Child: modify a.txt (b.txt doesn't exist on this branch at all).
        auto a2 = ledger.put_blob_safe(to_bytes("A-from-child"), owner);
        CHECK(a2.has_value());
        agentengine::Tree t_child;
        t_child.entries.push_back(agentengine::TreeEntry{"a.txt", *a2, false});
        auto cp_child = run(ledger.commit(child, t_child, owner, *quota));
        CHECK(cp_child.has_value());

        auto merge_result = run(ledger.merge(std::move(child), root, owner));
        CHECK(merge_result.has_value());
        auto merged_tree = ledger.get_tree_safe(merge_result->tree, owner);
        CHECK(merged_tree.has_value());
        CHECK(merged_tree->entries.size() == 2);
        bool a_is_child_version = false, b_is_parent_version = false;
        for (auto const& e : merged_tree->entries) {
            if (e.name == "a.txt" && e.digest == *a2) a_is_child_version = true;
            if (e.name == "b.txt" && e.digest == *b1) b_is_parent_version = true;
        }
        CHECK(a_is_child_version);
        CHECK(b_is_parent_version);
        std::printf("[2] REAL CLEAN MERGE: a.txt correctly took the CHILD's change, b.txt correctly "
                    "kept the PARENT's change -- a real three-way merge combined two divergent, "
                    "non-overlapping edits, not a fast-forward wholesale adoption -- PASS\n");
    }

    // === Scenario 2: real conflict -- both sides modify a.txt to DIFFERENT content from the =======
    // === CURRENT root head -- merge must be REJECTED, not silently resolved either way. ===========
    {
        auto branch_r = run(ledger.branch_from(root, owner, *branch_cost_quota));
        CHECK(branch_r.has_value());
        BranchHandle<> child = std::move(*branch_r);

        auto a_parent_side = ledger.put_blob_safe(to_bytes("A-changed-by-parent"), owner);
        CHECK(a_parent_side.has_value());
        auto current_root_tree_r = ledger.head_tree_digest(root.name(), owner);
        CHECK(current_root_tree_r.has_value());
        agentengine::Digest const current_root_tree = *current_root_tree_r;
        auto current_tree = ledger.get_tree_safe(current_root_tree, owner);
        CHECK(current_tree.has_value());
        agentengine::Tree t_parent2;
        for (auto const& e : current_tree->entries) {
            if (e.name == "a.txt") t_parent2.entries.push_back(agentengine::TreeEntry{"a.txt", *a_parent_side, false});
            else t_parent2.entries.push_back(e);
        }
        auto cp_parent2 = run(ledger.commit(root, t_parent2, owner, *quota));
        CHECK(cp_parent2.has_value());

        auto a_child_side = ledger.put_blob_safe(to_bytes("A-changed-by-child"), owner);
        CHECK(a_child_side.has_value());
        agentengine::Tree t_child2;
        t_child2.entries.push_back(agentengine::TreeEntry{"a.txt", *a_child_side, false});
        auto cp_child2 = run(ledger.commit(child, t_child2, owner, *quota));
        CHECK(cp_child2.has_value());

        std::string const child_name_before = child.name();
        auto conflict_result = run(ledger.merge(std::move(child), root, owner));
        CHECK(!conflict_result.has_value());
        CHECK(conflict_result.error().code == "ledger.merge_conflict");
        std::printf("[3] REAL CONFLICT correctly REJECTED (code=%s): both sides changed a.txt to "
                    "different content from a common base -- merge does NOT silently pick a side\n",
                    conflict_result.error().code.c_str());

        // The child branch must still exist (a rejected merge must not consume/erase it) -- since
        // conflict resolution is out of scope, the caller needs the branch to still be there to
        // retry or abandon explicitly.
        auto still_there_r = ledger.head_tree_digest(child_name_before, owner);
        CHECK(still_there_r.has_value());
        std::printf("[4] the child branch was NOT erased by the rejected merge -- still readable "
                    "for a real caller to inspect -- PASS\n");

        // REAL FIX PROOF (a code-review pass found the merge-rejection paths used to strand the
        // branch with no LIVE HANDLE anywhere and no reclaim registration -- "still resolvable to
        // retry or abandon" was previously only true for READS, not for actually getting a handle
        // back). Confirm the rejected merge's child branch is now a genuine, reclaimable A7 orphan.
        auto orphans = ledger.orphaned_branches();
        CHECK(std::find(orphans.begin(), orphans.end(), child_name_before) != orphans.end());
        auto reclaimed = ledger.reclaim_orphaned_branch(child_name_before, owner);
        CHECK(reclaimed.has_value());
        CHECK(reclaimed->name() == child_name_before);
        std::printf("[5] the rejected merge's child branch is a REAL reclaimable orphan -- the "
                    "owner got a genuinely fresh, live BranchHandle back via reclaim_orphaned_branch(), "
                    "not just read-only introspection -- PASS\n");
        auto abandon_r = run(ledger.abandon(std::move(*reclaimed)));
        CHECK(abandon_r.has_value());
    }

    std::printf("\nALL CHECKS PASSED -- Ledger::merge() now performs a REAL three-way merge "
                "(clean-merge combining divergent non-overlapping edits, and real conflict "
                "detection+rejection for overlapping edits), closing the real "
                "\"merge_trees() proven standalone but never wired to Ledger::merge()\" gap A4 "
                "found.\n");
    return 0;
}
