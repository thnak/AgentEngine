# Design draft: resolving OQ-20's six open questions

**Status:** Design draft, red-teamed once — **not an ADR, no code written.** Seeds a future ADR once
a milestone claims this work; per standing project-owner direction (2026-08-13): document, do not
implement. Companion docs: `batch-inference-coalescing-gap.md` (the six original open questions),
`docs/research/2026-08-13-vendor-batch-inference-apis.md` (real vendor mechanics, cited).

## The resolution, in one sentence

**Scope narrowly to N `agent`-kind workflow-fan-out nodes converging in one superstep round of one
workflow run, and answer every open question by reusing an already-proven AgentEngine mechanism —
`request_port`/`Interaction` suspend-resume (ADR-029) — instead of `StandingEffect`, because
`StandingEffect` is explicitly in-memory-only today and a real batch job can take up to 24 hours,
which must survive a process restart.** This deliberately narrows 004 §8 Q1's original framing
("rides Backgroundable/StandingEffect") for THIS specific embedding context — see §3 below for why
that's a refinement, not a contradiction, of the already-Resolved RFC text.

## Q1 — Eligibility and granularity

**First answer rejected by red-team.** A per-call flag decided inside the opaque `ExecutorBody`
closure (`std::function<result<ExecutorOutcome>(Message const&, EffectContext&)>`,
`rt/workflow_supervisor.hpp:156-157`) is invisible at the exact point `execute()` needs to know about
it — the round's `port_deliveries`/`exec_deliveries` split (`workflow_supervisor.hpp:727-734`) happens
using only `graph_.executors[d.executor_index].kind`, a STATIC graph property, BEFORE any body runs.
Discovering eligibility by running the body is dispatching before the decision that needs to happen
before dispatch — backwards.

