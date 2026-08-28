#pragma once
// PROVE-PHASE PROBE, ad hoc (2026-08-28, project-owner conversation, not yet its own ADR): answers
// the "should ExecutionSurface (this directory's execution_surface.hpp) just get folded INTO the
// real, production, Judged-track `agentengine::SandboxBackend` concept?" question raised while
// discussing ADR-096/098/099/100. Chosen answer, per that conversation: REFINE, don't widen --
// `SandboxBackend` (008 §2a) stays exactly as it is (a locked concept other Judged/shipped work
// already depends on: NativeJailBackend, WasmBackend, KataBackend), and a SEPARATE, additive concept
// requires it PLUS two more verbs, the same "additive, non-concept methods, never touching the base
// contract" discipline `NativeJailBackend::create_python_worker()`/`exec_session()` already
// establish as this codebase's own precedent for "a backend does more than the baseline concept"
// (ADR-100 F4 names this explicitly: "a deliberate boundary, not a gap to widen").
//
// DEPARTURE FROM THIS WHOLE PROVE-PHASE TREE'S OWN STANDING CONVENTION, disclosed rather than
// silent: every other file under docs/planning/proofs/ is deliberately NEVER linked against
// `include/agentengine/` (ADR-099's own §0 "no-reuse-at-design-time" framing). This file breaks
// that on purpose -- its entire point is to compile against the REAL `agentengine::SandboxBackend`
// concept (sandbox/sandbox.hpp) and the REAL `EffectContext`/`SandboxSpec`/`SandboxHandle`/
// `ExecRequest`/`ExecOutcome` types, not a parallel probe-only vocabulary, because "does this refine
// cleanly against the ACTUAL locked concept" is exactly the question being answered. Still not wired
// into any real backend registry, host, or CMake target -- a standalone compile-and-run probe only,
// same evidentiary bar as every sibling file here, not a production change.
//
// SCOPE, stated honestly: demonstrates the refinement is buildable for ONE conformer
// (`probe::DockerBackend`, already real and already fixed for shell-injection, docker_sandbox/
// docker_backend.hpp) using ONE isolation technology (Docker). Does not attempt a second conformer,
// and does not decide whether a real production Docker/containerd `SandboxBackend` should exist at
// all -- that remains exactly as undecided as before this file, per this whole design track's
// "reuse/production-wiring is an implementation-time decision" convention (ADR-099 §7).
//
// UPDATE (same day): `probe_docker_sandbox_backend_registry.cpp` (this directory) DOES now connect
// this type to the real `SandboxBackendRegistry` and runs it live. That probe's own banner records
// the real finding it surfaces: `RegisteredSandboxBackend`'s type erasure carries only create/exec/
// destroy, never this file's own reset()/drain_to() refinement -- the same structural shape ADR-100
// F4 already names for `NativeJailBackend::create_python_worker()`/`exec_session()`.
//
// UPDATE (same day, three independent adversarial red-team passes -- security/I2-I3, C++
// correctness, architecture-fit): all three found REAL defects in the first version of this file,
// summarized here rather than left implicit; fixes are described inline at each affected site:
//   - FATAL (security): `reset()`/`drain_to()` originally took no `EffectContext`/capability input
//     at all, making host<->container data movement reachable with NO authorization check --
//     structurally impossible to close without changing the signature, since `EffectContext` is the
//     only vehicle this codebase has for passing a `CapabilitySet`. FIXED: both verbs now take
//     `EffectContext&` and require a real `cap::SandboxMount` grant covering the requested host path
//     (`authorize_tree_path()` below), reusing the same `capability_detail::path_prefix_covers()`
//     logic `authorize_spec()` itself uses for `SandboxSpec::mounts`.
//   - MUST-FIX (security): no `SandboxHandle` ownership tracking -- any caller-supplied
//     `opaque_id` string was accepted by `exec()`/`destroy()`/`reset()`/`drain_to()`, letting a
//     caller on a shared Docker daemon hijack or destroy a container this backend never created.
//     FIXED: `live_containers_` (mutex-guarded) tracks IDs this instance actually created via
//     `create()`; every other verb fails closed (`docker_sandbox_backend.unknown_handle`) on an
//     untracked ID. This also makes the type genuinely stateful for the first time, so it is now
//     explicitly non-copyable/non-movable (see the class's own comment) -- the earlier, fully
//     stateless version's "safe to default-copy" property, confirmed correct by the C++-correctness
//     red-team pass, no longer applies and is not claimed.
//   - MUST-FIX (security): `SandboxSpec::net` was authorized on paper (`authorize_spec()` runs) but
//     never enforced against the real container -- `docker run` carried no `--network` flag, so
//     every container got full outbound internet access regardless of `deny_all`/grants. FIXED:
//     `create()` now (a) fails closed on any `NetPolicy` beyond `deny_all=true` with an empty
//     allowlist, matching `NativeJailBackend`'s own identical posture (ADR-098 §5 B3, ADR-086), and
//     (b) actually runs the container with `--network none` -- confirmed for real, live, this
//     session: a `ping` from inside a `--network none` container reports "Network unreachable," not
//     merely "no route found by a mocked check."
//   - SHOULD-FIX (security): `SandboxSpec::limits` was never applied to the real container (an I8
//     gap). PARTIALLY FIXED: `memory_bytes` and `pids` now become real `--memory`/`--pids-limit`
//     flags (confirmed for real: a limited container's own `/sys/fs/cgroup/memory.max` reflects the
//     requested byte count). `cpu_ms`/`wall_ms`/`disk_bytes`/`net_bytes`/`output_bytes`/`fds` remain
//     UNENFORCED by this probe -- disclosed, not silently claimed covered, the same
//     "confirmed-reliable vs. best-effort-only" split `NativeJailBackend`'s own header comment
//     already draws for its own `memory_bytes`/`pids` vs. `cpu_ms`.
//   - SHOULD-FIX (security + architecture-fit): the original `strength=0` comment claimed this kept
//     the conformer "out of any real resolve_strict() contest," but the registry probe's own test
//     showed the OPPOSITE -- as the sole strict-eligible candidate, it trivially won regardless of
//     strength. FIXED structurally, not just by disclosure: `probe_docker_sandbox_backend_registry.cpp`
//     now registers this backend via `register_hardware_isolation_backend()` (`named_only`), the
//     same entry point `KataBackend` uses for exactly this reason (`SandboxBackendRegistry`'s own
//     header comment: closes the blast-radius gap structurally, not by convention) -- it can no
//     longer win `Strict` resolution regardless of what else is or isn't registered alongside it.
//   - MUST-FIX (architecture-fit): the original version validated its "handle-scoped, copy-based
//     verbs are the concrete answer" claim against exactly ONE conformer (Docker) while never
//     mentioning `containerd_execution_surface.hpp` -- a second, already-proven `ExecutionSurface`
//     conformer in this SAME directory whose own header banner states plainly that copy-based
//     reset/drain_to "disappear" for a live-bind-mount-capable technology. Silently validating a
//     generalization claim against only the conformer that supports it, while a counter-example
//     already existed two files over, is exactly the "not empirically demonstrated to fit an
//     isolation technology of a different shape" gap `sandbox_runtime.hpp`'s own header comment
//     already names as a live, disclosed limitation for `ExecutionSurface` itself -- this file
//     failed to hold itself to the same bar. CORRECTED, not fixed by new code: the claim at
//     `TreeCapableSandboxBackend`'s own definition below is now stated as "validated for exactly one,
//     copy-based conformer," not "the concrete answer."
//   - SHOULD-FIX (architecture-fit): this refinement was a THIRD, uncoordinated tree-materialization
//     vocabulary in this same directory (`SandboxBackend`'s own create/exec/destroy has none;
//     `ExecutionSurface`'s object-scoped `reset/run/drain_to`, Ledger-integrated via
//     `SandboxRuntime`; and this file's handle-scoped `TreeCapableSandboxBackend`), never reconciled
//     with the other two. RECONCILED (same day, next round of the same project-owner conversation):
//     `tree_backend_execution_surface_adapter.hpp` -- a generic `TreeBackendExecutionSurface<Backend>`
//     adapter that drives ANY `TreeCapableSandboxBackend` conformer through the real `ExecutionSurface`
//     concept by lazily minting/re-minting one `SandboxHandle` internally. Proven live, end to end,
//     through the REAL `SandboxRuntime::run()`/`Ledger` machinery (`probe_tree_backend_execution_
//     surface_adapter.cpp`, mirroring `probe_execution_surface.cpp`'s own two-turn persistence-proving
//     shape exactly, 24/24 checks against real Docker as of this file's latest revision -- see that
//     probe's own history for what grew from 20) -- not merely asserted compatible. Does NOT
//     make `DockerSandboxBackend` and `DockerExecutionSurface` byte-for-byte interchangeable: the
//     adapter trades away real exit-code fidelity (`agentengine::ExecOutcome` has no exit-code field
//     to carry what `probe::ExecOutcome`/`ExecutionSurface::run()` requires), confirmed live rather
//     than merely asserted (`probe_tree_backend_execution_surface_adapter.cpp`'s own check: the same
//     `"exit 7"` command that returns the real `7` through `DockerExecutionSurface` directly reports
//     `0` through this adapter). `DockerExecutionSurface` and `DockerSandboxBackend` still each
//     implement their own `reset()`/`drain_to()` body independently -- NOT deduplicated by this
//     reconciliation, which unifies the CONSUMPTION path (both are now real `ExecutionSurface`s), not
//     the two conformers' own Docker-wrapping implementations.
//   - MUST-FIX (C++ correctness): `reset()`/`drain_to()` used throwing `std::filesystem::exists()`/
//     `create_directories()` overloads with no exception safety around an already-created,
//     already-real container -- an OS-level failure (permission-denied, a disconnected share) would
//     leak a live container with no cleanup attempted. FIXED: both now use the `std::error_code`
//     overloads (also matching `core/error.hpp`'s own "no exceptions for control flow" convention)
//     and translate a real filesystem failure into an ordinary `agentengine::result` error instead of
//     throwing.
//   - SHOULD-FIX (C++ correctness): `exec()`'s own comment claimed it "never observes enough from
//     `_popen`'s exit status to populate anything other than `ok` honestly" -- factually wrong, the
//     real exit code IS observed (`probe::ExecOutcome::exit_code`) but was simply discarded, and
//     `agentengine::ExecOutcome` has no field to carry it even if read. CORRECTED: the comment at
//     `exec()` now states this precisely -- a non-zero in-container exit is NOT distinguishable via
//     `klass` through this conformer, a real, disclosed data-fidelity gap, not a claimed impossibility.
//
// UPDATE (same day, an independent red-team pass on `tree_backend_execution_surface_adapter.hpp`,
// the reconciliation adapter this file's own earlier banner entry describes): found the adapter's
// own `reset()` had a real container-leak path -- `SandboxBackend::destroy()`'s locked `void` return
// gives NO way to know whether cleanup actually succeeded, and the adapter unconditionally forgot
// the handle regardless, silently orphaning a real Docker container on every transient `destroy()`
// failure after the FIRST turn (not a one-off edge case -- `SandboxRuntime::run()` calls
// `surface.reset()` on every turn). FIXED here, not just in the adapter: `try_destroy()` (below) is
// a new, checkable twin of `destroy()`, now REQUIRED by `TreeCapableSandboxBackend` itself -- the
// adapter uses it to keep (not clear) its handle on a confirmed failure, matching
// `DockerExecutionSurface`'s own already-proven "only untrack on confirmed success" discipline
// exactly, which the void-returning `destroy()` alone structurally could not support.

