# Research record — agent framework feature comparison against AgentEngine

**Compiled:** 2026-08-08 · **Status:** dated snapshot, not a living document

Companion to `2026-08-08-agent-framework-feature-trends.md` (the pure external landscape survey).
This record takes each feature from that survey and asks, for AgentEngine specifically: is there
an equivalent mechanism, is it spec'd or built, and how does it differ factually (no
better/worse judgments — this is a design-comparison record, not a critique). Every entry is
grounded in a direct read of the cited RFC section and cross-checked against build status via
`docs/planning/v1-implementation-roadmap.md` and the relevant milestone breakdown doc — **RFC
status alone (all 30 are "Reviewed") never implies build status**; those are tracked separately.

As of this compile: Milestones 0-5 complete, Milestone 6 (multi-agent orchestration, RFC 014) in
progress through Phase F (graph engine, all eight patterns, failure handling, checkpoint/resume/
time-travel), Milestones 7-9 (protocol conformance — 011/012/013/015; safety/observability/perf —
016/017/022/023; hosting/platform/bulk-data — 020/021/028) not started.

---

## 1. LangChain / LangGraph

| Feature | AgentEngine mechanism | Status | How it differs |
|---|---|---|---|
| `create_agent()` + middleware (node/wrap hooks, built-in HITL/PII/retry middleware) | **002 §5** — compile-time CRTP middleware chain (run/turn/model-call/tool-call hook points); example `RedactPii{before_model, after_model}` | Built (M2, complete) | Hooks are compile-time types (zero-cost, ADR-007 objdump-parity gate), not runtime functions; middleware is barred by I2 from widening capabilities. No pre-built HITL/summarization/retry middleware library is named — only the mechanism plus one example. |
| Checkpointing (full state snapshot per superstep, keyed by thread id, pluggable backend) | **019 §1** (turn/session checkpoints) + **014 §5** (workflow superstep checkpoints), both on Quark's `Store` seam (005 §2), keyed by `run_id` | Built — session-level since M4; workflow-level landed M6 Phase F (20/20 kill-and-resume proofs) | `Store` is a compile-time concept the actor is templated over, not a runtime-selectable backend list — no equivalent of choosing Postgres vs. SQLite at runtime; backend choice is a build-time seam. |
| `interrupt()` / `Command(resume=...)` (node re-executes from start on resume) | **014 §4** request port (`InputRequired` + suspend) + **001 §2** `Interaction` record, projected at **013 §2.2** | Engine half built (M6 Phase E); external protocol projections (013) not built (M7) | LangGraph resumes by re-executing the interrupted node from its start. AgentEngine's actor model forces the opposite: `passivate()` drains in-flight work but never interrupts mid-handler, so a suspended run **returns** rather than parks; `ResumeWorkflow` continues from the exact stopped point, never from the top of the node. |
| `BaseStore` (cross-thread, semantic search) vs. thread-scoped checkpointer | **029** (Memory System, principal-scoped, cross-session by construction) vs. **005 §1** (session state, thread-scoped) | Built (both M4) | Default retrieval has no embedding dependency — deterministic `salience × recency × tag overlap`, replayable (I5). Semantic/vector search is an opt-in plugin, the inverse default of LangGraph's vector-first store. |
| Streaming modes (`values`/`updates`/`messages`/`custom`, interleavable) | **013 §1** — one internal typed event stream, projected per protocol surface | Not built — 013 is M7 | No caller-selectable "mode": one canonical stream is projected differently per surface, rather than a parameter the caller passes. |
| Supervisor / Swarm / Hierarchical | **014 §3**'s eight-row pattern table (Sequential, Concurrent, Handoff, Group chat/debate, Planner/Magentic, Map-reduce, Router, Reflection) — "configurations of the graph, not separate subsystems" | Built, all eight (M6 Phase C) | Handoff is a statically typed, graph-declared edge set (I3-tested — a model can't route to an unwired label), unlike Swarm's dynamically callable handoff tools with open-ended routing. No distinct "Hierarchical" row — achieved by nesting a `sub_workflow` node, i.e. composition rather than a template. |
| MCP integration (multi-server tool conversion) | **011 §3.1** — multi-server discovery mapped into the 006 §2 tool plane | Spec'd only — M7 | Spec includes JSON Schema 2020-12 validation, `$ref`-fetch hardening, and cache-rule handling beyond what's asked here. |
| A2A support (thinner in LangGraph core) | **012** — full server (§2) + client (§3) roles, explicit topology statement (§5) | Spec'd only — M7 | RFC specs A2A with the same weight as MCP (both roles, its own conformance-suite gate), not as a thinner add-on — though since neither is built yet, actual support is zero either way today. |
| Deep Agents / dynamic subagents (driver scripts orchestrating subagents, isolated context, virtual filesystem) | **026 §5** `agent` library (CodeAct itself) + `agent.spawn` + **025** worktree as the virtual filesystem | CodeAct/interpreter/worktree/`agent.tools`/`agent.files` real since M3; **`agent.spawn` itself not implemented** — named M3 close-out residual, blocked on a real nested-agent-run path with genuine capability attenuation (the depth-budget mechanism, `SpawnBudget`/ADR-006, is built; the spawn path isn't wired to it) | Design directly parallels Deep Agents' dynamic-subagents idea (driver script + isolated sub-context + shared filesystem), but the actual invocation path is the specific piece not yet wired. |
| Structured output (`ToolStrategy`/`ProviderStrategy`) | **003 §4** `OutputSchema<T>` — native / tool-shaped / parse-and-repair, selected by provider capability | Built (M1) | CodeAct-facing `agent.output` module was a named residual at M3 close, blocked on 003 §4's enforcement landing — not reverified here whether later work closed it. |

**Genuine gaps:** none — every LangChain/LangGraph feature checked maps to at least a spec'd
AgentEngine mechanism.

**Spec'd but not built:** 013 (streaming/UI protocol projections), 011 (MCP), 012 (A2A) — all M7;
`agent.spawn`'s invocation path (026 §5) — named residual since M3.

**Different by design:** compile-time CRTP middleware vs. runtime wrap functions; resume-from-exact-point
vs. re-run-the-node (forced by actor `passivate()` semantics); deterministic non-vector memory
retrieval as default vs. vector-first; statically typed graph edges for handoff vs. open-ended
handoff tools; one canonical projected event stream vs. caller-selectable streaming modes.

---

## 2. OpenAI Agents SDK / Responses API

| Feature | AgentEngine mechanism | Status | How it differs |
|---|---|---|---|
| Handoffs as `transfer_to_X` tool | **`Handoff<Writer>`** (002 §4.1, single hop) + the **Handoff pattern** (014 §3, structured multi-hop routing as a graph configuration) | Built — 014 §3's patterns including Handoff shipped M6 Phase C; 002 §4's single-hop form spec'd, not independently confirmed built | AgentEngine splits into two mechanisms by design (one-hop tool-shaped transfer vs. structured multi-hop graph process); the RFC tells authors to promote long handoff chains into a workflow rather than chain `Handoff<T>`s. `input_filter`'s equivalent is the target agent's own history policy, not a caller-supplied filter function. |
| Guardrails (parallel functions, `tripwire_triggered`, input/output split) | **017 §4** Filters — invoked at six declared points (`input`/`pre_model`/`post_model`/`tool_args`/`tool_result`/`output`) with verdicts `allow`/`annotate`/`redact`/`require_approval`/`deny`, under 017 §2's eight-layer defense table | Spec'd — 017 is M8, not started | Richer verdict set than a boolean tripwire (includes `require_approval`); explicit invariant "filters can deny, never grant" (I3). No first-party injection/jailbreak classifier ships — only a deterministic PII filter — a deliberate scope choice against "an open-ended quality-maintenance commitment." |
| Sessions/memory (pluggable persistent history) | **005 §2** session persistence (Quark `Store` seam, Snapshot/Event-sourced modes) + **029** Memory System (structured `MemoryItem`s) as two separate concepts | Built — M4 complete | AgentEngine separates raw turn/event history (005) from extracted/attributed memory (029); OpenAI's Sessions conflates both into one flat history layer. |
| Background mode (`background:true`, poll/resume) | **006 §6b** `Backgroundable`/`StandingEffect` + **019 §2** wake-condition table row "Local background task completion" | **Spec'd only, zero implementation** — confirmed by grep (no `Backgroundable`/`StandingEffect`/`background_task` symbol anywhere); assigned to **M7**, tracked in `docs/planning/backgroundable-standingeffect-gap.md` | Design intent matches closely (detach effect, resume run when done); AgentEngine unifies background tool calls, `schedule_wakeup`, and `watch_resource` under one `StandingEffect` handle with one kill/introspect surface, vs. OpenAI's response-specific `background:true` + `sequence_number` polling protocol. |
| Batch API (submit JSONL, poll, retrieve) | Resolved in **004 §8 Q1** to ride the same `Backgroundable` mechanism — "not a bespoke batch-tracking structure"; `ChatClientCapabilities::batch` bit exists, currently `false` everywhere | Spec'd only, blocked on the same M7 gap | AgentEngine refuses a second tracking structure by design — batch is one instance of existing async machinery, not a new subsystem. |
| Prompt caching incl. GPT-5.6 explicit-breakpoint mode | **004 §2** `prompt_caching` capability bit; resolved in **004 §8 Q2** — no portable cache-hint abstraction, each backend inserts vendor-specific breakpoints as internal translation logic at 005 §3's context-assembly segment boundaries | Built for Anthropic (M5) — `cache_control` at the two reachable segment boundaries, proven live over real TLS; `Usage.cache_write_tokens` added 2026-08-07 | No portable cross-vendor cache-control API by design — rejected as "one vendor's shape leaking onto every other backend." Finer-grained per-contributor breakpoints are a named, not-yet-built gap (`ChatRequest.messages` is already flattened by the time it reaches a `ChatClient`). OpenAI's explicit `prompt_cache_key` field is deliberately deferred — a live test against OpenRouter/llama-server was inconclusive (both accept a fabricated field with HTTP 200), and shipping a field with no gate that can fail violates the project's promotion-gate discipline. |
| MCP tool type (Responses-API-only, tools-only, remote servers) | **011** full client+server — §3.1 tools, §3.2 resources **and** prompts, §3.4 MRTR, §6 auth, §7 transport | Comprehensive spec (36 numbered sub-obligations under §3.1 alone); not built — M7 | Materially broader scope than OpenAI's tools-only Responses integration: covers resources and prompts too, both client and server roles, and an executable conformance-percentage gate rather than a supported-feature claim. |
| Structured outputs (`strict:true`, refusal field) | **003 §4** `OutputSchema<T>` — native/tool-shaped/parse-and-repair | Built (M5 — real per-backend translation, including forced `additionalProperties:false`, live-verified against llama.cpp's grammar decoder) | No distinct "refusal" content kind exists in 003's `Content` table (`Text\|Reasoning\|Image\|Audio\|Video\|File\|Data\|ToolCall\|ToolResult\|Error\|Custom`) — a real, narrow gap against OpenAI's dedicated `refusal` field; would presumably surface as `Error` or `Text` today. |
| Tracing (on by default, every event) | **016 §2** — seven named spans (`invoke_agent`, `chat`, `execute_tool`, plus AgentEngine-specific `sandbox_exec`/`plugin_call`/`workflow_step`/`context_assembly`), OTel GenAI-conformant, cross-process propagation into MCP `_meta` and A2A headers | Spec'd — M8, not started | Finer-grained by category than OpenAI's agent loop has analogs for (sandbox exec, plugin call, workflow superstep spans); explicitly separates audit from telemetry with off-by-default content capture — stricter than "on by default." |
| Encrypted reasoning items (ZDR orgs) | **003 §1** `Content.Reasoning{encrypted}` with a dedicated promotion gate (§7 G4: a positive control proving a mis-wired logger/checkpoint/cross-provider path is caught) | **Built for parsing, not round-tripping** (M5) — Anthropic's `thinking`/`redacted_thinking` parsed inbound; a `Reasoning` item is silently dropped from outbound translation (no field for Anthropic's `signature`/tamper-evidence data) — named explicit future work | Directly covered by a first-class content kind and named gate, contrary to a first guess that it might be uncovered — the actual gap is narrower: stateless multi-turn continuity (resending an encrypted block across turns) isn't built yet. |
| Built-in server-side tools (web search, file search, code interpreter, computer use, image gen) | Code interpreter: **010**, built (M3). Generic catalog: **009 §7**'s C/C++ library track. Web search: only an illustrative `WebSearch` pattern (002 §2, 006 §1), not a shipped built-in. Computer use: **009 §7 explicitly "not yet a spec-ready candidate"** — needs new capabilities (`ScreenCapture`/`InputInject`), names two real documented attacks (ZombAIs, HiddenLayer) that defeat 017's current text-delimiting defense. Real-time voice: also flagged not-spec-ready (turn interruption tension with I5 replay). Image generation: no RFC mention. | Code interpreter built; everything else is either an unbuilt candidate list or an explicit non-candidate | Philosophy differs by design: server-side tools are capability-gated WASM plugins over open-source libraries, not vendor-hosted opaque tools; web search would be author-declared, not platform-provided. Computer use and image generation are the two clearest gaps against OpenAI's built-in-tools list. |

**Genuine gaps:** structured-output refusal as a distinct typed content kind; image generation as
a built-in tool.

**Spec'd but not built:** `Backgroundable`/`StandingEffect` (blocks both background mode and batch
API equivalents) — M7; MCP conformance (011) and tracing (016) — M7/M8; guardrail filters (017) —
M8; computer use and real-time voice — explicitly flagged in-RFC as not even spec-ready yet, not
just unbuilt; encrypted-reasoning outbound round-tripping — inbound built, outbound not;
`prompt_cache_key` — deliberately deferred pending a decisive live test the project can't currently run.

