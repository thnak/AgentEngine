# ADR-149: A Magentic-shaped workflow convenience API

## 1. The question

GitHub issue #28 named five convenience gaps versus MAF's `MagenticWorkflowBuilder` sample, all
sitting on top of AgentEngine's own already-proven, non-concurrent primitives
(`workflow/graph.hpp`'s `WorkflowBuilder`/`TypedExecutor`, `rt/workflow_supervisor.hpp`'s
`WorkflowSupervisor`, the `request_port`/`resume_workflow()` HITL mechanism): no reusable
manager/participant builder, no stall/reset loop-detection bound, no typed plan-signoff HITL
payload, no packaged checkpoint-persistence convenience, no named transcript reconstruction. Can
these be built as pure additive sugar over what already exists — 014-Workflow-and-Orchestration.md
§9 Q5 says Magentic "is not a separate subsystem... no new engine primitive" — without widening any
capability/authority boundary, and without silently mis-describing what they actually cover for a
cyclic (repeat-visit) graph, which is exactly Magentic's own shape?

Issue #28's item 5 (typed per-executor live-event multiplexing) is EXCLUDED from this ADR's scope.
It is the identical gap issue #29 tracks, and it is a genuine concurrency/stream-merging design —
categorically different risk from the rest of #28. Deferred to a follow-on ADR, explicitly framed as
closing #29 too.

## 2. Scope

Five pieces, all additive to already-shipped, tested code:

1. `MagenticWorkflowBuilder<TaskMsg, ReportMsg>` (`workflow/magentic.hpp`) — a manager/participant
   convenience wrapper over `WorkflowBuilder`.
2. `TerminationBound::max_stalls`/`max_resets` (`workflow/graph.hpp`) + engine enforcement in
   `WorkflowSupervisor::execute()` (`rt/workflow_supervisor.hpp`).
3. `MagenticPlanSignoffRequest`/`Response` + marshal helpers (`workflow/magentic.hpp`) riding the
   existing `request_port`/`resume_workflow()` path.
4. `WorkflowCheckpointManager<StoreT>` (`rt/workflow_checkpoint_manager.hpp`) — wraps the
   already-real `save_workflow_checkpoint()`/`load_workflow_checkpoint()`/`SessionStore`.
5. `latest_outputs_of(WorkflowResult const&)` (`rt/workflow_supervisor.hpp`) — NOT a full
   multi-visit transcript; see §5.

## 3. Red-team findings and how each is addressed

An independent red-team pass (fresh agent, no prior context) reviewed the first design draft
BEFORE any code existed. Full findings in
`docs/planning/magentic-workflow-convenience-api-design-draft.md` §3; 3 of its own 6 named punch-
list items had a concrete, code-verified defect, plus 3 additional MUST-FIX problems the punch list
didn't name. Summary and resolution:

- **`stalled`'s I2/I3 boundary.** The first draft let `ExecutorOutcome::stalled` from ANY executor
  directly end the run — a capability not even a human's `request_port` answer has today (a route
  only selects among edges the graph author already wired; it can never itself terminate the run).
  **Fixed**: `WorkflowSupervisor::initialize()` gains `designated_stall_reporter` — an explicit,
  host-named executor id. `execute()` only trusts `stalled` from THAT id; every other executor's
  `stalled` is inert. Empty (default) disables the mechanism entirely, regardless of
  `TerminationBound` — fails closed when unset, the ADR-070/ADR-071 Delegated Decision Seam shape.
- **`require_plan_signoff()`'s "no new primitive" claim was false as first drafted.** A bare,
  unwired port node fails `validate_workflow`'s reachability check outright; even wired, a manager
  with one static `Out` type cannot type-check an edge to a differently-typed signoff payload
  without a relay/adapter — itself new engine machinery. **Fixed**: the port is wired EXACTLY like a
  participant, reusing the SAME `TaskMsg`/`ReportMsg` pair the graph already shares. The typed
  payload rides inside that ordinary `Message` via a `Custom` `ContentItem` — the identical
  Args/Reply-over-untyped-JSON idiom already used for every tool call in this codebase
  (`AE_JSON_SCHEMA`), not a new mechanism.
