// Design -> red-team -> prove -> judge for decisions/ADR-071-native-unsandboxed-process-execution-
// providers.md's native_capability_announcer.hpp: composing multiple Native*Provider instances
// (Shell/Bash/Python/Node) into ONE seeded instructions block + merged tool table, via the EXISTING
// ComposedContextProvider<Ms...> -- proving reuse actually works for this family, not just that it
// compiles. Runs against the REAL host PATH (this dev machine genuinely has cmd/bash/python/node
// installed) rather than a synthetic fixture, so this is also a realistic end-to-end demonstration
// of the four-family design intent the original request asked for.

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "backends/native_process/native_capability_announcer.hpp"
#include "support/run_task_sync.hpp"

using namespace agentengine;
using namespace agentengine::native_process;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

std::wstring widen(std::string const& utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

bool has_tool(ContextContribution const& c, std::string const& name) {
    for (auto const& d : c.tools) {
        if (d.name == name) return true;
    }
    return false;
}

}  // namespace

int main() {
    std::filesystem::path const worktree_path =
        std::filesystem::temp_directory_path() / "ae_native_announcer_test_worktree";
    std::error_code ec;
    std::filesystem::remove_all(worktree_path, ec);
    std::filesystem::create_directories(worktree_path);
    std::wstring const mount_root = widen(worktree_path.string());

    std::vector<Message> const empty_history;
    Principal const principal{};

    NativeShellProvider shell({"cmd"}, mount_root, "workdir");
    NativeBashProvider bash({"bash"}, mount_root, "workdir");
    NativePythonProvider python({"python"}, mount_root, "workdir");
    NativeNodeProvider node({"node"}, mount_root, "workdir");

    auto announcer = make_native_capability_announcer(std::move(shell), std::move(bash),
                                                        std::move(python), std::move(node));

    EffectContext ctx{};
    ctx.capabilities = std::make_shared<CapabilitySet>(CapabilitySet::grant_root({
        Capability{cap::NativeExec{"cmd", "workdir", std::nullopt, std::nullopt, std::nullopt}},
        Capability{cap::NativeExec{"bash", "workdir", std::nullopt, std::nullopt, std::nullopt}},
        Capability{cap::NativeExec{"python", "workdir", std::nullopt, std::nullopt, std::nullopt}},
        Capability{cap::NativeExec{"node", "workdir", std::nullopt, std::nullopt, std::nullopt}},
    }));
    SessionContext sc{"announcer-test", principal, empty_history};

    auto contribution = test_support::run_task_sync<result<ContextContribution>>(
        announcer.on_context(sc, ctx));
    AE_CHECK(contribution.has_value(), "A1: composed on_context() succeeds across all 4 providers");
    if (contribution.has_value()) {
        AE_CHECK(contribution->tools.size() == 4, "A2: all 4 providers' tools are merged into one table");
        AE_CHECK(has_tool(*contribution, "native_shell_run"), "A2: native_shell_run is present");
        AE_CHECK(has_tool(*contribution, "native_bash_run"), "A2: native_bash_run is present");
        AE_CHECK(has_tool(*contribution, "native_python_run"), "A2: native_python_run is present");
        AE_CHECK(has_tool(*contribution, "native_node_run"), "A2: native_node_run is present");

        AE_CHECK(contribution->instructions.has_value(), "A3: a combined instructions block is seeded");
        if (contribution->instructions.has_value()) {
            std::string const& text = contribution->instructions->unsafe_view();
            AE_CHECK(text.find("cmd") != std::string::npos, "A3: the combined text mentions the discovered shell");
            AE_CHECK(text.find("bash") != std::string::npos, "A3: the combined text mentions the discovered bash");
            AE_CHECK(text.find("python") != std::string::npos,
                      "A3: the combined text mentions the discovered python");
            AE_CHECK(text.find("node") != std::string::npos, "A3: the combined text mentions the discovered node");
        }
    }

    // ---- R-A4: a provider with NO matching grant contributes nothing to the composition, while ----
    // ---- its siblings still do (composition does not fail-all on one absent grant) ----------------
    {
        NativeShellProvider shell2({"cmd"}, mount_root, "workdir");
        NativePythonProvider python2({"python"}, mount_root, "workdir");
        auto announcer2 = make_native_capability_announcer(std::move(shell2), std::move(python2));

        EffectContext ctx2{};  // only cmd granted, NOT python
        ctx2.capabilities = std::make_shared<CapabilitySet>(CapabilitySet::grant_root(
            {Capability{cap::NativeExec{"cmd", "workdir", std::nullopt, std::nullopt, std::nullopt}}}));
        SessionContext sc2{"announcer-test-2", principal, empty_history};
        auto contribution2 = test_support::run_task_sync<result<ContextContribution>>(
            announcer2.on_context(sc2, ctx2));
        AE_CHECK(contribution2.has_value(), "R-A4 setup: composed on_context() still succeeds");
        if (contribution2.has_value()) {
            AE_CHECK(contribution2->tools.size() == 1,
                      "R-A4: only the granted provider's tool is present, not the ungranted one");
            AE_CHECK(has_tool(*contribution2, "native_shell_run"), "R-A4: native_shell_run IS present");
            AE_CHECK(!has_tool(*contribution2, "native_python_run"),
                      "R-A4: native_python_run is absent -- its own provider had no matching grant");
        }
    }

    std::filesystem::remove_all(worktree_path, ec);

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All native_capability_announcer (ADR-071) checks passed.\n";
    return 0;
}
