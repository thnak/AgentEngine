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

#include <unistd.h>
#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: hostile_child_posix <alloc|spin|sleep|spawn|fail|flood> ...\n");
        return 2;
    }
    std::string mode = argv[1];
    if (mode == "alloc" && argc >= 3) return mode_alloc(std::atoi(argv[2]));
    if (mode == "spin") return mode_spin();
    if (mode == "sleep" && argc >= 3) return mode_sleep(std::atoi(argv[2]));
    if (mode == "spawn" && argc >= 4) return mode_spawn(std::atoi(argv[2]), argv[3]);
    if (mode == "fail" && argc >= 3) return mode_fail(std::atoi(argv[2]));
    if (mode == "flood") return mode_flood();
    fprintf(stderr, "unrecognized mode/args\n");
    return 2;
}
