# ADR-163: Real, cumulative `Usage` tracking for `WorkflowSupervisor` (closes ADR-162's named gap)

## 1. The question

ADR-162 built `WorkflowChatClient`, a `ChatClient`-shaped adapter over a whole `WorkflowSupervisor`,
but disclosed a real, named blocker in its own §5: `WorkflowSupervisor` had no usage-tracking
mechanism at all, so an `AgentSession` bound to `WorkflowChatClient` as its own `ChatClientId` backend
could never complete a call — `AgentSession::run_model_call()`'s pre-existing `chat()`-absent fallback
fails closed rather than fabricate a token count, and fabricating one was rejected outright across
ADR-162's rounds 6-9 as a genuine I8 violation. Explicit project-owner direction: close this gap for
real, going further than MAF's own `workflow.as_agent()` (which reports no usage at all from a wrapped
workflow) rather than settling for parity.

Can `WorkflowSupervisor` track real, cumulative `Usage` — sourced only from executors that actually
incur a metered cost, never fabricated, never silently dropped across a suspend/resume boundary or a
nested `sub_workflow` — without widening any executor's own authority (I2) or treating model output as
anything but data (I3)?

## 2. Scope

Three additions, all additive, no existing method's behavior changed:

1. `AgentSession::run_usage()` (`rt/agent_session.hpp`) — a new `run_usage_` member, reset alongside
   the pre-existing `run_tokens_consumed_ = 0` at every `start_run()`/`fork_from()` reset site,
   accumulated field-by-field alongside `run_tokens_consumed_ +=` on every real model response.
2. `agent_session_as_executor_body()` (`rt/agent_workflow_executor.hpp`) populates the new
   `ExecutorOutcome::usage` field from `session.run_usage()` after `start_run()` completes — reading
   `run_usage()`, never `driven->usage` (`AgentResponse::usage` only reflects the FINAL round's own
   model call; for a multi-round tool-calling turn it under-counts every earlier round).
3. `WorkflowSupervisor::usage()` (`rt/workflow_supervisor.hpp`) — a new `total_usage_` member,
   `accumulate_usage()` private helper, and the plumbing to actually reach it: `ExecutorOutcome::usage`
   → `ExecuteReply::usage` → folded into `total_usage_` at both places `execute()` already folds a
   reply (the port-prologue loop and the exec_deliveries loop) → `OpenPort::usage` for a nested
   `sub_workflow`'s retroactive contribution once it resolves.

`WorkflowChatClient::chat_stream()` (ADR-162, already built) already read a before/after `usage()`
delta around each `run_workflow()`/`resume_workflow()` dispatch — this ADR is what makes that delta a
real number instead of the honest `nullopt`/zero ADR-162 shipped with.

## 3. Design and verification

Not run through the full ten-round adversarial red-team process ADR-162/ADR-159 used — this addition
is a pure aggregation/plumbing extension with no new authority surface, no new suspend/resume state
machine, and no new thread-boundary crossing (it rides the same call/reply structs `execute()` already
folds every round). Verified instead by the same standard this project's own `CLAUDE.md` sets for a
hot path: a load-bearing invariant needs a test that can fail, and here the tests genuinely did fail
first, catching two real defects before this ADR was written (below).

**Two real bugs found and fixed, both by T10/T11 (`tests/test_rt_workflow_as_chat_client.cpp`)
actually failing at 0/0 instead of the scripted 100/50 tokens, not by code review:**

1. `run_executor_job()`'s `ExecuteReply` construction never threaded `outcome->usage` through at all —
   the field existed on both structs, but nothing copied one to the other. Fixed:
   `reply.usage = outcome->usage;` after construction.
