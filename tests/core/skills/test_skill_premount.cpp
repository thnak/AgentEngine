// decisions/ADR-224-host-premounted-builtin-skills.md (GitHub issue #47): a host pre-mounts the
// built-in skill paired with a tool it declares, BEFORE the first model call, and the mount is
// attributed to the host -- in `MountedSkillsState`, and in the system text the model is shown.
//
//   M1-M3  MountedSkillsState provenance: `mount()` records model, `mount_by_host()` records host, the
//          first mount fixes the origin, and neither path can rewrite the other's.
//   M4     premount_skills_by_host(): mounts only resolvable names; reports the rest.
//   E1-E4  End to end through a real rt::AgentSession and a strict scripted model: the shell-pipelines
//          body is in the FIRST request, under the host heading; a model `mount_skill` of the same
//          skill leaves it host-attributed; a model mount of another skill is labelled as the model's.
//   E5     Positive control: the same session without the host pre-mount shows no body in its first
//          request -- so E1 is caused by the pre-mount, not by something else putting it there.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/builtin_skills.hpp"
#include "agentengine/core/mounted_skills_state.hpp"
#include "agentengine/core/skill_premount.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/testing/scripted_chat_client.hpp"

namespace {

namespace ae = agentengine;

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

template <class T>
T drive(ae::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

struct MountSkillArgs {
    std::string skill_name;
};
AE_JSON_SCHEMA(MountSkillArgs, skill_name)
struct MountSkillReply {
    bool ok = false;
    std::string message;
};
AE_JSON_SCHEMA(MountSkillReply, ok, message)

// Same shape as tools/cli_chat.cpp's: the model-reachable mount path, reaching `MountedSkillsState::mount()`.
struct MountSkillTool : ae::Tool<MountSkillTool, ae::EffectClass<ae::effect_class::pure>> {
    static constexpr std::string_view name = "mount_skill";
    static constexpr std::string_view description = "Activate an advertised skill.";
    using Args = MountSkillArgs;
    using Reply = MountSkillReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext&) {
        return std::unexpected(ae::error{ae::failure_class::fatal, "unreachable", "test.unreachable"});
    }
};

// A minimal cli_chat-shaped history provider: one system message = the skills advertisement plus every
// mounted body via `render_mounted_skill_bodies`, then history; `mount_skill` bound to this provider.
class PremountProvider {
public:
    PremountProvider() : skills_(std::vector<ae::SkillSourceDescriptor>{ae::make_builtin_skills_source()}) {}

    // Host-only, configuration-time -- the cli_chat `premount_skills()` shape.
    ae::SkillPremountReport host_premount(std::vector<std::string> const& skills) {
        (void)skills_.ensure_loaded();
        std::vector<std::string> resolvable;
        for (auto const& m : skills_.mounted()) resolvable.push_back(m.mount_id);
        return ae::premount_skills_by_host(mounted_, skills, resolvable);
    }
    [[nodiscard]] ae::MountedSkillsState const& mounted() const { return mounted_; }

    ae::rt::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc, ae::EffectContext& ec) {
        auto adv = co_await skills_.on_context(sc, ec);
        if (!adv) co_return std::unexpected(adv.error());
        std::string text;
        if (!adv->messages.empty() && !adv->messages[0].content.empty()) {
            if (auto const* t = std::get_if<ae::Text>(&adv->messages[0].content[0].value)) text = t->text;
        }
        text += ae::render_mounted_skill_bodies(skills_, mounted_);
        ae::ContextContribution c;
        ae::Message m;
        m.role = ae::role::system;
        ae::ContentItem item;
        item.origin = ae::content_origin::system;
        item.value = ae::Text{std::move(text)};
        m.content.push_back(std::move(item));
        c.messages.push_back(std::move(m));
        c.messages.insert(c.messages.end(), sc.history.begin(), sc.history.end());
        c.tools.push_back(ae::make_tool_descriptor_with_invoke<MountSkillTool>(
            [this](MountSkillArgs a, ae::EffectContext&) -> ae::result<MountSkillReply> {
                mounted_.mount(a.skill_name);
                return MountSkillReply{true, "mounted: " + a.skill_name};
            }));
        co_return c;
    }
    ae::rt::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }

private:
    ae::SkillsProvider<> skills_;
    ae::MountedSkillsState mounted_;
};

ae::Message user_message(std::string text) {
    ae::Message m;
    m.role = ae::role::user;
    ae::ContentItem item;
    item.origin = ae::content_origin::user;
    item.value = ae::Text{std::move(text)};
    m.content.push_back(std::move(item));
    return m;
}

std::string system_text_of(ae::ChatRequest const& req) {
    for (ae::Message const& m : req.messages) {
        if (m.role != ae::role::system) continue;
        for (ae::ContentItem const& item : m.content) {
            if (auto const* t = std::get_if<ae::Text>(&item.value)) return t->text;
        }
    }
    return {};
}

bool contains(std::string const& hay, std::string_view needle) { return hay.find(needle) != std::string::npos; }

// A line that appears in shell-pipelines' body and nowhere in its advertisement.
constexpr std::string_view kShellBodyMarker = "## What stops a script";
constexpr std::string_view kHostHeading = "Skill 'shell-pipelines' (pre-mounted by the host";

}  // namespace

