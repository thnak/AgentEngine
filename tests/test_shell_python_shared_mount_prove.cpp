// PROVE-PHASE test (not a production wiring change to tools/cli_chat.cpp -- that file's real
// shared_python_runner() still uses its own separate "agentengine_cli_chat_workspace" scratch
// directory, untouched by this file). Proves, against the REAL MediatedShellRunner and the REAL
// MediatedPythonRunner (the identical two types test_mediated_shell_runner_python_composition.cpp
// already exercises), that pointing BOTH at the SAME real host directory for the "work" mount id
// actually gives them a shared, cross-visible filesystem view -- closing (for this standalone test,
// not for the shipped CLI) the real, disclosed gap confirmed in
// docs/planning/identity-native-sandbox-worktree-design.md §30: "Python's writes are invisible to
// this design's worktree entirely, today" and "never the same mount object MediatedShellRunner
// uses" -- both because, in the SHIPPED wiring, they are never pointed at the same directory at all.
// This test is the fix applied and proven, in isolation, before touching the real CLI.
//
// Only built when AGENTENGINE_BUILD_PYTHON_RUNNER is ON, mirroring
// test_mediated_shell_runner_python_composition.cpp's own real build gating exactly.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "agentengine/core/effect_context.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_command_registry.hpp"
#include "backends/native_jail/mediated_filesystem_adapter.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"
#include "backends/native_jail/mediated_shell_runner.hpp"

using namespace agentengine;
using namespace agentengine::native_jail::mediated_shell;
using agentengine::native_jail::MediatedPythonConfig;
using agentengine::native_jail::MediatedPythonRunner;
using agentengine::native_jail::NativeJailBackend;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::fprintf(stderr, "FAIL: %s at %s:%d\n", (label), __FILE__, __LINE__);              \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::printf("  ok: %s\n", (label));                                                    \
        }                                                                                           \
    } while (0)

void disable_crt_assert_dialog() {
#if defined(_WIN32)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

}  // namespace

