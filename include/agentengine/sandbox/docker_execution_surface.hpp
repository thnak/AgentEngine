#pragma once
// Implements ADR-102 Phase 3 -- `DockerCliBackend`/`DockerExecutionSurface`, the one real
// `ExecutionSurface` conformer this phase ports: a real Docker container, driven entirely by
// shelling out to the real `docker` CLI (`docker run`/`docker cp`/`docker exec`/`docker rm`), never
// a mock.
//
// Ported from docs/planning/proofs/{docker_sandbox/docker_backend.hpp,
// execution_surface/docker_execution_surface.hpp} (ADR-099's own standalone, red-teamed,
// live-Docker-tested prove-phase originals -- kept as-is, these are new files). Real changes made
// during the port:
//   - `probe::DockerBackend` -> `agentengine::DockerCliBackend`, deliberately NOT bare
//     `DockerBackend` -- this type does not conform to (and is not meant to imply conformance to)
//     the real, production `agentengine::SandboxBackend` concept (`sandbox.hpp`, 008 §2a); ADR-101's
//     own, separate, still-Proposed/unjudged `DockerSandboxBackend` wraps the SAME underlying `docker`
//     CLI shape as a REAL `SandboxBackend` conformer -- a real, disclosed future consolidation
//     opportunity (both could eventually share one production Docker-CLI wrapper), not acted on in
//     this phase, which stays deliberately independent of ADR-101 per its own scope decision.
//   - `probe::result<T>`/`probe::error{message, code}` -> the real `agentengine::result<T>`/
//     `agentengine::error{failure_class, message, code}` -- `policy` for the shell-injection-defense
//     rejections (a caller-supplied value that could break out of the intended quoting is refused,
//     matching this codebase's own `failure_class::policy` convention for "denied by policy, never
//     from a model", I3), `fatal` for a `docker` CLI invocation itself failing.
//   - `probe::ExecOutcome` -> `agentengine::SurfaceRunOutcome` (execution_surface.hpp, this phase's
//     own naming decision -- see that file's own top comment for why).
//
// The Linux port (ADR-104) added the `#ifdef _WIN32` split for `run_capture()` and the platform-
// specific `docker_cli_reject_unsafe_for_shell`/`docker_cli_reject_shell_breakout` pair. A same-day
// follow-on (ADR-104 §7, "SINCE WIDENED") added `docker_cli_reject_empty()` (shared) and
// `docker_cli_reject_unsafe_for_unquoted_arg()` (per-platform, real second check on Windows only) --
// see their own comments below for why.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/execution_surface.hpp"

#ifndef _WIN32
// Global scope, matching `ctr_cli_detail`'s own identical declaration
// (containerd_execution_surface.hpp) -- POSIX `environ` is a real global, not something a nested
// namespace `extern` declaration can bind to; declaring it inside `agentengine::docker_cli_detail`
// would instead declare a distinct, unresolvable `agentengine::docker_cli_detail::environ` symbol.
extern char** environ;
#endif

