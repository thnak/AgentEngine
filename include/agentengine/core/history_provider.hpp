#pragma once
// Implements 005-Sessions-State-and-Memory.md §4/§5's `HistoryProvider` kind — the first real
// `ContextProvider` conformer (Milestone 4 Phase B2,
// docs/planning/milestone-4-sessions-durability-memory-breakdown.md), replacing
// `AgentSession`'s M1-scope "the full history, trivially" shortcut (agent_session.hpp's own top
// comment) with a real windowed assembly path.
//
// `Window<N>` (005 §4's table: "Keep the last N turns verbatim") is a compile-time NTTP tag,
// matching this project's CRTP-policy idiom (`MaxTurns<N>`, `TokenBudget<N>`) rather than a
// runtime config value. `N == 0` means unbounded — keep the whole history — chosen as the
// default specifically so wiring `HistoryProvider` into `AgentSession` (below) is not itself a
// behavior change from what every Phase A test already proved.
//
// `Summarize<N>` (005 §4's other named strategy) gets its own `HistoryProvider` specialization in
// Milestone 4 Phase B4, added alongside this one — not a replacement of this shape. `Window<N>`'s
// own gate (005 §7 G3: "exact [replay] for Window") is true here by construction: the kept
// messages are an unmodified suffix of `history`, so replaying against a fixed history always
// reproduces the identical kept set.

#include <cstddef>

#include "agentengine/core/context_provider.hpp"

namespace agentengine {

template <std::size_t N>
struct Window {};  // ae-naming-lint: allow Window — 005 §4 names this concept normatively; 027 has not been updated to list it

// Only real strategies get a definition (`Window<N>` here, `Summarize<N, SummarizerT>` in Phase
// B4) — an unlisted `Strategy` fails to compile at the point of use, rather than silently doing
// nothing.
template <class Strategy>
class HistoryProvider;

template <std::size_t N>
class HistoryProvider<Window<N>> {
public:
    [[nodiscard]] result<ContextContribution> on_context(SessionContext& session_ctx,
                                                           EffectContext&) {
        ContextContribution contribution;
        auto const& h = session_ctx.history;
        if constexpr (N == 0) {
            contribution.messages.assign(h.begin(), h.end());
        } else {
            std::size_t const start = h.size() > N ? h.size() - N : 0;
            contribution.messages.assign(h.begin() + static_cast<std::ptrdiff_t>(start), h.end());
        }
        return contribution;
    }

    // 005 §5's `TurnView`-typed hook is elided (context_provider.hpp's own concept comment) — a
    // window over history has nothing to record at turn end; a future stateful provider (029's
    // memory extraction, Phase G) is where this becomes real logic, not here.
    void on_turn_end(EffectContext&) {}
};

static_assert(ContextProvider<HistoryProvider<Window<0>>>,
              "HistoryProvider<Window<0>> must satisfy ContextProvider (005 §5)");
static_assert(ContextProvider<HistoryProvider<Window<8>>>,
              "HistoryProvider<Window<N>> must satisfy ContextProvider (005 §5)");

} // namespace agentengine
