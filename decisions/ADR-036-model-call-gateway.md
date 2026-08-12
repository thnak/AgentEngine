# ADR-036 — A coroutine-based model-call gateway replaces `chat_stream()` wrapper-parity

**Status:** Proposed (2026-08-12). Designed, red-teamed twice (independent passes via the `Agent`
tool, general-purpose reviewer — §2 and §4), implemented, and proven (real code + a 20-check
deterministic offline test suite, §6); awaiting the project owner's explicit "Judged" sign-off per
this project's governance (`decisions/README.md`; `OpenQuestions.md` OQ-11's resolution that the
project owner is the ADR judge, never an AI).

**Relates to:** `decisions/ADR-035-chatclient-streaming-completeness.md` (Proposed — this ADR is
that one's superseded-and-replaced Phase 2; see that document's §4). `decisions/ADR-033-middleware-
model-call-chain.md` (Judged — `MiddlewareChatClient`'s `ModelCallContext`/`run_before`/`run_after`/
`enforce_backend_tool_call_provenance` machinery, reused here verbatim). `include/agentengine/core/
failover_chat_client.hpp`/`resilient_chat_client.hpp`/`middleware.hpp` (the three `chat()`-only
wrapper templates this ADR does not modify — see §7). `004-Model-Provider-Plane.md` §4 (Reliability
— failover/retry/circuit-breaking) and §5 (`TokenBudget<N>`).

## 1. The question

`FailoverChatClient`, `ResilientChatClient`, and `MiddlewareChatClient` each implement real
reliability/extensibility logic in `chat()` — failover across backends, retry with circuit-breaking,
a before/after middleware hook chain — but only a passthrough or partial equivalent in
`chat_stream()`, each file's own top comment naming the gap as a deliberate scoping decision. With
`chat()` no longer required by the `ChatClient` concept (`ADR-035`), and streaming established as
this project's default model-call shape, can these three wrappers be given genuine `chat_stream()`
parity — and if the naive approach (three separate wrapper templates, each independently patched)
turns out to be structurally unsafe, what actually works instead?

## 2. The first design, and why it was abandoned rather than patched

The original plan gave each of the three wrappers a real `chat_stream()`: internally drain the
underlying stream to completion via a shared buffer-then-decide primitive (unavoidable — a real
backend's failure can surface only *after* partial content has already been produced, so retrying or
failing over a partially-delivered stream would itself be the silent mid-stream backend substitution
004 §4 forbids), then either retry/fail over or replay the buffered result to the caller.

