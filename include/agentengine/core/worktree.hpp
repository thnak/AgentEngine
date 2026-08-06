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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/trust/capability.hpp"
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

// ============================================================================================
// Sub-worktrees (025 §3) -- a session may run several agents, each on its own subtree with a
// declared `sharing_mode`. Modeled as a `SubWorktree` value naming which Ref (if any) reads/writes
// actually go through, rather than inventing a separate name -> name alias registry:
//   - `shared`   -- the SAME mutable tree as the parent. No new Ref is created; `backing_ref_name`
//                   is literally the parent's own Ref name, so immediate cross-visibility (025 §3)
//                   falls out for free at read/write time, not from special-case logic here.
//   - `branch`   -- copy-on-write. A new, independent Ref, seeded at the parent's CURRENT tree
//                   digest; it diverges from the parent on the next write to either side.
//   - `scratch`  -- a new, independent Ref, seeded at a fresh EMPTY tree (never copies the parent).
//   - `readonly` -- a pinned digest, not a Ref at all: `backing_ref_name` is empty, so a write has
//                   structurally nothing to commit against (`write_sub_worktree` fails closed on
//                   the mode directly, before ever touching the store).
// The caller always supplies `mode` explicitly -- 025 §3's "default is chosen by concurrency, not
// by taste" is a scheduling-layer decision (which agents are running concurrently), not something
// this header can infer from a Ref and a name alone, so it is deliberately not attempted here.
// ============================================================================================

struct SubWorktree {
    std::string  name;              // this sub-worktree's own logical name
    std::string  backing_ref_name;  // the Ref actually read/written through; empty iff readonly
    sharing_mode mode = sharing_mode::branch;
    Digest       pinned_digest;     // meaningful only when mode == readonly
    Digest       base_digest;       // meaningful only when mode == branch: the common ancestor a
                                     // later three-way merge (Phase B2, below) needs -- the parent's
                                     // tree digest at the moment this branch was created, captured
                                     // here since it is otherwise lost the instant the parent or the
                                     // branch moves again.
};

// The digest of a Tree with zero entries -- deterministic and universal (canonical_tree_bytes has
// no dependency on anything but the entries themselves), so `scratch` sub-worktrees always start
// at the identical digest rather than each minting their own "empty" representation.
[[nodiscard]] inline result<Digest> empty_tree_digest() {
    return compute_digest(canonical_tree_bytes(Tree{}));
}

template <quark::Store S>
[[nodiscard]] result<SubWorktree> create_sub_worktree(S& store, Ref const& parent,
                                                       std::string child_name, sharing_mode mode) {
    switch (mode) {
        case sharing_mode::shared:
            return SubWorktree{std::move(child_name), parent.name, mode, {}, {}};
        case sharing_mode::branch: {
            auto committed = commit_ref(store, child_name, parent.tree_digest);
            if (!committed) return std::unexpected(committed.error());
            return SubWorktree{std::move(child_name), committed->name, mode, {}, parent.tree_digest};
        }
        case sharing_mode::scratch: {
            auto empty_digest = empty_tree_digest();
            if (!empty_digest) return std::unexpected(empty_digest.error());
            auto committed = commit_ref(store, child_name, *empty_digest);
            if (!committed) return std::unexpected(committed.error());
            return SubWorktree{std::move(child_name), committed->name, mode, {}, {}};
        }
        case sharing_mode::readonly:
            return SubWorktree{std::move(child_name), {}, mode, parent.tree_digest, {}};
    }
    return std::unexpected(
        error{failure_class::contract, "unrecognized sharing_mode value", "worktree.unknown_sharing_mode"});
}

// Reads through `sub` exactly as 025 §3 defines each mode: `readonly` returns the digest pinned at
// creation time WITHOUT touching the store (so a later move of the parent's own Ref, or of any
// other Ref, can never leak into a readonly view); every other mode replays `backing_ref_name`'s
// own durable log, which for `shared` IS the parent's log.
template <quark::Store S>
[[nodiscard]] result<std::optional<Ref>> read_sub_worktree(S& store, SubWorktree const& sub) {
    if (sub.mode == sharing_mode::readonly) {
        return std::optional<Ref>{Ref{sub.name, sub.pinned_digest}};
    }
    return read_ref(store, sub.backing_ref_name);
}

// Commits a new tree digest through `sub`. `readonly` fails closed on the mode itself -- 025 §5's
// "writes rejected" -- before the store is ever consulted; every other mode commits to
// `backing_ref_name` exactly as a top-level `commit_ref` would (for `shared`, this literally IS a
// commit to the parent's own Ref, which is what makes the write immediately visible to every
// sibling reading through that same name).
template <quark::Store S>
[[nodiscard]] result<Ref> write_sub_worktree(S& store, SubWorktree const& sub, Digest new_tree_digest) {
    if (sub.mode == sharing_mode::readonly) {
        return std::unexpected(error{failure_class::policy,
                                      "cannot write to a readonly sub-worktree",
                                      "worktree.readonly_write_rejected"});
    }
    return commit_ref(store, sub.backing_ref_name, std::move(new_tree_digest));
}

