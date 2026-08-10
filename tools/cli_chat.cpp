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
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/mounted_skills_state.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/core/skill_tool_scoping.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"
#include "backends/native_jail/skill_mount_materializer.hpp"

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

// Set exactly once, from main(), BEFORE any user turn can trigger the first execute_code call (and
// therefore shared_python_runner()'s first, lazy construction below) -- main()'s own startup sequence
// (materialize skills -> grant capabilities -> enter the interactive loop) always completes first, so
// there is no real ordering hazard despite this being a plain static rather than an injected
// dependency. `MediatedPythonConfig::mount_roots` has no public mutator after construction (must be
// set before `.initialize()`), so this side channel is how a lazily-constructed singleton picks up
// mount roots main() only learns about at runtime (materialization happens after this file's own
// process starts -- it can never be a compile-time constant).
[[nodiscard]] std::vector<native_jail::MaterializedSkillMount>& pending_skill_mount_roots() {
    static std::vector<native_jail::MaterializedSkillMount> roots;
    return roots;
}

// Skills Phase 3 (decisions/ADR-024's addendum): real, persistent, per-run state -- not a chat-history
// message the way MAF's own `load_skill` result is (see mounted_skills_state.hpp's own top comment for
// why that matters). Same function-local-static, single-process-scoped idiom as
// `pending_skill_mount_roots()`/`shared_python_runner()` above: one instance, shared by reference
// between `MountSkillTool::invoke()` (writer) and every later `on_context()`/tool-scoping call
// (readers) -- `AgentSession` has no generic mechanism for a `Tool<>` to reach its own `StateT`, so this
// works around that gap the same way this file already does for the Python runner's config.
[[nodiscard]] MountedSkillsState& shared_mounted_skills_state() {
    static MountedSkillsState state;
    return state;
}

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
        for (auto const& [mount_id, host_dir] : pending_skill_mount_roots()) {
            cfg.mount_roots[mount_id] = host_dir;
        }
        cfg.expose_agent_files_data = true;
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

// ---- The on-demand mount trigger: real state, no new authority ------------------------------------
// Skills Phase 3 (decisions/ADR-024's addendum): the agent must call this to activate a skill's
// tools/full body before they're usable -- researched from MAF's AgentSkillsProvider (three fixed
// meta-tools, load_skill/read_skill_resource/run_skill_script) but deliberately NOT that shape: this
// tool touches no content and no filesystem at all, it only records intent into
// MountedSkillsState. It grants no new file-read authority (every resolved skill's files are already
// readable via the capability the operator granted at startup, per 009 §8b -- unaffected by mount
// state); it only activates which TOOLS get declared+invocable (skill_tool_scoping.hpp) and whether the
// skill's full body gets re-injected into context (ToolDeclaringHistoryProvider::on_context below). A
// model calling this can never reach anything it wasn't already pre-authorized for (I3).
struct MountSkillArgs {
    std::string skill_name;
};
AE_JSON_SCHEMA(MountSkillArgs, skill_name)

struct MountSkillReply {
    bool ok = false;
    std::string message;
};
AE_JSON_SCHEMA(MountSkillReply, ok, message)

struct MountSkillTool : Tool<MountSkillTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "mount_skill";
    static constexpr std::string_view description =
        "Activate a skill you've seen advertised by name and description, so its full instructions "
        "become part of your context and any tools it names become callable -- starting next turn. "
        "This does not read or return the skill's content; call it before you need a skill's own "
        "tools, then wait for your next turn to use them.";
    using Args = MountSkillArgs;
    using Reply = MountSkillReply;

    static result<Reply> invoke(Args a, EffectContext&) {
        bool known = false;
        for (auto const& [mount_id, host_dir] : pending_skill_mount_roots()) {
            (void)host_dir;
            if (mount_id == a.skill_name) {
                known = true;
                break;
            }
        }
        if (!known) {
            return std::unexpected(error{failure_class::contract, "unknown skill: " + a.skill_name,
                                          "skill.unknown_name"});
        }
        shared_mounted_skills_state().mount(a.skill_name);
        return Reply{true, "mounted: " + a.skill_name};
    }
};

