#pragma once
// Composes a `HistoryProviderT` and a `SkillsProviderT` (skill_provider.hpp) into ONE
// `ContextProvider` conformer, via the REAL, already-built N-contributor assembler
// (core/context_assembly.hpp's `assemble_context`) -- reused exactly as it already is, not
// reimplemented. `AgentSession<ChatClientT, StateT, HistoryProviderT>` (core/agent_session.hpp) has
// exactly one `HistoryProviderT` template slot; this type lets that ONE slot carry both
// contributions without any change to `agent_session.hpp` itself, matching `assemble_context`'s own
// file-top comment that full N-contributor wiring into `AgentSession`'s production path is a
// deliberately separate, later "Phase G" -- this header does not pull that forward, it works AROUND
// it, entirely at the HistoryProviderT layer.

#include <concepts>
#include <utility>
#include <vector>

#include "agentengine/core/context_assembly.hpp"
#include "agentengine/core/context_provider.hpp"

namespace agentengine {

// `AgentSession<ChatClientT, StateT, HistoryProviderT>` (core/agent_session.hpp) holds its
// `HistoryProviderT history_provider_;` as a plain, default-CONSTRUCTED value member -- unlike
// `chat_client_`, it has no `emplace_*`/accessor pair (ADR-018's own fix for the same shape of
// problem, applied to a different member): there is no way to reach in and configure
// `history_provider_` after the actor exists. So THIS type must itself be default-constructible to
// occupy that slot at all -- the conditional default constructor below is not a convenience, it is
// the only way an `AgentSession<..., HistoryAndSkillsProvider<H, S>>` can be declared under
// `quark::TestKit<A>`'s `A actor_;` member (the same ADR-018 constraint `chat_client_` was fixed for,
// unfixed here because nothing needed a non-default-constructible `HistoryProviderT` before this).
template <class HistoryProviderT, class SkillsProviderT>
    requires ContextProvider<HistoryProviderT> && ContextProvider<SkillsProviderT>
class HistoryAndSkillsProvider {
public:
    HistoryAndSkillsProvider()
        requires std::default_initializable<HistoryProviderT> &&
                 std::default_initializable<SkillsProviderT>
        : HistoryAndSkillsProvider(HistoryProviderT{}, SkillsProviderT{}) {}

    // Both budgets default to `{0}` (unbounded): `assemble_context` drops the OLDEST MESSAGES first
    // when a contributor's OWN contribution exceeds ITS OWN declared budget. `SkillsProviderT`
    // contributes exactly ONE system message (its whole skill advertisement); a nonzero budget that
    // message doesn't fit under would silently drop the WHOLE advertisement, not trim it down to
    // something smaller and still useful. Unbounded by default; a caller may still opt into a real
    // cap for either contributor.
    HistoryAndSkillsProvider(HistoryProviderT history, SkillsProviderT skills,
                              ContextBudget history_budget = {}, ContextBudget skills_budget = {}) {
        // `contributors_` is built exactly ONCE, here, in the constructor -- never inside
        // `on_context()`. `make_context_provider_descriptor` move-constructs its argument into a
        // fresh `shared_ptr` each call; rebuilding it every turn would silently reconstruct a brand
        // new `SkillsProviderT` every turn, discarding its own `loaded_`/mounted state and breaking
        // `SkillsProvider`'s "resolve-once, freeze" guarantee (009 §8c) outright.
        contributors_.push_back(make_context_provider_descriptor(std::move(history), history_budget));
        contributors_.push_back(make_context_provider_descriptor(std::move(skills), skills_budget));
    }

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx,
                                                                 EffectContext& ctx) {
        ContextAssemblyResult assembled = co_await assemble_context(contributors_, session_ctx, ctx);
        co_return assembled.combined;
    }

    // `assemble_context` itself never calls `on_turn_end` on its contributors (confirmed by reading
    // its source -- it only ever invokes `on_context`) -- forwarded manually here to both wrapped
    // providers, or the hook would be silently dropped for both.
    task<std::monostate> on_turn_end(TurnView turn, EffectContext& ctx) {
        for (auto& contributor : contributors_) co_await contributor.on_turn_end(turn, ctx);
        co_return std::monostate{};
    }

private:
    std::vector<ContextProviderDescriptor> contributors_;
};

}  // namespace agentengine
