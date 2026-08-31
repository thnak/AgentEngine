// Proof for decisions/ADR-154-agent-output-codeact-module.md -- GitHub issue #39, 026-Agent-Facing-
// Runtime-Surface.md §5's `agent.output` ("Emit structured output conforming to the run's schema",
// zero capability). Runs a REAL script against the REAL built `agentengine_python_worker.exe`
// (native_jail_backend.cpp's create_python_worker()/exec_session()), the same harness
// test_native_jail_python_worker_slice1.cpp/test_agent_session_suspend_codeact_ask.cpp already use --
// not a description of what should happen.

#include <cstdio>
#include <filesystem>
#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"

using namespace agentengine;
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

}  // namespace

int main() {
    NativeJailBackend backend;

    // ---- R1: without expose_agent_output, agent.output does not exist -- fails loud (ImportError-
    // shaped AttributeError), matching 026 §5a's own "an ungranted module is simply absent" design,
    // never a silent no-op and never a worker crash the way an unhandled frame type would be. -------
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;

        MediatedPythonRunner runner(std::move(cfg), backend);
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "R1 setup: runner initializes a real jailed worker process");

        ExecState state{};
        EffectContext ctx{};
        ExecRequest req{"python", "import agent\nagent.output.set({'x': 1})\n"};
        auto out = runner.run(req, state, ctx);

        AE_CHECK(out.has_value(), "R1: exec_session() returns a real outcome, not a hang/crash");
        if (out.has_value()) {
            AE_CHECK(out->klass == exec_outcome_class::ok,
                     "R1: an AttributeError from a script is an ordinary 'ok' outcome with a "
                     "traceback on stderr, not a policy_violation/crash");
            AE_CHECK(out->stderr_text.find("AttributeError") != std::string::npos ||
                          out->stderr_text.find("Error") != std::string::npos,
                      "R1: the script's own traceback shows agent.output was never attached -- not "
                      "reachable without the opt-in");
            AE_CHECK(out->structured_output_json.empty(),
                     "R1: no structured output was recorded (the call never ran)");
        }
    }

    // ---- R2: with expose_agent_output, agent.output.set(value) round-trips a real JSON value into
    // ExecOutcome::structured_output_json. -----------------------------------------------------------
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.expose_agent_output = true;

        MediatedPythonRunner runner(std::move(cfg), backend);
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "R2 setup: runner initializes with agent.output exposed");

        ExecState state{};
        EffectContext ctx{};
        ExecRequest req{"python", "import agent\n"
                          "agent.output.set({'answer': 42, 'note': 'done'})\nprint('script ran')\n"};
        auto out = runner.run(req, state, ctx);

        AE_CHECK(out.has_value(), "R2: exec_session() returns a real outcome");
        if (out.has_value()) {
            AE_CHECK(out->klass == exec_outcome_class::ok, "R2: the run converges normally");
            AE_CHECK(out->stdout_text.find("script ran") != std::string::npos,
                      "R2: the script kept running after the call -- this is not a suspend/abort "
                      "the way agent.ask() is");
            AE_CHECK(out->structured_output_json.find("\"answer\"") != std::string::npos &&
                          out->structured_output_json.find("42") != std::string::npos,
                      "R2: the real JSON-encoded value round-tripped into ExecOutcome, not a stub");
        }

        // ---- R3: last-call-wins -- calling it twice keeps only the SECOND value. -------------------
        ExecRequest req2{"python",
                          "import agent\nagent.output.set('first')\nagent.output.set('second')\n"};
        auto out2 = runner.run(req2, state, ctx);
        AE_CHECK(out2.has_value(), "R3 setup: exec_session() returns a real outcome");
        if (out2.has_value()) {
            AE_CHECK(out2->structured_output_json == "\"second\"",
                      "R3: two calls in one script keep only the LAST value, not the first or both");
        }

        // ---- R4: a script that never calls agent.output.set() leaves it empty -- legitimately
        // absent, the SAME convention result_repr/ask_prompt already use, not "unpopulated." --------
        ExecRequest req3{"python", "x = 1 + 1\n"};
        auto out3 = runner.run(req3, state, ctx);
        AE_CHECK(out3.has_value(), "R4 setup: exec_session() returns a real outcome");
        if (out3.has_value()) {
            AE_CHECK(out3->structured_output_json.empty(),
                      "R4: a script that never calls agent.output.set() carries no structured output "
                      "-- and does NOT leak the PREVIOUS call's own value across executions");
        }
    }

    if (g_failures == 0) {
        std::printf("test_agent_output_codegen: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_output_codegen: %d FAILURE(S)\n", g_failures);
    return 1;
}
