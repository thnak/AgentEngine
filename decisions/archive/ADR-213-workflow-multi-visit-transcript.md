# ADR-213: A real multi-visit transcript for WorkflowSupervisor (issue #28 item 6)

Status: **Proposed** (implemented and tested; awaiting project-owner Judge).

## 1. The question

Issue #28 asked for a Magentic-shaped convenience surface. ADR-149 closed items 1-4; ADR-152 (issue #29)
closed item 5, the typed live-event stream. Item 6, a transcript, was only partially closed:
`latest_outputs_of()` returns the most recent message per executor, because `record_partial()` overwrites
an executor's entry on every revisit. A cyclic graph (Magentic: the manager and each participant are
revisited many times) therefore loses its history by the time a run completes.

## 2. Decision

`WorkflowSupervisor::enable_transcript(max_entries = 1024)` records every completed executor visit, in
completion order, into `WorkflowResult::transcript` (a `std::vector<ExecutorOutput>`: executor id, round,
payload). It is hooked at the two places `record_partial()` already runs (the request-port resume fold and
the per-round executor fold), so a visit is recorded exactly when `partial` would be updated.

- **Off by default.** No allocation or copy unless enabled.
- **Bounded (I8).** Past `max_entries` further visits are dropped and `WorkflowResult::transcript_truncated`
  is set; truncation never affects the run itself.
- **In-memory only.** It is not in `RunStateRecord`; a run restored from a checkpoint records only the visits
  made since the restore. The checkpoint format is unchanged, so no migration.
- **Data, not authority (I3).** Like `partial`, it carries model-derived payloads and is never read by the
  supervisor for any decision.
- A fresh `run_workflow()` starts an empty transcript (the run state is reset).

`latest_outputs_of()` is unchanged and still means "latest per executor".

## 3. Evidence

- `tests/workflow/test_workflow_transcript.cpp`: T1 off by default; T2 the six-visit Magentic run in order
  (`mgr, p1, mgr, p2, mgr, done`) while `partial` still holds four entries; T3 the cap and flag; T4 a fresh run
  starts fresh.
- `examples/33_magentic_transcript.cpp`: the demo, combining the ADR-152 live event stream with the transcript.

## 4. Residuals

- Not checkpointed (above); persisting it would grow every checkpoint by the run's full history.
- The cap drops the newest visits, not the oldest. A caller that wants the tail raises the cap.
