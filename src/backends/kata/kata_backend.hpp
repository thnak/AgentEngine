#pragma once
// Implements agentengine::SandboxBackend (sandbox/sandbox.hpp) for a first-party, Linux-only,
// hardware-isolation backend wrapping Kata Containers -- docs/planning/microvm-first-party-backend-
// design-draft.md Design A, narrowed per §7 C2's steelmanned scoping (NOT Revision 1's original
// two-runtime-class proposal): exactly ONE Kata runtime class (`kata-clh`, cloud-hypervisor --
// chosen specifically over `kata-qemu` to avoid QEMU's own CVE surface, per that same finding), one
// exact-pinned release (4.1.0), vendored the way `decisions/ADR-013-...mbedtls-vendoring.md` pins
// mbedTLS. Reopen condition (i) (`native-jail`'s own 008 §9 gate reaching Judged on both platforms)
// was independently verified met on 2026-08-23 before this file was written -- see
// `decisions/ADR-004-appcontainer-native-jail-windows-backend.md` and
// `decisions/ADR-083-linux-native-jail-pivot-root-containment.md`.
//
// SLICE 1 -- deliberately narrow, mirrors ADR-081's own "Slice 1" precedent for the jailed Python
// worker. What this proves: a real, working create()/exec()/destroy() round trip against a REAL
// Kata sandbox (cloud-hypervisor VMM, a distinct guest kernel measured different from the host's
// own, real state persistence across exec() calls into the same instance) -- not a mock, not a
// design sketch. What this does NOT yet do, named honestly rather than silently assumed:
//
//   - `SandboxSpec::capabilities`/`mounts`/`limits`/`net` are NOT inspected or enforced by this
//     backend at all this pass. Whatever a caller places in a `SandboxSpec` is currently a no-op --
//     neither granted nor denied by anything THIS code does. What containment exists today comes
//     entirely from Kata's own defaults: no CNI plugin configured means the guest VM has no route
//     out (an absence-based default-deny, not a policy this backend enforces or could relax), and
//     each container gets an ephemeral rootfs from the pulled OCI image with no host directory
//     bind-mounted in (so there is no MountSpec-shaped grant to honor OR violate yet). This is a
//     REAL GAP, not a design choice -- 008 §2's full contract (capability-scoped mounts, enforced
//     resource limits, an explicit NetPolicy allowlist) is Slice 2+ work. Do not treat this backend
//     as meeting 008 §2 in full; it is reachable only via `register_hardware_isolation_backend()`
//     (`named_only`, ADR-080/microvm-first-party-backend-design-draft.md finding #1) specifically so
//     a host must opt a session into it BY NAME, with these residuals known, never by `Strict`
//     falling through to it.
//   - `ExecRequest::source` is, like `LinuxNativeJailBackend`'s own M2-only scope
//     (linux_native_jail_backend.hpp), treated as a shell command line (`/bin/sh -c <source>`) the
//     caller is trusted to have already resolved from a name -- not yet Runner-mediated.
//   - GPU passthrough (Kata's headline capability) is explicitly OUT of scope -- the design draft's
//     own C3 finding named this as needing a real `capability_kind` this codebase does not have yet
//     and deliberately declined to add without a real consumer (ADR-004 §11's downstream-consumer
//     note; this file is that consumer now existing, but still doesn't wire GPU passthrough itself).
//   - No abuse-corpus/G1-G8 promotion-gate evidence exists for this backend yet -- that is real,
//     separate work this Slice does not claim.
//
// Mechanism: shells out to the containerd CLI (`ctr`), not an embedded gRPC/ttrpc client -- keeps
// this backend's own dependency footprint to "a `ctr`/containerd/kata-runtime install already on
// PATH," matching CONVENTIONS.md's "one dependency per backend" posture and avoiding vendoring a
// second RPC stack into this C++ tree for what host-side `ctr`, already needed to operate the Kata
// deployment at all, already does. `create()` launches `ctr run -d --runtime
// io.containerd.kata-clh.v2 <image> <id> sleep infinity` (a persistent, otherwise-idle task);
// `exec()` runs `ctr tasks exec --exec-id <uuid> <id> /bin/sh -c <source>` against it (proven, this
// same pass, to see state written by a PRIOR exec() call on the same instance -- the identical
// "REGRESSION test" concern `test_sandbox_backend_registry.cpp` item 1 exists for, verified here by
// a real VM, not a closure-over-a-fresh-instance bug); `destroy()` kills and removes the task/
// container. Every subprocess is spawned with `posix_spawn_file_actions_t`-style explicit fd
// wiring (stdin/stdout/stderr pipes only, `FD_CLOEXEC` on everything else via `pipe2(..., O_CLOEXEC)`)
// -- deliberately applying the lesson `decisions/ADR-004-...md` §12 Finding 6 found the hard way on
// Windows (`CreateProcessW` with no handle list inheriting every open host handle into a
// zero-capability child) to this backend's own host-side subprocess spawns, proactively rather than
// after an incident.
//
// Deployment precondition (mirrors `LinuxNativeJailBackend`'s own "cgroup delegation" and
// `NativeJailBackend`'s own "vendored interpreter must already exist" preconditions, stated rather
// than silently assumed): a working `containerd` daemon must already be running, with a
// `kata-clh`-named CRI runtime class OR the `io.containerd.kata-clh.v2` runtime type directly
// resolvable via `ctr run --runtime`, and `ctr` itself on `PATH`. This backend does NOT install,
// configure, or start any of that -- a one-time host/deployment bootstrap step, same posture as the
// cgroup-delegation precondition on the native-jail Linux side.

