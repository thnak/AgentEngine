// Milestone 2 Phase C, task C2 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// NativeJailBackend (src/backends/native_jail/native_jail_backend.{hpp,cpp}), the real Windows
// `native-jail` SandboxBackend -- create/exec/destroy end to end, ResourceLimits enforced,
// structured ExecOutcome values, never a raw host fault crossing the seam. Reuses
// tests/helpers/hostile_child.cpp (already built for test_job_object_limits.cpp) as the exec()
// target -- this is C2's own proof that the backend works, not C3's systematic 008 §7 abuse-case
// corpus with G2 positive controls, which is its own, later task.
//
// Real child processes are spawned under real AppContainer + Job Object isolation -- bounded by
// this test's own generous-but-finite wall_ms values (CLAUDE.md Machine Safety), same discipline
// as test_job_object_limits.cpp.

#include <windows.h>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "backends/native_jail/native_jail_backend.hpp"
#include "support/crt_fail_fast.hpp"

using namespace agentengine;
using agentengine::native_jail::NativeJailBackend;

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


std::string hostile_child_cmd(std::string const& args) {
    return std::string("\"") + AE_HOSTILE_CHILD_EXE + "\" " + args;
}

}  // namespace

int main() {
    ::agentengine::test_support::fail_fast_on_windows();

    std::filesystem::path work_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_backend_test";
    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);
    AE_CHECK(!ec, "setup: scratch mount directory exists");

    SandboxSpec spec;
    spec.mounts.push_back(MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
    // 5000, not 2000: measured this session (test_native_jail_backend_windows flaking right after
    // test_job_object_limits' own memory-heavy tests) -- under host memory pressure,
    // JOB_OBJECT_LIMIT_JOB_MEMORY enforcement for a 32 MB cap can take longer than 2000ms to catch a
    // 512 MB allocation attempt, so the OOM sub-test below was sometimes reaching this handle's own
    // wall_ms timeout first and (correctly, per the completion-port signal added this session)
    // reporting `timeout`, not `oom` -- a real race in this test's own budget, not a classification
    // bug. 5000ms matches test_native_jail_abuse_corpus_windows.cpp's identical 32 MB/512 MB
    // scenario, which has never been observed to flake.
    spec.limits.wall_ms = 5000;
    spec.limits.memory_bytes = 32ull * 1024 * 1024;  // 32 MB
    spec.limits.pids = 4;
    spec.limits.output_bytes = 1024 * 1024;

    NativeJailBackend backend;
    EffectContext ctx;

    auto handle = backend.create(spec, ctx);
    AE_CHECK(handle.has_value(), "C2: create() succeeds given a real mount and ResourceLimits");
    if (!handle.has_value()) {
        std::cerr << "create() failed, aborting remaining checks\n";
        return 1;
    }

    // A well-behaved exec reports `ok`, with stdout actually captured.
    {
        ExecRequest req{.language = "native", .source = hostile_child_cmd("sleep 50")};
        auto outcome = backend.exec(*handle, req, ctx);
        AE_CHECK(outcome.has_value(), "C2: exec() of a well-behaved child returns a result");
        if (outcome.has_value()) {
            AE_CHECK(outcome->klass == exec_outcome_class::ok, "C2: a well-behaved exec reports ok");
            AE_CHECK(outcome->stdout_text.find("SLEEP_DONE") != std::string::npos,
                      "C2: exec() captures the child's real stdout");
        }
    }

    // A CPU-spinning child never exits on its own -- the wall_ms watch must kill it and report
    // `timeout` (ADR-004 §10.5: this is the trusted enforcement point, not cpu_ms).
    {
        ExecRequest req{.language = "native", .source = hostile_child_cmd("spin")};
        auto outcome = backend.exec(*handle, req, ctx);
        AE_CHECK(outcome.has_value() && outcome->klass == exec_outcome_class::timeout,
                  "C2: a CPU-spinning exec is killed by wall_ms and reports timeout");
    }

    // A clean, ordinary nonzero exit (no resource limit involved) reports `crash`, not `ok` and
    // not `oom` -- the positive control for the crash/oom split, since it never approaches
    // memory_bytes.
    {
        ExecRequest req{.language = "native", .source = hostile_child_cmd("fail 7")};
        auto outcome = backend.exec(*handle, req, ctx);
        AE_CHECK(outcome.has_value(), "C2: exec() of a failing child returns a result");
        if (outcome.has_value()) {
            AE_CHECK(outcome->klass == exec_outcome_class::crash,
                      "C2: an ordinary nonzero exit reports crash, not oom (positive control)");
            AE_CHECK(outcome->stdout_text.find("FAIL_MODE") != std::string::npos,
                      "C2: stdout is captured even on a nonzero exit");
        }
    }

    // Exceeding memory_bytes reports `oom`.
    {
        ExecRequest req{.language = "native", .source = hostile_child_cmd("alloc 512")};  // 512 MB >> 32 MB cap
        auto outcome = backend.exec(*handle, req, ctx);
        AE_CHECK(outcome.has_value() && outcome->klass == exec_outcome_class::oom,
                  "C2: exceeding memory_bytes reports oom");
    }

    backend.destroy(*handle);

    // exec() on a destroyed handle fails closed rather than silently launching unbounded.
    {
        ExecRequest req{.language = "native", .source = hostile_child_cmd("sleep 10")};
        auto outcome = backend.exec(*handle, req, ctx);
        AE_CHECK(!outcome.has_value(), "C2: exec() on a destroyed SandboxHandle fails closed");
    }

    std::filesystem::remove_all(work_dir, ec);

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
