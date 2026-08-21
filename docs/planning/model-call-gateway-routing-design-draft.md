# `ModelCallGateway` routing/streaming/stickiness — design draft, three findings verified

**Status: designed, not implemented.** Matches this project's `design → red-team → prove → judge`
discipline (CLAUDE.md), same honesty level as `docs/planning/schedule-wakeup-standing-effect-design-
draft.md` and `docs/planning/tool-optimizer-provider-design-draft.md`.

**Origin:** a same-session comparison of `include/agentengine/core/model_call_gateway.hpp` (ADR-036)
against Microsoft's real `Microsoft.Extensions.AI` routing/failover primitives (`RoutingChatClient`,
`SemanticRoutingChatClient`, `FailoverChatClient`, `OrderedFailoverChatClient` —
https://devblogs.microsoft.com/dotnet/routing-and-failover-for-microsoft-extensions-ai/) surfaced
three candidate findings. Per this session's own established practice (the prior `OQ-18` pass found
a chat conclusion built on an incomplete read), all three were re-verified against real current code
before any design work — not assumed.

**Verification outcome: all three survive as real.** Unlike the OQ-18 pass, nothing here corrects a
false premise — but the verification changed the shape of the design for Findings 2 and 3.

## Finding 1 — No content/semantic routing primitive (confirmed real gap)

**Verification:** grepped `include/agentengine/` for any routing/selection-by-content concept
(`Rout`, `select_client`, `SemanticRouting`-shaped names). No hits beyond unrelated false positives
(`OpenRouter` as a provider name, workflow-graph `edge_kind::switch/case` routing, which is a
different concept entirely — 014 §3's Router *pattern* routes workflow control flow by a classifier's
typed output between *executors*, not chat backends by message content). `ModelCallGateway<Primary,
Fallback...>` is purely ordered retry/failover over a fixed chain — confirmed, no equivalent exists.

### Design — `RoutingModelCallGateway<SelectFn, Routes...>`

A third small composable type, alongside `ModelCallGateway`/`MiddlewareModelCallGateway`, matching
this file's own established shape (a `ModelCallGatewayLike` concept, not a fixed base class — any
conformer composes):

```cpp
// core/routing_model_call_gateway.hpp (sketch, not implemented)
template <class SelectFn, class... Routes>
    requires (ModelCallGatewayLike<Routes> && ...)
class RoutingModelCallGateway {
public:
    RoutingModelCallGateway(SelectFn select, std::tuple<Routes...> routes);
    task<result<ChatResponse>> call(ChatRequest request, EffectContext& ctx);
    // select(request, ctx) -> std::size_t, the C++ analogue of RoutingChatClient.Create's callback
private:
    SelectFn select_;
    std::tuple<Routes...> routes_;
};
```

Each `Route` already satisfying `ModelCallGatewayLike` means a route can itself be a full
`ModelCallGateway<Primary, Fallback...>` — router composition (MEAI's own "Router composition"
pattern: *"a cost- or capability-aware router can sit inside a failover chain"*) falls out for free,
no special-casing needed, the same way `MiddlewareModelCallGateway<Inner, Ms...>` already wraps
*any* `ModelCallGatewayLike` today.