// ---- ContextProvider fixture: real conversation history + ONE merged skill system message ----------
// Owns its own `SkillsProvider<>` (the only skills-related instance this provider needs) purely to
// reach `allowed_tool_names_for`/`body_of` -- `AgentSession` has no accessor into its private
// `history_provider_` member (by design, see `history_and_skills_provider.hpp`'s own top comment), so
// this provider cannot be handed a pre-resolved answer from outside; it must be able to compute one
// from nothing. This resolves the SAME deterministic builtin skill source `main()`'s own startup
// materializer instance resolves, so both independently reach byte-identical results -- see
// `core/skill_tool_scoping.hpp`'s own top comment for why the declared table here and the
// invocation-time table `main()`'s tool-call loop uses must (and, by this determinism, do) agree.
//
// A real wire dump this session (response-dumps/test-dump.json) showed the advertisement message
// (previously a SEPARATE `BuiltinSkillsProvider`'s own contribution) and the mounted-skill-body
// messages arriving as TWO-OR-MORE separate `role: system` entries -- structurally fine (every
// surveyed provider accepts multiple system messages) but not what a single, coherent system prompt
// should look like. Fixed by having THIS provider alone own the whole skill-related system prompt:
// `skills_.on_context(...)` is called for its own advertisement message, then every currently mounted
// skill's full body is appended into that SAME message's text rather than pushed as its own message --
// exactly one `role: system` entry, its content re-derived (never accumulated) fresh every call, so a
// newly mounted skill's body genuinely REPLACES the previous turn's system prompt with a longer one,
// not stacks alongside it. `BuiltinSkillsProvider`/`HistoryAndSkillsProvider` are no longer needed in
// this file's own composition as a result -- this provider is a complete `ContextProvider` by itself.
class ToolDeclaringHistoryProvider {
public:
    ToolDeclaringHistoryProvider() : skills_({make_builtin_skills_source()}) {}

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx,
                                                                 EffectContext& ec) {
        auto skills_contribution = co_await skills_.on_context(session_ctx, ec);
        if (!skills_contribution) co_return std::unexpected(skills_contribution.error());

        std::string combined_system_text;
        if (!skills_contribution->messages.empty() && !skills_contribution->messages[0].content.empty()) {
            if (auto const* t =
                    std::get_if<Text>(&skills_contribution->messages[0].content[0].value)) {
                combined_system_text = t->text;
            }
        }
        // Full body for every currently MOUNTED skill, re-derived fresh from real state on every call
        // and appended into the SAME text -- never a separate message. Unlike MAF's `load_skill` (an
        // ephemeral tool-result message that can be lost to history compaction), this is reliably
        // present on every subsequent turn as long as the skill stays mounted -- see
        // mounted_skills_state.hpp's own top comment.
        for (auto const& mounted_name : shared_mounted_skills_state().all()) {
            auto body = skills_.body_of(mounted_name);
            if (!body) continue;
            combined_system_text += "\nMounted skill '" + mounted_name + "':\n" + *body;
        }

        ContextContribution contribution;
        if (!combined_system_text.empty()) {
            Message m;
            m.role = role::system;
            ContentItem item;
            item.origin = content_origin::system;
            item.value = Text{std::move(combined_system_text)};
            m.content.push_back(std::move(item));
            contribution.messages.push_back(std::move(m));
        }

        contribution.messages.insert(contribution.messages.end(), session_ctx.history.begin(),
                                      session_ctx.history.end());

        // Recomputed every call, not cached: mount state can change mid-conversation (that's the whole
        // point of Phase 3), so the declared set must track it -- main()'s own invocation-time table
        // must be recomputed on the same cadence, or declared/invocable would diverge (the exact
        // cosmetic-scoping gap skill_tool_scoping.hpp's own top comment warns about).
        auto const universe = ToolTable::from_tools<ExecuteCodeTool, MountSkillTool>();
        auto const scoped = scope_tools_to_mounted_skills(
            universe, skills_.allowed_tool_names_for(shared_mounted_skills_state().all()),
            {std::string(MountSkillTool::name)});
        contribution.tools = scoped.descriptors();
        co_return contribution;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

private:
    SkillsProvider<> skills_;
};
static_assert(ContextProvider<ToolDeclaringHistoryProvider>);

using CliSession =
    AgentSession<openai::OpenAIChatClient<InMemorySecretStore>, NoSessionState, ToolDeclaringHistoryProvider>;
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

