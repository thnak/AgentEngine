// Proof for ADR-037 Phase 2, Slice 1: three real behavioral surfaces of agentengine::rt::AgentSession
// (include/agentengine/rt/agent_session.hpp) -- the Quark-actor-free replacement for
// agentengine::AgentSession (core/agent_session.hpp) -- ported from three OLD, Quark-actor-based test
// files onto rt::AgentSession directly. Deterministic, offline, no live model, no network throughout;
// every ChatClientT fixture below either co_returns immediately (chat()) or pushes its whole scripted
// round synchronously before closing (chat_stream(), same "generous ring, no background thread needed"
// shape the old file used -- core/stream.hpp's make_stream<T> is unaffected by ADR-037's own backend
// swap to rt::channel<T>, see that header's own banner). Each source file's own claims are proven here
// under that file's own label prefix, NOT its Quark/TestKit plumbing (which this migration retires):
//
//   S1-S4 (from test_agent_session_streaming_model_calls.cpp, ADR-034's opt-in streaming turn loop):
//     S1 -- a streamed, no-tool-call response: model_delta events fire once per Text delta pushed, in
//           order; the final reconstructed Message's text_of() matches the concatenation; usage is
//           captured and folded into the final AgentResponse exactly like the non-streaming path.
//     S2 -- a streamed tool-call round trip: round 1 streams a Text delta plus an assembled ToolCall
//           delta (the real backend shape -- a tool call is emitted whole, never incrementally); the
//           tool is invoked for real; round 2 streams the final answer.
//     S3 -- a stream whose terminal update carries no Usage fails closed (the ask returns an error,
//           never a fabricated response) rather than silently treating the call as zero-cost against
//           the token budget -- rt::AgentSession's own run_model_call() carries this exact check
//           forward (run.usage_unavailable), see agent_session.hpp lines ~920-928.
//     S4 -- set_stream_model_calls()/stream_model_calls() round-trips and defaults false.
//
//   L1-L4 (from test_agent_session_response_format_leak_scan.cpp, ADR-035 Phase 1's backend/path-
//   agnostic response-format-leak-scan):
//     L1 -- streaming path, scan armed: a Harmony-leaked commentary block naming a KNOWN tool is
//           promoted to a real, text_derived ToolCall and actually invoked.
//     L2 -- streaming path, scan NOT armed (default false): the identical leaked text passes through
//           completely unchanged, as plain (tainted=false) Text.
//     L3 -- non-streaming (chat()) path, scan armed via AgentSession: the same promotion happens,
//           proving the scan is applied uniformly by run_model_call() regardless of
//           stream_model_calls_.
//     L4 -- double-scan idempotence: calling apply_response_format_scan() twice in a row on the SAME
//           message never promotes a second, different candidate out of the first pass's own tainted
//           diagnostic text (this is a pure function of core/response_format_leak_scan.hpp, exercised
//           directly -- it has no rt::-specific surface to port).
//
//   A1-A5 (from test_agent_session_run_event_stream.cpp, Milestone 7 Phase A's real run-event stream):
//     A1 -- no enable_event_stream() call: a run proceeds exactly as before, no crash.
//     A2 -- a successful run emits the full real success-path sequence, in order, seq 1..6:
//           RunStarted/TurnStarted/ModelCallStarted/ModelCallFinished/TurnFinished/RunFinished.
//     A3 -- the sequence number resets to 1 on the SAME session's next run.
//     A4a/A4b/A4c -- RunFailed (never RunFinished) is the terminal event on each fail-closed branch
//           (chat failure, token-budget exceeded, no chat client configured), each with its real
//           error_code.
//     A5 -- an admission-denied StartRun mints no Run at all -- no event fires for it.
//
// DELIBERATELY NOT PORTED, named rather than invented a substitute for: rt::AgentSession's
// run_rounds()/start_run() emits ONE extra event kind, run_event_kind::warning, on any run that
// engages stream_model_calls_ (agent_session.hpp lines ~488-494) -- a genuinely NEW event the old
// Quark-based A2 test's "exactly 6 events" claim never had to account for (the old file's own success
// path never streamed). A2 below is therefore run WITHOUT streaming engaged (matching the old file's
// own fixture, which also never streamed), so the "exactly 6, in this exact order" claim still holds
// unmodified; S1-S3 above separately prove the streaming path's own event shape (model_delta, plus
// this file's own A2-adjacent checks would need to special-case the warning event -- not attempted
// here, since the old file never made that claim about a streaming run in the first place).

