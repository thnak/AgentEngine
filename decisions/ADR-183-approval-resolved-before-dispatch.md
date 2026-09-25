# ADR-183 — `approval_resolved` pairs with `approval_requested` and precedes dispatch

- **Status**: **Proposed — design + implementation + proof + red-team pass 1 (2026-09-25; no
  Critical, one Real gap in the test, closed, §8).**
- **Date**: 2026-09-25
- **Origin**: ADR-182 §12 C-1 (P1c). A red-team pass found that, on approve, `resolve_interaction`
  dispatched the tools first and emitted `approval_resolved` afterwards. A headless Claude tester
  independently rediscovered it (ADR-182 §15). Two checked-in scenarios deliberately locked in the old
  order so that this fix would show up as a diff (ADR-182 §16).
- **Amends**: 013 §1 (ordering of `ApprovalRequested`/`ApprovalResolved`).
- **Touches**: `include/agentengine/rt/agent_session.hpp` (`resolve_interaction`, `run_rounds`,
  `resolve_hook_decision`), `include/agentengine/core/tool_call_hook.hpp`
  (`PendingHookDecisionRound`).

## 1. What was wrong

Four emit sites made `approval_resolved` inconsistent across the paths that resolve an approval
interaction:

| Path | Before |
|---|---|
| Plain round, approve | emitted per call **after** `dispatch_tool_calls()` returned, so after every `tool_call_started` and `tool_call_finished` of the round |
| Plain round, deny | before the denial results were folded (no tool runs) |
| Hook-touched round, approve (`finish_hook_processed_round`) | **never emitted** |
| Hook-touched round, deny | emitted for every call in the round, including calls that never got `approval_requested` (after a cascade) |

A consumer that reads "resolved, then executed" off the stream saw the reverse on the most common path
and nothing at all on the hook path. 013 §1 promised an ordered event sequence but never stated this
pair's order, so nothing forbade it.

## 2. A crash found on the way (introduced by ADR-182 P1, fixed here)

ADR-182 P1 made `run_rounds()`'s approval emit loop read `processed[i].request.arguments` for a
hook-touched round. By then, `processed` had already been moved into `pending_hook_decisions_` a few
lines earlier. The read indexed an emptied vector (undefined behaviour). In a debug MSVC build the
process aborts. No test crossed a hook-touched round that suspends for approval, so it shipped in
`624fcc6`. The emit loop now reads the stored round. Proof: O4/O5 below; against the pre-fix engine the
test process dies inside O4.

## 3. Decision

**`approval_resolved` mirrors `approval_requested`.** For an `interaction_reason::approval`
interaction:

1. The engine emits `input_resolved`, then exactly one `approval_resolved{call_id, approved,
   interaction_id}` for each call that got `approval_requested` for that interaction, in the same
   order.
2. That happens before anything acts on the decision: before `on_context()`, before any
   `tool_call_started` of the resumed round, and before denial results are folded.
3. `approved` carries the operator's decision for the interaction (one decision per interaction
   today; per-call decisions are BUG-2, ADR-182 P1b).

It is implemented as one emit site in `resolve_interaction()`, right after `input_resolved`. The four
old sites are gone. Which calls to name:

- **A hook-touched round** carries the exact list forward. `PendingHookDecisionRound` gains
  `approval_requested_call_ids`, filled by the two sites that emit `approval_requested` for such a
  round: the direct suspend in `run_rounds()` (every call in the round) and the cascade in
  `resolve_hook_decision()` (pass-through calls only).
- **A plain round** needs no stored state: its `approval_requested` fired for every call in the
  suspended assistant message, which `resolve_interaction()` already rebuilds from history.

BUG-1 is unchanged on purpose: an ungated call in a suspended round still gets `approval_requested`,
so it also gets `approval_resolved`. P1b changes both together.

### Behaviour changes a consumer can see

- Plain approve: `approval_resolved` moves from after the round's tool events to directly after
  `input_resolved`.
- Hook-touched approve: `approval_resolved` now appears (before: never).
- Hook-touched deny after a cascade: `approval_resolved` now names only the calls that were asked
  (before: every call in the round).
- Approve where `on_context()` then fails: `approval_resolved` is now emitted before `run_failed`
  (the decision was made; the run failed afterwards).

No consumer in the tree depends on the old order. The AG-UI projection maps `approval_resolved` to
nothing (`protocol/agui/projection.hpp`), `cli_chat` prints it as a line, and the A2A projection does
not read it.

