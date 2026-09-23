#pragma once
// Implements decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.1 — a NEW, ADDITIVE
// concept for a network-backed `VectorIndex`-shaped conformer (e.g. `QdrantVectorIndex`,
// `protocol/qdrant/vector_index.hpp`, ADR-180 §2.5). Deliberately NOT a widened `VectorIndex`
// (core/vector_index.hpp): `VectorIndex`, `BruteForceCosineIndex`, and every already-Judged
// ADR-063 call site (`vector_rag_context_provider.hpp`'s `on_context()`/`recall`, `corpus_source.hpp`)
// remain byte-for-byte unmodified by this file's own existence — see ADR-180 §2.1's rejected Design B
// for why widening the existing concept was rejected (real regression risk to 193/193-green, Judged
// code, for a feature most deployments won't use).
//
// Shape mirrors `core/embedder.hpp`'s `Embedder` concept directly and deliberately: batch-only,
// `EffectContext`-threaded (a real network call needs a capability-resolution seam — the `SecretRef`
// a conformer resolves INSIDE each call, never at construction, ADR-063 §2.5's own rule), and a
// REQUIRED declared `synchronous_leaf` trait reusing ADR-064 §3 Design B's exact mechanism a second
// time: a provider's `recall(query)` tool `invoke` closure (synchronous-only, `tool_pipeline.hpp`)
// may drive a `RemoteVectorIndex::search()`/`contains()` call via `rt::drive_leaf_task()` ONLY when
// the conformer has actively declared `synchronous_leaf = true` — an explicit, reviewed claim that
// its coroutine body never awaits anything but nested `task<T>`/`task<void>`, not something this
// concept infers.
//
// `contains(id, ctx)` is a real network round-trip for a remote conformer, unlike `VectorIndex`'s
// O(1) local hash lookup — named explicitly here, not hidden: a `CorpusSource` re-mount's per-chunk
// dedup check (§2.4A) becomes materially more expensive against a `RemoteVectorIndex`, a cost ADR-180
// §7 names as a real, unaddressed residual (no batched "which of these ids exist" call is designed).

#include <concepts>
#include <span>
#include <string>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/core/vector_index.hpp"  // ScoredId, VectorIndex

namespace agentengine {

// ADR-180 §4 red-team finding R1 (2026-09-22): `VectorIndex` and `RemoteVectorIndex` were NOT
// structurally disjoint -- concepts only check "does this call expression compile with this return
// type," and a type offering BOTH a synchronous 2-arg overload set (satisfying `VectorIndex`) and an
// async 3-arg overload set (satisfying `RemoteVectorIndex`) -- a plausible thing to write, e.g. a
// caching wrapper offering a cheap local fast-path alongside a network fallback -- would satisfy
// BOTH concepts at once with no compiler diagnostic. Every dispatch site in this ADR family
// (`vector_rag_context_provider.hpp`, `hybrid_rag_context_provider.hpp`, `corpus_source.hpp`) checks
// `RemoteVectorIndex<IndexT>` FIRST in its `if constexpr` chain, so such a dual-conformer type would
// silently and unconditionally be routed onto the network/task path, even in call sites where the
// type's own synchronous path would have been correct and cheaper -- invisibly, to the author of
// that future conformer. Closed here, at the definition, rather than patched at every dispatch site:
// `RemoteVectorIndex<T>` explicitly excludes anything that already satisfies `VectorIndex<T>`, making
// `AnyVectorIndex`'s two disjuncts genuinely mutually exclusive by construction, not merely by
// convention. `remote_vector_index.hpp` already depends on `vector_index.hpp` (for `ScoredId`), so
// this adds no new include-order constraint.
template <class T>
// ae-naming-lint: allow RemoteVectorIndex — ADR-180: new vocabulary, not yet in 027 §2-4's tables.
concept RemoteVectorIndex =
    !VectorIndex<T> &&
    requires(T idx, std::vector<std::string> ids, std::vector<std::vector<float>> vecs,
             std::span<float const> query, std::size_t k, std::string const& id, EffectContext& ctx) {
        { idx.add_batch(ids, vecs, ctx) } -> std::same_as<task<result<void>>>;
        { idx.search(query, k, ctx) } -> std::same_as<task<result<std::vector<ScoredId>>>>;
        { idx.contains(id, ctx) } -> std::same_as<task<result<bool>>>;
        { T::synchronous_leaf } -> std::convertible_to<bool>;
    };

// The umbrella every `ContextProvider`'s `IndexT` template parameter is constrained by, going
// forward (ADR-180 §2.1 Design A): local, synchronous conformers (`BruteForceCosineIndex`,
// `VulkanCosineIndex`, ADR-180 §2.6 — GPU dispatch is local compute, no network, no capability
// gating, so it stays on the plain, unmodified `VectorIndex` concept) and network-backed,
// capability-gated ones (`QdrantVectorIndex`) are both valid `IndexT`s; call sites dispatch between
// them via `if constexpr`, never runtime polymorphism.
template <class T>
concept AnyVectorIndex = VectorIndex<T> || RemoteVectorIndex<T>;

}  // namespace agentengine