int main() {
    using ae::skill_mount_origin;

    // ---- M1-M3: MountedSkillsState provenance ------------------------------------------------------
    {
        ae::MountedSkillsState s;
        check(!s.origin_of("a").has_value(), "M1: an unmounted skill has no origin");
        s.mount("a");
        s.mount_by_host("b");
        check(s.origin_of("a") == skill_mount_origin::model, "M1: mount() records the model");
        check(s.origin_of("b") == skill_mount_origin::host, "M1: mount_by_host() records the host");
        s.mount("b");
        check(s.origin_of("b") == skill_mount_origin::host,
              "M2: a model mount of a host-mounted skill does not make it look model-earned");
        s.mount_by_host("a");
        check(s.origin_of("a") == skill_mount_origin::model,
              "M3: a host mount of a model-mounted skill does not rewrite it as the host's");
        check(s.all() == std::vector<std::string>({"a", "b"}), "M3: still exactly two mounts, in mount order");
    }

    // ---- M4: premount_skills_by_host -----------------------------------------------------------------
    {
        ae::MountedSkillsState s;
        s.mount("shell-pipelines");
        auto const r = ae::premount_skills_by_host(s, {"shell-pipelines", "using-the-code-interpreter", "nope"},
                                                   {"shell-pipelines", "using-the-code-interpreter"});
        check(r.mounted == std::vector<std::string>({"using-the-code-interpreter"}), "M4: mounts what it can");
        check(r.already_mounted == std::vector<std::string>({"shell-pipelines"}), "M4: reports an existing mount");
        check(r.not_resolvable == std::vector<std::string>({"nope"}) && !s.is_mounted("nope"),
              "M4: never mounts a name the session did not resolve");
        check(s.origin_of("shell-pipelines") == skill_mount_origin::model,
              "M4: an existing mount keeps its origin");
    }

    // ---- E1-E4: end to end, a real AgentSession ------------------------------------------------------
    {
        ae::testing::ScriptedChatClient script;
        // Turn 1: the model re-mounts the host's skill and mounts another; turn 2: answers.
        (void)script.push(ae::testing::tool_calls_turn({
            {"c1", "mount_skill", R"({"skill_name":"shell-pipelines"})"},
            {"c2", "mount_skill", R"({"skill_name":"using-codeact"})"},
        }));
        (void)script.push(ae::testing::text_turn("done"));

        ae::CapabilitySet held = ae::CapabilitySet::grant_root({});
        ae::rt::AgentSession<ae::testing::ScriptedChatClient, ae::rt::NoSessionState, PremountProvider> session;
        session.initialize("premount", ae::Principal{"p1", ""}, std::nullopt, /*max_turns=*/4);
        session.emplace_chat_client(script);
        session.set_capabilities(&held);

        // The host's action, before any model call -- what cli_chat does with its ToolPlan.
        auto const report = session.history_provider().host_premount(
            ae::builtin_skills_for_tools({"run_shell", "mount_skill"}));
        check(report.mounted == std::vector<std::string>({"shell-pipelines"}),
              "E1: the host pre-mounts shell-pipelines for a session declaring run_shell");
        check(script.call_count() == 0, "E1: ...before any model call");

        auto outcome = drive(session.start_run(ae::rt::StartRun{user_message("list the files")}));
        check(outcome.has_value(), "E1: the run completes");
        auto const reqs = script.requests();
        check(reqs.size() == 2, "E1: exactly two model calls");
        if (reqs.size() == 2) {
            std::string const first = system_text_of(reqs[0]);
            check(contains(first, kShellBodyMarker), "E1: the skill's body is in the FIRST model request");
            check(contains(first, kHostHeading), "E2: ...introduced as pre-mounted by the host");
            check(!contains(first, "Mounted skill 'shell-pipelines'"),
                  "E2: ...and never under the heading a model's own mount gets");
            std::string const second = system_text_of(reqs[1]);
            check(contains(second, kHostHeading) && !contains(second, "Mounted skill 'shell-pipelines'"),
                  "E3: after the model's own mount_skill of it, it is STILL presented as the host's");
            check(contains(second, "Mounted skill 'using-codeact':"),
                  "E4: a skill the model mounted itself is labelled as the model's");
        }
        auto const& m = session.history_provider().mounted();
        check(m.origin_of("shell-pipelines") == skill_mount_origin::host, "E3: provenance stays host");
        check(m.origin_of("using-codeact") == skill_mount_origin::model, "E4: provenance of the model's mount");
    }

    // ---- E5: positive control -- without the host pre-mount, the first request has no body ----------
    {
        ae::testing::ScriptedChatClient script;
        (void)script.push(ae::testing::text_turn("done"));
        ae::CapabilitySet held = ae::CapabilitySet::grant_root({});
        ae::rt::AgentSession<ae::testing::ScriptedChatClient, ae::rt::NoSessionState, PremountProvider> session;
        session.initialize("no-premount", ae::Principal{"p1", ""}, std::nullopt, /*max_turns=*/2);
        session.emplace_chat_client(script);
        session.set_capabilities(&held);
        auto outcome = drive(session.start_run(ae::rt::StartRun{user_message("hi")}));
        check(outcome.has_value(), "E5: the run completes");
        auto const reqs = script.requests();
        check(reqs.size() == 1 && contains(system_text_of(reqs[0]), "shell-pipelines:") &&
                  !contains(system_text_of(reqs[0]), kShellBodyMarker),
              "E5 control: no pre-mount -> the skill is only advertised, its body is absent");
    }

    if (g_failures == 0) {
        std::printf("test_skill_premount: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_skill_premount: %d check(s) failed\n", g_failures);
    return 1;
}