#include <cstdio>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/response_format_leak_scan.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentResponse;
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

// Same "safe here because nothing genuinely suspends externally" drive<T>() every other
// test_rt_agent_session*.cpp file uses -- every ChatClientT fixture below co_returns immediately, and
// every chat_stream() pushes its whole scripted round synchronously before closing.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::CapabilitySet;
using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::Custom;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Principal;
using agentengine::RunEvent;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::ToolDescriptor;
using agentengine::Usage;
using agentengine::call_provenance;
using agentengine::content_origin;
using agentengine::role;
using agentengine::run_event_kind;

// ---- A tool, for S2's streamed tool-call round trip ---------------------------------------------

struct EchoArgs { int value = 0; };
AE_JSON_SCHEMA(EchoArgs, value)
struct EchoReply { int value = 0; };
AE_JSON_SCHEMA(EchoReply, value)

struct EchoTool : agentengine::Tool<EchoTool, agentengine::Capabilities<>,
                                     agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "echo_tool";
    static constexpr std::string_view description = "Echoes its integer argument back.";
    using Args = EchoArgs;
    using Reply = EchoReply;
    static agentengine::result<Reply> invoke(Args a, EffectContext&) { return Reply{a.value}; }
};

// ---- A tool, for L1/L3's leaked-Harmony-block promotion -------------------------------------------

struct WeatherArgs { std::string location; };
AE_JSON_SCHEMA(WeatherArgs, location)
struct WeatherReply { std::string forecast; };
AE_JSON_SCHEMA(WeatherReply, forecast)

struct GetWeatherTool
    : agentengine::Tool<GetWeatherTool, agentengine::Capabilities<>,
                         agentengine::EffectClass<agentengine::effect_class::pure>> {
    static constexpr std::string_view name = "get_weather";
    static constexpr std::string_view description = "Looks up the weather for a location.";
    using Args = WeatherArgs;
    using Reply = WeatherReply;
    static agentengine::result<Reply> invoke(Args a, EffectContext&) {
        return Reply{"sunny in " + a.location};
    }
};

// A Harmony-shaped commentary block naming a REAL declared tool ("get_weather"), leaked into plain
// content exactly the way a serving layer that doesn't normalize Harmony would emit it (same shape
// the old file's own kHarmonyLeak used).
std::string const kHarmonyLeak =
    "Let me check that. "
    "<|start|>assistant<|channel|>commentary to=functions.get_weather <|constrain|>json"
    "<|message|>{\"location\":\"Seattle\"}<|call|>";

// A single HistoryProviderT fixture declares whichever tool table a given block needs -- one
// template, parameterized on which tool(s) to advertise, since S1-S4/L1-L3 each need a different
// (possibly empty) tool table and this project's own "no shared test helpers" convention prefers a
// local, explicit fixture over reaching for a cross-file shared one.
template <class... ToolTs>
class ToolLoopHistoryProvider {
public:
    [[nodiscard]] task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext& sc, EffectContext&) {
        agentengine::ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        if constexpr (sizeof...(ToolTs) > 0) {
            c.tools = agentengine::ToolTable::from_tools<ToolTs...>().descriptors();
        }
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) {
        co_return std::monostate{};
    }
};
static_assert(agentengine::ContextProvider<ToolLoopHistoryProvider<EchoTool>>);
static_assert(agentengine::ContextProvider<ToolLoopHistoryProvider<GetWeatherTool>>);
static_assert(agentengine::ContextProvider<ToolLoopHistoryProvider<>>);

// ---- A scripted, REAL streaming+buffered ChatClientT -----------------------------------------------
// Each `round` is a list of ChatResponseUpdate to push, in order, synchronously (the ring is sized
// generously enough that a small scripted round never blocks on credit -- no background thread
// needed, matching the old file's own fixture reasoning: this file isn't re-proving real cross-thread
// backpressure, that already has its own dedicated proof elsewhere). `chat_rounds` is a SEPARATE
// script for the non-streaming path (L3), so one fixture proves both paths without risking drift.
class ScriptedChatClient {
public:
    std::vector<std::vector<ChatResponseUpdate>> stream_rounds;
    std::vector<ChatResponse> chat_rounds;
    std::size_t stream_call_count = 0;
    std::size_t chat_call_count = 0;

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        if (chat_call_count >= chat_rounds.size()) {
            co_return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                           "no more scripted chat() rounds",
                                                           "test.no_chat"});
        }
        co_return chat_rounds[chat_call_count++];
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        agentengine::stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = agentengine::make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
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
static_assert(agentengine::ChatClient<ScriptedChatClient>);

