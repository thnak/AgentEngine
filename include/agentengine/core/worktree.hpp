#pragma once
// Implements 025-Worktree-and-Virtual-Filesystem.md §2 -- a content-addressed object store plus a
// mutable tree: Blob = immutable bytes addressed by digest, Tree = {name -> Blob|Tree} addressed by
// digest, Ref = a mutable name -> Tree digest. This header is the vocabulary + concept; concrete
// stores (InMemoryWorktreeObjectStore below, a durable pal::file_io-backed adapter as a tracked
// follow-up -- docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md Phase A) satisfy
// `WorktreeObjectStore`.
//
// Digest algorithm: SHA-256, hex-encoded (64 chars) -- the same digest+store vocabulary
// core/content.hpp's BlobRef already names (003 §3), not a second choice invented here.
// `compute_digest` is declared here and implemented per-platform in src/core/worktree_digest.cpp
// (Windows CNG/BCrypt for now -- a system API, not a third-party dependency, same posture ADR-005's
// capability_token.cpp already established for HMAC-SHA256). A Linux SHA-256 provider is a named,
// tracked gap (the milestone-3 breakdown's own decision 2), not silently assumed available -- links
// against `agentengine::worktree_store`, Windows-only for now.
//
// `Ref` persistence is deliberately NOT part of this header's store: a Ref is ordinary small
// Quark-actor state (mutates often, wants fencing/durability/history) and goes through Quark's
// `Store` seam directly, unlike Blob/Tree (immutable, digest-addressed, write-once) -- see the
// milestone-3 breakdown's decision 1 for why forcing both shapes through one seam would be a
// genuine misfit, not an elegant reuse.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine {

using Digest = std::string;  // hex-encoded SHA-256, 64 lowercase hex chars

// SHA-256(bytes), hex-encoded. Implemented in src/core/worktree_digest.cpp (Windows for now);
// requires linking `agentengine::worktree_store`.
[[nodiscard]] result<Digest> compute_digest(std::span<std::byte const> bytes);

// One entry in a Tree: a name plus what it points at. `is_tree` distinguishes a nested Tree digest
// from a Blob digest -- a flat flag rather than a recursive variant. The old M0 stub's
// `TreeEntry = std::variant<Blob, Tree*>` cannot round-trip through a digest-addressed store at
// all: a raw pointer is not a stable, content-derived address, and embedding a whole `Blob` inline
// (rather than its digest) would make two trees referencing the identical blob unable to share
// storage -- the opposite of 025 §2's dedup claim. A bool is the plainest way to say which of the
// store's two tables (`blobs_`/`trees_`) a digest should be looked up in, without inventing a
// second digest-space tag.
struct TreeEntry {
    std::string name;
    Digest      digest;
    bool        is_tree = false;
};

// `entries` MUST be sorted by `name` before this Tree is hashed or stored -- `put_tree` (below)
// sorts on the caller's behalf, both before computing the digest and before storing, so a Tree
// round-tripped through the store is always canonical on read too. This is what makes 025 §2's
// dedup claim ("the same file written by two agents is stored once") actually true rather than
// aspirational: two trees with identical entries built in different insertion order must hash
// identically, which requires a canonical order to hash over.
struct Tree {
    std::vector<TreeEntry> entries;
};

// A mutable name -> Tree digest (025 §2) -- what "the worktree" currently is, for one session or
// one principal (029 §2 reuses this exact shape one level up, scoped to a principal instead).
// Persistence is Quark's `Store` seam directly, not this header's object store -- see the file-top
// comment and the milestone-3 breakdown's decision 1.
struct Ref {
    std::string name;       // e.g. "session:s-42" or "principal:p-7"
    Digest      tree_digest;
};

enum class sharing_mode { shared, branch, readonly, scratch };

