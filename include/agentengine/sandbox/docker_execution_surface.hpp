#pragma once
// ADR-207 (#120 S7): the non-template bodies are in src/sandbox/docker_execution_surface.cpp, compiled once, and
// took <windows.h> and the POSIX process headers with them. Every declaration and comment stays here.
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
// specific shell-breakout denylist pair this file originally carried.
//
// UPDATE (docker-execution-surface-argv-hardening, closing issue #50): `run_capture()` and its
// platform-specific shell-quoting denylist are RETIRED as `DockerCliBackend`'s own internal transport --
// every subcommand now builds a real `std::vector<std::string>` argv and spawns `docker`/`docker.exe`
// DIRECTLY via the new `run_argv()` (this namespace, both platforms), never through `cmd.exe`/`/bin/sh`,
// matching `ContainerdCliBackend`'s own already-shipped shape (containerd_execution_surface.hpp)
// exactly. `run_capture()` itself is KEPT, unchanged, purely because real test files
// (`tests/sandbox/execution_surface/test_docker_orphan_reap.cpp`, `tests/sandbox/test_sandbox_runtime.cpp`) call it directly, with
// static, non-attacker-influenced strings, for host-side setup/assertions outside the code path under
// test -- it is no longer used by any production call site in this file. See the class-level comment
// on `DockerCliBackend` below and `docs/planning/docker-execution-surface-argv-hardening-design-draft.md`
// for the full before/after reasoning and red-team round.
//
// ==== WHAT THIS FILE IS, AND IS NOT (ADR-171, GitHub issue #63) ==================================
//
// NEITHER `DockerCliBackend` NOR `DockerExecutionSurface` IS A `SandboxBackend` (`sandbox.hpp`,
// 008 §2a), and neither is reachable through 008 §3's profile resolution (`Strict`,
// `SandboxBackendRegistry`). An agent declaring `SandboxProfile<P>` (002 §3) has no path to select
// Docker-backed execution through the normal machinery, by design and by explicit project-owner
// direction (ADR-099 §7). **Living under `include/agentengine/sandbox/` does not confer RFC 008's
// guarantees**, and issue #63 was filed precisely because a reader could reasonably assume it does.
//
// This file therefore does NOT satisfy 008 §2's backend contract, and has not cleared 008 §9's
// promotion gate (G1 parity / G2 containment / G3 no-ambient-authority / G4 teardown). What ADR-171
// changed is narrower and worth stating exactly: before it, `create()` emitted a `docker run` with
// **no isolation flags at all** -- unfiltered bridge egress, unbounded memory/pids/CPU, the default
// Linux capability set. That is now closed and proven against a live daemon by reading the container's
// OWN cgroup values from inside it (`tests/sandbox/execution_surface/test_docker_isolation.cpp`), not by trusting a flag string.
//
// STILL MISSING for a real 008 §2 backend, so nobody has to re-derive it:
//   - NO `CapabilitySet` mediation. `create()` takes no `EffectContext` and consults no capability;
//     008 §2 rule 1 ("empty-by-default authority", I2) is unsatisfied. THIS is the reason the type
//     is not a backend, and no amount of `docker run` flags fixes it.
//   - NO `NetPolicy` allowlist. `container_isolation_from()` REFUSES one rather than silently
//     widening it to full bridge access -- see that function's own comment.
//   - `ResourceLimits::cpu_ms`/`fds`/`disk_bytes`/`net_bytes` are unmapped; `wall_ms`/`output_bytes`
//     are enforced host-side by `run_argv()` (ADR-139), not by the container.
//   - `SandboxSpec::mounts` is not honored at all -- this surface moves data with `docker cp`.
//
// The identically-shaped sibling `ContainerdCliBackend` (containerd_execution_surface.hpp) still has
// the pre-ADR-171 gap: grepping it for isolation flags returns nothing. Not fixed here (its `ctr`
// flag syntax differs and this session could not execute a live containerd proof) -- tracked
// separately rather than silently fixed or silently ignored.

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
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


#include "agentengine/core/error.hpp"
// ADR-203 (#120 S8): the orphan-reaping process-identity check, shared with containerd_execution_surface.hpp.
#include "agentengine/sandbox/detail/process_identity.hpp"
#include "agentengine/sandbox/execution_surface.hpp"
// ADR-171 (issue #63): for `ResourceLimits`/`NetPolicy` -- the 008 §2 vocabulary
// `container_isolation_from()` bridges into real `docker run` flags. No cycle: `sandbox.hpp` does not
// include this file, and this file still does NOT make anything here a `SandboxBackend` conformer
// (see the top-of-file scope statement).
#include "agentengine/sandbox/sandbox.hpp"
// ADR-174 (issue #68): the root-owned ustar archive `seed_tree_as_root()` streams into `docker cp -`.
#include "agentengine/sandbox/ustar_writer.hpp"

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
[[nodiscard]] SurfaceRunOutcome run_capture(std::string const& command,
                                            int timeout_seconds = kProcessTimeoutSeconds,
                                            std::size_t output_cap = kOutputSafetyCapBytes);

