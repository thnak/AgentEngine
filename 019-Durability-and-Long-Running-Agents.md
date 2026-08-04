# 019 — Durability and Long-Running Agents

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 001, 005, 014, Quark 012/017/027 · **Gate:** §7

## Goal

Let a run last hours, days, or weeks — surviving process restarts, node loss, and deploys — without
holding resources while it waits, and without an interrupted run corrupting state or duplicating
external effects.

## 1. Checkpoints

- **Boundaries** are turn boundaries (001 §3) and workflow superstep boundaries (014 §5). These are
  the only points where state is consistent by construction.
- **Content** is the run's position, session delta, workflow executor states and in-flight messages,
  pending approvals/input requests, and the capability set (recorded as *references*, never as live
  handles — a serialized capability handle would be a forgeable capability).
- **Storage** is the Quark 012 `Store` seam, shared with sessions (005 §2). No second persistence
  engine.
- **Cost is bounded**: incremental deltas plus periodic full checkpoints, with the cadence a policy.

## 2. Suspension

A `Suspended` run holds **no activation, no sandbox, no connection, no thread**. It is a durable
record plus a wake condition. Wake conditions:

| Condition | Mechanism |
|---|---|
| Human/caller input | `InputRequired` resolution (001 §2, 013 §5) |
| Timer / schedule | Quark durable reminders (027) — at-least-once, wall-clock, mass-due-safe |
| External event | Webhook, queue message, or A2A push (012 §2) |
| Remote task completion | Poll or callback from a remote A2A/MCP task |
| Local background task completion | A `Backgroundable` tool call detached via `background_task` (006 §6b) |
| Manual resume | Operator action |

Suspension is what makes "an agent that waits three days for approval" cost storage rather than a
process. A framework that keeps a coroutine parked on a live connection for three days does not
survive a deploy.

**Agent-callable, not just host-triggered.** The first three rows above are not only something the
*host* arranges — `schedule_wakeup` and `watch_resource` (006 §6b) let the model itself arm a
Timer/schedule or External-event wake and then end its turn, driving the same suspension this section
already specifies. This adds a caller, not a new state machine.

## 3. Exactly-once effects

The hard part. Restart and rewind both re-execute code; external effects must not double-apply.

- **Every effect carries an idempotency key** derived deterministically from
  `{run_id, turn_index, call_index, argument_digest}`. Deterministic derivation is what makes the
  key survive a restart.
- **Effect journaling**: the intent to perform an effect is journaled *before* execution and the
  outcome journaled *after*. On resume, a journaled-but-unconfirmed effect is reconciled — retried
  with the same key, or surfaced as indeterminate, never silently retried without a key.
- **Effects are classified** by the tool declaration: `pure` (safe to repeat), `idempotent` (safe
  with a key), `at-most-once` (must not be repeated — journaled, and on ambiguity surfaced to a
  human rather than guessed).
- Quark 017's transactional-outbox discipline backs outbound messaging; this RFC does not invent a
  delivery mechanism.

**Stated plainly:** for an `at-most-once` effect interrupted at exactly the wrong moment, the engine
reports *indeterminate* rather than guessing. Guessing is how a payment gets made twice.

## 4. Recovery

- **Process restart**: sessions and runs reload on demand from the store; suspended runs wake on
  their conditions.
- **Node loss**: Quark placement (010/026) reactivates the session elsewhere; fencing prevents two
  activations of the same session, which is the split-brain that would corrupt history.
- **Poison runs**: a run that fails repeatedly on resume is quarantined after a bounded number of
  attempts, with its state preserved for inspection — not retried forever, and not discarded.
- **Deploys**: a version-skew policy declares whether an in-flight run resumes on a new agent
  version or pins to its original (default: **pin**, because resuming a run under changed
  instructions silently changes its behaviour mid-flight).

## 5. Retention and lifecycle

Checkpoints, recordings, artifacts, and audit records each have independent retention policies —
they have different legal and operational lifetimes and must not share one setting. Garbage
collection of abandoned runs is policy-driven, audited, and reversible within a grace period.

## 6. Interaction with time-travel

Rewinding a workflow (014 §5) rewinds *state*, not the world. On re-execution:

- `pure` effects re-run freely;
- `idempotent` effects re-run under their original keys;
- `at-most-once` effects **require explicit operator acknowledgement** before re-execution.

This is the guard rail that keeps time-travel a debugging tool rather than an incident generator.

## 7. Promotion gate

- **G1** — kill -9 at every checkpoint boundary of a scripted multi-turn run; every resume completes
  with output identical to the uninterrupted control.
- **G2** — an interrupted `idempotent` effect is retried exactly once end-to-end under a fault
  injector that duplicates and delays; the external counter reads exactly 1 over 10⁴ trials.
- **G3** — a suspended run's resident cost is storage-only: no activation, no sandbox, no
  connection, no thread (measured by census, not asserted).
- **G4** — 10⁶ reminders due simultaneously wake without a thundering herd (inherits Quark ADR-017's
  mass-due gate).
