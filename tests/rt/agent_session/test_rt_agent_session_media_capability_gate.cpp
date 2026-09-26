// Gap-audit finding 19, Phase 1 fix (2026-08-14): proves the pure-function gate
// (`validate_outbound_media_capabilities`, tests/test_outbound_media_capability_gate.cpp) is
// actually WIRED into `AgentSession::run_model_call()`, not merely defined and unused --
// end-to-end, a real run whose history carries `Media` content the bound backend hasn't declared
// support for fails closed with the real error, and the backend's own `chat()` is NEVER called at
// all (the whole point: fail BEFORE a request that would silently drop the content ever reaches a
// backend, not after).

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::StartRun;
using agentengine::task;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::ContextContribution;
using agentengine::ContextProvider;
using agentengine::EffectContext;
using agentengine::Media;
using agentengine::Message;
using agentengine::Principal;
using agentengine::SessionContext;
using agentengine::Text;
using agentengine::TurnView;
using agentengine::Usage;
using agentengine::content_origin;
using agentengine::role;

class RecordingChatClient {
public:
    // Defaults all-false (no multimodal_in_* bit declared, a plain text-only backend); a test that
    // needs the positive control sets `declared_caps` before starting a run.
    RecordingChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<ChatRequest> requests;
    };

    ChatClientCapabilities declared_caps{};

    [[nodiscard]] ChatClientCapabilities capabilities() const { return declared_caps; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest req, EffectContext&) {
        state_->requests.push_back(req);
        Message m;
        m.role = role::assistant;
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value  = Text{"ok"};
        m.content.push_back(item);
        co_return ChatResponse{m, Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

    [[nodiscard]] std::vector<ChatRequest> const& requests() const { return state_->requests; }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<RecordingChatClient>);

struct FixedSeedProvider {
    std::vector<Message> seed;

    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages = seed;
        c.messages.insert(c.messages.end(), sc.history.begin(), sc.history.end());
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<FixedSeedProvider>);

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message image_message(std::string message_id) {
    Message m;
    m.role       = role::user;
    m.message_id = std::move(message_id);
    ContentItem item;
    item.origin = content_origin::user;
    Media media;
    media.media_type = "image/png";
    media.payload    = std::vector<std::byte>{};
    item.value       = std::move(media);
    m.content.push_back(std::move(item));
    return m;
}

}  // namespace

int main() {
    // T1: history carries an image the bound backend never declared support for -> the run fails
    // closed, and the backend's own chat() is NEVER invoked at all.
    {
        AgentSession<RecordingChatClient, NoSessionState, FixedSeedProvider> session;
        session.initialize("t1", Principal{"p1", ""});
        session.history_provider().seed = {image_message("m-image")};
        RecordingChatClient& client = session.emplace_chat_client();

        auto outcome = drive(session.start_run(StartRun{user_message("describe this")}));
        check(!outcome.has_value(),
              "T1: the run fails closed -- an image/png Media item with no declared "
              "multimodal_in_image capability is refused before ever reaching the backend");
        check(!outcome.has_value() &&
                  outcome.error().code == "chat_client.multimodal_capability_missing",
              "T1: the real, attributable error code surfaces to the caller");
        check(client.requests().empty(),
              "T1: chat() was NEVER called -- the gate fires strictly before the backend sees the "
              "request, so nothing was silently sent with the image quietly dropped");
    }

    // T2 (positive control): the IDENTICAL history succeeds once the bound backend actually
    // declares multimodal_in_image -- proves T1's failure was real capability gating, not an
    // unconditional rejection of any Media content.
    {
        AgentSession<RecordingChatClient, NoSessionState, FixedSeedProvider> session;
        session.initialize("t2", Principal{"p1", ""});
        session.history_provider().seed = {image_message("m-image")};
        RecordingChatClient& client = session.emplace_chat_client();
        client.declared_caps.multimodal_in_image = true;

        auto outcome = drive(session.start_run(StartRun{user_message("describe this")}));
        check(outcome.has_value(),
              "T2 (positive control): with multimodal_in_image declared, the SAME image content "
              "reaches the backend and the run converges normally");
        check(client.requests().size() == 1,
              "T2: chat() WAS called exactly once this time -- the gate let a genuinely-supported "
              "request through unmodified");
    }

    std::printf(g_failures == 0 ? "test_rt_agent_session_media_capability_gate: OK\n"
                                 : "test_rt_agent_session_media_capability_gate: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
