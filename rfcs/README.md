# The RFC set

The authoritative design of AgentEngine. **When code and an RFC disagree, the RFC wins**; if the RFC is
wrong, fix it first, backed by an ADR in [`../decisions/`](../decisions/README.md), then fix the code.

Start with the [specification](../AgentEngineSpecification.md), then [CONVENTIONS.md](../CONVENTIONS.md),
then the RFCs. Load-bearing first: **001, 002, 006, 007, 008, 009, 010, 011**.

| # | Document | Covers | Status |
|---|---|---|---|
| — | [AgentEngineSpecification.md](../AgentEngineSpecification.md) | Vision, layering, invariants, locked decisions, the runtime substrate, glossary | Draft |
| — | [CONVENTIONS.md](../CONVENTIONS.md) | The binding coding contract | Draft |
| 001 | [Execution Model](001-Execution-Model.md) | Run lifecycle, turn loop, concurrency, cancellation, failure, replay | Reviewed |
| 002 | [Agent Model and Authoring](002-Agent-Model-and-Authoring.md) | CRTP policy surface, composition, middleware, metadata validation | Reviewed |
| 003 | [Message and Content Model](003-Message-and-Content-Model.md) | `Content` items, provenance and taint, blobs, structured output, usage | Reviewed |
| 004 | [ChatClient Plane](004-Model-Provider-Plane.md) | `ChatClient` seam, capability-driven degradation, reliability, cost, recording | Reviewed |
| 005 | [Sessions, State and Memory](005-Sessions-State-and-Memory.md) | `AgentSession` actor, persistence, context assembly, compaction, memory, redaction | Reviewed |
| 006 | [Tool and Function Plane](006-Tool-and-Function-Plane.md) | Declaration, the 10-step invocation pipeline, approval, concurrency, result hygiene | Reviewed |
| 007 | [Capability and Trust Model](007-Capability-and-Trust-Model.md) | Threat model, principals, capabilities, taint, policy, trust tiers, supply chain, audit | Reviewed |
| 008 | [Sandbox and Isolation](008-Sandbox-and-Isolation.md) | The isolation contract, profiles, per-backend enforcement, determinism, abuse handling | Reviewed |
| 009 | [Plugin and Extension System](009-Plugin-and-Extension-System.md) | WIT worlds, package format, lifecycle, host imports, the C/C++ library track, skills | Reviewed |
| 010 | [Code Interpreter and Shell](010-Python-Code-Interpreter.md) | Interpreter + CodeAct + Shell sharing one execution context, runtime selection and its evidence, workspace, packages, the `call_tool` bridge | Reviewed |
| 011 | [MCP Conformance](011-MCP-Conformance.md) | MCP `2026-07-28` client + server: stateless core, MRTR, tools/resources/prompts, extensions, authorization | Reviewed |
| 012 | [A2A Conformance](012-A2A-Conformance.md) | A2A v1.0: agent card, task lifecycle, bindings, delegation | Reviewed |
| 013 | [UI and Streaming Surfaces](013-UI-and-Streaming-Surfaces.md) | The internal run event stream and its projections (AG-UI, A2A, SSE) | Reviewed |
| 014 | [Workflow and Orchestration](014-Workflow-and-Orchestration.md) | Typed executor graph, supersteps, patterns, checkpointing, time-travel | Reviewed |
| 015 | [Declarative Agent Format](015-Declarative-Agent-Format.md) | YAML/JSON agents and workflows; the equivalence rule | Reviewed |
| 016 | [Observability](016-Observability.md) | OTel GenAI conformance, traces, metrics, content-capture privacy, audit vs telemetry | Reviewed |
| 017 | [Safety and Content Governance](017-Safety-and-Content-Governance.md) | Prompt injection defence in depth, filters, PII, attack-class table | Reviewed |
| 018 | [Identity, Authorization and Secrets](018-Identity-Authorization-and-Secrets.md) | Principals, admission vs effect authorization, secret seam, multi-tenancy | Reviewed |
| 019 | [Durability and Long-Running Agents](019-Durability-and-Long-Running-Agents.md) | Checkpoints, suspension, exactly-once effects, recovery, retention | Reviewed |
| 020 | [Configuration and Hosting](020-Configuration-and-Hosting.md) | Policy vs configuration, hosting shapes, server surfaces, operations | Reviewed |
| 021 | [Platform Support and Portability](021-Platform-Support-and-Portability.md) | Support tiers, per-subsystem portability risk, build matrix | Reviewed |
| 022 | [Testing and Evaluation](022-Testing-and-Evaluation.md) | Deterministic simulation, golden traces, evaluation, positive controls | Reviewed |
| 023 | [Performance Targets and Budgets](023-Performance-Targets-and-Budgets.md) | Budget classes, provisional numbers, machine-independent invariants | Reviewed |
| 024 | [Versioning, Compatibility and Governance](024-Versioning-Compatibility-and-Governance.md) | Versioning, deprecation, the decision process, RFC hygiene | Reviewed |
| 025 | [Worktree and Virtual Filesystem](025-Worktree-and-Virtual-Filesystem.md) | The session's virtual disk: content-addressed objects, sub-worktrees, sharing and merge, mounts, per-turn commit | Reviewed |
| 026 | [Agent-Facing Runtime Surface](026-Agent-Facing-Runtime-Surface.md) | What the model sees: the ordinary environment, plain Python, and the `agent` library that *is* CodeAct's action space | Reviewed |
| 027 | [Vocabulary and Naming](027-Vocabulary-and-Naming.md) | The canonical name for every concept, aligned to MAF, plus the collision register for words that mean different things in different specs | Reviewed |
| 028 | [Bulk Data Transfer and Zero-Copy](028-Bulk-Data-Transfer-and-Zero-Copy.md) | Moving large data without JSON, without copies, and without touching the context window: handles, Arrow, per-profile mapping | Reviewed |
| 029 | [Memory System](029-Memory-System.md) | Memory as a principal-scoped worktree, not a wrapped vector database: structured `MemoryItem`s, attributed extraction, deterministic default retrieval, vectors as an optional upgrade | Reviewed |
| 030 | [Project: Workspace Grouping and Lifecycle](030-Project-Workspace-and-Lifecycle.md) | A durable unit above a session — root session + owned sub-sessions + their worktree refs — with directed pause/restore/resume and no cap on concurrently open projects | Reviewed |

