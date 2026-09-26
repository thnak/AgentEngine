// Proof for 025-Worktree-and-Virtual-Filesystem.md §3/§10 Q2's `shared`-mode staleness note,
// Milestone 3 Phase B3 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md) --
// `diff_trees`/`summarize_diff`/`check_shared_staleness` (core/worktree.hpp) proven per the RFC's
// G7 gate: "A `shared` sub-worktree that changed since an agent's last read surfaces the staleness
// note on its next turn, measured, not asserted" (022 §5) -- the diffs below are computed from real
// Tree fixtures and real Ref commits, not asserted from a canned string.

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

Digest blob_of(InMemoryWorktreeObjectStore& store, std::string const& content) {
    std::vector<std::byte> bytes;
    bytes.reserve(content.size());
    for (char c : content) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return *store.put_blob(bytes);
}

Digest tree_of(InMemoryWorktreeObjectStore& store, std::vector<TreeEntry> entries) {
    return *store.put_tree(Tree{std::move(entries)});
}

std::size_t count_kind(TreeDiff const& diff, TreeDiffKind kind) {
    std::size_t n = 0;
    for (auto const& c : diff.changes) {
        if (c.kind == kind) ++n;
    }
    return n;
}

bool has_change(TreeDiff const& diff, std::string const& path, TreeDiffKind kind) {
    for (auto const& c : diff.changes) {
        if (c.path == path && c.kind == kind) return true;
    }
    return false;
}

} // namespace