// Widens a UTF-8 string to UTF-16 for CreateProcessW/the MS-CRT argv-quoting algorithm below --
// duplicated from `native_process/native_process_spawn.cpp`'s own identical helper rather than linked
// against: that file's own header comment already establishes this project's convention of
// duplicating small per-backend process-spawn helpers instead of sharing them ("duplicated per-backend
// ... since each backend's HANDLE lifetime story differs slightly"), and here there's a second, sharper
// reason to follow it -- `native_process_spawn.cpp` is only ever compiled when the ADR-071
// `AGENTENGINE_WITH_NATIVE_PROCESS` option is explicitly enabled (off by default, CMakeLists.txt).
// Linking this always-built, header-only file against that optional target would make
// `DockerExecutionSurface`'s own build silently depend on an unrelated, opt-in capability class.
[[nodiscard]] std::wstring widen(std::string const& utf8);

// code-review finding (post-ADR-165): `widen()` above documents its input as UTF-8, but
// `std::filesystem::path::string()` on Windows narrows via the process's ACTIVE CODE PAGE (`GetACP()`),
// never UTF-8 -- the OLD `run_capture()`/`CreateProcessA` path was internally ANSI-consistent (an ACP
// string handed to an ANSI API), so this mismatch was never live; the new `run_argv()`/`CreateProcessW`
// path made it real: a `host_path` containing a non-ASCII byte would silently mis-decode through
// `widen()`, corrupting the `docker cp` argv element for any non-ASCII path. Matches this codebase's own
// already-established fix for the identical class of bug -- `native_process/native_providers.hpp`'s own
// `detail::narrow()` (the same `WideCharToMultiByte(CP_UTF8, ...)` call, mirrored here) is paired with
// converting FROM a path's native `wstring()`, never its ACP-narrowed `string()`. This function is that
// same fix, scoped to this file: converts a path's TRUE Unicode content (`wstring()`, lossless) to UTF-8
// (matching `widen()`'s own documented contract), never through the lossy ACP `string()` narrowing.
[[nodiscard]] std::string path_to_utf8(std::filesystem::path const& p);

// The documented Microsoft C runtime argv-quoting algorithm
// (https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments), applied per
// argument -- the ONLY correct way to build a real Win32 command line from a real argv vector, and the
// primitive that makes `run_argv()` below possible: every argument is quoted independently and joined
// with single spaces, so a value containing `"`, `%`, `^`, or embedded whitespace lands as ONE argv
// element on the far side, with no host-shell reparsing step left for it to escape. Same algorithm as,
// and independently re-verifiable against the same test vectors as,
// `native_process/native_process_spawn.cpp`'s own `quote_one_argument()`/`build_command_line()`
// (exposed there for exactly this reason -- "the same class of bug this project's own
// MediatedShellRunner grammar work already treats as security-relevant") -- duplicated here per the
// `widen()` comment above, not reused via that file's own, differently-gated build target.
[[nodiscard]] std::wstring quote_one_argument(std::wstring const& arg);

[[nodiscard]] std::wstring build_command_line(std::vector<std::string> const& argv);