A semantic variant (`SemanticRoutingModelCallGateway<EmbedderT, Routes...>`) would embed the last
user message via a caller-supplied embedder conformer (reusing `OpenAIEmbedder` or any future
embedder, ADR-063), compare against per-route profile-utterance embeddings computed lazily and
cached on first call (mirroring MEAI's own lazy-cache behavior), and select the highest-scoring
route above a threshold, else a `default_route`.

### Self-red-team (Finding 1)

- **004 §4's "explicit policy, never implicit" rule extends to routing, not just failover.** The
  spec text (verified, `004-Model-Provider-Plane.md` line 129-130): *"Failover between providers is
  explicit policy, never implicit: a failover that silently changes model is a correctness change,
  and must appear in the trace and in the response metadata."* A routing decision is the same class
  of correctness-relevant model change. `ChatResponse::fallback_tier` already exists for failover
  tier attribution — routing needs an analogous field (`route_index`/`route_name`), not a silent
  selection. **This is a required part of the design, not an enhancement** — without it, a routed
  call violates 004 §4 by construction.
- **I5 replay**: an embedding-similarity routing decision depends on a live, external embedding call
  — not automatically bit-stable across replay. The *decision* (which route index was chosen), not
  just the final chat response, must be captured at the same I5 recording point 004 §6 already names
  the `ChatClient` seam as owning (*"the primary I5 recording point"*) — replay must reuse the
  recorded route index, never recompute the embedding live. No new recording mechanism needed, but
  the existing one must capture one more field.
- **Cost accounting (004 §5)**: an embedding call is real, billable cost not currently attributed
  anywhere in the model-call cost path (`{tenant, provider, model, agent, principal}` estimated-cost
  metric, 004 §5). Must be its own attributed entry (`{tenant, "embedding", embedder_model, ...}`),
  never silently folded into the eventually-chosen chat model's cost.
- **I2/capability**: the embedding call is an ordinary outbound network effect, already covered by
  the existing `NetOut<host>` capability model (007 §3) — no new capability class needed, just an
  ordinary grant for the embedding provider, same as any other outbound call.

## Finding 2 — Buffer-then-decide is stricter than 004 §4 actually requires (confirmed, and the file's own comment overstates its citation)

**Verification:** read 004 §4 directly (`004-Model-Provider-Plane.md` lines 116-130). The section
says nothing about mid-stream token buffering. The actual rule is line 129-130, quoted above: no
*silent, untraced* failover — not "no partial output before a decision is final." `model_call_
gateway.hpp`'s own top comment (lines 47-57) cites "004 §4" for *"the silent mid-stream backend
substitution 004 §4 forbids"* — that specific phrase is the file's own inference, not 004 §4's
literal text. MEAI's `FailoverChatClient` satisfies the actual rule (no silent substitution) via a
narrower mechanism: retry freely before any output reaches the caller (`OutputCommitted == false`);
once committed, failure is terminal — never continues to a different backend after showing content,
which is exactly what "no silent change" requires, achieved without full buffering.

Checked `include/agentengine/core/stream.hpp`: `rt::channel<T,E>`'s `push()` blocks (real
backpressure) rather than drops/retries — a local `bool any_pushed` flag at the call site, set on
the first successful push to the *caller's* stream, is sufficient to implement the same gate MEAI's
`OutputCommitted` provides. No new primitive needed in `stream.hpp` itself.

Also checked: the file's own already-named residual (blocking `sleep_for` inside `attempt_with_retry`,
lines 257-271) is a separate, orthogonal concurrency gap in the same function — this design neither
fixes nor worsens it; still open, named again here for completeness, not addressed by this draft.

### Design — commit-gated streaming, additive not breaking

Add a new opt-in entry point rather than changing `call()`'s existing buffered contract (which real
callers already depend on and test against):

```cpp
// Sketch, not implemented — additive alongside the existing buffered call()
task<result<ChatResponse>> call_stream(ChatRequest request, EffectContext& ctx,
                                        stream_producer<ChatEvent>& caller_stream);
```

Inside `attempt_with_retry`'s streaming path: push chunks to `caller_stream` as they arrive from
`chat_stream()` (rather than accumulating internally via `drain_chat_stream()` before any decision).
Track `bool any_pushed = false` per attempt; flip it true on the first successful push. If the
backend fails while `any_pushed == false`: behave exactly as `call()` does today — retry/failover
freely, nothing was shown. If it fails after `any_pushed == true`: **terminal**, no retry, no
fallback tier attempted — matching `call()`'s existing single-attempt failure contract for that
case, just reached via a different, streaming-capable path.

The existing blanket "gateway-routed calls never emit `model_delta`" warning (`run_model_call()`'s
comment, `agent_session.hpp`) would need to become conditional on which entry point (`call` vs.
`call_stream`) a given `ModelCallGatewayLike` conformer is used through — still a named, visible
trade (matching this codebase's "named trades, not silent ones" convention, ADR-034's identical
precedent for `stream_model_calls_`), just a narrower one than today's blanket statement.

### Self-red-team (Finding 2)

- **Is "pushed into the channel" the right commit boundary, or should it be "actually rendered to a
  human"?** Checked against how the rest of the engine already treats this class of question: I4
  ("every effect is attributable") treats emission onto the run's event stream as the attribution
  point, not downstream human perception — `push()` succeeding is the consistent analogue. Survives.
- **Partial-content coherence**: does a caller ever see spliced content from two different tiers?
  No — by construction, once `any_pushed` is true, no further tier is attempted; a committed-then-
  failed attempt just ends in partial content + error, identical in shape to how any *non-gateway*
  `ChatClient` already fails mid-stream today (a pre-existing, already-handled failure shape — this
  design doesn't invent a new one for callers, it lets the gateway degrade to that same shape instead
  of the current all-or-nothing buffered shape).
