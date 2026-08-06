// Proof for 025-Worktree-and-Virtual-Filesystem.md §5's mount resolution, Milestone 3 Phase C1
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md) -- `split_mount_path`/
// `mount_read`/`mount_write` (core/worktree.hpp) proven per 007 §3: a capability grant is checked
// BEFORE any store access, never after, and a capability scoped to one mount or one path prefix can
// never reach outside it -- each denial proven with a positive control showing the identical
// request succeeding once properly authorized (022 §5).
//
// This is explicitly NOT 025 §5's OS-level path-escape corpus (Phase C2, ADR-track) -- there is no
// real filesystem, no symlinks, no `..` to walk "up" through; see core/worktree.hpp's own section
// comment for why that's a structural fact here, not an incomplete defense.

#include <iostream>
#include <string>

#include "agentengine/core/worktree.hpp"

using namespace agentengine;
using quark::InMemoryStore;

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

std::vector<std::byte> bytes_of(std::string const& content) {
    std::vector<std::byte> bytes;
    bytes.reserve(content.size());
    for (char c : content) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return bytes;
}

std::string string_of(std::vector<std::byte> const& bytes) {
    std::string s;
    s.reserve(bytes.size());
    for (auto b : bytes) s.push_back(static_cast<char>(b));
    return s;
}

Digest blob_of(InMemoryWorktreeObjectStore& store, std::string const& content) {
    return *store.put_blob(bytes_of(content));
}

Digest tree_of(InMemoryWorktreeObjectStore& store, std::vector<TreeEntry> entries) {
    return *store.put_tree(Tree{std::move(entries)});
}

} // namespace

