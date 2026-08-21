// Implements native_process_spawn.hpp. See that header for the spec/ADR citation and scope
// statement (deliberately no AppContainer/seccomp isolation; Job Object resource accounting is
// reused from native_jail as an orthogonal safety net, not an isolation boundary).

#include "backends/native_process/native_process_spawn.hpp"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "backends/native_jail/job_object_limits.hpp"

namespace agentengine::native_process {

namespace {

std::unexpected<agentengine::error> win32_error(char const* what, failure_class klass, char const* code) {
    DWORD last = GetLastError();
    return std::unexpected(agentengine::error{
        klass,
        std::string(what) + " failed: Win32 error " + std::to_string(last),
        code,
    });
}

std::wstring widen(std::string const& utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

struct HandleGuard {
    HANDLE h = nullptr;
    ~HandleGuard() { close_now(); }
    void close_now() {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            h = nullptr;
        }
    }
};

// Fixed, minimal environment block -- 008 §9 G3's "no ambient authority" applies here exactly as it
// does to native_jail_backend.cpp's own build_minimal_environment_block() (this is a deliberate,
// small duplicate rather than a shared header, matching that file's own established per-backend
// pattern for this helper): CreateProcessW(lpEnvironment=nullptr) would inherit this HOST process's
// FULL environment, including anything the launching process set for its own use.
std::wstring build_minimal_environment_block() {
    wchar_t system_root_buf[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"SystemRoot", system_root_buf, MAX_PATH);
    std::wstring root =
        (len > 0 && len < MAX_PATH) ? std::wstring(system_root_buf, len) : std::wstring(L"C:\\Windows");

    wchar_t local_app_data_buf[MAX_PATH]{};
    DWORD lad_len = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data_buf, MAX_PATH);
    wchar_t user_profile_buf[MAX_PATH]{};
    DWORD up_len = GetEnvironmentVariableW(L"USERPROFILE", user_profile_buf, MAX_PATH);

