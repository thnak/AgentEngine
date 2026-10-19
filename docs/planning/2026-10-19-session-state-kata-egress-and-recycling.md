# Session state, 2026-10-19: ADR-237 follow-ups, SSH build server, Kata live verification, egress fix, recycling

A working record so none of this is lost to context compaction. Facts below were measured or read in
this session unless marked REASONED. Nothing here is pushed; `origin` is untouched.

## 1. Standing instructions from the project owner (this session)

- **No WSL on the dev machine; no docker for builds** (slow). Linux builds go over SSH to `thnak-server`
  with `tools/ssh-linux-build.sh`.
- **Use all cores on that server** (`-j$(nproc)`); it is a dedicated box. This overrides CONVENTIONS.md's
  `-j4` for that server only.
- Security-critical / architectural work goes through **design -> red-team -> prove -> judge**, not a patch
  (see memory `design-over-patch`).
- **Commits carry no co-author trailer** (CLAUDE.md, project owner).
- Outward-facing actions (push, issues, PRs, host firewall changes) are confirmed first. One GitHub issue
  has been created with approval: #194.

## 2. Branches (all local, none pushed)

| Branch | Content |
|---|---|
| `adr237-phase-g-deciders` | Phase G slices (deciders, executor bodies); base of everything below |
| `worktree-agent-ae6205...` | #174 triage table (merged into the Phase G branch tip) |
| `worktree-agent-a4b8d4...` | #181 `failure_class` mapping (merged into the Phase G branch tip, renumbered ADR-237 step 54) |
| `kata-live-verification` | merges of the two above + `tools/ssh-linux-build.sh`, MAF delta note, test fixes, reworked slice9_10, findings doc, and this file |
| `kata-egress-design` | design draft `docs/planning/2026-10-19-kata-egress-policy-design.md` (commit 1ad729c5) |
| `kata-egress-redteam` | red-team stage in progress (agent abe4e405...) |

Merge notes: the #174 table predated Phase G, so rows 23, 24, 44, 45 were marked **done** (steps 52-53);
outstanding conversions recounted to 9 (4 alias + 5 non-alias), 0 needs-ADR. Step 51 is reserved for
Phase E slice 1 on its own branch.

## 3. Build server (`ssh thnak-server`, user thnak, passwordless sudo via /etc/sudoers.d/thnak-nopasswd)

Bare-metal Xeon E5-2680 v4, 28 cores, 61 GB RAM, Ubuntu 26.04, gcc 15.2, clang 21.1.8, cmake 4.2.3,
ninja, ccache, mold. Cold full builds: gcc 5m07s, clang 8m14s (both clean). Full ctest on the merged tree
(gcc): 397/401; the four failures were `test_json_dump_escape` (kernel does not enforce the memory cap;
same as the old WSL baseline), and three containerd tests that need root and a pulled `alpine:latest`
(pass under sudo after the pull, once `test_containerd_isolation` was fixed, see section 5).

Use plain local `ssh thnak-server` from Bash; the MCP ssh tool's background tasks reported late with raw
curl progress. Never `pkill -f <pattern>` inside an ssh command string (it kills the ssh shell).

## 4. Kata on the server

Installed 2026-10-19: Kata 4.1.0 static tarball in /opt/kata (this is the **Rust runtime-rs**, not the Go
one), shim wrapper /usr/local/bin/containerd-shim-kata-clh-v2, CNI plugins 1.9.1 in /opt/cni/bin, cnitool
in /usr/local/bin (Go built in ~/.local), /etc/cni/net.d/10-ae-kata-net.conflist (bridge aekata0,
10.88.77.0/24; chains need the .conflist extension). The shim refuses a custom KATA_CONF_FILE, so the clh
config is installed as /etc/kata-containers/runtime-rs/configuration.toml. The shim leaks one
`/run/netns/cnitest-*` per run (40 present at the time of writing; safe to delete once no experiment is
running). A failed first `ctr run` leaves a stale socket in /run/containerd/s/.

## 5. Findings recorded elsewhere

- `docs/planning/2026-10-19-kata-live-verification-findings.md`: live suite 3/4 pass; slice9_10's network
  checks were vacuous (busybox has no curl); once real they show the nft allowlist does not filter
  microVM traffic; guest reaches host-local services. Issue #194.
- Test fixes (committed): `test_containerd_isolation` hardcoded `eth0`; Kata tests ignored `std::system`
  results (gcc `-Werror=unused-result`). slice9_10 reworked with `nc`/`wget` and a positive control (case
  9b); those checks fail until the egress fix lands.