int main() {
    disable_crt_assert_dialog();

    std::string const scratch = ::agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp") +
                                 "/ae_shell_python_shared_mount_prove";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    std::wstring scratch_w(scratch.begin(), scratch.end());

    // THE FIX under test: Python's mount_roots["work"] is explicitly pointed at the SAME real
    // directory Shell's own MediatedFileSystemAdapter uses -- unlike the real, shipped
    // tools/cli_chat.cpp::shared_python_runner(), whose own "work" mount is a DIFFERENT, separate
    // scratch directory, and unlike test_mediated_shell_runner_python_composition.cpp's own py_cfg,
    // which has NO "work" mount entry at all.
    NativeJailBackend backend;
    MediatedPythonConfig py_cfg;
    py_cfg.python_home = AE_PYTHON_HOME;
    py_cfg.mount_roots["work"] = scratch_w;
    MediatedPythonRunner python(std::move(py_cfg), backend);
    auto init = python.initialize();
    AE_CHECK(init.has_value(), "setup: real MediatedPythonRunner initializes, mount_roots[\"work\"] "
                                "explicitly set to the shared directory");

    auto adapter = MediatedFileSystemAdapter::create(scratch_w);
    AE_CHECK(adapter.has_value(), "setup: shell's MediatedFileSystemAdapter created over the SAME "
                                    "real directory");

    DefaultCommandRegistry registry;
    AE_CHECK(registry
                 .register_runner(RegisteredRunner{
                     "python",
                     [&](ExecRequest const& req, ExecState& state, EffectContext& ctx) -> result<ExecOutcome> {
                         return python.run(req, state, ctx);
                     }})
                 .has_value(),
             "setup: python registers as a composed Runner under the shell");

    MediatedShellRunner shell(*adapter, registry, "work");

    CapabilitySet caps = CapabilitySet::grant_root(
        {Capability{cap::RunnerCall{"python"}}, Capability{cap::FsRead{"work", "", std::nullopt}},
         Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}}});

    // === 1. Shell writes a real file; Python (invoked THROUGH shell, same ctx) reads it back =======
    {
        ExecState state{};
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);

        // NOTE, confirmed by this test's own first real run: shell's own mediated path grammar
        // (MediatedFileSystemAdapter's split_mount_path convention) takes paths RELATIVE to its own
        // mount root -- a LEADING '/' is rejected outright (worktree.mount_path_absolute). This is a
        // DIFFERENT convention from Python's own guest path grammar (split_guest_path,
        // python_worker_mediation.cpp), which requires the /<mount_id>/<rest> form. The two runners
        // share the same real DIRECTORY, but each keeps its own real path syntax for referring to it
        // -- a real, disclosed detail this test's first run surfaced, not assumed in advance.
        auto write_via_shell =
            shell.run(ExecRequest{"shell", "echo shared-mount-hello > from_shell.txt"}, state, ctx);
        if (!write_via_shell.has_value()) {
            std::fprintf(stderr, "DEBUG 1a error: code=%s message=%s\n",
                         write_via_shell.error().code.c_str(), write_via_shell.error().message.c_str());
        } else {
            std::fprintf(stderr, "DEBUG 1a: klass=%d stdout=[%s] stderr=[%s]\n",
                         static_cast<int>(write_via_shell->klass), write_via_shell->stdout_text.c_str(),
                         write_via_shell->stderr_text.c_str());
        }
        AE_CHECK(write_via_shell.has_value() && write_via_shell->klass == exec_outcome_class::ok,
                 "1a: shell writes a real file into the shared \"work\" mount");

        // Wrapped in a shell-level double-quoted word (stripped by expand_word(), same discipline
        // test_mediated_shell_runner_python_composition.cpp's own E3-PY4 already established) --
        // this test's own first real run confirmed the BARE, unwrapped form (with single quotes but
        // no wrapper) hits a real SyntaxError from the shell's own word-splitting mangling the
        // Python source before PythonRunner ever sees it, not a Python-side bug.
        auto read_via_python = shell.run(
            ExecRequest{"shell", "python \"print(open('/work/from_shell.txt').read().strip())\""},
            state, ctx);
        if (!read_via_python.has_value()) {
            std::fprintf(stderr, "DEBUG 1b error: code=%s message=%s\n",
                         read_via_python.error().code.c_str(), read_via_python.error().message.c_str());
        } else {
            std::fprintf(stderr, "DEBUG 1b: klass=%d stdout=[%s] stderr=[%s]\n",
                         static_cast<int>(read_via_python->klass), read_via_python->stdout_text.c_str(),
                         read_via_python->stderr_text.c_str());
        }
        AE_CHECK(read_via_python.has_value() &&
                     read_via_python->stdout_text.find("shared-mount-hello") != std::string::npos,
                 "1b: Python reads the SAME real file shell just wrote -- genuinely shared mount, "
                 "not two disconnected scratch directories");
    }

    // === 2. Python writes a real file; shell reads it back ==========================================
    {
        ExecState state{};
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);

        auto write_via_python = shell.run(
            ExecRequest{"shell",
                        "python \"with open('/work/from_python.txt', 'w') as f:\n    "
                        "f.write('shared-mount-goodbye')\""},
            state, ctx);
        AE_CHECK(write_via_python.has_value() && write_via_python->klass == exec_outcome_class::ok,
                 "2a: Python writes a real file into the shared \"work\" mount");

        // REAL, PREVIOUSLY-UNDISCLOSED FINDING from this test's own first real run (not assumed or
        // designed around in advance): native_jail_backend.cpp's exec-response handling does
        // `state.cwd = wp::get_string(*frame, "cwd")` UNCONDITIONALLY after every Python exec --
        // i.e. it overwrites the SHARED ExecState::cwd with the jailed Python WORKER PROCESS's own
        // real OS-level working directory (typically inherited from whatever process spawned it,
        // e.g. this test binary's own D:\...\build\tests), not a mount-relative logical path. This
        // corrupts `cwd` for any LATER shell-native command in the same session that resolves a
        // relative path against it (`cat`/`ls`/`cd`, via mediated_shell_dispatch.cpp's own
        // `resolve_against_cwd(state.cwd, ...)`) -- confirmed for real: state.cwd measured
        // "D:\\GitSrc\\AgentEngine\\build\\tests" immediately after the Python write above, and
        // `cat`'s own real path-character validator then correctly rejects the resulting absolute,
        // colon-containing combined path. This is a REAL defect in the shipped Shell<->Python
        // composition's cwd-sharing semantics -- RunShellTool's own description promises "State
        // (current directory...) persists across calls," but does not survive a Python call
        // correctly. Worked around HERE (reset to the shell's own logical root) so this test can
        // still prove the shared-MOUNT claim it exists for; the cwd-corruption bug itself is
        // reported separately, not silently fixed by this workaround.
        AE_CHECK(state.cwd.find(':') != std::string::npos || state.cwd.find('\\') != std::string::npos,
                 "REAL BUG CONFIRMED: after a Python-composed call, shared ExecState::cwd was "
                 "overwritten with the jailed worker's own raw host OS path (contains ':' or '\\\\'), "
                 "not a mount-relative logical cwd -- native_jail_backend.cpp's unconditional "
                 "`state.cwd = wp::get_string(*frame, \"cwd\")` after every exec_response");
        state.cwd.clear();  // workaround for THIS test only -- the real defect above is not fixed here
        auto read_via_shell = shell.run(ExecRequest{"shell", "cat from_python.txt"}, state, ctx);
        if (!read_via_shell.has_value()) {
            std::fprintf(stderr, "DEBUG 2b error: code=%s message=%s\n",
                         read_via_shell.error().code.c_str(), read_via_shell.error().message.c_str());
        } else {
            std::fprintf(stderr, "DEBUG 2b: klass=%d stdout=[%s] stderr=[%s]\n",
                         static_cast<int>(read_via_shell->klass), read_via_shell->stdout_text.c_str(),
                         read_via_shell->stderr_text.c_str());
        }
        AE_CHECK(read_via_shell.has_value() &&
                     read_via_shell->stdout_text.find("shared-mount-goodbye") != std::string::npos,
                 "2b: shell reads the SAME real file Python just wrote");
    }

    // === 3. Independent, real-disk verification -- both files genuinely exist in the ONE real host
    // directory, confirmed via plain std::filesystem, not through either mediation path -- the same
    // "verify via an independent side channel" discipline this project's own prove-phase work uses
    // throughout. ======================================================================================
    {
        bool const shell_file_on_disk = std::filesystem::exists(std::filesystem::path(scratch) / "from_shell.txt");
        bool const python_file_on_disk = std::filesystem::exists(std::filesystem::path(scratch) / "from_python.txt");
        AE_CHECK(shell_file_on_disk && python_file_on_disk,
                 "3: both files genuinely exist side by side in the ONE real host directory, "
                 "confirmed independently via plain std::filesystem, not through either runner's own "
                 "mediation");
    }

    // === 4. The shared mount does NOT widen authority -- without FsWrite granted, Python (through
    // shell) still cannot write into the shared mount, exactly as §E3-PY4's own real precedent
    // already proves for the disconnected-mount case -- sharing the DIRECTORY is not the same as
    // sharing AUTHORITY, and this test confirms the two stay independent. ============================
    {
        ExecState state{};
        EffectContext ctx{};
        CapabilitySet read_only_caps =
            CapabilitySet::grant_root({Capability{cap::RunnerCall{"python"}},
                                        Capability{cap::FsRead{"work", "", std::nullopt}}});
        ctx.capabilities = agentengine::borrow_capabilities(read_only_caps);
        auto denied = shell.run(
            ExecRequest{"shell",
                        "python \"try:\n    open('/work/should_be_denied.txt', 'w')\nexcept "
                        "PermissionError as e:\n    print('DENIED:', e)\""},
            state, ctx);
        AE_CHECK(denied.has_value() && denied->stdout_text.find("DENIED") != std::string::npos,
                 "4: sharing the mount DIRECTORY does not smuggle in FsWrite authority -- Python "
                 "still enforces its own real capability check against the same ctx");
        AE_CHECK(!std::filesystem::exists(std::filesystem::path(scratch) / "should_be_denied.txt"),
                 "4b: the denied write genuinely never landed on real disk");
    }

    std::filesystem::remove_all(scratch);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All shell<->python shared-mount checks passed -- Shell and Python genuinely share "
                "one real host directory when wired to the same mount_roots entry, with no "
                "accidental authority widening.\n");
    return 0;
}
