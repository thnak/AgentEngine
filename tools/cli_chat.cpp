// Interactive CLI chat driver, built to validate 009-Plugin-and-Extension-System.md §8's skill
// mechanism live, by hand, against a real remote model: type a greeting, then ask the agent to use a
// mounted skill, and watch it actually reach for the real tool the skill names AND actually execute
// code through the real embedded CPython sandbox (src/backends/native_jail/mediated_python_runner.hpp)
// -- not the deterministic arithmetic stand-in tests/test_agent_session_skills_live_e2e.cpp uses.
//
// Same composition tests/test_agent_session_skills_live_e2e.cpp already proves automatically
// (HistoryAndSkillsProvider<ToolDeclaringHistoryProvider, BuiltinSkillsProvider> driving a real
// agentengine::rt::AgentSession directly -- historical: originally via quark::TestKit, before
// ADR-037 ported that test off quark::TestKit<Session>/quark::Ask<> onto rt::AgentSession, the same
// pattern every live e2e test in tests/ already uses).
// What this file adds beyond that automated test: a REAL ExecuteCodeTool wired to
// native_jail::MediatedPythonRunner (a genuinely new capability this codebase had never had a Tool<>
// wrapper for -- see this file's own research: no such wrapper existed anywhere before this), and a
// human at the keyboard replacing the scripted prompts, so "on-demand" skill loading and real code
// execution can be watched turn by turn instead of only asserted after the fact.
//
// ADR-002 §5.5.6 scope carried forward unchanged: at most one MediatedPythonRunner alive per process.
// This CLI is that one process -- the runner itself stays a function-local static, shared for the
// CLI's whole lifetime (real session-scoped variable persistence, proven already by
// test_mediated_python_runner_smoke.cpp's own E2-C3). ADR-030 (session-scoped CodeAct wiring) moved
// everything genuinely SESSION-scoped (MountedSkillsState, ExecState) onto ToolDeclaringHistoryProvider
// as real per-instance members (ADR-028's make_tool_descriptor_with_invoke), reached instead of
// process-global statics -- and added CodeActRunnerBinding (codeact_runner_binding.hpp), which claims
// this session's exclusive right to the still-shared runner and fails closed if a second session ever
// tried to reach it: confirmed by red-team that MediatedPythonRunner's own internal state
// (mediated_python_runner.cpp's file-scope globals) is not safely reentrant across different callers,
// so "one interpreter per process" is enforced here, not merely commented.
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

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/core/builtin_skills.hpp"
#include "agentengine/core/codeact_runner_binding.hpp"
#include "agentengine/core/codeact_tool_union.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/mounted_skills_state.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/core/skill_tool_scoping.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/thread_pool.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"
#include "backends/native_jail/skill_mount_materializer.hpp"
#include "backends/native_jail/tool_bridge.hpp"

using namespace agentengine;

