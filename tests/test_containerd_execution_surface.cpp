// ADR-145 (decisions/ADR-145-containerd-execution-surface-promotion.md) -- drives the real, production
// `agentengine::ContainerdExecutionSurface` (include/agentengine/sandbox/containerd_execution_surface.hpp)
// directly. Ported from docs/planning/proofs/execution_surface/probe_containerd_execution_surface.cpp
// (the design-and-prove-phase original, 16/16 checks against a live containerd/runc deployment) --
// same checks, against the real production header instead of the `probe::` prove-phase copy.
//
// REQUIRES: Linux, a running containerd daemon reachable via the `ctr` CLI on PATH, and (on most
// real installs, including this project's own WSL2 development environment) either root or an
// unprivileged-containerd-socket ACL -- containerd's default socket permissions (`/run/containerd/
// containerd.sock`) require it. Matches DockerExecutionSurface's own "test just fails cleanly if the
// daemon isn't reachable" posture (decisions/ADR-104-real-io-filesystem-linux-parity.md,
// decisions/ADR-105-sandbox-tool-provider-composed-linux-parity.md) rather than a special opt-in
// CMake flag -- `ctr`/containerd is a comparably light dependency to `docker`, not the much heavier
// KVM/Kata-Containers deployment `test_kata_backend_linux` is gated behind.
//
// Exits 0 iff every check below passes; prints a [FAIL] line and exits nonzero on the first failure
// (subsequent checks are skipped, matching this design's own probes' "fail loud, don't keep going and
// pretend" posture) but always attempts best-effort cleanup first.

#include "agentengine/sandbox/containerd_execution_surface.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

std::string read_file(std::filesystem::path const& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "<no such file>";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_file(std::filesystem::path const& p, std::string const& content) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
}

// Forks a real child that exits immediately and reaps it -- a pid this test can PROVE is dead by the
// time it's used, not merely assumed to be. Deterministic on Linux's own default pid-reuse posture
// (a large, monotonically-advancing pid space); the tiny residual pid-reuse race this shares with
// `process_is_alive()` itself is the same inherent limitation that function's own header comment
// already discloses, not a new one this test introduces.
[[nodiscard]] long make_confirmed_dead_pid() {
    pid_t const pid = fork();
    if (pid == 0) { _exit(0); }
    int status = 0;
    waitpid(pid, &status, 0);
    return static_cast<long>(pid);
}

[[nodiscard]] bool ctr_container_exists(std::string const& id) {
    auto r = agentengine::ctr_cli_detail::run_argv({"ctr", "containers", "list", "-q"});
    if (!r.has_value()) return false;
    std::istringstream lines(r->stdout_text);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line == id) return true;
    }
    return false;
}

}  // namespace

