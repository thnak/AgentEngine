#pragma once
// Relocated verbatim from kata_backend.cpp's own anonymous namespace -- a pure physical split, zero
// behavior change. Three genuinely self-contained clusters that touch no `KataBackend` member state
// at all: the generic host-subprocess spawn/drain primitive (`run_ctr`), a small id-minting helper
// (`fresh_id`), `NetPolicy::allowlist` string parsing/IPv4 formatting, and the OCI runtime-spec JSON
// builder (`build_oci_spec_json`/`OciSpecInputs`). `create()`'s own staged-construction pipeline
// (mount validation, disk-quota/overlay staging, mount-point pre-creation, netns/CNI/nftables wiring,
// `cleanup_partial()`'s rollback closure, `Instance` registration) stays in kata_backend.cpp -- see
// that file's own top comment for why: that pipeline's local variables are threaded linearly from
// acquisition through to `Instance` construction and unwound in exact reverse order by one rollback
// closure called from ~15 early-return sites, the same "looks separable by name, isn't by data-flow"
// shape a full-body read of agent_session.hpp's `run_rounds()` found -- not attempted here.
//
// Anonymous namespace, matching the original's own internal linkage exactly -- this header is
// included by exactly one translation unit (kata_backend.cpp), so nothing here is meant to be a
// public API surface any other file reaches for.

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
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "agentengine/core/json_value.hpp"

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
    // SLICE 5 (2026-08-24): distinct from `output_truncated` above -- see the red-teamed reasoning at
    // this flag's own set-site below for why the two are NOT interchangeable (a read landing exactly
    // on the cap boundary force-closes a stream without dropping any bytes on that particular read,
    // so `output_truncated` alone silently misses it). `output_capped` is the reliable "this stream
    // was force-closed because it hit its cap" signal `KataBackend::exec()` needs to decide whether a
    // guest-side kill/classification is warranted; `output_truncated` keeps its original narrower
    // meaning (bytes were actually dropped) for anyone reading it in the future.
    bool output_capped = false;
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
//
// SLICE 9/10 (2026-08-24): despite the name, this helper is generic over any `argv[0]` -- SLICE 1-8
// only ever spawned `ctr`, but `create()`/`destroy()` now also spawn `ip`, `cnitool`, and `nft` for
// the `--config`-mode rootfs/netns/CNI/nftables pipeline (ADR-093). `extra_env`, when non-empty,
// gives the child THIS backend's own explicit key=value pairs (e.g. `cnitool`'s `CNI_PATH`/
// `NETCONFPATH`) IN ADDITION TO this process's ambient environment, rather than requiring the caller
// to mutate this process's own `environ` (not thread-safe against concurrent `posix_spawn` calls, and
// a wrong "the source of authority is inherited host env state" posture -- I2: this backend's own
// configuration should be what selects a CNI plugin directory, not ambient process state).
[[nodiscard]] result<ProcessOutcome> run_ctr(std::vector<std::string> const& args,
                                              int timeout_seconds = kProcessTimeoutSeconds,
                                              std::size_t output_cap_bytes = kOutputSafetyCapBytes,
                                              std::vector<std::string> const& extra_env = {}) {
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

    // SLICE 9/10: build an explicit envp only when `extra_env` is non-empty -- every pre-existing
    // call site (empty `extra_env`) keeps using `environ` directly, byte-for-byte the same spawn
    // this function has always done, no behavior change for SLICE 1-8's own call sites.
    std::vector<char*> envp;
    if (!extra_env.empty()) {
        for (char** e = environ; e != nullptr && *e != nullptr; ++e) envp.push_back(*e);
        for (auto const& kv : extra_env) envp.push_back(const_cast<char*>(kv.c_str()));
        envp.push_back(nullptr);
    }
    char** const envp_arg = extra_env.empty() ? environ : envp.data();

    pid_t pid = -1;
    int const spawn_rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), envp_arg);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (spawn_rc != 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        return std::unexpected(error{failure_class::fatal,
                                      std::string("kata_backend: posix_spawnp(") + argv[0] +
                                          ") failed: " + std::strerror(spawn_rc),
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
                //
                // SLICE 5 red-team finding #1 (BLOCKING): set `output_capped` HERE, unconditionally,
                // not folded into the `take < n` check above -- a read that lands EXACTLY on the cap
                // boundary (`take == n`, e.g. cap sizes that line up with normal read-chunk/pipe-
                // buffering boundaries, very plausible with round numbers like 4096/65536 against this
                // function's own 4096-byte `buf`) still force-closes the stream on this line but drops
                // no bytes on THIS read, so `output_truncated` alone silently misses that this stream
                // was capped -- the exact gap that would let the tail logic below fall through to an
                // unconditional blocking waitpid() on realistic cap sizes, reopening the hang this
                // slice exists to close.
                outcome.output_capped = true;
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
    // SLICE 5 (2026-08-24): `outcome.output_capped` joins `outcome.timed_out` here -- an output-cap
    // breach previously fell through to the unconditional blocking `waitpid(pid, &status, 0)` below,
    // the same unbounded-host-side-wait shape ADR-088 already fixed for the wall_ms timeout case (see
    // that fix's own comment in KataBackend::exec() below) but had explicitly disclosed as NOT fixed
    // for this case. Real risk closed by folding it in here: if the host-side `ctr` process doesn't
    // promptly die/error from writing into a pipe whose read end this function just closed (SIGPIPE
    // ignored/handled, or blocked elsewhere in its own RPC layer), that blocking waitpid could hang
    // this call indefinitely, defeating this function's own "every subprocess call is bounded"
    // contract -- not just leaving the guest-side process orphaned (the risk ADR-088 named).
    bool const force_terminate = outcome.timed_out || outcome.output_capped;
    if (force_terminate) {
        // Courtesy non-blocking check FIRST (SLICE 5 red-team finding, applies to the output_capped
        // case specifically): if the host-side `ctr` process already exited on its own -- plausible
        // if it does die from EPIPE/SIGPIPE on the write that raced this stream's closure -- prefer
        // its real exit code over unconditionally discarding it to -1. This is NOT a blocking wait
        // (that would reintroduce the exact bug above): a single WNOHANG poll, nothing more.
        if (waitpid(pid, &status, WNOHANG) == pid) {
            outcome.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            return outcome;
        }
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

// SLICE 10: `VerifiedEndpoint::ipv4_host_order` -> dotted-decimal, for both the `/etc/hosts` pin and
// the `nft` rule's `ip daddr` match -- neither consumer wants the raw integer.
[[nodiscard]] std::string ipv4_to_dotted(std::uint32_t host_order) {
    std::ostringstream oss;
    oss << ((host_order >> 24) & 0xFFu) << '.' << ((host_order >> 16) & 0xFFu) << '.'
        << ((host_order >> 8) & 0xFFu) << '.' << (host_order & 0xFFu);
    return oss.str();
}

// `NetPolicy::allowlist` entries are `"host:port:scheme"` (sandbox.hpp's own doc comment) -- exactly
// two colons, `host` itself must not embed one (no IPv6-literal support in this grammar, matching
// what the field's own comment promises; a bracketed `[::1]:port:scheme` form is not accepted).
[[nodiscard]] result<std::tuple<std::string, std::uint16_t, std::string>> parse_allowlist_entry(
    std::string const& raw) {
    auto const fail = [&](char const* why) {
        return std::unexpected(error{failure_class::policy,
                                      "kata_backend: NetPolicy::allowlist entry '" + raw +
                                          "' is not valid 'host:port:scheme': " + why,
                                      "kata_backend.allowlist_entry_malformed"});
    };
    std::size_t const c1 = raw.find(':');
    if (c1 == std::string::npos) return fail("missing ':' separators");
    std::size_t const c2 = raw.find(':', c1 + 1);
    if (c2 == std::string::npos) return fail("missing ':' separators");
    if (raw.find(':', c2 + 1) != std::string::npos) return fail("more than two ':' separators");

    std::string const host = raw.substr(0, c1);
    std::string const port_str = raw.substr(c1 + 1, c2 - c1 - 1);
    std::string const scheme = raw.substr(c2 + 1);
    if (host.empty() || port_str.empty() || scheme.empty()) return fail("an empty host/port/scheme field");

    std::uint16_t port = 0;
    {
        std::size_t consumed = 0;
        unsigned long value = 0;
        try {
            value = std::stoul(port_str, &consumed);
        } catch (...) {
            return fail("port is not a number");
        }
        if (consumed != port_str.size() || value == 0 || value > 65535) return fail("port out of range");
        port = static_cast<std::uint16_t>(value);
    }
    return std::make_tuple(host, port, scheme);
}

// SLICE 9: everything `--config` mode needs `create()` to have already decided, kept in one place
// rather than threading eight separate parameters through `build_oci_spec_json()`.
struct OciSpecInputs {
    std::string rootfs_path;
    std::vector<MountSpec> const* mounts;
    std::uint64_t memory_bytes;
    std::uint32_t fds;
    std::uint32_t pids;  // SLICE 11: 0 = unset, same "0 means don't add this resources member" idiom
                          // memory_bytes already uses.
    std::string cgroups_path;
    std::optional<std::string> netns_path;       // unset -> fresh, empty netns (deny_all posture);
                                                   // set -> join the pre-populated netns SLICE 10 built.
    std::optional<std::string> hosts_file_path;  // set only when SLICE 10's allowlist wrote DNS pins.
};

// Hand-authors a complete OCI runtime-spec JSON document -- `--config` mode's `oci.WithSpecFromFile`
// (containerd's `run_unix.go`, confirmed this pass) applies NO other SpecOpts, so this function alone
// is responsible for everything the convenience-flag path previously got from
// `oci.WithDefaultSpecForPlatform`/`oci.WithDefaultUnixDevices` plus this backend's own `--mount`/
// `--memory-limit`/`--rlimit-nofile` flags. Default namespaces/capabilities/masked-and-readonly-paths/
// mounts/rlimits below are copied from containerd's real `pkg/oci/spec.go`
// (`populateDefaultUnixSpec`/`defaultUnixCaps`/`defaultUnixNamespaces`) and `pkg/oci/mounts.go`
// (`defaultMounts`), fetched and read directly this pass -- real parity with what every container
// already gets via the convenience-flag path today, not a hand-invented, narrower security posture.
// Device-cgroup allowlisting beyond the base default-deny rule (`oci.WithDefaultUnixDevices`) is
// deliberately NOT reproduced -- for a Kata guest, `/dev` is populated by the GUEST's own kernel at
// boot, not governed by the HOST's device-cgroup rules the way a runc container's shared-kernel `/dev`
// is, so that specific default carries far less weight here; named as a scoped, deliberate difference
// from byte-for-byte parity, not an oversight.
[[nodiscard]] std::string build_oci_spec_json(OciSpecInputs const& in) {
    using agentengine::json::Value;

    std::vector<Value> caps;
    for (char const* c : {"CAP_CHOWN", "CAP_DAC_OVERRIDE", "CAP_FSETID", "CAP_FOWNER", "CAP_MKNOD",
                           "CAP_NET_RAW", "CAP_SETGID", "CAP_SETUID", "CAP_SETFCAP", "CAP_SETPCAP",
                           "CAP_NET_BIND_SERVICE", "CAP_SYS_CHROOT", "CAP_KILL", "CAP_AUDIT_WRITE"}) {
        caps.push_back(Value::make_string(c));
    }
    Value const capabilities = Value::make_object({
        {"bounding", Value::make_array(caps)},
        {"permitted", Value::make_array(caps)},
        {"effective", Value::make_array(caps)},
    });

    // SLICE 7's `fds` -> `RLIMIT_NOFILE` mapping, carried into the spec's own `process.rlimits`
    // field instead of a `--rlimit-nofile` flag; 1024/1024 is containerd's own default when unset
    // (`populateDefaultUnixSpec`), not a value this backend invented.
    std::uint64_t const rlimit_nofile = in.fds > 0 ? in.fds : 1024;
    Value const rlimits = Value::make_array({Value::make_object({
        {"type", Value::make_string("RLIMIT_NOFILE")},
        {"hard", Value::make_number(static_cast<double>(rlimit_nofile))},
        {"soft", Value::make_number(static_cast<double>(rlimit_nofile))},
    })});

    Value const process = Value::make_object({
        {"terminal", Value::make_bool(false)},
        {"user", Value::make_object({{"uid", Value::make_number(0)}, {"gid", Value::make_number(0)}})},
        {"args", Value::make_array({Value::make_string("sleep"), Value::make_string("infinity")})},
        {"cwd", Value::make_string("/")},
        {"env", Value::make_array({Value::make_string(
             "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin")})},
        {"noNewPrivileges", Value::make_bool(true)},
        {"capabilities", capabilities},
        {"rlimits", rlimits},
    });

    Value const root = Value::make_object({
        {"path", Value::make_string(in.rootfs_path)},
        {"readonly", Value::make_bool(false)},
    });

    std::vector<Value> mounts_json;
    auto const push_mount = [&](std::string dest, std::string type, std::string source,
                                 std::vector<std::string> options) {
        std::vector<Value> opts_json;
        opts_json.reserve(options.size());
        for (auto& o : options) opts_json.push_back(Value::make_string(std::move(o)));
        mounts_json.push_back(Value::make_object({
            {"destination", Value::make_string(std::move(dest))},
            {"type", Value::make_string(std::move(type))},
            {"source", Value::make_string(std::move(source))},
            {"options", Value::make_array(std::move(opts_json))},
        }));
    };
    push_mount("/proc", "proc", "proc", {"nosuid", "noexec", "nodev"});
    push_mount("/dev", "tmpfs", "tmpfs", {"nosuid", "strictatime", "mode=755", "size=65536k"});
    push_mount("/dev/pts", "devpts", "devpts",
               {"nosuid", "noexec", "newinstance", "ptmxmode=0666", "mode=0620", "gid=5"});
    push_mount("/dev/shm", "tmpfs", "shm", {"nosuid", "noexec", "nodev", "mode=1777", "size=65536k"});
    push_mount("/dev/mqueue", "mqueue", "mqueue", {"nosuid", "noexec", "nodev"});
    push_mount("/sys", "sysfs", "sysfs", {"nosuid", "noexec", "nodev", "ro"});
    push_mount("/run", "tmpfs", "tmpfs", {"nosuid", "strictatime", "mode=755", "size=65536k"});
    if (in.hosts_file_path.has_value()) {
        // SLICE 10: pins the guest's OWN DNS resolution of each allowlisted hostname to the EXACT IP
        // this backend resolved+validated+firewalled at create() time -- without this, the guest's
        // independent resolution of the same hostname could legitimately return a DIFFERENT IP
        // (round-robin/geo DNS, a different resolver), which the nftables allowlist (scoped to the
        // create()-time-resolved IP specifically) would then correctly, but unhelpfully, block. A
        // real correctness gap found during this design, not copied from an existing pattern.
        push_mount("/etc/hosts", "bind", *in.hosts_file_path, {"rbind", "ro"});
    }
    if (in.mounts != nullptr) {
        for (MountSpec const& m : *in.mounts) {
            // `MountSpec::source` as a `BlobRef`, and any ',' in either path, are already rejected by
            // `create()` before this function ever runs -- `std::get` here is safe by that contract,
            // not re-validated. The ',' check is now VESTIGIAL for injection purposes specifically
            // (each field is its own escaped JSON string; the old single delimited `--mount` value
            // this defended is gone) but is kept anyway: no legitimate absolute path needs a literal
            // comma, and silently widening what create() accepts without a reason is its own kind of
            // regression risk.
            std::string const& host_path = std::get<std::string>(m.source);
            push_mount(m.guest_path, "bind", host_path, {"rbind", m.read_write ? "rw" : "ro"});
        }
    }

    std::vector<Value> namespaces_json;
    namespaces_json.push_back(Value::make_object({{"type", Value::make_string("pid")}}));
    namespaces_json.push_back(Value::make_object({{"type", Value::make_string("ipc")}}));
    namespaces_json.push_back(Value::make_object({{"type", Value::make_string("uts")}}));
    namespaces_json.push_back(Value::make_object({{"type", Value::make_string("mount")}}));
    // The network namespace entry is ALWAYS present -- omitting it entirely would join the HOST's
    // own network namespace (OCI runtime-spec semantics: a namespace type absent from this list means
    // "inherit the runtime's own", not "none"), a full ambient-network regression from today's
    // behavior. `netns_path` unset -> a FRESH, empty netns (today's `deny_all` posture: isolated,
    // nothing bridged in -- matches what `oci.WithDefaultSpecForPlatform`'s own default namespace
    // list already produces for every container the convenience-flag path creates). `netns_path` set
    // -> join the pre-created, CNI-populated netns SLICE 10 built before this spec was written.
    if (in.netns_path.has_value()) {
        namespaces_json.push_back(Value::make_object(
            {{"type", Value::make_string("network")}, {"path", Value::make_string(*in.netns_path)}}));
    } else {
        namespaces_json.push_back(Value::make_object({{"type", Value::make_string("network")}}));
    }

    std::vector<Value> masked_paths;
    for (char const* p : {"/proc/acpi", "/proc/asound", "/proc/kcore", "/proc/keys",
                           "/proc/latency_stats", "/proc/timer_list", "/proc/timer_stats",
                           "/proc/sched_debug", "/sys/firmware", "/sys/devices/virtual/powercap",
                           "/proc/scsi"}) {
        masked_paths.push_back(Value::make_string(p));
    }
    std::vector<Value> readonly_paths;
    for (char const* p : {"/proc/bus", "/proc/fs", "/proc/irq", "/proc/sys", "/proc/sysrq-trigger"}) {
        readonly_paths.push_back(Value::make_string(p));
    }

    std::vector<std::pair<std::string, Value>> resources_members;
    resources_members.emplace_back(
        "devices", Value::make_array({Value::make_object(
                       {{"allow", Value::make_bool(false)}, {"access", Value::make_string("rwm")}})}));
    if (in.memory_bytes > 0) {
        resources_members.emplace_back(
            "memory", Value::make_object(
                          {{"limit", Value::make_number(static_cast<double>(in.memory_bytes))}}));
    }
    // SLICE 11 (docs/planning/kata-backend-config-pipeline-pids-disk-net-redesign-draft.md §3):
    // reopens ADR-090's own negative decision -- that investigation correctly found no `ctr run`
    // convenience flag for pids, and SLICE 9's own `--config` rewrite (unrelated motivation: the
    // NetPolicy allowlist) makes this the identical shape memory_bytes already has, no new mechanism.
    if (in.pids > 0) {
        resources_members.emplace_back(
            "pids", Value::make_object({{"limit", Value::make_number(static_cast<double>(in.pids))}}));
    }

    Value const linux_section = Value::make_object({
        {"namespaces", Value::make_array(std::move(namespaces_json))},
        {"maskedPaths", Value::make_array(std::move(masked_paths))},
        {"readonlyPaths", Value::make_array(std::move(readonly_paths))},
        {"cgroupsPath", Value::make_string(in.cgroups_path)},
        {"resources", Value::make_object(std::move(resources_members))},
    });

    Value const spec = Value::make_object({
        {"ociVersion", Value::make_string("1.0.2")},
        {"process", process},
        {"root", root},
        {"mounts", Value::make_array(std::move(mounts_json))},
        {"linux", linux_section},
    });
    return agentengine::json::dump(spec);
}

}  // namespace

}  // namespace agentengine::kata