- **I5 replay**: 004 §6 already requires the chunk sequence to reproduce exactly; this design needs
  the *attempt sequence* (which tier(s) were tried, how many chunks each committed before success or
  terminal failure) captured, not just the final single response — an extension of the existing
  recording contract's scope, not a new invariant.
- **Does this reopen the "silent substitution" risk in a subtler form** — e.g., a caller reading
  `fallback_tier` only from the final `ChatResponse` might not realize *which* chunks came from which
  tier if a partial pre-commit attempt is silently dropped and retried? No new risk here specifically
  *because* pre-commit chunks were never exposed to the caller at all under this design (only
  post-commit chunks are pushed) — the caller-visible chunk sequence is always from exactly one tier.

## Finding 3 — No sticky/session-scoped route pinning (confirmed real and architecturally consequential, not theoretical)

**Verification:** `include/agentengine/rt/agent_session.hpp` line 479-483: `AgentSession<ChatClientT,
...>` holds exactly **one** `ChatClientT chat_client_` instance for the session's entire lifetime;
`run_model_call()` (line 1150) calls `chat_client_->call(request, ctx)` on that same instance every
round of every turn (line 1166-1167, the `ModelCallGatewayLike` branch). `ModelCallGateway::call()`
always starts at `try_tier<0>` fresh (Primary) on every invocation — confirmed no state persists
across calls about which tier last succeeded. **This is not theoretical**: because the gateway
instance genuinely IS session-scoped and reused across every round, a real session can have turn 3
fail over to a Fallback tier and turn 4 silently retry-then-succeed on Primary again, with zero
continuity guarantee between them — exactly the shape MEAI's article warns strands opaque,
provider-specific reasoning-continuation state.

### Design — optional sticky mode, simpler than MEAI's because the gateway is already long-lived

MEAI needs an app-level `IDistributedCache` + session-ID because `RoutingChatClient` instances are
typically *not* long-lived per conversation. AgentEngine's gateway already is — `breakers_` (per-tier
`rt::CircuitBreaker` state) is already persistent, mutable, cross-call state living on the same
object. Sticky tier selection is the same shape of thing, no new plumbing:

```cpp
// Sketch, not implemented — additive field + constructor flag
class ModelCallGateway {
    // ...
    explicit ModelCallGateway(Primary primary, std::tuple<Fallback...> fallbacks,
                               RetryPolicy retry_policy = {}, BreakerConfig breaker_config = {},
                               bool sticky = false, /* ... */);
private:
    bool sticky_ = false;
    std::optional<std::size_t> last_successful_tier_;
};
```

`call()` starts `try_tier` at `sticky_ && last_successful_tier_ ? *last_successful_tier_ : 0`; on
success, updates `last_successful_tier_` when `sticky_`. Default `sticky_ = false` (today's
behavior) — sticky is the right choice specifically for reasoning-continuity-sensitive deployments,
not a universal default (a cheap-primary/expensive-fallback pairing usually *wants* to keep retrying
the cheap option first). Whether a stuck-on-fallback session should ever re-probe Primary
periodically is named here as an explicit open question, not designed — MEAI's own article flags the
identical unresolved point (*"splitting the two apart would... be a useful future improvement"*).

### Self-red-team (Finding 3)

- **Same class of state as the already-accepted `breakers_`** — not a new category of per-object
  mutable state this file didn't already have; the design cost is genuinely small.
- **Real open question, not resolved here**: does `AgentSession`'s snapshot/restore path
  (`fork_from`, passivation/resume) serialize `chat_client_`'s internal state today? Not verified in
  this pass — if it does not (plausible, since a non-gateway `ChatClientT` is typically stateless and
  snapshotting it may never have been exercised), then `breakers_` state *already* silently resets on
  restore today, and adding `last_successful_tier_` would inherit that same gap rather than introduce
  a new one — but this needs a real check before implementation, not an assumption either way.
  **Flagged, not verified — do not implement sticky mode without first confirming this.**
- **I2/capability**: none — pure bookkeeping over an already-authorized fixed chain, identical shape
  to the existing breaker state.

## What this draft is not

Not an implementation. Not an ADR. All three findings need the full `design → red-team → prove →
judge` cycle before code lands, per CLAUDE.md's rule for anything touching I2/I3/I5 or cost/replay
guarantees — this pass is the "design" step, self-red-teamed once, not judged or proven.
