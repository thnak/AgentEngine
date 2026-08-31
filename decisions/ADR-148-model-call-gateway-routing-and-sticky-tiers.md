# ADR-148 — `RoutingModelCallGateway` and sticky tier pinning, corrected from GitHub issue #16

**Status:** Proposed — designed (corrected from `docs/planning/model-call-gateway-routing-design-
draft.md` by an independent red-team pass, §2), implemented, and proven (real code + tests, §4).
**Awaiting project-owner judgment** — per this project's `design → red-team → prove → judge`
discipline (CLAUDE.md), this ADR is not "Judged" until the project owner signs off; recorded here in
Proposed state so the evidence trail is complete and reviewable before that sign-off.

**Relates to:** GitHub issue #16 (opens this line of work; three findings against `model_call_
gateway.hpp` vs. Microsoft.Extensions.AI's routing/failover primitives, plus a follow-up comment
naming a second `SelectFn` use case). `docs/planning/model-call-gateway-routing-design-draft.md`
(Findings 1 and 3, the two this ADR corrects and implements — Finding 2, commit-gated streaming, was
already designed, implemented, and Judged separately via **ADR-073**, unrelated to this ADR beyond
sharing the same source file). `decisions/ADR-036-model-call-gateway.md` (`ModelCallGateway`/
`MiddlewareModelCallGateway`, the two existing `ModelCallGatewayLike` conformers this ADR adds a third
to, and extends one of). `decisions/ADR-073-unified-streaming-gateway-call-stream-tool-argument-
result-streaming-ask-stream.md` (`call_stream()`, extended here for sticky-tier parity).
`include/agentengine/core/routing_model_call_gateway.hpp` (new), `include/agentengine/core/model_call_
gateway.hpp` (extended), `include/agentengine/core/chat_client.hpp` (`ChatResponse::route_index`, new
field), `tests/test_routing_model_call_gateway.cpp` (new), `tests/test_model_call_gateway.cpp`
(extended, G12-G15).

## 1. The question

**Stated so it has a wrong answer:** does `ModelCallGateway<Primary, Fallback...>` (ordered
retry/failover only) cover what a real deployment needs from a model-call gateway, or are there real,
verifiable gaps against a comparable shipped system (Microsoft.Extensions.AI's `RoutingChatClient`/
`FailoverChatClient`)?

**Before this ADR:** GitHub issue #16 named three findings, self-red-teamed once by the same session
that wrote them (not independently). Re-verified against current code before any design work (not
assumed):

- **Finding 1 — no content/semantic routing primitive.** Confirmed: grepping `include/agentengine/`
  found no routing-by-content concept; `ModelCallGateway` is purely ordered retry/failover.
- **Finding 2 — buffer-then-decide is stricter than 004 §4 requires.** Already resolved, separately:
  `ModelCallGateway::call_stream()` was designed, red-teamed (5 rounds), implemented, and **Judged**
  via ADR-073 (2026-08-22) — a different design track this ADR does not touch beyond extending
  `call_stream()` once more, for sticky-tier parity (§3).
- **Finding 3 — no sticky/session-scoped tier pinning.** Confirmed real by direct code reading, not
  theoretical: `AgentSession` holds exactly one `ChatClientT chat_client_` for its whole lifetime,
  reused every round; `ModelCallGateway::call()` always restarted at tier 0 (Primary) on every
  invocation, no cross-call memory of which tier last succeeded.

## 2. A correction to the design draft, found by an independent red-team pass

The draft's own design sketches for Findings 1 and 3 were reviewed by an independent pass (2026-08-31,
general-purpose agent, no shared context with the draft or this ADR's own first design memo) before
any code was written. It found two real, concrete problems — not style opinions — matching the bar
ADR-036's own second red-team pass set (three must-fix findings before any code existed there).

**MUST-FIX 1 — the draft's I5 claim (Finding 1) does not hold for the `SelectFn` shape actually
designed.** The first design memo's own `SelectFn` signature took `(ChatRequest const&, EffectContext&)`
— which can read `EffectContext::deadline` (`effect_context.hpp`), a `std::chrono::steady_clock::
time_point`. That file's own adjacent comment on `run_id` names exactly this class of hazard: "an
unrecorded wall-clock read here would be exactly the kind of untracked nondeterminism I5 forbids." A
`SelectFn` reading `deadline` (a natural policy: "route to the cheaper backend when little time is
left") would NOT be replay-reproducible, even though the memo claimed the base router needed "zero new
I5 recording plumbing" — true only by accident, because `AgentSession` never actually populates
`effect_context_.deadline` today, not as a structural guarantee.

**Resolution:** `RoutingModelCallGateway<SelectFn, Routes...>`'s `SelectFn` is narrowed to
`(ChatRequest const&) -> convertible_to<std::size_t>` — `EffectContext` is not passed at all. This
makes "replaying the exact same recorded `ChatRequest` reproduces the exact same selected index" a
real, `static_assert`-checkable structural property, not a documented-but-unenforced convention a
future `SelectFn` author could silently violate. A future selection signal genuinely needing
`EffectContext` (or a live, non-deterministic input — an embedding call, the draft's own
`SemanticRoutingModelCallGateway<EmbedderT, Routes...>` sketch) needs its own type with its own I5
recording story; not built here (see §5).

**MUST-FIX 2 — `capabilities() == Route 0`'s capabilities silently breaks capability-based content
routing, the headline use case the draft's own Finding 1 language implied.** `AgentSession::
run_model_call()` (`rt/agent_session.hpp`) calls `validate_outbound_media_capabilities(request,
chat_client_->capabilities())` **unconditionally, before `SelectFn` ever runs**, and hard-fails the
whole run if the request carries `Media` the reported capabilities don't declare (the same
`capabilities().tool_calling` gate recurs for the undeclared-tool-call-leak scan). Reporting only
Route 0's capabilities (matching `ModelCallGateway`'s own "primary only" precedent for a heterogeneous
chain) means a request needing a capability only a LATER Route declares is rejected at this
pre-dispatch gate before selection ever has a chance to route it there — silently defeating "route
image-bearing requests to a vision-capable Route, text-only elsewhere," the textbook routing use case.

**Resolution:** narrow this ADR's actual scope, rather than invent a fix to `AgentSession`'s own
pre-dispatch gates (real, separate, invasive work). `RoutingModelCallGateway` is documented, in its own
file-top comment, as being for Routes that are **capability-equivalent** — cost/latency/provider
selection, or (the concrete use case issue #16's own follow-up comment names) the same backend wrapped
at different `reasoning_effort` levels, routed by predicted task complexity. Checked directly: neither
of the two use cases actually named in issue #16 (content-category routing by message shape;
overthinking-risk routing by predicted complexity) needs heterogeneous Route capabilities — both are
"same backend, different policy" scenarios where Route 0's capabilities are already representative of
every Route. Capability-based content routing (genuinely different Route capabilities) is named as a
real, separate limitation this ADR does not solve — it would need `AgentSession`'s own pre-dispatch
gates to become router-aware, not attempted here.

**Two further, smaller corrections from the same pass:** two mis-citations in the first design memo
(a comment misattributed to the wrong field, and an inferred-not-documented convention presented as
documented) — fixed in this ADR's own text and the shipped code's comments, not repeated here.
Everything else in the memo — the runtime-index-to-compile-time-dispatch pattern, the sticky+
`call_stream()` tier-index-threading fix, the pre-existing restore/fork non-survival claim, and the
`route_index` vs. `fallback_tier` field separation — was independently re-derived from the real source
and confirmed correct, not rubber-stamped (full findings archived in this session's own record; not
reproduced verbatim here to keep this ADR to its own real content).

## 3. The design (as implemented, post-correction)

### Finding 1 — `RoutingModelCallGateway<SelectFn, Routes...>`

New file, `core/routing_model_call_gateway.hpp` — a THIRD small, composable `ModelCallGatewayLike`
type, matching `ModelCallGateway`/`MiddlewareModelCallGateway`'s own established shape (a concept, not
a base class; any conformer composes). Picks ONE of `Routes...` per call via `select_(request)`
(narrowed signature, §2), dispatched through a runtime-index-to-compile-time-tuple-access pattern
(`call_route<I>`, structurally a `std::visit`-style dispatch table — walks `I = 0..sizeof...(Routes)`,
with the terminal case at `I == sizeof...(Routes)` catching an out-of-range `select_()` result as a
real `failure_class::contract` error, never UB, never silently clamped to Route 0). Each `Route`
already satisfying `ModelCallGatewayLike` means a Route can itself be a full `ModelCallGateway<Primary,
Fallback...>` — router-wraps-failover composition falls out for free (proven, §4 G3), the same way
`MiddlewareModelCallGateway<Inner, Ms...>` already wraps any `ModelCallGatewayLike` `Inner`.

New `ChatResponse::route_index` field (`chat_client.hpp`), appended last (the file's own established
field-ordering discipline). Orthogonal to `fallback_tier`, not a replacement: `route_index` names WHICH
Route answered; `fallback_tier` (left untouched, exactly as the selected Route's own `call()` set it)
separately names which tier WITHIN that Route answered, if it's itself a `ModelCallGateway`. `0 = the
first Route answered, OR no RoutingModelCallGateway was in the loop at all` — the same accepted
ambiguity `fallback_tier` already carries for "no gateway." Also threaded through `chat_recording.hpp`'s
`chat_response_to_json`/`chat_response_from_json` codec, alongside `fallback_tier`, for field parity
(that codec is not yet wired to any live recording path — `RecordingChatClient`/`ReplayChatClient`
operate strictly below the gateway layer today — but a manual field-list codec that silently drops a
new response field is exactly the kind of "forgot to update it" trap worth closing on sight).

`capabilities()` reports Route 0's declared capabilities only — a real, named scope limit, not an
oversight (§2, MUST-FIX 2). `call_stream()` is NOT implemented for this type in this pass — a named,
disclosed residual, matching the existing precedent that `MiddlewareModelCallGateway`/
`ContentReplayGateway` also lack it (ADR-073); `AgentSession`'s existing duck-typed `if constexpr
(ModelCallGatewayStreamLike<ChatClientT>)` check already degrades correctly for any conformer lacking
it, no special-casing needed.

**Explicitly out of scope, named not silently dropped:** the draft's own `SemanticRoutingModelCallGateway
<EmbedderT, Routes...>` (embedding-similarity routing) — sketched at a level too shallow to build
correctly (no concrete cost-attribution or I5-recording mechanism ever specified for the embedding
call itself). Same honest-scoping move ADR-147 made for the live-model honeypot gate: implement what
was specified in enough detail to build correctly, name the rest as real future work needing its own
design pass.

### Finding 3 — sticky tier pinning on `ModelCallGateway<Primary, Fallback...>`

Two new private members (`bool sticky_ = false; std::optional<std::size_t> last_successful_tier_;`),
one new trailing constructor parameter (`bool sticky = false`, appended after `jitter` — every existing
positional call site keeps compiling and behaving unchanged). Both `call()` and `call_stream()` (added
after ADR-073 shipped streaming, so the original draft never considered this path) start the SAME
forward tier cascade each already runs (`try_tier<I>`/`stream_tier<I>`, both **unchanged**) at
`last_successful_tier_` instead of always at 0, via new runtime-to-compile-time dispatch helpers
(`try_tier_from<I>`/`stream_tier_from<I>`) that walk to the matching `I` then hand off to the existing
function verbatim — "start sticky, fail forward through whatever tiers remain" falls out for free from
the cascade those functions already implement.

For `call_stream()`, a real subtlety found by direct code reading (not assumed): `stream_attempt_with_
retry`'s `std::optional<error>` return overloads `nullopt` three ways (succeeded, caller-cancelled,
stop-requested-before-starting) — `stream_tier<I>` could not previously tell which one occurred, so it
had no way to report which tier succeeded back up to `call_stream()` for `last_successful_tier_` to be
stamped. Fixed by threading a `std::size_t tier_index` runtime parameter into `stream_attempt_with_
retry` (each `stream_tier<I>` call site already knows `I` at compile time) and stamping
`last_successful_tier_` at the ONE unambiguous real success point (`producer.close()`), guarded by
`sticky_`. Safe under the file's own pre-existing single-writer invariant for `breakers_[i]` (external
serialization via `AgentSession::session_mutex_`, since `call_stream()` runs on a detached thread) —
that invariant's own comment is widened to name `last_successful_tier_` too, rather than duplicated.

**Confirmed, not just re-flagged (the draft's own explicit "not verified" open question):**
`AgentSessionRecord`/`to_record()`/`restore_from_record()`/`fork_from()` (`rt/agent_session.hpp`) do
not reference `chat_client_` at all — verified by direct read of every field list and function body.
`ModelCallGateway`'s existing `breakers_` state already does not survive a session restore or fork
today; `last_successful_tier_` inherits the identical, pre-existing gap, not a new one.

**Named, not solved (the draft's own explicit open question, carried forward, now proven not just
described — §4 G15):** once stuck on a non-zero tier, a session never automatically re-probes a
MORE-preferred (lower-numbered) tier that previously failed — it only un-sticks FORWARD (the current
tier's own next full-chain failure moves `last_successful_tier_` to whatever succeeds after it, if
anything) or stays put. A skipped tier's own `breakers_[i]` also goes stale while bypassed — `on_send`/
`on_result` are never called for it, so it cannot self-heal Open→HalfOpen via the normal admission-probe
mechanism either (a real mechanism this ADR's own red-team pass surfaced, named explicitly here rather
than only implied by "no periodic re-probe"). Periodic re-probing of a more-preferred tier is real,
separate, undesigned work.

## 4. Evidence

`tests/test_routing_model_call_gateway.cpp` (new, 18 checks) and `tests/test_model_call_gateway.cpp`
G12-G15 (25 new checks, appended to the existing 46), built and run directly (Debug, MSVC, Windows):

```
test_routing_model_call_gateway: OK   (18/18 checks: G1 basic selection & route_index stamping,
  G2 out-of-range select() is a real named failure not UB, G3 router-wraps-failover composition
  proven -- route_index AND fallback_tier both correct simultaneously, G4 capabilities() == Route 0
  proven not just documented, G5 a sticky Route's own state persists correctly across repeated
  selection by the router)

