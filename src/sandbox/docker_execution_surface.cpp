// Implements ADR-102 Phase 3 (the Docker `ExecutionSurface` conformer), 008 §2; ADR-207 (#120 S7).
// The non-template bodies of include/agentengine/sandbox/docker_execution_surface.hpp, moved verbatim out of the
// header so they are compiled once here instead of in every file that includes it, and so the header no longer
// pulls in <windows.h> or the POSIX process headers. The header keeps every declaration and comment and the small
// helpers; the design and the rules each body implements are documented at its declaration there.

#include "agentengine/sandbox/docker_execution_surface.hpp"

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef _WIN32
// Global scope, matching `ctr_cli_detail`'s own identical declaration
// (containerd_execution_surface.hpp) -- POSIX `environ` is a real global, not something a nested
// namespace `extern` declaration can bind to; declaring it inside `agentengine::docker_cli_detail`
// would instead declare a distinct, unresolvable `agentengine::docker_cli_detail::environ` symbol.
extern char** environ;
#endif

#ifdef _WIN32
namespace agentengine {
namespace docker_cli_detail {

SurfaceRunOutcome run_capture(std::string const& command,
                              int timeout_seconds,
                              std::size_t output_cap) {
    SurfaceRunOutcome out;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_h = nullptr;
    HANDLE write_h = nullptr;
    if (!CreatePipe(&read_h, &write_h, &sa, 0)) { out.exit_code = -1; return out; }
    SetHandleInformation(read_h, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_h;
    si.hStdError = write_h;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};

    std::string cmdline = "cmd.exe /c " + command;
    std::vector<char> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back('\0');

    // Job object so a timeout kill reaches the whole process TREE (`cmd.exe` and whatever real child
    // it spawns to run `command`, e.g. `docker.exe`), not just the top-level `cmd.exe` -- see the
    // function-level comment above for the real, probed leak this closes. Created and configured
    // BEFORE the process itself, and the process is started SUSPENDED and only assigned to the job
    // before its first instruction runs, so there is no window where `cmd.exe` could spawn a child
    // that escapes the job.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    DWORD const creation_flags = CREATE_NO_WINDOW | (job != nullptr ? CREATE_SUSPENDED : 0);
    BOOL created = CreateProcessA(nullptr, mutable_cmdline.data(), nullptr, nullptr,
                                   /*bInheritHandles=*/TRUE, creation_flags, nullptr, nullptr, &si, &pi);
    CloseHandle(write_h);
    if (!created) {
        CloseHandle(read_h);
        if (job != nullptr) CloseHandle(job);
        out.exit_code = -1;
        return out;
    }
    if (job != nullptr) {
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            // Could not bind the process to the job after all (rare) -- fall back to the
            // single-process kill path rather than leaving the process suspended forever.
            CloseHandle(job);
            job = nullptr;
        }
        ResumeThread(pi.hThread);
    }

    // Bounded, non-blocking-relative-to-deadline read: PeekNamedPipe tells us whether data is
    // available without blocking, so this loop can honor the wall-clock deadline even while the
    // child stays silent, unlike a plain blocking ReadFile.
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    bool stopped_early = false;
    char buf[4096];
    for (;;) {
        if (out.stdout_text.size() >= output_cap) { stopped_early = true; break; }
        if (std::chrono::steady_clock::now() >= deadline) { stopped_early = true; break; }
        DWORD available = 0;
        if (!PeekNamedPipe(read_h, nullptr, 0, nullptr, &available, nullptr)) break;  // pipe closed/error -- natural EOF
        if (available == 0) {
            DWORD const wait_rc = WaitForSingleObject(pi.hProcess, 20);
            if (wait_rc == WAIT_OBJECT_0) {
                // Process exited; drain whatever it left buffered before treating this as EOF.
                if (!PeekNamedPipe(read_h, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
            } else {
                continue;
            }
        }
        DWORD to_read = static_cast<DWORD>(std::min<std::size_t>(sizeof(buf), available));
        DWORD read = 0;
        if (!ReadFile(read_h, buf, to_read, &read, nullptr) || read == 0) break;
        std::size_t const remaining = output_cap > out.stdout_text.size() ? output_cap - out.stdout_text.size() : 0;
        std::size_t const take = static_cast<std::size_t>(read) < remaining ? static_cast<std::size_t>(read) : remaining;
        out.stdout_text.append(buf, take);
    }
    CloseHandle(read_h);

    DWORD exit_code = 0;
    if (stopped_early) {
        // `TerminateJobObject` (when the job bind above succeeded) kills `cmd.exe` AND every real
        // child it spawned to run `command` -- `TerminateProcess` alone only reaches `cmd.exe`,
        // leaving e.g. `docker.exe` (and, transitively, whatever it was waiting on) running as a real
        // orphan; see the function-level comment above.
        if (job != nullptr) {
            TerminateJobObject(job, 1);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        WaitForSingleObject(pi.hProcess, 5000);
        out.exit_code = -1;
    } else {
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
            out.exit_code = static_cast<int>(exit_code);
        } else {
            out.exit_code = -1;
        }
    }
    if (job != nullptr) CloseHandle(job);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return out;
}

std::wstring widen(std::string const& utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

std::string path_to_utf8(std::filesystem::path const& p) {
    std::wstring const& w = p.native();
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                       nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr,
                         nullptr);
    return out;
}

std::wstring quote_one_argument(std::wstring const& arg) {
    bool const needs_quotes = arg.empty() || arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) return arg;
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) {
            out.append(backslashes, L'\\');
            backslashes = 0;
        }
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring build_command_line(std::vector<std::string> const& argv) {
    std::wstring line;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) line.push_back(L' ');
        line += quote_one_argument(widen(argv[i]));
    }
    return line;
}