// Argv-based sibling of `run_capture()` above -- spawns `argv[0]` (e.g. `"docker"`) DIRECTLY, never
// through `cmd.exe`, and never through a reconstructed command STRING: every element of `argv` is
// quoted independently by `build_command_line()` above, so a caller-supplied value (most importantly
// `exec()`'s own `command` argument, see `DockerCliBackend::exec()` below) can no longer break out of
// any surrounding quoting, because there is no longer a shell in front of it to escape from. This is
// the fix for the real, live-tested defect this file's own top comment and issue #50 both describe: a
// model-issued command containing `"`/`%`/`^` no longer needs to be rejected, because those characters
// were only ever dangerous relative to `cmd.exe`'s own reinterpretation of a shell STRING, and that
// reinterpretation step no longer exists.
//
// `lpApplicationName = nullptr` (like `spawn_native_process()`'s own identical choice,
// `native_process_spawn.cpp:209`): Win32's own standard module-search order, including `%PATH%`,
// resolves `argv[0]` the same way `cmd.exe`'s own lookup did before -- this is NOT a new grant of
// ambient authority, it is the exact same PATH-based resolution `docker`/`cmd.exe` always used,
// matching `ctr_cli_detail::run_argv()`'s own `posix_spawnp()` precedent on the POSIX side below (see
// this design's own red-team round for why requiring a pre-resolved absolute path here, matching
// `native_process::NativeExecRequest`'s stricter I2 posture, would be an inconsistency this change
// introduces rather than a gap it closes -- that posture is specific to ADR-071's deliberately
// weaker-isolation native-automation capability class, not a general rule this subsystem is bound by).
//
// Reuses the exact Job-Object timeout-kill machinery `run_capture()` above already carries (ADR-104) --
// see that function's own header comment for the real host-process-orphaning bug that machinery closes.
// Removing the `cmd.exe` layer here means the Job Object now binds THIS function's own top-level
// spawned process (`argv[0]` itself, e.g. `docker.exe`) directly instead of binding `cmd.exe` and
// relying on job-membership inheritance to reach its real child -- fewer moving parts, not a weaker
// guarantee: `CREATE_SUSPENDED` + `AssignProcessToJobObject()` before `ResumeThread()` still guarantees
// there is no window where the spawned process could do anything before it is job-bound.
//
// DISCLOSED, PRE-EXISTING, NOT a regression (ADR-165 red-team round, real bisection): Windows'
// `CreateProcessW` rejects an `lpCommandLine` longer than roughly 32K characters outright, and this
// function surfaces that failure identically to any other spawn failure -- `exit_code == -1`, empty
// output, no distinguishing error. The OLD `"cmd.exe /c " + command` path hit the same OS ceiling at an
// even SMALLER effective `command` length (the literal `"cmd.exe /c "` prefix ate into the same 32K
// budget), so this is not new; an extremely long `command`/argv value was always silently
// indistinguishable from an ordinary spawn failure on this platform, before and after this port.
// ADR-174 (issue #68): `stdin_file`, when non-null, is opened and handed to the child as its stdin.
// A FILE rather than a pipe we feed, deliberately. The first implementation wrote the archive into a
// pipe from a dedicated thread, which worked but needed a writer thread, SIGPIPE masking, a join
// ordered after the kill, and an in-memory copy of the whole archive -- four mechanisms, each with
// its own failure mode, to solve a problem the OS already solves. Redirecting stdin from a file the
// caller streamed to disk has none of them: no thread, no pipe, no signal handling, no deadlock to
// reason about, and peak memory independent of tree size. Callers that pass nullptr get
// byte-identical behaviour to before, `hStdInput` null included.
[[nodiscard]] SurfaceRunOutcome run_argv(std::vector<std::string> const& argv,
                                         int timeout_seconds = kProcessTimeoutSeconds,
                                         std::size_t output_cap = kOutputSafetyCapBytes,
                                         std::filesystem::path const* stdin_file = nullptr);
#else
// POSIX counterpart to the Windows-side `path_to_utf8()` -- a pure passthrough, not a real conversion:
// POSIX paths are already just byte sequences (by this codebase's own established convention, e.g.
// `ctr_cli_detail`'s own `host_dir.generic_string()` usage, containerd_execution_surface.hpp), with no
// Windows-style ACP-vs-UTF-8 distinction for `std::filesystem::path::string()` to get wrong. Defined
// under the SAME NAME on both platforms so the shared call sites below don't need their own `#ifdef`.
[[nodiscard]] inline std::string path_to_utf8(std::filesystem::path const& p) { return p.string(); }

// Real `posix_spawn("/bin/sh", {"/bin/sh", "-c", command})` + anonymous pipe (stdout AND stderr
// dup2'd to the SAME write end, reproducing the "2>&1" merge the previous `popen((command + "
// 2>&1").c_str(), "r")` shape relied on) + poll()-based bounded read, replacing `popen`/`pclose` --
// unlike that libc wrapper, this exposes the real child pid, so a hung/non-terminating child can
// actually be SIGKILLed on a wall-clock deadline instead of blocking this call forever
// (`ctr_cli_detail::run_argv`, containerd_execution_surface.hpp, is the proven precedent this
// mirrors -- single merged stream here instead of that function's two, since `SurfaceRunOutcome` has
// only one text field to begin with).
[[nodiscard]] SurfaceRunOutcome run_capture(std::string const& command,
                                            int timeout_seconds = kProcessTimeoutSeconds,
                                            std::size_t output_cap = kOutputSafetyCapBytes);

// Argv-based sibling of `run_capture()` above, adapted from `ctr_cli_detail::run_argv()`
// (containerd_execution_surface.hpp:85-213 -- the already-shipped, ADR-145 precedent this port
// follows) -- same `posix_spawnp()`/timeout/output-cap discipline, but merges stdout+stderr into ONE
// stream, matching `SurfaceRunOutcome`'s own single-field shape and `run_capture()`'s own existing
// merge convention (unlike `ctr_cli_detail::ProcessOutcome`'s two separate fields): nothing downstream
// of `DockerCliBackend` needs the streams kept separate, and widening `SurfaceRunOutcome` itself is out
// of scope for this change. `posix_spawnp()` (the `p`-suffixed, PATH-searching variant) resolves
// `argv[0]` (e.g. `"docker"`) exactly the way it already does for `ctr` in the sibling file -- not a
// new posture, a deliberately consistent one (see this file's own Windows-side `run_argv()` comment).
// ADR-174 (issue #68): `stdin_file`, when non-null, replaces `/dev/null` as the child's stdin -- see
// the Windows sibling's comment for why a file rather than a pipe this process feeds. `addopen` does
// the work in the CHILD after fork, so there is no parent-side descriptor to leak, close or set
// FD_CLOEXEC on. Callers that pass nullptr get byte-identical behaviour to before.
[[nodiscard]] SurfaceRunOutcome run_argv(std::vector<std::string> const& argv,
                                         int timeout_seconds = kProcessTimeoutSeconds,
                                         std::size_t output_cap = kOutputSafetyCapBytes,
                                         std::filesystem::path const* stdin_file = nullptr);