    std::wstring block;
    block += L"SystemRoot=" + root;
    block.push_back(L'\0');
    block += L"Path=" + root + L"\\System32;" + root;
    block.push_back(L'\0');
    if (lad_len > 0 && lad_len < MAX_PATH) {
        block += L"LOCALAPPDATA=" + std::wstring(local_app_data_buf, lad_len);
        block.push_back(L'\0');
    }
    if (up_len > 0 && up_len < MAX_PATH) {
        block += L"USERPROFILE=" + std::wstring(user_profile_buf, up_len);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');  // double-null terminator required by CREATE_UNICODE_ENVIRONMENT
    return block;
}

// Same shape/reasoning as native_jail_backend.cpp's own drain_pipe_bounded (008 §2 item 2 -- an
// unbounded stdout/stderr is a denial-of-service on the host); duplicated per-backend by this
// project's own established convention rather than shared, since each backend's HANDLE lifetime
// story differs slightly.
std::string drain_pipe_bounded(HANDLE read_handle, std::uint64_t cap_bytes, bool& out_truncated) {
    constexpr std::uint64_t kDefaultSafetyCapBytes = 16ull * 1024 * 1024;  // 16 MiB, host-safety floor
    std::uint64_t const cap = cap_bytes > 0 ? cap_bytes : kDefaultSafetyCapBytes;

    std::string out;
    char buf[4096];
    for (;;) {
        DWORD read = 0;
        BOOL ok = ReadFile(read_handle, buf, sizeof(buf), &read, nullptr);
        if (!ok || read == 0) break;
        std::uint64_t remaining = cap > out.size() ? cap - out.size() : 0;
        if (remaining == 0) {
            out_truncated = true;
            break;
        }
        std::uint64_t take = static_cast<std::uint64_t>(read) < remaining
                                  ? static_cast<std::uint64_t>(read)
                                  : remaining;
        out.append(buf, static_cast<std::size_t>(take));
    }
    return out;
}

}  // namespace

namespace detail {

// The documented Microsoft C runtime argv-quoting algorithm
// (https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments), applied per
// argument. This is the ONLY correct way to build a Win32 command line from a real argv: getting it
// wrong is a real injection vector (an unescaped embedded quote/backslash can make one intended
// argument split into two, or inject what looks like a second flag) -- exactly the class of bug
// this project's own MediatedShellRunner grammar work (ADR-001/ADR-015) already treats as
// security-relevant for its OWN (non-process-spawning) parser. Exposed (not anonymous-namespace-
// private) so tests/test_native_process_spawn.cpp can verify it directly against known-correct
// vectors, not only indirectly through a real spawned process's observed behavior.
std::wstring quote_one_argument(std::wstring const& arg) {
    bool const needs_quotes =
        arg.empty() || arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) return arg;

    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            // Every pending backslash must be doubled, then the quote itself escaped.
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
    // Trailing backslashes must be doubled before the closing quote (they would otherwise escape it).
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

}  // namespace detail

result<NativeExecOutcome> spawn_native_process(NativeExecRequest const& request) {
    if (request.program_path.empty()) {
        return std::unexpected(agentengine::error{
            failure_class::contract, "NativeExecRequest.program_path must not be empty",
            "native_process.empty_program_path"});
    }
    if (request.cwd.empty()) {
        return std::unexpected(agentengine::error{
            failure_class::contract,
            "NativeExecRequest.cwd must be a real, already-materialized directory -- "
            "worktree confinement is mandatory even though sandbox isolation is not "
            "(decisions/ADR-071-native-unsandboxed-process-execution-providers.md)",
            "native_process.empty_cwd"});
    }

    std::vector<std::string> argv = request.argv;
    if (argv.empty()) argv.push_back(request.program_path);

    std::wstring cmdline = detail::build_command_line(argv);
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');

    constexpr DWORD kPipeBufferBytes = 1024 * 1024;
    SECURITY_ATTRIBUTES pipe_sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HandleGuard stdout_read, stdout_write, stderr_read, stderr_write;
    if (!CreatePipe(&stdout_read.h, &stdout_write.h, &pipe_sa, kPipeBufferBytes) ||
        !CreatePipe(&stderr_read.h, &stderr_write.h, &pipe_sa, kPipeBufferBytes)) {
        return win32_error("CreatePipe", failure_class::fatal, "native_process.pipe_create_failed");
    }
    SetHandleInformation(stdout_read.h, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read.h, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_write.h;
    si.hStdError = stderr_write.h;
    si.hStdInput = nullptr;  // no stdin -- native providers are not interactive (ADR-071 scope)

    PROCESS_INFORMATION pi{};
    std::wstring env_block = build_minimal_environment_block();
    std::wstring cwd_w = widen(request.cwd);

    // CREATE_SUSPENDED so a Job Object resource cap (below) applies before the entry point runs,
    // matching native_jail_backend.cpp's own reasoning -- but NO SECURITY_CAPABILITIES attribute
    // list here, deliberately: this is the isolation ADR-071 does not provide.
    BOOL created = CreateProcessW(nullptr, mutable_cmdline.data(), nullptr, nullptr,
                                   /*bInheritHandles=*/TRUE,
                                   CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                   env_block.data(), cwd_w.c_str(), &si, &pi);
    stdout_write.close_now();
    stderr_write.close_now();
    if (!created) {
        return win32_error("CreateProcessW", failure_class::fatal, "native_process.create_process_failed");
    }
    HandleGuard process_guard{pi.hProcess};
    HandleGuard thread_guard{pi.hThread};

    // Job Object: best-effort resource accounting, reused from native_jail (no AppContainer half).
    // Always created -- even an all-unset cap set still gets a wall-clock safety ceiling, matching
    // this file's own "never literally unbounded" rule for output above.
    native_jail::JobObjectLimits job;
    ResourceLimits limits;
    limits.cpu_ms = request.cpu_ms_cap.value_or(0);
    limits.wall_ms = request.wall_ms_cap.value_or(0);
    limits.memory_bytes = request.memory_bytes_cap.value_or(0);
    auto job_created = job.create(limits);
    if (!job_created.has_value()) {
        TerminateProcess(pi.hProcess, 1);
        return std::unexpected(job_created.error());
    }
    auto assigned = job.assign_process(pi.hProcess);
    if (!assigned.has_value()) {
        TerminateProcess(pi.hProcess, 1);
        return std::unexpected(assigned.error());
    }

    ResumeThread(pi.hThread);

    constexpr std::uint64_t kDefaultWallMsSafetyCeiling = 5ull * 60 * 1000;  // 5 minutes
    auto wait_result = job.wait_or_kill(
        pi.hProcess,
        std::chrono::milliseconds{static_cast<std::int64_t>(
            request.wall_ms_cap.value_or(kDefaultWallMsSafetyCeiling))});
    if (!wait_result.has_value()) return std::unexpected(wait_result.error());
    native_jail::JobWaitOutcome const& wait_outcome = *wait_result;

    NativeExecOutcome outcome;
    outcome.stdout_text =
        drain_pipe_bounded(stdout_read.h, request.output_cap_bytes, outcome.stdout_truncated);
    outcome.stderr_text =
        drain_pipe_bounded(stderr_read.h, request.output_cap_bytes, outcome.stderr_truncated);

    if (wait_outcome.kill_reason == native_jail::job_kill_reason::wall_clock_timeout) {
        outcome.klass = native_exec_outcome_class::timeout;
    } else if (wait_outcome.kill_reason == native_jail::job_kill_reason::memory_limit) {
        outcome.klass = native_exec_outcome_class::oom;
    } else if (wait_outcome.exit_code == 0) {
        outcome.klass = native_exec_outcome_class::ok;
    } else {
        outcome.klass = native_exec_outcome_class::nonzero_exit;
    }
    outcome.exit_code = static_cast<int>(wait_outcome.exit_code);
    return outcome;
}

}  // namespace agentengine::native_process
