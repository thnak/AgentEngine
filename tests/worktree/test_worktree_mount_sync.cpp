// Proof for Milestone 3 Phase F1
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md) --
// `src/backends/native_jail/worktree_mount_sync.hpp`'s `materialize_mount`/`harvest_mount`, the
// bridge core/worktree_mount_fs.hpp's own header named as still missing: syncing a worktree mount's
// content-addressed Tree onto (and back from) the real host directory `MediatedPythonConfig::
// mount_roots`/`MediatedShellRunner` actually point at. Proves 025 §7's "the agent saves a file, the
// user receives an artifact" claim end to end against REAL files on disk, REAL capability checks
// (positive-control-paired denials, 022 §5), and the REAL `MediatedFileSystemAdapter` (ADR-014) --
// not a test double standing in for the host-I/O side.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "agentengine/core/worktree.hpp"
#include "agentengine/pal/env.hpp"
#include "backends/native_jail/mediated_filesystem_adapter.hpp"
#include "backends/native_jail/worktree_mount_sync.hpp"

using namespace agentengine;
using namespace agentengine::native_jail;
using namespace agentengine::native_jail::mediated_shell;
using InMemoryStore = agentengine::rt::InMemoryAppendLogStore;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::fprintf(stderr, "FAIL: %s at %s:%d\n", (label), __FILE__, __LINE__);              \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::printf("  ok: %s\n", (label));                                                    \
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

std::wstring widen_simple(std::string const& s) { return std::wstring(s.begin(), s.end()); }

