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
//      (case "J1-R8").
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

#ifdef AGENTENGINE_WITH_HTTPS

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;

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

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    char const* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::move(fallback);
}

constexpr char const* kDefaultModel = "~deepseek/deepseek-v4-flash-latest";
constexpr char const* kDefaultHost = "openrouter.ai";
constexpr std::uint16_t kHttpsPort = 443;
constexpr char const* kPathPrefix = "/api/v1";
constexpr char const* kSecretName = "openrouter-api-key";
constexpr int kMaxRounds = 6;  // fail the run (not hang) if a session never converges to plain Text

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
    static result<Reply> invoke(Args, EffectContext&) { return Reply{13.7, "overcast"}; }
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
    static result<Reply> invoke(Args, EffectContext&) { return Reply{"14:32"}; }
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
    static result<Reply> invoke(Args, EffectContext&) { return Reply{198.42, "USD"}; }
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

[[nodiscard]] std::vector<ToolCall> tool_calls_of(Message const& m) {
    std::vector<ToolCall> out;
    for (ContentItem const& item : m.content) {
        if (auto const* tc = std::get_if<ToolCall>(&item.value)) out.push_back(*tc);
    }
    return out;
}

[[nodiscard]] bool has_text(Message const& m) {
    for (ContentItem const& item : m.content) {
        if (std::holds_alternative<Text>(item.value)) return true;
    }
    return false;
}

// Executes one real ToolCall through the REAL ten-step pipeline (core/tool_pipeline.hpp) -- the same
// table the session itself declared, not a second hand-rolled dispatch. All four tools here are
// `Capabilities<>` + `EffectClass<pure>`, so an empty CapabilitySet and a null approve-decider are
// correct, not a shortcut: step 4 has nothing to bind, step 5 needs no decider under `never_require`.
[[nodiscard]] ToolResult execute_real(AllToolsTable const& table, ToolCall const& call,
                                       std::uint64_t call_index, EffectContext& ctx) {
    CapabilitySet empty;
    auto args = json::parse(call.arguments_json);
    ToolCallRequest req{call.call_id, call.tool_name, args ? *args : json::Value::make_object({}),
                        /*arguments_tainted=*/true, call_index};
    return invoke_tool(table, empty, req, ctx, nullptr);
}

[[nodiscard]] Message tool_results_message(std::vector<ToolResult> results) {
    Message m;
    m.role = role::tool;
    for (ToolResult& r : results) {
        ContentItem item;
        item.origin = content_origin::tool;
        item.value = std::move(r);
        m.content.push_back(std::move(item));
    }
    return m;
}

}  // namespace

