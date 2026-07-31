# 002 — Agent Model and Authoring

**Status:** Draft · **Depends on:** 001, 003, 004, 006, 015 · **Gate:** §8

## Goal

Define what an agent *is* to the engine, and the C++ surface an author writes. The shape is
MAF's — an agent with instructions, a model, tools, and middleware, run against a session — and the
mechanism is Quark's: **CRTP policy template parameters resolved to metadata at startup, with no
runtime configuration objects and no reflection**.

## 1. What an agent is

An agent is a **type** carrying:

- **identity** — stable `agent_id`, name, description, version;
- **instructions** — system-prompt content, static or assembled per run;
- **a provider binding** — which model, with which options (004);
- **a tool set** — declared, schema-typed (006);
- **a capability set** — the ceiling of what any of its effects may reach (007);
- **policies** — turn limits, budgets, sandbox profile, concurrency, retry, approval mode;
- **optional handlers** — hooks into the turn loop.

The compiled form of all of the above is the **agent metadata table**, built once at startup and
thereafter read-only. **I6** requires that the declarative format (015) produces the *same* table.

## 2. The authoring surface

```cpp
#include "agentengine/core/agent.hpp"

using namespace ae;

struct Researcher : Agent<Researcher,
        ChatClientId<"anthropic:claude-opus-5">,
        Tools<WebSearch, CodeInterpreter, Handoff<Writer>>,
        SandboxProfile<Profile::Strict>,
        MaxTurns<12>,
        TokenBudget<200'000>,
        Approval<Mode::PolicyDriven>,
        Telemetry<Capture::MetadataOnly>> {

    static constexpr std::string_view name = "researcher";
    static constexpr std::string_view instructions =
        "Research the question. Cite sources. Hand off to the writer when you have enough.";

    // Optional hooks — omit any of them and the default applies at zero cost.
    ae::task<> on_run_start(RunContext&);
    ae::task<> on_turn(TurnContext&);                 // override the default turn loop
    ae::task<ToolDecision> on_tool_call(ToolCallContext&);
    ae::task<> on_run_end(RunContext&);
};
```

**Policies are types, not values.** `MaxTurns<12>` is a template parameter that lands in
`.rodata` metadata, not a field read on the hot path — the same reasoning as Quark's `Priority<P>`
and `DrainBudget<N>` (Quark 005, ADR-007/008). A policy that is not specified is not "defaulted at
runtime"; the default is a compile-time selection with no branch.

**Hooks are detected, not virtual.** Presence is a concept check; an omitted hook compiles to
nothing. There is no vtable, no RTTI, and no null check per turn.

### 2.1 Running an agent

```cpp
Engine engine{EngineConfig::from_file("agentengine.toml")};
engine.register_agent<Researcher>();
engine.start();

auto session = engine.create_session("user-42");

// non-streaming
result<AgentResponse> r = block_on(session.run<Researcher>("Compare WASI 0.2 and 0.3."));

// streaming — credit-controlled, backpressure to the provider
auto stream = session.run_stream<Researcher>("Compare WASI 0.2 and 0.3.");
while (auto update = block_on(stream.next())) { render(*update); }
```

`session.run<A>()` posts an `Ask` to the session actor (001 §1). The typed `SessionRef` is Quark's
`ActorRef<A>` discipline: always typed, never stringly.

## 3. The policy vocabulary

**Naming:** the binding tag is `ChatClientId<"...">`, not `ChatClient<"...">` — `ChatClient` is
004's name for the backend interface itself (a concept, not a class template), and a concept and a
class template cannot share one identifier in the same namespace. `ChatClientId` selects which
`ChatClient` an agent binds to; the two are related, not interchangeable.

| Policy | Meaning | Default |
|---|---|---|
| `ChatClientId<"vendor:model">` | Model backend binding (004); overridable per run and by config | required |
| `Tools<Ts...>` | Declared tool set | empty |
| `SandboxProfile<P>` | Isolation profile for this agent's sandboxed effects (008) | `Profile::Strict` |
| `Capabilities<Cs...>` | Capability ceiling (007) | empty |
| `MaxTurns<N>` | Turn-loop bound | 16 |
| `TokenBudget<N>` | Cumulative token ceiling per run | unbounded |
| `Approval<Mode>` | `NeverRequire` / `AlwaysRequire` / `PolicyDriven` | `PolicyDriven` |
| `Concurrency<Mode>` | Tool-batch concurrency (001 §4) | `Sequential` |
| `Retry<Policy>` | Transient-failure retry shape | bounded exponential |
| `Memory<Ms...>` | Memory/context providers (005) | none |
| `Middleware<Ms...>` | Ordered middleware chain (§5) | none |
| `Telemetry<Capture>` | `MetadataOnly` / `WithContent` / `Off` (016) | `MetadataOnly` |
| `Stateless<N>` | Agent holds no cross-run state; hosted as a Quark pool | off |
| `OutputSchema<T>` | Structured-output contract (003) | free text |

