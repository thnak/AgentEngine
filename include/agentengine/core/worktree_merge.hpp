#pragma once
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

#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/sharing_mode.hpp"
#include "agentengine/core/worktree_ref_store.hpp"
#include "agentengine/core/worktree_sub.hpp"
#include "agentengine/core/worktree_types.hpp"
#include "agentengine/rt/append_log_store.hpp"

namespace agentengine {

// One genuine collision: `path` is slash-joined from the merge root (e.g. "a/b/c.txt"). Any of
// `base`/`ours`/`theirs` may be absent -- absent means "did not exist on that side" (an add/add
// divergence has no `base`; an edit/delete divergence has no `ours` or no `theirs`).
// ae-naming-lint: allow MergeConflict — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct MergeConflict {
    std::string               path;
    std::optional<TreeEntry>  base;
    std::optional<TreeEntry>  ours;
    std::optional<TreeEntry>  theirs;
};

// ae-naming-lint: allow MergeResult — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
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
// ae-naming-lint: allow BranchMergeOutcome — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
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
// full elimination needs the read-merge-commit sequence to run inside one serialized turn (025
// §4's "one writer per tree"), which this seam-level function -- called directly by a caller, not
// yet wrapped in one -- cannot itself guarantee. Phase B4 stress-proves this under concurrent
// load and, if the residual window between the recheck below and `commit_ref` proves reachable,
// drives a real fix (e.g. a compare-and-set primitive on `Store`) rather than living with this
// best-effort recheck indefinitely.
template <WorktreeObjectStore OS, rt::AppendLogStore RS>
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
template <WorktreeObjectStore OS, rt::AppendLogStore RS>
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

}  // namespace agentengine
