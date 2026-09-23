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
#include <cstdint>
#include <cstring>
#include <limits>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/worktree_types.hpp"

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

// Implements decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.4 — a REFINEMENT
// of `VectorIndex`, not a requirement on it: closes ADR-063 §7's long-named "vector-index persistence
// is undesigned" residual (`BruteForceCosineIndex` is in-memory-only; a process restart re-embeds the
// entire corpus regardless of what a caller's `previous_file_hashes` map claims). A conformer that
// wants restart-survival implements `snapshot()`/`restore()`; one that doesn't (e.g. a purely
// ephemeral per-session scratch index, or `VulkanCosineIndex`, ADR-180 §2.6, which is local compute
// with no durability story of its own either) remains a perfectly valid plain `VectorIndex` without
// being forced to carry machinery it has no use for. A `RemoteVectorIndex` conformer (ADR-180 §2.5,
// e.g. `QdrantVectorIndex`) deliberately does NOT implement this concept at all — its durable home
// IS the remote service by construction, so this concept's own problem (local process memory is not
// durable) does not apply to it.
//
// Two-parameter (`T`, `OS`) rather than one, mirroring how a caller actually uses it: persistence is
// always exercised against SOME concrete `WorktreeObjectStore` conformer (`InMemoryWorktreeObjectStore`
// in tests, a future disk-backed store in production, per ADR-063 §7's own already-scoped-as-ordinary-
// implementation-work note on that missing piece) — the same reason `write_corpus_chunk_record()`
// (corpus_chunk.hpp) is itself templated on `<WorktreeObjectStore OS, rt::AppendLogStore RS>` rather
// than fixed to one store type.
template <class T, class OS>
concept PersistentVectorIndex =
    VectorIndex<T> && WorktreeObjectStore<OS> &&
    requires(T const& idx, T& mutable_idx, OS& store, Digest const& digest) {
        { idx.snapshot(store) } -> std::same_as<result<Digest>>;
        { mutable_idx.restore(store, digest) } -> std::same_as<result<void>>;
    };

namespace vector_index_detail {

[[nodiscard]] inline bool all_finite(std::span<float const> values) {
    return std::all_of(values.begin(), values.end(), [](float x) { return std::isfinite(x); });
}

// Score desc, then id asc (ADR-063 §4 finding 7), with every NaN ordered after every non-NaN score
// (NaNs among themselves by id). The plain `a.score != b.score ? a.score > b.score : ...` comparator
// is not a strict weak ordering once a NaN is present -- undefined behavior in std::sort, observed as
// out-of-order FINITE results (ADR-180 §4e condition 2). add_batch()/search()/restore() reject
// non-finite input so a NaN score is unreachable; the comparator is total anyway so the sort's own
// precondition never rests on that.
inline void sort_and_truncate(std::vector<ScoredId>& scored, std::size_t k) {
    std::sort(scored.begin(), scored.end(), [](ScoredId const& a, ScoredId const& b) {
        bool const a_nan = std::isnan(a.score);
        bool const b_nan = std::isnan(b.score);
        if (a_nan != b_nan) return b_nan;
        if (!a_nan && a.score != b.score) return a.score > b.score;
        return a.id < b.id;
    });
    if (scored.size() > k) scored.resize(k);
}

// A compact, custom binary format for a `BruteForceCosineIndex` snapshot — deliberately NOT JSON
// (unlike `CorpusChunkRecord`): this is a hot-path-adjacent artifact read back once per process
// start, holding potentially thousands of `dimension`-length float arrays, not a small, human-
// inspected record. Layout: 4-byte magic `"AEV1"`, u64 `dimension`, u64 entry `count`, then per
// entry: u32 id length, id bytes, `dimension` little-endian float32s. All integers little-endian.
inline void append_u32(std::vector<std::byte>& buf, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}
inline void append_u64(std::vector<std::byte>& buf, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) buf.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}
inline void append_f32(std::vector<std::byte>& buf, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    append_u32(buf, bits);
}
inline void append_bytes(std::vector<std::byte>& buf, std::string_view s) {
    for (char c : s) buf.push_back(static_cast<std::byte>(c));
}