// UPDATE (same day, same red-team pass): also found `TreeBackendExecutionSurface::run()` silently
// collapsed `agentengine::ExecOutcome::klass` (which can legitimately be `timeout`/`oom`/`crash`/
// `policy_violation`/`escape_attempt`/`ask_pending` while `exec()` still returns a VALUE, per this
// whole design's own "a non-ok outcome is a normal result" convention) into a fabricated
// `exit_code=0` -- not merely losing precision on an ordinary nonzero exit (the already-disclosed
// gap above), but making a genuinely non-ok outcome (a policy block, an OOM kill, a timeout) look
// EXACTLY like ordinary success to any caller reading `exit_code==0`.
//
// CORRECTED (same day, a FOLLOW-UP verification pass on this paragraph's own first-reported fix,
// stale text replaced here rather than left standing): making `run()` return a `result<>` ERROR for
// `klass != ok` was itself wrong -- it collided with `SandboxRuntime::run()`'s own "an error means
// nothing was attempted, refund run_quota" rule, reopening the exact "run for free" bug class
// `RunCost` exists to prevent, for any genuinely-attempted-but-non-ok outcome. The ACTUAL, currently
// shipped fix (tree_backend_execution_surface_adapter.hpp's own top banner has the full, accurate
// account): `run()` returns a VALUE for a non-ok klass too (`exit_code=-1`, `probe::ExecOutcome`'s
// own documented "no real code available" default, with the real klass folded into `stdout_text`),
// preserving "genuinely attempted, keep the charge" semantics -- proven live via a synthetic
// `FakeCrashingBackend` conformer driven through the real `SandboxRuntime::run()`: `run_quota` is
// confirmed CONSUMED, not refunded, for a real `klass=crash` outcome.
//
// NAMED, NOT FIXED, RESIDUALS this last pass surfaced (from `SandboxRuntime::run()`'s own real step
// sequence, unrelated to whether the outcome is representable as a probe::ExecOutcome at all):
// steps 5-7 (drain_to/scan/commit) are not gated on `exec_r->klass` -- a non-ok outcome (crash,
// timeout, even `ask_pending`, whose own doc comment implies the turn hasn't really concluded) still
// gets committed as a real Ledger checkpoint, indistinguishable from an ordinary successful one
// (`Checkpoint` has no field recording the exec outcome/klass at all). Whether that is correct is a
// real, undecided design question for `SandboxRuntime` itself, out of this file's own scope. Also:
// `exit_code == -1` is already used, with a DIFFERENT meaning, by `docker_backend.hpp`'s own
// `run_capture()` (`_popen` itself failed to launch) and surfaced verbatim by `DockerExecutionSurface
// ::run()` -- a caller generic over `ExecutionSurface` conformers cannot tell "never even launched"
// (DockerExecutionSurface) from "genuinely ran, outcome not representable" (this adapter) by
// `exit_code` alone. Not currently exploitable (no such fully-generic caller exists yet), named for
// whoever eventually writes one.