**Different by design:** handoffs split into single-hop vs. structured multi-hop rather than one
mechanism; no portable cross-vendor cache-hint API; batch refused as its own subsystem; server-side
tools as sandboxed plugins over vendor-hosted black boxes; no bundled moderation classifier.

---

## 3. Microsoft Agent Framework — delta since 2026-07

AgentEngine's developer model is explicitly MAF-shaped (README, CLAUDE.md), so this is the most
directly comparable survey of the four. This section covers only what changed in MAF since
~2026-07 (per the trends doc §3) against AgentEngine's current design/build state.

| Feature | AgentEngine mechanism | Status | How it differs |
|---|---|---|---|
| All 5 patterns hit 1.0 together, uniform streaming/checkpoint/HITL/pause-resume | **014 §3**'s eight-row table, one shared superstep engine (§2) | Built — Phases A-F done; streaming surfaces (013) explicitly deferred to M7 (decision 1 in the M6 breakdown: M6 builds only the engine half of HITL) | AgentEngine's set is a superset (8 vs. 5 patterns) built from one shared primitive rather than 5 individually shipped implementations. The internal event stream type (013 §1) that streaming/telemetry key off exists; its four protocol projections don't run yet. Map-reduce's fixed-mapper-set limit (no runtime-determined K) is a named explicit gap, not silently faked. |
| Checkpoint replay covers pending HITL; single-call resume-with-responses | **014 §4/§5** + **019 §2** — `RunStateRecord.open_interactions` persists pending `Interaction`s, repopulated on restore | Built (M6 Phase E/F), functionally equivalent | Call shape differs: two separate asks (restore, then `ContinueWorkflow{}` or `ResumeWorkflow{interaction_id, response}`) rather than MAF's single `workflow.run(checkpoint_id=..., responses=...)` call. `interaction_id` is deterministically derived (`<run>:port:<executor>:<round>`), not a random UUID. |
| Declarative Workflows 1.0 (`If`, loop/jump, Power Fx expressions, HTTP-request step, MCP-from-YAML, YAML handoff routing) | **015 §3** (workflow document: `executors`/`edges`/`request_port`/`limits`) + 014 | **Spec'd only, and thinner than the MAF feature itself** — grepped 014/015 in full: no `If`/branching keyword, no expression language, no HTTP-request step type, no explicit "MCP tool invocation from YAML" construct. Branching/looping is expressed indirectly via 014 §1's generic `switch/case`/`multi-selection` edge kinds and bounded cycles (§2). | The one feature in this whole survey where the design vocabulary, not just the build, is behind — a loader could presumably build `If`/expression syntax atop the generic graph primitives, but that syntax isn't written down in 015 yet. |
| Agent Framework Harness (GA): bundled tool-loop + history + compaction + plan/execute w/ persistent todos + durable memory + skills discovery + tool-approval + built-in OTel | Spread across 002 §2 (`on_turn`), 005 §2/§4 (persistence/compaction), 029 (memory), 009 §8 (skills), 006 (tool-approval), 016 (OTel) | Mixed — 002/005/006/016 core built (M1-M5); 029 built (M4); 009 §8 built (M2) | Structural difference by design: MAF ships one runtime-wrapper object; AgentEngine composes the same capabilities as CRTP policy tags (`MaxTurns`, `Approval<Mode>`, `Telemetry<Capture>`, `Tools<...>`) plus separate provider/memory/skill mounts — I6 applies per-piece, not to one bundled type. **Real gap**: no RFC names a "plan/execute mode with persistent todos" primitive (grepped 002/026/005, found none) — multi-step planning is left to the agent's own instructions/CodeAct code, not a first-class engine construct. |
| Native cross-session memory as durable context-provider plug-in (Cosmos DB) | **029** attached through 005 §5's `ContextProvider`/`MemoryProvider` seam | Built — M4 | Deliberately different storage model (029 §1): principal-scoped worktree of structured, provenanced `MemoryItem`s with deterministic non-vector retrieval as default, vectors an explicit upgrade — vs. MAF's document-DB-backed provider. Seam shape matches (005 §5 explicitly names `MemoryProvider`s as pluggable, possibly WASM plugins); default backing store doesn't. |
| App-owned MCP hosting (expose own agents/workflows as MCP tools/servers) | **011 §4** "Exposing AgentEngine (server role)" | Spec'd, not built — M7 | Matches conceptually: agents-as-tools, long-running runs via the tasks extension, `InputRequired` → MRTR. Skills-over-MCP exposure explicitly deferred pending "the skills research" (§9) — narrower than MAF's July additions there. |
| Agent Skills discovery from MCP servers (skill-md/archive, hardened extraction) | **009 §8d** / **011 §11** — "we specify nothing [yet]" | **Explicitly not spec'd, by evidence-based decision** | Both RFCs independently verified there's no ratified MCP skill primitive — only an unmerged Draft SEP-2640 that has already changed shape once. MAF's shipped `UseMcpSkills`/`MCPSkillsSource` implements the *superseded* version of it. AgentEngine's position: keep extension negotiation (011 §3.6) general enough to slot the SEP in later, rather than build against a moving draft — a deliberate scope-out named in both RFCs, not an oversight. |
| "Squad" — external institutional-memory adapter (persistent decisions, learned preferences, bridged from GitHub Copilot CLI) | None found | **Genuine gap** — grepped the whole repo for "Squad"/"institutional memory"/"learned preferences", found nothing outside the research record itself | 029's `MemoryItem`/`MemoryOrigin` model (`AgentAuthored`/`ModelInferred`/`UserStated` provenance) is the closest conceptual relative — structured, attributed memory that could in principle hold "decisions" or "preferences" — but there's no adapter concept, no team/multi-agent-collective memory object distinct from per-principal memory, and no analog to Squad's external-framework bridging. Architecturally plausible to build on 029; not designed or named anywhere. |

