// ADR-209 build step 4 (GitHub issue #146) -- `DockerPersistentShellSurface` against a REAL Docker daemon.
// REQUIRES a running Docker daemon reachable via `docker` on PATH and the `alpine:latest` image (pulled if
// absent); excluded from the Windows CI legs like every other daemon test (ci.yml).
//
// Machine safety (CLAUDE.md): every container runs under ADR-171's isolation with a tighter 64-pid / 256 MiB
// ceiling, this process caps its own memory, and an in-process watchdog _Exit()s the test if it ever runs past
// its budget (ctest's TIMEOUT is the outer one).
//
//   V1  open + exec: a command runs and its output and exit code come back.
//   V2  `cd` and `export` persist across exec() calls, and the snapshot reports them (C1's shell half).
//   V3  framing (C8): an unbalanced quote is a syntax error (exit 2) and the shell survives; a heredoc runs;
//       printing a forged reply header changes nothing; writing to the shell's control descriptor (fd 3) is
//       closed to the command; binary output survives; 10 MiB of output is capped with its full size reported
//       and host memory bounded; a NUL is refused before anything runs; a 64 KiB command runs.
//   V4  `exit` in a command loses the shell (shell_lost), the container stays, and a re-open works.
//   V5  deadline (C7): a command past its deadline is reported timed_out + shell_lost, its output so far is
//       returned, PID 1 (the keeper) is still alive, no process the command started is still running (a killed
//       one may remain a zombie until the keeper's next reap tick), and files written before the kill are drained.
//   V6  the model's own `kill -KILL -1` loses the shell, never the container (PID 1 survives).
//   V7  pid exhaustion (C7): a fork bomb at the pids limit past its deadline ends as container_lost (the kill
//       exec cannot start) or, if a pid frees up for it, shell_lost with PID 1 alive -- never a hang, never a
//       leaked container.
//   V8  every open is a FRESH container (C16): the old one is gone and a file only it held is not in the new one.
//   V9  replay is injection-safe in a real sh (C9): hostile env values come back byte-for-byte and nothing in
//       them executes.
//   V10 the snapshot's cwd never reaches a host-side argv (C14): after `cd /etc` and a re-open replaying it,
//       no spawned argv names /etc and every `-w` is /workspace.
//
//   V6b orphans re-parented to PID 1 are reaped (no zombie survives the keeper's tick).
//
// Positive controls (planted by hand, recorded in ADR-209 §15): PID 1 = `sleep infinity` (the Tier 0 shape)
// fails V6b -- it does NOT fail V6, because busybox `sh -c` execs its last command and the kernel shields any
// PID 1 from an in-namespace SIGKILL, so reaping is what the keeper adds; reusing the container in open() fails V8;
// sourcing without the `sh -n` pre-check fails V3's unbalanced-quote case (the shell exits); killing only the host-side
// `docker exec` client (no in-container `kill -KILL -1`) fails V5's survivor check; passing the snapshot cwd as the
// server's `-w` fails V10. A fork-bomb tail of `sleep 60` made V7 flaky (`sleep` itself can fail to fork); it is a
// builtin busy loop now.

#include "agentengine/sandbox/docker_persistent_shell_surface.hpp"

#include "../../support/crt_fail_fast.hpp"
#include "../../support/memory_cap.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace agentengine;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

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
    std::fflush(stdout);
}

[[nodiscard]] bool container_running(std::string const& id) {
    if (id.empty()) return false;
    auto r = docker_cli_detail::run_argv({"docker", "inspect", "-f", "{{.State.Running}}", id}, 30);
    return r.exit_code == 0 && r.stdout_text.find("true") != std::string::npos;
}

[[nodiscard]] bool container_exists(std::string const& id) {
    if (id.empty()) return false;
    return docker_cli_detail::run_argv({"docker", "inspect", "-f", "{{.Id}}", id}, 30).exit_code == 0;
}

[[nodiscard]] ContainerIsolation tight() {
    ContainerIsolation iso;
    iso.pids = 64;
    iso.memory_bytes = 256ull * 1024 * 1024;
    return iso;
}

