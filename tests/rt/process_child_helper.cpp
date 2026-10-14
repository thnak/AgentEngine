// Child program for tests/rt/test_rt_reactor_process.cpp -- decisions/ADR-237-async-extension-points-and-io-
// reactor.md §6.3 "Processes", §8.2 gates 1d and 4. Every mode is bounded in time (machine safety, CLAUDE.md):
// even a "forever" sleeper a broken test leaks exits on its own after 120 s.
//
//   echo              copy stdin to stdout until EOF, exit 0
//   flood <MiB>       write <MiB> MiB of 'a' to stdout, then "done" to stderr, exit 0
//   exit <K>          write "out-K" to stdout and "err-K" to stderr, exit K
//   sleep             sleep (120 s cap), exit 0
//   sleepms <N>       sleep N ms, write "slept", exit 0
//   grandchild        start `sleep` as a grandchild sharing stdout, print "pid=<grandchild pid>", then sleep
//   orphan            the same, but exit 0 at once (the grandchild keeps stdout open)
//   env <NAME>        print the variable's value, or "<unset>"
//   cwd               print the current directory
//   argv              print each argument after "argv" on its own line

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr auto kForever = std::chrono::seconds(120);

void binary_stdio() {
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
    (void)_setmode(_fileno(stderr), _O_BINARY);
#endif
}

// Starts this program again in `sleep` mode, inheriting the standard handles; returns its pid or 0.
long start_grandchild() {
#ifdef _WIN32
    wchar_t self[MAX_PATH];
    DWORD const n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    std::wstring cmd = L"\"" + std::wstring(self, n) + L"\" sleep";
    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(self, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) == 0) {
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<long>(pi.dwProcessId);
#else
    char    self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) return 0;
    self[n]         = '\0';
    pid_t const pid = fork();
    if (pid == 0) {
        execl(self, self, "sleep", static_cast<char*>(nullptr));
        _exit(127);
    }
    return pid > 0 ? static_cast<long>(pid) : 0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    binary_stdio();
    if (argc < 2) return 64;
    std::string const mode = argv[1];

    if (mode == "echo") {
        std::vector<char> buf(64 * 1024);
        for (;;) {
            std::size_t const n = std::fread(buf.data(), 1, buf.size(), stdin);
            if (n == 0) break;
            std::fwrite(buf.data(), 1, n, stdout);
        }
        std::fflush(stdout);
        return 0;
    }
    if (mode == "flood" && argc >= 3) {
        long const              mib = std::strtol(argv[2], nullptr, 10);
        std::vector<char> const chunk(64 * 1024, 'a');
        for (long i = 0; i < mib * 16; ++i) std::fwrite(chunk.data(), 1, chunk.size(), stdout);
        std::fflush(stdout);
        std::fputs("done", stderr);
        std::fflush(stderr);
        return 0;
    }
    if (mode == "exit" && argc >= 3) {
        int const k = static_cast<int>(std::strtol(argv[2], nullptr, 10));
        std::printf("out-%d", k);
        std::fprintf(stderr, "err-%d", k);
        std::fflush(stdout);
        std::fflush(stderr);
        return k;
    }
    if (mode == "sleep") {
        std::this_thread::sleep_for(kForever);
        return 0;
    }
    if (mode == "sleepms" && argc >= 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::strtol(argv[2], nullptr, 10)));
        std::fputs("slept", stdout);
        std::fflush(stdout);
        return 0;
    }
    if (mode == "grandchild" || mode == "orphan") {
        long const pid = start_grandchild();
        std::printf("pid=%ld\n", pid);
        std::fflush(stdout);
        if (mode == "orphan") return pid != 0 ? 0 : 3;
        std::this_thread::sleep_for(kForever);
        return 0;
    }
    if (mode == "env" && argc >= 3) {
#ifdef _WIN32
        char        buf[4096];
        DWORD const n = GetEnvironmentVariableA(argv[2], buf, sizeof(buf));
        std::fputs(n > 0 && n < sizeof(buf) ? buf : "<unset>", stdout);
#else
        char const* v = std::getenv(argv[2]);
        std::fputs(v != nullptr ? v : "<unset>", stdout);
#endif
        return 0;
    }
    if (mode == "cwd") {
#ifdef _WIN32
        char  buf[MAX_PATH];
        DWORD n = GetCurrentDirectoryA(MAX_PATH, buf);
        std::fwrite(buf, 1, n, stdout);
#else
        char buf[4096];
        if (getcwd(buf, sizeof(buf)) != nullptr) std::fputs(buf, stdout);
#endif
        return 0;
    }
    if (mode == "argv") {
        for (int i = 2; i < argc; ++i) std::printf("%s\n", argv[i]);
        return 0;
    }
    return 64;
}