**Genuine gaps:** Squad-style external institutional-memory adapter; a named plan/execute-with-todos
primitive; an in-YAML expression language and HTTP-request step type (015/014 have only generic
graph/edge primitives, no such syntax yet).

**Spec'd but not built:** 013's four streaming-surface projections for the request-port/HITL shape;
015's declarative-workflow loader and its M7 binding into 014's validator; 011's MCP server role
and skills-over-MCP slot.

**Different by design:** skills-over-MCP deliberately not implemented against MCP's unmerged draft
(both 009 §8d and 011 §11 name this explicitly, noting MAF's own version already targets a
superseded draft); memory backing store (structured worktree vs. document DB) on the same
pluggable seam; two-ask checkpoint+HITL resume vs. one combined call; no single bundled harness
type — capabilities composed as CRTP policy tags instead.

---

## 4. CrewAI / AG2 / LlamaIndex

### CrewAI

| Feature | AgentEngine mechanism | Status | How it differs |
|---|---|---|---|
| Crews (autonomous team) vs. Flows (`@start`/`@listen`/`@router` deterministic graph invoking Crews as steps) | **014 §1-§3** — one typed executor graph (executors: agent \| function \| sub-workflow \| request port; edges: direct/fan-out/fan-in/switch-case/multi-selection/chain), with Group chat/debate, Planner (Magentic), Router, and Handoff as named pattern rows | Built — M6 Phases A-F (graph, superstep engine, all eight patterns, failure handling, HITL, checkpoint/resume/time-travel) | No separate "stateless team" primitive distinct from the graph — 014 §3 treats both Crews-like and Flows-like behavior as configurations of one graph, not two engines. Pydantic-model shared Flow state has no named equivalent; workflow state is the typed message/checkpoint-record model, not a user-defined shared-state schema. |
| `output_json`/`output_pydantic` | **003 §4** `OutputSchema<T>`, three enforcement strategies | Built and wired end-to-end (M1) — confirmed in `agent.hpp`, `chat_client.hpp`, provider translation, `agent_registry.hpp`'s strategy selector | No mechanism difference beyond CrewAI exposing two named Python helpers vs. one typed policy parameter. |
| MCP support (`mcps` field, multi-transport) | **011 §3** (client role) + §7 (transport — Streamable HTTP required; SSE explicitly *not* adopted, §3.5) | Spec'd only — M7 | 011 targets the dated 2026-07-28 MCP revision (stateless core, MRTR replacing sampling/roots/elicitation) with an executable conformance-percentage gate — a stricter, dated bar than CrewAI's `mcps` field implies. |
| A2A as first-class delegation, agent dynamically chooses local vs. remote execution | **012 §2/§3** + **002 §4** ("an author should not be able to tell from the call site whether the callee is in-process") | **Genuine gap** against this specific framing — 012 §5 states A2A is the inter-organization/inter-process seam, and the engine explicitly does *not* force local agents through A2A to talk to each other | AgentEngine's design goal is the opposite emphasis: call-site *uniformity* decided by whoever wires the agent (author/config), not a runtime choice the agent itself makes. CrewAI's "agent chooses" pattern has no named counterpart. Also spec'd-only, M7. |

