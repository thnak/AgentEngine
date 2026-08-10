// Interactive CLI chat driver, built to validate 009-Plugin-and-Extension-System.md §8's skill
// mechanism live, by hand, against a real remote model: type a greeting, then ask the agent to use a
// mounted skill, and watch it actually reach for the real tool the skill names AND actually execute
// code through the real embedded CPython sandbox (src/backends/native_jail/mediated_python_runner.hpp)
// -- not the deterministic arithmetic stand-in tests/test_agent_session_skills_live_e2e.cpp uses.
//
// Same composition tests/test_agent_session_skills_live_e2e.cpp already proves automatically
// (HistoryAndSkillsProvider<ToolDeclaringHistoryProvider, BuiltinSkillsProvider> driving a real
// AgentSession via quark::TestKit -- the only demonstrated way in this codebase to run an AgentSession
// synchronously from a plain main(), the same pattern every live e2e test in tests/ already uses).
// What this file adds beyond that automated test: a REAL ExecuteCodeTool wired to
// native_jail::MediatedPythonRunner (a genuinely new capability this codebase had never had a Tool<>
// wrapper for -- see this file's own research: no such wrapper existed anywhere before this), and a
// human at the keyboard replacing the scripted prompts, so "on-demand" skill loading and real code
// execution can be watched turn by turn instead of only asserted after the fact.
//
// ADR-002 §5.5.6 scope carried forward unchanged: at most one MediatedPythonRunner alive per process.
// This CLI is that one process -- the runner and its ExecState are function-local statics, shared by
// every execute_code call for the CLI's whole lifetime (real session-scoped variable persistence,
// proven already by test_mediated_python_runner_smoke.cpp's own E2-C3).
//
// This whole TARGET only exists (root CMakeLists.txt's own `add_executable` call is inside
// `if(AGENTENGINE_WITH_HTTPS AND AGENTENGINE_BUILD_PYTHON_RUNNER)`) when both a real chat backend and
// a real Python embed are configured -- `AGENTENGINE_BUILD_PYTHON_RUNNER` itself is a CMake
// configure-time switch only, never forwarded as a compiler `#define` anywhere in this build (unlike
// `AGENTENGINE_WITH_HTTPS`, which IS a real macro -- propagated PUBLIC from
// `agentengine::net_egress_proxy`, transitively reaching this target via `provider_http_client`).
// The `#ifdef AGENTENGINE_WITH_HTTPS` below is therefore the only preprocessor gate this file needs;
// it matches `protocol/openai/chat_client.hpp`'s own gate on `OpenAIChatClient`'s existence.
//
// CREDENTIALS ARE NEVER COMPILED IN (018 §4): AGENTENGINE_OPENROUTER_API_KEY must be set in the
// environment. AGENTENGINE_OPENROUTER_MODEL/AGENTENGINE_OPENROUTER_HOST are optional overrides.

#ifdef AGENTENGINE_WITH_HTTPS

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "quark/core/testkit.hpp"

#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/builtin_skills.hpp"
#include "agentengine/core/history_and_skills_provider.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"

using namespace agentengine;

namespace {

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    char const* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::move(fallback);
}

constexpr char const* kDefaultModel = "~deepseek/deepseek-v4-flash-latest";
constexpr char const* kDefaultHost = "openrouter.ai";
constexpr std::uint16_t kHttpsPort = 443;
constexpr char const* kPathPrefix = "/api/v1";
constexpr char const* kSecretName = "openrouter-api-key";
constexpr char const* kWorkMount = "work";
constexpr int kMaxToolRoundsPerTurn = 6;  // guards against a runaway tool-call loop, never a hard cap
                                            // on ordinary multi-step use

// ---- The real execute_code tool: wired to MediatedPythonRunner, not a stand-in --------------------

struct ExecuteCodeArgs {
    std::string code;
    std::string language;  // "python" if empty -- the only language MediatedPythonRunner backs today
};
AE_JSON_SCHEMA(ExecuteCodeArgs, code, language)

struct ExecuteCodeReply {
    bool ok = false;
    std::string stdout_text;
    std::string stderr_text;
    std::string result_repr;  // 010 §3's captured trailing-expression value, when non-empty
};
AE_JSON_SCHEMA(ExecuteCodeReply, ok, stdout_text, stderr_text, result_repr)