// ============================================================================================
// Three-way merge on branch join (025 §4) -- Phase B2. A `branch` sub-worktree's tree is merged
// back against its own recorded ancestor (`SubWorktree::base_digest`) and the parent's CURRENT
// tree, entry by entry, recursing into subtrees so a change two levels deep in one branch and an
// unrelated change two levels deep in the parent never conflict at a shared ancestor directory --
// matching 025 §4's "disjoint changes merge automatically" literally, not just at the top level.
// Three outcomes per RFC wording, decided per name at each tree level:
//   - only one side changed a name from the ancestor (or both changed it identically, including
//     both deleting it)              -> merged automatically / trivially, no conflict;
//   - both sides changed the SAME name differently, and both sides still agree it is a tree
//     (a subdirectory)               -> recurse one level and let the disjoint/trivial cases above
//                                        resolve it there, only surfacing what genuinely collides;
//   - anything else two-sided (a real content fork, an add/add with different content, an
//     edit/delete fork, or a blob-vs-tree type fork) -> a `MergeConflict`, and 025 §4's explicit
//     rule: NEVER resolved by guessing or by last-writer-wins. `merge_trees` reports every conflict
//     across the whole tree in one pass (not just the first) so a caller can surface all of them at
//     once, matching "both versions retained" for every collision, not only the first found.
// ============================================================================================

// One genuine collision: `path` is slash-joined from the merge root (e.g. "a/b/c.txt"). Any of
// `base`/`ours`/`theirs` may be absent -- absent means "did not exist on that side" (an add/add
// divergence has no `base`; an edit/delete divergence has no `ours` or no `theirs`).
struct MergeConflict {
    std::string               path;
    std::optional<TreeEntry>  base;
    std::optional<TreeEntry>  ours;
    std::optional<TreeEntry>  theirs;
};

struct MergeResult {
    Digest                     merged_tree_digest;  // valid only when conflicts.empty()
    std::vector<MergeConflict> conflicts;

    [[nodiscard]] bool ok() const { return conflicts.empty(); }
};

namespace detail {

// Compares two same-name entries by what they point at, not by name (the name is already the map
// key both sides were looked up under) -- two `nullopt`s (absent on both sides) count as equal, the
// delete/delete case 025 §4 requires to merge trivially rather than surface as a conflict.
[[nodiscard]] inline bool tree_entries_equal(std::optional<TreeEntry> const& a,
                                              std::optional<TreeEntry> const& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a.has_value()) return true;
    return a->digest == b->digest && a->is_tree == b->is_tree;
}

// `base_digest` absent means "this subtree does not exist on the base side" -- treated as an empty
// Tree without a store lookup, since a hypothetical empty ancestor was never actually `put_tree`'d
// anywhere (unlike `ours_digest`/`theirs_digest`, which always name a real, previously-stored tree).
template <WorktreeObjectStore S>
[[nodiscard]] result<MergeResult> merge_subtrees(S& store, std::optional<Digest> const& base_digest,
                                                  Digest const& ours_digest, Digest const& theirs_digest,
                                                  std::string const& path_prefix) {
    // Fast paths matching 025 §4's "identical content -> trivially merged" and "disjoint changes ->
    // merged automatically" directly, without ever loading either Tree: neither side changed this
    // subtree relative to the other, or only one side changed it relative to the ancestor.
    if (ours_digest == theirs_digest) return MergeResult{ours_digest, {}};
    if (base_digest.has_value() && *base_digest == ours_digest) return MergeResult{theirs_digest, {}};
    if (base_digest.has_value() && *base_digest == theirs_digest) return MergeResult{ours_digest, {}};

    result<Tree> base_tree = base_digest.has_value() ? store.get_tree(*base_digest) : result<Tree>{Tree{}};
    if (!base_tree) return std::unexpected(base_tree.error());
    auto ours_tree = store.get_tree(ours_digest);
    if (!ours_tree) return std::unexpected(ours_tree.error());
    auto theirs_tree = store.get_tree(theirs_digest);
    if (!theirs_tree) return std::unexpected(theirs_tree.error());

    std::unordered_map<std::string, TreeEntry> base_by_name, ours_by_name, theirs_by_name;
    for (auto& e : base_tree->entries) base_by_name.emplace(e.name, e);
    for (auto& e : ours_tree->entries) ours_by_name.emplace(e.name, e);
    for (auto& e : theirs_tree->entries) theirs_by_name.emplace(e.name, e);

    std::set<std::string> all_names;  // sorted, so conflict order is deterministic across runs
    for (auto const& [n, _] : base_by_name) all_names.insert(n);
    for (auto const& [n, _] : ours_by_name) all_names.insert(n);
    for (auto const& [n, _] : theirs_by_name) all_names.insert(n);

    auto find_opt = [](std::unordered_map<std::string, TreeEntry> const& m,
                        std::string const& n) -> std::optional<TreeEntry> {
        auto it = m.find(n);
        if (it == m.end()) return std::nullopt;
        return it->second;
    };

    std::vector<TreeEntry>     merged_entries;
    std::vector<MergeConflict> conflicts;

    for (auto const& name : all_names) {
        auto b = find_opt(base_by_name, name);
        auto o = find_opt(ours_by_name, name);
        auto t = find_opt(theirs_by_name, name);

        if (tree_entries_equal(o, t)) {
            if (o.has_value()) merged_entries.push_back(*o);
            continue;
        }
        if (tree_entries_equal(o, b)) {
            if (t.has_value()) merged_entries.push_back(*t);
            continue;
        }
        if (tree_entries_equal(t, b)) {
            if (o.has_value()) merged_entries.push_back(*o);
            continue;
        }

        // Both sides changed `name` differently from the ancestor. If both still agree it's a
        // subtree, recurse and let that level's own disjoint/trivial cases absorb what they can --
        // only what genuinely collides down there is surfaced, keeping this level's result clean.
        if (o.has_value() && t.has_value() && o->is_tree && t->is_tree) {
            std::optional<Digest> sub_base =
                (b.has_value() && b->is_tree) ? std::optional<Digest>{b->digest} : std::nullopt;
            auto sub = merge_subtrees(store, sub_base, o->digest, t->digest, path_prefix + name + "/");
            if (!sub) return std::unexpected(sub.error());
            if (!sub->ok()) {
                conflicts.insert(conflicts.end(), std::make_move_iterator(sub->conflicts.begin()),
                                  std::make_move_iterator(sub->conflicts.end()));
            } else {
                merged_entries.push_back(TreeEntry{name, sub->merged_tree_digest, true});
            }
            continue;
        }

        // A real fork: divergent content, add/add with different content, an edit/delete split, or
        // a blob-vs-tree type collision. 025 §4: never guessed, never last-writer-wins.
        conflicts.push_back(MergeConflict{path_prefix + name, b, o, t});
    }

    if (!conflicts.empty()) return MergeResult{Digest{}, std::move(conflicts)};

    auto tree_digest = store.put_tree(Tree{std::move(merged_entries)});
    if (!tree_digest) return std::unexpected(tree_digest.error());
    return MergeResult{*tree_digest, {}};
}

} // namespace detail