int main() {
    // C1-C1: split_mount_path's own contract, independent of any store or capability.
    {
        auto empty = split_mount_path("");
        AE_CHECK(empty.has_value() && empty->empty(), "C1-C1: an empty path splits to zero segments (the root)");

        auto simple = split_mount_path("a/b/c.txt");
        AE_CHECK(simple.has_value() && simple->size() == 3 && (*simple)[0] == "a" && (*simple)[1] == "b" &&
                     (*simple)[2] == "c.txt",
                 "C1-C1: an ordinary relative path splits into its segments in order");

        auto absolute = split_mount_path("/a/b");
        AE_CHECK(!absolute.has_value() && absolute.error().code == "worktree.mount_path_absolute",
                 "C1-C1: a leading '/' is rejected");

        auto trailing = split_mount_path("a/b/");
        AE_CHECK(!trailing.has_value() && trailing.error().code == "worktree.mount_path_malformed",
                 "C1-C1: a trailing '/' is rejected");

        auto doubled = split_mount_path("a//b");
        AE_CHECK(!doubled.has_value() && doubled.error().code == "worktree.mount_path_malformed",
                 "C1-C1: a double slash (empty segment) is rejected");

        auto dot = split_mount_path("a/./b");
        auto dotdot = split_mount_path("a/../b");
        AE_CHECK(!dot.has_value() && dot.error().code == "worktree.mount_path_malformed" && !dotdot.has_value() &&
                     dotdot.error().code == "worktree.mount_path_malformed",
                 "C1-C1: '.' and '..' are both rejected as malformed, not resolved");
    }

    // C1-C2 (mount_read happy path + capability checks, positive controls throughout).
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;

        auto small = blob_of(obj_store, "small file content");
        auto big = blob_of(obj_store, std::string(100, 'x'));
        auto root_tree = tree_of(obj_store, {{"small.txt", small, false}, {"big.txt", big, false}});
        auto ref = commit_ref(ref_store, "session:mount-1", root_tree);
        AE_CHECK(ref.has_value(), "C1-C2: setup commit succeeds");

        Mount mount{"/work", "session:mount-1", ""};

        // Happy path.
        cap::FsRead unrestricted{"/work", "", std::nullopt};
        auto read = mount_read(obj_store, ref_store, mount, unrestricted, "small.txt");
        AE_CHECK(read.has_value() && string_of(*read) == "small file content",
                 "C1-C2: an unrestricted FsRead reads the exact file content");

        // Capability/mount mismatch.
        cap::FsRead wrong_mount{"/other", "", std::nullopt};
        auto mismatched = mount_read(obj_store, ref_store, mount, wrong_mount, "small.txt");
        AE_CHECK(!mismatched.has_value() && mismatched.error().code == "worktree.mount_capability_mismatch",
                 "C1-C2: a capability for a DIFFERENT mount_id is rejected");

        // Path-prefix scoping: capability scoped to "allowed/" cannot read "small.txt" at the root,
        // but CAN read "allowed/x.txt" -- both checked so the denial isn't just "always denies".
        auto scoped_content = blob_of(obj_store, "scoped content");
        auto scoped_dir = tree_of(obj_store, {{"x.txt", scoped_content, false}});
        auto root_with_scope = tree_of(
            obj_store, {{"small.txt", small, false}, {"big.txt", big, false}, {"allowed", scoped_dir, true}});
        AE_CHECK(commit_ref(ref_store, "session:mount-1", root_with_scope).has_value(),
                 "C1-C2: setup: add a scoped subdirectory, keeping big.txt for the size-cap checks below");

        cap::FsRead scoped{"/work", "allowed", std::nullopt};
        auto out_of_scope = mount_read(obj_store, ref_store, mount, scoped, "small.txt");
        AE_CHECK(!out_of_scope.has_value() && out_of_scope.error().code == "worktree.mount_path_outside_capability",
                 "C1-C2: a capability scoped to 'allowed/' cannot read a file outside that prefix");
        auto in_scope = mount_read(obj_store, ref_store, mount, scoped, "allowed/x.txt");
        AE_CHECK(in_scope.has_value() && string_of(*in_scope) == "scoped content",
                 "C1-C2 (positive control): the SAME capability reads a file inside its own scope fine");

        // size_cap_bytes: a cap too small for the file is rejected, the same cap on a smaller file
        // succeeds.
        cap::FsRead tiny_cap{"/work", "", std::uint64_t{10}};
        auto too_big = mount_read(obj_store, ref_store, mount, tiny_cap, "big.txt");
        AE_CHECK(!too_big.has_value() && too_big.error().code == "worktree.mount_read_exceeds_size_cap",
                 "C1-C2: a file exceeding size_cap_bytes is rejected");
        cap::FsRead roomy_cap{"/work", "", std::uint64_t{1000}};
        auto under_roomy_cap = mount_read(obj_store, ref_store, mount, roomy_cap, "big.txt");
        AE_CHECK(under_roomy_cap.has_value(),
                 "C1-C2 (positive control): the same file reads fine under a large-enough size cap");

        // Reading a directory as a file, and reading a nonexistent path.
        auto as_dir = mount_read(obj_store, ref_store, mount, unrestricted, "allowed");
        AE_CHECK(!as_dir.has_value() && as_dir.error().code == "worktree.mount_read_is_directory",
                 "C1-C2: reading a directory path as a file is rejected");
        auto missing = mount_read(obj_store, ref_store, mount, unrestricted, "nope.txt");
        AE_CHECK(!missing.has_value() && missing.error().code == "worktree.mount_path_not_found",
                 "C1-C2: reading a nonexistent path is rejected");
    }

    // C1-C3 (mount_write: creates a new file, creates missing intermediate directories, overwrites
    // an existing file, and round-trips through mount_read).
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;

        auto base_tree = tree_of(obj_store, {});
        AE_CHECK(commit_ref(ref_store, "session:mount-2", base_tree).has_value(),
                 "C1-C3: setup empty-root commit succeeds");

        Mount mount{"/work", "session:mount-2", ""};
        cap::FsWrite unrestricted{"/work", "", std::nullopt, std::nullopt};

        auto write1 = mount_write(obj_store, ref_store, mount, unrestricted, "notes.txt", bytes_of("hello"));
        AE_CHECK(write1.has_value(), "C1-C3: writing a new top-level file succeeds");
        auto read1 = mount_read(obj_store, ref_store, mount, cap::FsRead{"/work", "", std::nullopt}, "notes.txt");
        AE_CHECK(read1.has_value() && string_of(*read1) == "hello",
                 "C1-C3: the written file reads back with the exact content");

        auto write2 =
            mount_write(obj_store, ref_store, mount, unrestricted, "a/b/deep.txt", bytes_of("deep content"));
        AE_CHECK(write2.has_value(), "C1-C3: writing through missing intermediate directories succeeds");
        auto read2 = mount_read(obj_store, ref_store, mount, cap::FsRead{"/work", "", std::nullopt}, "a/b/deep.txt");
        AE_CHECK(read2.has_value() && string_of(*read2) == "deep content",
                 "C1-C3: the deeply-nested file reads back correctly, intermediate dirs were created");

        auto overwrite =
            mount_write(obj_store, ref_store, mount, unrestricted, "notes.txt", bytes_of("updated"));
        AE_CHECK(overwrite.has_value(), "C1-C3: overwriting an existing file succeeds");
        auto read3 = mount_read(obj_store, ref_store, mount, cap::FsRead{"/work", "", std::nullopt}, "notes.txt");
        AE_CHECK(read3.has_value() && string_of(*read3) == "updated",
                 "C1-C3: the overwritten file reflects the NEW content, not the old");
        auto still_there =
            mount_read(obj_store, ref_store, mount, cap::FsRead{"/work", "", std::nullopt}, "a/b/deep.txt");
        AE_CHECK(still_there.has_value() && string_of(*still_there) == "deep content",
                 "C1-C3: overwriting notes.txt did not disturb the unrelated a/b/deep.txt");
    }

    // C1-C4 (mount_write capability checks: mismatch and out-of-scope rejected, in-scope succeeds;
    // writing through an existing FILE as if it were a directory is rejected).
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto existing_file = blob_of(obj_store, "i am a file");
        auto base_tree = tree_of(obj_store, {{"notafile", existing_file, false}});
        AE_CHECK(commit_ref(ref_store, "session:mount-3", base_tree).has_value(),
                 "C1-C4: setup commit succeeds");

        Mount mount{"/work", "session:mount-3", ""};

        cap::FsWrite wrong_mount{"/other", "", std::nullopt, std::nullopt};
        auto mismatched = mount_write(obj_store, ref_store, mount, wrong_mount, "x.txt", bytes_of("x"));
        AE_CHECK(!mismatched.has_value() && mismatched.error().code == "worktree.mount_capability_mismatch",
                 "C1-C4: a capability for a DIFFERENT mount_id cannot write here");

        cap::FsWrite scoped{"/work", "allowed", std::nullopt, std::nullopt};
        auto out_of_scope = mount_write(obj_store, ref_store, mount, scoped, "elsewhere.txt", bytes_of("x"));
        AE_CHECK(!out_of_scope.has_value() && out_of_scope.error().code == "worktree.mount_path_outside_capability",
                 "C1-C4: a capability scoped to 'allowed/' cannot write outside that prefix");
        auto in_scope = mount_write(obj_store, ref_store, mount, scoped, "allowed/y.txt", bytes_of("y"));
        AE_CHECK(in_scope.has_value(),
                 "C1-C4 (positive control): the SAME capability writes fine inside its own scope");

        cap::FsWrite unrestricted{"/work", "", std::nullopt, std::nullopt};
        auto through_file =
            mount_write(obj_store, ref_store, mount, unrestricted, "notafile/child.txt", bytes_of("x"));
        AE_CHECK(!through_file.has_value() && through_file.error().code == "worktree.mount_write_type_conflict",
                 "C1-C4: writing through an existing FILE as if it were a directory is rejected");

        auto root_write = mount_write(obj_store, ref_store, mount, unrestricted, "", bytes_of("x"));
        AE_CHECK(!root_write.has_value() && root_write.error().code == "worktree.mount_path_is_root",
                 "C1-C4: writing to the mount root itself (empty path) is rejected");
    }

    // C1-C5 (subtree_path mounts + sibling preservation): a mount rooted at "work" within a larger
    // ref that ALSO has "input"/"skills" siblings -- writing through the mount must reach the right
    // place AND must not disturb the unrelated siblings at the ref's actual root.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;

        auto input_file = blob_of(obj_store, "input data");
        auto skill_file = blob_of(obj_store, "skill code");
        auto input_tree = tree_of(obj_store, {{"data.csv", input_file, false}});
        auto skills_tree = tree_of(obj_store, {{"tool.py", skill_file, false}});
        auto work_tree = tree_of(obj_store, {});
        auto session_root = tree_of(
            obj_store, {{"input", input_tree, true}, {"skills", skills_tree, true}, {"work", work_tree, true}});
        AE_CHECK(commit_ref(ref_store, "session:mount-4", session_root).has_value(),
                 "C1-C5: setup: a session root with input/skills/work siblings");

        Mount work_mount{"/work", "session:mount-4", "work"};
        cap::FsWrite unrestricted_w{"/work", "", std::nullopt, std::nullopt};
        auto write_result =
            mount_write(obj_store, ref_store, work_mount, unrestricted_w, "output.txt", bytes_of("result"));
        AE_CHECK(write_result.has_value(), "C1-C5: writing through the 'work'-rooted mount succeeds");

        // The write must be visible through the /work mount...
        cap::FsRead unrestricted_r{"/work", "", std::nullopt};
        auto read_back = mount_read(obj_store, ref_store, work_mount, unrestricted_r, "output.txt");
        AE_CHECK(read_back.has_value() && string_of(*read_back) == "result",
                 "C1-C5: the write is visible through the SAME mount afterward");

        // ...and input/skills, sibling to "work" at the actual ref root, must be untouched.
        Mount input_mount{"/input", "session:mount-4", "input"};
        auto input_read = mount_read(obj_store, ref_store, input_mount,
                                      cap::FsRead{"/input", "", std::nullopt}, "data.csv");
        AE_CHECK(input_read.has_value() && string_of(*input_read) == "input data",
                 "C1-C5: writing through /work did not disturb the SIBLING /input mount's content");

        Mount skills_mount{"/skills", "session:mount-4", "skills"};
        auto skills_read = mount_read(obj_store, ref_store, skills_mount,
                                       cap::FsRead{"/skills", "", std::nullopt}, "tool.py");
        AE_CHECK(skills_read.has_value() && string_of(*skills_read) == "skill code",
                 "C1-C5: writing through /work did not disturb the SIBLING /skills mount's content either");
    }

    // C1-C6 (fails closed on a mount whose ref was never committed): a plausible-looking Mount over
    // a ref name nobody ever committed to must be rejected, not treated as an empty tree.
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        Mount ghost_mount{"/ghost", "session:never-committed", ""};
        auto read = mount_read(obj_store, ref_store, ghost_mount, cap::FsRead{"/ghost", "", std::nullopt}, "x.txt");
        AE_CHECK(!read.has_value() && read.error().code == "worktree.mount_ref_missing",
                 "C1-C6: reading through a mount over an uncommitted ref fails closed");
        auto write =
            mount_write(obj_store, ref_store, ghost_mount, cap::FsWrite{"/ghost", "", std::nullopt, std::nullopt},
                        "x.txt", bytes_of("x"));
        AE_CHECK(!write.has_value() && write.error().code == "worktree.mount_ref_missing",
                 "C1-C6: writing through a mount over an uncommitted ref fails closed too");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All worktree mount proof checks passed.\n";
    return 0;
}
