# Sandbox feature parity — what the market ships vs. what 008/010/025 commit to

Compiled: 2026-08-23. Companion to `docs/research/2026-08-23-microvm-sandbox-backend-landscape.md`
(which covers isolation-*technology* pluggability — E2B/Firecracker, Modal/gVisor, etc.). This file
is orthogonal: it surveys sandbox *feature surface* (what capabilities a caller gets, regardless of
which isolation tech is underneath) across production platforms, then cross-checks each area against
`008-Sandbox-and-Isolation.md`, `010-Python-Code-Interpreter.md`, and `025-Worktree-and-Virtual-
Filesystem.md` to separate: **(a) already specced and built**, **(b) specced but explicitly
deferred/scoped-out** (a deliberate decision, cited), or **(c) not mentioned in any of the three RFCs
at all** — a genuine blind spot, not a deferred decision. Feeds the same future ADR track as the
companion doc (pluggable custom-backend seam + possibly a microVM backend); does not itself propose
spec changes.

Surveyed platforms (sources fetched 2026-08-23 unless noted): E2B, Modal (Sandboxes), Daytona,
Cloudflare Sandboxes (Workers-based), OpenAI Code Interpreter, Anthropic's code execution tool,
Northflank (aggregator/2026-entrant, cited for concurrency/GPU data specifically). Replit's agent
sandbox was searched but has no public primary-source architecture doc — only third-party
commentary repeating "defense in depth, microVM-class isolation" without a features API reference;
excluded from the matrix rows below for that reason, cited once where relevant.

## Feature matrix