// The public entry point: `base_digest`/`ours_digest`/`theirs_digest` all name real, already-stored
// trees (the caller always has these from an actual branch/parent Ref, never a hypothetical one).
template <WorktreeObjectStore S>
[[nodiscard]] result<MergeResult> merge_trees(S& store, Digest const& base_digest,
                                               Digest const& ours_digest, Digest const& theirs_digest) {
    return detail::merge_subtrees(store, std::optional<Digest>{base_digest}, ours_digest, theirs_digest, "");
}

// The outcome of merging one `branch` sub-worktree back into its parent: either the parent's Ref
// moved to the merged tree (`parent_ref` set), or the merge produced one or more conflicts and
// NOTHING was committed (025 §4: a failed merge must never partially apply).
struct BranchMergeOutcome {
    std::optional<Ref>         parent_ref;
    std::vector<MergeConflict> conflicts;

    [[nodiscard]] bool ok() const { return conflicts.empty(); }
};

// Merges `branch` (which must be `sharing_mode::branch`) back into `expected_parent`, which the
// caller must have read via `read_ref`/`read_sub_worktree` no earlier than "just before this call"
// -- its `tree_digest` is used as the merge's "theirs" input AND, immediately before committing,
// re-read live and compared against what the caller passed: if the parent moved in between (someone
// else merged first), this fails closed with `worktree.merge_stale_parent` rather than silently
// merging against a base that is no longer current. This narrows, but does not eliminate, the race:
// full elimination needs the read-merge-commit sequence to run inside one Quark-actor turn (025
// §4's "one writer per tree"), which this seam-level function -- called directly by a caller, not
// yet wrapped in an actor -- cannot itself guarantee. Phase B4 stress-proves this under concurrent
// load and, if the residual window between the recheck below and `commit_ref` proves reachable,
// drives a real fix (e.g. a compare-and-set primitive on `Store`) rather than living with this
// best-effort recheck indefinitely.
template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<BranchMergeOutcome> merge_branch_into_parent(OS& object_store, RS& ref_store,
                                                                   SubWorktree const& branch,
                                                                   Ref const& expected_parent) {
    if (branch.mode != sharing_mode::branch) {
        return std::unexpected(error{failure_class::contract,
                                      "merge_branch_into_parent requires a branch-mode sub-worktree",
                                      "worktree.merge_requires_branch_mode"});
    }

    auto branch_ref = read_ref(ref_store, branch.backing_ref_name);
    if (!branch_ref) return std::unexpected(branch_ref.error());
    if (!branch_ref->has_value()) {
        return std::unexpected(error{failure_class::contract,
                                      "branch sub-worktree's own ref has never been committed",
                                      "worktree.merge_branch_ref_missing"});
    }

    auto merged =
        merge_trees(object_store, branch.base_digest, (*branch_ref)->tree_digest, expected_parent.tree_digest);
    if (!merged) return std::unexpected(merged.error());
    if (!merged->ok()) {
        return BranchMergeOutcome{std::nullopt, std::move(merged->conflicts)};
    }

    auto live_parent = read_ref(ref_store, expected_parent.name);
    if (!live_parent) return std::unexpected(live_parent.error());
    if (!live_parent->has_value() || (*live_parent)->tree_digest != expected_parent.tree_digest) {
        return std::unexpected(error{failure_class::transient,
                                      "parent ref moved since the merge's expected base was observed; "
                                      "re-read and retry",
                                      "worktree.merge_stale_parent"});
    }

    auto committed = commit_ref(ref_store, expected_parent.name, merged->merged_tree_digest);
    if (!committed) return std::unexpected(committed.error());
    return BranchMergeOutcome{*committed, {}};
}

