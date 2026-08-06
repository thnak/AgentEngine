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
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "quark/core/describe.hpp"
#include "quark/core/event_log.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/snapshot.hpp"

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

// ============================================================================================
// Ref persistence (025 §2's "a mutable name -> Tree digest") -- ordinary Quark-actor state, going
// through Quark's `Store` seam directly rather than this header's own object store (see the
// file-top comment and docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md
// decision 1). Modeled as EventSourced (012), not snapshot-only: an append-only log of `RefMoved`
// events gives a Ref its own history for free -- each committed digest is a retained, replayable
// log entry -- which Phase D's turn-boundary commit / rewind tasks (D1/D2) build on directly
// rather than needing a separate per-turn digest ledger invented from scratch.
// ============================================================================================

// One committed Ref update: the tree it now points at. `QUARK_SERIALIZE` must sit in the same
// namespace as the type so the generated `quark_describe` is found by ADL (the same rule Quark's
// own persistence tests document).
struct RefMoved {
    Digest tree_digest;
};
QUARK_SERIALIZE(RefMoved, (1, tree_digest))

// The folded state: just the latest digest. The number of moves is deliberately not duplicated
// here -- `EventLog::commit()`'s own return value, and a recovered `RecoveredState::last_seq`,
// already answer "how many," so there is nothing this struct needs to track redundantly.
struct RefState {
    Digest tree_digest;
};
QUARK_SERIALIZE(RefState, (1, tree_digest))

inline void apply_ref_moved(RefState& state, RefMoved const& event) {
    state.tree_digest = event.tree_digest;
}

namespace detail {
// `quark::error` (`errc` + a borrowed `string_view`) and `agentengine::error` (`failure_class` +
// owned `std::string` + a stable code) are two different error vocabularies -- Quark's `Store`
// returns the former, every seam header in this codebase returns the latter (core/error.hpp). This
// is the one place that boundary is crossed for the Ref persistence functions below. `errc::
// unavailable` specifically means 012's fencing rejection (a superseded writer) -- mapped to
// `contract`, not `transient`: retrying with the SAME stale fence can never succeed, only
// acquiring a fresh one can, which is a caller-contract fact, not a "try again later" one.
[[nodiscard]] inline error from_quark_error(quark::error const& e, std::string_view code) {
    failure_class klass = failure_class::fatal;
    switch (e.code) {
        case quark::errc::unavailable:
        case quark::errc::validation:
        case quark::errc::serialization:
        case quark::errc::not_found:
            klass = failure_class::contract;
            break;
        case quark::errc::timeout:
        case quark::errc::overloaded:
        case quark::errc::circuit_open:
            klass = failure_class::resource;
            break;
        case quark::errc::cancelled:
            klass = failure_class::transient;
            break;
        default:
            klass = failure_class::fatal;
            break;
    }
    return error{klass, std::string(e.detail), std::string(code)};
}
} // namespace detail

// The stable ActorId a Ref's human-readable `name` (e.g. "session:s-42") maps to: `RefState`'s own
// 016 fingerprint as the type tag, plus a hash of `name` as the instance key -- Quark's `ActorId`
// key is a `std::uint64_t`, not a string (ids.hpp), so this is the one place that gap is bridged.
[[nodiscard]] inline quark::ActorId ref_actor_id(std::string_view name) noexcept {
    return quark::ActorId{quark::durable_type_key<RefState>(), std::hash<std::string_view>{}(name)};
}

// Mint or move a Ref: acquires a fresh fence for `name`'s ActorId, appends one `RefMoved` event
// under it, and returns the resulting `Ref`. There is no separate "mint" vs "update" entry point --
// EventSourced append doesn't need one, since the fence and the store's own strict-seq-
// monotonicity (012) already make a first commit and a later commit the same call.
template <quark::Store S>
[[nodiscard]] result<Ref> commit_ref(S& store, std::string name, Digest tree_digest) {
    auto const id = ref_actor_id(name);
    auto const fence = store.acquire_fence(id);
    quark::EventLog<RefMoved, S> log(store, id, fence, store.last_seq(id) + 1);
    log.stage(RefMoved{tree_digest});
    auto committed = log.commit();
    if (!committed) {
        return std::unexpected(detail::from_quark_error(committed.error(), "worktree.ref_commit_failed"));
    }
    return Ref{std::move(name), std::move(tree_digest)};
}

// Read a Ref's current state by replaying its durable log -- a fresh process, a restart, or a node
// migration all reach the identical state this way, which is 025 §9 G1's mechanism in miniature.
// `nullopt` when `name` has never been committed (no snapshot, no log entries).
template <quark::Store S>
[[nodiscard]] result<std::optional<Ref>> read_ref(S& store, std::string name) {
    auto const id = ref_actor_id(name);
    auto rec = quark::recover_event_sourced<RefState, RefMoved>(store, id, RefState{}, apply_ref_moved);
    if (!rec) {
        return std::unexpected(detail::from_quark_error(rec.error(), "worktree.ref_read_failed"));
    }
    if (rec->last_seq == 0) return std::optional<Ref>{};  // never committed
    return std::optional<Ref>{Ref{std::move(name), rec->state.tree_digest}};
}

} // namespace agentengine
