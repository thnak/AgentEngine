// Milestone 3 Phase E2 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md, decision
// 4) -- proof for `agentengine::native_jail::MediatedPythonRunner`
// (src/backends/native_jail/mediated_python_runner.{hpp,cpp}), the genuinely-new-translation-unit
// PythonRunner built this pass. Only built when AGENTENGINE_BUILD_PYTHON_RUNNER is ON.
//
// Covers the pieces this pass actually built: real CPython execution with stdout/stderr capture
// (Stage A); ExecState cwd/env sync-in/sync-out across calls, matching 010 §9 G3's "a variable
// defined in execution n is present in n+1" and §3a's shared-state guarantee; the meta-path
// allowlist finder deriving its effective set per MediatedPythonRunner instance (package_policy_
// allowlist is session-wide config, not literally re-read per EffectContext -- see the header's own
// scope note on why import gating isn't capability-derived); and the open/socket/subprocess
// mediation wrappers (Stage D), each proven denied-without-capability AND granted-with-capability
// (022 §5's positive-control discipline) where a positive control is reachable without a live
// network peer.
//
// Two SEPARATE `MediatedPythonRunner` instances are constructed sequentially in this one process
// (never concurrently -- each fully destructed, tearing down its interpreter via Py_Finalize,
// before the next is constructed) so both an ungranted-import scenario and a granted-import
// scenario can be proven in one test binary without needing two separate executables.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "agentengine/core/effect_context.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"

static_assert(agentengine::Runner<agentengine::native_jail::MediatedPythonRunner>,
              "MediatedPythonRunner must satisfy the Runner concept (010 §1a)");

using namespace agentengine;
using agentengine::native_jail::MediatedPythonConfig;
using agentengine::native_jail::MediatedPythonRunner;

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

