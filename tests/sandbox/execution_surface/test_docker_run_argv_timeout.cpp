// Closing issue #50's own design-doc residual (docs/planning/docker-execution-surface-argv-hardening-
// design-draft.md §5, Finding 1): re-runs the exact live-Docker-Desktop timeout/orphan scenario
// docker_execution_surface.hpp's own `run_capture()` header comment documents ("run_capture(\"docker
// exec <id> sh -c \"tail -f /dev/null\"\", timeout_seconds=5)" left `docker.exe` orphaned before the
// original Job-Object fix) -- but against the NEW, `cmd.exe`-free `run_argv()` path, to PROVE (not just
// reason) that removing the `cmd.exe` layer did not weaken the Job-Object timeout-kill guarantee.
//
// Windows-only: this specific historical bug (an intermediate `cmd.exe` process a plain
// `TerminateProcess` can't see past) and its fix are both Windows-specific; the POSIX `run_argv()`
// SIGKILLs the `posix_spawnp()`-spawned `docker` process directly, with no intermediate-process class
// of bug to reproduce here.
//
// REQUIRES a running Docker daemon reachable via `docker` on PATH -- same posture as
// test_sandbox_runtime.cpp/test_docker_orphan_reap.cpp, not a special opt-in flag.
#ifdef _WIN32

#include "agentengine/sandbox/docker_execution_surface.hpp"

#include <chrono>
#include <cstdio>
#include <string>

#include <tlhelp32.h>
#include <windows.h>

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

// Counts live `docker.exe`/`com.docker.cli.exe` processes on this host -- a real, observable proxy for
// "did the Job Object kill actually reach the spawned process," matching this repo's own established
// preference for a positive, empirically-checkable control over a purely reasoned-about claim.
[[nodiscard]] int count_docker_cli_processes() {
    int count = 0;
    HANDLE const snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            std::string const name(pe.szExeFile);
            if (name == "docker.exe" || name == "com.docker.cli.exe") ++count;
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return count;
}

}  // namespace

int main() {
    std::printf("=== run_argv() timeout-kill leaves no orphaned docker.exe (no cmd.exe layer) ===\n");

    int const before = count_docker_cli_processes();
    check(before >= 0, "CreateToolhelp32Snapshot() succeeds");

    agentengine::DockerCliBackend docker;
    auto inst = docker.create("alpine:latest");
    check(inst.has_value(), "create() succeeds");
    if (!inst.has_value()) {
        std::printf("=== %d checks, %d failed ===\n", g_checks, g_failed);
        return 1;
    }

    // Directly calls run_argv() (not exec(), which hardcodes the real 30s default) with a short
    // timeout against a genuinely non-terminating command -- the same "tail -f /dev/null" shape the
    // original bug report used.
    auto const t0 = std::chrono::steady_clock::now();
    auto const outcome = agentengine::docker_cli_detail::run_argv(
        {"docker", "exec", inst->container_id, "sh", "-c", "tail -f /dev/null"},
        /*timeout_seconds=*/3);
    auto const elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
    std::printf("[info] run_argv() returned after %lld ms (exit_code=%d)\n",
                static_cast<long long>(elapsed_ms), outcome.exit_code);

    // Well under the real 30s default timeout -- proves the 3s Job-Object kill actually fired, not
    // that the call happened to finish naturally or hang.
    check(elapsed_ms < 15000, "run_argv() returned promptly, proving the Job-Object timeout kill fired");
    check(outcome.exit_code == -1, "timed-out run_argv() reports exit_code == -1");

    // Give Windows a moment to finish tearing down the killed process tree before re-snapshotting.
    Sleep(500);
    int const after_timeout = count_docker_cli_processes();
    std::printf("[info] docker.exe-family processes: before=%d, after timeout-kill=%d\n", before,
                after_timeout);
    check(after_timeout <= before,
          "no extra orphaned docker.exe-family process left behind by the timeout kill");

    auto const destroyed = docker.destroy(*inst);
    check(destroyed.has_value(), "destroy() cleanup succeeds");

    std::printf("=== %d checks, %d failed ===\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#else

int main() {
    return 0;  // Windows-only test -- see this file's own top comment for why.
}

#endif