// ADR-002 §5.5.6: exactly one interpreter per process, for the process's whole lifetime -- a
// function-local static, constructed on first real use, matching this CLI's own single-process scope
// exactly (there is no multi-session story here to conflict with that rule).
[[nodiscard]] native_jail::MediatedPythonRunner& shared_python_runner() {
    static native_jail::MediatedPythonRunner runner = [] {
        native_jail::MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        std::filesystem::path const scratch =
            std::filesystem::temp_directory_path() / "agentengine_cli_chat_workspace";
        std::error_code ec;
        std::filesystem::create_directories(scratch, ec);
        cfg.mount_roots[kWorkMount] = scratch.wstring();
        return native_jail::MediatedPythonRunner(std::move(cfg));
    }();
    static bool const initialized = [] {
        auto r = runner.initialize();
        if (!r) {
            std::cerr << "FATAL: MediatedPythonRunner failed to initialize: " << r.error().message << "\n";
        }
        return r.has_value();
    }();
    (void)initialized;
    return runner;
}

// Real session state (010 §3a "shared by reference"): a variable a user's code defines in one
// execute_code call is genuinely still there on the next call, across turns, for this CLI's whole
// run -- the same guarantee test_mediated_python_runner_smoke.cpp's E2-C3 proves offline, now
// observable interactively.
[[nodiscard]] ExecState& shared_exec_state() {
    static ExecState state{};
    return state;
}

struct ExecuteCodeTool : Tool<ExecuteCodeTool, Capabilities<cap::decl::FsRead<"work">,
                                                              cap::decl::FsWrite<"work">>,
                                EffectClass<effect_class::at_most_once>> {
    static constexpr std::string_view name = "execute_code";
    static constexpr std::string_view description =
        "Execute Python code in this session's real interpreter and return what it printed. State "
        "(variables, imports) persists across calls within this session -- the tool the "
        "using-the-code-interpreter skill teaches idioms for.";
    using Args = ExecuteCodeArgs;
    using Reply = ExecuteCodeReply;

    static result<Reply> invoke(Args a, EffectContext& ctx) {
        auto& runner = shared_python_runner();
        if (!runner.ok()) {
            return std::unexpected(error{failure_class::fatal,
                                          "the embedded Python interpreter failed to initialize",
                                          "cli_chat.python_runner_not_initialized"});
        }
        ExecRequest req{a.language.empty() ? "python" : a.language, a.code};
        auto outcome = runner.run(req, shared_exec_state(), ctx);
        if (!outcome) return std::unexpected(outcome.error());

        Reply reply;
        reply.ok = (outcome->klass == exec_outcome_class::ok);
        reply.stdout_text = outcome->stdout_text;
        reply.stderr_text = outcome->stderr_text;
        reply.result_repr = outcome->result_repr;
        return reply;
    }
};

// ---- ContextProvider fixture: real conversation history + the one real tool -----------------------
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
// tests/test_agent_session_skills_real_backend.cpp and
// tests/test_agent_session_skills_live_e2e.cpp already established, duplicated here per this
// codebase's own per-file-independence convention (those files' own top comments).
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

using CliSession =
    AgentSession<openai::OpenAIChatClient<InMemorySecretStore>, NoSessionState,
                 HistoryAndSkillsProvider<ToolDeclaringHistoryProvider, BuiltinSkillsProvider>>;
static_assert(std::is_default_constructible_v<CliSession>);

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

[[nodiscard]] std::string text_of(Message const& m) {
    std::string out;
    for (ContentItem const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) out += t->text;
    }
    return out;
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

void print_skills_banner() {
    auto skills = make_builtin_skills_source().load_skills();
    std::cout << "Skills mounted at /skills/<name> (resolved once, frozen for this session -- 009 "
                 "§8c):\n";
    if (skills) {
        for (auto const& s : *skills) {
            std::cout << "  - " << s.skill.frontmatter.name << ": " << s.skill.frontmatter.description
                       << "\n";
        }
    } else {
        std::cout << "  (failed to resolve: " << skills.error().message << ")\n";
    }
}

}  // namespace

