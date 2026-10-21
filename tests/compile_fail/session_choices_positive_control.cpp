// Positive control for decisions/ADR-244-required-choices-step-1b.md §2.2/§2.3 (G-C1): every required session
// choice stated, in the same shapes the four negatives below leave one out of. This MUST compile, or the
// negatives prove nothing.

#include <chrono>
#include <cstdint>

#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/session_choices.hpp"
#include "agentengine/rt/agent_session.hpp"

namespace {
struct NoClient {
    [[nodiscard]] agentengine::ChatClientCapabilities capabilities() const { return {}; }
    agentengine::task<agentengine::result<agentengine::ChatResponse>> chat(agentengine::ChatRequest,
                                                                           agentengine::EffectContext&) {
        co_return std::unexpected(agentengine::error{agentengine::failure_class::fatal, "unused", "unused"});
    }
    [[nodiscard]] agentengine::stream<agentengine::ChatResponseUpdate> chat_stream(agentengine::ChatRequest,
                                                                                   agentengine::EffectContext&) {
        return {};
    }
};
}  // namespace

int main() {
    using B = agentengine::Bound<std::uint64_t>;
    agentengine::SessionChoices const choices{agentengine::AuthoritySource::single_trusted_caller(), B::unbounded(),
                                              B::at_most(8)};
    agentengine::rt::AgentSession<NoClient> session;
    session.initialize("s", agentengine::Principal{"p", ""}, choices);

    agentengine::rt::RequestAuthority const authority{agentengine::Principal{"p", ""}, nullptr,
                                                      agentengine::Expiry::never()};

    agentengine::AgentSessionOptions const opts{.session_id = "s", .principal = agentengine::Principal{"p", ""},
                                                .choices = choices, .grants = {}, .chat_client_grants = {},
                                                .approve_tools = std::nullopt, .policy = {}};
    return session.choices().has_value() && authority.expiry.is_never() && !opts.session_id.empty() ? 0 : 1;
}