#endif
using detail::process_identity::current_pid;

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

// The process-identity check `reap_orphans()` runs on every `ae_des_` name (ADR-108 §5/§7): one implementation,
// shared with containerd_execution_surface.hpp since ADR-203 (#120 S8), in sandbox/detail/process_identity.hpp.
// These names stay spelled as they always were.
using detail::process_identity::check_process_identity;
using detail::process_identity::current_process_start_key;
using detail::process_identity::OrphanIdentity;
using detail::process_identity::process_is_alive;
using detail::process_identity::process_start_key_for;
using detail::process_identity::ProcessMatch;
#ifndef _WIN32
using detail::process_identity::read_process_start_ticks;
#endif

[[nodiscard]] inline std::optional<OrphanIdentity> parse_orphan_identity(std::string const& name) {
    return detail::process_identity::parse_orphan_identity(name, kOrphanNamePrefix);
}

// Diagnostic-only: joins an argv vector with spaces for an error message (ADR-146 §10's own "log what
// was actually generated" posture, adapted for a real argv vector instead of a single command string).
// Every real caller only invokes this AFTER every element has already passed
// `docker_cli_reject_argv_value()`/`docker_cli_reject_embedded_nul()`, so this is safe to log verbatim
// by construction -- not itself a validation step, and not meant to be re-parsed by anything.
[[nodiscard]] std::string join_argv_for_log(std::vector<std::string> const& argv);

}  // namespace docker_cli_detail

// ADR (docker-execution-surface-argv-hardening) -- every method below now builds a REAL argv vector
// (`docker_cli_detail::run_argv()` above) instead of a host-shell-interpreted command STRING. There is
// no longer a `cmd.exe`/`/bin/sh` sitting in front of the OUTER `docker` invocation for a
// caller-supplied value to break out of -- so the platform-split shell-breakout denylist this comment
// used to introduce (`docker_cli_reject_unsafe_for_shell`'s POSIX allowlist,
// `docker_cli_reject_unsafe_for_unquoted_arg`'s Windows whitespace check, `docker_cli_reject_shell_breakout`,
// `docker_cli_win_double_trailing_backslashes`) is REMOVED, not narrowed: none of it was ever a real
// defense against anything other than that now-gone shell layer, and it is exactly what made a
// legitimate model command containing `"`/`%`/`^` get rejected as "unsafe" (issue #50). What survives,
// unified across both platforms because the risk itself was never platform-specific, is:
//
//   - `docker_cli_reject_embedded_nul()` -- a `std::string` can legally hold an embedded NUL byte;
//     every argv element below ultimately reaches the OS's own NUL-terminated-buffer boundary
//     (`execve()`'s `argv[]` on POSIX, the wide command-line buffer `CreateProcessW` reads on
//     Windows), which would silently truncate at the first NUL -- an "approved" value could then
//     differ from what actually runs. Matches `ctr_cli_detail::reject_embedded_nul()`'s own identical
//     reasoning (containerd_execution_surface.hpp) -- the one check that already IS that file's entire
//     defense against `exec()`'s own `command` argument, now also this file's only defense against it.
//   - `docker_cli_reject_leading_dash()` -- unchanged in substance, still real: a leading `-` is read
//     as a `docker` CLI FLAG by `docker`'s OWN argument parser regardless of which OS spawned the
//     process or whether a shell sits in front of it at all (REAL, empirically proven 2026-08-29
//     against a live `docker` CLI). Applied to `image`/`host_path`/`container_path`, never to
//     `exec()`'s `command` (which is `sh -c`'s own positional argument, not something `docker`'s own
//     flag parser ever scans).
//   - `docker_cli_reject_empty()` -- kept as a fail-fast correctness check, but no longer a security
//     boundary: its original rationale (an empty value collapsing two literal spaces in a
//     concatenated STRING, shifting every later token) was purely an artifact of string concatenation.
//     A real argv vector has no such hazard -- an empty argv element is just its own, separate,
//     fixed-position slot regardless of content. Kept because rejecting it here still produces a
//     clearer error than `docker`'s own (e.g. attempting to pull image `""` as a nonexistent
//     `sh:latest`, the exact confusing failure this function's history already documented).
[[nodiscard]] agentengine::result<void> docker_cli_reject_embedded_nul(std::string const& value,
                                                                                   char const* what);

[[nodiscard]] agentengine::result<void> docker_cli_reject_leading_dash(std::string const& value,
                                                                                  char const* what);

[[nodiscard]] agentengine::result<void> docker_cli_reject_empty(std::string const& value,
                                                                            char const* what);

