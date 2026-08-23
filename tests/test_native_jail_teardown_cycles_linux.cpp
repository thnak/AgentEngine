// Milestone 2 Phase C, task C6 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md),
// Linux half: Teardown-cycle proof (008 SS9 G4) against LinuxNativeJailBackend -- see
// test_native_jail_teardown_cycles_windows.cpp's header for the full rationale on why 10^5 cycles
// (the RFC's literal figure) is scoped down to a machine-safe bounded count, and why the census
// approach substitutes for the "ASan" half of G4's parenthetical (this project has no sanitizer
// build configured yet).
//
// Three resources are censused across kTeardownCycles iterations of create()/exec()/destroy(), the
// real per-cycle-allocated resources this backend's own code opens
// (linux_native_jail_backend.cpp) or mutates (cgroup_limits.cpp):
//   1. Open file descriptor count (/proc/self/fd entry count) -- catches a leaked pipe fd (six per
//      exec() call: sync pipe read/write, stdout pipe read/write, stderr pipe read/write; FdGuard is
//      supposed to close every one of them, whether the cycle ran clean or was killed for timeout).
//   2. Resident set size (/proc/self/status VmRSS) -- catches unbounded per-cycle heap/mmap growth;
//      most sensitive to the highest-risk single resource here, the 1 MiB anonymous mmap() exec()
//      allocates for the cloned child's stack every call (munmap'd at the end of exec(), whether the
//      child exited cleanly or was SIGKILLed for a limit).
//   3. Delegated cgroup root directory-entry count (readdir on /sys/fs/cgroup/agentengine) -- catches
//      a leaked cgroup directory from CgroupLimits, the Linux analogue of a leaked Job Object handle
//      on Windows; each create() call makes a fresh subdirectory (cgroup_limits.cpp's
//      CgroupLimits::create) that destroy() must rmdir. MEASURED: the baseline count is NOT near
//      zero (~38 on the kernel this was verified against) -- /sys/fs/cgroup/agentengine is itself a
//      cgroup, and a cgroup directory always contains dozens of standard interface files
//      (cgroup.procs, memory.max, pids.max, ...) alongside any child-cgroup subdirectories. Those
//      files don't come and go, so the count is still exactly as sensitive to a leaked child
//      subdirectory as a from-zero count would be -- confirmed directly by positive control 2 below,
//      which adds and removes one real subdirectory and watches the count move by exactly 1.
//
// Non-vacuousness: two small positive controls run BEFORE the real cycle loop, through the exact
// same measurement functions the real loop's assertions use, deliberately producing a known-real
// leak of a bounded size -- see test_native_jail_teardown_cycles_windows.cpp's header for why this
// project treats that as load-bearing rather than optional polish.
//
// kTeardownCycles=300, matching the Windows half exactly (same sensitivity argument: a 1-fd-per-
// cycle leak would grow fd count by ~300 against a kMaxFdGrowth slack of 10 -- 30x over; a leak of
// even one of exec()'s own 1 MiB child-stack mmaps per cycle would grow RSS by ~300 MiB against a
// kMaxRssGrowthKb slack of 50 MiB -- 6x over).
//
// A 4th resource is censused, added once LinuxNativeJailBackend started building a real
// pivot_root/bind-mount jail per exec() call (setup_jail(), 008 SS9 G2/G3):
//   4. jail_root directory count under the backend's jail_root_base_ (default
//      /tmp/agentengine-native-jail) -- catches a leaked per-exec jail directory. Each exec()
//      mkdir's a fresh, uniquely-named directory before clone() and rmdir's it after the child is
//      fully reaped (linux_native_jail_backend.cpp); this is the concrete, falsifiable form of the
//      design draft's MUST-FIX 1 verification ("after N sandbox creations and teardowns, no leaked
//      entries remain") for the ONE piece of that leak surface visible from the HOST side without
//      inspecting another process's already-torn-down mount namespace -- MS_PRIVATE (setup_jail's
//      unconditional first step) is what keeps the mounts THEMSELVES from ever reaching host
//      /proc/mounts in the first place, so there is nothing to unmount from here; this loop instead
//      proves the plain host-side mkdir/rmdir bracketing the mount setup doesn't leak.

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

int dir_entry_count(std::string const& path) {
    DIR* d = opendir(path.c_str());
    if (d == nullptr) return -1;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        n++;
    }
    closedir(d);
    return n;
}

int current_fd_count() { return dir_entry_count("/proc/self/fd"); }

long current_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            long kb = 0;
            std::sscanf(line.c_str(), "VmRSS: %ld kB", &kb);
            return kb;
        }
    }
    return -1;
}

}  // namespace

