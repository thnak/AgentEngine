// Milestone 2 Phase C, task C3 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// the systematic 008-Sandbox-and-Isolation.md SS7 abuse-case corpus, scoped to the subset that
// needs no interpreter (010) -- fork bomb, OOM, infinite loop, fs-escape attempt, unbounded
// output -- against the real Windows NativeJailBackend (C2). This is the SS9 G2 gate itself:
// "every SS7 abuse case is contained, with a measured kill time, and a positive control
// (limits deliberately disabled) demonstrably fails -- so the test is not vacuous." C2's own test
// (test_native_jail_backend_windows.cpp) already proves the backend's create/exec/destroy shape
// works; this file is the abuse-case gate on top of it, with a positive control for every case.
//
// fs-escape attempt is Windows-only in this pass -- LinuxNativeJailBackend has no real path
// containment yet (CLONE_NEWNS alone restricts nothing without a populated mount namespace /
// chroot), a tracked gap (see the Linux corpus test's header and the planning doc), not silently
// dropped.
//
// Real child processes under real AppContainer + Job Object isolation -- bounded by this test's
// own generous-but-finite wall_ms values (CLAUDE.md Machine Safety).

#include <windows.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "backends/native_jail/native_jail_backend.hpp"

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

void disable_crt_assert_dialog() {
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
}

std::string quote(std::string const& s) { return "\"" + s + "\""; }

std::string hostile_child_cmd(std::string const& args) {
    return quote(AE_HOSTILE_CHILD_EXE) + " " + args;
}

// Counts "succeeded=<n>" out of a spawn probe's SPAWN_RESULT line. Returns -1 if not found.
int parse_spawn_succeeded(std::string const& stdout_text) {
    auto pos = stdout_text.find("succeeded=");
    if (pos == std::string::npos) return -1;
    return std::atoi(stdout_text.c_str() + pos + std::string("succeeded=").size());
}

long long elapsed_ms(std::chrono::steady_clock::time_point t0, std::chrono::steady_clock::time_point t1) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
}

// Launches `cmdline` as an ORDINARY host process -- no AppContainer, no Job Object, no
// PROCESS_CREATION_CHILD_PROCESS_RESTRICTED policy -- i.e. `sandbox_profile::none` (008 SS1). This
// is the fork-bomb case's positive control: NativeJailBackend's own SandboxSpec has no field that
// disables its unconditional nested-exec-denial policy (008 SS4 "Exec (nested): denied" is not
// gated by ResourceLimits::pids), so "limits deliberately disabled" for THIS specific mechanism
// means running outside the backend entirely, not reconfiguring it. Does not wait for the process
// to fully exit (mode_spawn's own children each sleep up to 500ms before it returns) -- reads
// whatever the child has written after a fixed grace period, then terminates it; this is a
// short-lived probe, not a general-purpose process launcher.
std::string run_raw_unsandboxed(std::string const& cmdline, DWORD grace_ms) {
    SECURITY_ATTRIBUTES pipe_sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_h = nullptr, write_h = nullptr;
    if (!CreatePipe(&read_h, &write_h, &pipe_sa, 0)) return {};
    SetHandleInformation(read_h, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_h;
    si.hStdError = write_h;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::vector<char> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back('\0');

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessA(nullptr, mutable_cmdline.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
                                   0, nullptr, nullptr, &si, &pi);
    CloseHandle(write_h);
    if (!created) {
        CloseHandle(read_h);
        return {};
    }

    Sleep(grace_ms);

    std::string out;
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(read_h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        DWORD read = 0;
        if (!ReadFile(read_h, buf, sizeof(buf), &read, nullptr) || read == 0) break;
        out.append(buf, read);
    }

    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read_h);
    return out;
}

}  // namespace

