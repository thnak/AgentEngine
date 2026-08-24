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
// SLICE 1 (2026-08-23) -- mirrors ADR-081's own "Slice 1" precedent for the jailed Python worker.
// Proved a real, working create()/exec()/destroy() round trip against a REAL Kata sandbox
// (cloud-hypervisor VMM, a distinct guest kernel measured different from the host's own, real
// state persistence across exec() calls into the same instance) -- not a mock, not a design
// sketch. Independently red-teamed (decisions/ADR-084-kata-backend-slice-1.md §4) -- two BLOCKING
// findings (unbounded host-side output capture, no subprocess timeout) fixed and re-verified there.
//
// SLICE 2 (2026-08-24) -- closes three of Slice 1's four named `SandboxSpec` gaps:
// `mounts`/`limits`/`net` are now inspected and enforced (`capabilities` stays deliberately out of
// scope this pass -- no `capability_kind` mapping exists for anything this backend could grant
// beyond what `mounts`/`net` already cover, same posture as GPU passthrough below). Per-axis:
//
//   - `MountSpec`: each grant translates to a real `ctr run --mount type=bind,src=<host>,
//     dst=<guest>,options=rbind:<ro|rw>` flag -- a real bind mount into the guest VM via virtiofs,
//     not a no-op. `MountSpec::source` as a `BlobRef` fails closed at `create()`
//     (`kata_backend.blob_mount_unsupported`), the identical posture and diagnostic-code shape
//     `LinuxNativeJailBackend::create()` already has for the same case (host paths only, M2 scope).
//     `MountSpec::quota_bytes` is NOT enforced -- named REAL GAP, no quota-aware mount mechanism
//     wired this pass (a real follow-on, not silently assumed done).
//   - `ResourceLimits`: `memory_bytes` -> `ctr run --memory-limit`; `wall_ms`, if nonzero, replaces
//     `run_ctr()`'s own fixed `kProcessTimeoutSeconds` default for every `ctr` call this instance's
//     `exec()`/`destroy()` makes (a REAL, host-side enforced deadline -- the exact class of
//     property ADR-004's own `wall_ms`-is-the-dependable-bound finding treats as trustworthy, unlike
//     `cpu_ms`). **Scope boundary, stated explicitly per Slice 2 red-team finding #3 rather than
//     left for a reader to infer**: `wall_ms` bounds `exec()`/`destroy()` calls on an
//     ALREADY-CREATED instance only -- `create()`'s own VM-boot `ctr run` call always uses the
//     fixed default, deliberately: applying a caller's possibly-sub-second `wall_ms` to boot time
//     itself (cold start is "milliseconds," not zero -- see `traits` below) would make `create()`
//     fail spuriously for realistic small values. `output_bytes`, if nonzero, replaces the Slice-1
//     red-team fix's fixed
//     `kOutputSafetyCapBytes` per-stream cap, mirroring `LinuxNativeJailBackend::
//     drain_pipe_bounded()`'s own "the safety cap, or `limits.output_bytes` if the caller set a
//     tighter one" precedent exactly. `cpu_ms`/`pids`/`fds`/`disk_bytes`/`net_bytes` are NOT
//     enforced -- named REAL GAPs: `cpu_ms` because a CFS quota (`ctr run --cpus`) is a RATE, not a
//     total-time budget, and silently reinterpreting one as the other would misrepresent what is
//     actually enforced (the same distinction ADR-004 draws for Windows' own `cpu_ms`); `pids` has
//     no direct `ctr run` CLI flag this pass wires (would need a full OCI `config.json` via `ctr run
//     --config` instead of the convenience flags used here -- a real, scoped-out follow-on);
//     `fds`/`disk_bytes`/`net_bytes` have no mechanism at all wired this pass.
//   - `NetPolicy`: `deny_all == true` (the only value Slice 1 silently accepted regardless) is now
//     an EXPLICITLY VERIFIED property, not merely an artifact of "no CNI is configured." A caller
//     requesting anything else (`deny_all == false`, or a nonempty `allowlist`) FAILS CLOSED at
//     `create()` (`kata_backend.net_allowlist_unsupported`) -- this backend has no CNI/egress-proxy
//     wired to honor a real allowlist yet, and silently granting no network while a caller believes
//     they requested some would be a correctness footgun even though it happens to be safe. Stricter
//     than `LinuxNativeJailBackend`'s own current posture (which silently ignores `NetPolicy`
//     entirely, a pre-existing gap this file does not fix there) -- deliberately, not by oversight.
//
// SLICE 3 (2026-08-24) -- closes the `SandboxSpec::capabilities` gap named below: `create()` now
// calls `agentengine::authorize_spec()` (sandbox/sandbox.hpp) first, before any of Slice 2's own
// mount/net logic. Generalized project-wide, not Kata-only, per project-owner direction (2026-08-24)
// -- see `docs/planning/sandbox-spec-capability-enforcement-design-draft.md` for the full design,
// its red-team pass, and why a Kata-only fix would itself have been wrong (per-backend divergence in
// what `SandboxSpec` fields mean). `LinuxNativeJailBackend`/`NativeJailBackend` (Windows) got the
// identical call in the same pass -- `decisions/ADR-087-sandbox-spec-capability-enforcement.md` is
// the closing ADR for all three. Two new capability kinds, `cap::SandboxMount`/`cap::SandboxNetOut`
// (`trust/capability.hpp`) -- deliberately NOT a reuse of `cap::FsRead`/`FsWrite`/`NetOut` (those are
// `mount_id`-keyed, Worktree/FileSystemAdapter-mediated; `SandboxSpec::MountSpec` names a raw host
// path directly, with no adapter indirection -- conflating the two would be a real capability-
// confusion hazard, ADR-071's precedent for why `NativeExec` is its own kind rather than overloading
// `Exec`). Opt-in, scoped to presence of the relevant grant kind (not `CapabilitySet::size()` --
// scoping to whole-set emptiness would fail open the moment any unrelated capability appeared on the
// same spec, a real finding from this design's own red-team pass): a caller that never grants
// `cap::SandboxMount`/`cap::SandboxNetOut` gets byte-for-byte the same behavior as before this Slice,
// unchanged.
//
// Still NOT done, unchanged from Slice 1/2, named honestly rather than silently assumed:
//
//   - `ExecRequest::source` is, like `LinuxNativeJailBackend`'s own M2-only scope
//     (linux_native_jail_backend.hpp), treated as a shell command line (`/bin/sh -c <source>`) the
//     caller is trusted to have already resolved from a name -- not yet Runner-mediated.
//   - GPU passthrough (Kata's headline capability) is explicitly OUT of scope -- the design draft's
//     own C3 finding named this as needing a real `capability_kind` this codebase does not have yet
//     and deliberately declined to add without a real consumer.
//   - No abuse-corpus/G1-G8 promotion-gate evidence exists for this backend yet -- that is real,
//     separate work this Slice does not claim.
//
// Reachable only via `register_hardware_isolation_backend()` (`named_only`,
// ADR-080/microvm-first-party-backend-design-draft.md finding #1) -- a host must opt a session into
// it BY NAME, with all of the above residuals known, never by `Strict` falling through to it.
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
        std::uint64_t exec_seq = 0;   // -> a fresh --exec-id per exec() call (containerd requires a
                                       // unique exec-id per additional process in one task).
        std::uint64_t wall_ms = 0;    // Slice 2: 0 = use run_ctr()'s own kProcessTimeoutSeconds
                                       // default; nonzero overrides it for every ctr call this
                                       // instance's exec()/destroy() makes (SandboxSpec::limits).
        std::uint64_t output_cap_bytes = 0;  // Slice 2: 0 = use kOutputSafetyCapBytes; nonzero
                                              // overrides it (SandboxSpec::limits.output_bytes).
    };

    std::string runtime_type_;
    std::string image_;
    std::unordered_map<std::string, Instance> instances_;
};

static_assert(SandboxBackend<KataBackend>,
              "KataBackend must satisfy the SandboxBackend concept (008 §2)");

}  // namespace agentengine::kata
