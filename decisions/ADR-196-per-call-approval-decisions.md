# ADR-196 — An approval decides exactly the calls it was asked about, names who decided, and resumes against the round it was asked in

- **Status**: **Proposed — design + implementation + proof (2026-09-25); red team round 1 (2026-09-26, issue #111)
  found six defects, all fixed (§7).**
- **Date**: 2026-09-25
- **Origin**: GitHub issues #104 (ADR-182's BUG-1/BUG-2, prerequisite P1b), #107 (ADR-183 §6 residuals), #108
  (ADR-182 R6), and two findings of the ADR-192 round-3 red team (2026-09-25) that live on the same resume paths.
- **Touches**: `include/agentengine/rt/agent_session.hpp` (`ResolveInteraction::call_decisions`/`approver_id`,
  `ApprovalCallDecision`, `SuspendedRoundRecord`, `resolve_interaction`, `resolve_hook_decision`,
  `resolve_codeact_ask`, `resume_tool_table`, `fork_from`, `restore_from_record`, `run_rounds`' suspend site),
  `include/agentengine/core/run_event.hpp` (`ApprovalResolved::approver_id`), `tools/test_driver/test_driver.hpp`
  (`interaction_resolve`'s `call_decisions`/`approver_id`), `tests/core/tools/test_approval_resume.cpp`,
  `tests/testing/test_agentengine_test_driver.cpp` (MIX, PC), `tests/scenarios/live_mixed_round.json`. §7 adds
  `core/tool_call_extraction.hpp` (`make_call_ids_unique`) and `run_event.hpp` (`InteractionRef::approver_id`).
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
  record. *Superseded by §7:* such an interaction is now closed with nothing run
  (`session.resolve_interaction.round_not_recorded`); the "every call in the suspended message" and turn-middleware
  fallbacks are gone. Durable records remain OQ-21 work.
- **Binding is by tool and arguments, not by call id.** Two identical calls in one round, one approved and one denied:
  the denied one is not dispatched and the approved one runs, which is the intended outcome; the decider itself cannot
  tell them apart.
- **Unattended replay re-check uses `text_derived` provenance** for the replayed `execute_code` (the record does not
  keep the original), which is at least as strict as the original.
- `approval_requested`'s `needs_approval` field is now always `true`; kept for compatibility.

## 5. Evidence

`tests/core/tools/test_approval_resume.cpp` (all pass):

| Check | What it proves |
|---|---|
| U1 (+ control) | a tool the turn middleware hid is not dispatched on a hook-decision resume in unattended mode; the same tool, not hidden, is |
| U2 (+ control) | clearing unattended mode while a script waits on `agent.ask` stops the replay; left on, it replays |
| A1 (+ control) | approving a round does not run a call the host's policy denies; allowed by the policy, it runs |
| A2 | a decision for an unasked call, or two for one call, is refused and the interaction stays open; the retry succeeds |
| A3 | a blank or multi-line approver is refused before anything is announced; `approval_resolved` names the approver; an unnamed resolve records an empty approver |
| S1 | after `fork_from()`, resolving a reused interaction id runs the new round, never the discarded round's calls |
| S2 | an `input` interaction from a record is refused; the next interaction id continues past the restored ones |

`tests/testing/test_agentengine_test_driver.cpp` MIX (only the gated call is listed; a decision for an unlisted call is refused)
and PC (two gated calls and a free call; the round is denied except the first call, approved by `alice`: each
`approval_resolved` carries its own decision and the approver, the approved call and the free call run, the denied one
does not). PC is ADR-182's C7: reintroducing BUG-2 fails it. `tests/scenarios/live_mixed_round.json` now expects only
the gated call's pair of events.

**Positive controls** (each fix reverted in `agent_session.hpp`, the named check seen to fail, source restored from a
scratchpad copy): resume filter and middleware fallback removed → U1; replay re-check skipped → U2; resume dispatched
with an always-yes decider and no policy → A1; `fork_from` stops clearing the per-interaction records → S1; restore
counter not continued → S2.

## 6. Red team

Round 1 (2026-09-26): §7.

## 7. Red team round 1 (2026-09-26)

A red team against this ADR as merged in #110 (e46c08f) reproduced six defects with an executed probe (issue #111).
They share one cause: §2 said an interaction "records what it asked", but the record was partial (tool *names* only,
no call identity the engine controlled, no binding to the suspended message) and optional (a missing record fell back
to reading `history_`). The fix reframes the rule rather than patching each path:

**An interaction resolves only against the round this session recorded when it suspended, and the record is complete:
the calls' engine-owned ids, the descriptors the model was offered, and the message the round suspended on. Every
request is validated, for every interaction kind, before anything closes, is announced or runs.**

### 7.1 Findings and fixes

| # | Finding (severity) | Fix |
|---|---|---|
| A1 | MAJOR (I3/I4). `restore_from_record()` onto a live session keeps `history_` and drops the records; a record-less approval asked about "every call in the last message" -- a later round's. Probe: alice approved `gated_tool{"value":1}`; `{"value":666}` ran, audited as hers. §4's "restored approvals cannot resume" was false. | Fail closed. An interaction with no `SuspendedRoundRecord` is closed with nothing run (`session.resolve_interaction.round_not_recorded`), the codeact branch's existing precedent, so it cannot keep `start_run()` refused. Both fallbacks (every call in the message; re-running the turn middleware) are removed. The record also carries `message_index`; a resolve whose history tail is not that message is `stale`. Chosen over clearing `history_` on restore: `AgentSessionRecord` carries no history, so clearing it would silently change what a restored-onto session's *next* run sends, and it would still leave a record-less interaction guessing. No documented host flow resolves a restored approval (checked: every `restore_from_record` test). |
| A2 | MAJOR (I3). Call ids are model output and were not unique: two `gated_tool` calls both `c1` -- a split decision was refused as a duplicate, and approving `c1` ran both. | The engine owns call identity. `make_call_ids_unique()` (`core/tool_call_extraction.hpp`) runs on every model response before it enters `history_`: the first call keeps its id, a repeat becomes `<id>_ae<n>` (smallest unused n; a second empty id becomes `call_ae<n>`). Every downstream consumer -- `approval_requested`, `call_decisions`, hook answers, audit, tool results -- derives from that message, so they agree by construction. Each rename is announced as a `warning` (I4). Renaming, not refusing: some local models repeat ids routinely, and a refusal would fail those runs. |
| A3 | MINOR. Same collision: `c1: gated_tool` + `c1: free_tool`; denying the round denied the free call. | Same fix. |
| A4 | MAJOR. `resume_tool_table` kept only names and dispatched the provider's *current* descriptors: a turn middleware that made `free_tool` `always_require` was ignored on a hook-decision resume (it ran with no human); one that wrapped `gated_tool.invoke` saw the raw invoke run on approval. | `SuspendedRoundRecord::offered_tools` holds the descriptors as the turn middleware left them. Every resume -- hook decision, approval, CodeAct -- decides approval against and dispatches exactly those. The provider is still asked, only to narrow: a recorded tool it no longer offers is dropped (and `schedule_wakeup` only while the grant holds). The hook cascade carries the record forward. |
| A5 | MINOR (I4). The hook-decision branch returned before the `approver_id` check and ignored `call_decisions`; the CodeAct branch recorded no approver. | One validation block before the branch, for every kind: bad `approver_id` refused; `call_decisions` on a non-approval interaction refused (`session.resolve_interaction.call_decisions_not_applicable`); a CodeAct resolve without `answer` and a hook resolve without `hook_dispatch_answers` refused -- all before the orphan close or anything else. `InteractionRef` gains `approver_id` (defaulted, appended last); `input_resolved` carries the resolver for every kind. The test driver adds it to the payload only when set. |
| A6 | MAJOR (predates this ADR). A CodeAct resolve appended the answer and emitted `input_resolved` before the fallible `resume_tool_table`/`on_context()`; after a failure the interaction stayed open with the answer kept, so the retry's answer landed on the *next* question (`[yes, NO]` with one question ever shown). | Validate first, commit second: the tool table is built first; the answer is appended and `input_resolved` emitted only after it succeeded. The approval and hook paths keep ADR-183's order (the decision is announced before `on_context()`, and a failure there fails the run with the interaction closed), since they have no answer to carry into a retry. |

### 7.2 Evidence

`tests/core/tools/test_approval_resume.cpp`, all pass.

| Check | What it proves |
|---|---|
| A7 (+ control) | A record restored onto the live session: approving `:interaction:1` as alice is refused `round_not_recorded`, nothing runs, nothing is attributed to alice, the orphan is closed. Control: on a session not restored, approving round 2 runs exactly `value=666`. |
| A8 | Two `gated_tool` calls both `c1` are named `c1` and `c1_ae1` in `approval_requested`, with one rename warning; approve `c1` + deny `c1_ae1` runs only `value=1`; `approval_resolved` and the history's tool results carry the two distinct ids. |
| A9 | `c1: gated_tool` + `c1: other_tool` (never gated), round denied: `other_tool` runs, `gated_tool` does not. |
| A10 (+ control) | Middleware makes `other_tool` `always_require`; after the hook allows it, the resume asks a human (one `approval_requested`) and does not run it. Control: not tightened, it runs. A middleware-wrapped `gated_tool.invoke` is the one that runs on approval; the raw invoke does not. |
| A11 | Hook-decision resolve: multi-line approver and `call_decisions` each refused, interaction open, nothing ran; the valid resolve's `input_resolved` names `carol`. CodeAct resolve: `call_decisions` and a blank approver refused before anything is announced; the valid answer's `input_resolved` names `bob`. |
| A12 | CodeAct: the first answer fails in `on_context()` (interaction open, no `input_resolved`); the retry answers Q1, Q2 is asked of a human, and the script acts with exactly `[NO, sure]`. |

**Positive controls** (each fix reverted in `agent_session.hpp` / `tool_call_extraction.hpp`, the named checks seen to
fail, sources restored from a scratchpad copy with `cp`): record-less fallback reinstated (a fake record built from
`history_.back()` and the provider's tools) → A7 (2 checks); the rename loop skipped → A8 (4) and A9; resume dispatches
the provider's descriptor instead of the recorded one → A10 (2); approver/`call_decisions` validation limited to
approval interactions → A11 (5); the CodeAct answer appended before the tool table → A12 (2).

### 7.3 Residuals

- **Records are still in memory (OQ-21).** A restored interaction now fails closed instead of guessing; making it
  resumable needs durable records (and durable history), unchanged scope.
- **A streamed response may already have announced the model's own id** in a `model_delta` before the rename; the
  tool events and every decision use the renamed id. A consumer that correlates deltas to tool events by id sees the
  first call only for a repeated id.
- **Approval binding is still by tool and canonical arguments** inside the decider (§4); with ids now unique, two
  *identical* calls with split decisions still run the approved one and settle the denied one, as intended.
- **Hook answers naming a call that is not awaiting dispatch are ignored**, not refused (unchanged; a stray answer
  cannot widen anything, since only pending calls are looked up).
- **The provider is still consulted on resume** (to narrow), so a failing `on_context()` still fails an approval
  resume after ADR-183's announce; the CodeAct path now keeps that retryable.