// The canonical byte serialization a Tree's digest is computed over. PRECONDITION: `tree.entries`
// is already sorted by name (callers that can't guarantee this should go through `put_tree`, which
// sorts first). Every variable-length field is length-prefixed (u32, little-endian), the same
// framing capability_token.cpp's `encode_root`/`encode_caveat` already use (ADR-005 §3.2's
// precedent) so no two distinct entry lists can ever collide onto the same byte string. Exposed as
// its own pure function -- independently testable without a store or a real digest algorithm --
// rather than inlined into `put_tree`, the same "small pure function, tested directly" shape
// ADR-011's `narrow_by_resource_limit` already established for this codebase.
[[nodiscard]] inline std::vector<std::byte> canonical_tree_bytes(Tree const& tree) {
    std::vector<std::byte> out;
    auto put_u32 = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    };
    auto put_str = [&](std::string const& s) {
        put_u32(static_cast<std::uint32_t>(s.size()));
        for (char c : s) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    };
    put_u32(static_cast<std::uint32_t>(tree.entries.size()));
    for (auto const& e : tree.entries) {
        put_str(e.name);
        put_str(e.digest);
        out.push_back(static_cast<std::byte>(e.is_tree ? 1 : 0));
    }
    return out;
}

// 025 §2's digest+store+tree contract. A concept, not a base class -- matches SandboxBackend/
// Runner/ChatClient's own established shape in this codebase. Return types are `result<T>`
// (synchronous) for the same reason every other seam header still is: `ae::task<T>` is not yet
// wired in project-wide (M2's own decision 2, unchanged).
template <class S>
concept WorktreeObjectStore =
    requires(S& s, std::span<std::byte const> bytes, Digest const& digest, Tree tree) {
        { s.put_blob(bytes) } -> std::same_as<result<Digest>>;
        { s.get_blob(digest) } -> std::same_as<result<std::vector<std::byte>>>;
        { s.put_tree(tree) } -> std::same_as<result<Digest>>;
        { s.get_tree(digest) } -> std::same_as<result<Tree>>;
    };

// The reference adapter -- in-memory, not durable across process exit. Exercises the full
// `WorktreeObjectStore` contract (dedup, tree-diff-by-digest-comparison, immutability) so it's
// directly testable; a crash-durable adapter (pal::file_io-backed, the same InMemory/File split
// Quark's own `Store` seam uses for a different shape of persistence) is a tracked follow-up, not
// built in this pass -- see docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md
// Phase A.
class InMemoryWorktreeObjectStore {
public:
    [[nodiscard]] result<Digest> put_blob(std::span<std::byte const> bytes) {
        auto digest = compute_digest(bytes);
        if (!digest) return std::unexpected(digest.error());
        blobs_.try_emplace(*digest, bytes.begin(), bytes.end());
        return *digest;
    }

    [[nodiscard]] result<std::vector<std::byte>> get_blob(Digest const& digest) const {
        auto it = blobs_.find(digest);
        if (it == blobs_.end()) {
            return std::unexpected(error{failure_class::contract,
                                          "no blob with this digest exists in the store",
                                          "worktree.blob_not_found"});
        }
        return it->second;
    }

    [[nodiscard]] result<Digest> put_tree(Tree tree) {
        std::ranges::sort(tree.entries, {}, &TreeEntry::name);
        auto digest = compute_digest(canonical_tree_bytes(tree));
        if (!digest) return std::unexpected(digest.error());
        trees_.insert_or_assign(*digest, std::move(tree));
        return *digest;
    }

    [[nodiscard]] result<Tree> get_tree(Digest const& digest) const {
        auto it = trees_.find(digest);
        if (it == trees_.end()) {
            return std::unexpected(error{failure_class::contract,
                                          "no tree with this digest exists in the store",
                                          "worktree.tree_not_found"});
        }
        return it->second;
    }

    // Test-only introspection (025 §2's "the same file written by two agents is stored once" is
    // only actually proven by watching this NOT grow on a duplicate put, not by digest equality
    // alone -- two equal digests could still coincidentally be backed by two map entries if a bug
    // used `insert` instead of dedup-on-key, so this is exercised directly rather than inferred).
    [[nodiscard]] std::size_t blob_count() const { return blobs_.size(); }
    [[nodiscard]] std::size_t tree_count() const { return trees_.size(); }

private:
    std::unordered_map<Digest, std::vector<std::byte>> blobs_;
    std::unordered_map<Digest, Tree>                   trees_;
};

static_assert(WorktreeObjectStore<InMemoryWorktreeObjectStore>);

} // namespace agentengine
