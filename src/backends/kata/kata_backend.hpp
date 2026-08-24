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
// SLICE 4 (2026-08-24) -- 008 §9 G1/G2 promotion-gate evidence (partial): the first abuse-corpus test
// for this backend, `tests/test_kata_backend_abuse_corpus_linux.cpp`. This slice began as an attempt
// to close the `exec_outcome_class::oom` gap named below via an exit-code-137 heuristic (mirroring
// `LinuxNativeJailBackend`'s own 128+signal fallback), but an independent red-team pass against that
// design (`decisions/ADR-088-kata-backend-abuse-corpus.md` §3) found it FATAL: native-jail's heuristic
// rests on a documented Linux kernel `wait()` fact about a process THIS backend's own `waitpid()`
// directly reaps; KataBackend's `outcome.exit_code` instead reflects the HOST-side `ctr` CLI's own
// exit, three RPC hops removed from the guest's actual death (guest kernel -> kata-agent ->
// containerd-shim-kata-v2 -> containerd daemon -> `ctr`), with no source in this repo confirming any
// hop preserves the 128+signal convention, no fallback for the SIGKILL-direct-to-`ctr` case (exit_code
// stays -1, never 137), and no documented default guest-VM memory size to size a positive control
// against. **Rejected, not shipped** -- `exec_outcome_class::oom` remains unreachable for this
// backend, still a named REAL GAP below, now with the investigation recorded rather than silently
// absent. `cap::SandboxMount`-shaped speculative fixes without a verifiable signal are exactly the
// kind of decorative-not-real containment this project's "no vacuous claims" posture rejects.
//
// The SAME red-team pass surfaced a genuinely separate, previously-undisclosed BLOCKING gap while
// reading `exec()`'s timeout path: `run_ctr()`'s own `kill(pid, SIGKILL)` on a `wall_ms` timeout only
// terminates the HOST-side `ctr` CLI wrapper process -- it has no host-visible pid for the GUEST-side
// process that CLI was attached to, which previously kept running orphaned inside the persistent
// `sleep infinity` container until a LATER exec()/destroy() call happened to reap it.
// `exec_outcome_class::timeout` was being returned without the workload having actually stopped --
// undermining any G2 containment claim built on it. **Fixed this slice**: `exec()`'s timeout branch
// now also issues a best-effort `ctr tasks kill --exec-id <id> --signal SIGKILL <container>` against
// the SAME `--exec-id` the timed-out call minted, targeting the guest process directly rather than
// only its host-side CLI wrapper. Like every `ctr` CLI surface this file assumes, the exact flag
// syntax is NOT independently re-verified against a live Kata deployment this session (none
// reachable) -- a wrong assumption here fails into a stderr log line, not silently.
//
// The new abuse-corpus test covers, each with a positive control per 008 §9 G2:
//   - infinite loop / `wall_ms` timeout -- now also proves the guest-side kill above actually stops
//     the workload (a heartbeat file the spin loop increments stops changing after the timeout fires),
//     not merely that the host-side CLI call returned.
//   - unbounded output / `output_bytes` -- captured-stdout-never-exceeds-cap only, the same claim
//     shape as `LinuxNativeJailBackend`'s own corpus; does NOT claim the guest producer process itself
//     is killed (closing the host read pipe likely surfaces as an RPC-layer error to `ctr`, not
//     necessarily a guest-visible EPIPE -- the SAME class of host/guest orphan risk as the timeout
//     case above, named here but NOT fixed this pass, scope-bounded to the timeout path only).
//   - fork bomb / `pids` -- deliberately NOT a containment claim: `ResourceLimits::pids` has no
//     mechanism wired for this backend (unchanged gap, see below), so the corpus documents the
//     absence with a bounded, non-destructive probe rather than silently omitting the case or
//     fabricating a containment assertion nothing backs.
//   - OOM / `memory_bytes` -- NOT covered; no reliable classification signal exists (see above).
//
// SLICE 5 (2026-08-24) -- closes the unbounded-output guest-process orphan risk SLICE 4 disclosed but
// did not fix: an `output_bytes` cap breach previously left `run_ctr()` falling through to an
// UNCONDITIONAL BLOCKING `waitpid()` on the host-side `ctr` process (no bound at all if `ctr` didn't
// promptly die/error from writing into the now-closed pipe), and never issued the guest-side kill
// `exec()`'s own SLICE 4 fix already does for a `wall_ms` timeout. Independently red-teamed before
// implementation (three real BLOCKING findings against the first draft, all fixed before landing):
//   1. The naive signal for "a stream was force-closed at its cap" (`take < n` on the read that
//      crossed the boundary) silently misses a read that lands EXACTLY on the cap -- a real, not
//      edge-case-only, gap for round cap sizes. Fixed with a dedicated `output_capped` flag set
//      unconditionally at the force-close site, not inferred from `output_truncated`.
//   2/3. Classifying a capped-and-killed exec via the existing exit-code-only `ok`/`crash` check
//      (which the initial draft left "unchanged") could report `ok` outright, or `crash` at best --
//      neither honest. Fixed: a capped exec now classifies as `exec_outcome_class::policy_violation`,
//      matching this codebase's own established idiom for "the host stopped this on policy grounds"
//      (`LinuxNativeJailBackend`'s idle-phase CPU-budget kill, `MediatedPythonRunner`'s capability
//      denials) rather than misleadingly implying success or workload-caused failure.
// `run_ctr()`'s tail now treats `output_capped` the same as `timed_out` -- a courtesy non-blocking
// reap first (preferring `ctr`'s real exit code if it already died on its own), then an unconditional
// SIGKILL + bounded reap if it hasn't -- and `exec()` now issues the identical best-effort
// `ctr tasks kill --exec-id` guest-side kill for both trigger conditions. Like SLICE 4's own fix, the
// exact `ctr` CLI surface this depends on is NOT independently re-verified against a live Kata
// deployment this session (none reachable). The abuse-corpus test's Case 2 (unbounded output) now
// proves guest-side death the same way Case 1 (infinite loop) already did -- a heartbeat file the
// flooding workload writes each iteration, checked to have stopped changing after `exec()` returns --
// without assuming which of the two possible classifications (`policy_violation` if `ctr` dies fast
// from the closed pipe, or `timeout` if it doesn't and the wall_ms deadline is reached instead, both
// now correctly containing the guest side) a real deployment would actually produce.
//
// SLICE 6 (2026-08-24) -- `ResourceLimits::pids` enforcement was investigated to a real conclusion,
// not merely deferred again: closing it would need `ctr run --config <path>`, the ONLY way to set
// `linux.resources.pids.limit` (no `--pids-limit` convenience flag exists in the current `ctr` CLI,
// checked against containerd's real source, `docs/research/2026-08-24-containerd-ctr-run-config-vs-
// convenience-flags.md`) -- but `--config` mode is fully exclusive of every convenience flag this
// backend's `create()` currently relies on (`--mount`, `--memory-limit`, implicit image pull/unpack),
// does not prepare a rootfs at all (the caller must already have one on disk), and the only
// CLI-only path to prepare one (`ctr images pull` + `ctr snapshot unpack <digest>` + `ctr snapshot
// prepare`) hits a genuine information gap: `unpack` creates a snapshot keyed by an internal,
// content-addressed chain ID containerd computes but does not expose through any stable `ctr`
// CLI-level query -- discovering it would mean scraping `ctr snapshot list` against undocumented
// internal naming conventions, not calling a documented API. No `ctr tasks update` command exists
// either (checked: `attach/checkpoint/delete/exec/list/kill/metrics/pause/ps/resume/start`, no
// `update`), closing off a lower-risk "create normally, patch pids after" alternative before it
// could be attempted. **Decision (`decisions/ADR-090-kata-backend-pids-limit-investigated-and-
// deferred.md`): `ResourceLimits::pids` stays unenforced.** This CLI-process-boundary limitation is
// the direct, foreseeable cost of this backend's own deliberate architecture choice (shell out to
// `ctr`, never embed containerd's gRPC/ttrpc client -- see "Mechanism" below); real enforcement would
// mean reopening that choice, not writing more shell-out code against an information gap that exists
// because the chain ID was never serialized to a CLI output surface at all.
//
// SLICE 7 (2026-08-24) -- closes `ResourceLimits::fds`, the real, smaller opportunity SLICE 6's own
// investigation surfaced: `ctr run --rlimit-nofile <n>` IS a genuine convenience flag
// (`platformRunFlags`, unlike `pids`) that works within the EXISTING convenience-flag `create()` path
// -- no `--config` rewrite, no snapshot/chain-ID problem. A bare `<n>` (no `soft:hard` colon) sets
// both the soft and hard `RLIMIT_NOFILE` to the same value, confirmed against containerd's real
// `run_unix.go` parsing, not guessed from CLI usage text. Governs the container's own initial process
// (this backend's `sleep infinity` placeholder) for certain; whether a LATER `ctr tasks exec`-spawned
// process (this backend's real per-call workload path) inherits it is **not independently verified
// against a live Kata deployment this session** (none reachable) -- disclosed at the fix site in
// `kata_backend.cpp`, not assumed. `tests/test_kata_backend_slice2_linux.cpp` gained a new case that
// runs `ulimit -n` inside an `exec()` call under a tight `fds` cap specifically to test that exact
// open question -- compile-verified only this session, so the answer itself remains unconfirmed until
// it can run against a real deployment.
//
// SLICE 8 (2026-08-24) -- `ResourceLimits::disk_bytes`/`net_bytes` were investigated to a real
// conclusion, like `pids` in SLICE 6, not left "uninvestigated" (decisions/ADR-092-kata-backend-disk-
// net-bytes-investigated-and-deferred.md). Unlike `pids`/`fds`, these two are assigned by 008 §1b to
// **interpreter-level mediation** (a mediating process AgentEngine itself controls, instrumenting
// `open()`/`socket()` at the point of use), not a create-time backend resource cap -- confirmed
// against this project's own spec and `cgroup_limits.hpp`'s own identical precedent for
// `LinuxNativeJailBackend`. `KataBackend::exec()` shells `/bin/sh -c <source>` directly inside the
// guest via `ctr tasks exec`, with NO AgentEngine-owned process inside the VM for such mediation to
// attach to at all -- unlike `native-jail`, which pairs its OS jail with exactly this mediation via
// `MediatedPythonRunner`/`HandleRelay` (ADR-081/ADR-085), entirely host-side. Building an equivalent
// for Kata means a wholly new guest-side mediating subsystem, not a resource-limit wiring change. A
// backend-level fallback for `disk_bytes` (a VM/snapshot quota, independent of mediation) was also
// checked and is blocked by the IDENTICAL `--config`-rewrite obstacles SLICE 6 already found for
// `pids` -- no `ctr run` convenience flag for disk quota exists either. `net_bytes` is additionally
// moot right now regardless of mediation: this backend already fails closed on any `NetPolicy` beyond
// `deny_all`, so there is no network path to meter in the first place -- it collapses into the
// separately-named CNI gap below, not an independent problem. **Decision: both stay unenforced.**
//
// SLICE 9 (2026-08-24, decisions/ADR-093-kata-backend-netpolicy-allowlist-config-cni.md) -- replaces
// `create()`'s convenience-flag `ctr run` invocation with `ctr run --config <path> <id>`, closing the
// rootfs-preparation gap SLICE 6/ADR-090 found blocking `--config` mode for `pids`: that
// investigation checked only the lower-level `ctr snapshot unpack`/`prepare` primitives (blocked by
// an internal, undiscoverable chain ID) and missed `ctr images mount <ref> <target>` -- a separate
// subcommand (`cmd/ctr/commands/images/mount.go`, fetched+read fresh this pass) that computes the
// chain ID ITSELF, `Prepare`/`View`s a snapshot under a caller-chosen key, and `mount.All`s the
// result at a caller-chosen host path -- a real, ready rootfs directory, zero chain-ID discovery
// needed by this backend. `--config` mode's OCI spec is hand-authored (`build_oci_spec_json()` in
// `kata_backend.cpp`) to real parity with what `oci.WithDefaultSpecForPlatform`/`defaultMounts()`
// already grant every container via the convenience-flag path today (containerd's `pkg/oci/spec.go`/
// `pkg/oci/mounts.go`, fetched+read this pass for the exact default namespaces/capabilities/
// masked-and-readonly-paths/mounts/rlimits) -- not a hand-invented, narrower security posture.
// `MountSpec`/`memory_bytes`/`fds` map onto structured spec JSON fields instead of `--mount`/
// `--memory-limit`/`--rlimit-nofile` convenience flags; behavior is unchanged from Slice 2/7 for all
// three. `exec()` is unchanged -- it already operates purely via `container_id`/`--exec-id`, never
// touching `create()`'s spec-authoring machinery.
//
// SLICE 10 (2026-08-24, same ADR) -- the actual point of this pass: a REAL `NetPolicy` allowlist,
// not the unconditional fail-closed Slice 2/3 left in place. Source-verified first that `ctr run
// --cni` (a real flag, unlike `--pids-limit`) cannot be used for this: containerd's own CNI setup
// runs AFTER task-CREATE (`run.go`), but Kata's network-endpoint discovery + VM boot both run
// synchronously DURING task-CREATE (`virtcontainers/api.go`'s `CreateSandbox()`: `createNetwork()`
// then `startVM()`, both inside the shim's `Create()` RPC) -- CNI would populate the netns too late
// to be seen (full citations: `docs/research/2026-08-24-containerd-ctr-run-cni-kata-ordering.md`,
// confirmed by contrast against containerd's own CRI plugin, which sets up CNI BEFORE
// `controller.Create()` -- the order Kata's design actually needs, unreachable via `ctr run`'s own
// convenience flag). So this backend does the SAME thing CRI does, manually: `ip netns add`, then
// `cnitool add <network> <netns-path>` (a real, separately-installed reference CLI from
// `github.com/containernetworking/cni/cnitool` -- NOT bundled with `containerd`/`ctr`, a NEW
// deployment precondition, see below) with `CNI_PATH`/`NETCONFPATH` supplied per-call, BEFORE
// `ctr run --config` ever runs, joining that pre-populated netns via the spec's own
// `linux.namespaces` entry (exactly matching `internal/cri/server/sandbox_run.go`'s own ordering).
//
// `NetPolicy::allowlist` entries are hostnames, not IP literals -- CNI itself only provides L2/L3
// connectivity, not an application-level `host:port:scheme` filter. Each entry is resolved ONCE, at
// `create()` time, through the SAME `agentengine::sandbox::resolve_and_validate()`
// (`net_egress_proxy.hpp`) `HostEgressProxy` itself uses -- SSRF-safe (blocks loopback/link-local/
// RFC1918/CGNAT/multicast/reserved/metadata-address ranges), reused rather than re-implemented. A
// default-deny nftables egress policy is then installed INSIDE the fresh netns (`ip netns exec <id>
// nft ...` -- rules live in that netns's own nftables namespace, discarded automatically when the
// netns itself is deleted, no separate `nft` cleanup step needed) permitting only the resolved
// `IP:port` pairs. A real correctness gap found during this design (not copied from an existing
// pattern): the GUEST's own DNS resolution of an allowlisted hostname could legitimately return a
// DIFFERENT IP than this backend resolved host-side (round-robin/geo DNS, a different resolver),
// which the IP-pinned nftables rule would then correctly but unhelpfully block -- closed by
// generating a per-instance `/etc/hosts` bind mount pinning each allowlisted hostname to the EXACT
// IP the firewall permits, so the guest's own resolution and the host-side firewall agree.
//
// **Disclosed residual, not hidden**: this pins each hostname's IP for the CONTAINER'S ENTIRE
// LIFETIME at `create()` time, unlike `HostEgressProxy`'s own per-request fresh resolution. This is
// NOT a rebinding/SSRF hole -- a later DNS change to an allowlisted hostname cannot admit new
// traffic, the firewall stays pinned to the originally-validated IP -- the residual is purely
// availability/correctness: if the legitimate destination's IP later rotates (CDN failover), the
// guest silently loses connectivity to it until the container is recreated.
//
// **New deployment precondition** (mirrors this file's existing `containerd`/`kata-clh` precondition
// below, stated rather than silently assumed): `cnitool`, a working CNI plugin binary directory, and
// a CNI network config must already exist on the host and be named via the three new constructor
// parameters (`cni_network_name`/`cni_plugin_dir`/`cni_conf_dir`) for SLICE 10's path to be reachable
// at all -- `deny_all == true` with an empty allowlist (unchanged default) never touches any of this.
//
// **Red-team finding (BLOCKING), fixed before landing** (ADR-093 §5): before this fix, a caller with
// ZERO `cap::SandboxNetOut` grants at all could still reach real network egress via `spec.net`
// directly -- `authorize_spec()`'s own capability-coverage check (sandbox.hpp) deliberately SKIPS
// itself when a caller holds no relevant grants at all (a documented cross-backend opt-out shape),
// and this backend's OWN unconditional pre-Slice-10 fail-closed check (which used to be the real
// backstop) was removed by Slice 10 in favor of a mechanism that acts on `spec.net` directly. Fixed:
// `create()` now requires at least one `cap::SandboxNetOut` grant whenever `spec.net` requests
// anything beyond `deny_all`, independent of `authorize_spec()`'s own opt-in scoping
// (`kata_backend.net_capability_required` otherwise) -- see the fix site in `kata_backend.cpp` for
// the full reasoning, and `tests/test_kata_backend_slice9_10_linux.cpp` cases 2a/2b/5 for the proof.
//
// SLICE 11 (2026-08-24, docs/planning/kata-backend-config-pipeline-pids-disk-net-redesign-draft.md,
// design -> independent red-team (8 findings, 5 BLOCKING, all fixed pre-implementation) -> implement)
// -- closes `pids`/`disk_bytes`/`net_bytes`, the three remaining unenforced `ResourceLimits` axes,
// via the SAME `--config`-mode pipeline SLICE 9/10 already built (this project-owner-authorized
// redesign, not a patch, per 2026-08-24 direction). Reopens and supersedes ADR-090's/ADR-092's own
// negative decisions -- both were correct at the time given the information available then, not
// wrong; SLICE 9's `ctr images mount` finding and SLICE 10's netns/nftables machinery independently
// unblocked what those two ADRs each separately found closed.
//
//   - `pids`: `build_oci_spec_json()`'s `resources` object gains a `pids.limit` member, the identical
//     shape `memory` already has -- no new mechanism, just a same-shape addition SLICE 9's own
//     `--config` pipeline already made possible.
//   - `net_bytes`: a NAMED `nft` quota object, shared by rules on BOTH the existing `output` chain
//     and a new `input` chain in the same per-instance netns table SLICE 10 already creates -- total
//     bidirectional bytes, not egress-only (an egress-only counter would miss a large inbound
//     response). Entirely host-side; zero guest cooperation, zero attachment to the guest's own
//     interpreter/shell the way 008 §1b's "interpreter-level mediation" framing correctly says this
//     backend has no attachment point for. New deployment precondition: the host's `nft` build must
//     support named quota objects (`nft_quota`).
//   - `disk_bytes`: a backend-owned, loop-device-backed, fixed-size ext4 filesystem hosts the
//     writable half of a REAL overlay mount (`ctr images mount`'s own read-only View() output as
//     `lowerdir`, the loop-backed filesystem as `upperdir`/`workdir`) -- enforcement is a real guest
//     kernel `ENOSPC` once the loop filesystem fills, not a heuristic. `fallocate -l` reserves the
//     full `disk_bytes` on the HOST eagerly at `create()` time (a considered choice, not an oversight
//     -- see the design draft §5 point 2 for why eager beats the lazier `truncate -s` alternative for
//     a multi-tenant host). New deployment preconditions: `losetup`, `mkfs.ext4` (`e2fsprogs`), a
//     loop-device-capable kernel.
//
// A REAL, capability-independent I2 gap was found and fixed by this slice's own red-team pass before
// any of the above landed: `create()`'s existing `MountSpec` validation (unchanged since SLICE 2)
// never rejected a host path targeting this backend's OWN `/run/agentengine-kata` workdir, and
// `authorize_spec()`'s (`sandbox.hpp`) own capability check skips itself entirely for a caller
// holding zero `cap::SandboxMount` grants -- meaning a caller with NO grant at all could bind-mount
// this backend's own live loop-backed disk-quota file straight into a guest, corrupting a filesystem
// mounted live through a different path (writes bypassing the loop/ext4 I/O path entirely). Fixed:
// `create()` now rejects any `MountSpec` host path equal to, an ancestor of, or contained within its
// own workdir root, unconditionally -- independent of any capability grant, `kata_backend.
// workdir_mount_forbidden`.
//
// A second real gap this slice's own rootfs restructuring would otherwise have silently introduced,
// also found and fixed by the same red-team pass: `rootfs_dir` previously did double duty as both
// the literal `ctr images mount` target AND (with this slice) the final overlay mountpoint; without
// a separately-tracked `lower_dir`, `destroy()`'s own `ctr images unmount` call would silently
// mistarget the overlay mountpoint instead of the real snapshot, leaking a containerd snapshot
// mount/lease on every `disk_bytes > 0` create/destroy cycle. Fixed: `Instance::lower_dir` is now
// always tracked separately (whether `disk_bytes` is set or not -- when unset, `rootfs_dir` is a
// plain `mount --bind` of `lower_dir`, preserving byte-for-byte prior behavior for callers who don't
// opt in) and `destroy()`/the `create()` failure-path cleanup always unmount `lower_dir`, never
// `rootfs_dir`, for the real snapshot release.
//
// A third finding, contract-shaped rather than security-shaped: `losetup -f` (find a free loop
// device) has a benign-for-corruption but real-for-the-registry's-own-"tolerate concurrent calls"
// TOCTOU under concurrent `create()` calls (the kernel's own `LOOP_SET_FD` exclusivity prevents
// double-use, but a purely load-driven race could still spuriously fail an unrelated caller's
// `create()`). Fixed with a `KataBackend`-private `std::mutex` serializing just the
// `fallocate`/`mkfs.ext4`/`losetup -f`/loop-mount sequence, not `create()` as a whole.
//
// Still NOT done, named honestly rather than silently assumed:
//
//   - `ExecRequest::source` is treated as a shell command line (`/bin/sh -c <source>`) the caller is
//     trusted to have already resolved and mediated -- 008-Sandbox-and-Isolation.md §2's "`ExecRequest`
//     mediation is the caller's responsibility, not the backend's" clause is the canonical statement
//     of this contract (shared verbatim with `LinuxNativeJailBackend`'s own identical M2-only scope,
//     linux_native_jail_backend.hpp); investigated in
//     docs/planning/sandbox-exec-request-capability-mediation-design-draft.md and found to be
//     correct-by-design, not an unfinished wiring task -- that same investigation also surfaced a
//     real, SEPARATE, still-open gap (`cap::Exec`, the top-level "may this caller create a sandbox of
//     this profile" capability, currently unenforced by any backend) named there as a disclosed
//     follow-on, not silently folded into this bullet.
//   - GPU passthrough (Kata's headline capability) is explicitly OUT of scope -- the design draft's
//     own C3 finding named this as needing a real `capability_kind` this codebase does not have yet
//     and deliberately declined to add without a real consumer.
//   - `exec_outcome_class::oom` is unreachable for this backend -- investigated and rejected in SLICE
//     4, not merely unattempted.
//   - `ResourceLimits::pids`/`disk_bytes`/`net_bytes` are now ENFORCED as of SLICE 11 (ADR-095,
//     reopening ADR-090's/ADR-092's own earlier negative decisions once ADR-093's unrelated findings
//     made them stale) -- see this file's own SLICE 11 header comment above for the mechanism.
//     Whether `linux.resources.pids.limit` is honored by `kata-agent` the way runc honors it on a
//     shared host kernel, and every other SLICE 11 runtime-behavior claim, is compile-verified only
//     this session (one exception: the workdir-mount-exclusion I2 fix is proven by real test
//     execution, see ADR-095 §6) -- NOT independently verified against a live deployment.
//   - `ResourceLimits::fds` maps to `--rlimit-nofile`-equivalent spec JSON (`process.rlimits`, SLICE
//     9's spec builder) as of SLICE 7's original convenience-flag fix; whether it reaches a
//     `ctr tasks exec`-spawned process (not just the container's own initial placeholder process) is
//     still disclosed, not verified -- unchanged by SLICE 9's spec-authoring rewrite.
//   - SLICE 9/10's entire `--config`-mode pipeline (spec-file authoring, `ctr images mount`, `ip
//     netns`/`cnitool`/`nft` orchestration) is compile-verified only -- NOT independently verified
//     against a live containerd/Kata/CNI/nftables deployment this session (none reachable), same
//     disclosed posture as every prior slice's own `ctr` CLI assumptions. The exact `nft` argv
//     tokenization (each clause as a separate argv entry rather than one shell-joined string) is
//     assumed, not empirically confirmed.
//   - The nftables allowlist targets the netns's own default output chain (not a named interface) --
//     correct as long as the CNI network attaches exactly one non-loopback interface per netns
//     (true for a single `cnitool add` call, this backend's only usage), but would need revisiting if
//     a future change ever attaches more than one.
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

