// Implements 009-Plugin-and-Extension-System.md §8 + decisions/ADR-024-skill-scoped-tool-and-mount-
// wiring.md's Phase 3 addendum, live: a real, self-contained `ContextProvider` (`FarmAdvisoryProvider`
// below -- its own `SkillsProvider<>` plus its own `MountedSkillsState`, the same shape
// tools/cli_chat.cpp's `ToolDeclaringHistoryProvider` uses, minus that file's Python/native_jail
// machinery, which this scenario has no need for) driven through a real remote model over OpenRouter.
//
// This is a NEW skill/tool pair, purpose-built for this file, not a reuse of the builtin five (009
// §8f) or of tests/test_rt_agent_session_skills_live_e2e.cpp's `execute_code`: a "farm-field-advisory"
// skill naming one tool, `check_field_weather`, framed around a concrete end-to-end scenario -- a rice
// farmer asking whether to spray or irrigate a named field today. The point of the scenario is the
// same as that file's: prove the model reaches for what the SKILL's advertisement names, in response
// to being asked for the skill by description, not the tool's literal name, which only works if the
// skill-mount round-trip (advertise -> mount_skill -> next turn's on_context re-injects the skill body
// and unlocks the tool -> the tool becomes callable) actually reaches and influences a real model, not
// just the outbound wire bytes.
//
// `check_field_weather` is a deterministic stand-in, not a real weather API -- same convention
// `examples/02_add_tools.cpp`'s `GetWeatherTool` and test_rt_agent_session_live_multitool_e2e.cpp's
// own weather tool already use; what this file proves is tool/skill SELECTION and the mount protocol,
// never a real forecast.
//
// Per this suite's own convention (test_rt_agent_session_live_multitool_e2e.cpp's top comment),
// assertions are structural only -- which tool was called, with what argument, whether the run
// converged -- never on the model's own prose.
//
// CREDENTIALS ARE NEVER COMPILED IN (018 §4) -- same environment-variable contract as every other live
// test in this suite: AGENTENGINE_OPENROUTER_API_KEY (required, else SKIP), AGENTENGINE_OPENROUTER_MODEL
// (optional), AGENTENGINE_OPENROUTER_HOST (optional). tools/run-live-provider-tests.ps1 populates these
// from a local, git-ignored key file; nothing here reads a file directly.
//
// Labelled `live-network` (tests/CMakeLists.txt) -- excluded from a default `ctest` run, and SKIPPED
// (exit 0), not failed, when the API key is absent, matching every other file in this group.

#ifdef AGENTENGINE_WITH_HTTPS

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/mounted_skills_state.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/core/skill_source.hpp"
#include "agentengine/core/skill_tool_scoping.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/pal/env.hpp"
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
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

// Same "safe here because nothing genuinely suspends externally" drive<T>() every other
// test_rt_agent_session*.cpp file uses.
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
constexpr char const* kSkillName = "farm-field-advisory";

// ---- Call logs -- AgentSession::start_run() resolves any tool call internally (ADR-027), so what
// actually got invoked, and with what argument, is observed from inside the tool bodies themselves,
// the same function-local-static idiom every other live test in this suite already uses. ----------
[[nodiscard]] bool& mount_skill_called_log() {
    static bool called = false;
    return called;
}
[[nodiscard]] bool& check_weather_called_log() {
    static bool called = false;
    return called;
}
[[nodiscard]] std::string& observed_field_name_log() {
    static std::string name;
    return name;
}

// ---- The new skill, purpose-built for this file -- names exactly one tool (check_field_weather) and
// teaches when to call it before answering an irrigation/spraying question. ------------------------
inline constexpr std::string_view kFarmFieldAdvisorySkillMd = R"SKILL(---
name: farm-field-advisory
description: Decide whether to irrigate or spray a rice field today, using the check_field_weather tool for that field's own local forecast. Use this before recommending an irrigation or pesticide action to a farmer for a named field.
allowed-tools: check_field_weather
metadata:
  version: "1"
---
# Farm field advisory

Before recommending irrigation or pesticide spraying for a named field, call `check_field_weather`
with that field's name. Do not guess the forecast or invent a number.

## Reading the forecast

- Rain expected today or tonight: postpone both irrigation and spraying -- irrigation is redundant,
  and spraying risks being washed off before it takes effect.
- Dry, no rain expected: irrigation may proceed if the field needs it; spraying may proceed if pest
  pressure warrants it.

Always state the forecast you actually received before giving the recommendation, so the farmer can
judge it against what they see in the sky themselves.
)SKILL";

