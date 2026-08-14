# ADR-043 — `ack_policy`: sequencing durable persistence ahead of a turn's acknowledgment

**Status:** Proposed (2026-08-14). Designed, self-red-teamed, implemented, and proven (real code +
new test file, full suite green); awaiting the project owner's explicit "Judged" sign-off per this
project's governance (`decisions/README.md`; `OpenQuestions.md` OQ-11).

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #15 (the finding this
ADR closes). `005-Sessions-State-and-Memory.md` §2 (the acknowledgment-protocol contract this ADR
implements — quoted directly in §2 below). `include/agentengine/rt/message_codec.hpp` (ADR-037 Phase
3 Slice 2's already-proven Message↔JSON codec, reused here rather than re-invented — this ADR is a
second real consumer of it, not its origin).

## 1. The question

**Stated so it has a wrong answer:** does `rt::AgentSession` acknowledge a turn to its caller only
after that turn's effects are durable, or can the caller observe a successful response before any
durable write has happened at all — and if the latter, is there a real, working opt-in to the former?

**Before this fix: acks always race ahead of durability, unconditionally, with no opt-in to change
that.** Confirmed against current code (`run_rounds()`, `include/agentengine/rt/agent_session.hpp`):
the no-tool-calls convergence branch pushes the response into `history_`, emits `turn_finished`/
`run_finished` (internal telemetry events), and returns `AgentResponse` directly to whatever called
`start_run()` — there is no `SessionStore` write anywhere in `run_rounds()`, `start_run()`, or
`resolve_interaction()`. Durability is entirely a separate, caller-orchestrated step
(`save_agent_session_snapshot()`/`checkpoint_if_due()`) that a caller may or may not perform, at any
time, with no ordering relationship to the response the caller already received.

## 2. What re-grounding against current code found (corrections to the audit, not just confirmations)

The audit's own recommended approach: *"Add an AckPolicy (default AtMostOnceAck, opt-in
RequireDurableAck), but the proposed insertion point sits after run_finished is already emitted
(breaks a tested invariant), and the required type-erased Store seam doesn't exist yet."*

- **The "type-erased Store seam doesn't exist yet" claim is wrong, on two counts.** A `SessionStore`
  seam already exists (`include/agentengine/rt/session_store.hpp`) and is already load-bearing —
  templated into `save_agent_session_snapshot()`/`load_agent_session_snapshot()`/
  `checkpoint_if_due()`/`delete_session()`, all real, already-wired free functions. And it was never
  *meant* to be type-erased in the first place: `SessionStore` is a C++20 `concept` (compile-time duck
  typing), matching this project's own established `ChatClient`/`SecretStore` precedent — an
  intentional design choice, not a missing seam. `checkpoint_if_due()`'s own comment states the real
  reason no Store access lives inside `AgentSession` itself: **"AgentSession has no ambient Store
  access, I2."** This ADR's design (§3) honors that same boundary rather than adding one.
- **005 §2 already specifies the exact contract, in its own vocabulary — not the audit's invented
  type names.** §2's text: *"a turn is acknowledged to the caller only after its effects and history
  delta are durable, or the session declares an `at_most_once_ack` durability policy... Silently
  acknowledging before durability is the failure mode that loses a user's conversation on a crash."*
  G2: *"kill -9 mid-turn; on restart, no acknowledged turn is lost."* This ADR's `ack_policy` enum
  values (`at_most_once`, `require_durable`) are named to match, not invented independently.
