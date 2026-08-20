# Design draft: content-triggered response replay

**Status:** Design draft, workflow-reviewed 2026-08-20 (fixes applied below, see §7). No code
written. Independent of `context-provider-provenance-design-draft.md` and `runtime-secret-
quarantine-design-draft.md` specifically. **Relationship to `context-turn-middleware-and-compaction-
design-draft.md` is a real open question this draft itself raises (§6's last bullet), not settled
independence** — corrected from an earlier version of this header that claimed flat independence
from all three. This mechanism hooks into `Middleware`'s already-shipped MODEL-CALL `after_model`
point (`decisions/ADR-033-middleware-model-call-chain.md`, real code today), which runs AFTER the
model call, distinct from the turn-middleware draft's `TurnContext`, which runs BEFORE it — the two
are believed non-overlapping but this has not been formally confirmed against both drafts together.
`runtime-secret-quarantine-design-draft.md` is one possible TRIGGER source for this mechanism (§1),
but this draft's replay machinery is general-purpose and does not depend on that draft shipping
first, or at all.

## 1. The gap this closes

`runtime-secret-quarantine-design-draft.md` §2e covers the CHEAP case: the model notices a problem
mid-round, before finalizing a response, and calls `quarantine_secret` as an ordinary tool — no
replay needed, the existing multi-round tool-call loop already handles it.

This draft covers the case that draft explicitly excludes from its own scope: **a response has
already been produced and is about to commit to history, and only THEN does something detect it
contains a secret (or any other content-policy violation) that should never have been said.**
Nothing in this codebase today discards an already-produced response and retries with corrected
input — `Retry<Policy>` (`002-Agent-Model-and-Authoring.md` §3, "The policy vocabulary" — corrected
citation; an earlier version of this draft attributed this table to "027 §4," which is actually
"Canonical names — workflow, trust, runtime" and contains no such table) is explicitly a
*"Transient-failure retry shape"* — retrying because a call errored, not because a call succeeded
but produced content that shouldn't be kept. This is new vocabulary, not a reuse of that mechanism.

## 2. Honest scope limit, stated up front

Replaying the turn **cannot un-send** what the model provider already received in this round's
request — if the secret was already IN the prompt sent to the vendor (e.g. it arrived via a tool
result the model then read), that transmission already happened and no local mechanism reverses it.
What this draft actually achieves: preventing the tainted response from **committing to durable
history/checkpoint/transcript**, and giving the model a corrected instruction to continue from. This
is containment of what AgentEngine persists and shows the user, not retroactive non-disclosure to
the vendor. State this limit explicitly wherever this mechanism is documented — do not oversell it.

## 3. The mechanism

Hooks into the EXISTING `after_model` point (`middleware.hpp`, real code, ADR-033) rather than
adding a parallel interception point:

```cpp
struct ContentReplayDecision {
    bool discard_and_retry = false;
    std::string corrective_instruction;   // injected as additional context for the retry attempt
};
```

An `after_model` hook inspects the settled `ChatResponse` (already exactly what `ModelCallContext`
carries, `middleware.hpp`, ADR-033 §4). If it detects a violation (a secret pattern — reusing
`runtime-secret-quarantine-design-draft.md`'s detector as one possible implementation, not a
dependency of the mechanism itself — or any other content-policy trigger a future filter wants to
plug in here), it returns `discard_and_retry = true`.

**Distinct from an ordinary `after_model` short-circuit**: ADR-033's existing chain semantics let a
hook substitute a response or deny; this needs the wrapper's CALLER (`MiddlewareModelCallGateway`,
`model_call_gateway.hpp`, ADR-036) to re-invoke the underlying model call with an amended
`ChatRequest` (appending `corrective_instruction`), not just return a different value once. This is
new: neither `run_before`/`run_after` (`middleware.hpp`) nor `ModelCallGateway`'s existing retry
(transient-failure-triggered, §1) has a "re-run because of settled-response content" path today.

