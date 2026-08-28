// Proves ADR-102 Phase 2 (decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md's own
// roadmap; this test itself belongs to Phase 2, ported from docs/planning/proofs/worktree_io/
// worktree_ledger.hpp) -- Ledger<Store>/BranchHandle<Store>/Checkpoint/merge_trees() (core/ledger.hpp)
// actually work against the REAL agentengine::InMemoryWorktreeObjectStore, not a stand-in.
//
//   [1]  create_root_branch() produces a real branch; commit() stores a real Tree through the real
//        object store and produces a real, self-addressed Checkpoint.
//   [2]  get_tree_safe()/get_blob_safe() succeed for the authorized owner, fail closed for an
//        unrelated identity.
//   [3]  branch_from() produces a real child that inherits read access to the parent's head tree.
//   [4]  A clean three-way merge succeeds and folds the child's real commit into the parent.
//   [5]  A real merge CONFLICT (both sides changed the same path to different values) is rejected
//        closed (ledger.merge_conflict), and the child branch becomes a real, reclaimable orphan.
//   [6]  reset_to() rolls a branch's head back to an earlier real checkpoint.
//   [7]  Dropping a BranchHandle out of scope queues a real abandon; reap_pending_abandons() collects
//        it and the branch becomes genuinely unknown afterward.
//   [8]  The ACL root cap fails closed after the configured number of distinct roots; a re-touch by
//        an already-authorized root is still a no-op success, never counted against the cap twice.
//   [9]  mark_digest_shared(): unauthorized caller refused; after marking, ANY caller can read.
//   [10] A commit with two entries that case-fold to the same real path is rejected
//        (ledger.case_folding_collision).
//   [11] Durability round-trip: a real process-local Ledger destruction + reconstruction against the
//        SAME durable_dir restores the branch as a real, reclaimable orphan.
//
// Each distinct IdentityHandle that spends from an AsyncQuota gets its OWN quota, minted for that
// exact identity -- AsyncQuota::try_consume() is identity-scoped (ADR-102 Phase 1 C3), so sharing one
// identity's quota as a different identity's spending source is a real bug, not a convenience; an
// earlier version of this file did exactly that and the resulting (CORRECT) "unauthorized_spender"
// refusal was then dereferenced via `*result` with no `has_value()` guard, triggering real UB (a
// std::expected access with no value) that manifested as a hang later in the same run -- found and
// fixed while first bringing this test up, not a defect in the ported Ledger code itself.

#include "agentengine/core/ledger.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] std::vector<std::byte> to_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

}  // namespace