| Feature area | Market (concrete mechanism) | AgentEngine spec status |
|---|---|---|
| **Multi-language execution** | E2B: Python/JS/TS/Bash, selectable per sandbox. Modal/Daytona/Cloudflare: arbitrary, container-defined. | **(b) Deliberately scoped, not a gap.** 010 §1/§2: exactly two modes (Python interpreter/CodeAct, shell) on "one runtime, permanently" — the architectural argument (010 §2) explicitly rejects a second interpreter/runtime as a source of behavioral drift. Not "missing multi-language," a stated non-goal. |
| **Snapshot/pause/resume (fs + memory)** | E2B: real, ~4s/GiB pause, ~1s resume, both fs+RAM. Modal: fs snapshots (diff-based) built, memory snapshots early preview. Daytona: point-in-time snapshot incl. packages/deps. | **(b) built for `wasm` only; explicitly absent for `native-jail`.** 008 §6a: wasm gets real memory snapshot at quiescent points (G8-gate-tested); **`native-jail`'s row in the same table says "Lost; session resumes with a clean interpreter and shell" (008 §6a line 420, 010 §3a)** — stated plainly, not silently. **Consequential**: `native-jail` is the profile Python/CodeAct actually run under in practice (per this session's earlier Finding O), so the profile competitors most differentiate on (fast full-state pause/resume) is exactly the one AgentEngine's own spec says loses interpreter state today. Worktree files still persist either way (025) — only in-memory interpreter/shell state is the gap. |
| **Warm-pool / pre-warmed sandboxes** | Core scaling lever for Modal (`min_containers`/`scaledown_window`/`buffer_containers`), Daytona (`SandboxWarmPoolService`, sub-second claims from a pool), Northflank (100k concurrent sandboxes in 24s from cold, explicitly pool-driven). | **(c) Blind spot.** Zero mention across all three RFCs (grepped "warm pool"/"pre-warm"/"cold-start budget" — no hits). `008 §2`'s `ProfileTraits.cold_start` is a **declared, static classification** (`zero`/`microseconds_to_low_ms`/`milliseconds`/`network_dependent`) used only for `Strict` backend ranking — there is no pooling/pre-provisioning *mechanism* anywhere the trait could feed into. |
| **Persistent filesystem independent of compute lifecycle** | E2B/Modal/Daytona/Cloudflare all separate durable storage from compute. OpenAI is the outlier: **no persistent filesystem at all** — container destroyed, `/mnt/data` gone, session-scoped only. | **(a) Built, and stronger than most competitors' framing.** 025's whole design point + 010 §4: "files survive everything the sandbox does not: destruction, passivation, process restart, node migration, profile change." Ahead of OpenAI's model; matches E2B/Modal/Daytona's separation-of-concerns. |
| **Runtime package installation** (pip/npm/apt *during* execution, agent-initiated) | Increasingly permitted with policy control: OpenAI's containers can now run bash/pip/npm install live (2026-01); Anthropic gates it behind an organization-level network-egress toggle (block all / approved package managers only / approved + extra domains) — an operator switch, not silently open. E2B instead does this at **template-build time**, not execution time (below). | **(b) Deliberately closed, cited explicitly.** 010 §232: *"`pip install` from model-generated code is not a capability that exists unless the operator [grants it]"* — closed-allowlist-by-construction (008 §1b layer 1), `preinstalled` image is the only default (010 §5/§10 Q1). This is the correct call under I3 (model output must never widen its own capability) but is a real, worth-naming product gap against where competitors are trending in 2026: Anthropic's shape — an **operator-set policy toggle that permits package-manager network access without permitting arbitrary code to expand its own grant** — is structurally compatible with I2/I3 (the grant is still operator-decided, not model-decided) and is not currently designed here. Worth flagging as a concrete, I3-safe feature a future revision could add without violating the locked invariant. |
| **Custom base images / templates a consumer-dev defines** | E2B (`template init`/Dockerfile chaining, `.pip_install()`/`.apt_install()`), Daytona (build snapshot from Dockerfile/image) both have a first-class, documented build workflow. | **(c) Borderline blind spot.** The underlying primitive exists (008 §1b layer 1's "granted package policy," set "at embedding time") but no RFC documents an actual authoring workflow analogous to `e2b template build` — same gap *shape* as the already-known Finding C from `docs/planning/2026-08-22-component-role-audit-tracker.md` (a real mechanism with no worked example for a third-party integrator). |
| **Exposed network ports / preview URLs** (inbound — sandbox serves an app reachable from outside) | Cloudflare Sandboxes' headline feature (`expose_port`, unique proxied HTTPS preview URL, token-scoped). E2B: `get host address for a sandbox port`, connect via HTTP/WebSocket from outside. Daytona/Modal: comparable port-forwarding primitives. | **(c) Blind spot, and a consequential one.** Grepped "expose.*port\|preview URL\|inbound\|reachable from outside" across all three RFCs — no hits. `NetPolicy` (008 §2) is defined purely as **outbound** egress control ("deny-all default; allowlist of host:port:scheme," 008 line 197) — there is no vocabulary anywhere for the opposite direction (host/outside reaching *into* a running sandbox). This isn't obviously an oversight — AgentEngine also does not implement HTTP networking itself (CLAUDE.md locked decision: host owns the socket) — but that decision covers the *engine's own* inbound MCP/A2A/AG-UI surface, not "does a sandboxed guest process get to bind a port a host could proxy." No RFC states which of these two framings applies to a guest-run dev server, which is exactly the "build and preview a web app" workflow several competitors ship as a named feature. |
| **Outbound network policy granularity** | E2B: `NetworkConfig.AllowOut` — IPs, CIDRs, wildcarded domains (`*.example.com`), HTTP/TLS-only domain filtering, hot-swappable without restart. Anthropic: org-level toggle + explicit domain list. | **(a) Built, comparable or stronger.** `NetPolicy{host:port:scheme allowlist}` (008 §2), host-mediated egress proxy with DNS-rebinding defense and RFC1918/link-local/metadata-range blocking (008 §4, `ADR-011-first-party-egress-proxy.md`, cross-checked against `docs/research/2026-08-05-ssrf-dns-rebinding-defense.md`) — this area was already independently confirmed ahead of the Cloudflare "Computer" product's egress story in the 2026-08-06 comparison doc. |
| **Streaming stdout/stderr (live, not buffered-only)** | Modal: `StreamReader`, multiplexed stdout/stderr. Cloudflare: live output over the SDK. | **(a) Built.** 010's own Q3, resolved 2026-08-04: ordinary `ToolCallDelta` streaming (006 §6a), same mechanism as any other long-running tool, projected onto MCP `notifications/progress` and AG-UI tool-call chunks (013 §3) — plus replay fidelity at full chunk-sequence granularity, a stronger recording guarantee than any competitor doc surveyed states. |
| **Background/long-running processes surviving the triggering call** | Cloudflare: explicit background-process lifecycle API (start/inspect/terminate, live URLs). Daytona: comparable. | **(a) Built.** 010's own Q6, resolved: `Backgroundable`-declared tool + `background_task()`, gated by `Background<max_concurrent>` capability (007 §3), resource use charged at pipeline step 10, agent lists/kills via `list_standing_effects`/`cancel_standing_effect` — a real, general mechanism (shared with scheduled wakeups/watches), not shell-specific. Survival across passivation explicitly deferred to "the sandbox profile's own properties" — i.e., inherits the same `native-jail` snapshot gap noted above. |
| **File upload/download API** (bytes in/out without going through code execution) | E2B: presigned-URL-style upload (`multipart/form-data` POST) and download endpoints. Anthropic: Files API — upload once, get `file_id`, reference across calls without re-encoding. | **(a) built as a primitive, (c) undocumented as a caller-facing workflow.** 025/010 §4: inputs are **mounted** via `FsRead`/`FsWrite` capabilities scoped to subtrees, never pasted into the prompt — architecturally a stronger, capability-gated version of "get bytes in/out." But since AgentEngine doesn't own HTTP transport (locked decision), there is no documented pattern for how a host actually exposes an upload/download *endpoint* backed by this mount primitive — same undocumented-integration-workflow gap shape as the template-build row above. |
| **GPU access** | Modal: T4 through B200+, selectable per sandbox. Daytona/Northflank: dedicated GPU sandbox tier. | **(b) Explicitly out of scope, not a gap.** 008 Q5 / 010 Q4, both resolved 2026-08-04: GPU passthrough into `wasm`/`native-jail` is "exactly the kind of second local isolation technology this decision declines to build" — pushed to the `remote` profile against infrastructure that already solves GPU device access (K8s device plugins, cloud GPU instances). A clean, cited decision, not silence. |
| **Per-sandbox resource limits, user-configurable** | Modal: CPU/memory/timeout configurable per sandbox, soft/hard limit split. Daytona: resource config + volumes. | **(a) Built.** `ResourceLimits{cpu_ms, wall_ms, memory_bytes, pids, fds, disk_bytes, net_bytes, output_bytes}` is a field on `SandboxSpec` (008 §2) — a single struct enforced identically across backends, already independently confirmed (2026-08-06 Cloudflare comparison, §7) to be **more developed** than Cloudflare Computer's own ad hoc per-backend limits. |
| **Multiple concurrent sandboxes / throughput at scale** | Northflank: 100,000 concurrent live sandboxes in 24s (P99 allocate 566ms) as a headline scaling claim; explicitly benchmarked against E2B/Modal. Modal: framed around RL-training workloads needing many parallel sandboxes. | **(c) Different design center, not addressed as a scaling axis.** 010 §3a: **one `ExecState`/sandbox instance per *session***, by design (the "one machine" guarantee). 025 §3 supports many *agents* sharing/branching one worktree — a collaboration-concurrency story, not a "spin up N ephemeral sandboxes for a batch job" throughput story. Not necessarily wrong for AgentEngine's stated product shape (conversational agent apps, not RL-training infra) — but no RFC states this scope boundary explicitly the way the GPU exclusion does, so a reader can't currently tell "not needed" from "not yet considered." |
| **Observability (logs/metrics/tracing exposed to the integrator)** | Modal: dashboard + Datadog export. Northflank: per-workload metrics. | **(a) Built, structured.** 008 §8 is a real section: bytes in/out, egress hosts contacted, capabilities used, outcome class; profile identity carried in metrics; backend-downgrade events land in startup diagnostics + run trace + metrics, proven by a negative test (008 §8/§9). This maps onto OpenTelemetry GenAI per this project's stated protocol commitments — a more structural guarantee than "dashboard + optional export." |
| **SDK/language binding breadth** | E2B: Python/TypeScript/Go SDKs over a hosted REST API. Modal/Daytona/Cloudflare: same shape — hosted service, multi-language client SDKs. | **N/A — structurally different product, not a gap to close.** AgentEngine is an embeddable C++23 engine a host links against, not a hosted API; CLAUDE.md's locked decision fixes v1 authoring to C++ CRTP + declarative YAML/JSON, with Python/.NET bindings explicitly deferred. Naming this as a framing note, not a matrix cell to "fix" — comparing SDK breadth 1:1 against hosted-SaaS competitors would be comparing different product categories. |

## Headline shape

Of the 15 comparable rows: **6 built and holding up well or ahead** (persistent filesystem, outbound
network policy, streaming, background processes, resource limits, observability), **4 explicitly
deferred/scoped by a cited decision** (multi-language, GPU, runtime package install, native-jail
snapshot/resume), **4 genuine blind spots** (warm-pool, exposed inbound ports/preview URLs, template
build workflow, upload/download workflow — two of the four are "primitive exists, workflow
undocumented" rather than "nothing exists at all"), plus 1 not-applicable framing note (SDK breadth)
and 1 real scope-ambiguity (concurrent-sandbox throughput, not clearly in- or out-of-scope).

## Most consequential gaps, prioritized

1. **`native-jail` has no snapshot/pause/resume of interpreter state** (008 §6a) — the profile
   Python/CodeAct actually run under today loses exactly the state competitors' headline
   fast-pause/resume features preserve. `wasm` already has the real mechanism (G8-gate-tested); the
   gap is that the profile matching real usage doesn't share it, and 008 §6a's own comment gives no
   indication this is being tracked as future work versus accepted as permanent.
2. **No inbound port-exposure / preview-URL vocabulary anywhere** — `NetPolicy` only models egress.
   Given "agent builds and live-previews a web app" is now a named, shipped feature on at least three
   surveyed platforms (Cloudflare, E2B, Daytona), this is a real product-capability gap, and it also
   raises an undesigned I2/I3 question (inbound exposure is a new capability *class*, not a variant of
   existing `NetOut`) that deserves a deliberate design pass rather than an ad hoc bolt-on later.
3. **No warm-pool/pre-provisioning mechanism**, despite `ProfileTraits.cold_start_class` already being
   a declared axis that begs for one — every scaling-focused competitor treats this as core
   infrastructure, not an afterthought.
4. **Runtime package installation is closed by default with no policy-gated middle ground** — the
   current default (`preinstalled` only) is I3-correct, but Anthropic's 2026 shape (operator toggles
   package-manager network access without touching the model's own capability grant) shows a policy
   design exists that stays I3-compliant while closing this gap; worth a real design pass rather than
   leaving `pip install` binary-closed.
5. **Two "primitive exists, integrator workflow doesn't" gaps** (custom template/image build,
   upload/download over a host-owned transport) — same gap *shape* the 2026-08-22 component-role-audit
   tracker already named for `ContextProvider` (Finding C): the mechanism is real, nothing shows a
   third-party how to use it end to end.

## Sources

E2B: `sandbox/persistence`, `quickstart/upload-download-files`, `sdk-reference` (Python/JS/Go),
`sandbox/internet-access`, `template/base-image`, "Introducing Build System 2.0" blog
(e2b.dev, fetched 2026-08-23). Modal: `guide/sandboxes`, `guide/sandbox-snapshots`,
`guide/memory-snapshots`, `guide/sandbox-networking`, `guide/sandbox-files`, `guide/sandbox-resources`,
`guide/resources`, `guide/cold-start`, `reference/modal.Sandbox`, "Best Sandboxes for Streaming Code
Execution" (modal.com, fetched 2026-08-23). Daytona: `docs/en/snapshots`, DeepWiki
`daytonaio/daytona` §2.1/§4.1 (daytona.io, deepwiki.com, fetched 2026-08-23). Cloudflare:
`developers.cloudflare.com/sandbox/` overview, `concepts/preview-urls`, `api/ports`, GitHub
`cloudflare/sandbox-sdk` releases, DeepWiki §3.5 (fetched 2026-08-23). OpenAI: "Code Interpreter File
Storage" (fast.io), "ChatGPT Containers can now run bash, pip/npm install packages" (simonwillison.net,
2026-01-26), "OpenAI Code Interpreter — Containers, Files, and Practical Patterns" (team400.ai), all
fetched 2026-08-23 — no primary OpenAI architecture doc found, noted as secondary-source-only.
Anthropic: `platform.claude.com/docs/en/build-with-claude/files` (Files API),
`platform.claude.com/docs/en/agents-and-tools/tool-use/code-execution-tool` (fetched 2026-08-23).
Northflank: "GPU sandboxes," "Top AI sandbox platforms," "Best platforms for high concurrency sandbox
environments" (northflank.com, fetched 2026-08-23, incl. the ComputeSDK 2026 Scale Invitational
concurrency benchmark). Replit: searched, no primary architecture doc found — excluded from the
matrix, third-party commentary only (modal.com/resources, augmentcode.com, fetched 2026-08-23).
Internal: `008-Sandbox-and-Isolation.md`, `010-Python-Code-Interpreter.md`,
`025-Worktree-and-Virtual-Filesystem.md` (repo root, current on-disk text, read 2026-08-23);
`docs/research/2026-08-23-microvm-sandbox-backend-landscape.md` and
`docs/planning/2026-08-22-component-role-audit-tracker.md` (cross-referenced, not re-derived).
