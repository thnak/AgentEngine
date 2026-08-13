// Implements 009-Plugin-and-Extension-System.md §8, live: a real, skills-composed `AgentSession`
// (`HistoryAndSkillsProvider<ToolDeclaringHistoryProvider, BuiltinSkillsProvider>`) driven through a
// two-turn conversation against a REAL remote model over OpenRouter -- a plain greeting, then a
// request that only makes sense if the model actually read the mounted `using-the-code-interpreter`
// skill's advertisement (name + description, injected as a `role::system` message by
// `SkillsProvider::on_context`) and reached for the `execute_code`-shaped tool it names.
//
// test_agent_session_skills_real_backend.cpp already proves the skill advertisement reaches a real
// outbound wire request, against a CANNED server whose reply is fixed -- it cannot show a real model
// actually ACTING on that advertisement, only that the bytes were sent. This file is the live
// behavioural proof: the model has no other way to know an "execute_code" tool exists for running
// code except (a) the tool's own declared name/description in the request's `tools` array (present
// regardless of skills) and (b) the skill advertisement telling it there is a *skill* covering when
// and how to use it. Asking specifically to "use your code execution skill" and getting a real
// `execute_code` call back is the observable signature of the skill mechanism actually working end to
// end against a real provider, not just reaching the wire.
//
// Per this suite's own convention (test_agent_session_live_multitool_e2e.cpp's top comment),
// assertions are structural only -- which tool was called, with what shape of argument, whether the
// run converged -- never on the model's own prose.
//
// CREDENTIALS ARE NEVER COMPILED IN (018 §4) -- same environment-variable contract as every other
// live test in this suite: AGENTENGINE_OPENROUTER_API_KEY (required, else SKIP),
// AGENTENGINE_OPENROUTER_MODEL (optional), AGENTENGINE_OPENROUTER_HOST (optional).
//
// ADR-037: ported off `quark::TestKit<Session>`/`quark::Ask<>` onto `agentengine::rt::AgentSession`
// directly -- `kit.ask<AgentResponse>(StartRun{...})` against an actor mailbox becomes
// `drive(session.start_run(StartRun{...}))`, the same `drive<T>()` idiom every other `rt::` test in
// this suite already uses (see test_rt_agent_session_tooling_and_delegation.cpp,
// test_rt_agent_session_real_backend.cpp). Nothing about WHAT this file proves changed -- only the
// AgentSession plumbing driving it.

#ifdef AGENTENGINE_WITH_HTTPS

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentengine/core/builtin_skills.hpp"
#include "agentengine/core/history_and_skills_provider.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;
using agentengine::rt::AgentResponse;
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

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    char const* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::move(fallback);
}

// Same "safe here because nothing genuinely suspends externally" drive<T>() every other
// test_rt_agent_session*.cpp file uses (OpenAIChatClient::chat()'s own blocking socket I/O happens as
// an ordinary synchronous call inside the coroutine body, never a real suspension point).
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

constexpr char const* kDefaultModel = "~deepseek/deepseek-v4-flash-latest";
constexpr char const* kDefaultHost = "openrouter.ai";
constexpr std::uint16_t kHttpsPort = 443;
constexpr char const* kPathPrefix = "/api/v1";
constexpr char const* kSecretName = "openrouter-api-key";
constexpr int kMaxRounds = 6;  // AgentSession's own max_turns now (agent_session.hpp)

// `AgentSession::start_run()` now resolves any tool call internally (ADR-027-agent-session-tool-call-
// loop.md) -- there is no external round loop left here to observe a raw `ToolCall` directly.
// Recorded from inside `ExecuteCodeTool::invoke()` itself instead, the same function-local-static
// idiom `tools/cli_chat.cpp` already uses.
[[nodiscard]] bool& execute_code_called_log() {
    static bool called = false;
    return called;
}
[[nodiscard]] std::string& observed_code_arg_log() {
    static std::string code;
    return code;
}