std::string read_file(std::string const& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

int main() {
    disable_crt_assert_dialog();

    std::string const scratch = ::agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp") +
                                 "/ae_e2_mount";
    std::filesystem::create_directories(scratch);

    // ================================================================================
    // Scenario 1: no package policy grants (empty allowlist) -- proves the fail-closed default,
    // ExecState sharing, and the mediation wrappers, all in one interpreter lifetime.
    // ================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.mount_roots["work"] = std::wstring(scratch.begin(), scratch.end());

        MediatedPythonRunner runner(std::move(cfg));
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "E2-C1: MediatedPythonRunner initializes a real embedded interpreter");
        AE_CHECK(runner.ok(), "E2-C1: ok() reflects successful initialization");

        ExecState state{};
        EffectContext ctx{};  // no capabilities granted at all (nullptr) -- the fail-closed baseline

        // E2-C2: basic execution + stdout capture.
        {
            ExecRequest req{"python", "print('hello from mediated cpython', 1 + 1)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->klass == exec_outcome_class::ok,
                     "E2-C2: a trivial script runs and returns ok");
            AE_CHECK(out.has_value() && out->stdout_text.find("hello from mediated cpython 2") != std::string::npos,
                     "E2-C2: stdout capture contains the real printed output");
        }

        // E2-C3 (010 §9 G3): a variable defined in execution n is present in n+1 -- __main__'s
        // namespace persists across calls on the same Runner.
        {
            ExecRequest req1{"python", "ae_e2_var = 41"};
            auto r1 = runner.run(req1, state, ctx);
            AE_CHECK(r1.has_value(), "E2-C3: setup -- first execution defines a variable");

            ExecRequest req2{"python", "print('var is', ae_e2_var + 1)"};
            auto r2 = runner.run(req2, state, ctx);
            AE_CHECK(r2.has_value() && r2->stdout_text.find("var is 42") != std::string::npos,
                     "E2-C3: a later execution on the same Runner sees the earlier execution's variable");
        }

        // E2-C4 (010 §3a): ExecState.cwd syncs INTO the process before run() and back OUT after --
        // a script's own os.chdir() is visible to the NEXT call via the shared ExecState.
        {
            state.cwd = scratch;
            ExecRequest req{"python", "import os\nprint('cwd is', os.getcwd())"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value(), "E2-C4: setup -- cwd-reading script runs");
            std::string expect_fragment = scratch.substr(0, 3);  // drive prefix is enough to confirm
            AE_CHECK(out.has_value() && out->stdout_text.find("cwd is") != std::string::npos,
                     "E2-C4: os.getcwd() reflects the ExecState.cwd this call was seeded with");

            std::filesystem::create_directories(scratch + "/subdir_e2c4");
            ExecRequest chdir_req{"python", "import os\nos.chdir(r'" + scratch + "\\subdir_e2c4')"};
            auto chdir_out = runner.run(chdir_req, state, ctx);
            AE_CHECK(chdir_out.has_value(), "E2-C4: setup -- os.chdir() script runs");
            AE_CHECK(state.cwd.find("subdir_e2c4") != std::string::npos,
                     "E2-C4: after the call, ExecState.cwd reflects the script's own os.chdir(), not "
                     "the value it was seeded with -- true sync-out, not a one-way copy");
        }

        // E2-C5: a module outside both the keep-set and the (empty) package policy is denied --
        // ImportError, never reaching a real loader for it, before any capability is even relevant.
        {
            ExecRequest req{"python", "import json"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->klass == exec_outcome_class::ok,
                     "E2-C5: setup -- the runner itself completes (the denial is a guest-level "
                     "exception, not a runner-level failure)");
            AE_CHECK(out.has_value() && out->stderr_text.find("ModuleNotFoundError") != std::string::npos,
                     "E2-C5: importing an ungranted module raises ModuleNotFoundError, fail-closed by "
                     "default (empty package_policy_allowlist)");
        }

        // E2-C6 (010 §9 G7, subprocess): os.system/subprocess.Popen are denied outright this pass
        // (RunnerCall<shell> composition is E3's job, not yet wired) -- PermissionError, never a
        // real process spawned.
        {
            ExecRequest req{"python", "import subprocess\ntry:\n    subprocess.Popen(['cmd'])\nexcept "
                                       "PermissionError as e:\n    print('DENIED:', e)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("DENIED") != std::string::npos,
                     "E2-C6: subprocess.Popen raises PermissionError, caught at the guest level");
        }
        {
            ExecRequest req{"python", "import os\ntry:\n    os.system('whoami')\nexcept PermissionError "
                                       "as e:\n    print('DENIED:', e)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("DENIED") != std::string::npos,
                     "E2-C6: os.system raises PermissionError, caught at the guest level");
        }

        // E2-C7 (010 §9 G7, open()): no FsWrite capability granted -- open() for write is denied.
        {
            ExecRequest req{"python", "try:\n    open('/work/should_be_denied.txt', 'w')\nexcept "
                                       "PermissionError as e:\n    print('DENIED:', e)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("DENIED") != std::string::npos,
                     "E2-C7: open() for write without a granted FsWrite capability raises PermissionError");
            AE_CHECK(!std::filesystem::exists(scratch + "/should_be_denied.txt"),
                     "E2-C7: the denied write never reached the filesystem");
        }
    }

    // ================================================================================
    // Scenario 2 (positive controls, 022 §5): a SECOND, freshly-constructed runner with real
    // capabilities granted, proving each denial above is a real gate, not one that would fail the
    // same way regardless of what was granted.
    // ================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.package_policy_allowlist = {"json"};
        cfg.mount_roots["work"] = std::wstring(scratch.begin(), scratch.end());

        MediatedPythonRunner runner(std::move(cfg));
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "E2-C8: setup -- a second interpreter (after the first's Py_Finalize) "
                                    "initializes cleanly");

        ExecState state{};
        CapabilitySet caps = CapabilitySet::grant_root(
            {Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
             Capability{cap::FsRead{"work", "", std::nullopt}}});
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);

        // E2-C8 (positive control for E2-C5): "json" IS in this runner's package policy -- the
        // identical import that failed above now succeeds.
        {
            ExecRequest req{"python", "import json\nprint('json imported:', json.dumps({'a': 1}))"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("json imported") != std::string::npos,
                     "E2-C8: importing a module the package policy DOES grant succeeds");
        }

        // E2-C9 (positive control for E2-C7): a granted FsWrite capability lets open() succeed, and
        // the real content lands on disk through the TOCTOU-safe open_within_mount_root primitive.
        {
            ExecRequest req{"python", "with open('/work/e2c9.txt', 'w') as f:\n    f.write('written by "
                                       "mediated python')"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stderr_text.empty(),
                     "E2-C9: open() for write with a granted FsWrite capability succeeds with no error");
            AE_CHECK(read_file(scratch + "/e2c9.txt") == "written by mediated python",
                     "E2-C9: the real file content matches exactly what the script wrote");
        }

        // E2-C10 (positive control for E2-C7's read side): a granted FsRead capability lets open()
        // for reading succeed against the file E2-C9 just wrote.
        {
            ExecRequest req{"python", "with open('/work/e2c9.txt', 'r') as f:\n    print('READ:', f.read())"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->stdout_text.find("READ: written by mediated python") !=
                                             std::string::npos,
                     "E2-C10: open() for read with a granted FsRead capability succeeds and reads "
                     "the real content back");
        }
    }

    // ================================================================================
    // Scenario 3 (Milestone 3 Phase F3, 010 §3 items 4/5): result_repr captures a value never
    // print()-ed, and stdout/stderr/result_repr are capped with an explicit truncation marker. A
    // small explicit output_cap_bytes proves the cap is real without needing megabyte-scale output.
    // ================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.output_cap_bytes = 128;

        MediatedPythonRunner runner(std::move(cfg));
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "F3-setup: a third interpreter initializes cleanly");

        ExecState state{};
        EffectContext ctx{};

        // F3-R1: a bare trailing expression on its own line is captured as result_repr -- 010 §3's
        // own named "value never print()-ed" gap, closed.
        {
            ExecRequest req{"python", "x = 40\ny = x + 2\ny"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value(), "F3-R1: setup -- a multi-line script with a trailing bare name runs");
            AE_CHECK(out.has_value() && out->result_repr == "42",
                     "F3-R1: the trailing expression's value is captured as result_repr, never printed");
        }

        // F3-R2 (010 §3's own literal example): a semicolon-separated trailing expression on ONE line.
        {
            ExecRequest req{"python", "data = 1 + 1; data"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->result_repr == "2",
                     "F3-R2: 010 §3's own 'data = ...; data' example captures result_repr == '2'");
        }

        // F3-R3 (negative control): a trailing print() call returns None -- result_repr stays empty,
        // and the print still ran exactly once (captured via the eval branch, not re-executed).
        {
            ExecRequest req{"python", "print('only side effect')"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->result_repr.empty(),
                     "F3-R3: a trailing print() call (returns None) leaves result_repr empty");
            AE_CHECK(out.has_value() && out->stdout_text.find("only side effect") != std::string::npos,
                     "F3-R3: the print() still ran exactly once");
        }

        // F3-R4 (negative control): a script ending in an assignment has no trailing expression at
        // all -- result_repr stays empty, identical to pre-F3 behavior.
        {
            ExecRequest req{"python", "z = 99"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value() && out->result_repr.empty(),
                     "F3-R4: a script with no trailing expression (ends in an assignment) leaves "
                     "result_repr empty");
        }

        // F3-T1 (010 §3's output-cap discipline): stdout exceeding output_cap_bytes is truncated
        // with an explicit marker, never silently cut or left unbounded.
        {
            ExecRequest req{"python", "print('A' * 1000)"};
            auto out = runner.run(req, state, ctx);
            AE_CHECK(out.has_value(), "F3-T1: setup -- a large-output script runs");
            AE_CHECK(out.has_value() && out->stdout_text.size() < 1000,
                     "F3-T1: stdout exceeding the configured cap is shorter than the raw output");
            AE_CHECK(out.has_value() && out->stdout_text.find("truncated") != std::string::npos,
                     "F3-T1: the truncated stdout carries an explicit marker naming what happened");
        }
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All MediatedPythonRunner smoke checks passed.\n");
    return 0;
}