SurfaceRunOutcome run_argv(std::vector<std::string> const& argv,
                           int timeout_seconds,
                           std::size_t output_cap,
                           std::filesystem::path const* stdin_file,
                           bool* timed_out) {
    SurfaceRunOutcome out;
    if (timed_out != nullptr) *timed_out = false;
    if (argv.empty()) { out.exit_code = -1; return out; }
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_h = nullptr;
    HANDLE write_h = nullptr;
    if (!CreatePipe(&read_h, &write_h, &sa, 0)) { out.exit_code = -1; return out; }
    SetHandleInformation(read_h, HANDLE_FLAG_INHERIT, 0);

    HANDLE in_h = nullptr;
    if (stdin_file != nullptr) {
        // FILE_SHARE_DELETE as well as READ (ADR-174 round-2 finding F5): without it, a child that
        // outlives the timeout's 5-second wait keeps the staged archive undeletable, and the guard's
        // `remove_all` then fails SILENTLY, leaving a full copy of the worktree in %TEMP% forever.
        in_h = CreateFileW(stdin_file->wstring().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, &sa, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (in_h == INVALID_HANDLE_VALUE) {
            CloseHandle(read_h);
            CloseHandle(write_h);
            out.exit_code = -1;
            return out;
        }
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_h;
    si.hStdError = write_h;
    si.hStdInput = in_h;
    PROCESS_INFORMATION pi{};

    std::wstring const cmdline = build_command_line(argv);
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');

    // Same Job Object shape as `run_capture()` above -- see this function's own header comment for why
    // it now binds `argv[0]` directly rather than `cmd.exe`.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    DWORD const creation_flags = CREATE_NO_WINDOW | (job != nullptr ? CREATE_SUSPENDED : 0);
    BOOL created = CreateProcessW(nullptr, mutable_cmdline.data(), nullptr, nullptr,
                                   /*bInheritHandles=*/TRUE, creation_flags, nullptr, nullptr, &si, &pi);
    CloseHandle(write_h);
    // The parent's own handle to the stdin file goes as soon as the child has inherited it.
    if (in_h != nullptr) CloseHandle(in_h);
    if (!created) {
        CloseHandle(read_h);
        if (job != nullptr) CloseHandle(job);
        out.exit_code = -1;
        return out;
    }
    if (job != nullptr) {
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
        ResumeThread(pi.hThread);
    }

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    bool stopped_early = false;
    char buf[4096];
    for (;;) {
        if (out.stdout_text.size() >= output_cap) { stopped_early = true; break; }
        if (std::chrono::steady_clock::now() >= deadline) { stopped_early = true; if (timed_out != nullptr) *timed_out = true; break; }
        DWORD available = 0;
        if (!PeekNamedPipe(read_h, nullptr, 0, nullptr, &available, nullptr)) break;  // pipe closed/error -- natural EOF
        if (available == 0) {
            DWORD const wait_rc = WaitForSingleObject(pi.hProcess, 20);
            if (wait_rc == WAIT_OBJECT_0) {
                if (!PeekNamedPipe(read_h, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
            } else {
                continue;
            }
        }
        DWORD to_read = static_cast<DWORD>(std::min<std::size_t>(sizeof(buf), available));
        DWORD read = 0;
        if (!ReadFile(read_h, buf, to_read, &read, nullptr) || read == 0) break;
        std::size_t const remaining = output_cap > out.stdout_text.size() ? output_cap - out.stdout_text.size() : 0;
        std::size_t const take = static_cast<std::size_t>(read) < remaining ? static_cast<std::size_t>(read) : remaining;
        out.stdout_text.append(buf, take);
    }
    CloseHandle(read_h);

    DWORD exit_code = 0;
    if (stopped_early) {
        if (job != nullptr) {
            TerminateJobObject(job, 1);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        WaitForSingleObject(pi.hProcess, 5000);
        out.exit_code = -1;
    } else {
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
            out.exit_code = static_cast<int>(exit_code);
        } else {
            out.exit_code = -1;
        }
    }
    if (job != nullptr) CloseHandle(job);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return out;
}

}  // namespace docker_cli_detail
}  // namespace agentengine
#endif  // _WIN32

#ifndef _WIN32
namespace agentengine {
namespace docker_cli_detail {

SurfaceRunOutcome run_capture(std::string const& command,
                              int timeout_seconds,
                              std::size_t output_cap) {
    SurfaceRunOutcome out;
    std::array<int, 2> pipe_fds{-1, -1};
    if (::pipe(pipe_fds.data()) != 0) { out.exit_code = -1; return out; }
    ::fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

    char shell[] = "/bin/sh";
    char flag[] = "-c";
    std::vector<char> command_buf(command.begin(), command.end());
    command_buf.push_back('\0');
    char* argv[] = {shell, flag, command_buf.data(), nullptr};

    pid_t pid = -1;
    int const spawn_rc = ::posix_spawn(&pid, "/bin/sh", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]);
    if (spawn_rc != 0) {
        ::close(pipe_fds[0]);
        out.exit_code = -1;
        return out;
    }

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    bool stopped_early = false;
    char buf[4096];
    for (;;) {
        if (out.stdout_text.size() >= output_cap) { stopped_early = true; break; }
        auto const now = std::chrono::steady_clock::now();
        if (now >= deadline) { stopped_early = true; break; }
        struct pollfd pfd{pipe_fds[0], POLLIN, 0};
        int const timeout_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        int const rc = ::poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 0);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) { stopped_early = true; break; }  // poll's own timeout hit the deadline
        if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
        ssize_t const n = ::read(pipe_fds[0], buf, sizeof(buf));
        if (n <= 0) break;  // natural EOF -- child closed its output
        std::size_t const remaining = output_cap > out.stdout_text.size() ? output_cap - out.stdout_text.size() : 0;
        std::size_t const take = static_cast<std::size_t>(n) < remaining ? static_cast<std::size_t>(n) : remaining;
        out.stdout_text.append(buf, take);
    }
    ::close(pipe_fds[0]);

    int status = 0;
    if (stopped_early) {
        // Deadline or output cap hit before the child closed its own output -- it must be assumed
        // still running (or blocked writing into a pipe we've stopped draining) and is force-killed,
        // never waited on with a plain blocking waitpid(), which could hang this call just as long as
        // the child it was meant to bound.
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        out.exit_code = -1;
    } else {
        // Natural EOF: the child has already closed its output, so it is expected to exit
        // imminently -- a bounded, blocking reap here mirrors `ctr_cli_detail::run_argv`'s own
        // "Bounded reap even in the non-timeout path" comment.
        pid_t const reaped = ::waitpid(pid, &status, 0);
        if (reaped == pid && WIFEXITED(status)) {
            out.exit_code = WEXITSTATUS(status);
        } else if (reaped == pid && WIFSIGNALED(status)) {
            out.exit_code = 128 + WTERMSIG(status);
        } else {
            out.exit_code = -1;
        }
    }
    return out;
}

SurfaceRunOutcome run_argv(std::vector<std::string> const& argv,
                           int timeout_seconds,
                           std::size_t output_cap,
                           std::filesystem::path const* stdin_file,
                           bool* timed_out) {
    SurfaceRunOutcome out;
    if (timed_out != nullptr) *timed_out = false;
    if (argv.empty()) { out.exit_code = -1; return out; }
    std::array<int, 2> pipe_fds{-1, -1};
    if (::pipe(pipe_fds.data()) != 0) { out.exit_code = -1; return out; }
    ::fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);

    std::string const stdin_path = stdin_file != nullptr ? stdin_file->string() : std::string("/dev/null");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, stdin_path.c_str(), O_RDONLY, 0);

    std::vector<char*> c_argv;
    c_argv.reserve(argv.size() + 1);
    for (auto const& a : argv) c_argv.push_back(const_cast<char*>(a.c_str()));
    c_argv.push_back(nullptr);

    pid_t pid = -1;
    int const spawn_rc = ::posix_spawnp(&pid, c_argv[0], &actions, nullptr, c_argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]);
    if (spawn_rc != 0) {
        ::close(pipe_fds[0]);
        out.exit_code = -1;
        return out;
    }

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    bool stopped_early = false;
    char buf[4096];
    for (;;) {
        if (out.stdout_text.size() >= output_cap) { stopped_early = true; break; }
        auto const now = std::chrono::steady_clock::now();
        if (now >= deadline) { stopped_early = true; if (timed_out != nullptr) *timed_out = true; break; }
        struct pollfd pfd{pipe_fds[0], POLLIN, 0};
        int const timeout_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        int const rc = ::poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 0);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) { stopped_early = true; if (timed_out != nullptr) *timed_out = true; break; }
        if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
        ssize_t const n = ::read(pipe_fds[0], buf, sizeof(buf));
        if (n <= 0) break;
        std::size_t const remaining = output_cap > out.stdout_text.size() ? output_cap - out.stdout_text.size() : 0;
        std::size_t const take = static_cast<std::size_t>(n) < remaining ? static_cast<std::size_t>(n) : remaining;
        out.stdout_text.append(buf, take);
    }
    ::close(pipe_fds[0]);

    int status2 = 0;
    if (stopped_early) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status2, 0);
        out.exit_code = -1;
    } else {
        pid_t const reaped = ::waitpid(pid, &status2, 0);
        if (reaped == pid && WIFEXITED(status2)) {
            out.exit_code = WEXITSTATUS(status2);
        } else if (reaped == pid && WIFSIGNALED(status2)) {
            out.exit_code = 128 + WTERMSIG(status2);
        } else {
            out.exit_code = -1;
        }
    }
    return out;
}

}  // namespace docker_cli_detail
}  // namespace agentengine
#endif  // _WIN32

