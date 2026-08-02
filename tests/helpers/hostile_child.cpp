// Test-only helper process for tests/test_job_object_limits.cpp. Deliberately hostile behavior
// (unbounded memory growth, CPU spin, unbounded child-process creation) so the Job Object limits
// under test (src/backends/native_jail/job_object_limits.{hpp,cpp}) have something real to contain.
// Never linked into any product target; built only under AGENTENGINE_BUILD_TESTS.
//
// Modes (argv[1]):
//   alloc <mb>            -- touch <mb> megabytes of committed memory, then idle.
//   spin                  -- consume CPU in a tight loop until killed.
//   sleep <ms>             -- sleep, then exit 0 (well-behaved baseline / positive-control child).
//   spawn <n> <self_path>  -- CreateProcess itself <n> times running "sleep 500" each, report how
//                             many creations actually succeeded, then idle.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int mode_alloc(int mb) {
    // Commit and touch memory a chunk at a time so the OS cannot lazily defer the commit past the
    // job's memory cap -- writing one byte per page forces real commitment.
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
    return 0; // exit promptly -- a positive-control caller needs a clean, timely exit to observe,
              // not a process that is merely still alive because it is sleeping.
}

int mode_spin() {
    volatile unsigned long long x = 0;
    for (;;) {
        for (int i = 0; i < 1'000'000; i++) x += i;
    }
    return 0;
}

int mode_sleep(int ms) {
    Sleep(static_cast<DWORD>(ms));
    printf("SLEEP_DONE %d ms\n", ms);
    return 0;
}

int mode_spawn(int n, std::string const& self_path) {
    int succeeded = 0;
    std::vector<PROCESS_INFORMATION> kids;
    for (int i = 0; i < n; i++) {
        std::string cmdline = "\"" + self_path + "\" sleep 500";
        std::vector<char> mutable_cmdline(cmdline.begin(), cmdline.end());
        mutable_cmdline.push_back('\0');

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessA(nullptr, mutable_cmdline.data(), nullptr, nullptr, FALSE, 0,
                                  nullptr, nullptr, &si, &pi);
        if (ok) {
            succeeded++;
            kids.push_back(pi);
        }
    }
    printf("SPAWN_RESULT requested=%d succeeded=%d\n", n, succeeded);
    fflush(stdout);
    for (auto& pi : kids) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: hostile_child <alloc|spin|sleep|spawn> ...\n");
        return 2;
    }
    std::string mode = argv[1];
    if (mode == "alloc" && argc >= 3) return mode_alloc(std::atoi(argv[2]));
    if (mode == "spin") return mode_spin();
    if (mode == "sleep" && argc >= 3) return mode_sleep(std::atoi(argv[2]));
    if (mode == "spawn" && argc >= 4) return mode_spawn(std::atoi(argv[2]), argv[3]);
    fprintf(stderr, "unrecognized mode/args\n");
    return 2;
}
