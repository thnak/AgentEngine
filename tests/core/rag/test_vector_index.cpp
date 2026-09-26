// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.3 --
// `BruteForceCosineIndex` (core/vector_index.hpp): correctness of cosine ranking, contract
// rejection on malformed batches, and the deterministic tie-break (closes §4 finding 7, "no
// tie-break defined for top-K on equal/near-equal scores") -- score desc, then id asc, mirroring
// `rank_memory_items()`'s own score-desc-then-tie-break-field-desc shape (memory_provider.hpp).
//
// Also implements decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.4/§3 claims
// 4-5 -- `BruteForceCosineIndex::snapshot()`/`restore()` (`PersistentVectorIndex`): round-trip
// fidelity, content-addressed re-snapshot dedup, and reject-not-coerce on a malformed/nonexistent
// blob.

#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/vector_index.hpp"
#include "agentengine/core/worktree.hpp"

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

    // --- Persistence (ADR-180 §2.4/§3 claims 4-5): snapshot -> restore round-trip fidelity --------
    {
        ae::InMemoryWorktreeObjectStore store;
        ae::BruteForceCosineIndex index;
        AE_CHECK(index.add_batch({"a", "b", "c"},
                                  {{1.0f, 0.0f}, {0.0f, 1.0f}, {0.7071f, 0.7071f}})
                     .has_value(),
                 "setup: 3 entries stored");

        float const query[2] = {1.0f, 0.0f};
        auto before = index.search(std::span<float const>(query, 2), /*k=*/3);
        AE_CHECK(before.has_value(), "pre-snapshot search succeeds");

        auto digest = index.snapshot(store);
        AE_CHECK(digest.has_value(), "snapshot() succeeds and returns a digest");

        // Claim 4's own re-snapshot-dedup property: an UNCHANGED index snapshotted twice produces
        // the identical digest -- content-addressing's own free dedup, not asserted, checked.
        auto digest_again = index.snapshot(store);
        AE_CHECK(digest_again.has_value() && *digest_again == *digest,
                 "an unchanged index re-snapshots to the IDENTICAL digest");

        ae::BruteForceCosineIndex restored;
        AE_CHECK(restored.size() == 0, "setup: the restore target starts empty");
        auto restore_result = restored.restore(store, *digest);
        AE_CHECK(restore_result.has_value(), "restore() succeeds");
        AE_CHECK(restored.size() == 3, "restore() repopulates every entry the snapshot held");

        auto after = restored.search(std::span<float const>(query, 2), /*k=*/3);
        AE_CHECK(after.has_value() && before.has_value() && after->size() == before->size(),
                 "claim 4: post-restore search returns the SAME NUMBER of results as pre-snapshot");
        if (after.has_value() && before.has_value()) {
            bool identical = true;
            for (std::size_t i = 0; i < before->size(); ++i) {
                if ((*before)[i].id != (*after)[i].id ||
                    std::fabs((*before)[i].score - (*after)[i].score) > 1e-6f) {
                    identical = false;
                }
            }
            AE_CHECK(identical,
                     "claim 4: post-restore search returns the IDENTICAL top-K (same ids, same "
                     "scores, same order) as the pre-snapshot index for the same query");
        }

        // Reject-not-coerce: a nonexistent digest fails cleanly rather than restoring garbage.
        // Freshly-empty targets, deliberately -- `restored` is already populated from the
        // successful restore() above, and (per M3's fix, tested separately below) restore() on a
        // non-empty index is now itself rejected; using a fresh index here keeps each assertion
        // testing exactly the ONE failure mode it names, not incidentally exercising M3's guard
        // instead of the thing it claims to test.
        ae::BruteForceCosineIndex missing_target;
        auto missing = missing_target.restore(store, std::string(64, '0'));
        AE_CHECK(!missing.has_value() && missing.error().code != "vector_index.restore_requires_empty_index",
                 "restore() from a digest the store has never seen fails");

        // Reject-not-coerce: a blob that exists but isn't a valid snapshot (wrong magic) is
        // rejected, not silently misinterpreted as index data.
        std::string const garbage = "not a snapshot blob";
        auto garbage_digest = store.put_blob(std::as_bytes(std::span{garbage.data(), garbage.size()}));
        AE_CHECK(garbage_digest.has_value(), "setup: a non-snapshot blob is stored");
        ae::BruteForceCosineIndex bad_magic_target;
        auto bad_magic = bad_magic_target.restore(store, *garbage_digest);
        AE_CHECK(!bad_magic.has_value() && bad_magic.error().code == "vector_index.snapshot_bad_magic",
                 "restore() rejects a blob with an unrecognized magic header rather than "
                 "misinterpreting arbitrary bytes as vector data");
    }

    // --- ADR-180 §4 red-team finding C1 (2026-09-22): a blob claiming an implausible entry count/
    // dimension must be rejected BEFORE any allocation is attempted, not crash the process via an
    // uncaught std::length_error/bad_alloc from reserve() -----------------------------------------
    {
        ae::InMemoryWorktreeObjectStore store;
        std::vector<std::byte> blob;
        auto push_bytes = [&](std::string_view s) {
            for (char c : s) blob.push_back(static_cast<std::byte>(c));
        };
        auto push_u64 = [&](std::uint64_t v) {
            for (int i = 0; i < 8; ++i) blob.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
        };
        push_bytes("AEV1");
        push_u64(0);                                   // dimension = 0
        push_u64(0xFFFFFFFFFFFFFFFFull);                // count = UINT64_MAX -- nothing close to that
                                                          // many bytes actually follows
        auto digest = store.put_blob(std::span<std::byte const>(blob.data(), blob.size()));
        AE_CHECK(digest.has_value(), "setup: an implausible-header blob is itself stored successfully "
                                       "-- put_blob() has no reason to reject arbitrary bytes");

        ae::BruteForceCosineIndex victim;
        auto restore_result = victim.restore(store, *digest);
        AE_CHECK(!restore_result.has_value() &&
                     restore_result.error().code == "vector_index.snapshot_header_implausible",
                 "restore() rejects a header whose declared count could not possibly fit in the "
                 "blob's actual remaining bytes, instead of reserve()-ing an unbounded allocation "
                 "and crashing the process with an uncaught exception");

        // A second, more devious case: a count that IS individually representable but that,
        // multiplied by a large dimension, would overflow the size computation itself if the
        // overflow guard were missing.
        std::vector<std::byte> blob2;
        auto push_bytes2 = [&](std::string_view s) {
            for (char c : s) blob2.push_back(static_cast<std::byte>(c));
        };
        auto push_u64_2 = [&](std::uint64_t v) {
            for (int i = 0; i < 8; ++i) blob2.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
        };
        push_bytes2("AEV1");
        push_u64_2(0xFFFFFFFFFFFFFFFFull);  // dimension = UINT64_MAX
        push_u64_2(2);                       // count = 2
        auto digest2 = store.put_blob(std::span<std::byte const>(blob2.data(), blob2.size()));
        AE_CHECK(digest2.has_value(), "setup: the overflow-shaped blob is stored");
        ae::BruteForceCosineIndex victim2;
        auto restore_result2 = victim2.restore(store, *digest2);
        AE_CHECK(!restore_result2.has_value() &&
                     restore_result2.error().code == "vector_index.snapshot_header_implausible",
                 "restore() rejects an overflow-shaped count*dimension header cleanly, rather than "
                 "the multiplication itself wrapping around and bypassing the plausibility check");
    }

    // --- ADR-180 §4 red-team finding M3 (2026-09-22): restore() on an already-populated index is
    // rejected, not a silent data-loss footgun ------------------------------------------------------
    {
        ae::InMemoryWorktreeObjectStore store;
        ae::BruteForceCosineIndex source_index;
        AE_CHECK(source_index.add_batch({"x"}, {{1.0f, 0.0f}}).has_value(), "setup: source populated");
        auto digest = source_index.snapshot(store);
        AE_CHECK(digest.has_value(), "setup: source snapshotted");

        ae::BruteForceCosineIndex already_populated;
        AE_CHECK(already_populated.add_batch({"pre-existing"}, {{0.0f, 1.0f}}).has_value(),
                 "setup: the restore TARGET already holds an unrelated entry");
        auto restore_result = already_populated.restore(store, *digest);
        AE_CHECK(!restore_result.has_value() &&
                     restore_result.error().code == "vector_index.restore_requires_empty_index",
                 "restore() on a non-empty index is rejected with a typed error, rather than "
                 "silently discarding the pre-existing entry");
        AE_CHECK(already_populated.size() == 1 && already_populated.contains("pre-existing"),
                 "the rejected restore() call left the target's pre-existing state completely "
                 "untouched -- not even partially overwritten");
    }

    // --- ADR-180 §4e condition 2 (2026-09-23): non-finite input is rejected at every entry point, and
    // the ranking comparator is a strict weak ordering even over NaN ------------------------------
    {
        float const nan = std::numeric_limits<float>::quiet_NaN();
        float const inf = std::numeric_limits<float>::infinity();

        ae::BruteForceCosineIndex idx;
        auto bad_nan = idx.add_batch({"n"}, {{1.0f, nan}});
        AE_CHECK(!bad_nan.has_value() && bad_nan.error().code == "vector_index.add_batch_non_finite" &&
                     bad_nan.error().klass == ae::failure_class::contract,
                 "add_batch() rejects a NaN component as a contract violation");
        auto bad_inf = idx.add_batch({"i"}, {{-inf, 0.0f}});
        AE_CHECK(!bad_inf.has_value() && bad_inf.error().code == "vector_index.add_batch_non_finite",
                 "add_batch() rejects an infinite component");
        AE_CHECK(idx.size() == 0, "a rejected batch leaves the index untouched");
        auto mixed = idx.add_batch({"ok", "bad"}, {{1.0f, 0.0f}, {nan, nan}});
        AE_CHECK(!mixed.has_value() && idx.size() == 0 && !idx.contains("ok"),
                 "one non-finite vector rejects the WHOLE batch -- no partial commit of the finite ones");

        AE_CHECK(idx.add_batch({"a"}, {{1.0f, 0.0f}}).has_value(), "setup: one finite vector");
        float const nan_query[2] = {nan, 1.0f};
        auto q = idx.search(std::span<float const>(nan_query, 2), 1);
        AE_CHECK(!q.has_value() && q.error().code == "vector_index.search_non_finite",
                 "search() rejects a non-finite query rather than scoring NaN against every entry");

        // restore(): a hand-built, otherwise well-formed AEV1 blob carrying a NaN component. Nothing
        // in snapshot() can produce one any more, but a blob is external bytes -- a corrupt or
        // crafted one must not smuggle NaN past add_batch()'s check.
        ae::InMemoryWorktreeObjectStore store;
        std::vector<std::byte> blob;
        ae::vector_index_detail::append_bytes(blob, "AEV1");
        ae::vector_index_detail::append_u64(blob, 2);
        ae::vector_index_detail::append_u64(blob, 1);
        ae::vector_index_detail::append_u32(blob, 1);
        ae::vector_index_detail::append_bytes(blob, "z");
        ae::vector_index_detail::append_f32(blob, nan);
        ae::vector_index_detail::append_f32(blob, 1.0f);
        auto digest = store.put_blob(std::span<std::byte const>(blob.data(), blob.size()));
        AE_CHECK(digest.has_value(), "setup: NaN-bearing snapshot blob stored");
        ae::BruteForceCosineIndex restored;
        auto r = restored.restore(store, *digest);
        AE_CHECK(!r.has_value() && r.error().code == "vector_index.snapshot_non_finite" && restored.size() == 0,
                 "restore() rejects a snapshot blob carrying a non-finite component, leaving the index empty");

        // The comparator itself, driven directly with NaN scores (defense in depth: the checks above
        // make a NaN score unreachable through the public API, but the sort's own precondition must
        // not depend on that). Under the old `a.score != b.score ? a.score > b.score : a.id < b.id`
        // comparator this input violated strict weak ordering -- undefined behavior in std::sort.
        std::vector<ae::ScoredId> scored;
        for (int i = 0; i < 200; ++i) {
            float const s = (i % 7 == 0) ? nan : static_cast<float>((i * 37) % 101) / 100.0f;
            scored.push_back({"id" + std::to_string(1000 + i), s});
        }
        ae::vector_index_detail::sort_and_truncate(scored, scored.size());
        bool finite_desc = true;
        bool nans_last = true;
        bool seen_nan = false;
        for (std::size_t i = 0; i < scored.size(); ++i) {
            bool const is_nan = std::isnan(scored[i].score);
            if (seen_nan && !is_nan) nans_last = false;
            seen_nan = seen_nan || is_nan;
            if (i > 0 && !is_nan && !std::isnan(scored[i - 1].score)) {
                auto const& p = scored[i - 1];
                auto const& c = scored[i];
                if (p.score < c.score || (p.score == c.score && !(p.id < c.id))) finite_desc = false;
            }
        }
        AE_CHECK(finite_desc, "every finite score is in score-desc/id-asc order despite NaNs in the input");
        AE_CHECK(nans_last, "every NaN score orders after every finite score");
        std::vector<ae::ScoredId> truncated = scored;
        ae::vector_index_detail::sort_and_truncate(truncated, 5);
        AE_CHECK(truncated.size() == 5 && !std::isnan(truncated.back().score),
                 "truncation to k keeps the top finite scores, never a NaN ahead of them");
    }

    std::cout << (g_failures == 0 ? "test_vector_index: OK\n" : "test_vector_index: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
