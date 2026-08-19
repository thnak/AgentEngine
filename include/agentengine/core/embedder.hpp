#pragma once
// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.2 — the Embedder
// seam: a declared backend shaped like ChatClient (004 §1) — `capabilities()` +
// `embed_batch(texts, EffectContext&)` — batch-only (no single-item `embed()`, §2.2B: extraction
// and bulk corpus ingestion both need batching from day one to be affordable at all), and
// deliberately NOT wrapped in a Recording/Replay determinism harness the way `ChatClient` backends
// are. That is a named, accepted I5 tradeoff (§2.2A), not an oversight: `VectorRagContextProvider`
// cannot claim 029 §9 G1's "no network call, byte-identical replay" guarantee the way
// `MemoryProvider` does — ADR-063 §3 claim 3 is the falsifiable version of this tradeoff, and any
// caller composing an `Embedder` into a provider must carry that non-determinism forward at the
// call site, not silently imply Memory-style replay safety.

#include <concepts>
#include <cstdint>
#include <string>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/task.hpp"

namespace agentengine {

// Declared, never probed — the same rule `ChatClientCapabilities` already follows (004 §2).
// `max_batch_size` must be populated from a real, confirmed provider limit (ADR-063 §2.5), not
// guessed; a proxying host (e.g. OpenRouter in front of several underlying embedding backends) may
// have a limit that differs from the backend it fronts.
// ae-naming-lint: allow EmbedderCapabilities — ADR-063: new vocabulary, not yet in 027 §2-4's tables.
struct EmbedderCapabilities {
    std::uint32_t dimensions = 0;
    std::uint32_t max_batch_size = 0;
};

template <class T>
// ae-naming-lint: allow Embedder — ADR-063: new vocabulary, not yet in 027 §2-4's tables.
concept Embedder = requires(T e, std::vector<std::string> const& texts, EffectContext& ctx) {
    { e.capabilities() } -> std::same_as<EmbedderCapabilities>;
    { e.embed_batch(texts, ctx) } -> std::same_as<task<result<std::vector<std::vector<float>>>>>;
};

}  // namespace agentengine
