# ADR-168 — Single-agent bounded reflection / self-loop

**Status:** Proposed — implemented, built and tested against a real compiler in this session's own
tree, self-red-teamed (disclosed as a residual, not laundered as independent), pending project-owner
sign-off and an independent review pass. Closes GitHub issue #55.

## 1. The question

MAF's Harness offers "Looping" directly on a single agent — "optional bounded re-invocation driven by
evaluators or predicates," `LoopEvaluators` (learn.microsoft.com/en-us/agent-framework/concepts/harness).
AgentEngine's only bounded-reflection concept is 014 §3's Reflection/critic pattern: "a cycle with an
explicit iteration bound" — a full workflow-graph construct (executors, edges, `WorkflowSupervisor`).

Stated so it has a wrong answer: **does single-agent bounded reflection reuse an existing seam — the
`Middleware` TURN interception point (002 §5), or the same `ContextProvider`+`PolicyDecider`
composition #54's `PlanExecuteMode` (ADR-167) just used for a mechanically-adjacent gap — or does it
need something neither seam provides?** Issue #55's own suggested scope explicitly raised sharing one
authoring mechanism with #54 rather than inventing two unrelated policies; this ADR checks that
against the real turn-loop code rather than assuming it transfers.

## 2. Design

**Ground truth checked before designing, not assumed** (in this order, because each finding ruled out
the previous option):

1. **Is the TURN middleware point actually still unwired, as 002 §5's text (as of #54's own pass)
   claimed?** No — checked directly against `include/agentengine/rt/agent_session.hpp`. It IS real
   and wired: `set_turn_middleware_hook()` runs a `TurnMiddlewareHook`
   (`decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md`,
   `core/turn_middleware.hpp`) once per round, `agent_session.hpp:2406-2417`, after the round's tool
   surface/instructions are assembled and *before* the model is called. 002 §5 amended (this session)
   to stop claiming TURN is unwired — a real spec/code disagreement this pass found and fixed at the
   spec, per CLAUDE.md's "spec wins; if the spec is wrong, fix the spec first" rule.
2. **Does that real TURN point reach what bounded reflection needs?** No. Bounded reflection needs to
   act on a COMPLETED response — "the model said it's done; an evaluator disagrees; make it try
   again" — but the TURN hook fires before the model is called, not after. Checked the exact
   termination path in `run_rounds()`: when a round's response carries zero tool calls
   (`agent_session.hpp:2510-2511`), the function `co_return`s an `AgentResponse` immediately
   (`:2544-2545`) with no hook, no seam, nothing between "model produced a final answer" and "the
   caller gets it back." There is structurally nowhere for an evaluator to intervene from inside
   `run_rounds()` at all — TURN, MODEL-CALL, and the tool-call hook (OQ-21) all fire at points
   strictly *before* that return.
3. **Does #54's `ContextProvider`+`PolicyDecider` composition transfer?** No, for a reason specific to
   what each seam can see: `ContextProvider::on_context()` runs before the model call and can only
   shape the NEXT request; `PolicyDecider` fires only for `policy_driven` *tool calls*
   (`resolve_approval_outcome()`), and a round with zero tool calls — the exact case bounded
   reflection cares about — never reaches it either. Both of #54's seams are, like TURN, upstream of
   the point this gap actually lives at. #54 gates *which tools are callable mid-run*; #55 needs to
   gate *whether the run is allowed to consider itself finished*. Different mechanical questions,
   confirming the two issues do NOT share an implementation, only living as siblings under the future
   Harness umbrella (#56).
4. **What DOES already do the thing bounded reflection needs — call the model again with new input,
   after a completed exchange?** `AgentSession::start_run()` itself, called again on the same session.
   Verified as an already-real, already-public, already-demonstrated pattern:
   `examples/03_multi_turn.cpp` calls `start_run()` twice on one session and shows the second call
   sees the first turn's history. And critically for the BOUND question: `start_run()` resets
   `run_tokens_consumed_ = 0` and `effect_context_.turn_index = 0` on every call
   (`agent_session.hpp:953-960`) — `MaxTurns`/`TokenBudget` are genuinely **per-run**, not
   per-session, so a reflection loop built from repeated `start_run()` calls needs, and gets, its own
   independent bound (§4 C-checked below), not a reuse of either existing policy.

**The mechanism (`include/agentengine/rt/bounded_reflection.hpp`):** `run_with_bounded_reflection
(session, initial_input, max_iterations, evaluator)` — a free function, an outer driver, NOT a
`ContextProvider`/`Middleware`/`PolicyDecider`/new `Agent<>` policy tag:

1. Calls `session.start_run(StartRun{initial_input})`.
2. Calls the host-supplied `Evaluator` — `task<result<EvaluationVerdict>>(AgentResponse const&)` —
   exactly once per iteration.
3. `verdict->satisfied == true` → returns immediately (`ReflectionOutcome{response, true, N}`).
4. `verdict->satisfied == false` and iterations remain → builds a feedback `Message` from
   `verdict->feedback` (`role::system`, `content_origin::external`, `tainted = true` — the same
   provenance shape `memory_provider.hpp`/`todo_provider.hpp` already use for informational,
   non-user-authored content re-presented to the model) and calls `start_run()` again.
5. Bound reached without satisfaction → returns `ReflectionOutcome{response, false, max_iterations}`,
   an OK result, not an error — a caller-visible, typed "best attempt within budget," matching this
   codebase's existing "put the state in the type" idiom (`AgentResponse::structured_output_json`,
   ADR-058).
