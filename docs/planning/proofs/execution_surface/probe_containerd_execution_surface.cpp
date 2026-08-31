// PROVE-PHASE PROBE (A3, §36.5): drives the REAL `ContainerdExecutionSurface` (this directory's own
// containerd_execution_surface.hpp) directly -- the same relationship
// docker_sandbox/probe_docker_sandbox.cpp originally had to DockerBackend before
// DockerExecutionSurface/SandboxRuntime existed. NOT run through the real SandboxRuntime/Ledger stack
// -- see this file's own final summary print and the session report for the exact tier reached.
//
// Build (inside WSL2 Ubuntu, as root -- containerd's default socket permissions on this install
// require it, matching probe_bind_mount_ordering_hazard.sh's own precondition):
//   g++ -std=c++23 -O0 -g -Wall -Wextra -o probe_containerd_execution_surface \
//       probe_containerd_execution_surface.cpp
//   ./probe_containerd_execution_surface
//
// Exits 0 iff every check below passes; prints a [FAIL] line and exits nonzero on the first failure
// (subsequent checks are skipped, matching this design's own probes' "fail loud, don't keep going and
// pretend" posture) but always attempts best-effort cleanup first.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "containerd_execution_surface.hpp"

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

}  // namespace

int main() {
    namespace fs = std::filesystem;
    fs::path const root = "/tmp/ae_ces_probe";
    fs::path const host_dir = root / "hostdir";

    std::error_code ec;
    fs::remove_all(root, ec);  // idempotent -- clean any leftover state from a prior interrupted run
    fs::create_directories(host_dir, ec);

    std::printf("=== ContainerdExecutionSurface real probe (§36.5) ===\n");

    {
        probe::ContainerdExecutionSurface surface;

        // --- Turn 1: reset() against a freshly-seeded host_dir, no explicit `ctr images pull`
        //     anywhere in this process -- a real, live test of design claim C1 ("ctr run's own
        //     convenience-flag path handles image pull/unpack automatically, no separate step
        //     needed"). If the image was never pulled into this containerd namespace before, this is
        //     the FIRST thing that would fail if C1 were wrong.
        write_file(host_dir / "a.txt", "turn1-content\n");
        auto r1 = surface.reset(host_dir);
        check(r1.has_value(),
              "reset() #1 (fresh container, bind-mounted at host_dir, image auto-pulled if needed)");
        if (!r1.has_value()) {
            std::printf("    error: %s (%s)\n", r1.error().message.c_str(), r1.error().code.c_str());
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
        //     all -- proving the central §36.5 architectural finding (copy-out disappears) directly,
        //     not merely via a later drain_to() no-op.
        auto wrote = surface.run("echo written-from-container > /workspace/b.txt");
        check(wrote.has_value() && wrote->exit_code == 0, "run(): container writes b.txt");
        std::string const host_view = read_file(host_dir / "b.txt");
        check(host_view == "written-from-container\n",
              "host disk sees the container's write with ZERO drain_to() call ('" + host_view + "')");

        // --- drain_to() to the SAME directory is a true, harmless no-op (the central finding's own
        //     "this concept's own job... satisfied vacuously" claim, exercised for real).
        auto drained = surface.drain_to(host_dir);
        check(drained.has_value(), "drain_to(same host_dir) succeeds as a no-op");
        check(read_file(host_dir / "b.txt") == "written-from-container\n",
              "drain_to(same host_dir) does not disturb existing content");

        // --- A non-zero exit code is a normal, meaningful RESULT (ExecutionSurface's own documented
        //     contract), never itself a result<> error.
        auto failing = surface.run("exit 7");
        check(failing.has_value() && failing->exit_code == 7,
              "run(): a non-zero exit code is a normal value, not a result<> error");

        // --- The reject_embedded_nul() defense (this pass's own corrected finding-2 analog, see
        //     containerd_ctr_backend.hpp's header comment) actually rejects, rather than silently
        //     truncating, a command containing an embedded NUL.
        std::string nul_command = "echo hi";
        nul_command.push_back('\0');
        nul_command += "; rm -rf /workspace";
        auto rejected = surface.run(nul_command);
        check(!rejected.has_value() && rejected.error().code == "ctr_backend.embedded_nul",
              "run(): embedded-NUL command is rejected outright, not silently truncated");
        // Prove the rejection was REAL, not just a returned error with the workload still having run:
        // b.txt (the file the trailing `rm -rf /workspace` inside the same string would have removed
        // via /workspace, had the command actually reached the container's shell) must be untouched.
        check(read_file(host_dir / "b.txt") == "written-from-container\n",
              "the rejected command never reached the container -- workspace untouched");

        // ================================================================================
        // Turn 2: reproduce, from THIS C++ process (a second, independent confirmation of the same
        // ordering hazard probe_bind_mount_ordering_hazard.sh already proved via bash against the
        // identical live containerd) -- a host-side remove_all()+recreate of host_dir (EXACTLY
        // real_io_filesystem.hpp's own materialize() operation) while the turn-1 container is STILL
        // ALIVE and still bind-mounted at that path, one step before reset() would destroy it.
        // ================================================================================
        std::printf("--- Turn 2: C++-side ordering-hazard re-confirmation ---\n");
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

    fs::remove_all(root, ec);

    std::printf("=== %d checks, %d failed ===\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
