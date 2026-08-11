# ADR-028 — Session-scoped stateful tools, the general mechanism

**Status:** Judged, accepted (2026-08-11). Designed, red-teamed, implemented, and proven (real code
+ deterministic tests, §5); accepted by the project owner per this project's governance
(`decisions/README.md`; `OpenQuestions.md` OQ-11's resolution that the project owner is the ADR
judge).

**Relates to:** `decisions/ADR-027-agent-session-tool-call-loop.md` (this ADR closes one of its two
named out-of-scope residuals — session-scoped stateful tools; suspend-for-human approval remains
separately open), `decisions/ADR-024-skill-scoped-tool-and-mount-wiring.md` §8 (the exact gap this
ADR closes: "`Tool<>::invoke()` is a static call with only `EffectContext&` — no way to reach
`AgentSession`-owned `StateT`"), `decisions/ADR-002-pythonrunner-embedding-and-mediation.md` §5.5.6/
§6 item 5 (the separate, still-open question this ADR deliberately does not touch — see §6).

## 1. The question

`Tool<Derived, Policies...>::invoke(Args, EffectContext&)` (`include/agentengine/core/tool.hpp`) is
a static function with no path to reach its owning `AgentSession`'s session-scoped data. Confirmed
this session, exhaustively: no hook exists anywhere in the type-erasure/dispatch chain
(`ToolDescriptor::InvokeFn` → `make_tool_descriptor<ToolT>()` → `invoke_tool()`/`background_task()`)
for a runtime payload beyond `EffectContext&`, and `EffectContext` itself carries no generic/opaque
slot. This blocks any tool needing session-scoped mutable state (a persistent Python interpreter's
exec state, mounted-skills tracking) from working correctly with multiple concurrent production
sessions — today it only works via process-global function-local statics in `tools/cli_chat.cpp`,
already named in ADR-024 §8 as "correct for this CLI's single-process scope, not a general solution
for a multi-session production `AgentSession`."

**Stated so it has a wrong answer:** does closing this gap require a new type-erasure mechanism
(a `void*`/`std::any`-shaped slot, tagged for safe recovery), or can it be closed entirely within
this codebase's existing conventions?

## 2. Prior art surveyed

Exhaustive search this session confirmed: **this codebase has never used `std::any`,
`std::type_index`, `typeid`, or `dynamic_cast` anywhere** — both AgentEngine and vendored Quark
explicitly avoid RTTI/reflection on principle (`include/agentengine/project/lifecycle.hpp`'s own
header comment states this as a verified negative while solving a structurally similar problem: a
type-erased, caller-declared `AgentSession<C,S,H>` instantiation needing a uniform interface). The
one real precedent for erasing a concrete type behind a uniform interface,
`lifecycle.hpp`'s `PassivatableHandle`, works by capturing a closure **at the point where the
concrete type is still statically known** (construction time) — never by storing an opaque pointer
and downcasting it later at an unrelated call site. `tool_pipeline.hpp`'s own
`make_tool_descriptor<ToolT>()` already uses the identical idiom for `ToolT::Args`/`Reply`.

## 3. The design considered and rejected

**First draft:** extend the `ContextProvider` concept with an optional, compile-time-detected
`on_context(SessionContext&, EffectContext&, StateT&)` overload, letting `AgentSession::handle()`
thread its own `StateT state_` into a state-aware `HistoryProviderT`.

**Rejected by red-team**, on the strongest possible grounds: it solves a problem that doesn't need
solving. `HistoryProviderT history_provider_` is already a per-`AgentSession`-instance member
(`agent_session.hpp`) — a state-aware provider can already own its own session-scoped state as
ordinary member variables, exactly the pattern `SkillsProvider`/`HistoryAndSkillsProvider` already
establish (`history_and_skills_provider.hpp`'s own comment: "a stateful provider's `on_turn_end`...
must persist that state across turns, not be reconstructed per call"). No core-seam change needed
at all — `context_provider.hpp`, `agent_session.hpp`, and the `ContextProvider` concept itself are
**completely untouched** by the accepted design below. This is the same "purpose-built composite
over generic seam-widening" preference `OpenQuestions.md` OQ-18 already judged for a related
question earlier this session.

## 4. The accepted design

A state-aware `ContextProvider` builds a tool's `ToolDescriptor` via a new, additive factory
(`tool_pipeline.hpp`), still extracting `ToolT`'s compile-time-checked declarations
(`Capabilities<...>`/`Approval<...>`/`EffectClass<...>`/schemas — the real safety net a
hand-built-from-scratch descriptor would silently lose) but letting the caller supply the actual
invoke logic as a callable that captures whatever session state it needs:

```cpp
template <class ToolT, class InvokeFn>
[[nodiscard]] ToolDescriptor make_tool_descriptor_with_invoke(InvokeFn custom_invoke);
```

A provider calls, e.g., `make_tool_descriptor_with_invoke<CounterTool>([this](Args a, EffectContext&
ctx) { return this->real_invoke(a, ctx); })` from inside its own `on_context()` — `this` is the
provider itself, with full, ordinary, type-safe access to its own member state. `ToolT`'s own static
`invoke` never runs on this path.

### Must-fix (fatal red-team finding) — `captures_session_state` vs. `Backgroundable`

The red-team found a real dangling-reference/data-race hazard independent of which seam carries the
mechanism: `start_background_task()`/`background_task()` (`tool_pipeline.hpp`) already detaches a
real `std::thread` today. A state-capturing closure backgrounded this way would hold a reference into
session state with **no synchronization** against `AgentSession::fork_from()`/
`clear_in_process_state()` — neither is part of `protocol`, so neither is Quark-`Sequential`-
serialized against a detached thread. **Fixed structurally, not by convention**:
`ToolDescriptor` gains `bool captures_session_state = false;` (always `true` for
`make_tool_descriptor_with_invoke`, unaffected — stays `false` — for the existing
`make_tool_descriptor<ToolT>()` path), and `background_task()`'s existing authorize step now also
refuses `captures_session_state && backgroundable` with a new error code
(`tool.state_capturing_not_backgroundable`), checked before step 4/7, so the combination can never
reach step 8 on a detached thread.

## 5. Falsifiable claims and verdicts

| # | Claim | Evidence | Verdict |
|---|---|---|---|
| S1 | A stateful tool's state persists correctly across multiple rounds within one run. | `tests/test_session_scoped_stateful_tools.cpp` S1: two sequential tool calls (delta 3, then 4) against the same provider instance; round 2's observed total is 7 (3+4), not 4 — proving genuine accumulation, not independent calls. | **CORRECT** |
| S2 | Two independent `AgentSession` instances never see each other's state. | S2: two separate sessions, deltas 10 and 1; session B's total is 1, not 11 — proving isolation rather than asserting it (an accidentally-shared state would have produced 11). | **CORRECT** |
| S3 | `captures_session_state && backgroundable` is refused structurally, before step 8, with a specific error code. | S3: `background_task()` returns `unexpected` with `tool.state_capturing_not_backgroundable`; the completion callback never fires — no thread was ever detached. | **CORRECT** |
| S4 | The existing `make_tool_descriptor<ToolT>()` path is completely unaffected. | S4: `captures_session_state` defaults `false`; an ordinary stateless `Backgroundable` tool still backgrounds and completes successfully — no regression. | **CORRECT** |

## 6. What this ADR does not claim

- **Real CodeAct wiring** (`MediatedPythonRunner`/`MountedSkillsState`/`ExecState` owned by a
  state-aware provider, replacing `cli_chat.cpp`'s process-global statics) is separate, later work,
  decided in scope before design started — this ADR proves the general mechanism only, against a
  deterministic stateful counter tool, not the real interpreter.
- **`MediatedPythonRunner`'s one-interpreter-per-process constraint is untouched.** Confirmed this
  session (reading ADR-002 §5.5.6/§6 item 5 directly, not from memory): that limit is *this
  project's own security-mechanism design choice* (one global mutable allowlist slot + one
  pre-`Py_Initialize` audit hook, both keyed on ambient state) — not an absolute CPython law; ADR-002
  itself names CPython subinterpreters (`Py_NewInterpreterFromConfig`/`PyInterpreterConfig_OWN_GIL`,
  PEP 684) as a real, unresolved candidate, explicitly deferred as its own follow-up. Multiple
  concurrent CodeAct sessions using this ADR's mechanism would still share one process-wide
  interpreter's actual Python variable state until that separate, harder problem is solved. This ADR
  makes session-scoped **data** (not the interpreter itself) genuinely per-session-capable.
- **"Provider-owned state must never hold a `Capability`/`CapabilitySet`"** is a real invariant with
  no type-system enforcement (C++23 has no reflection to check an arbitrary struct's members) —
  recorded with the same prominence this project already gives its other unenforced-but-documented
  invariants (ADR-024's own "enforced by comment, not the type system" precedent for
  `skill_tool_scoping.hpp`), not silently assumed safe. Capabilities remain reachable only through
  the existing, unchanged `EffectContext::capabilities`/`bound_capabilities` mechanism — nothing
  about this ADR widens what a tool can reach in terms of authority, only what data it can reach.
- **`fork_from()`/`clear_in_process_state()` don't touch `history_provider_` at all today** — an
  existing, orthogonal gap (a provider's own state isn't copied on fork or wiped on delete), not
  newly introduced by this ADR. Named, not fixed here.

## 6a. Addendum (same day) — `fork_from()`/`clear_in_process_state()` now handle provider-owned state

§6's own residual ("`fork_from()`/`clear_in_process_state()` don't touch `history_provider_` at
all today") is closed, not left named. Re-reading `fork_from()`'s own prior comment showed this was
a *correct* decision at the time it was written (`history_provider_` was pure wiring, same category
as `chat_client_`/`capabilities_`, deliberately left untouched) — but ADR-028 changes what a
`HistoryProviderT` can hold, invalidating that categorization for a provider using
`make_tool_descriptor_with_invoke`. Fixed: `fork_from()` now copies `history_provider_` (requires
`HistoryProviderT` to be copy-assignable, checked only at this method's own instantiation — a
provider owning something genuinely non-copyable, e.g. a real interpreter, correctly fails to
compile here rather than silently forking an unsafe partial copy); `clear_in_process_state()` now
resets it to `HistoryProviderT{}`, closing a real "no residue" gap (005 §6) a state-carrying
provider would otherwise leave readable after a hard delete. Proven (S5,
`tests/test_session_scoped_stateful_tools.cpp`): a fork's next tool call correctly continues from
the source's copied counter value (not a stale reset), and a value written before
`clear_in_process_state()` does not survive it (the next call starts fresh, not from the deleted
session's leftover total).

## 7. Files changed

- `include/agentengine/core/tool_pipeline.hpp` — `ToolDescriptor::captures_session_state`,
  `make_tool_descriptor_with_invoke<ToolT>(InvokeFn)`, and the `background_task()` guard.
- `tests/test_session_scoped_stateful_tools.cpp` (new) — this ADR's §5 evidence.
- `tests/CMakeLists.txt` — registers the new test target.

Full regression suite: **179/180** tests pass; the one failure,
`test_mediated_python_runner_hostile_corpus`, is the same pre-existing, unrelated flake already
recorded in ADR-024 §6 and reconfirmed unaffected by ADR-027 — untouched by any file this ADR
changes.