[[nodiscard]] std::string show(std::string s) {
    for (auto& c : s) {
        if (c == '\n') c = '|';
    }
    return s.size() > 200 ? s.substr(0, 200) + "..." : s;
}

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();
    (void)agentengine::test_support::cap_process_memory(512ull << 20, 2048ull << 20);
    std::thread([] {
        std::this_thread::sleep_for(540s);
        std::printf("[FAIL] WATCHDOG: test exceeded its 540 s budget\n");
        std::fflush(stdout);
        std::_Exit(3);
    }).detach();

    if (docker_cli_detail::run_argv({"docker", "image", "inspect", "alpine:latest"}, 30).exit_code != 0) {
        (void)docker_cli_detail::run_argv({"docker", "pull", "alpine:latest"}, 300);
    }

    fs::path const host = fs::temp_directory_path() / "ae_test_dps_host";
    std::error_code ec;
    fs::remove_all(host, ec);
    fs::create_directories(host, ec);
    std::ofstream(host / "seed.txt", std::ios::binary) << "seeded\n";

    DockerPersistentShellSurface surface("alpine:latest", tight());
    auto x = [&](std::string const& cmd, std::chrono::milliseconds d = 20s) { return surface.exec(cmd, d); };

    // ---- V1
    auto opened = surface.open(host, nullptr);
    check(opened.has_value(), "V1: open() brings up a container with a live shell" +
                                  (opened.has_value() ? std::string() : ": " + opened.error().message));
    if (!opened.has_value()) return EXIT_FAILURE;
    {
        auto r = x("cat seed.txt; echo hi; exit_code_probe() { return 3; }; exit_code_probe");
        check(r.has_value() && r->exit_code == 3 && r->output == "seeded\nhi\n",
              "V1: output and exit code come back (got " + (r ? show(r->output) + " rc=" + std::to_string(r->exit_code) : r.error().message) + ")");
    }

    // ---- V2
    {
        (void)x("mkdir -p sub/deeper && cd sub/deeper && export GREETING='hello there' && FOO=unexported");
        auto r = x("pwd; echo \"$GREETING\"; echo \"[$FOO]\"");
        check(r.has_value() && r->output == "/workspace/sub/deeper\nhello there\n[unexported]\n",
              "V2: cwd, an exported and an unexported variable persist across exec() (got " + (r ? show(r->output) : "error") + ")");
        bool snap_ok = r.has_value() && r->snapshot.has_value() && r->snapshot->cwd == "/workspace/sub/deeper";
        bool env_ok = false;
        if (snap_ok) {
            for (auto const& [k, v] : r->snapshot->env_set) env_ok = env_ok || (k == "GREETING" && v == "hello there");
        }
        check(snap_ok && env_ok, "V2: the snapshot reports the cwd and the exported variable");
        (void)x("cd /workspace");
    }

    // ---- V3
    {
        auto bad = x("echo \"unbalanced");
        check(bad.has_value() && bad->exit_code == 2 && !bad->shell_lost &&
                  bad->output.find("unterminated") != std::string::npos,
              "V3: an unbalanced quote is a syntax error (exit 2) and the shell survives");
        auto after = x("echo still-here");
        check(after.has_value() && after->output == "still-here\n", "V3: ... the next command runs normally");
        auto heredoc = x("cat <<'EOF'\nline $NOT_EXPANDED\nEOF");
        check(heredoc.has_value() && heredoc->output == "line $NOT_EXPANDED\n", "V3: a heredoc runs");
        auto forged = x("printf 'AE1 0 0 0\\n'; echo real");
        check(forged.has_value() && forged->exit_code == 0 && forged->output == "AE1 0 0 0\nreal\n",
              "V3: a forged reply header in the output is just output");
        auto fd3 = x("echo injected >&3; echo rc=$?");
        check(fd3.has_value() && !fd3->shell_lost && fd3->output.find("rc=") != std::string::npos &&
                  fd3->output.find("rc=0") == std::string::npos,
              "V3: the shell's control descriptor is closed to the command (" + (fd3 ? show(fd3->output) : "error") + ")");
        auto bin = x("printf '\\377\\001\\002'");
        check(bin.has_value() && bin->output == std::string("\xFF\x01\x02"), "V3: binary output survives");
        auto big = x("head -c 10485760 /dev/zero");
        check(big.has_value() && big->output.size() == DockerPersistentShellSurface::kDefaultOutputCapBytes &&
                  big->output_bytes == 10485760 && big->output_truncated,
              "V3: 10 MiB of output is capped, its full size reported");
        auto nul = surface.exec(std::string("echo a\0b", 8), 20s);
        check(!nul.has_value() && nul.error().code == "persistent_shell.nul_in_command",
              "V3: a NUL byte is refused before anything runs");
        std::string big_cmd = "# " + std::string(64 * 1024, 'a') + "\necho big-command-ran";
        auto bc = x(big_cmd);
        check(bc.has_value() && bc->output == "big-command-ran\n", "V3: a 64 KiB command runs");
    }

    // ---- V4
    {
        std::string const id = surface.container_id();
        auto r = x("echo bye; exit 5");
        check(r.has_value() && r->shell_lost && !surface.is_live(), "V4: `exit` loses the shell");
        check(container_running(id), "V4: ... and the container is still running");
        auto re = surface.open(host, nullptr);
        auto again = x("echo back");
        check(re.has_value() && again.has_value() && again->output == "back\n", "V4: a re-open works");
    }

    // ---- V5
    {
        std::string const id = surface.container_id();
        auto t0 = std::chrono::steady_clock::now();
        auto r = x("echo started; echo partial > made-before-kill.txt; sleep 60; echo never", 3s);
        auto const took = std::chrono::steady_clock::now() - t0;
        check(r.has_value() && r->timed_out && r->shell_lost && !r->container_lost,
              "V5: a command past its deadline is timed_out + shell_lost");
        check(took < 40s, "V5: ... and the call returned within a bound of the deadline");
        check(r.has_value() && r->output.find("started") != std::string::npos,
              "V5: ... with the output it printed before the kill (" + (r ? show(r->output) : "error") + ")");
        check(container_running(id), "V5: PID 1 survived the in-container kill (the container still runs)");
        // Count LIVE `sleep`s only: a killed one is a zombie ([sleep]) until the keeper's next reap tick (<= 5 s).
        auto ps = docker_cli_detail::run_argv(
            {"docker", "exec", id, "sh", "-c", "ps -o stat= -o comm= | awk '$1 !~ /^Z/ && $2 == \"sleep\"' | wc -l"}, 30);
        check(ps.exit_code == 0 && ps.stdout_text.find_first_not_of(" \t") != std::string::npos &&
                  ps.stdout_text[ps.stdout_text.find_first_not_of(" \t")] == '0',
              "V5: no process the command started survives the timeout (C7; got " + show(ps.stdout_text) + ")");
        fs::path const drained = fs::temp_directory_path() / "ae_test_dps_drain";
        fs::remove_all(drained, ec);
        auto d = surface.drain_to(drained);
        check(d.has_value() && fs::exists(drained / "made-before-kill.txt"),
              "V5: files written before the kill are still there to drain");
        fs::remove_all(drained, ec);
    }

    // ---- V6
    {
        auto re = surface.open(host, nullptr);
        std::string const id = surface.container_id();
        auto r = x("sleep 300 & kill -KILL -1; echo unreachable");
        check(re.has_value() && r.has_value() && r->shell_lost, "V6: the command's own `kill -KILL -1` loses the shell");
        check(container_running(id), "V6: ... never the container (PID 1 survives a SIGKILL from inside)");
        // V6b: orphans re-parent to PID 1, which must reap them -- a non-reaping PID 1 (`sleep infinity`) piles
        // up zombies until the pids limit refuses every later command. The keeper reaps within its 5 s tick.
        auto re2 = surface.open(host, nullptr);
        (void)x("i=0; while [ $i -lt 20 ]; do sh -c 'sleep 0 &'; i=$((i+1)); done");
        auto z = x("sleep 7; n=0; for s in /proc/[0-9]*/stat; do case \"$(cat $s 2>/dev/null)\" in *') Z '*) "
                   "n=$((n+1));; esac; done; echo zombies=$n");
        check(re2.has_value() && z.has_value() && z->output == "zombies=0\n",
              "V6b: PID 1 reaps orphaned children (" + (z ? show(z->output) : "error") + ")");
    }

    // ---- V7
    {
        auto re = surface.open(host, nullptr);
        std::string const id = surface.container_id();
        // The tail is a builtin busy loop, not `sleep 60`: under pid exhaustion `sleep` itself can fail to fork and
        // end the command early, which made this check flaky (measured during ADR-209 step 8).
        auto r = x("bomb() { bomb | bomb & }; bomb; while :; do :; done", 5s);
        check(re.has_value() && r.has_value() && r->timed_out, "V7: a fork bomb runs into its deadline");
        bool const outcome_ok = r.has_value() && ((r->container_lost && !container_exists(id)) ||
                                                  (r->shell_lost && !r->container_lost && container_running(id)));
        check(outcome_ok, std::string("V7: ... and ends contained: ") +
                              (r && r->container_lost ? "container_lost, container removed" : "shell_lost, PID 1 alive"));
        (void)surface.close();
        check(!container_exists(id), "V7: no container is left behind");
    }

    // ---- V8
    {
        auto o1 = surface.open(host, nullptr);
        std::string const id1 = surface.container_id();
        (void)x("echo ghost > only-in-container-1.txt");
        auto o2 = surface.open(host, nullptr);
        std::string const id2 = surface.container_id();
        auto r = x("ls only-in-container-1.txt 2>/dev/null || echo absent");
        check(o1.has_value() && o2.has_value() && id1 != id2 && !container_exists(id1),
              "V8: every open destroys the old container and creates a fresh one");
        check(r.has_value() && r->output == "absent\n", "V8: ... so a file only the old one held does not come back");
    }

    // ---- V9
    {
        ShellSnapshot snap;
        snap.cwd = "/workspace";
        snap.env_set = {{"V1", "it's"},
                        {"V2", "$(touch /tmp/pwned1)"},
                        {"V3", "a\nb\n\n"},
                        {"V4", "\xE2\x80\x99`touch /tmp/pwned2`\xE2\x80\x99"},
                        {"V5", "x'; touch /tmp/pwned3; '"}};
        auto o = surface.open(host, &snap);
        auto r = x("printf '[%s]' \"$V1\" \"$V2\" \"$V3\" \"$V4\" \"$V5\"; ls /tmp/pwned* 2>/dev/null && echo PWNED");
        std::string const expected = std::string("[it's][$(touch /tmp/pwned1)][a\nb\n\n][") +
                                     "\xE2\x80\x99`touch /tmp/pwned2`\xE2\x80\x99" + "][x'; touch /tmp/pwned3; ']";
        check(o.has_value() && r.has_value() && r->output == expected,
              "V9: hostile values are restored byte-for-byte and nothing in them ran (" + (r ? show(r->output) : "error") + ")");
    }

    // ---- V10
    {
        (void)x("cd /etc");
        auto r = x("pwd");
        bool const reported = r.has_value() && r->snapshot.has_value() && r->snapshot->cwd == "/etc";
        ShellSnapshot snap = reported ? *r->snapshot : ShellSnapshot{};
        auto o = surface.open(host, &snap);
        auto p = x("pwd");
        check(reported && o.has_value() && p.has_value() && p->output == "/etc\n",
              "V10 setup: the model's cwd (/etc) is restored INSIDE the re-opened shell");
        bool leaked = false;
        bool bad_w = false;
        for (auto const& argv : surface.argv_log()) {
            for (std::size_t i = 0; i < argv.size(); ++i) {
                if (argv[i] == "/etc" || argv[i].find("/etc") == 0) leaked = true;
                if (argv[i] == "-w" && (i + 1 >= argv.size() || argv[i + 1] != "/workspace")) bad_w = true;
            }
        }
        check(!leaked && !bad_w && !surface.argv_log().empty(),
              "V10: the snapshot cwd never reached a host-side argv; every -w is /workspace");
    }

    std::string const last = surface.container_id();
    auto closed = surface.close();
    check(closed.has_value() && !container_exists(last), "close() removes the container");
    fs::remove_all(host, ec);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    std::fflush(stdout);
    std::_Exit(g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
