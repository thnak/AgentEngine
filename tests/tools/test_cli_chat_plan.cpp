// GitHub issue #47; decisions/ADR-224. tools/cli_chat_plan.hpp is the decision part of
// tools/cli_chat.cpp's wiring -- command line -> which shell tools are offered, which capability that
// mints, which tools need a human, which built-in skills the host pre-mounts. cli_chat.cpp itself needs a
// model, a network and an embedded interpreter; this does not, so the decision is tested here.

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "cli_chat_plan.hpp"

namespace {

namespace cc = agentengine::cli_chat;

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

bool has(std::vector<std::string> const& v, std::string_view s) {
    for (auto const& x : v) {
        if (x == s) return true;
    }
    return false;
}

cc::ToolPlan plan_of(std::vector<std::string_view> args) {
    auto o = cc::parse_args(args);
    return o ? cc::plan_tools(*o) : cc::ToolPlan{};
}

}  // namespace

int main() {
    // ---- P1: the default is the mediated tier, which mints no container authority ------------------
    {
        auto const o = cc::parse_args({});
        check(o.has_value() && o->shell == cc::shell_tier::mediated && o->premount_skills && !o->help,
              "P1: no arguments -> mediated tier, pre-mount on");
        cc::ToolPlan const p = cc::plan_tools(*o);
        check(p.offer_run_shell && !p.offer_run_command, "P1: default offers run_shell, not run_command");
        check(!p.mint_run_command_capability, "P1: default mints no cap::RunCommand");
        check(p.approval_required.empty(), "P1: default needs no approval (run_shell is mediated)");
        check(has(p.host_declared_tools, "run_shell") && has(p.host_declared_tools, "execute_code") &&
                  !has(p.host_declared_tools, "run_command"),
              "P1: declared tools are execute_code, mount_skill, run_shell");
        check(p.premount_skills == std::vector<std::string>({"shell-pipelines", "using-the-code-interpreter"}),
              "P1: default pre-mounts shell-pipelines and using-the-code-interpreter, in table order");
    }

    // ---- P2: container tier -- run_command only, its capability minted, every call approved ---------
    {
        cc::ToolPlan const p = plan_of({"--shell=container"});
        check(!p.offer_run_shell && p.offer_run_command, "P2: container offers run_command only");
        check(p.mint_run_command_capability, "P2: container mints cap::RunCommand");
        check(p.approval_required == std::vector<std::string>({"run_command"}),
              "P2: run_command, and only it, requires approval");
        check(has(p.premount_skills, "shell-pipelines"), "P2: run_command also gets shell-pipelines");
    }

    // ---- P3: both, and none ------------------------------------------------------------------------
    {
        cc::ToolPlan const both = plan_of({"--shell", "both"});
        check(both.offer_run_shell && both.offer_run_command && both.mint_run_command_capability &&
                  both.approval_required == std::vector<std::string>({"run_command"}),
              "P3: both offers both, approval only on run_command ('--shell VALUE' form)");
        check(both.premount_skills == std::vector<std::string>({"shell-pipelines", "using-the-code-interpreter"}),
              "P3: two tools paired with one skill pre-mount it once");
        cc::ToolPlan const none = plan_of({"--shell=none"});
        check(!none.offer_run_shell && !none.offer_run_command && !none.mint_run_command_capability,
              "P3: none offers no shell tool and mints nothing for one");
        check(none.premount_skills == std::vector<std::string>({"using-the-code-interpreter"}),
              "P3: none still pre-mounts the interpreter skill, and not shell-pipelines");
    }

    // ---- P4: --no-premount-skills turns the host pre-mount off, tools unchanged --------------------
    {
        cc::ToolPlan const p = plan_of({"--no-premount-skills"});
        check(p.premount_skills.empty(), "P4: --no-premount-skills -> nothing pre-mounted");
        check(p.offer_run_shell && !p.offer_run_command, "P4: ...and the tool choice is untouched");
    }

    // ---- P5: bad input fails closed, never falls back to some tier ---------------------------------
    {
        check(!cc::parse_args({"--shell=containr"}), "P5: an unknown tier is an error");
        check(!cc::parse_args({"--shell"}), "P5: --shell with no value is an error");
        check(!cc::parse_args({"--shell="}), "P5: an empty tier is an error");
        check(!cc::parse_args({"--bogus"}), "P5: an unknown argument is an error");
        check(!cc::parse_args({"container"}), "P5: a bare positional is an error");
        auto const last = cc::parse_args({"--shell=container", "--shell=mediated"});
        check(last && last->shell == cc::shell_tier::mediated, "P5: a repeated --shell: the last one wins");
        auto const help = cc::parse_args({"-h"});
        check(help && help->help, "P5: -h asks for help");
        check(cc::help_text().find("--shell=") != std::string_view::npos &&
                  cc::help_text().find("default: mediated") != std::string_view::npos &&
                  cc::help_text().find("--no-premount-skills") != std::string_view::npos,
              "P5: --help documents both flags and the default");
    }

    // ---- P6: builtin_skills_for_tools is driven only by the host's own tool list -------------------
    {
        using agentengine::builtin_skills_for_tools;
        check(builtin_skills_for_tools({}).empty(), "P6: no tools -> no skills");
        check(builtin_skills_for_tools({"word_count", "mount_skill"}).empty(), "P6: unpaired tools -> no skills");
        check(builtin_skills_for_tools({"execute_code", "run_shell"}) ==
                  builtin_skills_for_tools({"run_shell", "execute_code"}),
              "P6: the result does not depend on the order the host listed its tools");
    }

    if (g_failures == 0) {
        std::printf("test_cli_chat_plan: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_cli_chat_plan: %d check(s) failed\n", g_failures);
    return 1;
}
