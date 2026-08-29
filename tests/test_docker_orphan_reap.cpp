// ADR-108 -- DockerCliBackend::reap_orphans(), the real positive AND negative controls, matching
// CLAUDE.md's "security claims need positive controls: a test that cannot fail proves nothing".
// Mirrors test_containerd_execution_surface.cpp's own "ADR-108: reap_orphans()" block (same four
// scenarios -- live-pid-with-matching-start-key survives, live-pid-with-WRONG-start-key (simulating a
// pid-reuse) gets reaped, dead-pid gets reaped, non-prefixed container is never touched) -- separate
// file, not appended there, since this backend is cross-platform and that one is Linux-only.
//
// REQUIRES a running Docker daemon reachable via the `docker` CLI on PATH -- same posture as
// test_sandbox_runtime.cpp/test_mandatory_sandbox_provider.cpp, not a special opt-in flag.

#include "agentengine/sandbox/docker_execution_surface.hpp"

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

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

// Spawns a real, trivial child process, waits for it to exit, and returns its now-dead pid -- a pid
// this test can PROVE is dead by the time it's used, not merely assumed to be.
[[nodiscard]] long make_confirmed_dead_pid() {
#ifdef _WIN32
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    char cmd[] = "cmd.exe /c exit 0";
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                         &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD const pid = pi.dwProcessId;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<long>(pid);
#else
    pid_t const pid = fork();
    if (pid == 0) { _exit(0); }
    int status = 0;
    waitpid(pid, &status, 0);
    return static_cast<long>(pid);
#endif
}

[[nodiscard]] long current_pid() {
#ifdef _WIN32
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(::getpid());
#endif
}

[[nodiscard]] bool docker_container_exists(std::string const& name) {
    auto r = agentengine::docker_cli_detail::run_capture("docker ps -a --format \"{{.Names}}\"");
    if (r.exit_code != 0) return false;
    std::istringstream lines(r.stdout_text);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line == name) return true;
    }
    return false;
}

[[nodiscard]] bool rename_to(std::string const& container_id, std::string const& new_name) {
    auto r = agentengine::docker_cli_detail::run_capture("docker rename " + container_id + " " +
                                                           new_name);
    return r.exit_code == 0;
}

}  // namespace

