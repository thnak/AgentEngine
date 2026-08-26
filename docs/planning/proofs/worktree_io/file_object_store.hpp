#pragma once
// PROVE-PHASE PROBE: a REAL, durable, file-backed agentengine::WorktreeObjectStore conformer --
// closing §27.4's "durability across a process restart" gap. InMemoryWorktreeObjectStore's own real
// file-header comment names this as "a tracked follow-up, not built in this pass" -- built here,
// standalone, conforming to the exact same real WorktreeObjectStore concept (put_blob/get_blob/
// put_tree/get_tree), so it is a drop-in replacement Ledger could hold instead, not a parallel
// invention.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

#include "agentengine/core/worktree_types.hpp"

namespace probe {

// FIX (post-review pass): a real, independent code review flagged that get_blob()/get_tree() built
// filesystem paths directly from a caller-supplied Digest string with NO validation -- not
// exploitable through the one real gated entry point in this tree today (Ledger only ever passes
// digests it computed itself via compute_digest(), never a caller-supplied string), but a real
// latent path-traversal surface given this file's own header comment frames it as "a drop-in
// replacement Ledger could hold instead" -- a future caller that passes an externally-influenced
// string (e.g. a digest round-tripped through an untrusted channel) would have no defense here at
// all. A real SHA-256 hex digest is always exactly 64 lowercase hex characters; anything else
// (including a lexical ".." traversal attempt) is rejected before ever touching the filesystem.
[[nodiscard]] inline bool is_well_formed_digest(agentengine::Digest const& digest) {
    if (digest.size() != 64) return false;
    return std::ranges::all_of(digest, [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// One file per blob/tree, named by its own digest -- content-addressed storage's natural file
// layout (git's own "objects/xx/yyyy..." shape, simplified to a flat directory since this probe
// doesn't need git's fan-out optimization for millions of objects).
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
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
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
        // Trees ARE re-written even if present -- unlike blobs, a tree digest collision with
        // differing bytes would be a real hash collision (astronomically unlikely for SHA-256), so
        // this is a harmless no-op rewrite, not a correctness issue; kept simple rather than adding
        // an existence check with no real benefit here.
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
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

    [[nodiscard]] std::size_t blob_count() const {
        std::size_t n = 0;
        for (auto const& e : std::filesystem::directory_iterator(root_ / "blobs")) { (void)e; ++n; }
        return n;
    }

private:
    // The inverse of canonical_tree_bytes() -- decodes the exact length-prefixed framing that
    // function writes. Kept private/local to this store (not shared with the real project's own
    // internal decode logic, which this probe never needed to read since canonical_tree_bytes()'s
    // own encoding is fully specified in its real, public doc comment).
    [[nodiscard]] static agentengine::result<agentengine::Tree> decode_tree(std::vector<std::byte> const& bytes) {
        std::size_t pos = 0;
        auto read_u32 = [&]() -> std::uint32_t {
            std::uint32_t v = 0;
            for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(bytes[pos + i]) << (8 * i);
            pos += 4;
            return v;
        };
        auto read_str = [&]() -> std::string {
            std::uint32_t len = read_u32();
            std::string s(len, '\0');
            for (std::uint32_t i = 0; i < len; ++i) s[i] = static_cast<char>(bytes[pos + i]);
            pos += len;
            return s;
        };
        agentengine::Tree tree;
        std::uint32_t count = read_u32();
        for (std::uint32_t i = 0; i < count; ++i) {
            std::string name = read_str();
            std::string digest = read_str();
            bool is_tree = static_cast<unsigned char>(bytes[pos]) != 0;
            pos += 1;
            tree.entries.push_back(agentengine::TreeEntry{name, digest, is_tree});
        }
        return tree;
    }

    std::filesystem::path root_;
    mutable std::unique_ptr<std::mutex> mutex_;
};

static_assert(agentengine::WorktreeObjectStore<FileWorktreeObjectStore>);

}  // namespace probe