int main() {
    // REGRESSION CHECK for a REAL, independent-red-team-found finding (ADR-108 §5): a decimal run
    // that fits in `long` (64-bit on LP64) but exceeds what `pid_t` (32-bit) can hold used to silently
    // TRUNCATE on the cast inside process_is_alive(), letting a foreign container carrying such a
    // value be misclassified "confirmed dead". No daemon needed -- pure parsing logic.
    check(!agentengine::ctr_cli_detail::parse_orphan_identity("ae_ces_10000000000_1_x").has_value(),
          "parse_orphan_identity() rejects a pid segment too large to fit in pid_t without truncating");
    check(agentengine::ctr_cli_detail::parse_orphan_identity("ae_ces_1234_1_x").has_value(),
          "parse_orphan_identity() still accepts a normal, in-range pid segment");

    namespace fs = std::filesystem;
    fs::path const root = "/tmp/ae_containerd_execution_surface_test";
    fs::path const host_dir = root / "hostdir";

    std::error_code ec;
    fs::remove_all(root, ec);  // idempotent -- clean any leftover state from a prior interrupted run
    fs::create_directories(host_dir, ec);

    std::printf("=== ContainerdExecutionSurface production test (ADR-145) ===\n");

    {
        agentengine::ContainerdExecutionSurface surface;

        // --- Turn 1: reset() against a freshly-seeded host_dir, no explicit `ctr images pull`
        //     anywhere in this process -- a real, live test of "ctr run's own convenience-flag path
        //     handles image pull/unpack automatically, no separate step needed."
        write_file(host_dir / "a.txt", "turn1-content\n");
        auto r1 = surface.reset(host_dir);
        check(r1.has_value(),
              "reset() #1 (fresh container, bind-mounted at host_dir, image auto-pulled if needed)");
        if (!r1.has_value()) {
            std::printf("    error: %s (%s)\n", r1.error().message.c_str(), r1.error().code.c_str());
            std::printf("    (a common cause: containerd's socket is not reachable by this process --\n"
                        "     this test REQUIRES root or an unprivileged containerd-socket ACL)\n");
            return 1;
        }

        // --- The container sees turn-1 content via the LIVE bind mount (no copy_to_container step
        //     exists in this conformer at all -- if this passes, the bind mount is real and live).
        auto seen = surface.run("cat /workspace/a.txt");
        check(seen.has_value() && seen->exit_code == 0, "run(): container reads turn-1 content");
        if (seen.has_value()) {
            check(seen->stdout_text == "turn1-content\n",
                  "run(): container's view of a.txt matches exactly what host_dir held ('" +
                      seen->stdout_text + "')");
        }

        // --- A write FROM the container round-trips onto real host disk with NO drain_to() call at
        //     all -- proving the central architectural finding (copy-out disappears) directly.
        auto wrote = surface.run("echo written-from-container > /workspace/b.txt");
        check(wrote.has_value() && wrote->exit_code == 0, "run(): container writes b.txt");
        std::string const host_view = read_file(host_dir / "b.txt");
        check(host_view == "written-from-container\n",
              "host disk sees the container's write with ZERO drain_to() call ('" + host_view + "')");

        // --- drain_to() to the SAME directory is a true, harmless no-op.
        auto drained = surface.drain_to(host_dir);
        check(drained.has_value(), "drain_to(same host_dir) succeeds as a no-op");
        check(read_file(host_dir / "b.txt") == "written-from-container\n",
              "drain_to(same host_dir) does not disturb existing content");

        // --- drain_to() to a DIFFERENT directory falls back to a real recursive host-side copy --
        //     the general ExecutionSurface contract this conformer's own header comment names as not
        //     exercised by SandboxRuntime::run() itself, verified for real here.
        fs::path const other_dir = root / "other";
        auto drained_elsewhere = surface.drain_to(other_dir);
        check(drained_elsewhere.has_value(), "drain_to(a DIFFERENT host_dir) succeeds via host-side copy");
        check(read_file(other_dir / "b.txt") == "written-from-container\n",
              "drain_to(a DIFFERENT host_dir) actually copies the current content there");

        // --- A non-zero exit code is a normal, meaningful RESULT (ExecutionSurface's own documented
        //     contract), never itself a result<> error.
        auto failing = surface.run("exit 7");
        check(failing.has_value() && failing->exit_code == 7,
              "run(): a non-zero exit code is a normal value, not a result<> error");

        // --- reject_embedded_nul() actually rejects, rather than silently truncating, a command
        //     containing an embedded NUL.
        std::string nul_command = "echo hi";
        nul_command.push_back('\0');
        nul_command += "; rm -rf /workspace";
        auto rejected = surface.run(nul_command);
        check(!rejected.has_value() && rejected.error().code == "ctr_cli_backend.embedded_nul",
              "run(): embedded-NUL command is rejected outright, not silently truncated");
        // Prove the rejection was REAL, not just a returned error with the workload still having run.
        check(read_file(host_dir / "b.txt") == "written-from-container\n",
              "the rejected command never reached the container -- workspace untouched");

        // ================================================================================
        // Turn 2: reproduce, from THIS production code path, the bind-mount ordering hazard the
        // design's own prove-phase already established empirically (docs/planning/oci-execution-
        // surface-design-draft.md §4 finding 1) -- a host-side remove_all()+recreate of host_dir
        // (EXACTLY real_io_filesystem.hpp's own materialize() operation) while the turn-1 container is
        // STILL ALIVE and still bind-mounted at that path, one step before reset() would destroy it.
        // ================================================================================
        std::printf("--- Turn 2: ordering-hazard re-confirmation against the production header ---\n");
        fs::remove_all(host_dir, ec);
        check(!ec, "host-side remove_all(host_dir) succeeds while turn-1 container is still running");
        fs::create_directories(host_dir, ec);
        check(!ec, "host-side create_directories(host_dir) succeeds immediately after");
        write_file(host_dir / "c.txt", "turn2-content\n");

        // reset() now destroys the still-alive turn-1 container and starts a fresh one at the SAME,
        // now-recreated path.
        auto r2 = surface.reset(host_dir);
        check(r2.has_value(), "reset() #2 (destroy old, still-alive container; create fresh one at "
                               "the recreated path)");
        if (r2.has_value()) {
            auto turn2_view = surface.run("ls /workspace; cat /workspace/c.txt 2>&1");
            check(turn2_view.has_value() && turn2_view->exit_code == 0,
                  "fresh turn-2 container starts and reads turn-2 content");
            if (turn2_view.has_value()) {
                bool const no_leak_from_turn1 = turn2_view->stdout_text.find("a.txt") == std::string::npos &&
                                                 turn2_view->stdout_text.find("b.txt") == std::string::npos;
                check(no_leak_from_turn1,
                      "fresh container sees NO turn-1 files (a.txt/b.txt) -- no leakage across the "
                      "materialize-before-reset ordering hazard ('" + turn2_view->stdout_text + "')");
                check(turn2_view->stdout_text.find("turn2-content") != std::string::npos,
                      "fresh container sees ONLY turn-2 content");
            }
        }

        // Destructor runs here -- destroys the turn-2 instance for real.
    }

    // ================================================================================
    // ADR-108: ContainerdCliBackend::reap_orphans() -- real positive AND negative controls, matching
    // CLAUDE.md's "security claims need positive controls: a test that cannot fail proves nothing".
    // Every container this block creates directly via ContainerdCliBackend (not through
    // ContainerdExecutionSurface -- reap_orphans() only reads containerd's own container table, it
    // never needs a live ExecutionSurface instance) so the embedded pid/start-key can be fully
    // controlled.
    // ================================================================================
    std::printf("--- ADR-108: reap_orphans() positive/negative controls ---\n");
    {
        fs::path const reap_host_dir = root / "reap_hostdir";
        fs::create_directories(reap_host_dir, ec);
        agentengine::ContainerdCliBackend backend;

        long const my_pid = static_cast<long>(::getpid());
        std::uint64_t const my_start_key = agentengine::ctr_cli_detail::current_process_start_key();
        check(my_start_key != 0,
              "current_process_start_key() returns a real, nonzero value for THIS process");

        // (a) A container whose embedded pid/start-key are BOTH THIS TEST PROCESS's own, real ones --
        // unambiguously alive for the whole duration of this block -- must survive reap_orphans()
        // untouched.
        std::string const alive_id = "ae_ces_" + std::to_string(my_pid) + "_" +
                                      std::to_string(my_start_key) + "_reaptest_alive";
        auto alive_created = backend.create(alive_id, reap_host_dir);
        check(alive_created.has_value(),
              "reap setup: create() a container with THIS process's own pid + start-key");

        // (b) A container whose embedded pid is THIS process's own (alive) but whose start-key is
        // WRONG -- simulates exactly the pid-reuse scenario ADR-108 §7/§5 disclosed: a later process
        // that reused a dead process's pid. Plain process_is_alive(my_pid) alone would say "alive"
        // (it's literally this test's own pid) -- proving reap_orphans() must still reap it via the
        // start-key mismatch is the whole point of this scenario.
        std::string const wrong_key_id = "ae_ces_" + std::to_string(my_pid) + "_" +
                                          std::to_string(my_start_key + 1) + "_reaptest_wrongkey";
        auto wrong_key_created = backend.create(wrong_key_id, reap_host_dir);
        check(wrong_key_created.has_value(),
              "reap setup: create() a container with THIS process's pid but a WRONG start-key");

        // (c) A container whose embedded pid is CONFIRMED dead -- the real target reap_orphans() must
        // find and destroy. The start-key segment's own value doesn't matter here:
        // process_is_alive(dead_pid) alone already resolves to "gone", short-circuiting before any
        // start-key comparison.
        long const dead_pid = make_confirmed_dead_pid();
        std::string const dead_id = "ae_ces_" + std::to_string(dead_pid) + "_1_reaptest_dead";
        auto dead_created = backend.create(dead_id, reap_host_dir);
        check(dead_created.has_value(), "reap setup: create() a container with a confirmed-dead pid");

        // (d) A container that does NOT carry the ae_ces_ prefix at all -- reap_orphans() must never
        // touch it regardless of any pid it might coincidentally look like it embeds. Name includes
        // this process's own pid -- a fixed literal here would collide with a leftover from a prior
        // interrupted run (found by ADR-108's own red-team round).
        std::string const foreign_id = "not-our-prefix-reaptest-" + std::to_string(my_pid);
        auto foreign_created = backend.create(foreign_id, reap_host_dir);
        check(foreign_created.has_value(), "reap setup: create() a non-ae_ces_-prefixed container");

        if (alive_created.has_value() && wrong_key_created.has_value() && dead_created.has_value() &&
            foreign_created.has_value()) {
            auto report = backend.reap_orphans();
            check(report.has_value(), "reap_orphans() itself succeeds (ctr containers list works)");
            if (report.has_value()) {
                check(report->reap_failures.empty(),
                      "reap_orphans() reports zero destroy failures");
                check(ctr_container_exists(alive_id),
                      "POSITIVE CONTROL: the matching-start-key container was NOT reaped");
                check(!ctr_container_exists(wrong_key_id),
                      "PID-REUSE FIX PROOF: same live pid but a WRONG start-key WAS reaped");
                check(!ctr_container_exists(dead_id),
                      "the confirmed-dead-pid container WAS reaped");
                check(ctr_container_exists(foreign_id),
                      "NEGATIVE CONTROL: the non-prefixed container was never touched");
            }
        }

        // Cleanup: destroy whichever survived reap_orphans() (the wrong-key and dead-pid ones should
        // already be gone; destroy() on an already-gone container is the same best-effort posture
        // this class's own destructor already relies on).
        (void)backend.destroy(agentengine::ContainerdCliBackend::Instance{alive_id});
        (void)backend.destroy(agentengine::ContainerdCliBackend::Instance{wrong_key_id});
        (void)backend.destroy(agentengine::ContainerdCliBackend::Instance{dead_id});
        (void)backend.destroy(agentengine::ContainerdCliBackend::Instance{foreign_id});
    }

    fs::remove_all(root, ec);

    std::printf("=== %d checks, %d failed ===\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