// Eagerly parsed once, exactly like builtin_skills.hpp's own make_builtin_skills_source() -- a
// malformed constant is a bug in the literal above, caught the first time this runs, not silently
// swallowed. `InlineSkillSource` (skill_source.hpp) needs no disk I/O and no native_jail mount root.
[[nodiscard]] SkillSourceDescriptor make_farm_skill_source() {
    result<std::vector<SkillSourceResult>> resolved = [] {
        auto skill = parse_skill_md(kFarmFieldAdvisorySkillMd, kSkillName);
        if (!skill) return result<std::vector<SkillSourceResult>>(std::unexpected(skill.error()));

        std::string const text(kFarmFieldAdvisorySkillMd);
        std::vector<std::byte> bytes(reinterpret_cast<std::byte const*>(text.data()),
                                      reinterpret_cast<std::byte const*>(text.data()) + text.size());
        std::vector<SkillBundleFile> files;
        files.push_back(SkillBundleFile{"SKILL.md", std::move(bytes)});

        std::vector<SkillSourceResult> out;
        out.push_back(SkillSourceResult{std::move(*skill), std::move(files)});
        return result<std::vector<SkillSourceResult>>(std::move(out));
    }();

    // Propagated as-is if construction above failed -- resolve_and_mount() (skill_provider.hpp) fails
    // the whole on_context() call closed on a bad source, the correct place to observe it, exactly
    // matching builtin_skills.hpp's own make_builtin_skills_source() convention.
    SkillSourceDescriptor d;
    d.origin_id = "farm-advisory-test";
    d.load_skills = [resolved]() { return resolved; };
    return d;
}

// ---- check_field_weather -- the one tool the farm-field-advisory skill names. A deterministic
// stand-in, not a real weather API (see this file's own top comment). ------------------------------
struct FieldWeatherArgs {
    std::string field_name;
};
AE_JSON_SCHEMA(FieldWeatherArgs, field_name)

struct FieldWeatherReply {
    std::string forecast;
    std::string advice;
};
AE_JSON_SCHEMA(FieldWeatherReply, forecast, advice)

struct CheckFieldWeatherTool
    : Tool<CheckFieldWeatherTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "check_field_weather";
    static constexpr std::string_view description =
        "Gets today's local weather forecast for a named field, for deciding whether to irrigate or "
        "spray it.";
    using Args = FieldWeatherArgs;
    using Reply = FieldWeatherReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        check_weather_called_log() = true;
        observed_field_name_log() = a.field_name;
        return Reply{"Rain expected this afternoon, 80% chance, ~24mm.",
                     "Do not spray today -- rain would wash it off before it takes effect. Hold off "
                     "on irrigation too; the expected rainfall should cover the field's needs."};
    }
};

// ---- mount_skill -- the on-demand activation trigger (Skills Phase 3, ADR-024's addendum). Real
// state (this provider's own MountedSkillsState), no new authority: every resolved skill's files are
// already readable per the capability grant at session start; mounting only changes which tools get
// declared+invocable and whether the skill's full body is injected. Simplified from
// tools/cli_chat.cpp's own MountSkillTool -- no native_jail mount-root table to validate against,
// since this file has exactly one skill to know about. --------------------------------------------
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
    // ADR-030: unreachable poison -- real logic lives in FarmAdvisoryProvider::real_mount_skill(),
    // reached via make_tool_descriptor_with_invoke() below.
    static result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(error{failure_class::fatal,
                                      "MountSkillTool::invoke() must never run directly -- reached "
                                      "only through FarmAdvisoryProvider::real_mount_skill()",
                                      "farm_advisory.dead_static_invoke_path"});
    }
};

// ---- The ContextProvider: its own SkillsProvider<> (for the advertisement + allowed-tools lookup)
// plus its own MountedSkillsState (Phase 3's real per-session mount state) -- a complete conformer by
// itself, occupying AgentSession's single HistoryProviderT slot directly, no HistoryAndSkillsProvider
// composition needed (mirrors tools/cli_chat.cpp's own ToolDeclaringHistoryProvider for exactly the
// same reason: this provider already owns the whole skill-related system prompt). -------------------
class FarmAdvisoryProvider {
public:
    // decisions/ADR-066-context-provider-attribution-provenance.md §3.
    static constexpr std::string_view name = "farm-advisory-live-test";

