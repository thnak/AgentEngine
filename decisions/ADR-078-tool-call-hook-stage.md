# ADR-078 — A tool-call hook stage, extending `ApprovalDecider` rather than duplicating it, with external dispatch routed through `Interaction` suspend/resume

**Status:** Judged (2026-08-23, approved by project owner). Design → red-team → prove complete, run as
an 8-agent workflow (1 design, 3 parallel red-team lenses — safety/I2-I3, concurrency/blocking,
completeness/regression — 1 judge/synthesis, 1 core implementation, 1 test implementation, 1
independent verify) followed by the orchestrating session's own separate, independent re-verification
(full build, full `ctest`, manual code review of the diff) rather than trusting the agents' self-reports.
**Resolves the "minimum real, coherent slice" of `OpenQuestions.md` OQ-21** (external process hooks) —
the tool-call gating point specifically; run/turn-level gating hooks, a reference external-process
dispatcher, and a declarative hook-registration surface remain real, named follow-on work (§7).

**Relates to:** `docs/planning/external-process-hooks-gap.md` and `docs/planning/external-process-hooks-
design-draft.md` (the full design record, including a 2026-08-23 Addendum re-grounding every claim
against nine days of code drift before this ADR's own work started), `decisions/ADR-023-response-
format-codec-seam.md` and `decisions/ADR-033-middleware-model-call-chain.md` (the confused-deputy/
provenance-laundering precedent this design's own guard mirrors), `decisions/ADR-029` (the `Interaction`
suspend/resume mechanism this design routes external dispatch through, unmodified),
`decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md` (an unrelated ADR whose
`TurnMiddlewareHook` turned out to already answer part of OQ-21's Q1, documented in the design draft's
addendum rather than re-solved here), `decisions/ADR-070-host-configurable-responsibility-boundary.md`
(`PolicyDecider`, threaded through unchanged as a sibling seam, never duplicated).

## 1. The question

**Stated so it has a wrong answer:** AgentEngine has no equivalent of Claude Code's Hooks feature —
an operator-attachable, allow/deny/rewrite-capable seam at a real lifecycle point, distinct from the
in-process, observation-only hooks the OpenAI Agents SDK ships (which AgentEngine's `Middleware`/
`RunEvent` already exceed). The one real gap, confirmed by fetching both frameworks' actual docs
(`docs/research/2026-08-14-claude-code-hooks-mechanics.md`): a gating hook at the tool-call point,
Claude Code's own most-used hook pair (`PreToolUse`/`PostToolUse`). Does closing this need a new,
parallel decision mechanism alongside `core/tool_pipeline.hpp`'s already-proven, already-wired
`ApprovalDecider`, or does it reuse that seam — and, separately, can a hook that needs to reach an
external process be supported at all without reopening the exact session-wide blocking hazard 009 §6
already rules out?

## 2. The competing designs

**Design A — a brand-new `before_tool`/`after_tool` Middleware pair, running alongside the existing
`ApprovalDecider` at the same call site.** Steelman: matches `Middleware`'s existing declared vocabulary
(002 §5) directly; no new concept to learn beyond what's already named. **Rejected** (original
2026-08-14 red-team pass, reconfirmed in this session's own re-grounding): creates two independent,
unordered gates with no stated precedence — the exact "two decision points that can drift" shape
ADR-023/ADR-033 both already had to close once for a different call site.

**Design B — a run/turn-level RAII boundary (`TurnBoundaryGuard`) generalized to also cover the
tool-call point.** Steelman: one mechanism for every lifecycle point, matching `AsyncMutex::Guard`'s
own precedent for firing cleanup across `run_rounds()`'s many exit paths. **Rejected**, on grounds that
turned out to be structural, not stylistic: a destructor cannot be a coroutine (C++20 forbids it), so
an async/external-dispatching hook cannot run from one, and `co_return` finalizes a coroutine's result
before any local's destructor runs, so an override-capable hook cannot be delivered from one either.
This mechanism survives ONLY for synchronous, non-overriding, observation-only run/turn hooks (real,
separate, unimplemented follow-on work, §7) — it cannot express a gating tool-call hook at all.

**Design C (chosen) — extend the existing `ApprovalDecider` seam with a new stage that runs strictly
BEFORE it, sequencing fixed by construction.** A new, optional `ToolCallHook` runs once per round, per
call, immediately after the round's `ToolCall`s are extracted and before either downstream gate
(the suspend-for-approval pre-check, or `invoke_tool()`'s own step-5 `ApprovalDecider`/`PolicyDecider`
consult) ever sees anything. Its output (a rewrite or a denial) is folded into the real
`ToolCallRequest` before either gate runs — one gate's worth of approval semantics for a caller to
reason about, not two independent ones. A hook that needs to reach an external process never runs
inline; it sets `needs_external_dispatch` and returns promptly, and the round suspends via a new
`interaction_reason::hook_decision`, resumed later through the EXISTING `resolve_interaction()` path
(ADR-029) — no new cancellation primitive, because none is needed.

## 3. Falsifiable claims

| # | Claim | Verdict | Basis |
|---|---|---|---|
| H1 | A denying hook stops the round before `ApprovalDecider` or the real tool's `invoke()` ever runs; the denial folds as an ordinary decided outcome, not a suspend. | **CORRECT** | `tests/test_rt_agent_session_tool_call_hook.cpp` H1 (8 checks): no suspend, no `Interaction` opened, the real tool's `invoke()` never reached, an `ApprovalDecider` tripwire wired into the same session never even consulted. |
| H2 | A rewriting hook's output reaches the real dispatched call, downgrades the call's `provenance` to `text_derived`, and is refused with no approving decider even against a tool that ALSO declares `Approval<never_require>` — the same override `ADR-023 P2-T2` already proved once, now at this new call site. | **CORRECT** | H2a-c (7 checks): the real tool observes the hook-rewritten argument value, never the model's original one; a capability-bearing, `never_require`-declaring tool is refused with no decider present; an explicit approval still lets the rewritten call through, proving this is a gate, not a permanent block. |
| H3 | With no hook registered, behavior is byte-for-byte identical to a session with no concept of a tool-call hook at all — both the immediate-success path and the full suspend-for-approval/resolve round trip. | **CORRECT** | H3a-b (9 checks): a `never_require` capability-bearing tool still runs with no decider; the existing suspend-for-approval path is untouched (`interaction_reason::approval`, never `hook_decision`, when no hook is wired); matches `test_rt_agent_session_suspend_approval.cpp`'s own SU1/SU3 assertions exactly. |
| H4 | The `interaction_reason::hook_decision` suspend/resume round trip works for both an external-process approval and an external-process denial, and correctly interacts with a wired `PolicyDecider` on resume. | **CORRECT** | H4a-c (18 checks): a real `Interaction` opens tagged `hook_decision`; `hook_decision_requested` fires naming the right `call_id`/`interaction_id`/`tool_name`; `resolve_interaction()` with `hook_dispatch_answers` resumes and completes the call on approve; the real tool is never invoked on an external-process deny; a `PolicyDecider`'s `auto_approve` verdict is honored on the resume path, not spuriously overridden for lack of an `ApprovalDecider` that was never actually needed. |
| C1 (coverage) | The hook stage cannot be bypassed by suspending for plain approval before a hook-touched round's rewrite is accounted for. | **CORRECT, closed during the design pass, not assumed clean from the start** | See §4 — this is the specific gap the design phase itself found and fixed before any code was written: `any_needs_external_dispatch` and `any_needs_approval` are computed from the SAME post-hook per-call state in one pass, so a call needing human approval can never be silently skipped because another call in the same round "got there first" via `hook_decision`. |
| C2 (authority) | An external process's own dispatch answer can never be silently treated as a human's approval. | **CORRECT, a second fatal finding closed during implementation, not present in the original 2026-08-14 red-team pass** | `resolve_hook_decision()` deliberately does NOT reuse `resolve_codeact_ask()`'s `one_shot_approve` shape — it re-checks approval need against the REAL `approval_decider_`/`policy_decider_` after folding in the external answer, and cascades into a genuine `interaction_reason::approval` suspend if a decider is still needed and none is configured. Proven structurally (the code path) and behaviorally (H4c's `PolicyDecider` check, and the absence of any test where a `hook_decision` resume alone satisfies an approval-required call with no decider present). |

## 4. The red-team attack, and two findings the original 2026-08-14 pass never had

**Findings already closed by the original design draft (2026-08-14), unchanged here:** (1)
`start_run()` holds `session_mutex_` for the whole run with no timeout — an inline-`co_await`ed
external-dispatch hook would stall the entire session; closed by the suspend/resume routing, not an
inline call, ever. (2) no concrete insertion point existed for a provenance-downgrade guard on a
hook-rewritten call, reopening the confused-deputy hole ADR-023/ADR-033 already closed once; closed by
`enforce_hook_rewritten_tool_call_provenance()`, mirroring `middleware.hpp`'s
`enforce_backend_tool_call_provenance()` exactly — diffing canonical JSON bytes before/after the hook
ran (`json::Value` has no `operator==` in this codebase; the comparison goes through `json::dump()`
text, the same equality-by-serialization idiom `derive_idempotency_key()` already relies on elsewhere
in `tool_pipeline.hpp`).

**Finding found by THIS pass's own design phase, before any code was written (C1 above):** a literal
reading of "insert the hook stage right before `invoke_tool()`" is insufficient. `run_rounds()` has
TWO sequential decision points that can suspend or deny a round — a suspend-for-approval pre-check
(evaluating the ORIGINAL, un-rewritten call), and `invoke_tool()`'s own step-5 gate (reached only for
calls the pre-check didn't suspend). A hook that rewrites a `never_require`, capability-bearing call's
arguments is invisible to the pre-check, which still sees the original, unrewritten `provenance`. Left
uncorrected: the pre-check decides "no approval needed," the hook then rewrites and
`enforce_hook_rewritten_tool_call_provenance()` downgrades to `text_derived`, and because this session
is in suspend-mode specifically because `approval_decider_` is unset, the call is silently **denied**
rather than suspended — fail-closed, not an I2/I3 violation, but a real UX/coverage defect defeating
the exact case (a hook that rewrites) this design exists to gate correctly. Closed by computing both
`any_needs_external_dispatch` and `any_needs_approval` from the identical post-hook per-call state, in
one pass, before either downstream mechanism runs.

**Finding found during implementation, not by any red-team pass (C2 above):** an earlier internal
sketch (named in `core/tool_call_hook.hpp`'s own file-top comment) proposed a `resolve_hook_decision()`
that reused `resolve_codeact_ask()`'s `one_shot_approve` shape verbatim — auto-approving the WHOLE
resumed round once an external process answered. This conflates two different questions ("did the
external process allow the call" and "did a human approve its execution") and would let an external
hook's own dispatch answer stand in for human approval with no decider ever consulted — a real
authority-widening bug, closed by making `resolve_hook_decision()` re-verify approval need against the
real deciders after folding in the external answer, cascading to a genuine approval suspend rather than
ever short-circuiting it.

**Three independent red-team lenses (safety/I2-I3, concurrency/blocking, completeness/regression), run
in parallel against the judged design before implementation started**, found no further fatal issues —
their findings were folded into the final design the implementation agents built from (the workflow's
own judge/synthesis stage; full per-lens findings live in the workflow's own transcript, not duplicated
here since every claim they'd have raised is already covered by H1-H4/C1-C2's testable shape above).

## 5. Executed evidence

`core/tool_call_hook.hpp` (new): `ToolCallHookContext` (`call_id`, `tool_name`, `arguments`,
read-only `provenance`, `caller: Principal const&` — no `EffectContext&`, no capability type, matching
`middleware.hpp`'s `ModelCallContext` I2 discipline exactly), `ToolCallHook` (`task<result<
std::monostate>>(ToolCallHookContext&)`, `nullptr` by default), `hook_call_outcome`,
`HookProcessedCall`/`PendingHookDecisionRound` (carrying hook-processed state across a suspend/resume,
the same shape `PendingCodeActAsk` already establishes), `HookDispatchAnswer`. `core/tool_pipeline.hpp`
gained `enforce_hook_rewritten_tool_call_provenance()`. `core/interaction.hpp` gained
`interaction_reason::hook_decision` (both directions of `rt/interaction_codec.hpp`'s JSON codec
updated; `protocol/agui/projection.hpp`'s event-kind `switch` updated so an unmatched case doesn't
silently fall through to the AG-UI projector; `001-Execution-Model.md` amended, since
`interaction_reason` is named there normatively). `rt/agent_session.hpp` gained the hook-stage block in
`run_rounds()`, `resolve_hook_decision()`, and `finish_hook_processed_round()`, plus
`set_tool_call_hook()`/`tool_call_hook_` matching `set_approval_decider()`'s existing convention and
`pending_hook_decisions_` (cleared on session reset alongside `pending_codeact_asks_`).

`tests/test_rt_agent_session_tool_call_hook.cpp` (new, mirroring `test_rt_agent_session_suspend_
approval.cpp`'s own precedent): **46/46 checks passing** (H1: 8, H2: 7, H3: 9, H4: 18, plus setup
checks) — see §3's table for what each proves. Full project build: clean, zero errors. Full `ctest`:
**229/231 real passes**; the only 2 failures (`test_openai_chat_client_live`,
`test_anthropic_chat_client_live`) are the same pre-existing, environment-specific live-TLS failures
already independently confirmed unrelated to this session's work (reproduced identically against an
untouched baseline via `git stash`, in this same session, for a different change earlier the same day)
— zero new regressions. All of the above independently re-verified by the orchestrating session directly
(its own `cmake --build`/`ctest` runs and a manual read of the real diff), not taken on the implementing
workflow agents' own self-report.

## 6. The decision

**Design C is accepted, implemented, and proven**, scoped narrowly to the tool-call point — the
"minimum real, coherent slice" the original design draft named (its own punch-list items 1-2).

**Binds:**
- `002-Agent-Model-and-Authoring.md` §5 — names the tool-call hook stage as the first real,
  gating, external-capable hook AgentEngine ships, distinct from `Middleware`'s own declared-but-
  narrower-scoped interception points.
- `001-Execution-Model.md` §2 — `interaction_reason`'s value set amended to include `hook_decision`
  (the enum's own comment is the authoritative amendment trail per this project's existing convention
  for this specific field, per the RFC's own now-added cross-reference).
- `OpenQuestions.md` — resolves OQ-21's minimum slice; the question moves to Resolved with the
  remaining scope named explicitly as follow-on, not silently dropped (§7).

**Explicitly out of scope, named rather than left implied** (unchanged from the design draft's own
punch list, items 3-8, still real, still unimplemented):
- `SandboxBackend::exec` is still synchronous (re-confirmed current, not stale, during this pass) — a
  prerequisite for any reference external-dispatcher built on `native-jail` to avoid hard-blocking its
  calling thread. Not this ADR's job to build.
- No reference `ExternalCommandMiddleware`-shaped example ships under `examples/` in this pass.
- Run/turn-level **gating/override** hooks and any **external-dispatching** run/turn hook remain
  unimplemented — `TurnBoundaryGuard`/`RunBoundaryGuard` (synchronous, observation-only `after_turn`/
  `after_run`) is real, separate, unimplemented follow-on work; `decisions/ADR-067-...`'s
  `TurnMiddlewareHook` already provides a real, wired, async, gating, PRE-model turn-level hook for a
  host that wants one today (documented in the design draft's own addendum, not re-solved here) — it
  cannot express override-after-outcome power, the one thing this ADR's mechanism and ADR-067's both
  structurally cannot deliver, for the same C++20-coroutine-destructor reason.
- A declarative/config-file-driven hook registration surface remains out of scope — registration stays
  compile-time, host/deployer-assembled, matching `ApprovalDecider`'s own existing convention.

## 7. Residual risks

- `pending_hook_decisions_` is not durably checkpointed — the same disclosed limitation
  `pending_codeact_asks_` already carries, inherited here, not newly introduced.
- A purely in-process (synchronous) hook body has no engine-enforced time bound — matching
  `ApprovalDecider`'s own existing unbounded-synchronous contract; an accidentally slow in-process hook
  is a host-authoring risk this design does not newly introduce or newly guard against.
- Format/vocabulary coverage is intentionally narrow: this closes Claude Code's `PreToolUse`/
  `PostToolUse` analog only. `SessionStart`/`SessionEnd` and most of Claude Code's remaining named
  events are already fully served by the existing, unchanged `RunEvent` stream (16 of Claude Code's 24
  named events are pure observation, per `docs/research/2026-08-14-claude-code-hooks-mechanics.md`
  §2's own table) — no new mechanism needed for those, confirmed, not re-litigated, by this ADR.
