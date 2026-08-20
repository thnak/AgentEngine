# ADR-069 — When a model response has already committed content that violates policy, does AgentEngine discard-and-retry inside the existing `after_model` point, or leave remediation entirely to the host?

**Status:** Proposed (design → red-team → prove phases complete for Design B; awaiting explicit user
"Judged"). Implemented: `ContentReplayDecision`/`ContentReplayTrigger`/`ContentReplayAttemptEvent`/
`ContentReplayTraceHook`/`corrective_message()`/`ContentReplayGateway<Inner>`
(`include/agentengine/core/content_replay_gateway.hpp`), proven by
`tests/test_content_replay_gateway.cpp` (30/30 checks, real Windows/MSVC build — see §5/§6 for the
updated evidence and verdicts, superseding this ADR's original, pre-implementation §5/§6).
Independent of `decisions/ADR-066-context-provider-attribution-provenance.md` and
`decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md` — confirmed by
implementation: `content_replay_gateway.hpp` has no include of either header. **Relationship to
`decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md` remains a real open question,
now with more grounding**: both are implemented, both are standalone (neither wired into
`rt::AgentSession`), and neither header includes the other — no code-level interaction exists to
even test for overlap yet, so §7's open question stands exactly as raised, not resolved by this
pass.

A real, mid-implementation finding, not spelled out by the design draft (recorded in
`content_replay_gateway.hpp`'s own top comment and §5 below): the amended retry request must NEVER
re-include the discarded response's own content — building the actual retry-request-construction
code forced this question, and re-including it would re-send whatever got a response discarded (a
secret, for the motivating case) back to the vendor a SECOND time, inside the very call meant to
correct it. The draft's §2 "honest scope limit" implied this direction but never stated it as an
explicit constraint on the retry request's own construction.

**Relates to:** `decisions/ADR-033-middleware-model-call-chain.md` (the real, shipped `after_model`
point and `MiddlewareTraceEvent`/`MiddlewareTraceHook` this design reuses), `002-Agent-Model-and-
Authoring.md` §3 (`Retry<Policy>`, the transient-failure retry shape this is explicitly NOT a reuse
of), `decisions/ADR-036-model-call-gateway.md` (`MiddlewareModelCallGateway`, the real caller that
would need to gain a re-invoke path). Design draft: `docs/planning/content-triggered-response-replay-
design-draft.md` (workflow-reviewed 2026-08-20, 4 findings, all applied).

## 1. The question

**Stated so it has a wrong answer:** `runtime-secret-quarantine-design-draft.md`'s `quarantine_secret`
tool handles the CHEAP case — the model notices a problem mid-round, before finalizing a response,
and self-corrects via an ordinary tool call. Nothing in this codebase handles the case that excludes:
a response has already been produced and is about to commit to history, and only then does something
detect it contains a secret (or any other content-policy violation). Should AgentEngine build a
discard-and-retry mechanism for this, or accept that once a response settles, remediation is entirely
the host's problem (e.g. an async post-hoc scan-and-redact over committed history)?

## 2. The competing designs

**Design A — no engine mechanism; host handles remediation post-hoc.** AgentEngine commits whatever
the model produced; a host that cares scans committed history asynchronously and redacts after the
fact. Steelman: zero new engine surface, zero new budget/turn-index risk, zero new interaction with
`Middleware`'s existing chain semantics.

**Design B (chosen) — content-triggered replay, hooked into the existing `after_model` point.** An
`after_model` hook (ADR-033, real code today) inspects the settled `ChatResponse`; on a violation, it
signals `discard_and_retry`, and the WRAPPER (`MiddlewareModelCallGateway`, ADR-036) re-invokes the
underlying model call with an amended request, bounded by both a per-trigger-site
`max_replay_attempts` and a NEW session-lifetime cap. Steelman: prevents the tainted content from ever
committing to durable history/checkpoint/transcript at all — strictly stronger than Design A, where
the violation is briefly durable before a host's async scan catches it. General-purpose: the trigger
predicate is pluggable, serving any "discard this settled response, retry with a correction" need
(secrets are one caller, not the reason the mechanism exists).

## 3. Falsifiable claims

| Claim | Disproving experiment |
|---|---|
| Replay attempts are bounded across the SESSION's lifetime, not just per trigger site (the must-fix finding's core claim). | Run an adversarial always-triggering `after_model` hook across many turns of one session; assert total replay-driven backend calls stays ≤ the session-lifetime cap, never proportional to session length. |
| `TokenBudget` accounts every replay attempt's real backend usage, not just the finally-returned response's. | Trigger N replay attempts in one trigger site; assert budget consumed equals N × per-call cost, not 1×. |
| Streaming calls (`chat_stream()`) are never subject to replay. | Attempt to register this mechanism against a streaming call path; assert it is inert/unreachable, matching ADR-033's own `chat_stream()` exclusion. |
| A replay attempt's own response is re-checked by the same `after_model` predicate (bounded recursion). | Configure a hook that always triggers; assert the mechanism fails closed at exactly `max_replay_attempts`, not before, not after. |

## 4. The red-team attack (text-level, not code-level)

Same three-lens workflow review as ADR-066/067/068. Four findings, no FATAL, three must-fix:

1. **Must-fix**: replay happens inside one model-call wrapper invocation without incrementing
   `effect_context_.turn_index` — structurally outside `MaxTurns<N>`'s reach (002 §3). A steered model
   could re-trigger the same violation across many different TURNS, each getting a fresh
   per-trigger-site `max_replay_attempts` budget: small local bound, unbounded session-lifetime cost.
   **Fixed**: a session-lifetime replay counter, independent of and in addition to
   `max_replay_attempts`, caps total replay-driven cost for the rest of the session once exhausted.
   Also flagged, not yet closed: `TokenBudget` accounting every replay attempt's real usage (not just
   the finally-kept response's) is asserted, not proven — must be verified against real accounting
   call sites before this ships.
2. **Must-fix**: a citation error — the design attributed `Retry<Policy>`'s "transient-failure retry
   shape" framing to "027 §4" (which contains no such table); corrected to
   `002-Agent-Model-and-Authoring.md` §3, the real location.
3. **Must-fix**: the header claimed flat independence from all three other drafts in this batch, while
   the design's own body asked the review workflow to confirm non-overlap with the turn-middleware
   design — an unresolved question the header's claim didn't reflect. Corrected: independence from
   the provenance and secret-quarantine designs is real; the relationship to the turn-middleware
   design's `TurnContext` is a genuinely open question, not settled.
4. **Worth-noting, resolved**: streaming was left as an open "maybe excluded" question even though
   ADR-033 already made this exact decision for the identical point. Resolved: gated to non-streaming
   calls only, firmly, inheriting ADR-033's own precedent rather than re-litigating it.

## 5. Executed evidence (superseding this ADR's original, pre-implementation §5)

Implemented `include/agentengine/core/content_replay_gateway.hpp`. **A real design change from the
draft's own §3**: the draft framed this as extending `MiddlewareModelCallGateway` (ADR-036) itself
with a re-invoke path inside its existing `after_model` hook contract. Building it that way would
have meant widening `ModelCallContext` (a shipped, Judged type, ADR-033) with new
`discard_and_retry`/`corrective_instruction` fields most existing `after_model` hooks have no use
for. What's actually implemented is a SEPARATE wrapper, `ContentReplayGateway<Inner>`, composed over
any `ModelCallGatewayLike` — including a `MiddlewareModelCallGateway<...>` directly, unmodified — the
identical compositional shape `MiddlewareModelCallGateway` itself already uses over
`ModelCallGateway<...>`. This is narrower and additive rather than what the draft's own wording
implied, and does not touch ADR-033/036's existing, Judged code at all (confirmed: zero diff to
`middleware.hpp`/`model_call_gateway.hpp` in this pass).