// Reject-not-coerce readers (this codebase's own convention, e.g. `corpus_chunk_record_from_json`'s
// identical stance): a truncated or tampered snapshot blob returns a real `error`, never undefined
// behavior or a silently short read.
struct ByteReader {
    std::span<std::byte const> data;
    std::size_t                pos = 0;

    [[nodiscard]] static error truncated() {
        return error{failure_class::contract, "vector index snapshot blob is truncated or malformed",
                      "vector_index.snapshot_truncated"};
    }

    // ADR-180 §4 red-team finding C1 (2026-09-22): `count`/`dimension` are read successfully (8
    // bytes genuinely present each) long before the loop that actually uses them to size a
    // `reserve()` call -- a blob claiming e.g. `count = UINT64_MAX` with only 20 bytes total present
    // passed the per-field bounds check trivially, then reached `reserve(UINT64_MAX)`, which throws
    // `std::length_error`/`std::bad_alloc` -- an EXCEPTION crossing into a plain, non-coroutine
    // `result<T>`-returning function with no try/catch anywhere in the call chain (CONVENTIONS.md:
    // "no exceptions for control flow"), crashing the process instead of `restore()` returning the
    // typed `error` its own contract promises. Fixed by checking `count`/`dimension` against how
    // many bytes could ACTUALLY remain (the cheapest possible per-entry size: 4-byte id_len field +
    // 0-byte id + `dimension` floats) before any allocation is attempted.
    [[nodiscard]] result<void> check_header_plausible(std::uint64_t count, std::uint64_t dimension) const {
        // Overflow guard on the multiplication below, checked BEFORE it runs, not after.
        constexpr std::uint64_t kOverflowGuard = std::numeric_limits<std::uint64_t>::max() / 8;
        if (dimension > kOverflowGuard || count > kOverflowGuard) return std::unexpected(header_implausible());

        std::uint64_t const min_bytes_per_entry = 4 + dimension * 4;  // id_len field + dimension floats
        std::uint64_t const remaining = static_cast<std::uint64_t>(data.size() - pos);
        if (count > remaining / min_bytes_per_entry) return std::unexpected(header_implausible());
        return {};
    }

    [[nodiscard]] static error header_implausible() {
        return error{failure_class::contract,
                      "vector index snapshot blob declares an entry count/dimension that cannot "
                      "possibly fit in the blob's remaining bytes",
                      "vector_index.snapshot_header_implausible"};
    }

    [[nodiscard]] result<std::uint32_t> read_u32() {
        if (pos + 4 > data.size()) return std::unexpected(truncated());
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[pos + static_cast<std::size_t>(i)]))
                 << (8 * i);
        }
        pos += 4;
        return v;
    }
    [[nodiscard]] result<std::uint64_t> read_u64() {
        if (pos + 8 > data.size()) return std::unexpected(truncated());
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(data[pos + static_cast<std::size_t>(i)]))
                 << (8 * i);
        }
        pos += 8;
        return v;
    }
    [[nodiscard]] result<float> read_f32() {
        auto bits = read_u32();
        if (!bits) return std::unexpected(bits.error());
        float f;
        std::memcpy(&f, &*bits, sizeof(f));
        return f;
    }
    [[nodiscard]] result<std::string> read_bytes(std::size_t n) {
        if (pos + n > data.size()) return std::unexpected(truncated());
        std::string s(reinterpret_cast<char const*>(data.data() + pos), n);
        pos += n;
        return s;
    }
};

}  // namespace vector_index_detail