#include <mutex>
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
    // namespace, or `ctr images mount` (SLICE 9) will pull it itself the first time (network-
    // dependent, not this backend's concern to police).
    //
    // SLICE 10: `cni_network_name`/`cni_plugin_dir`/`cni_conf_dir` are only consulted when a
    // `SandboxSpec::net` grant actually requests something beyond `deny_all` -- unused, and their
    // defaults never touched, for every caller that doesn't opt into `NetPolicy::allowlist`.
    // `cni_network_name` must match the `name` field inside a CNI config file already present under
    // `cni_conf_dir`; `cni_plugin_dir` is where the named CNI plugin binaries themselves live (both
    // passed to `cnitool` as `NETCONFPATH`/`CNI_PATH` per this file's own header comment).
    explicit KataBackend(std::string runtime_type = "io.containerd.kata-clh.v2",
                          std::string image = "docker.io/library/busybox:latest",
                          std::string cni_network_name = "ae-kata-net",
                          std::string cni_plugin_dir = "/opt/cni/bin",
                          std::string cni_conf_dir = "/etc/cni/net.d")
        : runtime_type_(std::move(runtime_type)),
          image_(std::move(image)),
          cni_network_name_(std::move(cni_network_name)),
          cni_plugin_dir_(std::move(cni_plugin_dir)),
          cni_conf_dir_(std::move(cni_conf_dir)) {}
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
        std::string rootfs_dir;       // SLICE 9: the final container root -- as of SLICE 11 this is
                                       // ALWAYS a mount (a plain bind mount of lower_dir when
                                       // disk_bytes is unset, or a real overlay when it is set), never
                                       // the literal `ctr images mount` target directly (see
                                       // lower_dir below) -- destroy() unmounts and removes this.
        std::string lower_dir;        // SLICE 11: the REAL `ctr images mount` target -- destroy()/
                                       // create()'s own failure-path cleanup always
                                       // `ctr images unmount` THIS path, never rootfs_dir (fixes a
                                       // real snapshot-leak red-team finding, see this file's own
                                       // SLICE 11 header comment).
        std::string loop_device;      // SLICE 11: the `/dev/loopN` device backing this instance's
                                       // disk-quota filesystem -- disk_quota_active only, empty
                                       // otherwise. Recorded here so destroy() detaches the EXACT
                                       // device this instance attached, never re-discovered.
        bool disk_quota_active = false;  // SLICE 11: true iff create() built the loop-device/overlay
                                          // disk-quota stack for this instance (spec.limits.disk_bytes
                                          // > 0) -- destroy() only reverses that stack when this is
                                          // set; otherwise rootfs_dir is a plain bind mount.
        bool net_created = false;     // SLICE 10: true iff create() ran the ip-netns/cnitool/nft
                                       // sequence for this instance -- destroy() only reverses it
                                       // when this is set (deny_all instances never touch it).
    };

    std::string runtime_type_;
    std::string image_;
    std::string cni_network_name_;
    std::string cni_plugin_dir_;
    std::string cni_conf_dir_;
    std::unordered_map<std::string, Instance> instances_;
    // SLICE 11: serializes ONLY the fallocate/mkfs.ext4/losetup-f/loop-mount sequence of create()'s
    // disk-quota setup across concurrent create() calls (released before ctr run --config itself,
    // which needs no serialization) -- closes a real, if corruption-benign, TOCTOU against the
    // sandbox_backend_registry.hpp-documented "tolerate concurrent create/exec/destroy calls from
    // unrelated sessions" contract. See this file's own SLICE 11 header comment.
    std::mutex disk_quota_setup_mutex_;
};

static_assert(SandboxBackend<KataBackend>,
              "KataBackend must satisfy the SandboxBackend concept (008 §2)");

}  // namespace agentengine::kata