namespace agentengine {
namespace docker_cli_detail {

std::string join_argv_for_log(std::vector<std::string> const& argv) {
    std::string out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) out += ' ';
        out += argv[i];
    }
    return out;
}

}  // namespace docker_cli_detail
}  // namespace agentengine

namespace agentengine {

agentengine::result<void> docker_cli_reject_embedded_nul(std::string const& value,
                                                                                   char const* what) {
    if (value.find('\0') != std::string::npos) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            std::string("refusing to spawn docker: '") + what +
                "' contains an embedded NUL byte (would silently truncate at the argv boundary)",
            "docker_cli_backend.unsafe_argv_value"});
    }
    return agentengine::result<void>{};
}

agentengine::result<void> docker_cli_reject_leading_dash(std::string const& value,
                                                                                  char const* what) {
    if (!value.empty() && value[0] == '-') {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            std::string("refusing to spawn docker: '") + what +
                "' starts with '-', which could be parsed as a docker CLI flag instead of a value",
            "docker_cli_backend.unsafe_argv_value"});
    }
    return agentengine::result<void>{};
}

agentengine::result<void> docker_cli_reject_empty(std::string const& value,
                                                                            char const* what) {
    if (value.empty()) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            std::string("refusing to spawn docker: '") + what + "' must not be empty",
            "docker_cli_backend.unsafe_argv_value"});
    }
    return agentengine::result<void>{};
}

