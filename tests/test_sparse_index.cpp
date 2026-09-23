// Implements decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.2/§3 claim 3 --
// `BM25Index` (core/sparse_index.hpp): standard Okapi BM25 formula correctness against a
// hand-computed, fixed corpus (not an approximation asserted equivalent), contract rejection on
// malformed batches, and the same deterministic score-desc/id-asc tie-break every other index in
// this ADR family uses.

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "agentengine/core/sparse_index.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

}  // namespace

int main() {
    // --- Hand-computed BM25 correctness (ADR-180 §3 claim 3) --------------------------------------
    // Docs: d1 "the cat sat on the mat" (6 tok), d2 "the dog sat on the log" (6 tok),
    //       d3 "cats and dogs are great" (5 tok). Query: "cat". N=3, avgdl = 17/3.
    // Only d1 contains "cat" (n_t=1, tf=1); k1=1.2 (default), b=0.75 (default).
    //   idf = ln(1 + (3-1+0.5)/(1+0.5)) = ln(2.666667) = 0.980829
    //   denom = 1 + 1.2*(0.25 + 0.75*6/(17/3)) = 1 + 1.2*(0.25 + 0.794118) = 2.252941
    //   score(d1) = idf * (1*2.2) / denom = 0.980829 * 2.2 / 2.252941 = 0.957781
    {
        ae::BM25Index bm25;
        auto added = bm25.add_batch(
            {"d1", "d2", "d3"},
            {"the cat sat on the mat", "the dog sat on the log", "cats and dogs are great"});
        AE_CHECK(added.has_value(), "setup: 3 docs added");
        AE_CHECK(bm25.size() == 3, "setup: size() reflects all 3 docs");

        auto scored = bm25.search("cat", 10);
        AE_CHECK(scored.has_value(), "search('cat') succeeds");
        if (scored.has_value()) {
            AE_CHECK(scored->size() == 1,
                     "only d1 matches 'cat' -- the tokenizer is stemming-free by design (ADR-180 "
                     "§2.2's own named residual), so 'cats' in d3 does not match");
            if (!scored->empty()) {
                AE_CHECK((*scored)[0].id == "d1", "d1 is the sole match");
                double const expected = 0.957781;
                AE_CHECK(std::fabs(static_cast<double>((*scored)[0].score) - expected) < 0.001,
                         "d1's score matches the hand-computed Okapi BM25 formula exactly, not an "
                         "approximation of it");
            }
        }

        auto no_match = bm25.search("nonexistent_term_xyz", 10);
        AE_CHECK(no_match.has_value() && no_match->empty(),
                 "a query with no matching term returns an empty, successful result, not an error");

        AE_CHECK(bm25.contains("d1") && !bm25.contains("nonexistent-id"),
                 "contains() reflects exactly what was added, no more, no less");
    }

    // --- IDF smoothing keeps a term present in every doc from going negative ----------------------
    {
        ae::BM25Index bm25;
        AE_CHECK(bm25.add_batch({"x", "y", "z"}, {"common word", "common term", "common phrase"})
                     .has_value(),
                 "setup: 3 docs, all sharing 'common'");
        auto scored = bm25.search("common", 10);
        AE_CHECK(scored.has_value() && scored->size() == 3,
                 "a term present in every document still matches every document (non-negative IDF "
                 "via the +0.5 Robertson-Sparck-Jones smoothing, not a zero/negative score that "
                 "would otherwise rank a universally-common term as anti-relevant)");
        if (scored.has_value()) {
            for (auto const& s : *scored) AE_CHECK(s.score >= 0.0f, "score for '" + s.id + "' is non-negative");
        }
    }

    // --- Contract violations, mirroring BruteForceCosineIndex's own identical checks --------------
    {
        ae::BM25Index bm25;
        auto mismatched = bm25.add_batch({"a", "b"}, {"one text"});
        AE_CHECK(!mismatched.has_value() && mismatched.error().code == "sparse_index.add_batch_length_mismatch",
                 "add_batch rejects ids/texts of differing length");

        auto duped = bm25.add_batch({"x", "x"}, {"text one", "text two"});
        AE_CHECK(!duped.has_value() && duped.error().code == "sparse_index.add_batch_duplicate_id",
                 "add_batch rejects a duplicate id within a single call");
        AE_CHECK(bm25.size() == 0, "a rejected add_batch call adds nothing at all");

        AE_CHECK(bm25.add_batch({"y"}, {"first"}).has_value(), "setup: 'y' added");
        auto redup = bm25.add_batch({"y"}, {"second"});
        AE_CHECK(!redup.has_value() && redup.error().code == "sparse_index.add_batch_duplicate_id",
                 "add_batch rejects an id already present from a PRIOR call, not just within one call");
    }

    // --- Deterministic tie-break: score desc, then id asc (same total order every other index in
    // this ADR family uses) -------------------------------------------------------------------------
    {
        ae::BM25Index bm25;
        // Three byte-identical documents against a query matching all of them equally.
        AE_CHECK(bm25.add_batch({"zzz-last", "aaa-first", "mmm-middle"},
                                 {"apple banana", "apple banana", "apple banana"})
                     .has_value(),
                 "setup: 3 tied docs stored");
        auto scored = bm25.search("apple", 10);
        AE_CHECK(scored.has_value() && scored->size() == 3, "all 3 tied docs returned");
        if (scored.has_value() && scored->size() == 3) {
            AE_CHECK((*scored)[0].score == (*scored)[1].score && (*scored)[1].score == (*scored)[2].score,
                     "setup: the 3 docs really do tie exactly");
            AE_CHECK((*scored)[0].id == "aaa-first" && (*scored)[1].id == "mmm-middle" &&
                         (*scored)[2].id == "zzz-last",
                     "on an exact score tie, entries are ordered by id ascending -- the same "
                     "deterministic total order BruteForceCosineIndex::search() and "
                     "reciprocal_rank_fusion() both use");
        }
    }

    // --- k truncates, does not merely sort ----------------------------------------------------------
    {
        ae::BM25Index bm25;
        AE_CHECK(bm25.add_batch({"a", "b", "c"}, {"word", "word", "word"}).has_value(), "setup");
        auto top1 = bm25.search("word", 1);
        AE_CHECK(top1.has_value() && top1->size() == 1, "k=1 truncates the result set to exactly 1 entry");
    }

    std::cout << (g_failures == 0 ? "test_sparse_index: OK\n" : "test_sparse_index: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
