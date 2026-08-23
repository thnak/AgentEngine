# MicroVM landscape and competitor sandbox-backend pluggability — does anyone else do "ship one, let the deployer swap it"?

Compiled: 2026-08-23. Feeds a possible future ADR on the `SandboxBackend` extension seam (008 §2a,
already real: `include/agentengine/sandbox/sandbox.hpp`'s `SandboxBackend` C++20 concept, satisfied
today by `NativeJailBackend`/`LinuxNativeJailBackend`/`WasmBackend`, pluggable via `SandboxProfile<P>`
at `register_agent<A>()` time). Scope: (1) re-verify the 2026-08-04 Windows-microVM finding for
currency, (2) survey how competing agent/code-execution platforms structure their sandbox backend and
whether any exposes a genuine bring-your-own-isolation-tech seam, (3) survey Linux-side projects a
consumer-dev could realistically supply as a custom `SandboxBackend` today. This does **not** revisit
whether AgentEngine itself should ship a `microvm` profile — that's closed (CLAUDE.md, `008 §1`,
`decisions/README.md`) — it grounds the *plugin seam* direction with current external evidence.

## 1. Windows-hosted microVM isolation — re-verified, unchanged since 2026-08-04

No new entrant found. Re-checking each claim in
`docs/research/2026-microvm-windows-portability.md`:

- **Firecracker**: still explicitly Linux-host-only. Its own `FAQ.md` (github.com/firecracker-microvm/firecracker,
  fetched 2026-08-23) is unchanged on this point — no Windows/BIOS/UEFI guest or host support, KVM
  dependency is structural, not a missing build target.
- **`firecracker-win`** (the unofficial Hyper-V-hosted fork, github.com/howard0su/firecracker-win,
  fetched 2026-08-23): still **4 stars, 0 forks, no visible tagged releases or maintenance cadence**
  — identical numbers to the 2026-08-04 check, i.e. genuinely dormant, not merely under-counted by a
  stale snapshot.
- **Cloud Hypervisor**: `docs/windows.md` still documents Windows only as a **guest** on a Linux+KVM
  host — no reversal.
- **Hyperlight**: still described by its own maintainers as experimental. New corroborating data
  point not in the 2026-08-04 record: Microsoft's own Agent Framework docs (`learn.microsoft.com`,
  "Local Code Interpreter in .NET... Hyperlight," fetched 2026-08-23) list Hyperlight as one
  documented option for a *local* custom code-interpreter sandbox on .NET — delivered as a native
  NuGet package, explicitly positioned as a lightweight alternative to a full VM for running
  LLM-generated code, not as MAF's own default. This is Microsoft citing its own ecosystem's
  experimental-microVM technology as a customization option, not a hardened built-in — consistent
  with, not contradicting, the existing "not production-grade" finding.
- **New this pass**: `CVE-2026-1386` — a real symlink-based arbitrary-host-file-overwrite privilege
  escalation in Firecracker's own `jailer` component (AWS Security Bulletin 2026-003, SentinelOne
  vulnerability database, both fetched 2026-08-23), affecting the jailer's directory-initialization
  step when run as root. Linux-only (jailer has no Windows relevance), but worth recording as current
  evidence that even the most production-proven microVM stack has an active 2026 CVE in exactly the
  privileged-setup code path a consumer-dev would have to operate correctly if they supplied a raw
  Firecracker+jailer `SandboxBackend` themselves.

**Conclusion: no update needed to 008 §1 or the 2026-08-04 record.** The Windows-microVM gap is
structural (KVM dependency), not a maintenance-lag gap that narrows with time.

## 2. Competitor sandbox backends — is any of them actually pluggable at the isolation-technology level?

The question that matters for this project's planned direction ("ship one native backend, let a
consumer-dev supply their own strategy") is narrower than "what isolation tech does X use" — it's
**does X expose a seam where the isolation technology itself can be swapped**, the way
`SandboxProfile<P>` accepts any type satisfying `SandboxBackend`. Surveyed seven platforms; sources
fetched 2026-08-23 unless noted.

