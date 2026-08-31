# Design draft: forwarding ADR-152's multiplexed events across nested `WorkflowSupervisor` runs
# (ADR-157 §4's last remaining named residual — issue #42 item 3)

Status: **implemented and proven.** Red-teamed once (fresh agent, zero prior context) before any
code was written, per CLAUDE.md's `design -> red-team -> prove -> judge` discipline and the user's
own explicit preference for this pipeline over a quick patch on architectural work (see memory
`feedback_design_over_patch`). The red team found two real MUST-FIX problems in §4 as originally
drafted (bind-time propagation) — both closed by moving the mechanism to dispatch-time wiring
instead (§4 below is the REVISED, accepted mechanism, not the one first proposed). See §3b for the
red-team's own findings and the resolution.

## 1. The question

ADR-152 (issue #29) gave `WorkflowSupervisor::enable_event_stream()` a live, per-node multiplexed
bucket (`agent_turn_event`/`moderator_stream_delta`, `workflow/multiplex_sink.hpp`) — every
concurrently-dispatched node's own real per-token output surfaces the instant it happens. ADR-157
(issues #33/#38) then gave `executor_kind::sub_workflow` a real runtime bridge, but explicitly did
NOT forward an inner graph's own multiplexed events outward — only the structural
`request_port_opened`/`_resolved` signal composes across nesting. A consumer watching the OUTER
run's stream is blind to everything a nested inner run's own agent/moderator nodes produce, no
matter how many levels deep.

Scope was already settled earlier this session via explicit user choice (AskUserQuestion):
**multiplexed bucket only** (never the structural bucket), tagged with the **full path** of
sub_workflow executor_ids from the outer to the originating node (not just the immediate parent).

## 2. Why the multiplexed bucket is the one that can compose for free, and the structural bucket can't

Traced directly against `workflow_event.hpp`/`workflow_supervisor.hpp`:

- The **structural** bucket rides an ordinary `agentengine::stream_producer<WorkflowEvent>`
  (`workflow_event_producer_`) — a single-writer, single-consumer channel. Only `execute()`'s own
  coroutine ever pushes into it, and `enable_event_stream()` hands the paired consumer out exactly
  once. There is no way to "add a second writer" to an already-live channel without either a new
  fan-in primitive (real new machinery) or a relay thread draining one channel and re-pushing into
  another (extra latency, extra thread-lifetime management, and a genuine batching-at-the-relay
  hazard — see §3, Approach 3 below).
- The **multiplexed** bucket rides `multiplex_sink<WorkflowEvent>` (`multiplex_sink_`) — a plain,
  never-null, always-constructed-up-front `shared_ptr` member that ANY number of producers can
  `push()` into concurrently (that is its entire reason for existing — ADR-152 §2's own red-team
  finding). Nothing about it is single-writer. **If an inner `WorkflowSupervisor`'s own
  `multiplex_sink_` were literally the SAME shared object as the outer's, forwarding needs no relay
  at all — the inner's own already-existing dispatch-time wiring (`run_executor_job()`'s
  `ctx.agent_turn_sink`/`ctx.moderator_delta_sink` closures, unchanged) would push directly into the
  queue the outer's own consumer already polls.**

This asymmetry is the real reason "multiplexed bucket only" is not just an arbitrarily narrower
scope — it is the ONLY bucket that can be extended to nesting via pure object-identity sharing, with
zero new concurrency machinery. The structural bucket staying unforwarded is a direct structural
consequence of `stream_producer<T>`'s own single-writer contract, not an arbitrary line drawn for
this pass.

## 3. Approaches considered

**Approach 1 (proposed): shared-sink adoption, path tagged at push time.** When an outer's
multiplexed stream is enabled (or a sub_workflow is bound to an already-enabled outer, in either
order), the inner's own `multiplex_sink_` member is REPLACED with the outer's (same `shared_ptr`
instance) — recursively, down through however many levels are already bound. Every
`AgentTurn`/`ModeratorDelta` payload additionally carries the pushing supervisor's own accumulated
path prefix (see §4), so a forwarded event's origin is unambiguous even though it lands in a shared
queue. Zero relay machinery; genuinely live (a nested node's delta is visible to the outer's
consumer the instant `push()` returns, same as a non-nested node today); reuses
`run_executor_job()`'s existing wiring completely unchanged.

**Approach 2 (rejected): a dedicated background pump/relay thread** that periodically drains an
inner's own independent sink and re-pushes into the outer's. Rejected: needs new thread-lifetime
machinery (who starts/stops it, at what granularity, across arbitrary nesting depth) for a job
Approach 1 gets for free by construction — the same "reserved worker"-style over-engineering this
session's own ThreadPool budget draft already rejected once for an analogous reason.

**Approach 3 (rejected): batch-drain the inner's sink inside `run_sub_workflow_job()` after
`drive()` returns**, appending everything at once into the outer's sink. Rejected on direct
precedent: `drive()` runs the ENTIRE inner run (potentially many rounds) synchronously in one shot,
so draining only after it returns would surface every nested event only once the whole inner run
finishes or suspends — exactly the "buffer privately, drain between yield points" fallback ADR-152
§2's own red team already found and rejected as "not a fix," since it silently degrades to
round-boundary batching, the precise hazard issue #29 was filed to eliminate. Same hazard, same
verdict, one nesting level deeper.

**Decision: ship Approach 1's central idea (shared object identity through `multiplex_sink<T>`), but
wire it at DISPATCH time, not at bind/enable time** — see §3b.

## 3b. Red-team pass: two MUST-FIX findings, both closed by moving to dispatch-time wiring

An independent red-team pass (fresh agent, zero prior context) traced the original bind-time/
enable-time `adopt_multiplex_sink()` design (below, struck through) against the REAL code and found
two real problems, not hypothetical ones:

1. **MUST-FIX — wrong `path` values under the exact construction order this codebase's own real
   nested trees use.** `bind_sub_workflow()` is a single-level operation with no cascade into
   already-bound descendants. Every real nested tree in this repo (including S8/S11) is built
   BOTTOM-UP: innermost first, then bound upward one level at a time. Tracing that order against a
   bind-time-computed `event_path_prefix_`: the innermost level's prefix gets baked in while its
   own ancestors are still unattached (still empty), and NOTHING ever fixes it up afterward once
   more binding happens above it. This is not hypothetical — it is the exact scenario the draft's
   own §6 proof #2 (3-level forwarding) was going to exercise, and it would have shipped broken for
   that scenario specifically.
2. **MUST-FIX — a genuine, NEW cross-thread data race.** `multiplex_sink_` is documented as "never
   null (constructed once, up front)" and, before this feature, is NEVER reassigned anywhere.
   `adopt_multiplex_sink()` would have been the first thing to ever mutate it post-construction, and
   cross-instance (an outer writing into an inner's own members). `enable_event_stream()`'s own doc
   comment already blesses calling it more than once ("a second call replaces the producer"), and
   nothing prevents `bind_sub_workflow()`/`enable_event_stream()` from running on one thread while a
   PREVIOUS run against the same `inner` is still mid-flight on another (I1's `run_mutex_` guards
   the three RUN entry points, not these two configuration calls). A bind-time/enable-time cascade
   would plainly write into a live object's members while its own dispatch loop concurrently reads
   them — a torn-`shared_ptr`/UB hazard that structurally cannot happen in the shipped code today
   (nothing reassigns `multiplex_sink_` at all pre-this-feature), reachable through a call pattern
   the class's own existing docs present as ordinary, not exotic.

The red team's own suggested resolution — compute and wire the sink/path at DISPATCH time instead,
immediately before the one `drive()` call that matters, on the SAME thread, restoring `inner`'s
prior state immediately after — closes BOTH findings at once: bind/enable order stops mattering
entirely (nothing is precomputed at bind time), and the write always immediately precedes, on the
calling thread, the one call whose own worker threads are the only readers — and those workers are
always spawned AND joined strictly inside that one call's own synchronous extent, never outside the
window the wiring covers. Accepted as the shipped mechanism (§4).

## 4. The accepted mechanism (revised, post-red-team)

- **`event_path_prefix_`** (new private member, `std::vector<std::string>`, default empty) — unlike
  `nesting_depth_`, this is NEVER set at `bind_sub_workflow()` time. It is wired TRANSIENTLY,
  immediately before an instance is driven as a nested inner, and restored immediately after, by
  `ScopedForwardedEventSink` (below) — computed fresh at dispatch time by whichever caller is about
  to drive it. A root/standalone instance's prefix stays empty forever (never wired by anything),
  so a non-nested run's own events are byte-for-byte unaffected (see §5).
- **`AgentTurn`/`ModeratorDelta` gain a new field: `std::vector<std::string> path;`** (additive,
  default empty) — populated from the dispatching supervisor's own CURRENT `event_path_prefix_`
  when `run_executor_job()`'s closures build the `WorkflowEvent` (passed in as an explicit
  parameter, since that function is `static` and has no `this` to read a member from directly).
  `executor_id` keeps its EXISTING meaning unchanged (the real originating node's own local id
  within its own graph) — `path` is a SEPARATE field carrying the lineage of sub_workflow
  executor_ids from the outer down to (but not including) that node. Deliberately NOT a single
  delimited string glued onto `executor_id`: a real node id could itself contain any delimiter
  character chosen, and a consumer should never have to unescape/parse a composite string to
  recover the real local id.
- **`ScopedForwardedEventSink`** (new private nested RAII class) — constructed with `(inner, sink,
  path_prefix)`: saves `inner`'s current `multiplex_sink_`/`workflow_event_stream_enabled_`/
  `event_path_prefix_`; if `sink` is non-null, overwrites all three (`multiplex_sink_ = sink;
  workflow_event_stream_enabled_ = true; event_path_prefix_ = path_prefix;`); on destruction,
  restores every saved value unconditionally. A null `sink` (the forwarding supervisor's own stream
  isn't enabled) makes construction a pure no-op — `inner`'s own prior state is saved and written
  back unchanged, never disturbed.
- **Two call sites wrap their own nested `drive()` call in this guard**, each computing its own
  `sink`/`path_prefix` freshly from ITS OWN current `this->multiplex_sink_`/`this->
  event_path_prefix_` (+ the sub_workflow node's own local id) at the moment of the call:
  - `execute()`'s sub_workflow dispatch loop, around `run_sub_workflow_job()`'s own
    `drive(inner->run_workflow(...))` call (the job now takes `sink`/`path_prefix` as two
    additional by-value parameters, mirroring how `run_executor_job()` already takes `sink`/
    `executor_id`/`round`/`attempt`).
  - `resume_workflow()`'s pending-sub-workflow branch, around its own direct
    `drive(inner->resume_workflow(...))` call — the SAME call shape, just re-entered later for an
    already-suspended nested run.
  Multi-level nesting cascades automatically and correctly with ZERO extra machinery: when a
  freshly-wired `inner` (now temporarily holding the OUTER's shared sink and the correct
  accumulated path) itself dispatches into ITS OWN sub_workflow children, its own dispatch code
  reads `this->multiplex_sink_`/`this->event_path_prefix_` — which the guard one level up just set
  — and passes THOSE down, one level deeper, through the identical mechanism. No recursive method,
  no cascade code, no order dependency: every level just reads its own current state at the moment
  it happens to dispatch.
- Nothing about `enable_live_view()` or the structural producer (`workflow_event_producer_`)
  changes at all — the structural bucket stays exactly as unforwarded as ADR-157 §4 already
  disclosed, by construction (this mechanism never touches it), not by a convention a future change
  could accidentally violate.

### Rejected (pre-red-team) first draft, for the record

~~`adopt_multiplex_sink(shared_ptr<multiplex_sink<WorkflowEvent>> sink)` (private method):
`multiplex_sink_ = sink; workflow_event_stream_enabled_ = true;` then recurses over
`sub_workflows_`, called from both `enable_event_stream()` (cascading into already-bound
descendants) and `bind_sub_workflow()` (propagating into a newly-bound inner and whatever it
already has bound)~~ — superseded by §3b/§4 above; kept here only so the record of what was
red-teamed and rejected is not silently lost.

## 5. What this design does not claim

- Does not forward the structural bucket (request/lifecycle/routing events) beyond what already
  composes today (`request_port_opened`/`_resolved`) — §2 explains why that bucket structurally
  cannot share this same mechanism.
- Does not support an inner instance's OWN independently-configured `enable_event_stream()` stream
  surviving the DURATION of a forwarding dispatch. Narrower than the pre-red-team draft's version of
  this limitation (which would have permanently clobbered it from the moment of binding onward):
  under the shipped mechanism, `ScopedForwardedEventSink` temporarily overrides `inner`'s sink only
  while `inner` is actively being driven as a nested dispatch, and restores it immediately
  afterward — so an inner's own independent consumer misses only events pushed during that specific
  window, and resumes seeing inner's own future events (if `inner` is later driven standalone
  again) once the window closes. Still unsupported as a combination, still named explicitly.
- Widens who can observe an inner's own intermediate stream (tool calls, reasoning, in-progress
  deltas — `AgentTurn.inner` wraps the real `RunEvent` UNCHANGED) to whoever holds the OUTER's
  stream, not just a caller with a direct relationship to `inner`. This does not cross I2/I3 as
  those invariants are defined (purely observational; never fed back into routing/policy/
  authorization, structurally, regardless of nesting) — `bind_sub_workflow()`'s own existing "zero
  implicit capability flow" comment already establishes `inner` may hold broader authority than the
  outer node's own capability ceiling implies, and this makes inner's own intermediate output
  visible further than before. Named here explicitly per this project's own stated preference for
  disclosing residuals rather than letting them go unmentioned (CLAUDE.md's ADR-070 discipline).
- Does not persist `event_path_prefix_` through checkpoint/resume at all — it is never persisted
  even during an active run (transient, dispatch-scoped only), so there is nothing to restore after
  a restart; a resumed nested run gets it re-derived fresh on its very next dispatch, same as any
  other run.
- Does not change `round`'s meaning for a forwarded event — it stays whichever level's OWN internal
  round counter was live when that level pushed the event (the pushing supervisor's own `rounds_`,
  unchanged from today), not the outer's round. A consumer distinguishing forwarded events from
  different levels/attempts should key on `{path, executor_id, round, attempt}`, not
  `{executor_id, round, attempt}` alone, once nesting is in play.
- Does not change the ATTEMPT DISCRIMINATOR's own existing contract (`workflow_event.hpp`'s file
  banner) — `attempt` still counts retries at whichever level dispatched the node; nesting adds a
  new independent axis (`path`), not a replacement for the existing one.

## 6. Required proof (all executed)

Per this session's own established discipline (ADR-157 Pass 2's `fan_in`-merge false-negative catch,
and the ThreadPool budget work's own background-sampler false-negative catch — both real, both only
caught by adversarially verifying a claim rather than trusting a clean first pass):