namespace {

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

constexpr char const* kDefaultModel = "~deepseek/deepseek-v4-flash-latest";
constexpr char const* kDefaultHost = "openrouter.ai";
constexpr std::uint16_t kHttpsPort = 443;
constexpr char const* kPathPrefix = "/api/v1";
constexpr char const* kSecretName = "openrouter-api-key";
constexpr char const* kWorkMount = "work";

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

// ADR-030 (session-scoped CodeAct wiring): `MediatedPythonRunner`/its mount-root config remain a
// genuinely process-wide singleton -- ADR-002 §5.5.6 Finding 7.8 already documented "this design
// requires one OS process per session" (CPython's classic embedding API only supports ONE
// `Py_InitializeFromConfig` call per process, ever), and `codeact_runner_binding.hpp`'s own top
// comment confirms directly (from `mediated_python_runner.cpp`'s own file-scope globals) that
// `run()`/`refresh_agent_tools()` are not even safely re-entrant across DIFFERENT callers -- two
// sessions cannot share this runner concurrently, not even under a lock/queue. So this stays a
// lazy function-local static (matching this CLI's own single-process scope, and avoiding the
// dangling-pointer risk a `main()`-local variable would have against Quark's own actor-drain
// lifetime) -- what moved to real per-session state is everything genuinely session-scoped
// (`MountedSkillsState`, `ExecState`, ADR-030's own `CodeActRunnerBinding` CLAIM, not the runner
// object itself), via `ToolDeclaringHistoryProvider`'s own members below, reached through ADR-028's
// `make_tool_descriptor_with_invoke` mechanism instead of a `Tool<>::invoke()` with no path back to
// per-session state. `mount_roots` is read only on the FIRST call (a lazy-static-IIFE's inherent
// "first caller wins" property, unchanged from this function's own pre-ADR-030 shape) -- callers
// after the first must not assume their own `mount_roots` argument had any effect.
[[nodiscard]] native_jail::MediatedPythonRunner& shared_python_runner(
    std::vector<native_jail::MaterializedSkillMount> const& mount_roots) {
    static native_jail::MediatedPythonRunner runner = [&] {
        native_jail::MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        std::filesystem::path const scratch =
            std::filesystem::temp_directory_path() / "agentengine_cli_chat_workspace";
        std::error_code ec;
        std::filesystem::create_directories(scratch, ec);
        cfg.mount_roots[kWorkMount] = scratch.wstring();
        for (auto const& [mount_id, host_dir] : mount_roots) {
            cfg.mount_roots[mount_id] = host_dir;
        }
        cfg.expose_agent_files_data = true;
        // ADR-057 §9 (026 §5's `agent.ask`, Design B: abort-and-replay) -- real wiring so the CLI's
        // own execute_code tool can reach `agent.ask()`, matching this file's own `expose_agent_
        // files_data = true` one line up. This CLI does not itself implement a host-driven resolve/
        // replay loop (that is `rt::AgentSession::resolve_interaction()`'s job, proven by
        // tests/test_agent_session_suspend_codeact_ask.cpp against a real embedded interpreter, not
        // this CLI's own single-session, non-AgentSession-hosted `real_execute_code()` wiring) -- a
        // script calling `agent.ask()` here simply surfaces `ExecOutcome::klass == ask_pending` as
        // an ordinary tool failure (`codeact.ask_pending`) with no resume path, same as any other
        // unresolved suspension a host has not wired a UI for yet (matching ADR-029 §6's identical
        // scoping call for this CLI's own approval path).
        cfg.expose_agent_ask = true;
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

// The claim guard (codeact_runner_binding.hpp) wrapping the singleton above -- also a lazy
// function-local static (same lifetime as the runner it wraps, constructed once, on first real
// use). `ToolDeclaringHistoryProvider::configure()` is this binding's one real caller: it fails
// closed if any session OTHER than the one that bound first ever tries to reach the runner through
// it, turning ADR-002 §5.5.6's rule into something enforced, not merely commented.
[[nodiscard]] CodeActRunnerBinding<native_jail::MediatedPythonRunner>& shared_python_runner_binding(
    std::vector<native_jail::MaterializedSkillMount> const& mount_roots) {
    static CodeActRunnerBinding<native_jail::MediatedPythonRunner> binding(
        shared_python_runner(mount_roots));
    return binding;
}

// ---- A trivial, capability-free demo tool: proves a mounted skill's allowed-tools reaches
// CodeAct's agent.tools bridge, not only the top-level model tool-call surface (see
// core/codeact_tool_union.hpp). Deliberately NOT in the top-level model-callable universe
// (round_universe/scope_tools_to_mounted_skills below still only ever declares execute_code/
// mount_skill) -- word_count is reachable ONLY as agent.tools.word_count(...) from inside a
// running execute_code call, once the codeact-demo skill (below) is mounted.
struct WordCountArgs {
    std::string text;
};
AE_JSON_SCHEMA(WordCountArgs, text)
struct WordCountReply {
    int count = 0;
};
AE_JSON_SCHEMA(WordCountReply, count)

struct WordCountTool : Tool<WordCountTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "word_count";
    static constexpr std::string_view description =
        "Counts whitespace-separated words in a string. A trivial, capability-free demo tool -- "
        "reachable from agent.tools only once the codeact-demo skill is mounted.";
    using Args = WordCountArgs;
    using Reply = WordCountReply;

    static result<Reply> invoke(Args a, EffectContext&) {
        int count = 0;
        bool in_word = false;
        for (char c : a.text) {
            bool const is_space = (c == ' ' || c == '\t' || c == '\n');
            if (!is_space && !in_word) {
                ++count;
                in_word = true;
            } else if (is_space) {
                in_word = false;
            }
        }
        return Reply{count};
    }
};

// ---- The demo skill that unlocks word_count -- codeact_tool_union.hpp's own proof fixture ---------
// An InlineSkillSource (skill_source.hpp), the same "no disk I/O" shape builtin_skills.hpp uses for
// the five real shipped skills, alongside them via demo_skill_sources() below rather than replacing
// them -- this file's own skill-resolution call sites (main()'s startup_skills,
// ToolDeclaringHistoryProvider's skills_, print_skills_banner) all read the SAME source list, so
// mounting/materializing/advertising this demo skill goes through exactly the real machinery a
// third-party-authored skill would.
inline constexpr std::string_view kCodeactDemoSkillMd = R"SKILL(---
name: codeact-demo
description: Demonstrates that a mounted skill's allowed-tools becomes callable from inside agent.tools (CodeAct), not only as a top-level model tool call. Mount this to unlock word_count for Python code.
allowed-tools: word_count
metadata:
  version: "1"
---
# CodeAct demo

Once mounted, `agent.tools.word_count(text=...)` becomes callable from inside `execute_code` --
proving a skill's `allowed-tools` reaches CodeAct's bridge, not just the top-level tool-call
surface. Try:

```python
from agent import tools
r = tools.word_count(text="a real bridged call, not a stub")
print(r.count)
```
)SKILL";

[[nodiscard]] inline SkillSourceDescriptor make_codeact_demo_skill_source() {
    std::string const text(kCodeactDemoSkillMd);
    auto skill = parse_skill_md(text, "codeact-demo");
    std::vector<SkillBundleFile> files;
    if (skill) {
        std::vector<std::byte> bytes(reinterpret_cast<std::byte const*>(text.data()),
                                       reinterpret_cast<std::byte const*>(text.data()) + text.size());
        files.push_back(SkillBundleFile{"SKILL.md", std::move(bytes)});
    }
    std::vector<SkillSourceResult> resolved;
    if (skill) resolved.push_back(SkillSourceResult{std::move(*skill), std::move(files)});
    return make_skill_source_descriptor(InlineSkillSource("cli_demo", std::move(resolved)));
}

// The ONE source list every independent SkillsProvider<> instance in this file resolves from --
// main()'s startup_skills and ToolDeclaringHistoryProvider's own skills_ (ADR-030 folded the
// former THIRD instance, shared_codeact_skills(), into reusing skills_ directly -- see that
// provider's own real_execute_code()) -- so both independently reach byte-identical results, the
// same "resolve the SAME deterministic source, never a differently-scoped one" invariant this
// file's own comments already establish for the builtin-only case.
[[nodiscard]] inline std::vector<SkillSourceDescriptor> demo_skill_sources() {
    return {make_builtin_skills_source(), make_codeact_demo_skill_source()};
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

    // ADR-030: this static invoke() is now an unreachable poison sentinel, not the real
    // implementation -- `ToolDeclaringHistoryProvider::on_context()` below builds this tool's
    // descriptor via `make_tool_descriptor_with_invoke`, whose closure captures `this` and calls
    // `real_execute_code()` instead. `ExecuteCodeTool` still declares the real
    // `Capabilities<...>`/`EffectClass<...>` policies -- `make_tool_descriptor_with_invoke` still
    // extracts those from `ToolT` at compile time (ADR-028 §4) -- only the invoke BODY moved.
    static result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(error{failure_class::fatal,
                                      "ExecuteCodeTool::invoke() must never run directly -- this "
                                      "tool is only ever reached through "
                                      "ToolDeclaringHistoryProvider::real_execute_code()",
                                      "cli_chat.dead_static_invoke_path"});
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

    // ADR-030: same shape as `ExecuteCodeTool::invoke()` above -- unreachable poison, real logic in
    // `ToolDeclaringHistoryProvider::real_mount_skill()`.
    static result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(error{failure_class::fatal,
                                      "MountSkillTool::invoke() must never run directly -- this tool "
                                      "is only ever reached through "
                                      "ToolDeclaringHistoryProvider::real_mount_skill()",
                                      "cli_chat.dead_static_invoke_path"});
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
//
// ADR-030: `mounted_skills_`/`exec_state_` are now REAL session-scoped members (ADR-028's
// `make_tool_descriptor_with_invoke` mechanism), not process-global statics -- a real fix for a
// latent, never-exercised bug (this CLI only ever runs one session, but nothing used to stop a
// second one from silently sharing the first's mounted-skill/exec state). `execute_code` itself
// still reaches a SHARED, process-wide `MediatedPythonRunner` (see `shared_python_runner_binding()`
// above and `codeact_runner_binding.hpp` for exactly why that part can never be made per-session
// with this codebase's current, single-interpreter-per-process CPython embedding) -- `configure()`
// below is what claims this session's exclusive right to that shared runner, structurally failing
// closed (not silently racing) if a second session ever tried to reach the same one.
class ToolDeclaringHistoryProvider {
public:
    ToolDeclaringHistoryProvider() : skills_(demo_skill_sources()) {}

    // Host-only, configuration-time call (mirrors `AgentSession::set_capabilities()`/
    // `emplace_chat_client()` -- never derived from model output, I3), made once by `main()` via the
    // new `AgentSession::history_provider()` accessor, before the first `StartRun` that could reach
    // `execute_code`.
    //
    // ADR-034 CORRECTION (2026-08-12), UPDATED for ADR-037's rt::ThreadPool migration: does NOT
    // resolve `shared_python_runner_binding()` here -- that call is what lazily runs
    // `Py_InitializeFromConfig` on first touch (`shared_python_runner()`'s own lazy static), and
    // `configure()` is called from `main()`'s own thread. `execute_code` itself runs on
    // `rt::ThreadPool`'s one worker thread (via `run_start_job`, this file's own -- originally the
    // Quark Engine's worker thread before ADR-037 replaced it, same single-worker-thread property
    // either way) -- a real, reproduced crash (CPython's C API touched from a thread that never
    // acquired its GIL state, since nothing in `mediated_python_runner.cpp` calls
    // `PyGILState_Ensure`; this codebase's embedding was never built or verified for multi-thread
    // access, and hardening it is real, separate, out-of-scope follow-on work, not attempted here)
    // is exactly as reachable through a ThreadPool worker as it was through an Engine worker, so the
    // fix stays the same: defer BOTH the runner's construction/initialization AND the binding claim
    // to `real_execute_code()`'s own first call, which runs wherever `execute_code` itself runs (the
    // pool's one worker thread) -- init and every real Python call now consistently happen on the
    // SAME thread. `mount_roots`/`session_id` are just stored here; nothing here touches Python.
    [[nodiscard]] result<void> configure(
        std::string session_id, std::vector<native_jail::MaterializedSkillMount> mount_roots) {
        session_id_  = std::move(session_id);
        mount_roots_ = std::move(mount_roots);
        return {};
    }

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
        for (auto const& mounted_name : mounted_skills_.all()) {
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
        //
        // ADR-030: built via `make_tool_descriptor_with_invoke`, not `ToolTable::from_tools<...>()`
        // -- each closure captures `this`, reaching this provider's own `mounted_skills_`/
        // `exec_state_`/`runner_binding_`/`skills_` instead of the process-global statics the
        // pre-ADR-030 shape used. `ExecuteCodeTool`/`MountSkillTool` still supply their compile-time
        // `Capabilities<...>`/`EffectClass<...>` declarations (ADR-028 §4) -- only the invoke BODY
        // moved to `real_execute_code()`/`real_mount_skill()` below.
        std::vector<ToolDescriptor> universe_descriptors = {
            make_tool_descriptor_with_invoke<ExecuteCodeTool>(
                [this](ExecuteCodeArgs a, EffectContext& ctx) { return real_execute_code(std::move(a), ctx); }),
            make_tool_descriptor_with_invoke<MountSkillTool>(
                [this](MountSkillArgs a, EffectContext& ctx) { return real_mount_skill(std::move(a), ctx); }),
        };
        ToolTable const universe = ToolTable::from_descriptors(std::move(universe_descriptors));
        auto const scoped = scope_tools_to_mounted_skills(
            universe, skills_.allowed_tool_names_for(mounted_skills_.all()),
            {std::string(MountSkillTool::name)});
        contribution.tools = scoped.descriptors();
        co_return contribution;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

    [[nodiscard]] MountedSkillsState const& mounted_skills() const noexcept { return mounted_skills_; }

private:
    // The real `execute_code` implementation (ADR-030) -- was `ExecuteCodeTool::invoke()`'s body
    // before this pass, unchanged in substance, only in where it reaches its state FROM: `skills_`
    // (this provider's own member, replacing the third independent `shared_codeact_skills()`
    // instance -- confirmed by red-team to be a behavior-preserving consolidation, since all
    // instances in this file always resolved the identical `demo_skill_sources()`),
    // `mounted_skills_`/`exec_state_` (real per-session members, replacing
    // `shared_mounted_skills_state()`/`shared_exec_state()`), and `runner_binding_->runner()` (the
    // still-process-wide-shared interpreter, reached only if THIS session holds the claim --
    // `configure()` is what established that, at session start, not per-call).
    [[nodiscard]] result<ExecuteCodeReply> real_execute_code(ExecuteCodeArgs a, EffectContext& ctx) {
        if (session_id_.empty()) {
            return std::unexpected(error{failure_class::fatal,
                                          "execute_code was called before configure() ran",
                                          "cli_chat.codeact_not_configured"});
        }
        // ADR-034 CORRECTION: lazily resolved and bound HERE, on first use, on THIS call's own
        // thread -- see configure()'s own comment for why. `shared_python_runner_binding()`'s
        // lazy-static IIFE (cli_chat.cpp, near main()) runs its Py_InitializeFromConfig call the
        // first time this line executes, never before.
        if (runner_binding_ == nullptr) {
            auto& binding = shared_python_runner_binding(mount_roots_);
            auto  bound   = binding.bind(session_id_);
            if (!bound) return std::unexpected(bound.error());
            runner_binding_ = &binding;
        }
        auto& runner = runner_binding_->runner();
        if (!runner.ok()) {
            return std::unexpected(error{failure_class::fatal,
                                          "the embedded Python interpreter failed to initialize",
                                          "cli_chat.python_runner_not_initialized"});
        }

        // CodeAct tool-bridge union, recomputed fresh on EVERY execute_code call -- the identical
        // per-turn cadence scope_tools_to_mounted_skills already runs on for the model-facing
        // declaration side above, so a mount that happened this same round is reflected before this
        // call's own code runs, never a call behind. Three sources, per
        // core/codeact_tool_union.hpp: this CLI's own bridgeable tools (none today -- execute_code
        // and mount_skill are deliberately excluded from their own bridge), tools unlocked by
        // currently mounted skills, and MCP-sourced tools (none in this demo -- no MCP server is
        // connected here; see protocol/mcp/mcp_tool_bridge.hpp for that real, tested, separately
        // wired path).
        auto const codeact_universe = ToolTable::from_tools<WordCountTool>();
        auto const codeact_skill_tools = scope_tools_to_mounted_skills(
            codeact_universe, skills_.allowed_tool_names_for(mounted_skills_.all()));
        auto bridged = union_codeact_tools(ToolTable::from_tools<>(), codeact_skill_tools);
        if (!bridged) return std::unexpected(bridged.error());
        auto refreshed = runner.refresh_agent_tools(
            native_jail::ToolBridgeConfig{*bridged, /*capabilities=*/{}, /*approved=*/true});
        if (!refreshed) return std::unexpected(refreshed.error());

        // ADR-057 §9: `ctx.codeact_preseeded_answers` is host-driven replay state, set (only) by
        // `rt::AgentSession::resolve_interaction()`'s `codeact_ask` branch immediately before
        // re-invoking this SAME function against a STORED script -- see that field's own comment on
        // `EffectContext` for why it is threaded this way rather than as a new `ExecuteCodeArgs`
        // field. Empty for every ordinary, model-issued call (the common case), so this line changes
        // nothing about this tool's existing behavior for a script that never calls `agent.ask()`.
        ExecRequest req{a.language.empty() ? "python" : a.language, a.code, ctx.codeact_preseeded_answers};
        auto outcome = runner.run(req, exec_state_, ctx);
        if (!outcome) return std::unexpected(outcome.error());

        // ADR-057 §9: an ask-pending outcome is mapped to a ToolResult carrying the sentinel error
        // code `codeact.ask_pending`, with the prompt as the error's own message -- NEVER folded as
        // an ordinary success or failure. `rt::AgentSession::run_rounds()`/`resolve_interaction()`
        // (rt/agent_session.hpp) is what recognizes this exact code and turns it into a real
        // suspend/resume cycle; this CLI's own driver (main(), above) has no such wiring, so a script
        // calling `agent.ask()` here simply surfaces as an ordinary tool failure with this code.
        if (outcome->klass == exec_outcome_class::ask_pending) {
            return std::unexpected(error{failure_class::contract, outcome->ask_prompt, "codeact.ask_pending"});
        }

        ExecuteCodeReply reply;
        reply.ok = (outcome->klass == exec_outcome_class::ok);
        reply.stdout_text = outcome->stdout_text;
        reply.stderr_text = outcome->stderr_text;
        reply.result_repr = outcome->result_repr;
        return reply;
    }

    // Real `mount_skill` implementation (ADR-030) -- was `MountSkillTool::invoke()`'s body; now
    // checks against this provider's own `mount_roots_` (set once by `configure()`) and mutates
    // this provider's own `mounted_skills_`, instead of the process-global statics the pre-ADR-030
    // shape used.
    [[nodiscard]] result<MountSkillReply> real_mount_skill(MountSkillArgs a, EffectContext&) {
        bool known = false;
        for (auto const& [mount_id, host_dir] : mount_roots_) {
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
        mounted_skills_.mount(a.skill_name);
        return MountSkillReply{true, "mounted: " + a.skill_name};
    }

    SkillsProvider<> skills_;
    MountedSkillsState mounted_skills_;
    ExecState exec_state_;
    std::string session_id_;  // set by configure(); real_execute_code()'s "configured yet?" gate
    std::vector<native_jail::MaterializedSkillMount> mount_roots_;
    // Non-owning -- points at `shared_python_runner_binding()`'s own function-local static (process
    // lifetime), lazily resolved by `real_execute_code()`'s first call (ADR-034 correction; see that
    // function's own comment for why NOT resolved eagerly by configure()/main() anymore).
    CodeActRunnerBinding<native_jail::MediatedPythonRunner>* runner_binding_ = nullptr;
};
static_assert(ContextProvider<ToolDeclaringHistoryProvider>);

using CliSession = agentengine::rt::AgentSession<openai::OpenAIChatClient<InMemorySecretStore>,
                                                   agentengine::rt::NoSessionState,
                                                   ToolDeclaringHistoryProvider>;
static_assert(std::is_default_constructible_v<CliSession>);

// ADR-037: wraps one turn's start_run() call as a task<void> job for rt::ThreadPool::submit() (which
// only accepts task<void> -- see thread_pool.hpp's own contract), stashing the real result in a
// shared slot the caller reads back after the job completes. Explicitly agentengine::rt::task<void>
// here for clarity at the call site (historical: during an intermediate ADR-037 phase, core/task.hpp
// briefly split task<T> per-T and the bare `task<>` alias resolved to quark::task<void> -- a
// DIFFERENT, non-awaitable type that would not have compiled against ThreadPool::submit()'s
// signature; that split is long gone, `task<>` now resolves to `agentengine::rt::task<void>` too, so
// the qualification here is a style choice, not a correctness requirement).
[[nodiscard]] agentengine::rt::task<void> run_start_job(
    CliSession& actor, agentengine::rt::StartRun req,
    std::shared_ptr<agentengine::result<agentengine::rt::AgentResponse>> out) {
    *out = co_await actor.start_run(std::move(req));
    co_return;
}

[[nodiscard]] Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

// `tool_calls_of`/`text_of`/`tool_results_message` now come from
// agentengine/core/tool_call_extraction.hpp (`using namespace agentengine;` above) -- the external
// round loop that used to need them here moved inside `AgentSession::handle()` itself; only
// `text_of` is still called directly, to print the session's final converged answer.

void print_skills_banner(std::vector<native_jail::MaterializedSkillMount> const& materialized,
                          SkillsProvider<>& startup_skills, MountedSkillsState const& mounted_skills) {
    std::cout << "Skills RESOLVED at /skills/<name> -- every one's files are unconditionally readable "
                 "from turn 1 (009 §8b, unaffected by mount state):\n";
    for (auto const& source : demo_skill_sources()) {
        auto skills = source.load_skills();
        if (skills) {
            for (auto const& s : *skills) {
                std::cout << "  - " << s.skill.frontmatter.name << ": "
                           << s.skill.frontmatter.description << "\n";
            }
        } else {
            std::cout << "  (failed to resolve '" << source.origin_id
                       << "': " << skills.error().message << ")\n";
        }
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
    auto const& mounted = mounted_skills.all();
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

// Renders one RunEvent (013 §1's real, currently-emitted vocabulary — run/turn/model-call/tool-call
// boundaries; AgentSession::handle() fires these today, see agent_session.hpp's own emit_run_event
// call sites) as one human-readable line. NOT token-by-token streaming: AgentSession's internal
// loop calls the blocking ChatClient::chat(), never chat_stream(), so a model_delta event never
// fires here -- what this renders is the real structural trace of one turn (which tools ran, in
// what order, whether they succeeded), drained right after the turn's ask() resolves, not
// interleaved with generation in real wall-clock time.
[[nodiscard]] std::string describe_event(RunEvent const& ev) {
    switch (ev.kind) {
        case run_event_kind::run_started: return "run started";
        case run_event_kind::run_finished: return "run finished";
        case run_event_kind::run_canceled: return "run canceled";
        case run_event_kind::run_failed: {
            auto const& p = std::get<run_event_payload::RunFailed>(ev.payload);
            return "run FAILED: " + p.message + " (" + p.error_code + ")";
        }
        case run_event_kind::turn_started: {
            auto const& p = std::get<run_event_payload::Turn>(ev.payload);
            return "turn " + std::to_string(p.turn_index) + " started";
        }
        case run_event_kind::turn_finished: {
            auto const& p = std::get<run_event_payload::Turn>(ev.payload);
            return "turn " + std::to_string(p.turn_index) + " finished";
        }
        case run_event_kind::model_call_started: return "  thinking... (calling the model)";
        case run_event_kind::model_call_finished: return "  model responded";
        case run_event_kind::tool_call_started: {
            auto const& p = std::get<run_event_payload::ToolCallStarted>(ev.payload);
            return "  -> calling tool '" + p.tool_name + "' (call_id=" + p.call_id + ")";
        }
        case run_event_kind::tool_call_finished: {
            auto const& p = std::get<run_event_payload::ToolCallFinished>(ev.payload);
            return std::string("  <- tool call ") + (p.ok ? "OK" : "FAILED") +
                   " (call_id=" + p.call_id + ")";
        }
        case run_event_kind::input_required: return "  [suspended: waiting for input]";
        case run_event_kind::input_resolved: return "  [resumed]";
        case run_event_kind::approval_requested: return "  [suspended: waiting for human approval]";
        case run_event_kind::approval_resolved: {
            auto const& p = std::get<run_event_payload::ApprovalResolved>(ev.payload);
            return std::string("  [approval ") + (p.approved ? "GRANTED" : "DENIED") + "]";
        }
        case run_event_kind::warning: {
            auto const& p = std::get<run_event_payload::Warning>(ev.payload);
            return "  [warning] " + p.message;
        }
        default: return "  [event]";  // auth_*/policy_decision/artifact_produced/sandbox_exec_*/
                                       // tool_call_delta/model_delta: real kinds, no emitter yet
                                       // (agent_session.hpp) -- kept generic rather than silently
                                       // dropped, so a future emitter is visible here immediately.
    }
}

// Reasoning/<think>-channel extraction (ADR-023, generalized ADR-035 Phase 1) only populates a real
// `Reasoning` content item when scanning is armed -- `actor.set_scan_response_format_leaks(true)` in
// main() below, applied post-hoc by AgentSession's own run_model_call() regardless of streaming.
[[nodiscard]] std::vector<std::string> reasoning_texts_of(Message const& m) {
    std::vector<std::string> out;
    for (ContentItem const& item : m.content) {
        if (auto const* r = std::get_if<Reasoning>(&item.value)) out.push_back(r->text);
    }
    return out;
}

}  // namespace

int main() {
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
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
    SkillsProvider<> startup_skills(demo_skill_sources());
    std::filesystem::path const skills_scratch =
        std::filesystem::temp_directory_path() / "agentengine_cli_chat_workspace" / "skills";
    auto materialized =
        native_jail::materialize_skill_mounts(startup_skills, skills_scratch, {kWorkMount});
    if (!materialized) {
        std::cerr << "FATAL: failed to materialize skill mounts: " << materialized.error().message << "\n";
        return 1;
    }

    InMemorySecretStore store;
    store.set(kSecretName, *key_env);
    std::vector<Capability> grants = {
        Capability{cap::Secret{kSecretName, std::chrono::seconds{0}}},
        Capability{cap::FsRead{kWorkMount, "", std::nullopt}},
        Capability{cap::FsWrite{kWorkMount, "", std::nullopt, std::nullopt}}};
    for (auto const& [mount_id, host_dir] : *materialized) {
        (void)host_dir;
        grants.push_back(Capability{cap::FsRead{mount_id, "", std::nullopt}});
    }
    CapabilitySet held = CapabilitySet::grant_root(std::move(grants));

    ChatClientCapabilities caps;
    caps.streaming = true;
    caps.tool_calling = true;
    caps.reasoning = true;
    caps.max_output_tokens = 2048;

    // ADR-037: real per-token streaming needs the session's own execution to genuinely run
    // concurrently with a caller draining its event stream. Previously a real, single-worker
    // `quark::Engine` (`quark::TestKit` runs everything "on the calling thread... no threads, no
    // wall-clock" per its own top comment, so it could not host this) -- now a single-worker
    // `agentengine::rt::ThreadPool`: `CliSession` is a plain local value (rt::AgentSession has no
    // actor-framework construction ceremony to satisfy), driven per-turn by submitting a `task<void>`
    // job (`run_start_job`, above) that runs `start_run()` to completion on the pool's one worker
    // thread while THIS thread's drain loop (below) concurrently polls the event stream.
    agentengine::rt::ThreadPool pool(1);

    CliSession actor;
    // Fixed and stable for this CLI's whole lifetime (one process, one session) -- passed below as
    // both AgentSession's own session_id AND the ChatClient's `end_user_id` (the OpenAI-compatible
    // `user` body field), so a caching-aware provider/gateway sees a CONSISTENT identity across
    // every call this process ever makes, not a fresh one per call, real Anthropic/OpenAI-side
    // prompt-cache locality depends on -- a random or per-call value would never keep a cache hit.
    std::string const session_id = "cli-chat-session";
    // Trailing args beyond `caps`/`store`/`kPathPrefix` are every one of OpenAIChatClient's own
    // defaults spelled out verbatim (chat_client.hpp's own constructor), EXCEPT two: `end_user_id`
    // (see above) and `scan_response_format_leaks=true`, which arms ADR-023's Reasoning/<think>-
    // channel extraction on this client's OWN `chat()`. Left armed only so the non-streaming path
    // this file no longer takes stays correct if streaming is ever toggled off -- with streaming
    // engaged below, this specific flag never fires (chat() is never called); the REAL scanning this
    // CLI relies on now comes from AgentSession's own flag, set right after set_stream_model_calls()
    // below (ADR-035 Phase 1 -- see that flag's own comment in agent_session.hpp for why it's the
    // backend/path-agnostic one).
    actor.emplace_chat_client(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store,
                                kPathPrefix, sandbox::resolve_host, std::string{}, std::string{},
                                std::string{}, session_id, std::nullopt,
                                sandbox::ProviderTransport::tls,
                                /*scan_response_format_leaks=*/true);
    // Unbounded by default now (agent_session.hpp's own initialize(), 2026-08-12 change) -- this
    // CLI no longer caps the internal tool-call loop's own round count at all.
    actor.initialize(session_id, Principal{"cli-user", ""});
    actor.set_capabilities(&held);
    // ADR-034: real token-by-token streaming, traded for failover/circuit-breaker-feedback not
    // applying on this path -- see set_stream_model_calls()'s own comment in agent_session.hpp. A
    // run_event_kind::warning fires once per run naming this trade; describe_event() below renders
    // it like any other event.
    actor.set_stream_model_calls(true);
    // ADR-035 Phase 1: the scan that actually matters for this CLI now that streaming is always on
    // -- runs post-hoc, once per round, on the reconstructed Message (agent_session.hpp's
    // run_model_call()), so reasoning_texts_of() below sees real extracted Reasoning items again.
    // NAMED RESIDUAL: this does NOT retroactively clean up the LIVE model_delta printout further
    // down (the drain thread prints raw text token-by-token AS it streams, before a round -- and
    // therefore this scan -- has even finished); a leaked `<think>`/Harmony/etc block still appears
    // once, raw, live, and then AGAIN, cleaned up, in the post-round `reasoning_texts_of()` output.
    // Fixing the live path would mean buffering and re-scanning partial deltas mid-stream, a
    // materially bigger feature than this pass's scope -- not attempted here.
    actor.set_scan_response_format_leaks(true);
    // 013 §1's real run-event stream -- fires run/turn/model-call/tool-call/model-delta boundaries
    // as AgentSession::handle() actually reaches them. Enabled once, for the whole session; the
    // drain thread below (per turn) is what actually prints model_delta text AS it arrives.
    auto event_stream = actor.enable_event_stream(std::pmr::get_default_resource());
    // `ExecuteCodeTool`/`MountSkillTool` declare no `Approval<M>` policy (`approval_mode::
    // never_require`, tool.hpp's own fail-open default), so `invoke_tool`'s step 5 never consults a
    // decider for either -- no `set_approval_decider()` call is needed for this demo to keep working.

    // ADR-030: hands the provider its real per-session mount-root/session-id knowledge -- must
    // happen before the first StartRun that could reach execute_code. ADR-034 correction: no
    // longer resolves/claims the shared runner binding here (that's deferred to
    // real_execute_code()'s own first call now, on whatever thread execute_code itself runs on --
    // see ToolDeclaringHistoryProvider::configure()'s own comment for why).
    auto configured = actor.history_provider().configure(session_id, *materialized);
    if (!configured) {
        std::cerr << "FATAL: failed to configure the CodeAct provider: " << configured.error().message
                   << "\n";
        return 1;
    }

    print_skills_banner(*materialized, startup_skills, actor.history_provider().mounted_skills());
    std::cout << "\nType a message and press Enter. Type 'exit' or 'quit' to stop.\n\n";

    std::string line;
    while (true) {
        std::cout << "You: ";
        if (!std::getline(std::cin, line)) break;
        if (line == "exit" || line == "quit") break;
        if (line.empty()) continue;

        // One start_run() now resolves the WHOLE multi-round tool conversation for this turn
        // internally (AgentSession::run_rounds()'s own loop), running on the ThreadPool's one worker
        // thread (submitted via run_start_job, above). This thread blocks on the returned
        // std::future while a dedicated drain thread concurrently polls `event_stream` and prints
        // model_delta text as it actually arrives -- genuine token-by-token output, not a trace
        // assembled after the fact. `std::jthread`'s destructor requests stop and joins automatically
        // on every exit path (including an exception escaping the block below), so the drain thread
        // never outlives this scope.
        auto result_slot =
            std::make_shared<agentengine::result<agentengine::rt::AgentResponse>>();
        agentengine::rt::JobOutcome outcome;
        {
            bool mid_line = false;
            std::jthread drain([&](std::stop_token stop) {
                auto drain_once = [&] {
                    while (std::optional<RunEvent> ev = event_stream.next()) {
                        if (ev->kind == run_event_kind::model_delta) {
                            auto const& d = std::get<run_event_payload::ModelDelta>(ev->payload);
                            std::cout << d.text_delta;
                            std::cout.flush();
                            mid_line = true;
                        } else {
                            if (mid_line) { std::cout << "\n"; mid_line = false; }
                            std::cout << describe_event(*ev) << "\n";
                        }
                    }
                };
                while (!stop.stop_requested()) {
                    drain_once();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                drain_once();  // whatever landed between the last poll and stop being requested
                if (mid_line) std::cout << "\n";
            });
            std::future<agentengine::rt::JobOutcome> fut = pool.submit(
                run_start_job(actor, agentengine::rt::StartRun{user_message(line)}, result_slot));
            outcome = fut.get();  // blocks until the worker finishes this turn's whole round loop
        }  // drain's destructor: request_stop() then join(), guaranteed before `result_slot` is read

        if (outcome.faulted) {
            std::cout << "[internal error: start_run() faulted unexpectedly]\n";
            continue;
        }
        agentengine::result<agentengine::rt::AgentResponse> const& resp = *result_slot;
        if (!resp.has_value()) {
            std::cout << "[error: " << resp.error().message << "]\n";
            continue;
        }
        for (std::string const& reasoning : reasoning_texts_of(resp->message)) {
            std::cout << "  [thinking] " << reasoning << "\n";
        }
        // No separate "Agent: <text>" line: every round's text (including the final converging
        // round's) was already printed token-by-token via model_delta above, live, as it was
        // actually generated -- repeating it here would just duplicate it.
    }

    std::cout << "Goodbye.\n";
    std::cout.flush();
    // ADR-034 CORRECTION, UPDATED for ADR-037: `shared_python_runner()`'s process-lifetime singleton
    // is destroyed as a function-local static during normal process exit -- on THIS (main()'s)
    // thread, via the ordinary C++ static-destruction chain. Since real_execute_code()'s lazy init
    // (see its own comment) now runs `Py_InitializeFromConfig` on rt::ThreadPool's one worker thread
    // whenever execute_code is ever actually called, a normal `return` here would let
    // `~MediatedPythonRunner()`'s
    // `Py_Finalize()` run on a DIFFERENT thread than the one that initialized -- reproduced for real
    // as a hard STATUS_ACCESS_VIOLATION crash on process exit, CPython's own finalization having the
    // same single-thread-affinity expectation as initialization. Every other real caller of
    // `MediatedPythonRunner` (the test suite's own single-threaded fixtures) never hits this, so the
    // shared class itself is untouched -- this is scoped to the one caller that made init and
    // finalize disagree about which thread owns them.
    //
    // Same fix this project already applied once for a related CPython-teardown hazard
    // (tests/test_python_subinterpreter_spike.cpp, ADR-002 Section 11): skip the now-known-dangerous
    // teardown path entirely and let the OS reclaim the process. Nothing here needs graceful
    // shutdown -- no other process depends on this one's static destructors running.
    std::_Exit(0);
}

#else   // AGENTENGINE_WITH_HTTPS
#include <cstdio>
int main() {
    std::fprintf(stderr, "cli_chat: not built -- requires AGENTENGINE_WITH_HTTPS to be ON.\n");
    return 1;
}
#endif