**An independent red-team pass found this design has a genuine, unresolved structural problem,
specific to the middleware piece, that the other two don't share.** `chat_stream()`'s literal
signature (004 §1) is a plain, non-coroutine function. `MiddlewareChatClient`'s `before_model`/
`after_model` hooks are arbitrary `task<std::monostate>` coroutines a middleware author writes —
there is no safe way to `co_await` one from inside a non-coroutine `chat_stream()`. The only
mechanism this codebase has for driving a `task<T>` outside a coroutine at all
(`tests/support/run_task_sync.hpp`'s "resume once" driver trick) is explicitly test-only, by its own
comment: "if the awaited task ever DID genuinely park, this would simply hang... never do this
outside a test." Investigating whether a safety net was possible — detect the violation after one
`.resume()` and fail closed rather than hang — the red-team confirmed even **"leak the frame instead
of destroying it"** is not actually safe: by reading `quark`'s own `ReplyCell`/`AskFuture`
implementation directly, a coroutine genuinely parked on a real async primitive (an actor `ask`) can
be *resumed later*, by whatever it awaited, on an unknowable future tick — and that resume would
touch stack-lifetime state (`ModelCallContext`, the middleware tuple) that `chat_stream()` already
returned and abandoned. Leaking doesn't neutralize the frame; it just defers the use-after-free to a
later, unpredictable moment.

`ResilientChatClient`'s retry classification also turned out weaker than assumed: `chat_stream()`'s
only failure-reporting channel is a `quark::error{errc, string_view}` — much coarser than `chat()`'s
own `ae::error::klass`-based classification — and the real `OpenAIChatClient` backend collapsed
*every* non-2xx HTTP status into one code on that path (`errc::validation`), meaning a naive
`quark::errc`-based retry policy would never actually retry a real 429/503 response, the exact case
retry-with-backoff exists for.

**Decision:** rather than patch around a confirmed use-after-free hazard with more defensive
machinery layered on the wrong abstraction, redesign at the right layer instead of the wrong one.
Two further pieces of project-owner direction shaped the redesign specifically: (1) don't force
retry/failover/middleware through each backend-wrapper's `chat_stream()` — treat `OpenAI`/
`Anthropic` as thin wire-protocol adapters (which, structurally, they already were) and centralize
model-call handling; (2) but don't fold that centralized logic into `AgentSession` either — it
already owns the turn loop, tool invocation, approval, budgets, history, and (per `ADR-035`) the
leak-scan; a single do-everything gateway class would just relocate the god-object problem the
second directive was explicitly against.

## 3. The accepted design

A new file, `core/model_call_gateway.hpp`, with **two small, separately-testable types** rather than
one that owns everything — the split exists specifically because a C++ class template can have only
one trailing parameter pack, so `Fallback...` (failover's backend list) and `Ms...` (middleware's
hook list) cannot both live on one template's parameter list even if that were otherwise desirable:

- **`ModelCallGateway<Primary, Fallback...>`** — retry (reusing `RetryPolicy` verbatim from
  `resilient_chat_client.hpp`) + circuit-breaking (reusing `BreakerConfig` verbatim, one REAL
  `quark::CircuitBreaker` PER backend — 004 §4/decision 6's breaker key is `{provider, model,
  secret}`, so a heterogeneous chain needs N distinct breakers, never one shared) + failover (tries
  Primary then each Fallback in order, first success wins, stamps `ChatResponse::fallback_tier` —
  the same contract `FailoverChatClient::chat()` already established). `Fallback...` may be empty
  (unlike `FailoverChatClient`, which requires ≥1) — "just retry+breaker, no failover" is a
  legitimate configuration here.
- **`MiddlewareModelCallGateway<Inner, Ms...>`** — middleware hooks ONLY, wrapping any
  `ModelCallGatewayLike` `Inner` (typically a `ModelCallGateway<...>`, but the concept is what's
  required, not the concrete type). Reuses `middleware.hpp`'s `ModelCallContext`/
  `middleware_detail::run_before`/`run_after`/`enforce_backend_tool_call_provenance` **verbatim** —
  this is `MiddlewareChatClient::chat()`'s own body, unchanged, calling `inner_.call(...)` instead of
  `inner_.chat(...)`.

Both expose exactly **one method, `call(request, ctx) -> task<result<ChatResponse>>`** — a real
coroutine. This is the whole fix: a middleware hook's `co_await` inside `call()` is now completely
ordinary, because `call()` genuinely is a coroutine, the same way `MiddlewareChatClient::chat()`
always was — no "resume once and hope" hack, no leaked-frame hazard, because nothing is being forced
through the wrong abstraction anymore.

**A new concept, `ModelCallGatewayLike`** (`core/chat_client.hpp`, alongside `ChatClient`):
`{ capabilities() } -> ChatClientCapabilities; { call(request, ctx) } -> task<result<ChatResponse>>`.
`AgentSession<ChatClientT, StateT, HistoryProviderT>`'s own template signature is **unchanged** — 62
existing files reference `AgentSession<` across tests/examples, and changing that signature to carry
a backend pack directly would have been a wide, invasive migration the project owner explicitly
wanted to avoid for this piece. Instead, the class's `requires` clause becomes `(ChatClient<ChatClientT>
|| ModelCallGatewayLike<ChatClientT>)`, and `run_model_call()` picks the branch with `if constexpr`:
a raw single backend (the common case, every pre-existing conformer) is completely unaffected,
keeping its own live, token-by-token `model_delta` emission; a gateway-backed session delegates with
one `co_await chat_client_->call(request, ctx)` — no live deltas for that round, an accepted, named
trade (the same reasoning as §2: a retried/failed-over/middleware-reviewed attempt cannot be shown
live without risking a silent mid-stream substitution). A one-time `run_event_kind::warning` per run
names this trade explicitly when a gateway is engaged, matching `ADR-034`'s own established pattern
for the identical class of tradeoff.

**A required co-fix**, found by the second red-team pass (§4): `OpenAIChatClient::chat_stream()`'s
HTTP-status error mapping is upgraded from one coarse `errc::validation` for every non-2xx status to
a real two-bucket split (429/5xx → `errc::overloaded`, retryable; everything else → `errc::validation`,
not) — status-code-only, deliberately never reusing the dynamic, body-derived message the
non-streaming `map_http_status_error` uses, since `quark::error::detail` is a non-owning
`string_view` and this classifier feeds a `producer.fail(...)` call site on a **detached background
thread** whose stack is gone the instant it returns.

## 4. The second red-team pass (post-design, pre-implementation)

Once the two-type split above was drafted, an independent pass found three further concrete,
narrowly-scoped issues before any code was written:

- **Must-fix — the class-level `requires ChatClient<ChatClientT>` clause itself had to change**,
  or the whole `ModelCallGatewayLike` branch would be dead code that doesn't even compile (the
  original design narrative described the `if constexpr` dispatch without noting the enclosing
  class template's own constraint needed the same widening). Fixed as described in §3.
- **Must-fix — a default-constructed `std::array<CircuitBreaker, N>` silently ships non-functional
  breakers.** `quark::CircuitBreaker`'s default constructor falls back to `open_ns_ = 0` — a
  zero-length cooldown, meaning a "tripped" breaker transitions Open→HalfOpen on the very next call,
  which would compile clean and *look* like real circuit-breaking while providing none. Fixed:
  `breakers_` is built explicitly from a real `BreakerConfig` in the constructor (a
  `std::vector<CircuitBreaker>`, not a fixed array, avoiding a fold-expression/index-sequence
  detour for what's a one-time, non-hot-path construction cost).
- **Must-fix — the co-fix's dangling-`string_view` risk** (§3's last paragraph) — confirmed via
  direct code reading, not assumed.
- **Confirmed correct, not re-litigated:** `middleware_detail::run_before`/`run_after` are genuinely
  generic free-function templates with no `MiddlewareChatClient`-specific coupling, safe to call from
  a brand-new coroutine; `enforce_backend_tool_call_provenance`'s `raw_backend_response` parameter
  correctly generalizes to "whichever tier actually answered," not hardcoded to tier 0; a single
  idempotency key for the whole multi-tier, multi-retry sequence (not one per tier) is the correct
  reading of `IdempotencyKey`'s "stable across every attempt" contract — and, independently
  confirmed, `ChatRequest::idempotency_key` is presently read by zero production backend adapters,
  so this is forward-looking plumbing, not something backed by a currently-observable wire effect.

## 5. Falsifiable claims and verdicts

| # | Claim | Verdict |
|---|---|---|
| G1 | Retry works: a retryable failure is retried, a successful subsequent attempt is returned, `fallback_tier` stays 0 (primary answered). | **CORRECT** — test G1. |
| G2 | A non-retryable failure exhausts its tier in exactly one attempt (no wasted retry), failover to the next tier succeeds, `fallback_tier` is stamped to the correct tier. | **CORRECT** — test G2. |
| G3 | A tripped circuit breaker sheds admission on the NEXT call without ever reaching `chat_stream()` again, falling straight through to the next tier — proven by an exact call-count assertion, not inferred from timing. | **CORRECT** — test G3 (6 rounds; the breaker trips on the 5th primary failure exactly at `fail_threshold`'s default, then round 6 confirms zero further primary attempts). |
| G4 | `MiddlewareModelCallGateway`'s `after_model` hook can be driven safely, and the fatal-finding provenance-enforcement fix (a fabricated `ToolCall` forced to `text_derived`) is reachable through this new composition path, unchanged. | **CORRECT** — test G4. |
| G5 | A `ModelCallGatewayLike`-backed `AgentSession` converges a real run, fires zero `model_delta` events for that round, and emits the ADR-036 warning naming the trade. | **CORRECT** — test G5, full `AgentSession` integration (real `TestKit`-hosted actor, real `StartRun` ask). |
| G6 (refactor) | Factoring `ModelCallGateway`'s internal drain loop into a shared `core/chat_stream_drain.hpp` (reused by `ADR-035`'s memory/history provider work) changes no observable behavior. | **CORRECT** — the full 20-check suite above re-run unmodified after the refactor, identical results. |

## 6. Proof

`tests/test_model_call_gateway.cpp` — 20 checks, deterministic and offline, a scripted `ChatClient`
fixture (`ScriptedGatewayBackend`) whose call count is observed via a `shared_ptr<size_t>` (the
gateway's constructor takes `Primary`/`Fallback` by value, so a plain member would silently diverge
from what the gateway's own internal copy actually does — a real bug caught and fixed during this
ADR's own test authoring, not a hypothetical one). Full project rebuild + regression suite: **208/208
pass**, both before and after the `chat_stream_drain.hpp` refactor.

## 7. What this ADR does not claim

- `FailoverChatClient`/`ResilientChatClient`/`MiddlewareChatClient` are **unmodified and untouched**
  by this ADR — they keep working exactly as before for any existing `chat()`-based consumer. This
  ADR does not delete them, deprecate them formally, or migrate any existing caller off them.
  Whether they should be formally superseded now that `ModelCallGateway`/`MiddlewareModelCallGateway`
  cover their purpose for streaming use cases is an explicit, separate, not-yet-made decision.
- The retry-worthiness classification for `chat_stream()` failures remains structurally coarser than
  `chat()`'s own `ae::failure_class`-based classification, even after the co-fix — `quark::errc` has
  no analog to `failure_class::policy` (e.g. 401/403), so those collapse into the same
  non-retryable bucket as an ordinary 4xx contract violation. Named, not silently improved beyond
  what `quark::error`'s thinner shape actually supports.
- No live-network verification was run for this ADR specifically — `ModelCallGateway`'s internal
  attempts always go through a real conformer's own `chat_stream()`, and the real backends'
  `chat_stream()` implementations are independently, separately verified (`ADR-034`, `ADR-035`); this
  ADR's own proof is scripted/deterministic only.
- Buffering an entire multi-tier, multi-retry sequence before any content reaches the caller is a
  real, accepted latency cost for whichever round engages the gateway — not hidden, but also not
  bounded or optimized (e.g., the last, no-more-retries-possible attempt could in principle stream
  live without risk, since there is nothing left to retry into; this optimization was considered and
  deliberately not built, to keep the first implementation's correctness surface small).
- **`attempt_with_retry`'s backoff blocks the calling thread** (`std::this_thread::sleep_for`, no
  `co_await`/suspension point across the whole retry loop) — code review finding (2026-08-12), named
  here rather than silently left. On a shared thread pool this can stall other actors scheduled on
  the same worker for the full multi-attempt window. Not a hazard newly introduced by this file:
  `ResilientChatClient::chat()` already does the identical blocking `sleep_for` inside a `task<>`
  coroutine, predating this ADR — `ModelCallGateway` faithfully replicated the established pattern
  rather than inventing a new one. A real fix needs a coroutine-suspending timer primitive that does
  not exist anywhere in Quark's `task<>` today and would touch both files; left for separate design →
  red-team → prove work, not patched in as part of this ADR or its post-review fixes.
