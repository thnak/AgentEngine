#pragma once
// Implements decisions/ADR-209-persistent-shell-sessions.md §4.2, §4.3, §7 (revision 5 §15) --
// `DockerPersistentShellSurface`, the `PersistentShellSurface` conformer over the same `DockerCliBackend` the
// Tier 0 `DockerExecutionSurface` uses. A held container with a live `sh` in it, so `cd`, `export`, a venv and
// background processes survive from one command to the next.
//
// Like `DockerExecutionSurface`, this is NOT a `SandboxBackend` and inherits none of RFC 008's guarantees by
// being one (execution_surface.hpp's ADR-171 statement applies verbatim): it contains its own blast radius
// with ADR-171's deny-all `ContainerIsolation` defaults, and the caller (`LiveShellSandboxProvider`) owns
// every authority decision.
//
// THE CONTAINER (one per open -- §4.2: every open destroys the old container and creates a fresh one, because
// seeding is an additive extract and a reused container would resurrect what a reset removed):
//   PID 1 is a non-forking pause-and-reap loop in the image's own `sh` (`kKeeperScript`): it holds a FIFO
//   open read-write, blocks in the `read -t` builtin and reaps with the `wait` builtin -- no fork in steady
//   state, so pid exhaustion cannot kill it, and as a namespace init with no handlers it ignores SIGKILL sent
//   from inside (a `kill -KILL -1` spares it). Revision 5 replaced the ADR's "static binary bind-mounted
//   read-only" with this: the property is the same, measured live (C7), and it needs no binary shipped and no
//   bind mount (which Docker Desktop restricts).
//
// THE SHELL (revision 5 replaced §7's in-band `printf '%b'` + `eval` + trailer framing with the out-of-band
// file transport ADR-209 §14 item 1 recommended): a detached `docker exec -d` runs `kServerScript`, a loop
// in ONE long-lived `sh` that reads a nonce from a FIFO in a 0700 directory (`/run/.ae_sh`, held in a
// readonly variable), checks the staged command file with `sh -n` (a syntax error in a sourced file would
// otherwise exit the shell), SOURCES it with stdin from /dev/null and output to a file, then writes the exit
// status, `pwd` and its exported environment to a file renamed into place. Each `exec()` is ONE
// `docker exec -i` of `kClientScript`, which stages the command from stdin (no quoting, no argv limit), pokes
// the FIFO, waits for the status file while checking the shell is alive, and prints a length-prefixed header,
// the status record and the capped output.
//
// The framing is NOT a security boundary (§7, unchanged): the command runs as the same uid in the same
// directory and can forge a status file or kill the client. That only corrupts the model's own reply; the
// host never trusts the record for anything but the reply and the snapshot it replays INTO a shell (I3).
// A malformed or missing header is `shell_lost`, so the next command re-opens.
//
// DEADLINE (§7): the host-side `docker exec` is bounded by `run_argv()`'s wall clock. On expiry a second
// `docker exec` runs `kill -KILL -1` inside the container (PID 1 survives; everything else, including the
// shell, dies) -> `shell_lost`, and the workspace is still drained and committed by the caller. If that exec
// cannot even start (pids exhausted) the container is `docker rm -f`'d -> `container_lost`.

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/docker_execution_surface.hpp"
#include "agentengine/sandbox/persistent_shell.hpp"

