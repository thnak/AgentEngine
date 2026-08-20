# ADR-069 — When a model response has already committed content that violates policy, does AgentEngine discard-and-retry inside the existing `after_model` point, or leave remediation entirely to the host?

**Status:** Design — question, competing designs, and decision recorded from a design→red-team→judge
pass. **NOT Proposed, NOT Judged**: no implementation exists, no executed evidence, no prove phase
has run (§5). Independent of `decisions/ADR-066-context-provider-attribution-provenance.md` and
`decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md`. **Relationship to
`decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md` is a real open question this ADR
itself raises (§7), not settled independence** — that design's `TurnContext` runs BEFORE the model
call, this one's `after_model` hook runs AFTER; believed non-overlapping, not formally confirmed
against both designs together.

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

## 5. Executed evidence

**None.** No implementation exists — not the `after_model` hook's replay-trigger contract, not the
session-lifetime cap, not the `TokenBudget`-accounting fix. The must-fix findings in §4 were found and
fixed at the design level (the review workflow attacked the proposed bounding logic against real
`agent_session.hpp`/002 §3 citations, catching a real gap between what the design claimed and what the
cited code actually does), but this is not a substitute for implementation, compilation, and testing.

## 6. Per-claim verdicts

Every claim in §3: **INCONCLUSIVE — no executed evidence exists to decide it.**

## 7. The decision

**Design B is adopted as the target for the future prove phase**, scoped to non-streaming calls only.
It binds:
- `decisions/ADR-033-middleware-model-call-chain.md` — extends the `after_model` point's consumer
  (`MiddlewareModelCallGateway`, ADR-036) with a re-invoke path; reuses `MiddlewareTraceEvent`/
  `MiddlewareTraceHook` for audit rather than inventing a second trace vocabulary.
- Does NOT bind `002-Agent-Model-and-Authoring.md` §3's `Retry<Policy>` — explicitly a distinct
  mechanism (content-triggered, not transient-failure-triggered), not an extension of it.

**Open question, deliberately not settled by this ADR**: whether `decisions/ADR-067-middleware-turn-
point-pre-model-enforcement.md`'s `pre_model` mechanism and this ADR's `post_model`-adjacent
mechanism are genuinely non-overlapping, or two designs solving adjacent halves of one problem that
should be unified — a future pass implementing both together must confirm this before either ships,
not assume it from the two designs' current, independently-written text.

**Residual risks:**
- The entire §5/§6 evidence gap.
- `TokenBudget`'s actual per-replay-attempt accounting behavior — asserted as a requirement, not yet
  verified against real code.
- Whether 017 §4's full `pre_model`/`post_model` verdict vocabulary (`allow`/`annotate`/`redact`/
  `require_approval`/`deny`) should eventually replace this mechanism's narrower discard-and-retry, or
  whether discard-and-retry is the permanent, sufficient answer for the post-model case — named as a
  real design question in the workflow review's judge synthesis, deliberately left open rather than
  settled by fiat.