void print_skills_banner(std::vector<native_jail::MaterializedSkillMount> const& materialized,
                          SkillsProvider<>& startup_skills) {
    auto skills = make_builtin_skills_source().load_skills();
    std::cout << "Skills RESOLVED at /skills/<name> -- every one's files are unconditionally readable "
                 "from turn 1 (009 §8b, unaffected by mount state):\n";
    if (skills) {
        for (auto const& s : *skills) {
            std::cout << "  - " << s.skill.frontmatter.name << ": " << s.skill.frontmatter.description
                       << "\n";
        }
    } else {
        std::cout << "  (failed to resolve: " << skills.error().message << ")\n";
    }

    std::cout << "Materialized into the real sandbox (native_jail::materialize_skill_mounts):\n";
    if (materialized.empty()) {
        std::cout << "  (none)\n";
    } else {
        for (auto const& [mount_id, host_dir] : materialized) {
            std::cout << "  - mount_id=" << mount_id
                       << " host_dir=" << std::string(host_dir.begin(), host_dir.end()) << "\n";
        }
    }

    std::cout << "Currently MOUNTED skills (agent-triggered via mount_skill -- 009 §8c Phase 3): ";
    auto const& mounted = shared_mounted_skills_state().all();
    if (mounted.empty()) {
        std::cout << "(none -- nothing is pre-mounted; the agent must call mount_skill)\n";
    } else {
        for (std::size_t i = 0; i < mounted.size(); ++i) std::cout << (i ? ", " : "") << mounted[i];
        std::cout << "\n";
    }

    std::cout << "Tools declared+invocable right now (mount_skill is always available; others unlock "
                 "once their owning skill is mounted): ";
    auto const universe = ToolTable::from_tools<ExecuteCodeTool, MountSkillTool>();
    auto const scoped = scope_tools_to_mounted_skills(
        universe, startup_skills.allowed_tool_names_for(mounted), {std::string(MountSkillTool::name)});
    if (scoped.descriptors().empty()) {
        std::cout << "(none)\n";
    } else {
        for (std::size_t i = 0; i < scoped.descriptors().size(); ++i) {
            std::cout << (i ? ", " : "") << scoped.descriptors()[i].name;
        }
        std::cout << "\n";
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

    // Resolve + materialize skills into REAL files on disk before shared_python_runner()'s lazy
    // singleton is ever touched (its mount_roots must be fully known at construction -- no public
    // mutator exists after .initialize()). This is main()'s OWN SkillsProvider<> instance, independent
    // of ToolDeclaringHistoryProvider's/BuiltinSkillsProvider's own instances inside CliSession -- all
    // three resolve the identical, deterministic builtin skill source, so their outputs agree.
    SkillsProvider<> startup_skills({make_builtin_skills_source()});
    std::filesystem::path const skills_scratch =
        std::filesystem::temp_directory_path() / "agentengine_cli_chat_workspace" / "skills";
    auto materialized =
        native_jail::materialize_skill_mounts(startup_skills, skills_scratch, {kWorkMount});
    if (!materialized) {
        std::cerr << "FATAL: failed to materialize skill mounts: " << materialized.error().message << "\n";
        return 1;
    }
    pending_skill_mount_roots() = *materialized;

    InMemorySecretStore store;
    store.set(kSecretName, key_env);
    std::vector<Capability> grants = {
        Capability{cap::Secret{kSecretName, std::chrono::seconds{0}}},
        Capability{cap::FsRead{kWorkMount, "", std::nullopt}},
        Capability{cap::FsWrite{kWorkMount, "", std::nullopt, std::nullopt}}};
    for (auto const& [mount_id, host_dir] : *materialized) {
        (void)host_dir;
        grants.push_back(Capability{cap::FsRead{mount_id, "", std::nullopt}});
    }
    CapabilitySet held = CapabilitySet::grant_root(std::move(grants));

    print_skills_banner(*materialized, startup_skills);
    std::cout << "\nType a message and press Enter. Type 'exit' or 'quit' to stop.\n\n";

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
            // Recomputed fresh EVERY round from the SAME live `shared_mounted_skills_state()` that
            // `ToolDeclaringHistoryProvider::on_context()` just used to build what was declared to the
            // model THIS round -- mount state can change mid-conversation now (Phase 3), so a table
            // computed once before the loop would drift from what's declared the moment a mount
            // happens. Never a differently-scoped table than what was just declared; see
            // skill_tool_scoping.hpp's own top comment for why that divergence would be a real gap, not
            // a cosmetic one.
            auto const round_universe = ToolTable::from_tools<ExecuteCodeTool, MountSkillTool>();
            auto const scoped_tools = scope_tools_to_mounted_skills(
                round_universe, startup_skills.allowed_tool_names_for(shared_mounted_skills_state().all()),
                {std::string(MountSkillTool::name)});
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
                } else if (call.tool_name == "mount_skill") {
                    auto args = json::parse(call.arguments_json);
                    if (args) {
                        if (auto const* s = args->find("skill_name"); s && s->is_string()) {
                            std::cout << "  skill_name: " << s->as_string() << "\n";
                        }
                    }
                }
                auto parsed_args = json::parse(call.arguments_json);
                ToolCallRequest req{call.call_id, call.tool_name,
                                    parsed_args ? *parsed_args : json::Value::make_object({}),
                                    /*arguments_tainted=*/true, static_cast<std::uint64_t>(i)};
                ToolResult r = invoke_tool(scoped_tools, held, req, exec_ctx, nullptr);
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
