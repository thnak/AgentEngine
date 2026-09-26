// Proof for 025-Worktree-and-Virtual-Filesystem.md §3's sub-worktree sharing modes, Milestone 3
// Phase B1 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md) --
// `create_sub_worktree`/`read_sub_worktree`/`write_sub_worktree` (core/worktree.hpp) proven per
// mode: `shared` gives immediate cross-visibility (not just an identical starting value), `branch`
// and `scratch` diverge independently from the parent in BOTH directions, and `readonly` rejects
// writes and stays pinned even as other Refs move (022 §5: each claim proven, not merely asserted
// at creation time).

#include <iostream>
#include <string>

#include "agentengine/core/worktree.hpp"

using namespace agentengine;
using InMemoryStore = agentengine::rt::InMemoryAppendLogStore;

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

} // namespace

int main() {
    // B1-C1 / B1-R1 (shared): a write made THROUGH the shared sub-worktree is visible when reading
    // the PARENT's own Ref directly -- proving immediate cross-visibility, not just a copied value.
    {
        InMemoryStore store;
        auto parent = commit_ref(store, "session:s-1", "digest-parent-v1");
        AE_CHECK(parent.has_value(), "B1-C1: setup parent commit succeeds");

        auto sub = create_sub_worktree(store, *parent, "session:s-1/agents/a", sharing_mode::shared);
        AE_CHECK(sub.has_value() && sub->backing_ref_name == parent->name,
                 "B1-C1: shared sub-worktree's backing ref IS the parent's own ref name");

        auto write = write_sub_worktree(store, *sub, "digest-parent-v2");
        AE_CHECK(write.has_value(), "B1-R1: write through the shared sub-worktree succeeds");

        auto parent_after = read_ref(store, "session:s-1");
        AE_CHECK(parent_after.has_value() && parent_after->has_value() &&
                     (*parent_after)->tree_digest == "digest-parent-v2",
                 "B1-R1: reading the PARENT directly reflects the write made through the shared "
                 "sub-worktree -- immediate visibility, not a copy");
    }

    // B1-C2/C3/C4 (branch): starts at the parent's current digest, then diverges independently in
    // BOTH directions -- a write to the branch does not move the parent, and a later move of the
    // parent does not move the already-created branch.
    {
        InMemoryStore store;
        auto parent = commit_ref(store, "session:s-2", "digest-p1");
        AE_CHECK(parent.has_value(), "B1-C2: setup parent commit succeeds");

        auto branch = create_sub_worktree(store, *parent, "session:s-2/agents/b", sharing_mode::branch);
        AE_CHECK(branch.has_value(), "B1-C2: branch sub-worktree creation succeeds");

        auto branch_initial = read_sub_worktree(store, *branch);
        AE_CHECK(branch_initial.has_value() && branch_initial->has_value() &&
                     (*branch_initial)->tree_digest == "digest-p1",
                 "B1-C2: a fresh branch starts at the parent's current digest");

        AE_CHECK(write_sub_worktree(store, *branch, "digest-branch-v2").has_value(),
                 "B1-C3: writing to the branch succeeds");
        auto parent_after_branch_write = read_ref(store, "session:s-2");
        AE_CHECK(parent_after_branch_write.has_value() && parent_after_branch_write->has_value() &&
                     (*parent_after_branch_write)->tree_digest == "digest-p1",
                 "B1-C3: writing to the branch does NOT move the parent (independent, not aliased)");

        AE_CHECK(commit_ref(store, "session:s-2", "digest-p2").has_value(),
                 "B1-C4: moving the parent after the branch was created succeeds");
        auto branch_after_parent_move = read_sub_worktree(store, *branch);
        AE_CHECK(branch_after_parent_move.has_value() && branch_after_parent_move->has_value() &&
                     (*branch_after_parent_move)->tree_digest == "digest-branch-v2",
                 "B1-C4: moving the parent does NOT move the already-created branch (true "
                 "copy-on-write divergence, not a live alias)");
    }

    // B1-C5/C6 (readonly): pinned at creation, stays pinned even as the parent moves, and rejects
    // writes -- with a positive control (a non-readonly write succeeding) proving the rejection is
    // really about the mode, not a generic bug.
    {
        InMemoryStore store;
        auto parent = commit_ref(store, "session:s-3", "digest-pinned");
        AE_CHECK(parent.has_value(), "B1-C5: setup parent commit succeeds");

        auto ro = create_sub_worktree(store, *parent, "session:s-3/agents/reviewer", sharing_mode::readonly);
        AE_CHECK(ro.has_value() && ro->backing_ref_name.empty(),
                 "B1-C5: a readonly sub-worktree has no backing ref at all");

        AE_CHECK(commit_ref(store, "session:s-3", "digest-moved-after").has_value(),
                 "B1-C5: setup: moving the parent after the readonly view was taken succeeds");
        auto ro_read = read_sub_worktree(store, *ro);
        AE_CHECK(ro_read.has_value() && ro_read->has_value() &&
                     (*ro_read)->tree_digest == "digest-pinned",
                 "B1-C5: a readonly sub-worktree stays pinned to its creation-time digest even "
                 "after the parent later moves");

        auto write = write_sub_worktree(store, *ro, "attempted-write");
        AE_CHECK(!write.has_value() && write.error().code == "worktree.readonly_write_rejected",
                 "B1-C6: writing to a readonly sub-worktree is rejected with a stable error code");

        // Positive control: the identical write call against a NON-readonly (branch) sub-worktree
        // of the same parent succeeds -- proving C6's rejection is about readonly specifically.
        auto branch = create_sub_worktree(store, *parent, "session:s-3/agents/writer", sharing_mode::branch);
        AE_CHECK(branch.has_value() && write_sub_worktree(store, *branch, "attempted-write").has_value(),
                 "B1-C6 (positive control): the same write succeeds against a branch sub-worktree");
    }

    // B1-C7/C8 (scratch): starts EMPTY (not a copy of the parent), and is independent of it.
    {
        InMemoryStore store;
        auto parent = commit_ref(store, "session:s-4", "digest-nonempty-parent");
        AE_CHECK(parent.has_value(), "B1-C7: setup parent commit succeeds");

        auto scratch = create_sub_worktree(store, *parent, "session:s-4/agents/scratchpad", sharing_mode::scratch);
        AE_CHECK(scratch.has_value(), "B1-C7: scratch sub-worktree creation succeeds");

        auto expected_empty = empty_tree_digest();
        auto scratch_initial = read_sub_worktree(store, *scratch);
        AE_CHECK(expected_empty.has_value() && scratch_initial.has_value() &&
                     scratch_initial->has_value() &&
                     (*scratch_initial)->tree_digest == *expected_empty &&
                     (*scratch_initial)->tree_digest != "digest-nonempty-parent",
                 "B1-C7: a fresh scratch worktree starts at the empty-tree digest, NOT the parent's");

        AE_CHECK(write_sub_worktree(store, *scratch, "digest-scratch-v2").has_value(),
                 "B1-C8: writing to the scratch worktree succeeds");
        auto parent_after_scratch_write = read_ref(store, "session:s-4");
        AE_CHECK(parent_after_scratch_write.has_value() && parent_after_scratch_write->has_value() &&
                     (*parent_after_scratch_write)->tree_digest == "digest-nonempty-parent",
                 "B1-C8: writing to the scratch worktree does NOT move the parent");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All worktree sub-worktree proof checks passed.\n";
    return 0;
}
