#pragma once
// Implements the content-durability half of the gap ADR-102/ADR-126/ADR-128 each named and declined to
// close: "a real, separate, materially larger piece of work (a durable object-store conformer for
// blob/tree content, not merely branch/ACL bookkeeping)". `Ledger<Store>::durable_dir` already makes
// branch/ACL bookkeeping durable (`persist_snapshot_locked()`/`load_durable_state()`) -- it never
// touches `store_` (`ledger.hpp`'s own constructor comment: "atop whatever durability `Store` itself
// already provides for blob/tree CONTENT"), and the production default, `InMemoryWorktreeObjectStore`,
// provides none. `FileWorktreeObjectStore` is a second, real `agentengine::WorktreeObjectStore`
// conformer a caller can hand `Ledger<Store>` instead, when it wants BOTH halves durable.
//
// Ported from `docs/planning/proofs/worktree_io/file_object_store.hpp` (`probe::FileWorktreeObjectStore`)
// -- a real, standalone, cross-process-proven prototype (`identity-native-sandbox-worktree-design.md`
// §34.3/§34.6: two genuinely separate OS processes shared nothing but a directory, and branch, ACL, AND
// content all survived a real process exit). This is a real port, not a rewrite -- the storage shape
// (one file per digest under two flat directories, git's own loose-object layout minus fan-out), the
// temp-file-plus-atomic-`rename` write discipline, the bounds-checked `decode_tree()`, and the
// `is_well_formed_digest()` path-traversal gate are all carried forward unchanged from the prove-phase
// original's own already-red-teamed form. Real changes made during the port, not cosmetic:
//   - `probe::` namespace dropped -- `agentengine::FileWorktreeObjectStore`, matching
//     `InMemoryWorktreeObjectStore`'s own sibling placement in `worktree_types.hpp`.
//   - The `std::unique_ptr<std::mutex>` indirection (rather than a plain `std::mutex` member) was
//     explicitly RE-CHECKED against this port's own real constraint, not assumed to still apply:
//     `Ledger<Store>`'s constructor (`ledger.hpp`: `explicit Ledger(Store store = Store{}, ...)
//     : store_(std::move(store)), ...`) unconditionally MOVES its `store` parameter into the `store_`
//     member on every construction -- not merely constructs it in place (C++17 guaranteed copy elision
//     covers the caller's own temporary-into-parameter step, but the parameter-into-member step is a
//     real, required move). `std::mutex` is neither movable nor copyable, so the indirection is not
//     legacy prove-phase caution -- it is still load-bearing today, for the identical reason.
//   - Doc comments reference this file's own real location and the real `Ledger<Store>` constructor
//     it composes with, rather than the standalone probe's own framing.

#include "agentengine/core/worktree_types.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