namespace agentengine {

namespace docker_persistent_shell_detail {

// PID 1. `read -t` and `wait` are builtins in busybox ash; nothing here forks after the loop starts.
inline constexpr std::string_view kKeeperScript =
    "mkdir -p /workspace /run; mkfifo /run/.ae_keep && exec 3<>/run/.ae_keep; rm -f /run/.ae_keep; "
    "while :; do read -t 5 _ <&3; wait; done";

// The live shell. `__ae_*` names live in the shell the command runs in; they are unexported (never in the
// environment record) and `__ae_D` is readonly. The `read -t 0` probe refuses an `sh` without `read -t`
// (dash), whose keeper above would spin.
inline constexpr std::string_view kServerScript = R"AE(umask 077
read -t 0 __ae_probe </dev/null 2>/dev/null; [ $? -ne 2 ] || exit 98
for __ae_c in mkfifo cat mv head wc sleep; do command -v "$__ae_c" >/dev/null 2>&1 || exit 98; done
__ae_D=/run/.ae_sh
mkdir -m 700 "$__ae_D" || exit 97
mkfifo "$__ae_D/ctl" || exit 97
readonly __ae_D
exec 3<>"$__ae_D/ctl"
cd /workspace || exit 97
cat /proc/self/environ > "$__ae_D/base" || exit 97
echo $$ > "$__ae_D/pid.t" && mv -f "$__ae_D/pid.t" "$__ae_D/pid"
while IFS= read -r __ae_n <&3; do
  case "$__ae_n" in ''|*[!0-9a-f]*) continue;; esac
  [ -f "$__ae_D/c.$__ae_n" ] || continue
  if /bin/sh -n "$__ae_D/c.$__ae_n" >"$__ae_D/o.$__ae_n" 2>&1 3<&-; then
    { . "$__ae_D/c.$__ae_n"; } 3<&- </dev/null >"$__ae_D/o.$__ae_n" 2>&1
    __ae_rc=$?
  else
    __ae_rc=2
  fi
  { printf '%s\0' "$__ae_rc"; pwd; printf '\0'; /bin/cat /proc/self/environ; } >"$__ae_D/x.$__ae_n.t" 3<&-
  /bin/mv -f "$__ae_D/x.$__ae_n.t" "$__ae_D/x.$__ae_n"
done
)AE";

// One command. $1 nonce, $2 output cap, $3 "1" to also print the open-time environment.
inline constexpr std::string_view kClientScript = R"AE(D=/run/.ae_sh; n=$1; cap=$2; base=$3
i=0
while [ ! -s "$D/pid" ]; do
  i=$((i+1)); [ "$i" -gt 200 ] && { echo AE_NO_SHELL; exit 90; }
  sleep 0.05
done
spid=$(cat "$D/pid")
kill -0 "$spid" 2>/dev/null || { echo AE_SHELL_LOST; exit 91; }
cat > "$D/c.$n.t" && mv -f "$D/c.$n.t" "$D/c.$n" || { echo AE_STAGE_FAILED; exit 92; }
printf '%s\n' "$n" > "$D/ctl"
while [ ! -e "$D/x.$n" ]; do
  kill -0 "$spid" 2>/dev/null || { echo AE_SHELL_LOST; exit 91; }
  sleep 0.05
done
if [ "$base" = 1 ]; then bl=$(wc -c < "$D/base"); else bl=0; fi
printf 'AE1 %s %s %s\n' "$(wc -c < "$D/x.$n")" "$(wc -c < "$D/o.$n")" "$bl"
cat "$D/x.$n"
[ "$base" = 1 ] && cat "$D/base"
head -c "$cap" "$D/o.$n"
rm -f "$D/c.$n" "$D/x.$n" "$D/o.$n"
)AE";

// After a timeout kill: what the command had printed so far, if the shell's output file is still there.
inline constexpr std::string_view kPartialOutputScript = R"AE(head -c "$2" "/run/.ae_sh/o.$1" 2>/dev/null; :)AE";

// The parsed client reply. `ok == false` means the header was missing or malformed (shell lost).
struct ClientReply {
    bool ok = false;
    int exit_code = -1;
    std::string cwd;
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<std::pair<std::string, std::string>> base_env;
    std::string output;
    std::uint64_t output_bytes = 0;
    std::string marker;  // AE_SHELL_LOST / AE_NO_SHELL / ... when not ok
};

// Pure: parses `kClientScript`'s stdout. Bounded by construction (every length is checked against `raw`).
[[nodiscard]] ClientReply parse_client_reply(std::string_view raw, bool with_base);

}  // namespace docker_persistent_shell_detail