// A chat client whose chat()/chat_stream() succeed or fail on demand -- both fail-closed branches A4a/
// A4b need (chat failure, and a reply engineered to blow a tiny token budget) ride the same conformer,
// matching the old event-stream test's own fixture exactly.
class FailableChatClient {
public:
    bool fail_next = false;
    std::uint64_t reply_tokens = 1;

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        if (fail_next) {
            co_return std::unexpected(
                agentengine::error{agentengine::failure_class::transient, "scripted failure",
                                    "test.fail"});
        }
        ContentItem item{};
        item.value  = Text{"reply"};
        item.origin = content_origin::assistant;
        Message reply{};
        reply.role       = role::assistant;
        reply.message_id = "m-reply";
        reply.content.push_back(item);
        co_return ChatResponse{reply, Usage{reply_tokens, reply_tokens, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};  // unused -- this fixture only ever drives the non-streaming chat() path
    }
};
static_assert(agentengine::ChatClient<FailableChatClient>);

// A ChatClientT that is never engaged (AgentSession::has_chat_client() stays false) -- the "no
// chat_client_" fail-closed branch (A4c). Deliberately NOT default-constructible (a required `int`
// constructor argument never actually used): AgentSession's own make_default_chat_client() only
// auto-engages a default-constructible ChatClientT (agent_session.hpp lines ~1065-1071), so a
// default-constructible conformer here would silently defeat the branch this test exists to prove.
class NeverEngagedChatClient {
public:
    explicit NeverEngagedChatClient(int) {}
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        co_return std::unexpected(
            agentengine::error{agentengine::failure_class::fatal, "never reached", "test.unreachable"});
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};  // never reached
    }
};
static_assert(agentengine::ChatClient<NeverEngagedChatClient>);

[[nodiscard]] ChatResponseUpdate text_delta(std::string text, bool is_final = false,
                                             std::optional<Usage> usage = std::nullopt) {
    ChatResponseUpdate upd;
    upd.delta.origin = content_origin::assistant;
    upd.delta.value  = Text{std::move(text)};
    upd.is_final     = is_final;
    upd.usage        = usage;
    return upd;
}

[[nodiscard]] ChatResponseUpdate tool_call_delta(std::string call_id, std::string tool_name,
                                                  std::string args_json, bool is_final = false,
                                                  std::optional<Usage> usage = std::nullopt) {
    ChatResponseUpdate upd;
    upd.delta.origin = content_origin::assistant;
    upd.delta.value  = ToolCall{std::move(call_id), std::move(tool_name), std::move(args_json),
                                 content_origin::assistant, call_provenance::vendor_structured};
    upd.is_final = is_final;
    upd.usage    = usage;
    return upd;
}

[[nodiscard]] Message text_message(std::string text) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin  = content_origin::assistant;
    item.value   = Text{std::move(text)};
    item.tainted = false;
    m.content.push_back(item);
    return m;
}

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

[[nodiscard]] Message make_turn(std::string message_id) {
    ContentItem item{};
    item.value  = Text{"hello"};
    item.origin = content_origin::user;
    Message input{};
    input.role       = role::user;
    input.message_id = std::move(message_id);
    input.content.push_back(item);
    return input;
}

std::vector<RunEvent> drain(agentengine::stream<RunEvent>& s) {
    std::vector<RunEvent> events;
    while (auto ev = s.next()) events.push_back(std::move(*ev));
    return events;
}

using EchoSession    = AgentSession<ScriptedChatClient, NoSessionState, ToolLoopHistoryProvider<EchoTool>>;
using WeatherSession = AgentSession<ScriptedChatClient, NoSessionState,
                                     ToolLoopHistoryProvider<GetWeatherTool>>;
using PlainSession   = AgentSession<ScriptedChatClient, NoSessionState, ToolLoopHistoryProvider<>>;

}  // namespace