// Combined check for `image`/`host_path`/`container_path` -- every value that becomes its own argv
// element (never embedded in a shell-parsed string) needs exactly these three, platform-independent
// checks, nothing more. Deliberately NOT applied to `exec()`'s own `command` argument -- that one only
// needs `docker_cli_reject_embedded_nul()` alone, matching `ctr_cli_detail`'s own precedent (see this
// section's own top comment).
[[nodiscard]] inline agentengine::result<void> docker_cli_reject_argv_value(std::string const& value,
                                                                                 char const* what) {
    if (auto ok = docker_cli_reject_embedded_nul(value, what); !ok.has_value()) return ok;
    if (auto ok = docker_cli_reject_empty(value, what); !ok.has_value()) return ok;
    return docker_cli_reject_leading_dash(value, what);
}

// ---- ADR-171 (GitHub issue #63): real, fail-closed container isolation --------------------------
//
// Until ADR-171, `create()` emitted a bare `docker run -d --rm -w /workspace <image> ...` -- grepping
// this file for `--network`, `--memory`, `--pids-limit` or `--cpus` returned NOTHING. Every container
// this class produced therefore got Docker's own defaults: full bridge-network egress, unbounded
// memory, unbounded pids, unbounded CPU, and the default capability set. That is not a theoretical
// gap: 008 §2 makes enforced resource limits and empty-by-default authority mandatory for a backend,
// and 008 §4 says egress is ALWAYS host-mediated. None of it was wired.
//
// This type is the fix, and its defaults are the point: a default-constructed `ContainerIsolation`
// denies the network outright, drops every Linux capability, forbids privilege escalation, and caps
// memory/pids/CPU. A caller that passes nothing gets containment; loosening any of it is an explicit,
// visible, host-written field assignment. That inversion -- rather than a comment asking callers to
// remember -- is what makes the header's own scope statement enforceable instead of advisory.
//
// The numeric ceilings below are deliberately CONSERVATIVE DEFAULTS a host raises, not tuned values.
// 006 §7's "a fixed byte constant applied uniformly is an anti-pattern" warning is about deriving a
// per-call budget from a model's context window; it does not argue for leaving a container
// unbounded. An unset `ResourceLimits` field maps to these, never to "unlimited" (see
// `container_isolation_from()` below).
struct ContainerIsolation {  // ae-naming-lint: allow ContainerIsolation — ADR-171 (issue #63); 008 §2 names the obligations, 027 has not been updated
    // `false` => `--network none`. The container gets a loopback interface and nothing else --
    // verified by reading `/sys/class/net` from inside, not merely by passing the flag (see
    // tests/sandbox/execution_surface/test_docker_isolation.cpp).
    bool          network_enabled = false;
    std::uint64_t memory_bytes    = 512ull * 1024 * 1024;
    std::uint32_t pids            = 128;
    // Thousandths of a CPU: 1000 == `--cpus 1.000`. An integer, not a double, so the argv this
    // produces is byte-stable and locale-independent -- `std::to_string(double)` is neither.
    std::uint32_t cpu_milli       = 1000;
    bool          drop_all_capabilities = true;  // --cap-drop ALL
    bool          no_new_privileges     = true;  // --security-opt no-new-privileges
};

// The isolation flags, as argv elements, for splicing into a `docker run` line. A pure function of
// its argument -- no daemon, no process, no I/O -- so the mapping is provable offline, which matters
// because the live half of this proof only runs where a Docker daemon exists.
[[nodiscard]] std::vector<std::string> docker_isolation_argv(ContainerIsolation const& iso);

// The bridge from the engine's own 008 §2 vocabulary (`ResourceLimits`/`NetPolicy`, sandbox.hpp) to
// what `docker run` can actually enforce. Written now, ahead of any `SandboxBackend` promotion,
// because it is where the honest gaps become visible rather than being discovered later by whoever
// attempts that promotion:
//
//   - `NetPolicy::allowlist` is REFUSED, not silently widened. 008 §4's rule is "egress is always
//     host-mediated"; `docker run --network bridge` grants unfiltered egress, which is not an
//     allowlist by any reading. Mapping a 3-entry allowlist onto full bridge access would be the
//     single most dangerous thing this function could do, so a non-empty allowlist fails closed and
//     says why. A host that needs filtered egress routes through the existing mediated proxy
//     (`sandbox/net_egress_proxy.hpp`), not through this surface.
//   - `ResourceLimits::cpu_ms` is NOT mapped to `--cpus`. They answer different questions: `cpu_ms`
//     is a total CPU-time BUDGET, `--cpus` is a bandwidth SHARE. Silently treating one as the other
//     would produce a limit that looks enforced and is not. Left at the default ceiling; named here.
//   - `wall_ms` is enforced host-side by `run_argv()`'s own timeout kill (ADR-139), not by any
//     container flag, and `output_bytes` by that same function's capture cap -- both already real,
//     neither expressible as a `docker run` argument.
//   - `fds`, `disk_bytes`, `net_bytes` have no `docker run` equivalent this function can honestly
//     emit. Unmapped, and said so.
//
// An UNSET (zero) limit maps to this type's own conservative default, never to "unlimited" -- the
// fail-closed direction. A caller that genuinely wants a bigger ceiling states it.
[[nodiscard]] agentengine::result<ContainerIsolation> container_isolation_from(
    agentengine::ResourceLimits const& limits, agentengine::NetPolicy const& net);