// The production-usable response to `merge_branch_into_parent`'s own error message ("re-read and
// retry"): tries the caller's already-observed `initial_expected_parent` first (the ordinary case --
// what a caller would already have from its last `read_sub_worktree`/turn-start read, at no extra
// cost), and only re-reads the parent live on a SUBSEQUENT attempt, specifically because the prior
// one was rejected as stale. A genuine merge conflict (`BranchMergeOutcome` with `conflicts` set) is
// NOT retried -- it is a real, terminal result, returned immediately like any other successful call;
// only `worktree.merge_stale_parent` drives another attempt. Phase B4's own concurrency proof is what
// exercises this under many simulated interleavings; this function is the mechanism, not the proof.
template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<BranchMergeOutcome> retry_merge_branch_into_parent(OS& object_store, RS& ref_store,
                                                                         SubWorktree const& branch,
                                                                         Ref initial_expected_parent,
                                                                         int max_attempts) {
    if (max_attempts < 1) {
        return std::unexpected(error{failure_class::contract, "max_attempts must be at least 1",
                                      "worktree.merge_retry_bad_max_attempts"});
    }
    std::string const parent_name = initial_expected_parent.name;
    std::optional<Ref> expected = std::move(initial_expected_parent);

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        if (!expected.has_value()) {
            auto fresh = read_ref(ref_store, parent_name);
            if (!fresh) return std::unexpected(fresh.error());
            if (!fresh->has_value()) {
                return std::unexpected(error{failure_class::contract,
                                              "retry_merge_branch_into_parent's parent ref has never "
                                              "been committed",
                                              "worktree.merge_parent_missing"});
            }
            expected = std::move(**fresh);
        }
        auto outcome = merge_branch_into_parent(object_store, ref_store, branch, *expected);
        if (outcome.has_value()) return outcome;
        if (outcome.error().code != "worktree.merge_stale_parent") return std::unexpected(outcome.error());
        expected.reset();  // rejected as stale -- force a fresh read on the next attempt
    }
    return std::unexpected(error{failure_class::resource,
                                  "exceeded max retry attempts merging under sustained contention",
                                  "worktree.merge_retries_exhausted"});
}

// ============================================================================================
// `shared`-mode staleness note (025 §3/§10 Q2) -- Phase B3. `shared` gives immediate cross-
// visibility (B1) with no merge step, which is exactly what makes it *safe* under 025 §4's single-
// writer serialization but still *confusing*: a sibling's write between an agent's reads is
// otherwise silent. §10 Q2's resolution is to give `shared` the same audit treatment §4 already
// gives merges -- "the writer changed 3 files" -- surfaced proactively at the start of a turn when
// the tree has moved, reusing one diff mechanism rather than inventing a second. Unlike B2's
// `merge_trees` (three-way, decides what wins), this is a two-way diff (old vs new, nothing to
// decide) -- a smaller, more general primitive that a three-way merge could be built from, not the
// other direction, so it is written standalone rather than as a special case of B2's algorithm.
// ============================================================================================

enum class TreeDiffKind { added, removed, modified };

// One changed FILE (never a directory -- see `detail::collect_leaves_as` below): `path` is
// slash-joined from the tree root. A whole subdirectory added or removed is walked all the way
// down so each actual file inside it gets its own entry, matching "N files changed" being a count
// of files, not of the top-level names that happened to move.
struct TreeDiffEntry {
    std::string   path;
    TreeDiffKind  kind;
};

struct TreeDiff {
    std::vector<TreeDiffEntry> changes;

    [[nodiscard]] std::size_t file_count() const { return changes.size(); }
};

namespace detail {

// Walks everything under `digest` and records each leaf (blob) it finds as `kind` -- used when one
// side of a diff has an entry the other side doesn't at all (a whole subtree added, or removed
// wholesale), and for the "type changed" fallback below.
template <WorktreeObjectStore S>
[[nodiscard]] result<void> collect_leaves_as(S& store, Digest const& digest, bool is_tree,
                                              std::string const& path, TreeDiffKind kind,
                                              std::vector<TreeDiffEntry>& out) {
    if (!is_tree) {
        out.push_back(TreeDiffEntry{path, kind});
        return {};
    }
    auto tree = store.get_tree(digest);
    if (!tree) return std::unexpected(tree.error());
    for (auto const& e : tree->entries) {
        auto r = collect_leaves_as(store, e.digest, e.is_tree, path + "/" + e.name, kind, out);
        if (!r) return r;
    }
    return {};
}

template <WorktreeObjectStore S>
[[nodiscard]] result<void> diff_subtrees(S& store, Digest const& old_digest, Digest const& new_digest,
                                          std::string const& path_prefix, std::vector<TreeDiffEntry>& out) {
    if (old_digest == new_digest) return {};

    auto old_tree = store.get_tree(old_digest);
    if (!old_tree) return std::unexpected(old_tree.error());
    auto new_tree = store.get_tree(new_digest);
    if (!new_tree) return std::unexpected(new_tree.error());

    std::unordered_map<std::string, TreeEntry> old_by_name, new_by_name;
    for (auto const& e : old_tree->entries) old_by_name.emplace(e.name, e);
    for (auto const& e : new_tree->entries) new_by_name.emplace(e.name, e);

    std::set<std::string> all_names;  // sorted, so the resulting diff order is deterministic
    for (auto const& [n, _] : old_by_name) all_names.insert(n);
    for (auto const& [n, _] : new_by_name) all_names.insert(n);

    for (auto const& name : all_names) {
        auto oit = old_by_name.find(name);
        auto nit = new_by_name.find(name);
        bool has_old = oit != old_by_name.end();
        bool has_new = nit != new_by_name.end();
        std::string path = path_prefix + name;

        if (has_old && !has_new) {
            auto r = collect_leaves_as(store, oit->second.digest, oit->second.is_tree, path,
                                        TreeDiffKind::removed, out);
            if (!r) return r;
            continue;
        }
        if (!has_old && has_new) {
            auto r = collect_leaves_as(store, nit->second.digest, nit->second.is_tree, path,
                                        TreeDiffKind::added, out);
            if (!r) return r;
            continue;
        }

        auto const& o = oit->second;
        auto const& n = nit->second;
        if (o.digest == n.digest && o.is_tree == n.is_tree) continue;  // unchanged

        if (o.is_tree && n.is_tree) {
            auto r = diff_subtrees(store, o.digest, n.digest, path + "/", out);
            if (!r) return r;
            continue;
        }
        if (!o.is_tree && !n.is_tree) {
            out.push_back(TreeDiffEntry{path, TreeDiffKind::modified});
            continue;
        }
        // A blob<->tree type change: report the old side as wholly removed and the new side as
        // wholly added, so a file that became a directory (or vice versa) is legible per-file
        // rather than collapsed into one misleading "modified" entry.
        auto removed = collect_leaves_as(store, o.digest, o.is_tree, path, TreeDiffKind::removed, out);
        if (!removed) return removed;
        auto added = collect_leaves_as(store, n.digest, n.is_tree, path, TreeDiffKind::added, out);
        if (!added) return added;
    }
    return {};
}

} // namespace detail