namespace agentengine {

namespace docker_cli_detail {

// ADR-139: matches `ctr_cli_detail::kProcessTimeoutSeconds`/`kOutputSafetyCapBytes`
// (containerd_execution_surface.hpp) exactly -- the sibling `ExecutionSurface` conformer this file
// always should have had parity with. Before this ADR, `run_capture()` had neither a wall-clock
// deadline nor an output cap at all: a model-issued `run_command` reaching a non-terminating
// container process (`tail -f`, `yes`, a backgrounded daemon) hung the calling coroutine forever
// (via popen/pclose blocking until the pipe hit EOF), and unbounded stdout grew this HOST process's
// memory without limit -- a real, reachable I8 gap (CLAUDE.md "sandbox and hostile tests are
// resource-capped") the containerd conformer's own header comment already cites this exact rule for,
// but this file never carried the rule over.
constexpr int kProcessTimeoutSeconds = 30;
constexpr std::size_t kOutputSafetyCapBytes = 1u << 20;  // 1 MiB, merged stdout+stderr

#ifdef _WIN32
// Real `CreateProcessA` + anonymous pipe (stdout AND stderr redirected to the SAME write end,
// reproducing `_popen`'s own "2>&1"-equivalent merge), replacing `_popen`/`_pclose` -- unlike that
// CRT wrapper, this gives a real process HANDLE, so a hung/non-terminating child can actually be
// killed on a wall-clock deadline instead of blocking this call forever, and the read loop below
// caps total bytes retained instead of accumulating without bound.
//
// The spawned process is `cmd.exe /c <command>`, and `command` is itself typically `docker ...`
// (`create()`/`exec()`/etc. below) -- so the process this call actually needs to be able to kill on
// timeout is NOT `cmd.exe` itself but `docker.exe`, a genuine CHILD `cmd.exe` spawns to run it (Windows
// has no exec()-style process-image replacement the way POSIX `sh -c "single simple command"` does, so
// `cmd.exe` always stays alive as a real parent). A first draft of this fix (`TerminateProcess` on just
// `pi.hProcess`, no job object) was independently probed against a real Docker daemon after landing:
// `run_capture("docker exec <id> sh -c \"tail -f /dev/null\"", timeout_seconds=5)` DID return within
// 5s (the coroutine-hang half of the original bug was genuinely fixed) but left `docker.exe` itself
// running as an orphaned HOST process afterward, AND the containerized `tail` process still alive
// inside the container (confirmed via `docker top`) -- the timeout kill never reached anything past
// `cmd.exe`. Fixed with a Job Object (`CREATE_SUSPENDED` + `AssignProcessToJobObject()` BEFORE
// `ResumeThread()`, so `cmd.exe` can never spawn a child before it is job-bound) configured with
// `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`: `TerminateJobObject()` on timeout kills `cmd.exe` AND every
// descendant it spawned (Windows job membership is inherited by children unless a process explicitly
// opts out via `CREATE_BREAKAWAY_FROM_JOB`, which neither `cmd.exe` nor `docker.exe` do), closing the
// exact gap the probe found. `CreateJobObject`/`AssignProcessToJobObject` failing (rare) degrades to
// the prior single-process `TerminateProcess` behavior rather than aborting the whole call -- a
// best-effort kill of the top-level process is still strictly better than none.
//
// `command` is concatenated onto `cmd.exe /c ` RAW -- deliberately NOT re-escaped through the
// Microsoft C runtime argv-quoting algorithm (`native_process_spawn.cpp`'s own
// `detail::quote_one_argument`), even though that looked like the obviously-correct choice at first:
// a real, executed regression found the hard way (this ADR's own build/test pass) that `cmd.exe`'s
// `/c`-remainder parsing does NOT apply CRT-style backslash-before-quote unescaping to what it finds
// there -- it is a completely different, much simpler grammar (roughly: strip one matching outer
// quote pair if the whole remainder is exactly that, otherwise take it verbatim). CRT-quoting a
// `command` string that already contains ITS OWN literal `"..."` (e.g. `docker exec <id> sh -c
// "<cmd>"`, built by this file's own callers) turned every embedded quote into a literal backslash-
// quote PAIR cmd.exe then passed straight through to `docker`/`sh` as two literal characters, silently
// corrupting every quoted argument -- this is exactly what `_popen` itself avoids by NOT re-escaping:
// it hands `command` to `cmd.exe /c` essentially as-is, which is why this file's own existing
// shell-quoting discipline (`docker_cli_win_double_trailing_backslashes`,
// `docker_cli_reject_shell_breakout`, etc.) was always designed and tested against a raw, unescaped
// concatenation -- reproduced here, not reinvented.
[[nodiscard]] inline SurfaceRunOutcome run_capture(std::string const& command,
                                                     int timeout_seconds = kProcessTimeoutSeconds,
                                                     std::size_t output_cap = kOutputSafetyCapBytes) {
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
#else
// Real `posix_spawn("/bin/sh", {"/bin/sh", "-c", command})` + anonymous pipe (stdout AND stderr
// dup2'd to the SAME write end, reproducing the "2>&1" merge the previous `popen((command + "
// 2>&1").c_str(), "r")` shape relied on) + poll()-based bounded read, replacing `popen`/`pclose` --
// unlike that libc wrapper, this exposes the real child pid, so a hung/non-terminating child can
// actually be SIGKILLed on a wall-clock deadline instead of blocking this call forever
// (`ctr_cli_detail::run_argv`, containerd_execution_surface.hpp, is the proven precedent this
// mirrors -- single merged stream here instead of that function's two, since `SurfaceRunOutcome` has
// only one text field to begin with).
[[nodiscard]] inline SurfaceRunOutcome run_capture(std::string const& command,
                                                     int timeout_seconds = kProcessTimeoutSeconds,
                                                     std::size_t output_cap = kOutputSafetyCapBytes) {
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
#endif
[[nodiscard]] inline long current_pid() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

// Monotonic per-process counter, not per-instance: `create()` is a non-static member, but two
// `DockerCliBackend` instances in the same process must never mint the same `pid_seq` pair (that
// would make two live containers indistinguishable to `reap_orphans()`'s own name-parsing below).
//
// SEEDED from a wall-clock nanosecond timestamp, not a fixed 0 -- fixes a REAL name-collision
// regression an independent red-team round found (ADR-108 §5): a purely 0-based counter means TWO
// DIFFERENT process instances compute the IDENTICAL name on each one's own FIRST create() call. That
// is exactly the scenario this whole ADR exists to clean up after -- process P1 creates
// `ae_des_<pid>_1`, crashes before its own destructor runs (orphaning it), the OS later reuses P1's
// exact pid for an unrelated process P2, and P2's own first create() call computes the SAME name
// while P1's still-alive orphan occupies it, so `docker run --name` fails outright instead of
// creating cleanly. Seeding from nanoseconds-since-epoch means two independent process starts collide
// only by an astronomically unlikely coincidence, without needing to parse docker's own error text to
// detect and retry a collision.
inline std::atomic<std::uint64_t> g_next_container_seq{
    static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count())};

// The discoverable marker every container `DockerCliBackend::create()` starts now carries via
// `docker run --name`, mirroring the naming scheme `ContainerdExecutionSurface::reset()` already uses
// for `ctr` container ids (`ae_ces_<pid>_<seq>`) -- kept a DIFFERENT prefix (`ae_des_`, not `ae_ces_`)
// so `reap_orphans()` on one backend can never accidentally match a name only the OTHER backend's own
// scheme produced, even though nothing stops both from running against the same host.
inline constexpr char const* kOrphanNamePrefix = "ae_des_";

// POSIX counterpart of the pid-liveness check `reap_orphans()` needs -- see the Windows overload
// below (under `#ifdef _WIN32`) for why this exists on both platforms, unlike most of this file's
// other platform splits. `kill(pid, 0)` sends no signal, only asks the kernel whether `pid` could be
// signaled at all; `ESRCH` is the one answer that actually means "no such process" -- every other
// outcome (success, or a failure this process lacks permission to fully diagnose, e.g. `EPERM` for a
// pid reused by a different, differently-owned process) is treated as "still alive", failing CLOSED:
// this function is the one gate standing between a caller and destroying a real container, so a wrong
// "dead" answer is the only wrong answer that has a real consequence.
#ifndef _WIN32
[[nodiscard]] inline bool process_is_alive(long pid) {
    if (pid <= 0) return true;
    if (::kill(static_cast<::pid_t>(pid), 0) == 0) return true;
    return errno != ESRCH;
}
#else
// Windows analog: `OpenProcess()` failing with `ERROR_INVALID_PARAMETER` is the one answer Microsoft
// documents as meaning the pid names no process at all; every other failure (most commonly
// `ERROR_ACCESS_DENIED` for a pid this process cannot fully query) fails closed as "assume alive",
// same posture as the POSIX side. A handle that DOES open is further checked via
// `GetExitCodeProcess()` -- a pid can remain a valid, openable handle for a zombie-equivalent
// not-yet-reaped exited process on Windows too, and `STILL_ACTIVE` is the only value that actually
// means "running". DISCLOSED, not solved: pid reuse (the kernel recycling a dead process's pid for an
// unrelated new one before this check runs) is a real, inherent race in ANY pid-liveness check on any
// platform, not specific to this function -- narrowing it further (e.g. a boot-id-scoped identity)
// is real follow-on work, not attempted here.
[[nodiscard]] inline bool process_is_alive(long pid) {
    if (pid <= 0) return true;
    HANDLE const h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) {
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }
    DWORD exit_code = 0;
    bool const got = GetExitCodeProcess(h, &exit_code) != 0;
    CloseHandle(h);
    return !got || exit_code == STILL_ACTIVE;
}
#endif

// ADR-108 §7 pid-reuse-race fix (narrows, not just disclosed): a plain pid-liveness check alone
// cannot tell "the ORIGINAL process that created this container is still running" from "the pid was
// later reused by a completely unrelated process" -- the second case reads as "alive" under
// `process_is_alive()` alone, so a genuinely orphaned container silently stays unreapable forever
// once its pid happens to get recycled by something else. Fixed by embedding, alongside the pid, a
// per-process-INSTANCE "start key" that is (for all practical purposes) never the same across two
// different process instances, even ones sharing the same pid: POSIX reads `/proc/<pid>/stat`'s own
// `starttime` field (ticks since boot -- man proc(5)), Windows reads `GetProcessTimes()`'s
// `lpCreationTime`. `check_process_identity()` below then answers a strictly more specific question
// than plain liveness: "is the SAME process instance that minted this key still running," not just
// "is SOME process running at this pid."
#ifndef _WIN32
// Reads `/proc/<pid>/stat`'s field 22 (`starttime`). Field 2 (`comm`, the process name) is the one
// field in this file that can itself contain spaces or parentheses (man proc(5)) -- every robust
// parser's own convention, reused here, is to find the LAST `)` in the line and count fields from
// there, rather than naively splitting on whitespace from the start. After that `)`, `state` (field
// 3) is the first token; `starttime` (field 22) is therefore the 20th token counting from there.
[[nodiscard]] inline std::optional<std::uint64_t> read_process_start_ticks(long pid) {
    if (pid <= 0) return std::nullopt;
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!f || !std::getline(f, line)) return std::nullopt;
    auto const close_paren = line.rfind(')');
    if (close_paren == std::string::npos) return std::nullopt;
    std::istringstream rest(line.substr(close_paren + 1));
    std::string token;
    for (int i = 0; i < 19; ++i) {
        if (!(rest >> token)) return std::nullopt;
    }
    if (!(rest >> token)) return std::nullopt;
    try {
        return static_cast<std::uint64_t>(std::stoull(token));
    } catch (...) {
        return std::nullopt;
    }
}
[[nodiscard]] inline std::uint64_t current_process_start_key() {
    return read_process_start_ticks(current_pid()).value_or(0);
}
// nullopt here means "could not read a start-key for this pid right now" -- deliberately NOT the
// same thing as "no such process" (`process_is_alive()`, unchanged, is still the one function that
// answers that). A transient read failure (the process exiting between the caller's own
// `process_is_alive()` check and this call) must resolve to "unknown", not "gone" -- the caller
// (`check_process_identity()` below) is the one place that decides what an unreadable key means.
[[nodiscard]] inline std::optional<std::uint64_t> process_start_key_for(long pid) {
    return read_process_start_ticks(pid);
}
#else
[[nodiscard]] inline std::uint64_t current_process_start_key() {
    FILETIME creation{}, exit_t{}, kernel_t{}, user_t{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_t, &kernel_t, &user_t)) return 0;
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}
[[nodiscard]] inline std::optional<std::uint64_t> process_start_key_for(long pid) {
    if (pid <= 0) return std::nullopt;
    HANDLE const h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) return std::nullopt;
    FILETIME creation{}, exit_t{}, kernel_t{}, user_t{};
    bool const ok = GetProcessTimes(h, &creation, &exit_t, &kernel_t, &user_t) != 0;
    CloseHandle(h);
    if (!ok) return std::nullopt;
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}
#endif

