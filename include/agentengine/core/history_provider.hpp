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
#include <utility>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/context_provider.hpp"

namespace agentengine {

template <std::size_t N>
struct Window {};  // ae-naming-lint: allow Window — 005 §4 names this concept normatively; 027 has not been updated to list it

// 005 §4's other named strategy: "Summarize older turns into a `system` summary message via a
// declared model." `SummarizerT` is that declared model — a second `ChatClient` (004 §1), not
// necessarily the same backend/deployment the session's own turn loop uses, matching 005 §4's own
// wording ("via a declared model," not "via the session's own model"). Bundled into the tag itself
// (`Summarize<N, SummarizerT>`), rather than a second `HistoryProvider` template parameter, so
// `HistoryProvider<Strategy>` keeps exactly one template parameter across both specializations —
// `Window<N>` (history_provider.hpp above) never had a second parameter to begin with, and giving
// `HistoryProvider` a defaulted-void second slot only for `Summarize` would make the two
// strategies' own call shape inconsistent for no benefit.
template <std::size_t N, class SummarizerT>
struct Summarize {};  // ae-naming-lint: allow Summarize — 005 §4 names this concept normatively; 027 has not been updated to list it

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

    // `TurnView` (Phase G3) is real now, but a window over history has nothing to record at turn
    // end -- memory extraction (029, `core/memory_provider.hpp`) is the real conformer for this hook.
    void on_turn_end(TurnView, EffectContext&) {}
};

static_assert(ContextProvider<HistoryProvider<Window<0>>>,
              "HistoryProvider<Window<0>> must satisfy ContextProvider (005 §5)");
static_assert(ContextProvider<HistoryProvider<Window<8>>>,
              "HistoryProvider<Window<N>> must satisfy ContextProvider (005 §5)");

// Milestone 4 Phase B4: keeps the last `N` messages verbatim (same rule `Window<N>` uses above)
// and folds everything OLDER than that into exactly one synthesized `system` summary message,
// produced by calling `SummarizerT::chat()` once per turn against the older slice. 005 §7 G3's
// "bounded divergence for `Summarize`" gate holds by construction here: given the SAME older
// slice and the SAME (mock, in every test this milestone runs, decision 8) summarizer, the
// summary is byte-identical every time — the only source of "divergence" from the uncompacted
// control is the summarizer's own text replacing the older messages' own text, which is exactly
// what 005 §4 calls a strategy that is NOT exact (unlike `Window<N>`, which is).
template <std::size_t N, class SummarizerT>
    requires ChatClient<SummarizerT>
class HistoryProvider<Summarize<N, SummarizerT>> {
public:
    [[nodiscard]] result<ContextContribution> on_context(SessionContext& session_ctx,
                                                           EffectContext& ctx) {
        auto const& h = session_ctx.history;
        ContextContribution contribution;
        if (h.size() <= N) {
            // Nothing older than the verbatim window yet — no summary to produce, matching
            // `Window<N>`'s own "a window wider than the history keeps all of it, fabricates
            // nothing" rule exactly (a summary of zero messages is not a message).
            contribution.messages.assign(h.begin(), h.end());
            return contribution;
        }

        std::size_t const split = h.size() - N;
        std::vector<Message> older(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(split));
        std::vector<Message> recent(h.begin() + static_cast<std::ptrdiff_t>(split), h.end());

        ChatRequest summarize_request{std::move(older)};
        result<ChatResponse> summary_response = summarizer_.chat(summarize_request, ctx);
        if (!summary_response) return std::unexpected(summary_response.error());

        // 005 §4: "a `system` summary message" — the summarizer's own reply, re-labeled `system`
        // regardless of what role it came back as, since a summary is never attributable to the
        // user or assistant turn it replaces.
        Message summary_message      = summary_response->message;
        summary_message.role         = role::system;
        contribution.messages.push_back(std::move(summary_message));
        contribution.messages.insert(contribution.messages.end(), recent.begin(), recent.end());
        return contribution;
    }

    void on_turn_end(TurnView, EffectContext&) {}

private:
    SummarizerT summarizer_;
};

} // namespace agentengine
