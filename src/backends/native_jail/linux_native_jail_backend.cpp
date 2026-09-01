// Implements linux_native_jail_backend.hpp. See that header for the spec citations this satisfies.

// clone()/pipe2() are GNU/Linux extensions -- must be requested before any system header is
// pulled in transitively (glibc gates their declarations on this macro).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "backends/native_jail/linux_native_jail_backend.hpp"

#include <csignal>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

#include "backends/native_jail/seccomp_filter.hpp"

// glibc has no wrapper for pivot_root(2) (it is deliberately not exposed as a stable libc API --
// every real container runtime invokes it via syscall() the same way this file does).
#ifndef SYS_pivot_root
#error "SYS_pivot_root not available -- this backend requires a Linux kernel with pivot_root(2)"
#endif

namespace agentengine::native_jail {

namespace {

std::unexpected<ae::error> errno_error(char const* what, failure_class klass, char const* code) {
    int e = errno;
    return std::unexpected(
        ae::error{klass, std::string(what) + " failed: " + std::strerror(e), code});
}

// One delegated cgroup v2 root per deployment (mirrors native_jail_backend.cpp's
// SharedProfileState pattern on Windows) -- `create()` is idempotent (EEXIST tolerated), the
// controller-enablement write is attempted every time (cheap, and recovers if a previous attempt
// raced with the precondition not yet being met).
struct DelegatedRootState {
    bool ready = false;
    std::optional<ae::error> init_error;
};

DelegatedRootState ensure_delegated_root(std::string const& root) {
    DelegatedRootState state;
    if (::mkdir(root.c_str(), 0755) != 0 && errno != EEXIST) {
        state.init_error = errno_error(("mkdir(" + root + ")").c_str(), failure_class::fatal,
                                        "linux_native_jail.delegated_root_mkdir_failed")
                                .error();
        return state;
    }
    std::string subtree_control = root + "/cgroup.subtree_control";
    int fd = ::open(subtree_control.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        state.init_error = errno_error(("open(" + subtree_control + ")").c_str(), failure_class::fatal,
                                        "linux_native_jail.subtree_control_open_failed")
                                .error();
        return state;
    }
    std::string const enable = "+memory +pids";
    ssize_t written = ::write(fd, enable.data(), enable.size());
    int write_errno = errno;
    ::close(fd);
    // A prior successful enable makes a re-write a harmless no-op on some kernels and EBUSY/EINVAL
    // on others (already-enabled is not uniformly re-writable) -- treat write failure here as
    // fatal only combined with the controller files genuinely being absent later (create() surfaces
    // that concretely when it tries to write memory.max/pids.max), not here, to avoid a false
    // negative on the common "already enabled from a previous call" path.
    (void)written;
    (void)write_errno;
    state.ready = true;
    return state;
}

// Passed into the cloned child's entry function. All strings are fully constructed BEFORE
// `clone()` -- the child only reads already-materialized memory (`.c_str()`), never allocates or
// mutates a std::string itself, so the classic multithreaded-fork-into-non-async-signal-safe-code
// hazard does not apply to the string data itself. The child's own control flow (close/read/chdir/
// dup2/execve) is limited to plain syscalls. This assumes a single-threaded caller at the moment
// of `exec()` -- documented here as a residual scope limit (CLAUDE.md "explicit gaps, not
// silent"), not solved by a fully async-signal-safe path (posix_spawn-style) in this pass.
//
// The mount/pivot_root sequence below (jail setup) DOES construct/concatenate std::strings in the
// child -- safe under the same single-threaded-caller assumption as the rest of this file (a
// `clone()` without `CLONE_VM`/`CLONE_THREAD` gives the child its own copy-on-write address space,
// the same fork-safety shape as an ordinary single-threaded `fork()`, not the multithreaded-parent
// hazard the async-signal-safety literature warns about).
struct ChildMount {
    std::string host_path;
    std::string guest_path;  // always starts with '/'; joined onto jail_root below
    bool read_write = false;
};

struct ChildArgs {
    int sync_read_fd = -1;
    int sync_write_fd = -1;  // child's own duplicate; must close before blocking on sync_read_fd
    int stdout_write_fd = -1;
    int stderr_write_fd = -1;
    std::string jail_root;    // fresh, empty, host-created tmpfs mountpoint for this one exec()
    std::string cwd_guest;    // chdir target AFTER pivot_root, e.g. "/work"; empty = stay at "/"
    std::vector<ChildMount> mounts;
    std::string command;
};

// mkdir(2) with parents, tolerant of EEXIST at every level -- `path` must be absolute.
bool mkdir_p(std::string const& path) {
    if (path.empty() || path[0] != '/') return false;
    std::string partial;
    std::size_t pos = 1;
    for (;;) {
        std::size_t next = path.find('/', pos);
        partial = (next == std::string::npos) ? path : path.substr(0, next);
        if (!partial.empty() && ::mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) return false;
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return true;
}

// 008 SS9 G2/G3's Linux filesystem/process containment (see linux_native_jail_backend.hpp's own
// header comment for the full rationale, docs/planning/linux-native-jail-pivot-root-containment-
// design-draft.md for the design + self-red-team record this implements verbatim, and
// decisions/ADR-083-linux-native-jail-pivot-root-containment.md for the closing ADR and its real
// build/test evidence). Runs in the child, after the sync-pipe handshake
// (cgroup membership already confirmed by the parent) and BEFORE `install_seccomp_filter()` --
// `mount`/`umount2`/`pivot_root` are all in that filter's denylist, so this jail-construction code
// (trusted backend code, not yet the untrusted guest) must finish before the filter goes on.
// Returns false on ANY failure -- every caller treats that as `_exit()`, matching this file's
// existing "fail closed on setup failure" posture (e.g. the cgroup add_process check in exec()).
bool setup_jail(ChildArgs const& args) {
    // MUST-FIX 1 (design draft SS3): unconditionally first, before any other mount/pivot_root call
    // in this whole sequence. A systemd-managed host's root mount defaults to MS_SHARED; without
    // this, every bind mount and the pivot_root call below would propagate into the HOST's own
    // mount table (child-to-parent propagation direction), leaking real host /proc/mounts entries
    // that ordinary per-process mount-namespace teardown would never clean up.
    if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) return false;

    if (!mkdir_p(args.jail_root)) return false;
    // A dedicated tmpfs, not a bind-mount of any host directory -- an isolated mount point to
    // pivot into with no host-disk footprint beyond the mount itself (design draft SS2 step 2).
    // Generous-but-finite size, matching this file's Machine Safety posture elsewhere.
    if (::mount("tmpfs", args.jail_root.c_str(), "tmpfs", 0, "size=67108864,mode=0755") != 0) {
        return false;
    }

    for (ChildMount const& m : args.mounts) {
        std::string target = args.jail_root + m.guest_path;
        if (!mkdir_p(target)) return false;
        if (::mount(m.host_path.c_str(), target.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
            return false;
        }
        if (!m.read_write) {
            // MUST-FIX 2 (design draft SS3): the initial MS_BIND call silently ignores MS_RDONLY
            // passed in the SAME call -- a read-only grant is only real after this second,
            // MS_REMOUNT-flagged call.
            if (::mount(m.host_path.c_str(), target.c_str(), nullptr,
                        MS_BIND | MS_REMOUNT | MS_RDONLY | MS_REC, nullptr) != 0) {
                return false;
            }
        }
    }

    // pivot_root(2) requires its new-root argument to already BE a mount point -- a self-bind-mount
    // achieves that cheaply for a directory that otherwise wouldn't qualify (design draft SS2
    // step 4). jail_root already IS a mount point (the tmpfs above), so this is technically
    // redundant here, but kept for robustness against a future jail_root that isn't tmpfs-backed.
    if (::mount(args.jail_root.c_str(), args.jail_root.c_str(), nullptr, MS_BIND | MS_REC, nullptr) !=
        0) {
        return false;
    }

    std::string old_root = args.jail_root + "/.old_root";
    if (::mkdir(old_root.c_str(), 0700) != 0 && errno != EEXIST) return false;
    if (::chdir(args.jail_root.c_str()) != 0) return false;
    if (::syscall(SYS_pivot_root, ".", ".old_root") != 0) return false;
    if (::chdir("/") != 0) return false;
    // Lazily detach the old root -- nothing new is openable under it from here on, closing the
    // `chroot`-style escape class structurally (pivot_root, not chroot, is why this is possible at
    // all -- design draft SS2 step 4). Any fd the guest already held into the old root before this
    // point stays valid (ordinary Unix fd semantics); nothing this backend does hands the guest
    // such an fd.
    if (::umount2("/.old_root", MNT_DETACH) != 0) return false;
    ::rmdir("/.old_root");  // best-effort; now-empty from inside the new root

    // Fresh procfs, not a bind-mounted one -- mounted AFTER pivot_root, from a process already
    // inside the new CLONE_NEWPID namespace, gives a namespace-local view by construction (design
    // draft SS2 step 5). A bind-mounted host /proc would keep showing the host's real process list.
    if (::mkdir("/proc", 0555) != 0 && errno != EEXIST) return false;
    if (::mount("proc", "/proc", "proc", 0, nullptr) != 0) return false;

    if (!args.cwd_guest.empty() && ::chdir(args.cwd_guest.c_str()) != 0) return false;
    return true;
}

int child_entry(void* raw) {
    auto* args = static_cast<ChildArgs*>(raw);

    ::close(args->sync_write_fd);
    char sync_byte;
    ssize_t rc;
    do {
        rc = ::read(args->sync_read_fd, &sync_byte, 1);
    } while (rc < 0 && errno == EINTR);
    ::close(args->sync_read_fd);

    if (!setup_jail(*args)) _exit(125);

    ::dup2(args->stdout_write_fd, STDOUT_FILENO);
    ::dup2(args->stderr_write_fd, STDERR_FILENO);
    ::close(args->stdout_write_fd);
    ::close(args->stderr_write_fd);

    if (!install_seccomp_filter().has_value()) _exit(124);

    char const* argv[] = {"/bin/sh", "-c", args->command.c_str(), nullptr};
    char const* envp[] = {"PATH=/usr/bin:/bin", nullptr};
    ::execve("/bin/sh", const_cast<char* const*>(argv), const_cast<char* const*>(envp));
    _exit(126);  // execve itself failed
}

struct FdGuard {
    int fd = -1;
    ~FdGuard() { close_now(); }
    void close_now() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

std::string drain_pipe_bounded(int read_fd, std::uint64_t cap_bytes) {
    constexpr std::uint64_t kDefaultSafetyCapBytes = 16ull * 1024 * 1024;
    std::uint64_t const cap = cap_bytes > 0 ? cap_bytes : kDefaultSafetyCapBytes;
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(read_fd, buf, sizeof(buf));
        if (n <= 0) break;  // EOF or error -- either way, done
        std::uint64_t remaining = cap > out.size() ? cap - out.size() : 0;
        if (remaining == 0) break;
        std::uint64_t take = static_cast<std::uint64_t>(n) < remaining
                                  ? static_cast<std::uint64_t>(n)
                                  : remaining;
        out.append(buf, static_cast<std::size_t>(take));
        if (out.size() >= cap) break;
    }
    return out;
}

}  // namespace

result<SandboxHandle> LinuxNativeJailBackend::create(SandboxSpec const& spec, EffectContext&) {
    // docs/planning/sandbox-spec-capability-enforcement-design-draft.md (Slice 3): capabilities are
    // the real authority behind mounts/net, checked FIRST -- a no-op for every caller that doesn't
    // hold a SandboxMount/SandboxNetOut grant (see authorize_spec()'s own comment, sandbox.hpp).
    if (auto authorized = authorize_spec(spec); !authorized.has_value()) {
        return std::unexpected(authorized.error());
    }
    // decisions/ADR-087-sandbox-spec-capability-enforcement.md: closes a real per-backend divergence
    // this design's own red-team pass found (finding B3) -- KataBackend::create() already fails
    // closed on any NetPolicy beyond deny_all=true (ADR-086); this backend previously ignored
    // NetPolicy entirely. Independent of capabilities/authorize_spec() above: this backend has no
    // CNI/egress-proxy of any kind wired to honor a real allowlist, so the same identical spec would
    // otherwise silently mean two different things depending on which backend a host selected.
    if (!spec.net.deny_all || !spec.net.allowlist.empty()) {
        return std::unexpected(error{
            failure_class::policy,
            "linux_native_jail: NetPolicy with deny_all=false or a nonempty allowlist is not "
            "supported yet -- this backend has no CNI/egress-proxy wired to honor a real allowlist",
            "linux_native_jail.net_allowlist_unsupported"});
    }

    DelegatedRootState root_state = ensure_delegated_root(delegated_cgroup_root_);
    if (!root_state.ready) return std::unexpected(*root_state.init_error);

    auto instance = std::make_unique<Instance>();
    instance->limits = spec.limits;

    for (MountSpec const& mount : spec.mounts) {
        if (!std::holds_alternative<std::string>(mount.source)) {
            return std::unexpected(ae::error{
                failure_class::policy,
                "MountSpec::source as a BlobRef is not supported by LinuxNativeJailBackend yet "
                "(M2 Phase C scope gap -- host paths only)",
                "linux_native_jail.blob_mount_unsupported",
            });
        }
        if (mount.read_write && instance->cwd_guest_path.empty()) {
            instance->cwd_guest_path = mount.guest_path;
        }
    }
    instance->mounts = spec.mounts;

    // Host-unique, not just process-unique: PID + a per-process counter. A counter alone collides
    // across DIFFERENT test/host processes sharing this backend's default delegated paths (real,
    // observed directly: two `ctest -j` binaries both minting "linux_native_jail-0" and racing to
    // `mkdir` the identical cgroup/jail_root directory -- decisions/ADR-083-linux-native-jail-
    // pivot-root-containment.md §4). A PID cannot repeat among processes alive at the same instant,
    // so prefixing it closes the collision at its root rather than only papering over it with test
    // serialization (`RUN_SERIAL`, still kept in tests/CMakeLists.txt as defense in depth against a
    // reused PID from a since-exited process racing a still-running one's leftover directory).
    static std::atomic<std::uint64_t> counter{0};
    std::string id = "linux_native_jail-" + std::to_string(::getpid()) + "-" +
                      std::to_string(counter.fetch_add(1, std::memory_order_relaxed));

    auto cgroup_created = instance->cgroup.create(delegated_cgroup_root_, id, spec.limits);
    if (!cgroup_created.has_value()) return std::unexpected(cgroup_created.error());

    insert_instance_locked(id, std::move(instance));
    return SandboxHandle{id};
}

result<ExecOutcome> LinuxNativeJailBackend::exec(SandboxHandle& handle, ExecRequest const& request,
                                                  EffectContext&) {
    Instance* inst_ptr = find_instance_locked(handle.opaque_id);
    if (inst_ptr == nullptr) {
        return std::unexpected(ae::error{
            failure_class::contract,
            "exec() called on an unknown or already-destroyed SandboxHandle",
            "linux_native_jail.unknown_handle",
        });
    }
    Instance& inst = *inst_ptr;

    int sync_fds[2];
    if (::pipe2(sync_fds, O_CLOEXEC) != 0) {
        return errno_error("pipe2(sync)", failure_class::fatal, "linux_native_jail.pipe_failed");
    }
    FdGuard sync_read{sync_fds[0]}, sync_write{sync_fds[1]};

    int stdout_fds[2], stderr_fds[2];
    if (::pipe2(stdout_fds, O_CLOEXEC) != 0 || ::pipe2(stderr_fds, O_CLOEXEC) != 0) {
        return errno_error("pipe2(stdio)", failure_class::fatal, "linux_native_jail.pipe_failed");
    }
    FdGuard stdout_read{stdout_fds[0]}, stdout_write{stdout_fds[1]};
    FdGuard stderr_read{stderr_fds[0]}, stderr_write{stderr_fds[1]};
    // Best-effort: enlarge the kernel pipe buffer past the Linux default (typically 64 KiB) so a
    // hostile child that writes far more than output_bytes still has somewhere to land before
    // blocking in write() -- exec()'s wait loop drains AFTER the child exits/is killed, not
    // concurrently, so a too-small buffer would make output_bytes containment look artificially
    // tight regardless of the configured cap (mirrors the Windows side's explicit CreatePipe
    // buffer size). Failure here (e.g. exceeding /proc/sys/fs/pipe-max-size without
    // CAP_SYS_RESOURCE) just leaves the OS default in place -- not fatal to containment, since
    // drain_pipe_bounded enforces the real cap regardless of how much the kernel could buffer.
    ::fcntl(stdout_write.fd, F_SETPIPE_SZ, 1024 * 1024);
    ::fcntl(stderr_write.fd, F_SETPIPE_SZ, 1024 * 1024);

    // The child's copies of stdout_write/stderr_write must be inheritable across clone() despite
    // O_CLOEXEC (which would otherwise close them across the child's own later execve) -- dup2 in
    // child_entry targets STDOUT_FILENO/STDERR_FILENO, which are never O_CLOEXEC, so the *targets*
    // survive execve even though the original fds (closed right after dup2) do not need to.
    //
    // jail_root is a fresh, unique-per-exec()-call directory: a mount namespace is per-PROCESS, not
    // persisted on Instance, so every exec() on the same SandboxHandle rebuilds its own jail from
    // scratch. Created HOST-SIDE (here, before clone()) rather than by the child, so its lifetime
    // brackets the child's the same way the cgroup directory's does -- and so it can be rmdir'd
    // after the child is reaped even if the child died before finishing its own jail setup.
    if (!mkdir_p(jail_root_base_)) {
        return errno_error(("mkdir_p(" + jail_root_base_ + ")").c_str(), failure_class::fatal,
                            "linux_native_jail.jail_root_base_mkdir_failed");
    }
    std::string jail_root =
        jail_root_base_ + "/" + handle.opaque_id + "-" + std::to_string(inst.exec_seq++);
    if (::mkdir(jail_root.c_str(), 0700) != 0 && errno != EEXIST) {
        return errno_error(("mkdir(" + jail_root + ")").c_str(), failure_class::fatal,
                            "linux_native_jail.jail_root_mkdir_failed");
    }

    ChildArgs args;
    args.sync_read_fd = sync_read.fd;
    args.sync_write_fd = sync_write.fd;
    args.stdout_write_fd = stdout_write.fd;
    args.stderr_write_fd = stderr_write.fd;
    args.jail_root = jail_root;
    args.cwd_guest = inst.cwd_guest_path;
    args.command = request.source;
    for (MountSpec const& m : inst.mounts) {
        args.mounts.push_back(ChildMount{
            .host_path = std::get<std::string>(m.source),
            .guest_path = m.guest_path,
            .read_write = m.read_write,
        });
    }

    constexpr std::size_t kStackSize = 1024 * 1024;
    void* stack_mem = ::mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack_mem == MAP_FAILED) {
        return errno_error("mmap(child stack)", failure_class::fatal,
                            "linux_native_jail.stack_alloc_failed");
    }
    void* stack_top = static_cast<std::byte*>(stack_mem) + kStackSize;