enum class ProcessMatch { kAliveSameProcess, kGoneOrReplaced, kUnknown };

// The real, narrowed identity check `reap_orphans()` uses in place of plain `process_is_alive()`.
// Layered ON TOP of that already-audited function, not a replacement for its own "ESRCH/
// ERROR_INVALID_PARAMETER is the one unambiguous 'gone' signal" logic: if no process at all is alive
// at `pid`, the creator is unambiguously gone regardless of any key comparison. Only when a process
// genuinely IS alive at that exact pid does the start-key comparison run, to tell "still the same
// process" apart from "the pid got recycled by something else" -- and if THAT read itself fails
// (e.g. a Windows `ERROR_ACCESS_DENIED` for a different-owner process, or a race where the process
// exits between the two checks), the outcome is `kUnknown`, which `reap_orphans()` treats exactly
// like `kAliveSameProcess` -- fail closed, never destroy on an ambiguous answer.
[[nodiscard]] inline ProcessMatch check_process_identity(long pid, std::uint64_t recorded_start_key) {
    if (!process_is_alive(pid)) return ProcessMatch::kGoneOrReplaced;
    auto const current_key = process_start_key_for(pid);
    if (!current_key.has_value()) return ProcessMatch::kUnknown;
    return *current_key == recorded_start_key ? ProcessMatch::kAliveSameProcess
                                               : ProcessMatch::kGoneOrReplaced;
}

// Extracts the (pid, start_key) `create()` embedded in `name` -- the two decimal segments between
// `kOrphanNamePrefix` and the SECOND following `_` (anything after that, including a third `_`, is
// the free-form seq suffix and not parsed further). Returns nullopt -- not a best-effort partial
// parse -- for anything that isn't a plain, purely decimal run of digits in EITHER segment: a name
// sharing this prefix by coincidence (never actually produced by this class, but not provably
// impossible on a shared host) must never be treated as one of ours just because it starts with the
// right characters. Fails closed (skip, don't reap) rather than guessing.
struct OrphanIdentity {
    long pid = 0;
    std::uint64_t start_key = 0;
};

[[nodiscard]] inline std::optional<OrphanIdentity> parse_orphan_identity(std::string const& name) {
    std::string const prefix = kOrphanNamePrefix;
    if (name.rfind(prefix, 0) != 0) return std::nullopt;
    std::string const rest = name.substr(prefix.size());
    std::string::size_type const sep1 = rest.find('_');
    if (sep1 == std::string::npos) return std::nullopt;
    std::string const pid_str = rest.substr(0, sep1);
    std::string const rest2 = rest.substr(sep1 + 1);
    std::string::size_type const sep2 = rest2.find('_');
    std::string const key_str = (sep2 == std::string::npos) ? rest2 : rest2.substr(0, sep2);
    if (pid_str.empty() || key_str.empty()) return std::nullopt;
    for (char const c : pid_str) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    for (char const c : key_str) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    try {
        long const pid = std::stol(pid_str);
        // REAL, independent-red-team-found finding (ADR-108 §5): `process_is_alive()` casts this
        // value to `pid_t` (POSIX, 32-bit) / `DWORD` (Windows, 32-bit), but `long` is 64-bit on
        // LP64 Linux -- a decimal run that fits in `long` yet exceeds INT32_MAX (no real pid ever
        // reaches that range) silently TRUNCATES on that cast, and the truncated value can
        // coincidentally read as a dead pid even when the original, untruncated value was never a
        // pid at all. Empirically proven exploitable: a foreign container on a shared daemon named
        // e.g. `ae_des_10000000000_x` (a value no real create() call could ever produce, but nothing
        // stops another party from naming a container that on a shared host) got misclassified
        // "confirmed dead" and destroyed. Rejecting anything outside a real pid's possible 32-bit
        // range closes this before the value ever reaches a liveness check, not after.
        if (pid <= 0 || pid > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
        std::uint64_t const key = std::stoull(key_str);
        return OrphanIdentity{pid, key};
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace docker_cli_detail

// Every method below builds a HOST-shell-interpreted command STRING by concatenation (`cmd.exe` on
// Windows, `/bin/sh` on Linux, since `popen`/`_popen` both invoke the platform's own shell) -- a
// caller-supplied value could otherwise break out of the intended quoting and execute
// attacker-controlled commands on the HOST, defeating the very isolation boundary this type exists
// to provide. Defended by REJECTING (not attempting to escape) any such value outright -- a
// NECESSARY, not sufficient, defense; a fully general shell escaper is its own hard problem this
// type does not attempt to solve in full.
// A caller-supplied `std::string` CAN legally hold an embedded NUL byte (it's length-based, not
// null-terminated); every caller below ultimately feeds the built command through `.c_str()` into
// `popen`/`_popen`, which silently truncates at the first NUL -- so a value the guard "approved"
// could differ from what the shell actually runs. Rejected here, once, for every caller, rather than
// folded into each platform's own `dangerous` character set (a plain C-string literal can't itself
// contain a NUL to add to that set). REAL, independent-red-team-found finding (2026-08-29): proven
// non-exploitable for `command` specifically (a NUL there always lands inside the still-open
// `sh -c "..."` region this file builds, so `dash`'s lexer fails closed on the resulting unterminated
// quote rather than running anything) -- fixed anyway as defense-in-depth, since "approved but not
// what actually ran" is a real correctness gap on its own, independent of whether today's specific
// callers happen to make it unexploitable.
[[nodiscard]] inline agentengine::result<void> docker_cli_reject_chars(std::string const& value,
                                                                           char const* what,
                                                                           char const* dangerous) {
    if (value.find('\0') != std::string::npos) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            std::string("refusing to build a shell command: '") + what +
                "' contains an embedded NUL byte (would silently truncate at the C-string boundary)",
            "docker_cli_backend.unsafe_shell_argument"});
    }
    if (value.find_first_of(dangerous) != std::string::npos) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            std::string("refusing to build a shell command: '") + what +
                "' contains a character that could break out of the surrounding quoting",
            "docker_cli_backend.unsafe_shell_argument"});
    }
    return agentengine::result<void>{};
}

