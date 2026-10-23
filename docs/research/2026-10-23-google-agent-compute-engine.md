# Google Agent Compute Engine (ACE) vs. AgentEngine's execution options

Compiled: 2026-10-23. Question: Google announced ACE on 2026-10-22; how much of it do we already
support, and does it change any `ADR-242` decision? Not an ADR; no spec change is proposed here.

## Evidence quality

Everything about ACE below comes from press coverage of the announcement. **The primary Google
blog post was not read.** The New Stack page returned only its headline when fetched. The Decoder
article explicitly omits the hypervisor, the secrets model, the API surface, pricing, MCP or
framework integration and the resource limits, and links to Google's deep-dive post for them.
Performance and density figures are Google's own and not independently reproduced.
Re-verify against Google's post before relying on any row.

## What was announced (as reported)

- Announced 2026-10-22 at the Linux Foundation's AGNTCon+MCPCon, San Jose. Already runs Gemini's
  agents; customer preview "in the coming months" (one outlet: later this year).
- One hardware-isolated microVM per agent, "secured by Google's Titanium chip"; one outlet reports
  Cloud Hypervisor as the VMM. (Single-source claim.)
- No secrets at rest in the VM; short-lived per-session identity. (Reported by one outlet; the
  Decoder does not mention it.)
- Idle VMs pause automatically; memory is shared between VMs. Google's claims: about 10x agents
  per server, and suspend/resume under 500 ms in a 200,000-agent test.
- Agent Substrate: Google open-sourced a Kubernetes-native runtime on GKE.
- Governance: token tracking, model routing, third-party identity providers (Ping Identity,
  SailPoint), extended to Cloud Run, GKE and managed endpoints.

## Mapping onto what we ship

Status column is from `decisions/ADR-242` "Execution environment" table, not from marketing.

| ACE capability | AgentEngine counterpart | Honest status |
|---|---|---|
| microVM per agent | `microvm` (Kata), `KataBackend` | Works, live-verified 2026-10-20. Same-kernel-escape class avoided (hardware VM boundary). |
| Hardware offload isolation (Titanium) | none | Not applicable: we run on the host's KVM; no offload silicon. |
| No secrets at rest, short-lived identity | I2 enforced at the VM boundary from the host (R3 rules 1-7); `AgentEngineSecretStore`; per-request authority in `agentengine_server` | Comparable intent. Not equivalent: we do not mint cloud workload identities. |
| Pause idle VMs | none | Planned only: §6 step 3 ("pause between runs"). Today guest processes keep running between calls. |
| Memory sharing across VMs / density | none | Measured ~170 MB idle to 2 GiB busy per VM, no admission cap, ~6 s cold start. A large gap vs. the claimed figures. |
| Idle-VM reaper | none | ADR-242: no reaper for idle VMs or crash leftovers (stale netns found 2026-10-20). |
| Token tracking | I8 budgets | Ours is per-session enforcement in the engine; ACE's is a fleet view. |
| Model routing | none | Out of scope by design (engine is single-process, no fleet framing). |
| Third-party IdP integration | R7 identity-to-authority policy | The policy is the developer's root of trust, not a seam. |
| Agent Substrate on GKE | none | We have no cluster scheduler and no plan for one. |

## Where the overlap really is

ACE competes with the **execution-environment half** of AgentEngine (the `microvm` and `remote`
rows), not with the agent, session, tool and workflow layers, the protocol surfaces (MCP, A2A,
AG-UI), or the invariants. It is a hosted service; AgentEngine is an embedded library whose
`SandboxBackend` seam is exactly where a hosted sandbox plugs in.

## Implications for ADR-242

1. **`remote` is the natural ACE integration point**, and it is currently a stub with "Not
   scheduled". ACE preview does not change that decision by itself: ADR-242 §3a requires an
   enforcement test with a positive control before any "engine enforces" cell, which cannot be
   written against a service we cannot reach.
2. **Do not claim parity.** ACE's pause, density and cold-start story is the part ours lacks;
   §6 step 3 is the planned work, and it should be sized from our measured numbers, not Google's.
3. **A `remote` backend must keep I2 and I3.** A remote microVM does not relax them: the
   `SandboxSpec` mounts still need covering `cap::SandboxMount` grants (ADR-244 slice 2a), and
   guest output is still data. Any cloud-minted identity must be a developer-chosen option, not an
   implicit one (ADR-242 §2).
4. **No action now.** Revisit when preview opens and Google's deep-dive post is readable.

## Open questions

- Which VMM does ACE actually use, and is the "no secrets at rest" claim in Google's own post?
- Does ACE expose an API a `SandboxBackend` can drive (create, exec, mount, destroy), or only a
  managed agent runtime? If the latter, it is a `Runtime`/hosting choice, not a backend.
- Does ACE support host-bind mounts, or an equivalent of the engine-owned durable worktree (D4)?
  ADR-242 R8 requires every backend to host the worktree.
- Egress control: can an allowlist be enforced at the host bridge as ADR-240 does for Kata?

## Sources

- [The Decoder: Google's Agent Compute Engine gives every Gemini agent its own hardware-isolated micro virtual machine](https://the-decoder.com/googles-agent-compute-engine-gives-every-gemini-agent-its-own-hardware-isolated-micro-virtual-machine/) (read)
- [The New Stack: Google turns the infrastructure behind Gemini's agents into a cloud service](https://thenewstack.io/google-agent-compute-engine/) (headline only; body not retrieved)
- [SiliconANGLE: Google Cloud introduces Agent Compute Engine to host AI agents](https://siliconangle.com/2026/10/22/google-cloud-introduces-agent-compute-engine-to-host-ai-agents/) (search snippet only)
- [daily.dev: Introducing Agent Compute Engine and expanding agent governance](https://daily.dev/posts/introducing-agent-compute-engine-and-expanding-agent-governance-cwyxhkzx1) (search snippet only)
- Internal: `decisions/ADR-242*.md` (Execution environment table, R2, R3, R8, §3a, §6)
