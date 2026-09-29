// ADR-211 prove phase: the `host-contained` profile's principal split, measured in a real container.
//
// One static binary, three roles:
//   launch   -- the container entrypoint. Starts as root, creates the broker socketpair, forks the
//               broker as uid X, drops itself to uid E and execs the engine role. (§4b)
//   broker   -- entered by fork from `launch`, never exec'd: runs `/bin/sh -c` per request as X with
//               rlimits, then reaps every X process and normalizes workspace modes. (§4c)
//   engine   -- the stand-in for the host application: holds a secret, owns a private directory and a
//               "ledger" directory, runs the preflight (§4e) and the claim checks (§5) through the broker.
//
// Controls are selected by launcher flags so each claim has a run that must FAIL it:
//   --same-uid     X = E                       (C1/C2/C6 control)
//   --no-reap      broker skips kill(-1)       (C3 control)
//   --no-nproc     broker sets no RLIMIT_NPROC (C4 control; the container's --pids-limit still caps it)
//   --no-fixup     broker skips mode fixup     (P5 attack control)
//   --leak-canary  engine creates its canary 0644 instead of 0600 (C6 control)
//   --broker-dumpable  broker left dumpable (P8 control)
//   --n2           launcher installs the executor egress REJECT rules (§4f); without it is C5's control
//   --force        continue past a refused preflight (controls only: shows what the preflight prevents)
//   --no-normalize reset() does not re-open the engine-materialized tree to the group (C11 control)
//   --no-tmp-sweep broker leaves executor files in /tmp and /dev/shm (C10 control)
//
// Linux only. Build: g++-14 -std=c++23 -O2 -static -o contained contained.cpp

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <ftw.h>
#include <grp.h>
#include <poll.h>
#include <string>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <atomic>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <thread>

namespace {

struct Options {
    // Deliberately uids no host account uses: RLIMIT_NPROC counts every process of a uid in the user
    // namespace, and without userns-remap that is the HOST's. With uid 1000 (a real WSL user) the first
    // same-uid control could not fork at all.
    uid_t engine_uid = 23000;
    uid_t exec_uid = 23001;
    gid_t group = 23500;
    bool same_uid = false;
    bool no_reap = false;
    bool no_nproc = false;
    bool no_fixup = false;
    bool leak_canary = false;
    bool broker_dumpable = false;
    bool n2 = false;
    bool force = false;
    bool no_normalize = false;
    bool no_tmp_sweep = false;
};

constexpr char const* kWorkspace = "/workspace";
constexpr char const* kPrivate = "/engine/private";
constexpr char const* kLedger = "/engine/ledger";
constexpr std::size_t kMaxMsg = 1 << 16;

[[noreturn]] void die(char const* what) {
    std::fprintf(stderr, "FATAL %s: %s\n", what, std::strerror(errno));
    std::_Exit(99);
}

void drop_to(uid_t uid, gid_t gid) {
    gid_t const groups[] = {gid};
    if (setgroups(1, groups) != 0) die("setgroups");
    if (setresgid(gid, gid, gid) != 0) die("setresgid");
    if (setresuid(uid, uid, uid) != 0) die("setresuid");
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) die("no_new_privs");
    if (setresuid(0, 0, 0) == 0) {  // must be impossible now
        std::fprintf(stderr, "FATAL regained root after drop\n");
        std::_Exit(99);
    }
}

void write_file(char const* path, std::string const& s) {
    std::ofstream(path) << s;
}

// ---- broker (§4c) ---------------------------------------------------------------------------------

int g_fixup_group = -1;
int fixup_one(char const* path, struct stat const* st, int type, struct FTW*) {
    if (type == FTW_SL || type == FTW_SLN) return 0;           // never follow, never chmod a link
    if (st->st_uid != geteuid()) return 0;                      // only what X owns
    mode_t const want = (S_ISDIR(st->st_mode) ? 02770 : (st->st_mode & 0777) | 0660) & 07770;
    if (::chmod(path, want) != 0) return 0;
    if (static_cast<int>(st->st_gid) != g_fixup_group) (void)::lchown(path, static_cast<uid_t>(-1), g_fixup_group);
    return 0;
}

