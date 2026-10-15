#pragma once
// Test-only synchronous drivers for the `task`-shaped index seams (decisions/ADR-237 §5.1, #172):
// `VectorIndex` and `SparseIndex` take an `EffectContext&` and return `task<result<...>>`. Tests that
// exercise an index's own behaviour (dimension checks, tie-break, BM25 scoring, ...) from a plain `main()`
// call these instead, so each assertion reads as it did against the old synchronous seam.
//
// Concept-constrained free functions rather than one variadic forwarder: the parameter types are concrete,
// so the braced-init arguments these tests use (`sync_add_batch(idx, {"a"}, {{1.0f, 0.0f}})`) still work.
// Each call uses a default `EffectContext` (no principal, no capabilities, no stop) -- right for the local
// indexes, which ignore it; a test that needs a real context calls the seam itself.

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/sparse_index.hpp"
#include "agentengine/core/vector_index.hpp"
#include "agentengine/rt/block_on.hpp"

namespace agentengine::test_support {

template <VectorIndex Idx>
result<void> sync_add_batch(Idx& idx, std::vector<std::string> const& ids,
                            std::vector<std::vector<float>> const& vectors) {
    EffectContext ctx;
    return rt::block_on(idx.add_batch(ids, vectors, ctx));
}

template <SparseIndex Idx>
result<void> sync_add_batch(Idx& idx, std::vector<std::string> const& ids, std::vector<std::string> const& texts) {
    EffectContext ctx;
    return rt::block_on(idx.add_batch(ids, texts, ctx));
}

template <VectorIndex Idx>
result<std::vector<ScoredId>> sync_search(Idx& idx, std::span<float const> query, std::size_t k) {
    EffectContext ctx;
    return rt::block_on(idx.search(query, k, ctx));
}

template <SparseIndex Idx>
result<std::vector<ScoredId>> sync_search(Idx& idx, std::string const& query, std::size_t k) {
    EffectContext ctx;
    return rt::block_on(idx.search(query, k, ctx));
}

// `true` only for a successful lookup that found the id; a failed lookup reads as "not contained".
template <class Idx>
    requires VectorIndex<Idx> || SparseIndex<Idx>
bool sync_contains(Idx& idx, std::string const& id) {
    EffectContext ctx;
    auto r = rt::block_on(idx.contains(id, ctx));
    return r.has_value() && *r;
}

}  // namespace agentengine::test_support