### AG2 (AutoGen)

| Feature | AgentEngine mechanism | Status | How it differs |
|---|---|---|---|
| Unified Group Chat + Swarm (Patterns, Handoffs, Guardrails, Context Variables, `AutoPattern`, `RoundRobinPattern`) | **014 §3** names Handoff and Group chat/debate rows directly, plus Planner (Magentic) as its own row (§9 Q5). Turn-taking styles (round-robin, LLM-selected-speaker) are expressible as the moderator's routing logic within those rows. "Guardrails" → **017** (not read in depth in this pass). | Graph engine + Group chat/debate + Planner rows built (M6 Phase C); Handoff's workflow-executor wiring vs. only `Handoff<T>` tool-composition not independently confirmed; 017 is M8, not started | "Context Variables" (a free-form shared mutable dict across a chat) has **no dedicated named primitive** — the closest is per-executor state carried in the typed message/checkpoint record (014 §1/§5), structurally narrower than AG2's free-form shared dict. |
| AG-UI / A2A / MCP as sample/ecosystem-level, not a dedicated field | **013** (AG-UI as primary projection), **012**, **011** — each a first-class, gated RFC with its own conformance suite and promotion gate | Spec'd only, all M7 — `protocol/agui/` is README-only per the M6 breakdown's dependency table | Where AG2 treats these as sample code bolted on, AgentEngine treats them as one internal run-event stream (013 §1/§3) projected uniformly onto AG-UI/A2A/MCP/SSE — "one mechanism, four renderings" for HITL (§5) — a design-weight difference, though neither is built yet either way. |