// Two-way file-level diff between `old_digest` and `new_digest` -- no ancestor, nothing to decide,
// unlike `merge_trees`.
template <WorktreeObjectStore S>
[[nodiscard]] result<TreeDiff> diff_trees(S& store, Digest const& old_digest, Digest const& new_digest) {
    TreeDiff diff;
    auto r = detail::diff_subtrees(store, old_digest, new_digest, "", diff.changes);
    if (!r) return std::unexpected(r.error());
    std::ranges::sort(diff.changes, {}, &TreeDiffEntry::path);
    return diff;
}

// A short, count-only line usable as model context ("the writer changed 3 files") -- deliberately
// NEVER includes a path or any content, matching 025 §4's own phrasing exactly: what changed, not
// what it changed to. `TreeDiff::changes` still carries full paths for a caller that needs them
// (an audit log, a human-facing detail view) -- only this summary stays content- and path-free.
// NOTE: the RFC's own example text also names the writer ("changed by `writer`"); this seam has no
// committer identity to attribute to -- `Ref`/`RefMoved` (025 §2, Phase A2) record only a tree
// digest, not who moved it -- so attribution is a named, tracked gap for whichever later phase
// threads an agent/run identity through a commit (a candidate for Phase D's turn-boundary work),
// not silently assumed here.
[[nodiscard]] inline std::string summarize_diff(TreeDiff const& diff) {
    if (diff.changes.empty()) return "no changes";

    std::size_t added = 0, removed = 0, modified = 0;
    for (auto const& c : diff.changes) {
        switch (c.kind) {
            case TreeDiffKind::added: ++added; break;
            case TreeDiffKind::removed: ++removed; break;
            case TreeDiffKind::modified: ++modified; break;
        }
    }

    std::string out = std::to_string(diff.changes.size()) + " file" +
                       (diff.changes.size() == 1 ? "" : "s") + " changed";
    std::vector<std::string> parts;
    if (modified) parts.push_back(std::to_string(modified) + " modified");
    if (added) parts.push_back(std::to_string(added) + " added");
    if (removed) parts.push_back(std::to_string(removed) + " removed");
    if (!parts.empty()) {
        out += " (";
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) out += ", ";
            out += parts[i];
        }
        out += ")";
    }
    return out;
}

// What an agent's turn opens with (025 §10 Q2): whether `shared` moved since `last_read_digest`
// (typically what the agent's own previous turn last observed via `read_sub_worktree`), and if so,
// the diff that moved it. `stale()` false and `diff.changes` empty both when nothing moved.
struct SharedStalenessNote {
    Digest    last_read_digest;
    Digest    current_digest;
    TreeDiff  diff;

    [[nodiscard]] bool stale() const { return last_read_digest != current_digest; }
};

// Computes the staleness note for `shared` (only `shared` has this hazard at all -- `branch`/
// `scratch` are private until an explicit merge, and `readonly` is pinned at creation, so neither
// can ever be "stale" in this sense; both fail closed here rather than silently returning an empty
// note that could be misread as "nothing changed").
template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<SharedStalenessNote> check_shared_staleness(OS& object_store, RS& ref_store,
                                                                  SubWorktree const& shared,
                                                                  Digest const& last_read_digest) {
    if (shared.mode != sharing_mode::shared) {
        return std::unexpected(error{failure_class::contract,
                                      "check_shared_staleness requires a shared-mode sub-worktree",
                                      "worktree.staleness_requires_shared_mode"});
    }

    auto current = read_sub_worktree(ref_store, shared);
    if (!current) return std::unexpected(current.error());
    if (!current->has_value()) {
        return std::unexpected(error{failure_class::contract,
                                      "shared sub-worktree's backing ref has never been committed",
                                      "worktree.staleness_ref_missing"});
    }
    Digest const& current_digest = (*current)->tree_digest;

    if (current_digest == last_read_digest) {
        return SharedStalenessNote{last_read_digest, current_digest, TreeDiff{}};
    }
    auto diff = diff_trees(object_store, last_read_digest, current_digest);
    if (!diff) return std::unexpected(diff.error());
    return SharedStalenessNote{last_read_digest, current_digest, std::move(*diff)};
}

// ============================================================================================
// Mounts (025 §5) -- Phase C1. "A worktree subtree becomes visible to a sandbox only through a
// capability": `Mount` is the host-side declaration of WHICH worktree location a guest-visible
// mount_id names (never guest-supplied, I2 -- a host policy value, the same posture every other
// `cap::*` payload in trust/capability.hpp already has), and `mount_read`/`mount_write` are the only
// way a guest-relative path is ever turned into an actual `Blob`/`Tree` lookup, gated by an already-
// bound `cap::FsRead`/`cap::FsWrite` (trust/capability.hpp, existing since ADR-009 -- this phase
// consumes that machinery, it does not invent a second capability shape).
//
// **This is NOT yet 025 §5's OS-level path-escape corpus** (Phase C2, ADR-track per the milestone-3
// breakdown's decision 6, `decisions/ADR-0NN-worktree-mount-path-canonicalization.md`). There is no
// real filesystem here: a `Tree` is a plain `name -> digest` map with no parent pointers, no
// symlinks, no junctions, no ADS, nothing an OS resolves -- so `..` has no "walk up" to perform, and
// `split_mount_path` below rejects it outright as malformed input rather than defending against it
// as an attack via canonicalize-then-check (the fragile pattern C2's whole corpus exists BECAUSE
// canonicalize-then-check is where real path-escape bugs hide). C2 hardens the DIFFERENT, later
// mechanism that materializes a mount onto a real OS filesystem for a sandboxed process to see --
// that mechanism doesn't exist yet, and this header does not get ahead of it.
// ============================================================================================

