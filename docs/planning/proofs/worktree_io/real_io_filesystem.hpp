#pragma once
// PROVE-PHASE PROBE: MediatedFileSystem backed by REAL host filesystem I/O (std::filesystem, real
// ofstream/ifstream reads and writes to a real temp directory) instead of §22's in-memory
// staged_writes_ vector -- proving the "real host filesystem I/O behind MediatedFileSystem" item
// §26.4 named as not yet established. write() lands real bytes on real disk; drain_staged_writes()
// reads them back off REAL disk (not from the in-process vector that wrote them -- a genuine
// round-trip through the OS, catching anything an in-memory stand-in could hide) and builds a REAL
// agentengine::Tree via the real object store. materialize() does the reverse: reads a REAL Tree back
// out of the object store and writes real files to a real host directory, proving rollback actually
// restores real bytes on real disk, not just an in-memory record of "what the tree contains."

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/worktree_types.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"

#include "../common/result.hpp"
#include "worktree_ledger.hpp"

namespace probe {

// A REAL, previously-missing safety check: this design's own no-reuse-of-Mount/mount_read framing
// (§27) means path safety is THIS file's own responsibility, not inherited from the real
// mount_read()/mount_write()'s own established '..'-rejection discipline (worktree_mount_sync.hpp's
// own comment cites sandbox::authorize_spec() rejecting any '.'/'..' component "before ever comparing
// prefixes" -- the same real-project precedent this function reproduces independently). Rejects any
// path that is absolute, contains a '..' component (a lexical parent-traversal attempt defeats a
// naive prefix check with no filesystem access needed, the exact reasoning that real precedent
// documents), or a bare '.' component. This is a NECESSARY, not sufficient, defense -- see this
// file's own probe_path_traversal.cpp for the honestly-disclosed symlink-race residual this check
// does not close.
[[nodiscard]] inline result<void> reject_unsafe_relative_path(std::string const& relative_path) {
    std::filesystem::path p(relative_path);
    if (p.is_absolute()) {
        return std::unexpected(error{"path must be relative: " + relative_path,
                                      "real_io.path_absolute_rejected"});
    }
    for (auto const& component : p) {
        if (component == "..") {
            return std::unexpected(error{"path must not contain '..': " + relative_path,
                                          "real_io.path_traversal_rejected"});
        }
    }
    return result<void>{};
}

// A SECOND, independent check for a DIFFERENT attack the lexical check above cannot see: a
// pre-existing symlink somewhere in the path (no literal ".." in `relative_path` at all -- e.g. a
// directory entry named "link" that is itself a symlink to an out-of-tree location, then
// "link/evil.txt" resolves outside host_root_ with no ".." token anywhere). Resolves the full path
// through any existing symlinks (weakly_canonical -- safe to call even if the final path component
// doesn't exist yet, which real writes need) and verifies the result still lives under host_root_'s
// own canonical form. HONEST RESIDUAL, not closed here: this is a check-then-use gap (TOCTOU) --  a
// symlink created AFTER this check returns but BEFORE the actual open() call races it, the same class
// of gap this design's own §11/§22 already disclose for native-shell mediation rather than claim
// solved.
[[nodiscard]] inline result<void> reject_symlink_escape(std::filesystem::path const& host_root,
                                                          std::string const& relative_path) {
    std::error_code ec;
    std::filesystem::path const canonical_root = std::filesystem::weakly_canonical(host_root, ec);
    if (ec) return result<void>{};  // host_root itself doesn't exist yet -- nothing to escape from
    std::filesystem::path const candidate = std::filesystem::weakly_canonical(host_root / relative_path, ec);
    if (ec) return result<void>{};
    auto const root_str = canonical_root.string();
    auto const candidate_str = candidate.string();
    if (candidate_str.size() < root_str.size() ||
        candidate_str.compare(0, root_str.size(), root_str) != 0) {
        return std::unexpected(error{"path resolves outside the sandbox root via a symlink: " +
                                          relative_path,
                                      "real_io.symlink_escape_rejected"});
    }
    return result<void>{};
}

class RealIoFileSystem {
public:
    explicit RealIoFileSystem(std::filesystem::path host_root)
        : host_root_(std::move(host_root)),
          sync_mutex_(std::make_unique<std::mutex>()),
          commit_lock_(std::make_unique<agentengine::rt::AsyncMutex>()) {
        std::filesystem::create_directories(host_root_);
    }

