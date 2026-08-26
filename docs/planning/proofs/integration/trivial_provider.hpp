#pragma once
// A second, trivially real ContextProvider -- proves SandboxReflector composes into a real
// ComposedContextProvider<Ms...> ALONGSIDE another ordinary contributor, with no special-casing, no
// coupling, matching this design's own §9 principle (an ordinary fan-out member, nothing more) and
// OQ-18's already-judged rejection of a chained/coupled ContextProvider pipeline.

#include "agentengine/core/context_provider.hpp"

namespace probe {

class TrivialInstructionsProvider {
public:
    static constexpr std::string_view name = "trivial_instructions";

    agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext&, agentengine::EffectContext&) {
        agentengine::ContextContribution contribution;
        contribution.instructions = agentengine::TaintedText{"Be concise."};
        co_return contribution;
    }
    agentengine::task<std::monostate> on_turn_end(agentengine::TurnView, agentengine::EffectContext&) {
        co_return std::monostate{};
    }
};

}  // namespace probe
