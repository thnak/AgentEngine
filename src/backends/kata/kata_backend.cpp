// Implements kata_backend.hpp -- see that file's header comment for full scope/residuals.
#include "backends/kata/kata_backend.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>
#include <variant>
#include <vector>

extern char** environ;

namespace agentengine::kata {

namespace {

// Red-team finding #1 (BLOCKING, verified by execution -- a 200MB guest `exec()` output landed in
// full in host RSS with no cap): matches `LinuxNativeJailBackend::drain_pipe_bounded()`'s own
// `kDefaultSafetyCapBytes` (linux_native_jail_backend.cpp) -- a host-safety cap independent of and
// underneath any future `SandboxSpec::limits`/`output_discipline.hpp` wiring, not a substitute for
// either.
constexpr std::size_t kOutputSafetyCapBytes = 16ull * 1024 * 1024;

// Red-team finding #2 (BLOCKING, verified by execution -- a hung/slow `ctr` call had no bound at
// all, including inside destroy()'s own cleanup sequence). No `SandboxSpec`/`ResourceLimits` axis
// covers this -- it bounds THIS backend's own host-side subprocess wait, not guest wall-clock time.
constexpr int kProcessTimeoutSeconds = 30;

struct ProcessOutcome {
    int exit_code = -1;
    bool timed_out = false;
    bool output_truncated = false;
    std::string stdout_text;
    std::string stderr_text;
};

// Spawns `argv[0]` with `argv[1..]`, waits for exit (bounded by `timeout_seconds`), captures
// stdout/stderr (each capped at `output_cap_bytes`). Uses `posix_spawn_file_actions_t` to wire
// exactly three fds (stdin closed, stdout/stderr to fresh pipes) into the child -- POSIX_SPAWN gives
// no equivalent of Win32's implicit "every inheritable handle crosses by default" hazard (a POSIX
// child only inherits fds the parent hasn't marked `FD_CLOEXEC`), but this backend still opens every
// pipe end with `O_CLOEXEC` explicitly and closes the child-side ends in the parent immediately after
// spawn, rather than relying on that platform default alone -- the same "don't rely on the platform
// default being safe, make it explicit" posture `decisions/ADR-004-...md` §12 Finding 6 named on the
// Windows side of this project after a real, reproduced ambient-authority leak there.
//
// Both pipes are drained via `poll()`, interleaved, rather than sequentially (stdout to EOF, then
// stderr) -- a child writing substantial output to BOTH streams concurrently could otherwise block
// on a full stderr pipe while this function is still fully draining stdout, a classic pipe deadlock
// this function's own predecessor (sequential `drain()`) was silently exposed to.
//
// Slice 2: `timeout_seconds`/`output_cap_bytes` default to the Slice-1 red-team fix's own fixed
// constants but are now caller-overridable -- `KataBackend::exec()`/`destroy()` pass
// `SandboxSpec::limits.wall_ms`/`output_bytes` through when an `Instance` was created with them set
// (kata_backend.hpp's own header comment has the full mapping).
[[nodiscard]] result<ProcessOutcome> run_ctr(std::vector<std::string> const& args,
                                              int timeout_seconds = kProcessTimeoutSeconds,
                                              std::size_t output_cap_bytes = kOutputSafetyCapBytes) {
    std::array<int, 2> out_pipe{-1, -1};
    std::array<int, 2> err_pipe{-1, -1};
    if (pipe2(out_pipe.data(), O_CLOEXEC) != 0) {
        return std::unexpected(error{failure_class::fatal,
                                      "kata_backend: pipe2() failed while preparing to spawn ctr",
                                      "kata_backend.pipe_failed"});
    }
    if (pipe2(err_pipe.data(), O_CLOEXEC) != 0) {
        // Red-team finding #3 (MINOR, code reading): the first pipe's two fds must be closed on
        // this path too -- every other error path in this function closes what it opened, this one
        // originally didn't.
        close(out_pipe[0]);
        close(out_pipe[1]);
        return std::unexpected(error{failure_class::fatal,
                                      "kata_backend: pipe2() failed while preparing to spawn ctr",
                                      "kata_backend.pipe_failed"});
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // dup2 the write ends onto 1/2 in the child -- dup2'd fds are never CLOEXEC, so this is the one
    // deliberate exception, exactly the two fds this child is meant to have.
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto const& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    int const spawn_rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (spawn_rc != 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        return std::unexpected(error{failure_class::fatal,
                                      std::string("kata_backend: posix_spawnp(ctr) failed: ") +
                                          std::strerror(spawn_rc),
                                      "kata_backend.spawn_failed"});
    }

    ProcessOutcome outcome;
    struct Stream {
        int fd;
        std::string* sink;
        bool open = true;
    };
    std::array<Stream, 2> streams{
        Stream{out_pipe[0], &outcome.stdout_text, true},
        Stream{err_pipe[0], &outcome.stderr_text, true},
    };

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    char buf[4096];
    for (;;) {
        bool const all_closed = !streams[0].open && !streams[1].open;
        auto const now = std::chrono::steady_clock::now();
        if (all_closed || now >= deadline) break;

        std::array<struct pollfd, 2> pfds{};
        int nfds = 0;
        for (auto& s : streams) {
            if (s.open) pfds[static_cast<std::size_t>(nfds++)] = pollfd{s.fd, POLLIN, 0};
        }
        int const timeout_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        int const rc = poll(pfds.data(), static_cast<nfds_t>(nfds), timeout_ms > 0 ? timeout_ms : 0);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) break;  // deadline reached with no activity -- outer loop re-checks `now`

        for (int i = 0; i < nfds; ++i) {
            if ((pfds[static_cast<std::size_t>(i)].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
            int const fd = pfds[static_cast<std::size_t>(i)].fd;
            auto* stream_it = (streams[0].fd == fd) ? &streams[0] : &streams[1];
            ssize_t const n = read(fd, buf, sizeof(buf));
            if (n <= 0) {
                close(fd);
                stream_it->open = false;
                continue;
            }
            std::size_t const remaining =
                output_cap_bytes > stream_it->sink->size() ? output_cap_bytes - stream_it->sink->size() : 0;
            std::size_t const take = static_cast<std::size_t>(n) < remaining ? static_cast<std::size_t>(n) : remaining;
            stream_it->sink->append(buf, take);
            if (take < static_cast<std::size_t>(n)) outcome.output_truncated = true;
            if (stream_it->sink->size() >= output_cap_bytes) {
                // Real cap reached -- stop reading this stream (leaving the rest unread is fine:
                // this fd is closed, `ctr`/the guest may see EPIPE/SIGPIPE on further writes, which
                // is the correct backpressure signal for a runaway output producer).
                close(fd);
                stream_it->open = false;
            }
        }
    }
    for (auto& s : streams) {
        if (s.open) {
            close(s.fd);
            s.open = false;
            outcome.timed_out = true;
        }
    }

    int status = 0;
    if (outcome.timed_out) {
        kill(pid, SIGKILL);
        // Bounded reap of a process we just SIGKILL'd -- not an unconditional blocking waitpid().
        for (int i = 0; i < 50; ++i) {
            if (waitpid(pid, &status, WNOHANG) != 0) break;
            struct timespec ts{0, 20'000'000};  // 20ms
            nanosleep(&ts, nullptr);
        }
        outcome.exit_code = -1;
        return outcome;
    }
    if (waitpid(pid, &status, 0) < 0) {
        return std::unexpected(error{failure_class::fatal,
                                      "kata_backend: waitpid() failed after spawning ctr",
                                      "kata_backend.waitpid_failed"});
    }
    outcome.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return outcome;
}

[[nodiscard]] std::string fresh_id(std::string const& prefix) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream oss;
    oss << prefix << "-" << std::hex << rng();
    return oss.str();
}

}  // namespace

result<SandboxHandle> KataBackend::create(SandboxSpec const& spec, EffectContext& /*ctx*/) {
    // Slice 3 (docs/planning/sandbox-spec-capability-enforcement-design-draft.md): capabilities are
    // the real authority behind mounts/net, checked FIRST, before any of this backend's own
    // mount/net logic below runs. A no-op for every caller that doesn't hold a SandboxMount/
    // SandboxNetOut grant (the mechanism's own opt-in scoping) -- see authorize_spec()'s own comment.
    if (auto authorized = authorize_spec(spec); !authorized.has_value()) {
        return std::unexpected(authorized.error());
    }

    // Slice 2: NetPolicy -- deny_all is the only value this backend can currently honor (see this
    // file's header comment). Fail closed rather than silently granting deny_all anyway when a
    // caller asked for something this backend cannot yet deliver.
    if (!spec.net.deny_all || !spec.net.allowlist.empty()) {
        return std::unexpected(error{
            failure_class::policy,
            "kata_backend: NetPolicy with deny_all=false or a nonempty allowlist is not supported "
            "yet -- this backend has no CNI/egress-proxy wired to honor a real allowlist (Slice 2 "
            "scope gap, see kata_backend.hpp)",
            "kata_backend.net_allowlist_unsupported"});
    }

    // Slice 2: MountSpec -- each host-path grant becomes a real `--mount` flag; a BlobRef source
    // fails closed, matching LinuxNativeJailBackend::create()'s identical posture for the same
    // *check* -- but NOT the same safety margin: `LinuxNativeJailBackend` passes host/guest paths
    // straight into a `::mount()` syscall (two separate arguments, no delimited-string grammar to
    // exploit); this backend must instead build a SINGLE comma-delimited `ctr run --mount
    // type=bind,src=...,dst=...,options=...` value, which `ctr`'s own parser reads as repeated
    // `key=value` pairs with LAST-WINS semantics. Red-team finding #1 (BLOCKING, verified by
    // execution): a `guest_path` (or `source`) containing an embedded `,src=...,dst=...` segment
    // silently overrode the caller's own intended `src`/`dst`, mounting an ARBITRARY host path the
    // caller never authorized (reproduced directly: `guest_path =
    // "/mnt/intended,src=/etc,dst=/mnt/hijacked"` bind-mounted the host's real `/etc` into the
    // guest, readable via `cat /mnt/hijacked/passwd`) -- a full I2 violation (ambient authority over
    // any host path an attacker names) reachable through a spec field this backend claims to
    // enforce. Fixed by rejecting a comma in either path outright: `ctr`'s mount-value grammar has
    // no escaping mechanism, so refusing the one delimiter it splits on removes the injection
    // surface entirely rather than trying to escape it. (The `options=rbind:<ro|rw>` segment itself
    // is NOT similarly injectable -- it is always appended last, and `ctr`'s last-wins parsing means
    // an embedded `options=` earlier in the string cannot escalate ro to rw. Only the source-path
    // hijack was real.)
    std::vector<std::string> mount_args;
    for (MountSpec const& mount : spec.mounts) {
        std::string const* host_path = std::get_if<std::string>(&mount.source);
        if (host_path == nullptr) {
            return std::unexpected(error{
                failure_class::policy,
                "kata_backend: MountSpec::source as a BlobRef is not supported by KataBackend yet "
                "(host paths only, same scope gap as LinuxNativeJailBackend)",
                "kata_backend.blob_mount_unsupported"});
        }
        if (host_path->find(',') != std::string::npos || mount.guest_path.find(',') != std::string::npos) {
            return std::unexpected(error{
                failure_class::policy,
                "kata_backend: MountSpec host path or guest_path contains ',' -- rejected outright "
                "rather than risking injection into ctr's comma-delimited --mount value grammar "
                "(no legitimate absolute path needs a literal comma)",
                "kata_backend.mount_path_invalid"});
        }
        mount_args.push_back("--mount");
        mount_args.push_back("type=bind,src=" + *host_path + ",dst=" + mount.guest_path +
                              ",options=rbind:" + (mount.read_write ? "rw" : "ro"));
    }

    // Slice 2: ResourceLimits -- memory_bytes maps directly to a real ctr flag; cpu_ms/pids/fds/
    // disk_bytes/net_bytes have no mechanism wired this pass (kata_backend.hpp's header comment
    // names each one honestly rather than silently ignoring them).
    std::vector<std::string> limit_args;
    if (spec.limits.memory_bytes > 0) {
        limit_args.push_back("--memory-limit");
        limit_args.push_back(std::to_string(spec.limits.memory_bytes));
    }

    std::string const id = fresh_id("ae-kata");
    std::vector<std::string> args{"ctr", "run", "-d", "--runtime", runtime_type_};
    args.insert(args.end(), mount_args.begin(), mount_args.end());
    args.insert(args.end(), limit_args.begin(), limit_args.end());
    args.insert(args.end(), {image_, id, "sleep", "infinity"});

    auto outcome = run_ctr(args);
    if (!outcome.has_value()) return std::unexpected(outcome.error());
    if (outcome->exit_code != 0) {
        // Red-team finding #2 (REAL GAP, verified by execution -- e.g. a nonsensically tiny
        // `memory_bytes` lets containerd register a container object before the Kata agent inside
        // the guest fails to actually create it; `ctr run` then exits nonzero, but the container
        // object itself is left behind on the host, unreachable by any code path here because NO
        // `Instance`/`SandboxHandle` was ever minted for it -- not even a later `destroy()` call can
        // find it). Best-effort cleanup on this exact path, mirroring `destroy()`'s own "at least
        // try, log if it fails, never let a cleanup failure block returning the real error" posture,
        // rather than leaving every `create()` failure to leak a containerd object silently.
        (void)run_ctr({"ctr", "task", "kill", id});
        (void)run_ctr({"ctr", "task", "rm", id});
        auto cleanup = run_ctr({"ctr", "container", "rm", id});
        if (!cleanup.has_value() || cleanup->exit_code != 0) {
            std::fprintf(stderr,
                          "kata_backend: create(%s) failed and its own cleanup attempt also did not "
                          "succeed cleanly -- possible leaked Kata sandbox resource, see host "
                          "containerd state directly\n",
                          id.c_str());
        }
        return std::unexpected(error{
            failure_class::fatal,
            "kata_backend: ctr run failed (exit " + std::to_string(outcome->exit_code) +
                "): " + outcome->stderr_text,
            "kata_backend.create_failed"});
    }
    // Slice 2: wall_ms/output_bytes carried onto the Instance -- every exec()/destroy() call this
    // instance makes uses these instead of run_ctr()'s own fixed defaults, when the caller set them.
    // NOTE (red-team finding #3, judgment call): this `create()` call's OWN `run_ctr(args)` above
    // does NOT use wall_ms as its timeout -- it always uses run_ctr()'s fixed kProcessTimeoutSeconds
    // default, deliberately: applying a caller's possibly-sub-second wall_ms (meant for a fast
    // in-guest workload) to VM BOOT time itself would make create() fail spuriously for realistic
    // small values, since cold start is "milliseconds" (traits, above) but not zero. `wall_ms` bounds
    // exec()/destroy() calls on an already-created instance ONLY, not create()'s own boot -- stated
    // explicitly here per the red-team's own recommendation, not left for a reader to infer.
    Instance inst{id, 0, spec.limits.wall_ms, spec.limits.output_bytes};
    instances_.emplace(id, std::move(inst));
    return SandboxHandle{id};
}

result<ExecOutcome> KataBackend::exec(SandboxHandle& handle, ExecRequest const& request,
                                       EffectContext& /*ctx*/) {
    auto it = instances_.find(handle.opaque_id);
    if (it == instances_.end()) {
        return std::unexpected(error{failure_class::contract,
                                      "kata_backend: exec() on a handle this instance never created "
                                      "(or already destroyed)",
                                      "kata_backend.unknown_handle"});
    }
    Instance& inst = it->second;
    std::string const exec_id = "e" + std::to_string(++inst.exec_seq);
    int const timeout_seconds =
        inst.wall_ms > 0 ? static_cast<int>((inst.wall_ms + 999) / 1000) : kProcessTimeoutSeconds;
    std::size_t const output_cap =
        inst.output_cap_bytes > 0 ? static_cast<std::size_t>(inst.output_cap_bytes) : kOutputSafetyCapBytes;
    auto outcome = run_ctr({"ctr", "tasks", "exec", "--exec-id", exec_id, inst.container_id,
                             "/bin/sh", "-c", request.source},
                            timeout_seconds, output_cap);
    if (!outcome.has_value()) return std::unexpected(outcome.error());

    ExecOutcome result_out;
    result_out.stdout_text = outcome->stdout_text;
    result_out.stderr_text = outcome->stderr_text;
    if (outcome->timed_out) {
        result_out.klass = exec_outcome_class::timeout;
        // SLICE 4 (2026-08-24) fix -- real gap found by this pass's own red-team review
        // (decisions/ADR-088-...md §3 finding #7, BLOCKING): the `kill(pid, SIGKILL)` inside
        // run_ctr()'s own timeout path above only terminates the HOST-side `ctr` CLI wrapper
        // process this backend posix_spawn'd -- it has no host-visible pid for the GUEST-side
        // process that CLI was attached to. Before this fix, that guest process kept running
        // orphaned inside the persistent `sleep infinity` container, invisible to the caller, until
        // a LATER exec()/destroy() call happened to reap it -- `exec_outcome_class::timeout` was
        // being returned without the workload actually having stopped, undermining any G2
        // containment claim built on it. Best-effort: ask containerd to kill the guest-side process
        // directly by the SAME --exec-id this call minted above. NOT independently re-verified
        // against a live Kata deployment this session (none reachable) -- if the exact `ctr` CLI
        // surface assumed here turns out wrong, this call fails into the log line below rather than
        // blocking the (already-decided) `timeout` classification from being returned.
        auto kill_outcome = run_ctr(
            {"ctr", "tasks", "kill", "--exec-id", exec_id, "--signal", "SIGKILL", inst.container_id});
        if (!kill_outcome.has_value() || kill_outcome->exit_code != 0) {
            std::fprintf(stderr,
                         "kata_backend: exec(%s) timed out and the best-effort guest-side kill of "
                         "--exec-id %s also did not report success -- the guest workload may still "
                         "be running orphaned inside container %s\n",
                         handle.opaque_id.c_str(), exec_id.c_str(), inst.container_id.c_str());
        }
    } else {
        result_out.klass = outcome->exit_code == 0 ? exec_outcome_class::ok : exec_outcome_class::crash;
    }
    return result_out;
}

void KataBackend::destroy(SandboxHandle& handle) {
    auto it = instances_.find(handle.opaque_id);
    if (it == instances_.end()) return;  // destroy() on an unknown/already-destroyed handle is a
                                          // silent no-op, matching LinuxNativeJailBackend's own
                                          // destroy() posture (idempotent, never throws).
    std::string const id = it->second.container_id;
    int const timeout_seconds = it->second.wall_ms > 0
                                     ? static_cast<int>((it->second.wall_ms + 999) / 1000)
                                     : kProcessTimeoutSeconds;
    std::size_t const output_cap = it->second.output_cap_bytes > 0
                                        ? static_cast<std::size_t>(it->second.output_cap_bytes)
                                        : kOutputSafetyCapBytes;
    // Red-team finding #4 (MINOR/REAL GAP, no leak confirmed but no observability at all before this
    // fix): `destroy()` is `void` per the `SandboxBackend` concept -- no `EffectContext`/return value
    // to surface a failure through. A failed cleanup step is no longer fully silent: it's logged to
    // stderr so it is at least visible in host logs rather than vanishing without a trace, the
    // cheapest honest fix available at this call's own signature. Full structured observability
    // (an audit hook, matching `SandboxBackendResolutionAuditHook`'s own pattern) is future work, not
    // done here -- named, not silently deferred.
    for (auto const& step : {std::array<std::string, 3>{"ctr", "task", "kill"},
                              std::array<std::string, 3>{"ctr", "task", "rm"},
                              std::array<std::string, 3>{"ctr", "container", "rm"}}) {
        auto step_outcome = run_ctr({step[0], step[1], step[2], id}, timeout_seconds, output_cap);
        bool const failed = !step_outcome.has_value() || step_outcome->timed_out ||
                             step_outcome->exit_code != 0;
        if (failed) {
            std::fprintf(stderr,
                          "kata_backend: destroy(%s): '%s %s %s' did not succeed cleanly -- possible "
                          "leaked Kata sandbox resource, see host containerd state directly\n",
                          id.c_str(), step[0].c_str(), step[1].c_str(), step[2].c_str());
        }
    }
    instances_.erase(it);
}

}  // namespace agentengine::kata