int main() {
    std::string const kDelegatedRoot = "/sys/fs/cgroup/agentengine";
    std::string const kJailRootBase = "/tmp/agentengine-native-jail";

    std::filesystem::path work_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_teardown_cycles_test_linux";
    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);
    AE_CHECK(!ec, "setup: scratch mount directory exists");

    // ---- Positive control 1: a deliberate fd leak IS detected by /proc/self/fd counting. ---------
    {
        int before = current_fd_count();
        constexpr int kLeakCount = 50;
        std::vector<int> leaked;
        for (int i = 0; i < kLeakCount; i++) {
            int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (fd >= 0) leaked.push_back(fd);
        }
        int after_leak = current_fd_count();
        AE_CHECK(after_leak >= before + kLeakCount - 2,
                  "positive control: a deliberate 50-fd leak is detected by /proc/self/fd counting "
                  "(validates the census this test's real assertions use)");
        for (int fd : leaked) ::close(fd);
    }

    // ---- Positive control 2: a cgroup subdirectory really does show up in / disappear from the
    // delegated-root directory count -- proves dir_entry_count() reflects the filesystem, not just
    // a number that never moves.
    {
        std::string probe_dir = kDelegatedRoot + "/teardown_cycles_probe";
        int before = dir_entry_count(kDelegatedRoot);
        AE_CHECK(before >= 0, "setup: delegated cgroup root is readable");
        int mkdir_rc = ::mkdir(probe_dir.c_str(), 0755);
        AE_CHECK(mkdir_rc == 0, "positive control: setup can create a subdirectory under the "
                                  "delegated cgroup root");
        int after_mkdir = dir_entry_count(kDelegatedRoot);
        AE_CHECK(after_mkdir == before + 1,
                  "positive control: a real subdirectory appears in the delegated-root directory "
                  "count (validates dir_entry_count()'s sensitivity ahead of the real loop below)");
        ::rmdir(probe_dir.c_str());
        int after_rmdir = dir_entry_count(kDelegatedRoot);
        AE_CHECK(after_rmdir == before,
                  "positive control: removing that subdirectory drops the count back down");
    }

    // ---- The real cycle loop. --------------------------------------------------------------------
    SandboxSpec spec;
    spec.mounts.push_back(
        MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
    agentengine::native_jail::test::add_shell_toolchain_mounts(spec);
    spec.limits.wall_ms = 2000;
    spec.limits.memory_bytes = 32ull * 1024 * 1024;  // 32 MB
    spec.limits.pids = 4;
    spec.limits.output_bytes = 1024 * 1024;

    LinuxNativeJailBackend backend(kDelegatedRoot);
    EffectContext ctx;

    auto run_cycle = [&](int cycle_index) {
        auto handle = backend.create(spec, ctx);
        if (!handle.has_value()) {
            std::cerr << "cycle " << cycle_index << ": create() failed: "
                      << ::agentengine::test_support::describe(handle.error()) << "\n";
            ++g_failures;
            return;
        }
        ExecRequest req{.language = "native", .source = hostile_child_cmd("sleep 1")};
        auto outcome = backend.exec(*handle, req, ctx);
        if (!outcome.has_value() || outcome->klass != exec_outcome_class::ok) {
            std::cerr << "cycle " << cycle_index << ": exec() did not report ok\n";
            ++g_failures;
        }
        backend.destroy(*handle);
    };

    // Warm-up cycles: absorb one-time costs (delegated-root controller-enable write, first lazy
    // page-ins) so the before/after census below measures steady-state per-cycle behavior.
    constexpr int kWarmupCycles = 5;
    for (int i = 0; i < kWarmupCycles; i++) run_cycle(i);

    int fds_before = current_fd_count();
    long rss_before = current_rss_kb();
    int cgroup_dirs_before = dir_entry_count(kDelegatedRoot);
    // 0, not -1: the kWarmupCycles loop above already ran, so kJailRootBase already exists (each
    // exec() mkdir_p's it) but should be empty again -- every warmup cycle's own jail directory was
    // already rmdir'd by the time its exec() call returned.
    int jail_dirs_before = dir_entry_count(kJailRootBase);

    constexpr int kTeardownCycles = 300;  // see file header for the sensitivity rationale
    for (int i = 0; i < kTeardownCycles; i++) run_cycle(kWarmupCycles + i);

    int fds_after = current_fd_count();
    long rss_after = current_rss_kb();
    int cgroup_dirs_after = dir_entry_count(kDelegatedRoot);
    int jail_dirs_after = dir_entry_count(kJailRootBase);

    std::cout << "  census: fds " << fds_before << " -> " << fds_after << ", rss_kb " << rss_before
              << " -> " << rss_after << ", cgroup_dirs " << cgroup_dirs_before << " -> "
              << cgroup_dirs_after << ", jail_dirs " << jail_dirs_before << " -> " << jail_dirs_after
              << "\n";

    constexpr int kMaxFdGrowth = 10;
    AE_CHECK(fds_after <= fds_before + kMaxFdGrowth,
              "C6/G4: open file descriptor count does not grow across 300 create/exec/destroy "
              "cycles beyond a small constant slack");

    constexpr long kMaxRssGrowthKb = 50 * 1024;  // 50 MiB
    AE_CHECK(rss_before < 0 || rss_after < 0 || rss_after <= rss_before + kMaxRssGrowthKb,
              "C6/G4: resident set size does not grow across 300 create/exec/destroy cycles beyond "
              "a generous bound");

    AE_CHECK(cgroup_dirs_after == cgroup_dirs_before,
              "C6/G4: no leftover cgroup subdirectory remains under the delegated root after 300 "
              "create/exec/destroy cycles");

    AE_CHECK(jail_dirs_before >= 0 && jail_dirs_after == jail_dirs_before,
              "C6/G4 (pivot_root jail, design draft MUST-FIX 1's host-visible half): no leftover "
              "jail_root directory remains under jail_root_base_ after 300 create/exec/destroy "
              "cycles -- each exec()'s mkdir/rmdir bracket is symmetric even under repetition");

    std::filesystem::remove_all(work_dir, ec);

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
