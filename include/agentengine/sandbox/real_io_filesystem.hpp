#pragma once
// Implements ADR-102 Phase 3 -- `RealIoFileSystem`: real host filesystem I/O (std::filesystem, real
// ReadFile/WriteFile against a real staging directory) staging bytes for `Ledger` (core/ledger.hpp),
// and materializing a real `Ledger` tree back onto real disk for rollback. This is what lets
// `SandboxRuntime` (sandbox_runtime.hpp) give an `ExecutionSurface` something real to seed from and
// drain real output back into.
//
// Ported from docs/planning/proofs/worktree_io/real_io_filesystem.hpp (ADR-099's own standalone,
// red-teamed, live-tested prove-phase original -- kept as-is, this is a new file). Real changes made
// during the port: `probe::Principal` -> `agentengine::IdentityHandle` throughout;
// `probe::result<T>`/`probe::error{message, code}` -> the real `agentengine::result<T>`/
// `agentengine::error{failure_class, message, code}` (`contract` for a caller-supplied unsafe/
// traversal path, `fatal` for a real I/O failure); `Ledger<>`/`Principal` in the prove-phase original
// now refer to this phase's own real, ported `agentengine::Ledger<>`/`agentengine::IdentityHandle`
// (core/ledger.hpp, ADR-102 Phase 2) rather than the prove-phase standalone types.
//
// PLATFORM-PORTABLE (2026-08-29, decisions/ADR-104-real-io-filesystem-linux-parity.md): this class's
// two methods that need a real, verified OS handle -- `write_verified()`/`read_verified()` -- are
// declared here but defined out-of-line, once per platform (src/sandbox/real_io_filesystem.cpp for
// Windows' `open_within_mount_root`+`WriteFile`/`ReadFile`/`GetFileSizeEx`,
// src/sandbox/real_io_filesystem_posix.cpp for Linux's own `open_within_mount_root`+`::write`/
// `::read`/`::fstat`) -- the same "portable declaration, two platform .cpp files" split
// `core/worktree_digest.hpp`'s `compute_digest()` and ADR-103's `MediatedFileSystemAdapter` already
// use, rather than `#ifdef`-ing inside one file. Every other method on this class (write(),
// read_real_file(), drain_into_tree(), scan_and_drain_into_tree(), materialize(), the constructor)
// is 100% portable already -- pure `std::filesystem`/`Ledger`/`AsyncMutex` calls with no OS-specific
// symbol anywhere -- so it stays inline, right here, unchanged. Was previously Windows-only
// (unconditionally including `core/worktree_mount_fs.hpp`, which pulls in `<windows.h>`); this pass
// removes that transitive include from the portable header entirely -- each platform `.cpp` includes
// only the mount-root header its own platform needs.

#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/ledger.hpp"
#include "agentengine/core/worktree_types.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/trust/identity_authority.hpp"

namespace agentengine {

// Rejects any path that is absolute, contains a '..' component (a lexical parent-traversal attempt
// defeats a naive prefix check with no filesystem access needed), or a bare '.' component. A
// NECESSARY, not sufficient, defense -- real containment against a symlink/junction escape is
// `open_within_mount_root`'s own job (ADR-014's accepted Design B), called by `write_verified()`/
// `read_verified()` below, not this lexical pre-check.
[[nodiscard]] inline agentengine::result<void> real_io_reject_unsafe_relative_path(
        std::string const& relative_path) {
    std::filesystem::path p(relative_path);
    if (p.is_absolute()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                      "path must be relative: " + relative_path,
                                                      "real_io.path_absolute_rejected"});
    }
    for (auto const& component : p) {
        if (component == "..") {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                          "path must not contain '..': " + relative_path,
                                                          "real_io.path_traversal_rejected"});
        }
    }
    return agentengine::result<void>{};
}

class RealIoFileSystem {
public:
    explicit RealIoFileSystem(std::filesystem::path host_root)
        : host_root_(std::move(host_root)),
          sync_mutex_(std::make_unique<std::mutex>()),
          commit_lock_(std::make_unique<agentengine::rt::AsyncMutex>()) {
        std::filesystem::create_directories(host_root_);
    }

    // A real caller composing this object with something OUTSIDE its own write()/materialize() API
    // (e.g. an `ExecutionSurface` conformer that needs a real host directory to seed from and drain
    // back into) needs the real path this object already owns -- read-only, no new mutation
    // capability granted by exposing it.
    [[nodiscard]] std::filesystem::path const& host_root() const noexcept { return host_root_; }