    FarmAdvisoryProvider() : skills_({make_farm_skill_source()}) {}

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx,
                                                                 EffectContext& ec) {
        auto skills_contribution = co_await skills_.on_context(session_ctx, ec);
        if (!skills_contribution) co_return std::unexpected(skills_contribution.error());

        std::string combined_system_text;
        if (!skills_contribution->messages.empty() && !skills_contribution->messages[0].content.empty()) {
            if (auto const* t = std::get_if<Text>(&skills_contribution->messages[0].content[0].value)) {
                combined_system_text = t->text;
            }
        }
        // Full body for every currently mounted skill, re-derived fresh every call (never cached) --
        // see skill_provider.hpp's own top comment for why "resolved" and "mounted" are independent.
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

        // Recomputed every call -- skill_tool_scoping.hpp's own top comment: declaration and
        // invocation must be recomputed from the SAME live state, at the SAME cadence, never diverge.
        // AgentSession::run_rounds() rebuilds its ToolTable from THIS SAME on_context() result every
        // turn (agent_session.hpp), so that invariant holds here structurally, not by discipline.
        std::vector<ToolDescriptor> universe_descriptors = {
            make_tool_descriptor<CheckFieldWeatherTool>(),
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

private:
    [[nodiscard]] result<MountSkillReply> real_mount_skill(MountSkillArgs a, EffectContext&) {
        if (a.skill_name != kSkillName) {
            return std::unexpected(
                error{failure_class::contract, "unknown skill: " + a.skill_name, "skill.unknown_name"});
        }
        mount_skill_called_log() = true;
        mounted_skills_.mount(a.skill_name);
        return MountSkillReply{true, "mounted: " + a.skill_name};
    }

    SkillsProvider<> skills_;
    MountedSkillsState mounted_skills_;
};
static_assert(ContextProvider<FarmAdvisoryProvider>);

using FarmAdvisoryLiveSession =
    AgentSession<openai::OpenAIChatClient<InMemorySecretStore>, NoSessionState, FarmAdvisoryProvider>;
static_assert(std::is_default_constructible_v<FarmAdvisoryLiveSession>);

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
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(stderr,
                     "test_rt_agent_session_farm_advisory_live_e2e: SKIPPED -- "
                     "AGENTENGINE_OPENROUTER_API_KEY is not set.\n"
                     "  Run tools/run-live-provider-tests.ps1, or set the variable yourself, to "
                     "exercise a real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_MODEL", kDefaultModel);
    std::string const host = env_or("AGENTENGINE_OPENROUTER_HOST", kDefaultHost);
    std::fprintf(stderr, "test_rt_agent_session_farm_advisory_live_e2e: host=%s model=%s\n",
                 host.c_str(), model.c_str());

    InMemorySecretStore store;
    store.set(kSecretName, *key_env);
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});

    ChatClientCapabilities caps;
    caps.streaming = true;
    caps.tool_calling = true;
    caps.max_output_tokens = 1024;

    FarmAdvisoryLiveSession session;
    session.emplace_chat_client(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store,
                                 kPathPrefix);
    session.initialize("farm-advisory-live-e2e-session", Principal{"live-e2e-principal", ""},
                        /*token_budget=*/std::nullopt,
                        /*max_turns=*/static_cast<std::uint64_t>(kMaxRounds));
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

    // ==================== Turn 2: the farmer's real question, phrased around the SKILL =============
    // Deliberately references what the skill's own advertisement DESCRIBES (deciding on irrigation/
    // spraying for a named field), not the tool's literal name "check_field_weather" -- the same
    // "ask for the skill, not the tool" convention test_rt_agent_session_skills_live_e2e.cpp uses, so
    // a real call proves the model actually read the mounted skill's advertisement/body, not that it
    // just happened to guess a plausible tool name.
    mount_skill_called_log() = false;
    check_weather_called_log() = false;
    observed_field_name_log().clear();

    result<AgentResponse> resp = drive(session.start_run(StartRun{user_message(
        "I'm a rice farmer. My field is called 'North Paddy'. Using whatever skill you have for "
        "advising on field irrigation and spraying decisions, please check whether I should spray "
        "pesticide on North Paddy today, and tell me your recommendation.")}));
    check(resp.has_value(),
          "TURN2: the session converges (resolves mount_skill then check_field_weather internally, "
          "within max_turns) against the real provider");
    if (resp.has_value()) {
        check(has_text(resp->message), "TURN2: the converged response carries a final recommendation");
    } else {
        note("TURN2 error.code", resp.error().code);
        note("TURN2 error.message", resp.error().message);
    }

    check(mount_skill_called_log(),
          "TURN2: mount_skill was called -- the model activated the advertised farm-field-advisory "
          "skill before acting on it");
    check(check_weather_called_log(),
          "TURN2: check_field_weather was actually called -- the live, behavioural proof that the "
          "mounted skill's body (not just its one-line advertisement) reached and influenced a real "
          "model's real tool selection, not just the outbound wire bytes");
    if (!observed_field_name_log().empty()) {
        note("check_field_weather field_name argument", observed_field_name_log());
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_rt_agent_session_farm_advisory_live_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_agent_session_farm_advisory_live_e2e: %d FAILURE(S)\n", g_failures);
    return 1;
}

#else   // AGENTENGINE_WITH_HTTPS
int main() { return 0; }
#endif  // AGENTENGINE_WITH_HTTPS
