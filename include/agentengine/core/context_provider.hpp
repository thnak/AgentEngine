#pragma once
// Implements 005-Sessions-State-and-Memory.md §5 — the one seam for "contribute to the context
// before the model is called." History, skills, and memory (029) are kinds of this, not parallel
// concepts (027 §3).
//
// Milestone 4 Phase B1 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md,
// decision 6): `ContextContribution.tools` was elided pending "006's ToolDecl" — a stale
// placeholder. 006 never minted a type by that name; its real per-run tool-table entry, built and
// proven in M2, is `ToolDescriptor` (core/tool_pipeline.hpp), the same type ADR-010's WASM host
// already reuses for `list_tools()`. There is no second, provider-facing declaration shape to
// invent — a provider-contributed tool is unioned into the exact same table (005 §5: "still
// traverses the full invocation pipeline, 006 §3"), so it must be the exact same type.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine {

// Mirrors MAF's `AIContext` shape deliberately (docs/research/2026-maf-provider-concepts.md §1):
// a provider is not limited to injecting text. `tools` here is the same declaration shape 006 §1
// tools use; a provider-contributed tool still traverses the full invocation pipeline (006 §3).
struct ContextContribution {  // ae-naming-lint: allow ContextContribution — pre-existing M0 scaffolding, reconcile at owning milestone
    std::optional<std::string> instructions;
    std::vector<Message>       messages;
    std::vector<ToolDescriptor> tools;
};

// 005 §3's "the per-run view a provider reads" — Milestone 4 Phase B1 models this for real. Kept
// deliberately narrow: `session_id`/`principal`/`history` are exactly the fields every kind 005 §5
// actually names (`HistoryProvider`, `SkillsProvider`, 029's memory providers) reads off a session
// — none of them need the caller-declared `StateT` scratch state (005 §8 Q1), which is why this
// type is not itself a template: a non-template seam can't see an arbitrary per-agent StateT
// without type erasure this milestone has no gate requiring yet. Reference members: an instance is
// built fresh per turn by whatever calls into a provider (`AgentSession`, a future assembler),
// never stored past that call, matching `EffectContext`'s own "attribution, not accumulation"
// shape.
struct SessionContext {
    std::string_view             session_id;
    Principal const&             principal;
    std::vector<Message> const&  history;
};

// Milestone 4 Phase G3 (029 §4: "ContextProvider.on_turn_end (005 §5) is where memory is written
// ... may call a declared ChatClient to extract candidate MemoryItems from the turn"). 005 §5's
// own text elided this as "TurnView elided" because nothing needed it before this phase — a
// memory-writing provider's `on_turn_end` is the first REAL conformer that needs to see what
// happened in the turn it's reacting to, so this is filling in an already-named gap, not
// reopening 005's own design. A non-owning view (this project's own "attribution, not
// accumulation" shape, matching `SessionContext`/`EffectContext`): exactly the messages THIS turn
// added to history (in this milestone's own one-model-call-per-turn scope, the input plus the
// response — never the whole history, which `SessionContext.history` already exposes to
// `on_context` for providers that need the full record).
struct TurnView {
    std::span<Message const> turn_messages;
};

// Return types constrained to their synchronous equivalents, same reason and same caveat as
// `Runner`/`ChatClient`/`SandboxBackend`: `ae::task<T>` is not yet wired in here.
template <class T>
concept ContextProvider = requires(T provider, SessionContext& session_ctx, EffectContext& ctx,
                                    TurnView turn) {
    { provider.on_context(session_ctx, ctx) } -> std::same_as<result<ContextContribution>>;
    { provider.on_turn_end(turn, ctx) } -> std::same_as<void>;
};

} // namespace agentengine
