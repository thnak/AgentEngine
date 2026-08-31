// Proves the content-durability half of a long-disclosed gap `ADR-102`/`ADR-126`/`ADR-128` each named
// and declined to close: "a real, separate, materially larger piece of work (a durable object-store
// conformer for blob/tree content, not merely branch/ACL bookkeeping)." `Ledger<Store>::durable_dir`
// already makes branch/ACL bookkeeping durable -- it never touches `store_`, and the production
// default, `InMemoryWorktreeObjectStore`, provides none. `agentengine::FileWorktreeObjectStore`
// (`core/file_worktree_object_store.hpp`, ported this same round from the prove-phase original) is a
// real `WorktreeObjectStore` conformer a caller can hand `Ledger<Store>` instead.
//
// This is a GENUINE TWO-PROCESS proof, mirroring `identity-native-sandbox-worktree-design.md` §34.3's
// own methodology (real separate OS processes, not an in-process simulation) -- the strongest bar this
// design line's own gate criteria named for this specific claim (a real, durable, on-disk content store
// feeding an ACL-gated read path is exactly the "security/hot-path-adjacent" class `CLAUDE.md` names as
// needing genuine adversarial pressure, not a same-process shortcut). This ONE executable plays BOTH
// roles: invoked with no arguments, it is "process 2" (the reader) -- it first launches ITSELF as a
// genuinely separate OS process (via `std::system()`) with `--writer-role` and three explicit directory
// arguments, waits for that child process to exit normally (a clean process exit *before ever merging*
// -- the exact "crash or a clean exit, indistinguishable and both fine" framing `Ledger::
// orphaned_branches()` already establishes, reused here rather than re-litigated), and only then
// proceeds with its own, separate process's own work. Invoked WITH `--writer-role <objects_dir>
// <ledger_dir> <identity_dir>`, it is "process 1" (the writer).
//
// A REAL, MATERIAL SCOPE BOUNDARY, found while building this (not assumed going in): `Ledger<Store>` is
// genuinely store-agnostic via its own template parameter, exactly as ADR-102/126/128 all claimed --
// but `SandboxRuntime`/`MandatorySandboxProvider` (the production TOOL-SURFACE layer this whole
// session's task-branch/crash-recovery line lives in) are hardcoded to `Ledger<>` (the DEFAULT
// `InMemoryWorktreeObjectStore`) throughout, not templated on `Store` at all -- `bind_sandbox()`,
// `bind_root_branch()` (`ADR-128`), and every task-branch verb all take `agentengine::Ledger<>&`
// literally. This proof therefore exercises the raw `Ledger<FileWorktreeObjectStore>` API directly
// (`create_root_branch()`/`branch_from()`/`commit()`/`orphaned_branches()`/`reclaim_orphaned_branch()`/
// `merge()`), NOT `MandatorySandboxProvider::bind_root_branch()` -- that integration is a real, separate,
// larger follow-on (templatizing the two most heavily-verified files in this whole session's own design
// line on `Store` too), explicitly out of this proof's own scope, not silently assumed solved.
//
//   [1] process 1 creates a root branch, spawns a real child, commits REAL, distinguishable content to
//       BOTH the root and the child (two different blobs, so a later read can tell which survived),
//       and exits cleanly WITHOUT merging.
//   [2] process 2 -- a genuinely separate OS process, a fresh Ledger<FileWorktreeObjectStore> against
//       the SAME durable_dir/objects root -- finds BOTH branches as real orphans via orphaned_branches().
//   [3] process 2 reclaims both via reclaim_orphaned_branch() (the SAME primitive `ADR-126`/`ADR-128`'s
//       own production automation is built on, used here at the raw Ledger level since the tool-surface
//       layer is not Store-generic yet).
//   [4] THE CORE CLAIM: process 2's merge(child, root, ...) SUCCEEDS -- not `ledger.merge_tree_load_
//       failed`, the exact, precise failure `tests/test_task_branch_durability_recovery.cpp` (`ADR-126`)
//       asserts as the correct, disclosed behavior for the in-memory-store case. Real content, written
//       by a DIFFERENT OS process, genuinely round-trips through real files on real disk.
//   [5] the merged tree's own content is read back via get_blob_safe() (the ACL-gated production read
//       path, not a raw filesystem check) and matches BOTH the root's own original blob and the child's
//       real, disk-recovered blob byte-for-byte -- proving this is REAL content durability, not merely
//       "the merge call returned success with no verification of what it actually merged."
//   [6] IdentityAuthority's own identity-durability precondition (`identity-native-sandbox-worktree-
//       design.md` §33/§34.2, gate item 6 of `docs/planning/content-durability-conformer-design-draft.md`):
//       process 2's IdentityAuthority::bootstrap(SAME identity_dir).adopt(SAME Principal) returns the
//       IDENTICAL internal id process 1's adopt() call minted -- without this, process 2 could never
//       even ATTEMPT the reclaim/merge above under the "same owner" identity at all, since IdentityHandle
//       has no public constructor a test (or any other caller) could otherwise use to reconstruct one.

