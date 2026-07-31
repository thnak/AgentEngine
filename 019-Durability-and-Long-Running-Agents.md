# 019 — Durability and Long-Running Agents

**Status:** Draft · **Depends on:** 001, 005, 014, Quark 012/017/027 · **Gate:** §7

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
| Manual resume | Operator action |

Suspension is what makes "an agent that waits three days for approval" cost storage rather than a
process. A framework that keeps a coroutine parked on a live connection for three days does not
survive a deploy.

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

## 8. Open questions

- **Q1** — Whether capability *references* in a checkpoint should be re-validated against current
  policy at resume (they should) and what happens when policy has since narrowed (unspecified).
- **Q2** — Long-running tool calls have three competing shapes: MCP's tasks extension, A2A tasks,
  and our `Suspended` state. Unifying them is the same problem as 001 §2's `InputRequired`
  unification and is not yet done.
- **Q3** — Cross-node workflow checkpoint consistency (014 Q4).
- **Q4** — Whether recordings should be first-class durable objects with their own lifecycle, given
  they are simultaneously a debugging asset and a prompt archive (016 §4).