| Platform | Isolation tech (fixed default) | Bring-your-own-backend seam? |
|---|---|---|
| **Microsoft Agent Framework** | Built-in `HostedCodeInterpreterTool` → Microsoft-managed, VM-isolated sandbox on Azure Container Apps dynamic sessions (Microsoft Learn, "Code Interpreter") | **Indirect, protocol-level, not a typed interface.** MAF's own "Configure a custom code interpreter for agents" doc names two escape hatches: (a) stand up your own code-interpreter service behind an MCP server and point a `ToolboxMcpClient` at it, or (b) hand-write a different tool/agent implementation entirely (e.g. the Hyperlight-backed local .NET sandbox pattern above). Neither is "implement one small concept and hand it to a template parameter" — both mean re-implementing or re-hosting the whole tool surface, not swapping an isolation-strategy object under a fixed contract. |
| **E2B** | Forked Firecracker microVM, fixed | **No isolation-tech swap.** Apache-2.0 self-hosted OSS core lets a deployer run E2B's *own* orchestrator+Firecracker stack on their own cloud/bare-metal (AWS/GCP/Azure/bare Linux) — that's "bring your own infrastructure for our one backend," not "bring your own backend." |
| **Modal Sandboxes** | gVisor, fixed | No public backend-swap interface found in current docs. |
| **Daytona** | Container by default; VM sandbox (dedicated Linux/Windows VM) and GPU sandbox as separate *first-party* offerings, gVisor layer for the container tier | Closer to AgentEngine's own multi-*profile* shape (pick among several first-party options) than to a custom-backend seam — no evidence of a third-party-suppliable isolation backend. Also: **moved closed-source June 2026**, which forecloses self-hosted extension going forward regardless. |
| **Cloudflare Sandboxes** | Two first-party tiers: V8-isolate "Dynamic Workers" (ephemeral, ~100x faster/lighter) vs. full-container Sandboxes (persistent, complete Linux env) | Same shape as Daytona — a config choice between two Cloudflare-built backends, not a custom-backend plugin point. |
| **OpenAI Code Interpreter** | gVisor-backed container (`user_machine` FastAPI service), fixed | Fully opaque, hosted, no customization surface documented. |
| **Anthropic code execution tool** | Hosted: Anthropic-managed sandboxed container, fixed. **Self-hosted sandboxes** (`platform.claude.com/docs/managed-agents/self-hosted-sandboxes`): orchestration stays with Anthropic, but *tool execution* — filesystem, process spawn, network egress — moves into infrastructure the deployer controls, enforced by ASRT (Anthropic Sandbox Runtime: OS-level, `bubblewrap` on Linux / Seatbelt on macOS, no container/VM) | **Closest external analog found, but still not a technology-swap seam.** The deployer relocates *where* the fixed isolation mechanism (ASRT) runs, not *what* mechanism enforces the boundary — ASRT itself isn't user-suppliable. |

**Finding: no competitor surveyed exposes a genuine "swap the isolation technology itself behind a
typed interface" seam.** Every platform either (a) ships one fixed backend and, at most, lets a
deployer relocate *infrastructure* for that same fixed backend (E2B self-host, Anthropic self-hosted
sandboxes), or (b) offers a small fixed menu of first-party backends as a config choice (Daytona,
Cloudflare), or (c) requires re-implementing the entire tool/service around a different backend via a
heavier protocol indirection (MAF's MCP-toolbox escape hatch) rather than conforming a narrow
interface. **AgentEngine's `SandboxBackend` concept — "any type satisfying three methods + traits,
selected as a compile-time template parameter" — is more granular and lower-ceremony than anything
found across these seven platforms.** This should be framed in any future ADR as a genuine
differentiator this project already built (008 §2a, since M2), not as catching up to prior art —
there isn't prior art at this grain among the platforms surveyed. The practical implication for the
planned "we ship native-jail, consumer-dev supplies microVM" direction: there is no existing
integration template to crib from for the *interface shape*; there's real prior art only for *which
Linux isolation technologies* a supplied backend might wrap (§3).

## 3. What a consumer-dev-supplied Linux `SandboxBackend` would realistically wrap

Since Windows-hosted microVM is closed (§1), a custom backend for genuine hardware-level isolation is
a Linux-only proposition — consistent with 008 §1's existing "the `remote` profile targets
Linux/KVM infrastructure" fallback. Three real candidates, with current tradeoffs (sources: Northflank's
Kata-vs-gVisor-vs-Firecracker comparison, Modal/Northflank sandboxing surveys, all fetched 2026-08-23):