#include <string>
#include <unordered_map>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/sandbox.hpp"

namespace agentengine::kata {

class KataBackend {
public:
    static constexpr ProfileTraits traits{
        /*strength=*/90,  // stronger than native-jail's namespace/cgroup boundary (50): a real KVM
                           // hardware VM boundary, not process-level isolation. Irrelevant to
                           // `resolve_strict()` in practice -- this backend is registered
                           // `named_only` (see this file's own header comment above) and never
                           // competes for `Strict` resolution regardless of this number.
        /*platform_mask=*/static_cast<std::uint8_t>(platform_id::linux_x86_64),
        cold_start_class::milliseconds,  // local VM boot via cloud-hypervisor, no network round
                                          // trip -- not `network_dependent` (that class is for the
                                          // `remote` profile specifically, 008 §3).
    };

    // `runtime_type`: the containerd runtime type string (`io.containerd.kata-clh.v2` by default --
    // see this file's header comment on why cloud-hypervisor, not QEMU). `image`: the OCI image
    // reference used as every container's rootfs -- must already be pulled into containerd's default
    // namespace, or `ctr run` will pull it itself the first time (network-dependent, not this
    // backend's concern to police).
    explicit KataBackend(std::string runtime_type = "io.containerd.kata-clh.v2",
                          std::string image = "docker.io/library/busybox:latest")
        : runtime_type_(std::move(runtime_type)), image_(std::move(image)) {}
    ~KataBackend() = default;
    KataBackend(KataBackend const&) = delete;
    KataBackend& operator=(KataBackend const&) = delete;
    KataBackend(KataBackend&&) = delete;
    KataBackend& operator=(KataBackend&&) = delete;

    result<SandboxHandle> create(SandboxSpec const& spec, EffectContext& ctx);
    result<ExecOutcome> exec(SandboxHandle& handle, ExecRequest const& request, EffectContext& ctx);
    void destroy(SandboxHandle& handle);

private:
    struct Instance {
        std::string container_id;
        std::uint64_t exec_seq = 0;  // -> a fresh --exec-id per exec() call (containerd requires a
                                      // unique exec-id per additional process in one task).
    };

    std::string runtime_type_;
    std::string image_;
    std::unordered_map<std::string, Instance> instances_;
};

static_assert(SandboxBackend<KataBackend>,
              "KataBackend must satisfy the SandboxBackend concept (008 §2)");

}  // namespace agentengine::kata
