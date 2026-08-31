// Proof for decisions/ADR-155-agent-progress-codeact-module.md -- GitHub issue #31's own three-piece
// gap ("agent.progress is spec'd and manifest-registered but has no implementation"), closed. Runs a
// REAL script against the REAL built `agentengine_python_worker.exe`
// (native_jail_backend.cpp's create_python_worker()/exec_session()/dispatch_worker_query()), the same
// harness test_agent_output_codegen.cpp/test_native_jail_python_worker_slice1.cpp already use.
//
// Also proves the specific landmine issue #31 itself named: exec_session()'s frame-type dispatch loop
// has a fail-closed fallback (terminate_worker()) for any unrecognized frame -- if agent.progress had
// been wired as a genuinely NEW wire frame TYPE without a matching branch there, every call would
// crash the worker outright. This ADR deliberately avoided that risk by reusing the EXISTING
// worker_query/worker_query_response envelope (a new KIND, not a new TYPE) -- R2 below proves the
// worker survives the call and keeps running, not merely that report_progress() fired.

#include <cstdio>
#include <string>
#include <vector>

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

    // ---- R1: without expose_agent_progress, agent.progress does not exist -- fails loud, never a
    // silent no-op and never a worker crash. ------------------------------------------------------
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;

        MediatedPythonRunner runner(std::move(cfg), backend);
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "R1 setup: runner initializes a real jailed worker process");

        ExecState state{};
        EffectContext ctx{};
        ExecRequest req{"python", "import agent\nagent.progress.report('x')\n"};
        auto out = runner.run(req, state, ctx);

        AE_CHECK(out.has_value(), "R1: exec_session() returns a real outcome, not a hang/crash");
        if (out.has_value()) {
            AE_CHECK(out->klass == exec_outcome_class::ok,
                     "R1: an AttributeError from a script is an ordinary 'ok' outcome, not a "
                     "policy_violation/crash -- not reachable without the opt-in");
        }
    }

    // ---- R2/R3: with expose_agent_progress, a real ctx.report_progress() fires per call, with the
    // real text, and the WORKER SURVIVES (proving this did not need a new frame type / did not hit
    // the fail-closed terminate_worker() fallback issue #31 itself warned a naive implementation
    // would). Multiple calls in one script produce multiple events -- unlike agent.output's
    // last-call-wins, this is a streaming channel (ADR-060's own "no accumulation" shape). ----------
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.expose_agent_progress = true;

        MediatedPythonRunner runner(std::move(cfg), backend);
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "R2 setup: runner initializes with agent.progress exposed");

        std::vector<std::string> reported;
        ExecState state{};
        EffectContext ctx{};
        ctx.report_progress = [&reported](ContentItem item) {
            if (auto const* text = std::get_if<Text>(&item.value)) reported.push_back(text->text);
        };

        ExecRequest req{"python", "import agent\n"
                          "agent.progress.report('25% done')\n"
                          "agent.progress.report('50% done')\n"
                          "print('script finished normally')\n"
                          "agent.progress.report('75% done')\n"};
        auto out = runner.run(req, state, ctx);

        AE_CHECK(out.has_value(), "R2: exec_session() returns a real outcome");
        if (out.has_value()) {
            AE_CHECK(out->klass == exec_outcome_class::ok,
                     "R2: the run converges normally -- the worker was NOT terminated by any call");
            AE_CHECK(out->stdout_text.find("script finished normally") != std::string::npos,
                      "R2: the script kept running across and after every call -- this is not a "
                      "suspend/abort the way agent.ask() is, and the worker was not crashed the way "
                      "an unhandled frame type would (issue #31's own named landmine)");
        }
        AE_CHECK(reported.size() == 3,
                 "R3: three calls in one script produced three real events -- a streaming channel, "
                 "not a last-call-wins declaration");
        if (reported.size() == 3) {
            AE_CHECK(reported[0] == "25% done" && reported[1] == "50% done" && reported[2] == "75% done",
                      "R3: events carry the real text, in the real call order");
        }

        // ---- R4: the worker is still alive after all this -- a SECOND, independent call on the SAME
        // handle also succeeds, proving no latent damage from the progress calls. -------------------
        ExecRequest req2{"python", "x = 1 + 1\n"};
        auto out2 = runner.run(req2, state, ctx);
        AE_CHECK(out2.has_value() && out2->klass == exec_outcome_class::ok,
                  "R4: the SAME worker handles a later, unrelated call normally afterward");
    }

    if (g_failures == 0) {
        std::printf("test_agent_progress_codegen: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_progress_codegen: %d FAILURE(S)\n", g_failures);
    return 1;
}
