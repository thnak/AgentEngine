# ADR-226 — The run bridge: a declared agent becomes a running session

**Status:** **Proposed (2026-10-01).** Design (§2–§3), one adversarial self red-team pass (§4, 11
findings, 6 fixed in the design or code, 5 disclosed residuals), implemented and proven (§5). Judge is
the project owner's.

**Relates to:** GitHub issue #46 (and issue #32's "Layer A", which it was split from).
`002-Agent-Model-and-Authoring.md` §2.1 (amended by this ADR), §3, §6, §9 Q2.
`015-Declarative-Agent-Format.md` §1 (I6). ADR-059 (the ceiling is attenuated, never granted), ADR-074
and the quickstart builder's findings 9/11 (a move-only, fail-closed context slot), ADR-058 (output
schema enforcement), ADR-070 (`PolicyDecider`), ADR-037 (no actor engine). New:
`include/agentengine/core/agent_session_bridge.hpp`, `tests/core/agent/test_agent_session_bridge.cpp`.
Changed: `core/agent_registry.hpp` (`validate_agent_metadata()`, `agent_output_schema_t`),
`core/session_builder.hpp` (guard narrowed, `Bundle::run()`, shared `install_deciders()`),
`examples/01_hello_agent.cpp`.

## 1. The question

`Agent<Derived, Policies...>` and `register_agent<A>()` are real: the second compiles and validates the
first into an `AgentMetadata`. Nothing consumed that metadata. Running a turn meant constructing
`rt::AgentSession<ChatClientT, ...>` by hand, or a quickstart builder — neither reads an agent's
declaration, so the host re-derived the policy set by hand, and nothing checked that the session it
built was the agent it declared. The question, stated so it has a wrong answer:

> How does a declared agent — native or declarative — become a running session whose enforced policy
> set is exactly the declared one, with no authority the caller did not pass?

Wrong answers this ADR rejects:

- **Grant the ceiling.** `CapabilitySet::grant_root(meta.capability_ceiling)` is the one-liner, and it
  is ADR-059's bug again: the agent's declaration would mint authority the caller never held (I2).
- **Build 002 §2.1 as written** (`Engine{config}.register_agent<A>(); engine.start();
  engine.create_session(id).run<A>(text)`). Three independent problems, any one sufficient:
  1. `engine.start()` started a Quark actor engine. ADR-037 removed it; `rt::` has no engine to start.
  2. `create_session()` then `run<A>()` decouples the session from its agent. The session's held
     authority, tool set, approval floor and budgets would become per-RUN choices over one shared
     history — a run of agent B would see tool results agent A's (different) ceiling produced, and the
     session would have to type-erase `ChatClientT` because the agent, not the session, names the
     model. Neither is wanted: a session is one agent's conversation (I1).
  3. An `Engine` that loads credentials from a config file and hands them to whichever agent runs next
     holds authority on no one's behalf — ambient authority (I2). The caller has to pass it.
  So the spec was wrong, not the code: §2.1 is amended (§6) rather than built.
- **A function over `AgentMetadata` alone, with no `ChatClientT`.** The session type is
  `AgentSession<ChatClientT, ...>`; the client type cannot come from a runtime string. It comes from
  the caller's already-constructed client, which is also the right owner of the client's credentials.

## 2. Design

One bridge over the compiled metadata **value**, serving both surfaces (I6):

```cpp
template <class ChatClientT>   // ChatClient or ModelCallGatewayLike
result<AgentBundle<ChatClientT>> bind_agent_session(AgentMetadata const&, ChatClientT client,
                                                    AgentSessionOptions opts = {}, registries...);
template <class A, class ChatClientT>
result<AgentBundle<ChatClientT>> make_agent_session(ChatClientT client, AgentSessionOptions opts = {},
                                                    registries...);
```