std::vector<std::string> docker_isolation_argv(ContainerIsolation const& iso) {
    std::vector<std::string> argv;
    argv.emplace_back("--network");
    argv.emplace_back(iso.network_enabled ? "bridge" : "none");
    // Docker's own `b` suffix -- explicit bytes, never a bare number whose unit the daemon has to
    // guess (it defaults to bytes today, but stating it costs nothing and cannot drift).
    argv.emplace_back("--memory");
    argv.emplace_back(std::to_string(iso.memory_bytes) + "b");
    argv.emplace_back("--pids-limit");
    argv.emplace_back(std::to_string(iso.pids));
    argv.emplace_back("--cpus");
    argv.emplace_back(std::to_string(iso.cpu_milli / 1000) + "." +
                       [&] {
                           std::string frac = std::to_string(iso.cpu_milli % 1000);
                           return std::string(3 - frac.size(), '0') + frac;
                       }());
    if (iso.drop_all_capabilities) {
        argv.emplace_back("--cap-drop");
        argv.emplace_back("ALL");
    }
    if (iso.no_new_privileges) {
        argv.emplace_back("--security-opt");
        argv.emplace_back("no-new-privileges");
    }
    return argv;
}

agentengine::result<ContainerIsolation> container_isolation_from(
    agentengine::ResourceLimits const& limits, agentengine::NetPolicy const& net) {
    if (!net.allowlist.empty()) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            "DockerExecutionSurface cannot enforce a NetPolicy allowlist: `docker run --network "
            "bridge` grants unfiltered egress, which is not host-mediated (008 §4). Use deny_all, or "
            "route egress through sandbox/net_egress_proxy.hpp.",
            "docker_execution_surface.netpolicy_allowlist_unsupported"});
    }
    ContainerIsolation iso;
    iso.network_enabled = !net.deny_all;
    if (limits.memory_bytes != 0) iso.memory_bytes = limits.memory_bytes;
    if (limits.pids != 0) iso.pids = limits.pids;
    return iso;
}

}  // namespace agentengine

namespace agentengine {

agentengine::result<DockerCliBackend::Instance> DockerCliBackend::create(std::string const& image,
                                                      ContainerIsolation const& isolation,
                                                      std::string const& init_script) {
    if (auto safe = docker_cli_reject_argv_value(image, "image"); !safe.has_value())
        return std::unexpected(safe.error());
    // A discoverable NAME (distinct from the docker-assigned `container_id` this method returns
    // below) -- exists purely so a later process can find and `reap_orphans()` this container if
    // THIS process dies before its own destructor runs (see that method's own comment). Carries
    // this process's own pid AND its start-key (ADR-108 §7 pid-reuse fix -- see
    // `check_process_identity()`'s own comment): a plain pid alone cannot tell "still the same
    // process" from "the pid was later reused by something unrelated", which would otherwise let
    // a genuinely orphaned container stay permanently unreapable. Built entirely from this
    // process's own pid/start-key and an internal counter, never caller/model input, so it already
    // trivially satisfies every check above; no validation call needed for a value this function
    // itself constructs from only digits and literal underscores (CLAUDE.md: don't add a check for
    // an input that's already impossible).
    std::string const name = std::string(docker_cli_detail::kOrphanNamePrefix) +
                              std::to_string(docker_cli_detail::current_pid()) + "_" +
                              std::to_string(docker_cli_detail::current_process_start_key()) + "_" +
                              std::to_string(++docker_cli_detail::g_next_container_seq);
    // Real argv vector -- `sh -c "mkdir -p /workspace && sleep infinity"` is still one shell string,
    // but it is the CONTAINER's own inner shell reading it (a documented, accepted risk layer, not
    // a host one -- see this section's own top comment), passed here as ONE literal argv element,
    // never concatenated into anything a host shell re-parses.
    //
    // ADR-171: the isolation flags are spliced in BEFORE `image` -- `docker run`'s own grammar is
    // `docker run [OPTIONS] IMAGE [COMMAND]`, so anything after the image name is the container's
    // argv, not a daemon option, and a flag placed there would be silently handed to `sh` instead
    // of being enforced. Every element comes from `docker_isolation_argv()`, which builds them
    // from this process's own numbers -- never caller or model input -- so no additional argv
    // validation is needed for a value this class itself constructs from digits and literals
    // (CLAUDE.md: don't add a check for an input that's already impossible).
    std::vector<std::string> argv = {"docker", "run", "-d", "--rm", "-w", "/workspace",
                                      "--name", name};
    for (std::string& flag : docker_isolation_argv(isolation)) argv.push_back(std::move(flag));
    argv.push_back(image);
    argv.insert(argv.end(),
                {"sh", "-c", init_script.empty() ? std::string("mkdir -p /workspace && sleep infinity") : init_script});
    auto r = docker_cli_detail::run_argv(argv);
    if (r.exit_code != 0) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "docker run failed: " + r.stdout_text + " (argv: " + docker_cli_detail::join_argv_for_log(argv) + ")",
            "docker_cli_backend.create_failed"});
    }
    // ADR-146 §11: a REAL, reproduced-on-CI bug found here, not the naive trailing-newline trim
    // this used to be. `run_argv()` merges stdout AND stderr into one stream (by design, matching
    // `run_capture()`'s own established convention -- see this file's own top comment). When
    // `image` is not yet cached locally, `docker run` writes multi-line pull-progress noise
    // ("Unable to find image '...' locally", "Pulling from...", "Status: Downloaded newer image
    // for...") BEFORE its own final, single-line, machine-readable container id -- the ONLY output
    // `docker run -d` guarantees on success is that its LAST line is the id. Trimming only trailing
    // whitespace kept that entire pull-progress preamble glued onto the id, which then got embedded
    // verbatim into `copy_to_container()`'s own generated command -- multiple embedded
    // newlines/spaces there is exactly what turned one shell argument into several, producing the
    // CI failure this ADR's own diagnostic finally captured: `docker: 'docker cp' requires 2
    // arguments`. Fixed by extracting only the LAST NON-EMPTY line of the captured output,
    // unconditionally correct whether or not a pull preamble is present (a cache-hit `docker run`
    // output IS just the id, a single line, so this is a strict generalization, not a special
    // case). Still relevant post-argv-port: the id is embedded into a fresh argv element at each
    // call site below, not concatenated into a string, but a multi-line/whitespace-polluted id
    // would still be the WRONG id.
    std::string const trimmed = [&] {
        std::string s = r.stdout_text;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s;
    }();
    auto const last_newline = trimmed.find_last_of('\n');
    std::string id = (last_newline == std::string::npos) ? trimmed : trimmed.substr(last_newline + 1);
    while (!id.empty() && id.back() == '\r') id.pop_back();
    // ADR-165 red-team Finding B: `id` is used as a bare/prefix argv element at every downstream
    // call site (exec()/destroy()/copy_to_container()/copy_from_container()) WITHOUT going through
    // docker_cli_reject_leading_dash() the way image/host_path/container_path already do -- not
    // currently reachable (`docker run -d`'s own documented output contract guarantees a hex
    // container id, which can never start with '-', and `id` is never attacker/model-influenced),
    // but checked here once, at the source, so every downstream call site is covered by
    // construction rather than by an argument this class's own contract happens to make true today.
    if (auto safe = docker_cli_reject_leading_dash(id, "container_id"); !safe.has_value())
        return std::unexpected(safe.error());
    return Instance{id};
}

