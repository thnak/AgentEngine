// Jailed-Python-worker design (final spec, 2026-08-23; supersedes native_jail_backend.hpp's former
// "Correction (2026-08-23)" comment) -- the minimum-slice PROVE-phase positive controls the design's
// own Part 3 test plan names as G-Q1/G-Q2/G-lifecycle-2, run against the REAL built
// `agentengine_python_worker.exe` (native_jail_backend.cpp's create_python_worker()/exec_session()),
// not a description of what should happen.
//
// Scope: this file proves component-role-audit-tracker.md's Finding Q ("zero OS-level resource
// containment on the interpreter process") is actually closed by the new jailed-worker model -- a
// payload that would have run forever / exhausted host memory unbounded under the OLD in-process
// embed is now caught and killed by the worker's own session-scoped watchdog (native_jail_backend.cpp
// session_watchdog_loop) within its configured budget. It does NOT re-prove Finding R's AppContainer
// filesystem boundary (test_native_jail_backend_windows.cpp / test_mediated_python_runner_hostile_corpus.cpp
// already do that for the native-jail profile generally) and does NOT re-prove the call_tool relay
// round-trip (test_mediated_python_runner_agent_tools.cpp already proves that end-to-end through the
// real bridge_tool_call()/tool_pipeline.hpp path -- G-tooltest is that file, not duplicated here).

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

    std::string const scratch = ::agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp") +
                                 "/ae_slice1_mount";
    std::filesystem::create_directories(scratch);

    // ================================================================================================
    // G-Q1: a `while True: pass` guest script under a short exec_wall_ms budget. Under the OLD
    // in-process embed (Finding Q) nothing external could ever stop this -- the one deadline check
    // that existed was pre-flight only (core/tool_pipeline.hpp:579-583). Under the new model, the
    // session-scoped watchdog's `call_active` phase kills the WORKER PROCESS via TerminateJobObject
    // once `phase_deadline` passes -- proven here by timing the real wall-clock elapsed and checking
    // the outcome classification, not by inspecting source.
    // ================================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.exec_wall_ms = 800;              // short, deliberate budget for this one call
        cfg.memory_bytes = 256ull * 1024 * 1024;
        cfg.pids = 4;

        MediatedPythonRunner runner(std::move(cfg), backend);
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "G-Q1 setup: runner initializes a real jailed worker process");

        ExecState state{};
        EffectContext ctx{};

        auto const t0 = std::chrono::steady_clock::now();
        ExecRequest req{"python", "while True:\n    pass\n"};
        auto out = runner.run(req, state, ctx);
        auto const elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);

        AE_CHECK(out.has_value(), "G-Q1: exec_session() returns a real outcome, not a hang/crash");
        if (out.has_value()) {
            AE_CHECK(out->klass == exec_outcome_class::timeout,
                     "G-Q1: an infinite busy-loop is classified as timeout, not ok/crash");
        }
        // 800ms budget + watchdog poll granularity (5ms in call_active phase) + process-teardown slop.
        // 10000ms is a generous upper bound -- if this fires, the payload was NOT actually preempted
        // (the exact failure mode Finding Q named), not a flake in this test's own timing.
        AE_CHECK(elapsed.count() < 10000,
                 "G-Q1 (Finding Q, closed): the busy-loop worker was actually killed within a small "
                 "bounded multiple of its 800ms budget -- real external preemption, not a pre-flight-"
                 "only check that never fires mid-call");
        std::printf("  measured: G-Q1 busy-loop kill took %lldms (budget was 800ms)\n",
                    static_cast<long long>(elapsed.count()));

        // G-lifecycle-2 (RT2 Finding 5): after a watchdog kill, the worker is dead -- the design's own
        // documented contract is fail-closed, no silent respawn. Prove the NEXT call on the same
        // handle returns session_terminated immediately (bounded), not a second hang.
        auto const t1 = std::chrono::steady_clock::now();
        ExecRequest req2{"python", "1 + 1"};
        auto out2 = runner.run(req2, state, ctx);
        auto const elapsed2 =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1);
        AE_CHECK(!out2.has_value(),
                 "G-lifecycle-2: a call on a killed worker's handle fails, it does not silently respawn");
        if (!out2.has_value()) {
            AE_CHECK(out2.error().code == "native_jail.session_terminated",
                     "G-lifecycle-2: the failure is specifically session_terminated, not a generic error");
        }
        AE_CHECK(elapsed2.count() < 2000,
                 "G-lifecycle-2: the post-kill call fails fast (no hang waiting on a dead worker)");
    }

    // ================================================================================================
    // G-Q2: a pure-Python memory bomb under a tight memory_bytes cap. Under the OLD in-process embed,
    // this ran in the HOST'S OWN address space with no cap at all -- a large allocation could exhaust
    // real host memory. Under the new model, JobObjectLimits' memory notification (drained
    // continuously by the session watchdog via poll_memory_limit_once(), §6) kills the worker before
    // the allocation is allowed to succeed unbounded.
    // ================================================================================================
    {
        MediatedPythonConfig cfg;
        cfg.python_home = AE_PYTHON_HOME;
        cfg.exec_wall_ms = 8000;             // generous wall budget -- this call should be caught by
                                               // the MEMORY axis, not time out first (isolates the axis
                                               // under test, same discipline as
                                               // test_native_jail_backend_windows.cpp's own OOM case).
        cfg.memory_bytes = 64ull * 1024 * 1024;   // 64 MiB cap
        cfg.pids = 4;

        MediatedPythonRunner runner(std::move(cfg), backend);
        auto init = runner.initialize();
        AE_CHECK(init.has_value(), "G-Q2 setup: runner initializes a real jailed worker process");

        ExecState state{};
        EffectContext ctx{};

        // Try to allocate ~512 MiB, well past the 64 MiB cap -- either the worker is killed by the Job
        // Object before the allocation completes (outcome: oom/timeout/crash, never ok with success
        // printed), or Python's own allocator raises MemoryError inside the still-alive worker (also
        // an acceptable, real containment outcome -- the cap acted before the guest could use the
        // memory either way). What must NEVER happen: 'ALLOCATED_OK' printed, meaning the guest
        // actually holds 512 MiB with no external limit stopping it -- Finding Q's exact defect.
        ExecRequest req{"python",
                          "buf = bytearray(512 * 1024 * 1024)\n"
                          "print('ALLOCATED_OK', len(buf))\n"};
        auto out = runner.run(req, state, ctx);

        bool const contained =
            !out.has_value() ||
            out->klass != exec_outcome_class::ok ||
            out->stdout_text.find("ALLOCATED_OK") == std::string::npos;
        AE_CHECK(contained,
                 "G-Q2 (Finding Q, closed): a 512MiB allocation under a 64MiB Job Object cap never "
                 "completes as an unconstrained success -- either the worker is killed, or the "
                 "allocation itself fails inside a still-alive, still-mediated interpreter");
        if (out.has_value()) {
            std::printf("  measured: G-Q2 outcome klass=%d stdout='%s' stderr='%s'\n",
                        static_cast<int>(out->klass), out->stdout_text.c_str(),
                        out->stderr_text.substr(0, 200).c_str());
        } else {
            std::printf("  measured: G-Q2 exec_session() returned error code=%s\n",
                        out.error().code.c_str());
        }
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_native_jail_python_worker_slice1: ALL PASS\n");
    return 0;
}
