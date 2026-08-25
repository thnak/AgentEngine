#pragma once
// `shared`-mode staleness note (025 §3/§10 Q2) -- Phase B3. `shared` gives immediate cross-
// visibility (B1) with no merge step, which is exactly what makes it *safe* under 025 §4's single-
// writer serialization but still *confusing*: a sibling's write between an agent's reads is
// otherwise silent. §10 Q2's resolution is to give `shared` the same audit treatment §4 already
// gives merges -- "the writer changed 3 files" -- surfaced proactively at the start of a turn when
// the tree has moved, reusing one diff mechanism rather than inventing a second. Unlike
// worktree_merge.hpp's `merge_trees` (three-way, decides what wins), this is a two-way diff (old vs
// new, nothing to decide) -- a smaller, more general primitive that a three-way merge could be built
// from, not the other direction, so it is written standalone rather than as a special case of that
// algorithm.

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/sharing_mode.hpp"
#include "agentengine/core/worktree_sub.hpp"
#include "agentengine/core/worktree_types.hpp"
#include "agentengine/rt/append_log_store.hpp"

namespace agentengine {

// ae-naming-lint: allow TreeDiffKind — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class TreeDiffKind { added, removed, modified };

// One changed FILE (never a directory -- see `detail::collect_leaves_as` below): `path` is
// slash-joined from the tree root. A whole subdirectory added or removed is walked all the way
// down so each actual file inside it gets its own entry, matching "N files changed" being a count
// of files, not of the top-level names that happened to move.
// ae-naming-lint: allow TreeDiffEntry — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct TreeDiffEntry {
    std::string   path;
    TreeDiffKind  kind;
};

// ae-naming-lint: allow TreeDiff — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
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
// ae-naming-lint: allow SharedStalenessNote — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
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
template <WorktreeObjectStore OS, rt::AppendLogStore RS>
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

}  // namespace agentengine