- **005 §2's own wording — "history delta," not "full history" — turns out to matter a great deal.**
  `AgentSessionRecord`'s own comment already names full-conversation serialization as a separate,
  larger, not-yet-built gap (a real, still-open residual — §5). But 005 §2 does not require the WHOLE
  conversation to be durable before acking one turn; it requires *that turn's own delta*. `TurnView`
  already models exactly this concept (the messages one turn added), and
  `include/agentengine/rt/message_codec.hpp` already has a complete, proven `Message`↔JSON codec
  (built for `WorkflowSupervisor`'s own checkpoint record). This makes "durably persist this turn's
  actual content, not just session bookkeeping" tractable *now*, not blocked on the larger
  full-history migration — a real, actionable reframing the audit's own citation didn't surface.

## 3. The design

**No change to `AgentSession`'s own methods or internals.** `run_rounds()` is untouched — this
directly avoids the audit's own concern about inserting a durability wait relative to
`run_finished`'s emission (an internal telemetry event, distinct from the caller-visible "ack" 005 §2
actually governs — see §4 for why that distinction is load-bearing, not a technicality).

**`ack_policy` is enforced entirely in two new free functions**, `start_run_with_ack_policy()` and
`resolve_interaction_with_ack_policy()` (`agent_session.hpp`, alongside `checkpoint_if_due()`), both
taking `AgentSession&` and `StoreT&` as separate parameters — the exact same shape every existing
Store-touching free function in this file already uses, preserving I2 rather than giving
`AgentSession` a self-referencing Store hook:

1. `ack_policy::at_most_once` (default, matching current behavior exactly): calls the underlying
   `start_run()`/`resolve_interaction()` and returns its result unchanged. Zero store interaction.
2. `ack_policy::require_durable`: after the underlying call succeeds, durably writes **both** the
   turn's own history delta (new `TurnDeltaRecord`/`save_turn_delta()`, reusing
   `rt/message_codec.hpp`) **and** the session's bookkeeping record (`save_agent_session_snapshot()`,
   already-existing), in that order — the caller only sees the successful `AgentResponse` once both
   writes succeed. Either failing surfaces as `run.durable_ack_failed`, never a false-success value.

**A real, latent header-hygiene bug this ADR surfaced and fixed, unrelated to the design itself**:
`rt/message_codec.hpp`'s own internal calls to `content_item_to_json`/`role_to_wire_string`/
`origin_to_wire_string` were unqualified. Since their arguments (`ContentItem`, `role`,
`content_origin`) live directly in namespace `agentengine`, and `core/chat_recording.hpp` maintains
its own separately-defined, identically-named/identically-shaped functions in that same namespace
(`message_codec.hpp`'s own top comment already explains this is a deliberate duplicate, not a shared
`#include`), any translation unit including *both* headers hit a genuine ADL ambiguity — invisible
until this ADR's new `#include "agentengine/rt/message_codec.hpp"` in `agent_session.hpp` combined,
for the first time, with an existing test file that already included `chat_recording.hpp` directly
(`test_rt_secret_hygiene_canary_scan.cpp`). Fixed by fully qualifying every such call
(`agentengine::rt::content_item_to_json(...)` etc.) in both `message_codec.hpp` and this ADR's own new
code in `agent_session.hpp` — behavior-neutral (both implementations are the same logic), a
disambiguation fix, not a functional change.

## 4. Self-red-team findings

