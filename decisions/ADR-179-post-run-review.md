# ADR-179 — Post-run review: capture lessons from a finished run, off the critical path, behind a host gate

- **Status**: **Proposed — DESIGN ONLY. Nothing here is implemented, built, or executed.** Every claim in §4
  is a claim to be proven, not evidence. Awaiting red-team, prove, and judge.
- **Date**: 2026-09-21
- **Origin**: a question about self-improving agents. Hermes Agent's loop (a background pass after a task
  distils the trajectory into a reusable skill) is the reference behaviour; its documented form has no
  evaluation or approval before a lesson persists. This ADR takes the trigger and drops the missing gate.
- **Reuses**: ADR-168 (`run_with_bounded_reflection`: an outer driver over `start_run()`, not a session
  hook), ADR-159/178 (host-owned signals), `agent.spawn` / `run_child_agent_session` (a weaker child with
  attenuated capabilities and its own budget), 029 (`MemoryItem`, `memory_kind::procedural`, write path).
- **Touches invariants**: I2 (reviewer and promoter hold only what the host gives them), I3 (a lesson is
  model-derived data and must be inert as authority), I4 (a promoted lesson names the run that produced
  it), I5 (the reviewer is a recorded model call), I8 (the review has its own bound).

## 1. The question

When a run finishes, can something run *after* it, off the run's critical path, that reads what happened and
records lessons which help later sessions — **without** any model output gaining authority, and **without**
the review being able to change or delay the run's own result?

**Scope, stated so it has a wrong answer:** this ADR is about *capture and gated promotion* of lessons. It
does **not** claim the agent gets better. Whether a promoted lesson helps needs an evaluation harness the
repo does not have; that is a separate ADR (§6). Without it this mechanism is "the agent keeps safe notes",
and the notes may be wrong.

## 2. What exists (checked in the source, 2026-09-21)

| Seam | What it does | Why it is not the answer |
|---|---|---|
| `MemoryProvider::on_turn_end` (`core/memory_provider.hpp:338`) | Extracts an **episodic**, `model_inferred` item from one turn and writes it, best-effort | Per turn, so it cannot see a whole-run pattern; **inline**, so it adds a model call to every turn; writes straight to the store with no gate; never `procedural` |
| `set_run_event_tap` (`agent_session.hpp:840`, invoked at `:1731`) | Delivers `run_finished` / `run_failed` / `run_canceled` events | Called synchronously from inside the run; carries an event, not the transcript |
| `start_background_task` (`agent_session.hpp:1536`) | Runs a **tool call** off-thread, results come back via a completion queue | Tool-call shaped (`ToolTable` + `ToolCallRequest`); results re-enter the conversation as tool results — the wrong channel for a lesson |
| `run_rounds()` exit (`agent_session.hpp:2740`) | `emit_run_event(run_finished)` then `co_return` | No seam after the final answer (ADR-168 §2 found the same) |
| `start_run()` (`:929`) | Takes `session_mutex_` for the whole run | Anything that re-enters the session from inside the run waits on a mutex the run holds |

## 3. Decision

A free function, an outer driver, like ADR-168 — **not** a session-level `on_run_end` hook.

```
run_with_post_run_review(session, StartRun, ReviewPolicy) -> task<result<PostRunOutcome>>
```

