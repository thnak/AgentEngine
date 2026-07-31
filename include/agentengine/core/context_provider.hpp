#pragma once
// Implements 005-Sessions-State-and-Memory.md §5 — the one seam for "contribute to the context
// before the model is called." History, skills, and memory (029) are kinds of this, not parallel
// concepts (027 §3).

#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"

namespace agentengine {

// Mirrors MAF's `AIContext` shape deliberately (docs/research/2026-maf-provider-concepts.md §1):
// a provider is not limited to injecting text. `tools` here is the same declaration shape 006 §1
// tools use; a provider-contributed tool still traverses the full invocation pipeline (006 §3).
struct ContextContribution {
    std::optional<std::string> instructions;
    std::vector<Message>       messages;
    // std::vector<ToolDecl> tools; — ToolDecl is 006's schema-derived declaration type, elided
    // here pending that header; the shape exists in prose (005 §5) even where not yet typed.
};

struct SessionContext; // fwd — the per-run view a provider reads (005 §3), not yet modeled

// Return types constrained to their synchronous equivalents, same reason and same caveat as
// `Runner`/`ChatClient`/`SandboxBackend`: `ae::task<T>` is not yet wired in here.
template <class T>
concept ContextProvider = requires(T provider, SessionContext& session_ctx, EffectContext& ctx) {
    { provider.on_context(session_ctx, ctx) } -> std::same_as<result<ContextContribution>>;
    { provider.on_turn_end(ctx) } -> std::same_as<void>;  // TurnView elided, see 005 §5
};

} // namespace agentengine
