#pragma once
// Implements agentengine::SandboxBackend (sandbox/sandbox.hpp) for the `native-jail` profile,
// Linux half (008-Sandbox-and-Isolation.md §1b, §2, §3) -- Milestone 2 Phase C task C2's own
// follow-up (021 §2 sequencing: Windows first, Linux once Windows reached a stable state, which
// C2's Windows half now has). Namespaces (PID/net/mount/UTS/IPC via `clone()`, atomically -- not
// `unshare()` from the host process, which would move the CALLING thread's own namespaces and
// leak across this backend's other calls) + cgroups v2 (`cgroup_limits.hpp`, already built) +
// seccomp-BPF (`seccomp_filter.hpp`, already built) + `no_new_privs`.
//
// Scope parity with `NativeJailBackend` (the Windows half, native_jail_backend.hpp) -- read that
// header's own scope note first, most of it applies here unchanged:
//   - `ExecRequest::source` is, for M2 only, a shell command line (`/bin/sh -c <source>`) the
//     CALLER is trusted to have already resolved from a name -- the Linux analogue of the Windows
//     side treating `source` as a Win32 command line. Same "not yet Runner-mediated" scope.
//   - `MountSpec::source` as a `BlobRef` fails closed (unsupported, same gap as Windows).
//   - Full filesystem isolation (pivot_root / bind-mount jail) is NOT attempted here, matching the
//     Windows side's ACL-grants-specific-paths-only scope: `CLONE_NEWNS` gives the guest a
//     PRIVATE COPY of the mount table (its own mount/umount calls never affect the host) but does
//     NOT by itself restrict which existing host paths remain visible inside it. Real filesystem
//     containment is 010's interpreter-level `open()` mediation (§1b layers 1-2), not built until
//     M3 on either platform -- this backend alone does not yet close the filesystem boundary any
//     more completely on Linux than the Windows half does.
//
// What this backend adds that the Windows half structurally cannot: cgroups v2's `memory.max`
// triggers the kernel's own cgroup-scoped OOM killer, countable via `memory.events`' `oom_kill`
// field (`cgroup_limits.hpp`) -- a real, positive OOM signal, not the peak-usage-vs-cap heuristic
// the Windows side needs because Job Objects expose no completion-port-free equivalent.
//
// Deployment precondition, stated rather than silently assumed (mirrors the Windows side's
// "vendored interpreter must already exist" precondition): the delegated cgroup v2 root
// (`/sys/fs/cgroup/agentengine` by default) must already have `memory`/`pids` enabled in its OWN
// ancestor's `cgroup.subtree_control` -- a one-time host/deployment bootstrap step this backend
// does NOT perform itself (moving arbitrary host processes between cgroups as a side effect of a
// per-sandbox `create()` call would be a much larger, host-wide action than a sandboxing library
// should take silently). `create()` fails closed with a specific error if this precondition was
// not met.
//
// Requires CAP_SYS_ADMIN (or an equivalent already-namespaced environment, e.g. a container's own
// user namespace) to create PID/net/mount/UTS/IPC namespaces via `clone()` -- a deployment
// requirement, same posture as the cgroup delegation precondition above.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "backends/native_jail/cgroup_limits.hpp"

namespace agentengine::native_jail {

class LinuxNativeJailBackend {
public:
    static constexpr ProfileTraits traits{
        /*strength=*/50,
        /*platform_mask=*/static_cast<std::uint8_t>(platform_id::linux_x86_64),
        cold_start_class::milliseconds,
    };

    explicit LinuxNativeJailBackend(std::string delegated_cgroup_root = "/sys/fs/cgroup/agentengine")
        : delegated_cgroup_root_(std::move(delegated_cgroup_root)) {}
    ~LinuxNativeJailBackend() = default;
    LinuxNativeJailBackend(LinuxNativeJailBackend const&) = delete;
    LinuxNativeJailBackend& operator=(LinuxNativeJailBackend const&) = delete;
    LinuxNativeJailBackend(LinuxNativeJailBackend&&) = delete;
    LinuxNativeJailBackend& operator=(LinuxNativeJailBackend&&) = delete;

    result<SandboxHandle> create(SandboxSpec const& spec, EffectContext& ctx);
    result<ExecOutcome> exec(SandboxHandle& handle, ExecRequest const& request, EffectContext& ctx);
    void destroy(SandboxHandle& handle);

private:
    struct Instance {
        CgroupLimits cgroup;
        ResourceLimits limits;
        std::string cwd;  // first read-write mount's host path, if any; empty = inherit
    };

    std::string delegated_cgroup_root_;
    std::unordered_map<std::string, std::unique_ptr<Instance>> instances_;
};

static_assert(SandboxBackend<LinuxNativeJailBackend>,
              "LinuxNativeJailBackend must satisfy the SandboxBackend concept (008 §2)");

}  // namespace agentengine::native_jail