#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "agentengine/trust/capability.hpp"

#include "../docker_sandbox/docker_backend.hpp"

namespace probe {

// The refinement itself: everything `agentengine::SandboxBackend<T>` already requires, PLUS two
// tree-materialization verbs, EffectContext-gated (see this file's own top banner, FATAL finding).
// Deliberately HANDLE-SCOPED (`reset(handle, host_dir, ctx)`, not `reset(host_dir)`) -- a naive copy
// of this directory's own `ExecutionSurface::reset(host_dir)` signature (execution_surface.hpp)
// assumes ONE instance per object, which fits `DockerExecutionSurface` but not `SandboxBackend`'s
// own multi-handle shape (one backend instance, many live `create()`d handles, `exec()`/`destroy()`
// already take a handle to say WHICH one). A real merge has to match create/exec/destroy's own
// shape, not ExecutionSurface's.
//
// VALIDATED AGAINST EXACTLY ONE, COPY-BASED CONFORMER -- NOT claimed to generalize. An independent
// architecture-fit red-team pass found this file's first version overclaimed "the concrete answer"
// off `DockerSandboxBackend` alone, without engaging `containerd_execution_surface.hpp` (this same
// directory) -- a second, live-tested `ExecutionSurface` conformer whose own header banner states
// that copy-based reset/drain_to "disappear" for a live-bind-mount-capable technology. Whether THIS
// handle-scoped shape would fit a bind-mount-capable `SandboxBackend` conformer (vs. needing its own,
// differently-shaped refinement) is exactly as unproven as `ExecutionSurface`'s own "generic" claim
// was before a second conformer existed for it (`sandbox_runtime.hpp`'s own disclosed limitation).
// `try_destroy()` (added same day, an independent red-team pass on the adapter that consumes this
// concept, tree_backend_execution_surface_adapter.hpp): the real, locked `SandboxBackend::destroy()`
// returns `void` (008 §2) -- no way for a caller that needs to know whether cleanup actually
// succeeded (a caller doing its own retry-until-success bookkeeping, exactly what
// `TreeBackendExecutionSurface::reset()` needs on every turn, not just the first) to find out.
// `try_destroy()` is the checkable twin every `TreeCapableSandboxBackend` conformer must also
// provide -- additive, does not change what `destroy()` itself returns.
template <class T>
concept TreeCapableSandboxBackend =
    agentengine::SandboxBackend<T> &&
    requires(T& backend, agentengine::SandboxHandle const& handle,
             std::filesystem::path const& host_dir, agentengine::EffectContext& ctx) {
        { backend.reset(handle, host_dir, ctx) } -> std::same_as<agentengine::result<void>>;
        { backend.drain_to(handle, host_dir, ctx) } -> std::same_as<agentengine::result<void>>;
        { backend.try_destroy(handle) } -> std::same_as<agentengine::result<void>>;
    };

// Maps a probe-vocabulary `probe::error` (message + code, no failure_class -- these probes are
// standalone by design, common/result.hpp's own header comment) onto the real, production
// `agentengine::error` (failure_class + message + code + native_code, core/error.hpp). `fatal` is
// the closest real failure_class for "the docker CLI transport itself failed" -- none of
// transient/policy/contract/resource fit a subprocess launch/exit failure, and this probe makes no
// claim about retryability (unlike a real production backend, which would need its own considered
// answer here, not this one).
[[nodiscard]] inline agentengine::error to_agentengine_error(probe::error const& e, char const* code) {
    return agentengine::error{agentengine::failure_class::fatal, e.message, code};
}

// Closes the FATAL security finding named in this file's own top banner: `reset()`/`drain_to()`
// authorize the requested host path against a real, caller-held `cap::SandboxMount` grant, reusing
// EXACTLY the prefix-covering logic `authorize_spec()` (sandbox.hpp) already uses for
// `SandboxSpec::mounts` -- not a new, parallel authorization scheme. Direction matters:
// `reset(host_dir)` READS host_dir (materializing it INTO the container), so `require_write=false`
// -- any covering grant suffices, matching a plain read-only `cap::FsRead`-shaped mount request.
// `drain_to(host_dir)` WRITES host_dir (materializing container output ONTO it), so
// `require_write=true` is required -- the same "a read grant never authorizes write" rule
// `authorize_spec()` itself enforces for `MountSpec::read_write` requests.
//
// HONEST LIMIT, disclosed rather than assumed away: `cap::SandboxMount` was designed for
// `SandboxSpec::mounts`' own live-bind-mount semantics, not for "the backend copies bytes between a
// host path and a container on the caller's behalf." Reusing the SAME capability type/prefix check
// for this different (but closely related) effect is this probe's own considered choice, not an
// existing, Judged decision -- a real production design would need to decide for itself whether that
// reuse is correct or whether tree-materialization needs its own capability kind.
//
// FIX (same day, an independent verification pass on this file's OWN first fix round): the first
// version of this function called `path_prefix_covers()` alone, with NO lexical `.`/`..` guard on
// `host_dir` -- reintroducing, in NEW code, the exact hole `authorize_spec()` (sandbox.hpp:222-223)
// exists to close for `MountSpec` paths. `path_prefix_covers()` is a plain string-prefix check with
// no filesystem awareness (its own doc comment says so); `std::filesystem::path::generic_string()`
// does NOT collapse `..` segments. A caller holding a real grant for `/tmp/allowed` could therefore
// call `drain_to(handle, "/tmp/allowed/../../../etc/anywhere", ctx)` -- the literal string still
// starts with the granted prefix, so the check passed, while the OS-resolved path the actual
// `std::filesystem`/`docker cp` calls operate on lands completely outside it: an arbitrary-file-write
// (via `drain_to`) or arbitrary-file-read (via `reset`) capability-scope escape, exactly the "reach
// an effect without an explicitly passed capability" shape CLAUDE.md's I2 forbids. FIXED by
// rejecting any `.`/`..` path component in `host_dir` BEFORE ever comparing prefixes -- the identical
// defense, reusing the identical (already-transitively-included) helper `authorize_spec()` itself
// calls for the same reason.
//
// STILL A LEXICAL CHECK ONLY, disclosed explicitly (a second independent verification pass, after
// this fix, asked for this line by name): neither this check nor `authorize_spec()`'s own identical
// one defends against a SYMLINK inside the granted prefix pointing outside it -- `has_dot_or_dotdot_
// component()` never touches the real filesystem, so a pre-existing symlink at, say,
// `&lt;granted_prefix&gt;/escape -> /etc` would pass this check and let `docker cp`/`std::filesystem`
// follow it for real. Symmetric with the production `authorize_spec()` precedent this file mirrors
// (same limitation, same undisclosed status there too) -- not a regression this file introduces, but
// worth naming rather than leaving implicit now that it has been asked about directly.
[[nodiscard]] inline agentengine::result<void> authorize_tree_path(
        agentengine::EffectContext const& ctx, std::filesystem::path const& host_dir,
        bool require_write, char const* code) {
    std::string const host_path = host_dir.generic_string();
    if (agentengine::sandbox_detail::has_dot_or_dotdot_component(host_path)) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            "DockerSandboxBackend (probe): host path '" + host_path + "' contains a '.'/'..' path "
            "component -- rejected outright rather than risking a lexical prefix-check bypass",
            code});
    }
    if (ctx.capabilities) {
        for (agentengine::cap::SandboxMount const& grant : ctx.capabilities->sandbox_mount_grants()) {
            if (!agentengine::capability_detail::path_prefix_covers(grant.host_path_prefix, host_path))
                continue;
            if (require_write && !grant.read_write) continue;
            return agentengine::result<void>{};
        }
    }
    return std::unexpected(agentengine::error{
        agentengine::failure_class::policy,
        "DockerSandboxBackend (probe): host path '" + host_path + "' is not covered by any granted "
        "cap::SandboxMount" + std::string(require_write ? " with read_write=true" : ""),
        code});
}