### LlamaIndex

| Feature | AgentEngine mechanism | Status | How it differs |
|---|---|---|---|
| Event-driven Workflows (typed Events, branching/looping/parallel/pause-resume/failure recovery, plain code no DSL) | **014 §1-§2** — executors are Quark actors, edges carry typed messages (003), execution proceeds in supersteps, cycles allowed with a mandatory bound (§9 Q2), pause/resume via request port (§4) + checkpoint/resume (§5), failure per-edge policy (§6) | Built for the engine core — Phases A-F (compile-time-validated typed-edge graph, superstep round delivery/fan-out, all eight patterns, failure/supervision, pause/resume, checkpoint/resume/time-travel with a 20-node kill-at-every-boundary proof) | Authoring is C++ CRTP (002), matching LlamaIndex's plain-code positioning, but AgentEngine also has an equivalent declarative YAML/JSON form (015, I6) rather than code-only. Introspection/live view (§7, Phase G) and the declarative-loader half of typed-edge validation (needs 015, M7) not yet built. |
| `Memory` class with pluggable `MemoryBlock`s, SQLite persistence, replacing token-capped buffer | **029** — principal-scoped worktree (content-addressed, not SQLite) of structured, provenanced `MemoryItem`s (`Episodic`/`Semantic`/`Procedural` × `UserStated`/`ModelInferred`/`ToolDerived`/`AgentAuthored`); extraction via attributed `ContextProvider.on_turn_end` (analogous to `FactExtractionMemoryBlock`) | Built for the core (M4) | Default retrieval is deterministic keyword/salience/recency arithmetic, no embedding dependency (029 §1 states this as a deliberate rejection of the Mem0/Zep/MemGPT shape on I3/I5 grounds) — the reverse emphasis of LlamaIndex's vector-first design. Persistence backend differs by design (content-addressed worktree, not SQLite). The vector-based `ae:memory` plugin (§5/§9 G6) is explicitly not built — deferred, optional. |
| MCP bidirectional (consume and serve) | **011 §3** (client) + **§4** (server) — both roles in the same RFC | No gap — bidirectionality explicit in spec; not built, M7 | — |
| A2A exposure of workflow-based conversational agents (multi-turn, streaming, citations) | **012 §2** (server surface) + **013**'s A2A projection (§3: "task status + artifact updates from the same stream") + 019's `Suspended` state for long-running behavior | Spec'd only, M7 | "Citations" has no named equivalent construct — closest is provenance/taint tagging on content items (003 §2), not a citation-specific feature. **Genuine gap.** |