// A value that is entirely alnum/`-` (e.g. `--privileged`) satisfies EVERY character-set check below
// on both platforms yet is emitted as a bare, unquoted token positioned exactly where the real
// `docker` CLI parses its own flags (`docker run -d --rm -w /workspace <image> ...`) -- CWE-88
// argument/flag injection, a different and in some ways worse class than a shell-quoting escape,
// since it needs no shell metacharacter at all. REAL, independent-red-team-found finding
// (2026-08-29), empirically proven against the real `docker` CLI on this host: `--privileged` is
// parsed as a flag (proceeds past arg-parsing to a daemon-dial error), while a bogus
// `--not-a-real-flag` is rejected immediately with `unknown flag`. Not live-reachable today (the one
// production caller, `MandatorySandboxProvider::bind_sandbox()`, always hardcodes `image`/
// `container_path` and derives `host_path` from `std::filesystem::temp_directory_path()`, never from
// attacker/model input) -- fixed anyway since this is a shared, reusable function.
//
// Rejects a LEADING DASH specifically, not "must start with alnum": `container_path`/`host_path`
// (POSIX) legitimately start with `/` (e.g. `/workspace`) and are ALSO checked by this same shared
// function at every call site -- an alnum-start requirement (correct for Docker's own image-reference
// grammar) would have broken those real, legitimate values. Only `image` actually needs the stricter
// reference-grammar rule; this weaker, shared rule is what's safe to apply uniformly at every
// call site without a false-positive rejection of a real path.
[[nodiscard]] inline agentengine::result<void> docker_cli_reject_leading_dash(std::string const& value,
                                                                                  char const* what) {
    if (!value.empty() && value[0] == '-') {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            std::string("refusing to build a shell command: '") + what +
                "' starts with '-', which could be parsed as a docker CLI flag instead of a value",
            "docker_cli_backend.unsafe_shell_argument"});
    }
    return agentengine::result<void>{};
}

// A caller-supplied value that is empty is never a legitimate `image`/`host_path`/`container_path` --
// all three are REQUIRED arguments to the `docker` subcommands that consume them. Rejecting it isn't
// merely a "no-op is odd" nicety: for the values embedded UNQUOTED in the generated command (`image`
// in create(), `container_path` in copy_to_container()/copy_from_container()), an empty value
// collapses the two literal spaces surrounding it in the source string into one, SHIFTING every token
// positioned after it by one slot -- e.g. `docker run -d --rm -w /workspace  sh -c "..."` (a double
// space where `image` should have landed) is re-tokenized by the shell as IMAGE=`sh`,
// COMMAND=[`-c`, "..."], not "IMAGE=<empty>, then sh -c ...". REAL, empirically proven (2026-08-29)
// against the real `docker` CLI with a live daemon: `create("")` makes docker attempt to pull a
// nonexistent `sh:latest` image ("pull access denied for sh, repository does not exist") -- proving
// the collapse really does reach the CLI's own argument parser, not just a reasoned-about string. This
// is the same "confuses docker's own argument/flag boundary logic" class `docker_cli_reject_leading_dash`
// exists to prevent, just triggered by the ABSENCE of a token rather than the presence of a dangerous
// one -- a token-count shift, not a token-content attack. Not live-reachable today (same disclaimer as
// `docker_cli_reject_leading_dash`: the one production caller, `MandatorySandboxProvider::bind_sandbox()`,
// always hardcodes non-empty `image`/`container_path`/`host_path` values) -- fixed anyway, on both
// platforms, since this is a shared, reusable function.
[[nodiscard]] inline agentengine::result<void> docker_cli_reject_empty(std::string const& value,
                                                                            char const* what) {
    if (value.empty()) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            std::string("refusing to build a shell command: '") + what +
                "' is empty, which would shift every token positioned after it in the generated "
                "command line instead of producing a single empty argument",
            "docker_cli_backend.unsafe_shell_argument"});
    }
    return agentengine::result<void>{};
}

#ifdef _WIN32
// For image names/paths: NONE of `"&|<>^%` are ever legitimately needed, so reject the whole set.
// Also anchors against leading-dash flag injection (see `docker_cli_reject_leading_dash`'s own
// comment) -- real on this platform too, not Linux-specific. Used for `host_path` at both its call
// sites (copy_to_container()/copy_from_container()): `host_path` is always wrapped in literal double
// quotes there (`docker cp \"<host_path>\" ...`), so it deliberately does NOT go through the
// additional `docker_cli_reject_unsafe_for_unquoted_arg` check below -- a real Windows path can
// legitimately contain a space (e.g. a username with a space in it,
// `C:\Users\John Doe\AppData\Local\Temp\...`, which `std::filesystem::temp_directory_path()` can
// genuinely produce), and that space stays safely inside the quoted region.
[[nodiscard]] inline agentengine::result<void> docker_cli_reject_unsafe_for_shell(
        std::string const& value, char const* what) {
    if (auto safe = docker_cli_reject_chars(value, what, "\"&|<>^%\r\n"); !safe.has_value())
        return safe;
    if (auto safe = docker_cli_reject_empty(value, what); !safe.has_value()) return safe;
    return docker_cli_reject_leading_dash(value, what);
}

