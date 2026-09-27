// Positive control for tests/compile_fail/agent_session_rejects_derivation.cpp and
// agent_session_rejects_hook_call.cpp (ADR-199 §8, red team MINOR "reachable internals"). Identical to
// each negative except for the one forbidden line: the same AgentSession type constructs, and its public
// surface is callable, so each negative's rejection is the forbidden line's, not the shared setup's.

#include "agentengine/core/chat_client.hpp"
#include "agentengine/rt/agent_session.hpp"

#include <memory_resource>

using namespace agentengine;

namespace {

struct DummyChatClient {
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 1;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        pair.producer.close();
        return std::move(pair.consumer);
    }
};

using Session = agentengine::rt::AgentSession<DummyChatClient>;

}  // namespace

int main() {
    Session session;
    return session.has_chat_client() ? 1 : 0;
}