// A thin, real wrapper over the `docker` CLI -- create()/exec()/destroy() over `docker run`/
// `docker exec`/`docker rm`, `copy_to_container()`/`copy_from_container()` over `docker cp`. Every
// invocation is a real argv vector via `docker_cli_detail::run_argv()` -- never a shell-interpreted
// string -- matching `ContainerdCliBackend`'s own already-shipped shape (containerd_execution_surface.hpp)
// exactly. Deliberately NOT a `SandboxBackend` conformer (see this file's own top comment).
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
    //
    // ADR-171 (issue #63): `isolation` defaults to a DENY-ALL `ContainerIsolation` -- see that type's
    // own comment. Every existing `create()`/`create(image)` call site keeps compiling and now gets
    // containment it did not have before; nothing has to opt in to be safe, and loosening is an
    // explicit field assignment a reader can see.
    [[nodiscard]] agentengine::result<Instance> create(std::string const& image = "alpine:latest",
                                                          ContainerIsolation const& isolation = {});

    // Real `docker cp <host_path> <container>:<container_path>`.
    [[nodiscard]] agentengine::result<void> copy_to_container(Instance const& inst,
                                                                  std::filesystem::path const& host_path,
                                                                  std::string const& container_path);

    // ADR-174 (GitHub issue #68): seeds `container_path` from `host_dir`'s CONTENTS with every entry
    // owned by root:root, by streaming a ustar archive into `docker cp -`.
    //
    // This exists because `copy_to_container()` above cannot do it. `docker cp <host path>` preserves
    // the HOST file's ownership, and ADR-171's `--cap-drop ALL` took CAP_DAC_OVERRIDE and
    // CAP_DAC_READ_SEARCH away from the container's root -- so a seeded file, which
    // `write_verified()` creates at mode 0600, was neither readable nor writable by the only user the
    // container has. `docker cp -` instead reads a tar from stdin, and what the container ends up
    // owning is whatever the ARCHIVE HEADERS say. Writing those headers ourselves (ustar_writer.hpp)
    // is what makes the seeded tree root-owned WITHOUT handing any capability back.
    //
    // The archive goes to a TEMP FILE which becomes the child's stdin, rather than being held in
    // memory and pushed through a pipe. That is what keeps peak memory independent of worktree size
    // -- an adversarial review demonstrated the in-memory version spending 2.0 GiB of RSS on a 1 GiB
    // file before refusing it, and `std::terminate`-ing via an uncaught `bad_alloc` under a memory
    // ceiling. The file is created 0600-equivalent by `ofstream` under the process umask, holds the
    // same bytes as the worktree it was built from (so it is not a new exposure class), and is
    // removed on every exit path including the failure ones.
    //
    // A second, unplanned property worth naming because a reviewer will ask: `host_dir` never reaches
    // the command line at all. `copy_to_container()` has to embed a host path as an argv element and
    // validate it; this path has no host-path argument to validate, so that whole class of question
    // does not arise here.
    [[nodiscard]] agentengine::result<void> seed_tree_as_root(Instance const& inst,
                                                                  std::filesystem::path const& host_dir,
                                                                  std::string const& container_path);

    // Real `docker cp <container>:<container_path> <host_path>`.
    [[nodiscard]] agentengine::result<void> copy_from_container(Instance const& inst,
                                                                    std::string const& container_path,
                                                                    std::filesystem::path const& host_path);

    // Real `docker exec <id> sh -c "<command>"` -- runs INSIDE the container's own isolated
    // filesystem/process namespace, never in this process at all. `command` reaches the CONTAINER's own
    // inner `sh -c` as ONE literal argv element (never a host-shell-parsed string, see this file's own
    // top comment) -- the fix for issue #50: a model command containing `"`/`%`/`^` no longer needs to
    // be rejected, because there is no host shell left for those characters to break out of. Matches
    // `ContainerdCliBackend::exec()`'s own already-shipped shape exactly.
    [[nodiscard]] agentengine::result<SurfaceRunOutcome> exec(Instance const& inst, std::string const& command);

    // GitHub issue #80 -- the resolved, content-addressed identity of the image `inst` is ACTUALLY
    // running, read back off the live container rather than re-resolved from the reference. That
    // distinction is the whole point: asking `docker image inspect alpine:latest` again can answer with
    // a different image than the one this container was created from, if the tag moved or a pull
    // happened in between; a container's own image binding is fixed at creation and cannot drift.
    //
    // WHAT KIND OF DIGEST THIS IS DEPENDS ON THE DAEMON, and that is a property of the value, not a
    // detail -- an earlier version of this comment asserted flatly that it is the image CONFIG digest,
    // which was measured FALSE on the machine this was written on. `{{.Image}}` is the image ID, and what
    // the image ID digests is decided by the daemon's image store: with the CONTAINERD image store
    // (Docker Desktop's default, and increasingly the Linux one) it is the digest of the descriptor the
    // reference resolves to -- an image INDEX for a multi-platform reference -- while with the classic
    // graph driver Docker documents it as the image CONFIG digest, a different value for the same image.
    //
    // This function does not try to say which; `resolve_image_digest_kind()` below answers that by
    // MEASURING it, and `DockerExecutionSurface::image_digest_kind()` reports the answer alongside the
    // digest so a consumer never has to infer comparability from the surface type (ADR-176 §9).
    //
    // Returns an EMPTY string, never an error, on any failure: this is provenance enrichment, and a
    // command must not fail to run because the daemon declined to describe its own container. An empty
    // digest travels outward as "not known" (see `ImageIdentifiedSurface` in execution_surface.hpp).
    // `inst.container_id` went through `docker_cli_reject_leading_dash()` at `create()` before this
    // class ever returned it, so it is already safe as a bare argv element here.
    [[nodiscard]] std::string resolve_image_digest(Instance const& inst);

    // GitHub issue #80 / ADR-176 §9 -- WHAT `image_id` digests, read off the daemon rather than inferred.
    //
    // `docker image inspect --format {{.Descriptor.mediaType}}` answers this directly: the media type of
    // the descriptor the image ID names. `application/vnd.oci.image.index.v1+json` means the ID is an
    // index digest, `...image.manifest.v1+json` a manifest digest, `...image.config.v1+json` a config
    // digest. No inference from which image store the operator configured, and no appeal to what Docker
    // documents an image ID to be -- both of which this file has already been wrong about once.
    //
    // THIS REPLACED A HEURISTIC, and the heuristic's failure is worth keeping visible because it looked
    // reasonable. The first version asked whether `image_id` appeared among the image's own RepoDigests:
    // present -> manifest, absent-but-RepoDigests-nonempty -> config, empty -> unknown. ADR-176 §11's
    // red-team round broke it in three ways. (1) It could not tell an INDEX digest from a per-platform
    // MANIFEST digest -- both are RepoDigest-shaped -- so it reported one kind for two different objects,
    // which is precisely the false "same kind" the enum exists to prevent. (2) Its `config` answer was
    // reached by ELIMINATION, so any cause other than the classic graph driver -- and the space was never
    // enumerated -- produced a confident `config` label for a digest nobody had measured. (3) Its stated
    // safety argument ("the template emits that one field and nothing else") was false: `run_argv()`
    // MERGES stderr into `stdout_text`, as `resolve_image_digest()` above reasons about explicitly, so
    // the searched text was never governed by the template at all. Reading a media type and matching it
    // EXACTLY against a closed list (`image_digest_kind_from_media_type()`) has none of those problems:
    // an unrecognized line is `unknown`, which is both the honest answer and the safe one.
    //
    // Degrades to `unknown`, never to a guess, on a daemon whose `docker image inspect` has no
    // `.Descriptor` field: the template fails, the exit code is non-zero or the text is `<no value>`, and
    // neither matches a known media type. `.Descriptor` is present on Docker 29.7.2 (measured); older
    // daemons are not verified here and get `unknown`, which is correct rather than merely safe.
    //
    // Costs ONE `docker image inspect`, resolved once per resolved digest -- see
    // `DockerExecutionSurface::reset()` for the caching and what it trades, and
    // bench/docker_image_digest_resolution.cpp for the numbers.
    [[nodiscard]] agentengine::ImageDigestKind resolve_image_digest_kind(std::string const& image_id);

    [[nodiscard]] agentengine::result<void> destroy(Instance const& inst);

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

    [[nodiscard]] agentengine::result<OrphanReapReport> reap_orphans();
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
    // ADR-171 (issue #63): `isolation` defaults to deny-all (`ContainerIsolation`'s own defaults) --
    // an existing `DockerExecutionSurface{}` / `DockerExecutionSurface{"alpine:latest"}` call site
    // keeps compiling and now runs contained. Use `container_isolation_from()` to derive this from an
    // engine `SandboxSpec`'s own `limits`/`net`.
    explicit DockerExecutionSurface(std::string image = "alpine:latest", ContainerIsolation isolation = {})
        : image_(std::move(image)), isolation_(isolation) {}

    ~DockerExecutionSurface() {
        if (instance_) { (void)docker_.destroy(*instance_); }
    }
    DockerExecutionSurface(DockerExecutionSurface const&) = delete;
    DockerExecutionSurface& operator=(DockerExecutionSurface const&) = delete;

    // Move constructor resets the moved-FROM `instance_` explicitly -- `std::optional`'s own
    // move-construction semantics only move the CONTAINED value, `has_value()` is unchanged by
    // default, which would otherwise fire a malformed `docker rm -f ` (empty id) from the moved-from
    // object's own destructor.
    //
    // ADR-171 (issue #63): `isolation_` is carried explicitly. Both this constructor and the move
    // assignment below enumerate their members by hand, so a member added without touching them is
    // silently dropped -- which for THIS member would mean a moved-to surface quietly reverting to
    // the deny-all defaults, discarding a host's deliberate opt-in. Safe direction, still wrong, and
    // exactly the class of silent divergence this file's own moved-from `instance_` comment above
    // exists because of.
    DockerExecutionSurface(DockerExecutionSurface&& other) noexcept
        : image_(std::move(other.image_)), resolved_digest_(std::move(other.resolved_digest_)),
          resolved_kind_(other.resolved_kind_), kind_resolved_for_(std::move(other.kind_resolved_for_)),
          isolation_(other.isolation_), docker_(std::move(other.docker_)),
          instance_(std::move(other.instance_)) {
        other.instance_.reset();
        // Issue #80: a moved-from surface runs nothing and must claim no image, so the cached digest goes
        // with `image_` and `instance_` rather than being left behind. Explicit because a moved-from
        // std::string is only "valid but unspecified" -- this makes the postcondition this type's own
        // guarantee instead of an inherited library habit.
        other.resolved_digest_.clear();
        // ADR-176 §9: and the kind with it. An enum is not "valid but unspecified" after a move -- it is
        // simply copied -- so without this line the moved-from surface would answer `manifest` while
        // answering "" for the digest it describes, which is the one combination that must be impossible.
        other.resolved_kind_ = agentengine::ImageDigestKind::unknown;
        other.kind_resolved_for_.clear();
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
            resolved_digest_.swap(other.resolved_digest_);  // issue #80 -- travels with `instance_`
            std::swap(resolved_kind_, other.resolved_kind_);  // ADR-176 §9 -- and never without it
            kind_resolved_for_.swap(other.kind_resolved_for_);
            std::swap(isolation_, other.isolation_);  // ADR-171 -- see the move ctor's own comment
            std::swap(docker_, other.docker_);
            instance_.swap(other.instance_);
        }
        return *this;
    }

    // `instance_` is only cleared once `destroy()` has actually succeeded -- a transient failure
    // (daemon contention, a network hiccup) leaves it in place, so the object still remembers the
    // leak and a caller retrying `reset()` gets another real attempt at destroying the SAME
    // container, rather than silently starting a second one alongside an orphaned first.
    [[nodiscard]] agentengine::result<void> reset(std::filesystem::path const& host_dir);

    [[nodiscard]] agentengine::result<SurfaceRunOutcome> run(std::string const& command);

    // Issue #80 (`ImageIdentifiedSurface`, execution_surface.hpp). `image()` is the reference this surface
    // was CONFIGURED with -- a tag, a digest reference, whatever the host passed; `image_digest()` is what
    // that reference resolved to for the container currently in place, and is empty before the first
    // `reset()` and whenever the daemon could not answer. Both are reported deliberately: the configured
    // reference is what an operator recognizes, the digest is what a provenance record can be trusted on.
    [[nodiscard]] std::string_view image() const noexcept { return image_; }
    [[nodiscard]] std::string_view image_digest() const noexcept { return resolved_digest_; }
    // ADR-176 §9 -- what `image_digest()` digests, so a consumer can decide whether comparing it against
    // another record's digest is even meaningful. `unknown` whenever there is no digest.
    [[nodiscard]] agentengine::ImageDigestKind image_digest_kind() const noexcept { return resolved_kind_; }

    [[nodiscard]] agentengine::result<void> drain_to(std::filesystem::path const& host_dir);

