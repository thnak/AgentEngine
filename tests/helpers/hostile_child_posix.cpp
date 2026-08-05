// Linux/POSIX counterpart to tests/helpers/hostile_child.cpp (the Windows hostile-child helper) --
// same modes, same purpose: a deliberately hostile test-only process for
// tests/test_native_jail_backend_linux.cpp to launch and try to contain. Never a product target,
// never linked into anything else.
//
// Modes (argv[1]):
//   alloc <mb>            -- touch <mb> megabytes of committed memory, then idle.
//   spin                  -- consume CPU in a tight loop until killed.
//   sleep <ms>             -- sleep, then exit 0 (well-behaved baseline / positive-control child).
//   spawn <n> <self_path>  -- fork+exec itself <n> times running "sleep 500" each, report how many
//                             creations actually succeeded, then idle.
//   fail <code>            -- exit immediately with <code> (a clean, ordinary nonzero exit -- not
//                             a resource-limit kill, not a crash).
//   flood                  -- write output continuously (never exits on its own) -- M2 Phase C
//                             task C3's unbounded-output probe (008 SS2 item 2 / SS7).
//   probe_env               -- dump every visible environment variable as "ENV name=value" lines,
//                             then "ENV_DONE count=<n>". M2 Phase C task C5's env axis probe
//                             (008 SS9 G3, no ambient authority).
//   probe_net <port>        -- attempt a TCP connect to 127.0.0.1:<port> (a listener the launching
//                             test owns); report "NET_OK" or "NET_DENIED err=<errno>". C5's
//                             network axis probe.
//   probe_proc <pid>        -- enumerate all PIDs visible under /proc; report
//                             "PROC_VISIBLE total=<n> target=yes|no" for whether <pid> (a host-side
//                             process the launching test owns) shows up. C5's process axis probe.

#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern char** environ;

namespace {

int mode_alloc(int mb) {
    constexpr size_t kChunk = 1 * 1024 * 1024;
    std::vector<std::vector<char>> chunks;
    for (int i = 0; i < mb; i++) {
        chunks.emplace_back(kChunk, static_cast<char>(i & 0xFF));
        for (size_t off = 0; off < kChunk; off += 4096) {
            chunks.back()[off] = static_cast<char>(i);
        }
    }
    printf("ALLOC_DONE %d MB\n", mb);
    fflush(stdout);
    return 0;
}

int mode_spin() {
    volatile unsigned long long x = 0;
    for (;;) {
        for (int i = 0; i < 1'000'000; i++) x += static_cast<unsigned long long>(i);
    }
    return 0;
}

int mode_sleep(int ms) {
    usleep(static_cast<useconds_t>(ms) * 1000);
    printf("SLEEP_DONE %d ms\n", ms);
    return 0;
}

int mode_spawn(int n, std::string const& self_path) {
    int succeeded = 0;
    std::vector<pid_t> kids;
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            execl(self_path.c_str(), self_path.c_str(), "sleep", "500", nullptr);
            _exit(127);  // execl failed
        }
        if (pid > 0) {
            succeeded++;
            kids.push_back(pid);
        }
    }
    printf("SPAWN_RESULT requested=%d succeeded=%d\n", n, succeeded);
    fflush(stdout);
    for (pid_t pid : kids) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
    return 0;
}

int mode_fail(int code) {
    printf("FAIL_MODE exiting %d\n", code);
    fflush(stdout);
    return code;
}

int mode_flood() {
    std::string chunk(256, 'A');
    for (;;) {
        fwrite(chunk.data(), 1, chunk.size(), stdout);
        fflush(stdout);
    }
    return 0;
}

int mode_probe_env() {
    int count = 0;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        printf("ENV %s\n", *e);
        count++;
    }
    printf("ENV_DONE count=%d\n", count);
    fflush(stdout);
    return 0;
}

int mode_probe_net(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("NET_DENIED err=socket_create_%d\n", errno);
        fflush(stdout);
        return 0;
    }
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rc = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    bool ok = false;
    int last_errno = errno;
    if (rc == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(s, &write_set);
        timeval tv{2, 0};  // 2s bound -- CLAUDE.md Machine Safety, never an unbounded wait
        int sel = select(s + 1, nullptr, &write_set, nullptr, &tv);
        if (sel > 0) {
            int err = 0;
            socklen_t err_len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &err_len);
            ok = (err == 0);
            last_errno = err;
        } else {
            last_errno = ETIMEDOUT;
        }
    }
    if (ok) {
        printf("NET_OK\n");
    } else {
        printf("NET_DENIED err=%d\n", last_errno);
    }
    fflush(stdout);
    close(s);
    return 0;
}

int mode_probe_proc(pid_t target_pid) {
    DIR* d = opendir("/proc");
    if (d == nullptr) {
        printf("PROC_VISIBLE total=0 target=unknown err=%d\n", errno);
        fflush(stdout);
        return 0;
    }
    int total = 0;
    bool found_target = false;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        char const* name = entry->d_name;
        bool all_digits = name[0] != '\0';
        for (char const* c = name; *c != '\0'; ++c) {
            if (!std::isdigit(static_cast<unsigned char>(*c))) {
                all_digits = false;
                break;
            }
        }
        if (!all_digits) continue;
        total++;
        if (static_cast<pid_t>(std::atoi(name)) == target_pid) found_target = true;
    }
    closedir(d);
    printf("PROC_VISIBLE total=%d target=%s\n", total, found_target ? "yes" : "no");
    fflush(stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: hostile_child_posix <alloc|spin|sleep|spawn|fail|flood|probe_env|"
                "probe_net|probe_proc> ...\n");
        return 2;
    }
    std::string mode = argv[1];
    if (mode == "alloc" && argc >= 3) return mode_alloc(std::atoi(argv[2]));
    if (mode == "spin") return mode_spin();
    if (mode == "sleep" && argc >= 3) return mode_sleep(std::atoi(argv[2]));
    if (mode == "spawn" && argc >= 4) return mode_spawn(std::atoi(argv[2]), argv[3]);
    if (mode == "fail" && argc >= 3) return mode_fail(std::atoi(argv[2]));
    if (mode == "flood") return mode_flood();
    if (mode == "probe_env") return mode_probe_env();
    if (mode == "probe_net" && argc >= 3) return mode_probe_net(std::atoi(argv[2]));
    if (mode == "probe_proc" && argc >= 3)
        return mode_probe_proc(static_cast<pid_t>(std::atoi(argv[2])));
    fprintf(stderr, "unrecognized mode/args\n");
    return 2;
}
