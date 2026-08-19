// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.3 --
// `BruteForceCosineIndex` (core/vector_index.hpp): correctness of cosine ranking, contract
// rejection on malformed batches, and the deterministic tie-break (closes §4 finding 7, "no
// tie-break defined for top-K on equal/near-equal scores") -- score desc, then id asc, mirroring
// `rank_memory_items()`'s own score-desc-then-tie-break-field-desc shape (memory_provider.hpp).

#include <atomic>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/vector_index.hpp"

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
    // --- Basic ranking correctness ------------------------------------------------------------
    {
        ae::BruteForceCosineIndex index;
        std::vector<std::string> ids = {"exact-match", "orthogonal", "opposite"};
        std::vector<std::vector<float>> vecs = {
            {1.0f, 0.0f, 0.0f},   // identical direction to the query
            {0.0f, 1.0f, 0.0f},   // orthogonal -- cosine 0
            {-1.0f, 0.0f, 0.0f},  // opposite direction -- cosine -1
        };
        auto added = index.add_batch(ids, vecs);
        AE_CHECK(added.has_value(), "setup: add_batch accepts 3 ids paired with 3 vectors");
        AE_CHECK(index.size() == 3, "setup: all 3 entries are stored");

        float const query[3] = {1.0f, 0.0f, 0.0f};
        auto results = index.search(std::span<float const>(query, 3), /*k=*/3);
        AE_CHECK(results.has_value() && results->size() == 3, "search returns all 3 entries for k=3");
        if (results.has_value() && results->size() == 3) {
            AE_CHECK((*results)[0].id == "exact-match" && (*results)[0].score > 0.99f,
                     "the identical-direction vector ranks first with cosine ~1.0");
            AE_CHECK((*results)[1].id == "orthogonal",
                     "the orthogonal vector ranks second (cosine ~0), ahead of the opposite one");
            AE_CHECK((*results)[2].id == "opposite" && (*results)[2].score < -0.99f,
                     "the opposite-direction vector ranks last with cosine ~-1.0");
        }

        auto top1 = index.search(std::span<float const>(query, 3), /*k=*/1);
        AE_CHECK(top1.has_value() && top1->size() == 1 && (*top1)[0].id == "exact-match",
                 "k truncates the result set to the top-k entries, not just sorts all of them");
    }

    // --- Contract violations ---------------------------------------------------------------------
    {
        ae::BruteForceCosineIndex index;
        std::vector<std::string> ids = {"a", "b"};
        std::vector<std::vector<float>> one_vec = {{1.0f, 0.0f}};
        auto mismatched = index.add_batch(ids, one_vec);
        AE_CHECK(!mismatched.has_value() && mismatched.error().code == "vector_index.add_batch_length_mismatch",
                 "add_batch rejects ids/vectors of differing length as a contract violation");

        std::vector<std::string> dup_ids = {"x", "x"};
        std::vector<std::vector<float>> dup_vecs = {{1.0f}, {2.0f}};
        auto duped = index.add_batch(dup_ids, dup_vecs);
        AE_CHECK(!duped.has_value() && duped.error().code == "vector_index.add_batch_duplicate_id",
                 "add_batch rejects a duplicate id within a single call as a contract violation");
        AE_CHECK(index.size() == 0,
                 "a rejected add_batch call adds nothing at all -- not a partial write of the "
                 "non-duplicate entries");
    }

    // --- Deterministic tie-break: score desc, then id asc (closes §4 finding 7) ------------------
    {
        ae::BruteForceCosineIndex index;
        // Three entries with byte-identical vectors -- guaranteed exact-tie cosine scores against
        // any query, the real corpora case named in the ADR ("duplicated boilerplate/license
        // headers/repeated snippets").
        std::vector<std::string> ids = {"zzz-last", "aaa-first", "mmm-middle"};
        std::vector<std::vector<float>> vecs = {{1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f}};
        AE_CHECK(index.add_batch(ids, vecs).has_value(), "setup: 3 tied entries stored");

        float const query[2] = {1.0f, 0.0f};
        auto results = index.search(std::span<float const>(query, 2), /*k=*/3);
        AE_CHECK(results.has_value() && results->size() == 3, "all 3 tied entries returned");
        if (results.has_value() && results->size() == 3) {
            AE_CHECK((*results)[0].score == (*results)[1].score && (*results)[1].score == (*results)[2].score,
                     "setup: the 3 entries really do have exactly equal scores (byte-identical "
                     "vectors), so any ordering among them is a pure tie-break decision");
            AE_CHECK((*results)[0].id == "aaa-first" && (*results)[1].id == "mmm-middle" &&
                         (*results)[2].id == "zzz-last",
                     "closes §4 finding 7: on an exact score tie, entries are ordered by id "
                     "ascending -- a genuine, deterministic total order, not insertion order or "
                     "unspecified hash-map iteration order");
        }

        // Re-run the identical search to prove the ordering is stable across calls, not merely
        // stable within one call (the ADR's own concern: "retrieval over a FIXED, already-embedded
        // corpus could return different top-K sets across runs").
        auto results_again = index.search(std::span<float const>(query, 2), /*k=*/3);
        AE_CHECK(results_again.has_value() && *results_again == *results,
                 "repeated search() calls over the same index and query return byte-identical "
                 "ordering -- deterministic, not order-of-iteration-dependent");
    }

    // --- Dimensionality validation (red-team finding, 2026-08-19): reject, don't silently truncate --
    {
        ae::BruteForceCosineIndex index;
        AE_CHECK(index.add_batch({"a"}, {{1.0f, 0.0f, 0.0f}}).has_value(),
                 "setup: a 3-dim vector establishes this index's dimensionality");

        auto wrong_dim = index.add_batch({"b"}, {{1.0f, 0.0f}});
        AE_CHECK(!wrong_dim.has_value() && wrong_dim.error().code == "vector_index.add_batch_dimension_mismatch",
                 "add_batch rejects a vector whose width disagrees with the index's established "
                 "dimensionality, instead of silently storing it and letting cosine_similarity() "
                 "later compare mismatched widths over only their shared (truncated) prefix");
        AE_CHECK(index.size() == 1, "a rejected add_batch call adds nothing, even when only ONE of "
                                     "several entries in the call has the wrong width");

        std::vector<std::string> mixed_ids = {"c", "d"};
        std::vector<std::vector<float>> mixed_vecs = {{1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}};
        auto mixed = index.add_batch(mixed_ids, mixed_vecs);
        AE_CHECK(!mixed.has_value() && mixed.error().code == "vector_index.add_batch_dimension_mismatch",
                 "a batch where entries disagree with EACH OTHER's width (not just the index) is "
                 "also rejected, not just the first-vs-established-dimension case");

        float const wrong_dim_query[2] = {1.0f, 0.0f};
        auto bad_search = index.search(std::span<float const>(wrong_dim_query, 2), /*k=*/1);
        AE_CHECK(!bad_search.has_value() && bad_search.error().code == "vector_index.search_dimension_mismatch",
                 "search() rejects a query vector whose width disagrees with the index's "
                 "dimensionality too, not just add_batch() -- same reject-not-coerce reasoning "
                 "applied symmetrically to the read side");

        float const right_dim_query[3] = {1.0f, 0.0f, 0.0f};
        auto ok_search = index.search(std::span<float const>(right_dim_query, 3), /*k=*/1);
        AE_CHECK(ok_search.has_value(), "search() still succeeds normally with a correctly-dimensioned query");
    }

    // --- Thread-safety under real concurrent read+write (ADR-064 §6's named residual, closed
    // 2026-08-19): a genuine writer thread interleaved with genuine reader threads -- a strictly
    // harder scenario than test_vector_rag_context_provider.cpp's R15, which only proves
    // concurrent READS are safe with no writer present. -----------------------------------------
    {
        ae::BruteForceCosineIndex index;
        constexpr int kWriterBatches = 200;
        constexpr int kReaderThreads = 4;

        std::atomic<bool> stop{false};
        std::atomic<int>  reader_iterations{0};
        std::atomic<bool> reader_saw_bad_state{false};

        std::vector<std::thread> readers;
        readers.reserve(kReaderThreads);
        for (int r = 0; r < kReaderThreads; ++r) {
            readers.emplace_back([&]() {
                float const query[2] = {1.0f, 0.0f};
                while (!stop.load(std::memory_order_acquire)) {
                    auto result = index.search(std::span<float const>(query, 2), /*k=*/1'000'000);
                    if (!result.has_value()) {
                        // Every entry the writer below ever adds is 2-dim, so a dimension error
                        // here would be a genuine bug, not an expected outcome.
                        reader_saw_bad_state.store(true, std::memory_order_release);
                    } else {
                        // Every id search() returns must still be present via contains() -- this
                        // index never removes entries, so a torn/corrupted read under contention
                        // (a returned id search() saw but that isn't genuinely committed) would
                        // surface here as a real, observable inconsistency.
                        for (auto const& s : *result) {
                            if (!index.contains(s.id)) {
                                reader_saw_bad_state.store(true, std::memory_order_release);
                            }
                        }
                    }
                    reader_iterations.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        std::thread writer([&]() {
            for (int i = 0; i < kWriterBatches; ++i) {
                std::string const id = "w" + std::to_string(i);
                auto added = index.add_batch({id}, {{1.0f, 0.0f}});
                if (!added.has_value()) reader_saw_bad_state.store(true, std::memory_order_release);
            }
            stop.store(true, std::memory_order_release);
        });

        writer.join();
        for (auto& t : readers) t.join();

        AE_CHECK(!reader_saw_bad_state.load(),
                 "concurrent readers never observe a corrupted/torn read (a returned id that "
                 "search() reports but contains() then denies, or a spurious dimension error) "
                 "while a writer concurrently calls add_batch() -- real concurrent read+write "
                 "safety, not just read-only concurrency");
        AE_CHECK(index.size() == static_cast<std::size_t>(kWriterBatches),
                 "every one of the writer's add_batch() calls actually landed -- no silently lost "
                 "write under contention");
        AE_CHECK(reader_iterations.load() > 0,
                 "setup: readers actually ran concurrently with the writer, not merely sequenced "
                 "entirely before or after it");
    }

    std::cout << (g_failures == 0 ? "test_vector_index: OK\n" : "test_vector_index: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