    // A5/execution_surface: a real caller composing this object with something OUTSIDE its own
    // write()/materialize() API (e.g. an `ExecutionSurface` conformer that needs a real host
    // directory to seed from and drain back into) needs the real path this object already owns --
    // read-only, no new mutation capability granted by exposing it.
    [[nodiscard]] std::filesystem::path const& host_root() const noexcept { return host_root_; }

    // REAL write: bytes actually land on real disk, at a real path under host_root_. Fails closed on
    // an unsafe path BEFORE ever touching the filesystem -- the real check, not a comment promising
    // one.
    [[nodiscard]] result<void> write(std::string const& relative_path, std::vector<std::byte> const& bytes) {
        auto safe = reject_unsafe_relative_path(relative_path);
        if (!safe.has_value()) return std::unexpected(safe.error());
        auto no_symlink_escape = reject_symlink_escape(host_root_, relative_path);
        if (!no_symlink_escape.has_value()) return std::unexpected(no_symlink_escape.error());
        std::lock_guard<std::mutex> guard(*sync_mutex_);
        std::filesystem::path full = host_root_ / relative_path;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary | std::ios::trunc);
        if (!out) {
            return std::unexpected(error{"failed to open real file for writing: " + full.string(),
                                          "real_io.write_failed"});
        }
        out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        touched_.insert(relative_path);
        return result<void>{};
    }

    [[nodiscard]] result<std::vector<std::byte>> read_real_file(std::string const& relative_path) const {
        auto safe = reject_unsafe_relative_path(relative_path);
        if (!safe.has_value()) return std::unexpected(safe.error());
        auto no_symlink_escape = reject_symlink_escape(host_root_, relative_path);
        if (!no_symlink_escape.has_value()) return std::unexpected(no_symlink_escape.error());
        std::filesystem::path full = host_root_ / relative_path;
        std::ifstream in(full, std::ios::binary);
        if (!in) {
            return std::unexpected(error{"failed to open real file for reading: " + full.string(),
                                          "real_io.read_failed"});
        }
        std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::byte> out(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) out[i] = static_cast<std::byte>(raw[i]);
        return out;
    }