// Host-side binding from a guest-visible `mount_id` (matched against `cap::FsRead::mount_id` /
// `cap::FsWrite::mount_id` exactly) to a concrete worktree location: `ref_name` names the Ref this
// mount reads/writes through, `subtree_path` (slash-joined, "" = the ref's own root) is where within
// that ref's tree this specific mount is rooted -- e.g. a "/work" mount and an "/input" mount might
// point at the SAME ref with different `subtree_path`s, or at entirely different refs. Never derived
// from a guest-supplied path (I2); constructed only by host policy, the same way a `Capability` is
// only ever minted by `CapabilitySet::grant_root`.
struct Mount {
    std::string mount_id;
    std::string ref_name;
    std::string subtree_path;
};

// Splits a slash-joined relative path into segments, rejecting `""`-as-a-segment (a leading `/`, a
// trailing `/`, or `//`), and `.`/`..` (meaningless in a content-addressed tree -- see the section
// comment above). `path == ""` is a legal INPUT meaning "the mount's own root" and returns an empty
// segment list; callers that cannot accept "the root itself" (both `mount_read` and `mount_write`
// below, since neither reads/writes raw tree-of-trees content) reject an empty combined segment list
// themselves, one level up, where the more specific `worktree.mount_path_is_root` code applies.
[[nodiscard]] inline result<std::vector<std::string>> split_mount_path(std::string const& path) {
    if (path.empty()) return std::vector<std::string>{};
    if (path.front() == '/') {
        return std::unexpected(error{failure_class::contract, "mount path must be relative, not start with '/'",
                                      "worktree.mount_path_absolute"});
    }
    std::vector<std::string> segments;
    std::string              current;
    for (char c : path) {
        if (c != '/') {
            current.push_back(c);
            continue;
        }
        if (current.empty()) {
            return std::unexpected(error{failure_class::contract,
                                          "mount path contains an empty segment (e.g. a double slash)",
                                          "worktree.mount_path_malformed"});
        }
        segments.push_back(std::exchange(current, std::string{}));
    }
    if (current.empty()) {
        return std::unexpected(error{failure_class::contract, "mount path must not end with '/'",
                                      "worktree.mount_path_malformed"});
    }
    segments.push_back(current);
    for (auto const& seg : segments) {
        if (seg == "." || seg == "..") {
            return std::unexpected(error{failure_class::contract,
                                          "'.'/'..' are not meaningful in a content-addressed tree path",
                                          "worktree.mount_path_malformed"});
        }
    }
    return segments;
}