    // The real, handle-based open+verify step -- ADR-014's own accepted Design B, reused verbatim
    // from production (`agentengine::open_within_mount_root`), not re-derived. The object each
    // platform's own mount-root primitive verifies IS the object the platform-specific write/read
    // syscalls below act on -- no window between "checked" and "used" for anything this function
    // itself does. Defined out-of-line, once per platform (see this header's own top comment).
    [[nodiscard]] agentengine::result<void> write_verified(std::string const& relative_path,
                                                               std::vector<std::byte> const& bytes);

    [[nodiscard]] agentengine::result<std::vector<std::byte>> read_verified(
            std::string const& relative_path) const;

    // REAL write: bytes actually land on real disk, at a real path under host_root_. Fails closed on
    // an unsafe path BEFORE ever touching the filesystem.
    [[nodiscard]] agentengine::result<void> write(std::string const& relative_path,
                                                      std::vector<std::byte> const& bytes) {
        auto safe = real_io_reject_unsafe_relative_path(relative_path);
        if (!safe.has_value()) return std::unexpected(safe.error());
        std::lock_guard<std::mutex> guard(*sync_mutex_);
        // HONEST, DISCLOSED, NARROWER residual: create_directories() still resolves the parent as a
        // STRING, ahead of write_verified()'s real handle-based check below. The worst this can do is
        // misdirect WHERE a brand-new, empty directory gets created if an intermediate segment is
        // swapped for a junction mid-call -- it cannot forge or leak file CONTENT, because
        // write_verified()'s own containment check runs independently and rejects the write
        // regardless of what create_directories() did.
        std::filesystem::path const parent = (host_root_ / relative_path).parent_path();
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        auto written = write_verified(relative_path, bytes);
        if (!written.has_value()) return std::unexpected(written.error());
        touched_.insert(relative_path);
        return agentengine::result<void>{};
    }

    [[nodiscard]] agentengine::result<std::vector<std::byte>> read_real_file(
            std::string const& relative_path) const {
        auto safe = real_io_reject_unsafe_relative_path(relative_path);
        if (!safe.has_value()) return std::unexpected(safe.error());
        return read_verified(relative_path);
    }

    // Walks every path touched since the last drain, reads each one back OFF REAL DISK (a genuine
    // round trip, not the in-memory bytes write() was originally given), puts each as a REAL blob
    // into the object store via `Ledger`, and builds a REAL, sorted `Tree`. Takes `Ledger<>&`, not a
    // raw object store -- routing every access through `Ledger`'s own mutex_-guarded
    // put_blob_safe()/get_tree_safe()/etc. is what keeps a real concurrent multi-session run from
    // racing two unsynchronized accesses to the same underlying store.
    [[nodiscard]] agentengine::rt::task<agentengine::result<agentengine::Tree>> drain_into_tree(
            agentengine::Ledger<>& ledger, agentengine::IdentityHandle author) {
        std::set<std::string> paths;
        {
            std::lock_guard<std::mutex> guard(*sync_mutex_);
            paths = std::move(touched_);
            touched_.clear();
        }
        agentengine::rt::AsyncMutex::Guard commit_guard = co_await commit_lock_->lock();

        agentengine::Tree tree;
        for (auto const& path : paths) {
            auto bytes = read_real_file(path);   // REAL disk read, independent of what write() staged
            if (!bytes.has_value()) co_return std::unexpected(bytes.error());
            auto blob_digest = ledger.put_blob_safe(*bytes, author);
            if (!blob_digest.has_value()) co_return std::unexpected(blob_digest.error());
            tree.entries.push_back(agentengine::TreeEntry{path, *blob_digest, false});
        }
        co_return tree;
    }