// The one real conformer this file builds: wraps the already-proven, already-fixed (identity-native
// design §35 finding 9) `probe::DockerBackend` behind BOTH the real `agentengine::SandboxBackend`
// concept and this file's own `TreeCapableSandboxBackend` refinement.
//
// NON-COPYABLE, NON-MOVABLE (a change from this file's first version, which was fully stateless and
// therefore trivially, safely default-copyable -- confirmed correct by an independent C++-correctness
// red-team pass on that version). Now holds real per-instance state (`live_containers_`, closing the
// MUST-FIX handle-hijack finding in this file's own top banner), guarded by `mutex_`. A
// `std::mutex` member already makes the implicit copy/move special members ill-formed; deleted here
// explicitly so the reason is a documented decision, not an incidental consequence a future reader
// has to rediscover. Used exclusively via `std::make_shared<DockerSandboxBackend>(...)` (constructs
// in place, needs neither), matching `SandboxBackendRegistry::register_backend()`'s own
// `shared_ptr<B>`-based contract.
class DockerSandboxBackend {
public:
    // Provisional, NOT a settled production ranking -- disclosed the same way `kDefaultShellWallClockBudget`
    // (mediated_shell_runner.hpp) discloses its own "named, provisional stand-in" status. Where a real
    // Docker container's strength should sit relative to native-jail's 50/wasm's 40/kata's 90
    // (src/backends/*/*.hpp) is a real design question this probe does not attempt to answer.
    // `strength=0` is NOT, by itself, sufficient to keep this conformer out of `resolve_strict()` --
    // an independent red-team pass's own registry probe proved that directly (as the sole
    // strict-eligible candidate in a registry, it wins trivially regardless of strength). The REAL
    // safeguard is registration mode: `probe_docker_sandbox_backend_registry.cpp` now registers this
    // type via `register_hardware_isolation_backend()` (`named_only`), the same structural guard
    // `KataBackend` relies on, not this field.
    static constexpr agentengine::ProfileTraits traits{
        /*strength=*/0,
        /*platform_mask=*/static_cast<std::uint8_t>(agentengine::platform_id::windows_x86_64) |
            agentengine::platform_id::linux_x86_64,
        agentengine::cold_start_class::milliseconds,
    };