**The rule for adding a policy:** a knob belongs here only if it changes *what the agent is*.
Knobs that change *how the deployment runs* (worker counts, endpoints, timeouts, credentials) are
configuration (020), never policies. This is Quark's policy-vs-config boundary (Quark 013),
applied one layer up.

## 4. Agent composition

Three composition modes, all first-class:

1. **Handoff** — `Handoff<Writer>` exposes another agent as a tool that *transfers* the run.
   Control moves; the session continues; the target agent sees the history per its own policy.
2. **Sub-agent (agent-as-tool)** — the target runs as an independent run on its own session and
   returns a result. Isolation of history and capability subsetting are the point (001 §4).
3. **Workflow** — for anything with structure (parallelism, conditionals, fan-in, checkpoints),
   agents become nodes in a graph (014). Handoff chains longer than a few hops should be a
   workflow; the spec does not forbid it, but the guidance is explicit.

**Remote agents compose identically.** An A2A peer (012) or an MCP server exposing an agent-like
tool (011) is wrapped as a tool or a handoff target with the same declaration syntax. An author
should not be able to tell from the call site whether the callee is in-process.

## 5. Middleware

An ordered chain wrapping the turn loop, with four interception points: **run**, **turn**,
**model call**, **tool call**. Middleware is a type with any subset of hooks; the chain is
assembled at compile time.

```cpp
struct RedactPii {
    ae::task<> before_model(ModelCallContext& c);
    ae::task<> after_model(ModelCallContext& c);
};
```

**Constraints:** middleware may inspect, annotate, rewrite content, short-circuit with a result, or
deny — it may **not** widen capabilities (I3/I7 of 007), and its effects are attributed to it by
name in the trace. Deny-capable middleware is how content policy (017) plugs in without the core
knowing about content policy.

## 6. Metadata, validation, and startup

At `register_agent<A>()` the engine compiles metadata and **validates**, failing fast on:

- a tool whose schema does not compile or whose name collides;
- a capability referenced by a tool but absent from the agent's ceiling;
- a `SandboxProfile` unavailable on this platform with no declared fallback (008);
- a `ChatClientId` binding with no configured credentials or endpoint;
- an `OutputSchema` the bound `ChatClient` cannot enforce and no fallback strategy;
- a handoff cycle without a bound;
- `Stateless<N>` combined with session-state usage.

**Validation is the same code path for declarative agents** (015) — a YAML agent that would fail
here fails at load with the same diagnostic. This is the enforcement mechanism for I6.

## 7. Versioning and identity

An agent's identity for interop is `{agent_id, version}`. Changing instructions, tool set, or
output schema **is** a version change; the engine records the metadata digest in every run's trace
so a behavioural change is attributable to a version rather than to a mystery. A2A Agent Cards
(012) and MCP tool listings (011) are generated from this same metadata.

## 8. Promotion gate

- **G1** — objdump/parity: an agent declared with all-default policies produces a turn loop with no
  additional indirect calls or branches versus a hand-written loop (the ADR-007 zero-cost standard).
- **G2** — a YAML agent and its hand-written C++ equivalent produce **byte-identical metadata
  tables** (I6, enforced as a test, not a review).
- **G3** — validation rejects each of the §6 defect classes with a specific diagnostic, proven by a
  negative test per class.
- **G4** — handoff, sub-agent, and remote-agent composition are indistinguishable at the call site
  in a compile-time test.

## 9. Open questions

- **Q1** — Should `instructions` support a typed template with compile-time-checked placeholders,
  or remain a runtime-assembled string? Typed is safer; it complicates the declarative parity.
- **Q2** — Per-run policy override: which policies may a caller override at `run()` time
  (`ChatClientId` and `MaxTurns` clearly; `Capabilities` clearly not) — the full matrix needs writing.
- **Q3** — Whether `Stateless<N>` agents should be the default for tool-only agents.
