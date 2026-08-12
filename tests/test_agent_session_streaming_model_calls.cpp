// Proves ADR-034's opt-in streaming turn loop (`AgentSession::set_stream_model_calls(true)`,
// `run_model_call()` in agent_session.hpp) — deterministic, offline, no live model, no network. A
// scripted `ChatClientT` test double with a REAL `chat_stream()` (built on `core/stream.hpp`'s real
// `make_stream<T>`, the same primitive `tests/test_chat_client_stream.cpp` proves against a real
// cross-thread producer) drives every scenario.
//
// Covers, one case per block in `main()`:
//   S1 — a streamed, no-tool-call response: model_delta events fire once per Text delta pushed, in
//        order; the final reconstructed Message's text_of() matches the concatenation; usage is
//        captured and folded into run_tokens_consumed_ exactly like the non-streaming path.
//   S2 — a streamed tool-call round trip: round 1 streams a Text delta plus an assembled ToolCall
//        delta (the real backend shape — a tool call is emitted whole, never incrementally); the
//        tool is invoked for real; round 2 streams the final answer. Proves the multi-round loop
//        still works, unmodified, driven entirely through the streaming path.
//   S3 — a stream whose terminal update carries no Usage: the run fails closed (the ask never
//        resolves) rather than silently treating the call as zero-cost against the token budget.
//   S4 — set_stream_model_calls()/stream_model_calls() round-trips and defaults false.

#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/trust/principal.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

// ---- A tool, for S2's round trip ---------------------------------------------------------------

struct EchoArgs { int value = 0; };
AE_JSON_SCHEMA(EchoArgs, value)
struct EchoReply { int value = 0; };
AE_JSON_SCHEMA(EchoReply, value)

struct EchoTool : ae::Tool<EchoTool, ae::Capabilities<>, ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name = "echo_tool";
    static constexpr std::string_view description = "Echoes its integer argument back.";
    using Args = EchoArgs;
    using Reply = EchoReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{a.value}; }
};

class ToolLoopHistoryProvider {
public:
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc,
                                                                              ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ae::ToolTable::from_tools<EchoTool>().descriptors();
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};
static_assert(ae::ContextProvider<ToolLoopHistoryProvider>);

// ---- A scripted, REAL streaming ChatClientT ----------------------------------------------------
// Each `round` is a list of ChatResponseUpdate to push, in order, synchronously (the ring is sized
// generously enough that a small scripted round never blocks on credit — no background thread
// needed for a handful of test deltas, unlike test_chat_client_stream.cpp's own real-backpressure
// proof, which is a different property this file isn't re-proving).
class ScriptedStreamingChatClient {
public:
    std::vector<std::vector<ae::ChatResponseUpdate>> rounds;  // consumed in order, one per chat_stream() call
    std::size_t call_count = 0;

    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        co_return std::unexpected(ae::error{ae::failure_class::contract,
                                              "this fixture only implements chat_stream()", "test.no_chat"});
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        if (call_count < rounds.size()) {
            for (auto const& upd : rounds[call_count]) {
                auto pushed = pair.producer.push(upd);
                (void)pushed;
            }
        }
        pair.producer.close();
        ++call_count;
        return std::move(pair.consumer);
    }
};
static_assert(ae::ChatClient<ScriptedStreamingChatClient>);

[[nodiscard]] ae::ChatResponseUpdate text_delta(std::string text, bool is_final = false,
                                                  std::optional<ae::Usage> usage = std::nullopt) {
    ae::ChatResponseUpdate upd;
    upd.delta.origin = ae::content_origin::assistant;
    upd.delta.value  = ae::Text{std::move(text)};
    upd.is_final     = is_final;
    upd.usage        = usage;
    return upd;
}

[[nodiscard]] ae::ChatResponseUpdate tool_call_delta(std::string call_id, std::string tool_name,
                                                       std::string args_json, bool is_final = false,
                                                       std::optional<ae::Usage> usage = std::nullopt) {
    ae::ChatResponseUpdate upd;
    upd.delta.origin = ae::content_origin::assistant;
    upd.delta.value  = ae::ToolCall{std::move(call_id), std::move(tool_name), std::move(args_json),
                                     ae::content_origin::assistant, ae::call_provenance::vendor_structured};
    upd.is_final = is_final;
    upd.usage    = usage;
    return upd;
}

[[nodiscard]] ae::Message user_message(std::string text) {
    ae::Message m;
    m.role = ae::role::user;
    ae::ContentItem item;
    item.origin = ae::content_origin::user;
    item.value = ae::Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

using Session = ae::AgentSession<ScriptedStreamingChatClient, ae::NoSessionState, ToolLoopHistoryProvider>;

}  // namespace