Supporting documents: [`OpenQuestions.md`](../OpenQuestions.md) (cross-cutting unresolved questions),
[`../decisions/`](../decisions/) (the ADR process and record), and the dated, cited research records:
[the 2026 standards landscape](../docs/research/2026-standards-landscape.md) and
[the MCP ecosystem](../docs/research/2026-mcp-ecosystem.md).

## Conformance is a number, not a paragraph

MCP publishes an **official conformance suite** that tests both client and server roles and
validates every message against the spec's schema, and SEP-2484 explicitly permits any
implementation to run it and report a compliance percentage. RFC 011's gate is therefore executable:
`conformance server --url … --suite all` and `conformance client --command … --spec-version
2026-07-28`, with a published percentage per role, pinned to a conformance release. Notably, **no C
or C++ implementation currently claims `2026-07-28` conformance** — AgentEngine would be among the
first.

## Maturity ladder

**Draft** → **Reviewed** (open questions resolved or deferred) → **Proven** (the RFC's named gate
executed and recorded as an ADR) → **Accepted (platform)** (proven, implemented, covered by the
suite on a named platform). **Reviewed is the floor today, not Draft** — all 30 RFCs cleared their
review gate 2026-08-05 (`../docs/planning/v1-review-signoff-workflow.md`; each RFC's own `**Status:**`
header confirms it). Promotion beyond Reviewed happens by executing a gate — not by consensus that a
document reads well — and several RFCs already have real, executed evidence in `decisions/` toward
their named gate: 007 (Capability and Trust Model, §9) via ADR-005/006/007/009; 008 (Sandbox and
Isolation, §9) via ADR-004/008/011/013/071; 009 (Plugin and Extension System, §10) via ADR-010; 010
(Python Code Interpreter, §9) via ADR-001/002/003/015; 011 (MCP Conformance, §10) via ADR-061 §11's
Tier 1 prove phase (real client-role conformance number, non-auth 75/75); 025 (Worktree and Virtual
Filesystem, §9) via ADR-014. None of that evidence is a full gate close, though — every one of those
ADRs is explicit about what it leaves open (e.g. ADR-004 is "Spiked, not Judged"; ADR-009 resolves "007 §3's
in-process enforcement half"; ADR-014 is Windows-only with named untested residuals), so no RFC has
actually reached **Proven** yet by this ladder's own strict definition. Check the ADR itself before
citing one as closing a specific gate item.
