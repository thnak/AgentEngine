// Milestone 2 Phase C, task C5 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// 008-Sandbox-and-Isolation.md SS9 G3 -- "a probe guest enumerating filesystem, network, env, and
// processes finds exactly the granted set and nothing else, on each backend." This is the Linux
// half against the real LinuxNativeJailBackend (C2).
//
// Filesystem is covered in a DEDICATED file, not here: test_native_jail_fs_containment_linux.cpp.
// Process enumeration below (axis 3) used to share its root cause with that gap (no /proc remount
// inside the new pid namespace) -- both are now closed together by the same
// LinuxNativeJailBackend::setup_jail() (008 SS9 G2/G3, pivot_root + a fresh, namespace-local
// /proc). Every SandboxSpec below now needs an explicit toolchain grant
// (helpers/native_jail_linux_toolchain_mounts.hpp) for `/bin/sh` itself to be reachable inside
// that jail.
//
// Requires a delegated cgroup v2 root and CAP_SYS_ADMIN, same as C2/C3's Linux tests -- run via
// tests/helpers/cgroup_v2_test_setup.sh.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

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

// fork()+exec() of the hostile-child probe binary with its args, capturing stdout after a fixed
// grace period -- an ORDINARY host process, no namespaces/cgroups/seccomp at all
// (sandbox_profile::none, 008 SS1). This is the positive-control baseline every axis below
// compares its contained result against.
std::string run_raw_unsandboxed(std::vector<std::string> const& args, int grace_ms) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return {};

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(AE_HOSTILE_CHILD_POSIX_EXE));
        for (auto const& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        // Deliberately execv (not execve with a scrubbed envp): this helper's whole point is "what
        // an ORDINARY unsandboxed child inherits," i.e. this test process's own environ, canary
        // included -- contrast with LinuxNativeJailBackend's exec(), which uses a fixed minimal
        // envp and is what the env axis below proves.
        execv(AE_HOSTILE_CHILD_POSIX_EXE, argv.data());
        _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0) {
        close(pipefd[0]);
        return {};
    }

    usleep(static_cast<useconds_t>(grace_ms) * 1000);

    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    close(pipefd[0]);

    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    return out;
}

// Ephemeral loopback TCP listener with a real backlog -- a connect() from anywhere that can
// actually reach 127.0.0.1 completes the handshake against the backlog, so no accept()-side thread
// is needed for the positive control to succeed.
int make_loopback_listener(int& port_out) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return -1;
    if (listen(s, 4) != 0) return -1;
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len);
    port_out = ntohs(bound.sin_port);
    return s;
}

}  // namespace