// ---- A stand-in execute_code tool -- NOT the real sandboxed interpreter (010/026's own scope), a
// deterministic fake that proves TOOL SELECTION driven by the mounted skill, the same way
// test_agent_session_live_multitool_e2e.cpp's GetWeatherTool stands in for a real weather API. -------

struct ExecuteCodeArgs {
    std::string code;
    std::string language;  // model-supplied, e.g. "python" -- not enforced here
};
AE_JSON_SCHEMA(ExecuteCodeArgs, code, language)

struct ExecuteCodeReply {
    std::string stdout_text;
};
AE_JSON_SCHEMA(ExecuteCodeReply, stdout_text)

// Extracts the first two base-10 integers appearing anywhere in `code` and combines them with
// whichever of +/-/* appears first between them -- enough to give a real, non-canned numeric result
// for a simple arithmetic request without attempting to actually parse or execute Python.
[[nodiscard]] std::string fake_execute(std::string const& code) {
    std::vector<long long> numbers;
    std::size_t i = 0;
    while (i < code.size() && numbers.size() < 2) {
        if (code[i] >= '0' && code[i] <= '9') {
            std::size_t j = i;
            long long v = 0;
            while (j < code.size() && code[j] >= '0' && code[j] <= '9') {
                v = v * 10 + (code[j] - '0');
                ++j;
            }
            numbers.push_back(v);
            i = j;
        } else {
            ++i;
        }
    }
    if (numbers.size() < 2) return "0";
    auto const plus = code.find('+');
    auto const minus = code.find('-');
    auto const star = code.find('*');
    if (star != std::string::npos && (plus == std::string::npos || star < plus) &&
        (minus == std::string::npos || star < minus)) {
        return std::to_string(numbers[0] * numbers[1]);
    }
    if (minus != std::string::npos && (plus == std::string::npos || minus < plus)) {
        return std::to_string(numbers[0] - numbers[1]);
    }
    return std::to_string(numbers[0] + numbers[1]);
}

struct ExecuteCodeTool : Tool<ExecuteCodeTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "execute_code";
    static constexpr std::string_view description =
        "Execute a snippet of code in the session's interpreter and return what it printed. This is "
        "the tool the using-the-code-interpreter skill teaches idioms for.";
    using Args = ExecuteCodeArgs;
    using Reply = ExecuteCodeReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        execute_code_called_log() = true;
        observed_code_arg_log() = a.code;
        return Reply{fake_execute(a.code)};
    }
};

// ---- ContextProvider fixture: history + the one declared tool -- mirrors
// test_agent_session_live_multitool_e2e.cpp's FourToolHistoryProvider shape, one tool instead of four.
class ToolDeclaringHistoryProvider {
public:
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx, EffectContext&) {
        ContextContribution contribution;
        contribution.messages.assign(session_ctx.history.begin(), session_ctx.history.end());
        contribution.tools = ToolTable::from_tools<ExecuteCodeTool>().descriptors();
        co_return contribution;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<ToolDeclaringHistoryProvider>);

// Default-constructible wrapper baking in the real §8f builtin skills -- same shape
// test_agent_session_skills_real_backend.cpp's own `BuiltinSkillsProvider` already established,
// duplicated here per this suite's per-file independence convention (that file's own top comment).
class BuiltinSkillsProvider {
public:
    BuiltinSkillsProvider() : inner_({make_builtin_skills_source()}) {}
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext& ec) {
        return inner_.on_context(sc, ec);
    }
    task<std::monostate> on_turn_end(TurnView tv, EffectContext& ec) { return inner_.on_turn_end(tv, ec); }

private:
    SkillsProvider<> inner_;
};
static_assert(ContextProvider<BuiltinSkillsProvider>);

using SkillsLiveSession =
    AgentSession<openai::OpenAIChatClient<InMemorySecretStore>, NoSessionState,
                 HistoryAndSkillsProvider<ToolDeclaringHistoryProvider, BuiltinSkillsProvider>>;