int main() {
    char const* key_env = std::getenv("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || !*key_env) {
        std::cerr << "AGENTENGINE_OPENROUTER_API_KEY is not set. Export a real OpenRouter API key "
                     "and re-run.\n";
        return 1;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", kDefaultModel);
    std::string const host = env_or("AGENTENGINE_OPENROUTER_HOST", kDefaultHost);

    std::cout << "AgentEngine CLI chat -- host=" << host << " model=" << model << "\n";
    print_skills_banner();
    std::cout << "\nType a message and press Enter. Type 'exit' or 'quit' to stop.\n\n";

    InMemorySecretStore store;
    store.set(kSecretName, key_env);
    CapabilitySet held = CapabilitySet::grant_root(
        {Capability{cap::Secret{kSecretName, std::chrono::seconds{0}}},
         Capability{cap::FsRead{kWorkMount, "", std::nullopt}},
         Capability{cap::FsWrite{kWorkMount, "", std::nullopt, std::nullopt}}});

    ChatClientCapabilities caps;
    caps.streaming = true;
    caps.tool_calling = true;
    caps.max_output_tokens = 2048;

    quark::TestKit<CliSession> kit;
    kit.actor().emplace_chat_client(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store,
                                     kPathPrefix);
    kit.actor().initialize("cli-chat-session", Principal{"cli-user", ""});
    kit.actor().set_capabilities(&held);

    std::string line;
    while (true) {
        std::cout << "You: ";
        if (!std::getline(std::cin, line)) break;
        if (line == "exit" || line == "quit") break;
        if (line.empty()) continue;

        Message next_input = user_message(line);
        bool converged = false;

        for (int round = 0; round < kMaxToolRoundsPerTurn; ++round) {
            quark::result<AgentResponse> resp = kit.ask<AgentResponse>(StartRun{next_input});
            if (!resp.has_value()) {
                std::cout << "[error: " << resp.error().detail << "]\n";
                converged = true;
                break;
            }

            auto calls = tool_calls_of(resp->message);
            if (calls.empty()) {
                std::string const text = text_of(resp->message);
                if (!text.empty()) std::cout << "Agent: " << text << "\n";
                converged = true;
                break;
            }

            std::vector<ToolResult> results;
            results.reserve(calls.size());
            EffectContext exec_ctx;
            exec_ctx.principal = Principal{"cli-user", ""};
            exec_ctx.capabilities = &held;
            auto const table = ToolTable::from_tools<ExecuteCodeTool>();
            for (std::size_t i = 0; i < calls.size(); ++i) {
                ToolCall const& call = calls[i];
                std::cout << "[agent calls " << call.tool_name << "]\n";
                if (call.tool_name == "execute_code") {
                    auto args = json::parse(call.arguments_json);
                    if (args) {
                        if (auto const* c = args->find("code"); c && c->is_string()) {
                            std::cout << "  code:\n";
                            std::cout << "    " << c->as_string() << "\n";
                        }
                    }
                }
                auto parsed_args = json::parse(call.arguments_json);
                ToolCallRequest req{call.call_id, call.tool_name,
                                    parsed_args ? *parsed_args : json::Value::make_object({}),
                                    /*arguments_tainted=*/true, static_cast<std::uint64_t>(i)};
                ToolResult r = invoke_tool(table, held, req, exec_ctx, nullptr);
                if (!r.content.empty()) {
                    if (auto const* d = std::get_if<Data>(&r.content[0].value)) {
                        auto parsed = json::parse(d->json);
                        if (parsed) {
                            if (auto const* out = parsed->find("stdout_text"); out && out->is_string() &&
                                !out->as_string().empty()) {
                                std::cout << "  stdout: " << out->as_string() << "\n";
                            }
                            if (auto const* err = parsed->find("stderr_text"); err && err->is_string() &&
                                !err->as_string().empty()) {
                                std::cout << "  stderr: " << err->as_string() << "\n";
                            }
                        }
                    } else if (auto const* e = std::get_if<Error>(&r.content[0].value)) {
                        std::cout << "  error: " << e->message << "\n";
                    }
                }
                results.push_back(std::move(r));
            }
            next_input = tool_results_message(std::move(results));
        }

        if (!converged) std::cout << "[gave up after " << kMaxToolRoundsPerTurn << " tool rounds]\n";
    }

    std::cout << "Goodbye.\n";
    return 0;
}

#else   // AGENTENGINE_WITH_HTTPS
#include <cstdio>
int main() {
    std::fprintf(stderr, "cli_chat: not built -- requires AGENTENGINE_WITH_HTTPS to be ON.\n");
    return 1;
}
#endif