class DockerPersistentShellSurface {
public:
    // The largest command accepted (staged through stdin, so this is a sanity bound, not an argv limit).
    static constexpr std::size_t kMaxCommandBytes = 256 * 1024;
    // Output returned to the caller per command; the rest is counted (`output_bytes`) and dropped.
    static constexpr std::size_t kDefaultOutputCapBytes = 64 * 1024;

    explicit DockerPersistentShellSurface(std::string image = "alpine:latest", ContainerIsolation isolation = {},
                                          std::size_t output_cap_bytes = kDefaultOutputCapBytes)
        : image_(std::move(image)), isolation_(isolation), output_cap_(output_cap_bytes) {}
    ~DockerPersistentShellSurface() { (void)close(); }
    DockerPersistentShellSurface(DockerPersistentShellSurface const&) = delete;
    DockerPersistentShellSurface& operator=(DockerPersistentShellSurface const&) = delete;
    DockerPersistentShellSurface(DockerPersistentShellSurface&& other) noexcept;
    DockerPersistentShellSurface& operator=(DockerPersistentShellSurface&& other) noexcept;

    [[nodiscard]] agentengine::result<void> open(std::filesystem::path const& host_dir, ShellSnapshot const* snapshot);
    [[nodiscard]] agentengine::result<LiveExecOutcome> exec(std::string const& command,
                                                            std::chrono::milliseconds deadline);
    [[nodiscard]] agentengine::result<void> drain_to(std::filesystem::path const& host_dir);
    [[nodiscard]] agentengine::result<void> close();
    [[nodiscard]] bool is_live() const noexcept { return live_ && instance_.has_value(); }
    [[nodiscard]] bool has_container() const noexcept { return instance_.has_value(); }

    // Issue #80 provenance, as `DockerExecutionSurface` reports it: resolved per open.
    [[nodiscard]] std::string_view image() const noexcept { return image_; }
    [[nodiscard]] std::string_view image_digest() const noexcept { return resolved_digest_; }
    [[nodiscard]] agentengine::ImageDigestKind image_digest_kind() const noexcept { return resolved_kind_; }

    // The current container's id, empty when there is none. Observability for tests and audit.
    [[nodiscard]] std::string container_id() const { return instance_ ? instance_->container_id : std::string{}; }

    // ADR-209 C14 instrumentation: the last argv vectors this surface spawned (bounded). A test asserts that no
    // snapshot value (cwd, env) ever reached a host-side argv -- e.g. never an `exec -w <model cwd>`.
    [[nodiscard]] std::deque<std::vector<std::string>> const& argv_log() const noexcept { return argv_log_; }

private:
    [[nodiscard]] SurfaceRunOutcome spawn(std::vector<std::string> argv, int timeout_seconds, std::size_t cap,
                                          std::filesystem::path const* stdin_file = nullptr,
                                          bool* timed_out = nullptr);
    [[nodiscard]] agentengine::result<docker_persistent_shell_detail::ClientReply> send(
        std::string const& command, std::chrono::milliseconds deadline, bool with_base, bool* timed_out,
        std::string* nonce_out);
    void forget_container() noexcept;
    // Host-side check (`docker top`, nothing run in the container) that only PID 1 is still alive (§15.5 M2).
    [[nodiscard]] bool only_init_alive();

    std::string image_;
    ContainerIsolation isolation_{};
    std::size_t output_cap_ = kDefaultOutputCapBytes;
    DockerCliBackend docker_;
    std::optional<DockerCliBackend::Instance> instance_;
    bool live_ = false;
    std::vector<std::pair<std::string, std::string>> base_env_;
    std::string resolved_digest_;
    agentengine::ImageDigestKind resolved_kind_{agentengine::ImageDigestKind::unknown};
    std::deque<std::vector<std::string>> argv_log_;
};

static_assert(PersistentShellSurface<DockerPersistentShellSurface>,
              "DockerPersistentShellSurface must satisfy PersistentShellSurface (ADR-209)");

}  // namespace agentengine
