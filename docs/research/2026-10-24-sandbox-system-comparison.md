# AgentEngine's sandbox system compared with existing systems: where do we earn the right to build our own?

Compiled: 2026-10-24. Companion to
[ADR-245](../../decisions/ADR-245-sandbox-backends-in-a-separate-repository.md) and
[`2026-10-24-cloud-sandbox-options-for-remote-backend.md`](2026-10-24-cloud-sandbox-options-for-remote-backend.md).

**Owner rule (2026-10-24):** we build or keep our own sandbox code only where it is **richer or
better** than what already exists. Everywhere else we use an existing system.

## Evidence quality

- **AgentEngine:** status is taken from `ADR-242` §3a (what an enforcement test proves, as of
  2026-10-20, updated by ADR-244), plus a check on main @ a6e6709f that the named features exist
  (`core/ledger.hpp` `reset_to_turn`, `rt/workflow_supervisor.hpp` merge-on-join, `trust/capability.hpp`
  `TaskBranch`, `sandbox/net_egress_proxy.hpp:93` `resolve_and_validate`). "Planned" means designed in
  ADR-242 and not built.
- **kubernetes-sigs/agent-sandbox:** read from source at `9afee70` (see the companion note).
- **E2B, Daytona, Modal, AWS AgentCore:** each vendor's own documentation pages, through search
  results; pages not opened in full. **Google ACE:** press only. Vendor feature flags (beta, alpha,
  "enabled customers") are noted where the docs state them.
- "Not found" means not found in the pages read, not proven absent.

## 0. Per-backend feature matrix, and the rule for reading it

**Rule (owner, 2026-10-24):** a feature counts for a backend only if **that backend** has it. Many
backends that each have some features is **breadth of backend support**, not richness. A developer
picks one backend per agent and gets only that backend's column, so the comparison with an
external system in §1 and §3 is **column by column**, never against the union of our columns.