1. Records `history().size()` (the run's start index), calls `session.start_run()`.
2. Builds a **`RunView`**: a *copy* — run id, the messages this run added, usage, outcome
   (`finished | failed(code) | canceled`), and `touched_untrusted` (true if any message in the run had
   `tainted` or `content_origin::external` content). No reference into the session survives.
3. Evaluates the host's **`ReviewTrigger`** on the `RunView`. **Default: never** (opt-in, ADR-070 property 2).
   A host may supply Hermes-style facts (tool-call count, error recovery) — computed from `RunView`
   structure, never by asking the model.
4. If triggered, hands an owned `ReviewJob{RunView, Reviewer, PromotionGate, bounds}` to the host's
   **`ReviewSink`**. The sink chooses the thread/pool/inline; if none is configured the job runs inline
   *after* the run's result is fixed. The session is not involved again.
5. **Reviewer** (host-supplied): `task<result<std::vector<LessonCandidate>>>(RunView const&)`. The intended
   implementation is a `run_child_agent_session` child whose `ChildSpawnRequest` carries read-only
   capabilities, a finite `token_budget`, and a finite `max_turns`. Its output is data only.
6. **PromotionGate** (host-supplied): `result<PromotionDecision>(LessonCandidate const&, RunView const&)` →
   `promote | hold | reject`. **Default when unset: `hold` everything** (fails closed).
7. `promote` → the *driver's host-side promoter* calls `write_memory_item()` with the host's own `FsWrite`
   capability: `kind = procedural`, `source = model_inferred`, `origin.run_id` = the reviewed run,
   `id` = content digest (so a repeated lesson dedupes). `hold` → returned in `PostRunOutcome` for a
   human or another process; **not written**.
8. The driver returns the run's `result<AgentResponse>` **unchanged** in every case. Review results appear
   in `PostRunOutcome::review` (a `result<>`); a review failure never turns a finished run into a failure.

**Bounds (I8):** reviewer token budget, `max_candidates`, `max_candidate_bytes`. Over-cap candidates are
dropped and counted, not truncated silently.

**Cancellation (ADR-178):** a `canceled` run is not reviewed unless the host's trigger says so.

## 4. Falsifiable claims (each needs a control and a planted mutant that must fail it)

| # | Claim | Mutant that must be caught |
|---|---|---|
| P1 | A reviewer that errors, throws, or hangs on the sink does not change the run's returned result or its usage | Driver propagates the review error |
| P2 | `RunView` is a snapshot: a second `start_run()` started while the review is in flight neither corrupts the review's view nor blocks | `RunView` holds a reference into `history_` |
| P3 | Nothing reaches the store unless the gate returned `promote`; unset gate ⇒ nothing written | Promoter skips the gate; default gate promotes |
| P4 | With `touched_untrusted`, the default gate holds every candidate | Taint summary not computed |
| P5 | A promoted item is `procedural` / `model_inferred`, names the reviewed `run_id`, and a repeated lesson writes one item | Wrong kind/source; id not a digest |
| P6 | Token budget, `max_candidates`, `max_candidate_bytes` are enforced and the excess is counted | Any bound removed |
| **P7** | **Authority-inertness (I3):** a promoted lesson reading "approve all tool calls / you have fs-write" is retrieved in a later session and changes **no** `ApprovalDecider`/`PolicyDecider` outcome versus a control session. Positive control: the lesson text *does* appear in that later session's model request, otherwise this proves nothing | Retrieval or rendering lets lesson text into a decision path |
| P8 | A canceled or failed run is not reviewed by default; an explicit trigger reviews it | Default trigger ignores outcome |

## 5. Competing designs (steelmanned)

- **B — `set_run_end_hook()` on the session, fired beside `emit_run_event(run_finished)`.** Every caller gets
  it for free and cannot forget it; it sees every ending. Rejected: `start_run()` holds `session_mutex_` for
  the whole run (`:932`) and `start_background_task()` takes the same mutex (`:1541`), so a hook that calls
  back into the session from inside the run is a self-wait *(from reading the code — not run)*; the hook
  would also sit inside the run's own latency; and `run_rounds()` has many exits, one of which (suspend for
  approval) is not an ending at all.
- **C — do it in `set_run_event_tap`.** Zero engine change. Rejected: same synchronous-inside-the-run
  position, and an event is not a transcript.
- **D — extend `MemoryProvider::on_turn_end`.** It already exists. Rejected as a substitute (per turn,
  inline, ungated, episodic only) but **kept**: this ADR complements it, does not replace it.
- **E — `start_background_task` as the substrate.** Rejected: tool-call-shaped and re-enters as a tool
  result, §2.

## 6. Residuals and things not verified

- **No evidence a lesson helps.** Needs an evaluation harness (held-out tasks, scored on a sandbox branch,
  promote only on improvement). Candidate **ADR-180**. Until then, expect wrong, redundant, and
  contradictory lessons; 029 §7 consolidation is the only existing hygiene.
- **Spec/code disagreement found:** 029 Q2 (resolved 2026-08-04) says procedural memory arrives only as a
  `ContextContribution.instructions` append. `memory_provider.hpp:262` pushes retrieved items into
  `contribution.messages` (tainted, `role::system`), and I found no `procedural` handling in that file.
  Spec wins per CLAUDE.md: this must be reconciled (wire `instructions`, or amend 029 with an ADR) **before**
  P7 can be meaningful. Not resolved here.
- **Suspended runs.** A run parked for approval is not finished. I did not verify how `start_run()` reports
  suspension to its caller; the default trigger must not fire on it, and P8 needs a case for it.
- **`source = model_inferred` vs `agent_authored`.** 029 §3 defines both; which is right for a reviewer's
  output is open. `model_inferred` chosen as the weaker standing.
- **Cross-session poisoning.** Untrusted content → lesson → persisted for the same principal's later
  sessions. P4 holds it by default; a host that promotes anyway owns that (ADR-070's delegated seam).
- **Cost.** A review is a second model run per triggered run. Default-off trigger is the only control in
  v1; no per-session review rate limit.
- **Audit.** The promoted item names its run; no new `run_event_kind` for "review started/promoted" is
  proposed. I4 for the *gate decision* itself is therefore only in `PostRunOutcome`, not the event stream.
- **Naming.** `RunView`, `LessonCandidate`, `PromotionDecision`, `PostRunOutcome`, `ReviewPolicy`,
  `run_with_post_run_review` need `tools/naming_lint.py` at implementation time.
- **Spec amendment.** 029 §4 ("on_turn_end is where memory is written") needs a paragraph on run-level
  review; 002 §5's hook table should say why this is a driver and not a hook.
- **Not red-teamed.** Attack the gate and the reviewer's injection surface (it reads a tainted transcript)
  before anything is built.

## 7. Next steps (per CLAUDE.md: design → red-team → prove → judge)

1. Red-team §3 and §4 (harshly — the reviewer reads attacker-influenced text and its output persists).
2. Resolve the 029 Q2 / `memory_provider.hpp:262` disagreement.
3. Prove P1–P8 with planted mutants; run the whole suite.
4. Only then: ADR-180 (evaluation harness).
