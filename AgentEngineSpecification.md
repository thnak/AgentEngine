# AgentEngine Specification — vision, principles, invariants

**Status:** Draft · **Revision:** 0.1 · **Date:** 2026-07-31

AgentEngine is a **C++23 engine for building agent applications**: a runtime that hosts agents,
sessions, tools, and multi-agent workflows on top of the [Quark](https://github.com/thnak/QuarkCpp)
actor engine, with **untrusted code isolation and a Python code interpreter and shell as
first-class, built-in subsystems**, speaking the **open agent protocols of 2026** (MCP, A2A, AG-UI,
OpenTelemetry GenAI) rather than a proprietary surface.

Its developer model is deliberately **MAF-shaped** — the agent / session / tool / middleware /
graph-workflow vocabulary that Microsoft Agent Framework established — expressed in Quark's
**zero-cost CRTP policy** idiom instead of runtime configuration objects.

---

## 1. Why this exists

Every production agent platform re-solves the same five problems, badly and separately:

1. **Concurrency and lifecycle** — thousands of long-lived, stateful, resumable conversations,
   each of which must not race with itself. This is an *actor* problem, and it has already been
   solved: Quark.
2. **Isolation** — a model emits code and tool arguments that are, by construction, attacker
   influenced. Most frameworks bolt a sandbox on as an optional tool; it must be the substrate.
3. **Interoperability** — the protocol layer moved fast (MCP went stateless on 2026-07-28; A2A
   hit v1.0 in April 2026). A framework whose object model is not protocol-shaped is a migration
   treadmill.
4. **Observability and replay** — a run that cannot be replayed cannot be debugged, evaluated, or
   audited.
5. **Portability** — Windows and Linux, with the *same* isolation guarantees, not two different
   security stories. (macOS is explicitly out of scope — 021 §7 OQ-1, resolved 2026-08-04.)

AgentEngine's thesis: solve (1) by *not* solving it — layer on a proven actor engine — and spend
the entire design budget on (2)–(5).

## 2. Non-goals

- **Not a model.** AgentEngine never trains, quantizes, or serves weights. Inference is a seam.
- **Not a Python framework.** Python appears *inside the sandbox* as the interpreter language,
  never as the engine's implementation or its v1 authoring surface.
- **Not a UI.** AG-UI events are emitted; rendering is somebody else's job.
- **Not a cloud service.** Hosting profiles are specified; a hosted control plane is not.
- **Not a protocol author.** We conform to open standards; we do not invent competing ones.

## 3. Layering

```
 L4  Protocol surfaces     MCP server + client · A2A · AG-UI · OpenAI-compatible HTTP
 L3  Orchestration         workflow graph, handoff, group chat, checkpoint / resume / time-travel
 L2  Agent core            Agent · AgentSession · Tool plane · ChatClient plane · Middleware
 L1  Trust & isolation     capability model · sandbox seam · WASM component plugin ABI
 L0  Runtime substrate     Quark: scheduler, mailbox, cluster, persistence, timers, PAL
```

**The layering rule:** a layer may depend only on layers below it, and only through the seam that
layer publishes. L4 has no privileged access to L0. L1 is the *only* layer that may hold an OS
capability; everything above it holds handles.

## 4. Core invariants

These are the load-bearing statements of the whole system. Every RFC restates the subset it
touches; every one of them is a testable gate, not a slogan.

- **I1 — One session, one executor.** A `Session` is a Quark actor. Its message history and state
  are mutated by at most one executor at any instant, and turn order is the mailbox FIFO order.
  Inherited verbatim from Quark 001; AgentEngine adds no locking of its own.

- **I2 — No ambient authority.** Filesystem, network, clock-as-entropy, environment, subprocess,
  and secret access are reachable *only* through an explicitly passed capability handle. Code
  running in a sandbox begins with the empty capability set. There is no "allow by default" tier.

- **I3 — Model output is data, never authority.** Nothing a model emits — text, tool arguments,
  structured output, generated code — can widen a capability set, approve an action, or alter
  policy. Approval decisions are made by the host against a pre-registered policy, before the
  effect, never by a model and never by inference over model output.

- **I4 — Every effect is attributable.** Every tool invocation, sandbox execution, model call, and
  outbound protocol request carries `{run_id, session_id, agent_id, principal, capability_set,
  trace_context}` and emits both an OpenTelemetry span and an audit record. An effect that cannot
  be attributed is a bug, not a feature.

- **I5 — Nondeterminism crosses a recorded seam.** Model responses, clocks, RNG, sandbox results,
  and remote protocol calls each pass through a seam that can record and replay. A run is
  replayable from its recording without contacting any external service.

- **I6 — Declarative and native surfaces are equivalent.** The YAML/JSON agent definition and the
  C++ CRTP definition compile to the *same* metadata table and are validated by the same
  validator. Neither is a subset of the other; a feature that exists in only one is a defect.

- **I7 — Protocol conformance is a gate.** Each protocol RFC names an executable conformance suite
  and a protocol revision. "Supports MCP" without a passing suite at a named revision is not a
  claim this project makes.

- **I8 — Budgets are enforced, not documented.** Turn latency, sandbox cold start, per-session
  footprint, and engine overhead have numeric budgets (023) checked by a benchmark gate.

## 5. The three decisions locked before RFC-001

### D1 — Quark is a dependency, not a fork

Quark is consumed as an **unmodified submodule**. AgentEngine maps its concepts onto Quark's
(§7) and never patches Quark in-tree. If AgentEngine needs a runtime change, it goes upstream as a
Quark RFC + ADR. Rationale: Quark's 28 specs and 36 ADRs are an asset only while they describe the
code actually running; a fork forfeits them on day one.

### D2 — WASM Component Model is the plugin ABI; the sandbox is a seam with profiles

Two different problems get two different answers (008, 009, 010):

- **Plugins/extensions** — tools, skills, model providers, memory stores, content filters
  contributed by third parties — are **WASI 0.3 / Component Model components**, locked. One
  artifact runs bit-identically on Windows and Linux; capability-based by construction;
  language-agnostic; instantiation in microseconds. Ecosystem risk is low because *we* define the
  WIT world and the plugin author compiles to it.
- **The Python code interpreter and the shell** are `Runner`s (010 §1a) sharing one `ExecState` —
  a `cd` or an exported variable is visible to both. Python is **embedded native CPython,
  permanently — one runtime, never a WASM alternative** (010 §2), running under `native-jail` or
  cluster-managed under `remote`; there is no `microvm` profile (008 §1). **The shell is not a
  wrapped binary at all** — `ShellRunner` is engine-native code that never resolves a name against a
  search path; it dispatches to builtins over the worktree or to other registered `Runner`s and
  `Tool`s, so there is no ambient exec surface to sandbox in the first place (010 §2).

The ecosystem evidence is in [docs/research/2026-standards-landscape.md](docs/research/2026-standards-landscape.md)
and summarized in 010 §2: as of July 2026 the rich WASM Python ecosystem (NumPy, pandas, SciPy,
scikit-learn, plus PEP 783 `pyemscripten_*_wasm32` wheels published straight to PyPI) is
**Emscripten**-targeted and requires a JavaScript host, so it cannot be embedded in a C++ host via
wasmtime; while **WASI** CPython — the embeddable one — is governed and improving (PEP 816,
accepted, binding from Python 3.15) but still has no sockets, no threads, no wheel platform tag,
and no binary-wheel ecosystem. That evidence explains why a WASM Python costs nothing to *not* have
today — it is not the reason one Python runtime is the permanent decision. The reason is
architectural: two Python runtimes means an agent's generated code, and the humans verifying it,
must know which one they are dealing with, and a bug that reproduces on one and not the other is
the exact confusion the "ordinary environment" principle (026 §1) exists to prevent. Even a mature
WASI Python would still be a second runtime.

**What this buys:** the isolation *contract* (capabilities, limits, audit, replay) is the invariant;
the isolation **backend** for the interpreter can still evolve (008 is a seam), but the *interpreter
itself* does not fork into two. Isolation strength for CPython comes from 008 §1b — the sandbox is
the whole execution environment (worktree, `Runner`s, resource limits, capabilities, network policy,
tool registry), with CPython's own dangerous entry points (`open`, `socket`, `subprocess`, `ctypes`)
mediated at the point of use, backed by the OS-level jail as a second layer — not from swapping
CPython for a differently-capable interpreter.

### D3 — Authoring surfaces for v1: native C++ and declarative

v1 ships **C++23 CRTP-policy authoring** (002) and the **declarative YAML/JSON format** (015),
bound by I6. Python and .NET bindings are explicitly deferred, not designed out: the C ABI in 020
is the seam they will use — design starts against a small set of stable concepts now, freezing is
gated on a reference out-of-process consumer (020 §8 Q3, resolved 2026-08-04).

### D4 — Files belong to the worktree, not to the sandbox

Every session owns a **worktree** (025): a content-addressed virtual disk the engine manages,
durable through Quark's `Store` seam, mounted into sandboxes as ordinary directories. Multiple agents
in one session share it or branch from it with an explicit merge.

The reason this is a locked decision rather than an implementation detail: if files live inside the
sandbox, then persistence, crash survival, portability across profiles, shareability between agents,
auditability, and rewind all become properties of whichever isolation technology is selected.
Hoisting them into engine state makes file semantics **identical on every profile and every OS**, and
makes the sandbox genuinely disposable — which is what allows 008's backend choice to be a
performance-and-ecosystem decision rather than a data-loss decision.

The sandbox itself is **session-scoped** (008 §6): it lives with the session's actor, so a
conversation keeps its interpreter *and shell* state — working directory, environment variables,
in-memory objects — across turns (010 §3a). The isolation boundary that matters is *between
sessions*, and cross-session reuse is prohibited in every profile.

### D5 — CodeAct is the primary action space, and the environment is unremarkable

The agent's main way of acting is **writing a program** that calls a host-backed library
(`agent.tools`, `agent.files`, `agent.memory`, …), not emitting one tool-call JSON object per step
(026 §5). Control flow, filtering, and aggregation happen inside the sandbox; only results transit
the context window. The tool-call channel remains for single high-consequence actions, where an
approval over one call with concrete arguments is reviewable in a way that an approval over a whole
program is not.

That surface is **deliberately ordinary**: normal Python, normal paths, normal exceptions, and **no
prompt text explaining sandboxes, capabilities, or profiles** (026 §1). A model has seen millions of
lines of ordinary Python and none of our architecture; describing the architecture costs tokens on
every turn, competes with the actual task for attention, and does not make the model better at it.

**Worktree, shell, interpreter, and CodeAct are one feature, seen from four angles, not four
features that happen to cooperate.** The worktree (D4) is the one disk; the interpreter and the
shell are two `Runner`s (010 §1a) sharing one `ExecState` — the same `cwd` and environment (010
§3a); CodeAct is what that context is *for*. An agent that `cd`s in the shell, reads the result with
`open()` in Python, and calls a tool from either is operating one ordinary machine, not switching
between subsystems — that coherence, not any one mechanism alone, is what "unremarkable environment"
means. It is also why the shell is engine-native rather than a wrapped binary: a real shell's
name-to-binary resolution is exactly the kind of ambient authority **I2** rules out, so `ShellRunner`
dispatches only to capability-gated primitives and other `Runner`s/`Tool`s (010 §2) — more to build
than adopting an existing shell, and the difference between covering a gap and designing the system
properly.

**This is a prompt-surface decision and never a security mechanism.** Assume the model knows it is
isolated, assume an attacker tells it, assume it probes — 007 and 008 hold regardless. RFC 026 §8 G4
proves it by re-running the full hostile suite against an agent that has been given accurate
architecture detail: containment results must be identical.

## 6. Vocabulary

| Term | Meaning |
|---|---|
| **Agent** | A named, addressable unit that turns input into output using a model, tools, and instructions. Hosted as a Quark actor type. |
| **`AgentSession`** | The durable conversation state an agent operates on: message history, state bag, and the seam to persistence. One Quark actor instance per session. |
| **Run** | One invocation of an agent against a session, producing a response (possibly streamed) and zero or more effects. The unit of tracing, checkpointing, and replay. |
| **Turn** | One model call plus the tool invocations it triggers, inside a run. |
| **Tool** | A declared, schema-typed capability an agent may invoke. Backed by a native function, a WASM component, an MCP server, or a remote agent. |
| **Skill** | A bundle of instructions, resources, and optional scripts giving an agent a competence on demand. Format of record is `SKILL.md` (agentskills.io); skills that ship code are packaged as plugins. |
| **Worktree** | The session's virtual disk: a content-addressed object store plus a mutable tree, owned by the engine and mounted into sandboxes. Agents share it or branch from it. |
| **Capability** | An unforgeable handle authorizing one class of effect (a mounted path, an outbound host, a secret, a tool). Held by the host, passed explicitly, never inferred. |
| **Sandbox** | An isolation boundary instance with an attached capability set and resource limits. Created per execution; profiles select the backend. |
| **Profile** | A named sandbox configuration (`wasm`, `native-jail`, `remote`, `none`) resolved at startup to a backend + limits. |
| **Plugin** | A signed package containing one or more WASM components implementing a WIT world (tool, skill, provider, store, filter). |
| **Workflow** | A typed graph of executors (agents, functions, sub-workflows) with edges, checkpointing, and human-in-the-loop request points. |
| **`ChatClient`** | The seam to an inference API (OpenAI-compatible, Anthropic, local, hosted). Not named `Provider` — that word stays free for the colloquial "model vendor" sense (004, 027 §5). |
| **Principal** | The authenticated identity on whose behalf a run executes; propagated into every effect and every outbound protocol call. |

Quark's vocabulary (Actor, Activation, Worker, Shard, Mailbox, Policy, `ActorRef`) is used
verbatim where it appears; AgentEngine does not rename it.

## 7. The Quark mapping

| AgentEngine concept | Quark mechanism | Spec |
|---|---|---|
| `AgentSession` | Actor instance, keyed by `session_id`, `Sequential` | Quark 001/005 |
| Run | Ask-message to the session actor; `ask_stream` when streamed | Quark 006, ADR-018 |
| Streaming response | `ask_stream` reply-credit-ring | Quark 006/024 |
| Session history durability | `Store` seam — snapshot + event-sourced | Quark 012 |
| Worktree objects and refs | Same `Store` seam, content-addressed | Quark 012 |
| Live sandbox bound to a conversation | Sandbox owned by the session actor; passivation drives snapshot/teardown | Quark ADR-028/034 |
| Long-running / scheduled agents | Durable reminders (SEGSTREAM) | Quark 027 |
| Tool invocation | Message to a tool actor / stateless worker pool | Quark 025 |
| Multi-node agent placement | HRW / VirtualBins placement | Quark 010/026 |
| Cancellation & deadlines | `std::stop_token` + monotonic deadline propagation | Quark 018 |
| Backpressure on token streams | Credit-controlled inbound streams | Quark 024 |
| Failure isolation per run | Guarded handler core, supervision policies | Quark 007 |
| Deterministic replay | Simulation scheduler | Quark 014 |
| OS portability | PAL (`linux_x86_64`, `windows_x86_64` backends present) | Quark 019 |

**Consequence:** AgentEngine writes no scheduler, no mailbox, no cluster membership, no persistence
engine, and no timer wheel. Every one of those is a Quark seam it configures.

## 8. Reading order

Start with [README.md](README.md) for the RFC index, then [CONVENTIONS.md](CONVENTIONS.md) — the
binding coding contract — then the RFCs in numeric order. Load-bearing first:
001 (execution), 002 (agent model), 006 (tools), 007 (capabilities), 008 (sandbox),
009 (plugins), 010 (code interpreter), 011 (MCP).

## 9. Maturity ladder

Every RFC carries a status. The ladder is deliberately harsher than "we wrote it down":

| Status | Means |
|---|---|
| **Draft** | Design written; open questions unresolved. Default for everything in revision 0.1. |
| **Reviewed** | Open questions resolved or explicitly deferred; interfaces stable enough to implement against. |
| **Proven** | The RFC's named gate has been executed — real code, real measurements — and recorded as an ADR in [`decisions/`](decisions/). |
| **Accepted (platform)** | Proven *and* implemented *and* covered by the conformance/correctness suite on a named platform. |

No RFC in revision 0.1 is above **Draft**. Promotion happens by executing the gate the RFC names,
in the `design → red-team → prove → judge` loop inherited from Quark.
