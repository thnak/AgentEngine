// Proof that `Ledger::BlobWriteBatch` -- what `RealIoFileSystem::drain_into_tree()` and
// `scan_and_drain_into_tree()` now write their blobs through -- writes the durable snapshot once per
// drain instead of once per file, and leaves everything else exactly as a loop of `put_blob_safe()`
// calls left it.
//
// WHAT WAS WRONG. Every `put_blob_safe()` ended in `persist_snapshot_locked()`, which rewrites every
// branch, every checkpoint and the whole blob/tree ACL to a temp file and renames it. Both drains
// called it once per file, and `SandboxRuntime::run()` drains once per command, so an n-file sandbox
// wrote Theta(n^2) bytes to disk per command.
//
// What the batch must NOT change is where authority is recorded: each blob's ACL root is inserted by
// `put()` itself, under the ledger mutex, exactly as before. Only the snapshot write moves.
//
//   B1 (positive control) -- the counter sees the old cost: n `put_blob_safe()` calls on a durable
//         ledger are n snapshot writes. Without this, "1 write" below could mean a broken counter.
//   B2 (the guard) -- both real drains over n files write the snapshot exactly once.
//   B3 -- the snapshot a batched drain leaves is the same set of lines the per-blob loop leaves.
//   B4 -- authority is recorded at put(), not at finish(): inside an open batch the writer can already
//         read the blob and an unrelated identity still cannot.
//   B5 -- the durability window, measured rather than asserted away: inside an open batch the blob's
//         ACL line is not yet on disk; finish() puts it there.
//   B6 -- an early return: when a put() is refused partway (ACL-root cap), the batch's destructor still
//         persists the blobs written before it, so the durable state matches the old loop's.
//   B7 -- a batch that wrote nothing (empty, or only refused puts) writes no snapshot.
//
// Needs no daemon, no network and no credentials.

