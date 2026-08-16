// Milestone 2 Phase C, task C6 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// Teardown-cycle proof (008 SS9 G4) against NativeJailBackend -- scoped down from the RFC's literal
// 10^5 create/exec/destroy cycles to a machine-safe bounded count (CLAUDE.md Machine Safety), same
// posture as M1 deferring 001's 10^4-session gate.
//
// G4's text: "10^5 create/exec/destroy cycles leak no memory, no handles, no processes, no temp
// files (ASan + handle/pid census)." This project has no ASan build configured yet (no sanitizer
// flag anywhere in CMakeLists.txt/tests/CMakeLists.txt) -- the census half of G4's parenthetical is
// what this test performs; an ASan-instrumented build remains open work, tracked in the planning
// doc rather than silently assumed covered.
//
// Four resources are censused across kTeardownCycles iterations of create()/exec()/destroy(), each
// the real per-cycle-allocated resource this backend's own code opens (native_jail_backend.cpp) or
// mutates (app_container_profile.cpp):
//   1. Process handle count (GetProcessHandleCount) -- catches a leaked Job Object/process/thread/
//      pipe HANDLE (four kinds exec() opens every call; HandleGuard/JobObjectLimits are supposed to
//      close all of them).
//   2. Process private-bytes usage (GetProcessMemoryInfo's PrivateUsage) -- catches unbounded
//      per-cycle heap growth; most sensitive to the highest-risk single resource here, the two
//      1 MiB CreatePipe buffers exec() allocates every call (kPipeBufferBytes).
//   3. AppContainer DACL entry count on the reused mount directory -- catches ACE accumulation from
//      calling create() (and therefore grant_path) once per cycle on the SAME mount.
//      app_container_profile.hpp's own header says grant_path is "additive... not idempotent
//      against repeated calls with DIFFERENT read_write values" -- implying repeated calls with the
//      SAME value should merge into the existing ACE rather than accumulate. This test is that
//      claim's first proof (a positive control below shows the differing-value case genuinely does
//      accumulate, so a flat count here is not just an insensitive metric).
//   4. No `job_object_hostile_child.exe` process left running (CreateToolhelp32Snapshot by name) --
//      catches a process that outlived its Job Object.
//
// Non-vacuousness: two small positive controls run BEFORE the real cycle loop, through the exact
// same measurement functions the real loop's assertions use, deliberately producing a known-real
// leak of a bounded size -- proving the technique is sensitive, not merely reporting a number that
// happens not to have moved in this one run.
//
// kTeardownCycles=300 (not 10^5): each cycle launches a real AppContainer process under a real Job
// Object -- 10^5 of those would take this test from seconds to potentially hours and is exactly the
// kind of unbounded-runtime risk CLAUDE.md's Machine Safety section rules out. 300 is chosen for its
// OWN sensitivity, not merely "fast": a systemic leak of even one HANDLE per cycle would grow the
// handle count by ~300 against a kMaxHandleGrowth slack of 20 (15x over), and a leak of even one of
// exec()'s own 1 MiB pipe buffers per cycle would grow private-bytes usage by ~300 MiB against a
// kMaxPrivateBytesGrowth slack of 50 MiB (6x over) -- either real leak is caught with a wide margin
// at this count, which is what actually justifies calling 300 "enough," not just "cheap."

#include <windows.h>

#include <aclapi.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cassert>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "backends/native_jail/app_container_profile.hpp"
#include "backends/native_jail/native_jail_backend.hpp"
#include "support/crt_fail_fast.hpp"
#include "support/error_detail.hpp"

using namespace agentengine;
using agentengine::native_jail::AppContainerProfile;
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

DWORD current_handle_count() {
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return n;
}

SIZE_T current_private_bytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                          sizeof(pmc));
    return pmc.PrivateUsage;
}

// Total DACL entry count on `path` -- not filtered to a specific SID; the point is whether the
// count changes across repeated grant_path calls, not which trustee owns which entry.
DWORD dacl_entry_count(std::wstring const& path) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS || dacl == nullptr) return 0;
    ACL_SIZE_INFORMATION size_info{};
    GetAclInformation(dacl, &size_info, sizeof(size_info), AclSizeInformation);
    if (sd != nullptr) LocalFree(sd);
    return size_info.AceCount;
}

// Count of live processes whose image name matches `name_lower` (already lowercase) -- used to
// confirm no `job_object_hostile_child.exe` outlives its Job Object across the whole run.
int count_processes_named(std::wstring const& name_lower) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    int count = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            std::wstring exe = entry.szExeFile;
            std::transform(exe.begin(), exe.end(), exe.begin(),
                            [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
            if (exe == name_lower) count++;
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return count;
}

}  // namespace