    // A DIFFERENT drain strategy from drain_into_tree() above: that one trusts `touched_` (paths
    // this OBJECT'S OWN write() calls tracked) -- correct for its own purpose, but blind to anything
    // that landed in host_root_ some other way (a bypassing native tool's direct write; concretely,
    // `docker cp` depositing a file from a real container without ever calling this class's write()).
    // This method instead does a REAL, full recursive scan of host_root_ itself -- every byte
    // actually present gets captured and attributed to whoever's turn is committing it, rather than
    // silently omitted because no write() call happened to track it. Still cannot attribute WHICH
    // specific caller produced an untracked file (that information was never recorded) -- only that
    // turn's owner is credited.
    //
    // Validates the WHOLE batch against the Ledger's own ACL-root cap BEFORE writing any of it --
    // collecting every file's bytes first, then checking `would_accept_blob_write()` for the whole
    // set, then writing -- so a rejection partway through never leaves earlier files durably
    // persisted with no Tree/Checkpoint ever referencing them.
    [[nodiscard]] agentengine::rt::task<agentengine::result<agentengine::Tree>> scan_and_drain_into_tree(
            agentengine::Ledger<>& ledger, agentengine::IdentityHandle author) {
        agentengine::rt::AsyncMutex::Guard commit_guard = co_await commit_lock_->lock();

        std::vector<std::pair<std::string, std::vector<std::byte>>> collected;
        if (std::filesystem::exists(host_root_)) {
            for (auto const& entry : std::filesystem::recursive_directory_iterator(host_root_)) {
                if (!entry.is_regular_file()) continue;
                std::string rel = std::filesystem::relative(entry.path(), host_root_).generic_string();
                auto bytes = read_real_file(rel);
                if (!bytes.has_value()) co_return std::unexpected(bytes.error());
                collected.emplace_back(std::move(rel), std::move(*bytes));
            }
        }
        for (auto const& [rel, bytes] : collected) {
            if (!ledger.would_accept_blob_write(bytes, author)) {
                co_return std::unexpected(agentengine::error{
                    agentengine::failure_class::resource,
                    "scanned file '" + rel + "' would exceed its digest's ACL-root cap -- rejecting "
                    "the whole scan before writing any of it, not partway through",
                    "ledger.acl_root_cap_exceeded"});
            }
        }

        agentengine::Tree tree;
        for (auto const& [rel, bytes] : collected) {
            auto blob_digest = ledger.put_blob_safe(bytes, author);
            if (!blob_digest.has_value()) co_return std::unexpected(blob_digest.error());
            tree.entries.push_back(agentengine::TreeEntry{rel, *blob_digest, false});
        }

        {
            std::lock_guard<std::mutex> guard(*sync_mutex_);
            touched_.clear();   // a full scan supersedes anything the tracked set still held
        }
        co_return tree;
    }

    // The REAL reverse direction -- reads a REAL Tree back out of the store and writes its content
    // to REAL files on real disk, wiping whatever was there before. This is what a real rollback
    // must do to actually restore a session's working directory, not just move a pointer in the
    // Ledger.
    [[nodiscard]] agentengine::rt::task<agentengine::result<void>> materialize(
            agentengine::Ledger<>& ledger, agentengine::Digest const& tree_digest,
            agentengine::IdentityHandle caller) {
        agentengine::rt::AsyncMutex::Guard commit_guard = co_await commit_lock_->lock();

        auto tree = ledger.get_tree_safe(tree_digest, caller);
        if (!tree.has_value()) co_return std::unexpected(tree.error());

        // Validate every entry's NAME before touching disk -- commit() only ACL-gates an entry's
        // DIGEST, never its NAME, so a committed Tree entry named "../../evil.txt" would otherwise
        // escape the sandbox root on rollback.
        for (auto const& entry : tree->entries) {
            auto safe = real_io_reject_unsafe_relative_path(entry.name);
            if (!safe.has_value()) co_return std::unexpected(safe.error());
        }

        // Takes the SAME lock write() uses for the actual filesystem mutation (not a different one),
        // so a concurrent write() cannot land mid-remove_all() or mid-rewrite of the same real
        // directory tree.
        std::lock_guard<std::mutex> guard(*sync_mutex_);

        std::error_code ec;
        std::filesystem::remove_all(host_root_, ec);
        std::filesystem::create_directories(host_root_);

        for (auto const& entry : tree->entries) {
            auto bytes = ledger.get_blob_safe(entry.digest, caller);
            if (!bytes.has_value()) co_return std::unexpected(bytes.error());
            std::filesystem::path const full_parent = (host_root_ / entry.name).parent_path();
            std::error_code mkdir_ec;
            std::filesystem::create_directories(full_parent, mkdir_ec);
            auto written = write_verified(entry.name, *bytes);
            if (!written.has_value()) co_return std::unexpected(written.error());
        }
        co_return agentengine::result<void>{};
    }

private:
    std::filesystem::path host_root_;
    std::unique_ptr<std::mutex> sync_mutex_;
    std::set<std::string> touched_;
    std::unique_ptr<agentengine::rt::AsyncMutex> commit_lock_;
};

}  // namespace agentengine
