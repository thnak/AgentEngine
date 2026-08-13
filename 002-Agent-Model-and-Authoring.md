# 002 — Agent Model and Authoring

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 001, 003, 004, 006, 015 · **Gate:** §8

## Goal

Define what an agent *is* to the engine, and the C++ surface an author writes. The shape is
MAF's — an agent with instructions, a model, tools, and middleware, run against a session — and the
mechanism is AgentEngine's own zero-cost CRTP policy idiom (historical: originally modeled on
Quark's identical idiom; ADR-037 removed Quark as a runtime dependency, but the authoring style it
inspired is unchanged): **CRTP policy template parameters resolved to metadata at startup, with no
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
        Capabilities<NetOut<"api.search.example">>,
        SandboxProfile<Strict>,
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

`session.run<A>()` calls `AgentSession::start_run<A>()`, an `rt::task<result<AgentResponse>>`
coroutine (001 §1; historical: this was `Ask<StartRun,...>` to a Quark session actor before
ADR-037 removed Quark as a dependency). The typed `SessionRef` keeps the same always-typed,
never-stringly discipline (historical: originally Quark's `ActorRef<A>` discipline).

## 3. The policy vocabulary

**Naming:** the binding tag is `ChatClientId<"...">`, not `ChatClient<"...">` — `ChatClient` is
004's name for the backend interface itself (a concept, not a class template), and a concept and a
class template cannot share one identifier in the same namespace. `ChatClientId` selects which
`ChatClient` an agent binds to; the two are related, not interchangeable.

| Policy | Meaning | Default |
|---|---|---|
| `ChatClientId<"vendor:model">` | Model backend binding (004); overridable per run and by config | required |
| `Tools<Ts...>` | Declared tool set | empty |
| `SandboxProfile<P>` | Isolation profile for this agent's sandboxed effects (008) | `Strict` |
| `Capabilities<Cs...>` | Capability ceiling (007) | empty |
| `MaxTurns<N>` | Turn-loop bound | 16 |
| `TokenBudget<N>` | Cumulative token ceiling per run | unbounded |
| `Approval<Mode>` | `NeverRequire` / `AlwaysRequire` / `PolicyDriven` | `PolicyDriven` |
| `Concurrency<Mode>` | Tool-batch concurrency (001 §4) | `Sequential` |
| `Retry<Policy>` | Transient-failure retry shape | bounded exponential |
| `Memory<Ms...>` | Memory/context providers (005) | none |
| `Middleware<Ms...>` | Ordered middleware chain (§5) | none |
| `Telemetry<Capture>` | `MetadataOnly` / `WithContent` / `Off` (016) | `MetadataOnly` |
| `Stateless<N>` | Agent holds no cross-run state (historical: hosted as a Quark pool before ADR-037 removed Quark; the `rt::` hosting mechanism for this policy is not yet re-specified) | off |
| `OutputSchema<T>` | Structured-output contract (003) | free text |

**What `SandboxProfile<P>` governs.** It is not one dial over every sandboxed effect an agent can
reach: 009 §6 hardcodes plugins to run in the `wasm` profile regardless of what an agent declares
here, and 008 §1/CLAUDE.md permanently lock the code interpreter to `native-jail` — neither is
redirected by this policy. `SandboxProfile<P>` selects the backend for the agent's *other*
script-executing tools: a custom `Tool` that needs sandboxed execution but is neither a plugin (009)
nor the built-in interpreter (010) runs under the profile declared here, or is rejected at
`register_agent()` (§6) if its own backend requirement is incompatible with it. See 008 §3 for the
profile table this selects among.

`P` is any type satisfying `SandboxBackend` (008 §2), or the resolution selector `Strict` (008 §3's
"the strongest profile available on this platform, never `none`") — never an enum-like value; a
deployer's own custom backend type works exactly the same way an engine-shipped one does (008 §2a).
`decisions/ADR-012-sandbox-profile-template-parameter-kind.md` is the record of this decision.

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
    ae::task<std::monostate> before_model(ModelCallContext& c);
    ae::task<std::monostate> after_model(ModelCallContext& c);
};
```

(Hook return type corrected 2026-08-11, ADR-033: `ae::task<>` — i.e. `task<void>` (historical: at
the time of ADR-033 this was `quark::task<void>`; ADR-037 later removed Quark, and `ae::task<T>` is
now a plain `agentengine::rt::task<T>` alias, `core/task.hpp`) — has no `await_resume()` and cannot
be `co_await`ed by another coroutine at all; only `task<T>` for `T != void` (ADR-047) is genuinely
awaitable, the same distinction `core/agent_session.hpp`'s `run_rounds` already had to correct for.
A middleware hook is `co_await`ed by the chain runner, so it must return `task<std::monostate>`,
matching that established codebase convention.)

**Constraints:** middleware may inspect, annotate, rewrite content, short-circuit with a result, or
deny — it may **not** widen capabilities (**I2**, no ambient authority; 007 §3.2's attenuation-only
rule is the same constraint applied to this call site), and its effects are attributed to it by
name in the trace. Deny-capable middleware is how content policy (017) plugs in without the core
knowing about content policy.

**A sharper form of the capability-widening rule, found during ADR-033's own red-team pass and worth
stating explicitly: rewriting CONTENT can be a capability-reach change in effect, even though it
mints no new `Capability`.** A `ToolCall` content item defaults to `call_provenance::vendor_structured`
— the fully-trusted class `invoke_tool`'s step 5 checks only against the target tool's own
`approval_mode` (007 §4 amendment, ADR-023). Middleware that rewrites `ChatResponse.message` can
fabricate or mutate a `ToolCall`, laundering an untrusted decision through the same channel a
genuine, vendor-returned call travels — the confused-deputy shape ADR-023 already closed once for
raw model text, relocated to a middleware content rewrite. The model-call implementation (below)
closes this by forcing any `ToolCall` not identical to one the real backend actually returned to
`call_provenance::text_derived` before ever returning it — routing it through ADR-023's stricter
gate, which overrides even a tool's own `never_require` for anything with a real capability ceiling
or a non-pure effect class.

**Implementation status (updated 2026-08-12, ADR-033/ADR-036):** the MODEL-CALL interception point is
real: `middleware.hpp`'s `ModelCallContext`/`run_before`/`run_after`/
`enforce_backend_tool_call_provenance` run a two-phase (`before_model` forward, `after_model`
backward) chain, with compile-time hook detection (a middleware may define any subset of the two
hooks) and the provenance-downgrade fix above. The real consumer of this machinery is
`MiddlewareModelCallGateway<Inner, Ms...>` (`core/model_call_gateway.hpp`, ADR-036), wrapping any
`ModelCallGatewayLike` `Inner` — typically `ModelCallGateway<Primary, Fallback...>` (ADR-036's own
retry+circuit-breaking+failover type), composed as `MiddlewareModelCallGateway<ModelCallGateway<
Primary, Fallback...>, Ms...>` so the middleware chain runs exactly once per logical call, reviewing
only the final, post-retry outcome (the same reasoning that originally justified a strict outer/inner
composition order). This supersedes ADR-033's original consumer, `MiddlewareChatClient<Inner, Ms...>`
(a `ChatClient`-conforming decorator composed over `ResilientChatClient<Inner>`) — both, along with
`FailoverChatClient`, were REMOVED 2026-08-12: this repo had shipped nowhere, so there was no
deprecation-then-migration cost to justify keeping the three `chat()`-only wrappers once
`ModelCallGateway`/`MiddlewareModelCallGateway` gave `AgentSession` a real, streaming-capable
successor with the identical combined-not-layered reasoning. The RUN/TURN/TOOL-CALL interception
points remain declared-but-unwired — `AgentSession`'s turn loop
is large, mature, and heavily tested; threading a middleware chain through it is separately-scoped,
larger work this pass does not attempt, matching this project's own established "prove the mechanism
against one real consumer, name the rest" precedent (`decisions/ADR-028-session-scoped-stateful-tools.md`).
MAF's verified decorator-chain mechanics remain the prior art for that future work — a *separate*
wrapping pipeline around the raw model call from the one around the whole run/turn (`AIAgentBuilder`),
both composed by reverse-apply ordering so the first-registered middleware ends up outermost —
`docs/research/2026-08-11-maf-middleware-codeact-skills-deep-dive.md` §1.

## 6. Metadata, validation, and startup

At `register_agent<A>()` the engine compiles metadata and **validates**, failing fast on:

- a tool whose schema does not compile or whose name collides;
- a capability referenced by a tool but absent from the agent's ceiling;
- a `SandboxProfile` unavailable on this platform with no declared fallback (008);
- a declared tool requiring a backend incompatible with the agent's `SandboxProfile<P>` (§3) — a
  tool/profile mismatch distinct from the platform-unavailability case above, since the declared
  profile may otherwise be perfectly available;
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

- ~~**Q1** — Should `instructions` support a typed template with compile-time-checked placeholders,
  or remain a runtime-assembled string? Typed is safer; it complicates the declarative parity.~~
  **Resolved, No, stay a runtime-assembled string (2026-08-04):** nothing else in this spec currently
  needs `instructions` to be a typed template — the §2 example uses a plain literal, and dynamic
  content already has a designed path through middleware's `before_model` hook (§5) without
  `instructions` itself becoming a template type. Paying I6's declarative-parity cost (a
  compile-time-checked C++ type needs a *second*, runtime-checked validation path for YAML, per §6's
  existing dual-path pattern for `OutputSchema`/tool schemas) for a feature nothing currently demands
  is exactly the speculative-abstraction cost this project's own discipline argues against. If
  placeholder-safety becomes a real, demonstrated problem later, the cheaper fix is a **runtime-only**
  validation at `register_agent()` (checking a string's declared placeholders are satisfiable,
  reusing §6's existing validation-at-load-time mechanism) — addable without breaking existing C++
  agents and without inventing a new compile-time-checked type or paying I6's parity cost twice.
- ~~**Q2** — Per-run policy override: which policies may a caller override at `run()` time
  (`ChatClientId` and `MaxTurns` clearly; `Capabilities` clearly not) — the full matrix needs writing.~~
  **Resolved by a test, not a hand-written matrix (2026-08-04):** the question only feels like it needs
  a per-policy matrix because it was framed as "list every row." The two invariants already governing
  every other override surface in this project answer it directly — 007 §3.2's attenuation-only rule
  and 020 §1's "configuration may never widen" rule, applied here for the first time to call-site
  overrides: **a per-run override may only move a policy in the direction that reduces authority,
  cost, or blast radius relative to the agent's compiled default, never increase it.** Applying that
  test across §3's table:
  - **Never overridable** — `Capabilities`: it's the ceiling every other check is measured against;
    narrowing happens through derivation (007 §2), not a caller request naming a smaller set.
  - **Narrowing-only** — `SandboxProfile` (stricter, never looser — matches 020 §1's floor rule
    exactly), `MaxTurns`/`TokenBudget` (lower, never above the compiled ceiling), `Approval` (more
    cautious, never less), `Concurrency` (force-sequential, never force-parallel beyond what the tool
    set already declared safe), `Memory` (disable a declared provider, never add one), `Telemetry`
    (reduce capture, never increase it beyond the agent's/operator's declared posture, 016/017 §5).
  - **Freely overridable** — `ChatClientId` (already stated overridable; bounded to an operator-
    declared allowlist of acceptable substitutes, since it's a cost/quality choice, not an authority
    one) and `Retry` (touches only `Transient`-classified retries, §6's failure classification is a
    hardcoded rule the retry policy can't reach around, so it's a genuine 020 §1 "configuration" knob
    — it doesn't change what the agent would do given the same input, only how reliably).
  - **Not a runtime axis** — `Tools`, `Middleware`, `Stateless<N>`, `OutputSchema`: compile-time
    template parameters with no runtime override mechanism to design in the first place.
- ~~**Q3** — Whether `Stateless<N>` agents should be the default for tool-only agents.~~ **Resolved,
  No, keep off as the default (2026-08-04):** "default for tool-only agents" isn't implementable as
  stated — whether an agent touches session state isn't generally decidable without the same static
  analysis this question would need to invent, so a conditional default can't exist; the real choice
  is only "should the *global* default flip to on." Flipping it is a regression risk with no
  compensating benefit: §6 already rejects `Stateless<N>` combined with session-state usage, but only
  when `Stateless<N>` is written explicitly — if it became the default, an author who never wrote it
  and later adds session-state usage to a previously tool-only agent would hit that rejection out of
  nowhere, for a policy they never asked for. `Stateless<N>` has no authority implication (007's
  threat model doesn't touch hosting shape), so the safer route to the same performance win is a
  **lint**, not a default: `register_agent()` validation (§6) can suggest `Stateless<N>` when it can
  statically prove no session-state usage, leaving the conservative off-by-default unchanged.