// `image` (create()) and `container_path` (copy_to_container()/copy_from_container()) are always
// embedded UNQUOTED in the generated cmd.exe command line -- unlike `host_path` (see the comment
// above), an embedded space/tab here does NOT stay inside a quoted region: it becomes a real argv
// boundary once cmd.exe hands the built command line through to `docker.exe`, whose own Windows argv
// reconstruction follows the same space/tab-delimited, quote-aware convention every standard
// MSVC-CRT-started process (and the Go runtime, which `docker.exe` uses) does. REAL, empirically
// proven (2026-08-29) against a real Docker Desktop daemon and the real `docker.exe` CLI, driven
// through the exact `cmd.exe /c "docker cp ..."` shape `_popen()` itself uses:
// `container_path = "tmp --archive"` made the resulting `docker cp <id>:tmp --archive <dest>`
// invocation apply `--archive` (archive mode, preserving uid/gid) as a REAL `docker cp` flag rather
// than treating it as part of the path -- confirmed both via a direct `docker.exe` invocation and via
// `cmd /c "docker cp ..."` end-to-end (the actual code path this file uses), against a live Docker
// Desktop daemon and a real running container, with the destination directory populated as proof the
// flag really was consumed, not merely accepted as an extra positional argument. `docker cp`'s own
// flag parser (Cobra/pflag, default interspersed-flags behavior) accepts a flag positioned AFTER the
// first positional SRC_PATH argument, not only before it -- a more consequential flag in the same
// family (`-L`/`--follow-link`, "always follow symlinks in SRC_PATH") would let a value shaped this
// way make a copy operation follow a symlink an attacker planted inside the container, a real
// path-escape primitive, not merely a cosmetic argument-count mismatch. This closes a WIDER class than
// `docker_cli_reject_leading_dash` alone can: that check only protects a guarded value's OWN first
// character; this one prevents the value from smuggling in a SECOND, independent argv element
// anywhere after itself. (Checked and ruled out as unnecessary: parenthesis grouping, `%`-expansion,
// `&&`/`||` chaining, and `^`-escaping were re-verified against a real `cmd.exe` in this same pass --
// see this ADR's own residuals section -- and none of them create a NEW gap beyond what the existing
// blacklist plus this whitespace check already close; comma/semicolon were confirmed, empirically,
// NOT to split into separate argv elements for a real Windows console app the way space/tab do, so
// adding them here would be a redundant, unproven-necessary restriction, not a real fix.)
[[nodiscard]] inline agentengine::result<void> docker_cli_reject_unsafe_for_unquoted_arg(
        std::string const& value, char const* what) {
    if (auto safe = docker_cli_reject_unsafe_for_shell(value, what); !safe.has_value()) return safe;
    if (value.find_first_of(" \t") != std::string::npos) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            std::string("refusing to build a shell command: '") + what +
                "' contains whitespace, which could split into a separate, unquoted docker CLI "
                "argument once this value reaches docker's own argv parser",
            "docker_cli_backend.unsafe_shell_argument"});
    }
    return agentengine::result<void>{};
}

// For exec()'s own `command` argument specifically: it is legitimately a shell command meant for the
// CONTAINER's inner `sh` and needs `&|<>` for its own real use (pipes/redirects/backgrounding) --
// rejecting those would defeat exec()'s whole purpose. Only the characters that actually break the
// OUTER cmd.exe quoting this string sits inside (a literal double-quote; `%`/`^`, cmd.exe's own
// escape/expansion quirks) are rejected here.
[[nodiscard]] inline agentengine::result<void> docker_cli_reject_shell_breakout(
        std::string const& value, char const* what) {
    return docker_cli_reject_chars(value, what, "\"%^\r\n");
}

// Windows-only: doubles a TRAILING run of backslashes in `value` before it is embedded inside a
// closing double-quote (`"...<value>\""`) at its call sites -- `host_path` in
// copy_to_container()/copy_from_container(), `command` in exec(). REAL, independent-red-team-found
// finding (2026-08-29), adjacent to but distinct from the argument-injection checks above (this is a
// QUOTING-correctness bug, not an argument-boundary one): the Microsoft C-runtime/Go-runtime
// argv-parsing convention treats a run of N backslashes immediately preceding a `"` specially -- an
// EVEN N contributes N/2 literal backslashes and the `"` toggles quote mode; an ODD N contributes
// (N-1)/2 literal backslashes PLUS one literal `"` character, and quote mode does NOT toggle. Neither
// `host_path` nor `command` was ever guaranteed to end in an even number of backslashes -- a bare
// Windows directory path ending in exactly one `\` (the common case: `std::filesystem::
// temp_directory_path()` itself returns a trailing `\` on this platform) hits the ODD case, which
// SWALLOWS the closing quote this file appends, leaving the rest of the generated command line inside
// an unterminated quoted region. Proven live (2026-08-29) against a real Docker Desktop daemon:
// `copy_to_container()` with a `host_path` ending in a single `\` produced `docker: 'docker cp'
// requires 2 arguments` -- the SRC_PATH and DEST_PATH tokens had merged into one, exactly as this
// analysis predicts (an even nonzero trailing count has a quieter failure mode: the quote still
// toggles correctly, but half the value's own trailing backslashes silently vanish). Doubling the
// trailing run before embedding guarantees the closing `"` this file appends always sees an EVEN count
// immediately before it, so it always toggles correctly and always preserves every one of the value's
// own literal trailing backslashes. Not needed for `image`/`container_path`: those are never wrapped
// in a closing quote at all (see their own call sites), so this specific hazard cannot arise for them
// regardless of what characters they contain.
[[nodiscard]] inline std::string docker_cli_win_double_trailing_backslashes(std::string value) {
    std::size_t trailing = 0;
    while (trailing < value.size() && value[value.size() - 1 - trailing] == '\\') ++trailing;
    value.append(trailing, '\\');
    return value;
}
#else
// POSIX parity, NOT a mechanical port of the Windows character set -- `/bin/sh`'s injection surface
// is structurally different from `cmd.exe`'s, so blacklisting the same characters here would be a
// real, exploitable gap (e.g. a value containing `` ` `` or `$(...)`  triggers HOST-side command
// substitution on Linux even though neither character is special to cmd.exe).
//
// `docker_cli_reject_unsafe_for_shell` covers `image` and `container_path` -- both interpolated
// UNQUOTED into the generated `sh -c` string (see create()/copy_to_container()/copy_from_container()
// below). An unquoted POSIX shell word treats almost every non-alphanumeric character as meaningful
// (word-splitting, globbing, redirection, substitution, ...), so this is a POSITIVE allowlist --
// exactly the grammar Docker's own image-name/path reference already restricts itself to -- rather
// than an attempted blacklist of "every POSIX shell metacharacter", which is the class of bug this
// project treats as a real defect, not an acceptable residual (CLAUDE.md: "security claims need
// positive controls -- a test that cannot fail proves nothing"). DISCLOSED, narrower than the
// Windows side: a `host_path` containing a space is rejected here even though it is legitimate on
// Windows (host_path is quoted below, but reuses this same allowlist-based check for both platforms'
// call sites rather than forking the call site itself) -- fails closed on an unusual path rather than
// silently mishandling it.
[[nodiscard]] inline agentengine::result<void> docker_cli_reject_unsafe_for_shell(
        std::string const& value, char const* what) {
    for (char const c : value) {
        bool const safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                           c == '.' || c == '_' || c == '-' || c == '/' || c == ':' || c == '@';
        if (!safe) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::policy,
                std::string("refusing to build a shell command: '") + what +
                    "' contains a character outside the safe image/path allowlist",
                "docker_cli_backend.unsafe_shell_argument"});
        }
    }
    // See `docker_cli_reject_leading_dash`'s own comment -- an all-allowlisted value like
    // `--privileged` would otherwise pass this function cleanly and be emitted as a bare docker CLI
    // flag, not a value.
    if (auto safe = docker_cli_reject_empty(value, what); !safe.has_value()) return safe;
    return docker_cli_reject_leading_dash(value, what);
}

