// End-to-end proof that a real `AgentSession` can drive a real multi-tool, multi-turn conversation
// against a REAL remote model -- something that had literally never worked before this file's own
// prerequisite fix (see below), and had never been exercised live even after that fix, since
// test_agent_session_real_backend.cpp only ever ran against a canned loopback server and
// test_openrouter_live_e2e.cpp only ever drove the raw `ChatClient` layer directly, never a real
// `AgentSession` turn loop.
//
// TWO REAL BUGS, FOUND WHILE SCOPING THIS TEST, ARE FIXED AS ITS OWN PREREQUISITE (not pre-existing
// product code this file merely exercises):
//   1. `AgentSession::handle(StartRun)` (core/agent_session.hpp) built `ChatRequest{contribution->
//      messages}`, silently discarding `contribution->tools` -- a real session run had NEVER been
//      able to present a tool declaration to a model, through any HistoryProviderT, ever. Fixed to
//      forward `contribution->tools`; regression-proven offline in test_agent_session_real_backend.cpp
//      (case "J1-R8", now ported to test_rt_agent_session_real_backend.cpp's own "J1-R8").
//   2. `openai::detail::translate_message` (protocol/openai/chat_client.hpp) tracked only ONE
//      `tool_call_id` when a single AE `Message` carried multiple `ToolResult` content items -- the
//      natural shape for resolving N parallel tool calls in ONE session turn (StartRun takes exactly
//      one input `Message` per run, so a caller resolving 2+ parallel calls in one turn has no other
//      structurally sound option). Silently dropped the first result and merged its text under the
//      SECOND result's `tool_call_id` -- a real provider would 400 on the unresolved first id. Fixed
//      via `translate_message_to_wire` (splits into N wire messages); regression-proven offline in
//      test_openai_chat_client_translation.cpp (case "D1-R9").
//
// This file is the LIVE proof both fixes actually work end to end against a real remote model, not
// just offline against literal wire fixtures -- the same "canned server proves plumbing, real server
// proves the bytes are actually well-formed" gap test_openrouter_live_e2e.cpp's own top comment names.
//
// DELIBERATELY HARD, not a toy round-trip (the whole point of this file):
//   - MT-1 forces a genuine SEQUENTIAL DEPENDENCY: `convert_temperature`'s `celsius` argument must
//     equal the EXACT value `get_weather` actually returned -- not a value the model could plausibly
//     guess or compute from general knowledge (a deliberately un-round number). A model that fabricates
//     the second call instead of truly reading the first call's result, or that never calls
//     `convert_temperature` at all, fails this structurally, not on wording.
//   - MT-1 also declares two DISTRACTOR tools (`get_time`, `stock_price`) that are irrelevant to the
//     prompt -- the run must never touch them, proving real tool-selection discipline under a table
//     wider than what a naive single-tool test would exercise.
//   - MT-2 forces genuine PARALLEL tool calls (two calls in ONE assistant turn) and resolves them
//     through the just-fixed multi-ToolResult path -- never exercised against a real provider before.
//
// I5 (nondeterminism crosses a recorded seam) is respected by exclusion: gated on the same environment
// variables as test_openrouter_live_e2e.cpp, labelled `live-network`, structural assertions only
// (numeric/structural facts about WHICH tools ran and WHAT values threaded between them -- never
// asserting on the model's own prose).
//
// CREDENTIALS ARE NEVER COMPILED IN (018 §4) -- same environment-variable contract as
// test_openrouter_live_e2e.cpp: AGENTENGINE_OPENROUTER_API_KEY (required, else SKIP),
// AGENTENGINE_OPENROUTER_MODEL (optional), AGENTENGINE_OPENROUTER_HOST (optional).
//
// ADR-037: ported off `quark::TestKit<Session>`/`kit.ask<...>(...)` onto `agentengine::rt::AgentSession`
// directly -- both live turns now go through a local `drive<T>()` helper (`agentengine::rt::task<T>`
// resumed to completion inline), the same idiom every other `test_rt_agent_session*.cpp` file already
// uses (see e.g. test_rt_agent_session_real_backend.cpp), instead of `block_on(kit.ask<...>(...))`
// against a real actor mailbox. `emplace_chat_client()`/`initialize()`/`set_capabilities()` are called
// directly on the session value -- there is no `kit.actor()` indirection left to go through. The four
// tools, the `FourToolHistoryProvider` ContextProvider fixture, the prompts, and every assertion below
// are unchanged: same behavior proven, no actor/mailbox left to stand it up. This file touches no
// checkpoint/snapshot machinery (neither did the file it supersedes), so `save_agent_session_snapshot`/
// `InMemorySessionStore` (rt/agent_session.hpp's own Slice 2) are not involved here.