**Why the internal `run_finished` event's timing is not the same question as the caller-visible
ack, and why that distinction is load-bearing, not a rationalization.** 005 §2 says a turn is
"acknowledged to the *caller*." `run_finished` is an internal telemetry event
(`emit_run_event()`, 013 §1's event stream, projected onto AG-UI/observers) — a side channel for
observability, not the actual value crossing the trust/durability boundary. The real ack is the
`AgentResponse` *value* reaching whoever asked for it, which is exactly what
`start_run_with_ack_policy()` controls: for `require_durable`, that value is withheld (replaced by an
error) until durability succeeds, regardless of what the internal event stream already emitted. This
is not a loophole in 005 §2's contract — internal observability events firing as things happen is a
different, unrelated concern from whether the *return value* a caller can act on has been gated
correctly.

**A real, named concurrency residual, not silently accepted.** The delta capture
(`session.history().size()` before/after the underlying call) is correct for the single-caller,
sequential turn-taking usage 005 §2's own G2 gate describes — the dominant, expected pattern. It is
**not** safe against a second, genuinely concurrent, overlapping `start_run()` call on the *same*
`AgentSession` instance racing between this wrapper's own call returning and its subsequent
`history()` read: I1's FIFO `session_mutex_` serializes each individual `start_run()`/
`resolve_interaction()` call, but does not extend that critical section to this wrapper's own
post-hoc read. A fully race-free version would need to capture the delta *inside* `run_rounds()`'s own
locked region — a `run_rounds()` change this ADR deliberately avoids (§3's own reasoning: avoiding
exactly the kind of internal-loop change the audit flagged as its own blocker). Named explicitly in
the code (`start_run_with_ack_policy()`'s own doc comment) and here, not discovered later.

**Fail-closed on durability failure was a deliberate choice, checked against the alternative.** When
the delta or snapshot write fails under `require_durable`, the caller receives an error, not the
successful `AgentResponse` — even though the model call itself succeeded and the message already
lives in `history_` in-process. This matches 005 §2's own stated failure mode ("silently acknowledging
before durability... loses a user's conversation on a crash") — if it wasn't durably saved, it must
not be reported as durably acknowledged. The alternative (return the response anyway, with a
durability-failed flag) was considered and rejected: it would let a caller who opted into
`require_durable` specifically to avoid data loss silently ignore exactly the signal that data loss is
about to happen.

**`resolve_interaction_with_ack_policy()` is proven only by compiling, not independently
behavior-tested.** It is structurally the identical sequencing as `start_run_with_ack_policy()`
applied to `resolve_interaction()`'s own result shape — building the fixture needed to drive a
suspend-for-approval round to completion (tool descriptors, an approval decider, a real interaction)
would have meant substantially duplicating `test_rt_agent_session_suspend_approval.cpp`'s own harness
for no new logic path. Named as a real scope reduction, not a silent gap.

## 5. What this ADR does not claim

- **Does not achieve full 005 §2 compliance for the WHOLE conversation** — `AgentSessionRecord` still
  does not serialize the entire `history_` array (a separate, larger, already-named gap). This ADR
  closes the part 005 §2's own wording actually requires per-turn ("its effects and history delta"),
  via the new, narrower `TurnDeltaRecord` mechanism — a real, working, byte-verified durability
  guarantee for the turn just acknowledged, not a hollow policy switch that only persists bookkeeping.
- **Does not close the concurrent-overlapping-calls residual** (§4) — real, scoped follow-up work,
  needing a `run_rounds()`-internal change this ADR deliberately avoided.
- **Does not wire any restore/rehydration path.** `load_turn_delta()` exists (symmetric with
  `save_turn_delta()`) but nothing calls it to reconstruct `history_` on session restart — that is a
  separate, larger "how does a restored session's in-memory state get built from durable records"
  design question, out of scope here.
- **Does not touch `run_rounds()` or any `AgentSession` method** — the entire mechanism lives in two
  new free functions, preserving I2 exactly as the existing `checkpoint_if_due()` precedent does.

## 6. Evidence

`tests/test_rt_agent_session_ack_policy.cpp` (new file, T1-T3), against a real `AgentSession` and a
real `InMemorySessionStore`/a configurable-failure `FlakyStore` fixture:

- **T1**: `at_most_once` makes zero `store.save()` calls — byte-identical to today's unchanged
  behavior.
- **T2**: `require_durable`'s success path — the caller still receives the real response (not just a
  receipt); the session bookkeeping record is durably present; the turn's own delta is durably
  retrievable via `load_turn_delta()` and, decoded back through the full JSON round trip, contains
  *both* the turn's input and its response text, byte-correct.
- **T3**: `require_durable`'s failure path — an injected store failure surfaces as
  `run.durable_ack_failed`, never a false-success `AgentResponse`.

Full suite: green (`ctest`, this pass — see the commit's own test count), including the pre-existing
`test_rt_secret_hygiene_canary_scan.cpp`, which exercises the exact ADL collision §3 fixed.