- **Checkpoint/resume silently discards a moderator's conversation history**, which is specifically
  damaging for Magentic (its whole value is a moderator that RETAINS context across rounds) —
  `docs/planning/agent-as-workflow-executor-design-draft.md` already accepted this gap for the
  general case, but `WorkflowCheckpointManager` making resume a one-liner lowers the friction that
  today forces an app author to reckon with it. **Fixed**: `resume_or_start()` refuses (a
  `contract`-class error) to resume any graph containing an `agent`-kind executor unless
  `acknowledge_agent_history_reset=true` is explicitly passed.
- **`stall_streak_`/`resets_used_` were missing from `RunStateRecord`** — since the checkpoint hook
  fires at EVERY superstep boundary unconditionally (the normal, encouraged persistence pattern),
  any host that checkpoints a Magentic run at all would silently reset stall bookkeeping to zero on
  every resume — an unlimited-stall-budget bypass of the exact safety valve being added. **Fixed**:
  both fields added to `RunStateRecord`, threaded through `to_record()`/`restore_from_record()`,
  proven by a real checkpoint/resume round-trip test (S6, below) that a resumed run trips at the
  SAME total round count an uninterrupted run would.
- **`max_stalls` vs. `max_rounds`/`deadline_ms` precedence was undefined** for the same round
  boundary. **Resolved by the chosen insertion point, not a special case**: the stall check runs
  while folding round N's results (the same point `broke`/`merge_failed` already resolve at);
  `max_rounds`/`deadline_ms` are only re-checked at the top of the loop for a would-be round N+1.
  A stall/reset trip on round N always takes effect first, mechanically.
- **NEW, MUST-FIX, not on the original punch list — `transcript_of()`/`Transcript` as first drafted
  is broken for cyclic graphs, i.e. for Magentic itself.** `WorkflowResult::partial` keeps AT MOST
  ONE entry per `executor_id` (`record_partial()` overwrites in place on every revisit) — no test in
  this codebase had ever visited a node twice, so this never surfaced. Magentic is DEFINED by
  repeat visits; by completion, `partial` holds only the LAST thing each node said. **Resolution: an
  honest scope cut, not a silent gap.** This pass ships `latest_outputs_of()`, named for exactly
  what `partial` contains — the most recent message per executor, not a history. A real multi-visit
  transcript needs a per-round hook into `execute()`'s dispatch loop, right where `record_partial()`
  currently overwrites — the SAME mechanism a follow-on ADR/#29 needs anyway, so it is deferred there rather
  than building a second, throwaway hook here. **Issue #28 item 6 is therefore only partially closed
  by this ADR.**

**A further, real defect found DURING implementation** (not by the red-team pass, since no code
existed yet when it ran): the first implementation of `MagenticWorkflowBuilder` used ONE shared
`<In, Out>` type pair for both `.manager()` and `.participant()`. Since `WorkflowBuilder::connect`
requires `FromOut == ToIn` per edge, and both manager and participant were the SAME
`TypedExecutor<In, Out>` instantiation, the very first `manager->participant` edge's static_assert
required `In == Out` — collapsing the builder to one degenerate type for the WHOLE graph, unable to
compile with two genuinely distinct message types at all. Caught before any test ran, by working
through the type-flow by hand. **Fixed**: two independent type parameters, `TaskMsg` (manager→
participant) and `ReportMsg` (participant→manager), REVERSED between `.manager()`'s
`TypedExecutor<ReportMsg, TaskMsg>` and `.participant()`'s `TypedExecutor<TaskMsg, ReportMsg>` —
exactly what makes both edge directions type-check under `connect`'s real rule.