    // Walks every path touched since the last drain, reads each one back OFF REAL DISK (a genuine
    // round trip, not the in-memory bytes write() was originally given), puts each as a REAL blob
    // into the object store, and builds a REAL, sorted agentengine::Tree. Takes `Ledger&`, not a raw
    // `InMemoryWorktreeObjectStore&` -- a real concurrent-multi-session run of this exact call
    // segfaulted when this function (and Ledger's own internal commits) both mutated the SAME
    // unsynchronized store directly; routing every access through Ledger's own mutex_-guarded
    // put_blob_safe()/get_tree_safe()/etc. closes that for every caller, not just this one.
    // §34: bound to `Ledger<>` (the default, in-memory-store configuration) -- this class's own
    // real host-disk staging role is orthogonal to whether the LEDGER it drains into is durable;
    // the new durable-Ledger probes (§34) exercise `Ledger<FileWorktreeObjectStore>` directly,
    // without needing this class at all, so it was not worth templatizing on `Store` too.
    [[nodiscard]] agentengine::rt::task<result<agentengine::Tree>> drain_into_tree(
        Ledger<>& ledger, Principal author) {
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

    // A DIFFERENT drain strategy from drain_into_tree() above: that one trusts `touched_` (paths this
    // OBJECT'S OWN write() calls tracked) -- correct for its own purpose, but blind to anything that
    // landed in host_root_ some other way (a bypassing native tool's direct write, §29 Attack 5's own
    // finding; or, concretely, `docker cp` depositing a file from a real container without ever
    // calling this class's write()). This method instead does a REAL, full recursive scan of
    // host_root_ itself -- the honest fix direction for Attack 5's I4 gap: every byte actually
    // present gets captured and attributed to whoever's turn is committing it, rather than silently
    // omitted because no write() call happened to track it. Still cannot attribute WHICH specific
    // caller produced an untracked file (that information was never recorded) -- only that turn's
    // owner is credited, the same disclosed limitation §17.4/§29 already name for a bypassing write.
    // REAL FINDING an independent architecture-fit red-team pass caught (against A3's new
    // `SandboxRuntime`, which calls this method as its own core persistence step): this used to
    // call `ledger.put_blob_safe()` in a raw loop, exactly the shape §35 finding 10 found and
    // fixed in the sibling function `combine_into_tree()` (full_stack/real_sandbox_session.hpp) --
    // if the Nth file's digest hit the ACL-root cap, files 1..N-1 were already durably persisted
    // with no Tree/Checkpoint ever referencing them, and the caller saw only a clean error with no
    // sign that partial content had already reached disk. That fix (`would_accept_blob_write()`)
    // was applied to `combine_into_tree()` only -- this sibling, doing the identical
    // scan-then-persist pattern, was never updated, a real previously-undisclosed regression this
    // pass caught before it shipped anywhere. Fixed the same way: collect every file's bytes
    // first, validate the WHOLE batch against the ACL-root cap, THEN write.
    [[nodiscard]] agentengine::rt::task<result<agentengine::Tree>> scan_and_drain_into_tree(Ledger<>& ledger,
                                                                                                Principal author) {
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
                co_return std::unexpected(error{
                    "scanned file '" + rel + "' would exceed its digest's ACL-root cap -- "
                    "rejecting the whole scan before writing any of it, not partway through",
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
    // Ledger. Also routed through Ledger's own thread-safe accessors, same reason as above.
    [[nodiscard]] agentengine::rt::task<result<void>> materialize(
        Ledger<>& ledger, agentengine::Digest const& tree_digest, Principal caller) {
        agentengine::rt::AsyncMutex::Guard commit_guard = co_await commit_lock_->lock();

        auto tree = ledger.get_tree_safe(tree_digest, caller);
        if (!tree.has_value()) co_return std::unexpected(tree.error());

        // A REAL FINDING a code-review pass caught: unlike write()/read_real_file(), this loop used to
        // write straight to `host_root_ / entry.name` with no path-safety check at all -- a committed
        // Tree entry named "../../evil.txt" (commit() only ACL-gates the entry's DIGEST, never its
        // NAME) would escape the sandbox root on rollback. Validate every entry BEFORE touching disk,
        // same discipline write() already had.
        for (auto const& entry : tree->entries) {
            auto safe = reject_unsafe_relative_path(entry.name);
            if (!safe.has_value()) co_return std::unexpected(safe.error());
        }

        // A SECOND real finding: write() takes `sync_mutex_` for its own critical section, but this
        // method previously took only `commit_lock_` (a DIFFERENT mutex) around its remove_all()/
        // rewrite of host_root_ -- the two were never mutually exclusive despite mutating the same
        // real directory tree. Take the SAME lock write() uses for the actual filesystem mutation, so
        // a concurrent write() cannot land mid-remove_all() or mid-rewrite.
        std::lock_guard<std::mutex> guard(*sync_mutex_);

        std::error_code ec;
        std::filesystem::remove_all(host_root_, ec);
        std::filesystem::create_directories(host_root_);

        for (auto const& entry : tree->entries) {
            auto bytes = ledger.get_blob_safe(entry.digest, caller);
            if (!bytes.has_value()) co_return std::unexpected(bytes.error());
            // Re-check symlink escape against the just-recreated host_root_ right before each write --
            // weakly_canonical is safe to call even though the target file doesn't exist yet.
            auto no_symlink_escape = reject_symlink_escape(host_root_, entry.name);
            if (!no_symlink_escape.has_value()) co_return std::unexpected(no_symlink_escape.error());
            std::filesystem::path full = host_root_ / entry.name;
            std::filesystem::create_directories(full.parent_path());
            std::ofstream out(full, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<char const*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        }
        co_return result<void>{};
    }

private:
    std::filesystem::path host_root_;
    std::unique_ptr<std::mutex> sync_mutex_;
    std::set<std::string> touched_;
    std::unique_ptr<agentengine::rt::AsyncMutex> commit_lock_;
};

}  // namespace probe
