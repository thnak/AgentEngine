// Milestone 2 Phase C, task C2 (Linux half, its own follow-up per 021 §2 sequencing):
// LinuxNativeJailBackend (src/backends/native_jail/linux_native_jail_backend.{hpp,cpp}) --
// create/exec/destroy end to end, ResourceLimits enforced via cgroups v2, structured ExecOutcome
// values, never a raw host fault crossing the seam. Reuses tests/helpers/hostile_child_posix.cpp
// as the exec() target -- this is C2's own proof the backend works, not C3's systematic 008 §7
// abuse-case corpus with G2 positive controls, which is its own, later task.
//
// Requires a delegated cgroup v2 root with memory/pids already enabled in its own ancestor's
// cgroup.subtree_control (see linux_native_jail_backend.hpp's own precondition note) and
// CAP_SYS_ADMIN for the PID/net/mount/UTS/IPC namespaces this backend creates via clone() -- run
// via tests/helpers/cgroup_v2_test_setup.sh inside a --privileged container, not directly on an
// unprivileged host.
//
// Real child processes are spawned under real namespace + cgroup + seccomp isolation -- bounded by
// this test's own generous-but-finite wall_ms values (CLAUDE.md Machine Safety).

#include <filesystem>
#include <iostream>
#include <string>

#include "backends/native_jail/linux_native_jail_backend.hpp"
#include "helpers/native_jail_linux_toolchain_mounts.hpp"
#include "support/error_detail.hpp"

using namespace agentengine;
using agentengine::native_jail::LinuxNativeJailBackend;

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
    return std::string("\"") + AE_HOSTILE_CHILD_POSIX_EXE + "\" " + args;
}

}  // namespace

int main() {
    std::filesystem::path work_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_backend_test_linux";
    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);
    AE_CHECK(!ec, "setup: scratch mount directory exists");

    SandboxSpec spec;
    spec.mounts.push_back(MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
    agentengine::native_jail::test::add_shell_toolchain_mounts(spec);
    spec.limits.wall_ms = 2000;
    spec.limits.memory_bytes = 32ull * 1024 * 1024;  // 32 MB
    spec.limits.pids = 4;
    spec.limits.output_bytes = 1024 * 1024;

    LinuxNativeJailBackend backend;
    EffectContext ctx;

    auto handle = backend.create(spec, ctx);
    AE_CHECK(handle.has_value(), "C2-Linux: create() succeeds given a real mount and ResourceLimits");
    if (!handle.has_value()) {
        std::cerr << "create() failed, aborting remaining checks: "
                  << ::agentengine::test_support::describe(handle.error()) << "\n";
        return 1;
    }

    {
        ExecRequest req{.language = "native", .source = hostile_child_cmd("sleep 50")};
        auto outcome = backend.exec(*handle, req, ctx);
        AE_CHECK(outcome.has_value(), "C2-Linux: exec() of a well-behaved child returns a result");
        if (outcome.has_value()) {
            AE_CHECK(outcome->klass == exec_outcome_class::ok, "C2-Linux: a well-behaved exec reports ok");
            AE_CHECK(outcome->stdout_text.find("SLEEP_DONE") != std::string::npos,
                      "C2-Linux: exec() captures the child's real stdout");
        }
    }

    {
        ExecRequest req{.language = "native", .source = hostile_child_cmd("spin")};
        auto outcome = backend.exec(*handle, req, ctx);
        AE_CHECK(outcome.has_value() && outcome->klass == exec_outcome_class::timeout,
                  "C2-Linux: a CPU-spinning exec is killed by wall_ms and reports timeout");
    }

    {
        ExecRequest req{.language = "native", .source = hostile_child_cmd("fail 7")};
        auto outcome = backend.exec(*handle, req, ctx);
        AE_CHECK(outcome.has_value(), "C2-Linux: exec() of a failing child returns a result");
        if (outcome.has_value()) {
            AE_CHECK(outcome->klass == exec_outcome_class::crash,
                      "C2-Linux: an ordinary nonzero exit reports crash, not oom (positive control)");
            AE_CHECK(outcome->stdout_text.find("FAIL_MODE") != std::string::npos,
                      "C2-Linux: stdout is captured even on a nonzero exit");
        }
    }

    {
        // A separate, longer-wall_ms handle for this one case: measured directly against this
        // harness's own environment (a manual cgroup v2 memory.max repro, outside this backend
        // entirely, and repeated runs of this very test), the kernel's own reclaim-before-OOM-kill
        // dance took anywhere from ~2s to ~7.9s wall-clock for a 512 MB request against a 32 MB
        // cap -- real variance under host load, and MUCH slower in every case than Windows' Job
        // Object memory limit (14-22ms, ADR-004 §10.2). A short wall_ms here would make this
        // backend's OWN timeout watcher pre-empt the real OOM kill and misclassify it as
        // `timeout`, not a bug in the backend -- the primary handle above deliberately keeps a
        // short wall_ms=2000 for the *timeout* proof, which needs to stay fast. 25s here is a
        // safety margin over the observed 2-7.9s range, not a tight bound.
        SandboxSpec oom_spec = spec;
        oom_spec.limits.wall_ms = 25000;
        auto oom_handle = backend.create(oom_spec, ctx);
        AE_CHECK(oom_handle.has_value(), "C2-Linux: create() succeeds for the oom-case handle");
        if (oom_handle.has_value()) {
            ExecRequest req{.language = "native", .source = hostile_child_cmd("alloc 512")};  // 512 MB >> 32 MB cap
            auto outcome = backend.exec(*oom_handle, req, ctx);
            AE_CHECK(outcome.has_value() && outcome->klass == exec_outcome_class::oom,
                      "C2-Linux: exceeding memory_bytes reports oom (real cgroups v2 OOM-kill signal, "
                      "not the peak-usage heuristic the Windows side needs)");
            backend.destroy(*oom_handle);
        }
    }

    backend.destroy(*handle);

    {
        ExecRequest req{.language = "native", .source = hostile_child_cmd("sleep 10")};
        auto outcome = backend.exec(*handle, req, ctx);
        AE_CHECK(!outcome.has_value(), "C2-Linux: exec() on a destroyed SandboxHandle fails closed");
    }

    std::filesystem::remove_all(work_dir, ec);

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
