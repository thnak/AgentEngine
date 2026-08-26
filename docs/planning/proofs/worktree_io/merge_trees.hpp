#pragma once
// PROVE-PHASE PROBE: a REAL three-way merge algorithm -- closes §11's "MergeStrategy is named, not
// designed" gap and §23/§26's own trivial "parent adopts the child's head wholesale" stub. Standard
// three-way-merge semantics (the same shape git/most VCS merges use): for every path across
// base/ours/theirs, only a path BOTH sides changed to DIFFERENT values is a real conflict; a path
// only one side touched takes that side's value; a path both sides changed to the IDENTICAL value is
// not a conflict either.

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/worktree_types.hpp"

namespace probe {

struct MergeConflict {
    std::string path;
    std::optional<agentengine::Digest> base_digest;
    agentengine::Digest ours_digest;
    agentengine::Digest theirs_digest;
};

struct MergeResult {
    agentengine::Tree merged;
    std::vector<MergeConflict> conflicts;   // non-empty means `merged` is NOT authoritative for the
                                              // conflicting paths -- a real caller must resolve them
                                              // before treating `merged` as the real outcome; this
                                              // probe's own test proves both the clean-merge and the
                                              // conflict-detection paths, not just the happy path
};

namespace detail {
[[nodiscard]] inline std::unordered_map<std::string, agentengine::TreeEntry> index_by_path(
    agentengine::Tree const& t) {
    std::unordered_map<std::string, agentengine::TreeEntry> out;
    for (auto const& e : t.entries) out.emplace(e.name, e);
    return out;
}
}  // namespace detail

[[nodiscard]] inline MergeResult merge_trees(agentengine::Tree const& base, agentengine::Tree const& ours,
                                                agentengine::Tree const& theirs) {
    auto base_idx = detail::index_by_path(base);
    auto ours_idx = detail::index_by_path(ours);
    auto theirs_idx = detail::index_by_path(theirs);

    std::vector<std::string> all_paths;
    for (auto const& [k, v] : base_idx) all_paths.push_back(k);
    for (auto const& [k, v] : ours_idx) all_paths.push_back(k);
    for (auto const& [k, v] : theirs_idx) all_paths.push_back(k);
    std::sort(all_paths.begin(), all_paths.end());
    all_paths.erase(std::unique(all_paths.begin(), all_paths.end()), all_paths.end());

    MergeResult result;
    for (auto const& path : all_paths) {
        auto b = base_idx.find(path);
        auto o = ours_idx.find(path);
        auto t = theirs_idx.find(path);
        bool has_b = b != base_idx.end(), has_o = o != ours_idx.end(), has_t = t != theirs_idx.end();

        // Both sides agree (including "both deleted it", or "both added it identically" when base
        // never had this path at all) -- take that (possibly absent) value.
        bool const o_eq_t = (has_o == has_t) && (!has_o || o->second.digest == t->second.digest);
        if (o_eq_t) {
            if (has_o) result.merged.entries.push_back(o->second);
            continue;
        }

        // "Unchanged relative to base" must account for PRESENCE, not just digest equality when
        // present -- a path absent from base that one side newly ADDS is "changed" even though
        // there is no base entry to compare a digest against. The bug this fixes (found by this
        // probe's own first real run, not by inspection): the original version only compared
        // digests when `has_b` was true, so a brand-new path added by exactly one side (never in
        // base at all) fell through to "neither side matches base" and was wrongly reported as a
        // conflict, even though the other side never touched that path.
        bool const ours_matches_base =
            (has_o == has_b) && (!has_o || (has_b && o->second.digest == b->second.digest));
        bool const theirs_matches_base =
            (has_t == has_b) && (!has_t || (has_b && t->second.digest == b->second.digest));

        if (ours_matches_base && !theirs_matches_base) {
            // Ours left this path exactly as base had it (or never had it, matching base's own
            // absence) -- theirs is the side that actually changed something -- take theirs.
            if (has_t) result.merged.entries.push_back(t->second);
            continue;
        }
        if (theirs_matches_base && !ours_matches_base) {
            // Symmetric: theirs matches base, ours is the side that changed something -- take ours.
            if (has_o) result.merged.entries.push_back(o->second);
            continue;
        }

        // Both sides changed this path, to DIFFERENT values, and neither matches base (or the path
        // is new on both sides with different content, or one deleted while the other modified) --
        // a REAL conflict. Recorded, not silently resolved by picking a side.
        result.conflicts.push_back(MergeConflict{
            path, has_b ? std::optional<agentengine::Digest>(b->second.digest) : std::nullopt,
            has_o ? o->second.digest : agentengine::Digest{"<deleted>"},
            has_t ? t->second.digest : agentengine::Digest{"<deleted>"}});
        // A real caller decides resolution; this function still includes OURS in `merged` as a
        // working default so `merged` remains a structurally valid Tree even with conflicts present
        // (matching git's own "conflicted files still exist in the working tree, marked" posture) --
        // but callers MUST check `conflicts.empty()` before trusting `merged` as final.
        if (has_o) result.merged.entries.push_back(o->second);
    }

    std::ranges::sort(result.merged.entries, {}, &agentengine::TreeEntry::name);
    return result;
}

}  // namespace probe
