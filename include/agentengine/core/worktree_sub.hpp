#pragma once
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

#include <optional>
#include <string>

#include "agentengine/core/error.hpp"
#include "agentengine/core/sharing_mode.hpp"
#include "agentengine/core/worktree_ref_store.hpp"
#include "agentengine/core/worktree_types.hpp"
#include "agentengine/rt/append_log_store.hpp"

namespace agentengine {

// ae-naming-lint: allow SubWorktree — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct SubWorktree {
    std::string  name;              // this sub-worktree's own logical name
    std::string  backing_ref_name;  // the Ref actually read/written through; empty iff readonly
    sharing_mode mode = sharing_mode::branch;
    Digest       pinned_digest;     // meaningful only when mode == readonly
    Digest       base_digest;       // meaningful only when mode == branch: the common ancestor a
                                     // later three-way merge (worktree_merge.hpp) needs -- the
                                     // parent's tree digest at the moment this branch was created,
                                     // captured here since it is otherwise lost the instant the
                                     // parent or the branch moves again.
};

// The digest of a Tree with zero entries -- deterministic and universal (canonical_tree_bytes has
// no dependency on anything but the entries themselves), so `scratch` sub-worktrees always start
// at the identical digest rather than each minting their own "empty" representation.
[[nodiscard]] inline result<Digest> empty_tree_digest() {
    return compute_digest(canonical_tree_bytes(Tree{}));
}

template <rt::AppendLogStore S>
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
template <rt::AppendLogStore S>
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
template <rt::AppendLogStore S>
[[nodiscard]] result<Ref> write_sub_worktree(S& store, SubWorktree const& sub, Digest new_tree_digest) {
    if (sub.mode == sharing_mode::readonly) {
        return std::unexpected(error{failure_class::policy,
                                      "cannot write to a readonly sub-worktree",
                                      "worktree.readonly_write_rejected"});
    }
    return commit_ref(store, sub.backing_ref_name, std::move(new_tree_digest));
}

}  // namespace agentengine