6. Evaluator failure (a real `result` error, not "not satisfied") aborts the whole loop immediately
   via propagation — never retried, never treated as satisfied.

## 3. Competing designs (steelmanned)

**Design A (adopted): a thin outer driver over repeated `start_run()`.** No engine change, reuses an
already-real, already-demonstrated pattern (multi-turn conversation), the loop's own bound is
naturally independent of `MaxTurns`/`TokenBudget` because each `start_run()` already resets those.

**Design B: wire a NEW, real "post-response" interception point into `run_rounds()` itself** (a
genuine engine change, the only way to make this reachable as a hook the way TURN/MODEL-CALL are).
Steelmanned: this would be the "correct," fully-integrated shape — a real `Middleware` hook alongside
the other four, giving reflection the same first-class status TURN now has, and would let a single
`start_run()` call transparently loop internally rather than requiring the caller to use a different
entry point. Rejected for this pass, for the identical reason 002 §5 already gives for RUN/TOOL-CALL:
`AgentSession`'s turn loop is large, mature, and heavily tested; adding a new interception point inside
it is separately-scoped, larger engine work this issue does not need in order to close, matching this
project's own "prove the mechanism against one real consumer, name the rest" precedent
(`decisions/ADR-028-session-scoped-stateful-tools.md`) and #54's own identical reasoning for declining
to wire TOOL-CALL. If this becomes a recurring need (multiple future features all wanting a real
post-response hook), Design B is the honest long-term answer — not built here, named as future work
(§7).

**Design C: reuse #54's `ContextProvider`+`PolicyDecider` composition, e.g. a provider that injects
"you must call a `reflect_done` tool to finish" and a decider that denies finishing.** Steelmanned:
would keep #54 and #55 sharing one mechanism, exactly what the issue's suggested scope raised.
Rejected on inspection (§2 step 3): this would require the model to volunteer a structured "I'm done"
call before it's allowed to stop, changing the actual termination CONTRACT of every round (a
zero-tool-call response would need to become an error/denial until `reflect_done` fires) — a much more
invasive change to `run_rounds()`'s calling convention than Design A, for saving nothing over calling
`start_run()` again with feedback. Also reintroduces an I3 question Design A avoids entirely: trusting
the model's own choice to call `reflect_done` as evidence it should be evaluated, versus Design A where
the evaluator runs unconditionally on every completed response regardless of what the model does or
says.

## 4. Falsifiable claims

- **C1** — Satisfied on the first attempt costs exactly one `start_run()` call. *Disproof:* more than
  one real turn recorded in session history when the evaluator is satisfied immediately.
- **C2** — Unsatisfied-then-satisfied costs exactly as many `start_run()` calls as it took to satisfy,
  never more, never fewer. *Disproof:* iteration count/history size doesn't match the evaluator's own
  satisfaction point.
- **C3** — Reaching `max_iterations` without satisfaction is a successful `result`, not an error, with
  `satisfied == false`. *Disproof:* the call errors, or silently reports `satisfied == true`.
- **C4** — The loop performs at most `max_iterations` real `start_run()` calls, never more, regardless
  of what the evaluator returns. *Disproof:* a persistently-unsatisfied evaluator causes more than
  `max_iterations` turns.
