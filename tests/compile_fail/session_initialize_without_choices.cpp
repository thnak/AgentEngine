// This file MUST NOT compile (decisions/ADR-244-required-choices-step-1b.md §2.2, G-C1). `initialize()` has no
// overload without a `SessionChoices`: a session whose authority source, token budget and turn cap nobody stated
// would run under whatever the engine picked. If this ever compiles, that implicit choice is back.

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
    agentengine::rt::AgentSession<NoClient> session;
    session.initialize("s", agentengine::Principal{"p", ""});  // no SessionChoices
    return 0;
}
