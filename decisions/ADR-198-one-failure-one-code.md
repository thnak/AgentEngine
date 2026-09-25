# ADR-198 — One failure, one code: `run_failed` carries the run's own error code, and the stage beside it

- **Status**: **Proposed — design + implementation + proof (2026-09-25; not yet red-teamed).**
- **Date**: 2026-09-25
- **Origin**: GitHub issue #106, found by the agent test driver scenario `tests/scenarios/scripted_model_failure.json`
  (ADR-182 §16, "Findings from this round").
- **Touches**: `include/agentengine/core/run_event.hpp` (`RunFailed::stage`), `include/agentengine/rt/agent_session.hpp`
  (the `run.chat_failed`, `run.context_unavailable` and `run.turn_denied` emit sites),
  `tools/test_driver/test_driver.hpp` (payload), `tests/scenarios/scripted_model_failure.json`,
  `tests/test_approval_resume.cpp` F1.

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

`tests/test_approval_resume.cpp` F1: a scripted `provider.overloaded` failure yields exactly one `run_failed` whose
`error_code` equals the result's code, with `stage == "run.chat_failed"`. `scenario_scripted_model_failure` expects
`{"error_code":"provider.overloaded","message":"overloaded","stage":"run.chat_failed"}`; with the old emit it fails.
