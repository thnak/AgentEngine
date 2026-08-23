// Milestone 3 Phase E3 -- proves `RunnerCall<python>` composition (010 §1a, §9 G4) for real:
// `MediatedShellRunner` invoking the REAL `MediatedPythonRunner` (E2), not a fake registered Runner
// (that gate mechanism is already covered by test_mediated_shell_runner_smoke.cpp's E3-RC1/RC2).
// "ShellRunner cannot invoke PythonRunner without the capability, and cannot exceed the capability
// set it was itself granted when it does" -- both halves proven here, including the negative
// control the breakdown doc's own E3 task text explicitly asks for.
//
// Only built when AGENTENGINE_BUILD_PYTHON_RUNNER is ON (links both agentengine::mediated_shell_
// runner and agentengine::mediated_python_runner).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

} // namespace

int main() {
    disable_crt_assert_dialog();

    std::string const scratch = ::agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp") +
                                 "/ae_e3_py_compose";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    std::wstring scratch_w(scratch.begin(), scratch.end());

    NativeJailBackend backend;
    MediatedPythonConfig py_cfg;
    py_cfg.python_home = AE_PYTHON_HOME;
    MediatedPythonRunner python(std::move(py_cfg), backend);
    auto init = python.initialize();
    AE_CHECK(init.has_value(), "E3-PY1: setup -- the real MediatedPythonRunner initializes");

    auto adapter = MediatedFileSystemAdapter::create(scratch_w);
    AE_CHECK(adapter.has_value(), "E3-PY1: setup -- the filesystem adapter is created");

    DefaultCommandRegistry registry;
    AE_CHECK(registry
                 .register_runner(RegisteredRunner{
                     "python",
                     [&](ExecRequest const& req, ExecState& state, EffectContext& ctx) -> result<ExecOutcome> {
                         return python.run(req, state, ctx);
                     }})
                 .has_value(),
             "E3-PY1: setup -- the real PythonRunner registers under the name 'python'");

    MediatedShellRunner shell(*adapter, registry, "work");

    // E3-PY2 (the gate, positive half): with RunnerCall{"python"} granted, `ShellRunner` really
    // invokes the real embedded CPython interpreter, and the shared ExecState makes the round trip
    // observable -- a Python-side computation's stdout comes back through the shell's own ExecOutcome.
    {
        ExecState state{};
        CapabilitySet caps = CapabilitySet::grant_root({Capability{cap::RunnerCall{"python"}}});
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);

        auto out = shell.run(ExecRequest{"shell", "python print(6 * 7)"}, state, ctx);
        AE_CHECK(out.has_value() && out->stdout_text.find("42") != std::string::npos,
                 "E3-PY2: ShellRunner invokes the REAL MediatedPythonRunner and returns its real stdout");
    }

    // E3-PY3 (negative control, explicitly required by the breakdown doc's E3 task text):
    // WITHOUT RunnerCall{"python"} granted, the identical command is denied, and the real
    // interpreter is never reached at all -- proven by a side effect (a variable) that would only
    // exist if CPython actually ran.
    {
        // Both probes below are deliberately QUOTE-FREE, SPACE-FREE Python expressions: this
        // grammar's shell-level word-splitting/quote-stripping (ADR-001's own accepted v1 scope,
        // §3 -- no heredocs) would otherwise mangle a naive multi-word or quoted Python argument
        // before it ever reaches PythonRunner -- a real, named limitation of composing through this
        // grammar for anything beyond a simple expression, not a ShellRunner correctness bug.
        ExecState state{};
        EffectContext ctx{};  // no capabilities granted at all
        auto out = shell.run(ExecRequest{"shell", "python ae_e3_side_effect=1"}, state, ctx);
        AE_CHECK(!out.has_value() && out.error().code == "shell.capability_denied",
                 "E3-PY3 (negative control): ShellRunner cannot invoke PythonRunner without RunnerCall");

        // Prove the interpreter really never ran: grant the capability NOW and reference the
        // variable from the denied attempt directly -- if the denied call had actually reached
        // CPython, this would be defined from that earlier, supposedly-blocked execution; a real
        // NameError instead proves it was never set.
        CapabilitySet caps = CapabilitySet::grant_root({Capability{cap::RunnerCall{"python"}}});
        ctx.capabilities = agentengine::borrow_capabilities(caps);
        auto probe = shell.run(ExecRequest{"shell", "python print(ae_e3_side_effect)"}, state, ctx);
        AE_CHECK(probe.has_value() && probe->stderr_text.find("NameError") != std::string::npos,
                 "E3-PY3: the denied call's side effect never happened -- the interpreter was truly "
                 "never reached, not merely reported as denied");
    }

    // E3-PY4 ("cannot exceed the capability set it was itself granted"): ShellRunner passes the
    // IDENTICAL EffectContext& into PythonRunner.run(), never an attenuated-then-silently-widened
    // copy -- proven by a capability PythonRunner's own mediation needs (FsWrite) being denied
    // inside the composed call exactly as it would be denied to ShellRunner itself, even though
    // RunnerCall{"python"} alone was granted.
    {
        ExecState state{};
        CapabilitySet caps = CapabilitySet::grant_root({Capability{cap::RunnerCall{"python"}}});
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);
        // The whole multi-line, quote-containing Python body is wrapped in ONE shell-level
        // double-quoted word so this grammar's word-splitting never sees the spaces/quotes inside
        // it -- the double quotes are stripped by expand_word(), leaving the real newlines and
        // single-quoted Python string literals intact in the source PythonRunner actually executes.
        std::string source =
            "python \"try:\n    open('/work/should_be_denied.txt', 'w')\nexcept PermissionError as e:\n"
            "    print('DENIED:', e)\"";
        auto out = shell.run(ExecRequest{"shell", source}, state, ctx);
        AE_CHECK(out.has_value() && out->stdout_text.find("DENIED") != std::string::npos,
                 "E3-PY4: PythonRunner, invoked THROUGH ShellRunner, still enforces its own "
                 "capability checks against the SAME ctx -- RunnerCall{\"python\"} alone does not "
                 "smuggle in an FsWrite grant it was never given");
    }

    std::filesystem::remove_all(scratch);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All RunnerCall<python> composition checks passed.\n");
    return 0;
}
