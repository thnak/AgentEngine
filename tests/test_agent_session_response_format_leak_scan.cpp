// Proves ADR-035 Phase 1's backend/path-agnostic response-format-leak-scan:
// `AgentSession::set_scan_response_format_leaks(true)` applies `apply_response_format_scan()`
// (core/response_format_leak_scan.hpp) once per round, inside `run_model_call()`, to the
// reconstructed `Message` -- regardless of whether the round streamed or called `chat()`. Previously
// this protection existed ONLY as `OpenAIChatClient`'s own `scan_response_format_leaks` constructor
// flag, reachable solely through that backend's non-streaming `chat()`.
//
// Deterministic, offline, no live model, no network -- a scripted `ChatClientT` test double (same
// shape as test_agent_session_streaming_model_calls.cpp's `ScriptedStreamingChatClient`, extended
// with a real, scriptable `chat()` too) drives every scenario.
//
// Covers, one case per block in `main()`:
//   L1 -- streaming path, scan armed: a Harmony-leaked commentary block naming a KNOWN tool is
//         promoted to a real, text_derived ToolCall and actually invoked.
//   L2 -- streaming path, scan NOT armed (default false): the identical leaked text passes through
//         completely unchanged, as plain (tainted=false) Text -- proving the flag is genuinely
//         off by default and genuinely gates the behavior, not merely decorative.
//   L3 -- non-streaming (chat()) path, scan armed via AgentSession (NOT via OpenAIChatClient's own
//         flag, which this fixture doesn't even have): the same promotion happens -- proving the
//         scan is truly path-agnostic, not just a streaming-path add-on.
//   L4 -- double-scan idempotence regression (the red-team's must-fix #1): calling
//         `apply_response_format_scan` twice in a row on the SAME message never promotes a SECOND,
//         different candidate out of the first pass's own tainted diagnostic text -- proving the
//         `tainted`-skip fix actually closes the gap, not merely that the happy path still works.

#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/response_format_leak_scan.hpp"
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

// A Harmony-shaped commentary block naming a REAL declared tool ("get_weather"), leaked into plain
// content exactly the way a serving layer that doesn't normalize Harmony would emit it (same shape
// test_response_format_codec.cpp's own G1 cases use).
std::string const kHarmonyLeak =
    "Let me check that. "
    "<|start|>assistant<|channel|>commentary to=functions.get_weather <|constrain|>json"
    "<|message|>{\"location\":\"Seattle\"}<|call|>";

struct WeatherArgs { std::string location; };
AE_JSON_SCHEMA(WeatherArgs, location)
struct WeatherReply { std::string forecast; };
AE_JSON_SCHEMA(WeatherReply, forecast)

struct GetWeatherTool
    : ae::Tool<GetWeatherTool, ae::Capabilities<>, ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name = "get_weather";
    static constexpr std::string_view description = "Looks up the weather for a location.";
    using Args = WeatherArgs;
    using Reply = WeatherReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{"sunny in " + a.location}; }
};

class ToolLoopHistoryProvider {
public:
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc,
                                                                              ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ae::ToolTable::from_tools<GetWeatherTool>().descriptors();
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};
static_assert(ae::ContextProvider<ToolLoopHistoryProvider>);

// A scripted ChatClientT with a REAL chat_stream() (L1/L2, same shape as
// test_agent_session_streaming_model_calls.cpp's own fixture) AND a real, independently scriptable
// chat() (L3) -- so the same fixture proves both paths without two separate test doubles that could
// drift apart.
class ScriptedChatClient {
public:
    std::vector<std::vector<ae::ChatResponseUpdate>> stream_rounds;
    std::vector<ae::ChatResponse> chat_rounds;
    std::size_t stream_call_count = 0;
    std::size_t chat_call_count = 0;

    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        if (chat_call_count >= chat_rounds.size()) {
            co_return std::unexpected(
                ae::error{ae::failure_class::contract, "no more scripted chat() rounds", "test.no_chat"});
        }
        co_return chat_rounds[chat_call_count++];
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        if (stream_call_count < stream_rounds.size()) {
            for (auto const& upd : stream_rounds[stream_call_count]) {
                auto pushed = pair.producer.push(upd);
                (void)pushed;
            }
        }
        pair.producer.close();
        ++stream_call_count;
        return std::move(pair.consumer);
    }
};
static_assert(ae::ChatClient<ScriptedChatClient>);

