# ADR-093 — KataBackend: real `NetPolicy` allowlist via `--config` mode + manual CNI + host-pinned nftables

Status: Proposed (2026-08-24; implemented, red-teamed -- one BLOCKING finding, fixed and re-verified
in the same pass -- WSL compile-verified, awaiting project-owner Judged sign-off)

## 1. The question

`KataBackend` has failed closed on any `NetPolicy` beyond `deny_all` since Slice 2/3 (ADR-086/087) --
no network endpoint is ever created for the guest VM. Continuing this session's "close the next real
KataBackend gap" sequence (Slices 4-8, ADR-088 through ADR-092), the project owner chose to
investigate the `NetPolicy` allowlist next and, after the investigation surfaced the same
`ctr run --config`-exclusivity wall `decisions/ADR-090-kata-backend-pids-limit-investigated-and-
deferred.md` already found blocking `pids`, explicitly chose to do the full `--config` + manual CNI
orchestration rather than close this as investigated-and-deferred (ADR-090's own precedent).

## 2. Two findings that change ADR-090's picture

1. **`ctr run --cni` is real but functionally broken for Kata specifically.** Source-verified: `ctr
   run`'s own `--cni` flag runs CNI setup AFTER containerd's task-CREATE step (`run.go`), but Kata's
   own network-endpoint discovery (`createNetwork()`) and VM boot (`startVM()`) both run
   synchronously DURING task-CREATE (`virtcontainers/api.go`'s `CreateSandbox()`, called from
   `containerd-shim-v2/create.go`'s `Create()` RPC handler) -- CNI populates the netns too late for
   Kata to see it. Confirmed by contrast against containerd's own CRI plugin
   (`internal/cri/server/sandbox_run.go`), which sets up CNI BEFORE `controller.Create()` -- the
   order Kata's design actually needs, and the reason Kata+CNI works fine in production under
   Kubernetes/CRI despite not working via `ctr run --cni`. Full citations:
   `docs/research/2026-08-24-containerd-ctr-run-cni-kata-ordering.md`.
2. **`ctr images mount <ref> <target>` solves the rootfs-prep gap ADR-090 flagged as blocking
   `--config` mode.** ADR-090 checked only the lower-level `ctr snapshot unpack`/`prepare`
   primitives, which are blocked by an internal, undiscoverable chain ID. `cmd/ctr/commands/images/
   mount.go` -- a separate subcommand that investigation's session did not fetch -- computes that
   chain ID itself, `Prepare`/`View`s a snapshot under a caller-chosen key, and `mount.All`s the
   result at a caller-chosen host path: a real, ready rootfs directory, zero chain-ID discovery
   needed by the caller.

Together: the `--config` rewrite is real and buildable, but CNI orchestration must be done manually,
entirely outside `ctr run`, mirroring exactly what containerd's own CRI plugin does.

## 3. What `NetPolicy::allowlist` becomes at the network layer

CNI plugins provide L2/L3 connectivity only -- not an application-level `host:port:scheme` filter.
`NetPolicy::allowlist` entries are hostnames, so enforcing them needs a DNS-to-IP step. Design: each
entry is resolved ONCE, at `create()` time, through `agentengine::sandbox::resolve_and_validate()`
(`net_egress_proxy.hpp`) -- the exact SSRF-safe resolver (blocks loopback/link-local/RFC1918/CGNAT/
multicast/reserved/metadata-address ranges) `HostEgressProxy` (ADR-011) itself uses, reused rather
than re-implemented. A default-deny nftables egress policy is then installed inside a freshly-created,
CNI-populated network namespace, permitting only the resolved `IP:port` pairs.

**A real correctness gap found during this design** (not copied from an existing pattern): the
GUEST's own DNS resolution of an allowlisted hostname could legitimately return a DIFFERENT IP than
this backend resolved host-side (round-robin/geo DNS, a different resolver) -- the IP-pinned
nftables rule would then correctly, but unhelpfully, block the guest's own connection attempt. Closed
by generating a per-instance `/etc/hosts` bind mount pinning each allowlisted hostname to the EXACT
IP the firewall permits, so the guest's own resolution and the host-side firewall always agree.

**Disclosed residual, not hidden**: this pins each hostname's IP for the container's ENTIRE LIFETIME
at `create()` time, unlike `HostEgressProxy`'s own per-request fresh resolution. This is NOT a
rebinding/SSRF hole -- a later DNS change to an allowlisted hostname cannot admit new traffic, the
firewall stays pinned to the originally-validated IP -- the residual is purely availability/
correctness: if the legitimate destination's IP later rotates (CDN failover), the guest silently
loses connectivity to it until the container is recreated.

## 4. The implementation

`KataBackend::create()` (`src/backends/kata/kata_backend.cpp`) is restructured:

1. **Policy validation first, before any resource is acquired.** Whenever `spec.net` requests
   anything beyond `deny_all` (`wants_network`), `create()` first requires the caller to hold AT
   LEAST ONE `cap::SandboxNetOut` grant at all (`kata_backend.net_capability_required` otherwise) --
   see §7's red-team finding for why this check exists and is load-bearing, not decorative. Past that
   gate, `NetPolicy{deny_all=false, empty allowlist}` (unrestricted egress -- this backend can only
   ever enforce a positive allowlist) is rejected, but by `authorize_spec()` itself
   (`sandbox.net_not_authorized`) rather than a second check in `KataBackend::create()` -- provably
   the only reachable path once the capability gate exists (see §7). Each allowlist entry is then
   parsed (`"host:port:scheme"`, `kata_backend.allowlist_entry_malformed` on a grammar violation) and
   resolved (`kata_backend.allowlist_entry_blocked` on a blocked/unresolvable address) BEFORE the
   rootfs mount or any netns/CNI resource is touched -- a caller asking for something this backend
   cannot honor fails fast, not after paying for a VM's worth of setup.
2. **Rootfs**: `ctr images mount <image_> <mount-dir>` (a fresh per-instance dir under
   `/run/agentengine-kata/<id>/rootfs`), becomes `root.path` in a hand-authored OCI spec.
3. **Network** (only when the allowlist check above passed and is non-vacuous): `ip netns add <id>`,
   then `cnitool add <cni_network_name_> /var/run/netns/<id>` with `CNI_PATH`/`NETCONFPATH` supplied
   per-call (a real, separately-installed reference CLI from
   `github.com/containernetworking/cni/cnitool`, NOT bundled with `containerd`/`ctr` -- a new
   deployment precondition), then a default-deny `nft` ruleset installed inside that netns
   (`ip netns exec <id> nft ...`) permitting only the resolved allowlist `IP:port` pairs plus
   loopback.
4. **OCI spec**: a small internal builder (`build_oci_spec_json()`, using the already-shipped
   dependency-free `agentengine::json::Value`/`dump()`) producing the fields `oci.WithSpecFromFile`
   needs -- `root`, `process` (args/cwd/env/capabilities/rlimits), `mounts` (MountSpec grants +,
   when the allowlist path ran, the `/etc/hosts` pin), `linux.namespaces` (network entry ALWAYS
   present -- omitting it would join the HOST's own netns, a real ambient-network regression; unset
   `path` for a fresh empty netns under `deny_all`, set to the CNI-populated netns path otherwise),
   `linux.resources`/`maskedPaths`/`readonlyPaths`. Default namespaces/capabilities/masked-and-
   readonly-paths/mounts/rlimits were copied from containerd's real `pkg/oci/spec.go`
   (`populateDefaultUnixSpec`/`defaultUnixCaps`/`defaultUnixNamespaces`) and `pkg/oci/mounts.go`
   (`defaultMounts`), fetched and read directly this pass -- real parity with what every container
   already gets via the convenience-flag path today, not a hand-invented, narrower security posture.
   Device-cgroup allowlisting beyond the base default-deny rule is deliberately NOT reproduced
   byte-for-byte (`oci.WithDefaultUnixDevices`) -- for a Kata guest, `/dev` is populated by the
   GUEST's own kernel at boot, not governed by the HOST's device-cgroup rules the way a runc
   container's shared-kernel `/dev` is, so this specific default carries far less weight here; a
   scoped, deliberate difference from byte-for-byte parity, named rather than silently dropped.
5. `ctr run -d --runtime <runtime_type_> --config <path> <id>` replaces the convenience-flag
   invocation. `--runtime` is confirmed to apply unconditionally regardless of `--config`
   (`run_unix.go`: `containerd.WithRuntime(...)` sits after the config/non-config branch closes).

`destroy()` reverses everything in the opposite order: task kill/rm/container rm (unchanged), rootfs
unmount (`ctr images unmount`) + local workdir removal, then (only for instances that used the
allowlist path) `cnitool del` + `ip netns delete` -- nftables rules live inside the netns's own
nftables namespace and are discarded automatically when the netns itself is deleted, no separate
`nft` cleanup step needed.

Three new constructor parameters, defaulted, same pattern as `runtime_type`/`image`:
`cni_network_name`, `cni_plugin_dir`, `cni_conf_dir` -- consulted only when a caller's `NetPolicy`
actually requests something beyond `deny_all`; unused, and their defaults never touched, otherwise.

`exec()` is unchanged -- it already operates purely via `container_id`/`--exec-id`, never touching
`create()`'s spec-authoring or netns/CNI machinery.

## 5. Red-team finding (BLOCKING) and fix

A real, independent adversarial pass (via this session's `Agent` tool, mirroring the SAME discipline
Slice 5's own red-team pass used) against this exact Slice found ONE genuine BLOCKING defect before
this ADR was closed:

**Finding: real network egress reachable with ZERO `cap::SandboxNetOut` capability held.**
`authorize_spec()` (`sandbox.hpp`) deliberately SKIPS its own `cap::SandboxNetOut` coverage check
when a caller holds zero `SandboxNetOut` grants at all -- a documented "opt-out preserved"
backward-compatibility shape, proven live by `test_sandbox_capability_authorization.cpp`'s own G1
case, that exists so a caller who never adopted the capability system sees byte-for-byte unchanged
behavior. Before this Slice, that vacuous skip was harmless FOR KATA SPECIFICALLY, because
`KataBackend::create()` itself always failed closed on any non-`deny_all` `NetPolicy` regardless of
capabilities (Slice 2/3) -- KataBackend's own unconditional check was the real backstop, not
`authorize_spec()`'s. Slice 10 removed that backstop and replaced it with a mechanism that acts on
`spec.net` directly. Without a fix, a caller could reach REAL network egress (a real netns, a real
CNI-wired interface, a real nftables ACCEPT rule to a real resolved destination) via
`spec.net.allowlist` alone, with NO `cap::SandboxNetOut` grant EVER held -- ambient authority over a
real network path, a direct I2 violation, and the red-team's own proof cited the brand-new Slice 10
test itself (case 6, the positive allowlist-reachability proof) as live evidence: it never populated
`spec.capabilities` before this fix, yet expected -- and, on a live deployment, would have gotten --
real guest network reachability.

**Fix**: `KataBackend::create()` gained its own explicit check, independent of `authorize_spec()`'s
opt-in scoping: whenever `wants_network` is true, `spec.capabilities.sandbox_net_out_grants()` must
be non-empty, or `create()` fails closed with `kata_backend.net_capability_required`. This restores
Kata's own backstop -- scoped to the zero-grant case only, since `authorize_spec()` already correctly
rejects a request that's covered by SOME grant but not the specific one requested
(`sandbox.net_not_authorized`) whenever grants are non-empty. A second-order effect, addressed in the
same pass rather than left inconsistent: this new check also makes `KataBackend::create()`'s own
former "`deny_all=false` with an empty allowlist" check (`kata_backend.net_unrestricted_unsupported`)
PROVABLY UNREACHABLE dead code -- with zero grants, the new capability check already rejects that
NetPolicy shape earlier; with any grant present, `authorize_spec()`'s own identical rejection
(`sandbox.net_not_authorized`) fires first, at the very top of `create()`, before any of this
backend's own NetPolicy validation runs at all. That now-dead check was removed rather than kept as
inert code, per this project's own "don't validate scenarios that can't happen" discipline --
`tests/test_kata_backend_slice9_10_linux.cpp` cases 2a/2b prove both surviving paths (the new
capability gate, and `authorize_spec()`'s own redundant rejection) fire correctly end-to-end through
KataBackend, not just in `sandbox.hpp`'s own isolated unit tests.

The red-team pass also traced through and explicitly RULED OUT (not exploitable) several other
angles: `/etc/hosts`-mount shadowing via a caller `MountSpec`, DNS-rebinding/TOCTOU between
`resolve_and_validate()` and use, an `nft`-installation-order race between `cnitool add` and the
`nft` rules landing, `run_ctr()`'s new `extra_env`/envp construction, `destroy()` cleanup-ordering
netns reuse via `fresh_id()` collision, and JSON injection into `build_oci_spec_json()` via
`MountSpec`/allowlist strings (confirmed `agentengine::json::dump_escaped_string()` correctly
escapes everything the old `--mount`-flag comma-injection finding was about). Two real-but-minor
hardening gaps were also named (not blocking, not fixed this pass): the generated `/etc/hosts`
content doesn't separately reject CR/LF in a hostname before writing it (very likely moot in
practice since `resolve_and_validate()`'s own resolution would reject a control-character hostname
first, and even a successful injection there could only mislead the guest's own DNS convenience,
never grant a route past the IP-based `nft` firewall); and the hosts-file/spec-file `std::ofstream`
writes in `create()` don't separately check stream failure state (a write failure would surface as
`ctr run --config` failing on a malformed/missing file -- an availability failure already caught by
the existing exit-code check and `cleanup_partial()`, not a fail-open).

## 6. Verification performed this pass

- WSL Ubuntu `build-linux` (real g++/cmake/ninja, the same cache every prior Kata slice this session
  used): `agentengine_kata_backend`, all three pre-existing Kata test binaries
  (`test_kata_backend_linux`, `test_kata_backend_slice2_linux`, `test_kata_backend_abuse_corpus_
  linux`), and the new `test_kata_backend_slice9_10_linux` all compile clean against the rewritten
  backend.
- A new test file, `tests/test_kata_backend_slice9_10_linux.cpp`, gained 7 cases: `--config`-mode
  round-trip parity (create/exec/destroy under `deny_all`, unchanged caller-visible behavior); the
  §5 capability-gate fail-closed check (case 2a) and `authorize_spec()`'s own redundant
  unrestricted-egress rejection firing end-to-end through KataBackend (case 2b); a
  malformed-allowlist-entry fail-closed check and a blocked-range (loopback) allowlist entry
  fail-closed check (cases 3-4, each now carrying the capability grant needed to reach KataBackend's
  own validation past `authorize_spec()`); the standalone allowlist-without-any-grant fail-closed
  proof (case 5); and the actual point of Slice 10 (case 6) -- proving BOTH directions of the
  allowlist (the guest reaches the allowlisted destination, and cannot reach an unlisted one) via two
  live `curl` calls inside a real `exec()`, this time WITH the matching capability grant held.
- **No live containerd/Kata/`cnitool`/nftables deployment is reachable this session** -- every claim
  about actual runtime behavior (does the hand-authored OCI spec parse and boot correctly under
  `io.containerd.kata-clh.v2`, does `cnitool add` produce a usable interface, does the `nft` argv
  tokenization used here actually parse the way a space-joined command line would, does the
  allowlist test's own two `curl` assertions actually pass) is disclosed as compile-verified-only /
  logically-derived-from-source, not empirically confirmed -- the identical disclosed posture every
  prior Kata slice this session has carried for its own `ctr` CLI assumptions.

## 7. Residuals, carried forward explicitly

- The create-time DNS-pin residual (§3) -- an availability, not security, gap.
- Three new host dependencies this backend did not previously have: `cnitool`, a CNI plugin binary
  directory + network config, and `nft` on `PATH` -- disclosed in `kata_backend.hpp`'s own header
  comment as a new deployment precondition, mirroring the existing `containerd`/`kata-clh` one.
- `ResourceLimits::pids` remains unenforced (ADR-090) -- SLICE 9's `ctr images mount` finding closes
  the SPECIFIC rootfs-prep obstacle ADR-090 named for `pids` too, but `pids` itself was not reopened
  this pass; a real, undone follow-on, not silently resolved by this unrelated NetPolicy work.
- `ResourceLimits::disk_bytes`/`net_bytes` remain unenforced (ADR-092) -- unaffected by this pass:
  Slice 10 controls WHICH destinations are reachable, not how many bytes flow to them.
- The nftables allowlist targets the netns's own default output chain, not a named interface --
  correct as long as the CNI network attaches exactly one non-loopback interface per netns (true for
  this backend's single `cnitool add` call), but would need revisiting if a future change ever
  attaches more than one.
- None of this has run against a live deployment (§5's own disclosure).
