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

// Spawns `argv[0]` with `argv[1..]`, waits for exit (bounded by `kProcessTimeoutSeconds`), captures
// stdout/stderr (each capped at `kOutputSafetyCapBytes`). Uses `posix_spawn_file_actions_t` to wire
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
[[nodiscard]] result<ProcessOutcome> run_ctr(std::vector<std::string> const& args) {
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

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kProcessTimeoutSeconds);
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
                kOutputSafetyCapBytes > stream_it->sink->size() ? kOutputSafetyCapBytes - stream_it->sink->size() : 0;
            std::size_t const take = static_cast<std::size_t>(n) < remaining ? static_cast<std::size_t>(n) : remaining;
            stream_it->sink->append(buf, take);
            if (take < static_cast<std::size_t>(n)) outcome.output_truncated = true;
            if (stream_it->sink->size() >= kOutputSafetyCapBytes) {
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

result<SandboxHandle> KataBackend::create(SandboxSpec const& /*spec*/, EffectContext& /*ctx*/) {
    // `spec` is deliberately unused this pass -- see this file's header comment's "REAL GAP" note.
    std::string const id = fresh_id("ae-kata");
    auto outcome = run_ctr({"ctr", "run", "-d", "--runtime", runtime_type_, image_, id, "sleep",
                             "infinity"});
    if (!outcome.has_value()) return std::unexpected(outcome.error());
    if (outcome->exit_code != 0) {
        return std::unexpected(error{
            failure_class::fatal,
            "kata_backend: ctr run failed (exit " + std::to_string(outcome->exit_code) +
                "): " + outcome->stderr_text,
            "kata_backend.create_failed"});
    }
    instances_.emplace(id, Instance{id, 0});
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
    auto outcome = run_ctr({"ctr", "tasks", "exec", "--exec-id", exec_id, inst.container_id,
                             "/bin/sh", "-c", request.source});
    if (!outcome.has_value()) return std::unexpected(outcome.error());

    ExecOutcome result_out;
    result_out.stdout_text = outcome->stdout_text;
    result_out.stderr_text = outcome->stderr_text;
    if (outcome->timed_out) {
        result_out.klass = exec_outcome_class::timeout;
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
        auto step_outcome = run_ctr({step[0], step[1], step[2], id});
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
