// ADR-199 §8 (red team MINOR "reachable internals"): AgentSession is `final`. Before that fix, a class
// derived from it could re-override the core's hooks -- for example `run_model_call`, to skip the media
// gate -- or form a member pointer to core state such as `unattended_by_`. Host code is trusted, so this
// is surface, not an I2/I3 break, but the red team's probe was never checked in: nothing stopped the
// `final` from being dropped again. This file must NOT compile; its positive control,
// agent_session_sealed_positive_control.cpp, is identical minus the derived class and must.

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

struct DerivedSession : Session {};  // the forbidden line

}  // namespace

int main() {
    DerivedSession session;
    return session.has_chat_client() ? 1 : 0;
}
