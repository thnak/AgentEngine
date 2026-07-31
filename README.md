# AgentEngine

**A C++23 engine for building agent applications — sandboxed by construction, standards-native,
cross-platform.**

AgentEngine hosts agents, sessions, tools, and multi-agent workflows on top of the
[Quark](https://github.com/thnak/QuarkCpp) actor engine. Untrusted code isolation and a Python code
interpreter are **built-in subsystems, not optional add-ons**, and the protocol surface is the open
agent stack of 2026 — MCP, A2A, AG-UI, OpenTelemetry GenAI — rather than a proprietary API.

The developer model is deliberately **MAF-shaped** (the agent / session / tool / middleware /
graph-workflow vocabulary Microsoft Agent Framework established), expressed in **Quark's zero-cost
CRTP policy idiom** instead of runtime configuration objects.

> **Status: design phase.** There is no implementation yet. This repository currently contains the
> specification set: 24 RFCs, the conventions contract, and a dated research record. Every RFC is
> **Draft**, and each names the gate that would promote it. That is deliberate — the design is the
> product right now.

## The shape of it

```cpp
struct Researcher : Agent<Researcher,
        Provider<"anthropic:claude-opus-5">,
        Tools<WebSearch, CodeInterpreter, Handoff<Writer>>,
        SandboxProfile<Profile::Strict>,
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

## What makes it different

- **Isolation is the substrate, not a tool.** Every effect — filesystem, network, secrets,
  subprocess — is reachable only through an explicitly passed capability handle. Sandboxed code
  starts with *none*. There is no "allow everything" constructor.
- **Model output is data, never authority.** Nothing a model emits can widen a capability, approve
  an action, or change policy. Enforced by the type system, not by discipline.
- **Plugins are WASM components.** One signed artifact runs bit-identically on Windows, Linux, and
  macOS, holding only what it is granted. That includes the open-source **C/C++ library ecosystem**
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
| **I1** | One session, one executor (Quark's single-executor invariant) |
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
 L2  Agent core            Agent · Session · Tool plane · Model provider plane · Middleware
 L1  Trust & isolation     capability model · sandbox seam · WASM component plugin ABI
 L0  Runtime substrate     Quark: scheduler, mailbox, cluster, persistence, timers, PAL
```

AgentEngine writes no scheduler, no mailbox, no cluster membership, no persistence engine, and no
timer wheel — every one of those is a Quark seam it configures.

## The specification set

Start with the [specification](AgentEngineSpecification.md), then
[CONVENTIONS.md](CONVENTIONS.md), then the RFCs. Load-bearing first: **001, 002, 006, 007, 008,
009, 010, 011**.

| # | Document | Covers | Status |
|---|---|---|---|
| — | [AgentEngineSpecification.md](AgentEngineSpecification.md) | Vision, layering, invariants, locked decisions, Quark mapping, glossary | Draft |
| — | [CONVENTIONS.md](CONVENTIONS.md) | The binding coding contract | Draft |
| 001 | [Execution Model](001-Execution-Model.md) | Run lifecycle, turn loop, concurrency, cancellation, failure, replay | Draft |
| 002 | [Agent Model and Authoring](002-Agent-Model-and-Authoring.md) | CRTP policy surface, composition, middleware, metadata validation | Draft |
| 003 | [Message and Content Model](003-Message-and-Content-Model.md) | Parts, provenance and taint, blobs, structured output, usage | Draft |
| 004 | [Model Provider Plane](004-Model-Provider-Plane.md) | Provider seam, capability-driven degradation, reliability, cost, recording | Draft |
| 005 | [Sessions, State and Memory](005-Sessions-State-and-Memory.md) | Session actor, persistence, context assembly, compaction, memory, redaction | Draft |
| 006 | [Tool and Function Plane](006-Tool-and-Function-Plane.md) | Declaration, the 10-step invocation pipeline, approval, concurrency, result hygiene | Draft |
| 007 | [Capability and Trust Model](007-Capability-and-Trust-Model.md) | Threat model, principals, capabilities, taint, policy, trust tiers, supply chain, audit | Draft |
| 008 | [Sandbox and Isolation](008-Sandbox-and-Isolation.md) | The isolation contract, profiles, per-backend enforcement, determinism, abuse handling | Draft |
| 009 | [Plugin and Extension System](009-Plugin-and-Extension-System.md) | WIT worlds, package format, lifecycle, host imports, the C/C++ library track, skills | Draft |
| 010 | [Code Interpreter and Shell](010-Python-Code-Interpreter.md) | Interpreter + CodeAct + Shell sharing one execution context, runtime selection and its evidence, workspace, packages, the `call_tool` bridge | Draft |
| 011 | [MCP Conformance](011-MCP-Conformance.md) | MCP `2026-07-28` client + server: stateless core, MRTR, tools/resources/prompts, extensions, authorization | Draft |
| 012 | [A2A Conformance](012-A2A-Conformance.md) | A2A v1.0: agent card, task lifecycle, bindings, delegation | Draft |
| 013 | [UI and Streaming Surfaces](013-UI-and-Streaming-Surfaces.md) | The internal run event stream and its projections (AG-UI, A2A, SSE) | Draft |
| 014 | [Workflow and Orchestration](014-Workflow-and-Orchestration.md) | Typed executor graph, supersteps, patterns, checkpointing, time-travel | Draft |
| 015 | [Declarative Agent Format](015-Declarative-Agent-Format.md) | YAML/JSON agents and workflows; the equivalence rule | Draft |
| 016 | [Observability](016-Observability.md) | OTel GenAI conformance, traces, metrics, content-capture privacy, audit vs telemetry | Draft |
| 017 | [Safety and Content Governance](017-Safety-and-Content-Governance.md) | Prompt injection defence in depth, filters, PII, attack-class table | Draft |
| 018 | [Identity, Authorization and Secrets](018-Identity-Authorization-and-Secrets.md) | Principals, admission vs effect authorization, secret seam, multi-tenancy | Draft |
| 019 | [Durability and Long-Running Agents](019-Durability-and-Long-Running-Agents.md) | Checkpoints, suspension, exactly-once effects, recovery, retention | Draft |
| 020 | [Configuration and Hosting](020-Configuration-and-Hosting.md) | Policy vs configuration, hosting shapes, server surfaces, operations | Draft |
| 021 | [Platform Support and Portability](021-Platform-Support-and-Portability.md) | Support tiers, per-subsystem portability risk, build matrix | Draft |
| 022 | [Testing and Evaluation](022-Testing-and-Evaluation.md) | Deterministic simulation, golden traces, evaluation, positive controls | Draft |
| 023 | [Performance Targets and Budgets](023-Performance-Targets-and-Budgets.md) | Budget classes, provisional numbers, machine-independent invariants | Draft |
| 024 | [Versioning, Compatibility and Governance](024-Versioning-Compatibility-and-Governance.md) | Versioning, deprecation, the decision process, RFC hygiene | Draft |
| 025 | [Worktree and Virtual Filesystem](025-Worktree-and-Virtual-Filesystem.md) | The session's virtual disk: content-addressed objects, sub-worktrees, sharing and merge, mounts, per-turn commit | Draft |
| 026 | [Agent-Facing Runtime Surface](026-Agent-Facing-Runtime-Surface.md) | What the model sees: the ordinary environment, plain Python, and the `agent` library that *is* CodeAct's action space | Draft |
| 027 | [Vocabulary and Naming](027-Vocabulary-and-Naming.md) | The canonical name for every concept, aligned to MAF, plus the collision register for words that mean different things in different specs | Draft |
| 028 | [Bulk Data Transfer and Zero-Copy](028-Bulk-Data-Transfer-and-Zero-Copy.md) | Moving large data without JSON, without copies, and without touching the context window: handles, Arrow, per-profile mapping | Draft |
| 029 | [Memory System](029-Memory-System.md) | Memory as a principal-scoped worktree, not a wrapped vector database: structured `MemoryItem`s, attributed extraction, deterministic default retrieval, vectors as an optional upgrade | Draft |

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
suite on a named platform). Nothing is above Draft today, and promotion happens by executing a
gate — not by consensus that a document reads well.

## Relationship to Quark

Quark is consumed as an **unmodified submodule**. AgentEngine maps its concepts onto Quark's
(sessions are actors, runs are asks, streams are credit-rings, durability is the `Store` seam) and
never patches it in-tree; runtime changes go upstream as Quark RFCs. Quark's 28 specs and 36 ADRs
are an asset precisely because they describe the code that actually runs.

## Contributing

Read [CONVENTIONS.md](CONVENTIONS.md) first. Every change must follow the specs; **when code and a
spec disagree, the spec wins** — if the spec is wrong, fix the spec first, backed by an ADR, then
the code. Contested, hot-path, or security-critical designs go through
`design → red-team → prove → judge` and produce an ADR, not an ad-hoc change.

## Licence

Not yet decided ([024 Q1](024-Versioning-Compatibility-and-Governance.md)); MIT is the working
assumption, matching Quark.