// O(n*d) per query, correct by construction, not tuned. ADR-063 §3 claim 1 is the (as yet unset)
// latency bound a future bench must check before this can be called sufficient for any specific
// deployment's corpus size — this class makes no claim about that bound itself.
//
// THREAD-SAFETY (ADR-064 §6's named residual, closed 2026-08-19): `VectorRagContextProvider` takes
// its index by REFERENCE specifically so it can be shared across provider instances (this class's
// own original design intent, `vector_rag_context_provider.hpp`'s constructor comment); a future
// `CorpusSource` ingestion step (ADR-063 §2.4A, not yet built) writing to a shared index while a
// live session's `recall()` concurrently reads it was a genuine, if not-yet-reachable, data race —
// this class had no internal synchronization at all. Fixed here, not left as a caller obligation:
// a `std::shared_mutex` guards every access -- `add_batch()` takes a unique (writer) lock,
// `search()`/`contains()`/`size()` take a shared (reader) lock, so any number of concurrent readers
// may proceed together, but a writer excludes everyone. This is a real, permanent fix (not merely a
// "currently unreachable" residual note) — correct today for the pure-read-concurrency case R15
// (`tests/test_vector_rag_context_provider.cpp`) already proves, AND now also correct once a real
// concurrent-writer ingestion path exists, without needing to revisit this file again.
// ae-naming-lint: allow BruteForceCosineIndex — ADR-063: new vocabulary, not yet in 027 §2-4's tables.
class BruteForceCosineIndex {
public:
    [[nodiscard]] result<void> add_batch(std::vector<std::string> const& ids,
                                          std::vector<std::vector<float>> const& vectors) {
        std::unique_lock lock(mutex_);
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
            if (!vector_index_detail::all_finite(v)) {
                return std::unexpected(error{failure_class::contract,
                                              "vector components must be finite -- NaN or infinity cannot be scored",
                                              "vector_index.add_batch_non_finite"});
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
        std::shared_lock lock(mutex_);
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
        if (!vector_index_detail::all_finite(query)) {
            return std::unexpected(error{failure_class::contract,
                                          "query components must be finite -- NaN or infinity cannot be scored",
                                          "vector_index.search_non_finite"});
        }
        std::vector<ScoredId> scored;
        scored.reserve(order_.size());
        for (auto const& id : order_) {
            scored.push_back({id, cosine_similarity(query, entries_.at(id))});
        }
        vector_index_detail::sort_and_truncate(scored, k);
        return scored;
    }

    [[nodiscard]] bool contains(std::string const& id) const {
        std::shared_lock lock(mutex_);
        return entries_.contains(id);
    }

    [[nodiscard]] std::size_t size() const {
        std::shared_lock lock(mutex_);
        return entries_.size();
    }

    // ADR-180 §2.4: writes this index's entire current state as one content-addressed blob via the
    // caller-supplied `object_store` (the same store a corpus mount already writes chunk blobs/records
    // through — no new storage subsystem, matching ADR-063 §1's own "reuse, don't invent" thesis). An
    // unchanged index re-snapshots to the IDENTICAL digest (content-addressing's own free dedup), so a
    // caller can snapshot after every mount pass cheaply when nothing actually changed.
    template <WorktreeObjectStore OS>
    [[nodiscard]] result<Digest> snapshot(OS& object_store) const {
        std::shared_lock lock(mutex_);
        std::vector<std::byte> buf;
        vector_index_detail::append_bytes(buf, "AEV1");
        vector_index_detail::append_u64(buf, static_cast<std::uint64_t>(dimension_));
        vector_index_detail::append_u64(buf, static_cast<std::uint64_t>(order_.size()));
        for (auto const& id : order_) {
            auto const& vec = entries_.at(id);
            vector_index_detail::append_u32(buf, static_cast<std::uint32_t>(id.size()));
            vector_index_detail::append_bytes(buf, id);
            for (float f : vec) vector_index_detail::append_f32(buf, f);
        }
        return object_store.put_blob(std::span<std::byte const>(buf.data(), buf.size()));
    }

    // Replaces this index's ENTIRE current state with what `digest` names — the intended use is a
    // fresh, empty index calling `restore()` once at process start (ADR-180 §3 claims 4/5), not a
    // merge with whatever this index already held. Reject-not-coerce on any structural inconsistency
    // (truncated data, a bad magic header, a duplicate id) rather than silently loading a partial or
    // corrupted index — the same posture `corpus_chunk_record_from_json()` already takes toward a
    // malformed record.
    //
    // ADR-180 §4 red-team finding M3 (2026-09-22): calling `restore()` on an already-populated index
    // used to silently DISCARD whatever was already there (the final swap replaces `entries_`/
    // `order_` unconditionally) -- no error, no warning, just quietly gone. That is exactly the kind
    // of silent-not-rejected behavior this function's own "reject-not-coerce" contract (above) is
    // supposed to rule out; it was simply never checked. Now enforced: `restore()` requires the
    // index be empty first, converting a silent data-loss footgun into a real, typed error. This
    // narrows, but does not fully close, the adjacent residual ADR-180 §7 already names (a
    // concurrent `add_batch()` racing between this check and the final swap below could still slip
    // an entry in that then gets silently discarded) -- the sequential misuse case this finding
    // actually demonstrated is now closed; the race is still open, unchanged, and still named there.
    template <WorktreeObjectStore OS>
    [[nodiscard]] result<void> restore(OS& object_store, Digest const& digest) {
        {
            std::shared_lock empty_check(mutex_);
            if (!entries_.empty()) {
                return std::unexpected(error{
                    failure_class::contract,
                    "restore() was called on a non-empty index -- restore() REPLACES an index's "
                    "entire state and is intended for a fresh index at process start, never a "
                    "merge; construct a fresh index to restore into instead of reusing a populated "
                    "one",
                    "vector_index.restore_requires_empty_index"});
            }
        }

        auto bytes = object_store.get_blob(digest);
        if (!bytes) return std::unexpected(bytes.error());

        vector_index_detail::ByteReader reader{std::span<std::byte const>(bytes->data(), bytes->size())};
        auto magic = reader.read_bytes(4);
        if (!magic) return std::unexpected(magic.error());
        if (*magic != "AEV1") {
            return std::unexpected(error{failure_class::contract,
                                          "vector index snapshot blob has an unrecognized magic header",
                                          "vector_index.snapshot_bad_magic"});
        }
        auto dim = reader.read_u64();
        if (!dim) return std::unexpected(dim.error());
        auto count = reader.read_u64();
        if (!count) return std::unexpected(count.error());
        auto plausible = reader.check_header_plausible(*count, *dim);
        if (!plausible) return std::unexpected(plausible.error());

        std::unordered_map<std::string, std::vector<float>> new_entries;
        std::vector<std::string> new_order;
        new_entries.reserve(static_cast<std::size_t>(*count));
        new_order.reserve(static_cast<std::size_t>(*count));
        for (std::uint64_t i = 0; i < *count; ++i) {
            auto id_len = reader.read_u32();
            if (!id_len) return std::unexpected(id_len.error());
            auto id = reader.read_bytes(*id_len);
            if (!id) return std::unexpected(id.error());
            if (new_entries.contains(*id)) {
                return std::unexpected(error{failure_class::contract,
                                              "vector index snapshot blob contains a duplicate id",
                                              "vector_index.snapshot_duplicate_id"});
            }
            std::vector<float> vec;
            vec.reserve(static_cast<std::size_t>(*dim));
            for (std::uint64_t d = 0; d < *dim; ++d) {
                auto f = reader.read_f32();
                if (!f) return std::unexpected(f.error());
                if (!std::isfinite(*f)) {
                    return std::unexpected(error{failure_class::contract,
                                                  "vector index snapshot blob contains a non-finite component",
                                                  "vector_index.snapshot_non_finite"});
                }
                vec.push_back(*f);
            }
            new_order.push_back(*id);
            new_entries.emplace(*id, std::move(vec));
        }

        std::unique_lock lock(mutex_);
        entries_   = std::move(new_entries);
        order_     = std::move(new_order);
        dimension_ = static_cast<std::size_t>(*dim);
        return {};
    }

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

    mutable std::shared_mutex                            mutex_;
    std::unordered_map<std::string, std::vector<float>> entries_;
    std::vector<std::string>                             order_;
    std::size_t                                          dimension_ = 0;
};

static_assert(VectorIndex<BruteForceCosineIndex>);
static_assert(PersistentVectorIndex<BruteForceCosineIndex, InMemoryWorktreeObjectStore>);

}  // namespace agentengine