- **C5** — An evaluator failure aborts the loop immediately, propagated verbatim, with no further
  `start_run()` calls after it. *Disproof:* the loop continues past a failing evaluator, or the error
  is replaced/discarded.
- **C6** — `max_iterations == 0` is rejected before any `start_run()` call. *Disproof:* a `start_run()`
  call happens anyway, or the zero case is silently treated as 1.
- **C7 (I3 provenance)** — Feedback re-injected as the next turn is `role::system`,
  `content_origin::external`, `tainted = true` — never presented as if the live user said it.
  *Disproof:* the feedback message has any other role/origin/taint combination.
- **C8 (independence from `MaxTurns`)** — A session capped at `MaxTurns<1>` still completes a
  multi-iteration reflection loop, because each `start_run()` gets its own fresh per-run turn budget.
  *Disproof:* the loop fails with `run.max_turns_exceeded` despite each individual turn needing only
  one model call.

## 5. Red-team round (self-conducted, disclosed — not independently fresh)

1. **Assumed #54's mechanism would transfer, then found it structurally can't.** The first design
   sketch (before reading `run_rounds()`'s actual termination path) assumed a `PolicyDecider`-shaped
   answer, by analogy to #54. Reading the real code (§2 steps 2-3) showed this is wrong: a
   zero-tool-call round never reaches `PolicyDecider` at all, and `ContextProvider::on_context()` only
   ever shapes the NEXT request, never inspects the response that already returned. This is the
   central finding of this pass — not a rubber stamp of the issue's own suggested framing.
2. **Bound conflation risk.** If this loop had reused `MaxTurns`/`TokenBudget` as its own bound (the
   naive first instinct — "there's already a turn limit, why add another"), it would have been silently
   wrong: those reset to zero on every `start_run()` call (§2 step 4), so an evaluator that's never
   satisfied would loop *forever* under a `MaxTurns`-based check, each individual call legitimately
   staying under its own fresh limit. `max_iterations` had to be a genuinely separate counter, checked
   by the driver itself, not derived from anything `AgentSession` already tracks. C8 is the positive
   control proving this distinction is real, not asserted.
3. **Feedback provenance.** An early draft used `role::user` for the re-injected feedback (mimicking
   "the user said try again"). Wrong per I3/the precedent `context_provider.hpp`'s own comment on
   `ContextContribution.instructions` already warns about for a different channel: this is
   evaluator-produced content, not a genuine user statement, and a judge-model-backed evaluator's
   output is exactly the kind of content this codebase already treats as external/tainted elsewhere
   (`memory_provider.hpp`, `todo_provider.hpp`). Fixed to `role::system` + `content_origin::external` +
   `tainted = true` (C7).
4. **"Not satisfied at the bound" as an error vs. an OK result.** Considered making a bound-without-
   satisfaction case a hard error (simpler for a careless caller to notice). Rejected: MAF's own
   `LoopEvaluators` framing is "bounded re-invocation," not "must eventually succeed or throw," and
   forcing an error would discard the caller's ability to use "best attempt within budget" as a
   legitimate outcome (e.g. returning a lower-confidence answer rather than failing the whole run).
   Made explicit and typed instead (`ReflectionOutcome::satisfied`), never silently swallowed either
   way (C3).
5. **Evaluator-failure vs. unsatisfied-forever, told apart.** Checked that a persistently-failing
   evaluator (a broken judge-model call, say) cannot be mistaken for a persistently-unsatisfied one:
   the first aborts immediately via `result` propagation (C5); the second legitimately keeps going to
   the bound (C4). Conflating them either way (aborting on "not satisfied," or retrying past a real
   error) was the one thing the directive for this work called out explicitly to get right.

## 6. Executed evidence

```
$ cd build && cmake .. -G Ninja     # picks up the new test target
[... configure clean ...]
$ cmake --build . --target test_bounded_reflection
[1/4] Scanning D:/GitSrc2/AgentEngine/tests/test_bounded_reflection.cpp for CXX dependencies
[2/4] Generating CXX dyndep file tests/CMakeFiles/test_bounded_reflection.dir/CXX.dd
[3/4] Building CXX object tests/CMakeFiles/test_bounded_reflection.dir/test_bounded_reflection.cpp.obj
[4/4] Linking CXX executable tests\test_bounded_reflection.exe
$ ./tests/test_bounded_reflection.exe
  ok: R1: loop succeeds
  ... (25/25 checks)
test_bounded_reflection: all checks passed

$ cmake --build . --target test_memory_provider   # regression: no shared file touched, sanity check only
$ ./tests/test_memory_provider.exe
test_memory_provider: OK
```

Toolchain: Ninja + `clang++` (this session's configured `build/` tree, Windows). Clean build on the
first attempt — no fix-iterate cycle was needed (worth stating plainly rather than implying a struggle
that didn't happen). **Not run:** the full test suite (matching ADR-166 §6/ADR-167 §6's identical
disclosure); `test_todo_provider`/`test_plan_execute_mode` (this branch is based on `main`, not
`feature/plan-execute-mode` — see §7's branch-base note, those targets don't exist on this branch's
`tests/CMakeLists.txt`); any Linux/GCC/MSVC build.

## Per-claim verdicts

| # | Claim | Verdict |
|---|-------|---------|
| C1 | Satisfied-first-try costs exactly one `start_run()` | **CORRECT** — R1 passes. |
| C2 | Iteration count matches the evaluator's real satisfaction point | **CORRECT** — R2 passes (3 iterations, 3 real turns). |
| C3 | Bound-without-satisfaction is OK, not an error | **CORRECT** — R3 passes. |
| C4 | Never exceeds `max_iterations` real calls | **CORRECT** — R3 (exactly 3, never 4+) and R6 both confirm. |
| C5 | Evaluator failure aborts immediately, propagated verbatim | **CORRECT** — R4 passes (exactly 1 turn before abort, exact error code preserved). |
| C6 | `max_iterations == 0` rejected before any call | **CORRECT** — R5 passes (zero history). |
| C7 | Feedback provenance: system/external/tainted | **CORRECT** — R2's provenance checks pass. |
| C8 | Independent of `MaxTurns` | **CORRECT** — R6 passes (`MaxTurns<1>`, 3-iteration loop still completes). |

No claim resolved **INCONCLUSIVE** or **WRONG** in this pass.

## 7. Decision and residual risks

**Decision:** Adopt Design A — `run_with_bounded_reflection()` (`include/agentengine/rt/
bounded_reflection.hpp`), an outer driver over `AgentSession::start_run()`, independent of #54's
`PlanExecuteMode`/`ContextProvider`/`PolicyDecider` machinery. No new `Agent<>` policy tag, no new
engine interception point. 002 §5 amended to correct the stale "TURN is unwired" claim and to record
why TURN, once checked for real, still doesn't reach this gap.

**Branch base:** `origin/main`, not `feature/plan-execute-mode` — confirmed independent of #53/#54's
work (§2 step 3's finding is precisely that this does NOT share a mechanism with `PlanExecuteMode`),
so there is no real dependency to stack on. `feature/plan-execute-mode`'s branch adds
`test_plan_execute_mode`/`test_todo_provider` targets to `tests/CMakeLists.txt` that don't exist on
this branch — not a gap, a consequence of the independent base.

**Residual risks, disclosed:**

- **Not independently red-teamed** — self-conducted, same disclosed posture as ADR-166 §7/ADR-167 §7.
- **No `Agent<>`-declarative authoring surface.** A caller writes `run_with_bounded_reflection(session,
  ...)` directly; there is no CRTP policy sugar. Same honest state as most of 002 §3's table today
  (ADR-167 §7's identical disclosure) — a future declarative form, if built, should compile down to
  this same outer-driver shape rather than inventing new turn-loop behavior.
- **Design B (a real, wired post-response `Middleware` interception point) is the more "native"
  long-term answer** if this pattern recurs elsewhere — not built here, named as future work, not a
  precondition to shipping this (§3).
- **Evaluator cost/latency is entirely the caller's own responsibility** — this mechanism does not
  budget or bound how expensive a single `Evaluator` invocation is (e.g. a judge-model call with its
  own token cost); only the number of iterations is bounded.
- **Does not persist reflection-loop progress across a checkpoint restart** — there is no loop-level
  state at all beyond local variables in the driving coroutine (a structurally simpler position than
  `TodoState`/`GateState`'s ADR-166/167 §7 disclosures, not a gap of the same kind, but worth stating:
  a crash mid-loop restarts the whole reflection sequence from the caller's own retry logic, not from
  wherever the loop left off).
- **Windows/clang++/Ninja only this session** — same disclosed single-platform gap as ADR-165 §7/
  ADR-166 §7/ADR-167 §7.
