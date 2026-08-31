# Design draft: a Magentic-shaped workflow convenience API

Closes GitHub issue #28's items 1, 2, 3, 4, 6 (item 5 — typed per-executor live-event
multiplexing — is deferred to a follow-on ADR, since it is the same gap issue #29
tracks and has a categorically different, concurrency-shaped risk profile from the rest of this
issue). Tracked from issue #32's design-pass consolidation.

## Why this is convenience sugar, not a new primitive

014-Workflow-and-Orchestration.md §3's Planner (Magentic) row is explicit: "the same
cyclic-moderator shape as Group chat/debate, but the moderator maintains a task/progress ledger,
picks the next participant and decides completion itself, and self-triggers a replan on stall
detection — round/stall/reset bounds are a safety valve, not the termination contract." §9 Q5
reaffirms: "No new engine primitive: cyclic graphs with a mandatory bound (§2) and a request port
for optional human plan review (§4) already express it." `examples/17_planner_live.cpp` already
hand-builds exactly this shape against a real model today. What's missing is purely ergonomic:
every app author re-derives the manager/participant wiring, the stall-loop safety valve, the typed
HITL payload, and the checkpoint-persistence glue by hand.

## 1. The first draft

- `MagenticWorkflowBuilder` (`include/agentengine/workflow/magentic.hpp`, `namespace
  agentengine::workflow`) wraps `WorkflowBuilder`: `.manager(TypedExecutor<In,Out>)`,
  `.participant(TypedExecutor<In,Out>)` (repeatable), `.done_selector(std::string = "done")`,
  `.max_stalls(n)`, `.max_resets(n)`, `.require_plan_signoff(std::string port_id =
  "plan_review")`. `.build()` produces: `switch_case` manager→each participant (case label =
  participant id), `direct` each participant→manager, `switch_case` manager→a synthetic identity
  "done" sink (case label = `done_selector()`), `start_at(manager)`,
  `select_output(done_sink_id)`, plus (if `require_plan_signoff` was called) one
  `request_port`-kind node under `port_id`. Falls through to the existing shared
  `validate_workflow()` — no new validation path.
