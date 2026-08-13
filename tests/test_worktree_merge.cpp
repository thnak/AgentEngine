// Proof for 025-Worktree-and-Virtual-Filesystem.md §4's three-way merge on branch join, Milestone 3
// Phase B2 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md) --
// `merge_trees`/`merge_branch_into_parent` (core/worktree.hpp) proven per the RFC's three explicit
// outcomes -- disjoint changes merge automatically, identical content merges trivially, a genuine
// conflict fails closed and retains both versions -- plus recursion into subtrees, add/add and
// edit/delete forks, and the stale-parent race check `merge_branch_into_parent` performs before
// committing (022 §5: each claim proven, not merely asserted at implementation time).

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

} // namespace

int main() {
    // B2-C1 (disjoint changes merge automatically): base has {a.txt}; ours adds b.txt, theirs adds
    // c.txt -- neither side touched the other's addition, so both land with no conflict.
    {
        InMemoryWorktreeObjectStore store;
        auto a = blob_of(store, "a-content");
        auto b = blob_of(store, "b-content");
        auto c = blob_of(store, "c-content");
        auto base = tree_of(store, {{"a.txt", a, false}});
        auto ours = tree_of(store, {{"a.txt", a, false}, {"b.txt", b, false}});
        auto theirs = tree_of(store, {{"a.txt", a, false}, {"c.txt", c, false}});

        auto result = merge_trees(store, base, ours, theirs);
        AE_CHECK(result.has_value() && result->ok(), "B2-C1: disjoint additions merge with no conflict");
        if (result.has_value() && result->ok()) {
            auto merged = store.get_tree(result->merged_tree_digest);
            AE_CHECK(merged.has_value() && merged->entries.size() == 3,
                     "B2-C1: merged tree contains all three files (a, b, c)");
        }
    }

    // B2-C2 (identical content trivially merges): both sides independently make the SAME edit to
    // a.txt -- not a conflict, since the resulting content agrees.
    {
        InMemoryWorktreeObjectStore store;
        auto a_v1 = blob_of(store, "v1");
        auto a_v2 = blob_of(store, "v2");
        auto base = tree_of(store, {{"a.txt", a_v1, false}});
        auto ours = tree_of(store, {{"a.txt", a_v2, false}});
        auto theirs = tree_of(store, {{"a.txt", a_v2, false}});

        auto result = merge_trees(store, base, ours, theirs);
        AE_CHECK(result.has_value() && result->ok(),
                 "B2-C2: identical edits on both sides merge trivially, no conflict");
        if (result.has_value() && result->ok()) {
            auto merged = store.get_tree(result->merged_tree_digest);
            AE_CHECK(merged.has_value() && merged->entries.size() == 1 &&
                         merged->entries[0].digest == a_v2,
                     "B2-C2: merged content is the agreed-upon v2");
        }
    }

    // B2-C3 (genuine conflict fails closed, both versions retained): both sides edit a.txt
    // DIFFERENTLY -- must surface as a conflict, never guessed, never last-writer-wins.
    {
        InMemoryWorktreeObjectStore store;
        auto a_v1 = blob_of(store, "v1");
        auto a_ours = blob_of(store, "ours-edit");
        auto a_theirs = blob_of(store, "theirs-edit");
        auto base = tree_of(store, {{"a.txt", a_v1, false}});
        auto ours = tree_of(store, {{"a.txt", a_ours, false}});
        auto theirs = tree_of(store, {{"a.txt", a_theirs, false}});

        auto result = merge_trees(store, base, ours, theirs);
        AE_CHECK(result.has_value() && !result->ok(), "B2-C3: divergent edits surface as a conflict");
        AE_CHECK(result.has_value() && result->conflicts.size() == 1 &&
                     result->conflicts[0].path == "a.txt",
                 "B2-C3: the conflict is reported at the right path");
        if (result.has_value() && !result->conflicts.empty()) {
            auto const& c = result->conflicts[0];
            AE_CHECK(c.base.has_value() && c.base->digest == a_v1, "B2-C3: base version retained");
            AE_CHECK(c.ours.has_value() && c.ours->digest == a_ours, "B2-C3: ours version retained");
            AE_CHECK(c.theirs.has_value() && c.theirs->digest == a_theirs,
                     "B2-C3: theirs version retained");
        }
    }

    // B2-C4 (recursion: disjoint changes in a shared subdirectory don't conflict at the parent
    // level): base has dir/{x.txt}; ours adds dir/y.txt, theirs adds dir/z.txt. Both sides changed
    // the SAME "dir" entry differently from base (different digest), but the changes inside it are
    // disjoint -- must merge cleanly, proving recursion actually happens rather than conflicting at
    // the directory boundary.
    {
        InMemoryWorktreeObjectStore store;
        auto x = blob_of(store, "x-content");
        auto y = blob_of(store, "y-content");
        auto z = blob_of(store, "z-content");
        auto base_dir = tree_of(store, {{"x.txt", x, false}});
        auto ours_dir = tree_of(store, {{"x.txt", x, false}, {"y.txt", y, false}});
        auto theirs_dir = tree_of(store, {{"x.txt", x, false}, {"z.txt", z, false}});
        auto base = tree_of(store, {{"dir", base_dir, true}});
        auto ours = tree_of(store, {{"dir", ours_dir, true}});
        auto theirs = tree_of(store, {{"dir", theirs_dir, true}});

        auto result = merge_trees(store, base, ours, theirs);
        AE_CHECK(result.has_value() && result->ok(),
                 "B2-C4: disjoint changes inside a commonly-modified subdirectory merge cleanly");
        if (result.has_value() && result->ok()) {
            auto merged = store.get_tree(result->merged_tree_digest);
            AE_CHECK(merged.has_value() && merged->entries.size() == 1 && merged->entries[0].is_tree,
                     "B2-C4: top level still has exactly one 'dir' entry");
            auto merged_dir = store.get_tree(merged->entries[0].digest);
            AE_CHECK(merged_dir.has_value() && merged_dir->entries.size() == 3,
                     "B2-C4: merged subdirectory contains x, y, AND z");
        }
    }

    // B2-C5 (recursion surfaces a real conflict at the right nested path, not the directory level):
    // same setup as C4, but both sides ALSO edit x.txt differently -- the disjoint y/z additions
    // still merge, but x.txt's divergent edit must surface as a conflict at "dir/x.txt", not
    // silently resolved and not reported merely as "dir" changed.
    {
        InMemoryWorktreeObjectStore store;
        auto x_v1 = blob_of(store, "x-v1");
        auto x_ours = blob_of(store, "x-ours");
        auto x_theirs = blob_of(store, "x-theirs");
        auto y = blob_of(store, "y-content");
        auto base_dir = tree_of(store, {{"x.txt", x_v1, false}});
        auto ours_dir = tree_of(store, {{"x.txt", x_ours, false}, {"y.txt", y, false}});
        auto theirs_dir = tree_of(store, {{"x.txt", x_theirs, false}});
        auto base = tree_of(store, {{"dir", base_dir, true}});
        auto ours = tree_of(store, {{"dir", ours_dir, true}});
        auto theirs = tree_of(store, {{"dir", theirs_dir, true}});

        auto result = merge_trees(store, base, ours, theirs);
        AE_CHECK(result.has_value() && !result->ok(),
                 "B2-C5: a genuine nested conflict is NOT masked by the sibling disjoint addition");
        AE_CHECK(result.has_value() && result->conflicts.size() == 1 &&
                     result->conflicts[0].path == "dir/x.txt",
                 "B2-C5: the conflict path is the full nested path, not just 'dir'");
    }

    // B2-C6 (delete/delete merges trivially; edit/delete forks): base has {a.txt, b.txt}. Both sides
    // delete a.txt (trivial, no conflict) but ours edits b.txt while theirs deletes it (a genuine
    // edit/delete fork -- must surface, not silently pick either side).
    {
        InMemoryWorktreeObjectStore store;
        auto a = blob_of(store, "a-content");
        auto b_v1 = blob_of(store, "b-v1");
        auto b_edited = blob_of(store, "b-edited");
        auto base = tree_of(store, {{"a.txt", a, false}, {"b.txt", b_v1, false}});
        auto ours = tree_of(store, {{"b.txt", b_edited, false}});   // deleted a.txt, edited b.txt
        auto theirs = tree_of(store, {});                            // deleted both

        auto result = merge_trees(store, base, ours, theirs);
        AE_CHECK(result.has_value() && !result->ok(),
                 "B2-C6: edit/delete fork on b.txt surfaces as a conflict");
        AE_CHECK(result.has_value() && result->conflicts.size() == 1 &&
                     result->conflicts[0].path == "b.txt" && !result->conflicts[0].theirs.has_value() &&
                     result->conflicts[0].ours.has_value() &&
                     result->conflicts[0].ours->digest == b_edited,
                 "B2-C6: the conflict correctly shows ours=edited, theirs=absent (deleted)");
    }

    // B2-C6b: isolate delete/delete on its own (no accompanying conflict) to prove it merges clean.
    {
        InMemoryWorktreeObjectStore store;
        auto a = blob_of(store, "a-content");
        auto b = blob_of(store, "b-content");
        auto base = tree_of(store, {{"a.txt", a, false}, {"b.txt", b, false}});
        auto ours = tree_of(store, {{"b.txt", b, false}});    // deleted a.txt only
        auto theirs = tree_of(store, {{"b.txt", b, false}});  // deleted a.txt only, identically

        auto result = merge_trees(store, base, ours, theirs);
        AE_CHECK(result.has_value() && result->ok(), "B2-C6b: identical deletion merges trivially");
    }

    // B2-C7 (add/add with different content is a conflict; add/add with identical content is not):
    // neither side has a base entry for "new.txt" -- both sides independently create it.
    {
        InMemoryWorktreeObjectStore store;
        auto content_same = blob_of(store, "same-new-file");
        auto base = tree_of(store, {});
        auto ours = tree_of(store, {{"new.txt", content_same, false}});
        auto theirs = tree_of(store, {{"new.txt", content_same, false}});

        auto result = merge_trees(store, base, ours, theirs);
        AE_CHECK(result.has_value() && result->ok(), "B2-C7: add/add identical content merges trivially");
    }
    {
        InMemoryWorktreeObjectStore store;
        auto ours_content = blob_of(store, "ours-new-file");
        auto theirs_content = blob_of(store, "theirs-new-file");
        auto base = tree_of(store, {});
        auto ours = tree_of(store, {{"new.txt", ours_content, false}});
        auto theirs = tree_of(store, {{"new.txt", theirs_content, false}});

        auto result = merge_trees(store, base, ours, theirs);
        AE_CHECK(result.has_value() && !result->ok() && result->conflicts.size() == 1 &&
                     !result->conflicts[0].base.has_value(),
                 "B2-C7: add/add divergent content is a conflict with no base entry");
    }

    // B2-C8 (blob-vs-tree type fork is a conflict, not a silent pick): ours turns "x" into a
    // subdirectory while theirs edits it as a file.
    {
        InMemoryWorktreeObjectStore store;
        auto x_v1 = blob_of(store, "x-v1");
        auto x_edited = blob_of(store, "x-edited");
        auto inner = blob_of(store, "inner-file");
        auto ours_subtree = tree_of(store, {{"inner.txt", inner, false}});
        auto base = tree_of(store, {{"x", x_v1, false}});
        auto ours = tree_of(store, {{"x", ours_subtree, true}});
        auto theirs = tree_of(store, {{"x", x_edited, false}});

        auto result = merge_trees(store, base, ours, theirs);
        AE_CHECK(result.has_value() && !result->ok() && result->conflicts.size() == 1 &&
                     result->conflicts[0].ours->is_tree && !result->conflicts[0].theirs->is_tree,
                 "B2-C8: a blob-vs-tree type fork surfaces as a conflict, not a silent resolution");
    }

    // B2-R1 (merge_branch_into_parent, happy path): a disjoint-changes branch merge actually
    // commits, and the resulting parent Ref reflects the merged tree.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto a = blob_of(obj_store, "a-content");
        auto base_tree = tree_of(obj_store, {{"a.txt", a, false}});

        auto parent = commit_ref(ref_store, "session:s-10", base_tree);
        AE_CHECK(parent.has_value(), "B2-R1: setup parent commit succeeds");
        auto branch = create_sub_worktree(ref_store, *parent, "session:s-10/agents/w", sharing_mode::branch);
        AE_CHECK(branch.has_value() && branch->base_digest == base_tree,
                 "B2-R1: freshly created branch records the parent's digest as its base");

        auto b = blob_of(obj_store, "b-content");
        auto branch_tree = tree_of(obj_store, {{"a.txt", a, false}, {"b.txt", b, false}});
        AE_CHECK(write_sub_worktree(ref_store, *branch, branch_tree).has_value(),
                 "B2-R1: setup: branch writes b.txt");

        auto outcome = merge_branch_into_parent(obj_store, ref_store, *branch, *parent);
        AE_CHECK(outcome.has_value() && outcome->ok() && outcome->parent_ref.has_value(),
                 "B2-R1: merge with no conflicting parent-side change commits cleanly");
        if (outcome.has_value() && outcome->ok()) {
            AE_CHECK(outcome->parent_ref->tree_digest == branch_tree,
                     "B2-R1: parent Ref now points at the merged (here: branch's) tree");
            auto reread = read_ref(ref_store, "session:s-10");
            AE_CHECK(reread.has_value() && reread->has_value() &&
                         (*reread)->tree_digest == branch_tree,
                     "B2-R1: re-reading the parent directly confirms the commit actually landed");
        }
    }

    // B2-R2 (merge_branch_into_parent, conflict path: NEVER last-writer-wins). Parent and branch
    // both edit a.txt differently. The merge must fail AND the parent Ref must NOT move -- proving
    // the "never partially applied, never silently resolved" guarantee, not just that an error value
    // came back.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto a_v1 = blob_of(obj_store, "v1");
        auto base_tree = tree_of(obj_store, {{"a.txt", a_v1, false}});

        auto parent = commit_ref(ref_store, "session:s-11", base_tree);
        AE_CHECK(parent.has_value(), "B2-R2: setup parent commit succeeds");
        auto branch = create_sub_worktree(ref_store, *parent, "session:s-11/agents/w", sharing_mode::branch);
        AE_CHECK(branch.has_value(), "B2-R2: setup: branch creation succeeds");

        auto a_ours = blob_of(obj_store, "ours-edit");
        auto branch_tree = tree_of(obj_store, {{"a.txt", a_ours, false}});
        AE_CHECK(write_sub_worktree(ref_store, *branch, branch_tree).has_value(),
                 "B2-R2: setup: branch edits a.txt");

        auto a_theirs = blob_of(obj_store, "theirs-edit");
        auto parent_moved_tree = tree_of(obj_store, {{"a.txt", a_theirs, false}});
        auto parent_after = commit_ref(ref_store, "session:s-11", parent_moved_tree);
        AE_CHECK(parent_after.has_value(), "B2-R2: setup: parent independently edits a.txt too");

        auto outcome = merge_branch_into_parent(obj_store, ref_store, *branch, *parent_after);
        AE_CHECK(outcome.has_value() && !outcome->ok() && !outcome->parent_ref.has_value(),
                 "B2-R2: divergent edit surfaces as a conflict, nothing committed by this call");

        auto reread = read_ref(ref_store, "session:s-11");
        AE_CHECK(reread.has_value() && reread->has_value() &&
                     (*reread)->tree_digest == parent_moved_tree,
                 "B2-R2: the parent Ref is UNCHANGED after a failed merge -- never last-writer-wins, "
                 "never a silent partial apply");
    }

    // B2-R3 (stale-parent race detection): caller observes the parent, then someone ELSE commits to
    // it before the caller's merge call lands -- must fail closed with a distinct, retriable error
    // code rather than silently merging against (and overwriting) a base that is no longer current.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto a = blob_of(obj_store, "a-content");
        auto base_tree = tree_of(obj_store, {{"a.txt", a, false}});

        auto parent_observed = commit_ref(ref_store, "session:s-12", base_tree);
        AE_CHECK(parent_observed.has_value(), "B2-R3: setup parent commit succeeds");
        auto branch =
            create_sub_worktree(ref_store, *parent_observed, "session:s-12/agents/w", sharing_mode::branch);
        AE_CHECK(branch.has_value(), "B2-R3: setup: branch creation succeeds");

        auto b = blob_of(obj_store, "b-content");
        auto branch_tree = tree_of(obj_store, {{"a.txt", a, false}, {"b.txt", b, false}});
        AE_CHECK(write_sub_worktree(ref_store, *branch, branch_tree).has_value(),
                 "B2-R3: setup: branch adds b.txt (disjoint -- would merge cleanly if not stale)");

        // Someone else moves the parent AFTER `parent_observed` was captured but BEFORE the merge
        // call below uses it.
        auto c = blob_of(obj_store, "c-content");
        auto other_writer_tree = tree_of(obj_store, {{"a.txt", a, false}, {"c.txt", c, false}});
        AE_CHECK(commit_ref(ref_store, "session:s-12", other_writer_tree).has_value(),
                 "B2-R3: setup: a different writer moves the parent in between");

        auto outcome = merge_branch_into_parent(obj_store, ref_store, *branch, *parent_observed);
        AE_CHECK(!outcome.has_value() && outcome.error().code == "worktree.merge_stale_parent",
                 "B2-R3: merging against a now-stale parent snapshot is rejected with a distinct code");

        auto reread = read_ref(ref_store, "session:s-12");
        AE_CHECK(reread.has_value() && reread->has_value() &&
                     (*reread)->tree_digest == other_writer_tree,
                 "B2-R3: the other writer's commit survives untouched -- the stale merge never landed");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All worktree merge proof checks passed.\n";
    return 0;
}
