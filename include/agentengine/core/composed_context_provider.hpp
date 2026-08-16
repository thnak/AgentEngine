#pragma once
// Generalizes history_and_skills_provider.hpp's `HistoryAndSkillsProvider<H,S>` -- a hand-written,
// fixed TWO-provider composite -- to an arbitrary `Ms...` pack, all composed through the same real,
// already-proven `core/context_assembly.hpp::assemble_context()`. That file's own top comment names
// exactly this shape ("a purpose-built composite ContextProvider that owns its sub-providers
// directly ... see history_and_skills_provider.hpp's HistoryAndSkillsProvider for the proven shape")
// as the answer for a caller needing more than one contributor in `AgentSession`'s single
// `ContextProvider` template slot, without widening `AgentSession` itself (that wider change --
// `AgentSession` natively taking a provider pack and calling `assemble_context` inside its own turn
// loop -- stays deferred, named in context_assembly.hpp/memory_provider.hpp as later, separately-
// scoped "Phase G" work touching a large, mature, heavily tested file). This type is that named
// answer made generic instead of hand-duplicated per combination -- `AgentSession<ChatClientT,
// StateT, ComposedContextProvider<HistoryProvider<Window<0>>, SkillsProvider, MemoryProvider>>` now
// plugs N real contributors into the existing slot exactly the way `HistoryAndSkillsProvider` always
// plugged in exactly two.

#include <array>
#include <concepts>
#include <cstddef>
#include <tuple>
#include <utility>
#include <vector>

#include "agentengine/core/context_assembly.hpp"
#include "agentengine/core/context_provider.hpp"

namespace agentengine {

// `Ms...` in declared order == `assemble_context`'s own contributor order (`contributors_[0]`
// first) -- 005 §3's drop-order-determinism rule, and (see history_and_skills_provider.hpp's own
// file-top comment on why skills had to go before history on the wire) the final wire-message order,
// both follow directly from that, same as the two-provider original.
template <class... Ms>
    requires (sizeof...(Ms) >= 1) && (ContextProvider<Ms> && ...)
// ae-naming-lint: allow ComposedContextProvider — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class ComposedContextProvider {
public:
    // Default-constructible only when every Ms is -- the same AgentSession-slot constraint
    // history_and_skills_provider.hpp's own comment documents: `AgentSession<...>::history_provider_`
    // is a plain, default-constructed value member with no emplace_*/accessor pair to reach in and
    // configure it after construction, so whatever occupies that slot must itself be default-
    // constructible to be declared there at all.
    ComposedContextProvider()
        requires (std::default_initializable<Ms> && ...)
        : ComposedContextProvider(std::tuple<Ms...>{Ms{}...}) {}

    // `providers` as an explicit `std::tuple<Ms...>`, not a trailing pack, is what lets `budgets`
    // follow it -- the identical reason `core/model_call_gateway.hpp`'s `ModelCallGateway` takes its
    // own `Fallback...` as `std::tuple<Fallback...>` rather than a second pack (a class template may
    // have only one trailing parameter pack, and a pack must be the LAST constructor parameter for
    // positional construction to work at all). `budgets` defaults to `{}` -- every `ContextBudget`
    // default-constructs to `max_tokens == 0` (unbounded), matching `HistoryAndSkillsProvider`'s own
    // "unbounded by default, a caller may still opt into a real cap" rule.
    explicit ComposedContextProvider(std::tuple<Ms...> providers,
                                      std::array<ContextBudget, sizeof...(Ms)> budgets = {}) {
        contributors_.reserve(sizeof...(Ms));
        build_contributors(providers, budgets, std::index_sequence_for<Ms...>{});
    }

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx,
                                                                 EffectContext& ctx) {
        ContextAssemblyResult assembled = co_await assemble_context(contributors_, session_ctx, ctx);
        co_return assembled.combined;
    }

    // `assemble_context` itself never calls `on_turn_end` on its contributors (context_assembly.hpp's
    // own function only ever invokes `on_context`) -- forwarded manually here to every wrapped
    // provider, the same reason `history_and_skills_provider.hpp` forwards it for its own two.
    task<std::monostate> on_turn_end(TurnView turn, EffectContext& ctx) {
        for (auto& contributor : contributors_) (void)co_await contributor.on_turn_end(turn, ctx);
        co_return std::monostate{};
    }

private:
    template <std::size_t... I>
    void build_contributors(std::tuple<Ms...>& providers,
                             std::array<ContextBudget, sizeof...(Ms)> const& budgets,
                             std::index_sequence<I...>) {
        (contributors_.push_back(
             make_context_provider_descriptor(std::move(std::get<I>(providers)), budgets[I])),
         ...);
    }

    std::vector<ContextProviderDescriptor> contributors_;
};

}  // namespace agentengine