agentengine::result<void> DockerCliBackend::copy_to_container(Instance const& inst,
                                                              std::filesystem::path const& host_path,
                                                              std::string const& container_path) {
    // `path_to_utf8()`, not `host_path.string()` -- on Windows, `std::filesystem::path::string()`
    // narrows via the process's ACTIVE CODE PAGE, not UTF-8; `run_argv()`'s own `widen()` explicitly
    // expects UTF-8 (code-review finding, post-ADR-165). Validating and embedding the SAME
    // UTF-8-correct bytes keeps what was checked and what actually reaches `docker` identical.
    std::string const host_path_utf8 = docker_cli_detail::path_to_utf8(host_path);
    if (auto safe = docker_cli_reject_argv_value(host_path_utf8, "host_path"); !safe.has_value())
        return std::unexpected(safe.error());
    if (auto safe = docker_cli_reject_argv_value(container_path, "container_path"); !safe.has_value())
        return std::unexpected(safe.error());
    std::vector<std::string> const argv = {"docker", "cp", host_path_utf8,
                                             inst.container_id + ":" + container_path};
    auto r = docker_cli_detail::run_argv(argv);
    if (r.exit_code != 0) {
        // ADR-146 §10: what was actually generated is included here, not just the daemon's own
        // reply -- `test_composed_sandbox_providers_live` failed on ubuntu-latest CI with `docker:
        // 'docker cp' requires 2 arguments` and no visibility into what was actually run. Safe to
        // log verbatim by construction (`host_path`/`container_path` both passed
        // `docker_cli_reject_argv_value` above before being embedded).
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "docker cp (to container) failed: " + r.stdout_text +
                " (argv: " + docker_cli_detail::join_argv_for_log(argv) + ")",
            "docker_cli_backend.copy_to_failed"});
    }
    return agentengine::result<void>{};
}

agentengine::result<void> DockerCliBackend::seed_tree_as_root(Instance const& inst,
                                                              std::filesystem::path const& host_dir,
                                                              std::string const& container_path) {
    if (auto safe = docker_cli_reject_argv_value(container_path, "container_path"); !safe.has_value())
        return std::unexpected(safe.error());

    std::error_code ec;
    std::filesystem::path const temp_root = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                  "no temp directory to stage the seed archive in",
                                                  "docker_cli_backend.seed_stage_failed"});
    }

    // ADR-174 §3, round-2 finding F1 -- a REAL, demonstrated local arbitrary-file-overwrite, not
    // a theoretical one. The first version staged to
    // `<temp>/ae_seed_<pid>_<g_next_container_seq>.tar`. On Linux `<temp>` is /tmp, mode 1777, and
    // `g_next_container_seq` is the SAME counter that mints container names -- which `docker ps`
    // PUBLISHES as `ae_des_<pid>_<ticks>_<seq>`. So any local user could read one container name,
    // compute the next staged filename exactly, plant a symlink there, and have this function
    // truncate and overwrite any file the engine's uid can write. The red-team executed it: a
    // victim file went from 24 bytes of text to 2048 bytes of tar, the guard then deleted only the
    // symlink, and this function returned SUCCESS.
    //
    // Two things fix it, and both are needed. The name is now UNPREDICTABLE (random_device, not a
    // counter a bystander can observe), and the archive lives inside a PRIVATE DIRECTORY created
    // 0700 in one atomic step -- so even a guessed name lands somewhere no other user may create
    // an entry. `::mkdir(..., 0700)` rather than `create_directory()` + `permissions()`, because
    // the two-step version is 0755 for the width of the window between them, which is all the
    // attack needs.
    std::string nonce;
    {
        std::random_device rd;
        for (int i = 0; i < 4; ++i) {
            char chunk[9];
            std::snprintf(chunk, sizeof(chunk), "%08x", static_cast<unsigned>(rd()));
            nonce += chunk;
        }
    }
    std::filesystem::path const stage_dir =
        temp_root / ("ae_seed_" + std::to_string(docker_cli_detail::current_pid()) + "_" + nonce);
