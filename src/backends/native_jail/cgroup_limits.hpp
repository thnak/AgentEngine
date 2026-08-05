#pragma once
// Implements 008-Sandbox-and-Isolation.md §2 (ResourceLimits enforcement) via Linux cgroups v2 --
// the Linux half of Milestone 2 Phase C task C2 (docs/planning/milestone-2-tools-capabilities-
// sandbox-breakdown.md), the counterpart to job_object_limits.hpp on Windows.
//
// Scope: an RAII wrapper around ONE delegated cgroup v2 subtree enforcing
// ResourceLimits::{memory_bytes, pids} via memory.max/pids.max (the kernel's own controllers --
// unlike Windows' Job Object memory cap, which blocks further commits and lets the guest's own
// allocator fail, cgroups v2's memory.max invokes the kernel OOM killer scoped to this cgroup,
// giving a positive, countable signal via memory.events' `oom_kill` field -- see oom_kill_count()).
// `cpu_ms` and `wall_ms` are NOT cgroup-native limits here: cpu.max is a bandwidth throttle (how
// fast), not a cumulative budget (how much) -- ResourceLimits::cpu_ms means the latter, so it is
// enforced the same way as `wall_ms`, by polling (this class exposes the raw counters; the
// poll/kill loop itself lives in linux_native_jail_backend.cpp, which owns the single wait loop
// checking child-exited / wall_ms / cpu_ms together, mirroring job_object_limits.hpp's
// wait_or_kill shape but as a poll loop rather than one blocking wait, since cgroups v2 has no
// waitable "job time exceeded" handle the way a Windows Job Object does).
//
// disk_bytes, net_bytes, output_bytes, fds are explicitly NOT enforced here, same posture as
// job_object_limits.hpp -- 008 §1b assigns those to interpreter-level mediation (not yet built,
// M3) or, for output_bytes, to the bounded pipe-drain the backend itself performs (mirroring the
// Windows side).
//
// Requires a delegated cgroup v2 hierarchy the calling process can create subdirectories under
// (typically /sys/fs/cgroup/agentengine/, itself created once at host startup) -- CAP_SYS_ADMIN or
// the container/host already having been granted delegation. This is a deployment requirement,
// stated here rather than silently assumed: `create()` fails closed with a specific error if the
// delegated root does not exist or is not writable.

#include <cstdint>
#include <string>

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/sandbox.hpp"

namespace agentengine::native_jail {

struct CgroupUsage {
    std::uint64_t peak_memory_bytes = 0;   // memory.peak (or memory.current as a fallback)
    std::uint64_t cpu_usage_usec = 0;      // cpu.stat's usage_usec field
    std::uint64_t oom_kill_count = 0;      // memory.events' oom_kill field
};

class CgroupLimits {
public:
    CgroupLimits() = default;
    ~CgroupLimits();

    CgroupLimits(CgroupLimits const&) = delete;
    CgroupLimits& operator=(CgroupLimits const&) = delete;
    CgroupLimits(CgroupLimits&& other) noexcept;
    CgroupLimits& operator=(CgroupLimits&& other) noexcept;

    // Creates a fresh subdirectory under `delegated_root` (e.g. "/sys/fs/cgroup/agentengine"),
    // named `name` (caller-chosen, must be a single path segment -- a fresh id per sandbox
    // instance, matching the opaque_id NativeJailBackend already mints), and writes every nonzero
    // ResourceLimits field this class enforces. A zero field means "do not set this limit axis at
    // all" (matching job_object_limits.hpp's identical convention).
    [[nodiscard]] result<void> create(std::string const& delegated_root, std::string const& name,
                                       ResourceLimits const& limits);

    // Moves `pid` into this cgroup (writes to cgroup.procs). Must be called before -- or, for a
    // CLONE_NEWPID child, immediately after -- the process begins running untrusted code, same
    // "before the entry point runs" discipline as job_object_limits.hpp's assign_process.
    [[nodiscard]] result<void> add_process(int pid) const;

    [[nodiscard]] result<CgroupUsage> query_usage() const;

    // Full teardown: removes the cgroup directory. Requires the cgroup to already be empty
    // (cgroup.procs has no live members) -- the caller's job to have reaped/killed the child
    // first; this does not itself kill anything (008 §2 clause 4's teardown obligation is met by
    // the backend's wait/kill loop plus this rmdir, together, not by this method alone).
    void destroy();

private:
    void destroy_now();

    std::string path_;  // "" = not created / already destroyed
};

}  // namespace agentengine::native_jail