- `TerminationBound` (`workflow/graph.hpp`) gains `max_stalls`/`max_resets`
  (`optional<uint32_t>`), engine-enforced (unlike `token_budget`, which ADR-070 documents as
  host-monitored-only). `ExecutorOutcome` (`rt/workflow_supervisor.hpp`) gains `bool stalled =
  false;` — an explicit, opt-in signal a moderator body sets on its own outcome, threaded through
  `ExecuteReply` the same way `routes` already is. `WorkflowSupervisor::execute()`'s round loop
  tracks `stall_streak_`: any reply this round with `stalled == true` increments it, any round
  with none resets it to 0. Crossing `max_stalls` counts a "reset" (`resets_used_++`,
  `stall_streak_` cleared); with no `max_resets` set, a stall trip ends the run immediately
  (`workflow_status::bound_max_stalls`); with `max_resets` set, the run only ends
  (`bound_max_resets`) once resets exceed the ceiling — every reset under it is silently absorbed.
  The engine never infers what a stall MEANS; it only counts an explicit boolean, exactly as
  trusted as `routes` already is (I3 — model/host output is data, never authority; `stalled` is
  data the host chooses to emit off the moderator's own judgment, same trust level a route label
  already carries per `tests/test_rt_workflow_supervisor_request_port.cpp`'s IQ5 finding that "a
  human's answer gets no more authority than a model's").
- `MagenticPlanSignoffRequest{std::string plan}` / `MagenticPlanSignoffResponse{bool approved;
  std::string feedback}`, each `AE_JSON_SCHEMA`-annotated (mirrors the tool Args/Reply idiom,
  e.g. `ScheduleWakeupArgs`, `rt/agent_session.hpp:366-370`). `make_plan_signoff_request()`/
  `parse_plan_signoff_response()` marshal these through a `Custom` `ContentItem` inside the
  existing, already-proven `request_port`/`resume_workflow(ResumeWorkflow{...})` path — no new
  `WorkflowSupervisor` mechanism.
- `WorkflowCheckpointManager<StoreT>` (`include/agentengine/rt/workflow_checkpoint_manager.hpp`)
  wraps the ALREADY-EXISTING `save_workflow_checkpoint()`/`load_workflow_checkpoint()`
  (`rt/workflow_supervisor.hpp:1232-1250`) and `SessionStore` concept
  (`rt/session_store.hpp`) — no new serialization or store abstraction. `.attach(sup)` registers
  an auto-persisting `checkpoint_hook_`; `resume_or_start(store, run_id, sup, graph, bodies)`
  packages the existing "brand-new supervisor → initialize → restore_from_record" resume idiom
  `tests/test_rt_workflow_checkpoint_g2.cpp` already establishes into one call.
- `Transcript = std::vector<agentengine::Message>` (alias, not a new type — `Message` already
  carries `role`) + `transcript_of(WorkflowResult const&)` extracting `.payload` from
  `WorkflowResult.partial` in round order.

## 2. Punch list for the red-team pass

Not resolved here — this is what the red-team pass (a fresh, independently-briefed agent) should
attack before any code lands:

1. **I2/I3 boundary of `stalled`.** Does letting a moderator's own outcome influence engine-level
   run *termination* (not just routing, which is already-accepted precedent) cross from "data" into
   "authority" in a way `routes` doesn't? `routes` only *selects among edges the graph author
   already wired* — it cannot reach an effect the graph doesn't already offer. Does `stalled`
   pushing the run toward `bound_max_stalls`/`bound_max_resets` (an engine-decided outcome) have
   the same bounded-authority shape, or does it let output steer something routes-selection
   doesn't?
2. **Graph-shape correctness against the REAL `validate_workflow()` rules**, not just the shape
   `17_planner_live.cpp` hand-authors. Does the builder's auto-generated graph actually pass every
   rule the validator enforces (reachability, edge type-checking via `message_type_id_of`,
   `switch_case` case-label completeness, the request-port node's required
   input/output-type declaration)? Any rule that requires information the builder doesn't collect
   (e.g. the request-port node's message type)?
3. **`require_plan_signoff`'s claimed "no new engine primitive."** Is inserting a bare
   `request_port` node under a fixed id enough by itself, or does the builder's auto-wiring (which
   owns the manager→participant edges) need to ALSO wire an edge to/from that port for the graph
   to validate/be reachable, contradicting "the app's own `ExecutorBody` logic, not new engine
   machinery"?
4. **Checkpoint/resume interaction with the already-accepted AgentSession-history gap.**
   `docs/planning/agent-as-workflow-executor-design-draft.md` already documents that an
   agent-kind node's conversation history does NOT survive checkpoint/resume. Does
   `WorkflowCheckpointManager` make this gap easier to hit accidentally (e.g. by making
   checkpoint/resume a one-liner apps reach for casually), and if so, does it need its own
   documented warning at the point of use?
5. **`max_stalls`/`max_resets` interaction with `max_rounds`/`deadline_ms`.** Ordering of checks
   in `execute()`'s round loop — does a stall trip on the SAME round `max_rounds` would also
   trip need a defined precedence? Does `resets_used_`/`stall_streak_` need to be part of
   `RunStateRecord`/checkpoint so a resumed run doesn't reset its stall bookkeeping to zero
   (silently granting extra stall budget across a checkpoint/resume boundary)?
6. **`ExecutorOutcome::stalled` threading through the quarantine path.** `execute()` already
   quarantines a second concurrent delivery to the same agent-kind node within one round
   (`workflow_supervisor.hpp:848-882`) with a synthetic `contract`-class failure. Does a
   quarantined delivery need an explicit `stalled = false`, or could the synthetic
   `ExecuteReply` default ambiguously interact with the streak count?

## 3. Resolving the punch list

An independent red-team pass (fresh agent, no prior context, briefed only on this draft plus the
real cited source) found the §1 design was **not implementation-ready**: 3 of the 6 punch-list
items had a concrete, code-verified defect, plus 3 additional MUST-FIX problems the punch list
didn't name — most seriously, `transcript_of()` is flatly broken for the exact cyclic-graph shape
this whole feature targets. Full findings kept in the red-team agent's own report (not reproduced
here in full); resolutions below.