private:
    std::string image_;
    // Issue #80. What `image_` resolved to, cached: empty until the first `reset()` that creates a
    // container the daemon could describe, then reused for this surface's whole life. See `reset()` for
    // the measured cost this caching exists for, and the one staleness case it accepts.
    std::string resolved_digest_;
    // ADR-176 §9. Moved/swapped with `resolved_digest_` everywhere, because a kind that outlives its
    // digest describes a value this surface no longer holds. `kind_resolved_for_` is the digest
    // `resolved_kind_` was measured for -- the key that makes a stale kind unrepresentable rather than
    // merely unlikely, and that lets a failed kind lookup be retried without re-spawning on every reset.
    agentengine::ImageDigestKind resolved_kind_{agentengine::ImageDigestKind::unknown};
    std::string kind_resolved_for_;
    // ADR-171 (issue #63). Held by value and applied on every `reset()`, so a re-materialized surface
    // is contained identically to its first container -- not only the first one.
    ContainerIsolation isolation_{};
    DockerCliBackend docker_;
    std::optional<DockerCliBackend::Instance> instance_;
};

static_assert(ImageIdentifiedSurface<DockerExecutionSurface>,
              "DockerExecutionSurface must report which image it runs (issue #80)");

static_assert(ExecutionSurface<DockerExecutionSurface>,
              "DockerExecutionSurface must satisfy the real ExecutionSurface concept");

}  // namespace agentengine