int main() {
    // B3-C1: identical digests diff to nothing.
    {
        InMemoryWorktreeObjectStore store;
        auto a = blob_of(store, "a-content");
        auto t = tree_of(store, {{"a.txt", a, false}});

        auto diff = diff_trees(store, t, t);
        AE_CHECK(diff.has_value() && diff->changes.empty(), "B3-C1: identical trees diff to nothing");
        AE_CHECK(diff.has_value() && summarize_diff(*diff) == "no changes",
                 "B3-C1: summarize_diff reports 'no changes'");
    }

    // B3-C2: a single added top-level file.
    {
        InMemoryWorktreeObjectStore store;
        auto a = blob_of(store, "a-content");
        auto b = blob_of(store, "b-content");
        auto old_tree = tree_of(store, {{"a.txt", a, false}});
        auto new_tree = tree_of(store, {{"a.txt", a, false}, {"b.txt", b, false}});

        auto diff = diff_trees(store, old_tree, new_tree);
        AE_CHECK(diff.has_value() && diff->file_count() == 1, "B3-C2: exactly one file changed");
        AE_CHECK(diff.has_value() && has_change(*diff, "b.txt", TreeDiffKind::added),
                 "B3-C2: b.txt is reported as added");
        AE_CHECK(diff.has_value() && summarize_diff(*diff) == "1 file changed (1 added)",
                 "B3-C2: summarize_diff text matches exactly");
    }

    // B3-C3: a single modified top-level file (content changed, same name, both blobs).
    {
        InMemoryWorktreeObjectStore store;
        auto a_v1 = blob_of(store, "v1");
        auto a_v2 = blob_of(store, "v2");
        auto old_tree = tree_of(store, {{"a.txt", a_v1, false}});
        auto new_tree = tree_of(store, {{"a.txt", a_v2, false}});

        auto diff = diff_trees(store, old_tree, new_tree);
        AE_CHECK(diff.has_value() && diff->file_count() == 1 &&
                     has_change(*diff, "a.txt", TreeDiffKind::modified),
                 "B3-C3: a.txt is reported as modified, not added/removed");
    }

    // B3-C4: a removed top-level file.
    {
        InMemoryWorktreeObjectStore store;
        auto a = blob_of(store, "a-content");
        auto b = blob_of(store, "b-content");
        auto old_tree = tree_of(store, {{"a.txt", a, false}, {"b.txt", b, false}});
        auto new_tree = tree_of(store, {{"a.txt", a, false}});

        auto diff = diff_trees(store, old_tree, new_tree);
        AE_CHECK(diff.has_value() && diff->file_count() == 1 &&
                     has_change(*diff, "b.txt", TreeDiffKind::removed),
                 "B3-C4: b.txt is reported as removed");
    }

    // B3-C5: a whole nested subdirectory added -- must be walked all the way down so EACH file
    // inside it is its own entry, not one entry for the top-level directory name.
    {
        InMemoryWorktreeObjectStore store;
        auto x = blob_of(store, "x-content");
        auto y = blob_of(store, "y-content");
        auto new_dir = tree_of(store, {{"x.txt", x, false}, {"y.txt", y, false}});
        auto old_tree = tree_of(store, {});
        auto new_tree = tree_of(store, {{"dir", new_dir, true}});

        auto diff = diff_trees(store, old_tree, new_tree);
        AE_CHECK(diff.has_value() && diff->file_count() == 2,
                 "B3-C5: a newly-added directory counts as 2 files, not 1");
        AE_CHECK(diff.has_value() && has_change(*diff, "dir/x.txt", TreeDiffKind::added) &&
                     has_change(*diff, "dir/y.txt", TreeDiffKind::added),
                 "B3-C5: each nested file has its own full path");
    }

    // B3-C6: a disjoint change inside a commonly-modified subdirectory -- one file added, one
    // unchanged, proving the recursive diff doesn't report the whole directory as one blob change.
    {
        InMemoryWorktreeObjectStore store;
        auto x = blob_of(store, "x-content");
        auto y = blob_of(store, "y-content");
        auto old_dir = tree_of(store, {{"x.txt", x, false}});
        auto new_dir = tree_of(store, {{"x.txt", x, false}, {"y.txt", y, false}});
        auto old_tree = tree_of(store, {{"dir", old_dir, true}});
        auto new_tree = tree_of(store, {{"dir", new_dir, true}});

        auto diff = diff_trees(store, old_tree, new_tree);
        AE_CHECK(diff.has_value() && diff->file_count() == 1 &&
                     has_change(*diff, "dir/y.txt", TreeDiffKind::added),
                 "B3-C6: only the actually-added nested file is reported, x.txt is silent");
    }

    // B3-C7: a blob<->tree type change is reported as a full removed+added, not a misleading single
    // "modified" entry -- the old file's content is gone, and the new directory's files are new.
    {
        InMemoryWorktreeObjectStore store;
        auto x_v1 = blob_of(store, "x-as-file");
        auto inner = blob_of(store, "inner-content");
        auto new_subtree = tree_of(store, {{"inner.txt", inner, false}});
        auto old_tree = tree_of(store, {{"x", x_v1, false}});
        auto new_tree = tree_of(store, {{"x", new_subtree, true}});

        auto diff = diff_trees(store, old_tree, new_tree);
        AE_CHECK(diff.has_value() && diff->file_count() == 2, "B3-C7: type change yields 2 entries");
        AE_CHECK(diff.has_value() && has_change(*diff, "x", TreeDiffKind::removed) &&
                     has_change(*diff, "x/inner.txt", TreeDiffKind::added),
                 "B3-C7: the old file path is removed, the new directory's file is added");
    }

    // B3-C8: summarize_diff's breakdown covers all three kinds together, in a stable order.
    {
        InMemoryWorktreeObjectStore store;
        auto a_v1 = blob_of(store, "a-v1");
        auto a_v2 = blob_of(store, "a-v2");
        auto b = blob_of(store, "b-content");
        auto c = blob_of(store, "c-content");
        auto old_tree = tree_of(store, {{"a.txt", a_v1, false}, {"c.txt", c, false}});
        auto new_tree = tree_of(store, {{"a.txt", a_v2, false}, {"b.txt", b, false}});

        auto diff = diff_trees(store, old_tree, new_tree);
        AE_CHECK(diff.has_value() && count_kind(*diff, TreeDiffKind::modified) == 1 &&
                     count_kind(*diff, TreeDiffKind::added) == 1 &&
                     count_kind(*diff, TreeDiffKind::removed) == 1,
                 "B3-C8: setup: one of each kind in a single diff");
        AE_CHECK(diff.has_value() &&
                     summarize_diff(*diff) == "3 files changed (1 modified, 1 added, 1 removed)",
                 "B3-C8: summarize_diff's breakdown text matches exactly");
    }

    // B3-R1 (check_shared_staleness, not stale): an agent that reads immediately after its own
    // read sees no staleness -- no false positive.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto a = blob_of(obj_store, "a-content");
        auto base_tree = tree_of(obj_store, {{"a.txt", a, false}});
        auto parent = commit_ref(ref_store, "session:s-20", base_tree);
        AE_CHECK(parent.has_value(), "B3-R1: setup parent commit succeeds");
        auto shared = create_sub_worktree(ref_store, *parent, "session:s-20/agents/w", sharing_mode::shared);
        AE_CHECK(shared.has_value(), "B3-R1: setup shared sub-worktree creation succeeds");

        auto note = check_shared_staleness(obj_store, ref_store, *shared, base_tree);
        AE_CHECK(note.has_value() && !note->stale() && note->diff.changes.empty(),
                 "B3-R1: no staleness when nothing moved since the agent's last read");
    }

    // B3-R2 (check_shared_staleness, real staleness end-to-end): a SIBLING writes through the same
    // shared sub-worktree between the agent's reads -- measured against real commits, not asserted.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto a = blob_of(obj_store, "a-content");
        auto base_tree = tree_of(obj_store, {{"a.txt", a, false}});
        auto parent = commit_ref(ref_store, "session:s-21", base_tree);
        AE_CHECK(parent.has_value(), "B3-R2: setup parent commit succeeds");

        auto agent_view = create_sub_worktree(ref_store, *parent, "session:s-21/agents/reader",
                                               sharing_mode::shared);
        auto sibling_view = create_sub_worktree(ref_store, *parent, "session:s-21/agents/writer",
                                                 sharing_mode::shared);
        AE_CHECK(agent_view.has_value() && sibling_view.has_value(),
                 "B3-R2: setup: two shared sub-worktrees over the same parent");

        // The agent's "last read" is the tree digest at this point.
        Digest agent_last_read = base_tree;

        auto b = blob_of(obj_store, "b-content");
        auto moved_tree = tree_of(obj_store, {{"a.txt", a, false}, {"b.txt", b, false}});
        AE_CHECK(write_sub_worktree(ref_store, *sibling_view, moved_tree).has_value(),
                 "B3-R2: setup: the sibling writes b.txt through ITS OWN shared view");

        auto note = check_shared_staleness(obj_store, ref_store, *agent_view, agent_last_read);
        AE_CHECK(note.has_value() && note->stale(), "B3-R2: the agent's next turn sees staleness");
        AE_CHECK(note.has_value() && note->diff.file_count() == 1 &&
                     has_change(note->diff, "b.txt", TreeDiffKind::added),
                 "B3-R2: the staleness note's diff correctly attributes b.txt as added");
        AE_CHECK(note.has_value() && summarize_diff(note->diff) == "1 file changed (1 added)",
                 "B3-R2: the note's summary is the same count-only text summarize_diff produces");

        // Positive control: after the agent "catches up" (re-reads), staleness is gone again.
        auto caught_up = check_shared_staleness(obj_store, ref_store, *agent_view, moved_tree);
        AE_CHECK(caught_up.has_value() && !caught_up->stale(),
                 "B3-R2 (positive control): re-reading clears the staleness, proving it isn't sticky");
    }

    // B3-R3 (fails closed on non-shared modes): `branch`/`readonly`/`scratch` have no staleness
    // hazard by construction (private-until-merge, pinned, or discarded) -- calling
    // check_shared_staleness on them must be rejected, not silently return "not stale".
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto a = blob_of(obj_store, "a-content");
        auto base_tree = tree_of(obj_store, {{"a.txt", a, false}});
        auto parent = commit_ref(ref_store, "session:s-22", base_tree);
        AE_CHECK(parent.has_value(), "B3-R3: setup parent commit succeeds");

        auto branch = create_sub_worktree(ref_store, *parent, "session:s-22/agents/b", sharing_mode::branch);
        AE_CHECK(branch.has_value(), "B3-R3: setup branch creation succeeds");
        auto rejected = check_shared_staleness(obj_store, ref_store, *branch, base_tree);
        AE_CHECK(!rejected.has_value() && rejected.error().code == "worktree.staleness_requires_shared_mode",
                 "B3-R3: check_shared_staleness rejects a branch-mode sub-worktree with a stable code");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All worktree staleness proof checks passed.\n";
    return 0;
}