**Corrected: eligibility is a static, graph-declared property**, alongside `Executor::kind` (the same
place OQ-19's own `capability_ceiling` question already proposes adding a field) — decided at
workflow-build time by whoever authors the graph, not discovered per-call at runtime. Still checked
against the backend's real `ChatClientCapabilities::batch` (`core/chat_client.hpp:52`) at bind time;
still fails closed if the declared-eligible node's bound `ChatClientT` doesn't actually support batch
— matching 004 §8 Q1's "explicit policy choice... not automatic."

## Q2 — Coordinator ownership and flush trigger

**Holds up under red-team, unchanged.** `execute()`'s round loop already gathers every pending
delivery into `exec_deliveries` synchronously, BEFORE any concurrent dispatch begins
(`workflow_supervisor.hpp:725-753`, "decision 5, first half: ISSUE every job before awaiting any").
Splitting that already-gathered, already-single-threaded list into "ordinary" and "batch-eligible"
groups at that exact point needs no new coordinator, no new shared mutable accumulator, and therefore
none of the concurrency-hazard class `agent-as-workflow-executor-design-draft.md` §2 found real for a
DIFFERENT (naive `AgentSession&`-capture) design. Scope stays: one flush per superstep round of one
workflow run — not cross-run, not cross-session.

**Real gap found and must be closed**: the `broke=true` sibling-failure branch
(`workflow_supervisor.hpp:790-799`) only drains `port_deliveries` into `state_.unopened_ports` when a
round breaks mid-flight — the exact fix M6 Phase E built for ports specifically
(`docs/planning/milestone-6-multi-agent-orchestration-breakdown.md`'s own port-loss fix). A
batch-eligible group pulled out alongside `port_deliveries` needs the IDENTICAL accounting, or it
silently reintroduces the bug class M6 already closed for ports, just for a new kind of pending item.

## Q3 — Result fan-out shape

**Simpler than either draft proposed — reuse `OpenPort` wholesale, no new message type.** Two rounds
of refinement:

1. First correction (this doc's own initial pass): batch-eligible deliveries suspend the round the
   same way `request_port` does — `Interaction` (`core/interaction.hpp:35-46`) IS already durable
   (part of what `to_record()`/`restore_from_record()` checkpoint, proven by
   `tests/test_rt_workflow_checkpoint_g2.cpp`'s 20/20 result), unlike `StandingEffect`, which
   `core/standing_effect.hpp`'s own file-top comment states directly is "in-memory-only, never
   persisted" — ADR-037 found and removed a dead persistence tag on it for exactly this reason. A
   24-hour-capable wait needs the durable mechanism.
2. **Second correction, from red-team**: a sketched new `ResolveBatch` message resolving N pending
   items in one call has no precedent and isn't needed. `WorkflowSupervisor::resume_workflow()`
   already resolves ports ONE AT A TIME and only re-enters `execute()` once every `OpenPort.resolved`
   is true (`workflow_supervisor.hpp:559-567`) — that already IS N-way fan-out resolution, achieved by
   N separate calls. **Batch-eligible pending deliveries become literal `OpenPort`s**, and whoever
   polls the vendor batch job calls the EXISTING `resume_workflow()` once per `custom_id`, resolving
   them the same way N independent human approvals would resolve N independent `request_port`s. Zero
   new resolution-side code needed.

**Real gap this creates, not yet closed**: `OpenPort`/`Interaction` has no field for a vendor batch
job id or `custom_id`. Two options, neither decided here: pack it into `interaction_id` itself (a
composed string, matching Q4's own naming convention below), or a new sidecar record keyed by
`interaction_id`. A future ADR must pick one explicitly — "already solved by suspension" (this draft's
own first-pass framing) was too optimistic; the ENCODING is a real, unaddressed piece.

## Q4 — `custom_id` and attribution

**Holds up, unchanged.** `custom_id = run_id + ":" + round_number + ":" + executor_id` — mirrors
`tool_pipeline.hpp`'s existing `IdempotencyKey::to_string()` convention
(`{run_id}:{turn_index}:{call_index}:{argument_digest}`), and `Executor::id` is already guaranteed
unique within one workflow by `validate_workflow()`. No new naming scheme invented.

**Cross-tenant metadata question, re-scoped by a red-team finding that makes it MORE urgent, not
less**: this draft's own §5 originally called the risk "currently inert" because `contexts_`
(`std::vector<EffectContext>` parallel to `graph_.executors`) is never populated with genuinely
distinct principals today. But closing Q3 above means batch resolution rides `resume_workflow()` —
and red-team found **`resume_workflow()` (`workflow_supervisor.hpp:540-570`) has NO caller/admission
check at all**, unlike `AgentSession::resolve_interaction()`, which DOES check the resolving caller
against the pending interaction's owner via `principal_admitted_for()` (ADR-029's own finding #6).
This is a real, PRE-EXISTING gap in `WorkflowSupervisor`'s HITL resume path — any caller who learns an
`interaction_id` can resolve it today, batch or not — not something batch introduces, but something
batch would inherit and make higher-stakes (a vendor-visible batch job aggregating multiple
principals' prompts, resolved through a resume path with no ownership check at all). **Must be fixed
— add a caller/principal field to `ResumeWorkflow` and a real admission check — before this design
proceeds, independent of whether batch itself ships.** Worth its own, separate small fix regardless.

## Q5 — Polling ownership and durability

**Resolved as a consequence of Q3, not separately.** Once batch-pending deliveries are `OpenPort`s,
they're already covered by the EXISTING checkpoint/resume machinery (modulo Q3's own remaining
encoding gap) — no new durability primitive needed. Polling itself is explicitly NOT AgentEngine's own
runtime's job: whoever resolves the port (the embedding host) is also whoever polls the vendor batch
job on whatever cadence it likes, then calls `resume_workflow()` — the same "host supplies the
transport/timing, AgentEngine supplies the durable suspend point" shape ADR-039 already established
for inbound protocol handling. No new timer/poll-loop primitive inside AgentEngine's own runtime.

## Q6 — Cost-vs-latency visibility

**Real gap found — the natural-seeming reuse doesn't exist at this layer.** `run_event_kind::warning`
(`rt/agent_session.hpp:440-449`, also used by `core/model_call_gateway.hpp:55-57`) is real, but it is
**`AgentSession`-only**. `WorkflowSupervisor` has zero occurrences of `run_event_kind`/
`emit_run_event` — its only event surface is the narrower `WorkflowLiveEvent`/`live_view_producer_`
stream (014 §7's live-view bullet, superstep-boundary-scoped). Surfacing "this round is now waiting on
a vendor batch job, expect up to Nh" needs either extending `WorkflowLiveEvent` with a new variant, or
building a `WorkflowSupervisor`-level warning surface analogous to `AgentSession`'s — real, separate,
not-yet-scoped work, not something to assume comes free.

## What's left for a real ADR (punch list)

1. Add `Executor::batch_eligible` (or fold into whatever OQ-19's own `capability_ceiling` field
   addition lands as) — static, graph-declared, checked against `ChatClientCapabilities::batch` at
   bind time.
2. Extend `execute()`'s `broke=true` sibling-failure accounting to a third (batch-eligible) delivery
   group, matching the existing `unopened_ports` treatment.
3. Decide and implement the vendor-job-id/`custom_id` encoding on `Interaction`/`OpenPort` (packed
   string vs. sidecar record) — the one piece Q3's "reuse OpenPort wholesale" simplification still
   leaves genuinely open.
4. **Fix `WorkflowSupervisor::resume_workflow()`'s missing caller/admission check** — a real,
   independent, separately-landable prerequisite (mirrors `AgentSession::resolve_interaction()`'s
   existing `principal_admitted_for()` check, ADR-029), needed before batch resolution (or arguably
   before ANY multi-principal use of `request_port`) can be considered safe.
5. Decide the `WorkflowSupervisor`-level visibility surface for Q6 (extend `WorkflowLiveEvent`, or
   build a new warning-event mechanism) — not free, not yet designed.

Item 4 is the one finding here with value independent of whether batch coalescing ever ships — it's a
real gap in the already-shipped HITL resume path, worth flagging on its own regardless of this
design's fate.