- `docs/research/2026-10-19-maf-delta-since-09-25.md`: 325 MAF commits since 09-25; AG-UI trust-boundary
  hardening (#9368 approval state protected by shape), approval no-response fail-closed (#9152), MCP 2.x,
  workflow fan-out dedup/pause. Title-level only; no issues filed yet.

## 6. Egress design (issue #194)

Design draft recommends candidate B: per-sandbox nftables **bridge-family** table in the host namespace
keyed on the veth host-end port; hooks input/forward/output at priority -300; default-drop; exact
(guest ip+mac, dest ip, tcp port) allow tuples; ARP restricted to the gateway; IPv6 dropped; one shared
`quota` for `net_bytes`; loaded atomically after `cnitool add` and before `ctr run`; deleted after guest
kill; orphan sweep at startup; the backend never touches iptables/ufw/Docker (internet egress on a
FORWARD-DROP host stays an operator precondition). Measured by the design agent: B blocked host-local,
guest-to-guest and IPv6; a 200000-byte quota cut a download at 196,792 bytes and then blocked upload.
Rejected: netns netdev hooks (bypassed), inet hooks keyed on IP (spoofable, no guest-to-guest), tc, routed
ptp, host vsock proxy (right long-term, too big), macvlan.

Red-team questions: fail-open on table loss; br_netfilter/VLAN; fragments/options; forged ARP from a
CAP_NET_RAW guest; stateless inbound; whether adding all A records is I3-clean; bridge mDNS/DHCP/RA;
teardown races; one unexplained 0-byte first download; the "N plus one GSO packet" net_bytes contract;
scaling to 100+. Next stages: prove (build B in kata_backend.cpp, run slice9_10 live), judge, ADR that
also corrects ADR-093/095's claims.

## 7. Measurements (idle busybox guests, shipped default 1 vCPU / 2 GiB)

| What | Result |
|---|---|
| Cold boot (`ctr run -d` returns) | 5.8 s; first command done at 5.9 s |
| Parallel boot, all ready | 10 VMs 6.1 s, 20 VMs 6.1 s, 40 VMs 6.5 s |
| Warm exec of `true` into a running guest | p50 72 ms, p90 80 ms (30 runs); no degradation to 40 VMs |
| Host memory per idle VM | about 170 MB (40 VMs used 6.7 GB of 61 GB) |
| Worst case | busy guests use up to 2 GiB each: about 28 VMs by RAM |

Not tested: guests under load, counts above 40, `KataBackend::create()` alone (its `traits` say
`cold_start_class::milliseconds`; measured boot is about 6 s), per-sandbox sizing via `SandboxSpec`
(unverified whether `memory_bytes` resizes the VM or is only an in-guest limit).

## 8. Sandbox lifetime facts (read from code/spec)

- RFC 008 section 6: default lifetime `per_session`; cross-session reuse prohibited in every profile;
  pooled instances must be reset to a pre-untrusted-input snapshot; files persist via the worktree (025),
  not the sandbox. `sandbox_lifetime { per_exec, per_run, per_session }`, default per_session.
- ADR-096 `SandboxToolProvider`: lazy create on first `on_context()`, per-session scratch dir named by the
  SHA-256 digest of the session id, move-only (fork_from is a compile error). Its sandbox is a scratch
  directory, not a 025 worktree; the worktree-to-Kata mount wiring was not found in code.
- **ADR-209 `LiveShellLimits{max_idle,max_alive,max_runs}` + `ShellLifetime` (PerRun default, PerSession)**
  exist in `live_shell_lifetime.hpp` for `LiveShellSandboxProvider` (docker/containerd persistent shell
  surfaces) and `NativeShellSessionProvider`. `KataBackend` is not wired into it. No idle timeout, TTL or
  reap exists for a Kata microVM (an earlier answer in this session wrongly said none existed anywhere).

## 9. Recycling proposal (not yet an ADR; owner wants a recycle mechanism)

1. Per-sandbox idle/age/run ceilings, enforced in the Kata backend, reusing ADR-209's set, no unbounded
   default.
2. Host-wide admission cap on concurrent sandboxes or configured memory, refusing with a `resource` error
   (count configured, not idle, memory).
3. Owner-tagged orphan reaper at startup (host crash leaves VMs, netns, loop mounts; ADR-108 is the
   precedent).
4. Optional warm pool of **blank** pre-booted VMs only (never a used VM; RFC 008's pooling rule) to hide
   the 6 s boot.
5. Destroy-and-rebuild after N runs or any failed teardown; no "clean and keep".
Open: pool or not (idle RAM + shared-state surface); suspend/resume (leave out; no host passivation);
engine-owned vs host-owned cap (lean engine-owned, as ADR-209 does). Goes through the full loop; order
relative to the egress fix is the owner's call (egress first suggested: both touch create/destroy).

## 10. Other open work

- ADR-237 tracker #187: #175 (3 `make_sync_invoke` sites, two blocked on #176), #176 needs the design
  loop, #177, #178 (after #183), #179, #180 (after #181), #182 last, #185 needs Judge, #186, #170 (Dynamic
  Tool; not looked at). PR #24 (`feature/repl-command-dispatch`) looks stale; not examined.
- #181 judgement calls pending: http_error_to_error maps deadline/idle to `transient` but the new mappings
  use `resource` per D6 (recommend changing http in #180); TLS certificate rejection maps to `transient`
  (copied from http; arguably `fatal`).
- #174 stale items: issue title still says "~15"; §5.1 over-claims all three `BatchableModelCall` members
  go async (only `Call` does).
- MAF follow-ups need the diff read first (AG-UI approval-state protection, approval no-response path,
  fan-out dedup / pause-with-inflight, telemetry redaction audit, MCP 2.x).
- Remote hygiene: sweep `/run/netns/cnitest-*` when no experiment runs; `~/ae` build trees and `~/dl` on
  the server are disposable.