namespace detail {

// A Tree with zero entries, guaranteed to actually EXIST in `store` (unlike bare
// `empty_tree_digest()`, which only computes what the digest would be) -- needed here because
// `set_entry_at_path` below may need to recurse INTO a freshly-created intermediate directory that
// nothing has ever `put_tree`'d before. Idempotent: `put_tree` already dedups, so calling this
// repeatedly across many writes never grows the store past one empty-tree entry.
template <WorktreeObjectStore S>
[[nodiscard]] result<Digest> ensure_empty_tree(S& store) {
    return store.put_tree(Tree{});
}

// Walks `segments` down from `tree_digest`, returning the `TreeEntry` found at the end. Every
// intermediate segment must resolve to a Tree (a file "in the middle" of a path is a contract
// violation, not a valid deeper lookup).
template <WorktreeObjectStore S>
[[nodiscard]] result<TreeEntry> resolve_entry_at_path(S& store, Digest const& tree_digest,
                                                       std::span<std::string const> segments) {
    auto tree = store.get_tree(tree_digest);
    if (!tree) return std::unexpected(tree.error());

    std::string const& head = segments.front();
    auto it = std::ranges::find_if(tree->entries, [&](TreeEntry const& e) { return e.name == head; });
    if (it == tree->entries.end()) {
        return std::unexpected(error{failure_class::contract, "no entry named '" + head + "' at this path",
                                      "worktree.mount_path_not_found"});
    }
    if (segments.size() == 1) return *it;
    if (!it->is_tree) {
        return std::unexpected(error{failure_class::contract, "'" + head + "' is a file, not a directory",
                                      "worktree.mount_path_not_a_directory"});
    }
    return resolve_entry_at_path(store, it->digest, segments.subspan(1));
}

// Sets (inserts or replaces) the entry named by the LAST segment of `segments` to
// `{leaf_digest, leaf_is_tree}`, creating any missing intermediate directories along the way, and
// returns the digest of the (necessarily new) tree at `tree_digest`'s own level -- the caller
// commits that returned digest as the new Ref, propagating the change all the way to the root in
// one recursive unwind rather than needing a separate "patch the ancestors" pass.
template <WorktreeObjectStore S>
[[nodiscard]] result<Digest> set_entry_at_path(S& store, Digest const& tree_digest,
                                                std::span<std::string const> segments,
                                                Digest const& leaf_digest, bool leaf_is_tree) {
    auto tree = store.get_tree(tree_digest);
    if (!tree) return std::unexpected(tree.error());
    std::vector<TreeEntry> entries = tree->entries;

    std::string const& head = segments.front();
    auto it = std::ranges::find_if(entries, [&](TreeEntry const& e) { return e.name == head; });

    TreeEntry new_entry;
    if (segments.size() == 1) {
        new_entry = TreeEntry{head, leaf_digest, leaf_is_tree};
    } else {
        Digest child_digest;
        if (it != entries.end()) {
            if (!it->is_tree) {
                return std::unexpected(error{failure_class::contract,
                                              "cannot write through '" + head +
                                                  "': it already names a file, not a directory",
                                              "worktree.mount_write_type_conflict"});
            }
            child_digest = it->digest;
        } else {
            auto empty = ensure_empty_tree(store);
            if (!empty) return std::unexpected(empty.error());
            child_digest = *empty;
        }
        auto new_child = set_entry_at_path(store, child_digest, segments.subspan(1), leaf_digest, leaf_is_tree);
        if (!new_child) return std::unexpected(new_child.error());
        new_entry = TreeEntry{head, *new_child, true};
    }

    if (it != entries.end()) *it = new_entry; else entries.push_back(new_entry);
    return store.put_tree(Tree{std::move(entries)});
}

// Resolves `subtree_path` (a Mount's fixed, host-configured root within its ref) down from
// `root_digest` to the Digest of the Tree living there -- `""` means the mount is rooted at the
// ref's own root, returned as-is. Used by write-quota enforcement (025 §5, Milestone 3 Phase C3) to
// find exactly the subtree a quota is scoped to, never the whole ref (a ref may host several mounts
// with independent quotas via different `subtree_path`s -- 025 §5's own example, `/work` and
// `/input` on the same ref).
template <WorktreeObjectStore S>
[[nodiscard]] result<Digest> resolve_subtree_digest(S& store, Digest const& root_digest,
                                                      std::string const& subtree_path) {
    if (subtree_path.empty()) return root_digest;
    auto segments = split_mount_path(subtree_path);
    if (!segments) return std::unexpected(segments.error());
    if (segments->empty()) return root_digest;
    auto entry = resolve_entry_at_path(store, root_digest, *segments);
    if (!entry) return std::unexpected(entry.error());
    if (!entry->is_tree) {
        return std::unexpected(error{failure_class::fatal, "mount subtree path does not name a directory",
                                      "worktree.mount_subtree_not_a_directory"});
    }
    return entry->digest;
}

// Total bytes (sum of every reachable Blob's size) and total file count (every reachable Blob leaf,
// recursively) under `tree_digest` -- what a write-quota check (025 §5) means by "this mount's
// current usage". Recomputed from the tree on every write rather than tracked as separate running
// state: the content-addressed store has no side state to keep in sync by construction, and this
// keeps the quota check correct-by-construction against whatever the tree actually contains (e.g. a
// write that REPLACES a large file with a small one must show reduced usage, not accumulate a
// stale delta) at the cost of walking the subtree on every write -- a named, accepted cost for a
// milestone whose own gates (025 §9 G4) defer real p99 cost measurement past M3 project-wide.
template <WorktreeObjectStore S>
[[nodiscard]] result<std::pair<std::uint64_t, std::uint32_t>> subtree_usage(S& store, Digest const& tree_digest) {
    auto tree = store.get_tree(tree_digest);
    if (!tree) return std::unexpected(tree.error());
    std::uint64_t bytes = 0;
    std::uint32_t files = 0;
    for (auto const& entry : tree->entries) {
        if (entry.is_tree) {
            auto sub = subtree_usage(store, entry.digest);
            if (!sub) return std::unexpected(sub.error());
            bytes += sub->first;
            files += sub->second;
        } else {
            auto blob = store.get_blob(entry.digest);
            if (!blob) return std::unexpected(blob.error());
            bytes += blob->size();
            ++files;
        }
    }
    return std::make_pair(bytes, files);
}

// `mount.subtree_path` (host-configured, fixed) followed by `guest_path` (per-call), as one combined
// segment list resolved in a single walk from the Ref's own root -- rather than descending
// `subtree_path` and `guest_path` as two separate phases, which would need to reconcile two
// intermediate digests instead of one.
[[nodiscard]] inline result<std::vector<std::string>> combined_mount_segments(Mount const& mount,
                                                                               std::string const& guest_path) {
    std::vector<std::string> full_segments;
    if (!mount.subtree_path.empty()) {
        auto sub = split_mount_path(mount.subtree_path);
        if (!sub) return std::unexpected(sub.error());
        full_segments = std::move(*sub);
    }
    auto guest = split_mount_path(guest_path);
    if (!guest) return std::unexpected(guest.error());
    full_segments.insert(full_segments.end(), guest->begin(), guest->end());
    if (full_segments.empty()) {
        return std::unexpected(error{failure_class::contract,
                                      "path names the mount root itself, not a file",
                                      "worktree.mount_path_is_root"});
    }
    return full_segments;
}

} // namespace detail

