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
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "agentengine/core/json_value.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"

extern char** environ;

namespace agentengine::kata {

namespace {

namespace fs = std::filesystem;

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

// SLICE 10: the env pair `cnitool` needs on every invocation -- built once per call site rather than
// duplicated at each of create()'s/destroy()'s several `cnitool` call sites.
[[nodiscard]] std::vector<std::string> cni_env(std::string const& plugin_dir, std::string const& conf_dir) {
    return {"CNI_PATH=" + plugin_dir, "NETCONFPATH=" + conf_dir};
}

// SLICE 11: this backend's own per-instance workdir root -- named once so create()'s mount-exclusion
// check (below) and the workdir path this function's own callers build cannot silently drift apart.
constexpr std::string_view kKataWorkdirRoot = "/run/agentengine-kata";

// SLICE 11 (red-team finding, BLOCKING, fixed before landing -- design draft §5a'): a MountSpec host
// path targeting this backend's OWN workdir was never rejected by anything -- `authorize_spec()`
// (sandbox.hpp) skips its own cap::SandboxMount coverage check entirely for a caller holding ZERO
// SandboxMount grants at all (its own documented opt-out shape), and this backend's own mount
// validation in create() (unchanged since SLICE 2) only ever rejected a BlobRef source and a literal
// ','. Once disk_bytes > 0 makes `<workdir>/upper.img` the live backing file of an actively
// loop-mounted, actively overlay-mounted filesystem, a bind mount straight into it bypasses the
// loop/ext4 I/O path entirely -- a way to corrupt a filesystem mounted live through a different path,
// reachable with NO capability grant at all. This check is therefore unconditional, independent of
// any cap::SandboxMount grant, called for every MountSpec regardless of the caller's capabilities --
// the backend's own bookkeeping directory was never meant to be nameable by a caller at all.
[[nodiscard]] bool targets_own_workdir(std::string const& host_path) {
    // Reject any lexical '.'/'..' component outright first -- the same "don't try to canonicalize
    // through it, reject the injection surface instead" idiom
    // sandbox_detail::has_dot_or_dotdot_component (sandbox.hpp) already establishes for exactly this
    // reason: a path with an embedded '..' could otherwise defeat a naive prefix comparison with no
    // filesystem access at all.
    if (agentengine::sandbox_detail::has_dot_or_dotdot_component(host_path)) return true;
    std::string const root(kKataWorkdirRoot);
    if (host_path == root) return true;
    std::string const prefix = root + "/";
    return host_path.rfind(prefix, 0) == 0;
}

// SLICE 11: `losetup -f --show` prints the attached device path (e.g. "/dev/loop3") followed by a
// single trailing newline -- the first place this file needs to parse a `ctr`/host-CLI call's stdout
// as a VALUE to feed a later command, rather than only checking exit_code. A trailing '\n' (or '\r\n'
// under no circumstance this backend runs on, but stripped anyway for cheap defense-in-depth) would
// otherwise become part of a device path this function's caller later hands to `mount`/`losetup -d`.
[[nodiscard]] std::string trim_trailing_newline(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
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

    // Slice 2 check, unchanged in substance (just no longer building a delimited `--mount` flag
    // value out of it -- SLICE 9's spec builder takes structured MountSpec objects directly):
    // MountSpec::source as a BlobRef fails closed, and a ',' in either path is rejected outright.
    for (MountSpec const& mount : spec.mounts) {
        if (std::get_if<std::string>(&mount.source) == nullptr) {
            return std::unexpected(error{
                failure_class::policy,
                "kata_backend: MountSpec::source as a BlobRef is not supported by KataBackend yet "
                "(host paths only, same scope gap as LinuxNativeJailBackend)",
                "kata_backend.blob_mount_unsupported"});
        }
        std::string const& host_path = std::get<std::string>(mount.source);
        if (host_path.find(',') != std::string::npos || mount.guest_path.find(',') != std::string::npos) {
            return std::unexpected(error{
                failure_class::policy,
                "kata_backend: MountSpec host path or guest_path contains ',' -- rejected outright "
                "(no legitimate absolute path needs a literal comma)",
                "kata_backend.mount_path_invalid"});
        }
        // SLICE 11 (red-team finding, BLOCKING, fixed before landing -- see targets_own_workdir()'s
        // own comment above): unconditional, independent of any cap::SandboxMount grant.
        if (targets_own_workdir(host_path)) {
            return std::unexpected(error{
                failure_class::policy,
                "kata_backend: MountSpec host path targets this backend's own workdir root (" +
                    std::string(kKataWorkdirRoot) + ") -- rejected outright, independent of any "
                    "cap::SandboxMount grant (this directory is never nameable by a caller)",
                "kata_backend.workdir_mount_forbidden"});
        }
        // SLICE 12 (red-team finding, BLOCKING, fixed before landing): `guest_path` now feeds a REAL
        // `fs::create_directories()` call under `rootfs_dir` (this function's own new mount-point
        // pre-creation loop, below) -- `std::filesystem::path::relative_path()` strips only the
        // leading root-name/root-directory, it does NOT strip embedded `..` components (empirically
        // verified), so an unvalidated `guest_path` like `/../../../../etc/cron.d/x` would make that
        // call create real directories OUTSIDE `rootfs_dir`, on the HOST, as whatever privilege this
        // process runs under. `authorize_spec()` (sandbox.hpp) only rejects a `.`/`..` component when
        // the caller holds a `cap::SandboxMount` grant -- every existing call site today holds none at
        // all (that function's own documented "opt-out preserved" shape), so nothing upstream of this
        // point defends `guest_path` in the common case. Unconditional here, exactly matching
        // `targets_own_workdir()`'s own unconditional treatment of `host_path` for the identical risk
        // class on the other side of this same struct -- never gated on a capability grant this
        // backend's own directory-creation side effect does not depend on.
        if (agentengine::sandbox_detail::has_dot_or_dotdot_component(mount.guest_path)) {
            return std::unexpected(error{
                failure_class::policy,
                "kata_backend: MountSpec guest_path contains a '.'/'..' path component -- rejected "
                "outright, independent of any cap::SandboxMount grant (this function creates real "
                "host-side directories from this value)",
                "kata_backend.mount_path_invalid"});
        }
    }

    // SLICE 9/10 ordering note: NetPolicy validation -- including allowlist parsing and DNS
    // resolution -- runs BEFORE any resource is acquired (rootfs mount, netns, CNI). A caller who
    // asked for something this backend cannot honor fails fast, the same low cost Slice 2's original
    // convenience-flag-path check had, rather than paying for a full `ctr images mount` first only to
    // reject the request afterward.
    bool const wants_network = !spec.net.deny_all || !spec.net.allowlist.empty();
    if (wants_network && spec.capabilities.sandbox_net_out_grants().empty()) {
        // Red-team finding (BLOCKING, found against this exact Slice -- see decisions/ADR-093-
        // kata-backend-netpolicy-allowlist-config-cni.md §5): `authorize_spec()` (sandbox.hpp)
        // deliberately SKIPS its own `cap::SandboxNetOut` coverage check when a caller holds ZERO
        // `SandboxNetOut` grants at all -- a documented "opt-out preserved" backward-compatibility
        // shape for callers who never adopted the capability system at all (sandbox.hpp's own
        // comment, proven live by `test_sandbox_capability_authorization.cpp`'s own G1 case). Before
        // this Slice that vacuous skip was harmless HERE specifically, because `KataBackend::create()`
        // itself ALWAYS failed closed on any non-`deny_all` `NetPolicy` regardless of capabilities
        // (Slice 2/3) -- THIS backend's own unconditional check was the real backstop, not
        // `authorize_spec()`'s. Slice 10 removed that backstop and replaced it with a mechanism that
        // acts on `spec.net` directly: without this check, a caller could reach REAL network egress
        // via `spec.net.allowlist` alone, with NO `cap::SandboxNetOut` grant ever held -- ambient
        // authority over a real network path, a direct I2 violation. This restores this backend's own
        // backstop, scoped to the zero-grant case only: `authorize_spec()` above already rejects a
        // request that IS covered by some grant but not this exact one (`sandbox.net_not_authorized`),
        // so only the "zero grants at all" gap needs closing here, not coverage logic again.
        return std::unexpected(error{
            failure_class::policy,
            "kata_backend: NetPolicy requests real network access (deny_all=false or a nonempty "
            "allowlist) but the caller holds no cap::SandboxNetOut grant at all -- this backend's "
            "NetPolicy mechanism is real as of Slice 10 and can no longer rely on authorize_spec()'s "
            "own opt-out-when-no-grants shape as an implicit backstop",
            "kata_backend.net_capability_required"});
    }
    // NOTE: "deny_all == false with an EMPTY allowlist" (unrestricted egress -- this backend can only
    // ever enforce a positive allowlist) is NOT separately checked here. It is provably unreachable
    // at this point: with zero cap::SandboxNetOut grants, the capability check just above already
    // rejected it (kata_backend.net_capability_required); with any grant present, authorize_spec()
    // itself (sandbox.hpp, called at the very top of this function, before this line ever runs) has
    // its own identical rejection for exactly this shape ("sandbox.net_not_authorized" -- "deny_all
    // == false with an EMPTY allowlist means unrestricted egress requested... no finite grant can
    // authorize that"). A third, redundant check here would be dead code on every real call path --
    // this project's own "don't validate scenarios that can't happen" discipline, not an oversight.
    //
    // SLICE 10 (ADR-093): resolve each allowlist entry to a verified IPv4 literal via the SAME
    // resolve_and_validate() HostEgressProxy itself uses (SSRF-safe: blocks loopback/link-local/
    // RFC1918/CGNAT/multicast/reserved/metadata-address ranges) -- reused, not re-implemented. Any
    // entry that fails to parse or resolve fails create() closed, before any resource is acquired.
    std::vector<std::tuple<std::string, std::string, std::uint16_t>> resolved;  // host, ip, port
    std::ostringstream hosts_content;
    hosts_content << "127.0.0.1 localhost\n::1 localhost\n";
    if (wants_network) {
        for (std::string const& raw_entry : spec.net.allowlist) {
            auto parsed = parse_allowlist_entry(raw_entry);
            if (!parsed.has_value()) return std::unexpected(parsed.error());
            auto const& [host, port, scheme] = *parsed;
            (void)scheme;  // this backend enforces TCP host:port reachability only -- the scheme
                            // field is recorded in the allowlist entry for the caller's own
                            // documentation, matching HostEgressProxy's own posture of trusting a
                            // grant's scheme field rather than re-deriving it here.
            auto verified = agentengine::sandbox::resolve_and_validate(host, port);
            if (!verified.has_value()) {
                return std::unexpected(error{
                    failure_class::policy,
                    "kata_backend: NetPolicy::allowlist entry '" + raw_entry +
                        "' failed to resolve to a permitted address: " + verified.error().message,
                    "kata_backend.allowlist_entry_blocked"});
            }
            std::string const ip = ipv4_to_dotted(verified->ipv4_host_order);
            resolved.emplace_back(host, ip, verified->port);
            hosts_content << ip << ' ' << host << '\n';
        }
    }

    std::string const id = fresh_id("ae-kata");
    fs::path const workdir = fs::path("/run/agentengine-kata") / id;
    fs::path const lower_dir = workdir / "lower";
    fs::path const rootfs_dir = workdir / "rootfs";
    fs::path const quota_root = workdir / "quota_root";  // only populated when disk_bytes > 0, but
                                                            // the path itself is cheap to compute
                                                            // unconditionally -- cleanup_partial()
                                                            // below needs it in scope regardless.
    std::error_code fs_ec;

    // SLICE 11: a cleanup lambda for every early-return path below -- lower/loop/overlay/netns/CNI
    // resources are acquired incrementally, and each failure path must unwind exactly what was
    // acquired so far, IN REVERSE ORDER, never more (an `ip netns delete` before `ip netns add` ever
    // ran is a harmless no-op error, but calling it unconditionally would mask a REAL failure's own
    // error text in the logs with an irrelevant one). Red-team finding (BLOCKING, fixed before
    // landing): the pre-SLICE-11 version of this lambda ran `fs::remove_all(workdir)` BEFORE its own
    // `ctr images unmount` call -- harmless while rootfs_dir was always a read-only View() mount
    // (remove_all recursing into it just yielded per-file EROFS, silently swallowed), but a real
    // hazard once rootfs_dir/quota_root can be LIVE WRITABLE mounts (std::filesystem::remove_all has
    // no "stay on one filesystem" guard) -- fixed by unmounting/detaching everything first, in exact
    // reverse-acquisition order, before removing the workdir tree.
    bool netns_created = false;
    bool cni_added = false;
    bool lower_mounted = false;
    bool loop_attached = false;
    bool loop_mounted = false;
    bool overlay_mounted = false;  // covers BOTH the real disk_bytes>0 overlay mount and the plain
                                    // disk_bytes==0 bind mount of lower_dir onto rootfs_dir -- both
                                    // unwind identically (a plain `umount rootfs_dir`).
    std::optional<std::string> netns_path_for_cleanup;
    std::string loop_device;
    auto const cleanup_partial = [&]() {
        if (cni_added) {
            (void)run_ctr({"cnitool", "del", cni_network_name_, *netns_path_for_cleanup},
                           kProcessTimeoutSeconds, kOutputSafetyCapBytes,
                           cni_env(cni_plugin_dir_, cni_conf_dir_));
        }
        if (netns_created) (void)run_ctr({"ip", "netns", "delete", id});
        if (overlay_mounted) (void)run_ctr({"umount", rootfs_dir.string()});
        if (loop_mounted) (void)run_ctr({"umount", quota_root.string()});
        if (loop_attached) (void)run_ctr({"losetup", "-d", loop_device});
        if (lower_mounted) (void)run_ctr({"ctr", "images", "unmount", lower_dir.string()});
        std::error_code ec;
        fs::remove_all(workdir, ec);
    };

    fs::create_directories(lower_dir, fs_ec);
    if (!fs_ec) fs::create_directories(rootfs_dir, fs_ec);
    if (fs_ec) {
        std::error_code ec;
        fs::remove_all(workdir, ec);
        return std::unexpected(error{failure_class::fatal,
                                      "kata_backend: failed to create rootfs mount directories under " +
                                          workdir.string() + ": " + fs_ec.message(),
                                      "kata_backend.rootfs_dir_failed"});
    }

    // SLICE 9 (ADR-093 finding 2): `ctr images mount` pulls+unpacks `image_` if needed and mounts its
    // (read-only View()) rootfs at a caller-chosen path -- solves the exact chain-ID rootfs-prep gap
    // ADR-090 found blocking `--config` mode for `pids`, via a subcommand that investigation's own
    // session did not fetch (`cmd/ctr/commands/images/mount.go`). SLICE 11: this now targets
    // `lower_dir`, never `rootfs_dir` directly -- `rootfs_dir` is always a SEPARATE mount on top of
    // it (a real overlay when disk_bytes > 0, a plain bind mount otherwise, see below) -- fixes a
    // real snapshot-leak red-team finding where `destroy()`'s own unmount call would otherwise
    // silently mistarget the overlay mountpoint instead of the real `ctr images mount` target once
    // the two stopped being the same path.
    auto mount_outcome = run_ctr({"ctr", "images", "mount", image_, lower_dir.string()});
    if (!mount_outcome.has_value() || mount_outcome->exit_code != 0) {
        std::error_code ec;
        fs::remove_all(workdir, ec);
        return std::unexpected(error{
            failure_class::fatal,
            "kata_backend: ctr images mount failed: " +
                (mount_outcome.has_value() ? mount_outcome->stderr_text : mount_outcome.error().message),
            "kata_backend.rootfs_mount_failed"});
    }
    lower_mounted = true;

    // SLICE 11 (docs/planning/kata-backend-config-pipeline-pids-disk-net-redesign-draft.md §5):
    // `disk_bytes > 0` builds a backend-owned, loop-device-backed, fixed-size ext4 filesystem and
    // uses it as a real overlay's writable half -- enforcement is a real guest-kernel ENOSPC once the
    // loop filesystem fills, not a heuristic.
    //
    // SLICE 12 fix (this pass, found via the FIRST real deployment this whole subsystem has ever run
    // against -- see this file's own top comment / decisions/ADR-096-... for the full account):
    // `disk_bytes == 0` (unset) used to be a PLAIN, READ-ONLY bind mount of `lower_dir` straight onto
    // `rootfs_dir` -- `ctr images mount` (above) mounts a snapshot's content read-only, and a bind
    // mount of a read-only source is itself read-only, so `rootfs_dir` was read-only in the default,
    // no-quota case. Empirically reproduced against a real Kata 4.1.0/cloud-hypervisor deployment:
    // Kata's Rust guest agent does NOT auto-create a missing mount-point directory before mounting
    // something there (unlike runc, which does) -- `busybox:latest`'s own image ships ONLY `/dev` as a
    // pre-existing top-level directory, so every one of `build_oci_spec_json()`'s own default mounts
    // targeting `/proc`, `/sys`, `/run`, `/dev/pts`, `/dev/shm`, `/dev/mqueue` failed guest-side
    // `create_container` with a bare `ENOENT`, for EVERY default (no-disk-quota) `create()` call, with
    // ANY image that doesn't happen to ship all of those directories pre-created. Bisected empirically
    // (manual `ctr run --config` probes against the real deployment, narrowing field-by-field) before
    // writing this fix -- confirmed a real, writable overlay with the missing directories pre-created
    // resolves it (a real `sleep infinity` task reaches RUNNING state and is killable normally).
    //
    // Fixed by giving `rootfs_dir` a real, writable overlay UNCONDITIONALLY, not just when a disk
    // quota was requested: `disk_bytes > 0` still builds the loop-device-backed ext4 filesystem
    // (enforcement stays real, unchanged); `disk_bytes == 0` now builds a plain, unbounded `tmpfs` at
    // `quota_root` instead -- existing ONLY so this function can create the missing mount-point
    // directories into a writable layer, not for any quota-enforcement purpose (a `tmpfs` has no size
    // cap here, deliberately -- the no-quota case was never meant to enforce one). Both branches feed
    // the SAME overlay-mount step below, so `rootfs_dir` is a real overlay in every case now -- the
    // separate plain-bind-mount code path this comment used to describe is gone.
    bool const disk_quota_active = spec.limits.disk_bytes > 0;
    fs::create_directories(quota_root, fs_ec);
    if (fs_ec) {
        cleanup_partial();
        return std::unexpected(error{failure_class::fatal,
                                      "kata_backend: failed to create rootfs writable-layer staging "
                                      "directory " +
                                          quota_root.string() + ": " + fs_ec.message(),
                                      "kata_backend.disk_quota_dir_failed"});
    }

    if (disk_quota_active) {
        fs::path const upper_img = workdir / "upper.img";
        {
            // SLICE 11 (red-team finding, resolved rather than left open): serializes ONLY this
            // sequence -- fallocate/mkfs.ext4/losetup -f/loop-mount -- across concurrent create()
            // calls. `losetup -f` (find a free loop device) has a TOCTOU race under concurrency that
            // the kernel's own LOOP_SET_FD exclusivity makes benign for corruption (one racer gets
            // EBUSY, never a double-attach) but real against this registry's own documented
            // "tolerate concurrent create/exec/destroy calls from unrelated sessions" contract (a
            // purely load-driven race would otherwise spuriously fail an unrelated caller's
            // create()). Released before the overlay mount / `ctr run --config`, neither of which
            // touch shared loop-device allocation state.
            std::lock_guard<std::mutex> const disk_quota_lock(disk_quota_setup_mutex_);

            // fallocate -l is an EAGER reservation (verified against fallocate(2)/ext4's own extent
            // behavior, NOT the lazy/sparse truncate -s primitive) -- a considered choice, not an
            // oversight: this makes disk_bytes a real, immediate host-disk-reserving admission cost
            // at create() time, the safer multi-tenant behavior (an operator oversubscribing hosts
            // around this backend's own declared budgets gets an honest, immediate ENOSPC here,
            // rather than a later, harder-to-attribute one after several lazily-allocated quotas
            // collide inside their guests).
            auto fallocate_outcome =
                run_ctr({"fallocate", "-l", std::to_string(spec.limits.disk_bytes), upper_img.string()});
            if (!fallocate_outcome.has_value() || fallocate_outcome->exit_code != 0) {
                cleanup_partial();
                return std::unexpected(error{
                    failure_class::fatal,
                    "kata_backend: fallocate for disk-quota image failed: " +
                        (fallocate_outcome.has_value() ? fallocate_outcome->stderr_text
                                                        : fallocate_outcome.error().message),
                    "kata_backend.disk_quota_fallocate_failed"});
            }

            // ext4, not a faster/simpler filesystem: the writable rootfs layer needs POSIX
            // permissions/symlinks/hardlinks/special files, which a FAT-family filesystem does not
            // support.
            auto mkfs_outcome = run_ctr({"mkfs.ext4", "-q", upper_img.string()});
            if (!mkfs_outcome.has_value() || mkfs_outcome->exit_code != 0) {
                cleanup_partial();
                return std::unexpected(error{
                    failure_class::fatal,
                    "kata_backend: mkfs.ext4 for disk-quota image failed: " +
                        (mkfs_outcome.has_value() ? mkfs_outcome->stderr_text
                                                   : mkfs_outcome.error().message),
                    "kata_backend.disk_quota_mkfs_failed"});
            }

            auto losetup_outcome = run_ctr({"losetup", "-f", "--show", upper_img.string()});
            if (!losetup_outcome.has_value() || losetup_outcome->exit_code != 0) {
                cleanup_partial();
                return std::unexpected(error{
                    failure_class::fatal,
                    "kata_backend: losetup -f --show for disk-quota image failed: " +
                        (losetup_outcome.has_value() ? losetup_outcome->stderr_text
                                                      : losetup_outcome.error().message),
                    "kata_backend.disk_quota_losetup_failed"});
            }
            loop_device = trim_trailing_newline(losetup_outcome->stdout_text);
            if (loop_device.empty()) {
                cleanup_partial();
                return std::unexpected(
                    error{failure_class::fatal,
                          "kata_backend: losetup -f --show reported success but printed no device path",
                          "kata_backend.disk_quota_losetup_failed"});
            }
            loop_attached = true;

            auto loop_mount_outcome = run_ctr({"mount", loop_device, quota_root.string()});
            if (!loop_mount_outcome.has_value() || loop_mount_outcome->exit_code != 0) {
                cleanup_partial();
                return std::unexpected(error{
                    failure_class::fatal,
                    "kata_backend: mounting the disk-quota loop device failed: " +
                        (loop_mount_outcome.has_value() ? loop_mount_outcome->stderr_text
                                                         : loop_mount_outcome.error().message),
                    "kata_backend.disk_quota_loop_mount_failed"});
            }
            loop_mounted = true;
        }  // disk_quota_lock released here -- overlay mount below needs no serialization.
    } else {
        // SLICE 12: the no-quota writable layer -- a SIZE-CAPPED tmpfs, existing only so this function
        // can create missing mount-point directories (and the empty /etc/hosts file, when network
        // access was granted) into a writable layer below (see this block's own top comment).
        //
        // REAL, independent-red-team-found finding (fixed same day, before landing): an earlier
        // version of this fix used an UNSIZED tmpfs here -- Linux's kernel default for an unsized
        // tmpfs is 50% of physical RAM, and because this writable layer is shared into the guest via
        // Kata's own virtiofs (a HOST-side daemon, entirely outside the guest's own memory cgroup),
        // ANY writable path in the overlay -- not just the pre-created mount-point directories -- was
        // a guest-reachable, zero-capability-gated, up-to-50%-of-host-RAM write primitive for every
        // default (no-disk-quota) `create()` call, composing across concurrently-created instances
        // with no coordination. This directly reopened a resource-budget question
        // (docs/planning/kata-backend-config-pipeline-pids-disk-net-redesign-draft.md §"Scope decision
        // for this draft"/§8) the project's own design doc explicitly deferred as a separate,
        // owner-decided follow-on -- landing it silently inside an unrelated ENOENT bugfix would have
        // been exactly the "contested... design landed without design -> red-team -> prove -> judge"
        // mistake CLAUDE.md's own discipline exists to catch. A real I8 (budgets enforced) regression
        // versus the ACTUAL prior behavior too: the pre-fix read-only bind mount enforced a budget of
        // literally zero writable bytes, not "no cap, same as before".
        //
        // Fixed by capping the tmpfs at a small, fixed size -- generous for its own STATED purpose
        // (a handful of empty directory entries plus one empty file; real directory/inode metadata for
        // that is a tiny fraction of this) but nowhere near enough to matter as a host-RAM DoS vector:
        // worst case, ANY writable path under this layer now costs the host a few MiB per instance, not
        // up to half of physical RAM. This does not reopen "should the no-quota case have real
        // writable capacity for actual data" -- that question stays exactly where the design doc left
        // it, a separate, not-yet-made decision -- it only makes this code's own already-stated intent
        // ("existing ONLY so this function can create missing mount-point directories") structurally
        // true, not merely descriptive.
        //
        // `loop_mounted` (not a new, separate flag) marks that SOMETHING is mounted at `quota_root`
        // needing `umount` at cleanup time -- the exact same cleanup step the loop-device branch
        // already uses, reused verbatim rather than adding a second, parallel flag for what is, from
        // cleanup's point of view, the identical obligation ("unmount quota_root"). `loop_attached`/
        // `loop_device` deliberately stay unset here -- no loop device exists to detach in this branch.
        auto tmpfs_outcome =
            run_ctr({"mount", "-t", "tmpfs", "-o", "size=4m", "tmpfs", quota_root.string()});
        if (!tmpfs_outcome.has_value() || tmpfs_outcome->exit_code != 0) {
            cleanup_partial();
            return std::unexpected(error{
                failure_class::fatal,
                "kata_backend: tmpfs mount for the rootfs writable layer failed: " +
                    (tmpfs_outcome.has_value() ? tmpfs_outcome->stderr_text
                                                : tmpfs_outcome.error().message),
                "kata_backend.disk_quota_loop_mount_failed"});
        }
        loop_mounted = true;
    }

    // overlayfs requires upperdir/workdir as two empty directories on the SAME filesystem (kernel
    // requirement, not this design's choice) -- both live inside `quota_root` (the just-mounted loop
    // filesystem or tmpfs, whichever branch above ran), not beside it. Runs UNCONDITIONALLY now
    // (SLICE 12) -- `rootfs_dir` is always a real overlay, never a plain bind mount, so every `create()`
    // call gets a writable rootfs regardless of whether a disk quota was requested.
    fs::path const upper_subdir = quota_root / "upper";
    fs::path const work_subdir = quota_root / "work";
    fs::create_directories(upper_subdir, fs_ec);
    if (!fs_ec) fs::create_directories(work_subdir, fs_ec);
    if (fs_ec) {
        cleanup_partial();
        return std::unexpected(
            error{failure_class::fatal,
                  "kata_backend: failed to create overlay upper/work directories under " +
                      quota_root.string() + ": " + fs_ec.message(),
                  "kata_backend.disk_quota_dir_failed"});
    }

    std::string const overlay_opts = "lowerdir=" + lower_dir.string() +
                                      ",upperdir=" + upper_subdir.string() +
                                      ",workdir=" + work_subdir.string();
    auto overlay_outcome =
        run_ctr({"mount", "-t", "overlay", "overlay", "-o", overlay_opts, rootfs_dir.string()});
    if (!overlay_outcome.has_value() || overlay_outcome->exit_code != 0) {
        cleanup_partial();
        return std::unexpected(error{
            failure_class::fatal,
            "kata_backend: overlay mount for rootfs failed: " +
                (overlay_outcome.has_value() ? overlay_outcome->stderr_text
                                              : overlay_outcome.error().message),
            "kata_backend.disk_quota_overlay_mount_failed"});
    }
    overlay_mounted = true;

    // SLICE 12 fix (this block's own top comment has the full account): Kata's guest agent does not
    // auto-create a missing mount-point directory the way runc does -- every destination
    // `build_oci_spec_json()` is about to declare below must already exist inside `rootfs_dir`, or
    // guest-side `create_container` fails closed with a bare ENOENT. Covers this function's own fixed
    // default mount set (matching that function's own list exactly) and every caller-supplied
    // `MountSpec::guest_path` -- both are always directories in this codebase's own convention
    // (matching `LinuxNativeJailBackend`'s identical treatment of `MountSpec`), so `create_directories`
    // is always the right call, never a file-touch.
    for (char const* guest_dir : {"/proc", "/dev", "/dev/pts", "/dev/shm", "/dev/mqueue", "/sys", "/run"}) {
        fs::path const p = rootfs_dir / fs::path(guest_dir).relative_path();
        fs::create_directories(p, fs_ec);
        if (fs_ec) {
            cleanup_partial();
            return std::unexpected(error{
                failure_class::fatal,
                "kata_backend: failed to pre-create the guest mount-point directory '" +
                    std::string(guest_dir) + "' inside the rootfs: " + fs_ec.message(),
                "kata_backend.mount_point_precreate_failed"});
        }
    }
    for (MountSpec const& m : spec.mounts) {
        fs::path const p = rootfs_dir / fs::path(m.guest_path).relative_path();
        fs::create_directories(p, fs_ec);
        if (fs_ec) {
            cleanup_partial();
            return std::unexpected(error{
                failure_class::fatal,
                "kata_backend: failed to pre-create the guest mount-point directory '" + m.guest_path +
                    "' inside the rootfs: " + fs_ec.message(),
                "kata_backend.mount_point_precreate_failed"});
        }
    }

    std::optional<std::string> netns_path;
    std::optional<std::string> hosts_file_path;

    if (wants_network) {
        fs::path const hosts_file = workdir / "hosts";
        std::ofstream(hosts_file) << hosts_content.str();
        hosts_file_path = hosts_file.string();

        // SLICE 12: `/etc/hosts` is a FILE bind-mount destination, not a directory -- a bind mount's
        // destination must already exist as the SAME type as the source (Linux mount(2) requirement),
        // so the directory-only pre-creation loop above does not cover it. `/etc` itself may not exist
        // either (busybox does ship it, but this must not assume any particular image does).
        {
            fs::path const etc_dir = rootfs_dir / "etc";
            fs::path const hosts_dest = etc_dir / "hosts";
            fs::create_directories(etc_dir, fs_ec);
            // REAL, independent-red-team-found finding (fixed same day, before landing): a plain
            // `fs::exists()` check does not distinguish file vs. directory -- if an image ever shipped
            // `/etc/hosts` as a directory (unusual, but `image_` is an arbitrary registry reference,
            // not trusted-by-construction), the old check would have wrongly reported "already there",
            // and this method would have returned success with the wrong destination type in place --
            // the real failure would only have surfaced later as a confusing guest-side ENOTDIR from
            // Kata's own bind-mount attempt, not the clear host-side pre-flight error this comment
            // claims to provide. `fs::status()` + `fs::is_regular_file()`/`fs::is_directory()` tell the
            // two cases apart for real.
            if (!fs_ec) {
                std::error_code status_ec;
                fs::file_status const st = fs::status(hosts_dest, status_ec);
                bool const missing = status_ec || !fs::exists(st);
                if (missing) {
                    std::ofstream(hosts_dest).close();
                } else if (!fs::is_regular_file(st)) {
                    fs_ec = std::make_error_code(std::errc::not_a_directory);
                }
            }
            if (fs_ec || !fs::is_regular_file(hosts_dest)) {
                cleanup_partial();
                return std::unexpected(error{
                    failure_class::fatal,
                    "kata_backend: failed to pre-create the guest /etc/hosts mount-point file inside "
                    "the rootfs (or it already exists as the wrong type -- a directory, not a file): " +
                        (fs_ec ? fs_ec.message() : std::string("file still missing after create")),
                    "kata_backend.mount_point_precreate_failed"});
            }
        }

        auto netns_outcome = run_ctr({"ip", "netns", "add", id});
        if (!netns_outcome.has_value() || netns_outcome->exit_code != 0) {
            cleanup_partial();
            return std::unexpected(error{
                failure_class::fatal,
                "kata_backend: ip netns add failed: " +
                    (netns_outcome.has_value() ? netns_outcome->stderr_text
                                                : netns_outcome.error().message),
                "kata_backend.netns_create_failed"});
        }
        netns_created = true;
        netns_path = std::string("/var/run/netns/") + id;
        netns_path_for_cleanup = netns_path;

        // cnitool (github.com/containernetworking/cni/cnitool) -- a real, separately-installed
        // reference CLI, NOT bundled with containerd/`ctr`. See this file's own header comment for
        // why manual orchestration, run BEFORE `ctr run --config`, is required for Kata specifically.
        auto cni_outcome = run_ctr({"cnitool", "add", cni_network_name_, *netns_path},
                                    kProcessTimeoutSeconds, kOutputSafetyCapBytes,
                                    cni_env(cni_plugin_dir_, cni_conf_dir_));
        if (!cni_outcome.has_value() || cni_outcome->exit_code != 0) {
            cleanup_partial();
            return std::unexpected(error{
                failure_class::fatal,
                "kata_backend: cnitool add failed: " +
                    (cni_outcome.has_value() ? cni_outcome->stderr_text : cni_outcome.error().message),
                "kata_backend.cni_add_failed"});
        }
        cni_added = true;

        // Default-deny egress inside the netns; loopback and the resolved allowlist IP:port pairs
        // only. Rules live in this netns's own nftables namespace -- deleting the netns (destroy(),
        // or this function's own cleanup_partial() on a later failure) discards them with it, no
        // separate `nft delete` step needed. NOT independently verified against a live deployment
        // this session (none reachable) -- the exact `nft` argv tokenization below (each clause as
        // its own argv entry) is assumed to parse the same as a space-joined command line, matching
        // every other `ctr` CLI assumption this file already discloses rather than hides.
        std::vector<std::vector<std::string>> nft_cmds;
        nft_cmds.push_back({"ip", "netns", "exec", id, "nft", "add", "table", "inet", "ae_netpolicy"});
        nft_cmds.push_back({"ip", "netns", "exec", id, "nft", "add", "chain", "inet", "ae_netpolicy",
                             "output", "{", "type", "filter", "hook", "output", "priority", "0", ";",
                             "policy", "drop", ";", "}"});
        nft_cmds.push_back({"ip", "netns", "exec", id, "nft", "add", "rule", "inet", "ae_netpolicy",
                             "output", "oif", "lo", "accept"});
        // SLICE 11 (docs/planning/kata-backend-config-pipeline-pids-disk-net-redesign-draft.md §4):
        // `net_bytes` -- a NAMED quota object shared by a rule on BOTH `output` (egress, this table's
        // pre-existing chain) and a NEW `input` chain (ingress) -- total bidirectional bytes, not
        // egress-only (an egress-only counter would miss a large inbound response). Red-team finding
        // (BLOCKING), fixed before landing: the quota-definition command needs `{ }` braces around
        // the quota spec (`nft(8)`'s own grammar) -- a first draft that omitted them would have been
        // a silent no-op (nft rejects it, create() correctly fails closed via nft_setup_failed below,
        // but net_bytes would ship completely non-functional). Ordering matters and is the actual
        // enforcement mechanism, not a style choice: nftables evaluates rules top-to-bottom per chain
        // and stops at the first terminal verdict -- `quota over` only matches once the counter has
        // ALREADY exceeded the budget, so the quota-drop rule must be added BEFORE the per-destination
        // `accept` rules (while under budget, it doesn't match and falls through to them; once
        // exceeded, it matches and drops before the allowlist rule below is ever reached). `input`'s
        // policy is `accept` (not `drop`) deliberately: `NetPolicy` is a *destination* allowlist,
        // already fully enforced by `output`'s own per-destination rules -- `input` here exists ONLY
        // to feed the shared byte counter, not to filter a second time (and, as `output` already does
        // today, this does not add any source/connection-tracking filter to `input` -- a pre-existing,
        // unchanged gap, not a new one this rule introduces).
        if (spec.limits.net_bytes > 0) {
            nft_cmds.push_back({"ip", "netns", "exec", id, "nft", "add", "chain", "inet", "ae_netpolicy",
                                 "input", "{", "type", "filter", "hook", "input", "priority", "0", ";",
                                 "policy", "accept", ";", "}"});
            nft_cmds.push_back({"ip", "netns", "exec", id, "nft", "add", "quota", "inet", "ae_netpolicy",
                                 "ae_net_budget", "{", "over", std::to_string(spec.limits.net_bytes),
                                 "bytes", "}"});
            nft_cmds.push_back({"ip", "netns", "exec", id, "nft", "add", "rule", "inet", "ae_netpolicy",
                                 "input", "iif", "lo", "accept"});
            nft_cmds.push_back({"ip", "netns", "exec", id, "nft", "add", "rule", "inet", "ae_netpolicy",
                                 "output", "quota", "name", "ae_net_budget", "drop"});
            nft_cmds.push_back({"ip", "netns", "exec", id, "nft", "add", "rule", "inet", "ae_netpolicy",
                                 "input", "quota", "name", "ae_net_budget", "drop"});
        }
        for (auto const& [host, ip, port] : resolved) {
            (void)host;
            nft_cmds.push_back({"ip", "netns", "exec", id, "nft", "add", "rule", "inet", "ae_netpolicy",
                                 "output", "ip", "daddr", ip, "tcp", "dport", std::to_string(port),
                                 "accept"});
        }
        for (auto const& cmd : nft_cmds) {
            auto nft_outcome = run_ctr(cmd);
            if (!nft_outcome.has_value() || nft_outcome->exit_code != 0) {
                cleanup_partial();
                return std::unexpected(error{
                    failure_class::fatal,
                    "kata_backend: nftables allowlist rule setup failed -- refusing to boot a guest "
                    "with a requested network policy this backend could not actually install (fail "
                    "closed, never fall back to unrestricted or silently-unenforced egress): " +
                        (nft_outcome.has_value() ? nft_outcome->stderr_text
                                                  : nft_outcome.error().message),
                    "kata_backend.nft_setup_failed"});
            }
        }
    }

    OciSpecInputs const spec_inputs{rootfs_dir.string(),
                                     &spec.mounts,
                                     spec.limits.memory_bytes,
                                     spec.limits.fds,
                                     spec.limits.pids,
                                     std::string("/agentengine-kata/") + id,
                                     netns_path,
                                     hosts_file_path};
    std::string const spec_json = build_oci_spec_json(spec_inputs);
    fs::path const spec_file = workdir / "config.json";
    std::ofstream(spec_file) << spec_json;

    auto outcome =
        run_ctr({"ctr", "run", "-d", "--runtime", runtime_type_, "--config", spec_file.string(), id});
    if (!outcome.has_value() || outcome->exit_code != 0) {
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
        cleanup_partial();
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
    Instance inst{id,
                  0,
                  spec.limits.wall_ms,
                  spec.limits.output_bytes,
                  rootfs_dir.string(),
                  lower_dir.string(),
                  loop_device,
                  disk_quota_active,
                  netns_created};
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

    // SLICE 4 (2026-08-24) fix, extended by SLICE 5 (2026-08-24) -- real gap found by this pass's own
    // red-team review (decisions/ADR-088-...md §3 finding #7, BLOCKING): the `kill(pid, SIGKILL)`
    // inside run_ctr()'s own force-terminate path only terminates the HOST-side `ctr` CLI wrapper
    // process this backend posix_spawn'd -- it has no host-visible pid for the GUEST-side process
    // that CLI was attached to. Before the SLICE 4 fix, a `wall_ms` timeout left that guest process
    // running orphaned inside the persistent `sleep infinity` container, invisible to the caller,
    // until a LATER exec()/destroy() call happened to reap it. SLICE 4 fixed this for the timeout
    // case only; SLICE 5 closes the structurally identical, previously-disclosed-but-not-fixed gap
    // for an `output_bytes` cap breach (`outcome->output_capped`) -- same orphan risk, same fix.
    // Best-effort: ask containerd to kill the guest-side process directly by the SAME --exec-id this
    // call minted above. NOT independently re-verified against a live Kata deployment this session
    // (none reachable) -- if the exact `ctr` CLI surface assumed here turns out wrong, this call
    // fails into the log line below rather than blocking the classification decided below it.
    if (outcome->timed_out || outcome->output_capped) {
        auto kill_outcome = run_ctr(
            {"ctr", "tasks", "kill", "--exec-id", exec_id, "--signal", "SIGKILL", inst.container_id});
        if (!kill_outcome.has_value() || kill_outcome->exit_code != 0) {
            std::fprintf(stderr,
                         "kata_backend: exec(%s) was force-terminated host-side (%s) and the "
                         "best-effort guest-side kill of --exec-id %s also did not report success -- "
                         "the guest workload may still be running orphaned inside container %s\n",
                         handle.opaque_id.c_str(),
                         outcome->timed_out ? "wall_ms timeout" : "output_bytes cap exceeded",
                         exec_id.c_str(), inst.container_id.c_str());
        }
    }

    // SLICE 5 red-team findings #2/#3 (BLOCKING): `output_capped` must be its own classification
    // branch, not folded into the exit_code-only ok/crash check below -- leaving it unchecked let a
    // run whose output was truncated and force-killed still report `ok` whenever the courtesy
    // non-blocking reap above happened to observe a clean exit race. `policy_violation` (not `crash`)
    // matches this codebase's own established idiom for "the HOST stopped this on policy grounds, not
    // the workload's own fault" -- the identical class `LinuxNativeJailBackend`'s idle-phase
    // CPU-budget kill and `MediatedPythonRunner`'s capability denials already use
    // (native_jail_backend.cpp, python_runner.hpp) -- rather than `crash`, which risks a caller
    // reading "the workload itself failed" and retrying unmodified, burning another full cap's worth
    // of guest resources for the same predictable result.
    if (outcome->timed_out) {
        result_out.klass = exec_outcome_class::timeout;
    } else if (outcome->output_capped) {
        result_out.klass = exec_outcome_class::policy_violation;
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

    // SLICE 9/10/11 cleanup, reverse creation order: rootfs/overlay/loop teardown, then the real
    // `ctr images unmount` of the snapshot, then (if used) CNI del + netns delete. nftables rules
    // inside the netns are discarded automatically when the netns itself is removed -- no separate
    // `nft` cleanup call needed (SLICE 10's own header-comment claim, unverified against a live
    // deployment this session, same disclosed posture as everything else here).
    fs::path const workdir = fs::path("/run/agentengine-kata") / id;
    fs::path const quota_root = workdir / "quota_root";

    // SLICE 11 (red-team finding, fixed before landing): the guest process is already dead by this
    // point (the task kill/rm/container-rm loop above already ran) -- overlay/loop teardown must
    // still happen in exact reverse-acquisition order (overlay -> loop mount -> loop detach) BEFORE
    // the real snapshot unmount below, mirroring create()'s own cleanup_partial() discipline.
    //
    // SLICE 12: `rootfs_dir` is a real overlay and `quota_root` has SOMETHING mounted at it
    // (loop-backed ext4 or a plain tmpfs) UNCONDITIONALLY now, not just when `disk_quota_active` --
    // both unmounts run every time; only the loop-device DETACH stays conditional on
    // `disk_quota_active` (a tmpfs has no loop device to detach). See create()'s own SLICE 12 comment
    // for the full rationale (a real, empirically-reproduced ENOENT this change fixes).
    auto overlay_unmount = run_ctr({"umount", it->second.rootfs_dir}, timeout_seconds, output_cap);
    if (!overlay_unmount.has_value() || overlay_unmount->exit_code != 0) {
        std::fprintf(stderr,
                      "kata_backend: destroy(%s): overlay unmount ('umount %s') did not succeed "
                      "cleanly -- possible leaked host mount, see host state directly\n",
                      id.c_str(), it->second.rootfs_dir.c_str());
    }
    auto loop_unmount = run_ctr({"umount", quota_root.string()}, timeout_seconds, output_cap);
    if (!loop_unmount.has_value() || loop_unmount->exit_code != 0) {
        std::fprintf(stderr,
                      "kata_backend: destroy(%s): rootfs writable-layer unmount ('umount %s') did not "
                      "succeed cleanly -- possible leaked host mount, see host state directly\n",
                      id.c_str(), quota_root.string().c_str());
    }
    if (it->second.disk_quota_active && !it->second.loop_device.empty()) {
        auto loop_detach = run_ctr({"losetup", "-d", it->second.loop_device}, timeout_seconds, output_cap);
        if (!loop_detach.has_value() || loop_detach->exit_code != 0) {
            std::fprintf(stderr,
                          "kata_backend: destroy(%s): 'losetup -d %s' did not succeed cleanly -- "
                          "possible leaked host loop device, see host state directly\n",
                          id.c_str(), it->second.loop_device.c_str());
        }
    }

    // SLICE 11 fix: the REAL snapshot mount/lease is always `lower_dir` now, never `rootfs_dir` --
    // before this slice, once `rootfs_dir` stopped being the literal `ctr images mount` target,
    // unmounting it here would have silently mistargeted and leaked the real snapshot every
    // `disk_bytes > 0` create/destroy cycle (a red-team finding, fixed before this code landed).
    auto unmount_outcome =
        run_ctr({"ctr", "images", "unmount", it->second.lower_dir}, timeout_seconds, output_cap);
    if (!unmount_outcome.has_value() || unmount_outcome->exit_code != 0) {
        std::fprintf(stderr,
                      "kata_backend: destroy(%s): rootfs snapshot unmount ('ctr images unmount %s') "
                      "did not succeed cleanly -- possible leaked host mount, see host state "
                      "directly\n",
                      id.c_str(), it->second.lower_dir.c_str());
    }
    std::error_code fs_ec;
    fs::remove_all(workdir, fs_ec);

    if (it->second.net_created) {
        std::string const netns_path = std::string("/var/run/netns/") + id;
        auto cni_del = run_ctr({"cnitool", "del", cni_network_name_, netns_path}, timeout_seconds,
                                output_cap, cni_env(cni_plugin_dir_, cni_conf_dir_));
        if (!cni_del.has_value() || cni_del->exit_code != 0) {
            std::fprintf(stderr,
                          "kata_backend: destroy(%s): 'cnitool del' did not succeed cleanly -- "
                          "possible leaked host network resource, see host state directly\n",
                          id.c_str());
        }
        auto netns_del = run_ctr({"ip", "netns", "delete", id}, timeout_seconds, output_cap);
        if (!netns_del.has_value() || netns_del->exit_code != 0) {
            std::fprintf(stderr,
                          "kata_backend: destroy(%s): 'ip netns delete' did not succeed cleanly -- "
                          "possible leaked host network namespace, see host state directly\n",
                          id.c_str());
        }
    }

    instances_.erase(it);
}

}  // namespace agentengine::kata