#include "agentengine/core/file_worktree_object_store.hpp"
#include "agentengine/core/ledger.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

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

// The one, fixed real Principal both processes adopt() -- a real, external, cross-process-stable
// identity string, exactly the shape adopt()'s own doc comment describes as its real intended use.
Principal const kOwnerPrincipal{"content-durability-cross-process-owner", "content-durability-tenant"};

// ---- Process 1 (the writer). Returns a real process exit code -- 0 on success, 1 on any real
// ---- check failure, so process 2 can tell a genuine failure apart from a clean run via std::system()'s
// ---- own return value.
int run_writer_role(std::filesystem::path const& objects_dir, std::filesystem::path const& ledger_dir,
                     std::filesystem::path const& identity_dir) {
    IdentityAuthority& authority = IdentityAuthority::bootstrap(identity_dir);
    IdentityHandle owner = authority.adopt(kOwnerPrincipal);

    auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 1'000'000);
    check(branch_quota_r.has_value() && storage_quota_r.has_value(),
          "[1] writer: BranchCost/StorageBytes quota mint_root() calls succeed");
    if (!branch_quota_r.has_value() || !storage_quota_r.has_value()) return 1;

    Ledger<FileWorktreeObjectStore> ledger(FileWorktreeObjectStore(objects_dir), ledger_dir);

    auto root_r = drive(ledger.create_root_branch(owner, "content-durability"));
    check(root_r.has_value(), "[1] writer: create_root_branch() succeeds against a durable, "
                               "file-backed Ledger<FileWorktreeObjectStore>");
    if (!root_r.has_value()) return 1;

    auto root_blob_r = ledger.put_blob_safe(to_bytes("real content committed by process 1, on the root"),
                                              owner);
    check(root_blob_r.has_value(), "[1] writer: put_blob_safe() on the root succeeds through the real, "
                                    "file-backed object store");
    if (!root_blob_r.has_value()) return 1;

    Tree root_tree;
    root_tree.entries.push_back(TreeEntry{"root-note.txt", *root_blob_r, false});
    auto root_commit_r = drive(ledger.commit(*root_r, root_tree, owner, *storage_quota_r));
    check(root_commit_r.has_value(), "[1] writer: commit() on the root succeeds and writes real tree "
                                      "bytes to real disk");
    if (!root_commit_r.has_value()) return 1;

    auto child_r = drive(ledger.branch_from(*root_r, owner, *branch_quota_r));
    check(child_r.has_value(), "[1] writer: branch_from() produces a real child branch");
    if (!child_r.has_value()) return 1;

    auto child_blob_r =
        ledger.put_blob_safe(to_bytes("real content committed by process 1, on the CHILD"), owner);
    check(child_blob_r.has_value(), "[1] writer: put_blob_safe() on the child succeeds");
    if (!child_blob_r.has_value()) return 1;

    Tree child_tree;
    child_tree.entries.push_back(TreeEntry{"root-note.txt", *root_blob_r, false});
    child_tree.entries.push_back(TreeEntry{"child-note.txt", *child_blob_r, false});
    auto child_commit_r = drive(ledger.commit(*child_r, child_tree, owner, *storage_quota_r));
    check(child_commit_r.has_value(), "[1] writer: commit() on the child succeeds and writes real, "
                                       "distinguishable tree bytes to real disk");
    if (!child_commit_r.has_value()) return 1;

    if (g_failures != 0) return 1;
    std::fprintf(stderr, "writer role: real root+child content committed, exiting cleanly WITHOUT "
                          "merging (root=%s, child=%s)\n",
                 root_r->name().c_str(), child_r->name().c_str());
    // root_r/child_r/ledger all go out of scope HERE, in THIS process, without ever merging -- the
    // exact "crash or a clean exit, indistinguishable and both fine" case this whole design line's own
    // orphaned_branches()/reclaim_orphaned_branch() machinery is built to handle.
    return 0;
}