1. **Positive, single level** (W10, `test_rt_workflow_event_stream.cpp`): a nested run where the
   INNER graph's own node pushes a `moderator_stream_delta`; the OUTER's `enable_event_stream()`
   consumer observes it, `executor_id == "leaf"` (unchanged meaning), `path == ["sub"]`,
   `text_delta` intact.
2. **Positive, multi-level** (W11, 3 levels): the innermost level's own event carries the FULL path
   (`["sub", "wrap"]`), not just the immediate parent's.
3. **Liveness, not just presence** (W12): a deliberately-blocked inner node (parked on a
   `condition_variable`, bounded 10s wait) driven on a background `std::thread`; the main thread
   polls `stream.next()` and observes the nested `moderator_stream_delta` while `run_finished` is
   STILL `false` (structurally guaranteed, not a timing race — the flag can only become true after
   this same test explicitly releases the block, which has not happened yet at the assertion point).
   This directly rules out Approach 3's batching hazard having shipped by accident.
4. **Order independence** (W13a/b): bind-then-enable and enable-then-bind produce identical
   forwarding (same path, same content) for the same tree shape — confirmed trivial under the
   dispatch-time mechanism, since nothing is precomputed at bind/enable time at all.
5. **Negative/mutation**: temporarily short-circuited `ScopedForwardedEventSink`'s constructor
   (`if (false && sink)`) — rebuilt, reran: W10/W11/W13a/W13b's forwarding-dependent checks all
   FAILED as expected (8 failures), and W12's presence check failed too (its "still in progress"
   check passed vacuously, as expected, since nothing was ever observed to make it not-vacuous —
   noted, not a flaw in the proof: the presence check failing is what correctly signals the mutation
   broke forwarding). Reverted; rebuilt; reran — clean, 53/53 checks passing again.
