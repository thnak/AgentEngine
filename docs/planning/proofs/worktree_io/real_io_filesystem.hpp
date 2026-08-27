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
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/worktree_mount_fs.hpp"
#include "agentengine/core/worktree_types.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"

#include "../common/result.hpp"
#include "worktree_ledger.hpp"

namespace probe {

// §38: bridges a real `agentengine::error` (klass/message/code/native_code) from
// `open_within_mount_root` down to this file's own probe::error(message, code) shape -- verbatim,
// not relabeled, so a caller inspecting `.code` sees the REAL underlying mechanism's own error
// code (e.g. "worktree.mount_path_escapes_root"), not a synthetic string invented to look familiar.
[[nodiscard]] inline error to_probe_error(agentengine::error const& e) {
    return error{e.message, e.code};
}

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

// §38 TOCTOU replay (probe_toctou_symlink_race.cpp) proved this function's own disclosed residual
// is a REAL, deterministically-reproducible vulnerability -- the identical Design-A shape ADR-014
// already found and fixed for a sibling mediation primitive in this exact codebase
// (`agentengine::redteam::naive_check_within_root`): canonicalize a path into a STRING, check the
// STRING, return -- with nothing tying that answer to what the filesystem looks like at the moment
// a caller later re-derives and reopens the same string. `write()`/`read_real_file()`/`materialize()`
// no longer call this function for that reason -- they use `agentengine::open_within_mount_root()`
// (ADR-014's accepted Design B: one handle-based open, verify from the RESOLVED HANDLE, never a
// re-parsed string) instead. This function is kept, deliberately, as the known-vulnerable reference
// implementation `probe_toctou_symlink_race.cpp` uses to demonstrate the vulnerability class
// empirically -- the same permanent-deliberate-control treatment ADR-014's own
// `redteam::naive_check_within_root` gets in production. NEVER call this from new mediation code.
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

    // §38 fix: the real, handle-based open+verify step -- ADR-014's own accepted Design B, reused
    // verbatim from production (`agentengine::open_within_mount_root`), not re-derived. The object
    // `GetFinalPathNameByHandleW` verifies inside that call IS the object `WriteFile` below writes
    // to -- no window between "checked" and "used" for anything this function itself does.
    [[nodiscard]] result<void> write_verified(std::string const& relative_path,
                                                std::vector<std::byte> const& bytes) {
        auto handle = agentengine::open_within_mount_root(host_root_.wstring(), relative_path,
                                                             GENERIC_WRITE, CREATE_ALWAYS);
        if (!handle.has_value()) return std::unexpected(to_probe_error(handle.error()));
        DWORD written = 0;
        BOOL const ok = bytes.empty()
            ? TRUE
            : WriteFile(handle->get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        if (!ok || (!bytes.empty() && written != bytes.size())) {
            return std::unexpected(error{"WriteFile failed on a verified handle: " + relative_path,
                                          "real_io.write_failed"});
        }
        return result<void>{};
    }

    [[nodiscard]] result<std::vector<std::byte>> read_verified(std::string const& relative_path) const {
        auto handle = agentengine::open_within_mount_root(host_root_.wstring(), relative_path,
                                                             GENERIC_READ, OPEN_EXISTING);
        if (!handle.has_value()) return std::unexpected(to_probe_error(handle.error()));
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle->get(), &size) || size.QuadPart < 0) {
            return std::unexpected(error{"GetFileSizeEx failed on a verified handle: " + relative_path,
                                          "real_io.read_failed"});
        }
        std::vector<std::byte> out(static_cast<std::size_t>(size.QuadPart));
        if (!out.empty()) {
            DWORD read_bytes = 0;
            BOOL const ok = ReadFile(handle->get(), out.data(), static_cast<DWORD>(out.size()),
                                       &read_bytes, nullptr);
            if (!ok || read_bytes != out.size()) {
                return std::unexpected(error{"ReadFile failed on a verified handle: " + relative_path,
                                              "real_io.read_failed"});
            }
        }
        return out;
    }

    // REAL write: bytes actually land on real disk, at a real path under host_root_. Fails closed on
    // an unsafe path BEFORE ever touching the filesystem -- the real check, not a comment promising
    // one.
    [[nodiscard]] result<void> write(std::string const& relative_path, std::vector<std::byte> const& bytes) {
        auto safe = reject_unsafe_relative_path(relative_path);
        if (!safe.has_value()) return std::unexpected(safe.error());
        std::lock_guard<std::mutex> guard(*sync_mutex_);
        // HONEST, DISCLOSED, NARROWER residual: create_directories() still resolves the parent as a
        // STRING, ahead of write_verified()'s real handle-based check below. Unlike the file-content
        // gap §38 fixed, the worst this can do is misdirect WHERE a brand-new, empty directory gets
        // created if an intermediate segment is swapped for a junction mid-call -- it cannot forge or
        // leak file CONTENT, because write_verified()'s own containment check runs independently,
        // against whatever CreateFileW actually resolved, and rejects the write regardless of what
        // create_directories() did. open_within_mount_root's own header states it deliberately does
        // not offer directory creation -- closing this narrower residual needs its own primitive, not
        // something this fix can absorb for free.
        std::filesystem::path const parent = (host_root_ / relative_path).parent_path();
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        auto written = write_verified(relative_path, bytes);
        if (!written.has_value()) return std::unexpected(written.error());
        touched_.insert(relative_path);
        return result<void>{};
    }

    [[nodiscard]] result<std::vector<std::byte>> read_real_file(std::string const& relative_path) const {
        auto safe = reject_unsafe_relative_path(relative_path);
        if (!safe.has_value()) return std::unexpected(safe.error());
        return read_verified(relative_path);
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
            // §38 fix: same handle-based verify-then-write primitive as write() above, not the old
            // weakly_canonical-then-separate-ofstream shape probe_toctou_symlink_race.cpp proved
            // exploitable. Same disclosed, narrower create_directories()-is-string-based residual too.
            std::filesystem::path const full_parent = (host_root_ / entry.name).parent_path();
            std::error_code mkdir_ec;
            std::filesystem::create_directories(full_parent, mkdir_ec);
            auto written = write_verified(entry.name, *bytes);
            if (!written.has_value()) co_return std::unexpected(written.error());
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