1. **`stalled`'s I2/I3 boundary — real problem, resolved by narrowing.** The red-team's core
   objection: `routes` (IQ5's precedent) only *selects among edges the graph author already
   wired*; it can never by itself end the run. The original `ExecutorOutcome::stalled` design let
   it directly flip the terminal `workflow_status` — an engine-level decision no single
   `request_port` answer can make today — and, worse, scoped it to *any* executor, not just the
   moderator, so a misbehaving participant could force early termination of a run the host wanted
   to continue. The closer precedent is actually `TerminationBound::token_budget`
   (ADR-070: host-monitored-only, deliberately NOT engine-enforced, specifically because the
   engine can't independently verify it) — the original design went further than ADR-070 was
   willing to go.

   **Resolution: `stalled` is host-scoped, not global.** `WorkflowSupervisor::initialize()` gains
   an optional `std::string designated_stall_reporter` (empty = stall/reset tracking disabled
   regardless of `TerminationBound` — an explicit host opt-in, matching ADR-070/ADR-071's
   Delegated Decision Seam: fails closed when unset). `execute()` only reads `.stalled` off a
   round's replies when the reporting `executor_index`'s id equals `designated_stall_reporter_`;
   every other executor's `stalled` is inert. This makes the signal structurally the same shape as
   every other engine-enforced bound: a host-authored graph naming a host-authored bound value,
   fed by ONE host-named node's self-report — narrower than "any output mints termination
   authority," closer to "the host delegates a judgment call to a node it explicitly trusted with
   it," which is the ADR-070 seam's actual shape.

2. **Case-label completeness — draft's own framing was wrong, corrected, not a design change.**
   `validate_workflow` only requires a non-empty `case_label`, never that a moderator's live
   output will match one. An unmatched label is `routing_failed` at runtime
   (`route_from()`/E5's negative control) — a pre-existing property of every `switch_case`-routed
   workflow in this codebase already, not something the builder introduces. No design change;
   the draft's §2 item 2 is corrected to say this plainly rather than posing it as open.

3. **`require_plan_signoff` unreachable and untypeable — real, and the fix removes the defect by
   making the port an ordinary node, not a special one.** A bare, unwired node fails
   `validate_workflow`'s reachability pass outright; even wired, a manager with one static `Out`
   type cannot type-check an edge to a port carrying a structurally different
   `MagenticPlanSignoffRequest` payload without a relay/adapter — which WOULD be new engine
   machinery, contradicting "no new primitive."

   **Resolution: the port is wired exactly like a participant, reusing the SAME `<In,Out>` type
   pair the rest of the graph already shares** — `switch_case` manager→port (case label =
   `port_id`), `direct` port→manager, identical to every manager→participant/participant→manager
   pair the builder already emits. This works because `MessageTypeId`/`In`/`Out` are
   compile-time/validation-time tags on `agentengine::Message`, not a constraint on
   `Message::content`'s actual shape — the SAME idiom already established for tool
   Args/Reply (`AE_JSON_SCHEMA`-typed structs riding inside an otherwise-untyped JSON/Custom
   field) applies here unchanged: `make_plan_signoff_request()`/`parse_plan_signoff_response()`
   read/write a `Custom` `ContentItem` inside the graph's ordinary `<In,Out>`-typed `Message`,
   orthogonal to the graph-level type tag. No new type, no adapter node, no variant — "no new
   engine primitive" now actually holds.

4. **Checkpoint/resume vs. AgentSession-history loss — real, and specifically dangerous for this
   feature's target shape (a moderator whose whole value is retained ledger context).**
   `WorkflowCheckpointManager::resume_or_start()` making resume a one-liner lowers the friction
   that today forces an app author to consciously reckon with the already-accepted
   history-not-restored gap.

   **Resolution: fail closed, not a footnote.** `resume_or_start()` takes an additional
   `bool acknowledge_agent_history_reset = false`. If the graph being resumed contains any
   `agent`-kind executor and this is `false`, it returns a `contract`-class error instead of
   silently resuming — the caller must consciously pass `true` (or restructure to avoid resuming
   an agent-bearing graph) to proceed, matching ADR-070's "fails closed/safe when unset"
   discipline with a real, tested engine behavior rather than a doc comment.

5. **`max_stalls`/`max_resets` vs. `max_rounds`/`deadline_ms` ordering, and missing checkpoint
   fields — both real; the second is a genuine bug in the safety mechanism itself.**
   `RunStateRecord` has no `stall_streak_`/`resets_used_` fields, and the checkpoint hook fires at
   *every* superstep boundary unconditionally — so any host that checkpoints a Magentic run at all
   (the normal, encouraged persistence pattern in this repo) silently resets stall bookkeeping to
   zero on every resume, which is an unlimited-stall-budget bypass of the exact safety valve this
   feature exists to add.

   **Resolution:** `stall_streak_`/`resets_used_` are added to `RunStateRecord` and threaded
   through `to_record()`/`restore_from_record()` alongside every other piece of round state.
   Precedence is resolved by the existing mechanical structure, not a new special case: a
   stall/reset trip is detected and ends the run while folding round N's results (the same
   fold phase that already resolves `merge_conflict`/`routing_failed`); `max_rounds`/`deadline_ms`
   are only re-checked at the top of the loop for a would-be round N+1. So **a stall/reset trip
   on round N always takes precedence over a `max_rounds`/`deadline_ms` bound that would only have
   fired starting round N+1** — the loop never reaches that check once round N's fold already
   ended the run. Documented here as the chosen, and only mechanically possible, precedence.

6. **`TerminationBound::any()` — real gap, closed; the policy question it raises is resolved by
   finding 1's narrowing.** With `stalled` now host-scoped and engine-enforced the same way
   `max_rounds` is (finding 1), letting `max_stalls`/`max_resets` alone satisfy 014 §2's mandatory-
   bound requirement is no longer "the engine's whole termination guarantee rests on unchecked
   model output" — it rests on a host-named node's self-report, which the host explicitly opted
   into trusting. `any()` is updated to include both new fields.

7. **Quarantine-path threading — confirmed clean, implementation checklist only.** Appending
   `stalled` as a trailing, default-`false` field on `ExecuteReply` keeps every existing
   brace-init compiling. Implementation must audit all `ExecuteReply` construction sites in
   `workflow_supervisor.hpp` (six, per the red-team's count) rather than trust the default
   silently — tracked as a checklist item, not a design question.

8. **NEW, MUST-FIX (not on the original punch list): `transcript_of()`/`Transcript` is broken for
   cyclic graphs — i.e., for Magentic itself.** `WorkflowResult::partial` is populated by
   `record_partial()`, which keeps **at most one entry per `executor_id`** (overwritten in place
   on each revisit), not a history. Every existing test happens to visit each node once, so this
   never surfaced before. Magentic is *defined* by cyclic revisits — by completion, `partial`
   holds only the LAST thing the manager and each participant said; every earlier round is
   silently gone. The draft's "extract `.payload` from `partial` in round order" claim has no
   "round order" left to extract.

   **Resolution: rescoped, not silently shipped broken.** This pass ships
   `latest_outputs_of(WorkflowResult const&) -> std::vector<Message>` — an honestly-named function
   that returns exactly what `partial` actually contains (the most recent message per executor),
   with a doc comment and a test proving the collapsing behavior on a cyclic graph rather than
   asserting full history. A genuine multi-visit transcript needs a per-round hook into
   `execute()`'s dispatch loop (right where `record_partial()` currently overwrites) — that is the
   SAME mechanism a follow-on ADR/#29's per-executor event multiplexing needs to build anyway, so full
   transcript reconstruction is deferred there rather than building a second, throwaway hook
   mechanism here. Item 6 of issue #28 is therefore only partially closed by ADR-149; the ADR
   states this residual explicitly rather than claiming full closure.

9. **NEW (context, not a defect): the cited reference example never exercised real type-checking.**
   `examples/17_planner_live.cpp` builds every node from the untyped `Executor` struct directly
   with an identical `"T"`/`"T"` type on every node, bypassing `TypedExecutor`'s real
   `static_assert` entirely — it is not evidence the typed builder's wiring type-checks. The new
   `examples/18_magentic_builder_live.cpp` uses two genuinely distinct declared message types
   (a task-assignment type into participants, a specialist-report type back to the manager) so the
   example actually exercises the type-checked path, not a placeholder.

10. **Accepted residuals, not blocking:** a `done_selector()` string colliding with a real
    participant id trips the existing `workflow.duplicate_executor_id` validation cheaply — no
    special-casing needed. The single shared `<In,Out>` type pair forced across manager and every
    participant is an existing property of `WorkflowBuilder::connect`'s static type-equality rule,
    not new risk this feature introduces.