6. **Non-regression**: `test_rt_workflow_event_stream.cpp` W1-W9 and `test_rt_workflow_sub_workflow.
   cpp` S1-S13 all still pass unchanged; full repo-wide `ctest` run to confirm (see ADR-157 addendum
   for the exact count).

## 7. Files changed

- `include/agentengine/workflow/workflow_event.hpp` — `AgentTurn::path`/`ModeratorDelta::path`.
- `include/agentengine/rt/workflow_supervisor.hpp` — `event_path_prefix_`,
  `ScopedForwardedEventSink`, `run_executor_job()`'s new `path_prefix` parameter, `run_sub_workflow_
  job()`'s new `sink`/`path_prefix` parameters and guard-wrapped `drive()` call, `execute()`'s two
  dispatch sites (ordinary + sub_workflow) passing the new parameters, `resume_workflow()`'s
  pending-sub-workflow branch's guard-wrapped `drive()` call.
- `tests/test_rt_workflow_event_stream.cpp` — W10-W13, plus `<atomic>`/`<condition_variable>`/
  `<mutex>`/`<thread>` includes.
- `decisions/ADR-157-sub-workflow-nested-request-port.md` — addendum closing the last of its four
  named residuals, matching the pattern already used twice this session for the other three.

## Status

**Implemented and proven.** Design drafted, independently red-teamed once (fresh agent, zero prior
context) before any code was written — two MUST-FIX findings, both closed by moving the mechanism
from bind-time propagation to dispatch-time wiring. Implemented, all six required proofs (§6)
executed including a real mutation/negative proof. 53/53 checks passing in
`test_rt_workflow_event_stream.cpp`. Full repo-wide regression pending final confirmation before the
ADR-157 addendum is written.