// POSIX counterpart to the Windows-side `docker_cli_reject_unsafe_for_unquoted_arg` -- kept as a pure
// ALIAS, not a second real check: the allowlist above already excludes every whitespace character
// (space/tab are not in the safe set), so a value that passes `docker_cli_reject_unsafe_for_shell` can
// never contain the token-splitting character the Windows-side function's own comment describes.
// Defined under the SAME NAME on both platforms purely so the shared call sites below
// (create()/copy_to_container()/copy_from_container()) don't need their own `#ifdef` -- verified, not
// assumed, that this is genuinely redundant here rather than a real second defense (CLAUDE.md: don't
// add a check for an input that's already impossible).
[[nodiscard]] inline agentengine::result<void> docker_cli_reject_unsafe_for_unquoted_arg(
        std::string const& value, char const* what) {
    return docker_cli_reject_unsafe_for_shell(value, what);
}

// Covers exec()'s own `command` argument, embedded inside escaped double quotes in the generated
// string (`docker exec <id> sh -c "<command>"`) -- deliberately NOT applied to `host_path` (that one
// goes through the stricter allowlist above at both call sites, unchanged from the Windows version,
// which is why the allowlist function's own comment discloses the host_path residual). Inside POSIX
// double quotes, only `` ` ``, `$`, `"`, and `\` remain special (word-splitting/globbing/`&|<>(){}`
// do NOT apply inside double quotes) -- rejecting exactly those four (plus CR/LF for hygiene,
// matching the Windows side) blocks HOST-shell command substitution and quote breakout while still
// letting `command` use `&|<>` for the CONTAINER's own inner `sh`, preserving this function's
// original intent unchanged from the Windows version.
[[nodiscard]] inline agentengine::result<void> docker_cli_reject_shell_breakout(
        std::string const& value, char const* what) {
    return docker_cli_reject_chars(value, what, "\"$`\\\r\n");
}

// POSIX counterpart to the Windows-side `docker_cli_win_double_trailing_backslashes` -- a pure
// no-op passthrough, not a real transformation: `/bin/sh`'s own quoting rules have no analogue of the
// Windows/CRT trailing-backslash-before-quote special case, and both of this function's real callers
// (`host_path` via `docker_cli_reject_unsafe_for_shell`'s allowlist, `command` via
// `docker_cli_reject_shell_breakout`'s `"\"$\`\\\r\n"` blacklist) already reject a literal backslash
// outright on this platform, so a value reaching this function can never contain one to begin with.
// Defined under the SAME NAME on both platforms so the shared call sites below don't need their own
// `#ifdef`.
[[nodiscard]] inline std::string docker_cli_win_double_trailing_backslashes(std::string value) {
    return value;
}
#endif

// A thin, real wrapper over the `docker` CLI -- create()/exec()/destroy() over `docker run`/
// `docker exec`/`docker rm`, `copy_to_container()`/`copy_from_container()` over `docker cp`.
// Deliberately NOT a `SandboxBackend` conformer (see this file's own top comment).
class DockerCliBackend {
public:
    struct Instance {
        std::string container_id;
    };

