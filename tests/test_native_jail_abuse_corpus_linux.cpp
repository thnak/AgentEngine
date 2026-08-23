// Milestone 2 Phase C, task C3 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// the systematic 008-Sandbox-and-Isolation.md SS7 abuse-case corpus, scoped to the subset that
// needs no interpreter (010) -- fork bomb, OOM, infinite loop, unbounded output -- against the
// real Linux LinuxNativeJailBackend (C2). This is the SS9 G2 gate itself: "every SS7 abuse case is
// contained, with a measured kill time, and a positive control (limits deliberately disabled)
// demonstrably fails -- so the test is not vacuous." C2's own test
// (test_native_jail_backend_linux.cpp) already proves the backend's create/exec/destroy shape
// works; this file is the abuse-case gate on top of it, with a positive control for every case.
//
// fs-escape attempt is covered in a DEDICATED file, not here:
// test_native_jail_fs_containment_linux.cpp -- LinuxNativeJailBackend now builds a real
// pivot_root/bind-mount jail (setup_jail(), 008 SS9 G2/G3), closing the gap this comment used to
// describe (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md's C3 writeup / the
// GitHub issue it was tracked under). Every SandboxSpec below now needs an explicit toolchain
// grant (helpers/native_jail_linux_toolchain_mounts.hpp) for `/bin/sh` itself to be reachable
// inside that jail -- see this backend's own header comment for why no such grant is implicit.
//
// Requires a delegated cgroup v2 root with memory/pids already enabled (see
// linux_native_jail_backend.hpp's precondition note) and CAP_SYS_ADMIN -- run via
// tests/helpers/cgroup_v2_test_setup.sh inside a --privileged container, same as C2's Linux test.
//
// Real child processes are spawned under real namespace + cgroup + seccomp isolation -- bounded by
// this test's own generous-but-finite wall_ms values (CLAUDE.md Machine Safety).

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "backends/native_jail/linux_native_jail_backend.hpp"
#include "helpers/native_jail_linux_toolchain_mounts.hpp"

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

std::string quote(std::string const& s) { return "\"" + s + "\""; }

std::string hostile_child_cmd(std::string const& args) {
    return quote(AE_HOSTILE_CHILD_POSIX_EXE) + " " + args;
}

int parse_spawn_succeeded(std::string const& stdout_text) {
    auto pos = stdout_text.find("succeeded=");
    if (pos == std::string::npos) return -1;
    return std::atoi(stdout_text.c_str() + pos + std::string("succeeded=").size());
}

long long elapsed_ms(std::chrono::steady_clock::time_point t0, std::chrono::steady_clock::time_point t1) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
}

}  // namespace