namespace agentengine {

// A real SHA-256 hex digest is always exactly 64 lowercase hex characters; a caller-supplied string
// that is anything else (including a lexical ".." traversal attempt) is rejected before it is ever used
// to build a filesystem path. `Ledger` itself only ever passes digests it computed via
// `compute_digest()`, never a caller-supplied string, so this is defense-in-depth for a future caller
// this store's own top comment explicitly frames as a real possibility ("a drop-in replacement `Ledger`
// could hold instead"), not a live gap in the one real gated entry point this tree has today.
[[nodiscard]] inline bool is_well_formed_digest(agentengine::Digest const& digest) {
    if (digest.size() != 64) return false;
    return std::ranges::all_of(digest, [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// A real `WorktreeObjectStore` conformer backed by real files on real disk -- one regular file per
// digest, two flat directories (`root/blobs/`, `root/trees/`), content-addressed by construction (the
// file's own name IS its integrity check: a caller can always re-hash what it read and compare).
// ae-naming-lint: allow FileWorktreeObjectStore — ADR-025 §4c precedent (InMemoryWorktreeObjectStore's own identical suppression): deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class FileWorktreeObjectStore {
public:
    explicit FileWorktreeObjectStore(std::filesystem::path root)
        : root_(std::move(root)), mutex_(std::make_unique<std::mutex>()) {
        std::filesystem::create_directories(root_ / "blobs");
        std::filesystem::create_directories(root_ / "trees");
    }

    [[nodiscard]] agentengine::result<agentengine::Digest> put_blob(std::span<std::byte const> bytes) {
        auto digest = agentengine::compute_digest(bytes);
        if (!digest) return std::unexpected(digest.error());
        std::lock_guard<std::mutex> guard(*mutex_);
        std::filesystem::path path = root_ / "blobs" / *digest;
        if (!std::filesystem::exists(path)) {   // real dedup: an existing blob file is never rewritten
            // Temp-file + atomic-rename, the same discipline `persist_snapshot_locked()`
            // (`ledger.hpp`) and `identity_authority.hpp`'s `persist_high_water_mark()` already use
            // elsewhere in this codebase -- a crash mid-write must never leave a half-written file
            // sitting at the final digest-named path, where a later `get_blob()` would return
            // truncated content under a digest that no longer matches its own bytes.
            std::filesystem::path temp = root_ / "blobs" / (*digest + ".tmp");
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            std::error_code ec;
            std::filesystem::rename(temp, path, ec);
        }
        return *digest;
    }

    [[nodiscard]] agentengine::result<std::vector<std::byte>> get_blob(agentengine::Digest const& digest) const {
        if (!is_well_formed_digest(digest)) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                          "digest is not a well-formed 64-char hex SHA-256 string",
                                                          "worktree.malformed_digest"});
        }
        std::filesystem::path path = root_ / "blobs" / digest;
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                          "no blob with this digest exists in the store",
                                                          "worktree.blob_not_found"});
        }
        std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::byte> out(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) out[i] = static_cast<std::byte>(raw[i]);
        return out;
    }

    [[nodiscard]] agentengine::result<agentengine::Digest> put_tree(agentengine::Tree tree) {
        std::ranges::sort(tree.entries, {}, &agentengine::TreeEntry::name);
        auto bytes = agentengine::canonical_tree_bytes(tree);
        auto digest = agentengine::compute_digest(bytes);
        if (!digest) return std::unexpected(digest.error());
        std::lock_guard<std::mutex> guard(*mutex_);
        std::filesystem::path path = root_ / "trees" / *digest;
        // Trees ARE re-written even if present -- unlike blobs, a tree digest collision with differing
        // bytes would be a real hash collision (astronomically unlikely for SHA-256), so this is a
        // harmless no-op rewrite, not a correctness issue; kept simple rather than adding an existence
        // check with no real benefit here. Same temp-file + atomic-rename discipline as put_blob().
        {
            std::filesystem::path temp = root_ / "trees" / (*digest + ".tmp");
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            std::error_code ec;
            std::filesystem::rename(temp, path, ec);
        }
        return *digest;
    }

    [[nodiscard]] agentengine::result<agentengine::Tree> get_tree(agentengine::Digest const& digest) const {
        if (!is_well_formed_digest(digest)) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                          "digest is not a well-formed 64-char hex SHA-256 string",
                                                          "worktree.malformed_digest"});
        }
        std::filesystem::path path = root_ / "trees" / digest;
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                          "no tree with this digest exists in the store",
                                                          "worktree.tree_not_found"});
        }
        std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) bytes[i] = static_cast<std::byte>(raw[i]);
        return decode_tree(bytes);
    }

    // Test-only introspection, mirroring InMemoryWorktreeObjectStore's own blob_count()/tree_count().
    [[nodiscard]] std::size_t blob_count() const {
        std::size_t n = 0;
        std::error_code ec;
        for (auto const& e : std::filesystem::directory_iterator(root_ / "blobs", ec)) {
            (void)e;
            ++n;
        }
        return n;
    }

    [[nodiscard]] std::size_t tree_count() const {
        std::size_t n = 0;
        std::error_code ec;
        for (auto const& e : std::filesystem::directory_iterator(root_ / "trees", ec)) {
            (void)e;
            ++n;
        }
        return n;
    }

private:
    // The inverse of canonical_tree_bytes() -- decodes the exact length-prefixed framing that function
    // writes. Every read below checks remaining length first and fails closed with a normal `result`
    // error rather than reading past the end of the vector -- a truncated/corrupt file (a crash
    // mid-write, before put_tree()'s atomic-rename write path made this unreachable in ordinary
    // operation) must produce an attributable error, never undefined behavior.
    [[nodiscard]] static agentengine::result<agentengine::Tree> decode_tree(std::vector<std::byte> const& bytes) {
        std::size_t pos = 0;
        auto has_remaining = [&](std::size_t n) { return bytes.size() - pos >= n; };
        auto read_u32 = [&](std::uint32_t& out) -> bool {
            if (!has_remaining(4)) return false;
            std::uint32_t v = 0;
            for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(bytes[pos + i]) << (8 * i);
            pos += 4;
            out = v;
            return true;
        };
        auto read_str = [&](std::string& out) -> bool {
            std::uint32_t len = 0;
            if (!read_u32(len)) return false;
            if (!has_remaining(len)) return false;
            std::string s(len, '\0');
            for (std::uint32_t i = 0; i < len; ++i) s[i] = static_cast<char>(bytes[pos + i]);
            pos += len;
            out = std::move(s);
            return true;
        };
        auto corrupt = [] {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "tree bytes are truncated or malformed (a crash mid-write before an atomic-rename "
                "completed, or real corruption)",
                "worktree.tree_decode_failed"});
        };

        agentengine::Tree tree;
        std::uint32_t count = 0;
        if (!read_u32(count)) return corrupt();
        for (std::uint32_t i = 0; i < count; ++i) {
            std::string name, digest;
            if (!read_str(name)) return corrupt();
            if (!read_str(digest)) return corrupt();
            if (!has_remaining(1)) return corrupt();
            bool is_tree = static_cast<unsigned char>(bytes[pos]) != 0;
            pos += 1;
            tree.entries.push_back(agentengine::TreeEntry{name, digest, is_tree});
        }
        return tree;
    }

    std::filesystem::path root_;
    // unique_ptr indirection, not a plain std::mutex member -- see this file's own top comment for why
    // this is still load-bearing (Ledger<Store>'s constructor moves its Store parameter unconditionally).
    mutable std::unique_ptr<std::mutex> mutex_;
};

static_assert(agentengine::WorktreeObjectStore<FileWorktreeObjectStore>);

}  // namespace agentengine