std::string run_one(std::string const& cmd, long wall_ms, bool set_nproc, long nproc) {
    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0) return "E\npipe failed";
    pid_t const child = fork();
    if (child == 0) {
        setsid();
        umask(0007);
        struct rlimit as{1ull << 30, 1ull << 30};
        setrlimit(RLIMIT_AS, &as);
        struct rlimit fs{64ull << 20, 64ull << 20};
        setrlimit(RLIMIT_FSIZE, &fs);
        if (set_nproc) {
            struct rlimit np{static_cast<rlim_t>(nproc), static_cast<rlim_t>(nproc)};
            setrlimit(RLIMIT_NPROC, &np);
        }
        dup2(out[1], 1);
        dup2(out[1], 2);
        if (chdir(kWorkspace) != 0) std::_Exit(126);
        char const* env[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/workspace", nullptr};
        execle("/bin/sh", "sh", "-c", cmd.c_str(), nullptr, env);
        std::_Exit(127);
    }
    close(out[1]);
    std::string output;
    timespec start{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status = 0;
    bool timed_out = false;
    char buf[4096];
    for (;;) {
        pollfd p{out[0], POLLIN, 0};
        int const r = poll(&p, 1, 50);
        if (r > 0) {
            ssize_t const n = read(out[0], buf, sizeof buf);
            if (n > 0 && output.size() < 8192) output.append(buf, static_cast<std::size_t>(n));
            if (n == 0) break;  // every writer closed
        }
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        long const ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (ms > wall_ms) {
            timed_out = true;
            break;
        }
    }
    close(out[0]);
    if (timed_out) kill(-child, SIGKILL);
    waitpid(child, &status, 0);
    int const code = timed_out ? 124 : (WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    return std::to_string(code) + "\n" + output;
}

int sweep_one(char const* path, struct stat const* st, int, struct FTW* ftw) {
    if (ftw->level == 0) return 0;                  // never the directory itself
    if (st->st_uid == geteuid()) (void)::remove(path);
    return 0;
}

[[noreturn]] void broker_main(int fd, Options const& o) {
    clearenv();  // the broker holds nothing; its children get only the explicit environment in run_one
    std::vector<char> buf(kMaxMsg);
    for (;;) {
        ssize_t const n = recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) std::_Exit(0);
        std::string const msg(buf.data(), static_cast<std::size_t>(n));
        // "<wall_ms> <nproc>\n<command>"
        std::string reply;
        {
            auto const nl = msg.find('\n');
            long wall = 5000, nproc = 64;
            std::sscanf(msg.substr(0, nl).c_str(), "%ld %ld", &wall, &nproc);
            reply = run_one(msg.substr(nl + 1), wall, !o.no_nproc, nproc);
            if (!o.no_reap) {
                (void)kill(-1, SIGKILL);  // every process X may signal, except this one (§4c)
                // Orphans reparent to this subreaper, not to the engine (PID 1), so every one of them is
                // this process's child now and a blocking wait collects all of them.
                while (waitpid(-1, nullptr, 0) > 0 || errno == EINTR) {
                }
            }
            if (!o.no_fixup) {
                g_fixup_group = static_cast<int>(o.group);
                (void)nftw(kWorkspace, fixup_one, 32, FTW_PHYS);
            }
            if (!o.no_tmp_sweep) {
                // Shared scratch outlives the reap otherwise, and on tmpfs it is memory (red-team #11).
                for (char const* dir : {"/tmp", "/dev/shm"}) (void)nftw(dir, sweep_one, 32, FTW_PHYS | FTW_DEPTH);
            }
        }
        if (reply.size() > kMaxMsg) reply.resize(kMaxMsg);
        (void)send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
    }
}

// ---- engine (the host application stand-in) ------------------------------------------------------

int g_fd = -1;
int g_checks = 0;
int g_failed = 0;

void check(bool ok, std::string const& what) {
    ++g_checks;
    if (!ok) ++g_failed;
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    std::fflush(stdout);
}

struct Reply {
    int code = -1;
    std::string out;
};

Reply exec_x(std::string const& cmd, long wall_ms = 5000, long nproc = 64) {
    std::string const msg = std::to_string(wall_ms) + " " + std::to_string(nproc) + "\n" + cmd;
    if (send(g_fd, msg.data(), msg.size(), MSG_NOSIGNAL) < 0) return {};
    std::vector<char> buf(kMaxMsg);
    ssize_t const n = recv(g_fd, buf.data(), buf.size(), 0);
    if (n <= 0) return {};
    std::string const r(buf.data(), static_cast<std::size_t>(n));
    auto const nl = r.find('\n');
    return {std::atoi(r.substr(0, nl).c_str()), r.substr(nl + 1)};
}

bool contains(std::string const& h, std::string const& n) { return h.find(n) != std::string::npos; }

// Counted by the ENGINE from /proc/<pid>/status (world-readable), excluding the broker: the reap is the
// broker's job, but whether it happened is not the broker's word to give (red-team #5).
int engine_counts_x(uid_t x) {
    int n = 0;
    char const* bpid = std::getenv("AE_CONTAINED_BROKER_PID");
    for (auto const& e : std::filesystem::directory_iterator("/proc")) {
        std::string const name = e.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
        if (bpid != nullptr && name == bpid) continue;
        std::ifstream st(e.path() / "status");
        std::string line;
        while (std::getline(st, line)) {
            if (line.rfind("Uid:", 0) == 0) {
                if (std::strtoul(line.c_str() + 4, nullptr, 10) == x) ++n;
                break;
            }
        }
    }
    return n;
}

// Every probe appends this marker, so "the output lacks the secret" can never pass because the
// command did not run at all (a dead broker returns an empty reply -- the first run of this harness
// passed C1/C2 vacuously exactly that way).
constexpr char const* kRan = "RAN-MARK-211";
Reply probe_x(std::string const& cmd) { return exec_x("{ " + cmd + " ; } 2>&1; echo " + kRan); }
bool ran(Reply const& r) { return contains(r.out, kRan); }

// Uid of a process as the kernel reports it in /proc/<pid>/status (0444 even when the process is not
// dumpable), or -1.
long uid_of(std::string const& pid) {
    std::ifstream st("/proc/" + pid + "/status");
    std::string line;
    while (std::getline(st, line)) {
        if (line.rfind("Uid:", 0) == 0) return std::strtol(line.c_str() + 4, nullptr, 10);
    }
    return -1;
}

// P0 -- BEFORE any command runs. Measured: with X = E, the broker's per-request reap (kill(-1) as the
// shared uid) killed the engine on the very first preflight probe, so a preflight that learns the
// executor's uid by running `id -u` never gets to refuse. The engine reads the broker's uid itself.
bool p0_split_before_any_command() {
    char const* bpid = std::getenv("AE_CONTAINED_BROKER_PID");
    long const buid = bpid ? uid_of(bpid) : -1;
    bool const ok = buid > 0 && buid != static_cast<long>(getuid()) && getuid() != 0;
    std::printf("  preflight %s P0 broker uid (%ld, from /proc) differs from engine uid (%d), neither root, no command run yet\n",
                ok ? "pass" : "FAIL", buid, getuid());
    return ok;
}

// The preflight (§4e). Returns true only if every probe ran and held.
bool preflight(std::string const& secret) {
    bool ok = true;
    auto probe = [&](bool cond, std::string const& what, Reply const& r) {
        std::printf("  preflight %s %s\n", cond ? "pass" : "FAIL", what.c_str());
        if (!cond) std::printf("    output: %.300s\n", r.out.c_str());
        ok = ok && cond;
    };
    std::string const epid = std::to_string(getpid());
    Reply const id = probe_x("id -u");
    long const xuid = std::atol(id.out.c_str());
    probe(ran(id) && xuid != static_cast<long>(getuid()) && xuid != 0 && getuid() != 0,
          "P1 executor uid (" + std::to_string(xuid) + ") differs from engine uid (" + std::to_string(getuid()) +
              ") and neither is root", id);
    Reply const canary = probe_x(std::string("cat ") + kPrivate + "/canary");
    probe(ran(canary) && !contains(canary.out, secret), "P2 executor cannot read the engine's 0600 canary", canary);
    Reply const env = probe_x("cat /proc/" + epid + "/environ | tr '\\0' '\\n' | grep AE_MODEL; head -c 16 /proc/" +
                              epid + "/mem | od -c | head -1");
    probe(ran(env) && !contains(env.out, secret), "P3 executor cannot read the engine's environ or mem", env);
    Reply const led = probe_x(std::string("echo forged > ") + kLedger + "/forged && echo WROTE");
    probe(ran(led) && !contains(led.out, "WROTE"), "P4 executor cannot write the ledger directory", led);
    Reply const ws = probe_x("mkdir -p pf/a && echo x > pf/a/f && echo MADE");
    std::error_code ec;
    std::filesystem::remove_all(std::string(kWorkspace) + "/pf", ec);
    probe(ran(ws) && contains(ws.out, "MADE") && !ec && !std::filesystem::exists(std::string(kWorkspace) + "/pf"),
          "P5 executor writes the workspace and the engine can delete what it wrote", ws);
    char const* bpid = std::getenv("AE_CONTAINED_BROKER_PID");
    Reply const br = probe_x(std::string("if cat /proc/") + (bpid ? bpid : "0") +
                             "/environ >/dev/null 2>&1; then echo BROKER-ENV-READABLE; fi");
    probe(ran(br) && !contains(br.out, "BROKER-ENV-READABLE"),
          "P8 executor cannot read the broker's /proc (non-dumpable, so no ptrace or mem either)", br);
    return ok;
}

int engine_main(Options const& o, char** argv) {
    char const* fd_s = std::getenv("AE_CONTAINED_BROKER_FD");
    if (fd_s == nullptr) {
        check(false, "C6: no broker fd -- preflight refuses (expected only in the no-fd control)");
        return 3;
    }
    std::string const secret = "SECRET-211-" + std::to_string(getpid()) + "-model-api-key";
    // /proc/<pid>/environ shows the environment the process was exec'd with, so re-exec once with the
    // secret in it: that is where a real engine's API key sits when the host passes it by environment.
    if (std::getenv("AE_REEXECED") == nullptr) {
        setenv("AE_MODEL_API_KEY", secret.c_str(), 1);
        setenv("AE_REEXECED", "1", 1);
        argv[1] = const_cast<char*>("engine-reexec");
        execv("/proc/self/exe", argv);
        die("reexec");
    }
    g_fd = std::atoi(fd_s);
    // adopt: the fd must not leak into anything the engine itself runs later (red-team #13)
    fcntl(g_fd, F_SETFD, FD_CLOEXEC);
    umask(077);  // the engine's own files are private by default (red-team #10)
    timeval tv{20, 0};  // a stopped or wedged broker must fail closed, never hang the engine
    setsockopt(g_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::mkdir(kPrivate, 0700);
    write_file((std::string(kPrivate) + "/canary").c_str(), secret);
    ::chmod((std::string(kPrivate) + "/canary").c_str(), o.leak_canary ? 0644 : 0600);
    if (o.leak_canary) ::chmod(kPrivate, 0755);
    ::mkdir(kLedger, 0700);
    write_file((std::string(kLedger) + "/checkpoint-1").c_str(), "tree=abc author=engine");

    std::printf("engine uid=%d pid=%d ppid=%d\n", getuid(), getpid(), getppid());
    bool const p0 = p0_split_before_any_command();
    if (!p0 && !o.force) {
        std::printf("PREFLIGHT REFUSED (P0: no command was run)\n");
        return 2;
    }
    bool const pf = p0 && preflight(secret);
    std::printf("PREFLIGHT %s\n", pf ? "PASSED" : "REFUSED");
    if (!pf && !o.force) return 2;

    // ---- the claims (§5), against the running split.
    Reply const r1 = probe_x(std::string("cat ") + kPrivate + "/canary; cat /proc/" + std::to_string(getpid()) + "/environ");
    check(ran(r1) && !contains(r1.out, secret), "C1: executor cannot read the engine's secret (file + environ)");
    Reply const r2 = probe_x(std::string("touch ") + kLedger + "/x && echo WROTE; cat " + kLedger +
                             "/checkpoint-1; echo forged >> " + kLedger + "/checkpoint-1 && echo APPENDED");
    std::ifstream cp1(std::string(kLedger) + "/checkpoint-1");
    std::string const cp1_now((std::istreambuf_iterator<char>(cp1)), std::istreambuf_iterator<char>());
    check(ran(r2) && !contains(r2.out, "WROTE") && !contains(r2.out, "APPENDED") && !contains(r2.out, "tree=abc") &&
              !contains(cp1_now, "forged"),
          "C2: executor cannot create, read or append to the ledger's existing records");

    // C3: a double-forked, setsid'd daemon does not outlive its command.
    Reply const d = probe_x("(setsid sh -c 'sleep 300' </dev/null >/dev/null 2>&1 &) ; echo started");
    int const left = engine_counts_x(static_cast<uid_t>(std::atol(probe_x("id -u").out.c_str())));
    check(ran(d) && left == 0,
          "C3: nothing a command starts outlives it -- counted by the engine (" + std::to_string(left) + " left)");

    // C4: a fork bomb is bounded for X, and the ENGINE can still fork while it runs. "The engine works
    // after" alone cannot fail: the reap frees every pid once the request ends. What RLIMIT_NPROC buys is
    // the engine not being starved DURING the bomb, so that is what is measured, from a second thread.
    std::atomic<int> engine_fork_ok{0};
    std::atomic<int> engine_fork_fail{0};
    std::thread prober([&] {
        ::usleep(1500 * 1000);  // the bomb has saturated by now
        for (int i = 0; i < 10; ++i) {
            pid_t const c = fork();
            if (c == 0) std::_Exit(0);
            if (c > 0) {
                waitpid(c, nullptr, 0);
                ++engine_fork_ok;
            } else {
                ++engine_fork_fail;
            }
            ::usleep(100 * 1000);
        }
    });
    Reply const bomb = exec_x("b(){ b|b& }; b; sleep 3; echo survived", 5000, 64);
    prober.join();
    Reply const after = probe_x("echo alive-after-bomb");
    check(engine_fork_fail == 0 && engine_fork_ok == 10,
          "C4: the engine can fork while an executor fork bomb runs (" + std::to_string(engine_fork_ok.load()) +
              "/10 forks succeeded)");
    check(contains(after.out, "alive-after-bomb"), "C4: ... and completes a request after it");

    // P5 attack: the executor locks the engine out of what it wrote.
    (void)exec_x("mkdir -p lock/in && echo x > lock/in/f && chmod 600 lock/in/f && chmod 700 lock/in lock && chmod g-s lock");
    std::error_code ec;
    std::filesystem::remove_all(std::string(kWorkspace) + "/lock", ec);
    check(!ec && !std::filesystem::exists(std::string(kWorkspace) + "/lock"),
          "P5-attack: after chmod 700/600 by the executor, the engine can still delete it" +
              (ec ? " (" + ec.message() + ")" : std::string{}));

    // C10: executor scratch in /tmp and /dev/shm does not outlive the command either.
    (void)probe_x("echo x > /tmp/x-leftover; echo x > /dev/shm/x-leftover");
    check(!std::filesystem::exists("/tmp/x-leftover") && !std::filesystem::exists("/dev/shm/x-leftover"),
          "C10: executor files in /tmp and /dev/shm are swept after the command");

    // C11: the engine writes with umask 077 (its secrets stay private), so what it materializes into the
    // workspace would be 0600 and uneditable by commands. reset() re-opens exactly the staging tree to the
    // group -- the engine owns those files, so it can -- and a command can then edit them.
    {
        std::filesystem::path const staged = std::filesystem::path(kWorkspace) / "staged";
        std::filesystem::create_directories(staged / "sub");
        write_file((staged / "sub" / "from-engine.txt").string().c_str(), "engine-materialized\n");
        if (!o.no_normalize) {
            for (auto const& e : std::filesystem::recursive_directory_iterator(staged)) {
                if (e.is_symlink()) continue;
                ::chmod(e.path().c_str(), e.is_directory() ? 02770 : 0660);
            }
            ::chmod(staged.c_str(), 02770);
        }
        Reply const edit = probe_x("echo edited-by-command >> staged/sub/from-engine.txt && echo EDITED");
        check(ran(edit) && contains(edit.out, "EDITED"),
              "C11: a command can edit what the engine materialized (engine umask 077, reset() normalizes)");
        std::error_code ec2;
        std::filesystem::remove_all(staged, ec2);
    }

    // C5 (N2): the executor cannot open a connection; the engine can. A listener the engine owns on
    // loopback stands in for "the network": it is the harder case (no route needed) and it is also
    // where a real engine's own local services would sit.
    {
        int const ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(18211);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool const listening = bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && listen(ls, 8) == 0;
        int const cs = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        bool const engine_connects = connect(cs, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
        close(cs);
        Reply const net = probe_x("nc -w 2 127.0.0.1 18211 </dev/null && echo CONNECTED");
        check(listening && engine_connects, "C5 setup: the engine's own loopback listener accepts the engine");
        check(ran(net) && !contains(net.out, "CONNECTED"), "C5: the executor cannot connect to it (N2 egress rules)");
        close(ls);
    }

    // Last, because it ends the channel: the executor can kill the broker (both are uid X). The engine
    // must survive and every later request must FAIL, never succeed or hang (§8 residual, fail closed).
    // The engine is NOT PID 1 (the launcher is), so kill(-1) would reach it if the uids allowed: with
    // --same-uid --force the engine dies here and never prints C9 -- that is this check's control.
    (void)exec_x("kill -9 -1; true");
    Reply const dead = probe_x("echo should-not-run");
    check(!ran(dead) && getpid() != 1,
          "C9: the executor's kill(-1) kills the broker, not the engine (not PID 1), and the next request fails closed");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}

// ---- launcher (§4b) ------------------------------------------------------------------------------

int launch_main(Options o, int argc, char** argv) {
    if (getuid() != 0) {
        std::fprintf(stderr, "launch must start as root\n");
        return 98;
    }
    if (o.same_uid) o.exec_uid = o.engine_uid;
    ::mkdir(kWorkspace, 02770);
    ::chown(kWorkspace, o.engine_uid, o.group);
    ::chmod(kWorkspace, 02770);
    ::mkdir("/engine", 0755);
    ::chown("/engine", o.engine_uid, o.engine_uid);
    if (o.n2) {
        // §4f N2: before anything drops privilege. Both families, every protocol, loopback included --
        // the owner match sees the socket's uid, so an executor process cannot reach the engine's own
        // listeners or the network through any address.
        std::string const x = std::to_string(o.exec_uid);
        for (char const* tool : {"iptables", "ip6tables"}) {
            std::string const cmd = std::string(tool) + " -A OUTPUT -m owner --uid-owner " + x + " -j REJECT";
            if (std::system(cmd.c_str()) != 0) {
                std::fprintf(stderr, "FATAL %s failed -- N2 must not start without its rules\n", cmd.c_str());
                return 96;
            }
        }
    }
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) != 0) die("socketpair");
    pid_t const b = fork();
    if (b == 0) {
        close(sp[0]);
        write_file("/proc/self/oom_score_adj", "1000");
        drop_to(o.exec_uid, o.group);
        // setresuid from root already cleared dumpable; state it, because the broker never execs and
        // this is what keeps an executor process from ptracing it or reading its /proc (P8).
        prctl(PR_SET_DUMPABLE, o.broker_dumpable ? 1 : 0, 0, 0, 0);
        // Without this, a double-forked daemon reparents to the container's PID 1 -- the ENGINE -- and
        // after the reap survives there as an executor-owned zombie in the engine's own process tree
        // (measured: C3 left 1 process). As subreaper, the broker inherits and collects every orphan.
        prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
        broker_main(sp[1], o);
    }
    close(sp[1]);
    setenv("AE_CONTAINED_BROKER_PID", std::to_string(b).c_str(), 1);
    write_file("/proc/self/oom_score_adj", "-500");
    drop_to(o.engine_uid, o.group);
    // The launcher stays PID 1, dropped to E: an init that only waits. The engine must not be PID 1,
    // because kill(-1) never signals init -- "the engine survives kill(-1)" would pass whatever the uids
    // were (red-team, weak claim P7). Orphans of the ENGINE reparent here and are reaped; orphans of the
    // executor go to the broker (subreaper).
    pid_t const engine = fork();
    if (engine == 0) {
        int const fl = fcntl(sp[0], F_GETFD);
        fcntl(sp[0], F_SETFD, fl & ~FD_CLOEXEC);  // the engine end, and only it, survives exec
        std::string const fd_s = std::to_string(sp[0]);
        setenv("AE_CONTAINED_BROKER_FD", fd_s.c_str(), 1);
        std::vector<char*> args;
        args.push_back(const_cast<char*>("contained"));
        args.push_back(const_cast<char*>("engine"));
        for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
        args.push_back(nullptr);
        execv("/proc/self/exe", args.data());
        die("exec engine");
    }
    close(sp[0]);
    int status = 0;
    for (;;) {
        pid_t const w = wait(&status);
        if (w == engine) break;
        if (w < 0 && errno != EINTR) break;
    }
    if (WIFSIGNALED(status)) {
        std::printf("ENGINE KILLED by signal %d\n", WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 0; i < argc; ++i) {
        std::string const a = argv[i];
        if (a == "--same-uid") o.same_uid = true;
        if (a == "--no-reap") o.no_reap = true;
        if (a == "--no-nproc") o.no_nproc = true;
        if (a == "--no-fixup") o.no_fixup = true;
        if (a == "--leak-canary") o.leak_canary = true;
        if (a == "--broker-dumpable") o.broker_dumpable = true;
        if (a == "--n2") o.n2 = true;
        if (a == "--force") o.force = true;
        if (a == "--no-normalize") o.no_normalize = true;
        if (a == "--no-tmp-sweep") o.no_tmp_sweep = true;
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 97;
    std::string const role = argv[1];
    Options const o = parse(argc - 2, argv + 2);
    if (role == "launch") return launch_main(o, argc - 2, argv + 2);
    if (role == "engine" || role == "engine-reexec") return engine_main(o, argv);
    return 97;
}