## 4. Proof

- **`tests/test_rt_agent_session_approval_resolved_order.cpp`** (new, 58 checks). For each case it
  checks:
  - the resolved ids equal the requested ids, in order;
  - `approved` carries the decision;
  - every `approval_resolved` follows its interaction's `input_resolved` and precedes the first
    `tool_call_started`;
  - no `approval_resolved` names another interaction;
  - which tools actually ran.

  The cases:
  - O1/O2: plain round, one gated call, approve / deny.
  - O3: plain mixed round (gated + ungated), approve: both calls asked and resolved, both before the
    first start.
  - O4/O5: hook-touched round that suspends directly for approval (the hook rewrites c1's arguments,
    denies c2), approve / deny. Also checks that `approval_requested` shows the rewritten arguments.
  - O6/O7: a `hook_decision` resume that cascades into an approval suspend, approve / deny. c1 is
    gated, c2 is answered "allow" by the external dispatch, and c3 is hook-denied. The cascade must
    ask about exactly [c1, c2], the resolution must name exactly those (never c3), and the
    `hook_decision` interaction itself gets no `approval_resolved`.
- **Positive controls.**
  - Against the pre-fix engine headers (stashed), O1 and O3 fail on the ordering check, then the
    process aborts inside O4 (§2).
  - Mutation: with the hook round's recorded list ignored, so every call in the message is named,
    O6 and O7 fail with "resolved [c1,c2,c3] pairs with asked [c1,c2]".
- **ADR-182 C4 restored to its original form.** `tests/test_agentengine_test_driver.cpp` now checks
  that `approval_resolved` comes strictly before `tool_call_started` through the MCP driver.
- **Scenarios.** With the fix, exactly `live_gated_approve` and `live_mixed_round` failed, at exactly
  the moved event ("event 7 differs": expected `tool_call_started`, actual `approval_resolved`). Their
  goldens were updated by moving each `approval_resolved` to directly after its `input_resolved` and
  renumbering `seq`. A check confirmed that every other field of both files is unchanged and that the
  event multiset is identical. All 7 scenarios replay.
- **Existing suites**: `ctest -R "approval|tool_call_hook|suspend|hitl|scripted_chat|agui|test_driver|interaction|hook"`,
  12/12. Full-suite result in §7.

## 5. Finding, not fixed here: a text-derived call skips its tool's own `always_require`

While building O4, a hook that only rewrote a gated call's arguments let the call **run with no
approval at all**: no suspend, no decider consulted. The cause is `tool_call_requires_approval()`
(`core/tool_pipeline.hpp`):

```cpp
return (provenance == call_provenance::text_derived)
           ? !is_auto_declassifiable_text_derived_call(tool)
           : (tool.approval != approval_mode::never_require);
```

For a `text_derived` call the tool's own `approval` mode is never read. A pure, capability-free (or
inert-only) tool is auto-declassified even when it declares `always_require`. So lowering a call's
trust removes an approval the higher-trust form of the same call must pass. ADR-023's declassifier
(a′) was meant to lift the approval that text-derived provenance *itself* imposes on a harmless tool
("otherwise mandatory operator approval, unconditionally", ADR-023 §6 point 4). It says nothing about
overriding a tool's own declaration.

- **Reachable by** (red-team pass 1 traced the `text_derived` assignments):
  - a host tool-call hook that rewrites arguments (host code);
  - middleware that adds calls the backend's own response did not contain (`core/middleware.hpp`);
  - calls parsed from model text by the response-format leak scan (`response_format_leak_scan.hpp`,
    called from `rt/agent_session.hpp` and `protocol/openai/chat_client.hpp`). This is model output,
    so an I3 concern, but only in a session whose host turned the scan on; it is off by default
    (ADR-023 Finding 6).

  Bounded by (a′) itself: only a pure tool whose capabilities are all inert qualifies, so no egress
  or mutation capability is in reach.
- **Minimal reproduction:** a session with `set_suspend_for_approval(true)` and no decider; a pure,
  capability-free `always_require` tool; a hook that sets `rewritten_arguments` for that call. The
  round completes and the tool runs. With a hook that does not rewrite, it suspends.