**Genuine gaps (union across CrewAI/AG2/LlamaIndex):** an agent dynamically choosing local-vs-remote
execution at runtime (design goal is call-site uniformity instead); a citation-specific
content/provenance feature; free-form shared mutable "Context Variables" across a group chat (only
typed per-executor state exists).

**Spec'd but not built:** MCP (011), A2A (012), AG-UI/streaming (013) — all M7; workflow
introspection/live view and the 030 Project pause/resume layer — remainder of M6; declarative-YAML
workflow loading (015) — M7; vector-based `ae:memory` plugin — explicitly deferred/optional; safety
guardrails (017) — M8.

**Built and real, verified against code:** the workflow/superstep graph engine and all eight 014 §3
patterns (M6 Phases A-F, committed through `e21233a`); request-port HITL + checkpoint/resume/
time-travel with a 20-node kill-at-every-boundary proof; `OutputSchema<T>` wired into real
provider translation for OpenAI/Anthropic/OpenRouter; the core memory system.

---

## 5. Cross-cutting synthesis

Consolidating all four comparisons (dedup'd — MCP/A2A/streaming, for instance, come up in every
section pointing at the same three M7 RFCs):

### Genuine gaps — no RFC names an equivalent at all

- **A structured-output "refusal" as a distinct typed content kind** (vs. OpenAI's dedicated
  `refusal` field) — 003's `Content` kind table has no `Refusal` type separate from `Error`/`Text`.