int main() {
    // ---- S1: streamed, no-tool-call response ---------------------------------------------------
    {
        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.rounds = {{
            text_delta("Hello"),
            text_delta(", world"),
            text_delta("!", /*is_final=*/true, ae::Usage{5, 7, 0, 0, 0.0}),
        }};
        kit.actor().initialize("s-s1", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);
        kit.actor().set_stream_model_calls(true);
        AE_CHECK(kit.actor().stream_model_calls(), "S1: streaming is engaged");

        auto viewer = kit.actor().enable_event_stream(std::pmr::get_default_resource());
        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("hi")});
        AE_CHECK(r.has_value(), "S1: the streamed run converges");
        if (r.has_value()) {
            AE_CHECK(text_of(r->message) == "Hello, world!",
                     "S1: the reconstructed Message's text matches every pushed delta, concatenated "
                     "in order -- text_of() being count-agnostic across content items is what makes "
                     "this equivalent to the non-streaming parse");
            AE_CHECK(r->usage.input_tokens == 5 && r->usage.output_tokens == 7,
                     "S1: usage from the terminal update reached the final AgentResponse");
        }
        AE_CHECK(kit.actor().history().size() == 2,
                 "S1: the streamed response was pushed to history exactly like a non-streamed one");

        std::vector<ae::RunEvent> events;
        while (auto ev = viewer.next()) events.push_back(std::move(*ev));
        std::size_t delta_count = 0;
        std::string joined_deltas;
        for (auto const& ev : events) {
            if (ev.kind == ae::run_event_kind::model_delta) {
                ++delta_count;
                joined_deltas += std::get<ae::run_event_payload::ModelDelta>(ev.payload).text_delta;
            }
        }
        AE_CHECK(delta_count == 3, "S1: exactly one model_delta event fired per pushed Text delta");
        AE_CHECK(joined_deltas == "Hello, world!",
                 "S1: the model_delta events, joined in emission order, reconstruct the same text");
    }

    // ---- S2: streamed tool-call round trip -------------------------------------------------------
    {
        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.rounds = {
            {
                text_delta("Let me check. "),
                tool_call_delta("c1", "echo_tool", R"({"value":42})", /*is_final=*/true,
                                 ae::Usage{3, 4, 0, 0, 0.0}),
            },
            {
                text_delta("The value is 42.", /*is_final=*/true, ae::Usage{6, 5, 0, 0, 0.0}),
            },
        };
        kit.actor().initialize("s-s2", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);
        kit.actor().set_stream_model_calls(true);

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("echo 42")});
        AE_CHECK(r.has_value(), "S2: the streamed multi-round tool call converges");
        if (r.has_value()) {
            AE_CHECK(text_of(r->message) == "The value is 42.",
                     "S2: the second round's streamed text is the final answer");
        }
        AE_CHECK(client.call_count == 2,
                 "S2: exactly 2 streamed model calls happened -- the tool round and the converging round");
        AE_CHECK(kit.actor().history().size() == 4,
                 "S2: history holds input, the tool-call round's Message, the folded tool result, and "
                 "the final round's Message");
    }

    // ---- S3: no usage on the terminal update -- fails closed, never silently zero-cost -----------
    {
        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.rounds = {{
            text_delta("hi", /*is_final=*/true, /*usage=*/std::nullopt),
        }};
        kit.actor().initialize("s-s3", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);
        kit.actor().set_stream_model_calls(true);

        auto viewer = kit.actor().enable_event_stream(std::pmr::get_default_resource());
        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("hi")});
        AE_CHECK(!r.has_value(),
                 "S3: a streamed call with no reported usage never resolves the ask -- fail-closed, "
                 "the same shape every other unresolved branch in this loop already uses, never "
                 "silently treated as a free call");

        bool saw_usage_failure_message = false;
        while (auto ev = viewer.next()) {
            if (ev->kind == ae::run_event_kind::run_failed) {
                auto const& p = std::get<ae::run_event_payload::RunFailed>(ev->payload);
                if (p.message.find("usage") != std::string::npos) saw_usage_failure_message = true;
            }
        }
        AE_CHECK(saw_usage_failure_message,
                 "S3: the run_failed event's message names the real reason (missing usage), not a "
                 "generic, indistinguishable failure");
    }

    // ---- S4: the accessor pair defaults false and round-trips -------------------------------------
    {
        quark::TestKit<Session> kit;
        kit.actor().emplace_chat_client();
        kit.actor().initialize("s-s4", ae::Principal{"p", ""});
        AE_CHECK(!kit.actor().stream_model_calls(), "S4: streaming defaults false");
        kit.actor().set_stream_model_calls(true);
        AE_CHECK(kit.actor().stream_model_calls(), "S4: set_stream_model_calls(true) engages it");
        kit.actor().set_stream_model_calls(false);
        AE_CHECK(!kit.actor().stream_model_calls(), "S4: set_stream_model_calls(false) disengages it");
    }

    std::cout << (g_failures == 0 ? "test_agent_session_streaming_model_calls: OK\n"
                                   : "test_agent_session_streaming_model_calls: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