test_model_call_gateway: OK  (71/71 checks total, G1-G11 unchanged plus:
  G12 sticky call(): call 2 starts directly at the fallback, primary never re-attempted
  G13 sticky call_stream(): same pinning proven through the streaming entry point specifically
      (proves the tier_index-threading fix actually reaches last_successful_tier_)
  G14 sticky=false (the default): every call still starts fresh at the primary -- opt-in confirmed
  G15 the named residual, proven not documentation: a later failure of the stuck tier (nothing left
      to cascade to) fails the whole call without falling back to the primary, and the call AFTER
      that still starts at the stuck tier again -- no automatic upward recovery, ever)
```

`test_chat_recording_codec` (route_index round-trip, G1-R3 extended) and a full project rebuild + the
full `ctest` suite: zero regressions, all pre-existing tests green alongside the new ones.
`python tools/naming_lint.py`: clean (`RoutingModelCallGateway` suppressed via the identical `ae-
naming-lint: allow` precedent `ModelCallGateway`/`MiddlewareModelCallGateway` already carry, ADR-025
§4c — deferred bulk reconciliation against 027, not a new gap).

## 4a. A real gap found by independent code review, named not fixed

An independent code-review pass (2026-08-31, general-purpose agent, fresh rebuild + fresh test runs of
its own, not trusting this session's prior output) confirmed every claim in §2-§4 above by direct
re-derivation and found one further, real gap this ADR's own text had not named: `AgentSession::
run_model_call()`'s streaming reconstruction path (`detail::drain_streaming_response`,
`rt/agent_session_trust.hpp`) rebuilds the final `ChatResponse` from accumulated `ChatResponseUpdate`s
via the bare 2-argument aggregate `ChatResponse{accumulated, *usage}` — `ChatResponseUpdate` carries
neither `fallback_tier` nor `route_index`, so **both fields are silently reset to 0** in the response
`AgentSession` actually records and returns, regardless of which tier or Route really answered,
whenever a gateway is driven through `AgentSession` with `stream_model_calls_ == true` (the
`call_stream()` path).

This is **not a new gap `route_index` introduces** — `fallback_tier` already had it before this ADR,
inherited unchanged. But it is real, and it is more consequential now than before: sticky mode's
entire value proposition is staying on a non-zero tier, and `RoutingModelCallGateway`'s `route_index`
is meant to be inspectable after a call — a caller reading `AgentSession`'s own recorded/returned
`ChatResponse` after a streamed, gateway-routed turn cannot actually see which tier or Route answered,
even though the gateway itself tracked it correctly internally (proven, §4 G13). `RoutingModelCallGateway`
itself is not reachable through this specific path today (it has no `call_stream()`, so `AgentSession`
always falls to the buffered `call()` path for it, where both fields ARE set correctly) — only
`ModelCallGateway` used directly for streaming is affected. Named here as a real, disclosed residual,
not fixed in this pass: fixing it means widening `ChatResponseUpdate` (or `drain_streaming_response`'s
own reconstruction) to carry tier/route identity through the accumulation loop, a real, separate change
to a file this ADR does not otherwise touch.

## 5. What this ADR does not claim

- **Does not build the embedding-based `SemanticRoutingModelCallGateway<EmbedderT, Routes...>`** the
  original draft sketched — no cost-attribution or I5-recording mechanism for a live embedding call
  exists in this codebase, and designing both from scratch is real, separate work (§3).
- **Does not make `AgentSession`'s pre-dispatch capability gates router-aware** — `capabilities()`
  reporting Route 0 only means genuinely heterogeneous-capability routing (the "vision-capable Route
  vs. text-only Route" case) is not supported by this type as built; a real, separate, invasive change
  to `rt/agent_session.hpp`'s own gates would be needed, not attempted here (§2, §3).
- **Does not implement `call_stream()` for `RoutingModelCallGateway`** — a named residual matching
  `MiddlewareModelCallGateway`/`ContentReplayGateway`'s own existing gap (ADR-073).
- **Does not solve sticky mode's "never re-probes a more-preferred tier" limitation**, nor the breaker
  staleness that compounds it for a skipped tier — both real, named, undesigned follow-on work (§3),
  proven to actually behave this way (§4 G15), not merely asserted.
- **Does not change `try_tier<I>`/`stream_tier<I>`/`attempt_with_retry`/`stream_attempt_with_retry`'s
  own pre-existing behavior or residuals** (the documented blocking `sleep_for` in the retry backoff,
  the coarser streaming failure classification, etc., all named already in ADR-036) — every change here
  is additive (a new type, new optional constructor parameters, new optional fields), zero behavior
  change for any existing caller not opting in.