    explicit DockerSandboxBackend(std::string image = "alpine:latest") : image_(std::move(image)) {}

    DockerSandboxBackend(DockerSandboxBackend const&) = delete;
    DockerSandboxBackend& operator=(DockerSandboxBackend const&) = delete;
    DockerSandboxBackend(DockerSandboxBackend&&) = delete;
    DockerSandboxBackend& operator=(DockerSandboxBackend&&) = delete;

    // --- Real `agentengine::SandboxBackend` conformance (008 §2a) ---------------------------------

    [[nodiscard]] agentengine::result<agentengine::SandboxHandle> create(
            agentengine::SandboxSpec const& spec, agentengine::EffectContext& /*ctx*/) {
        if (auto authorized = agentengine::authorize_spec(spec); !authorized.has_value())
            return std::unexpected(authorized.error());
        // This conformer has no live bind-mount / SandboxSpec::mounts support of its own (matching
        // `probe::DockerBackend::create()`'s own header comment: this environment's Docker Desktop
        // restricts host bind-mounts to a GUI allowlist) -- fails closed on any real mount request
        // rather than silently ignoring it. Rejects the SandboxSpec::mounts case wholesale (both the
        // host-path and BlobRef source alternatives) -- broader than `NativeJailBackend`'s own
        // precedent, which rejects only the BlobRef alternative and supports host-path mounts; the
        // real reason here is Docker Desktop's own bind-mount restriction, not a structural parallel
        // to that precedent (an earlier version of this comment overstated the analogy).
        if (!spec.mounts.empty()) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::policy,
                "DockerSandboxBackend (probe): no live SandboxSpec::mounts support -- use this "
                "conformer's reset()/drain_to() refinement for host<->container data movement instead",
                "docker_sandbox_backend.mounts_unsupported"});
        }
        // Real fix for the MUST-FIX security finding named in this file's own top banner:
        // authorize_spec() checks a NetPolicy allowlist against granted cap::SandboxNetOut entries
        // WHEN grants exist, but this conformer has no CNI/egress-proxy of any kind (no partial
        // allowlist enforcement is possible) -- so, matching NativeJailBackend's/KataBackend's own
        // real precedent (ADR-098 §5 B3, ADR-086: "fails closed on any NetPolicy beyond
        // deny_all=true"), any policy requesting anything other than full deny is rejected outright,
        // never partially honored.
        if (!spec.net.deny_all || !spec.net.allowlist.empty()) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::policy,
                "DockerSandboxBackend (probe): no partial network-egress enforcement -- only "
                "NetPolicy{deny_all=true, allowlist={}} is supported",
                "docker_sandbox_backend.net_policy_unsupported"});
        }
        if (auto safe = reject_unsafe_for_shell(image_, "image"); !safe.has_value())
            return std::unexpected(to_agentengine_error(safe.error(), "docker_sandbox_backend.create_failed"));

        // Builds the real `docker run` command directly (bypassing `DockerBackend::create()`, which
        // has no room for extra flags) so `--network none` (the ONLY policy accepted above, now
        // actually enforced -- confirmed live this session: a `ping` from inside a `--network none`
        // container reports "Network unreachable") and real resource limits can be applied at
        // creation time, with no post-creation window where the container has more access than
        // requested.
        std::ostringstream cmd;
        cmd << "docker run -d --rm -w /workspace --network none";
        if (spec.limits.memory_bytes > 0) cmd << " --memory " << spec.limits.memory_bytes;
        if (spec.limits.pids > 0) cmd << " --pids-limit " << spec.limits.pids;
        // cpu_ms/wall_ms/disk_bytes/net_bytes/output_bytes/fds remain UNENFORCED by this probe --
        // disclosed (this file's own top banner), not silently claimed covered.
        cmd << " " << image_ << " sh -c \"mkdir -p /workspace && sleep infinity\"";
        auto r = docker_detail::run_capture(cmd.str());
        if (r.exit_code != 0) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                        "docker run failed: " + r.stdout_text,
                                                        "docker_sandbox_backend.create_failed"});
        }
        std::string id = r.stdout_text;
        while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) id.pop_back();

        std::lock_guard<std::mutex> lock(mutex_);
        live_containers_.insert(id);
        return agentengine::SandboxHandle{std::move(id)};
    }

    [[nodiscard]] agentengine::result<agentengine::ExecOutcome> exec(
            agentengine::SandboxHandle& handle, agentengine::ExecRequest request,
            agentengine::EffectContext& /*ctx*/) {
        if (auto owned = check_owned(handle); !owned.has_value()) return std::unexpected(owned.error());
        DockerBackend::Instance const inst{handle.opaque_id};
        // "cd /workspace && " prefix, matching DockerExecutionSurface::run()'s own convention
        // exactly (docker_execution_surface.hpp) -- not load-bearing today (create()'s own `-w
        // /workspace` already sets the container's default WORKDIR, which `docker exec` inherits),
        // kept for defense in depth against that assumption changing and for consistency between
        // this codebase's two independent Docker wrappers over the same primitive.
        auto r = docker_.exec(inst, "cd /workspace && " + request.source);
        if (!r.has_value())
            return std::unexpected(to_agentengine_error(r.error(), "docker_sandbox_backend.exec_failed"));
        agentengine::ExecOutcome outcome;
        // A non-zero exit code is a normal, meaningful RESULT (this directory's own ExecutionSurface
        // contract, execution_surface.hpp's own doc comment). REAL, DISCLOSED GAP (corrected from an
        // earlier, factually wrong comment claiming this couldn't be observed at all): `r->exit_code`
        // IS available here but `agentengine::ExecOutcome` (sandbox.hpp) has no field to carry a raw
        // exit code -- `klass` distinguishes crash/timeout/oom/policy_violation from an ordinary
        // failing exit, which this conformer cannot populate from an exit-code alone without
        // guessing. A caller of THIS conformer must not treat `klass == ok` as "the in-container
        // command exited 0" -- only `stdout_text` (stdout+stderr merged, `_popen`'s own convention)
        // carries any signal of in-container failure here.
        outcome.klass = agentengine::exec_outcome_class::ok;
        outcome.stdout_text = r->stdout_text;
        return outcome;
    }

    void destroy(agentengine::SandboxHandle handle) {
        (void)try_destroy(handle);  // SandboxBackend::destroy()'s real, locked signature returns
                                     // void (008 §2) -- this is the ONLY place that discards
                                     // try_destroy()'s real result; every other caller in this tree
                                     // (this file's own reset()/drain_to() cleanup paths,
                                     // TreeBackendExecutionSurface's adapter) uses try_destroy()
                                     // directly and checks it.
    }

    // Real fix (an independent red-team pass on `tree_backend_execution_surface_adapter.hpp`, this
    // same day): the void-returning `destroy()` above gives a caller NO way to know whether the real
    // `docker rm -f` actually succeeded -- a caller that needs to retry-and-eventually-succeed
    // cleanup (exactly what `TreeBackendExecutionSurface::reset()` needs to do on every turn, not
    // just the first) has nothing to check. `try_destroy()` is the checkable twin: same real
    // `docker_.destroy()` call, same "only untrack on CONFIRMED success" discipline
    // `DockerExecutionSurface`'s own destructor already established (its own header comment), but
    // returns the real `agentengine::result<void>` instead of discarding it. Additive, NOT part of
    // the real `SandboxBackend` concept (`destroy()` above still satisfies that, unchanged) --
    // `TreeCapableSandboxBackend`'s own refinement now requires this too, alongside `reset()`/
    // `drain_to()`.
    [[nodiscard]] agentengine::result<void> try_destroy(agentengine::SandboxHandle const& handle) {
        DockerBackend::Instance const inst{handle.opaque_id};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!live_containers_.contains(handle.opaque_id)) return agentengine::result<void>{};
            // unknown/foreign/already-destroyed handle -- a harmless no-op-success, matching
            // destroy()'s own historical fail-silent posture for this same case (never itself the
            // FAILURE this method exists to make checkable).
        }
        auto destroyed = docker_.destroy(inst);
        if (!destroyed.has_value())
            return std::unexpected(to_agentengine_error(destroyed.error(), "docker_sandbox_backend.destroy_failed"));
        // Only untrack on a CONFIRMED successful destroy -- mirrors DockerExecutionSurface's own
        // established discipline (docker_execution_surface.hpp's own comment) of never discarding
        // the only in-process reference to a possibly-still-running container on a transient
        // failure. A caller retrying try_destroy()/destroy() with the same handle gets a real second
        // attempt, not a silent no-op against an ID this instance no longer remembers owning.
        std::lock_guard<std::mutex> lock(mutex_);
        live_containers_.erase(handle.opaque_id);
        return agentengine::result<void>{};
    }

    // --- TreeCapableSandboxBackend refinement, additive, NOT part of the real SandboxBackend concept ---

    [[nodiscard]] agentengine::result<void> reset(agentengine::SandboxHandle const& handle,
                                                      std::filesystem::path const& host_dir,
                                                      agentengine::EffectContext& ctx) {
        if (auto owned = check_owned(handle); !owned.has_value()) return std::unexpected(owned.error());
        if (auto authorized = authorize_tree_path(ctx, host_dir, /*require_write=*/false,
                                                    "docker_sandbox_backend.reset_not_authorized");
            !authorized.has_value())
            return std::unexpected(authorized.error());
        DockerBackend::Instance const inst{handle.opaque_id};
        std::error_code ec;
        bool const exists = std::filesystem::exists(host_dir, ec);
        if (ec) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                        "reset(): std::filesystem::exists(" +
                                                            host_dir.string() + ") failed: " + ec.message(),
                                                        "docker_sandbox_backend.reset_fs_error"});
        }
        if (!exists) return agentengine::result<void>{};  // nothing to seed yet
        // Trailing "/." copies host_dir's CONTENTS into /workspace, matching
        // DockerExecutionSurface::reset()'s own already-proven convention (execution_surface/
        // docker_execution_surface.hpp) exactly, including its own `generic_string()` fix (a C++
        // correctness red-team pass on that sibling file found `string()` + a literal "/." producing
        // a mixed-separator path on Windows).
        std::filesystem::path const source(host_dir.generic_string() + "/.");
        auto copied = docker_.copy_to_container(inst, source, "/workspace");
        if (!copied.has_value())
            return std::unexpected(to_agentengine_error(copied.error(), "docker_sandbox_backend.reset_failed"));
        return agentengine::result<void>{};
    }

    [[nodiscard]] agentengine::result<void> drain_to(agentengine::SandboxHandle const& handle,
                                                         std::filesystem::path const& host_dir,
                                                         agentengine::EffectContext& ctx) {
        if (auto owned = check_owned(handle); !owned.has_value()) return std::unexpected(owned.error());
        if (auto authorized = authorize_tree_path(ctx, host_dir, /*require_write=*/true,
                                                    "docker_sandbox_backend.drain_not_authorized");
            !authorized.has_value())
            return std::unexpected(authorized.error());
        DockerBackend::Instance const inst{handle.opaque_id};
        std::error_code ec;
        std::filesystem::create_directories(host_dir, ec);
        if (ec) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::fatal,
                "drain_to(): std::filesystem::create_directories(" + host_dir.string() +
                    ") failed: " + ec.message(),
                "docker_sandbox_backend.drain_fs_error"});
        }
        auto copied = docker_.copy_from_container(inst, "/workspace/.", host_dir);
        if (!copied.has_value())
            return std::unexpected(to_agentengine_error(copied.error(), "docker_sandbox_backend.drain_failed"));
        return agentengine::result<void>{};
    }