    // Real `docker run -d --rm -w /workspace <image> sleep infinity` -- NO bind mount: Docker
    // Desktop restricts host bind-mounts to an explicit GUI-configured "File Sharing" allowlist on
    // many real developer machines (a real environment constraint, not a code defect). Real
    // host<->container data movement uses `docker cp` instead (below), which needs no such
    // allowlist. The container's OWN internal filesystem is still real, still isolated by the
    // kernel's own mount/pid/network namespaces regardless.
    [[nodiscard]] agentengine::result<Instance> create(std::string const& image = "alpine:latest") {
        if (auto safe = docker_cli_reject_unsafe_for_unquoted_arg(image, "image"); !safe.has_value())
            return std::unexpected(safe.error());
        // A discoverable NAME (distinct from the docker-assigned `container_id` this method returns
        // below) -- exists purely so a later process can find and `reap_orphans()` this container if
        // THIS process dies before its own destructor runs (see that method's own comment). Carries
        // this process's own pid AND its start-key (ADR-108 §7 pid-reuse fix -- see
        // `check_process_identity()`'s own comment): a plain pid alone cannot tell "still the same
        // process" from "the pid was later reused by something unrelated", which would otherwise let
        // a genuinely orphaned container stay permanently unreapable. Built entirely from this
        // process's own pid/start-key and an internal counter, never caller/model input, so it already
        // trivially satisfies every character-set check above; no validation call needed for a value
        // this function itself constructs from only digits and literal underscores (CLAUDE.md: don't
        // add a check for an input that's already impossible).
        std::string const name = std::string(docker_cli_detail::kOrphanNamePrefix) +
                                  std::to_string(docker_cli_detail::current_pid()) + "_" +
                                  std::to_string(docker_cli_detail::current_process_start_key()) + "_" +
                                  std::to_string(++docker_cli_detail::g_next_container_seq);
        std::ostringstream cmd;
        cmd << "docker run -d --rm -w /workspace --name " << name << " " << image
            << " sh -c \"mkdir -p /workspace && sleep infinity\"";
        auto r = docker_cli_detail::run_capture(cmd.str());
        if (r.exit_code != 0) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                          "docker run failed: " + r.stdout_text,
                                                          "docker_cli_backend.create_failed"});
        }
        // ADR-146 §11: a REAL, reproduced-on-CI bug found here, not the naive trailing-newline trim
        // this used to be. `run_capture()` merges stdout AND stderr into one stream (by design, see
        // its own header comment). When `image` is not yet cached locally, `docker run` writes
        // multi-line pull-progress noise ("Unable to find image '...' locally", "Pulling from...",
        // "Status: Downloaded newer image for...") BEFORE its own final, single-line, machine-
        // readable container id -- the ONLY output `docker run -d` guarantees on success is that its
        // LAST line is the id. Trimming only trailing whitespace kept that entire pull-progress
        // preamble glued onto the id, which then got embedded verbatim into `copy_to_container()`'s
        // generated `docker cp <id>:<path>` command -- multiple embedded newlines/spaces there is
        // exactly what turned one shell argument into several, producing the CI failure this ADR's
        // own diagnostic finally captured: `docker: 'docker cp' requires 2 arguments`. Fixed by
        // extracting only the LAST NON-EMPTY line of the captured output, unconditionally correct
        // whether or not a pull preamble is present (a cache-hit `docker run` output IS just the id,
        // a single line, so this is a strict generalization, not a special case).
        std::string const trimmed = [&] {
            std::string s = r.stdout_text;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            return s;
        }();
        auto const last_newline = trimmed.find_last_of('\n');
        std::string id = (last_newline == std::string::npos) ? trimmed : trimmed.substr(last_newline + 1);
        while (!id.empty() && id.back() == '\r') id.pop_back();
        return Instance{id};
    }

    // Real `docker cp <host_path> <container>:<container_path>`.
    [[nodiscard]] agentengine::result<void> copy_to_container(Instance const& inst,
                                                                  std::filesystem::path const& host_path,
                                                                  std::string const& container_path) {
        if (auto safe = docker_cli_reject_unsafe_for_shell(host_path.string(), "host_path"); !safe.has_value())
            return std::unexpected(safe.error());
        if (auto safe = docker_cli_reject_unsafe_for_unquoted_arg(container_path, "container_path"); !safe.has_value())
            return std::unexpected(safe.error());
        std::ostringstream cmd;
        cmd << "docker cp \"" << docker_cli_win_double_trailing_backslashes(host_path.string()) << "\" "
            << inst.container_id << ":" << container_path;
        auto r = docker_cli_detail::run_capture(cmd.str());
        if (r.exit_code != 0) {
            // ADR-146 §10: the command itself is included here, not just the daemon's own reply --
            // `test_composed_sandbox_providers_live` failed on ubuntu-latest CI with `docker: 'docker
            // cp' requires 2 arguments` and no visibility into what was actually generated (every
            // input to `cmd` above already passed a strict allowlist, so the malformed shape has to be
            // in something this function assembles, not in unchecked user input). `cmd.str()` is
            // already fully allowlist-safe to log by construction (`host_path`/`container_path` both
            // passed `docker_cli_reject_unsafe_for_shell`/`docker_cli_reject_unsafe_for_unquoted_arg`
            // above before being embedded).
            return std::unexpected(agentengine::error{
                agentengine::failure_class::fatal,
                "docker cp (to container) failed: " + r.stdout_text + " (command: " + cmd.str() + ")",
                "docker_cli_backend.copy_to_failed"});
        }
        return agentengine::result<void>{};
    }

    // Real `docker cp <container>:<container_path> <host_path>`.
    [[nodiscard]] agentengine::result<void> copy_from_container(Instance const& inst,
                                                                    std::string const& container_path,
                                                                    std::filesystem::path const& host_path) {
        if (auto safe = docker_cli_reject_unsafe_for_unquoted_arg(container_path, "container_path"); !safe.has_value())
            return std::unexpected(safe.error());
        if (auto safe = docker_cli_reject_unsafe_for_shell(host_path.string(), "host_path"); !safe.has_value())
            return std::unexpected(safe.error());
        std::ostringstream cmd;
        cmd << "docker cp " << inst.container_id << ":" << container_path << " \""
            << docker_cli_win_double_trailing_backslashes(host_path.string()) << "\"";
        auto r = docker_cli_detail::run_capture(cmd.str());
        if (r.exit_code != 0) {
            // ADR-146 §10: see copy_to_container()'s own comment above -- same reasoning, kept
            // symmetric for whichever direction fails next.
            return std::unexpected(agentengine::error{
                agentengine::failure_class::fatal,
                "docker cp (from container) failed: " + r.stdout_text + " (command: " + cmd.str() + ")",
                "docker_cli_backend.copy_from_failed"});
        }
        return agentengine::result<void>{};
    }

    // Real `docker exec <id> sh -c "<command>"` -- runs INSIDE the container's own isolated
    // filesystem/process namespace, never in this process at all.
    [[nodiscard]] agentengine::result<SurfaceRunOutcome> exec(Instance const& inst, std::string const& command) {
        if (auto safe = docker_cli_reject_shell_breakout(command, "command"); !safe.has_value())
            return std::unexpected(safe.error());
        std::ostringstream cmd;
        cmd << "docker exec " << inst.container_id << " sh -c \""
            << docker_cli_win_double_trailing_backslashes(command) << "\"";
        return docker_cli_detail::run_capture(cmd.str());   // exit_code intentionally passed through
                                                                // as-is -- a non-zero exit from the
                                                                // CONTAINED command is a normal,
                                                                // meaningful result, never itself a
                                                                // result<>-level error
    }

    [[nodiscard]] agentengine::result<void> destroy(Instance const& inst) {
        auto r = docker_cli_detail::run_capture("docker rm -f " + inst.container_id);
        if (r.exit_code != 0) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                          "docker rm failed: " + r.stdout_text,
                                                          "docker_cli_backend.destroy_failed"});
        }
        return agentengine::result<void>{};
    }

    // Closes the "container orphaned on abrupt host-process death or a destructor-time transient
    // `docker rm -f` failure" residual this class's own accompanying `DockerExecutionSurface` comment
    // (ADR-104/ADR-145) has disclosed since it was first written -- true, and unchanged by this
    // method: nothing can run a destructor for a process that no longer exists, and this doesn't
    // retry a failed destructor-time `destroy()` either. What it adds is the "persisting instance ids
    // somewhere reclaimable" follow-on that comment itself pointed at: `create()` now names every
    // container `ae_des_<pid>_<seq>`, so a LATER process (the next invocation of this same tool, a
    // cron-style maintenance call, or a test) can list docker's own container table, find names
    // matching that scheme whose embedded pid is no longer alive, and destroy them for real --
    // exactly `Ledger`'s own orphan-branch precedent (`reclaim_orphaned_branch()`), adapted: there,
    // reclaiming hands back a live handle for continued use; here, nobody is left to continue using an
    // orphaned CONTAINER (no in-process handle ever referenced it), so this is `Ledger::abandon()`'s
    // shape, not `reclaim_orphaned_branch()`'s -- garbage-collection, not resumption.
    //
    // Deliberately NOT called automatically from any constructor/reset()/destructor -- an explicit,
    // caller-invoked maintenance operation (this codebase's own Delegated Decision Seam framing,
    // CLAUDE.md "Feature vs. safety balance"): reaping touches OTHER processes' containers (by
    // definition -- this instance's own live container is never a candidate, `Instance` isn't even
    // consulted here), which is a side effect no `ExecutionSurface` verb's own contract promises.
    struct OrphanReapReport {
        std::size_t inspected = 0;              // ae_des_-prefixed names with a parseable identity
        std::size_t reaped = 0;                  // of those, confirmed gone/replaced and destroyed
        std::vector<std::string> reap_failures;  // confirmed gone/replaced, but `docker rm -f` failed
    };

    [[nodiscard]] agentengine::result<OrphanReapReport> reap_orphans() {
        auto listed = docker_cli_detail::run_capture("docker ps -a --format \"{{.Names}}\"");
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
            auto rm = docker_cli_detail::run_capture("docker rm -f " + name);
            if (rm.exit_code == 0) {
                ++report.reaped;
            } else {
                report.reap_failures.push_back(name);
            }
        }
        return report;
    }
};