// ---- Process 2 (the reader/orchestrator). Launches process 1 as a genuinely separate OS process,
// ---- waits for it, then does its own, separate work.
int run_reader_role(std::filesystem::path const& self_exe) {
    namespace fs = std::filesystem;
    fs::path const objects_dir = fs::temp_directory_path() / "ae_test_content_durability_objects";
    fs::path const ledger_dir = fs::temp_directory_path() / "ae_test_content_durability_ledger";
    fs::path const identity_dir = fs::temp_directory_path() / "ae_test_content_durability_identity";
    std::error_code ec;
    fs::remove_all(objects_dir, ec);
    fs::remove_all(ledger_dir, ec);
    fs::remove_all(identity_dir, ec);

    // Launch process 1 as a REAL, separate OS process -- not a function call, not a thread. Quoted
    // defensively (this repo's own real paths have no spaces, but a caller-relocated checkout might).
    // On Windows, std::system() runs the command via `cmd /c`, which has a real, well-known quirk: when
    // the command string itself starts with a quoted token AND contains further quoted tokens, cmd.exe
    // misparses which quoted substring is the executable name unless the WHOLE command is wrapped in
    // one more, redundant pair of quotes -- found empirically (the unwrapped form failed with "The
    // filename, directory name, or volume label syntax is incorrect", not a real logic bug in this
    // test), not assumed from documentation alone.
    //
    // REAL FINDING an independent red-team pass caught (2026-08-30, same day as the port): the original
    // comment here claimed the extra outer wrap is "harmless" on POSIX `/bin/sh -c` too -- that claim was
    // FALSE, not merely untested. std::system() on POSIX runs the command via `/bin/sh -c <command>`,
    // which applies standard shell quote-removal: wrapping an ALREADY-quoted command in one more pair of
    // quotes shifts quote-state parity by one, which (a) leaves the leading token (the executable path)
    // UNQUOTED -- reintroducing exactly the space-in-path fragility this quoting exists to prevent -- and
    // (b) makes the space between the executable path and its first argument spuriously QUOTED, merging
    // them into ONE argv element instead of two. Empirically confirmed with `sh -c` directly: the
    // double-wrapped form collapses the executable path AND all four arguments into a single, nonexistent
    // command name ("No such file or directory"); the SAME command string without the extra wrap parses
    // into the correct four separate arguments. The workaround is therefore genuinely Windows-`cmd.exe`-
    // specific, not a universally-harmless no-op -- applied conditionally here rather than unconditionally.
    std::string const inner = "\"" + self_exe.string() + "\" --writer-role \"" + objects_dir.string() +
                               "\" \"" + ledger_dir.string() + "\" \"" + identity_dir.string() + "\"";
#ifdef _WIN32
    std::string const command = "\"" + inner + "\"";
#else
    std::string const command = inner;
#endif
    int const writer_exit = std::system(command.c_str());
    check(writer_exit == 0, "[1] reader: process 1 (the writer) exits with a real, clean, successful "
                             "exit code, as a genuinely separate OS process");
    if (writer_exit != 0) return EXIT_FAILURE;

    // A FRESH IdentityAuthority::bootstrap() call, in THIS process, against the SAME identity_dir.
    IdentityAuthority& authority = IdentityAuthority::bootstrap(identity_dir);
    IdentityHandle owner = authority.adopt(kOwnerPrincipal);

    // [6] the identity-durability precondition itself, checked directly rather than assumed: re-
    // adopt()-ing the SAME real Principal in a genuinely different process must return the IDENTICAL
    // internal id -- without this, nothing below could even be attempted under "the same owner".
    // There is no direct way to read process 1's own minted id back out (it already exited), so this
    // is proven INDIRECTLY but decisively by [3]/[4] below: reclaim_orphaned_branch() and merge() are
    // both real, ACL-gated operations that fail closed for a wrong/unrelated identity (test_ledger.cpp's
    // own [2] already proves this precise failure mode) -- their SUCCESS below is only possible if
    // `owner` here is genuinely the SAME internal identity process 1 used, not a coincidentally-similar
    // but different one.

    auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 1'000'000);
    auto merge_quota_r = agentengine::rt::AsyncQuota<MergeCost>::mint_root(authority, owner, 100);
    check(branch_quota_r.has_value() && storage_quota_r.has_value() && merge_quota_r.has_value(),
          "[2] reader: BranchCost/StorageBytes/MergeCost quota mint_root() calls succeed (a FRESH set, "
          "by design -- quotas are in-process state, not durable, unchanged and unrelated to this claim)");
    if (!branch_quota_r.has_value() || !storage_quota_r.has_value() || !merge_quota_r.has_value()) {
        return EXIT_FAILURE;
    }

    // A FRESH Ledger<FileWorktreeObjectStore>, in THIS process, against the SAME two directories.
    Ledger<FileWorktreeObjectStore> ledger(FileWorktreeObjectStore(objects_dir), ledger_dir);

    auto orphans = ledger.orphaned_branches();
    check(orphans.size() == 2, "[2] reader: exactly two real orphans (the root and the child) are "
                                "restored by load_durable_state() at THIS, genuinely separate process's "
                                "own Ledger construction");

    std::string root_name, child_name;
    for (std::string const& name : orphans) {
        if (name.find("/child-") != std::string::npos) {
            child_name = name;
        } else {
            root_name = name;
        }
    }
    check(!root_name.empty() && !child_name.empty(),
          "[2] reader: the two orphans are genuinely distinguishable as one root and one child by name");
    if (root_name.empty() || child_name.empty()) return EXIT_FAILURE;

    auto root_reclaim_r = ledger.reclaim_orphaned_branch(root_name, owner);
    auto child_reclaim_r = ledger.reclaim_orphaned_branch(child_name, owner);
    check(root_reclaim_r.has_value() && child_reclaim_r.has_value(),
          "[3] reader: reclaim_orphaned_branch() succeeds for BOTH branches under the SAME owner "
          "identity a genuinely different OS process adopted -- the identity-durability precondition "
          "(check [6]) holds, proven by this real ACL-gated success rather than assumed");
    if (!root_reclaim_r.has_value() || !child_reclaim_r.has_value()) return EXIT_FAILURE;

    // [4] THE CORE CLAIM.
    auto merge_r = drive(ledger.merge(std::move(*child_reclaim_r), *root_reclaim_r, owner, *merge_quota_r));
    check(merge_r.has_value(),
          "[4] THE CORE CLAIM: merge() on a branch pair recovered ENTIRELY from real disk, in a "
          "genuinely different OS process than the one that committed the real content, SUCCEEDS -- "
          "not ledger.merge_tree_load_failed, the exact failure test_task_branch_durability_recovery.cpp "
          "(ADR-126) correctly asserts for the in-memory-store case this proof does NOT use");
    if (!merge_r.has_value()) {
        std::fprintf(stderr, "merge() failed with code=%s message=%s\n", merge_r.error().code.c_str(),
                     merge_r.error().message.c_str());
        return EXIT_FAILURE;
    }

    // [5] read the REAL, merged, disk-recovered content back through the ACL-gated production read
    // path and confirm it is genuinely both blobs, byte-for-byte -- not merely "merge() returned
    // success".
    auto merged_tree_r = ledger.get_tree_safe(merge_r->tree, owner);
    check(merged_tree_r.has_value(), "[5] reader: get_tree_safe() on the merged checkpoint's own tree "
                                      "succeeds");
    if (!merged_tree_r.has_value()) return EXIT_FAILURE;
    check(merged_tree_r->entries.size() == 2,
          "[5] the merged tree genuinely contains BOTH entries (the root's own, unchanged by the merge, "
          "and the child's own, folded in) -- not a partial or empty result");

    bool root_note_ok = false, child_note_ok = false;
    for (TreeEntry const& entry : merged_tree_r->entries) {
        auto blob_r = ledger.get_blob_safe(entry.digest, owner);
        if (!blob_r.has_value()) continue;
        std::string content(reinterpret_cast<char const*>(blob_r->data()), blob_r->size());
        if (entry.name == "root-note.txt" && content == "real content committed by process 1, on the root") {
            root_note_ok = true;
        }
        if (entry.name == "child-note.txt" &&
            content == "real content committed by process 1, on the CHILD") {
            child_note_ok = true;
        }
    }
    check(root_note_ok, "[5] the root's own real blob content, read back via the ACL-gated "
                         "get_blob_safe() production path, matches exactly what process 1 committed");
    check(child_note_ok, "[5] the CHILD's own real, disk-recovered blob content -- written by a "
                          "genuinely different OS process, never touched by process 2 itself -- matches "
                          "exactly what process 1 committed, byte-for-byte");

    fs::remove_all(objects_dir, ec);
    fs::remove_all(ledger_dir, ec);
    fs::remove_all(identity_dir, ec);

    if (g_failures == 0) {
        std::printf("ALL CHECKS PASSED -- real blob/tree CONTENT, not merely branch/ACL bookkeeping, "
                     "genuinely survives across two REAL, SEPARATE OS PROCESSES via "
                     "agentengine::FileWorktreeObjectStore: a durable Ledger<FileWorktreeObjectStore>, "
                     "reconstructed from scratch in a different process after the first exited cleanly "
                     "without merging, can reclaim both branches under the SAME durably-adopted "
                     "identity and successfully merge() real, disk-recovered content -- closing the "
                     "content-durability half of the gap ADR-102/ADR-126/ADR-128 each named and "
                     "declined to close.\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 5 && std::string_view(argv[1]) == "--writer-role") {
        return run_writer_role(argv[2], argv[3], argv[4]);
    }
    if (argc < 1 || argv[0] == nullptr || std::string_view(argv[0]).empty()) {
        std::fprintf(stderr, "FAIL: argv[0] is unavailable -- cannot self-relaunch as a genuinely "
                              "separate OS process\n");
        return EXIT_FAILURE;
    }
    return run_reader_role(std::filesystem::absolute(argv[0]));
}