#ifdef AGENTENGINE_WITH_HTTPS

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "agentengine/core/json_schema.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

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
void note(char const* label, std::string const& value) {
    std::fprintf(stderr, "  .. %s = %s\n", label, value.c_str());
}

// Same "safe here because nothing genuinely suspends externally" drive<T>() every other
// test_rt_agent_session*.cpp file uses -- OpenAIChatClient::chat()'s own blocking socket I/O happens
// as an ordinary synchronous call inside the coroutine body, never a real suspension point.
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    char const* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::move(fallback);
}

constexpr char const* kDefaultModel = "~deepseek/deepseek-v4-flash-latest";
constexpr char const* kDefaultHost = "openrouter.ai";
constexpr std::uint16_t kHttpsPort = 443;
constexpr char const* kPathPrefix = "/api/v1";
constexpr char const* kSecretName = "openrouter-api-key";
constexpr int kMaxRounds = 6;  // AgentSession's own max_turns -- fails the run (not hang) if it never
                                // converges to plain Text; this file only ever OBSERVES via one ask now.

// `AgentSession::start_run()` (rt/agent_session.hpp) resolves the whole multi-round tool conversation
// INTERNALLY -- there is no external round loop left in this file to inspect each round's raw
// `ToolCall`s/arguments directly (see agent_session.hpp's own design/red-team record,
// ADR-027-agent-session-tool-call-loop.md). Structural facts this file still needs to prove --
// which tools were actually called, and the EXACT value threaded from `get_weather`'s real result
// into `convert_temperature`'s real argument -- are instead recorded from INSIDE each tool's own
// `invoke()`, the same function-local-static idiom `tools/cli_chat.cpp` already uses for
// process-scoped, test/demo-only observability. Cleared at the start of each of MT-1/MT-2 below.
[[nodiscard]] std::set<std::string>& called_tools_log() {
    static std::set<std::string> log;
    return log;
}
[[nodiscard]] std::optional<double>& weather_returned_celsius_log() {
    static std::optional<double> v;
    return v;
}
[[nodiscard]] std::optional<double>& convert_arg_celsius_log() {
    static std::optional<double> v;
    return v;
}

// ---- The four tools: two real, two distractors that must never be called ------------------------

struct GetWeatherArgs {
    std::string location;
};
AE_JSON_SCHEMA(GetWeatherArgs, location)

struct GetWeatherReply {
    double temp_c = 0.0;
    std::string condition;
};
AE_JSON_SCHEMA(GetWeatherReply, temp_c, condition)

// A deliberately un-round number: 13.7 is not something a model could plausibly "helpfully round" its
// way into producing on its own, unlike a value like 10.0/20.0/0.0 -- if `convert_temperature`'s own
// `celsius` argument matches this exactly, that is real evidence the model read the tool's actual
// reply rather than estimating or hallucinating a plausible-looking one.
struct GetWeatherTool : Tool<GetWeatherTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "get_weather";
    static constexpr std::string_view description =
        "Get the current weather for a city. Returns temperature in Celsius ONLY -- never Fahrenheit.";
    using Args = GetWeatherArgs;
    using Reply = GetWeatherReply;
    static result<Reply> invoke(Args, EffectContext&) {
        called_tools_log().insert("get_weather");
        Reply const reply{13.7, "overcast"};
        weather_returned_celsius_log() = reply.temp_c;
        return reply;
    }
};

struct ConvertTempArgs {
    double celsius = 0.0;
    std::string to_unit;  // "fahrenheit" | "kelvin"
};
AE_JSON_SCHEMA(ConvertTempArgs, celsius, to_unit)

struct ConvertTempReply {
    double value = 0.0;
    std::string unit;
};
AE_JSON_SCHEMA(ConvertTempReply, value, unit)

struct ConvertTempTool : Tool<ConvertTempTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "convert_temperature";
    static constexpr std::string_view description =
        "Convert an EXACT celsius value (as returned by get_weather) to fahrenheit or kelvin. Must be "
        "called with the precise celsius number from a prior get_weather result, never an estimate.";
    using Args = ConvertTempArgs;
    using Reply = ConvertTempReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        called_tools_log().insert("convert_temperature");
        convert_arg_celsius_log() = a.celsius;
        if (a.to_unit == "kelvin") return Reply{a.celsius + 273.15, "kelvin"};
        return Reply{a.celsius * 9.0 / 5.0 + 32.0, "fahrenheit"};
    }
};

