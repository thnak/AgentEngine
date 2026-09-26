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
//   M7 -- conflicts_mount_id()/conflicts_mount() (ADR-055's own /conflicts-mount amendment): the
//         SAME parent always produces the same mount id/Mount; two different parents never collide.
//   M8 -- end-to-end: real conflict evidence, read back through the ORDINARY mount_read() path (the
//         same mechanism a human/supervising-agent host would use) -- proving 025 §4's "surfaced" is
//         a genuinely reachable claim, not just a Ref that happens to exist with nothing to read it.
//         Caught a real reachability bug in the process (fixed in the same pass, see worktree.hpp's
//         own comment on materialize_merge_conflicts): a branch's own name routinely contains '/'
//         (workflow executors), which the original flat-tree design baked verbatim into a single
//         entry name mount_read()'s segment-walk could never reach.
//   M9 -- a nested MergeConflict::path ("a/b/c.txt") becomes real nested Tree structure under
//         /conflicts, mirroring the original file's own location, not a second flat-name bug.

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

std::string string_of(std::vector<std::byte> const& bytes) {
    std::string s;
    s.reserve(bytes.size());
    for (auto b : bytes) s.push_back(static_cast<char>(b));
    return s;
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
                // branch.name is "session:s-20/agents/writer" -- '/' is sanitized to '_' so the
                // result stays a single, mount_read()-reachable leaf, never mistaken for a nested
                // directory separator (the M8 reachability finding below).
                if (e.name == "a.txt.session:s-20_agents_writer") theirs_entry = &e;
            }
            AE_CHECK(ours_entry != nullptr && ours_entry->digest == a_ours,
                     "M2: ours's content lands at <path>.<ours_agent_id>, the real content preserved");
            AE_CHECK(theirs_entry != nullptr && theirs_entry->digest == a_theirs,
                     "M2: theirs's content lands at <path>.<branch's own name, sanitized>, the real "
                     "content preserved");
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

    // --- M7: conflicts_mount_id()/conflicts_mount() are deterministic and derived -------------------
    {
        AE_CHECK(conflicts_mount_id("session:s-30") == conflicts_mount_id("session:s-30"),
                 "M7: the same parent name always produces the same mount id");
        AE_CHECK(conflicts_mount_id("session:s-30") != conflicts_mount_id("session:s-31"),
                 "M7: two different parents never collide on the same mount id");
        Mount const m = conflicts_mount("session:s-30");
        AE_CHECK(m.mount_id == conflicts_mount_id("session:s-30") &&
                     m.ref_name == conflicts_ref_name("session:s-30") && m.subtree_path.empty(),
                 "M7: the Mount binds the derived mount id to the derived conflicts ref, rooted at "
                 "the ref's own root");
    }

    // --- M8: end-to-end -- real conflict evidence, read through the ORDINARY mount_read() path -----
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        SubWorktree branch;
        branch.name = "session:s-32/agents/writer";

        MergeConflict c;
        c.path   = "a.txt";
        c.ours   = TreeEntry{"a.txt", blob_of(obj_store, "ours-content"), false};
        c.theirs = TreeEntry{"a.txt", blob_of(obj_store, "theirs-content"), false};

        auto materialized =
            materialize_merge_conflicts(obj_store, ref_store, "session:s-32", "planner", branch, {c});
        AE_CHECK(materialized.has_value(), "M8 setup: real conflict evidence is materialized");

        // The host builds the mount and grants read access -- exactly the two host-policy-owned
        // steps neither conflicts_mount() nor anything else in this codebase performs automatically
        // (mirroring memory_mount()'s own precedent: a primitive a host calls, never self-attaching).
        Mount const mount = conflicts_mount("session:s-32");
        cap::FsRead const granted{mount.mount_id, "", std::nullopt};

        auto ours_bytes = mount_read(obj_store, ref_store, mount, granted, "a.txt.planner");
        AE_CHECK(ours_bytes.has_value() && string_of(*ours_bytes) == "ours-content",
                 "M8: ours's evidence is readable through the ordinary mount_read() path, byte-exact");

        // branch.name's own '/' is sanitized to '_' by materialize_merge_conflicts() specifically so
        // this stays a single reachable leaf, not a directory `mount_read()` would try to descend
        // into -- an '/'-preserving guest path here would 404, not merely read the wrong thing.
        auto theirs_bytes =
            mount_read(obj_store, ref_store, mount, granted, "a.txt.session:s-32_agents_writer");
        AE_CHECK(theirs_bytes.has_value() && string_of(*theirs_bytes) == "theirs-content",
                 "M8: theirs's evidence is ALSO readable through the same mount -- both versions "
                 "genuinely retained AND genuinely reachable, not just present in the store");

        // A capability minted for a DIFFERENT mount_id is correctly refused -- the mount is not
        // accidentally wide open to any FsRead grant that happens to pass by.
        cap::FsRead const wrong_grant{"/some/other/mount", "", std::nullopt};
        auto denied = mount_read(obj_store, ref_store, mount, wrong_grant, "a.txt.planner");
        AE_CHECK(!denied.has_value() && denied.error().code == "worktree.mount_capability_mismatch",
                 "M8: a capability minted for a different mount_id is rejected, not silently honored");
    }

    // --- M9: a NESTED conflict path (MergeConflict::path's own documented "a/b/c.txt" shape) --------
    // --- becomes REAL nested Tree structure, mirroring the original file's own location, not a     --
    // --- flat entry with an embedded '/'.                                                          ---
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        SubWorktree branch;
        branch.name = "writer";

        MergeConflict c;
        c.path = "dir/sub/file.txt";
        c.ours = TreeEntry{"file.txt", blob_of(obj_store, "nested-ours"), false};

        auto materialized =
            materialize_merge_conflicts(obj_store, ref_store, "session:s-33", "planner", branch, {c});
        AE_CHECK(materialized.has_value(), "M9 setup: a nested-path conflict is materialized");

        Mount const mount = conflicts_mount("session:s-33");
        cap::FsRead const granted{mount.mount_id, "", std::nullopt};
        auto bytes = mount_read(obj_store, ref_store, mount, granted, "dir/sub/file.txt.planner");
        AE_CHECK(bytes.has_value() && string_of(*bytes) == "nested-ours",
                 "M9: a nested conflict path is reachable via the SAME directory structure the "
                 "original file had -- real nested Tree entries, not a flat name with '/' baked in");

        auto conflicts_ref = read_ref(ref_store, conflicts_ref_name("session:s-33"));
        if (conflicts_ref.has_value() && conflicts_ref->has_value()) {
            auto root = obj_store.get_tree((*conflicts_ref)->tree_digest);
            AE_CHECK(root.has_value() && root->entries.size() == 1 && root->entries[0].name == "dir" &&
                          root->entries[0].is_tree,
                     "M9: the conflicts ref's own ROOT has exactly one real subdirectory entry "
                     "(\"dir\"), not a flat entry literally named \"dir/sub/file.txt.planner\"");
        }
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All worktree conflict-evidence proof checks passed.\n";
    return 0;
}
