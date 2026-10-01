#pragma once
// Implements decisions/ADR-209-persistent-shell-sessions.md §9 (build step 7) -- the process half of a HELD native
// shell: one `pwsh` per session, inside its own Job Object, driven out of band through files (the same transport
// shape ADR-209 §15 adopted for the container shell: the command never reaches the shell as a spliced string).
//
//   - `pwsh` only (§9): `bash` here can resolve to WSL, outside any Job Object; `cmd.exe` cannot be framed.
//   - The Job Object (job_object_limits.hpp): KILL_ON_JOB_CLOSE always, breakaway never allowed (neither
//     BREAKAWAY_OK nor SILENT_BREAKAWAY_OK is set), the grant's memory cap (a provider default when the grant
//     has none) and the grant's `max_processes` as the active-process limit. A timeout, a revocation, the
//     session ceiling and `terminate()` all end the session the same way: TerminateJobObject -- every process
//     the shell started, background ones included.
//   - The shell starts at the worktree root (`cwd`, host-authored) with the one-shot providers' minimal
//     environment block (no ambient authority from this host process's own environment). The snapshot's cwd/env
//     are replayed INSIDE pwsh, base64-decoded (§5; never a quoted literal), never used as the spawn cwd or the
//     spawn environment (C14).
//   - NOT confined (ADR-071 §6 item 3, ADR-209 §12 item 6): a command can reach any path this host's user can,
//     and processes the OS starts on the shell's behalf outside the job (WMI, Task Scheduler, `wsl.exe`) are
//     not contained.
//
// Transport, per command: the host writes `c.<nonce>` (UTF-8) and then `r.<nonce>` into a private directory; the
// server loop (kServerScript) dot-sources the command's ScriptBlock in its own scope (so `cd`, `$env:`, functions
// and variables persist), redirects every stream to `o.<nonce>`, and publishes `x.<nonce>` by rename:
// "<rc>\n<base64 cwd>\n" then one "<base64 name> <base64 value>" line per environment variable. A parse error
// is exit 2 and the shell survives; `exit` ends pwsh (shell lost). Nothing host-side trusts the status record
// beyond the model's own reply: it is read bounded and parsed bounded.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/persistent_shell.hpp"
#include "backends/native_jail/job_object_limits.hpp"

namespace agentengine::native_process {

struct NativeShellSessionConfig {
    std::string pwsh_path;          // resolved from a held grant by a PATH scan, never from model output
    std::wstring cwd;               // the worktree root (host-authored)
    std::uint64_t memory_bytes = 0;  // the job's memory cap
    std::uint32_t max_processes = 0;  // the job's active-process limit
    std::size_t output_cap_bytes = 64 * 1024;
    std::chrono::milliseconds startup_deadline{30000};
};

namespace native_shell_detail {
// Parses an `x.<nonce>` status record. Bounded: a malformed record is nullopt, never an out-of-range read.
struct StatusRecord {
    int exit_code = -1;
    std::string cwd;
    std::vector<std::pair<std::string, std::string>> env;
};
[[nodiscard]] std::optional<StatusRecord> parse_status_record(std::string_view raw);
[[nodiscard]] std::optional<std::vector<std::pair<std::string, std::string>>> parse_env_lines(std::string_view raw);
}  // namespace native_shell_detail

class NativePwshSession {
public:
    static constexpr std::size_t kMaxCommandBytes = 256 * 1024;
    static constexpr std::size_t kMaxStatusBytes = 1024 * 1024;

    NativePwshSession() = default;
    ~NativePwshSession() { terminate(); }
    NativePwshSession(NativePwshSession const&) = delete;
    NativePwshSession& operator=(NativePwshSession const&) = delete;

    // Starts a fresh pwsh in a fresh Job Object (terminating any previous one), waits for it to be ready, then
    // replays `snapshot` inside it.
    [[nodiscard]] agentengine::result<void> open(NativeShellSessionConfig const& config, ShellSnapshot const* snapshot);
    // One command. An error means it never reached the shell (not live, NUL, too large); after that, a value.
    [[nodiscard]] agentengine::result<LiveExecOutcome> exec(std::string const& command, std::chrono::milliseconds deadline);
    // Ends the session: TerminateJobObject (every process in it), close the job, remove the private directory.
    void terminate() noexcept;
    [[nodiscard]] bool is_live() const noexcept;

    // Observability for tests and audit.
    [[nodiscard]] DWORD shell_pid() const noexcept { return pid_; }
    [[nodiscard]] std::optional<std::uint32_t> active_processes() const noexcept;
    [[nodiscard]] HANDLE job_handle() const noexcept { return job_ ? job_->native_handle() : nullptr; }
    [[nodiscard]] std::filesystem::path const& private_dir() const noexcept { return dir_; }

private:
    [[nodiscard]] agentengine::result<LiveExecOutcome> send(std::string const& command, std::chrono::milliseconds deadline,
                                                            bool with_snapshot);
    void kill_job() noexcept;

    std::optional<native_jail::JobObjectLimits> job_;
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    std::filesystem::path dir_;
    std::size_t output_cap_ = 64 * 1024;
    std::vector<std::pair<std::string, std::string>> base_env_;
    bool live_ = false;
};

}  // namespace agentengine::native_process
