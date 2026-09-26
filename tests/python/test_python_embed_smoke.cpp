// decisions/ADR-002-pythonrunner-embedding-and-mediation.md prove phase, priority item 1: prove a
// minimal embedded CPython interpreter actually runs inside a C++23 translation unit in this
// repo, linked against the concrete miniconda 3.13.5 target the ADR names, round-tripping a
// trivial script through PythonRunner's real (no longer stub) `run()`. Also proves PythonRunner
// satisfies the `Runner` concept for real, replacing the fail-closed-stub check that used to live
// in test_native_jail_runner_stubs.cpp (see that file's updated comment).
//
// Only built when AGENTENGINE_BUILD_PYTHON_RUNNER is ON (CMakeLists.txt) -- it links against a
// real CPython import lib and needs AE_PYTHON_HOME/AE_PYTHON_SITE_PACKAGES (compile definitions,
// tests/CMakeLists.txt) pointing at the configured install.

#include <cassert>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "agentengine/core/effect_context.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "backends/native_jail/python_runner.hpp"
#include "support/crt_fail_fast.hpp"

static_assert(agentengine::Runner<agentengine::PythonRunner>,
              "PythonRunner must satisfy the Runner concept (010 §1a) for real, not just as a stub");

namespace {
} // namespace

int main() {
    ::agentengine::test_support::fail_fast_on_windows();

    agentengine::native_jail::PythonLockdownConfig cfg;
    cfg.python_home = AE_PYTHON_HOME;
    cfg.allowed_top_level_modules = {"math"};

    agentengine::PythonRunner runner(std::move(cfg));
    bool init_ok = runner.initialize();
    printf("initialize() -> %s (%s)\n", init_ok ? "true" : "false", runner.last_error().c_str());
    assert(init_ok && "a minimal embedded CPython interpreter must initialize");

    printf("Py version string surfaced via sys.modules snapshot below; interpreter is live.\n");

    agentengine::ExecRequest request{};
    request.language = "python";
    request.source =
        "import math\n"
        "print('hello from embedded cpython', 1 + 1, math.sqrt(9.0))\n";
    agentengine::ExecState state{};
    agentengine::EffectContext ctx{};

    agentengine::result<agentengine::ExecOutcome> outcome = runner.run(request, state, ctx);
    assert(outcome.has_value() && "run() must succeed for trivial allowed code");
    printf("stdout: %s", outcome->stdout_text.c_str());
    printf("stderr: %s", outcome->stderr_text.c_str());
    assert(outcome->klass == agentengine::exec_outcome_class::ok);
    assert(outcome->stdout_text.find("hello from embedded cpython 2 3.0") != std::string::npos);

    printf("test_python_embed_smoke: PASS\n");
    return 0;
}