private:
    // Real fix for the MUST-FIX "any caller-supplied opaque_id string is accepted" security finding
    // in this file's own top banner: fails closed on a handle this instance never `create()`d.
    //
    // DISCLOSED, NOT FIXED (a verification pass on the first fix round found this and correctly
    // judged it not worth chasing for a probe): `check_owned()` locks, checks, and unlocks BEFORE
    // the caller's own real `docker_.exec()`/`copy_to_container()`/`copy_from_container()` call --
    // there is a real TOCTOU window where a concurrent `destroy()` on another thread can untrack (and
    // `docker rm -f`) the same container between this check and the caller's use of it. This cannot
    // become a capability/authority bypass (Docker container IDs are effectively globally unique, so
    // there is no ID-reuse-driven hijack of a DIFFERENT container through this window), only a
    // confusing runtime failure against a container that is mid-teardown -- acceptable for a
    // standalone probe, not something a real production conformer should carry unexamined.
    [[nodiscard]] agentengine::result<void> check_owned(agentengine::SandboxHandle const& handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (live_containers_.contains(handle.opaque_id)) return agentengine::result<void>{};
        return std::unexpected(agentengine::error{
            agentengine::failure_class::policy,
            "DockerSandboxBackend (probe): handle does not refer to a container this instance "
            "created -- refusing to act on a foreign/forged/already-destroyed handle",
            "docker_sandbox_backend.unknown_handle"});
    }

    std::string image_;
    DockerBackend docker_;
    std::mutex mutex_;
    std::unordered_set<std::string> live_containers_;  // guarded by mutex_
};

static_assert(agentengine::SandboxBackend<DockerSandboxBackend>,
              "DockerSandboxBackend must still satisfy the real, unmodified, locked SandboxBackend "
              "concept (008 §2a) -- the whole point of refining rather than widening");
static_assert(TreeCapableSandboxBackend<DockerSandboxBackend>);

}  // namespace probe