#ifdef _WIN32
    // `%LOCALAPPDATA%\Temp` is already per-user on Windows, so there is no shared-directory race
    // to close; `create_directory` fails if the name is taken, which is the property that matters.
    if (!std::filesystem::create_directory(stage_dir, ec) || ec) {
#else
    if (::mkdir(stage_dir.c_str(), 0700) != 0) {
#endif
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "cannot create a private directory to stage the seed archive in",
            "docker_cli_backend.seed_stage_failed"});
    }
    std::filesystem::path const staged = stage_dir / "seed.tar";

    // Removes the whole private directory on EVERY exit path below, including the early failures
    // -- an abandoned archive is a copy of the worktree left lying in the temp directory.
    struct StagedDirGuard {
        std::filesystem::path path;
        ~StagedDirGuard() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } const guard{stage_dir};

    {
        std::ofstream archive(staged, std::ios::binary | std::ios::trunc);
        if (!archive) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::fatal,
                "cannot open a temp file to stage the seed archive: " +
                    agentengine::ustar::detail::path_to_utf8(staged),
                "docker_cli_backend.seed_stage_failed"});
        }
        // Round-2 finding F2: `ofstream` obeys the umask, so the archive was mode 0644 -- a
        // world-readable copy of the entire worktree. The private directory above already denies
        // traversal, but the file's own bits are stated rather than inherited, so the claim in
        // this function's comment is true of the file and not only of its parent.
        std::filesystem::permissions(staged,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
        // That `ec` used to be set and never read, which made the paragraph above a claim rather
        // than a fact: if the call failed, the archive stayed at whatever the umask gave it
        // (0644 was the measured default -- a world-readable copy of the entire worktree) and
        // nothing said so. A hardening step that cannot report its own failure is not hardening.
        if (ec) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::fatal,
                "cannot restrict the staged seed archive to owner-only: " + ec.message(),
                "docker_cli_backend.seed_stage_permissions_failed", ec.value()});
        }
        auto written = agentengine::ustar::write_archive_as_root(host_dir, archive);
        if (!written.has_value()) return std::unexpected(written.error());
        archive.flush();
        if (!archive) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                      "failed writing the staged seed archive",
                                                      "docker_cli_backend.seed_stage_failed"});
        }
    }

    // The literal "-" is this class's own constant, not caller input, so it is deliberately not
    // run through `docker_cli_reject_argv_value()` -- that function rejects a leading dash, which
    // is exactly what this argument is.
    std::vector<std::string> const argv = {"docker", "cp", "-",
                                             inst.container_id + ":" + container_path};
    auto r = docker_cli_detail::run_argv(argv, docker_cli_detail::kProcessTimeoutSeconds,
                                         docker_cli_detail::kOutputSafetyCapBytes, &staged);
    if (r.exit_code != 0) {
        std::error_code size_ec;
        auto const bytes = std::filesystem::file_size(staged, size_ec);
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "docker cp (tar stream to container) failed: " + r.stdout_text +
                " (argv: " + docker_cli_detail::join_argv_for_log(argv) + ", archive bytes: " +
                (size_ec ? std::string("unknown") : std::to_string(bytes)) + ")",
            "docker_cli_backend.seed_tree_failed"});
    }
    return agentengine::result<void>{};
}

agentengine::result<void> DockerCliBackend::copy_from_container(Instance const& inst,
                                                                std::string const& container_path,
                                                                std::filesystem::path const& host_path) {
    if (auto safe = docker_cli_reject_argv_value(container_path, "container_path"); !safe.has_value())
        return std::unexpected(safe.error());
    // See copy_to_container()'s own comment above -- same `path_to_utf8()` reasoning.
    std::string const host_path_utf8 = docker_cli_detail::path_to_utf8(host_path);
    if (auto safe = docker_cli_reject_argv_value(host_path_utf8, "host_path"); !safe.has_value())
        return std::unexpected(safe.error());
    std::vector<std::string> const argv = {"docker", "cp", inst.container_id + ":" + container_path,
                                             host_path_utf8};
    auto r = docker_cli_detail::run_argv(argv);
    if (r.exit_code != 0) {
        // ADR-146 §10: see copy_to_container()'s own comment above -- same reasoning, kept
        // symmetric for whichever direction fails next.
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "docker cp (from container) failed: " + r.stdout_text +
                " (argv: " + docker_cli_detail::join_argv_for_log(argv) + ")",
            "docker_cli_backend.copy_from_failed"});
    }
    return agentengine::result<void>{};
}

agentengine::result<SurfaceRunOutcome> DockerCliBackend::exec(Instance const& inst, std::string const& command) {
    if (auto safe = docker_cli_reject_embedded_nul(command, "command"); !safe.has_value())
        return std::unexpected(safe.error());
    return docker_cli_detail::run_argv({"docker", "exec", inst.container_id, "sh", "-c", command});
    // exit_code intentionally passed through as-is -- a non-zero exit from the CONTAINED command is
    // a normal, meaningful result, never itself a result<>-level error.
}

