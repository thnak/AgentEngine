// Proof for ADR-055 (2026-08-10-full-codebase-adr-gap-audit.md gap #14): `conflicts_ref_name()`/
// `materialize_merge_conflicts()` (core/worktree.hpp), closing 025-Worktree-and-Virtual-Filesystem.md
// §4's "the merge fails and is surfaced, with both versions retained at /conflicts/<path>.<agent>"
// requirement. Modeled on test_worktree_merge.cpp's own fixture shape (blob_of/tree_of helpers, the
// same InMemoryWorktreeObjectStore/InMemoryAppendLogStore pair) so the two can be compared side by
// side.
//
//   M1 -- conflicts_ref_name() is deterministic and derived: the SAME parent name always produces the
//         SAME conflicts ref name; two DIFFERENT parents never collide.
//   M2 -- end-to-end: a real merge_branch_into_parent() conflict is materialized -- both ours's and
//         theirs's content land at <path>.<ours_agent_id>/<path>.<branch's own name>, retrievable
//         through the conflicts ref's own tree -- and the PARENT ref stays completely untouched
//         (025 §4: never partially applied).
//   M3 -- an add/add divergence (no `base`): both ours and theirs are materialized.
//   M4 -- an edit/delete fork (one side absent): only the PRESENT side is materialized -- no entry is
//         fabricated for the side that didn't exist.
//   M5 -- a SECOND failed merge against the SAME parent ACCUMULATES into the SAME conflicts ref --
//         the first attempt's evidence survives, the second attempt's evidence is added alongside it,
//         nothing is silently overwritten (unless the exact same <path>.<agent> key repeats).
//   M6 -- an empty conflicts list is a no-op: no conflicts ref is created at all.

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

}  // namespace