int main() {
    std::filesystem::path work_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_abuse_corpus_test_linux";
    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);
    AE_CHECK(!ec, "setup: scratch work_dir exists");

    EffectContext ctx;

    // ---- Case 1: fork bomb (008 SS7) -- contained by ResourceLimits::pids (cgroups v2 pids.max) --
    {
        LinuxNativeJailBackend backend;
        SandboxSpec contained_spec;
        contained_spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        agentengine::native_jail::test::add_shell_toolchain_mounts(contained_spec);
        contained_spec.limits.wall_ms = 5000;
        contained_spec.limits.memory_bytes = 64ull * 1024 * 1024;
        contained_spec.limits.pids = 3;
        contained_spec.limits.output_bytes = 1024 * 1024;

        auto handle = backend.create(contained_spec, ctx);
        AE_CHECK(handle.has_value(), "C3-Linux fork-bomb: create() with pids=3 succeeds");
        if (handle.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req{
                .language = "native",
                .source = hostile_child_cmd("spawn 40 " + quote(AE_HOSTILE_CHILD_POSIX_EXE))};
            auto outcome = backend.exec(*handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            AE_CHECK(outcome.has_value(), "C3-Linux fork-bomb: contained exec() returns a result");
            if (outcome.has_value()) {
                int succeeded = parse_spawn_succeeded(outcome->stdout_text);
                std::cout << "  measured: fork-bomb contained (pids=3), requested=40 succeeded="
                          << succeeded << ", exec took " << elapsed_ms(t0, t1) << " ms\n";
                AE_CHECK(succeeded >= 0 && succeeded < 40,
                          "C3-Linux fork-bomb: pids=3 stops far fewer than 40 children from being "
                          "created");
            }
            backend.destroy(*handle);
        }

        // Positive control: pids DISABLED (0) -- proves the containment above is real.
        LinuxNativeJailBackend positive_backend;
        SandboxSpec positive_spec = contained_spec;
        positive_spec.limits.pids = 0;
        auto positive_handle = positive_backend.create(positive_spec, ctx);
        AE_CHECK(positive_handle.has_value(),
                  "C3-Linux fork-bomb positive control: create() with pids disabled succeeds");
        if (positive_handle.has_value()) {
            ExecRequest req{
                .language = "native",
                .source = hostile_child_cmd("spawn 15 " + quote(AE_HOSTILE_CHILD_POSIX_EXE))};
            auto outcome = positive_backend.exec(*positive_handle, req, ctx);
            AE_CHECK(outcome.has_value(), "C3-Linux fork-bomb positive control: exec() returns a result");
            if (outcome.has_value()) {
                int succeeded = parse_spawn_succeeded(outcome->stdout_text);
                std::cout << "  measured: fork-bomb positive control (pids disabled), requested=15 "
                             "succeeded=" << succeeded << "\n";
                AE_CHECK(succeeded == 15,
                          "C3-Linux fork-bomb positive control: with pids disabled, all 15 children "
                          "succeed (the containment above is real, not vacuous)");
            }
            positive_backend.destroy(*positive_handle);
        }
    }

    // ---- Case 2: OOM (008 SS7) -- contained by ResourceLimits::memory_bytes (cgroups v2) --------
    {
        LinuxNativeJailBackend backend;
        SandboxSpec contained_spec;
        contained_spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        agentengine::native_jail::test::add_shell_toolchain_mounts(contained_spec);
        // 25s: C2-Linux's own measured finding -- cgroups v2's reclaim-before-OOM-kill dance took
        // ~2-7.9s wall-clock in this harness's environment for this exact 512 MB-vs-32 MB shape; a
        // short wall_ms would make this backend's own timeout watcher pre-empt the real OOM kill
        // and misclassify it as `timeout`.
        contained_spec.limits.wall_ms = 25000;
        contained_spec.limits.memory_bytes = 32ull * 1024 * 1024;
        contained_spec.limits.pids = 4;
        contained_spec.limits.output_bytes = 1024 * 1024;

        auto handle = backend.create(contained_spec, ctx);
        AE_CHECK(handle.has_value(), "C3-Linux OOM: create() with memory_bytes=32MB succeeds");
        if (handle.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req{.language = "native", .source = hostile_child_cmd("alloc 512")};
            auto outcome = backend.exec(*handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            AE_CHECK(outcome.has_value() && outcome->klass == exec_outcome_class::oom,
                      "C3-Linux OOM: exceeding memory_bytes reports oom");
            std::cout << "  measured: OOM contained (32 MB cap, 512 MB request), exec took "
                      << elapsed_ms(t0, t1) << " ms\n";
            backend.destroy(*handle);
        }

        // Positive control: memory_bytes DISABLED -- the identical shape of allocation succeeds.
        LinuxNativeJailBackend positive_backend;
        SandboxSpec positive_spec = contained_spec;
        positive_spec.limits.memory_bytes = 0;
        positive_spec.limits.wall_ms = 5000;  // no OOM-kill wait needed on this path
        auto positive_handle = positive_backend.create(positive_spec, ctx);
        AE_CHECK(positive_handle.has_value(),
                  "C3-Linux OOM positive control: create() with memory disabled succeeds");
        if (positive_handle.has_value()) {
            ExecRequest req{.language = "native", .source = hostile_child_cmd("alloc 64")};
            auto outcome = positive_backend.exec(*positive_handle, req, ctx);
            AE_CHECK(outcome.has_value() && outcome->klass == exec_outcome_class::ok,
                      "C3-Linux OOM positive control: with memory disabled, the same-shape "
                      "allocation succeeds (the containment above is real, not vacuous)");
            positive_backend.destroy(*positive_handle);
        }
    }

    // ---- Case 3: infinite loop (008 SS7) -- contained by ResourceLimits::wall_ms ----------------
    {
        LinuxNativeJailBackend backend;
        SandboxSpec short_spec;
        short_spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        agentengine::native_jail::test::add_shell_toolchain_mounts(short_spec);
        short_spec.limits.wall_ms = 500;
        short_spec.limits.memory_bytes = 64ull * 1024 * 1024;
        short_spec.limits.pids = 4;
        short_spec.limits.output_bytes = 1024 * 1024;

        auto handle = backend.create(short_spec, ctx);
        AE_CHECK(handle.has_value(), "C3-Linux infinite-loop: create() with wall_ms=500 succeeds");
        long long short_elapsed = -1;
        if (handle.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req{.language = "native", .source = hostile_child_cmd("spin")};
            auto outcome = backend.exec(*handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            short_elapsed = elapsed_ms(t0, t1);
            AE_CHECK(outcome.has_value() && outcome->klass == exec_outcome_class::timeout,
                      "C3-Linux infinite-loop: a CPU-spinning exec is killed by wall_ms and reports "
                      "timeout");
            std::cout << "  measured: infinite-loop killed at wall_ms=500, actual " << short_elapsed
                      << " ms\n";
            backend.destroy(*handle);
        }

        // Same rationale as the Windows corpus: a literal "wall_ms disabled" run would hang this
        // test forever (spin never exits on its own) -- the safe stand-in is a materially longer
        // budget on the identical workload, proving the short kill above is real enforcement.
        LinuxNativeJailBackend long_backend;
        SandboxSpec long_spec = short_spec;
        long_spec.limits.wall_ms = 1800;
        auto long_handle = long_backend.create(long_spec, ctx);
        AE_CHECK(long_handle.has_value(),
                  "C3-Linux infinite-loop positive control: create() with wall_ms=1800 succeeds");
        if (long_handle.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req{.language = "native", .source = hostile_child_cmd("spin")};
            auto outcome = long_backend.exec(*long_handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            long long long_elapsed = elapsed_ms(t0, t1);
            std::cout << "  measured: infinite-loop positive control, wall_ms=1800, actual "
                      << long_elapsed << " ms\n";
            AE_CHECK(outcome.has_value() && outcome->klass == exec_outcome_class::timeout,
                      "C3-Linux infinite-loop positive control: still killed by timeout, just later");
            AE_CHECK(short_elapsed >= 0 && long_elapsed > short_elapsed + 500,
                      "C3-Linux infinite-loop positive control: a longer wall_ms budget lets spin "
                      "run materially longer (proves it never exits on its own -- the short kill "
                      "above is real enforcement, not vacuous)");
            long_backend.destroy(*long_handle);
        }
    }

    // ---- Case 4: unbounded output (008 SS7) -- contained by ResourceLimits::output_bytes --------
    {
        LinuxNativeJailBackend backend;
        SandboxSpec contained_spec;
        contained_spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        agentengine::native_jail::test::add_shell_toolchain_mounts(contained_spec);
        contained_spec.limits.wall_ms = 1000;
        contained_spec.limits.memory_bytes = 64ull * 1024 * 1024;
        contained_spec.limits.pids = 4;
        contained_spec.limits.output_bytes = 4096;

        auto handle = backend.create(contained_spec, ctx);
        AE_CHECK(handle.has_value(),
                  "C3-Linux unbounded-output: create() with output_bytes=4096 succeeds");
        if (handle.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req{.language = "native", .source = hostile_child_cmd("flood")};
            auto outcome = backend.exec(*handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            AE_CHECK(outcome.has_value(), "C3-Linux unbounded-output: contained exec() returns a result");
            if (outcome.has_value()) {
                std::cout << "  measured: unbounded-output contained, exec took " << elapsed_ms(t0, t1)
                          << " ms, captured " << outcome->stdout_text.size() << " bytes\n";
                AE_CHECK(outcome->stdout_text.size() <= 4096,
                          "C3-Linux unbounded-output: captured stdout never exceeds the configured "
                          "cap");
                AE_CHECK(!outcome->stdout_text.empty(),
                          "C3-Linux unbounded-output: something was actually captured (the flood ran)");
            }
            backend.destroy(*handle);
        }

        // Positive control: same probe, a materially looser (never literally unbounded --
        // drain_pipe_bounded's own safety floor forbids that by design) output_bytes cap.
        LinuxNativeJailBackend positive_backend;
        SandboxSpec positive_spec = contained_spec;
        positive_spec.limits.output_bytes = 2ull * 1024 * 1024;
        positive_spec.limits.wall_ms = 300;
        auto positive_handle = positive_backend.create(positive_spec, ctx);
        AE_CHECK(positive_handle.has_value(),
                  "C3-Linux unbounded-output positive control: create() with a loose cap succeeds");
        if (positive_handle.has_value()) {
            ExecRequest req{.language = "native", .source = hostile_child_cmd("flood")};
            auto outcome = positive_backend.exec(*positive_handle, req, ctx);
            AE_CHECK(outcome.has_value(),
                      "C3-Linux unbounded-output positive control: exec() returns a result");
            if (outcome.has_value()) {
                std::cout << "  measured: unbounded-output positive control, captured "
                          << outcome->stdout_text.size() << " bytes\n";
                AE_CHECK(outcome->stdout_text.size() > 4096 * 4,
                          "C3-Linux unbounded-output positive control: with a looser cap, materially "
                          "more than the tight 4096-byte cap gets through (the containment above is "
                          "real, not the flood incidentally producing little output)");
            }
            positive_backend.destroy(*positive_handle);
        }
    }

    std::filesystem::remove_all(work_dir, ec);

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