static_assert(std::is_default_constructible_v<SkillsLiveSession>);

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
                     "test_rt_agent_session_skills_live_e2e: SKIPPED -- "
                     "AGENTENGINE_OPENROUTER_API_KEY is not set.\n"
                     "  Run tools/run-live-provider-tests.ps1, or set the variable yourself, to "
                     "exercise a real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", kDefaultModel);
    std::string const host = env_or("AGENTENGINE_OPENROUTER_HOST", kDefaultHost);
    std::fprintf(stderr, "test_rt_agent_session_skills_live_e2e: host=%s model=%s\n", host.c_str(),
                 model.c_str());

    InMemorySecretStore store;
    store.set(kSecretName, key_env);
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});

    ChatClientCapabilities caps;
    caps.streaming = true;
    caps.tool_calling = true;
    caps.max_output_tokens = 1024;

    SkillsLiveSession session;
    session.emplace_chat_client(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store,
                                 kPathPrefix);
    session.initialize("skills-live-e2e-session", Principal{"live-e2e-principal", ""},
                        /*token_budget=*/std::nullopt, /*max_turns=*/static_cast<std::uint64_t>(kMaxRounds));
    session.set_capabilities(&held);

    // ==================== Turn 1: an ordinary greeting -- no tool involved ==========================
    result<AgentResponse> greeting =
        drive(session.start_run(StartRun{user_message("Hi there! How are you doing today?")}));
    check(greeting.has_value(), "TURN1: the greeting round completes against the real provider");
    if (greeting.has_value()) {
        check(has_text(greeting->message), "TURN1: the greeting gets back a real text reply");
        auto stray_calls = tool_calls_of(greeting->message);
        note("TURN1 tool_calls (expected 0)", std::to_string(stray_calls.size()));
    }

    // ==================== Turn 2: ask the agent to use ITS SKILL, not just "a tool" =================
    // Deliberately phrased around the SKILL, not the tool name -- "your code execution skill" only
    // resolves to anything if the model actually received and read the using-the-code-interpreter
    // skill's advertisement (SkillsProvider::on_context's system message), matching how a real skill
    // consumer discovers capability by name, not by memorizing the tool schema in isolation.
    //
    // `AgentSession::start_run()` now resolves any tool call internally (agent_session.hpp) -- one
    // call returns the final converged response directly; whether `execute_code` was actually called,
    // and with what argument, is recorded from inside `ExecuteCodeTool::invoke()` itself (see
    // `execute_code_called_log()`/`observed_code_arg_log()` above).
    execute_code_called_log() = false;
    observed_code_arg_log().clear();

    result<AgentResponse> resp = drive(session.start_run(StartRun{user_message(
        "Please use your code execution skill to run some code that computes the sum 17 + 25, then "
        "tell me the result.")}));
    check(resp.has_value(),
          "TURN2: the session converges (resolves the tool call internally, within max_turns) "
          "against the real provider");
    if (resp.has_value()) {
        check(has_text(resp->message), "TURN2: the converged response carries a final text answer");
    }

    check(execute_code_called_log(),
          "TURN2: execute_code was actually called -- the model reached for the tool its mounted "
          "using-the-code-interpreter skill names, in response to being asked for the SKILL by name, "
          "not the tool name -- the live, behavioural proof that a mounted skill actually reaches and "
          "influences a real model's real tool selection, not just the outbound wire bytes");
    if (!observed_code_arg_log().empty()) {
        note("execute_code code argument", observed_code_arg_log());
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_rt_agent_session_skills_live_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_agent_session_skills_live_e2e: %d FAILURE(S)\n", g_failures);
    return 1;
}

#else   // AGENTENGINE_WITH_HTTPS
int main() { return 0; }
#endif  // AGENTENGINE_WITH_HTTPS