// The one real `ExecutionSurface` conformer this phase ports -- wraps `DockerCliBackend` behind the
// generic `reset()`/`run()`/`drain_to()` shape `SandboxRuntime` drives.
//
// HONEST RESIDUAL, disclosed not solved: containers are started via `docker run -d --rm ...
// sleep infinity` -- `--rm` fires on the container's own exit, which `sleep infinity` never triggers
// on its own. If the hosting process crashes/aborts between a successful `reset()` and this
// destructor running, the container is orphaned and keeps running indefinitely, with no id
// persisted anywhere and no reclaim mechanism analogous to `Ledger`'s own `orphaned_branches()`/A7
// design.
//
// CORRECTED (2026-08-28, an independent red-team pass on this port found the prior wording here
// overclaimed): this is NOT limited to an actual process crash. The destructor below discards
// `destroy()`'s own result via `(void)`, with no retry and nothing to retry it later -- a perfectly
// ORDINARY destructor call whose `docker rm -f` transiently fails (daemon contention, a network
// hiccup -- the same transient-failure class `reset()` itself already defends against, by NOT
// clearing `instance_` on a failed `destroy()` there, since a live caller can retry) leaks the
// container just as silently, on a completely normal, non-crash exit path. Confirmed consistent with
// a real, pre-existing orphaned container observed on this development host during this same
// red-team pass (an `alpine:latest` container running the exact `create()` command this class emits,
// with no crash known to have produced it). Not fixed in this pass -- the fix (persisting instance
// ids somewhere reclaimable, mirroring `Ledger`'s own orphan-branch design) is real follow-on work,
// named accurately here rather than left understated a second time.
//
// SINCE ADDED (ADR-108): `DockerCliBackend::reap_orphans()` is exactly that follow-on work.
// `create()` now names every container `ae_des_<pid>_<seq>`; `reap_orphans()` lists them back,
// checks each embedded pid for liveness, and destroys the ones that are dead. Both classes of orphan
// this comment names (real crash, and an ordinary-exit `destroy()` that transiently failed) are
// reachable by it identically -- the container's own name persists on the daemon regardless of which
// path produced it. Deliberately NOT wired to run automatically (see `reap_orphans()`'s own comment
// for why) -- a caller (e.g. `tools/sandboxed_shell_chat.cpp`'s own startup) must invoke it
// explicitly. Still a real residual after this: a container from a run where `reap_orphans()` is
// never subsequently invoked by anything stays leaked forever, same as before -- this closes the
// "no reclaim mechanism exists at all" gap, not "orphans can never accumulate".
class DockerExecutionSurface {
public:
    explicit DockerExecutionSurface(std::string image = "alpine:latest") : image_(std::move(image)) {}

    ~DockerExecutionSurface() {
        if (instance_) { (void)docker_.destroy(*instance_); }
    }
    DockerExecutionSurface(DockerExecutionSurface const&) = delete;
    DockerExecutionSurface& operator=(DockerExecutionSurface const&) = delete;

    // Move constructor resets the moved-FROM `instance_` explicitly -- `std::optional`'s own
    // move-construction semantics only move the CONTAINED value, `has_value()` is unchanged by
    // default, which would otherwise fire a malformed `docker rm -f ` (empty id) from the moved-from
    // object's own destructor.
    DockerExecutionSurface(DockerExecutionSurface&& other) noexcept
        : image_(std::move(other.image_)), docker_(std::move(other.docker_)),
          instance_(std::move(other.instance_)) {
        other.instance_.reset();
    }
    // Move assignment SWAPS rather than overwrite-then-discard: `a = std::move(b)` where `a` already
    // owns a live container must not silently leak `a`'s own instance by simply copying `b`'s state
    // over it with no cleanup attempt. Swapping means `other` (almost always an about-to-be-destroyed
    // moved-from temporary) ends up owning what `this` used to own, and `other`'s own, already-correct
    // destructor performs the real cleanup when it goes out of scope -- no possibly-failing `destroy()`
    // call happens inside this operator at all, so there is no failure path here to mishandle.
    DockerExecutionSurface& operator=(DockerExecutionSurface&& other) noexcept {
        if (this != &other) {
            image_.swap(other.image_);
            std::swap(docker_, other.docker_);
            instance_.swap(other.instance_);
        }
        return *this;
    }

    // `instance_` is only cleared once `destroy()` has actually succeeded -- a transient failure
    // (daemon contention, a network hiccup) leaves it in place, so the object still remembers the
    // leak and a caller retrying `reset()` gets another real attempt at destroying the SAME
    // container, rather than silently starting a second one alongside an orphaned first.
    [[nodiscard]] agentengine::result<void> reset(std::filesystem::path const& host_dir) {
        if (instance_) {
            auto destroyed = docker_.destroy(*instance_);
            if (!destroyed.has_value()) return std::unexpected(destroyed.error());
            instance_.reset();
        }
        auto inst = docker_.create(image_);
        if (!inst.has_value()) return std::unexpected(inst.error());
        instance_ = *inst;

        if (!std::filesystem::exists(host_dir)) return agentengine::result<void>{};  // nothing to seed yet
        // Trailing "/." copies host_dir's CONTENTS into /workspace (which create() already made),
        // not host_dir itself as a nested subdirectory. `generic_string()`, not `string()`, so the
        // result uses forward slashes throughout even on Windows -- `string()` + a literal "/."
        // would produce a mixed-separator path fragile against docker CLI's own path normalization.
        std::filesystem::path const source(host_dir.generic_string() + "/.");
        auto copied = docker_.copy_to_container(*instance_, source, "/workspace");
        if (!copied.has_value()) return std::unexpected(copied.error());
        return agentengine::result<void>{};
    }

    [[nodiscard]] agentengine::result<SurfaceRunOutcome> run(std::string const& command) {
        if (!instance_) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                          "reset() must be called before run()",
                                                          "docker_execution_surface.not_reset"});
        }
        return docker_.exec(*instance_, "cd /workspace && " + command);
    }

    [[nodiscard]] agentengine::result<void> drain_to(std::filesystem::path const& host_dir) {
        if (!instance_) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                          "reset() must be called before drain_to()",
                                                          "docker_execution_surface.not_reset"});
        }
        std::filesystem::create_directories(host_dir);
        // Same "/." convention in the other direction: copies /workspace's CONTENTS onto host_dir.
        auto copied = docker_.copy_from_container(*instance_, "/workspace/.", host_dir);
        if (!copied.has_value()) return std::unexpected(copied.error());
        return agentengine::result<void>{};
    }

private:
    std::string image_;
    DockerCliBackend docker_;
    std::optional<DockerCliBackend::Instance> instance_;
};

static_assert(ExecutionSurface<DockerExecutionSurface>,
              "DockerExecutionSurface must satisfy the real ExecutionSurface concept");

}  // namespace agentengine
