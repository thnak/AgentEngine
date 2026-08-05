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
//   fail <code>            -- exit immediately with <code> (a clean, ordinary nonzero exit -- not
//                             a resource-limit kill, not a crash -- so a caller distinguishing
//                             "this exec merely returned nonzero" from "a limit killed it" has a
//                             positive-control shape for the FIRST case too, not only the second).
//   escape <path>          -- attempt to open and read <path> for GENERIC_READ; report
//                             "ESCAPE_OK <path> bytes=<n>" or "ESCAPE_DENIED <path> err=<code>".
//                             M2 Phase C task C3's fs-escape-attempt probe (008 SS7) -- never
//                             modifies or deletes anything, read-only.
//   flood                  -- write output continuously (never exits on its own) -- C3's
//                             unbounded-output probe (008 SS2 item 2 / SS7).
//   probe_env               -- dump every visible environment variable as "ENV name=value" lines,
//                             then "ENV_DONE count=<n>". M2 Phase C task C5's env axis probe
//                             (008 SS9 G3, no ambient authority).
//   probe_net <port>        -- attempt a TCP connect to 127.0.0.1:<port> (a listener the launching
//                             test owns); report "NET_OK" or "NET_DENIED err=<code>". C5's network
//                             axis probe.
//   probe_proc <pid>        -- enumerate all visible processes (CreateToolhelp32Snapshot); report
//                             "PROC_VISIBLE total=<n> target=yes|no" for whether <pid> (a host-side
//                             process the launching test owns) shows up. C5's process axis probe.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>

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

int mode_fail(int code) {
    printf("FAIL_MODE exiting %d\n", code);
    fflush(stdout);
    return code;
}

int mode_escape(std::string const& path) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        printf("ESCAPE_DENIED %s err=%lu\n", path.c_str(), GetLastError());
        fflush(stdout);
        return 0;
    }
    char buf[256];
    DWORD read = 0;
    ReadFile(h, buf, sizeof(buf), &read, nullptr);
    CloseHandle(h);
    printf("ESCAPE_OK %s bytes=%lu\n", path.c_str(), read);
    fflush(stdout);
    return 0;
}

int mode_flood() {
    std::string chunk(256, 'A');
    for (;;) {
        fwrite(chunk.data(), 1, chunk.size(), stdout);
        fflush(stdout);
    }
    return 0;
}

std::string narrow(wchar_t const* utf16, int len) {
    if (len <= 0) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, utf16, len, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16, len, out.data(), needed, nullptr, nullptr);
    return out;
}

int mode_probe_env() {
    LPWCH block = GetEnvironmentStringsW();
    int count = 0;
    if (block != nullptr) {
        wchar_t const* p = block;
        while (*p != L'\0') {
            size_t len = wcslen(p);
            std::string line = narrow(p, static_cast<int>(len));
            // Skip the "=C:=..." per-drive-cwd pseudo-vars Windows always injects -- not a real
            // named env var and not what this probe cares about.
            if (!line.empty() && line[0] != '=') {
                printf("ENV %s\n", line.c_str());
                count++;
            }
            p += len + 1;
        }
        FreeEnvironmentStringsW(block);
    }
    printf("ENV_DONE count=%d\n", count);
    fflush(stdout);
    return 0;
}

int mode_probe_net(int port) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("NET_DENIED err=wsastartup_failed\n");
        fflush(stdout);
        return 0;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        printf("NET_DENIED err=socket_create_%d\n", WSAGetLastError());
        fflush(stdout);
        WSACleanup();
        return 0;
    }
    u_long non_blocking = 1;
    ioctlsocket(s, FIONBIO, &non_blocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int rc = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    bool ok = false;
    if (rc == 0) {
        ok = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(s, &write_set);
        timeval tv{2, 0};  // 2s bound -- CLAUDE.md Machine Safety, never an unbounded wait
        int sel = select(0, nullptr, &write_set, nullptr, &tv);
        if (sel > 0) {
            int err = 0;
            int err_len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &err_len);
            ok = (err == 0);
        }
    }
    if (ok) {
        printf("NET_OK\n");
    } else {
        printf("NET_DENIED err=%d\n", WSAGetLastError());
    }
    fflush(stdout);
    closesocket(s);
    WSACleanup();
    return 0;
}

int mode_probe_proc(DWORD target_pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        printf("PROC_VISIBLE total=0 target=unknown err=%lu\n", GetLastError());
        fflush(stdout);
        return 0;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    int total = 0;
    bool found_target = false;
    if (Process32FirstW(snap, &entry)) {
        do {
            total++;
            if (entry.th32ProcessID == target_pid) found_target = true;
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    printf("PROC_VISIBLE total=%d target=%s\n", total, found_target ? "yes" : "no");
    fflush(stdout);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: hostile_child <alloc|spin|sleep|spawn|fail|escape|flood|probe_env|"
                "probe_net|probe_proc> ...\n");
        return 2;
    }
    std::string mode = argv[1];
    if (mode == "alloc" && argc >= 3) return mode_alloc(std::atoi(argv[2]));
    if (mode == "spin") return mode_spin();
    if (mode == "sleep" && argc >= 3) return mode_sleep(std::atoi(argv[2]));
    if (mode == "spawn" && argc >= 4) return mode_spawn(std::atoi(argv[2]), argv[3]);
    if (mode == "fail" && argc >= 3) return mode_fail(std::atoi(argv[2]));
    if (mode == "escape" && argc >= 3) return mode_escape(argv[2]);
    if (mode == "flood") return mode_flood();
    if (mode == "probe_env") return mode_probe_env();
    if (mode == "probe_net" && argc >= 3) return mode_probe_net(std::atoi(argv[2]));
    if (mode == "probe_proc" && argc >= 3)
        return mode_probe_proc(static_cast<DWORD>(std::atoi(argv[2])));
    fprintf(stderr, "unrecognized mode/args\n");
    return 2;
}
