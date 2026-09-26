// 008-Sandbox-and-Isolation.md §9 G7 -- "a guest instance is never reused across sessions or
// principals; a canary written in session A is unreachable from session B on every profile,
// including under pooling and snapshot-restore. Positive control: a deliberately shared pool is
// caught." Traced as a real, previously-untracked gap in decisions/ADR-082-native-jail-promotion-
// gate-008-9.md §4 (unlike G5/G6, G7 was absent from M2 Phase C's own "what's deferred" list
// entirely -- no test existed anywhere for this claim before this file).
//
// SCOPE ADAPTATION, named rather than silent: this codebase's `NativeJailBackend` has no instance
// pool at all (`create_python_worker()` mints a fresh `Instance`/OS process every call,
// native_jail_backend.cpp) -- there is no "deliberately shared pool" to configure and catch the way
// the gate's own text imagines. What this file proves instead is the load-bearing claim ADR-082 §4
// left as an argument, not a fact: that two independently-created worker instances are genuinely
// separate OS processes with genuinely separate CPython interpreter state, not merely "no pooling
// code happens to run today." Since decisions/ADR-081-jailed-python-worker-process-slice-1.md moved
// the interpreter into its own OS process per worker, this is now a real KERNEL-ENFORCED boundary
// (two processes cannot share heap memory), not a language-level argument about missing code paths
// -- this file is the first test to actually exercise that.
//
// Non-vacuousness, in place of a literal "shared pool" positive control: session A's canary is read
// back from session A itself (2) both BEFORE and (4) AFTER session B's own negative probe (3) --
// proving A's own state survived independently of B's creation and probe (ruling out "A's worker
// happened to die/reset around the same time" as an alternative explanation for B not seeing it),
// the same sandwich-the-negative-check-between-two-positive-checks shape
// test_native_jail_python_worker_slice1.cpp's own G-lifecycle-2 case uses.

#include <cstdio>
#include <string>

#include "agentengine/core/effect_context.hpp"
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
    // Two independent per-session worker processes, both alive at once, against the SAME backend
    // object -- the backend's own instances_ map is exercised with two live entries simultaneously,
    // not just sequentially (a sequential-only test could not distinguish "isolated" from "reset
    // between calls").
    NativeJailBackend backend;

    MediatedPythonConfig cfg_a;
    cfg_a.python_home = AE_PYTHON_HOME;
    MediatedPythonRunner runner_a(cfg_a, backend);
    AE_CHECK(runner_a.initialize().has_value(), "setup: session A's jailed worker initializes");

    MediatedPythonConfig cfg_b;
    cfg_b.python_home = AE_PYTHON_HOME;
    MediatedPythonRunner runner_b(cfg_b, backend);
    AE_CHECK(runner_b.initialize().has_value(), "setup: session B's jailed worker initializes");

    ExecState state_a{};
    ExecState state_b{};
    EffectContext ctx{};

    static constexpr char kCanaryValue[] = "leak_should_not_cross_sessions_98765";

    // ---- 1: plant the canary in session A. ------------------------------------------------------
    {
        std::string src = std::string("session_a_canary = '") + kCanaryValue + "'";
        auto out = runner_a.run(ExecRequest{"python", src}, state_a, ctx);
        AE_CHECK(out.has_value() && out->klass == exec_outcome_class::ok,
                  "1: session A plants its own canary");
    }

    // ---- 2: positive control -- A reads its own canary back, BEFORE B ever runs. -----------------
    {
        auto out = runner_a.run(ExecRequest{"python", "print(session_a_canary)"}, state_a, ctx);
        AE_CHECK(out.has_value() && out->klass == exec_outcome_class::ok &&
                      out->stdout_text.find(kCanaryValue) != std::string::npos,
                  "2 (non-vacuousness): session A sees its own canary -- the persistence mechanism "
                  "this test depends on is real, not merely assumed");
    }

    // ---- 3: G7 itself -- session B must NOT see session A's canary. ------------------------------
    {
        auto out = runner_b.run(ExecRequest{"python", "print(session_a_canary)"}, state_b, ctx);
        // A NameError inside the worker is a normal, well-formed exec_response (klass=ok, a Python
        // traceback on stderr), not an exec_session()-level failure -- the same shape G-Q2's own
        // memory-bomb case (test_native_jail_python_worker_slice1.cpp) already established for an
        // in-worker Python exception.
        bool const canary_leaked =
            out.has_value() && out->stdout_text.find(kCanaryValue) != std::string::npos;
        AE_CHECK(!canary_leaked,
                  "3 (G7): session B's worker never sees session A's canary -- no cross-session "
                  "state leak");
        if (out.has_value()) {
            AE_CHECK(out->stderr_text.find("NameError") != std::string::npos,
                      "3: the absence is a real NameError (undefined name), not some other failure "
                      "shape that happens to also not print the canary");
        }
    }

    // ---- 4: positive control -- A STILL sees its own canary, AFTER B's probe. --------------------
    {
        auto out = runner_a.run(ExecRequest{"python", "print(session_a_canary)"}, state_a, ctx);
        AE_CHECK(out.has_value() && out->klass == exec_outcome_class::ok &&
                      out->stdout_text.find(kCanaryValue) != std::string::npos,
                  "4 (non-vacuousness): session A STILL sees its own canary after B's own probe -- "
                  "rules out 'A's state was reset/lost around the same time' as an alternative "
                  "explanation for step 3's negative result");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_native_jail_session_boundary_windows: ALL PASS\n");
    return 0;
}
