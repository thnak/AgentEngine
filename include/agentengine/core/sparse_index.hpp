#pragma once
// Implements decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.2 — the
// `SparseIndex` seam and its core-resident default, `BM25Index` (std-only, zero third-party
// dependency, CONVENTIONS.md tier 1, matching `VectorIndex`/`BruteForceCosineIndex`'s own bar
// directly one file over). Mirrors `core/vector_index.hpp`'s exact shape — concept + default
// conformer in one file, `ScoredId` reused unmodified (a chunk id and a score, regardless of which
// retrieval mechanism produced it) — substituting query/document TEXT for query/document VECTORS.
//
// No `RemoteSparseIndex` counterpart exists in this pass — no vendor need was named (unlike Qdrant
// for the dense side, ADR-180 §2.5); the concept itself is the extension seam if one shows up later,
// the identical answer already given for `VectorIndex` before this ADR existed.
//
// Tokenizer (named honestly as a real quality residual, ADR-180 §7, not silently claimed
// equivalent to a real IR library's): ASCII lowercasing plus a split on any non-alphanumeric byte —
// no stemming, no stop-word list, no Unicode awareness. `k1`/`b` are declared, swappable constructor
// parameters (matching `RecursiveChunker`'s own "declared policy, not a fixed algorithm" precedent,
// ADR-063 §2.4b), defaulting to the standard Okapi BM25 literature values (`k1 = 1.2`, `b = 0.75`).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/vector_index.hpp"  // ScoredId

namespace agentengine {

template <class T>
// ae-naming-lint: allow SparseIndex — ADR-180: new vocabulary, not yet in 027 §2-4's tables.
concept SparseIndex = requires(T idx, std::vector<std::string> ids, std::vector<std::string> texts,
                                 std::string const& query, std::size_t k, std::string const& id) {
    { idx.add_batch(ids, texts) } -> std::same_as<result<void>>;
    { idx.search(query, k) } -> std::same_as<result<std::vector<ScoredId>>>;
    { idx.contains(id) } -> std::same_as<bool>;
};

namespace bm25_index_detail {

// Lowercases and splits on any byte that is not an ASCII letter/digit — a real, named quality gap
// (ADR-180 §7), not a claimed-equivalent stand-in for a real tokenizer (no stemming, no stop words,
// no multi-byte-aware Unicode handling; a UTF-8 multi-byte sequence's continuation bytes are treated
// as non-alphanumeric splitters, so non-ASCII text tokenizes crudely rather than incorrectly-but-
// silently).
[[nodiscard]] inline std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    for (unsigned char c : text) {
        if (std::isalnum(c) != 0) {
            current.push_back(static_cast<char>(std::tolower(c)));
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

}  // namespace bm25_index_detail

// Okapi BM25 (Robertson-Sparck-Jones IDF with the `+0.5` smoothing that keeps IDF non-negative for a
// term appearing in every document). O(q * avg_postings_per_term) per query, not O(n) like
// `BruteForceCosineIndex` — an inverted index, not a linear scan.
//
// THREAD-SAFETY: same `std::shared_mutex` reader/writer discipline `BruteForceCosineIndex` uses
// (ADR-064 §6's fix, directly reused here rather than reinvented) — `add_batch()` takes a unique
// (writer) lock, `search()`/`contains()`/`size()` take a shared (reader) lock.
// ae-naming-lint: allow BM25Index — ADR-180: new vocabulary, not yet in 027 §2-4's tables.
class BM25Index {
public:
    explicit BM25Index(double k1 = 1.2, double b = 0.75) : k1_(k1), b_(b) {}

    [[nodiscard]] result<void> add_batch(std::vector<std::string> const& ids,
                                          std::vector<std::string> const& texts) {
        std::unique_lock lock(mutex_);
        if (ids.size() != texts.size()) {
            return std::unexpected(error{failure_class::contract,
                                          "ids and texts must have the same length",
                                          "sparse_index.add_batch_length_mismatch"});
        }
        // Same two-fold duplicate check `BruteForceCosineIndex::add_batch()` already makes: an id
        // already present from a PRIOR call, and an id repeated TWICE within this same call.
        std::unordered_set<std::string> seen_in_this_call;
        for (auto const& id : ids) {
            if (doc_length_.contains(id) || !seen_in_this_call.insert(id).second) {
                return std::unexpected(error{failure_class::contract,
                                              "duplicate id in a single add_batch call",
                                              "sparse_index.add_batch_duplicate_id"});
            }
        }

        for (std::size_t i = 0; i < ids.size(); ++i) {
            auto const& id = ids[i];
            auto tokens = bm25_index_detail::tokenize(texts[i]);
            std::unordered_map<std::string, std::size_t> term_freq;
            for (auto const& t : tokens) ++term_freq[t];
            for (auto const& [term, tf] : term_freq) postings_[term].push_back({id, tf});
            doc_length_.emplace(id, tokens.size());
            total_length_ += tokens.size();
            order_.push_back(id);
        }
        return {};
    }

    [[nodiscard]] result<std::vector<ScoredId>> search(std::string const& query, std::size_t k) const {
        std::shared_lock lock(mutex_);
        if (order_.empty()) return std::vector<ScoredId>{};

        auto query_tokens = bm25_index_detail::tokenize(query);
        std::unordered_set<std::string> unique_terms(query_tokens.begin(), query_tokens.end());
        double const n_docs = static_cast<double>(order_.size());
        double const avg_doc_length = static_cast<double>(total_length_) / n_docs;

        std::unordered_map<std::string, double> scores;
        for (auto const& term : unique_terms) {
            auto it = postings_.find(term);
            if (it == postings_.end()) continue;
            double const n_t = static_cast<double>(it->second.size());
            double const idf = std::log(1.0 + (n_docs - n_t + 0.5) / (n_t + 0.5));
            for (auto const& [id, tf] : it->second) {
                double const dl = static_cast<double>(doc_length_.at(id));
                double const denom = static_cast<double>(tf) + k1_ * (1.0 - b_ + b_ * dl / avg_doc_length);
                scores[id] += idf * (static_cast<double>(tf) * (k1_ + 1.0)) / denom;
            }
        }

        std::vector<ScoredId> out;
        out.reserve(scores.size());
        for (auto const& [id, s] : scores) out.push_back({id, static_cast<float>(s)});
        // Same deterministic total order `BruteForceCosineIndex::search()` uses: score desc, then id
        // asc (ADR-063 §4 finding 7's tie-break, applied identically here).
        std::sort(out.begin(), out.end(), [](ScoredId const& a, ScoredId const& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.id < b.id;
        });
        if (out.size() > k) out.resize(k);
        return out;
    }

    [[nodiscard]] bool contains(std::string const& id) const {
        std::shared_lock lock(mutex_);
        return doc_length_.contains(id);
    }

    [[nodiscard]] std::size_t size() const {
        std::shared_lock lock(mutex_);
        return order_.size();
    }

private:
    mutable std::shared_mutex mutex_;
    double                    k1_;
    double                    b_;
    // term -> [(doc id, term frequency in that doc)] -- the inverted index itself.
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::size_t>>> postings_;
    std::unordered_map<std::string, std::size_t>                                       doc_length_;
    std::vector<std::string>                                                            order_;
    std::size_t                                                                         total_length_ = 0;
};

static_assert(SparseIndex<BM25Index>);

}  // namespace agentengine