int main() {
    char const* key_env = std::getenv("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || !*key_env) {
        std::fprintf(stderr,
                     "test_agent_session_live_multitool_e2e: SKIPPED -- "
                     "AGENTENGINE_OPENROUTER_API_KEY is not set.\n"
                     "  Run tools/run-live-provider-tests.ps1, or set the variable yourself, to "
                     "exercise a real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", kDefaultModel);
    std::string const host = env_or("AGENTENGINE_OPENROUTER_HOST", kDefaultHost);
    std::fprintf(stderr, "test_agent_session_live_multitool_e2e: host=%s model=%s\n", host.c_str(),
                 model.c_str());

    InMemorySecretStore store;
    store.set(kSecretName, key_env);
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});

    ChatClientCapabilities caps;
    caps.streaming = true;
    caps.tool_calling = true;
    caps.parallel_tool_calls = true;
    caps.max_output_tokens = 1024;

    AllToolsTable const table = all_tools_table();

    using RealClient = openai::OpenAIChatClient<InMemorySecretStore>;
    using Session = AgentSession<RealClient, NoSessionState, FourToolHistoryProvider>;

    // ==================== MT-1: sequential dependency + distractor-tool discipline =================
    {
        quark::TestKit<Session> kit;
        kit.actor().emplace_chat_client(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store,
                                         kPathPrefix);
        kit.actor().initialize("mt1-session", Principal{"live-e2e-principal", ""});
        kit.actor().set_capabilities(&held);

        std::set<std::string> called_tools;
        std::optional<double> weather_returned_celsius;
        std::optional<double> convert_arg_celsius;
        bool converged = false;

        Message next_input = user_message(
            "Step 1: call get_weather for Seattle. Step 2: once you have that result, call "
            "convert_temperature with to_unit=fahrenheit, passing the EXACT celsius number "
            "get_weather returned -- do not estimate, round, or use outside knowledge of Seattle's "
            "weather. Step 3: after both calls, state the final Fahrenheit temperature in one "
            "sentence. Do not skip either tool call.");

        for (int round = 0; round < kMaxRounds; ++round) {
            quark::result<AgentResponse> resp = kit.ask<AgentResponse>(StartRun{next_input});
            check(resp.has_value(), "MT1: round completes against the real provider");
            if (!resp.has_value()) break;

            auto calls = tool_calls_of(resp->message);
            note("MT1 round tool_calls", std::to_string(calls.size()));
            if (calls.empty()) {
                check(has_text(resp->message), "MT1: a no-tool-call round carries a final Text answer");
                converged = true;
                break;
            }

            std::vector<ToolResult> results;
            results.reserve(calls.size());
            EffectContext exec_ctx;
            exec_ctx.principal = Principal{"live-e2e-principal", ""};
            for (std::size_t i = 0; i < calls.size(); ++i) {
                ToolCall const& call = calls[i];
                called_tools.insert(call.tool_name);
                if (call.tool_name == "convert_temperature") {
                    auto args = json::parse(call.arguments_json);
                    if (args) {
                        if (auto const* c = args->find("celsius"); c && c->is_number()) {
                            convert_arg_celsius = c->as_number();
                        }
                    }
                }
                ToolResult r = execute_real(table, call, static_cast<std::uint64_t>(i), exec_ctx);
                if (r.is_error) {
                    // NOT a hard failure by itself: this pipeline's own real schema validation
                    // (tool_pipeline.hpp step 2) legitimately rejects a malformed call, and a model
                    // recovering by retrying with corrected arguments on the next round is realistic,
                    // observed behaviour for a smaller/faster model tier -- proof the validation is
                    // real, not proof AgentEngine is broken. The genuinely hard bar is below: the run
                    // must still CONVERGE with the CORRECT threaded value, not that every single
                    // attempt succeeds on the first try.
                    std::string detail;
                    if (!r.content.empty()) {
                        if (auto const* e = std::get_if<Error>(&r.content[0].value)) detail = e->message;
                    }
                    note("MT1 tool call rejected (model will see the error, may retry)",
                         call.tool_name + ": " + detail);
                }
                if (call.tool_name == "get_weather" && !r.is_error && !r.content.empty()) {
                    if (auto const* d = std::get_if<Data>(&r.content[0].value)) {
                        auto parsed = json::parse(d->json);
                        if (parsed) {
                            if (auto const* t = parsed->find("temp_c"); t && t->is_number()) {
                                weather_returned_celsius = t->as_number();
                            }
                        }
                    }
                }
                results.push_back(std::move(r));
            }

            next_input = tool_results_message(std::move(results));
        }

        check(converged, "MT1: the session reached a final Text answer within the round budget "
                          "(it did not loop forever or dead-end mid-chain)");
        check(called_tools.count("get_weather") == 1, "MT1: get_weather was actually called");
        check(called_tools.count("convert_temperature") == 1,
              "MT1: convert_temperature was actually called -- the model did not shortcut the "
              "explicit two-step instruction by answering from its own arithmetic/estimate");
        check(called_tools.count("get_time") == 0,
              "MT1: the get_time DISTRACTOR was never called, despite being declared and available");
        check(called_tools.count("stock_price") == 0,
              "MT1: the stock_price DISTRACTOR was never called, despite being declared and available");

        if (weather_returned_celsius && convert_arg_celsius) {
            note("get_weather returned celsius", std::to_string(*weather_returned_celsius));
            note("convert_temperature received celsius", std::to_string(*convert_arg_celsius));
            check(std::fabs(*weather_returned_celsius - *convert_arg_celsius) < 0.01,
                  "MT1: convert_temperature's OWN celsius argument matches get_weather's ACTUAL "
                  "returned value (13.7) to within float noise -- real value-threading across a real "
                  "multi-turn session against a real model, not a fabricated/guessed number");
        }
    }

    // ==================== MT-2: forced PARALLEL tool calls, resolved via the fixed wire path =========
    {
        quark::TestKit<Session> kit;
        kit.actor().emplace_chat_client(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store,
                                         kPathPrefix);
        kit.actor().initialize("mt2-session", Principal{"live-e2e-principal", ""});
        kit.actor().set_capabilities(&held);

        quark::result<AgentResponse> resp = kit.ask<AgentResponse>(StartRun{user_message(
            "You must call BOTH get_weather for \"Seattle\" AND get_time for \"Tokyo\" together, as "
            "two separate tool calls issued in this SAME response -- not one after the other across "
            "two turns. Do not answer with text yet; only issue both tool calls now.")});
        check(resp.has_value(), "MT2: the forced-parallel round completes against the real provider");
        if (!resp.has_value()) {
            if (g_failures == 0) {
                std::fprintf(stderr, "test_agent_session_live_multitool_e2e: ALL PASS\n");
                return 0;
            }
            std::fprintf(stderr, "test_agent_session_live_multitool_e2e: %d FAILURE(S)\n", g_failures);
            return 1;
        }

        auto calls = tool_calls_of(resp->message);
        note("MT2 parallel tool_calls in one turn", std::to_string(calls.size()));
        check(calls.size() >= 2,
              "MT2: the model issued at least two ToolCall content items in a SINGLE assistant "
              "turn -- real parallel tool calling, never exercised live in this codebase before");
        if (calls.size() >= 2) {
            check(calls[0].call_id != calls[1].call_id,
                  "MT2: the two parallel calls carry distinct provider-issued call_ids");
            std::set<std::string> names{calls[0].tool_name, calls[1].tool_name};
            check(names.count("get_weather") == 1 && names.count("get_time") == 1,
                  "MT2: both requested tools are present -- the model genuinely parallelized two "
                  "DIFFERENT, independent calls, not the same call twice");
        }

        // Resolve ALL calls from this turn into ONE role::tool Message carrying N ToolResult items --
        // the exact shape that was structurally broken before this file's own prerequisite fix #2.
        std::vector<ToolResult> results;
        results.reserve(calls.size());
        EffectContext exec_ctx;
        exec_ctx.principal = Principal{"live-e2e-principal", ""};
        for (std::size_t i = 0; i < calls.size(); ++i) {
            ToolResult r = execute_real(table, calls[i], static_cast<std::uint64_t>(i), exec_ctx);
            check(!r.is_error, "MT2: real tool execution succeeds for each parallel call");
            results.push_back(std::move(r));
        }

        quark::result<AgentResponse> resp2 = kit.ask<AgentResponse>(
            StartRun{tool_results_message(std::move(results))});
        check(resp2.has_value(),
              "MT2: feeding back a SINGLE Message carrying multiple ToolResult items is ACCEPTED by "
              "the real provider -- this is exactly fix #2 (translate_message_to_wire) proven live: "
              "before it, at most one of the N tool_call_ids would ever reach the wire, and a real "
              "provider would reject the request as having an unresolved tool_call from the prior turn");
        if (resp2.has_value()) {
            check(has_text(resp2->message),
                  "MT2: after both parallel calls are resolved, the session produces a final Text "
                  "answer -- the whole forced-parallel round trip completed end to end live");
        } else {
            std::fprintf(stderr, "       error: %.*s\n",
                         static_cast<int>(resp2.error().detail.size()), resp2.error().detail.data());
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_agent_session_live_multitool_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_session_live_multitool_e2e: %d FAILURE(S)\n", g_failures);
    return 1;
}

#else   // AGENTENGINE_WITH_HTTPS
int main() { return 0; }
#endif  // AGENTENGINE_WITH_HTTPS
