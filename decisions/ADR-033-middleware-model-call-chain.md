# ADR-033 — `Middleware<Ms...>`: the model-call interception point

**Status:** Judged, accepted (2026-08-12, by the project owner). Designed, red-teamed (independent
pass via the `Agent` tool, findings in §3), implemented, and proven (real code + tests, §4).
`MiddlewareChatClient`, this ADR's original accepted consumer, was removed 2026-08-12 and superseded
by `MiddlewareModelCallGateway` (ADR-036) — part of what was judged, not left outstanding; see §5a.

**Relates to:** `002-Agent-Model-and-Authoring.md` §5 (the RFC this closes one interception point
of); `decisions/ADR-023-response-format-codec-seam.md` (007 §4's `call_provenance` amendment — the
mechanism this ADR's fatal-finding fix routes middleware-introduced tool calls through, unmodified);
the (now-removed, see §5a) `ResilientChatClient<Inner>` decorator-wrapper idiom this design originally
copied the shape of; `docs/research/2026-08-11-maf-middleware-codeact-skills-deep-dive.md` §1 (the
MAF prior art this design's ordering convention is grounded in); `decisions/ADR-036-model-call-
gateway.md` (Judged — this ADR's machinery's current real consumer).

## 1. The question

002 §5 defines `Middleware<Ms...>` — "an ordered chain wrapping the turn loop, with four
interception points: run, turn, model call, tool call" — and states its own implementation status
plainly: "currently a declared CRTP policy tag with no chain-composition logic or consumer yet."
Confirmed by grep: `include/agentengine/core/agent.hpp`'s `Middleware<Ms...>` is a genuinely empty
tag, zero consumers anywhere in `src/`/`include/`.

**Stated so it has a wrong answer:** can the model-call interception point be turned into real,
composed, tested code using the SAME decorator-wrapper shape this codebase already uses for
`ResilientChatClient<Inner>` — no virtual base, no new core-seam change — while genuinely satisfying
§5's stated constraint that middleware "may not widen capabilities"?

## 2. Scope, narrower than "implement Middleware<Ms...>" sounds

Only the MODEL-CALL interception point is wired to a real consumer this pass:
`MiddlewareChatClient<Inner, Ms...>` (`core/middleware.hpp`), a template decorator conforming to the
`ChatClient` concept itself. The run/turn/tool-call points are **not** wired into `AgentSession`'s
turn loop — that file is large, mature, and heavily tested; threading a middleware chain through its
template parameters and every call site is separately-scoped, larger work, matching this project's
own established "prove the mechanism against one real consumer, name the rest"
precedent (`decisions/ADR-028-session-scoped-stateful-tools.md`, which proved its own mechanism
against a synthetic test tool rather than wiring real CodeAct in the same pass). `chat_stream()` is
forwarded uninterceped — 004 §1's streaming path is a fundamentally different shape (a
`stream<ChatResponseUpdate>`, not an awaitable `task<result<ChatResponse>>`) that needs its own
design.

## 3. Red-team findings and how each is addressed

An independent red-team pass (via the `Agent` tool, dispatched against the design before any code
was written) found one fatal, exploitable flaw and three must-fix gaps.

| # | Finding | Severity | Fix |
|---|---|---|---|
| 1 | A `before_model`/`after_model` hook can freely rewrite `ChatResponse.message.content`, which may contain `ToolCall` items. A `ToolCall` defaults to `call_provenance::vendor_structured` — the FULLY TRUSTED class `invoke_tool`'s step 5 checks only against the target tool's own `approval_mode` (which may be `never_require`). A middleware that fabricates or mutates a `ToolCall` therefore LAUNDERS an untrusted decision through the same channel a genuine, vendor-returned call travels — exactly the confused-deputy shape `decisions/ADR-023...` already forced closed once for raw model text, relocated to a middleware content rewrite. Traced end to end through real file:line evidence, not theoretical. | **FATAL** | `middleware_detail::enforce_backend_tool_call_provenance`, called once on the FINAL response before this wrapper ever returns it: any `ToolCall` not byte-identical (call_id+tool_name+arguments, ignoring provenance/origin) to one the REAL backend call actually returned is forced to `call_provenance::text_derived` — routing it through ADR-023's stricter gate (`is_auto_declassifiable_text_derived_call`), which overrides even a tool's own `never_require` for anything with a real capability ceiling or non-pure effect class. A `before_model` short-circuit (the real backend is never called — `raw_backend_response` stays `nullopt`) downgrades EVERY `ToolCall` in its fabricated response, since none of it can claim vendor trust. Proven in `test_middleware_chat_client.cpp` T10 (fabrication), T12 (mutation of a genuine call), with T11 as the mandatory positive control (an untouched genuine call KEEPS `vendor_structured` — the mechanism is targeted, not a blanket downgrade). |
| 2 | §5's own text — "its effects are attributed to it by name in the trace" — had no wiring at all in the first draft; the design read as though this constraint were simply dropped rather than deliberately deferred. | Must-fix | Every `Middleware` type with at least one hook must declare `static constexpr std::string_view name` (checked via `HasMiddlewareName`, a `static_assert` with a clear diagnostic on failure — this codebase does not use typeid/RTTI to fabricate a name, per its own no-RTTI rule). `MiddlewareTraceEvent{name, hook, settled_here, threw}` fires through an optional, caller-injected `MiddlewareTraceHook` (nullptr by default), mirroring `workflow/supervisor.hpp`'s `WorkflowSupervisor::CheckpointHook` idiom exactly — no trace sink exists anywhere in this codebase yet (ADR-031 §3's own precedent for naming this rather than building one), so a host that wants real durability/OTel export closes over its own sink in the hook. Proven in T14. |
| 3 | Composition order with `ResilientChatClient<Inner>` (already-shipped retry+circuit-breaking wrapper) was unspecified, and the two orders are NOT interchangeable: composing `MiddlewareChatClient` INSIDE `ResilientChatClient` would re-run the whole before-phase under one retry-stable idempotency key on every attempt (a hook with any per-call variance breaks the key's "stable" contract), and an `after_model` hook that synthesizes a fallback response in place of a real transient failure would make `ResilientChatClient` observe a false success and silently corrupt its circuit breaker's health tracking. | Must-fix | Documented as a mandatory caller contract (cannot be compile-time enforced — both wrappers have the identical generic `Inner`-wrapping shape): `MiddlewareChatClient` must be the OUTER layer, i.e. `MiddlewareChatClient<ResilientChatClient<Real>, Ms...>`, never the reverse. Stated in both `core/middleware.hpp`'s own top comment and 002 §5's updated implementation-status text. |
| 4 | Hook exception semantics were undefined — every other control-flow surface this design touches (`ChatClient::chat()`, `invoke_tool()`) uses `result<T>`/`error` discipline, never a thrown C++ exception crossing the boundary; nothing said what happens when a hook merely has a bug and throws. | Must-fix | A throwing hook is caught (both `std::exception` and a catch-all), converted to `error{failure_class::fatal, ..., "middleware.hook_threw"}`, and treated as settling the chain (the before-phase stops there; the after-phase still unwinds through whichever middlewares already ran). Never escapes `chat()` as a raw exception. Proven in T9. |

One finding was independently confirmed rather than treated as new: 002 §5's own illustrative
snippet writes `ae::task<>` as the hook return type — confirmed directly against
`quark/core/task.hpp` that `task<void>` implements NO co_await protocol at all (only `task<T>` for
`T != void` does, ADR-047), so that snippet is unbuildable as literally written. The accepted design
uses `task<std::monostate>` throughout (matching `agent_session.hpp`'s own `run_rounds` precedent for
the identical correction), and 002 §5's text is corrected to match rather than left silently
diverging.

Two findings were confirmed as non-flaws during red-team and are recorded for completeness rather
than fixed: the "stopped_at unwinds only through middlewares that got a turn, including the settling
one" semantics are sound and match a genuine nested-decorator's own behavior (traced directly, not
merely asserted); the narrowed model-call-only scope does not violate any of §5's or §8's own text
(none of the promotion-gate items require all four interception points simultaneously).

## 4. The accepted design

- **`ModelCallContext`** (`core/middleware.hpp`): `{ChatRequest request; std::optional<ChatResponse>
  response; std::optional<error> failure;}`. Deliberately carries NEITHER `EffectContext&` nor any
  capability-related type — the I2 enforcement mechanism: a hook structurally cannot widen (or even
  read) a capability, because it is never handed one. `response`/`failure` are the SAME fields used
  both for "a `before_model` hook short-circuits" and "an `after_model` hook reviews the real
  outcome" — one context object, no separate pair to keep in sync.
- **Hook detection**: `HasBeforeModel<M>`/`HasAfterModel<M>` via `requires`-expressions — a
  middleware type may define any subset of the two hooks, including neither (proven in T15).
- **Chain composition**: `Ms...` template-argument order is registration order, position 0 is
  OUTERMOST (matches MAF's "first-registered ends up outermost" convention). Before-phase iterates
  forward, stopping at the first settlement; after-phase iterates backward, but only through
  middlewares that actually got a turn during the before-phase (an onion unwind, symmetric with a
  real nested-decorator chain — including giving the settling middleware itself an after-phase turn).
  Proven in T6 (full chain) and T8 (mid-chain short-circuit).
- **`MiddlewareChatClient<Inner, Ms...>`**: owns `Inner inner_` and `std::tuple<Ms...> middlewares_`,
  constructed via the wrapper's own constructor (runtime instances, never default-constructed by the
  chain — a middleware needing configuration is built by the caller and handed in). `capabilities()`
  forwards unchanged; `chat_stream()` forwards uninterceped (§2).

## 5. What this ADR does not claim

- **No wiring into `AgentSession`'s turn loop** — run/turn/tool-call interception points are declared
  vocabulary only, same as before this ADR, for the model-call point specifically now backed by real
  code (§2).
- **No streaming interception** — `chat_stream()` is forwarded untouched (§2).
- **Composition order with `ResilientChatClient` is a documented caller contract, not a compile-time
  enforced one** — both wrappers share the identical generic shape, so nothing statically distinguishes
  "wrapped the right way" from "wrapped the wrong way" (finding #3).
- **No real trace sink** — `MiddlewareTraceHook` is a real, structured, optional callback; nothing in
  this codebase yet exists to feed it into (finding #2's own scoping).
- **§5's `run`/`turn` context types (`RunContext`/`TurnContext`) are not defined at all** — this ADR
  does not attempt API-completeness scaffolding for interception points it does not wire; a future
  pass building those adds their vocabulary alongside real consumers, not ahead of them.

## 5a. Amendment (2026-08-12): MiddlewareChatClient removed, superseded by MiddlewareModelCallGateway

`MiddlewareChatClient<Inner, Ms...>` — this ADR's accepted consumer of the model-call machinery below
— was REMOVED 2026-08-12, together with `FailoverChatClient`/`ResilientChatClient` (ADR-036 §7's own
residual, closed by that removal): this repo had shipped nowhere, so there was no deprecation-then-
migration cost to justify keeping three `chat()`-only wrapper templates once `ModelCallGateway`/
`MiddlewareModelCallGateway` (ADR-036) gave `AgentSession` a real, streaming-capable path with the
identical composition-order reasoning §5's "Composition order with `ResilientChatClient`" residual
above named. `MiddlewareModelCallGateway<Inner, Ms...>` is the current real consumer — it reuses
`ModelCallContext`/`middleware_detail::run_before`/`run_after`/`enforce_backend_tool_call_provenance`
(§4 below) VERBATIM, unchanged by the removal; only `MiddlewareChatClient` itself is gone. This ADR's
own regression suite (T1-T15, §6) survives: ported to `tests/test_middleware_model_call_gateway.cpp`
(was `test_middleware_chat_client.cpp`), same 15 checks, same meaning, retargeted at the new wrapper.

## 6. Falsifiable claims and verdicts

`tests/test_middleware_chat_client.cpp`, 15 blocks / 30 checks, each tracing to a specific design
claim or red-team finding.

| # | Claim | Verdict |
|---|---|---|
| T1 | Zero middleware forwards to `Inner` and returns its response unchanged. | **CORRECT** |
| T2 | `before_model` rewrites the request; `Inner` receives the rewritten version, not the original. | **CORRECT** |
| T3 | A single middleware's `before_model` then `after_model` both run, in that order, around a real call. | **CORRECT** |
| T4 | `before_model` short-circuiting means `Inner` is NEVER called. | **CORRECT** |
| T5 | `before_model` denying returns the error verbatim; `Inner` is never called. | **CORRECT** |
| T6 | Three middlewares, no short-circuit: before runs A,B,C; after runs C,B,A. | **CORRECT** |
| T7 | A middleware defining only `after_model` still runs exactly once on the ordinary path. | **CORRECT** |
| T8 | A mid-chain short-circuit: the third middleware's before never runs; only the first (before the settling one) gets a before+after turn. | **CORRECT** |
| T9 | A throwing hook is caught, converted to a `result<T>` failure with a stable code, never escapes as a raw exception. | **CORRECT** |
| T10 | (FATAL FINDING FIX) A middleware-fabricated `ToolCall` is forced to `text_derived`. | **CORRECT** |
| T11 | (positive control) A genuine, untouched backend `ToolCall` KEEPS `vendor_structured`. | **CORRECT** |
| T12 | A middleware-MUTATED genuine `ToolCall`'s arguments also loses trust — mutation, not just addition. | **CORRECT** |
| T13 | `capabilities()` forwards to `Inner` unchanged. | **CORRECT** |
| T14 | The trace hook fires with correct name/hook/settled attribution. | **CORRECT** |
| T15 | A hookless middleware compiles and is inert. | **CORRECT** |

Full regression suite: **185/186** pass. The one failure, `test_mediated_python_runner_hostile_corpus`,
is the same pre-existing, unrelated failure tracked separately (backlog item #38) — untouched by any
file this ADR changes. **Resolved (2026-08-11, backlog item #38):** not a flake — two real,
deterministic test-authoring bugs, both fixed the same day; see ADR-024 §6's own corrected note.

## 7. Files changed

- `include/agentengine/core/middleware.hpp` (new) — `ModelCallContext`, `MiddlewareTraceEvent`/
  `MiddlewareTraceHook`, hook-detection concepts, the before/after chain runners,
  `enforce_backend_tool_call_provenance`, `MiddlewareChatClient<Inner, Ms...>`.
- `tests/test_middleware_chat_client.cpp` (new) — this ADR's §6 evidence.
- `tests/CMakeLists.txt` — registers the new test target.
- `002-Agent-Model-and-Authoring.md` §5 — hook signature corrected (`task<>` → `task<std::monostate>`),
  the confused-deputy-shaped capability-widening note added, implementation status updated to name
  what is real now and what remains declared-only.