int main() {
    std::filesystem::path work_dir =
        std::filesystem::temp_directory_path() / "ae_native_jail_ambient_authority_test_linux";
    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);
    AE_CHECK(!ec, "setup: scratch work_dir exists");

    EffectContext ctx;

    // ---- Axis 1: env (008 SS9 G3) -- a host secret must not reach the guest ----------------------
    {
        std::string const kCanaryName = "AE_C5_HOST_SECRET_CANARY";
        std::string const kCanaryValue = "leak_canary_should_never_reach_guest_12345";
        setenv(kCanaryName.c_str(), kCanaryValue.c_str(), 1);

        LinuxNativeJailBackend backend;
        SandboxSpec spec;
        spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        agentengine::native_jail::test::add_shell_toolchain_mounts(spec);
        spec.limits.wall_ms = 5000;
        spec.limits.memory_bytes = 32ull * 1024 * 1024;
        spec.limits.pids = 4;
        spec.limits.output_bytes = 64 * 1024;

        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), "C5-Linux env: create() succeeds");
        if (handle.has_value()) {
            ExecRequest req{.language = "native", .source = hostile_child_cmd("probe_env")};
            auto outcome = backend.exec(*handle, req, ctx);
            AE_CHECK(outcome.has_value(), "C5-Linux env: contained exec() returns a result");
            if (outcome.has_value()) {
                bool leaked = outcome->stdout_text.find(kCanaryName) != std::string::npos;
                std::cout << "  measured: guest env dump does " << (leaked ? "" : "NOT ")
                          << "contain the host canary\n";
                AE_CHECK(!leaked,
                          "C5-Linux env: the guest's environment does not contain a secret only "
                          "the launching host process set (008 SS9 G3, no ambient authority; "
                          "LinuxNativeJailBackend's fixed envp already made this true)");
            }
            backend.destroy(*handle);
        }

        // Positive control: same canary, same probe, unsandboxed -- proves the canary-detection
        // mechanism itself is real.
        std::string raw_out = run_raw_unsandboxed({"probe_env"}, 300);
        bool raw_leaked = raw_out.find(kCanaryName) != std::string::npos;
        std::cout << "  measured: unsandboxed positive control env dump does "
                  << (raw_leaked ? "" : "NOT ") << "contain the host canary\n";
        AE_CHECK(raw_leaked,
                  "C5-Linux env positive control: an ordinary unsandboxed child DOES inherit the "
                  "host canary (the containment above is real, not vacuous)");
    }

    // ---- Axis 2: network (008 SS9 G3) -- CLONE_NEWNET leaves no route, not even to loopback ------
    {
        int port = 0;
        int listener = make_loopback_listener(port);
        AE_CHECK(listener >= 0, "C5-Linux net: setup loopback listener");
        std::cout << "  setup: loopback listener on 127.0.0.1:" << port << "\n";

        LinuxNativeJailBackend backend;
        SandboxSpec spec;
        spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        agentengine::native_jail::test::add_shell_toolchain_mounts(spec);
        spec.limits.wall_ms = 5000;
        spec.limits.memory_bytes = 32ull * 1024 * 1024;
        spec.limits.pids = 4;
        spec.limits.output_bytes = 4096;

        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), "C5-Linux net: create() succeeds");
        if (handle.has_value()) {
            ExecRequest req{.language = "native",
                             .source = hostile_child_cmd("probe_net " + std::to_string(port))};
            auto outcome = backend.exec(*handle, req, ctx);
            AE_CHECK(outcome.has_value(), "C5-Linux net: contained exec() returns a result");
            if (outcome.has_value()) {
                std::cout << "  measured: contained probe_net stdout=" << outcome->stdout_text;
                AE_CHECK(outcome->stdout_text.find("NET_DENIED") != std::string::npos,
                          "C5-Linux net: the guest's fresh CLONE_NEWNET network namespace has no "
                          "configured interface (not even loopback brought up), so it cannot reach "
                          "a host loopback listener (008 SS9 G3)");
            }
            backend.destroy(*handle);
        }

        // Positive control: same probe, unsandboxed -- must actually connect.
        std::string raw_out = run_raw_unsandboxed({"probe_net", std::to_string(port)}, 300);
        std::cout << "  measured: unsandboxed positive control probe_net stdout=" << raw_out;
        AE_CHECK(raw_out.find("NET_OK") != std::string::npos,
                  "C5-Linux net positive control: an ordinary unsandboxed child DOES reach the "
                  "same loopback listener (the containment above is real, not vacuous)");

        close(listener);
    }

    // ---- Axis 3: process enumeration (008 SS9 G3) -- CLOSED by setup_jail()'s fresh /proc mount ---
    // CLONE_NEWPID puts the guest in a fresh pid namespace; setup_jail() now additionally mounts a
    // FRESH procfs inside it (after pivot_root, from a process already inside that namespace), so
    // /proc/[pid] entries are namespace-local by construction, not the host's real process list.
    // This axis used to be a known gap sharing its root cause with the fs-escape gap -- both closed
    // together, matching the Windows half's own CreateToolhelp32Snapshot restriction (008 SS9 G3).
    {
        pid_t canary_pid = fork();
        if (canary_pid == 0) {
            execl(AE_HOSTILE_CHILD_POSIX_EXE, AE_HOSTILE_CHILD_POSIX_EXE, "sleep", "4000", nullptr);
            _exit(127);
        }
        AE_CHECK(canary_pid > 0, "C5-Linux proc: setup host canary process");
        std::cout << "  setup: host canary process pid=" << canary_pid << "\n";

        LinuxNativeJailBackend backend;
        SandboxSpec spec;
        spec.mounts.push_back(
            MountSpec{.source = work_dir.string(), .guest_path = "/work", .read_write = true});
        agentengine::native_jail::test::add_shell_toolchain_mounts(spec);
        spec.limits.wall_ms = 3000;
        spec.limits.memory_bytes = 32ull * 1024 * 1024;
        spec.limits.pids = 4;
        spec.limits.output_bytes = 4096;

        auto handle = backend.create(spec, ctx);
        AE_CHECK(handle.has_value(), "C5-Linux proc: create() succeeds");
        if (handle.has_value()) {
            ExecRequest req{
                .language = "native",
                .source = hostile_child_cmd("probe_proc " + std::to_string(canary_pid))};
            auto outcome = backend.exec(*handle, req, ctx);
            AE_CHECK(outcome.has_value(), "C5-Linux proc: contained exec() returns a result");
            if (outcome.has_value()) {
                std::cout << "  measured: contained probe_proc stdout=" << outcome->stdout_text;
                bool visible = outcome->stdout_text.find("target=yes") != std::string::npos;
                AE_CHECK(!visible,
                          "C5-Linux proc: the guest's /proc is a fresh, namespace-local mount (not "
                          "the host's unremounted procfs instance), so it cannot enumerate a host "
                          "PID it was never granted (008 SS9 G3)");
                bool self_visible = outcome->stdout_text.find("PROC_VISIBLE total=") != std::string::npos &&
                                     outcome->stdout_text.find("total=0") == std::string::npos;
                AE_CHECK(self_visible,
                          "C5-Linux proc positive control: the guest's own /proc is NOT empty (it "
                          "sees at least itself) -- proves the containment above is real "
                          "namespace-local scoping, not /proc failing to mount at all");
            }
            backend.destroy(*handle);
        }

        kill(canary_pid, SIGKILL);
        int status = 0;
        waitpid(canary_pid, &status, 0);
    }

    std::filesystem::remove_all(work_dir, ec);

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
