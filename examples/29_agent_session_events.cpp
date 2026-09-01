// AgentEngine "get started" examples, 29 -- watching a run live via AgentSession's real event stream.
//
// 07_streaming.cpp shows the primitive underneath: a bare `ChatClient::chat_stream()` handed back a
// `stream<ChatResponseUpdate>` to drain by hand. This example shows the layer app code actually talks
// to: `agentengine::rt::AgentSession` opts a session into streamed model calls with
// `set_stream_model_calls(true)` (ADR-034), then `enable_event_stream()` (Milestone 7 Phase A, 013
// §1) hands back a `stream<RunEvent>` that reports the WHOLE turn lifecycle -- not just model text,
// but `run_started`/`turn_started`/`model_call_started`/`model_delta`(*)/`model_call_finished`/
// `turn_finished`/`run_finished` -- as one real, ordered, per-run sequence (`RunEvent::seq`, 1-based,
// reset on every new run). A streamed run also emits one `run_event_kind::warning` (a real, distinct
// kind -- not a fabricated placeholder) right after `run_started`, since engaging
// `stream_model_calls_` is itself an operator-visible choice worth surfacing on the event stream.
// This is the exact mechanism an AG-UI/A2A/SSE bridge (see the Events API page) projects onto its
// own wire format; nothing about this event stream is bridge-specific.
//
// Mirrors tests/test_rt_agent_session_streaming_and_events.cpp's S1 (streamed deltas -> model_delta
// events) and A2 (the full non-streaming success-path sequence), combined into one narrative program.
// Fully offline: the scripted ChatClientT below pushes its whole reply synchronously, no background
// thread, no network.
//
// Run: ./agentengine_example_29_agent_session_events

#include <cstdio>
#include <memory_resource>
#include <string>
#include <vector>

#include "agentengine/rt/agent_session.hpp"

using namespace agentengine;
using agentengine::rt::AgentSession;
using agentengine::rt::NoSessionState;
using agentengine::rt::StartRun;

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

// Drives an rt::task<T> that never genuinely parks -- the same "safe here" driver every offline
// rt:: example/test in this repo uses, since ScriptedChatClient::chat_stream() below pushes its
// whole scripted reply before returning.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// A real ChatClient conformer that streams three word-deltas then closes -- same shape as
// 07_streaming.cpp's fixture, but synchronous (no producer thread) since this example's point is the
// event stream, not cross-thread backpressure.
class ScriptedChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        co_return std::unexpected(
            error{failure_class::fatal, "unused -- this example only drives chat_stream()", "unused"});
    }

    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 8;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);

        auto push_text = [&](std::string text, bool is_final, std::optional<Usage> usage) {
            ChatResponseUpdate upd;
            upd.delta.origin = content_origin::assistant;
            upd.delta.value  = Text{std::move(text)};
            upd.is_final     = is_final;
            upd.usage        = usage;
            (void)pair.producer.push(std::move(upd));
        };
        push_text("Hello", false, std::nullopt);
        push_text(", world", false, std::nullopt);
        push_text("!", true, Usage{5, 7, 0, 0, 0.0});
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<ScriptedChatClient>);

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

[[nodiscard]] char const* event_kind_name(run_event_kind k) {
    switch (k) {
        case run_event_kind::run_started:          return "run_started";
        case run_event_kind::turn_started:         return "turn_started";
        case run_event_kind::model_call_started:   return "model_call_started";
        case run_event_kind::model_delta:          return "model_delta";
        case run_event_kind::model_call_finished:  return "model_call_finished";
        case run_event_kind::turn_finished:        return "turn_finished";
        case run_event_kind::run_finished:         return "run_finished";
        case run_event_kind::warning:
            return "warning";  // fires once per streamed run -- see this file's own header comment
        default:                                   return "other";
    }
}

}  // namespace

int main() {
    AgentSession<ScriptedChatClient> session;
    session.initialize("s-events-demo", Principal{"p-demo", ""});
    CapabilitySet const held = CapabilitySet::grant_root({});
    session.set_capabilities(&held);

    // The one flag that engages the streaming turn loop (ADR-034) -- without it, run_model_call()
    // dispatches to the plain chat() method instead, and model_delta never fires.
    session.set_stream_model_calls(true);

    // Subscribed BEFORE start_run(): enable_event_stream() must be live when the run happens, or
    // there is nothing to attach events to (see A5 in the mirrored test for the admission-denied
    // case, where no run_id is even minted).
    stream<RunEvent> events = session.enable_event_stream(std::pmr::get_default_resource());

    auto result = drive(session.start_run(StartRun{user_message("hi")}));
    check(result.has_value(), "the streamed run converges to a real AgentResponse");

    // The event stream stays open for the session's whole lifetime (a later run would append more
    // events, still numbered from seq 1 -- see A3 in the mirrored test), so draining it is just
    // "take whatever is already buffered," not "wait for it to close" the way a single chat_stream()
    // call is.
    std::string joined_deltas;
    std::size_t delta_count = 0;
    while (auto ev = events.next()) {
        std::fprintf(stderr, "  event seq=%llu kind=%s\n",
                      static_cast<unsigned long long>(ev->seq), event_kind_name(ev->kind));
        if (ev->kind == run_event_kind::model_delta) {
            ++delta_count;
            auto const& d = std::get<run_event_payload::ModelDelta>(ev->payload);
            if (auto const* t = std::get_if<run_event_payload::ModelTextDelta>(&d.value)) {
                joined_deltas += t->text;
            }
        }
    }

    check(delta_count == 3, "exactly one model_delta event fired per pushed Text delta");
    check(joined_deltas == "Hello, world!",
          "the model_delta events, joined in emission order, reconstruct the same text the final "
          "Message carries -- the event stream and the returned response never disagree");
    if (result.has_value()) {
        check(text_of(result->message) == joined_deltas,
              "the final AgentResponse's message matches what the live event stream already showed");
    }

    std::fprintf(stderr, g_failures == 0 ? "example_29_agent_session_events: OK\n"
                                          : "example_29_agent_session_events: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