**Bounded per trigger site, AND bounded across the session's lifetime (must-fix, was missing)**: a
`max_replay_attempts` (small, e.g. 1-2) alone is insufficient. The review workflow found replay
happens inside one model-call wrapper invocation without incrementing `effect_context_.turn_index`
(`agent_session.hpp`'s turn loop only advances `turn_index` per round) — structurally outside
`MaxTurns<N>`'s reach (002 §3, default 16). A steered model can re-trigger the same violation across
many different TURNS, each getting its own fresh per-trigger-site budget of `max_replay_attempts` —
small local bound, unbounded session-lifetime cost.

**Fix**: replay attempts are additionally capped against a session-lifetime counter (part of
`AgentSession`'s own state, not per-trigger-site state), independent of and in addition to
`max_replay_attempts`. Once the session-lifetime cap is hit, this mechanism fails closed for the
REST of the session (surfaces an error, no further replay attempts of any kind), not just for the
current trigger site — matching this project's own general "bounded, not silent" posture (`MaxTurns`
already bounding `run_rounds()`'s own loop the same way, at a different layer).

**`TokenBudget` accounting, must be proven, not assumed**: `TokenBudget<N>` (002 §3, real and
fail-closed per `agent_session.hpp:1137`'s own comment) must account EVERY replay attempt's real
backend call — not just the usage of whichever attempt's response is ultimately kept. A design that
only charges the final, returned response's token usage undercounts real cost by up to
`max_replay_attempts`-fold per trigger site. This must be verified against `TokenBudget`'s actual
accounting call sites before this mechanism ships, not asserted here as already true.

## 4. Why this is general-purpose, not secret-specific

The trigger predicate (`after_model` hook body) is pluggable — the SAME replay machinery serves any
"discard this settled response, retry with a correction" need: a toxicity/policy classifier catching
something after the fact, a schema-violation the model's free text drifted into, a hallucinated
citation a downstream check flags. `runtime-secret-quarantine-design-draft.md`'s Case 2 (secrets
already committed to a response) is ONE caller of this, not the reason it exists — the mechanism
itself belongs in this draft, standalone, precisely because tying it to secrets specifically would
make it a one-off instead of reusable infrastructure.

## 5. Attribution and audit

Every replay is a real event: which hook triggered it, how many attempts, what the discarded
response contained (metadata only, per `runtime-secret-quarantine-design-draft.md` §3's same
"never persist the sensitive value itself" reasoning if the trigger was a secret — the audit
principle generalizes even though the mechanism doesn't depend on that draft). Should reuse
`MiddlewareTraceEvent`/`MiddlewareTraceHook`'s existing shape (ADR-033 §4) rather than invent a
second trace vocabulary for the same kind of event.

## 6. Open questions, and two now resolved

- **Resolved (§3)**: cost is accounted via the session-lifetime replay cap plus `TokenBudget` proof
  obligation added above — no separate budget primitive needed, reuses what exists.
- **Resolved: streaming is gated OUT of this mechanism's scope, firmly, not "maybe."** ADR-033
  already made this exact decision for the identical point (`chat_stream()` forwarded uninterceped,
  needs its own design, §2 of that ADR) — this draft inherits it rather than re-opening it. If a
  violation is only detectable after a response has ALREADY streamed to the caller, replay is moot —
  the content is already out; a streaming-specific buffer-then-release design is real, valuable
  follow-up work, but building it before the non-streaming mechanism itself is proven (§3's own
  still-unverified `TokenBudget` accounting) would repeat the same premature-widening risk this
  project's own precedent (ADR-028, ADR-033 §2) already warns against.
- **Does a replay attempt's OWN response need to go through the SAME `after_model` check again**
  (recursion, bounded by `max_replay_attempts`) — confirm this is the intended semantics of §3, not
  an oversight.
- **Where does this sit relative to the `turn`-level compaction/filter mechanism**
  (`context-turn-middleware-and-compaction-design-draft.md`)? That draft's `turn` point runs BEFORE
  the model call (shaping what's sent); this draft's `after_model` point runs AFTER (reacting to
  what came back). Confirm these are genuinely non-overlapping, not two designs solving adjacent
  halves of one problem that should actually be unified.

## Red-team findings (workflow review)

Reconciled from three independent reviews (connectivity/orphan audit, feature-advocate,
safety-advocate) run against all four drafts, cross-checked against real code (`agent_session.hpp`,
002 §3) and ADR-033.

| # | Finding | Severity | Fix |
|---|---|---|---|
| 1 | Replay happens inside one model-call wrapper invocation without incrementing `effect_context_.turn_index` (confirmed: `agent_session.hpp`'s turn loop only advances `turn_index` per round), so it is structurally outside `MaxTurns<N>`'s reach (002 §3, default 16). Whether `TokenBudget<N>` (002 §3, real and fail-closed per `agent_session.hpp:1137`'s own comment) accounts each replay's real backend call — versus only the final settled response's usage — is asserted nowhere in this draft, only left as its own open question. A steered model can re-trigger the violation across many different turns, each getting its own fresh per-site budget: small local bound, unbounded session-lifetime cost. | Must-fix | Cap replay attempts against a session-lifetime counter, not just a per-trigger `max_replay_attempts`, and prove — not assume — that `TokenBudget` sees every replay attempt's real backend usage (each retried call, not just the one whose response is ultimately returned) before this ships. |
| 2 | §1 attributes the `RetryPolicy`/`BreakerConfig` "Transient-failure retry shape" table to "027 §4." The table is actually `Retry<Policy>` in `002-Agent-Model-and-Authoring.md` §3 ("The policy vocabulary," line 105); `027-Vocabulary-and-Naming.md` §4 is "Canonical names — workflow, trust, runtime" and contains no such table. | Must-fix | Correct the citation to `002-Agent-Model-and-Authoring.md` §3. |
| 3 | The header states this draft is "Independent of the other three drafts in this batch," but §6's own open question asks the review workflow to "Confirm these are genuinely non-overlapping" against the turn-middleware draft — a live, unresolved question the header's flat independence claim doesn't reflect. | Must-fix | Reword the header to hedge the same way §6 already does: independent of the provenance and secret-quarantine drafts specifically; relationship to the turn-middleware draft's `turn`-point mechanism is a real open question this draft itself raises in §6, not settled independence. |
| 4 | §6 leaves streaming as an open "maybe excluded" question, even though ADR-033 already made a firm decision on the identical point (`chat_stream()` is forwarded uninterceped; needs its own design) that this draft could simply inherit rather than re-opening as unresolved. | Worth-noting (resolved: keep deferred, make it firm) | Adopt ADR-033's own precedent explicitly: this mechanism is gated to non-streaming calls only for its initial scope, full stop — not "maybe." A streaming-specific buffer-then-release design is real, valuable follow-up work, but building it now, before the non-streaming mechanism itself is proven (see finding #1), repeats the same premature-widening risk. |

**Applied**: finding 1 (§3 now adds a session-lifetime replay cap and a `TokenBudget`-accounting
proof obligation), 2 (§1 citation corrected to `002-Agent-Model-and-Authoring.md` §3), 3 (header
hedged to match §6's own open question about the turn-middleware draft), 4 (§6 streaming resolved
firmly deferred) are all incorporated above.
