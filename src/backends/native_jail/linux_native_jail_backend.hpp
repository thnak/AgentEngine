#pragma once
// Implements agentengine::SandboxBackend (sandbox/sandbox.hpp) for the `native-jail` profile,
// Linux half (008-Sandbox-and-Isolation.md §1b, §2, §3) -- Milestone 2 Phase C task C2's own
// follow-up (021 §2 sequencing: Windows first, Linux once Windows reached a stable state, which
// C2's Windows half now has). Namespaces (PID/net/mount/UTS/IPC via `clone()`, atomically -- not
// `unshare()` from the host process, which would move the CALLING thread's own namespaces and
// leak across this backend's other calls) + cgroups v2 (`cgroup_limits.hpp`, already built) +
// seccomp-BPF (`seccomp_filter.hpp`, already built) + `no_new_privs`.
//
// docs/planning/sandbox-spec-capability-enforcement-design-draft.md (2026-08-24, decisions/ADR-087-
// sandbox-spec-capability-enforcement.md): `create()` now calls `agentengine::authorize_spec()`
// (sandbox/sandbox.hpp) first -- a real, opt-in check that `spec.mounts`/`spec.net` are covered by
// `spec.capabilities`' `cap::SandboxMount`/`cap::SandboxNetOut` grants, a no-op for every caller that
// doesn't hold one. Same pass also closes a real per-backend divergence (that design's own red-team
// finding B3): `create()` now fails closed on any `NetPolicy` beyond `deny_all=true`, matching
// `KataBackend`'s existing identical posture (ADR-086) -- this backend has no CNI/egress-proxy of
// any kind, so a caller's spec previously meant two different things depending on which backend was
// selected.
//
// Scope parity with `NativeJailBackend` (the Windows half, native_jail_backend.hpp) -- read that
// header's own scope note first, most of it applies here unchanged:
//   - `ExecRequest::source` is, for M2 only, a shell command line (`/bin/sh -c <source>`) the
//     CALLER is trusted to have already resolved from a name -- the Linux analogue of the Windows
//     side treating `source` as a Win32 command line. Same "not yet Runner-mediated" scope.
//   - `MountSpec::source` as a `BlobRef` fails closed (unsupported, same gap as Windows).
//   - Filesystem and process-visibility containment (008 §9 G2/G3's Linux half): the child, after
//     `clone()` returns there and before `execve`, makes its mount namespace private
//     (`MS_REC|MS_PRIVATE` on `/`, unconditionally first -- otherwise every subsequent bind
//     mount/`pivot_root` would leak into the host's own mount table, since a systemd-managed
//     host's root mount is `MS_SHARED` by default), builds a fresh `tmpfs`-backed root under
//     `jail_root_base_`, bind-mounts EXACTLY the granted `MountSpec` set into it (read-only grants
//     get a required second `MS_REMOUNT|MS_RDONLY` call -- the initial `MS_BIND` call silently
//     ignores `MS_RDONLY` on its own), `pivot_root()`s into it (not `chroot`, which a process
//     holding a pre-existing fd to outside the new root can escape via `fchdir`), detaches the old
//     root, and mounts a fresh `procfs` (namespace-local by construction, since this runs already
//     inside the new `CLONE_NEWPID` namespace -- a bind-mounted `/proc` would keep showing the
//     host's real process list). No implicit `/bin`/`/lib`/`/usr` mount of any kind -- a guest
//     command needing a toolchain beyond its explicit grants needs its own explicit `MountSpec`,
//     same as the Windows side's vendored-interpreter-tree precedent (ADR-004 §3 step 1). Proven
//     against real Linux namespaces + cgroups v2 in `test_native_jail_fs_containment_linux.cpp` --
//     `decisions/ADR-083-linux-native-jail-pivot-root-containment.md` is the closing ADR; design
//     and self-red-team record in
//     `docs/planning/linux-native-jail-pivot-root-containment-design-draft.md`.
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
#include <vector>

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

    explicit LinuxNativeJailBackend(std::string delegated_cgroup_root = "/sys/fs/cgroup/agentengine",
                                     std::string jail_root_base = "/tmp/agentengine-native-jail")
        : delegated_cgroup_root_(std::move(delegated_cgroup_root)),
          jail_root_base_(std::move(jail_root_base)) {}
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
        std::vector<MountSpec> mounts;  // full grant set; host_path form only (BlobRef already
                                         // rejected in create()) -- exec() rebuilds the jail from
                                         // this on every call, since a mount namespace is
                                         // per-process, not persisted on the Instance.
        std::string cwd_guest_path;     // first read-write mount's GUEST path, if any; empty = "/"
        std::uint64_t exec_seq = 0;     // monotonic per-instance counter -> a fresh, unique
                                         // jail_root directory name per exec() call.
    };

    std::string delegated_cgroup_root_;
    std::string jail_root_base_;
    std::unordered_map<std::string, std::unique_ptr<Instance>> instances_;
};

static_assert(SandboxBackend<LinuxNativeJailBackend>,
              "LinuxNativeJailBackend must satisfy the SandboxBackend concept (008 §2)");

}  // namespace agentengine::native_jail
