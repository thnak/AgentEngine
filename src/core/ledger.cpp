// Implements ADR-102 Phase 2 (the identity-scoped checkpoint/branch Ledger); ADR-207 (#120 S7).
// The non-template bodies of include/agentengine/core/ledger.hpp (merge_trees() and its helpers), moved verbatim out
// of the header, and the explicit instantiations of Ledger<Store> for the two object stores the codebase has:
// InMemoryWorktreeObjectStore (the default) and FileWorktreeObjectStore. Ledger's member bodies are in
// core/ledger_impl.hpp; instantiating them here means no file that includes ledger.hpp compiles them again.

#include "agentengine/core/ledger.hpp"

#include "agentengine/core/file_worktree_object_store.hpp"
#include "agentengine/core/ledger_impl.hpp"

namespace agentengine {
namespace ledger_detail {

std::optional<std::pair<std::size_t, std::size_t>> find_case_folding_collision(
    agentengine::Tree const& tree) {
    std::vector<std::pair<std::string, std::size_t>> folded;
    folded.reserve(tree.entries.size());
    for (std::size_t i = 0; i < tree.entries.size(); ++i) {
        std::string f = tree.entries[i].name;
        for (char& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        folded.emplace_back(std::move(f), i);
    }
    std::sort(folded.begin(), folded.end());  // by folded name, then by index within equal folds

    std::optional<std::pair<std::size_t, std::size_t>> best;
    for (std::size_t run = 0; run < folded.size();) {
        std::size_t run_end = run + 1;
        while (run_end < folded.size() && folded[run_end].first == folded[run].first) ++run_end;
        std::size_t const i = folded[run].second;
        if (!best.has_value() || i < best->first) {
            std::string const& first_name = tree.entries[i].name;
            for (std::size_t k = run + 1; k < run_end; ++k) {
                std::size_t const j = folded[k].second;
                if (tree.entries[j].name != first_name) {
                    best = std::pair{i, j};
                    break;
                }
            }
        }
        run = run_end;
    }
    return best;
}

}  // namespace ledger_detail
}  // namespace agentengine

namespace agentengine {

LedgerMergeResult merge_trees(agentengine::Tree const& base, agentengine::Tree const& ours,
                              agentengine::Tree const& theirs) {
    auto base_idx = ledger_detail::index_by_path(base);
    auto ours_idx = ledger_detail::index_by_path(ours);
    auto theirs_idx = ledger_detail::index_by_path(theirs);

    std::vector<std::string> all_paths;
    for (auto const& [k, v] : base_idx) all_paths.push_back(k);
    for (auto const& [k, v] : ours_idx) all_paths.push_back(k);
    for (auto const& [k, v] : theirs_idx) all_paths.push_back(k);
    std::sort(all_paths.begin(), all_paths.end());
    all_paths.erase(std::unique(all_paths.begin(), all_paths.end()), all_paths.end());

    LedgerMergeResult result;
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
        // present -- a path absent from base that one side newly ADDS is "changed" even though there
        // is no base entry to compare a digest against.
        bool const ours_matches_base =
            (has_o == has_b) && (!has_o || (has_b && o->second.digest == b->second.digest));
        bool const theirs_matches_base =
            (has_t == has_b) && (!has_t || (has_b && t->second.digest == b->second.digest));

        if (ours_matches_base && !theirs_matches_base) {
            // Ours left this path exactly as base had it (or never had it) -- theirs is the side
            // that actually changed something -- take theirs.
            if (has_t) result.merged.entries.push_back(t->second);
            continue;
        }
        if (theirs_matches_base && !ours_matches_base) {
            // Symmetric: theirs matches base, ours is the side that changed something -- take ours.
            if (has_o) result.merged.entries.push_back(o->second);
            continue;
        }

        // Both sides changed this path, to DIFFERENT values, and neither matches base -- a REAL
        // conflict. Recorded, not silently resolved by picking a side.
        result.conflicts.push_back(LedgerMergeConflict{
            path, has_b ? std::optional<agentengine::Digest>(b->second.digest) : std::nullopt,
            has_o ? o->second.digest : agentengine::Digest{"<deleted>"},
            has_t ? t->second.digest : agentengine::Digest{"<deleted>"}});
        // A real caller decides resolution; this function still includes OURS in `merged` as a
        // working default so `merged` remains a structurally valid Tree even with conflicts present
        // -- callers MUST check `conflicts.empty()` before trusting `merged` as final.
        if (has_o) result.merged.entries.push_back(o->second);
    }

    std::ranges::sort(result.merged.entries, {}, &agentengine::TreeEntry::name);
    return result;
}

agentengine::result<agentengine::Digest> compute_self_digest(
        agentengine::Digest const& tree, agentengine::Digest const& parent,
        std::uint64_t authored_by_id, std::uint64_t turn_index) {
    std::ostringstream in;
    in << tree << '|' << parent << '|' << authored_by_id << '|' << turn_index;
    std::string const s = in.str();
    std::vector<std::byte> bytes(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) bytes[i] = static_cast<std::byte>(s[i]);
    auto digest = agentengine::compute_digest(bytes);
    if (!digest) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal, digest.error().message,
                                                     "ledger.digest_failed"});
    }
    return *digest;
}

}  // namespace agentengine

namespace agentengine {

// Every member of both specializations, compiled once. A third store would be instantiated the same way, in its own
// translation unit (core/ledger_impl.hpp's top comment).
template class Ledger<InMemoryWorktreeObjectStore>;
template class Ledger<FileWorktreeObjectStore>;

}  // namespace agentengine