int main() {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    IdentityHandle owner = authority.mint_root("ledger-test-owner");
    IdentityHandle unrelated = authority.mint_root("ledger-test-unrelated");

    Ledger<> ledger;
    auto owner_branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    check(owner_branch_quota_r.has_value(), "AsyncQuota<BranchCost>::mint_root(owner) succeeds");
    auto owner_storage_quota_r =
        agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 1'000'000);
    check(owner_storage_quota_r.has_value(), "AsyncQuota<StorageBytes>::mint_root(owner) succeeds");
    auto& owner_branch_quota = *owner_branch_quota_r;
    auto& owner_storage_quota = *owner_storage_quota_r;

    // `child_identity` is a genuine IDENTITY descendant of `owner` (authority.derive_child()), NOT a
    // second independent mint_root() -- matching how this system is actually meant to be used (a
    // real child session's IdentityHandle is derived from its parent's, e.g.
    // mandatory_sandbox_provider.hpp's own spawn_child_branch()/IdentityAuthority::adopt() usage).
    // `Ledger`'s own branch ancestry (branch_from()) and `IdentityAuthority`'s own IDENTITY ancestry
    // (derive_child()/is_ancestor_of()) are two DIFFERENT ancestry relationships -- authorized_for()'s
    // real check is keyed on IDENTITY ancestry, so a "child" that is only branch-related but not
    // identity-related (two independent mint_root() calls, as an earlier version of this test used)
    // has NO inherited blob-level access, only the coarser tree-digest-level grant branch_from()
    // itself inserts directly -- a real, load-bearing distinction this test now models correctly.
    IdentityHandle child_identity = authority.derive_child(owner, "ledger-test-child-identity");
    auto child_branch_quota_r =
        agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, child_identity, 100);
    check(child_branch_quota_r.has_value(), "AsyncQuota<BranchCost>::mint_root(child_identity) succeeds");
    auto child_storage_quota_r =
        agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, child_identity, 1'000'000);
    check(child_storage_quota_r.has_value(),
          "AsyncQuota<StorageBytes>::mint_root(child_identity) succeeds");
    auto& child_branch_quota = *child_branch_quota_r;
    auto& child_storage_quota = *child_storage_quota_r;

    // ---- [1] create_root_branch() + commit() through the real object store. --------------------
    auto root_r = drive(ledger.create_root_branch(owner));
    check(root_r.has_value(), "create_root_branch() succeeds");
    BranchHandle<> root = std::move(*root_r);

    auto blob1_r = ledger.put_blob_safe(to_bytes("hello"), owner);
    check(blob1_r.has_value(), "put_blob_safe() succeeds through the real object store");

    Tree tree1;
    tree1.entries.push_back(TreeEntry{"note.txt", *blob1_r, /*is_tree=*/false});
    auto cp1_r = drive(ledger.commit(root, tree1, owner, owner_storage_quota));
    check(cp1_r.has_value(), "commit() succeeds and produces a real Checkpoint");
    check(cp1_r->turn_index == 1, "the first commit is turn_index 1");
    check(!cp1_r->self_digest.empty() && !cp1_r->tree.empty(),
          "the checkpoint carries real, non-empty digests");

    // ---- [2] get_tree_safe()/get_blob_safe(): authorized owner succeeds, unrelated refused. ------
    auto tree_read_r = ledger.get_tree_safe(cp1_r->tree, owner);
    check(tree_read_r.has_value(), "get_tree_safe() succeeds for the committing owner");
    check(tree_read_r->entries.size() == 1 && tree_read_r->entries[0].name == "note.txt",
          "the read-back tree genuinely contains what was committed");

    auto blob_read_r = ledger.get_blob_safe(*blob1_r, owner);
    check(blob_read_r.has_value(), "get_blob_safe() succeeds for the writing owner");

    auto tree_denied_r = ledger.get_tree_safe(cp1_r->tree, unrelated);
    check(!tree_denied_r.has_value() && tree_denied_r.error().code == "ledger.tree_access_denied",
          "get_tree_safe() fails closed for an unrelated identity");
    auto blob_denied_r = ledger.get_blob_safe(*blob1_r, unrelated);
    check(!blob_denied_r.has_value() && blob_denied_r.error().code == "ledger.blob_access_denied",
          "get_blob_safe() fails closed for an unrelated identity");

    // ---- [3] branch_from(): a real child inherits read access to the parent's head. --------------
    auto child_r = drive(ledger.branch_from(root, child_identity, child_branch_quota));
    check(child_r.has_value(), "branch_from() succeeds");
    if (!child_r.has_value()) { std::fprintf(stderr, "  error: %s\n", child_r.error().message.c_str()); return EXIT_FAILURE; }
    BranchHandle<> child = std::move(*child_r);
    auto inherited_read_r = ledger.get_tree_safe(cp1_r->tree, child_identity);
    check(inherited_read_r.has_value(),
          "the child's creator inherits real read access to the parent's head tree it branched from");

    // ---- [4] a clean three-way merge succeeds. -----------------------------------------------------
    // blob1 (note.txt) is re-referenced by the child's own commit below WITHOUT the child having
    // separately touched it -- this is only authorized because `child_identity` is a real IDENTITY
    // descendant of `owner` (authorized_for()'s ancestry check, not the coarser branch-level grant).
    auto blob2_r = ledger.put_blob_safe(to_bytes("child content"), child_identity);
    check(blob2_r.has_value(), "the child's own put_blob_safe() succeeds");
    Tree child_tree;
    child_tree.entries.push_back(TreeEntry{"note.txt", *blob1_r, false});
    child_tree.entries.push_back(TreeEntry{"child.txt", *blob2_r, false});
    auto child_commit_r = drive(ledger.commit(child, child_tree, child_identity, child_storage_quota));
    check(child_commit_r.has_value(), "the child's own commit() succeeds");
    if (!child_commit_r.has_value()) {
        std::fprintf(stderr, "  error: %s\n", child_commit_r.error().message.c_str());
    }

    // The merge REQUESTER is `child_identity`, not `owner` -- a real, load-bearing ACL asymmetry
    // this test models correctly: authorization flows DOWNWARD only (a descendant inherits read
    // access to what an ancestor wrote), never upward. `child_identity` is authorized for its own
    // tree (theirs) and, being a real descendant of `owner`, inherits access to blob1 (ours); `owner`
    // itself would NOT be authorized for blob2 (child_identity's own write) without this direction.
    //
    // CORRECTION (an independent red-team pass caught this): an earlier version of this comment
    // claimed this flow "matches the real 'the one who did the child's work merges it back' flow
    // this design's own task-branch tool track (A10) uses" -- checked directly against
    // `docs/planning/proofs/task_branch_tool/task_branch_sandbox.hpp` and found FALSE. A10 uses
    // exactly ONE identity throughout (`spawn_child_branch(owner_,...)`, `merge_into(*main_,
    // owner_)`) -- `derive_child()`/a real second identity never appears there at all, so A10
    // sidesteps this exact scenario entirely rather than answering it. This test is the first real
    // exercise of a genuine two-identity orchestrator/sub-agent merge flow against this Ledger.
    auto merge_r = drive(ledger.merge(std::move(child), root, child_identity));
    check(merge_r.has_value(), "a clean merge (child only ADDED a new file) succeeds");
    if (!merge_r.has_value()) {
        std::fprintf(stderr, "  error: %s\n", merge_r.error().message.c_str());
    }
    if (merge_r.has_value()) {
        auto merged_tree_r = ledger.get_tree_safe(merge_r->tree, child_identity);
        check(merged_tree_r.has_value() && merged_tree_r->entries.size() == 2,
              "the merged tree genuinely contains both the parent's and the child's real content");

        // REAL FIX, proven: `owner` -- the PARENT branch's own creator, who never stopped holding
        // `root`'s live BranchHandle -- retains real read access to its own branch's new head after
        // an authorized descendant merges into it. Before this fix (an independent red-team pass's
        // own MUST-FIX finding, confirmed live with a standalone probe before this test's own version
        // caught it too): this call failed with `ledger.tree_access_denied`, permanently locking the
        // orchestrator out of its own branch -- exactly the "spawn a sub-agent, it merges its work
        // back, the orchestrator resumes" flow this whole design exists to support.
        auto owner_post_merge_r = ledger.get_tree_safe(merge_r->tree, owner);
        check(owner_post_merge_r.has_value() && owner_post_merge_r->entries.size() == 2,
              "the PARENT branch's own creator retains real read access to its own branch's new head "
              "after a descendant's merge -- the orchestrator is never locked out of its own resource");
        auto owner_head_r = ledger.head_tree_digest(root.name(), owner);
        check(owner_head_r.has_value() && *owner_head_r == merge_r->tree,
              "head_tree_digest() for the branch's own owner also succeeds post-merge, not just "
              "get_tree_safe() by coincidence of a shared digest elsewhere");
    }

    // ---- [5] a real merge CONFLICT is rejected closed, and the child becomes a reclaimable orphan. -
    auto conflict_child_r = drive(ledger.branch_from(root, owner, owner_branch_quota));
    check(conflict_child_r.has_value(), "branch_from() for the conflict scenario succeeds");
    if (!conflict_child_r.has_value()) { return EXIT_FAILURE; }
    BranchHandle<> conflict_child = std::move(*conflict_child_r);

    // Parent changes note.txt to one value...
    auto parent_change_r = ledger.put_blob_safe(to_bytes("parent's new note"), owner);
    Tree parent_tree;
    parent_tree.entries.push_back(TreeEntry{"note.txt", *parent_change_r, false});
    auto parent_commit_r = drive(ledger.commit(root, parent_tree, owner, owner_storage_quota));
    check(parent_commit_r.has_value(), "the parent's own conflicting commit succeeds");

    // ...while the child independently changes note.txt to a DIFFERENT value.
    auto child_change_r = ledger.put_blob_safe(to_bytes("child's DIFFERENT new note"), owner);
    Tree conflicting_child_tree;
    conflicting_child_tree.entries.push_back(TreeEntry{"note.txt", *child_change_r, false});
    auto conflicting_child_commit_r =
        drive(ledger.commit(conflict_child, conflicting_child_tree, owner, owner_storage_quota));
    check(conflicting_child_commit_r.has_value(), "the child's own conflicting commit succeeds");

    std::string const conflict_child_name = conflict_child.name();
    auto conflict_merge_r = drive(ledger.merge(std::move(conflict_child), root, owner));
    check(!conflict_merge_r.has_value() && conflict_merge_r.error().code == "ledger.merge_conflict",
          "a real conflicting merge is rejected closed, never silently picking a side");

    auto orphans = ledger.orphaned_branches();
    bool const found_orphan =
        std::find(orphans.begin(), orphans.end(), conflict_child_name) != orphans.end();
    check(found_orphan,
          "a rejected merge registers the child as a real, reclaimable orphan -- never a dead end");
    auto reclaimed_r = ledger.reclaim_orphaned_branch(conflict_child_name, owner);
    check(reclaimed_r.has_value(), "the orphaned branch is genuinely reclaimable by an authorized "
                                     "identity");

    // ---- [6] reset_to() rolls back to an earlier real checkpoint. ---------------------------------
    auto reset_r = drive(ledger.reset_to(root, 1, owner));
    check(reset_r.has_value(), "reset_to() succeeds");
    if (reset_r.has_value()) {
        check(reset_r->tree == cp1_r->tree, "reset_to(1) genuinely restores turn 1's exact tree digest");
    }

    // ---- [7] a dropped handle queues a real abandon; reap collects it. ----------------------------
    auto reap_target_r = drive(ledger.branch_from(root, owner, owner_branch_quota));
    check(reap_target_r.has_value(), "branch_from() for the reap scenario succeeds");
    if (!reap_target_r.has_value()) { return EXIT_FAILURE; }
    std::string const reap_target_name = reap_target_r->name();
    {
        BranchHandle<> scoped = std::move(*reap_target_r);
        (void)scoped;
    }  // scoped's destructor queues an abandon here
    auto reaped = drive(ledger.reap_pending_abandons());
    check(reaped >= 1, "reap_pending_abandons() processes at least the just-dropped handle");
    auto gone_r = ledger.head_tree_digest(reap_target_name, owner);
    check(!gone_r.has_value() && gone_r.error().code == "ledger.unknown_branch",
          "the reaped branch is genuinely gone -- head_tree_digest() reports unknown_branch");

    // ---- [8] the ACL root cap fails closed; a re-touch by an already-authorized root is a no-op. ---
    Ledger<> capped_ledger(InMemoryWorktreeObjectStore{}, std::nullopt, /*max_acl_roots_per_digest=*/2);
    auto capped_root_r = drive(capped_ledger.create_root_branch(owner));
    check(capped_root_r.has_value(), "create_root_branch() on the capped ledger succeeds");
    if (!capped_root_r.has_value()) { return EXIT_FAILURE; }
    auto shared_blob_r = capped_ledger.put_blob_safe(to_bytes("shared"), owner);
    check(shared_blob_r.has_value(), "the first writer's put_blob_safe() succeeds (root 1 of 2)");
    // A second, distinct root touching the SAME digest via commit()'s own authorization path: mint a
    // second identity (with its OWN branch-cost quota), have it inherit access via a branch (the only
    // real path a second root legitimately gains access to inherited content).
    IdentityHandle second_root = authority.mint_root("capped-second-root");
    auto second_root_branch_quota_r =
        agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, second_root, 10);
    check(second_root_branch_quota_r.has_value(), "AsyncQuota<BranchCost>::mint_root(second_root) succeeds");
    auto capped_child_r =
        drive(capped_ledger.branch_from(capped_root_r.value(), second_root, *second_root_branch_quota_r));
    check(capped_child_r.has_value(), "branch_from() on the capped ledger succeeds (root 2 of 2)");

    IdentityHandle third_root = authority.mint_root("capped-third-root");
    auto third_root_branch_quota_r =
        agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, third_root, 10);
    check(third_root_branch_quota_r.has_value(), "AsyncQuota<BranchCost>::mint_root(third_root) succeeds");
    auto capped_over_cap_r =
        drive(capped_ledger.branch_from(capped_root_r.value(), third_root, *third_root_branch_quota_r));
    check(!capped_over_cap_r.has_value() &&
              capped_over_cap_r.error().code == "ledger.acl_root_cap_exceeded",
          "a THIRD distinct root past the configured cap of 2 is rejected closed");
    // Re-touching with an ALREADY-authorized root (the original owner) must still succeed -- never
    // counted against the cap a second time.
    auto retouch_r = capped_ledger.put_blob_safe(to_bytes("shared"), owner);
    check(retouch_r.has_value(),
          "a re-touch by an already-authorized root succeeds even though the cap is already met");

    // ---- [9] mark_digest_shared(): unauthorized refused; after marking, anyone can read. -----------
    auto mark_denied_r = ledger.mark_digest_shared(cp1_r->tree, /*is_tree=*/true, unrelated);
    check(!mark_denied_r.has_value() &&
              mark_denied_r.error().code == "ledger.mark_shared_unauthorized",
          "mark_digest_shared() refuses a caller with no prior authorization for the digest");
    auto mark_ok_r = ledger.mark_digest_shared(cp1_r->tree, /*is_tree=*/true, owner);
    check(mark_ok_r.has_value(), "mark_digest_shared() succeeds for an already-authorized caller");
    IdentityHandle stranger = authority.mint_root("total-stranger");
    auto stranger_read_r = ledger.get_tree_safe(cp1_r->tree, stranger);
    check(stranger_read_r.has_value(),
          "AFTER marking shared, a total stranger with no prior relationship can read the digest");

    // ---- [10] a case-folding collision is rejected. --------------------------------------------
    auto blob_a_r = ledger.put_blob_safe(to_bytes("A"), owner);
    auto blob_b_r = ledger.put_blob_safe(to_bytes("B"), owner);
    Tree colliding_tree;
    colliding_tree.entries.push_back(TreeEntry{"readme.txt", *blob_a_r, false});
    colliding_tree.entries.push_back(TreeEntry{"README.txt", *blob_b_r, false});
    auto collision_r = drive(ledger.commit(root, colliding_tree, owner, owner_storage_quota));
    check(!collision_r.has_value() && collision_r.error().code == "ledger.case_folding_collision",
          "a tree with two case-folding-colliding entry names is rejected before materialize() could "
          "silently drop one");

    // ---- [11] durability round-trip: destroy + reconstruct against the SAME durable_dir. -----------
    namespace fs = std::filesystem;
    fs::path const durable_dir = fs::temp_directory_path() / "ae_ledger_test_durable";
    std::error_code ec;
    fs::remove_all(durable_dir, ec);
    IdentityHandle durable_owner = authority.mint_root("durable-owner");
    auto durable_storage_quota_r =
        agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, durable_owner, 1'000'000);
    check(durable_storage_quota_r.has_value(),
          "AsyncQuota<StorageBytes>::mint_root(durable_owner) succeeds");
    std::string durable_branch_name;
    Digest durable_tree_digest;
    {
        Ledger<> durable_ledger(InMemoryWorktreeObjectStore{}, durable_dir);
        auto d_root_r = drive(durable_ledger.create_root_branch(durable_owner));
        check(d_root_r.has_value(), "create_root_branch() on a durable ledger succeeds");
        if (!d_root_r.has_value()) { return EXIT_FAILURE; }
        durable_branch_name = d_root_r->name();
        auto d_blob_r = durable_ledger.put_blob_safe(to_bytes("durable content"), durable_owner);
        Tree d_tree;
        d_tree.entries.push_back(TreeEntry{"durable.txt", *d_blob_r, false});
        auto d_cp_r = drive(durable_ledger.commit(*d_root_r, d_tree, durable_owner, *durable_storage_quota_r));
        check(d_cp_r.has_value(), "commit() on a durable ledger succeeds");
        if (d_cp_r.has_value()) durable_tree_digest = d_cp_r->tree;
        // durable_ledger (and d_root_r's live BranchHandle) go out of scope HERE -- a real process
        // exit has no chance to run any destructor at all, so this is at least as strong a test as
        // the real crash case, per this design's own "a BranchHandle cannot survive a process exit"
        // reasoning.
    }
    {
        // A FRESH InMemoryWorktreeObjectStore -- deliberately, not a bug in this test: Phase 2's own
        // scope note (this file's own header comment / core/ledger.hpp's file-top comment) is
        // explicit that a durable object-store CONFORMER (content durability) is NOT ported in this
        // phase, only Ledger's own branch/ACL bookkeeping durability. So this reopened ledger's
        // `store_` genuinely has no blob/tree content in it -- only `durable_dir`'s branch/ACL
        // snapshot is real and restored here.
        Ledger<> reopened_ledger(InMemoryWorktreeObjectStore{}, durable_dir);
        auto reopened_orphans = reopened_ledger.orphaned_branches();
        bool const restored = std::find(reopened_orphans.begin(), reopened_orphans.end(),
                                          durable_branch_name) != reopened_orphans.end();
        check(restored, "a NEW Ledger instance against the SAME durable_dir restores the branch as a "
                          "real orphan -- Ledger's own branch/ACL bookkeeping durability, in scope for "
                          "this phase");
        auto reclaim_r = reopened_ledger.reclaim_orphaned_branch(durable_branch_name, durable_owner);
        check(reclaim_r.has_value(),
              "the restored orphan is genuinely reclaimable by the same real, restored ACL check -- "
              "requires the durable ACL snapshot to have round-tripped the real authorized root id, "
              "not just the branch name");
        // Content readability is explicitly OUT of this phase's scope (see comment above) -- a
        // durable object-store conformer is Phase 3+ work, not asserted here as if it already worked.
        (void)durable_tree_digest;
    }
    fs::remove_all(durable_dir, ec);

    if (g_failures == 0) {
        std::printf("test_ledger: all checks passed\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
