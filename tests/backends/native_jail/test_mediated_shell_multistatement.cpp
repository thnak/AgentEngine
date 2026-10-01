// GitHub issues #140 and #141 -- MediatedShellRunner (ADR-096, ADR-100, ADR-208 §7).
// #141: every statement's stdout/stderr is returned in order, and an unquoted newline separates statements.
// #140: a `>` redirect resolves against the working directory, `cd /` goes to the mount root, and a `cd` to a
// missing path fails the statement and leaves the working directory unchanged.

#include <cstdio>
#include <filesystem>
#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_command_registry.hpp"
#include "backends/native_jail/mediated_filesystem_adapter.hpp"
#include "backends/native_jail/mediated_shell_runner.hpp"

using namespace agentengine;
using namespace agentengine::native_jail::mediated_shell;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                       \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "FAIL: %s at %s:%d\n", (label), __FILE__, __LINE__); \
            ++g_failures;                                                           \
        } else {                                                                    \
            std::printf("  ok: %s\n", (label));                                     \
        }                                                                           \
    } while (0)

}  // namespace

int main() {
    std::string const scratch = (std::filesystem::temp_directory_path() / "ae_issue_140_141_mount").string();
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);

    auto adapter = MediatedFileSystemAdapter::create(scratch);
    AE_CHECK(adapter.has_value(), "setup: adapter creates");
    if (!adapter) return 1;
    DefaultCommandRegistry registry;
    MediatedShellRunner shell(*adapter, registry, "work");

    CapabilitySet caps = CapabilitySet::grant_root({
        Capability{cap::FsRead{"work", "", std::nullopt}},
        Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
    });
    EffectContext ctx{};
    ctx.capabilities = agentengine::borrow_capabilities(caps);
    ExecState state{};
    auto run = [&](std::string const& src) { return shell.run(ExecRequest{"shell", src}, state, ctx); };

    // ---- #141 ----
    {
        auto r = run("echo one; echo two");
        AE_CHECK(r && r->stdout_text == "one\ntwo\n", "#141: ';'-separated statements all contribute stdout");
    }
    {
        auto r = run("echo one\necho two");
        AE_CHECK(r && r->stdout_text == "one\ntwo\n", "#141: a newline separates statements like ';'");
    }
    {
        auto r = run("echo 'a\nb'");
        AE_CHECK(r && r->stdout_text == "a\nb\n", "#141: a newline inside quotes is not a separator");
    }
    {
        auto r = run("mkdir c\necho y > c/d.txt");
        AE_CHECK(r && std::filesystem::exists(scratch + "/c/d.txt"), "#141: mkdir then redirect on the next line");
    }
    {
        auto r = run("echo a &&\necho b");
        AE_CHECK(r && r->stdout_text == "a\nb\n", "#141: a line may end in && and continue");
    }
    {
        auto r = run("if echo x\nthen\necho yes\nfi");
        AE_CHECK(r && r->stdout_text == "x\nyes\n", "#141: if/then on separate lines");
    }
    {
        auto r = run("for i in 1 2\ndo\necho $i\ndone");
        AE_CHECK(r && r->stdout_text == "1\n2\n", "#141: for/do on separate lines accumulates every iteration");
    }

    // ---- #140 ----
    {
        auto r = run("mkdir p; cd p; echo hi > r.txt; pwd");
        AE_CHECK(r && r->stdout_text == "/p\n", "#140: cd then pwd");
        AE_CHECK(std::filesystem::exists(scratch + "/p/r.txt"), "#140: redirect lands in the working directory");
        AE_CHECK(!std::filesystem::exists(scratch + "/r.txt"), "#140: ...and not at the mount root");
    }
    {
        auto r = run("cd /; pwd");
        AE_CHECK(r && r->stdout_text == "//\n" ? false : r && r->stdout_text == "/\n",
                 "#140: cd / returns to the mount root");
    }
    {
        auto r = run("cd nowhere");
        AE_CHECK(r && r->klass != exec_outcome_class::ok && !r->stderr_text.empty(),
                 "#140: cd to a missing directory fails with stderr");
        auto pwd = run("pwd");
        AE_CHECK(pwd && pwd->stdout_text == "/\n", "#140: a failed cd leaves the working directory unchanged");
    }
    {
        auto r = run("cd c/d.txt");
        AE_CHECK(r && r->klass != exec_outcome_class::ok, "#140: cd to a file fails");
    }

    std::filesystem::remove_all(scratch);
    return g_failures == 0 ? 0 : 1;
}