- **Image generation as a built-in/server-side tool** — no mention in any RFC.
- **A runtime-dynamic local-vs-remote execution choice** for an agent (CrewAI's A2A-client pattern)
  — 012/002 deliberately design for call-site uniformity decided at wiring time instead.
- **A citation-specific content/provenance feature** for A2A-streamed conversational responses —
  provenance/taint tagging exists (003 §2) but nothing citation-shaped.
- **Free-form shared mutable state across a group chat/swarm** (AG2's Context Variables) — only
  typed per-executor message/checkpoint state exists.
- **A named "plan/execute mode with persistent todos" primitive** (MAF Harness) — multi-step
  planning is left to the agent's own instructions/CodeAct code, not an engine construct.
- **An in-YAML expression language and HTTP-request workflow step type** (MAF Declarative
  Workflows 1.0) — 014/015 have only generic graph/edge primitives, no such syntax written down yet.
  This is the one place in the whole survey where the *design*, not just the build, trails a
  comparable framework's current feature set.
- **A "Squad"-style external institutional-memory adapter** (persistent team decisions/learned
  preferences bridged from an outside framework) — no RFC, ADR, or planning doc names this;
  029's provenanced `MemoryItem` model is architecturally close but nothing adapter-shaped exists.

### Spec'd but not built — design-complete, waiting on a milestone

- **`Backgroundable`/`StandingEffect`** (006 §6b) and the two remaining rows of 019 §2's
  wake-condition table — zero implementation, confirmed by grep, assigned to M7
  (`docs/planning/backgroundable-standingeffect-gap.md`). This single gap blocks the AgentEngine
  equivalents of OpenAI's background mode *and* batch API *and* MAF's/OpenAI's async task
  completion handling.
- **011 (MCP), 012 (A2A), 013 (UI/streaming), 015 (declarative format)** — all fully spec'd,
  all M7, none started. Every framework surveyed has these as either shipped or actively
  maturing features; AgentEngine's versions are comprehensively specified (011 alone has 36
  numbered sub-obligations under §3.1) but present zero running code today.
- **016 (Observability/tracing) and 017 (Safety/guardrail filters)** — spec'd, M8, not started.
- **`agent.spawn`**'s actual invocation path (026 §5) — the capability-attenuation mechanism it
  needs (`SpawnBudget`, ADR-006) is built; the spawn path itself isn't wired to it. Named residual
  since M3 close.
- **Workflow introspection/live view** (014 §7) and the **030 Project** pause/resume layer — the
  remaining Phases G-J of the milestone currently in progress.
- **Vector-based `ae:memory` plugin** (029 §5) — explicitly deferred as an optional upgrade over
  the deterministic default, not a blocking gap.
- **Encrypted-reasoning outbound round-tripping** — inbound parsing built, outbound resend isn't
  (no field for Anthropic's tamper-evidence signature data yet).
- **Computer use and real-time voice** — the only items where even the *RFC itself* names them as
  "not yet a spec-ready candidate" (009 §7), rather than spec'd-and-unbuilt like everything else
  in this list.

### Different by design — not a gap, a deliberate divergence

- **Memory**: every framework surveyed (LangGraph's `BaseStore`, MAF's Cosmos provider,
  LlamaIndex's `Memory`) defaults to a vector/document-DB-backed store; AgentEngine's 029 defaults
  to a deterministic, non-vector, content-addressed worktree of provenanced items, with vector
  search as an explicit opt-in upgrade — argued in 029 §1 on I3 (attribution)/I5 (replay) grounds.
- **Prompt caching**: no portable cross-vendor cache-hint API exists or is planned; each backend
  translates caching as internal logic on top of context-assembly's segment boundaries, rejecting
  "one vendor's shape leaking onto every other backend."
- **Checkpoint + HITL resume**: every framework surveyed (LangGraph, MAF) offers a single call that
  both restores a checkpoint and supplies pending human responses; AgentEngine splits this into two
  typed asks (restore, then `ContinueWorkflow`/`ResumeWorkflow`) — same durability guarantee,
  different call ergonomics, forced in part by the actor model's `passivate()` semantics (a
  suspended run must *return*, never resume mid-handler).
- **No single bundled "harness"/"runtime" object**: MAF's Harness and OpenAI's `Runner` both ship
  one wrapper type bundling loop + memory + compaction + tracing. AgentEngine composes the same
  capabilities as CRTP policy tags across several RFCs (`MaxTurns`, `Approval<Mode>`,
  `Telemetry<Capture>`, `Tools<...>`) plus separate provider/memory/skill mounts — consistent with
  "policies are types," and I6 (declarative/native equivalence) applying per-piece rather than to
  one bundled type.
- **Server-side tools**: OpenAI ships vendor-hosted opaque tools (web search, code interpreter,
  computer use); AgentEngine's equivalent is capability-gated WASM plugins wrapping open-source
  libraries — a heavy library becomes *safer* as a sandboxed plugin than as a linked host
  dependency, per the README's own framing.
- **Handoffs**: every framework surveyed treats handoff as one mechanism (a tool call that
  transfers ownership); AgentEngine splits it into a single-hop tool-shaped composition (002 §4)
  and a structured multi-hop graph pattern (014 §3), explicitly steering long chains toward the
  latter.
- **Skills-over-MCP**: both 009 §8d and 011 §11 explicitly decline to build against MCP's unmerged
  Draft SEP-2640, noting that MAF's own shipped implementation already targets a superseded version
  of that draft — a deliberate scope-out, not an oversight, named by both RFCs.
- **Guardrails/filters**: richer verdict set than a boolean tripwire (`allow`/`annotate`/`redact`/
  `require_approval`/`deny`), with an explicit "filters can deny, never grant" invariant (I3); no
  bundled injection/jailbreak classifier ships, only deterministic PII detection — filters are an
  extension point, not a moderation feature.

### One observation across all four comparisons

Every framework surveyed independently converged on the same three protocol/capability areas
(MCP, A2A, streaming/UI projection) as recent or actively maturing work. AgentEngine's versions of
all three (011, 012, 013) are comprehensively specified — often more so, in stated scope, than the
external frameworks' current shipped versions — but are the single largest concentration of
**zero-implementation, fully-specified** surface area in the project, all parked on Milestone 7.
That milestone's own promotion gates (011 §10's conformance percentage, `a2a-tck`) already depend
on `Backgroundable`/`StandingEffect` being real first (per the M7 roadmap rationale recorded
2026-08-08) — so closing that one M7-assigned gap is the load-bearing precondition for most of the
"spec'd but not built" list above, not just one item on it.