[[nodiscard]] ae::ChatResponseUpdate text_delta(std::string text, bool is_final = false,
                                                  std::optional<ae::Usage> usage = std::nullopt) {
    ae::ChatResponseUpdate upd;
    upd.delta.origin = ae::content_origin::assistant;
    upd.delta.value  = ae::Text{std::move(text)};
    upd.is_final     = is_final;
    upd.usage        = usage;
    return upd;
}

[[nodiscard]] ae::Message text_message(std::string text) {
    ae::Message m;
    m.role = ae::role::assistant;
    ae::ContentItem item;
    item.origin = ae::content_origin::assistant;
    item.value = ae::Text{std::move(text)};
    item.tainted = false;
    m.content.push_back(item);
    return m;
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

using Session = ae::AgentSession<ScriptedChatClient, ae::NoSessionState, ToolLoopHistoryProvider>;

}  // namespace

int main() {
    // ---- L1: streaming path, scan armed -- the leaked Harmony block is promoted and invoked -------
    {
        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.stream_rounds = {
            {text_delta(kHarmonyLeak, /*is_final=*/true, ae::Usage{3, 4, 0, 0, 0.0})},
            {text_delta("It's sunny in Seattle.", /*is_final=*/true, ae::Usage{5, 3, 0, 0, 0.0})},
        };
        kit.actor().initialize("s-l1", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);
        kit.actor().set_stream_model_calls(true);
        kit.actor().set_scan_response_format_leaks(true);
        AE_CHECK(kit.actor().scan_response_format_leaks(), "L1: the new flag reads back armed");

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("what's the weather?")});
        AE_CHECK(r.has_value(), "L1: the run converges");
        if (r.has_value()) {
            AE_CHECK(text_of(r->message) == "It's sunny in Seattle.",
                     "L1: the final round's text is the converged answer, proving the promoted "
                     "text_derived call from round 1 was actually invoked and folded back in");
        }
        AE_CHECK(client.stream_call_count == 2,
                 "L1: exactly 2 streamed rounds happened -- the leaked-tool-call round and the "
                 "converging round, same shape as an ordinary vendor_structured tool round trip");
    }

    // ---- L2: streaming path, scan NOT armed (default) -- the leak passes through unchanged --------
    {
        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.stream_rounds = {
            {text_delta(kHarmonyLeak, /*is_final=*/true, ae::Usage{3, 4, 0, 0, 0.0})},
        };
        kit.actor().initialize("s-l2", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);
        kit.actor().set_stream_model_calls(true);
        AE_CHECK(!kit.actor().scan_response_format_leaks(),
                 "L2: the new flag defaults false -- ADR-023 Finding 6, operator-armed, never "
                 "content-triggered");

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("what's the weather?")});
        AE_CHECK(r.has_value(), "L2: the run converges (no tool call ever gets recognized/invoked)");
        if (r.has_value()) {
            AE_CHECK(text_of(r->message) == kHarmonyLeak,
                     "L2: with scanning off, the raw leaked Harmony text reaches the final Message "
                     "completely unchanged, byte for byte -- proving the flag genuinely gates the "
                     "behavior rather than the scan running unconditionally regardless of it");
        }
        AE_CHECK(client.stream_call_count == 1,
                 "L2: only 1 round happened -- with no promotion, there is no tool call to invoke, "
                 "so the loop converges immediately on the (unscanned) text answer");
    }

    // ---- L3: non-streaming (chat()) path, scan armed via AgentSession -- proves path-agnosticism --
    {
        quark::TestKit<Session> kit;
        auto& client = kit.actor().emplace_chat_client();
        client.chat_rounds = {
            ae::ChatResponse{text_message(kHarmonyLeak), ae::Usage{3, 4, 0, 0, 0.0}},
            ae::ChatResponse{text_message("It's sunny in Seattle."), ae::Usage{5, 3, 0, 0, 0.0}},
        };
        kit.actor().initialize("s-l3", ae::Principal{"p", ""});
        ae::CapabilitySet const held = ae::CapabilitySet::grant_root({});
        kit.actor().set_capabilities(&held);
        // Deliberately NOT calling set_stream_model_calls() -- this fixture has no OpenAIChatClient
        // and thus no scan_response_format_leaks flag of its own; the ONLY way this scenario can
        // possibly promote the leaked call is through AgentSession's own flag, on the chat() path.
        kit.actor().set_scan_response_format_leaks(true);

        auto r = kit.ask<ae::AgentResponse>(ae::StartRun{user_message("what's the weather?")});
        AE_CHECK(r.has_value(), "L3: the non-streaming run converges");
        if (r.has_value()) {
            AE_CHECK(text_of(r->message) == "It's sunny in Seattle.",
                     "L3: the promoted call was invoked on the chat() path too -- the scan is "
                     "applied uniformly by run_model_call() regardless of stream_model_calls_");
        }
        AE_CHECK(client.chat_call_count == 2,
                 "L3: exactly 2 chat() rounds happened -- the leaked-tool-call round and the "
                 "converging round");
    }

    // ---- L4: double-scan idempotence -- a diagnostic's own text never re-triggers a promotion ------
    {
        // A DIFFERENT tool ("real_tool") is declared this round than the one named in the leak
        // ("decoy_tool") -- so the FIRST pass leaves it as an inert, tainted diagnostic Text (Phase
        // 1 behavior: unrecognized recipient, not promoted). The diagnostic's own body then embeds
        // the recipient name verbatim: "[unrecognized tool-call attempt, not executed: decoy_tool(...)]".
        // If a SECOND pass ever re-decoded that diagnostic and somehow produced a promotable
        // candidate from it, that would be the exact laundering path the red-team found -- proven
        // absent here by asserting the message is bit-for-bit identical after a second scan.
        std::string const leak =
            "<|start|>assistant<|channel|>commentary to=functions.decoy_tool <|constrain|>json"
            "<|message|>{\"x\":1}<|call|>";
        std::vector<ae::ToolDescriptor> real_tool_only;
        {
            ae::ToolDescriptor d;
            d.name = "real_tool";
            real_tool_only.push_back(d);
        }
        ae::Message once = ae::apply_response_format_scan(text_message(leak), real_tool_only);
        bool any_promoted_first_pass = false;
        for (auto const& item : once.content) {
            if (std::holds_alternative<ae::ToolCall>(item.value)) any_promoted_first_pass = true;
        }
        AE_CHECK(!any_promoted_first_pass,
                 "L4 setup: with only 'real_tool' declared, the leak naming 'decoy_tool' stays an "
                 "inert diagnostic after the FIRST pass -- unrecognized recipient, per Phase 1");

        ae::Message twice = ae::apply_response_format_scan(once, real_tool_only);
        AE_CHECK(once.content.size() == twice.content.size() && once == twice,
                 "L4: a second scan of the SAME message (as happens when both OpenAIChatClient's own "
                 "flag and AgentSession's flag are armed together) is a byte-for-byte no-op -- the "
                 "tainted diagnostic from pass 1 is skipped whole, not re-decoded and re-promoted");
    }

    std::cout << (g_failures == 0 ? "test_agent_session_response_format_leak_scan: OK\n"
                                   : "test_agent_session_response_format_leak_scan: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