// Reads the file at `guest_path` (relative to `mount`) through `granted`, the caller's already-bound
// `cap::FsRead`. Two independent checks come before any store access at all (007 §3 -- capability
// grants ARE the authority, checked before the effect, not after): `granted.mount_id` must name
// exactly this `mount` (a capability for one mount can never reach another, even by accident), and
// `granted.path_prefix` -- reusing `capability_detail::path_prefix_covers`, the SAME subsumption
// primitive `CapabilitySet::attenuate`/`contains` already use, not a second path-matching routine
// invented here -- must cover `guest_path` (a capability scoped to `/work/output` can't read
// `/work/private`). `granted.size_cap_bytes`, if set, is enforced after the read (the size is only
// known once the blob is fetched).
template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<std::vector<std::byte>> mount_read(OS& object_store, RS& ref_store, Mount const& mount,
                                                          cap::FsRead const& granted,
                                                          std::string const& guest_path) {
    if (granted.mount_id != mount.mount_id) {
        return std::unexpected(error{failure_class::policy,
                                      "this capability does not authorize the requested mount",
                                      "worktree.mount_capability_mismatch"});
    }
    if (!capability_detail::path_prefix_covers(granted.path_prefix, guest_path)) {
        return std::unexpected(error{failure_class::policy,
                                      "this capability's path scope does not cover the requested path",
                                      "worktree.mount_path_outside_capability"});
    }

    auto full_segments = detail::combined_mount_segments(mount, guest_path);
    if (!full_segments) return std::unexpected(full_segments.error());

    auto ref = read_ref(ref_store, mount.ref_name);
    if (!ref) return std::unexpected(ref.error());
    if (!ref->has_value()) {
        return std::unexpected(error{failure_class::contract, "this mount's ref has never been committed",
                                      "worktree.mount_ref_missing"});
    }

    auto entry = detail::resolve_entry_at_path(object_store, (*ref)->tree_digest, *full_segments);
    if (!entry) return std::unexpected(entry.error());
    if (entry->is_tree) {
        return std::unexpected(error{failure_class::contract, "the requested path names a directory, not a file",
                                      "worktree.mount_read_is_directory"});
    }

    auto bytes = object_store.get_blob(entry->digest);
    if (!bytes) return std::unexpected(bytes.error());
    if (granted.size_cap_bytes.has_value() && bytes->size() > *granted.size_cap_bytes) {
        return std::unexpected(error{failure_class::policy,
                                      "the requested file exceeds this capability's size cap",
                                      "worktree.mount_read_exceeds_size_cap"});
    }
    return bytes;
}

// Writes `content` to `guest_path` (relative to `mount`) through `granted`, the caller's already-
// bound `cap::FsWrite`, returning the mount's Ref after the commit. Same two capability checks as
// `mount_read` before any store access. **`granted.quota_bytes`/`granted.file_count_cap` ARE
// enforced here** (025 §5, Milestone 3 Phase C3): the candidate new tree is built first, then this
// mount's subtree usage is recomputed against it (`detail::subtree_usage`, scoped to
// `mount.subtree_path` via `detail::resolve_subtree_digest` -- never the whole ref, since a ref may
// host several independently-quota'd mounts) -- and the Ref is committed ONLY if usage stays within
// both caps. A write that would exceed either cap is rejected before `commit_ref` ever runs, same
// fail-closed shape 025 §5's other checks already use: the guest never observes a state where the
// Ref moved and THEN the quota was found to be exceeded. `std::nullopt` on either cap field means
// uncapped (the same convention `trust/capability.hpp`'s own header comment already documents for
// every `cap::*` limit field) -- an omitted cap is never treated as "0" or silently skipped.
//
// Error framing matches 026 §3's mapping table exactly ("Quota exhausted -> OSError (No space left
// on device)"): a `failure_class::resource` error with that literal message, so a future guest-
// facing translator (Phase E's `PythonRunner`/`ShellRunner`) has an ordinary OS-shaped message ready
// to raise, not a policy identifier to reword.
template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<Ref> mount_write(OS& object_store, RS& ref_store, Mount const& mount,
                                       cap::FsWrite const& granted, std::string const& guest_path,
                                       std::span<std::byte const> content) {
    if (granted.mount_id != mount.mount_id) {
        return std::unexpected(error{failure_class::policy,
                                      "this capability does not authorize the requested mount",
                                      "worktree.mount_capability_mismatch"});
    }
    if (!capability_detail::path_prefix_covers(granted.path_prefix, guest_path)) {
        return std::unexpected(error{failure_class::policy,
                                      "this capability's path scope does not cover the requested path",
                                      "worktree.mount_path_outside_capability"});
    }

    auto full_segments = detail::combined_mount_segments(mount, guest_path);
    if (!full_segments) return std::unexpected(full_segments.error());

    auto ref = read_ref(ref_store, mount.ref_name);
    if (!ref) return std::unexpected(ref.error());
    if (!ref->has_value()) {
        return std::unexpected(error{failure_class::contract, "this mount's ref has never been committed",
                                      "worktree.mount_ref_missing"});
    }

    auto blob_digest = object_store.put_blob(content);
    if (!blob_digest) return std::unexpected(blob_digest.error());

    auto new_root = detail::set_entry_at_path(object_store, (*ref)->tree_digest, *full_segments,
                                               *blob_digest, /*leaf_is_tree=*/false);
    if (!new_root) return std::unexpected(new_root.error());

    if (granted.quota_bytes.has_value() || granted.file_count_cap.has_value()) {
        auto subtree_digest = detail::resolve_subtree_digest(object_store, *new_root, mount.subtree_path);
        if (!subtree_digest) return std::unexpected(subtree_digest.error());
        auto usage = detail::subtree_usage(object_store, *subtree_digest);
        if (!usage) return std::unexpected(usage.error());
        if (granted.quota_bytes.has_value() && usage->first > *granted.quota_bytes) {
            return std::unexpected(
                error{failure_class::resource, "No space left on device", "worktree.mount_write_quota_exceeded"});
        }
        if (granted.file_count_cap.has_value() && usage->second > *granted.file_count_cap) {
            return std::unexpected(error{failure_class::resource, "No space left on device",
                                          "worktree.mount_write_file_count_exceeded"});
        }
    }

    return commit_ref(ref_store, mount.ref_name, *new_root);
}

} // namespace agentengine
