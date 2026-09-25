# ADR-196 — An approval decides exactly the calls it was asked about, names who decided, and resumes against the round it was asked in

- **Status**: **Proposed — design + implementation + proof (2026-09-25; not yet red-teamed).**
- **Date**: 2026-09-25
- **Origin**: GitHub issues #104 (ADR-182's BUG-1/BUG-2, prerequisite P1b), #107 (ADR-183 §6 residuals), #108
  (ADR-182 R6), and two findings of the ADR-192 round-3 red team (2026-09-25) that live on the same resume paths.
- **Touches**: `include/agentengine/rt/agent_session.hpp` (`ResolveInteraction::call_decisions`/`approver_id`,
  `ApprovalCallDecision`, `SuspendedRoundRecord`, `resolve_interaction`, `resolve_hook_decision`,
  `resolve_codeact_ask`, `resume_tool_table`, `fork_from`, `restore_from_record`, `run_rounds`' suspend site),
  `include/agentengine/core/run_event.hpp` (`ApprovalResolved::approver_id`), `tools/test_driver/test_driver.hpp`
  (`interaction_resolve`'s `call_decisions`/`approver_id`), `tests/test_approval_resume.cpp`,
  `tests/test_agentengine_test_driver.cpp` (MIX, PC), `tests/scenarios/live_mixed_round.json`.
- **Invariants**: I3 (a decision is host input, never model output), I4 (every decision names who made it).

## 1. The defects

When a round suspended for approval, the session treated the whole round as one question:

1. **BUG-1 (#104).** `approval_requested` fired for every call in the round, including calls that needed no
   decision, so a UI could not tell which calls were waiting on a human. Each got a paired `approval_resolved`.
2. **BUG-2 (#104).** A deny folded `tool.approval_denied` into every call in the round, including calls that never
   needed approval. There was no way to approve one call and deny another.
3. **Approve ran everything on one yes.** An approve dispatched the whole round under an always-yes decider and **no
   policy**. So a `policy_driven` call the host's `PolicyDecider` denies, sitting in the same round as a gated call,
   ran on the strength of a human's approval of a different call. (Found while fixing BUG-2; not in any issue.)
4. **No approver (#108).** `approval_resolved` said whether a call was approved but not who approved it (I4).
5. **Stale per-interaction state (#107).** `fork_from()` and `restore_from_record()` kept `pending_hook_decisions_`
   (and `pending_codeact_asks_`). Interaction ids come from a counter that both reset, so a later interaction could
   reuse an id and `resolve_interaction` then ran the discarded round's stored requests. An `on_context()` failure
   after an approve left the hook round's entry behind. An `input`/`auth` interaction supplied through a record fell
   into the approval branch.
6. **Stale tool list on resume (ADR-192 round 3, MAJOR).** Every resume path — hook decision, approval, CodeAct
   `agent.ask` — rebuilt its tool table from the raw provider (`history_provider_.on_context`). The main loop builds
   its table *after* the host's turn middleware, so a tool the middleware had removed from the round came back on
   resume. In default mode a human still stood in front of it; with ADR-192's unattended approvals, nothing did.
   Probe: `hidden_tool ran 1`, audited "approved with no human".
7. **CodeAct replay after unattended mode changed (ADR-192 round 3, MAJOR).** `resolve_codeact_ask` re-ran the whole
   script with an always-yes approver, justified as "already resolved by a real human". When unattended mode had
   approved the original `execute_code`, that was false: clearing unattended mode (or adding a veto or a deny policy)
   while the script waited on its question changed nothing, and the script — side effects before the ask included —
   ran again.

## 2. Decision

**An approval interaction records what it asked, and a resolve decides exactly that.**

- `run_rounds` names in `approval_requested` only the calls that need a decision, and records, per interaction, the
  gated call ids and the tool names the model was offered (`SuspendedRoundRecord`, dropped with the interaction).
  `resolve_hook_decision`'s cascade does the same.
- `ResolveInteraction::call_decisions` (optional) carries one decision per call. A call it does not name takes
  `approved`. Naming a call the interaction did not ask about, or naming one twice, is refused
  (`session.resolve_interaction.call_not_pending` / `.duplicate_call_decision`) **before** the interaction closes, so
  the resolve can be retried.
- `ResolveInteraction::approver_id` (optional, host-supplied) is carried on every `approval_resolved`. Unset is an
  anonymous decision, recorded as such (an empty `approver_id`). A blank or control-character id is refused
  (`session.resolve_interaction.bad_approver`), again before anything closes or is announced.
- A denied call is settled with `tool.approval_denied` and never dispatched. An approved call is approved for **exactly
  its tool and canonical arguments**. Every other call — one nobody was asked about — is dispatched under the
  session's ordinary decider (`effective_approval_decider`) **and policy**, exactly as `run_rounds` would have. A plain
  round and a hook-touched round now share one tail (`finish_hook_processed_round`); a plain round is converted to a
  hook-processed one whose calls all pass through.
- **Resume uses the round's own tool list** (`resume_tool_table`): the provider's current tools, restricted to the ones
  the round offered (a tool removed since does not come back; `schedule_wakeup` is re-offered only while the grant
  holds). Without a record — a restored session, OQ-21 — the host's turn middleware decides the list again, as for a
  fresh turn, and a middleware denial fails the resume (`run.turn_denied`).
- **CodeAct replay.** `PendingCodeActAsk::approved_unattended_by` records when unattended mode approved the original
  call. Such a replay is re-checked against the session's *current* policy and effective decider before it runs; a
  refusal folds `tool.approval_denied`. One approved by a human or the host's own decider replays as before.
- **Stale state.** `fork_from()` and `restore_from_record()` clear `pending_hook_decisions_`, `pending_codeact_asks_`
  and the round records. `restore_from_record()` continues the interaction counter past the highest restored id.
  The approval path erases the stored round before any step that can fail. `resolve_interaction` refuses an `input`
  or `auth` interaction (`session.resolve_interaction.unsupported_reason`).
- **Driver.** `interaction_resolve` takes `call_decisions` (`[{call_id, decision}]`, each a call `interaction_list`
  shows) and `approver_id`, records both in the scenario step, and the runner replays them. `interaction_list` now
  lists only the calls that wait on the decision.

## 3. Why this shape

- **Bind the yes to what was shown (I3).** The human saw a tool name and arguments. Binding the approval to those
  bytes — not to the round, not to a call id the decider cannot see — means nothing that was not shown can ride on it.
  Everything else keeps the session's own rules, so an approval never *widens* what the session would allow.
- **Validate first, close second.** Every refusal leaves the interaction open, the same rule ADR-179 stage 0 set for
  hook answers: a malformed resolve must be retryable, not a dead session.
- **Record the round, don't rebuild it.** The alternative, re-running the turn middleware on every resume, would run a
  host hook a second time for one turn. The record is exact and cheap; the middleware is only the fallback for a
  session that has no record.
- Rejected: **refusing an anonymous resolve.** Every existing host resolves without naming anyone; #108 allows
  "recorded explicitly as anonymous", and an empty field says so without breaking them.

## 4. What this does NOT claim (residuals)

- **The records are in memory (OQ-21).** A restored session has no `SuspendedRoundRecord`, no hook round and no CodeAct
  record: an approval asks about every call in the suspended message (as before), and the tool list comes from the
  turn middleware. A restored session also has no history (`AgentSessionRecord` does not carry it), so its approvals
  cannot actually resume today.
- **Binding is by tool and arguments, not by call id.** Two identical calls in one round, one approved and one denied:
  the denied one is not dispatched and the approved one runs, which is the intended outcome; the decider itself cannot
  tell them apart.
- **Unattended replay re-check uses `text_derived` provenance** for the replayed `execute_code` (the record does not
  keep the original), which is at least as strict as the original.
- `approval_requested`'s `needs_approval` field is now always `true`; kept for compatibility.

## 5. Evidence

`tests/test_approval_resume.cpp` (all pass):

| Check | What it proves |
|---|---|
| U1 (+ control) | a tool the turn middleware hid is not dispatched on a hook-decision resume in unattended mode; the same tool, not hidden, is |
| U2 (+ control) | clearing unattended mode while a script waits on `agent.ask` stops the replay; left on, it replays |
| A1 (+ control) | approving a round does not run a call the host's policy denies; allowed by the policy, it runs |
| A2 | a decision for an unasked call, or two for one call, is refused and the interaction stays open; the retry succeeds |
| A3 | a blank or multi-line approver is refused before anything is announced; `approval_resolved` names the approver; an unnamed resolve records an empty approver |
| S1 | after `fork_from()`, resolving a reused interaction id runs the new round, never the discarded round's calls |
| S2 | an `input` interaction from a record is refused; the next interaction id continues past the restored ones |

`tests/test_agentengine_test_driver.cpp` MIX (only the gated call is listed; a decision for an unlisted call is refused)
and PC (two gated calls and a free call; the round is denied except the first call, approved by `alice`: each
`approval_resolved` carries its own decision and the approver, the approved call and the free call run, the denied one
does not). PC is ADR-182's C7: reintroducing BUG-2 fails it. `tests/scenarios/live_mixed_round.json` now expects only
the gated call's pair of events.

**Positive controls** (each fix reverted in `agent_session.hpp`, the named check seen to fail, source restored from a
scratchpad copy): resume filter and middleware fallback removed → U1; replay re-check skipped → U2; resume dispatched
with an always-yes decider and no policy → A1; `fork_from` stops clearing the per-interaction records → S1; restore
counter not continued → S2.

## 6. Red team

Not yet run.