int main() {
    // ================================================================================================
    // S1-S4 (from test_agent_session_streaming_model_calls.cpp)
    // ================================================================================================

    // ---- S1: streamed, no-tool-call response --------------------------------------------------------
    {
        PlainSession session;
        session.initialize("s-s1", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.stream_rounds = {{
            text_delta("Hello"),
            text_delta(", world"),
            text_delta("!", /*is_final=*/true, Usage{5, 7, 0, 0, 0.0}),
        }};
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_stream_model_calls(true);
        check(session.stream_model_calls(), "S1: streaming is engaged");

        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        auto r = drive(session.start_run(StartRun{user_message("hi")}));
        check(r.has_value(), "S1: the streamed run converges");
        if (r.has_value()) {
            check(text_of(r->message) == "Hello, world!",
                  "S1: the reconstructed Message's text matches every pushed delta, concatenated in "
                  "order -- text_of() being count-agnostic across content items is what makes this "
                  "equivalent to the non-streaming parse");
            check(r->usage.input_tokens == 5 && r->usage.output_tokens == 7,
                  "S1: usage from the terminal update reached the final AgentResponse");
        }
        check(session.history().size() == 2,
              "S1: the streamed response was pushed to history exactly like a non-streamed one");

        std::vector<RunEvent> events = drain(viewer);
        std::size_t delta_count = 0;
        std::string joined_deltas;
        for (auto const& ev : events) {
            if (ev.kind == run_event_kind::model_delta) {
                ++delta_count;
                joined_deltas += std::get<agentengine::run_event_payload::ModelDelta>(ev.payload).text_delta;
            }
        }
        check(delta_count == 3, "S1: exactly one model_delta event fired per pushed Text delta");
        check(joined_deltas == "Hello, world!",
              "S1: the model_delta events, joined in emission order, reconstruct the same text");
    }

    // ---- S2: streamed tool-call round trip ------------------------------------------------------------
    {
        EchoSession session;
        session.initialize("s-s2", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.stream_rounds = {
            {
                text_delta("Let me check. "),
                tool_call_delta("c1", "echo_tool", R"({"value":42})", /*is_final=*/true,
                                 Usage{3, 4, 0, 0, 0.0}),
            },
            {
                text_delta("The value is 42.", /*is_final=*/true, Usage{6, 5, 0, 0, 0.0}),
            },
        };
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_stream_model_calls(true);

        auto r = drive(session.start_run(StartRun{user_message("echo 42")}));
        check(r.has_value(), "S2: the streamed multi-round tool call converges");
        if (r.has_value()) {
            check(text_of(r->message) == "The value is 42.",
                  "S2: the second round's streamed text is the final answer");
        }
        check(client.stream_call_count == 2,
              "S2: exactly 2 streamed model calls happened -- the tool round and the converging round");
        check(session.history().size() == 4,
              "S2: history holds input, the tool-call round's Message, the folded tool result, and the "
              "final round's Message");
    }

    // ---- S3: no usage on the terminal update -- fails closed, never silently zero-cost -----------------
    {
        PlainSession session;
        session.initialize("s-s3", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.stream_rounds = {{
            text_delta("hi", /*is_final=*/true, /*usage=*/std::nullopt),
        }};
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_stream_model_calls(true);

        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());
        auto r = drive(session.start_run(StartRun{user_message("hi")}));
        check(!r.has_value(),
              "S3: a streamed call with no reported usage never resolves to a real response -- "
              "fail-closed, never silently treated as a free call");
        check(r.has_value() || r.error().code == "run.usage_unavailable",
              "S3: the failure is specifically run.usage_unavailable (agent_session.hpp's own "
              "run_model_call() check)");

        bool saw_usage_failure_message = false;
        for (auto const& ev : drain(viewer)) {
            if (ev.kind == run_event_kind::run_failed) {
                auto const& p = std::get<agentengine::run_event_payload::RunFailed>(ev.payload);
                if (p.message.find("usage") != std::string::npos) saw_usage_failure_message = true;
            }
        }
        check(saw_usage_failure_message,
              "S3: the run_failed event's message names the real reason (missing usage), not a "
              "generic, indistinguishable failure");
    }

    // ---- S4: the accessor pair defaults false and round-trips ------------------------------------------
    {
        PlainSession session;
        session.emplace_chat_client();
        session.initialize("s-s4", Principal{"p", ""});
        check(!session.stream_model_calls(), "S4: streaming defaults false");
        session.set_stream_model_calls(true);
        check(session.stream_model_calls(), "S4: set_stream_model_calls(true) engages it");
        session.set_stream_model_calls(false);
        check(!session.stream_model_calls(), "S4: set_stream_model_calls(false) disengages it");
    }

    // ================================================================================================
    // L1-L4 (from test_agent_session_response_format_leak_scan.cpp)
    // ================================================================================================

    // ---- L1: streaming path, scan armed -- the leaked Harmony block is promoted and invoked -----------
    {
        WeatherSession session;
        session.initialize("s-l1", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.stream_rounds = {
            {text_delta(kHarmonyLeak, /*is_final=*/true, Usage{3, 4, 0, 0, 0.0})},
            {text_delta("It's sunny in Seattle.", /*is_final=*/true, Usage{5, 3, 0, 0, 0.0})},
        };
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_stream_model_calls(true);
        session.set_scan_response_format_leaks(true);
        check(session.scan_response_format_leaks(), "L1: the flag reads back armed");

        auto r = drive(session.start_run(StartRun{user_message("what's the weather?")}));
        check(r.has_value(), "L1: the run converges");
        if (r.has_value()) {
            check(text_of(r->message) == "It's sunny in Seattle.",
                  "L1: the final round's text is the converged answer, proving the promoted "
                  "text_derived call from round 1 was actually invoked and folded back in");
        }
        check(client.stream_call_count == 2,
              "L1: exactly 2 streamed rounds happened -- the leaked-tool-call round and the converging "
              "round, same shape as an ordinary vendor_structured tool round trip");
    }

    // ---- L2: streaming path, scan NOT armed (default) -- the leak passes through unchanged -------------
    {
        WeatherSession session;
        session.initialize("s-l2", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.stream_rounds = {
            {text_delta(kHarmonyLeak, /*is_final=*/true, Usage{3, 4, 0, 0, 0.0})},
        };
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        session.set_stream_model_calls(true);
        check(!session.scan_response_format_leaks(),
              "L2: the flag defaults false -- ADR-023 Finding 6, operator-armed, never "
              "content-triggered");

        auto r = drive(session.start_run(StartRun{user_message("what's the weather?")}));
        check(r.has_value(), "L2: the run converges (no tool call ever gets recognized/invoked)");
        if (r.has_value()) {
            check(text_of(r->message) == kHarmonyLeak,
                  "L2: with scanning off, the raw leaked Harmony text reaches the final Message "
                  "completely unchanged, byte for byte -- proving the flag genuinely gates the "
                  "behavior rather than the scan running unconditionally regardless of it");
        }
        check(client.stream_call_count == 1,
              "L2: only 1 round happened -- with no promotion, there is no tool call to invoke, so "
              "the loop converges immediately on the (unscanned) text answer");
    }

    // ---- L3: non-streaming (chat()) path, scan armed via AgentSession -- proves path-agnosticism --------
    {
        WeatherSession session;
        session.initialize("s-l3", Principal{"p", ""});
        ScriptedChatClient& client = session.emplace_chat_client();
        client.chat_rounds = {
            ChatResponse{text_message(kHarmonyLeak), Usage{3, 4, 0, 0, 0.0}},
            ChatResponse{text_message("It's sunny in Seattle."), Usage{5, 3, 0, 0, 0.0}},
        };
        CapabilitySet const held = CapabilitySet::grant_root({});
        session.set_capabilities(&held);
        // Deliberately NOT calling set_stream_model_calls() -- this fixture has no OpenAIChatClient
        // and thus no scan_response_format_leaks flag of its own; the ONLY way this scenario can
        // possibly promote the leaked call is through AgentSession's own flag, on the chat() path.
        session.set_scan_response_format_leaks(true);

        auto r = drive(session.start_run(StartRun{user_message("what's the weather?")}));
        check(r.has_value(), "L3: the non-streaming run converges");
        if (r.has_value()) {
            check(text_of(r->message) == "It's sunny in Seattle.",
                  "L3: the promoted call was invoked on the chat() path too -- the scan is applied "
                  "uniformly by run_model_call() regardless of stream_model_calls_");
        }
        check(client.chat_call_count == 2,
              "L3: exactly 2 chat() rounds happened -- the leaked-tool-call round and the converging "
              "round");
    }

    // ---- L4: double-scan idempotence -- a diagnostic's own text never re-triggers a promotion -----------
    {
        // A DIFFERENT tool ("real_tool") is declared this round than the one named in the leak
        // ("decoy_tool") -- so the FIRST pass leaves it as an inert, tainted diagnostic Text (Phase 1
        // behavior: unrecognized recipient, not promoted). The diagnostic's own body then embeds the
        // recipient name verbatim: "[unrecognized tool-call attempt, not executed: decoy_tool(...)]".
        // If a SECOND pass ever re-decoded that diagnostic and somehow produced a promotable
        // candidate from it, that would be the exact laundering path the red-team found -- proven
        // absent here by asserting the message is bit-for-bit identical after a second scan. Pure
        // function of response_format_leak_scan.hpp -- no rt::-specific surface to port, exercised
        // directly exactly like the old file did.
        std::string const leak =
            "<|start|>assistant<|channel|>commentary to=functions.decoy_tool <|constrain|>json"
            "<|message|>{\"x\":1}<|call|>";
        std::vector<ToolDescriptor> real_tool_only;
        {
            ToolDescriptor d;
            d.name = "real_tool";
            real_tool_only.push_back(d);
        }
        Message once = agentengine::apply_response_format_scan(text_message(leak), real_tool_only);
        bool any_promoted_first_pass = false;
        for (auto const& item : once.content) {
            if (std::holds_alternative<ToolCall>(item.value)) any_promoted_first_pass = true;
        }
        check(!any_promoted_first_pass,
              "L4 setup: with only 'real_tool' declared, the leak naming 'decoy_tool' stays an inert "
              "diagnostic after the FIRST pass -- unrecognized recipient, per Phase 1");

        Message twice = agentengine::apply_response_format_scan(once, real_tool_only);
        check(once.content.size() == twice.content.size() && once == twice,
              "L4: a second scan of the SAME message (as happens when both OpenAIChatClient's own "
              "flag and AgentSession's flag are armed together) is a byte-for-byte no-op -- the "
              "tainted diagnostic from pass 1 is skipped whole, not re-decoded and re-promoted");
    }

    // ================================================================================================
    // A1-A5 (from test_agent_session_run_event_stream.cpp)
    // ================================================================================================

    // ---- A1: no enable_event_stream() call -- a run proceeds exactly as before, no crash --------------
    {
        AgentSession<FailableChatClient> session;
        session.initialize("s-no-stream", Principal{"p1", ""});
        auto r = drive(session.start_run(StartRun{make_turn("m-1")}));
        check(r.has_value(), "A1: a run with no event stream enabled still succeeds normally");
    }

    // ---- A2: a successful run emits the full real success-path sequence, in order, seq 1..6 -----------
    {
        AgentSession<FailableChatClient> session;
        session.initialize("s-success", Principal{"p1", ""});
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

        auto r = drive(session.start_run(StartRun{make_turn("m-1")}));
        check(r.has_value(), "A2: the scripted successful run still succeeds");

        auto events = drain(viewer);
        check(events.size() == 6, "A2: exactly 6 events fire on the real success path");
        if (events.size() == 6) {
            run_event_kind const expected[] = {
                run_event_kind::run_started,         run_event_kind::turn_started,
                run_event_kind::model_call_started,  run_event_kind::model_call_finished,
                run_event_kind::turn_finished,       run_event_kind::run_finished,
            };
            bool order_ok  = true;
            bool seq_ok    = true;
            bool run_id_ok = true;
            for (std::size_t i = 0; i < 6; ++i) {
                if (events[i].kind != expected[i]) order_ok = false;
                if (events[i].seq != i + 1) seq_ok = false;
                if (events[i].run_id != "s-success:run:1") run_id_ok = false;
            }
            check(order_ok, "A2: events fire in the exact order 013 §1's turn-loop boundaries produce");
            check(seq_ok, "A2: sequence numbers are monotonic, starting at 1 (013 §1)");
            check(run_id_ok, "A2: every event in the run carries that run's own run_id");
        }
    }

    // ---- A3: sequence resets to 1 on the SAME session's next run -- never carries the prior run's ------
    // ---- count forward.                                                                              ---
    {
        AgentSession<FailableChatClient> session;
        session.initialize("s-reset", Principal{"p1", ""});
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

        (void)drive(session.start_run(StartRun{make_turn("m-1")}));
        auto first_run_events = drain(viewer);
        (void)drive(session.start_run(StartRun{make_turn("m-2")}));
        auto second_run_events = drain(viewer);

        check(first_run_events.size() == 6 && second_run_events.size() == 6,
              "A3: both runs on the same session each emit their own full 6-event sequence");
        check(!second_run_events.empty() && second_run_events.front().seq == 1,
              "A3: the second run's first event is seq 1 again, not seq 7 -- 013 §1's sequence number "
              "is monotonic PER RUN, not across the session's whole lifetime");
        check(!second_run_events.empty() && second_run_events.front().run_id == "s-reset:run:2",
              "A3: the second run's events carry the second run's own run_id");
    }

    // ---- A4a: a chat-call failure emits RunFailed as the terminal event, never RunFinished --------------
    {
        AgentSession<FailableChatClient> session;
        session.initialize("s-chat-fail", Principal{"p1", ""});
        session.emplace_chat_client().fail_next = true;
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

        auto r = drive(session.start_run(StartRun{make_turn("m-1")}));
        check(!r.has_value(), "A4a: a scripted chat failure still fails the run (fail-closed)");

        auto events = drain(viewer);
        check(!events.empty() && events.back().kind == run_event_kind::run_failed,
              "A4a: RunFailed is the terminal event on a chat-call failure");
        if (!events.empty()) {
            auto const* payload =
                std::get_if<agentengine::run_event_payload::RunFailed>(&events.back().payload);
            check(payload != nullptr && payload->error_code == "run.chat_failed",
                  "A4a: RunFailed carries the real error_code for a chat-call failure");
        }
        bool saw_run_finished = false;
        for (auto const& ev : events) saw_run_finished |= (ev.kind == run_event_kind::run_finished);
        check(!saw_run_finished, "A4a: a failed run never also emits RunFinished");
    }

    // ---- A4b: token-budget exceeded emits RunFailed with the budget error_code ---------------------------
    {
        AgentSession<FailableChatClient> session;
        session.initialize("s-budget", Principal{"p1", ""}, /*token_budget=*/std::uint64_t{1});
        session.emplace_chat_client().reply_tokens = 1000;  // blows a budget of 1
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

        auto r = drive(session.start_run(StartRun{make_turn("m-1")}));
        check(!r.has_value(), "A4b: exceeding the per-run token budget still fails the run");

        auto events = drain(viewer);
        check(!events.empty() && events.back().kind == run_event_kind::run_failed,
              "A4b: RunFailed is the terminal event on a token-budget failure");
        if (!events.empty()) {
            auto const* payload =
                std::get_if<agentengine::run_event_payload::RunFailed>(&events.back().payload);
            check(payload != nullptr && payload->error_code == "run.token_budget_exceeded",
                  "A4b: RunFailed carries the real error_code for a token-budget failure");
        }
    }

    // ---- A4c: no chat client configured emits RunFailed, before ModelCallStarted ever fires --------------
    {
        AgentSession<NeverEngagedChatClient> session;
        session.initialize("s-no-client", Principal{"p1", ""});
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

        auto r = drive(session.start_run(StartRun{make_turn("m-1")}));
        check(!r.has_value(), "A4c: no chat client configured still fails the run");

        auto events = drain(viewer);
        bool saw_model_call = false;
        for (auto const& ev : events) saw_model_call |= (ev.kind == run_event_kind::model_call_started);
        check(!saw_model_call, "A4c: ModelCallStarted never fires when there is no chat client to call");
        check(!events.empty() && events.back().kind == run_event_kind::run_failed,
              "A4c: RunFailed is still the terminal event");
        if (!events.empty()) {
            auto const* payload =
                std::get_if<agentengine::run_event_payload::RunFailed>(&events.back().payload);
            check(payload != nullptr && payload->error_code == "run.no_chat_client",
                  "A4c: RunFailed carries the real error_code for a missing chat client");
        }
    }

    // ---- A5: an admission-denied StartRun mints no Run at all -- no event fires for it --------------------
    {
        AgentSession<FailableChatClient> session;
        Principal const owner    = Principal{"p-owner", "tenant-a"};
        Principal const stranger = Principal{"p-stranger", "tenant-a"};
        session.initialize("s-denied", owner);
        auto viewer = session.enable_event_stream(std::pmr::get_default_resource());

        auto r = drive(session.start_run(
            StartRun{make_turn("m-1"), agentengine::rt::SessionCaller{stranger.id, stranger.tenant_id}}));
        check(!r.has_value(), "A5: the admission-denied run still fails (018 §2)");

        auto events = drain(viewer);
        check(events.empty(),
              "A5: no RunEvent fires for an admission-denied StartRun -- 001 §1 mints no Run at all "
              "before admission passes, so there is no run_id to attach an event to");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_agent_session_streaming_and_events: ALL PASS\n");
    return 0;
}