- **Kata Containers** — VM-per-workload via KVM, hardware-enforced boundary, full GPU passthrough
  (one GPU per pod, unlike gVisor's `nvproxy`), selectable runtime class (`kata-clh` for
  cloud-hypervisor, `kata-qemu`). Attack surface is the hypervisor + virtio devices. Startup ~200-300ms
  (4-6x slower than a plain container), near-native runtime performance after boot.
- **gVisor (`runsc`)** — user-space kernel intercepting syscalls, no hardware VM boundary; what Modal
  and (per the ITNEXT/InfoQ pieces above) OpenAI's own Code Interpreter both use in production today.
  Weaker isolation class than a VM boundary but materially lower overhead — "the middle ground," per
  every comparison surveyed.
- **Firecracker + `jailer` directly** (not through E2B's fork) — strongest of the three on paper
  (≤125ms boot, <5 MiB overhead per the existing standards-landscape record §8), but real, current
  operational cost: manual TAP/iptables/routing management (no CNI-plugin maturity equivalent to
  Docker), networking setup becoming the dominant bottleneck at scale (one source cites startup
  latency degrading up to 263% around ~400 concurrent starts), warm-pool economics to hide cold-start
  cost, and the active `CVE-2026-1386` jailer privilege-escalation bug (§1) landing in exactly the
  privileged host-setup path a self-integrator would have to run correctly.

These map directly onto the `ProfileTraits{strength, platform_mask, cold_start_class}` fields the
`SandboxBackend` concept already requires (`sandbox.hpp`) — a consumer-dev choosing among these three
is choosing a point on the same strength/cold-start axis `NativeJailBackend`'s own
`traits{strength=50, ...}` already occupies a place on, just further out. No RFC/ADR change is implied
by this section; it's evidence for whoever eventually writes the "supplying a custom `SandboxBackend`"
how-to guide (which does not exist yet — same gap class as Finding C in the 2026-08-22 component-role
audit tracker: the mechanism is real, the worked example for a third-party isn't written).

## Sources

Firecracker `FAQ.md`, `docs/design.md`, `docs/prod-host-setup.md`
(github.com/firecracker-microvm/firecracker, fetched 2026-08-23); `howard0su/firecracker-win`
(github.com, fetched 2026-08-23); AWS Security Bulletin 2026-003 / SentinelOne CVE-2026-1386 entry
(fetched 2026-08-23); Microsoft Agent Framework docs — "Code Interpreter," "Configure a custom code
interpreter for agents," "Providers Overview" (learn.microsoft.com, fetched 2026-08-23);
"Local Code Interpreter in .NET: Mastering CodeAct and Hyperlight" (candede.com, fetched 2026-08-23);
E2B docs and `api-evangelist/e2b-dev` mirror (e2b.dev, github.com, fetched 2026-08-23); Modal Sandboxes
resource pages (modal.com, fetched 2026-08-23); Daytona docs and Northflank Daytona-vs-Modal/E2B
comparisons (daytona.io, northflank.com, fetched 2026-08-23); Cloudflare Sandbox SDK docs, Cloudflare
blog "Sandboxing AI agents, 100x faster" and "Agents have their own computers with Sandboxes GA"
(developers.cloudflare.com, blog.cloudflare.com, fetched 2026-08-23); OpenAI Code Interpreter
architecture write-up (itnext.io, fetched 2026-08-23, secondary source — no primary OpenAI
architecture doc found); Anthropic "Code execution tool" and "Self-hosted sandboxes" docs
(docs.anthropic.com, platform.claude.com, fetched 2026-08-23), `anthropic-experimental/sandbox-runtime`
README (github.com, fetched 2026-08-23); Northflank "Kata Containers vs Firecracker vs gVisor" and
"How to sandbox AI agents in 2026" (northflank.com, fetched 2026-08-23).
