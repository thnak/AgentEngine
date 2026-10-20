# ADR-198 — One failure, one code: `run_failed` carries the run's own error code, and the stage beside it

- **Status**: **Proposed — design + implementation + proof (2026-09-25); red team round 1 (2026-09-26, issue #112
  B3) found a second emit site, fixed (§5).**
- **Date**: 2026-09-25
- **Origin**: GitHub issue #106, found by the agent test driver scenario `tests/scenarios/scripted_model_failure.json`
  (ADR-182 §16, "Findings from this round").
- **Touches**: `include/agentengine/core/run_event.hpp` (`RunFailed::stage`), `include/agentengine/rt/agent_session.hpp`
  (the `run.chat_failed`, `run.context_unavailable` and `run.turn_denied` emit sites),
  `tools/test_driver/test_driver.hpp` (payload), `tests/scenarios/scripted_model_failure.json`,
  `tests/core/tools/test_approval_resume.cpp` F1.

## 1. The defect

When the model call failed, the event stream said `run_failed{error_code: "run.chat_failed"}` while `start_run()`'s
result carried the real cause (e.g. `provider.overloaded`). Every event consumer — AG-UI `RUN_ERROR`, A2A, the test
driver — saw only the category, never the cause; every result consumer saw only the cause. The same split existed at
two more sites: a context provider failure (`run.context_unavailable` on the event, the provider's code on the result)
and a turn-middleware denial (`run.turn_denied` vs. the middleware's code).

## 2. Decision

`RunFailed.error_code` is always the run's own error code — the one its result carries. A new `RunFailed.stage` names
where the run failed when the code alone does not: `run.chat_failed`, `run.context_unavailable`, `run.turn_denied`.
Emit sites whose code already is the run's code (`run.max_turns_exceeded`, `run.token_budget_exceeded`, …) leave
`stage` empty. The AG-UI projection keeps mapping `error_code` to `RUN_ERROR`'s code, so it now carries the cause.
The test driver's payload adds `stage` only when it is set, so every other scenario keeps its shape.

**Why the cause in `error_code`, not the category.** The issue's question was which one the projections expose. A
consumer matching on `run.chat_failed` learns only "the model call failed"; `provider.overloaded` tells it whether to
retry. The category is not lost — it moved to `stage`.

## 3. Residuals

- A consumer that matched on `error_code == "run.chat_failed"` must now read `stage`. In-tree that was
  `test_rt_agent_session_streaming_and_events` A4a and the `scripted_model_failure` scenario golden; both are updated
  to the new pair (A4a now asserts the event's code equals the result's).
- The issue's side note — a `transient` model failure is not retried within the session — is ADR-177's configuration
  (retries are opt-in); not changed here.

## 4. Evidence

`tests/core/tools/test_approval_resume.cpp` F1: a scripted `provider.overloaded` failure yields exactly one `run_failed` whose
`error_code` equals the result's code, with `stage == "run.chat_failed"`. `scenario_scripted_model_failure` expects
`{"error_code":"provider.overloaded","message":"overloaded","stage":"run.chat_failed"}`; with the old emit it fails.

## 5. Red team round 1 (2026-09-26, issue #112 B3)

**Finding (MAJOR, §2's claim false): one failure, two `run_failed`.** `run_model_call()` emitted its own `run_failed`
at three sites -- the outbound media-capability gate (gap-audit finding 19) and both undeclared-tool-call leak
refusals (OQ-23, buffered and streamed paths) -- and then returned the error to `run_rounds()`, whose `run.chat_failed`
site emitted a second one. AG-UI projects each as a `RUN_ERROR`. Probe: an image input to a client that declares no
multimodal support produced two events for one failure.

**Fix: one failure, one emit site.** `run_model_call()` never emits `run_failed`; it only returns the error. The caller
emits exactly one, with `error_code` = the result's code and `stage = run.chat_failed`. One stage is kept for all
three causes, deliberately: the code already names the cause (`chat_client.multimodal_capability_missing` vs.
`chat_client.undeclared_tool_call_leak`), and `stage` answers only "where in the run" -- the model-call step. The only other stage-bearing
emit sites (`run.context_unavailable`, `run.turn_denied`) were already single.

**Evidence.** `tests/core/tools/test_approval_resume.cpp` F2: a media-gate refusal yields exactly one `run_failed`, whose
`error_code` equals the result's code, with `stage == "run.chat_failed"`. Positive control: re-adding the gate's own
emit fails F2 (two events), source restored from a scratchpad copy.

**Residual.** The leak-refusal sites are covered by the same single emit path but not by their own check (F2 drives
the media gate, the one reachable with a plain scripted client); they share the code path verbatim.
