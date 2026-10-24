# Existing cloud and cluster sandbox systems as the `remote` backend

Compiled: 2026-10-24. Question: before writing our own scale-out sandbox fleet, which existing
system should the `remote` backend (`src/backends/remote/README.md`, a stub, "Not scheduled" in
`ADR-242`) drive? Owner direction for this note: **prefer an existing system, Kubernetes first.**
Not an ADR; no spec change is proposed. Follows
`docs/research/2026-10-23-google-agent-compute-engine.md`.

## Evidence quality

- **kubernetes-sigs/agent-sandbox (§2): primary source.** The repository was shallow-cloned at
  `9afee70` (commit date 2026-10-23) and the API types, SDK, `sandboxd` guide, router authorizer,
  roadmap and threat model were read directly. Line references below are to that commit.
- **Hosted services (§4): secondary sources only.** Vendor-written comparison posts, which
  disagree with each other (for example on Daytona's isolation and on cold-start figures). No
  vendor's own documentation was read except AWS's. Treat every row as unverified.
- Nothing was run. No cluster was created and no latency or density figure was measured.

## 1. Recommendation

Make the first `remote` backend a client of **kubernetes-sigs/agent-sandbox**, with the isolation
runtime (Kata or gVisor) chosen explicitly through the Kubernetes RuntimeClass. Reasons:

- It is a SIG Apps project (Apache-2.0) with a served `v1beta1` API on all CRDs and installs
  pinned at `v1.0.2`. It is not a single vendor's product.
- It keeps the isolation we already chose. It runs Kata (including `kata-fc`, Firecracker) or
  gVisor underneath, so our hardware-VM boundary survives the move to a cluster.
- It solves what `KataBackend` does badly at scale: scheduling across nodes, warm pools,
  expiry (`shutdownTime`), suspend with retained volumes, and failed-pod recreation.
- **It removes the root requirement from the engine process.** The engine needs Kubernetes
  credentials (RBAC on the Sandbox CRDs), not `CAP_SYS_ADMIN`. Host privilege moves to the
  cluster's node runtime, which the operator already runs. This is a different answer to the same
  problem as `ADR-242` G-R11's privileged helper; it does not replace G-R11 for single-host Kata.
- ACE's open-sourced "Agent Substrate" is reported to be Kubernetes-native on GKE; if it is this
  project or built on it, the same backend would cover it. **Unverified.**

Hosted APIs (E2B, Daytona, Modal) are a later, second conformer behind the same backend contract,
not the first target.

## 2. What agent-sandbox provides (as read from source)

**Resources.** Core `Sandbox` (`agents.x-k8s.io/v1beta1`): "a single, stateful pod with a stable
identity and persistent storage". Extensions (`extensions.agents.x-k8s.io/v1beta1`):
`SandboxTemplate`, `SandboxClaim` (allocate from a pool), `SandboxWarmPool`. The roadmap marks
`v1beta1` as "the exclusive served API version"; `v1alpha1` is removed.

**Spec fields** (`api/v1beta1/sandbox_types.go`):

- `podTemplate` (a full `corev1.PodSpec`), `volumeClaimTemplates` (PVCs owned by the Sandbox),
  optional `service`, `failedPodPolicy` (`Retain` default, or `Recreate`; PVCs are kept).
- `operatingMode`: `Running`, or `Suspended`, which "terminates the backing Pod while retaining
  the Sandbox object and its volumes" (`:243-247`). **Suspend is pod deletion, not a memory
  snapshot**: process state is lost and only volume contents survive.
- `shutdownTime` and `shutdownPolicy` (`Retain` default, or `Delete`): expiry deletes the Pod and
  Service. "If unset, the Sandbox never expires" (`:369-390`).

**Command and file access.** The in-pod daemon `sandboxd` (`packages/sandboxd/USER_GUIDE.md`):

- gRPC `ProcessService` on `:9090`: `Start` (streaming), `Execute`, `WriteStdin`, `SendSignal`,
  `ResizeTTY` (`spec/process/v1/process.proto:30-47`).
- REST filesystem API on `:8080`: atomic write, read, list, exists, recursive delete.
- **Unauthenticated unless configured**: `--auth-token-sha256` / `--auth-token-file`; "Neither
  leaves the API unauthenticated" (`USER_GUIDE.md:200-201`).
- A legacy HTTP `/execute` endpoint (python-runtime) also exists; the Python SDK supports both.

**Router.** `sandbox-router` is an HTTP reverse proxy from clients to sandbox pods. Two caveats
read from the source:

- **Its default authorizer is `AllowAll`** (`sandbox-router/authz/authorizer.go:158-165`). The
  threat model says the pod-IP class check is then "currently the *only* SSRF defense" and
  recommends a custom authorizer. Scoped-token and TokenReview authorizers exist in the same
  directory.
- It "is not a complete `sandboxd` transport" for the gRPC `ProcessService`
  (`USER_GUIDE.md:22-24`). A "1st Class Router" is Planned on the roadmap.

**Network.** Per the threat model, `SandboxTemplate` defaults to a managed NetworkPolicy: ingress
only from the router, **egress to the public Internet** with RFC1918 and metadata endpoints
blocked. FQDN allowlists need an L7-capable CNI (Cilium examples exist); attaching L4/L7 policy
per claim is Planned.

**Isolation.** Delegated to the RuntimeClass. The Kata-on-GKE example states that by default
"all sandboxes share the host node's kernel". Kata on GKE needs nested virtualisation (Intel N2,
Ubuntu node image). The Firecracker example claims a ~125 ms boot (the project's figure, not ours).

**Clients.** Python and Go SDKs; TypeScript in progress. **No C++ client.**

**Planned, not built** (roadmap): auto suspend/resume, scale to zero, TTL auto-deletion of idle
sandboxes, identity association at claim time, network policy attach at claim time, storage
sizing at claim time.

## 3. Fit against AgentEngine's constraints

| Constraint | What agent-sandbox gives | What our backend must add or disclose |
|---|---|---|
| `ADR-242` §2: nothing implicit | Defaults to the shared-kernel runtime, `AllowAll` router, unauthenticated `sandboxd`, Internet egress, no expiry | Every one of these becomes a named, required option with a startup error when unset. Runtime class, `sandboxd` auth, egress posture and `shutdownTime` are never inherited from cluster defaults |
| I2: mounts need covering `cap::SandboxMount` grants | `podTemplate` is a full `PodSpec`, which can express `hostPath` and arbitrary volumes | Generate the PodSpec from `SandboxSpec` only, after `authorize_spec`; never accept a developer PodSpec fragment. A mount means a PVC or `sandboxd` file transfer, never `hostPath` |
| R8 / D4: every backend hosts the engine-owned durable worktree | Sandbox-owned PVCs that survive suspend and failed-pod recreation | **Open design question.** Either the worktree lives on a PVC the engine reaches only through `sandboxd`'s file API, or it stays on the engine host and is synced. Whether a cluster PVC satisfies "engine-owned" needs an ADR |
| Egress allowlist (`ADR-240`) | Managed NetworkPolicy defaults to Internet egress | Default to deny-all; render the allowlist as NetworkPolicy `ipBlock`s (we already pin hostnames to IPs at `create()`). FQDN rules need Cilium: disclose it as a cluster precondition |
| I8 limits | Pod `resources` (CPU, memory, ephemeral storage); PVC size | `pids` depends on the kubelet's pod PID limit (node configuration). `wall_ms` stays engine-side. `net_bytes` has no equivalent: disclose it as not enforced |
| Who may create a sandbox | Kubernetes RBAC on the CRDs | The engine's service account is a resource the developer chooses and owns (R7). It must be namespace-scoped, never cluster-wide |
| I3 | n/a | Unchanged: guest output is data |
| Strict eligibility | Depends on RuntimeClass | Kata or `kata-fc` could be Strict-eligible; gVisor (userspace kernel) and the default runtime should not be, pending a judge |
| Transport from C++ | gRPC `:9090` + REST `:8080` + Kubernetes REST API | gRPC needs HTTP/2, a heavy dependency. Tier 2 allows one behind a CMake option. The router's incomplete gRPC support means talking to pod IPs in-cluster or through port-forward |
| Persistent shell (§6 step 3) | `ProcessService.Start` streams with stdin/PTY | A better fit than `ctr exec`; still lost on suspend |

## 4. Hosted alternatives (secondary sources, unverified)

| Service | Isolation (as reported) | Relevance |
|---|---|---|
| E2B | Firecracker microVM | API-shaped (create, exec, files). Hobby sessions capped at 1 h, Pro at 24 h; no GPUs; bring-your-own-cloud only on Enterprise |
| Daytona | Disputed: Docker containers vs. Firecracker | Open source, self-hostable; GPUs; Linux and Windows machines |
| Modal Sandboxes | gVisor | GPUs; part of a larger compute platform |
| Vercel Sandbox | Not established | Mainly for Vercel deployments |
| AWS Bedrock AgentCore Runtime | microVM per session, memory sanitised at end, 8 h cap (AWS docs) | Hosts the whole agent; a deployment target for AgentEngine, not a backend. AWS says it "does not enforce session-to-user mappings" |
| Google ACE | microVM per agent (press) | Preview not open; see the 2026-10-23 note |

All of these need the same R8 worktree answer as §3 and leave egress enforcement with the vendor.
None can carry an "engine enforces" cell in `ADR-242` §3a without a positive-control test run
against the real service.

## 5. Suggested next steps (when the repo is stable)

1. Stand up `kind` with gVisor, and a nested-virt node with Kata, on `thnak-remote`. Record
   claim latency, cold start and per-sandbox memory ourselves, so we don't rely on project
   figures.
2. Write an ADR (`design → red-team → prove → judge`) for an `AgentSandboxBackend` covering §3's
   rows. The R8 worktree question and the gRPC dependency are the two design decisions.
3. Shape the backend contract so a hosted API (E2B or self-hosted Daytona) can be a second
   conformer later, without designing for it now.

## 6. Build-our-own criteria

Owner direction (2026-10-24): build our own fleet only if it is stronger and richer than the
Kubernetes option. Before any ADR, the prove phase on a real cluster should decide these gates.
Each gate is falsifiable.

| Gate | Passes if | If it fails |
|---|---|---|
| K1 boundary | A Kata RuntimeClass sandbox's guest cannot reach the node, other sandboxes, or the metadata endpoint; positive control: same probe from a default-runtime pod succeeds | Blocker: no fleet backend until fixed |
| K2 egress | A deny-all NetworkPolicy plus `ipBlock` allowlist binds traffic leaving a Kata guest; positive control: an unlisted IP is reachable without the policy | Replace only the egress layer (our own node-level enforcement, as `ADR-240` does), keep the rest |
| K3 control channel | Every exec and file call is authenticated per sandbox; a request with another sandbox's token is refused | Ship our own in-guest daemon that speaks the `sandboxd` protocol with mandatory auth (`USER_GUIDE.md` "Implementing a compatible runtime") |
| K4 worktree | The engine-owned worktree survives pod loss and suspend, and nothing but the engine writes it outside the sandbox | Keep the worktree on the engine host and sync it; no change to the cluster side |
| K5 limits | CPU, memory, disk and pids hold under a capped hostile corpus | Disclose any failing limit in `ADR-242` §3a; it is not a reason to rebuild |
| K6 measured cost | Claim latency, cold start and idle memory are no worse than `KataBackend`'s measured ~6 s and ~170 MB | Tune warm pools and `kata-fc` first |

Only these failures would justify our own fleet: suspend that keeps process memory (snapshot and
restore), which agent-sandbox does not do today, or a K1 failure. Even then, contributing a
snapshot to agent-sandbox should be weighed first, because a fork would leave us maintaining the
scheduler, warm pools and reaping ourselves.

## Open questions

- Is ACE's "Agent Substrate" agent-sandbox, a fork, or something separate?
- Can `ProcessService` be reached through the router once "1st Class Router" lands, removing the
  need for in-cluster networking from the engine host?
- Does a Sandbox-owned PVC, reached only through `sandboxd`, satisfy D4's "engine-owned and
  durable" worktree, or must the worktree stay on the engine host?
- Do NetworkPolicy `ipBlock` rules bind traffic leaving a Kata guest, or only the pod network
  namespace on the node? This is the same boundary question `ADR-240` answered for host Kata, and
  it needs a positive-control test.

## Sources

- kubernetes-sigs/agent-sandbox, commit `9afee70` (2026-10-23), read from a local clone:
  `api/v1beta1/sandbox_types.go`, `roadmap.md`, `docs/security/threat_model.md`,
  `packages/sandboxd/USER_GUIDE.md`, `packages/sandboxd/spec/process/v1/process.proto`,
  `sandbox-router/authz/authorizer.go`,
  `clients/python/agentic-sandbox-client/k8s_agent_sandbox/commands/command_executor.py`,
  `examples/kata-gke-sandbox/README.md`, `examples/firecracker-sandbox/README.md`.
  <https://github.com/kubernetes-sigs/agent-sandbox>
- [Agent Sandbox overview](https://agent-sandbox.sigs.k8s.io/docs/getting_started/overview/) (read)
- [Kubernetes blog, 2026-03-20: Running Agents on Kubernetes with Agent Sandbox](https://kubernetes.io/blog/2026/03/20/running-agents-on-kubernetes-with-agent-sandbox) (only the introduction was retrieved)
- [Google Open Source blog: agent-sandbox launch](https://opensource.googleblog.com/2025/11/unleashing-autonomous-ai-agents-why-kubernetes-needs-a-new-standard-for-agent-execution.html) (search snippet only)
- [AWS: AgentCore runtime sessions](https://docs.aws.amazon.com/bedrock-agentcore/latest/devguide/runtime-sessions.md) (search snippet only)
- [AWS: AgentCore runtime security best practices](https://docs.aws.amazon.com/bedrock-agentcore/latest/devguide/runtime-security-best-practices.md) (search snippet only)
- Vendor comparisons, all secondary and each written by a competitor:
  [Morph](https://www.morphllm.com/comparisons/daytona-alternative),
  [Novita](https://blogs.novita.ai/best-ai-agent-sandboxes-2026/),
  [Upstash](https://upstash.com/blog/best-e2b-alternatives),
  [Northflank](https://northflank.com/blog/e2b-vs-modal),
  [DEV Community](https://dev.to/alexcloudstar/sandboxing-ai-generated-code-e2b-vs-vercel-sandbox-vs-modal-vs-daytona-in-2026-3c7g)
- Internal: `decisions/ADR-242*.md` (R2, R7, R8, §2, §3a, G-R11, Execution environment table),
  `src/backends/remote/README.md`, `docs/research/2026-10-23-google-agent-compute-engine.md`
