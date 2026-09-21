# ADR-178 — `AgentSession::cancel()`: a run the host can stop, cooperatively, and how it ends

- **Status**: **Proposed — implemented and proven; awaiting project-owner judgment.**
- **Date**: 2026-09-21
- **Origin**: ADR-177 §9 disclosed that `effect_context_.cancellation` is never assigned anywhere in the
  session, so the stop-token clause in `should_retry_stream` could not fire. Following that thread showed
  the gap is bigger than one clause: 001 §5 says cancellation is a plain `std::stop_token`, and 020 says a
  host cancels a run by triggering it — but `AgentSession` had no way to be canceled at all. `run_canceled`
  is a real event kind (013) that nothing in the session ever emitted.
- **Reuses**: ADR-159 (`WorkflowSupervisor::cancel()`: one `std::stop_source`, checked cooperatively, the
  token riding to bodies on `EffectContext::cancellation`) and ADR-017 (`stream<T>::cancel()` reaches the
  transport's blocking read). No new mechanism; the session gets the shape the supervisor already has.
- **Touches invariants**: I4 (a canceled run stays attributable), I8 (usage of a call that completed is
  still charged), I3 (a host signal; nothing model-derived can raise it).

## 1. Decision

`AgentSession::cancel()` (callable from any thread, never blocks) and `cancellation_token()`.

| Question | Answer |
|---|---|
| Whose signal? | The host's. It is `std::stop_source::request_stop()`; no model output and no tool result can call it (I3). A tool can call it only if the host handed the tool a reference to the session. |
| Scope | **One source per run.** `start_run()` replaces the source; `cancel()` targets the run executing (or suspended). A cancel with no run in flight is a no-op for the *next* run — a stale cancel can never poison it. A run suspended for approval and later resumed keeps its source, so a cancel raised while it was suspended is seen at the next checkpoint after it resumes (§2). |
| Where is it read? | (1) the top of every round; (2) inside a streaming model call — the drain abandons the stream, which is what reaches the transport (ADR-017); (3) after a model call returns and **before** its tool calls run; (4) by tools, cooperatively, as `EffectContext::cancellation` (already carried into every per-call copy, including parallel batches). |
| How does a run end? | `run_canceled` event, then the run returns `error{failure_class::fatal, "the run was canceled", "run.canceled"}`. `fatal` deliberately: `transient` would invite a caller to retry a run the host just stopped. Not `run_failed`: a canceled run is a state, not a fault (001 §5). |
| What of the canceled round? | Nothing uncommitted is appended: an abandoned stream and a dropped response add nothing to `history_`, exactly like ADR-177's failed attempt. Usage of a call that *completed* is charged (I8) and attributable. The model-call bracket still closes (`model_call_finished`), so a consumer's per-call state is not left open. |
| ADR-177's retry predicate | Its stop clause is now live: a stream that dies while the run is canceled is not retried, and ends `run.canceled`, not `run.chat_failed`. |

## 2. What this deliberately does not do

- **Not preemptive.** A tool that never reads its token runs to its own end; a sandbox execution is not
  interrupted. That is 001 §9 G3's "mid-tool-call / mid-sandbox" cells — this ADR closes the
  round-boundary and mid-stream cells and the API those need, not the whole gate.
- **Gateway sessions** (`ModelCallGateway`) get the round-top and post-response checks, and the drain check
  on the streaming path; a gateway's own retry/fallback loop is not told to stop.
- **Cancel while suspended for approval is UNTESTED.** By reading the code, a resumed run keeps its
  source and reaches the round-top check, but the approved tool batch runs before that check, so an
  approved tool would still run once. Stated from the source, not proven; a test needs an
  approval-gated tool (a second `AgentSession` instantiation) and was left out.
- **No cancel state is persisted** through `fork_from`/snapshot (matches ADR-159).

## 3. Claims and proof (`tests/test_rt_agent_session_cancel.cpp`)

Each claim has a control (the same shape without the cancel) and a planted mutant that must fail it.

| Claim | Proof | Mutant that removes it (all caught) |
|---|---|---|
| K1 a cancel during a tool ends the run at the next round; no second model call; the tool saw the signal | run.canceled, `calls == 1`, tool saw `EffectContext::cancellation` | M1 round-top check removed → K1 fails; M5 token never wired → K1 (6 checks) |
| K2 a silent provider is abandoned on cancel, well inside the silence; bracket closes | 4 s watchdog would otherwise end it; `ms < 2500`; started == finished == 1 | M2 drain check disabled → K2 timing fails |
| K3 a response arriving after the cancel is dropped before its tools run, usage still charged | tool never ran, `run_tokens_consumed` == the response's usage, history unchanged | M3 post-response check removed → K3 (2 checks) |
| K4 the source is per run | cancel after a finished run, then a new run completes with a fresh token | M4 source not replaced → K4 (2) + K6 |
| K5 ADR-177's stop clause is reachable and correct | (a) cancel before the drain; (b) cancel from the event tap DURING the drain, then the stream dies: not retried, `run.canceled`, nothing discarded; control: same failure without cancel IS retried | M7 predicate ignores the token → K5b; M6 `run.chat_failed` instead of canceled → K5b |
| K7 canceled ≠ failed | `run_canceled` once, `run_failed` zero | (in K1/K2/K5b) |

**A finding while proving it:** with the drain reading the token at the top of its loop, a cancel raised
*before* the drain makes ADR-177's stop clause unreachable by any ordinary ordering — only a cancel landing
*mid-drain* reaches it. The first K5 therefore passed while its mutant (M6) survived. K5b lands the cancel
from the run's own thread inside the event tap, the one deterministic way to put it after the drain's last
look at the token; both mutants are then caught. The clause is defence for that window, and is now proven,
not asserted.

## 4. Residuals

1. Not preemptive (§2): tool bodies and sandbox executions must opt in by reading their token.
2. The drain polls (5 ms) — cancellation latency for a stream is bounded by that poll plus the transport's
   own wake, not instantaneous.
3. No conformance run exercises `run_canceled` end-to-end through A2A/AG-UI. Both projections already map
   the event kind; this ADR did not test that mapping.
4. `WorkflowSupervisor::cancel()` (ADR-159) does not yet reach a nested `AgentSession`'s new `cancel()`;
   wiring the supervisor's token into a session it dispatches is separate work.
