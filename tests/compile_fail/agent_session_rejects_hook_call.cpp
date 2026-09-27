// ADR-199 §8 (red team MINOR "reachable internals"): the six hooks AgentSessionCore's loop calls are
// private in both the core and AgentSession, and the template is the core's only friend. Nothing outside
// may call one -- `bound_model_route()` decides the ADR-034/ADR-036 warnings and whether a failed stream
// is retried. This file must NOT compile; its positive control, agent_session_sealed_positive_control.cpp,
// is identical minus the hook call and must.

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
    return session.bound_model_route() == agentengine::rt::model_route::direct ? 1 : 0;  // the forbidden line
}
