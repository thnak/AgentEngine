#pragma once
// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.3 — the
// VectorIndex seam and its core-resident default, `BruteForceCosineIndex` (std-only, zero
// third-party dependency, CONVENTIONS.md tier 1). ANN (hnswlib) and GPU-accelerated (Vulkan)
// conformers are opt-in seam backends (`src/backends/*`, tier 2), gated on a real bench against
// this default proving it is the actual bottleneck (§2.3B) — not built here.
//
// Tie-break (closes ADR-063 §4 finding 7, "no tie-break defined for top-K on equal/near-equal
// scores"): score desc, then `id` asc. `add_batch()` rejects a duplicate id outright (a caller
// passing one is a contract violation, not a case this index silently tolerates), so within one
// index every id is unique and this is a genuine total order — the same
// score-desc-then-tie-break-field-desc shape `rank_memory_items()` (memory_provider.hpp) already
// uses for `MemoryItem::write_seq`, with the analogous unique-per-entry field substituted in.

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine {

// ae-naming-lint: allow ScoredId — ADR-063: new vocabulary, not yet in 027 §2-4's tables.
struct ScoredId {
    std::string id;
    float       score = 0.0f;

    friend bool operator==(ScoredId const&, ScoredId const&) = default;
};

template <class T>
// ae-naming-lint: allow VectorIndex — ADR-063: new vocabulary, not yet in 027 §2-4's tables.
concept VectorIndex =
    requires(T idx, std::vector<std::string> ids, std::vector<std::vector<float>> vecs,
             std::span<float const> query, std::size_t k, std::string const& id) {
        { idx.add_batch(ids, vecs) } -> std::same_as<result<void>>;
        { idx.search(query, k) } -> std::same_as<result<std::vector<ScoredId>>>;
        // ADR-063 §4 finding 2 ("the design never names a manifest/lookup that lets re-mount
        // answer 'does this digest already have a stored vector' before calling embed_batch()
        // again"): a CorpusSource conformer's re-mount path checks this BEFORE re-embedding a
        // chunk, both to avoid the wasted embedder call and because add_batch() itself rejects a
        // duplicate id outright (§2.4A's own content-addressed dedup means two DIFFERENT files
        // producing byte-identical chunk text is expected, not an error condition to propagate).
        { idx.contains(id) } -> std::same_as<bool>;
    };

// O(n*d) per query, correct by construction, not tuned. ADR-063 §3 claim 1 is the (as yet unset)
// latency bound a future bench must check before this can be called sufficient for any specific
// deployment's corpus size — this class makes no claim about that bound itself.
// ae-naming-lint: allow BruteForceCosineIndex — ADR-063: new vocabulary, not yet in 027 §2-4's tables.
class BruteForceCosineIndex {
public:
    [[nodiscard]] result<void> add_batch(std::vector<std::string> const& ids,
                                          std::vector<std::vector<float>> const& vectors) {
        if (ids.size() != vectors.size()) {
            return std::unexpected(error{failure_class::contract,
                                          "ids and vectors must have the same length",
                                          "vector_index.add_batch_length_mismatch"});
        }
        // Two distinct checks: an id already present from a PRIOR add_batch call, and an id
        // repeated TWICE WITHIN this same call (a bare `entries_.contains()` scan misses the
        // latter entirely, since none of a fresh batch's own ids are in `entries_` yet).
        std::unordered_set<std::string> seen_in_this_call;
        for (auto const& id : ids) {
            if (entries_.contains(id) || !seen_in_this_call.insert(id).second) {
                return std::unexpected(error{failure_class::contract,
                                              "duplicate id in a single add_batch call",
                                              "vector_index.add_batch_duplicate_id"});
            }
        }
        // Reject rather than silently truncate (red-team, 2026-08-19): every vector in this index
        // must share one dimensionality, established by the first vector ever added. Without this
        // check, `cosine_similarity()` would silently compare mismatched-width vectors over only
        // their shared prefix (via `std::min`), returning an ordinary-looking but semantically
        // meaningless score instead of an error -- exactly the "silently wrong answer" this
        // codebase's reject-not-coerce convention exists to prevent.
        std::size_t const expected_dim = dimension_ != 0 ? dimension_ : (vectors.empty() ? 0 : vectors.front().size());
        for (auto const& v : vectors) {
            if (v.size() != expected_dim) {
                return std::unexpected(
                    error{failure_class::contract,
                          "vector dimensionality mismatch: expected " + std::to_string(expected_dim) +
                              ", got " + std::to_string(v.size()),
                          "vector_index.add_batch_dimension_mismatch"});
            }
        }
        for (std::size_t i = 0; i < ids.size(); ++i) {
            entries_.emplace(ids[i], vectors[i]);
            order_.push_back(ids[i]);
        }
        if (dimension_ == 0) dimension_ = expected_dim;
        return {};
    }

    [[nodiscard]] result<std::vector<ScoredId>> search(std::span<float const> query,
                                                         std::size_t k) const {
        // Same reject-not-coerce reasoning as add_batch()'s dimension check above, applied to the
        // query side (red-team, 2026-08-19) -- a mismatched-width query would otherwise silently
        // score against only a truncated prefix of every stored vector.
        if (dimension_ != 0 && query.size() != dimension_) {
            return std::unexpected(error{
                failure_class::contract,
                "query vector dimensionality (" + std::to_string(query.size()) +
                    ") does not match index dimensionality (" + std::to_string(dimension_) + ")",
                "vector_index.search_dimension_mismatch"});
        }
        std::vector<ScoredId> scored;
        scored.reserve(order_.size());
        for (auto const& id : order_) {
            scored.push_back({id, cosine_similarity(query, entries_.at(id))});
        }
        std::sort(scored.begin(), scored.end(), [](ScoredId const& a, ScoredId const& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.id < b.id;
        });
        if (scored.size() > k) scored.resize(k);
        return scored;
    }

    [[nodiscard]] bool contains(std::string const& id) const { return entries_.contains(id); }

    [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
    // Unreachable in practice once add_batch()/search() both enforce dimension_ above -- kept as
    // std::min() defensively rather than an unchecked a[i]/b[i] loop, since this private helper has
    // no way to independently verify its callers upheld that contract.
    [[nodiscard]] static float cosine_similarity(std::span<float const> a, std::span<float const> b) {
        double dot = 0.0, na = 0.0, nb = 0.0;
        std::size_t const n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i) {
            dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
            na += static_cast<double>(a[i]) * static_cast<double>(a[i]);
            nb += static_cast<double>(b[i]) * static_cast<double>(b[i]);
        }
        if (na == 0.0 || nb == 0.0) return 0.0f;
        return static_cast<float>(dot / (std::sqrt(na) * std::sqrt(nb)));
    }

    std::unordered_map<std::string, std::vector<float>> entries_;
    std::vector<std::string>                             order_;
    std::size_t                                          dimension_ = 0;
};

static_assert(VectorIndex<BruteForceCosineIndex>);

}  // namespace agentengine