int main() {
    // --- M1: deterministic and derived, no collision across distinct parents ------------------------
    {
        AE_CHECK(conflicts_ref_name("session:s-1") == conflicts_ref_name("session:s-1"),
                 "M1: the same parent name always produces the same conflicts ref name");
        AE_CHECK(conflicts_ref_name("session:s-1") != conflicts_ref_name("session:s-2"),
                 "M1: two different parents never collide on the same conflicts ref name");
        AE_CHECK(conflicts_ref_name("session:s-1") == "session:s-1:conflicts",
                 "M1: the derivation is the parent's own name, suffixed -- matching memory_ref_name's "
                 "own pattern shape");
    }

    // --- M2: end-to-end -- a REAL merge conflict, materialized, parent untouched ---------------------
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto a_v1 = blob_of(obj_store, "v1");
        auto base_tree = tree_of(obj_store, {{"a.txt", a_v1, false}});

        auto parent = commit_ref(ref_store, "session:s-20", base_tree);
        AE_CHECK(parent.has_value(), "M2 setup: parent commit succeeds");
        auto branch = create_sub_worktree(ref_store, *parent, "session:s-20/agents/writer",
                                           sharing_mode::branch);
        AE_CHECK(branch.has_value(), "M2 setup: branch creation succeeds");

        auto a_ours = blob_of(obj_store, "ours-edit");
        auto branch_tree = tree_of(obj_store, {{"a.txt", a_ours, false}});
        AE_CHECK(write_sub_worktree(ref_store, *branch, branch_tree).has_value(),
                 "M2 setup: branch edits a.txt");

        auto a_theirs = blob_of(obj_store, "theirs-edit");
        auto parent_moved_tree = tree_of(obj_store, {{"a.txt", a_theirs, false}});
        auto parent_after = commit_ref(ref_store, "session:s-20", parent_moved_tree);
        AE_CHECK(parent_after.has_value(), "M2 setup: parent independently edits a.txt too");

        auto outcome = merge_branch_into_parent(obj_store, ref_store, *branch, *parent_after);
        AE_CHECK(outcome.has_value() && !outcome->ok(), "M2 setup: the merge genuinely conflicts");

        auto materialized = materialize_merge_conflicts(obj_store, ref_store, "session:s-20", "planner",
                                                          *branch, outcome->conflicts);
        AE_CHECK(materialized.has_value(), "M2: materialize_merge_conflicts succeeds");

        auto conflicts_ref = read_ref(ref_store, conflicts_ref_name("session:s-20"));
        AE_CHECK(conflicts_ref.has_value() && conflicts_ref->has_value(),
                 "M2: a real conflicts ref now exists");
        if (conflicts_ref.has_value() && conflicts_ref->has_value()) {
            auto tree = obj_store.get_tree((*conflicts_ref)->tree_digest);
            AE_CHECK(tree.has_value() && tree->entries.size() == 2,
                     "M2: exactly two entries -- ours and theirs -- were materialized");
            TreeEntry const* ours_entry = nullptr;
            TreeEntry const* theirs_entry = nullptr;
            for (auto const& e : tree->entries) {
                if (e.name == "a.txt.planner") ours_entry = &e;
                if (e.name == "a.txt.session:s-20/agents/writer") theirs_entry = &e;
            }
            AE_CHECK(ours_entry != nullptr && ours_entry->digest == a_ours,
                     "M2: ours's content lands at <path>.<ours_agent_id>, the real content preserved");
            AE_CHECK(theirs_entry != nullptr && theirs_entry->digest == a_theirs,
                     "M2: theirs's content lands at <path>.<branch's own name>, the real content "
                     "preserved");
        }

        auto parent_reread = read_ref(ref_store, "session:s-20");
        AE_CHECK(parent_reread.has_value() && parent_reread->has_value() &&
                     (*parent_reread)->tree_digest == parent_moved_tree,
                 "M2: the PARENT ref is completely untouched -- materialization commits only to the "
                 "genuinely separate conflicts ref, 025 §4's own never-partially-applied guarantee");
    }

    // --- M3: an add/add divergence (no base) -- both sides materialized -----------------------------
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto ours_blob = blob_of(obj_store, "ours-new-file");
        auto theirs_blob = blob_of(obj_store, "theirs-new-file");

        SubWorktree branch;
        branch.name = "session:s-21/agents/w2";

        MergeConflict c;
        c.path   = "new.txt";
        c.base   = std::nullopt;
        c.ours   = TreeEntry{"new.txt", ours_blob, false};
        c.theirs = TreeEntry{"new.txt", theirs_blob, false};

        auto materialized =
            materialize_merge_conflicts(obj_store, ref_store, "session:s-21", "planner", branch, {c});
        AE_CHECK(materialized.has_value(), "M3: materialize_merge_conflicts succeeds for an add/add fork");

        auto conflicts_ref = read_ref(ref_store, conflicts_ref_name("session:s-21"));
        AE_CHECK(conflicts_ref.has_value() && conflicts_ref->has_value(), "M3: a conflicts ref exists");
        if (conflicts_ref.has_value() && conflicts_ref->has_value()) {
            auto tree = obj_store.get_tree((*conflicts_ref)->tree_digest);
            AE_CHECK(tree.has_value() && tree->entries.size() == 2,
                     "M3: both ours and theirs are materialized even with no base entry at all");
        }
    }

    // --- M4: an edit/delete fork -- only the PRESENT side is materialized ---------------------------
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto ours_blob = blob_of(obj_store, "ours-still-here");

        SubWorktree branch;
        branch.name = "session:s-22/agents/w3";

        MergeConflict c;
        c.path   = "deleted.txt";
        c.base   = TreeEntry{"deleted.txt", blob_of(obj_store, "original"), false};
        c.ours   = TreeEntry{"deleted.txt", ours_blob, false};  // ours kept it, edited
        c.theirs = std::nullopt;                                 // theirs deleted it

        auto materialized =
            materialize_merge_conflicts(obj_store, ref_store, "session:s-22", "planner", branch, {c});
        AE_CHECK(materialized.has_value(), "M4: materialize_merge_conflicts succeeds for an edit/delete fork");

        auto conflicts_ref = read_ref(ref_store, conflicts_ref_name("session:s-22"));
        AE_CHECK(conflicts_ref.has_value() && conflicts_ref->has_value(), "M4: a conflicts ref exists");
        if (conflicts_ref.has_value() && conflicts_ref->has_value()) {
            auto tree = obj_store.get_tree((*conflicts_ref)->tree_digest);
            AE_CHECK(tree.has_value() && tree->entries.size() == 1,
                     "M4: exactly ONE entry -- the side that actually still exists -- not a fabricated "
                     "entry for the side that was deleted");
            if (tree.has_value() && tree->entries.size() == 1) {
                AE_CHECK(tree->entries[0].name == "deleted.txt.planner",
                         "M4: the surviving entry is ours's (the side that kept the file)");
            }
        }
    }

    // --- M5: a second failed merge against the SAME parent accumulates ------------------------------
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        SubWorktree branch;
        branch.name = "session:s-23/agents/w4";

        MergeConflict first;
        first.path = "first.txt";
        first.ours = TreeEntry{"first.txt", blob_of(obj_store, "first-ours"), false};

        auto r1 = materialize_merge_conflicts(obj_store, ref_store, "session:s-23", "planner", branch,
                                               {first});
        AE_CHECK(r1.has_value(), "M5 setup: the first materialization succeeds");

        MergeConflict second;
        second.path = "second.txt";
        second.ours = TreeEntry{"second.txt", blob_of(obj_store, "second-ours"), false};

        auto r2 = materialize_merge_conflicts(obj_store, ref_store, "session:s-23", "planner", branch,
                                               {second});
        AE_CHECK(r2.has_value(), "M5: the second materialization against the SAME parent succeeds");

        auto conflicts_ref = read_ref(ref_store, conflicts_ref_name("session:s-23"));
        if (conflicts_ref.has_value() && conflicts_ref->has_value()) {
            auto tree = obj_store.get_tree((*conflicts_ref)->tree_digest);
            AE_CHECK(tree.has_value() && tree->entries.size() == 2,
                     "M5: BOTH attempts' evidence coexists in the same conflicts ref -- the first "
                     "attempt's entry was not silently dropped when the second one committed");
        }
    }

    // --- M6: an empty conflicts list is a no-op -- no conflicts ref is created at all ---------------
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        SubWorktree branch;
        branch.name = "session:s-24/agents/w5";

        auto materialized = materialize_merge_conflicts(obj_store, ref_store, "session:s-24", "planner",
                                                          branch, {});
        AE_CHECK(materialized.has_value(), "M6: an empty conflicts list succeeds trivially");

        auto conflicts_ref = read_ref(ref_store, conflicts_ref_name("session:s-24"));
        AE_CHECK(conflicts_ref.has_value() && !conflicts_ref->has_value(),
                 "M6: no conflicts ref was ever created -- there was nothing to materialize");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All worktree conflict-evidence proof checks passed.\n";
    return 0;
}