`make_agent_session<A>()` = `register_agent<A>()` (all 002 §6 checks, including the one only the C++
type permits — `Stateless<N>`) + the bridge + the one thing only the type can supply: a typed validator
for `OutputSchema<T>`. `bind_agent_session()` is how a 015 document compiled by
`compile_agent_document()` runs. Both call `validate_agent_metadata()` (new, `agent_registry.hpp`) on
every metadata they bind — 002 §6's "validation is the same code path for declarative agents", which
was not true before (no caller of `compile_agent_document()` validated its output).

`AgentSessionOptions` is everything the **caller** supplies, all host code (I3): session id, principal,
`grants` (narrowed to the ceiling), `chat_client_grants` (the client's own `cap::Secret` credentials,
nothing else), narrowing-only `max_turns`/`token_budget` overrides, and the same `approve_tools`/
`policy` deciders the quickstart builders take.

**Reuse, not a parallel path.** The result is the quickstart `Bundle` (`core/session_builder.hpp`) with
`AgentToolSurface<>` in its context-provider slot: the same heap-owned store/capabilities/session with
the same destruction order, the same `ask()`/`ask_stream()` driven by `rt::block_on()` (ADR-175), plus a
new `Bundle::run()` returning the whole `AgentResponse` (002 §2.1's non-streaming form). To make that
possible in the default build, `session_builder.hpp`'s `AGENTENGINE_WITH_HTTPS` guard now covers only
what names a real backend (`QuickstartSessionBuilder`, `ComposedQuickstartSessionBuilder` and their
`detail` helpers); `Bundle` and `RawQuickstartSessionBuilder` were never backend-specific. The decider
installation the three builders repeated verbatim is one `detail::install_deciders()`, used by the
bridge too.

**`AgentToolSurface<Inner>`** — a `ContextProvider` wrapping `Inner` (unbounded history by default) that
appends the agent's declared descriptors to every contribution. Being the context slot is what makes
the declared set reach every place the session asks for tools: each round's request, the turn
middleware, and `resume_tool_table()`'s narrowing on an approval resume (ADR-196), with no change to
`AgentSessionCore`. Bound once, after the session exists (`history_provider_` is always
default-constructed — the reason `ComposedContextProvider::engage()` exists).

## 3. Field by field: what the session enforces, and what it only records

| `AgentMetadata` field | At run time | Proof (§5) |
|---|---|---|
| `agent_instructions` | First system text: `set_static_instructions(instructions + capability summary)` — host-authored, untainted. | P1 |
| `tools` | `AgentToolSurface` offers exactly these; a call to any other name fails `unknown tool` in the ordinary pipeline. | P2, P3 |
| `capability_ceiling` | Session holds `narrow_to_ceiling(grants, ceiling)` + `chat_client_grants` (secrets only). A declared tool whose capability was not granted is denied (`capability not held`). | P4, P11 |
| `max_turns` | `initialize(..., max_turns)`; always bounded (metadata default 16). Override may only lower; higher is refused (`agent_session.override_widens`), not clamped. | P5 |
| `token_budget` | Same, `initialize(..., token_budget)`. | P6 |
| `approval` | A floor: `always_require` raises every declared descriptor to `always_require`. `policy_driven` (default) and `never_require` leave each tool's declaration as written — an agent cannot loosen a tool (002 §9 Q2, "more cautious, never less"). | P7 |
| `concurrency` | `sequential` (default) clears `parallelizable`/`exclusivity_group` on the descriptors, so no batch fans out; `parallel` keeps each tool's own declaration (001 §4: both axes must allow it). | P8 |
| `output_schema_json` | `set_output_schema(json, select_output_schema_strategy(client.capabilities()), validator<T>)` on the native path. The metadata-only path has no `T` and no generic validator exists, so it **refuses** (`agent_session.output_schema_unvalidated`) rather than run unvalidated. | P9 |
| `chat_client_id` | Recorded (`metadata()["agent.chat_client_id"]`). The caller's client IS the binding — 002 §9 Q2 makes it freely overridable; with a `ChatClientRegistry` passed, its credential check runs. Not compared to the client instance (no general identity on `ChatClient`). | P12 |
| `telemetry` | Recorded only. No session-level telemetry consumer exists (016 exporter not built). | P12 |
| `sandbox_profile` | Recorded only; its availability check runs in validation when a `SandboxBackendRegistry` is passed. No session-level sandbox selection exists. | — |
| `stateless_pool_size` | Recorded only; no `rt::` pool hosting exists (002 §3's own table says so). | — |
| `agent_name`, `agent_version` | Recorded. | P12 |
| `Memory<>`, `Middleware<>`, `Retry<>` | Not compiled into `AgentMetadata` at all (agent.hpp: API surface only); nothing to bind. | — |

"Recorded" entries are plain strings in the session's metadata map; nothing reads them for a decision,
and they must never be used for one.

## 4. Red-team (adversarial self pass, against the first working version)

- **R1 (I2, design).** Granting the ceiling — rejected in §1. `narrow_to_ceiling()` keeps a grant only
  if the ceiling covers it, and a ceiling entry only if a grant covers it; every output entry is inside
  both. Proven by P4 (grants outside the ceiling → empty set; the ceiling alone is not authority) and by
  mutant M1 (pass grants through unnarrowed) being killed.
- **R2 (I2, FIXED).** The first version took `chat_client_grants` (deliberately un-narrowed, because a
  backend's API key is deployment configuration, not something the agent is — 002 §3) of any kind. That
  is a bypass channel: `cap::Schedule` or `cap::AgentCall` passed there would make the session offer
  `schedule_wakeup`/`agent.spawn` regardless of the ceiling. Now `cap::Secret` only; anything else is
  refused (`agent_session.chat_client_grant_not_secret`, P11, mutant M6 killed).
- **R3 (residual, disclosed).** Secrets in `chat_client_grants` sit in the session's held set, and
  `SecretStore::resolve()` checks the held set (`ctx.capabilities`), not a tool's per-call bound set. A
  declared tool that has its own reference to the same store object could resolve the client's key.
  Same property every quickstart builder already has (they put the key's grant in the held set);
  reaching it needs host code that hands a tool the store — not model output. Closing it needs secret
  resolution to check bound capabilities, a `trust/secret.hpp` change outside this ADR.
- **R4 (FIXED, `agent_registry.hpp`).** `output_schema_of` is last-tag-wins; the first draft of the
  type extractor was first-match. An agent declaring two `OutputSchema<>` tags would get the second's
  schema text validated with the first's type. `output_schema_type_of` is now last-wins too.
- **R5 (design).** Overrides above the declared bound: clamping silently would let a caller believe it
  got 10 turns when it got 3. Refused instead (P5, P6; mutants M7, M13 killed).
- **R6 (FIXED).** `AgentSession::clear_in_process_state()` resets the context slot to a default
  instance AND resets `max_turns`/`token_budget` to unbounded. An agent session reused after a clear
  (re-`initialize()`d) would have run with no tools and no budget — or, had the tool set lived anywhere
  that survives the clear, with tools and no budget (I8). An unbound `AgentToolSurface` now fails the
  round (`agent_tool_surface.not_bound`) before any model call (P13; mutant M5 killed).
- **R7 (FIXED).** If `Inner` contributes a tool with a declared tool's name, appending would offer two
  descriptors under one name and `ToolTable::find()`'s first match — the inner one — would dispatch. Now
  refused (`agent_tool_surface.name_collision`, P13; mutant M12 killed).
- **R8 (design).** Agent-level `Approval<never_require>` read as "un-gate every tool" would let an
  agent's declaration loosen a tool's own `always_require` — rejected; the agent mode is a floor (§3).
- **R9 (FIXED).** `fork_from()` copy-assigns the context slot but does not copy the source's
  capabilities, budgets or static instructions (`fork_core_from()`). A copyable surface carried
  `bound_ = true` into a fork with no turn bound — the declared tools, runnable unbudgeted, R6 by
  another door. `AgentToolSurface` is now move-only (copy deleted; `fork_from()` on an agent session no
  longer compiles — the ADR-074/finding-9 precedent), and a move leaves the source unbound (finding-11's
  lesson; P13, mutant M11 killed).
- **R10 (residual, disclosed — I6).** `compile_agent_document()` never fills `capability_ceiling`
  (`spec.capabilities` is unparsed, an open gap that compiler's own header names). A declarative agent
  whose tools need any capability therefore fails `bind_agent_session()` with
  `agent.capability_ceiling_exceeded` — the bind-time validation this ADR added is what makes that a
  refusal instead of a session whose tools are all denied at call time. The equivalence proven in P14 is
  therefore for capability-free tools; full I6 needs that compiler gap closed.
- **R11 (residual, disclosed).** No allowlist bounds which client may stand in for a declared
  `ChatClientId` (002 §9 Q2 asks for "an operator-declared allowlist of acceptable substitutes"). The
  caller constructing the client is the operator here; an allowlist is host policy this ADR does not
  invent.

## 5. Evidence

`tests/core/agent/test_agent_session_bridge.cpp` — every claim drives a real `start_run()` through the
bridge against the strict `testing::ScriptedChatClient` (no network; an unscripted model call fails the
test), each with a positive control:

- P1 instructions first; P2 exactly the declared tools offered; P3 a declared tool runs, an undeclared
  one fails and its `invoke` never runs; P4 narrowing (denied without the grant, runs with it, an
  out-of-ceiling grant never arrives, a tighter-capped grant is kept with its cap); P5 `MaxTurns<3>`
  stops a looping model at exactly 3 calls, override 1 takes effect, 10 is refused; P6 token budget
  (fails at 60/50, converges at 20, override 15 takes effect, 1000 refused); P7 agent `always_require`
  gates a `never_require` tool, `approve_tools` re-admits it, the same tool on a default agent runs
  ungated; P8 `Parallelizable` stripped on a sequential agent, kept on `Concurrency<parallel>`; P9 valid
  JSON → structured output, invalid → `run.output_schema_validation_failed`, metadata-only bind refuses;
  P10 missing `ChatClientId` refused, a hand-edited metadata re-validated at bind; P11 secrets only;
  P12 recorded fields; P13 cleared session, shadowed tool name, rebind, move-only, moved-from;
  P14 the 015 document equivalent of a native agent, compiled and bound by the same bridge, makes
  byte-identical model requests (messages and tool descriptors), stops at the same turn bound with the
  same error, holds the same authority, and denies identically.

Result (Windows, clang-cl 21 via the project's Debug Ninja build): **62/62 checks, `OK`**.

Mutation check (each mutant applied to `agent_session_bridge.hpp`, rebuilt, run; header restored from a
copy): M1 no narrowing, M2 no approval floor, M3 no sequential strip, M4 no bind-time validation, M5 no
`not_bound` guard, M6 no secret-only check, M7/M13 no override-widening checks, M8 tools not bound, M9
instructions dropped, M10 no output schema, M11 moved-from stays bound, M12 no name-collision check —
**15/15 killed** (1–13 failing checks each; M4 and M6 needed a second, warning-clean spelling to
compile under `-Werror`, then were killed too).

`examples/01_hello_agent.cpp` now declares `Joker : Agent<Joker, ChatClientId<...>, Tools<RollDie>,
MaxTurns<4>, TokenBudget<1'000>>` and runs it through `make_agent_session<Joker>()` offline: the fake
model calls the declared tool, then answers from its result (`ctest -R example_01_hello_agent`).

## 6. Decision and spec changes

- 002 §2.1 is replaced by the `make_agent_session<A>()` / `bind_agent_session()` shape, with the reasons
  `Engine`/`create_session`/`run<A>` is not built (§1). 002 §3's `Approval` row states the floor
  semantics.
- 002 §6's "same code path for declarative agents" is now true at bind time (`validate_agent_metadata`).
- Not changed: `AgentSessionCore` (no new session slot was needed), `compile_agent_document()` (R10 is
  its own gap), any RFC 015 text.

Residuals carried: R3, R10, R11 above; `Telemetry`/`SandboxProfile`/`Stateless` remain recorded-only
until a session-level consumer exists; `Memory<>`/`Middleware<>`/`Retry<>` are still not compiled into
metadata at all.
