// Milestone 2 Phase C, task C5 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// 008-Sandbox-and-Isolation.md SS9 G3 -- "a probe guest enumerating filesystem, network, env, and
// processes finds exactly the granted set and nothing else, on each backend." This is the Windows
// half against the real NativeJailBackend (C2). Filesystem is deliberately NOT retested here --
// C3's fs-escape-attempt case (test_native_jail_abuse_corpus_windows.cpp) already is this axis's
// G3 proof for Windows, positive control and all; duplicating it here would just be the same
// measurement twice.
//
// Each axis follows the same shape as C3's corpus: a contained probe, then a positive control that
// proves the containment is real (not vacuous, not an artifact of the probe itself being broken).
//
// Real child processes under real AppContainer + Job Object isolation -- bounded by this test's
// own generous-but-finite timeouts (CLAUDE.md Machine Safety).

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

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

// Same shape as test_native_jail_abuse_corpus_windows.cpp's own helper (duplicated rather than
// shared -- each abuse/probe test file is a standalone translation unit in this project's existing
// pattern): launches `cmdline` as an ORDINARY host process, no AppContainer/Job Object/nested-exec
// policy at all (sandbox_profile::none, 008 SS1) -- the positive-control baseline every axis below
// compares its contained result against.
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
    // lpEnvironment=nullptr here is deliberate and correct -- this helper's whole point is "what an
    // ORDINARY unsandboxed process inherits," i.e. this test process's own environment, canary
    // included. Contrast with native_jail_backend.cpp's exec(), which this file's env axis exists
    // to prove no longer does that.
    BOOL created = CreateProcessA(nullptr, mutable_cmdline.data(), nullptr, nullptr,
                                   /*bInheritHandles=*/TRUE, 0, nullptr, nullptr, &si, &pi);
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

// Binds an ephemeral loopback TCP listener with a real backlog -- a connect() from anywhere that
// can actually reach 127.0.0.1 completes the handshake against the backlog without this test ever
// calling accept(), so no background thread is needed to make the positive control succeed.
SOCKET make_loopback_listener(int& port_out) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int bind_rc = bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    (void)bind_rc;
    int listen_rc = listen(s, 4);
    (void)listen_rc;
    sockaddr_in bound{};
    int len = sizeof(bound);
    getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len);
    port_out = ntohs(bound.sin_port);
    return s;
}

}  // namespace