**A second defect found DURING implementation, via the end-to-end test (B6), not the earlier
self-loop-only unit tests**: the first `execute()` implementation reset `stall_streak_` to 0 on ANY
round the designated reporter simply didn't run (e.g. a participant's turn) — not just when it
explicitly reported progress. In a real manager/participant alternation (manager runs, then a
participant, then the manager again), the reporter never runs on two CONSECUTIVE rounds, so the
streak could never exceed 1, silently defeating `max_stalls` for exactly the graph shape this
feature targets. Every hand-built self-loop unit test (S1-S6) happened to have the reporter run
every round, so none of them caught it. **Fixed**: a round the reporter did not run in is now
NEUTRAL (streak unchanged); only a round it DID run in updates the streak, from that round's own
`stalled` value.

**A third, pre-existing, unrelated defect found via the checkpoint manager's own disk-backed test
(C5)**: `FileSessionStore::path_for()` (`rt/session_store.hpp`, ADR-037-era code, untouched by this
ADR's own design) rejects `/`, `\`, `..` but does not sanitize `:` — a legal `SessionId` character
that is NOT a legal Windows filename character (reserved for drive letters / NTFS alternate data
streams). `WorkflowSupervisor::run_id()` ALWAYS has the shape `"<graph.id>:run:<counter>"` — so
`save_workflow_checkpoint()`/`WorkflowCheckpointManager` against `FileSessionStore` was silently
broken on Windows for EVERY workflow, before this fix, confirmed by a direct repro
(`std::ofstream` fails to open a path containing `:`). **Fixed** (in `session_store.hpp`, not new
code this ADR owns going forward, but a real bug this pass found and closed): a
`sanitize_for_filesystem()` helper percent-escapes `:`/`<`/`>`/`"`/`|`/`?`/`*`/control chars/a
literal `%` (kept injective) before joining with `root_`. The existing `/`/`\`/`..` REJECTION is
unchanged (it exists to catch a caller's mistake, not to make an id filesystem-safe) — verified
`tests/test_rt_session_store.cpp`'s full existing suite (43 checks) still passes unmodified.

## 4. The accepted design

See `docs/planning/magentic-workflow-convenience-api-design-draft.md` §1/§3 for the full design
writeup with the reasoning behind each choice. Summary of the shipped shape:

- `MagenticWorkflowBuilder<TaskMsg, ReportMsg>::build()` produces the exact cyclic graph
  `examples/17_planner_live.cpp` already hand-builds — `switch_case` manager→{participants, done,
  optional plan-review port}, `direct` back to the manager, falling through to the SAME shared
  `validate_workflow()` every hand-built graph uses. Layering preserved: `workflow/magentic.hpp`
  never depends on `agentengine::rt::` — the done sink's identity body is caller-supplied, matching
  every other graph's "bodies always caller-supplied, parallel by index" convention.
- `designated_stall_reporter` + `ExecutorOutcome::stalled` (default `false`, additive) + `execute()`
  round-loop enforcement, exactly at the `broke`/`merge_failed` terminal-path position.
- `MagenticPlanSignoffRequest`/`Response`, `AE_JSON_SCHEMA`-annotated, marshaled via
  `make_plan_signoff_request()`/`make_plan_signoff_response()`/`parse_plan_signoff_response()`
  through an ordinary `Custom` `ContentItem`.
- `WorkflowCheckpointManager<StoreT>::attach()` (auto-persist every round) /
  `resume_or_start()` (one-call "resume if a checkpoint exists, else start fresh," fail-closed on
  agent-kind executors) — zero new serialization or store abstraction; wraps what already exists.
- `latest_outputs_of()` — see §3's honest rescoping.

## 5. What this ADR does not claim

- Issue #28 item 6 (transcript) is only PARTIALLY closed — `latest_outputs_of()` returns the most
  recent message per executor, not a full multi-visit history. A real transcript is deferred to
  a follow-on ADR alongside the per-executor event multiplexing it structurally needs.
- Issue #28 item 5 (typed live-event stream) is entirely out of scope here — deferred to a follow-on ADR,
  which also closes #29.
- `require_plan_signoff()`'s port is wired into the graph, but the DECISION of when the manager
  routes to it (e.g., "only on the first round") is the app author's own `ExecutorBody` logic, not
  new engine machinery — matching 014 §9 Q5's own answer.
- `WorkflowCheckpointManager` does not add durability beyond what `SessionStore`'s conformers
  already provide — `FileSessionStore`'s own documented "no atomic rename, a crash mid-write can
  torn-write" limitation is unchanged and unaddressed by this ADR.
- The `sanitize_for_filesystem()` fix closes the specific Windows-reserved-character class found;
  it is not a claim that `FileSessionStore` is now fully cross-platform-hardened against every
  possible id (e.g. reserved Windows device names like `CON`/`NUL` as a whole id are not special-
  cased) — out of this ADR's bounded scope, named rather than silently assumed handled.

## 6. Falsifiable claims and verdicts

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 1 | The builder's produced graph validates against the shared `validate_workflow()` for a well-formed manager+participants+optional-signoff-port configuration. | CORRECT | `test_workflow_magentic_builder.cpp` B2/B3 |
| 2 | `stalled` from a non-designated executor is inert; the run is unaffected regardless of how persistently a non-reporter node reports it. | CORRECT | `test_rt_workflow_stall_reset_bounds.cpp` S4 |
| 3 | A stall trip with no `max_resets` ends the run at the exact expected round via `bound_max_stalls`, with `resets_used()==1`. | CORRECT | S1 |
| 4 | With `max_resets` set, a trip under the ceiling is silently absorbed; only a trip exceeding it ends the run, via `bound_max_resets`. | CORRECT | S2 |
| 5 | A non-stalled round from the reporter resets the streak; a round the reporter simply didn't run in does NOT. | CORRECT (second half caught a real bug, then fixed) | S3 (first half); `test_workflow_magentic_builder.cpp` B6 (second half, against a real manager/participant graph) |
| 6 | `TerminationBound` with ONLY `max_stalls`/`max_resets` set (no `max_rounds`/`deadline_ms`/`token_budget`) satisfies 014 §2's mandatory-bound validation. | CORRECT | S5 |
| 7 | `stall_streak_`/`resets_used_` round-trip through checkpoint/resume — a resumed run trips at the SAME total round count an uninterrupted run would, not a reset-to-zero streak. | CORRECT | S6 |
| 8 | The typed plan-signoff request/response round-trips through `AE_JSON_SCHEMA`'s generated codec, and a REAL `request_port`/`resume_workflow()` cycle delivers the typed response to the manager's next invocation. | CORRECT | `test_workflow_magentic_plan_signoff.cpp` P1/P2/P4 |
| 9 | `parse_plan_signoff_response()` rejects (not silently defaults) a Message carrying no matching `Custom` item. | CORRECT | P3 |
| 10 | `WorkflowCheckpointManager::attach()` auto-persists every round with zero caller-written hook code, against both `InMemorySessionStore` and a REAL on-disk `FileSessionStore`. | CORRECT | `test_rt_workflow_checkpoint_manager.cpp` C1, C5 |
| 11 | `resume_or_start()` correctly resumes a BRAND-NEW supervisor from a real mid-flight (suspended-at-a-port) checkpoint into the identical open-interaction state, and driving it onward matches an uninterrupted run's result. | CORRECT | C3 |
| 12 | `resume_or_start()` fails closed on an agent-kind executor without explicit acknowledgment, and proceeds once acknowledged. | CORRECT | C4 |
| 13 | The `FileSessionStore` fix does not regress any existing `SessionStore` behavior. | CORRECT | `test_rt_session_store.cpp` full suite (43 checks) unmodified, still passing |
| 14 | Every pre-existing `workflow_supervisor`-family test still passes after the additive `ExecutorOutcome`/`ExecuteReply`/`TerminationBound`/`RunStateRecord` field changes. | CORRECT | `test_rt_workflow_supervisor`, `test_rt_workflow_supervisor_request_port`, `test_rt_workflow_checkpoint_g2`, `test_rt_workflow_time_travel`, `test_rt_workflow_live_view`, `test_rt_agent_workflow_executor` — all pass, zero regressions |
| 15 | The full project (every existing target, not just the workflow-family tests) still builds clean after these header changes. | CORRECT | Full `cmake --build` (Debug, Visual Studio 18 2026 generator, MSVC), exit code 0, zero errors |

Live-network verification of `examples/19_magentic_builder_live.cpp` against a real OpenRouter model
was NOT run this pass (needs `AGENTENGINE_OPENROUTER_API_KEY`, and a `build-https`-configured
build) — the example builds against the same headers verified above, and its logic is a direct,
minimally-adapted port of `17_planner_live.cpp`'s already-live-verified moderator/specialist
pattern, but the live run itself is a named residual, not claimed as executed evidence here.

## 7. Files changed

**New:**
- `docs/planning/magentic-workflow-convenience-api-design-draft.md`
- `include/agentengine/workflow/magentic.hpp`
- `include/agentengine/rt/workflow_checkpoint_manager.hpp`
- `tests/test_rt_workflow_stall_reset_bounds.cpp`
- `tests/test_workflow_magentic_builder.cpp`
- `tests/test_workflow_magentic_plan_signoff.cpp`
- `tests/test_rt_workflow_checkpoint_manager.cpp`
- `examples/19_magentic_builder_live.cpp`

**Edited:**
- `include/agentengine/workflow/graph.hpp` — `TerminationBound::max_stalls`/`max_resets`,
  `WorkflowBuilder::max_stalls()`/`max_resets()`.
- `include/agentengine/rt/workflow_supervisor.hpp` — `ExecutorOutcome::stalled`,
  `ExecuteReply::stalled`, `workflow_status::bound_max_stalls`/`bound_max_resets`,
  `WorkflowSupervisor::initialize()`'s `designated_stall_reporter` parameter +
  `stall_streak()`/`resets_used()` accessors, `execute()`'s round-loop enforcement,
  `RunStateRecord::stall_streak`/`resets_used` (+ JSON codec, backward-compatible on read),
  `to_record()`/`restore_from_record()` threading, `latest_outputs_of()`/`Transcript`.
- `include/agentengine/rt/session_store.hpp` — `FileSessionStore::sanitize_for_filesystem()` (the
  Windows-reserved-character fix, §3).
- `include/agentengine/rt/workflow_checkpoint_manager.hpp` — §8 fix 3 (below).
- `tests/test_rt_workflow_stall_reset_bounds.cpp`, `tests/test_rt_workflow_checkpoint_manager.cpp` —
  §8 fixes 1/3 regression tests (S7, C4b).
- `tests/test_workflow_magentic_builder.cpp`, `tests/test_workflow_magentic_plan_signoff.cpp`,
  `examples/19_magentic_builder_live.cpp` — §8 fixes 2/4.
- `tests/CMakeLists.txt`, `examples/CMakeLists.txt` — new target registrations.

## 8. Post-implementation red-team audit

A SECOND independent red-team pass (fresh agent, no prior context) audited the IMPLEMENTATION —
not the design draft this time — after the project owner's own review, per this repo's "judge only
on evidence" discipline. It independently re-ran every test binary itself (reading the actual
`ok:`/`FAIL:` lines, not trusting exit codes), rebuilt `build-https` from scratch to check whether
`examples/19_magentic_builder_live.cpp` had ever actually been compiled, and wrote a standalone
empirical repro to test a specific adversarial hypothesis before removing it.

**All 15 falsifiable claims in §6 independently reproduced**, with the audit's own evidence, not a
restatement of this document's. One genuine MUST-FIX bug and three lower-severity residuals were
found and are fixed below.

1. **MUST-FIX, real bug: the stall-check loop could silently drop a real stall signal from a
   `function`-kind (or any non-`agent`-kind) `designated_stall_reporter` that received TWO
   concurrent deliveries in one round.** The existing quarantine mechanism (`execute()`,
   `workflow_supervisor.hpp`) only dedupes concurrent same-round deliveries to an `agent`-kind
   executor; nothing did so for other kinds. The original stall-check loop took the FIRST matching
   delivery's `stalled` value and `break`-ed — silently discarding a genuine `stalled=true` on a
   second delivery to the same reporter index, the opposite of what a safety valve should fail
   toward. The audit wrote and ran a standalone repro (`root --fan_out--> {srcA,srcB} --direct-->
   mgr`) proving a real, deterministic drop. **Fixed**: the loop now OR-aggregates `stalled` across
   EVERY delivery to the reporter's index this round, rather than stopping at the first match.
   Regression test added: `test_rt_workflow_stall_reset_bounds.cpp` S7, using the audit's own repro
   shape.
2. **Real, but not yet reachable: `struct TaskMsg {}`/`ReportMsg {}` declared at true file scope
   (external linkage) with a DIFFERENT `AE_WORKFLOW_MESSAGE` string per file, in three separate
   files** (`test_workflow_magentic_builder.cpp`, `test_workflow_magentic_plan_signoff.cpp`,
   `examples/19_magentic_builder_live.cpp`) — a latent ODR violation (three non-identical explicit
   specializations of `agentengine::workflow::message_type<TaskMsg>` for the same qualified-name
   entity) if these TUs were ever linked together. Not reachable today (no `CMAKE_UNITY_BUILD`, each
   is its own `add_executable`), but not structurally guaranteed either. **Attempted fix, then
   corrected**: moving the declarations into each file's anonymous namespace does NOT compile — an
   explicit specialization of `agentengine::workflow::message_type<T>` must be declared in a
   namespace enclosing `agentengine::workflow`, and MSVC (correctly) rejects an anonymous namespace
   there with C2888. **Actual fix**: the type names AND their `AE_WORKFLOW_MESSAGE` strings are now
   IDENTICAL across all three files — matching this codebase's own pre-existing, working precedent
   (`Question`/`Draft`/`Verdict`, reused verbatim across `test_workflow_graph_validation.cpp` and the
   `compile_fail/workflow_edge_type_*` pair) — since IDENTICAL definitions of the same entity across
   TUs are not an ODR violation; only differing ones are.
3. **Real, currently-inert residual: `WorkflowCheckpointManager::resume_or_start()`'s fail-closed
   guard checked only `executor_kind::agent`, not `sub_workflow`.** Not exploitable today
   (`sub_workflow` cannot execute at all — `check_workflow_executable()` refuses it unconditionally,
   issue #33), but a future `sub_workflow` runtime bridge would carry the same "hidden per-executor
   state lost on resume" hazard `agent`-kind already has, and the guard's shape wasn't broad enough
   to catch it without a second fix later. **Fixed**: the guard now also checks
   `executor_kind::sub_workflow`. Regression test added: `test_rt_workflow_checkpoint_manager.cpp`
   C4b.
4. **Evidence-accuracy issue, not a code defect: §6's residual paragraph asserted the live example
   "builds against the same headers verified above" in present tense, but `build-https` had NEVER
   actually compiled `examples/19_magentic_builder_live.cpp` before this audit** — no `.vcxproj` for
   it even existed in that build tree. The audit triggered the build itself; it compiled and linked
   clean and ran correctly (SKIPPED, exit 0, no API key set), but surfaced one real, previously-
   unseen MSVC warning (`C4244` narrowing, `int`→`char`, in the `upper()` helper's
   `std::toupper()`-returning lambda). **Fixed**: explicit `static_cast<char>(...)`. Re-verified via
   a real `build-https` rebuild (not just the default `build/` config) after the fix: clean compile,
   zero warnings, correct skip behavior.

Re-verified after all four fixes: all four new test binaries (`ALL PASS`, including the two new
regression scenarios), every pre-existing `workflow_supervisor`-family test plus
`test_rt_session_store` (zero regressions), `examples/19_magentic_builder_live.cpp` rebuilt clean
under `build-https` (not just the default config), and a full project rebuild (`cmake --build`,
Debug, MSVC) — zero errors.

## Status

**Proposed — implemented, independently red-teamed twice (design draft, then implementation), all
listed evidence executed and passing, all findings from both passes fixed and re-verified.** Per
`decisions/README.md`'s own convention, this ADR needs explicit project-owner sign-off to move to
Judged.