// Distractors: schema-valid, callable, but irrelevant to every prompt in this file. Their own
// existence in the declared table (never their outputs) is what's under test.
struct GetTimeArgs {
    std::string timezone;
};
AE_JSON_SCHEMA(GetTimeArgs, timezone)
struct GetTimeReply {
    std::string time;
};
AE_JSON_SCHEMA(GetTimeReply, time)
struct GetTimeTool : Tool<GetTimeTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "get_time";
    static constexpr std::string_view description = "Get the current local time for a timezone.";
    using Args = GetTimeArgs;
    using Reply = GetTimeReply;
    static result<Reply> invoke(Args, EffectContext&) {
        called_tools_log().insert("get_time");
        return Reply{"14:32"};
    }
};

struct StockPriceArgs {
    std::string ticker;
};
AE_JSON_SCHEMA(StockPriceArgs, ticker)
struct StockPriceReply {
    double price = 0.0;
    std::string currency;
};
AE_JSON_SCHEMA(StockPriceReply, price, currency)
struct StockPriceTool : Tool<StockPriceTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "stock_price";
    static constexpr std::string_view description = "Get the current market price for a stock ticker.";
    using Args = StockPriceArgs;
    using Reply = StockPriceReply;
    static result<Reply> invoke(Args, EffectContext&) {
        called_tools_log().insert("stock_price");
        return Reply{198.42, "USD"};
    }
};

using AllToolsTable = ToolTable;
[[nodiscard]] AllToolsTable all_tools_table() {
    return ToolTable::from_tools<GetWeatherTool, ConvertTempTool, GetTimeTool, StockPriceTool>();
}

// The ContextProvider fixture: same windowing behaviour as HistoryProvider<Window<0>>, plus a fixed
// four-tool declaration -- default-constructible (ADR-018's constraint on every AgentSession template
// argument), the tool list baked in via `all_tools_table()` rather than injected state.
class FourToolHistoryProvider {
public:
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx, EffectContext&) {
        ContextContribution contribution;
        contribution.messages.assign(session_ctx.history.begin(), session_ctx.history.end());
        contribution.tools = all_tools_table().descriptors();
        co_return contribution;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<FourToolHistoryProvider>);

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] bool has_text(Message const& m) {
    for (ContentItem const& item : m.content) {
        if (std::holds_alternative<Text>(item.value)) return true;
    }
    return false;
}

}  // namespace