int main() {
    disable_crt_assert_dialog();

    std::filesystem::path work_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_abuse_corpus_test";
    std::filesystem::path secret_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_abuse_corpus_secret";
    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);
    AE_CHECK(!ec, "setup: scratch work_dir exists");
    std::filesystem::create_directories(secret_dir, ec);
    AE_CHECK(!ec, "setup: scratch secret_dir exists");

    std::filesystem::path secret_file = secret_dir / "secret.txt";
    {
        std::ofstream out(secret_file, std::ios::binary);
        out << "TOP_SECRET_CONTENT_NOT_GRANTED_TO_THE_GUEST";
    }

    EffectContext ctx;

    // ---- Case 1: fork bomb (008 SS7) -- contained by PROCESS_CREATION_CHILD_PROCESS_RESTRICTED --
    // MEASURED FINDING (this pass): the contained case below returns succeeded=0 REGARDLESS of
    // ResourceLimits::pids -- native_jail_backend.cpp's exec() unconditionally sets
    // PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED (008
    // SS4 "Exec (nested): denied"), which fails CreateProcess for ANY child the guest tries to
    // launch before the Job Object's ActiveProcessLimit ever comes into play. So on this backend,
    // fork-bomb containment is the nested-exec-denial policy, not the pids axis -- pids is
    // defense-in-depth for a mechanism this backend doesn't currently expose a path to reach. This
    // also means "disable pids" is not a meaningful positive control here (confirmed directly: it
    // still returns succeeded=0) -- the real positive control is running the identical probe with
    // NO containment at all (sandbox_profile::none), below.
    {
        NativeJailBackend backend;
        SandboxSpec contained_spec;
        contained_spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        contained_spec.limits.wall_ms = 5000;
        contained_spec.limits.memory_bytes = 64ull * 1024 * 1024;
        contained_spec.limits.pids = 3;
        contained_spec.limits.output_bytes = 1024 * 1024;

        auto handle = backend.create(contained_spec, ctx);
        AE_CHECK(handle.has_value(), "C3 fork-bomb: create() succeeds");
        if (handle.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req{.language = "native",
                             .source = hostile_child_cmd("spawn 40 " + quote(AE_HOSTILE_CHILD_EXE))};
            auto outcome = backend.exec(*handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            AE_CHECK(outcome.has_value(), "C3 fork-bomb: contained exec() returns a result");
            if (outcome.has_value()) {
                int succeeded = parse_spawn_succeeded(outcome->stdout_text);
                std::cout << "  measured: fork-bomb contained, requested=40 succeeded=" << succeeded
                          << ", exec took " << elapsed_ms(t0, t1) << " ms\n";
                AE_CHECK(succeeded == 0,
                          "C3 fork-bomb: nested exec is fully denied -- zero children ever created "
                          "(PROCESS_CREATION_CHILD_PROCESS_RESTRICTED, 008 SS4)");
            }
            backend.destroy(*handle);
        }

        // Positive control: the SAME probe, unsandboxed (no AppContainer, no Job Object, no
        // nested-exec-denial policy at all) -- proves the succeeded=0 above is real containment,
        // not e.g. mode_spawn itself being broken or CreateProcess failing for an unrelated reason.
        std::string raw_cmd = hostile_child_cmd("spawn 10 " + quote(AE_HOSTILE_CHILD_EXE));
        std::string raw_out = run_raw_unsandboxed(raw_cmd, 1500);
        int raw_succeeded = parse_spawn_succeeded(raw_out);
        std::cout << "  measured: fork-bomb positive control (unsandboxed), requested=10 succeeded="
                  << raw_succeeded << "\n";
        AE_CHECK(raw_succeeded == 10,
                  "C3 fork-bomb positive control: unsandboxed, all 10 children succeed (the "
                  "containment above is real, not vacuous)");
    }

    // ---- Case 2: OOM (008 SS7) -- contained by ResourceLimits::memory_bytes --------------------
    {
        NativeJailBackend backend;
        SandboxSpec contained_spec;
        contained_spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        contained_spec.limits.wall_ms = 5000;
        contained_spec.limits.memory_bytes = 32ull * 1024 * 1024;  // 32 MB cap
        contained_spec.limits.pids = 4;
        contained_spec.limits.output_bytes = 1024 * 1024;

        auto handle = backend.create(contained_spec, ctx);
        AE_CHECK(handle.has_value(), "C3 OOM: create() with memory_bytes=32MB succeeds");
        if (handle.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req{.language = "native", .source = hostile_child_cmd("alloc 512")};  // 512 MB >> 32 MB
            auto outcome = backend.exec(*handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            AE_CHECK(outcome.has_value() && outcome->klass == exec_outcome_class::oom,
                      "C3 OOM: exceeding memory_bytes reports oom");
            std::cout << "  measured: OOM contained (32 MB cap, 512 MB request), exec took "
                      << elapsed_ms(t0, t1) << " ms\n";
            backend.destroy(*handle);
        }

        // Positive control: memory_bytes DISABLED -- the identical shape of allocation (well under
        // the disabled backend's actual host memory) must now succeed cleanly.
        SandboxSpec positive_spec = contained_spec;
        positive_spec.limits.memory_bytes = 0;
        NativeJailBackend positive_backend;
        auto positive_handle = positive_backend.create(positive_spec, ctx);
        AE_CHECK(positive_handle.has_value(), "C3 OOM positive control: create() with memory disabled succeeds");
        if (positive_handle.has_value()) {
            ExecRequest req{.language = "native", .source = hostile_child_cmd("alloc 64")};  // 64 MB, > the 32 MB cap above
            auto outcome = positive_backend.exec(*positive_handle, req, ctx);
            AE_CHECK(outcome.has_value() && outcome->klass == exec_outcome_class::ok,
                      "C3 OOM positive control: with memory disabled, the same-shape allocation "
                      "succeeds (the containment above is real, not vacuous)");
            positive_backend.destroy(*positive_handle);
        }
    }

    // ---- Case 3: infinite loop (008 SS7) -- contained by ResourceLimits::wall_ms ---------------
    {
        NativeJailBackend backend;
        SandboxSpec short_spec;
        short_spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        short_spec.limits.wall_ms = 500;
        short_spec.limits.memory_bytes = 64ull * 1024 * 1024;
        short_spec.limits.pids = 4;
        short_spec.limits.output_bytes = 1024 * 1024;

        auto handle = backend.create(short_spec, ctx);
        AE_CHECK(handle.has_value(), "C3 infinite-loop: create() with wall_ms=500 succeeds");
        long long short_elapsed = -1;
        if (handle.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req{.language = "native", .source = hostile_child_cmd("spin")};
            auto outcome = backend.exec(*handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            short_elapsed = elapsed_ms(t0, t1);
            AE_CHECK(outcome.has_value() && outcome->klass == exec_outcome_class::timeout,
                      "C3 infinite-loop: a CPU-spinning exec is killed by wall_ms and reports timeout");
            std::cout << "  measured: infinite-loop killed at wall_ms=500, actual " << short_elapsed
                      << " ms\n";
            backend.destroy(*handle);
        }

        // "Positive control, disabled" is unsafe here -- wall_ms==0 means wait_or_kill blocks
        // INFINITE (job_object_limits.cpp), and `spin` never exits on its own, so a literal
        // disabled run would hang the test forever (CLAUDE.md Machine Safety forbids this). The
        // safe stand-in: a materially LONGER wall_ms budget on the identical spin workload. If the
        // short-budget kill above were incidental (the process happening to finish quickly on its
        // own) rather than real wall_ms enforcement, the long-budget run would finish in roughly
        // the same short time too -- it does not; it runs the full extra distance, proving `spin`
        // truly never exits on its own and the short kill above is the wall_ms mechanism acting.
        NativeJailBackend long_backend;
        SandboxSpec long_spec = short_spec;
        long_spec.limits.wall_ms = 1800;
        auto long_handle = long_backend.create(long_spec, ctx);
        AE_CHECK(long_handle.has_value(), "C3 infinite-loop positive control: create() with wall_ms=1800 succeeds");
        if (long_handle.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req{.language = "native", .source = hostile_child_cmd("spin")};
            auto outcome = long_backend.exec(*long_handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            long long long_elapsed = elapsed_ms(t0, t1);
            std::cout << "  measured: infinite-loop positive control, wall_ms=1800, actual "
                      << long_elapsed << " ms\n";
            AE_CHECK(outcome.has_value() && outcome->klass == exec_outcome_class::timeout,
                      "C3 infinite-loop positive control: still killed by timeout, just later");
            AE_CHECK(short_elapsed >= 0 && long_elapsed > short_elapsed + 500,
                      "C3 infinite-loop positive control: a longer wall_ms budget lets spin run "
                      "materially longer (proves it never exits on its own -- the short kill above "
                      "is real enforcement, not vacuous)");
            long_backend.destroy(*long_handle);
        }
    }

    // ---- Case 4: fs-escape attempt (008 SS7) -- contained by the AppContainer mount grant ------
    {
        NativeJailBackend backend;
        SandboxSpec contained_spec;
        contained_spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        contained_spec.limits.wall_ms = 3000;
        contained_spec.limits.memory_bytes = 32ull * 1024 * 1024;
        contained_spec.limits.pids = 4;
        contained_spec.limits.output_bytes = 1024 * 1024;

        auto handle = backend.create(contained_spec, ctx);
        AE_CHECK(handle.has_value(), "C3 fs-escape: create() with only work_dir mounted succeeds");
        if (handle.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req{.language = "native",
                             .source = hostile_child_cmd("escape " + quote(secret_file.string()))};
            auto outcome = backend.exec(*handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            AE_CHECK(outcome.has_value(), "C3 fs-escape: contained exec() returns a result");
            if (outcome.has_value()) {
                std::cout << "  measured: fs-escape contained, exec took " << elapsed_ms(t0, t1)
                          << " ms, stdout=" << outcome->stdout_text;
                AE_CHECK(outcome->stdout_text.find("ESCAPE_DENIED") != std::string::npos,
                          "C3 fs-escape: a file outside the granted mount is not readable "
                          "(008 SS1b/ADR-004 SS3 -- AppContainer's default-deny ACL)");
            }
            backend.destroy(*handle);
        }

        // Positive control: same backend mechanism, deliberately grant the secret directory too
        // (the AppContainer-shaped equivalent of "disable the limit") -- proves the DENIED result
        // above is really the mount grant acting, not e.g. the file being globally unreadable for
        // some unrelated reason.
        NativeJailBackend positive_backend;
        SandboxSpec positive_spec = contained_spec;
        positive_spec.mounts.push_back(
            MountSpec{.source = secret_dir.string(), .guest_path = "/secret", .read_write = false});
        auto positive_handle = positive_backend.create(positive_spec, ctx);
        AE_CHECK(positive_handle.has_value(), "C3 fs-escape positive control: create() granting the secret dir succeeds");
        if (positive_handle.has_value()) {
            ExecRequest req{.language = "native",
                             .source = hostile_child_cmd("escape " + quote(secret_file.string()))};
            auto outcome = positive_backend.exec(*positive_handle, req, ctx);
            AE_CHECK(outcome.has_value(), "C3 fs-escape positive control: exec() returns a result");
            if (outcome.has_value()) {
                std::cout << "  measured: fs-escape positive control, stdout=" << outcome->stdout_text;
                AE_CHECK(outcome->stdout_text.find("ESCAPE_OK") != std::string::npos,
                          "C3 fs-escape positive control: once the directory is actually granted, "
                          "the read succeeds (the DENIED result above is real, not vacuous)");
            }
            positive_backend.destroy(*positive_handle);
        }

        // Known, already-documented gap (ADR-004 SS6.1, 008 SS7): a curated set of host files carry
        // default `ALL (RESTRICTED) APPLICATION PACKAGES` read ACEs independent of any grant this
        // backend makes -- win.ini is the ADR's own example. Asserted explicitly (not silently
        // skipped) so a future ACL-mediation fix (008 SS1b's "interpreter-level open() mediation is
        // primary") shows up as a diff here, not as a silent behavior change.
        char windir[MAX_PATH]{};
        UINT windir_len = GetWindowsDirectoryA(windir, MAX_PATH);
        if (windir_len > 0 && windir_len < MAX_PATH) {
            std::string win_ini = std::string(windir) + "\\win.ini";
            NativeJailBackend gap_backend;
            auto gap_handle = gap_backend.create(contained_spec, ctx);
            if (gap_handle.has_value()) {
                ExecRequest req{.language = "native", .source = hostile_child_cmd("escape " + quote(win_ini))};
                auto outcome = gap_backend.exec(*gap_handle, req, ctx);
                if (outcome.has_value()) {
                    std::cout << "  measured: known-gap win.ini escape probe, stdout="
                              << outcome->stdout_text;
                    AE_CHECK(outcome->stdout_text.find("ESCAPE_OK") != std::string::npos,
                              "C3 fs-escape known gap (ADR-004 SS6.1): win.ini is readable via the "
                              "OS-default ACE regardless of any grant -- documented, not silently "
                              "dropped; a future fix should update this assertion, not delete it");
                }
                gap_backend.destroy(*gap_handle);
            }
        }
    }

    // ---- Case 5: unbounded output (008 SS7) -- contained by ResourceLimits::output_bytes -------
    {
        NativeJailBackend backend;
        SandboxSpec contained_spec;
        contained_spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        contained_spec.limits.wall_ms = 1000;
        contained_spec.limits.memory_bytes = 64ull * 1024 * 1024;
        contained_spec.limits.pids = 4;
        contained_spec.limits.output_bytes = 4096;  // tight cap

        auto handle = backend.create(contained_spec, ctx);
        AE_CHECK(handle.has_value(), "C3 unbounded-output: create() with output_bytes=4096 succeeds");
        if (handle.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req{.language = "native", .source = hostile_child_cmd("flood")};
            auto outcome = backend.exec(*handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            AE_CHECK(outcome.has_value(), "C3 unbounded-output: contained exec() returns a result");
            if (outcome.has_value()) {
                std::cout << "  measured: unbounded-output contained, exec took " << elapsed_ms(t0, t1)
                          << " ms, captured " << outcome->stdout_text.size() << " bytes\n";
                AE_CHECK(outcome->stdout_text.size() <= 4096,
                          "C3 unbounded-output: captured stdout never exceeds the configured cap");
                AE_CHECK(!outcome->stdout_text.empty(),
                          "C3 unbounded-output: something was actually captured (the flood ran)");
            }
            backend.destroy(*handle);
        }

        // Positive control: the SAME probe with a materially LOOSER output_bytes cap (never
        // literally "disabled" -- drain_pipe_bounded's own documented safety floor forbids a truly
        // unbounded read even when SandboxSpec sets output_bytes=0, deliberately, since an
        // unbounded stdout is itself a host-safety hazard, 008 SS2 item 2) -- if the tight 4096-byte
        // cap above is real containment and not just the flood happening to produce little output,
        // loosening the cap must let materially more through.
        NativeJailBackend positive_backend;
        SandboxSpec positive_spec = contained_spec;
        positive_spec.limits.output_bytes = 2ull * 1024 * 1024;  // 2 MiB, still bounded by design
        positive_spec.limits.wall_ms = 300;  // short: only need to show it captures far more, fast
        auto positive_handle = positive_backend.create(positive_spec, ctx);
        AE_CHECK(positive_handle.has_value(), "C3 unbounded-output positive control: create() with a loose cap succeeds");
        if (positive_handle.has_value()) {
            ExecRequest req{.language = "native", .source = hostile_child_cmd("flood")};
            auto outcome = positive_backend.exec(*positive_handle, req, ctx);
            AE_CHECK(outcome.has_value(), "C3 unbounded-output positive control: exec() returns a result");
            if (outcome.has_value()) {
                std::cout << "  measured: unbounded-output positive control, captured "
                          << outcome->stdout_text.size() << " bytes\n";
                AE_CHECK(outcome->stdout_text.size() > 4096 * 4,
                          "C3 unbounded-output positive control: with a looser cap, materially more "
                          "than the tight 4096-byte cap gets through (the containment above is real, "
                          "not the flood incidentally producing little output)");
            }
            positive_backend.destroy(*positive_handle);
        }
    }

    std::filesystem::remove_all(work_dir, ec);
    std::filesystem::remove_all(secret_dir, ec);

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