Legend: **E** = enforced, with an enforcement test cited in `ADR-242` §3a or named below;
**set** = enforced only when the limit is set; **S** = accepted or smoke-tested only, not proven;
**—** = absent; **P** = planned (ADR-242 §6), not built; **n/a** = does not apply. Sources: ADR-242
§3a (as of 2026-10-20, with ADR-244's updates) and the code at main @ a6e6709f where named.

| Feature | Kata (`microvm`) | native-jail Windows | native-jail Linux | `wasm` | Docker / containerd surfaces | ADR-071 native providers | `NoSandboxBackend` | `remote` |
|---|---|---|---|---|---|---|---|---|
| Boundary | Hardware VM (KVM, Cloud Hypervisor) | AppContainer + restricted token + Job Object | Namespaces, `pivot_root`, seccomp-BPF, cgroups v2 | WASI capabilities, no kernel boundary | Container (shared kernel) | None (host process) | None | Stub |
| Declared strength (`traits`) | 90 | 50 | 50 | 40 | not a backend | not a backend | — | — |
| Platform | Linux + KVM | Windows 11 | Linux | Both | Linux (Docker or containerd) | Both | — | — |
| Host privilege | **Root** | User | User (needs namespaces) | User | Docker group / root for `ctr` | User | — | — |
| Mount grants checked (`authorize_spec`, I2) | E | E | E | **—** (never calls it, ADR-245 §1.2) | **— (no capability check at all)** | n/a | P | — |
| Who may create a sandbox (`cap::Exec`) | — | — | — | — | — | `cap::NativeExec` E | P (`NativeExec`) | — |
| Read-write mount semantics | Live `rbind`, no `nosuid`/`nodev` | ACL'd host directory | Bind mount | Manifest-granted subtrees | Bind mount | Whole host | P (host directory) | — |
| Egress control | E: IP allowlist at the host bridge (ADR-240, Proposed) | Python sockets through HandleRelay `NetOut` checks | **Fails closed** (no network) | Through the host egress proxy (`net_bytes` narrowed) | Network off by default (ADR-171) | — | P | — |
| Egress byte budget (`net_bytes`) | set | — | n/a | set | — | — | — | — |
| Memory limit | S (passed through; OOM never reported) | set (Job Object) | set (cgroups) | Not verified here | Default 512 MiB (ADR-171) | Engine floor 1 GiB for a held shell (`engine_floor.hpp`) | P | — |
| Process count (`pids`) | S | set | set | n/a | Default 128 | — | P | — |
| Disk | set (`disk_bytes`, loop device) | — | — | n/a | — | — | — | — |
| CPU time | — (rate only) | Unreliable (fired 3 of 11 runs) | — | — | Rate (`--cpus`) | — | — | — |
| Wall clock | set (exec and destroy, not `create()`) | E | E | E (ADR-010) | ADR-209 lifetimes | Engine floor 5 min | P | — |
| Shell tier | — (no shell provider over Kata yet) | Parsed shell, no PATH (ADR-001) | Commands only | — | Real shell, persistent (ADR-209) | Real host shell | P | — |
| CodeAct `execute_code` | P | E (dev CLI only) | P (§6 step 3b) | — | — | — (`NativePythonProvider` is never `execute_code`) | Open question (R8) | — |
| `agent.*` tool bridge through the pipeline | P | E | — | n/a (`tool-call` import not linked) | — | — | — | — |
| Per-`open()` capability checks | n/a (guest is one principal) | E (HandleRelay) | — | WASI preopens | — | — | — | — |
| Persistent shell / interpreter state | — (fresh `/bin/sh -c` per exec) | Interpreter persists | — | Fresh instance per call | E (ADR-209) | Held shell session | — | — |
| Pause with memory kept | P (unproven) | — | — | n/a | — | — | — | — |
| Snapshot / fork | — | — | — | n/a | — | — | — | — |
| Reaping idle or crashed sandboxes | — | Job Object kill on close | — | Per call | E (ADR-209 idle, age, runs) | Engine floors | — | — |
| Cold start | ~6 s | Not measured here | Not measured here | µs to low ms | Container start | Process spawn | — | — |
| Worktree verbs (ADR-242 R8) | P | Engine-side harvest (`harvest_mount`) | P | Manifest subtrees | — | — | P | — |
| `Strict`-eligible | Yes (after vetting) | Yes | Yes | Yes (lowest) | No | No | Never | — |

**What the matrix shows:**

- **No backend has every feature.** Kata has the strongest boundary and the egress controls, but
  no shell, no CodeAct, no persistent state and needs root. Windows native-jail has the richest
  agent features (parsed shell, CodeAct, `agent.*` bridge, per-`open()` checks) but a same-kernel
  boundary and only on Windows. Linux native-jail has the boundary and limits but no interpreter and
  no network. The Docker/containerd surfaces have persistence and lifetimes but no capability check.
- **The agent-layer features of §2 exist, in full, on one column: Windows native-jail**, and there
  only in the dev CLI. On a Linux server, the target of ADR-242 R1, no column has CodeAct or the
  `agent.*` bridge today.
- So "AgentEngine is richer than E2B" is true **only** for the claims in §2 that are engine-side
  and backend-independent (capability-checked mounts on three backends, the durable worktree with
  branches, merge-on-join and `reset_to_turn`, attribution). It is **not** true of any single
  backend column against a hosted system's infrastructure (§1).

**Correction found while building the matrix:** ADR-242 §3a's Docker/containerd row says the
surfaces "inherit none of RFC 008's guarantees" for hardening, limits and network. The code no
longer matches: ADR-171's `ContainerIsolation` defaults to no network, all capabilities dropped,
`no-new-privileges`, 512 MiB memory, 128 pids and one CPU (`docker_execution_surface.hpp:422-432`,
tested in `test_docker_isolation.cpp`). The authority gap in that row (no capability is checked)
still holds. The row should be corrected when §3a is next amended.

## 1. Infrastructure: isolation, lifecycle, scale

| Capability | AgentEngine (today) | agent-sandbox (K8s) | E2B | Daytona | Modal | AgentCore |
|---|---|---|---|---|---|---|
| Boundary | Kata microVM (Linux + KVM); native-jail (Windows AppContainer + Job Object; Linux namespaces + seccomp + cgroups); WASM | Whatever RuntimeClass: Kata, `kata-fc`, gVisor; shared kernel by default | Firecracker microVM | VM or container sandboxes | gVisor | microVM per session |
| Pause with memory kept | **None** (VMM pause planned, unproven, ADR-242 R3 rule 2) | No: suspend deletes the pod, keeps volumes | **Yes, the default**; filesystem-only option; auto-resume on connect | Yes for VM sandboxes ("pause / resume, fork, hot snapshots") | Filesystem snapshots; memory snapshots in alpha | Not found |
| Fork / snapshot a sandbox | None | None | Snapshots | Fork, snapshots | Filesystem, directory, memory (alpha) | Not found |
| Cold start | ~6 s (measured) | Warm pools; ~125 ms boot claimed for `kata-fc` | Sub-second claimed by third parties | Sub-100 ms claimed by third parties | Not measured here | Not found |
| Density and admission | ~170 MB idle to 2 GiB busy per VM; no admission cap | Kubernetes scheduling | Managed | Managed | Managed | Managed |
| Reaping idle or crashed sandboxes | None for Kata | `shutdownTime`, failed-pod policy | Timeouts, lifecycle | Managed | Managed | 8 h session cap |
| Egress control | IP allowlist pinned at `create()`, SSRF-safe resolver, `net_bytes` budget (ADR-240, Proposed) | NetworkPolicy; default allows the public Internet; FQDN rules need Cilium | IP, CIDR and domain allow/deny lists; bring-your-own SOCKS5 proxy (private beta), fails closed | CIDR allow list, block-all; sandbox-to-sandbox blocked by default | `block_network`, outbound CIDR and domain allow lists | Modes: sandbox, public, VPC |
| Egress byte budget | **Yes**, `net_bytes` at the host bridge | Not found | Not found | Not found | Not found | Not found |
| Host privilege needed | Root on the host for Kata (privileged helper planned) | Cluster operator only | None (hosted) | None (hosted) | None (hosted) | None (hosted) |
| GPUs | No | Via Kubernetes | No | Yes | Yes | Not found |
| Runs locally, offline, on Windows | **Yes**: native-jail on Windows 11 and Linux, no daemon, no service | Needs a cluster | Hosted (self-host via its open-source infra not verified here) | Hosted or self-hosted | Hosted | Hosted |

**Reading:** at the infrastructure layer, **existing systems are better than ours on every row
that matters for scale**: pause with memory, fork, cold start, density, reaping, GPUs, and no host
root. Ours is better in two places only:

1. **Local and embedded:** native-jail runs in-process-adjacent on Windows and on Linux without
   KVM, Kubernetes or a network service. Nothing in the table covers a desktop or an offline host.
2. **Egress byte budget** (`net_bytes`), which none of the docs read offers. It is one feature, and
   ADR-240 is still Proposed.

## 2. Agent layer: what sits above the sandbox

These are properties of the engine's contract with a sandbox, not of the sandbox. The systems above
are agent-agnostic: they expose "create, exec, files" to whoever holds the API key or the RBAC
binding.

| Capability | AgentEngine | Existing systems (all of §1) |
|---|---|---|
| **Who may create what.** Each mount and egress destination is checked against a capability held by the calling principal before the sandbox exists (`authorize_spec`, unconditional since ADR-244 slice 2a; I2) | **Built** | API key or RBAC: whoever holds it gets any sandbox and any mount the API allows |
| **Per-request authority.** A sandbox is bound to the authority it was built under; a change of principal or capability set rebuilds it (ADR-242 R3 rule 1) | Planned | Not found |
| **Code in the sandbox calls the agent's tools through the agent's pipeline**, with approval, budgets and audit, nonce-bound to one exec (`agent.*`, ADR-242 R3 rule 5, G-R8) | Built over Windows native-jail (dev CLI only); planned elsewhere | Not found. The usual pattern is network calls out of the sandbox, which needs credentials inside it |
| **Secrets never enter the sandbox**; they are used host-side through a tool call (R3 rule 6) | A rule of the design; enforced where the shim exists | ACE (press only): "no secrets at rest", short-lived per-session identity. Otherwise not found: the caller passes secrets in as environment or files |
| **Engine-owned, content-addressed, durable worktree** that outlives any sandbox (D4), with branch per child agent, merge-on-join, task branches and rollback to an earlier turn (`reset_to_turn`) | **Built** | Volumes and snapshots: whole-filesystem state, not per-agent branches with merge |
| **Harvest checks** on everything coming out (regular files only, no links or devices, path, size and count limits; ADR-242 R3 rule 4) | Specified; engine-side under ADR-245 §2.3 | Not found: file APIs copy what the sandbox has |
| **Every effect attributed** to a principal and a run in the ledger (I4); sandbox output is data, never authority (I3) | **Built** for engine effects | Platform audit logs, not tied to an agent run or a model turn |
| **Mediated shell with no ambient PATH; per-`open()` checks in Python** (ADR-001, ADR-003, ADR-085) | **Built** on Windows native-jail | Not found; a real shell sees the whole guest |

**Reading:** **this is where AgentEngine is richer.** None of it is a feature of the sandbox
infrastructure; all of it lives in the engine side of the `SandboxBackend` contract, which
ADR-245 keeps in AgentEngine.

## 3. Verdict, by the owner's rule

| Piece | Richer or better than existing? | Keep building our own? |
|---|---|---|
| Agent-layer contract and enforcement (§2) | **Yes**, and no existing system offers it | **Yes, in AgentEngine.** This is the product |
| native-jail (Windows, Linux without KVM) | **Yes for its niche**: local, offline, Windows, no daemon, per-call mediation | **Yes**, in `agentengine-sandboxes`, scoped to desktop and single-host use. Not a server-scale option |
| `KataBackend` on a single host | **No.** Same boundary as agent-sandbox with Kata; worse lifecycle, no pause, needs root | **Stop adding features.** Keep it working against the contract; do not build VMM pause, the staging volume or the privileged helper for it |
| Fleet, scale, pause, fork, warm pools | **No** | **Use existing:** agent-sandbox (Kata RuntimeClass) first, a hosted API (E2B, Daytona) second, both through the `remote` backend |
| Egress byte budget | Yes, narrowly | Keep it in the engine's egress layer where the engine controls the path; disclose it as not enforced on systems that cannot meter it |

**Consequence for ADR-242 §6 step 3:** the backend half of step 3 (Kata VMM pause, persistent guest
shell, staging volume, privileged helper) builds infrastructure that §1 shows already exists
elsewhere. Under the owner's rule it should be dropped for Kata and met instead by the `remote`
backend on agent-sandbox, whose Kata pods need no root in the engine. The contract half of step 3
(worktree verbs, engine-side harvest, `NoSandboxBackend`, the interpreter facet) stays, because it
is the §2 layer. That change to ADR-242 needs its own ADR; this note is the evidence for it.

**Open gaps that decide it** (from the companion note's gates K1–K6): whether a NetworkPolicy binds a
Kata guest's traffic (K2), whether agent-sandbox's `sandboxd` channel can carry the `agent.*` shim
with per-sandbox authentication (K3), and whether pause with memory is needed for the persistent
CodeAct interpreter (agent-sandbox has none; E2B and Daytona do).

## Sources

- Internal: `decisions/ADR-242*.md` §3a, R3, R8; ADR-244 header; ADR-245; main @ a6e6709f.
- [kubernetes-sigs/agent-sandbox](https://github.com/kubernetes-sigs/agent-sandbox), read at `9afee70`
- [E2B: sandbox persistence](https://e2b.dev/docs/sandbox/persistence),
  [auto-resume](https://www.e2b.dev/docs/sandbox/auto-resume),
  [internet access](https://e2b.dev/docs/network/internet-access),
  [bring your own proxy](https://docs.e2b.dev/network/byop)
- [Daytona: isolation](https://www.daytona.io/docs/en/isolation),
  [network limits](https://www.daytona.io/docs/ja/network-limits)
- [Modal: sandbox networking](https://modal.com/docs/guide/sandbox-networking),
  [V2 sandboxes](https://modal.com/docs/guide/sandbox-v2),
  [VM memory snapshots](https://modal.com/docs/guide/vm-memory-snapshots),
  [running untrusted code](https://modal.com/resources/run-untrusted-code-safely),
  [`modal.Sandbox` reference](https://modal.com/docs/sdk/py/latest/Sandbox)
- [AWS AgentCore: code interpreter resource management](https://docs.aws.amazon.com/bedrock-agentcore/latest/devguide/code-interpreter-resource-management.md),
  [network configuration API](https://docs.aws.amazon.com/bedrock-agentcore-control/latest/APIReference/API_CodeInterpreterNetworkConfiguration.md),
  [runtime sessions](https://docs.aws.amazon.com/bedrock-agentcore/latest/devguide/runtime-sessions.md)
- Google ACE: see `2026-10-23-google-agent-compute-engine.md` (press only)