std::string DockerCliBackend::resolve_image_digest(Instance const& inst) {
    auto r = docker_cli_detail::run_argv({"docker", "inspect", "--format", "{{.Image}}",
                                            inst.container_id});
    if (r.exit_code != 0) return {};
    // `run_argv()` merges stdout and stderr (this file's own convention), so take the LAST non-empty
    // line -- the same reason, and the same shape, as `create()`'s own id extraction.
    std::string digest;
    std::istringstream lines(r.stdout_text);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) digest = line;
    }
    // Only a value that LOOKS like the digest Docker documents itself as printing is reported. A
    // daemon that answered with a warning, a template error, or `<no value>` must produce "not
    // known", not a provenance record naming a string that is not an image.
    if (digest.rfind("sha256:", 0) != 0 || digest.size() != 7 + 64) return {};
    if (digest.find_first_not_of("0123456789abcdef", 7) != std::string::npos) return {};
    return digest;
}

agentengine::ImageDigestKind DockerCliBackend::resolve_image_digest_kind(std::string const& image_id) {
    // `image_id` is this class's own output from `resolve_image_digest()`, already validated to be
    // `sha256:` + 64 hex -- it cannot be a leading-dash argv injection. Re-checked anyway, because
    // that reasoning is about the CALLER and this function is public.
    if (image_id.rfind("sha256:", 0) != 0 || image_id.size() != 7 + 64) {
        return agentengine::ImageDigestKind::unknown;
    }
    if (image_id.find_first_not_of("0123456789abcdef", 7) != std::string::npos) {
        return agentengine::ImageDigestKind::unknown;
    }
    auto r = docker_cli_detail::run_argv({"docker", "image", "inspect", "--format",
                                            "{{.Descriptor.mediaType}}", image_id});
    if (r.exit_code != 0) return agentengine::ImageDigestKind::unknown;
    // LAST non-empty line, for the reason `resolve_image_digest()` takes the last one: `run_argv()`
    // merges stderr into this text, so a daemon warning can precede the answer. A warning cannot be
    // MISTAKEN for the answer -- the match below is exact against known media types -- but it can
    // precede it, and taking the first line would lose it.
    std::string media_type;
    std::istringstream lines(r.stdout_text);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) media_type = line;
    }
    return agentengine::image_digest_kind_from_media_type(media_type);
}

agentengine::result<void> DockerCliBackend::destroy(Instance const& inst) {
    auto r = docker_cli_detail::run_argv({"docker", "rm", "-f", inst.container_id});
    if (r.exit_code != 0) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                      "docker rm failed: " + r.stdout_text,
                                                      "docker_cli_backend.destroy_failed"});
    }
    return agentengine::result<void>{};
}

agentengine::result<DockerCliBackend::OrphanReapReport> DockerCliBackend::reap_orphans() {
    auto listed = docker_cli_detail::run_argv({"docker", "ps", "-a", "--format", "{{.Names}}"});
    if (listed.exit_code != 0) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                      "docker ps failed: " + listed.stdout_text,
                                                      "docker_cli_backend.reap_list_failed"});
    }
    OrphanReapReport report;
    std::istringstream lines(listed.stdout_text);
    std::string name;
    while (std::getline(lines, name)) {
        while (!name.empty() && (name.back() == '\r' || name.back() == '\n')) name.pop_back();
        auto const identity = docker_cli_detail::parse_orphan_identity(name);
        if (!identity) continue;  // not one of ours (wrong prefix, or only coincidentally similar)
        ++report.inspected;
        // kAliveSameProcess AND kUnknown both fail closed here -- only a CONFIRMED gone-or-replaced
        // creator is ever reaped (see check_process_identity()'s own comment for why kUnknown must
        // never be treated as reapable).
        if (docker_cli_detail::check_process_identity(identity->pid, identity->start_key) !=
            docker_cli_detail::ProcessMatch::kGoneOrReplaced) {
            continue;
        }
        auto rm = docker_cli_detail::run_argv({"docker", "rm", "-f", name});
        if (rm.exit_code == 0) {
            ++report.reaped;
        } else {
            report.reap_failures.push_back(name);
        }
    }
    return report;
}

}  // namespace agentengine