    int clone_flags = CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD;
    pid_t child_pid = ::clone(child_entry, stack_top, clone_flags, &args);
    if (child_pid < 0) {
        ::munmap(stack_mem, kStackSize);
        ::rmdir(jail_root.c_str());
        return errno_error("clone", failure_class::fatal, "linux_native_jail.clone_failed");
    }

    // Parent-only from here: close our copies of the child's ends (mirrors the Windows side
    // closing its own copy of the pipe write ends after CreateProcessW).
    sync_read.close_now();
    stdout_write.close_now();
    stderr_write.close_now();

    auto added = inst.cgroup.add_process(static_cast<int>(child_pid));
    if (!added.has_value()) {
        // Containment isn't in place -- fail closed: kill the child before releasing it (it is
        // still blocked on its own sync-pipe read, so nothing it doesn't yet control has run).
        ::kill(child_pid, SIGKILL);
        int status = 0;
        ::waitpid(child_pid, &status, 0);
        sync_write.close_now();
        ::munmap(stack_mem, kStackSize);
        ::rmdir(jail_root.c_str());
        return std::unexpected(added.error());
    }
    sync_write.close_now();  // releases the child's blocking read()

    // Single poll loop: child-exited / wall_ms / cpu_ms, mirroring job_object_limits.hpp's
    // wait_or_kill shape but as a poll (cgroups v2 has no waitable "budget exceeded" handle the way
    // a Windows Job Object's own wait does).
    auto const t0 = std::chrono::steady_clock::now();
    bool killed_for_timeout = false;
    int status = 0;
    for (;;) {
        pid_t reaped = ::waitpid(child_pid, &status, WNOHANG);
        if (reaped == child_pid) break;

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);
        bool wall_exceeded = inst.limits.wall_ms > 0 &&
                              static_cast<std::uint64_t>(elapsed.count()) >= inst.limits.wall_ms;
        bool cpu_exceeded = false;
        if (inst.limits.cpu_ms > 0) {
            auto usage = inst.cgroup.query_usage();
            if (usage.has_value()) {
                cpu_exceeded = (usage->cpu_usage_usec / 1000) >= inst.limits.cpu_ms;
            }
        }
        if (wall_exceeded || cpu_exceeded) {
            ::kill(child_pid, SIGKILL);
            ::waitpid(child_pid, &status, 0);
            killed_for_timeout = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ExecOutcome outcome;
    outcome.stdout_text = drain_pipe_bounded(stdout_read.fd, inst.limits.output_bytes);
    outcome.stderr_text = drain_pipe_bounded(stderr_read.fd, inst.limits.output_bytes);
    ::munmap(stack_mem, kStackSize);
    // The child (and with it, its private mount namespace and every mount/pivot_root this exec()
    // call made) is fully reaped by this point (the poll loop above only exits after a successful
    // waitpid) -- MS_PRIVATE (setup_jail's unconditional first step) means none of that ever
    // propagated into the HOST's own mount table, so this directory is already just an ordinary,
    // empty host directory again; rmdir is real cleanup, not racing a lazy unmount.
    ::rmdir(jail_root.c_str());

    if (killed_for_timeout) {
        outcome.klass = exec_outcome_class::timeout;
    } else {
        // cgroups v2 memory.max enforcement is NOT uniformly a hard SIGKILL -- measured directly
        // in this harness, running the IDENTICAL workload (a 512 MB request against a 32 MB cap)
        // repeatedly produced two different real outcomes: sometimes a page fault on
        // already-mapped memory can't be satisfied within the cap, forcing the kernel's OOM killer
        // to SIGKILL (a hard, page-fault-time enforcement -- page faults cannot fail back to
        // userspace the way a syscall can); other times the allocator's own mmap() call is the one
        // that crosses the cap and simply fails with ENOMEM, which C++ surfaces as an unhandled
        // std::bad_alloc -> std::terminate() -> SIGABRT, a clean process-side death, not a kernel
        // kill. A THIRD wrinkle, also measured directly: the guest is PID 1 of its own CLONE_NEWPID
        // namespace, and the kernel reports a namespace-init process's signal death to ITS parent
        // (this backend, in the outer namespace) as an ORDINARY exit with code 128+signal --
        // WIFEXITED/WEXITSTATUS==137, NOT WIFSIGNALED/WTERMSIG==SIGKILL -- a documented Linux
        // behavior specific to namespace-init processes, not a bug in this backend's wait loop.
        // Four signals, checked together, none alone sufficient:
        //   1. memory.events' `oom_kill` counter, read via query_usage() -- can occasionally read
        //      as still-stale immediately after waitpid() returns (observed directly), so it is
        //      corroborating evidence, not the sole determinant.
        //   2. SIGKILL reported directly (WIFSIGNALED): nothing else in this pipeline sends
        //      SIGKILL to the guest outside the killed_for_timeout branch already excluded above.
        //   3. SIGKILL reported via the namespace-init 128+signal exit-code convention (WIFEXITED,
        //      WEXITSTATUS==137) -- the form THIS backend's children actually take, per the above.
        //   4. A peak-usage-vs-cap heuristic (same shape ADR-004's Windows side needed) for the
        //      clean-ENOMEM/SIGABRT case: a non-clean-exit death whose peak memory usage reached
        //      the configured cap is memory-related even without any SIGKILL signal at all.
        auto usage = inst.cgroup.query_usage();
        bool oom_kill_counter = usage.has_value() && usage->oom_kill_count > 0;
        bool sigkill_reported_directly =
            WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL && inst.limits.memory_bytes > 0;
        bool sigkill_reported_via_namespace_init_exit_code =
            WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGKILL && inst.limits.memory_bytes > 0;
        bool non_clean_death = WIFSIGNALED(status) || sigkill_reported_via_namespace_init_exit_code;
        bool peak_near_cap_and_abnormal =
            non_clean_death && usage.has_value() && inst.limits.memory_bytes > 0 &&
            usage->peak_memory_bytes >= (inst.limits.memory_bytes * 9) / 10;
        if (oom_kill_counter || sigkill_reported_directly || sigkill_reported_via_namespace_init_exit_code ||
            peak_near_cap_and_abnormal) {
            outcome.klass = exec_outcome_class::oom;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            outcome.klass = exec_outcome_class::ok;
        } else {
            outcome.klass = exec_outcome_class::crash;
        }
    }
    return outcome;
}

void LinuxNativeJailBackend::destroy(SandboxHandle& handle) {
    // exec() always fully reaps the child (via the wait loop above) before returning, so the
    // cgroup is already empty of live processes by the time destroy() runs -- CgroupLimits'
    // destructor (via erase below) removes the directory, 008 §2 clause 4's teardown for whatever
    // this SandboxHandle was holding. The delegated root itself is deployment-scoped, not
    // session-scoped, and is never removed here (mirrors the Windows side not deleting its shared
    // AppContainer profile on a per-session destroy()).
    erase_instance_locked(handle.opaque_id);
}

LinuxNativeJailBackend::Instance* LinuxNativeJailBackend::find_instance_locked(std::string const& id) {
    std::lock_guard<std::mutex> guard(instances_mutex_);
    auto it = instances_.find(id);
    return it == instances_.end() ? nullptr : it->second.get();
}

void LinuxNativeJailBackend::insert_instance_locked(std::string id, std::unique_ptr<Instance> instance) {
    std::lock_guard<std::mutex> guard(instances_mutex_);
    instances_.emplace(std::move(id), std::move(instance));
}

void LinuxNativeJailBackend::erase_instance_locked(std::string const& id) {
    std::lock_guard<std::mutex> guard(instances_mutex_);
    instances_.erase(id);
}

}  // namespace agentengine::native_jail
