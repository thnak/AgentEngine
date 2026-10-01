#pragma once
// Implements decisions/ADR-209-persistent-shell-sessions.md §4-§5, §7 -- the shared core of a LIVE shell:
// the `PersistentShellSurface` concept a held-container surface conforms to (`SandboxRuntime::run_live()`
// drives it), the per-command outcome, and the cwd/env SNAPSHOT that survives a re-open as data (§5).
// Both the sandboxed tier (`DockerExecutionSurface`'s persistent mode) and the native tier
// (`NativeShellSessionProvider`, pwsh) use the snapshot type and its helpers; only the transport differs.
//
// I3, the whole reason the snapshot is shaped this way: a cwd and an environment the shell reports are
// MODEL-DERIVED DATA. They are only ever replayed back INTO a shell as quoted literals (sh single quotes,
// pwsh base64) -- never used host-side as a spawn cwd, an `exec -w`, a host environment block or an input to
// any grant check (ADR-209 C14).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine {

// ADR-209 §5: what persists across a re-open. `env_set`/`env_unset` are a DIFF against the environment the
// shell had when it was opened (before any replay), sorted by name; replaying them onto a fresh shell
// reproduces the exported environment the last completed command left. `truncated` is set when a cap
// (below) dropped something -- the snapshot is still usable, just incomplete, and the reply says so.
struct ShellSnapshot {
    std::string cwd;
    std::vector<std::pair<std::string, std::string>> env_set;
    std::vector<std::string> env_unset;
    bool truncated = false;

    friend bool operator==(ShellSnapshot const&, ShellSnapshot const&) = default;
};

// ADR-209 §5's caps. A snapshot is held in memory per checkpoint (bounded LRU, the provider's job), so each
// one is bounded too.
inline constexpr std::size_t kShellSnapshotMaxCwdBytes = 4 * 1024;
inline constexpr std::size_t kShellSnapshotMaxVars = 64;
inline constexpr std::size_t kShellSnapshotMaxEnvBytes = 32 * 1024;

// One command's outcome in a live shell. A value, never an error, once the command was handed to the
// shell -- `timed_out`/`shell_lost`/`container_lost` are outcomes the model must see, not failures that
// erase its output.
struct LiveExecOutcome {
    int exit_code = -1;
    std::string output;              // merged stdout+stderr, capped
    std::uint64_t output_bytes = 0;  // the command's full output size, before the cap
    bool output_truncated = false;
    bool timed_out = false;
    // The shell is gone (`exit`, a timeout kill, its own `kill -1`) but the container is not: the workspace
    // is still there to drain, every process is gone, and the next command re-opens (ADR-209 §4.3).
    bool shell_lost = false;
    // The container itself is gone (the timeout kill could not even start, ADR-209 §7): nothing to drain,
    // nothing committed.
    bool container_lost = false;
    // cwd/env after the command, when the shell reported them (absent after a lost shell).
    std::optional<ShellSnapshot> snapshot;
};

// ADR-209 §4: a held execution environment with a live shell in it.
//   open(host_dir, snap) -- DESTROY any existing environment, create a fresh one, seed it from `host_dir`,
//                           start the shell and replay `snap` (when non-null) INSIDE it (§4.2: there is no
//                           partial re-sync; reusing a container would resurrect what a reset removed).
//   exec(cmd, deadline)  -- run one command in the live shell. An error means the command was never handed
//                           to the shell (refused up front, or no live shell); after that, a value.
//   drain_to(host_dir)   -- `ExecutionSurface::drain_to()`'s contract exactly: afterwards `host_dir` holds
//                           exactly the environment's workspace (issue #143).
//   close()              -- destroy the environment. Idempotent; a failure keeps the handle for a retry.
//   is_live()            -- an environment exists AND its shell is usable.
template <class T>
concept PersistentShellSurface = requires(T& t, T const& ct, std::filesystem::path const& host_dir,
                                          std::string const& command, ShellSnapshot const* snapshot,
                                          std::chrono::milliseconds deadline) {
    { t.open(host_dir, snapshot) } -> std::same_as<agentengine::result<void>>;
    { t.exec(command, deadline) } -> std::same_as<agentengine::result<LiveExecOutcome>>;
    { t.drain_to(host_dir) } -> std::same_as<agentengine::result<void>>;
    { t.close() } -> std::same_as<agentengine::result<void>>;
    { ct.is_live() } -> std::same_as<bool>;
};

namespace persistent_shell {

// ADR-209 §5: names never replayed -- they change how the shell itself parses or starts, not what the
// command sees -- plus the shell-maintained ones (`PWD`, `OLDPWD`, `SHLVL`, `_`) the cwd and the shell
// already account for. `LD_*`, `PS*`, `BASH_FUNC_*` are prefixes. Also refuses a name that is not a
// portable identifier, which nothing could replay safely anyway.
[[nodiscard]] bool is_replayable_env_name(std::string_view name);

// Splits a NUL-separated `NAME=value` block (`/proc/self/environ`'s shape). An entry with no `=` is
// skipped. Later duplicates win.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> parse_environ_block(std::string_view block);

// Builds a snapshot from what the shell reported after a command: `env_after` diffed against `env_base` (the
// environment at open), filtered by `is_replayable_env_name()`, capped by the limits above. A cwd over its
// cap is not truncated (a truncated path names a different directory); it is dropped and `truncated` set,
// so a re-open starts in the workspace root.
[[nodiscard]] ShellSnapshot make_snapshot(
    std::string cwd, std::vector<std::pair<std::string, std::string>> const& env_after,
    std::vector<std::pair<std::string, std::string>> const& env_base);

// sh: a POSIX single-quoted literal. Inside single quotes NOTHING is special in a POSIX shell (no `$`, no
// backquote, no backslash, no other quote character), so the only character to handle is `'` itself,
// written as `'\''`. Injection-safe by construction for any byte string without NUL (paths and
// environment values cannot contain NUL).
[[nodiscard]] std::string sh_single_quote(std::string_view value);

// sh replay: `cd -- '<cwd>'` and `export NAME='<value>'` / `unset NAME` lines. Names are re-checked with
// `is_replayable_env_name()` (a name is spliced unquoted, so it must be an identifier). A failing `cd`
// (the directory is gone) is tolerated: the shell stays in the workspace root.
[[nodiscard]] std::string sh_replay_script(ShellSnapshot const& snapshot);

// pwsh replay: every value is base64 and decoded by `[Convert]::FromBase64String` -- never a quoted
// literal, because PowerShell single quotes also close on U+2018-U+201B (ADR-209 §5).
[[nodiscard]] std::string pwsh_replay_script(ShellSnapshot const& snapshot);

}  // namespace persistent_shell

}  // namespace agentengine