int main() {
    ::agentengine::test_support::fail_fast_on_windows();

    std::filesystem::path work_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_teardown_cycles_test";
    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);
    AE_CHECK(!ec, "setup: scratch mount directory exists");

    std::wstring hostile_child_name_lower =
        std::filesystem::path(AE_HOSTILE_CHILD_EXE).filename().wstring();
    std::transform(hostile_child_name_lower.begin(), hostile_child_name_lower.end(),
                    hostile_child_name_lower.begin(),
                    [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    // NOT asserted to be exactly zero here: job_object_hostile_child.exe is shared with several
    // sibling test binaries (test_job_object_limits, test_native_jail_parity_windows, ...) that can
    // legitimately have their own instances alive at this moment under a parallel `ctest -j4` run
    // (measured directly -- an early version of this test asserted ==0 here and failed under -j4
    // for exactly this reason, a test-methodology bug, not a backend leak). This test is marked
    // RUN_SERIAL in tests/CMakeLists.txt to minimize that overlap, and the real leak check below
    // is delta-based (before vs. after THIS test's own 300 cycles), which is robust to whatever
    // sibling-process count happens to be nonzero at either snapshot.

    // ---- Positive control 1: a deliberate handle leak IS detected by GetProcessHandleCount. -----
    {
        DWORD before = current_handle_count();
        constexpr int kLeakCount = 50;
        std::vector<HANDLE> leaked;
        for (int i = 0; i < kLeakCount; i++) {
            HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (h != nullptr) leaked.push_back(h);
        }
        DWORD after_leak = current_handle_count();
        AE_CHECK(after_leak >= before + kLeakCount - 2,
                  "positive control: a deliberate 50-handle leak is detected by "
                  "GetProcessHandleCount (validates the census this test's real assertions use)");
        for (HANDLE h : leaked) CloseHandle(h);
    }

    // ---- Positive control 2: grant_path with DIFFERING read_write values on the same path really
    // does accumulate ACEs (app_container_profile.hpp's own documented non-idempotent case) --
    // proves dacl_entry_count() is sensitive to real accumulation, not just a number that never
    // moves regardless of what happens.
    {
        std::filesystem::path probe_dir = work_dir / "dacl_probe";
        std::error_code probe_ec;
        std::filesystem::create_directories(probe_dir, probe_ec);
        AE_CHECK(!probe_ec, "setup: DACL positive-control directory exists");

        auto profile = AppContainerProfile::ensure(
            L"AgentEngine.NativeJail", L"AgentEngine Native Jail",
            L"AgentEngine native-jail sandbox AppContainer profile (008 SS1b, ADR-004)");
        AE_CHECK(profile.has_value(),
                  "setup: AppContainerProfile::ensure() succeeds for the positive control (same "
                  "deployment-scoped profile NativeJailBackend uses internally)");
        if (profile.has_value()) {
            DWORD before = dacl_entry_count(probe_dir.wstring());
            auto g1 = profile->grant_path(probe_dir.wstring(), /*read_write=*/false);
            AE_CHECK(g1.has_value(), "positive control: grant_path(read-only) succeeds");
            auto g2 = profile->grant_path(probe_dir.wstring(), /*read_write=*/true);
            AE_CHECK(g2.has_value(), "positive control: grant_path(read-write) succeeds");
            DWORD after = dacl_entry_count(probe_dir.wstring());
            AE_CHECK(after >= before + 2,
                      "positive control: grant_path called with DIFFERING read_write values on the "
                      "same path accumulates ACEs instead of merging (validates dacl_entry_count()'s "
                      "sensitivity ahead of the real loop below, where every cycle uses the SAME "
                      "value and is expected to stay flat)");
        }
    }

    // ---- The real cycle loop. --------------------------------------------------------------------
    SandboxSpec spec;
    spec.mounts.push_back(
        MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
    spec.limits.wall_ms = 2000;
    spec.limits.memory_bytes = 32ull * 1024 * 1024;  // 32 MB
    spec.limits.pids = 4;
    spec.limits.output_bytes = 1024 * 1024;

    NativeJailBackend backend;
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

    // Warm-up cycles: absorb one-time costs (AppContainer profile creation, first DACL grant, first
    // lazy DLL page-ins) so the before/after census below measures steady-state per-cycle behavior,
    // not first-call setup misclassified as a leak.
    constexpr int kWarmupCycles = 5;
    for (int i = 0; i < kWarmupCycles; i++) run_cycle(i);

    DWORD handles_before = current_handle_count();
    SIZE_T private_bytes_before = current_private_bytes();
    DWORD dacl_before = dacl_entry_count(work_dir.wstring());
    int hostile_children_before = count_processes_named(hostile_child_name_lower);

    constexpr int kTeardownCycles = 300;  // see file header for the sensitivity rationale
    for (int i = 0; i < kTeardownCycles; i++) run_cycle(kWarmupCycles + i);

    DWORD handles_after = current_handle_count();
    SIZE_T private_bytes_after = current_private_bytes();
    DWORD dacl_after = dacl_entry_count(work_dir.wstring());
    int hostile_children_after = count_processes_named(hostile_child_name_lower);

    std::cout << "  census: handles " << handles_before << " -> " << handles_after
              << ", private_bytes " << private_bytes_before << " -> " << private_bytes_after
              << ", dacl_entries " << dacl_before << " -> " << dacl_after
              << ", hostile_children " << hostile_children_before << " -> " << hostile_children_after
              << "\n";

    constexpr DWORD kMaxHandleGrowth = 20;
    AE_CHECK(handles_after <= handles_before + kMaxHandleGrowth,
              "C6/G4: process handle count does not grow across 300 create/exec/destroy cycles "
              "beyond a small constant slack");

    constexpr SIZE_T kMaxPrivateBytesGrowth = 50ull * 1024 * 1024;
    AE_CHECK(private_bytes_after <= private_bytes_before + kMaxPrivateBytesGrowth,
              "C6/G4: process private-bytes usage does not grow across 300 create/exec/destroy "
              "cycles beyond a generous bound");

    AE_CHECK(dacl_after == dacl_before,
              "C6/G4: repeated create() calls on the SAME mount with the SAME read_write value do "
              "not accumulate DACL entries (grant_path's documented merge behavior, now measured)");

    constexpr int kMaxHostileChildGrowth = 5;  // slack for sibling tests' own instances, not ours
    AE_CHECK(hostile_children_after <= hostile_children_before + kMaxHostileChildGrowth,
              "C6/G4: job_object_hostile_child.exe process count does not grow across 300 "
              "create/exec/destroy cycles beyond a small constant slack (delta-based -- see the "
              "hostile_child_name_lower comment above for why an absolute zero isn't safe under "
              "parallel ctest)");

    std::filesystem::remove_all(work_dir, ec);

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