namespace agentengine {

agentengine::result<void> DockerExecutionSurface::reset(std::filesystem::path const& host_dir) {
    if (instance_) {
        auto destroyed = docker_.destroy(*instance_);
        if (!destroyed.has_value()) return std::unexpected(destroyed.error());
        instance_.reset();
    }
    auto inst = docker_.create(image_, isolation_);
    if (!inst.has_value()) return std::unexpected(inst.error());
    instance_ = *inst;
    // Issue #80, a cost decision, and a CORRECTED one -- the numbers that first justified this cache
    // were wrong, and are recorded here rather than quietly replaced. The original comment claimed
    // `docker inspect` measured ~480 ms against a ~900 ms warm `docker run -d`, "a >50% regression on
    // every run_command". Those numbers existed only as prose; writing the harness that ADR-176 §10
    // should have had from the start (bench/docker_image_digest_resolution.cpp) measured them, on the
    // same machine, as:
    //
    //     docker exec (what a tool call pays)               123 ms
    //     docker inspect {{.Image}} (the digest)             65 ms
    //     docker image inspect RepoDigests (the kind)       115 ms
    //     docker run -d (a full reset())                    622 ms
    //
    // So `docker inspect` is CHEAPER than the `docker exec` it was claimed to dwarf, and per-command
    // resolution of the digest alone would have been about +9% on a reset()+exec tool call, not >50%.
    // Cross-checked against a shell loop outside this process, which agrees on the ordering and the
    // order of magnitude. The ">50%" figure should not be repeated; it is preserved here only so a
    // reader who saw it elsewhere knows it was retracted.
    //
    // The cache did NOT survive that correction for the digest, and ADR-176 §16 records why the
    // second look changed the answer. Once the real number was ~69 ms rather than ~480 ms, what the
    // cache bought was ~9% of a `reset()` -- itself an under-estimate of a whole tool call -- and what
    // it cost was a documented, known-wrong case: an external re-pull that moved the tag mid-session
    // left the cached digest naming the image the FIRST container ran, attached to a command that ran
    // in a later one. Under **I4** that is not "stale but real" for that effect; it is wrong for that
    // effect, and a provenance record is the one artifact that cannot survive being quietly wrong.
    // Nine percent is not a price worth charging for that.
    //
    // So: RESOLVED EVERY RESET, from the container this surface just created. `resolve_image_digest()`
    // reads `{{.Image}}` off that live container, so the answer describes the thing the next command
    // will actually run in, by construction rather than by assumption.
    //
    // A failed resolution now BLANKS the digest rather than leaving the previous one standing. That is
    // deliberate and is the same rule as everywhere else in this design: empty means "not known", and
    // the previous container's digest is not an answer about this one. `test_execution_surface_image_
    // identity`'s N16 moves a tag between two resets and requires the reported digest AND kind to move
    // with it -- the staleness case, executed.
    resolved_digest_ = docker_.resolve_image_digest(*instance_);
    // ADR-176 §9, and keyed by the DIGEST rather than guarded by a "have we tried yet" flag. The kind
    // is a pure function of the digest, so caching it against the digest it describes makes drift
    // impossible by construction: if the digest ever changes, the kind is re-resolved for the new one;
    // while it does not, nothing is spawned. A first version guarded this inside the digest's own
    // `if (empty)` block, which ADR-176 §11's red-team round broke -- a digest that resolved while its
    // kind lookup failed pinned `image_digest_kind()` to `unknown` for the surface's ENTIRE LIFE, from
    // one transient CLI failure, with the comment above still promising a retry. Keyed this way the
    // retry is automatic and costs nothing in the common case.
    //
    // THIS cache is the one §16 kept. The digest is now re-read every reset (above) and the kind is
    // memoized against it, so the steady state is one CLI spawn per reset rather than two, and the
    // staleness the other cache bought is gone. The kind cannot go stale while keyed this way: a tag
    // that moves changes the digest, which is exactly what re-triggers the lookup.
    if (!resolved_digest_.empty() && kind_resolved_for_ != resolved_digest_) {
        resolved_kind_ = docker_.resolve_image_digest_kind(resolved_digest_);
        kind_resolved_for_ = resolved_digest_;
    } else if (resolved_digest_.empty()) {
        resolved_kind_ = agentengine::ImageDigestKind::unknown;
        kind_resolved_for_.clear();
    }

    // The error_code overload: a status query that cannot be answered is not the same as "the
    // directory is not there", and the throwing one would unwind past this function's own
    // `result<void>` contract (issue #71). A query that fails is reported; only a query that
    // SUCCEEDS and says "absent" takes the nothing-to-seed shortcut.
    std::error_code exists_ec;
    bool const seed_present = std::filesystem::exists(host_dir, exists_ec);
    if (exists_ec) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "cannot determine whether there is anything to seed: " + exists_ec.message(),
            "docker_execution_surface.seed_stat_failed", exists_ec.value()});
    }
    if (!seed_present) return agentengine::result<void>{};  // nothing to seed yet
    // ADR-174 (issue #68): a root-owned tar stream, NOT `docker cp <host path>`. With ADR-171's
    // `--cap-drop ALL` the container's root has neither CAP_DAC_OVERRIDE nor CAP_DAC_READ_SEARCH,
    // and `write_verified()` materializes at mode 0600 -- so a seed carrying host ownership was
    // neither readable NOR writable by the only user the container has, and every turn after the
    // first got a tree it could not touch. `seed_tree_as_root()` takes `host_dir` itself (its
    // CONTENTS are what the archive holds), so the trailing "/." that `docker cp`'s own path
    // grammar needed is gone with it.
    auto seeded = docker_.seed_tree_as_root(*instance_, host_dir, "/workspace");
    if (!seeded.has_value()) return std::unexpected(seeded.error());
    return agentengine::result<void>{};
}

agentengine::result<SurfaceRunOutcome> DockerExecutionSurface::run(std::string const& command) {
    if (!instance_) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                      "reset() must be called before run()",
                                                      "docker_execution_surface.not_reset"});
    }
    return docker_.exec(*instance_, "cd /workspace && " + command);
}

agentengine::result<void> DockerExecutionSurface::drain_to(std::filesystem::path const& host_dir) {
    if (!instance_) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                      "reset() must be called before drain_to()",
                                                      "docker_execution_surface.not_reset"});
    }
    std::error_code mkdir_ec;
    std::filesystem::create_directories(host_dir, mkdir_ec);
    if (mkdir_ec) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "cannot create the host directory to drain into: " + mkdir_ec.message(),
            "docker_execution_surface.host_dir_create_failed", mkdir_ec.value()});
    }
    // `docker cp` only adds, so empty host_dir first: a file the command deleted must not survive here
    // for the caller's scan to commit again (issue #143, execution_surface.hpp's drain_to() contract).
    auto cleared = clear_directory_contents(host_dir, "docker_execution_surface.drain_clear_failed");
    if (!cleared.has_value()) return std::unexpected(cleared.error());
    // Same "/." convention in the other direction: copies /workspace's CONTENTS onto host_dir.
    auto copied = docker_.copy_from_container(*instance_, "/workspace/.", host_dir);
    if (!copied.has_value()) return std::unexpected(copied.error());
    return agentengine::result<void>{};
}

}  // namespace agentengine