int main() {
    disable_crt_assert_dialog();

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    std::filesystem::path work_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_ambient_authority_test";
    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);
    AE_CHECK(!ec, "setup: scratch work_dir exists");

    EffectContext ctx;

    // ---- Axis 1: env (008 SS9 G3) -- a host secret must not reach the guest ----------------------
    {
        std::string const kCanaryName = "AE_C5_HOST_SECRET_CANARY";
        std::string const kCanaryValue = "leak_canary_should_never_reach_guest_12345";
        SetEnvironmentVariableA(kCanaryName.c_str(), kCanaryValue.c_str());

        NativeJailBackend backend;
        SandboxSpec spec;
        spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        spec.limits.wall_ms = 3000;
        spec.limits.memory_bytes = 32ull * 1024 * 1024;
        spec.limits.pids = 4;
        spec.limits.output_bytes = 64 * 1024;

        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), "C5 env: create() succeeds");
        if (handle.has_value()) {
            ExecRequest req{.language = "native", .source = hostile_child_cmd("probe_env")};
            auto outcome = backend.exec(*handle, req, ctx);
            AE_CHECK(outcome.has_value(), "C5 env: contained exec() returns a result");
            if (outcome.has_value()) {
                bool leaked = outcome->stdout_text.find(kCanaryName) != std::string::npos;
                std::cout << "  measured: guest env dump does " << (leaked ? "" : "NOT ")
                          << "contain the host canary\n";
                AE_CHECK(!leaked,
                          "C5 env: the guest's environment does not contain a secret only the "
                          "launching host process set (008 SS9 G3, no ambient authority)");
            }
            backend.destroy(*handle);
        }

        // Positive control: the SAME canary, with the SAME probe run unsandboxed -- proves the
        // canary-detection mechanism is real (the probe and the canary both work), so "not found"
        // above is genuine containment, not e.g. the probe or the env var itself being broken.
        std::string raw_out = run_raw_unsandboxed(hostile_child_cmd("probe_env"), 500);
        bool raw_leaked = raw_out.find(kCanaryName) != std::string::npos;
        std::cout << "  measured: unsandboxed positive control env dump does "
                  << (raw_leaked ? "" : "NOT ") << "contain the host canary\n";
        AE_CHECK(raw_leaked,
                  "C5 env positive control: an ordinary unsandboxed child DOES inherit the host "
                  "canary (the containment above is real, not vacuous)");
    }

    // ---- Axis 2: network (008 SS9 G3) -- no capability granted means no reachable loopback -------
    {
        int port = 0;
        SOCKET listener = make_loopback_listener(port);
        AE_CHECK(listener != INVALID_SOCKET, "C5 net: setup loopback listener");
        std::cout << "  setup: loopback listener on 127.0.0.1:" << port << "\n";

        NativeJailBackend backend;
        SandboxSpec spec;
        spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        spec.limits.wall_ms = 5000;
        spec.limits.memory_bytes = 32ull * 1024 * 1024;
        spec.limits.pids = 4;
        spec.limits.output_bytes = 4096;

        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), "C5 net: create() succeeds");
        if (handle.has_value()) {
            ExecRequest req{.language = "native",
                             .source = hostile_child_cmd("probe_net " + std::to_string(port))};
            auto outcome = backend.exec(*handle, req, ctx);
            AE_CHECK(outcome.has_value(), "C5 net: contained exec() returns a result");
            if (outcome.has_value()) {
                std::cout << "  measured: contained probe_net stdout=" << outcome->stdout_text;
                AE_CHECK(outcome->stdout_text.find("NET_DENIED") != std::string::npos,
                          "C5 net: the guest (zero granted AppContainer capabilities -- no "
                          "internetClient) cannot reach even a host loopback listener (008 SS9 G3)");
            }
            backend.destroy(*handle);
        }

        // Positive control: same probe, unsandboxed -- must actually connect, proving the listener
        // and the probe both work and the DENIED result above is real containment.
        std::string raw_out =
            run_raw_unsandboxed(hostile_child_cmd("probe_net " + std::to_string(port)), 500);
        std::cout << "  measured: unsandboxed positive control probe_net stdout=" << raw_out;
        AE_CHECK(raw_out.find("NET_OK") != std::string::npos,
                  "C5 net positive control: an ordinary unsandboxed child DOES reach the same "
                  "loopback listener (the containment above is real, not vacuous)");

        closesocket(listener);
    }

    // ---- Axis 3: process enumeration (008 SS9 G3) -- contained (measured, not assumed) -----------
    // Windows has no per-process "process namespace" primitive analogous to Linux's CLONE_NEWPID, so
    // this axis was measured rather than assumed. MEASURED FINDING: the AppContainer low-privilege
    // token DOES restrict what CreateToolhelp32Snapshot returns, not just object-handle access -- a
    // guest in this backend enumerates only a handful of its own processes, never a host PID it was
    // not granted.
    {
        std::string canary_cmd = hostile_child_cmd("sleep 4000");
        std::vector<char> mutable_cmd(canary_cmd.begin(), canary_cmd.end());
        mutable_cmd.push_back('\0');
        STARTUPINFOA csi{};
        csi.cb = sizeof(csi);
        PROCESS_INFORMATION cpi{};
        BOOL canary_created =
            CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &csi, &cpi);
        AE_CHECK(canary_created, "C5 proc: setup host canary process");
        DWORD canary_pid = cpi.dwProcessId;
        std::cout << "  setup: host canary process pid=" << canary_pid << "\n";

        NativeJailBackend backend;
        SandboxSpec spec;
        spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        spec.limits.wall_ms = 3000;
        spec.limits.memory_bytes = 32ull * 1024 * 1024;
        spec.limits.pids = 4;
        spec.limits.output_bytes = 4096;

        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), "C5 proc: create() succeeds");
        if (handle.has_value()) {
            ExecRequest req{.language = "native",
                             .source = hostile_child_cmd("probe_proc " + std::to_string(canary_pid))};
            auto outcome = backend.exec(*handle, req, ctx);
            AE_CHECK(outcome.has_value(), "C5 proc: contained exec() returns a result");
            if (outcome.has_value()) {
                std::cout << "  measured: contained probe_proc stdout=" << outcome->stdout_text;
                bool visible = outcome->stdout_text.find("target=yes") != std::string::npos;
                // MEASURED FINDING (this pass, contrary to this test's original assumption): the
                // AppContainer token DOES restrict CreateToolhelp32Snapshot's results -- the guest
                // enumerates only a handful of processes (its own, measured at 3) and the host
                // canary is NOT among them. Real containment, not the known-gap-style finding this
                // file originally expected to document.
                AE_CHECK(!visible,
                          "C5 proc: the guest's process enumeration does not include a host process "
                          "it was never granted (008 SS9 G3 -- AppContainer restricts "
                          "CreateToolhelp32Snapshot, not just object access)");
            }
            backend.destroy(*handle);
        }

        // Positive control: same probe, unsandboxed -- proves the PID-detection mechanism itself is
        // real (the canary and the probe both work), so "not visible" above is genuine containment.
        std::string raw_out = run_raw_unsandboxed(
            hostile_child_cmd("probe_proc " + std::to_string(canary_pid)), 500);
        std::cout << "  measured: unsandboxed positive control probe_proc stdout=" << raw_out;
        AE_CHECK(raw_out.find("target=yes") != std::string::npos,
                  "C5 proc positive control: an ordinary unsandboxed prober DOES see the host "
                  "canary's pid (the containment above is real, not vacuous)");

        TerminateProcess(cpi.hProcess, 0);
        CloseHandle(cpi.hProcess);
        CloseHandle(cpi.hThread);
    }

    std::filesystem::remove_all(work_dir, ec);
    WSACleanup();

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
