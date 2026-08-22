# AgentEngine

**A C++23 engine for building agent applications — sandboxed by construction, standards-native,
cross-platform.**

AgentEngine hosts agents, sessions, tools, and multi-agent workflows on its own `agentengine::rt::`
runtime substrate (historical: originally built on top of the
[Quark](https://github.com/thnak/QuarkCpp) actor engine; ADR-037 removed that dependency entirely
and the `third_party/quark` submodule is gone from the tree). Untrusted code isolation and a Python
code interpreter are **built-in subsystems, not optional add-ons**, and the protocol surface is the
open agent stack of 2026 — MCP, A2A, AG-UI, OpenTelemetry GenAI — rather than a proprietary API.

The developer model is deliberately **MAF-shaped** (the agent / session / tool / middleware /
graph-workflow vocabulary Microsoft Agent Framework established), expressed in AgentEngine's own
**zero-cost CRTP policy idiom** (originally modeled on Quark's identical idiom) instead of runtime
configuration objects.

> **Status: implementation under way.** All 30 RFCs have passed review (each RFC's own header reads
> **Reviewed**, dated 2026-08-05) and this repository now contains a real, tested C++23
> implementation alongside the specification set — hundreds of passing tests under `tests/`, dozens
> of headers under `include/agentengine/`, and a growing set of ADRs in
> [`decisions/`](decisions/README.md) recording gates that were actually executed against real code,
> not just designed. Per the build order in
> [`docs/planning/v1-implementation-roadmap.md`](docs/planning/v1-implementation-roadmap.md),
> Milestones 0-6 (core substrate, tools/capabilities/sandbox, worktree/interpreter/CodeAct,
> sessions/durability/memory, real providers/identity/secrets, multi-agent orchestration) are
> complete. Milestone 7 (protocol conformance) is **in progress**: real code exists for its earlier
> phases (MCP, A2A, AG-UI, the declarative YAML/JSON compilers), and a Phase G gate audit has run
> against that code — the audit found the milestone's own exit criterion **not yet met**. Milestones
> 8-9 (safety/observability/perf, hosting/platform/bulk-data) have not started. Each RFC still names
> the gate that promotes it further, from Reviewed toward Proven and Accepted — see the
> [per-milestone breakdown docs](docs/planning/) for live, phase-by-phase status.

## The shape of it

```cpp
struct Researcher : Agent<Researcher,
        ChatClientId<"anthropic:claude-opus-5">,
        Tools<WebSearch, CodeInterpreter, Handoff<Writer>>,
        Capabilities<NetOut<"api.search.example">>,
        SandboxProfile<Strict>,
        MaxTurns<12>> {
    static constexpr std::string_view name = "researcher";
    static constexpr std::string_view instructions =
        "Research the question. Cite sources. Hand off to the writer when you have enough.";
};

auto session = engine.create_session("user-42");
auto stream  = session.run_stream<Researcher>("Compare WASI 0.2 and 0.3.");
```

The same agent is expressible in YAML and compiles to **byte-identical metadata** — that equivalence
is a tested invariant, not a convention.

For a single-provider script that doesn't need the full `Agent<>` policy surface, a smaller facade
sits over the same `AgentSession` wiring — one credential, a `ModelCallGateway`-wrapped client by
default, and a synchronous `.ask()`:

```cpp
auto built = quickstart::OpenAiSessionBuilder("gpt-4o-mini")
                 .api_key_from_env("openai-api-key", "OPENAI_API_KEY")
                 .grant(Capability{cap::FsRead{"scratch", "", std::nullopt}})
                 .build();
if (built) { auto reply = built->ask("Compare WASI 0.2 and 0.3."); }
```

`build()` fails closed (no credential, no store) instead of failing at the first `chat()` call. See
`include/agentengine/core/session_builder.hpp` and
[`docs/planning/quickstart-session-builder-design-draft.md`](docs/planning/quickstart-session-builder-design-draft.md)
for scope and the three red-team passes it went through before promotion.

## What makes it different

- **Isolation is the substrate, not a tool.** Every effect — filesystem, network, secrets,
  subprocess — is reachable only through an explicitly passed capability handle. Sandboxed code
  starts with *none*. There is no "allow everything" constructor.
- **Model output is data, never authority.** Nothing a model emits can widen a capability, approve
  an action, or change policy. Enforced by the type system, not by discipline.
- **Plugins are WASM components.** One signed artifact runs bit-identically on Windows and Linux
  (the targeted OSes, 021 §2), holding only what it is granted. That includes the open-source
  **C/C++ library ecosystem**
  via `wasi-sdk` — SQLite, tree-sitter, libarchive, codecs, tokenizers, ONNX Runtime — where a heavy
  library becomes *safer* as a plugin than as a linked host dependency.
- **A Python interpreter that can actually `import numpy`.** The runtime choice is grounded in
  dated research rather than fashion: see [the decision](#the-sandbox-decision).
- **Files outlive the sandbox.** Every session owns a **worktree** — a content-addressed virtual disk
  the engine manages — so persistence, rewind, and multi-agent sharing are engine properties, not
  properties of whichever isolation technology is selected. Agents in one session share it or branch
  from it with an explicit merge.
- **CodeAct is the action space.** The agent's primary way of acting is writing a program that calls
  a host-backed library, not emitting one tool-call JSON per step — so control flow and large
  intermediate data stay inside the sandbox instead of transiting the context window.
- **An unremarkable environment.** Normal Python, normal paths, normal exceptions, and *zero* prompt
  text about sandboxes or capabilities. A model has seen millions of lines of ordinary Python and
  none of our architecture. This is a prompt-surface choice and never a security mechanism — proven
  by re-running the hostile suite against an agent that has been *told* the architecture.
- **Standards-native.** MCP `2026-07-28` (stateless core, MRTR), A2A v1.0, AG-UI, OTel GenAI — each
  with a named revision and an executable conformance suite. "Supports MCP" without a passing suite
  is not a claim this project makes.
- **Runs are replayable.** Every nondeterministic input crosses a recorded seam, so a run replays
  offline with no external service contacted.

## The sandbox decision

The most consequential decision in the design, and the one most likely to be questioned:

- **WASM Component Model (WASI 0.3) is the plugin ABI — locked.** Portable, capability-based by
  construction, language-agnostic, microsecond instantiation. The ecosystem risk is low because
  *we* define the WIT world and authors compile to it.
- **The Python code interpreter is embedded native CPython, permanently — never WASM, and never a
  second runtime.** As of July 2026 the rich Python-on-WASM ecosystem (NumPy, pandas, SciPy, and
  PEP 783 wheels on PyPI) is **Emscripten**-targeted and needs a JavaScript host, so it cannot be
  embedded via wasmtime; the embeddable **WASI** CPython — governed by the now-accepted PEP 816 —
  still lacks sockets, threads, a wheel platform tag, and a binary-wheel ecosystem. That evidence is
  corroborating, not the reason: even a mature WASI Python would still be a second Python runtime,
  and this project deliberately chooses one, permanently, so an agent's generated code — and the
  humans verifying it — never have to know which runtime they're dealing with.

Isolation is still a seam with named profiles (`wasm`, `native-jail`, `remote`), identical in
*contract* and differing only in *strength* — but the interpreter itself does not fork across them.
Isolation strength for CPython comes from treating the **whole execution environment** (worktree,
runners, capabilities, network policy) as the sandbox, with CPython's own dangerous entry points
mediated at the point of use and the OS-level jail as a second layer (008 §1b) — not from swapping
the interpreter for a differently-capable one. There is no `microvm` profile: a workload that would
have reached for hardware isolation uses `remote` against infrastructure that already provides it.
Evidence: [`docs/research/2026-standards-landscape.md`](docs/research/2026-standards-landscape.md)
§6–8.

## Core invariants

| | |
|---|---|
| **I1** | One session, one executor — `rt::AsyncMutex`, a runtime-checked guard (historical: originally Quark's structural, mailbox-enforced single-executor invariant; ADR-037) |
| **I2** | No ambient authority — every effect needs an explicitly passed capability |
| **I3** | Model output is data, never authority |
| **I4** | Every effect is attributable — span + audit record, always |
| **I5** | Nondeterminism crosses a recorded seam — runs replay offline |
| **I6** | Declarative and native authoring surfaces are equivalent |
| **I7** | Protocol conformance is a gate, at a named revision |
| **I8** | Budgets are enforced by a benchmark gate, not documented |

Full statements: [AgentEngineSpecification.md §4](AgentEngineSpecification.md).

## Layering

```
 L4  Protocol surfaces     MCP server + client · A2A · AG-UI · OpenAI-compatible HTTP
 L3  Orchestration         workflow graph, handoff, group chat, checkpoint / resume / time-travel
 L2  Agent core            Agent · AgentSession · Tool plane · ChatClient plane · Middleware
 L1  Trust & isolation     capability model · sandbox seam · WASM component plugin ABI
 L0  Runtime substrate     agentengine::rt:: async mutex, thread pool, session/append-log store,
                           circuit breaker, channel/stream backend, PAL
```

AgentEngine owns its own scheduler-equivalent (`rt::ThreadPool`), its own single-executor guard
(`rt::AsyncMutex`), and its own durability seam (`rt::SessionStore`/`rt::AppendLogStore`) — no
distributed cluster membership, no actor mailbox (historical: L0 used to be a configured Quark seam;
ADR-037 replaced it with this self-contained substrate, and there is no multi-node cluster story at
all post-migration).

## The specification set

Start with the [specification](AgentEngineSpecification.md), then
[CONVENTIONS.md](CONVENTIONS.md), then the RFCs. Load-bearing first: **001, 002, 006, 007, 008,
009, 010, 011**.

| # | Document | Covers | Status |
|---|---|---|---|
| — | [AgentEngineSpecification.md](AgentEngineSpecification.md) | Vision, layering, invariants, locked decisions, the runtime substrate, glossary | Draft |
| — | [CONVENTIONS.md](CONVENTIONS.md) | The binding coding contract | Draft |
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

Supporting documents: [`OpenQuestions.md`](OpenQuestions.md) (cross-cutting unresolved questions),
[`decisions/`](decisions/) (the ADR process and record), and the dated, cited research records —
[the 2026 standards landscape](docs/research/2026-standards-landscape.md) (protocol content,
sandboxing trade space, the Python-on-WASM finding) and
[the MCP ecosystem](docs/research/2026-mcp-ecosystem.md) (registry, SDKs, the C/C++ landscape, and
the official conformance suite).

### Conformance is a number, not a paragraph

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
review gate 2026-08-05 (`docs/planning/v1-review-signoff-workflow.md`; each RFC's own `**Status:**`
header confirms it). Promotion beyond Reviewed happens by executing a gate — not by consensus that a
document reads well — and several RFCs already have real, executed evidence in `decisions/` toward
their named gate: 007 (Capability and Trust Model, §9) via ADR-005/006/007/009; 008 (Sandbox and
Isolation, §9) via ADR-004/008/011/013; 009 (Plugin and Extension System, §10) via ADR-010; 010
(Python Code Interpreter, §9) via ADR-001/002/003/015; 025 (Worktree and Virtual Filesystem, §9) via
ADR-014. None of that evidence is a full gate close, though — every one of those ADRs is explicit
about what it leaves open (e.g. ADR-004 is "Spiked, not Judged"; ADR-009 resolves "007 §3's
in-process enforcement half"; ADR-014 is Windows-only with named untested residuals), so no RFC has
actually reached **Proven** yet by this ladder's own strict definition. Check the ADR itself before
citing one as closing a specific gate item.

## Relationship to Quark (historical)

AgentEngine used to consume [Quark](https://github.com/thnak/QuarkCpp) as an **unmodified
submodule**, mapping its own concepts onto Quark's (sessions were actors, runs were asks, streams
were credit-rings, durability was the `Store` seam) and never patching it in-tree — runtime changes
went upstream as Quark RFCs. `decisions/ADR-037-remove-quark-as-core-runtime.md` (executed
2026-08-13) removed that dependency entirely: `third_party/quark` is no longer a submodule of this
repository, and AgentEngine's runtime substrate is now its own `agentengine::rt::` namespace (see
`AgentEngineSpecification.md` §7). The design → red-team → prove → judge ADR process this project
uses is still modeled on Quark's own governance discipline, which is unaffected by the runtime
change.

## Contributing

Read [CONVENTIONS.md](CONVENTIONS.md) first. Every change must follow the specs; **when code and a
spec disagree, the spec wins** — if the spec is wrong, fix the spec first, backed by an ADR, then
the code. Contested, hot-path, or security-critical designs go through
`design → red-team → prove → judge` and produce an ADR, not an ad-hoc change.

## Licence

Not yet decided ([024 Q1](024-Versioning-Compatibility-and-Governance.md)); MIT is the working
assumption (historical: originally chosen to match Quark's own licence, back when Quark was a
consumed dependency; ADR-037 removed that dependency, the MIT assumption itself is unchanged).
