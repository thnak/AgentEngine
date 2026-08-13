# AgentEngine Specification — vision, principles, invariants

**Status:** Draft · **Revision:** 0.1 · **Date:** 2026-07-31

AgentEngine is a **C++23 engine for building agent applications**: a self-contained runtime that
hosts agents, sessions, tools, and multi-agent workflows on plain C++23 objects and coroutines
(`agentengine::rt::`, ADR-037 — no actor engine underneath), with **untrusted code isolation and a
Python code interpreter and shell as first-class, built-in subsystems**, speaking the **open agent
protocols of 2026** (MCP, A2A, AG-UI, OpenTelemetry GenAI) rather than a proprietary surface.

Its developer model is deliberately **MAF-shaped** — the agent / session / tool / middleware /
graph-workflow vocabulary that Microsoft Agent Framework established — expressed in AgentEngine's
own **zero-cost CRTP policy** idiom (originally modeled on the [Quark](https://github.com/thnak/QuarkCpp)
actor engine's identical idiom, before ADR-037 removed that dependency) instead of runtime
configuration objects.

---

## 1. Why this exists

Every production agent platform re-solves the same five problems, badly and separately:

1. **Concurrency and lifecycle** — thousands of long-lived, stateful, resumable conversations,
   each of which must not race with itself. A single-process agent SDK does not need a distributed
   actor engine to guarantee this (ADR-037): a plain object plus a runtime-checked async mutex per
   session (`rt::AsyncMutex`) is enough, and it is what every comparable framework — Microsoft Agent
   Framework, the OpenAI Agents SDK, Anthropic's own agent patterns — actually ships.
2. **Isolation** — a model emits code and tool arguments that are, by construction, attacker
   influenced. Most frameworks bolt a sandbox on as an optional tool; it must be the substrate.
3. **Interoperability** — the protocol layer moved fast (MCP went stateless on 2026-07-28; A2A
   hit v1.0 in April 2026). A framework whose object model is not protocol-shaped is a migration
   treadmill.
4. **Observability and replay** — a run that cannot be replayed cannot be debugged, evaluated, or
   audited.
5. **Portability** — Windows first, Linux next once Windows is stable, with the *same* isolation
   guarantees on each, not a different security story per OS. macOS is not a target (021 §2/§7).

AgentEngine's thesis: (1) does not need a distributed actor engine's worth of machinery for a
single-process SDK — a small, self-contained runtime substrate (`agentengine::rt::`) covers it —
freeing the design budget for (2)–(5).

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
 L0  Runtime substrate     agentengine::rt::: coroutine tasks, AsyncMutex, ThreadPool, channel/stream,
                           SessionStore/AppendLogStore, a vendored socket PAL (ADR-037)
```

**The layering rule:** a layer may depend only on layers below it, and only through the seam that
layer publishes. L4 has no privileged access to L0. L1 is the *only* layer that may hold an OS
capability; everything above it holds handles.

## 4. Core invariants

These are the load-bearing statements of the whole system. Every RFC restates the subset it
touches; every one of them is a testable gate, not a slogan.

- **I1 — One session, one executor.** A `Session`'s message history and state are mutated by at
  most one executor at any instant, and turn order is FIFO. Enforced by `rt::AsyncMutex`, acquired
  for the whole duration of every public async entry point (`start_run`, `resolve_interaction`) —
  a runtime-checked guard, not a mailbox's structural exclusivity (ADR-037 §5's own named
  narrowing: a new entry point that forgets to acquire it would reintroduce the exact race a
  mailbox used to make unreachable by construction).

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

## 5. Decisions locked before RFC-001

### D1 — Quark is a dependency, not a fork (SUPERSEDED by ADR-037)

Originally: Quark consumed as an **unmodified submodule**, never patched in-tree, with AgentEngine
mapping its own concepts onto Quark's. **ADR-037 (`decisions/ADR-037-remove-quark-as-core-runtime.md`,
executed 2026-08-13) overturned this decision entirely** — the project owner's reasoning was that an
agent engine should provide an API to build AI systems the way Microsoft Agent Framework, the OpenAI
Agents SDK, and Anthropic's own agent patterns do, none of which ship a custom actor/distributed
runtime; carrying one as the foundation clutters the implementation and confuses the purpose of what
an agent engine is for. `third_party/quark` is no longer a submodule of this repository; AgentEngine's
runtime substrate is now `agentengine::rt::` (§7). Kept here, marked superseded rather than deleted,
so the historical record of why the *original* choice was made is not lost.

### D2 — WASM Component Model is the plugin ABI; the sandbox is a seam with profiles

Two different problems get two different answers (008, 009, 010):

- **Plugins/extensions** — tools, skills, model providers, memory stores, content filters
  contributed by third parties — are **WASI 0.3 / Component Model components**, locked. One
  artifact runs bit-identically on Windows and Linux (the targeted OSes, 021 §2); capability-based
  by construction;
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
durable through `rt::AppendLogStore` (§7), mounted into sandboxes as ordinary directories. Multiple
agents in one session share it or branch from it with an explicit merge.

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
| **Agent** | A named, addressable unit that turns input into output using a model, tools, and instructions. Expressed as an `agentengine::rt::AgentSession<ChatClientT, StateT, HistoryProviderT>` CRTP instantiation. |
| **`AgentSession`** | The durable conversation state an agent operates on: message history, state bag, and the seam to persistence. A plain, host-held C++ object — one instance per session, no actor engine managing its lifecycle. |
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

## 7. The runtime substrate

AgentEngine was originally built on top of Quark, a distributed actor engine (mailbox dispatch,
cluster placement, `Sequential` policy, snapshot/event-sourced persistence) — the vocabulary that
motivated this document's own earlier title for this section, "The Quark mapping." **ADR-037
(`decisions/ADR-037-remove-quark-as-core-runtime.md`) removed that dependency entirely**: the
project owner's own reasoning was that an agent engine should provide an API to build AI systems,
the way Microsoft Agent Framework, the OpenAI Agents SDK, and Anthropic's own agent patterns do —
none of which ship a custom actor/distributed runtime — not carry a distributed-actor-engine's own
concerns baked into its foundation. `third_party/quark` is no longer a submodule of this repository.

AgentEngine's runtime is now the `agentengine::rt::` namespace — plain, host-held C++23 objects and
coroutines, no actor engine underneath:

| AgentEngine concept | `rt::` mechanism | Header |
|---|---|---|
| `AgentSession` | A plain templated class instance, one per session, no actor lifecycle | `rt/agent_session.hpp` |
| Run | `AgentSession::start_run()`, an `rt::task<result<AgentResponse>>` coroutine | `rt/agent_session.hpp` |
| Streaming response | `agentengine::stream<T>`, a credit-controlled producer/consumer pair over `rt::channel<T,E>` | `core/stream.hpp`, `rt/channel.hpp` |
| Session/workflow durability | `rt::SessionStore` (single-slot, overwrite-latest) and `rt::AppendLogStore` (append-only, multi-version) — two distinct contracts for two distinct needs, not one seam doing both | `rt/session_store.hpp`, `rt/append_log_store.hpp` |
| Worktree objects and refs | `rt::AppendLogStore`-backed `Ref` history (content-addressed Blob/Tree storage is, and always was, independent of any actor engine) | `core/worktree.hpp` |
| Multi-agent workflows | `WorkflowSupervisor`, a plain object driving a superstep loop via `rt::ThreadPool` for real concurrent fan-out | `rt/workflow_supervisor.hpp`, `rt/thread_pool.hpp` |
| Tool invocation | An ordinary (possibly coroutine) function call through `tool_pipeline.hpp`'s invoke path — no message send | `core/tool_pipeline.hpp` |
| Cancellation & deadlines | `std::stop_token` + monotonic deadline propagation (unchanged — this was never actor-specific) | throughout |
| Failure isolation per unit of work | `rt::ThreadPool::submit()`'s `JobOutcome{faulted, fault_ptr}` containment, not actor-restart supervision | `rt/thread_pool.hpp` |
| One-session-one-executor (I1) | `rt::AsyncMutex`, acquired for the whole duration of every public async entry point — a runtime-checked guard, not a mailbox's structural exclusivity | `rt/async_mutex.hpp` |
| OS portability (sockets) | `agentengine::pal` — a small, self-contained, vendored socket PAL (the one slice of Quark's own PAL this project ever used) | `pal/net.hpp` |

**Consequence:** AgentEngine writes no scheduler, no mailbox, no cluster membership, no distributed
persistence engine, and no timer wheel — none of that ever needed reproducing, because none of it
was load-bearing for a single-process agent SDK in the first place. What *is* reproduced, narrowly,
is the thin slice of real capability the actor engine happened to also provide: `rt::AsyncMutex` for
I1 in place of mailbox exclusivity, `rt::ThreadPool` for genuine concurrent fan-out in place of
per-actor scheduling, and `agentengine::pal` for portable sockets in place of Quark's own PAL. A few
real, permanent narrowings resulted and are named as such, not silently smoothed over: there is no
host-managed passivation/reactivation of a session across process restarts or node loss in `rt::`
land (ADR-037's own completion note, §9, names the five test files retired as an accepted gap rather
than ported), and there is no multi-node cluster placement story at all (AgentEngine was already a
single-process SDK in practice before this migration — §2 of ADR-037 audited zero real footprint
from Quark's cluster/transport machinery going in).

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