std::string fresh_dir(std::string const& base, std::string const& name) {
    std::string dir = base + "/" + name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

int main() {
    std::string const scratch = ::agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp") +
                                 "/ae_f1_mount_sync";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);

    // ---- F1-M1: materialize_mount primes a real directory from the worktree's current tree ----
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;

        auto greeting = blob_of(obj_store, "hello from worktree");
        auto nested = blob_of(obj_store, "nested content");
        auto sub_tree = tree_of(obj_store, {{"nested.txt", nested, false}});
        auto root_tree = tree_of(obj_store, {{"greeting.txt", greeting, false}, {"sub", sub_tree, true}});
        AE_CHECK(commit_ref(ref_store, "session:sync-1", root_tree).has_value(),
                 "F1-M1: setup commit succeeds");

        Mount mount{"work", "session:sync-1", ""};
        auto target_dir = fresh_dir(scratch, "materialize_target");
        auto adapter = MediatedFileSystemAdapter::create(widen_simple(target_dir));
        AE_CHECK(adapter.has_value(), "F1-M1: setup -- MediatedFileSystemAdapter::create succeeds");

        cap::FsRead granted{"work", "", std::nullopt};
        auto materialized = materialize_mount(obj_store, ref_store, mount, granted, *adapter);
        AE_CHECK(materialized.has_value(), "F1-M1: materialize_mount succeeds against a fully-granted FsRead");

        AE_CHECK(std::filesystem::exists(target_dir + "/greeting.txt"),
                 "F1-M1: the top-level file is really on disk after materialize");
        AE_CHECK(std::filesystem::exists(target_dir + "/sub/nested.txt"),
                 "F1-M1: the nested directory and file are really on disk after materialize");

        auto greeting_bytes = adapter->read_file("greeting.txt");
        AE_CHECK(greeting_bytes.has_value() && string_of(*greeting_bytes) == "hello from worktree",
                 "F1-M1: the materialized top-level file has the exact tree content");
        auto nested_bytes = adapter->read_file("sub/nested.txt");
        AE_CHECK(nested_bytes.has_value() && string_of(*nested_bytes) == "nested content",
                 "F1-M1: the materialized nested file has the exact tree content");

        // Negative control: a capability for a DIFFERENT mount_id primes nothing.
        auto denied_dir = fresh_dir(scratch, "materialize_denied");
        auto denied_adapter = MediatedFileSystemAdapter::create(widen_simple(denied_dir));
        AE_CHECK(denied_adapter.has_value(), "F1-M1: setup -- second adapter for the denial case");
        cap::FsRead wrong_mount{"other", "", std::nullopt};
        auto denied = materialize_mount(obj_store, ref_store, mount, wrong_mount, *denied_adapter);
        AE_CHECK(!denied.has_value() && denied.error().code == "worktree.mount_capability_mismatch",
                 "F1-M1: materialize_mount fails closed on a capability for a different mount_id");
        AE_CHECK(!std::filesystem::exists(denied_dir + "/greeting.txt"),
                 "F1-M1: the denied materialize never reached the filesystem");

        // Fails closed on an uncommitted ref, matching mount_read/mount_write's own precedent.
        Mount ghost_mount{"ghost", "session:sync-never-committed", ""};
        auto ghost_dir = fresh_dir(scratch, "materialize_ghost");
        auto ghost_adapter = MediatedFileSystemAdapter::create(widen_simple(ghost_dir));
        AE_CHECK(ghost_adapter.has_value(), "F1-M1: setup -- third adapter for the ghost-ref case");
        auto ghost = materialize_mount(obj_store, ref_store, ghost_mount, cap::FsRead{"ghost", "", std::nullopt},
                                        *ghost_adapter);
        AE_CHECK(!ghost.has_value() && ghost.error().code == "worktree.mount_ref_missing",
                 "F1-M1: materialize_mount over an uncommitted ref fails closed");
    }

    // ---- F1-H1: harvest_mount collects real files a run produced, digests them, surfaces Content -
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto empty_tree = tree_of(obj_store, {});
        AE_CHECK(commit_ref(ref_store, "session:sync-2", empty_tree).has_value(),
                 "F1-H1: setup -- an initially-empty /out-style ref commits");

        Mount mount{"out", "session:sync-2", ""};
        auto out_dir = fresh_dir(scratch, "harvest_target");
        auto adapter = MediatedFileSystemAdapter::create(widen_simple(out_dir));
        AE_CHECK(adapter.has_value(), "F1-H1: setup -- MediatedFileSystemAdapter::create succeeds");

        // Simulate what a real run's interpreter would have done: real writes to the real host
        // directory, entirely independent of the worktree Tree (exactly PythonRunner's own
        // Internal_open wrapper's write path, minus spinning up embedded CPython for this proof).
        AE_CHECK(adapter->write_file("result.txt", bytes_of("computed result"), false).has_value(),
                 "F1-H1: setup -- a top-level output file is really written to disk");
        AE_CHECK(adapter->make_directory("plots", false).has_value(),
                 "F1-H1: setup -- a nested output directory is really created");
        AE_CHECK(adapter->write_file("plots/chart.csv", bytes_of("x,y\n1,2\n"), false).has_value(),
                 "F1-H1: setup -- a nested output file is really written to disk");

        cap::FsWrite granted{"out", "", std::nullopt, std::nullopt};
        auto harvested = harvest_mount(obj_store, ref_store, mount, granted, *adapter);
        AE_CHECK(harvested.has_value(), "F1-H1: harvest_mount succeeds against a fully-granted FsWrite");
        AE_CHECK(harvested.has_value() && harvested->size() == 2,
                 "F1-H1: harvest_mount reports exactly the two real files it found (025 SS7)");

        bool found_result = false, found_chart = false;
        if (harvested.has_value()) {
            for (auto const& item : *harvested) {
                AE_CHECK(item.origin == content_origin::tool, "F1-H1: each harvested item is origin=tool");
                AE_CHECK(item.tainted, "F1-H1: each harvested item is tainted (003 SS2 -- sandbox-produced data)");
                auto const& media = std::get<Media>(item.value);
                auto const& blob = std::get<BlobRef>(media.payload);
                AE_CHECK(blob.store == "worktree", "F1-H1: the BlobRef names the worktree store");
                if (blob.digest == *compute_digest(bytes_of("computed result"))) {
                    found_result = true;
                    AE_CHECK(blob.media_type == "text/plain", "F1-H1: result.txt is guessed as text/plain");
                    AE_CHECK(blob.size == std::string("computed result").size(),
                             "F1-H1: result.txt's BlobRef size matches its real content's own size");
                } else if (blob.digest == *compute_digest(bytes_of("x,y\n1,2\n"))) {
                    found_chart = true;
                    AE_CHECK(blob.media_type == "text/csv", "F1-H1: plots/chart.csv is guessed as text/csv");
                }
            }
        }
        AE_CHECK(found_result, "F1-H1: the top-level output file was harvested");
        AE_CHECK(found_chart, "F1-H1: the nested output file was harvested too");

        // The harvest must have actually committed the Tree, not just constructed Content items --
        // mount_read against the SAME mount now sees exactly what was on disk.
        auto read_back = mount_read(obj_store, ref_store, mount, cap::FsRead{"out", "", std::nullopt}, "result.txt");
        AE_CHECK(read_back.has_value() && string_of(*read_back) == "computed result",
                 "F1-H1: the worktree Tree was really updated -- mount_read sees the harvested content");
        auto read_nested =
            mount_read(obj_store, ref_store, mount, cap::FsRead{"out", "", std::nullopt}, "plots/chart.csv");
        AE_CHECK(read_nested.has_value() && string_of(*read_nested) == "x,y\n1,2\n",
                 "F1-H1: the nested harvested file round-trips through mount_read too");

        // Negative control: a capability for a DIFFERENT mount_id fails closed BEFORE anything
        // commits, and the Ref does not move.
        auto ref_before = read_ref(ref_store, "session:sync-2");
        AE_CHECK(ref_before.has_value() && ref_before->has_value(), "F1-H1: setup -- ref is readable before the denial");

        auto denied_dir = fresh_dir(scratch, "harvest_denied");
        auto denied_adapter = MediatedFileSystemAdapter::create(widen_simple(denied_dir));
        AE_CHECK(denied_adapter.has_value(), "F1-H1: setup -- adapter for the denial case");
        AE_CHECK(denied_adapter->write_file("sneaky.txt", bytes_of("should never land"), false).has_value(),
                 "F1-H1: setup -- a real file exists for the denied harvest to find");

        cap::FsWrite wrong_mount{"other", "", std::nullopt, std::nullopt};
        auto denied = harvest_mount(obj_store, ref_store, mount, wrong_mount, *denied_adapter);
        AE_CHECK(!denied.has_value() && denied.error().code == "worktree.mount_capability_mismatch",
                 "F1-H1: harvest_mount fails closed on a capability for a different mount_id");

        auto ref_after = read_ref(ref_store, "session:sync-2");
        AE_CHECK(ref_after.has_value() && ref_after->has_value() &&
                     (*ref_after)->tree_digest == (*ref_before)->tree_digest,
                 "F1-H1: a denied harvest never moved the mount's Ref");
    }

    std::filesystem::remove_all(scratch);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All worktree mount-sync (Phase F1) proof checks passed.\n");
    return 0;
}