#include "agentengine/core/ledger.hpp"
#include "agentengine/sandbox/real_io_filesystem.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace agentengine;
namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] std::vector<std::byte> to_bytes(std::string const& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

[[nodiscard]] fs::path fresh_dir(std::string const& name) {
    fs::path const p = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

// The snapshot's lines, sorted: its ACL maps are unordered, so line ORDER is not part of what it says.
[[nodiscard]] std::vector<std::string> snapshot_lines(fs::path const& durable_dir) {
    std::vector<std::string> lines;
    std::ifstream in(durable_dir / "ledger_state.snapshot");
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    std::sort(lines.begin(), lines.end());
    return lines;
}

[[nodiscard]] bool snapshot_names_blob(fs::path const& durable_dir, Digest const& digest) {
    auto const lines = snapshot_lines(durable_dir);
    std::string const prefix = "BLOB_ACL\t" + digest + "\t";
    return std::any_of(lines.begin(), lines.end(),
                       [&](std::string const& l) { return l.starts_with(prefix); });
}

constexpr int kFiles = 200;

[[nodiscard]] std::string file_content(int i) { return "content of file " + std::to_string(i); }

}  // namespace

int main() {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    IdentityHandle owner = authority.mint_root("blob-batch-owner");
    IdentityHandle unrelated = authority.mint_root("blob-batch-unrelated");

    // ---- B1: the positive control.
    fs::path const loop_dir = fresh_dir("ae_blob_batch_loop");
    {
        Ledger<> ledger(InMemoryWorktreeObjectStore{}, loop_dir);
        std::uint64_t const before = ledger.snapshot_write_count();
        bool all_ok = true;
        for (int i = 0; i < kFiles; ++i) {
            all_ok = all_ok && ledger.put_blob_safe(to_bytes(file_content(i)), owner).has_value();
        }
        std::uint64_t const writes = ledger.snapshot_write_count() - before;
        check(all_ok && writes == static_cast<std::uint64_t>(kFiles),
              "B1 (positive control): " + std::to_string(kFiles) + " put_blob_safe() calls on a durable ledger "
                  "are " + std::to_string(kFiles) + " snapshot writes, got " + std::to_string(writes));
        // B3 compares against this snapshot, so it must actually hold the SEQ line and every ACL line;
        // two missing files would compare equal.
        check(snapshot_lines(loop_dir).size() == static_cast<std::size_t>(kFiles) + 1,
              "B1: ... and the snapshot they leave names all " + std::to_string(kFiles) + " blobs");
    }

    // ---- B2 + B3: the real drains.
    fs::path const scan_dir = fresh_dir("ae_blob_batch_scan");
    fs::path const scan_root = fresh_dir("ae_blob_batch_scan_root");
    {
        Ledger<> ledger(InMemoryWorktreeObjectStore{}, scan_dir);
        RealIoFileSystem io(scan_root);
        bool staged = true;
        for (int i = 0; i < kFiles; ++i) {
            staged = staged && io.write("f" + std::to_string(i) + ".txt", to_bytes(file_content(i))).has_value();
        }
        std::uint64_t const before = ledger.snapshot_write_count();
        auto tree = drive(io.scan_and_drain_into_tree(ledger, owner));
        std::uint64_t const writes = ledger.snapshot_write_count() - before;
        check(staged && tree.has_value() && tree->entries.size() == static_cast<std::size_t>(kFiles),
              "B2: scan_and_drain_into_tree() drains all " + std::to_string(kFiles) + " files");
        check(writes == 1, "B2 (the guard): ... and writes the snapshot exactly once, got " + std::to_string(writes));
        check(snapshot_lines(scan_dir) == snapshot_lines(loop_dir),
              "B3: the snapshot it leaves holds the same lines as " + std::to_string(kFiles) +
                  " put_blob_safe() calls leave");
    }

    fs::path const tracked_dir = fresh_dir("ae_blob_batch_tracked");
    fs::path const tracked_root = fresh_dir("ae_blob_batch_tracked_root");
    {
        Ledger<> ledger(InMemoryWorktreeObjectStore{}, tracked_dir);
        RealIoFileSystem io(tracked_root);
        bool staged = true;
        for (int i = 0; i < kFiles; ++i) {
            staged = staged && io.write("f" + std::to_string(i) + ".txt", to_bytes(file_content(i))).has_value();
        }
        std::uint64_t const before = ledger.snapshot_write_count();
        auto tree = drive(io.drain_into_tree(ledger, owner));
        std::uint64_t const writes = ledger.snapshot_write_count() - before;
        check(staged && tree.has_value() && tree->entries.size() == static_cast<std::size_t>(kFiles),
              "B2: drain_into_tree() drains all " + std::to_string(kFiles) + " tracked files");
        check(writes == 1, "B2 (the guard): ... and writes the snapshot exactly once, got " + std::to_string(writes));
        check(snapshot_lines(tracked_dir) == snapshot_lines(loop_dir),
              "B3: the snapshot it leaves holds the same lines as the per-blob loop's");
    }

    // ---- B4 + B5: inside an open batch.
    fs::path const open_dir = fresh_dir("ae_blob_batch_open");
    {
        Ledger<> ledger(InMemoryWorktreeObjectStore{}, open_dir);
        Ledger<>::BlobWriteBatch batch(ledger, owner);
        auto d = batch.put(to_bytes("written inside an open batch"));
        check(d.has_value(), "B4: put() inside a batch succeeds");
        if (d.has_value()) {
            check(ledger.get_blob_safe(*d, owner).has_value(),
                  "B4: before finish(), the writer can already read the blob (its ACL root is in place)");
            check(!ledger.get_blob_safe(*d, unrelated).has_value(),
                  "B4: before finish(), an unrelated identity is still refused");
            check(!snapshot_names_blob(open_dir, *d),
                  "B5: before finish(), the blob's ACL line is not yet on disk (the disclosed window)");
            batch.finish();
            check(snapshot_names_blob(open_dir, *d), "B5: finish() writes it");
        }
    }

    // ---- B6: an early return after a refused put().
    fs::path const refused_dir = fresh_dir("ae_blob_batch_refused");
    {
        // Cap of 1 distinct root per digest: once `unrelated` owns "taken", `owner` writing the same
        // bytes is refused with ledger.acl_root_cap_exceeded.
        Ledger<> ledger(InMemoryWorktreeObjectStore{}, refused_dir, /*max_acl_roots_per_digest=*/1);
        auto taken = ledger.put_blob_safe(to_bytes("taken"), unrelated);
        check(taken.has_value(), "B6: setup -- another root writes the capped blob first");

        Digest first;
        Digest second;
        std::uint64_t writes_before = 0;
        std::string refusal_code;
        {
            writes_before = ledger.snapshot_write_count();
            Ledger<>::BlobWriteBatch batch(ledger, owner);
            auto a = batch.put(to_bytes("first"));
            auto b = batch.put(to_bytes("second"));
            auto refused = batch.put(to_bytes("taken"));
            if (a.has_value()) first = *a;
            if (b.has_value()) second = *b;
            if (!refused.has_value()) refusal_code = refused.error().code;
            // Leaves scope here without finish() -- the shape of every `co_return std::unexpected`
            // inside a drain.
        }
        check(refusal_code == "ledger.acl_root_cap_exceeded",
              "B6: the third put() is refused by the ACL-root cap, got '" + refusal_code + "'");
        check(!first.empty() && snapshot_names_blob(refused_dir, first) && !second.empty() &&
                  snapshot_names_blob(refused_dir, second),
              "B6: the destructor persisted both blobs written before the refusal");
        check(ledger.snapshot_write_count() - writes_before == 1,
              "B6: ... in one snapshot write, got " + std::to_string(ledger.snapshot_write_count() - writes_before));
    }

    // ---- B7: nothing written, nothing persisted.
    fs::path const empty_dir = fresh_dir("ae_blob_batch_empty");
    {
        Ledger<> ledger(InMemoryWorktreeObjectStore{}, empty_dir, /*max_acl_roots_per_digest=*/1);
        auto taken = ledger.put_blob_safe(to_bytes("taken"), unrelated);
        check(taken.has_value(), "B7: setup -- another root writes the capped blob first");
        std::uint64_t const before = ledger.snapshot_write_count();
        {
            Ledger<>::BlobWriteBatch empty(ledger, owner);
        }
        {
            Ledger<>::BlobWriteBatch only_refused(ledger, owner);
            (void)only_refused.put(to_bytes("taken"));
        }
        check(ledger.snapshot_write_count() == before,
              "B7: an empty batch and a batch of only refused puts write no snapshot, got " +
                  std::to_string(ledger.snapshot_write_count() - before));
    }

    std::error_code ec;
    for (auto const& p : {loop_dir, scan_dir, scan_root, tracked_dir, tracked_root, open_dir, refused_dir, empty_dir}) {
        fs::remove_all(p, ec);
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