int main() {
    std::printf("=== DockerCliBackend::reap_orphans() test (ADR-108) ===\n");

    // REGRESSION CHECK for a REAL, independent-red-team-found finding (ADR-108 §5): a decimal run
    // that fits in `long` (64-bit on LP64) but exceeds what `pid_t`/`DWORD` (32-bit) can hold used to
    // silently TRUNCATE on the cast inside process_is_alive(), letting a foreign container carrying
    // such a value be misclassified "confirmed dead". No daemon needed -- pure parsing logic.
    check(!agentengine::docker_cli_detail::parse_orphan_identity("ae_des_10000000000_1_x").has_value(),
          "parse_orphan_identity() rejects a pid segment too large to fit in pid_t/DWORD without "
          "truncating");
    check(agentengine::docker_cli_detail::parse_orphan_identity("ae_des_1234_1_x").has_value(),
          "parse_orphan_identity() still accepts a normal, in-range pid segment");

    long const my_pid = current_pid();
    std::uint64_t const my_start_key = agentengine::docker_cli_detail::current_process_start_key();
    check(my_start_key != 0, "current_process_start_key() returns a real, nonzero value for THIS process");

    agentengine::DockerCliBackend backend;

    // (a) A container this test creates via the real DockerCliBackend::create() -- its embedded
    // pid/start-key are both THIS process's own, real ones -- must survive reap_orphans() untouched.
    auto alive_created = backend.create();
    check(alive_created.has_value(), "reap setup: create() a container (embeds THIS process's own "
                                      "pid + start-key)");

    // (b) A container RENAMED to embed THIS process's own real, currently-alive pid but a WRONG
    // start-key -- simulates exactly the pid-reuse scenario ADR-108 §7/§5 disclosed: a later process
    // that reused a dead process's pid. `process_is_alive(my_pid)` alone would say "alive" (it's
    // literally this test's own pid) -- proving reap_orphans() must still reap it via the start-key
    // mismatch, not the plain liveness check, is the whole point of this scenario.
    auto wrong_key_created = backend.create();
    std::string wrong_key_name;
    if (wrong_key_created.has_value()) {
        wrong_key_name = std::string(agentengine::docker_cli_detail::kOrphanNamePrefix) +
                          std::to_string(my_pid) + "_" + std::to_string(my_start_key + 1) +
                          "_reaptest_wrongkey";
        check(rename_to(wrong_key_created->container_id, wrong_key_name),
              "reap setup: docker rename to THIS process's pid with a WRONG start-key succeeds");
    }

    // (c) A container whose NAME is directly overwritten (via `docker rename`) to embed a CONFIRMED
    // dead pid -- create() itself always uses this process's own live pid, so the only way to get a
    // container carrying a dead one is to rename an existing container after the fact. The start-key
    // segment's own value doesn't matter here: process_is_alive(dead_pid) alone already resolves to
    // "gone", short-circuiting before any start-key comparison.
    long const dead_pid = make_confirmed_dead_pid();
    check(dead_pid > 0, "reap setup: make_confirmed_dead_pid() produced a real, now-exited pid");
    auto dead_created = backend.create();
    std::string dead_name;
    if (dead_created.has_value() && dead_pid > 0) {
        dead_name = std::string(agentengine::docker_cli_detail::kOrphanNamePrefix) +
                    std::to_string(dead_pid) + "_1_reaptest_dead";
        check(rename_to(dead_created->container_id, dead_name),
              "reap setup: docker rename to a confirmed-dead-pid name succeeds");
    }

    // (d) A container renamed to something that does NOT carry the ae_des_ prefix at all --
    // reap_orphans() must never touch it regardless of any pid it might coincidentally look like it
    // embeds. Name includes this process's own pid -- a fixed literal here would collide with a
    // leftover from a prior interrupted run (found by ADR-108's own red-team round).
    auto foreign_created = backend.create();
    std::string const foreign_name = "not-our-prefix-reaptest-" + std::to_string(my_pid);
    bool foreign_renamed_ok = false;
    if (foreign_created.has_value()) {
        foreign_renamed_ok = rename_to(foreign_created->container_id, foreign_name);
        check(foreign_renamed_ok, "reap setup: docker rename to a non-ae_des_-prefixed name succeeds");
    }

    if (alive_created.has_value() && !wrong_key_name.empty() && !dead_name.empty() &&
        foreign_renamed_ok) {
        auto report = backend.reap_orphans();
        check(report.has_value(), "reap_orphans() itself succeeds (docker ps works)");
        if (report.has_value()) {
            check(report->reap_failures.empty(), "reap_orphans() reports zero destroy failures");
            // The alive container's exact name has an internal counter suffix this test doesn't
            // predict; check by absence-from-reap instead: it must still exist under docker's own id.
            auto still_there = agentengine::docker_cli_detail::run_capture(
                "docker inspect " + alive_created->container_id);
            check(still_there.exit_code == 0,
                  "POSITIVE CONTROL: the matching-start-key container was NOT reaped (still "
                  "inspectable)");
            check(!docker_container_exists(wrong_key_name),
                  "PID-REUSE FIX PROOF: same live pid but a WRONG start-key WAS reaped");
            check(!docker_container_exists(dead_name), "the confirmed-dead-pid container WAS reaped");
            check(docker_container_exists(foreign_name),
                  "NEGATIVE CONTROL: the non-prefixed container was never touched");
        }
    }

    // Cleanup: destroy whichever survived reap_orphans() (the wrong-key and dead-pid ones should
    // already be gone; a `docker rm -f` on an already-gone id is the same best-effort posture
    // destroy() and reap_orphans() itself already rely on, so no special-casing needed here).
    if (alive_created.has_value()) (void)backend.destroy(*alive_created);
    if (wrong_key_created.has_value()) (void)backend.destroy(*wrong_key_created);
    if (dead_created.has_value()) (void)backend.destroy(*dead_created);
    if (foreign_created.has_value()) (void)backend.destroy(*foreign_created);

    std::printf("=== %d checks, %d failed ===\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