2. `run_sub_workflow_job()`'s COMPLETED branch had the identical gap for `inner->usage()`. Found
   proactively (same pattern as #1, no separate failing test isolates this branch specifically) and
   fixed the same way: `reply.usage = inner->usage();`.

Both are exactly the class of defect this project's own "prove" discipline exists to catch: the design
was correct end-to-end on paper (every struct had the field, every fold site called
`accumulate_usage()`), but two of the several places a `Usage` value has to cross a struct boundary
were silently no-ops. A design review reading the fold sites in isolation would not have caught either
one — only running a scripted, non-zero `Usage` through the whole chain and checking the number that
came out the other end did.

**What "tracked" means, precisely** (the accessor's own comment, `workflow_supervisor.hpp:869-884`,
states this in full): `usage()` sums every `agent`-kind executor's real `AgentSession::run_usage()`
(via `agent_session_as_executor_body()`), plus any nested `sub_workflow`'s own recursively-summed
total, folded in at the moment that nested run resolves. It is LIVE — a caller can read it mid-run, the
same "read directly off the supervisor instance" shape `open_interactions()` already has. It resets
only on a fresh `run_workflow()` call, never on `resume_workflow()`/`continue_workflow()` — matching
`rounds_`'s own "accumulates across a suspend/resume lifecycle" contract, deliberately, since a caller
draining `WorkflowChatClient` across a paused `request_port` needs the WHOLE run's cost at the end, not
just the last call's.

## 4. What this ADR does not claim

- **Not every executor's cost is tracked — only `agent`-kind dispatches and resolved nested
  `sub_workflow`s.** A `function`-kind `ExecutorBody` is arbitrary C++, free to hold and call a real
  `ChatClient` directly without ever reporting through this path. `usage()` answers "how much did every
  TRACKED dispatch cost," not "prove this run made zero untracked model calls." Named, disclosed,
  unfixed — the same shape of residual `ADR-070`'s Delegated Decision Seam already accepts elsewhere:
  the engine cannot audit authority a host-authored body never routed through it in the first place.
  A caller relying on `usage()` for a hard budget ceiling should know it bounds the common, supported
  path (an `agent`-kind node, or `WorkflowChatClient`'s own headline composition), not every
  conceivable graph shape.
- **A nested `sub_workflow` that suspends mid-run contributes ZERO to the outer total until it
  resolves** — its cost is bubbled in retroactively, at the exact port-prologue fold site that already
  processes every other resolved interaction, via `OpenPort::usage`. Between suspension and resolution
  the outer `usage()` is an honest undercount of the true in-flight total, not a wrong number — the
  same "cost isn't known until the thing that incurred it finishes" property `AgentSession::run_usage()`
  itself has within a single multi-round turn.
- **Closes `WorkflowChatClient`'s own headline limitation disclosure, in full, not just for the common
  case.** An earlier draft of this ADR claimed a wrapped graph with ONLY `function`-kind nodes would
  still fail closed against `AgentSession::run_model_call()`'s `chat()`-absent fallback (ADR-162 §3
  finding 3, `rt/agent_session_trust.hpp`'s `drain_streaming_response()`), reasoning that a genuinely
  zero tracked cost would be indistinguishable from "nothing reports usage at all." **That claim was
  untested and wrong** — corrected here after actually composing the scenario (T12b,
  `test_rt_workflow_as_chat_client.cpp`) rather than reasoning about it from the fold sites alone.
  `WorkflowChatClient::chat_stream()` (`workflow_as_chat_client.hpp`) sets `upd.usage = usage_delta`
  UNCONDITIONALLY on every terminal push — never merely "when there's something to report" — so an
  honest zero `Usage{}` still satisfies `std::optional<Usage>::has_value()`. `drain_streaming_response()`'s
  fail-closed check is specifically `!usage.has_value()`, which a real, present, zero-valued `Usage`
  never trips. So an outer `AgentSession` bound to a `WorkflowChatClient` completes `start_run()`
  regardless of the wrapped graph's shape — T12a proves the real-`agent`-kind-node case (non-fabricated,
  non-zero cost reported), T12b proves the all-`function`-kind case (honest zero, still completes).
  ADR-162 §5's original blanket "an `AgentSession` bound to this adapter cannot complete a call at all,
  in any configuration" is fully superseded, not merely narrowed.
- Does not add `Usage::operator+=` to `core/content.hpp` — `accumulate_usage()` is a private,
  field-by-field helper local to `WorkflowSupervisor`, not a new operator on a widely-shared core type.
- Does not touch `ExecuteReply`'s SUSPENDED branch in `run_sub_workflow_job()` — deliberately: a
  suspended nested run's cost-so-far is captured later, retroactively, via `OpenPort::usage` at
  resolution, not estimated or partially reported at suspend time.
- Does not build the recursive walk needed to expose a nested `sub_workflow`'s own real ask through
  `open_interaction_asks()` — unrelated, already-named ADR-162 residual, unchanged by this ADR.

## 5. Falsifiable claims and verdicts

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 1 | `AgentSession::run_usage()` reflects the FULL turn's real usage (every round of a multi-round tool-calling turn), not just the final round's own model call. | CORRECT | `agent_workflow_executor.hpp:121-129`'s own comment traces why `driven->usage` alone would under-count; `run_usage_` accumulates on every real model response, reset only at `start_run()`/`fork_from()` |
| 2 | A real agent-kind node's real usage reaches `WorkflowSupervisor::usage()` exactly, with no double-counting across a suspend/resume cycle. | CORRECT | `test_rt_workflow_as_chat_client.cpp` T10 (100/50 scripted tokens land exactly), T11 (a second call with no further agent-kind dispatch reports an honest zero delta, not the first call's cost repeated) |
| 3 | `usage()` resets on a fresh `run_workflow()` call but persists across `resume_workflow()`/`continue_workflow()`, matching `rounds_`'s own lifecycle contract. | CORRECT | `workflow_supervisor.hpp:951` (reset site, `run_workflow()` only); T11's own resume-carries-cumulative-total shape |
| 4 | A nested `sub_workflow`'s cost is folded into the outer total at resolution, via `OpenPort::usage`, not lost or double-counted. | CORRECT | `workflow_supervisor.hpp:1204-1212` (`OpenPort::usage` comment), the `resolved_port.usage = inner->usage()` site in `resume_workflow()`'s nested-resolution branch |
| 5 | Every pre-existing `WorkflowSupervisor`/`AgentSession`/`WorkflowChatClient`-family test still passes after this addition. | CORRECT | `test_rt_workflow_as_chat_client` (71/71), `test_rt_agent_workflow_executor`, `test_rt_agent_session` — all rebuilt and rerun clean, zero failures; broader regression sweep `ctest -C Debug -R "workflow\|agent_session\|agent_workflow"`: 68/68 passed |
| 6 | The full project builds clean under this codebase's enforced `/WX`. | CORRECT | `cmake --build` (Debug, Visual Studio 18 2026, MSVC), zero errors, zero warnings, across `workflow_supervisor.hpp`, `agent_session.hpp`, `agent_workflow_executor.hpp`, `workflow_as_chat_client.hpp` |
| 7 | `examples/28_workflow_as_chat_client.cpp` reports a real, non-`nullopt` `Usage` (honest zero, since that example's graph has no `agent`-kind node) rather than the `nullopt` ADR-162 shipped with. | CORRECT | Example rerun directly: `OK`, updated checks assert `usage.has_value()==true` with `input_tokens==0`/`output_tokens==0` |
| 8 | An outer, real `rt::AgentSession<WorkflowChatClient>` completes `start_run()` through the `chat()`-absent fallback, for a wrapped graph WITH a real `agent`-kind node (reporting that node's own real cost). | CORRECT | T12a — the outer session's `start_run()` returns a real value, carrying the wrapped workflow's own output, cost 7/3 exactly |
| 9 | The SAME composition ALSO completes for a wrapped graph that is ALL `function`-kind nodes (zero tracked cost) — this was an earlier, untested assumption of this ADR that turned out to be wrong once actually composed. | CORRECT (corrects an earlier draft's own wrong claim) | T12b — the outer session's `start_run()` returns a real value even though `WorkflowSupervisor::usage()` never left zero; `drain_streaming_response()`'s fail-closed check is on `!usage.has_value()`, not on the value being zero, and `WorkflowChatClient` always sets `usage` on its terminal push |

## 6. Files changed

**Edited (all additive, no existing method's behavior changed):**
- `include/agentengine/rt/agent_session.hpp` — `run_usage_` member, `run_usage()` accessor, reset at
  3 sites, accumulated alongside `run_tokens_consumed_ +=`.
- `include/agentengine/rt/agent_workflow_executor.hpp` — `agent_session_as_executor_body()` populates
  `ExecutorOutcome::usage` from `session.run_usage()`.
- `include/agentengine/rt/workflow_supervisor.hpp` — `ExecutorOutcome::usage`, `ExecuteReply::usage`,
  `OpenPort::usage` fields; `total_usage_` member, `accumulate_usage()` private helper, `usage()` public
  accessor; fold-site calls in both places `execute()` already folds a reply; the two bug fixes in
  `run_executor_job()` and `run_sub_workflow_job()`; the `resolved_port.usage = inner->usage()` site in
  `resume_workflow()`.
- `tests/test_rt_workflow_as_chat_client.cpp` — `ScriptedChatClient`/`ScriptedSession` fixtures, T10/T11
  (usage reaches the terminal push exactly; usage is per-call, not cumulative-duplicated across
  suspend/resume); T12 (a real `rt::AgentSession<WorkflowChatClient>` composed end to end, for both a
  real-`agent`-kind-node wrapped graph and an all-`function`-kind one — §4's own correction); T4/T5
  updated to expect a real, honest-zero `Usage` rather than `nullopt`.
- `examples/28_workflow_as_chat_client.cpp` — updated check to match.

## Status

**Proposed — implemented, verified by real test execution (two genuine defects found and fixed by
tests failing, not by review, plus one wrong disclosure claim caught and corrected the same way),
pending project-owner sign-off.** Closes ADR-162 §5's usage-tracking disclosure in full: `WorkflowChatClient`
setting `usage` unconditionally on every terminal push (never leaving it unset) means an outer
`AgentSession` bound to it completes `start_run()` for ANY wrapped graph shape — a real `agent`-kind
node's own honest, non-fabricated cost, or an all-`function`-kind graph's honest zero — because the
`chat()`-absent fallback's fail-closed check is on missing usage, not on zero usage. T12
(`test_rt_workflow_as_chat_client.cpp`) composes both shapes for real rather than leaving either as a
reasoned-but-untested claim.