- **Why not fixed here:** it changes the approval core that ADR-023 and 007 §4 define, which is a
  security-critical design change with its own ADR and red-team. The obvious fix (a `text_derived`
  call needs approval when the tool is not `never_require` *or* is not declassifiable) is stricter
  than today everywhere, so it fails safe, but it also changes `policy_driven` text-derived calls, and
  that needs judging. The O4–O6 fixture uses an `idempotent` gated tool so this ADR's proof does not
  depend on the bug.

## 6. Residuals

- **Restored sessions.** `pending_hook_decisions_` is in-memory only (it was before this ADR;
  `restore_from_record()` does not carry it). A restored session with an open approval interaction
  from a hook-touched round resolves through the plain path, so it names every call in the suspended
  message. That can differ from what was asked after a cascade. The larger, pre-existing consequence
  is that the resume also bypasses the stored hook-processed state. That is OQ-21's to fix, not this
  ADR's.
- **Stale hook state is also *retained* (red-team, older bug).** `restore_from_record()` clears
  neither `pending_hook_decisions_` nor `interaction_counter_`, and `fork_from()` resets the counter
  without clearing `pending_hook_decisions_`. If an interaction id is ever reused (a re-fork into the
  same session id, or a restore over a live session), resolving it would pick up a stale hook round:
  its recorded ids and, more seriously, its stored requests run through `finish_hook_processed_round`.
  This predates ADR-183 and belongs with OQ-21's persistence work. It is named here because §6's
  first bullet described only the *lost*-state half.
- **A hook-denied call is resolved with `approved=true`** (O4's c2) when the operator approves the
  round. That pairing is correct under this ADR's rule (c2 was asked, per BUG-1), but a consumer may
  read it as "approved" for a call that never runs. It is in BUG-1's class and goes with P1b.
- **Approve, then `on_context()` fails.** The stream shows `approval_resolved{approved=true}`, then
  `run_failed`, and no tool runs. That order is intended, since the decision was made. The older
  consequences on that path are not changed here: the interaction is already closed, and a hook
  round's `pending_hook_decisions_` entry is left behind.
- **Other interaction reasons.** An `input` or `auth` interaction supplied through
  `restore_from_record()` falls through `resolve_interaction()` into the approval branch. That
  predates this ADR, and the host supplies the record, not the model. So the rule in §3 reads,
  precisely: every interaction that reaches the approval branch.
- **I4.** `ApprovalResolved` still carries no approver identity (ADR-182 R6).

## 7. Full offline suite

313/314 on the first run. The one failure was `test_agentengine_test_driver`'s "cancel-while-running
is refused" check. That check was racy: under full-suite load the one-turn run could finish before the
cancel arrived, and a cancel on an idle session is, correctly, not marked non-deterministic. The check
now retries in fresh sessions until `session_cancel` reports `was: running`. Final run: **314/314**
(`ctest -j 8`, Windows, Debug, 2026-09-25).

## 8. Red-team pass 1 (2026-09-25, `general-purpose` agent, no prior context)

No Critical findings.

**Verified correct by the red-team:**
- There is one emit site and nothing emits twice. The `codeact_ask` and `hook_decision` branches
  return before it, and no error exit sits between `input_resolved` and it.
- The recorded ids always match what was emitted, and the `operator[]` lookups never create an entry.
- The moved-from read is fixed and index-aligned.
- The change is events only: deciders, dispatch and denial folding are unchanged, so there is no
  I2/I3 impact.
- §5's finding is accurate.

**Real gap, closed:** O6 could not tell "the cascade names only the calls it asked about" from "names
every call", because every call in it was pass-through. O6/O7 now add a hook-denied c3 and a deny
variant, and pin the exact list. The mutation control above shows the check fails when the list is
ignored.

**Minor, addressed:**
- O1/O2 now pin `asked == [c1]`, so an empty-vs-empty comparison cannot pass.
- The test now also checks that each `approval_resolved` follows `input_resolved` and that none names
  another interaction.
- §5's reachability now names the middleware path and the leak scan's opt-in.

**Minor, recorded as residuals in §6:**
- a hook-denied call resolved with `approved=true`;
- the `on_context()`-failure leak;
- other reasons falling into the approval branch;
- stale hook state *retained* across restore and fork.

**Not addressed:** the ordering check compares against `tool_call_started` only. On the deny path no
tool starts, so a deny-path ordering bug against the folded denial result would go unseen. The
single emit site, before any branch, is what rules that out, and a test of it would need a hook into
history folding that the session does not expose.