- **G5** — a poison run is quarantined after its bound with state intact and an operator-visible
  reason.
- **G6** — an `at-most-once` effect is interrupted by a fault injector at exactly the ambiguous
  moment (journaled intent, unconfirmed outcome); over 10⁴ trials the engine surfaces
  *indeterminate* every time — never auto-retries, never silently drops. §6's rewind path is
  exercised against the same halted effect and confirmed to refuse re-execution until an operator
  acknowledgement is recorded.

## 8. Open questions

- ~~**Q1** — Whether capability *references* in a checkpoint should be re-validated against current
  policy at resume (they should) and what happens when policy has since narrowed (unspecified).~~
  **Resolved, both halves (2026-08-04):**
  1. **Re-validation is forced by construction, not a choice.** §1 already forbids serializing live
     capability handles ("a serialized capability handle would be a forgeable capability") — a
     checkpoint only ever holds a reference. Resuming necessarily means re-deriving the actual grant
     from current policy (007 §5); there was never another way to do it. This was already true; it
     just hadn't been stated as forced rather than recommended.
  2. **When policy has narrowed: resume with the narrower set, never refuse to resume the whole run
     over it, and surface the narrowing explicitly.** A capability held at checkpoint time but not at
     resume behaves exactly like any other ungranted capability already does (001 §6, 007 §3's
     empty-by-default doctrine) — a future call to it fails with the ordinary `Policy`-classified
     failure the model is already expected to be robust to, not a new failure shape needing new
     design. What's added: resume emits a `Warning` event (013 §1, already a first-class run event)
     naming which capabilities narrowed, so the gap is visible immediately rather than only
     discovered when a specific call happens to fail later. Refusing to resume entirely would turn a
     routine, encouraged policy tightening into an operational incident for every in-flight run
     holding the now-narrower capability — exactly the wrong incentive for an operator trying to
     tighten a policy.
- ~~**Q2** — Long-running tool calls have three competing shapes: MCP's tasks extension, A2A tasks,
  and our `Suspended` state.~~ **Resolved (OQ-4):** the three shapes were never peers at the same
  granularity, and unifying them meant recognizing that, not building one new abstraction:
  - **A2A tasks already are our `Suspended` run** — `Task ← Run` (012 §1) is a direct identity, not
    a mapping to design. A long-running remote A2A task is consumed as our own `Suspended` state
    (§2's "Remote task completion" row); nothing new was needed here.
  - **MCP's tasks extension is scoped to one long *tool call*, not a whole run** — MCP's stateless
    design has no run-level "come back later" concept beyond MRTR's retry-with-`requestState`
    (already unified via `interaction_id`, 001 §2). A single long-running MCP tool invocation maps
    onto the existing `Backgroundable`/`StandingEffect` mechanism (006 §6b): `tasks/get` polling is
    served from the same durable handle `list_standing_effects` already exposes, not a second
    tracking structure.
  - **Where MCP needs to represent a whole *run* pausing** (not just one tool call), that is
    `InputRequired`/`Suspended` again, and it is carried the same way MRTR already carries it:
    `interaction_id` inside `requestState` (001 §2, 013 §2.2). MCP gets no third mechanism.
  
  Net result: no new unification primitive. `Interaction`/`interaction_id` (whole-run pauses) and
  `StandingEffect` (single detached calls) were each already being built for other reasons this
  session and turned out to jointly cover every shape MCP, A2A, and our own model needed.
- ~~**Q3** — Cross-node workflow checkpoint consistency (014 Q4).~~ **Resolved (2026-08-04):** see
  014 §9 Q4 — the superstep barrier already provides the global quiescence point a distributed
  snapshot protocol would otherwise need to establish; a two-phase (pending → per-executor persisted
  → committed) checkpoint commit reuses this RFC's own §3 intent-then-confirm discipline rather than
  inventing a new consistency mechanism. New gate: 014 §8 G6.
- ~~**Q4** — Whether recordings should be first-class durable objects with their own lifecycle, given
  they are simultaneously a debugging asset and a prompt archive (016 §4).~~ **Resolved, Yes, the
  same "new schema on the existing `Store` seam" pattern this project already applies elsewhere
  (2026-08-04):** §5 already gives recordings their own retention-policy slot, distinct from
  checkpoints/artifacts/audit — the remaining question was whether that's backed by one addressable
  record type or remains an implicit by-product scattered across 001 §7's five recorded seams
  (provider call, sandbox execution, tool invocation, clock, RNG). Making it first-class — one
  record, keyed by `run_id`, aggregating all five kinds of recorded nondeterminism for that run,
  stored through the same `Store` seam every other durable object already uses (005 §2, §1 above,
  030 §3's identical pattern for Project manifests) — is what makes 016 §4's prompt-archive concern
  actually tractable: a redaction request has exactly one thing to reach per run, not five separately-
  tracked seams each needing its own redaction path verified independently, closing a gap 005 §6 /
  017 §5's "redaction must propagate to... recordings" rule would otherwise leave implicit. No new
  storage engine — a new schema on the existing seam.