int main() {
    char const* key_env = std::getenv("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || !*key_env) {
        std::fprintf(stderr,
                     "test_rt_agent_session_live_multitool_e2e: SKIPPED -- "
                     "AGENTENGINE_OPENROUTER_API_KEY is not set.\n"
                     "  Run tools/run-live-provider-tests.ps1, or set the variable yourself, to "
                     "exercise a real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", kDefaultModel);
    std::string const host = env_or("AGENTENGINE_OPENROUTER_HOST", kDefaultHost);
    std::fprintf(stderr, "test_rt_agent_session_live_multitool_e2e: host=%s model=%s\n", host.c_str(),
                 model.c_str());

    InMemorySecretStore store;
    store.set(kSecretName, key_env);
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});

    ChatClientCapabilities caps;
    caps.streaming = true;
    caps.tool_calling = true;
    caps.parallel_tool_calls = true;
    caps.max_output_tokens = 1024;

    using RealClient = openai::OpenAIChatClient<InMemorySecretStore>;
    using Session = AgentSession<RealClient, NoSessionState, FourToolHistoryProvider>;

    // ==================== MT-1: sequential dependency + distractor-tool discipline =================
    // `AgentSession::start_run()` now resolves the whole tool chain internally (rt/agent_session.hpp)
    // -- one `StartRun` call returns the FINAL converged response, not an intermediate round.
    // Structural facts this test needs (which tools ran, the exact threaded celsius value) are
    // recorded from inside each tool's own `invoke()` via `called_tools_log()`/`*_celsius_log()` above.
    {
        called_tools_log().clear();
        weather_returned_celsius_log().reset();
        convert_arg_celsius_log().reset();

        Session session;
        session.emplace_chat_client(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store,
                                     kPathPrefix);
        session.initialize("mt1-session", Principal{"live-e2e-principal", ""}, /*token_budget=*/std::nullopt,
                            /*max_turns=*/static_cast<std::uint64_t>(kMaxRounds));
        session.set_capabilities(&held);

        Message const input = user_message(
            "Step 1: call get_weather for Seattle. Step 2: once you have that result, call "
            "convert_temperature with to_unit=fahrenheit, passing the EXACT celsius number "
            "get_weather returned -- do not estimate, round, or use outside knowledge of Seattle's "
            "weather. Step 3: after both calls, state the final Fahrenheit temperature in one "
            "sentence. Do not skip either tool call.");

        agentengine::result<agentengine::rt::AgentResponse> resp =
            drive(session.start_run(StartRun{input}));
        check(resp.has_value(),
              "MT1: the session converges (resolves the whole tool chain internally, within "
              "max_turns) against the real provider");
        if (resp.has_value()) {
            check(has_text(resp->message), "MT1: the converged response carries a final Text answer");
        }

        auto const& called_tools = called_tools_log();
        check(called_tools.count("get_weather") == 1, "MT1: get_weather was actually called");
        check(called_tools.count("convert_temperature") == 1,
              "MT1: convert_temperature was actually called -- the model did not shortcut the "
              "explicit two-step instruction by answering from its own arithmetic/estimate");
        check(called_tools.count("get_time") == 0,
              "MT1: the get_time DISTRACTOR was never called, despite being declared and available");
        check(called_tools.count("stock_price") == 0,
              "MT1: the stock_price DISTRACTOR was never called, despite being declared and available");

        if (weather_returned_celsius_log() && convert_arg_celsius_log()) {
            note("get_weather returned celsius", std::to_string(*weather_returned_celsius_log()));
            note("convert_temperature received celsius", std::to_string(*convert_arg_celsius_log()));
            check(std::fabs(*weather_returned_celsius_log() - *convert_arg_celsius_log()) < 0.01,
                  "MT1: convert_temperature's OWN celsius argument matches get_weather's ACTUAL "
                  "returned value (13.7) to within float noise -- real value-threading across a real "
                  "multi-round session against a real model, resolved internally by AgentSession's "
                  "own tool-call loop, not a fabricated/guessed number");
        }
    }

    // ==================== MT-2: forced PARALLEL tool calls, resolved internally =====================
    // What's no longer externally observable under the internal-loop architecture: the STRICT "both
    // calls landed in one round, as two distinct ToolCall content items" property this test used to
    // assert directly on a raw intermediate response -- that response never leaves `start_run()` now.
    // A named, deliberate narrowing (proving it again would mean consuming `enable_event_stream()`'s
    // `tool_call_started` events between two `turn_started` events, not wired up this pass), not
    // silently dropped coverage. What MT-2 still proves: the model, explicitly instructed to call
    // both tools "together, in this SAME response," genuinely reaches for BOTH real tools, and the
    // run still converges -- which only happens if AgentSession's internal multi-`ToolResult`
    // feed-back path (the same `tool_results_message` helper this file's fix #2 originally proved,
    // now shared via tool_call_extraction.hpp) actually works against a real provider.
    {
        called_tools_log().clear();

        Session session;
        session.emplace_chat_client(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store,
                                     kPathPrefix);
        session.initialize("mt2-session", Principal{"live-e2e-principal", ""}, /*token_budget=*/std::nullopt,
                            /*max_turns=*/static_cast<std::uint64_t>(kMaxRounds));
        session.set_capabilities(&held);

        agentengine::result<agentengine::rt::AgentResponse> resp = drive(session.start_run(StartRun{
            user_message("You must call BOTH get_weather for \"Seattle\" AND get_time for \"Tokyo\" "
                         "together, as two separate tool calls issued in this SAME response -- not one "
                         "after the other across two turns. Do not answer with text yet; only issue "
                         "both tool calls now.")}));
        check(resp.has_value(),
              "MT2: the session converges (resolves both real tool calls internally, however many "
              "rounds that takes) against the real provider");
        if (resp.has_value()) {
            check(has_text(resp->message),
                  "MT2: after both calls are resolved internally, the session produces a final Text "
                  "answer -- the whole forced-parallel round trip completed end to end live");
        } else {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        }

        auto const& called_tools = called_tools_log();
        check(called_tools.count("get_weather") == 1,
              "MT2: get_weather was actually called (real tool execution, not a fabricated answer)");
        check(called_tools.count("get_time") == 1,
              "MT2: get_time was actually called -- the model genuinely reached for BOTH requested "
              "tools, not just one");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_rt_agent_session_live_multitool_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_agent_session_live_multitool_e2e: %d FAILURE(S)\n", g_failures);
    return 1;
}

#else   // AGENTENGINE_WITH_HTTPS
int main() { return 0; }
#endif  // AGENTENGINE_WITH_HTTPS