**A second, real, mid-implementation finding**: the amended retry request must never re-include the
discarded response's own content. §1's header records the full reasoning; `tests/
test_content_replay_gateway.cpp` proves it directly against the real request the gateway sends, not
merely by inspecting `corrective_message()` in isolation.

`tests/test_content_replay_gateway.cpp`, **30/30 checks passed**, Windows/MSVC:
- A non-triggering response passes through with exactly one backend call and zero replay budget
  consumed.
- A discard-and-retry scenario: the retried response is checked by the SAME trigger predicate again
  (§3 claim 4, bounded recursion — proven via a call counter, not merely by construction), the final
  kept response is the retry's, and the actual second request sent to the backend contains the
  original message PLUS exactly one corrective message — the discarded text itself is absent,
  checked by string search over every message in the real, captured `ChatRequest`.
- Per-trigger-site bounding (`max_replay_attempts=2`): an always-triggering scripted backend causes
  exactly 2 real calls, then a `content_replay.max_attempts_exhausted` failure — not 1, not 3 (§3
  claim 1's own "not before, not after" framing, now literally checked).
- Session-lifetime bounding, independent of the per-trigger-site cap (the must-fix finding's core
  claim): with `max_replay_attempts=5` (deliberately generous) and `session_lifetime_cap=1`, a FIRST
  `call()` invocation's replay succeeds and consumes the session's one allowed replay; a SECOND,
  independent `call()` invocation on the SAME gateway instance (modeling a later round of the same
  session) fails immediately on its first attempt with `content_replay.session_cap_exhausted` —
  proving the session-lifetime cap persists ACROSS separate `call()` invocations, not just within one
  trigger site's own retry loop.
- An unconfigured trigger (`nullptr`) fails open to no-replay — the identical posture ADR-068's
  `SecretDetector` already established for "host wires nothing."
- A real backend failure (not a content-triggered discard) propagates with its original error code
  unchanged, is never presented to the trigger at all, and consumes no replay budget.

Streaming exclusion (§3 claim 3) is proven structurally, not behaviorally: `ContentReplayGateway` has
no `chat_stream()` method at all — there is no code path for a caller to reach, matching
`turn_middleware.hpp`'s own "proof by absence" precedent for `Compactor<N>`/`history[]`.

Full-tree rebuild (`cmake --build . --config Debug`, all targets): **zero compile errors**. Full
`ctest`: same 10 pre-existing, unrelated not-run CPython-embedding targets as ADR-066/067; all real
targets pass. Commands: `cmake --build build --target test_content_replay_gateway --config Debug`,
`ctest --test-dir build -C Debug --output-on-failure`.

**Not run, named rather than left implied**: whether `rt::AgentSession`'s OWN `TokenBudget<N>`
mechanism is ever wired to consume `ContentReplayTraceHook`'s per-attempt `Usage` values — this file
makes every attempt's real cost OBSERVABLE to a host-wired hook, but does not itself change what
`AgentSession`'s existing budget enforcement sees (§7).

## 6. Per-claim verdicts (superseding this ADR's original, pre-implementation §6)

| Claim (§3) | Verdict |
|---|---|
| Replay attempts are bounded across the SESSION's lifetime, not just per trigger site. | **CORRECT** — a session-lifetime cap independent of `max_replay_attempts`, proven to persist across separate `call()` invocations on the same gateway instance and to fail closed immediately once exhausted, regardless of how much per-trigger-site budget remains. |
| `TokenBudget` accounts every replay attempt's real usage, not just the finally-returned response's. | **INCONCLUSIVE, narrowed and partially addressed.** `ContentReplayGateway::call()`'s own return type structurally can only carry ONE `Usage` value to its caller — this is unavoidable given `ModelCallGatewayLike`'s existing signature, not a bug this ADR introduces. What IS built and proven: `ContentReplayTraceHook` fires once per attempt (discarded or not) carrying that attempt's real `Usage`, making every attempt's true cost observable to whatever consumes the hook. Whether `rt::AgentSession`'s own `TokenBudget<N>` enforcement is ever wired to that hook — the draft's original concern — remains genuinely untested and unimplemented; this ADR does not claim it, and marking it CORRECT would launder an unverified integration into a settled one (`decisions/README.md` §6's own rule). |
| Streaming calls are never subject to replay. | **CORRECT, structurally** — `ContentReplayGateway` declares no `chat_stream()` method; there is no expression by which a caller could route a streaming call through it, the identical "proof by absence" already used for `Compactor<N>`/`history[]` in ADR-067. |
| A replay attempt's own response is re-checked by the same predicate (bounded recursion). | **CORRECT** — proven via an explicit trigger-call counter (2 calls for one discard-then-succeed scenario), not merely inferred from the loop's own shape. |

## 7. The decision

**Design B, as corrected during implementation (§5), is adopted and implemented**, scoped to
non-streaming calls only. It binds:
- `decisions/ADR-033-middleware-model-call-chain.md` / `decisions/ADR-036-model-call-gateway.md` —
  composes OVER a `ModelCallGatewayLike` (typically a `MiddlewareModelCallGateway<...>`), rather than
  extending `ModelCallContext`/`MiddlewareModelCallGateway` itself — a narrower binding than
  originally drafted (§5), leaving ADR-033/036's own shipped, Judged code completely untouched.
- Does NOT bind `002-Agent-Model-and-Authoring.md` §3's `Retry<Policy>` — explicitly a distinct
  mechanism (content-triggered, not transient-failure-triggered), not an extension of it.
- Does NOT reuse `MiddlewareTraceEvent`/`MiddlewareTraceHook` verbatim as originally drafted — a
  separate `ContentReplayAttemptEvent`/`ContentReplayTraceHook` pair was built instead, because this
  mechanism's own event shape (per-attempt `Usage`, `discarded`, a `reason` string) doesn't fit
  `MiddlewareTraceEvent`'s `{middleware_name, hook, settled_here, threw}` shape without overloading
  fields to mean something different than they do at the model-call point — named here as a real,
  minor divergence from the draft's own §5 audit-reuse intent, not silently substituted.

**Open question, deliberately not settled by this ADR, and not narrowed by implementation**: whether
`decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md`'s `pre_model` mechanism and this
ADR's `post_model`-adjacent mechanism are genuinely non-overlapping, or two designs solving adjacent
halves of one problem that should be unified. Both are now implemented and both are standalone,
un-wired into `rt::AgentSession` — there is no shared code path between `turn_middleware.hpp` and
`content_replay_gateway.hpp` (confirmed: neither includes the other) to even test overlap against yet.
A future pass implementing both together, wired into one real session, must confirm this before either
ships as a session-level feature.

**Residual risks:**
- **No production call site wires `ContentReplayGateway<Inner>` into `rt::AgentSession`'s own
  chat-client slot anywhere** — this ADR proves the mechanism in isolation, matching every other ADR
  in this batch's "prove the mechanism, name the rest" precedent.
- `rt::AgentSession`'s own `TokenBudget<N>` enforcement is not wired to `ContentReplayTraceHook` —
  every attempt's real usage is now OBSERVABLE to a host-wired hook, but the ENGINE's own existing
  budget enforcement does not itself consume it yet (§5/§6's own narrowed verdict on this point).
- Whether 017 §4's full `pre_model`/`post_model` verdict vocabulary (`allow`/`annotate`/`redact`/
  `require_approval`/`deny`) should eventually replace this mechanism's narrower discard-and-retry, or
  whether discard-and-retry is the permanent, sufficient answer for the post-model case — still open,
  unchanged by implementation.
- The corrective-message-only retry-request construction (§1's own real finding) has not been checked
  against every possible trigger use case (e.g. a trigger wanting to preserve SOME safe portion of the
  discarded response for continuity) — the current mechanism is all-or-nothing (discard entirely,
  replace with one corrective message), which may be too blunt for some future trigger authors; not
  addressed here.
