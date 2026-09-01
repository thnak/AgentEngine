# ADR-160 — 006 §8 G4: the parallel tool-call batch scheduler

**Status:** Proposed — design corrected by one independent red-team pass (§3), implemented and
proven (real code + tests, §7). Awaiting project-owner judgment. Per this project's
`design → red-team → prove → judge` discipline (CLAUDE.md), this file now records all three of
`design`, `red-team`, and `prove` — `judge` (project-owner sign-off) is the remaining step. The
red-team's own verdict (issue #43 discussion) was that the original design's high-level framing
(§1) was sound but its concrete mechanics reopened three already-fixed hazard classes (ADR-060,
ADR-028) through fields and call sites the first draft never inventoried — §5 is the corrected
design; §3 records exactly what was wrong and how each defect was closed; §7 is the real,
executed proof that §5's implementation works, including real concurrent execution positively
observed, not merely inferred.

**Filed as GitHub issue #43** (same repository, own title/body still say "ADR-159" — issues aren't
renumbered the way files are, same convention ADR-158's own note already established), restating
this gap and this draft's design sketch for independent tracking.

**NOTE:** drafted and implemented locally as "ADR-159", renumbered to ADR-160 after `git fetch`
found `origin/main` had, in the meantime, independently merged a completely unrelated ADR-159
(`decisions/ADR-159-workflow-mid-run-cancellation.md`, GitHub issue #37, `WorkflowSupervisor`
cancellation) — the same multi-session numbering collision this table's ADR-149/150/151 and
ADR-157/158 rows already document happening repeatedly. All in-file, in-code, and in-comment
"ADR-159" references below were renumbered to "ADR-160" before this file was committed; nothing in
the colliding ADR-159 file itself was touched.

**Relates to:** `006-Tool-and-Function-Plane.md` §5 (Concurrency) and §8 G4 (the gate this ADR
exists to satisfy). `decisions/ADR-158-tool-concurrency-exclusivity-policy.md` (`ExclusivityGroup
<Name>` — a real, tested, but currently-inert declaration this scheduler is the first thing to ever
read at dispatch time). `decisions/ADR-060-tool-call-progress-reporting.md` (the by-value
`EffectContext` precedent §2 below reuses). `decisions/ADR-037-...` (the Quark-removal migration
that produced `agentengine::rt::task<T>`, `rt::AsyncMutex`, `rt::ThreadPool`). `include/agentengine/
core/tool_pipeline.hpp` (`invoke_tool()`). `include/agentengine/rt/agent_session.hpp` (the three
sequential batch-dispatch loops this ADR must replace, per §3 MUST-FIX 4's corrected inventory).
`007-Capability-and-Trust-Model.md` §3
(`Background<max_concurrent>` — a disjoint, already-shipped mechanism this ADR does not touch,
confirmed disjoint by ADR-158 §4).

## 0. Where this picks up

ADR-158 shipped `ExclusivityGroup<Name>` as a declared policy tag with **zero pipeline logic** —
by its own §8, explicitly deferring "the thing that would ever read `ToolDescriptor::
exclusivity_group` and do something with it," i.e. 006 §8 G4's scheduler itself. This ADR is that
follow-on design pass. It was requested directly by the project owner, not sourced from external
research this time — the prior MAF comparative review (ADR-158 §0) already established that MAF
itself has not solved this either (Python's `concurrency_group` PR #7881 remains open and
deliberately incomplete), so there is no reference implementation to adapt from; this design must
be derived from AgentEngine's own source.

## 1. The question, stated so it has a wrong answer

**How does a batch of model-emitted tool calls, some or all declared `Parallelizable` or
`ExclusivityGroup<Name>`, actually execute concurrently — while (a) preserving 006 §5's determinism
requirement (result append order is the model's emitted order, regardless of completion order),
(b) preserving I1 ("one session, one executor") in the same sense `Background<max_concurrent>`
already does — the session's own admission/authorization sequencing stays single-threaded; only the
already-authorized `invoke()` body itself runs concurrently, and (c) not reintroducing, at a
different layer, the exact per-call state-sharing race `ADR-060` already found and fixed once for
`Backgroundable`?**

## 2. Grounding facts, verified directly against the current source

- **`invoke_tool()` (`core/tool_pipeline.hpp`) is a plain, synchronous, blocking function** —
  `[[nodiscard]] inline ToolResult invoke_tool(ToolTable const&, CapabilitySet const&,
  ToolCallRequest const&, EffectContext&, ApprovalDecider const&, ToolInvocationAudit* = nullptr,
  PolicyDecider const& = {})` — not a coroutine, not `task<ToolResult>`, despite 006 §1's own
  declaration example showing a tool's `invoke()` returning `ae::task<result<Reply>>`. Whatever
  async shape an individual tool's `invoke()` has today, the pipeline wrapper around it blocks the
  calling thread until it resolves. "Concurrent" execution of two calls to `invoke_tool()`
  sequentially on one thread does nothing — real concurrency requires either running it on separate
  OS threads, or restructuring it to be genuinely awaitable end to end (out of scope here — a
  strictly larger change than this ADR proposes; see §6).
- **`invoke_tool()` takes `EffectContext& ctx` by reference and mutates it for the duration of the
  call**, per its own comment: *"`ctx` is filled in with this call's per-invocation
  `bound_capabilities` (step 7) for the duration of `invoke`, and cleared again before returning
  regardless of outcome (step 10)"* — this is exactly what makes G3 ("a capability handle from call
  *n* is unusable in call *n+1*") true, under the standing assumption that calls are strictly
  sequential. Running two `invoke_tool()` calls concurrently against **the same shared `ctx`**
  would race on `bound_capabilities`, `report_progress`, and `sandbox_fs` — not a hypothetical: this
  exact race, for the exact same fields, was already found and fixed once, for `Backgroundable`
  (next bullet).
- **`AgentSession`'s own batch-dispatch code already reuses one shared `effect_context_` member
  across an entire batch, sequentially** — both `resolve_interaction()`'s post-approval loop and
  `run_rounds()`'s own dispatch loop (`rt/agent_session.hpp`) run a plain `for` loop over
  `pending_calls`/`calls`, each iteration writing `effect_context_.report_progress = [...]` before
  calling `invoke_tool(..., effect_context_, ...)` and resetting it to a no-op after. This is the
  exact code this ADR's scheduler must change for whichever calls in a batch are eligible to run
  concurrently — and the exact pattern that cannot simply be run in parallel as written.
  - **The identical hazard was already found and fixed once, for a different mechanism.**
    `start_background_task()`'s own comment (`tool_pipeline.hpp`) documents finding a real,
    reachable data race: a live `report_progress` closure (captured `[this, call_id]` into the
    originating session) surviving into a detached call's own copy of `EffectContext` would let a
    backgrounded tool's *ordinary* call to `report_progress()` reach back into the original
    session's `emit_run_event()` from a foreign thread — "a genuine, unlocked data race... reachable
    with no tool misuse at all, just the intended use of the very feature." The fix: `ctx` is taken
    **by value** (a real, independent copy), and `report_progress`/`sandbox_fs` are reset on that
    copy unconditionally before `invoke()` ever runs. **This ADR's scheduler needs the identical
    fix, generalized**: each concurrently-dispatched call in a batch needs its own `EffectContext`
    copy, with its own `report_progress` rebound to its own `call_id` (not reset to a no-op — unlike
    `Backgroundable`, an ordinary parallel-batch call's progress deltas are still meant to reach the
    run's event stream, 006 §6a; they must simply not collide with a sibling call's own delta
    stream) and its own bound-capabilities lifetime scoped to that one call only.
- **`rt::ThreadPool` exists already**, built (per its own file banner) as "a blocking utility — the
  thing driving async model-call continuations — not a hot-path scheduler," with `submit(task<void>
  job) -> std::future<JobOutcome>`. It is a candidate primitive for fanning out concurrent `invoke()`
  calls, but its stated purpose to date is model-call continuations, not tool dispatch — reusing it
  here is a design choice this ADR must make explicitly (§5), not inherit by default. (§3
  MUST-FIX/SHOULD-FIX 7's correction: this ADR ultimately does NOT reuse `rt::ThreadPool`, for
  exactly the reason named here plus a further one found by the red-team — see §5.)
  `Backgroundable`'s own detachment (`start_background_task()`) uses a raw, unmanaged, detached
  `std::thread` instead of `ThreadPool` — so this codebase already has **two different** ad hoc
  concurrency mechanisms for two different "run this off the session's own sequencing" needs; a
  third, bespoke one for G4 would need a stated reason, not just added by default.
- **I1's steady-state statement — "a run's steady state costs one `rt::AsyncMutex`-guarded in-flight
  call, not a thread"** — is not violated by `Backgroundable`'s own detached thread, because (per
  006 §6b) "the session actor still processes exactly one thing at a time (I1); what is detached is
  the *model's wait*, not a second executor inside the session." The same framing is available to
  G4: the session's own admission sequence (pipeline steps 1–7: resolve, validate, taint, authorize,
  approve, bind) stays strictly single-threaded and ordered exactly as today; only step 8 (`invoke`)
  fans out, across calls the batch has *already* admitted one at a time. What must NOT happen is
  admission itself (steps 4/5 — authorize/approve, which may consult a stateful `PolicyDecider` or
  suspend for human approval) running concurrently — that stays sequential, per call, in the
  request's original order, exactly as it does today.
- **006 §5's determinism requirement is unconditional and pre-existing**: "Parallel batches still
  serialize their result append in the model's emitted order, so the history is deterministic
  regardless of completion order — a precondition for I5 replay." This ADR must satisfy it, not
  merely preserve it as an aspiration — 006 §8 G4's own literal wording is a *measured* gate:
  "parallel batches produce deterministic history order across 10⁴ randomized completions."
- **No existing OpenQuestions.md entry or ADR names this scheduler** — confirmed by direct search;
  next free ADR number is 159 (highest existing is 158, this file's own predecessor).

## 3. Red-team correction (independent adversarial pass, GitHub issue #43)

An independent reviewer, given no context beyond this repo's own invariants/conventions and this
file, was instructed to attack §4 as it originally stood, verifying every load-bearing claim
directly against source rather than trusting this draft's own quotes. It found six MUST-FIX and
three SHOULD-FIX defects. Two of the MUST-FIX claims were independently re-verified line-by-line
before acceptance (`run_event_seq_by_run_`'s unlocked mutation at `rt/agent_session.hpp:1664`, and
`ToolDescriptor::captures_session_state` at `core/tool_pipeline.hpp:98/184/754` going unchecked by
the original partitioning scheme) — both held up exactly as reported. Its overall verdict: §1's
framing (sequential admission, only `invoke()` fans out, results append in emitted order) remains
sound, but §4 as originally drafted reopened three already-fixed hazard classes through fields and
call sites it never inventoried. All nine findings are resolved below; §5 is the corrected design.

- **MUST-FIX 1 — batch partitioning ignored `ToolDescriptor::captures_session_state`.** The
  original scheme partitioned purely on `Parallelizable`/`ExclusivityGroup<Name>`. `background_task()`
  already refuses to background a `captures_session_state` tool structurally (ADR-028) precisely
  because such a descriptor's `invoke` closure holds a reference into session-scoped state with no
  synchronization against `fork_from()`/teardown. Nothing stopped a tool from declaring
  `Parallelizable` *and* `captures_session_state` — the real, shipped `ScheduleWakeupTool`
  (`rt/agent_session.hpp:2190-2211`) is exactly such a descriptor. **Resolved:** partitioning checks
  `captures_session_state` *first*, before either concurrency tag — true forces the call into its
  own sequential singleton class unconditionally, regardless of what it also declares. This is a
  *runtime* guard in the partitioning step, not a compile-time `static_assert` — descriptors built
  via `make_tool_descriptor_with_invoke()` bypass the `Tool<>` CRTP path entirely, so ADR-158's
  compile-time checks can never see them.
- **MUST-FIX 2 — `report_progress` was kept live across concurrently-running calls, but its target
  is neither lock-protected nor safe for concurrent producers.** `emit_run_event_for()` does
  `++run_event_seq_by_run_[run_id]` (`agent_session.hpp:1664`) on a plain `std::unordered_map` with
  no lock — ADR-060 already named this exact map as the hazard `Backgroundable` avoids by resetting
  `report_progress` to a no-op. Worse than the red-team's own framing: `run_event_producer_` is a
  `stream_producer<RunEvent>` wrapping `rt::channel_producer<T,E>`, which documents "Multiple
  PRODUCERS are similarly unsupported" (`rt/channel.hpp:55-57`) — concurrent `push()` from several
  worker threads is unsupported usage of the channel type itself, not just a map race. **Resolved:**
  a new, dedicated `run_event_mutex_` (a plain `std::mutex` — nothing here ever needs to suspend a
  coroutine, only exclude a few memory writes) now guards `emit_run_event_for()`'s entire body: the
  seq increment, the tap call, and the producer push, in that order, as one critical section. Every
  concurrently-dispatched call's progress deltas now funnel through one serialization point,
  restoring "effectively single producer" for the channel and eliminating the map race in the same
  stroke. **Named residual, not solved:** a tap or producer callback that itself reentrantly calls
  `emit_run_event_for()` on the same thread would deadlock against a non-recursive mutex; no such
  reentrant call exists in the current tree, and this ADR treats non-reentrancy as a contract on any
  future tap rather than something the type system enforces.
- **MUST-FIX 3 — the `EffectContext` per-call-copy fix named only `report_progress`/`sandbox_fs`,
  missing `agent_turn_sink`/`moderator_delta_sink` (ADR-152, `effect_context.hpp:116-144`).**
  **Resolved for the top-level case, explicitly scoped out for the nested/workflow case:**
  `agent_turn_sink`/`moderator_delta_sink` are rebound per-call exactly like `report_progress` (not
  reset to a no-op — their deltas should still reach a live stream). Where either calls into *this*
  session's own `emit_run_event_for()`, MUST-FIX 2's mutex covers it for free. But `agent_turn_sink`
  specifically may instead call into a *different*, inner `AgentSession`'s own tap machinery
  (`rt::agent_session_as_executor_body()`, ADR-077) when this session is itself a workflow node —
  whether *that* inner session's `emit_run_event_for()` is safe against concurrent invocation from a
  sibling call dispatched by the *outer* session is an ADR-152/`WorkflowSupervisor` interaction this
  ADR does not resolve. Scoped out structurally (§4 non-goals, below): this design's initial scope
  is a plain, non-workflow-embedded `AgentSession`'s own top-level dispatch; a `function`-kind
  executor body's own nested tool dispatch stays sequential, exactly as it is today, until that
  interaction is separately verified.
- **MUST-FIX 4 — §2's inventory of dispatch loops undercounted the real call sites.** Corrected:
  four `invoke_tool()` call sites exist (`agent_session.hpp:1217, 1868, 1994, 2540`), not two. Of
  these, `:1868` (the `codeact_ask` replay branch) invokes exactly one stored-script call — never a
  multi-call batch, nothing to partition — and stays out of this ADR's scope by construction, not
  oversight. The other three (`:1217` `resolve_interaction()`'s approved branch, `:1994`
  `finish_hook_processed_round()`, `:2540` `run_rounds()`'s own loop) are all genuine "loop over
  calls" sites; the scheduler change applies uniformly to each of the three.
- **MUST-FIX 5 — within-group serialization was unspecified and risked pool starvation/deadlock.**
  **Resolved by changing the unit of fan-out**: a *concurrency class* — not each individual call —
  is what gets submitted to the fan-out mechanism as one job. A bare-`Parallelizable` call is a class
  of one; an `ExclusivityGroup<Name>` is a class of however many calls share that name, and the
  *entire class* runs as a single job, internally executing its members as an ordinary, strictly
  sequential loop (the same shape as today's existing dispatch loop, just scoped to the group) on
  whichever one worker picked it up. No group member ever separately occupies, or waits on, a second
  worker slot — this rules the starvation scenario out structurally, not by convention.
- **MUST-FIX 6 — `bound_capabilities` lifetime across the admission-thread → worker-thread handoff
  was unspecified**, a real dangling-pointer risk (`bound_capabilities` is a raw, non-owning
  `std::vector<BoundCapability> const*`, `effect_context.hpp:52`). **Resolved** by mirroring
  `background_task()`'s own precedent exactly: each concurrency class's admission step builds its
  own `bound` capability vector as a *member of the same movable job object* that also carries its
  by-value `EffectContext` copy. `ctx.bound_capabilities = &bound` is set exactly once, inside that
  job's own invoke step, pointing at the job's own member — never at a stack frame or a shared
  batch-level container the admission loop could reallocate out from under a still-queued job.
- **SHOULD-FIX 7 — a genuinely-suspending nested await inside `invoke()` would be silently abandoned
  by `rt::ThreadPool`'s one-resume-then-fault semantics** (`thread_pool.hpp:292-323`), leaking
  capability tickets (`invoke_tool()`'s step-10 revocation is a plain post-return loop, not RAII).
  **Resolved by not choosing `rt::ThreadPool` as the fan-out mechanism**: a small, purpose-built,
  bounded pool of real, reusable worker threads is used instead, each running `invoke()` as the
  plain, ordinary *blocking* synchronous call it already is today — no coroutine resumption involved
  at all. A worker blocked on a nested session's own `AsyncMutex` just blocks, occupying that one
  slot until it can proceed, exactly like today's single-threaded sequential case generalized to N
  workers — never abandoned, never faulted. Named trade-off: a batch can stall waiting for a free
  worker if every worker is blocked on a nested/recursive call — a liveness cost, not a
  capability-leak correctness defect, bounded by the same worker-count cap MUST-FIX 5 requires.
- **SHOULD-FIX 8 — `blob_sink`'s concurrent-write safety against the real
  `WorktreeObjectStore::put_blob` is unverified.** Not resolved by design; named as a required
  implementation-time proof obligation (matching ADR-158 §7's own evidence discipline) — before G4
  ships, `put_blob`'s behavior under concurrent callers from sibling classes in the same batch must
  be verified, or the sink call must itself be serialized here too.
- **SHOULD-FIX 9 — event-stream ordering is an I5/audit concern, not merely a UI nicety.**
  **Resolved** by distinguishing two separate claims that the original draft conflated: (a) 013 §1's
  "ordered and monotonic per run" requirement is satisfied structurally by MUST-FIX 2's mutex — `seq`
  is strictly increasing, with no gaps and no duplicates, ever, which is a real proof obligation, not
  a display nicety; (b) *which* sibling call's delta happens to land on `seq N` vs `N+1` among
  concurrently-running classes is genuinely unordered (a real wall-clock race) and is explicitly out
  of I5's scope, because I5 replay-determinism is governed solely by `ToolResult` append order into
  `history_` (already fixed by §4's own "emitted order, never completion order" rule) — progress
  deltas are observability, never replayed authoritative history.

## 4. Non-goals, stated up front

- **Not restructuring `invoke_tool()` into a coroutine.** Individual tools may already declare
  `ae::task<result<Reply>>`-shaped `invoke()` per 006 §1's own example, but the pipeline wrapper
  around them stays a blocking call for this ADR — becoming fully awaitable end to end, with a real
  multi-task executor pumping suspended coroutines instead of blocking OS threads, is a strictly
  larger change (§6 Option B) this ADR does not attempt.
- **Not touching `Background<max_concurrent>` or `background_task()`.** ADR-158 §4 already
  established these govern a disjoint call shape (explicitly detached, never a batch member); this
  ADR does not revisit that boundary.
- **Not resolving ADR-158 §5's remaining open questions** (non-transitivity of a named group,
  remote/MCP-tool defaults) by itself — but it is the first design that can actually test the third
  one, determinism under I5, since it is the first to build real scheduling code (§5's own proof
  obligation, once implemented).
- **Not extending parallel fan-out into a workflow-embedded nested session's own tool dispatch**
  (MUST-FIX 3, above) — a `function`-kind executor body's own nested `invoke_tool()` calls stay
  sequential until ADR-152/`WorkflowSupervisor`'s own event-multiplexing is separately verified safe
  against being driven by a concurrently-running sibling call from the *outer* session's batch.

## 5. Corrected design

**Batch partitioning.** Given an admitted batch of `ToolCallRequest`s (post steps 1–3, resolve/
validate/taint, already done sequentially exactly as today), partition it into concurrency classes
using each request's `ToolDescriptor`, in this order:
1. A call whose tool declares `captures_session_state` is **always its own sequential singleton
   class**, unconditionally — regardless of whether it also declares `Parallelizable` or
   `ExclusivityGroup<Name>` (MUST-FIX 1). This is checked first, before either concurrency tag, as a
   runtime guard (these descriptors bypass `Tool<>`'s compile-time path entirely).
2. Otherwise, a call whose tool declares neither `Parallelizable` nor `ExclusivityGroup<Name>` is
   likewise its own sequential singleton class, ordered exactly where it falls in the model's
   emitted sequence, blocking the whole batch's progression at that point — matching 006 §5's
   existing "sequential by default" rule. **Still open, see §6**: whether ONE such call in an
   otherwise-eligible batch demotes the *entire* batch to sequential (006 §5's current literal "a
   batch is parallel only when *every* call in it is `Parallelizable`"), or whether this ADR is the
   point that all-or-nothing rule is loosened to a per-class partition. This design leans toward
   keeping the existing all-or-nothing gate for now, treating "partial fan-out inside a mixed batch"
   as a separate, later RFC-amendment question — the red-team pass did not adjudicate this, since it
   is a policy decision, not a correctness defect.
3. Within a fan-out-eligible batch, every call declaring bare `Parallelizable` is its own singleton
   concurrency class (may run alongside anything). Every call declaring `ExclusivityGroup<Name>` is
   grouped by `Name` into **one class per group name**, not one class per call (MUST-FIX 5) — the
   class, as a whole, is the unit of fan-out; see "Fan-out" below.

**Admission stays sequential.** Steps 4/5/7 (authorize, approve, bind) run for every call in the
batch, in the model's emitted order, exactly as `invoke_tool()` does today — one call's admission
completing before the next call's admission begins. This preserves today's `PolicyDecider`/
`ApprovalDecider` semantics untouched (no decider ever sees two calls "at once," matching ADR-070's
existing per-call-only contract) and keeps I1's single-executor sequencing intact for every decision
that can suspend the run or consult host policy.

For each call, admission builds a **job object** — the unit that will later run on a worker — and
that job object *owns*, as its own members:
- the call's own `bound` capability vector (step 7's output), built in place inside the job, never
  in a stack frame or shared batch-level container (MUST-FIX 6);
- a **freshly-copied, by-value `EffectContext`** (ADR-060's precedent, generalized to every
  reverse-channel field, not just the two it originally covered — MUST-FIX 3): `report_progress`,
  `agent_turn_sink`, and `moderator_delta_sink` are all rebound to close over *this* call's own
  `call_id` (none are reset to a no-op — a parallel-batch call's deltas are still meant to reach the
  run's event stream, unlike `Backgroundable`'s deliberate suppression); `sandbox_fs` and `blob_sink`
  are carried through unchanged; `ctx.bound_capabilities` is set to point at the job's own `bound`
  member only once the job actually runs (never earlier).

For an `ExclusivityGroup<Name>` class with more than one member, admission builds **one job per
group**, containing an ordered list of that group's own per-call bound-records/EffectContext copies
(built in emitted order) — not one job per member.

**Fan-out.** Once every call in the batch is admitted (all jobs built, still on the session's own
thread, still sequential), each concurrency class's job is submitted to a **small, purpose-built,
bounded pool of reusable worker threads** (SHOULD-FIX 7) — explicitly *not* `rt::ThreadPool` (its
own file banner scopes it to model-call continuations, and its one-resume-then-fault semantics
would abandon, and leak the capability tickets of, any job whose `invoke()` genuinely suspends on a
nested await — e.g. a nested `invoke_agent_tool()` waiting on another session's own `AsyncMutex`),
and explicitly *not* a raw detached `std::thread` per class (unbounded thread creation, no reuse).
Each worker runs its job's `invoke()` call(s) as the plain, ordinary *blocking* synchronous call(s)
they already are today — no coroutine resumption on the worker at all. A singleton-class job runs
one call; an `ExclusivityGroup<Name>` job runs its members as an ordinary sequential loop, in
emitted order, entirely within that one job, on that one worker — no group member ever separately
occupies, or blocks waiting on, a second worker slot. **The pool's worker count is a bound this ADR
requires but does not yet fix a source for** (a new capability, a fixed engine constant, or a
per-run configuration knob — 006 §9 Q5's precedent favors the latter) — still open, §6.

**`emit_run_event_for()` becomes the batch's serialization point for progress delivery.** A new,
dedicated `run_event_mutex_` (a plain `std::mutex`) guards the method's entire body — the `seq`
increment, the tap call, and the producer push — as one critical section (MUST-FIX 2). This
restores "effectively single producer" for `run_event_producer_` (a `stream_producer<RunEvent>`
wrapping `rt::channel_producer<T,E>`, which documents multiple producers as unsupported) and
eliminates the `run_event_seq_by_run_` map race, for every concurrently-dispatched call's
`report_progress`/`agent_turn_sink` delta alike. Named residual: a tap/producer callback that
itself reentrantly calls `emit_run_event_for()` on the same thread would deadlock against this
non-recursive mutex; no such call exists today, and this ADR treats non-reentrancy as a contract on
any future tap.

**Join and serialize.** Each job's completion produces one `ToolResult` per call it ran (steps 9/10
— normalize, account — run on whichever worker executed step 8, since they operate only on that
call's own, independently-owned data; `blob_sink` calls made concurrently from sibling jobs remain
an implementation-time proof obligation against `WorktreeObjectStore::put_blob`, SHOULD-FIX 8).
Once every class in the batch has completed, results are appended to `history_` **in the model's
original emitted order** — never completion order — satisfying 006 §5's existing determinism rule
and giving G4's gate something concrete to measure: run the same batch with artificially randomized
completion latencies (a test-only seam, not production behavior) 10⁴ times and assert the appended
history order never varies. This determinism claim is scoped to `history_`'s append order only —
the run event stream's own `seq` values are structurally monotonic (013 §1, via the new mutex) but
*which* sibling call's delta lands on a given `seq` is a genuine, unordered wall-clock race,
explicitly outside I5's scope (SHOULD-FIX 9): progress deltas are observability, never replayed
authoritative history.

**Failure isolation.** A single call's `invoke()` throwing or faulting must not abort sibling calls
already in flight in the same batch — 006 §3's "a tool error is a value returned to the model, not
an exception and not a run abort" already establishes this for the sequential case; a concurrent
class must preserve it identically: one call's fault becomes that call's own `ToolResult{is_error}`
(exactly as `invoke_tool()` already produces today for an in-process exception), with no effect on
any other concurrently-running class or on other members of its own `ExclusivityGroup` job (a
group's own internal loop continues past one member's error exactly as the sequential batch loop
does today).

**Scope boundary.** This design covers the three genuine "loop over calls" dispatch sites
(`agent_session.hpp:1217, 1994, 2540` — MUST-FIX 4) on a plain, non-workflow-embedded `AgentSession`
only. It does not extend fan-out into a `function`-kind executor body's own nested tool dispatch
(MUST-FIX 3 / §4 non-goals).

## 6. Open questions this design does not resolve

- **All-or-nothing vs. per-class fan-out.** Does a batch with one non-eligible call fall back
  entirely to sequential (this design's default, §5), or does this ADR loosen 006 §5's literal
  "every call" wording to allow a mixed batch to partially fan out? This is a real RFC amendment (a
  second one, after ADR-158's own amendment to the same section), not a pure implementation detail —
  needs an explicit project-owner decision, not an inherited default. The red-team pass did not
  attack this because it is a policy choice, not a correctness question.
- **The concurrency bound's source** — is an unbounded batch size, fanned out to the pool's full
  worker count, an acceptable risk on its own (i.e. is the model's own batch size already
  effectively bounded elsewhere, making a separate cap moot), or does the worker-pool size need its
  own capability/config knob, analogous to `Background<max_concurrent>`? If the latter, is it a NEW
  capability (widening the surface a tool ceiling must cover) or a run-level/host-level
  configuration value (006 §9 Q5's precedent)?
- **Cancellation.** 006 §6b explicitly states no cancellation mechanism exists yet for in-flight
  native `invoke()` work in the `Backgroundable` case; this design inherits the identical gap for a
  parallel batch's in-flight jobs — named, not solved, here.
- **Determinism proof shape.** §5 sketches "randomize completion latency, run 10⁴ times" as the
  literal mechanism 006 §8 G4 asks for — is that actually implementable as a deterministic,
  CI-stable test (a seeded RNG driving artificial per-class delay, not real wall-clock scheduling
  jitter, to avoid a flaky test), and does 10⁴ iterations run in acceptable CI time once real thread
  fan-out is involved (unlike ADR-158's purely compile-time proof, this gate requires runtime
  execution at scale)?
- **The ADR-152/`WorkflowSupervisor` interaction** named in MUST-FIX 3 and excluded by §4's own
  non-goal — extending fan-out into workflow-embedded nested sessions needs its own, separate design
  pass once this top-level scope is proven.

## 7. Evidence (real code, real tests, real build/test run)

§5's corrected design is implemented, not just drafted. Files changed:

- **`include/agentengine/core/tool_pipeline.hpp`**: `ToolDescriptor::parallelizable` (a new `bool`,
  populated in both `make_tool_descriptor<ToolT>()` and `make_tool_descriptor_with_invoke<ToolT>()`
  from `ToolT::kHasParallelizable` — `ExclusivityGroup<Name>` and bare `Parallelizable` are now both
  runtime-queryable on a real descriptor, matching the pattern ADR-158 already established for
  `exclusivity_group`). `admit_call()` — steps 1/4-7/5 extracted from `invoke_tool()` verbatim, no
  logic change, so the parallel-batch path and the sequential path share ONE admission
  implementation rather than two independently-drifting copies. `run_admitted_call()` — steps 8/9/10
  extracted the same way, returning a raw `RunOutcome` (result + optional `error` + bytes) with
  no audit bookkeping, specifically so `invoke_tool()`'s own audit-duration semantics (measured from
  before admission) and the parallel path's own (measured per-job) never collide by sharing a
  `finish()`-shaped closure. `make_call_audit()` — a small, shared audit builder for the parallel
  path only (`invoke_tool()`'s own inline `finish()` and `background_task()`'s own closure are both
  untouched, per §4's non-goals). `concurrency_class_kind`/`ConcurrencyClass`/`partition_batch()` —
  §5's batch-partitioning logic, a pure function with no I/O.
- **`include/agentengine/rt/bounded_call_fanout.hpp`** (new file): `rt::run_jobs_bounded()` — the
  SHOULD-FIX 7 resolution. A short-lived, join-before-return fan-out over up to `worker_cap` real
  `std::jthread` workers pulling job indices off one shared `std::atomic<std::size_t>` counter — not
  `rt::ThreadPool`, not a persistent pool, not a fourth ad hoc mechanism beyond what §5 already
  justified.
- **`include/agentengine/rt/agent_session.hpp`**: `run_event_mutex_` (MUST-FIX 2) — a new
  `std::mutex` guarding `emit_run_event_for()`'s entire body. `dispatch_tool_calls()` — the real
  implementation of §5's corrected design: partitions the batch, admits every call sequentially in
  emitted order (building each call's own by-value `EffectContext` copy and its own `bound`
  capability vector as members of a movable `ParallelJob`, MUST-FIX 6), runs `sequential`-class
  calls inline exactly as today, and fans `parallel`/`exclusivity_group` classes out via
  `rt::run_jobs_bounded()` — one job per CLASS, not per call (MUST-FIX 5), each group's own members
  running as an ordinary sequential loop inside that one job. All three real dispatch call sites
  (`resolve_interaction()`'s approved branch, `finish_hook_processed_round()`, `run_rounds()`'s own
  loop — MUST-FIX 4's corrected inventory) now call this one shared method instead of each
  maintaining its own inline loop; the codeact-ask replay site (a single call, never a batch) is
  untouched, as scoped.
- **`tests/test_tool_batch_partition.cpp`** (new, 20 checks): `partition_batch()` in complete
  isolation — the eligibility gate, `ExclusivityGroup<Name>` members correlating into one class,
  `captures_session_state` forcing sequential even inside an eligible batch (built against a real
  `make_tool_descriptor_with_invoke<T>()` descriptor, the same shape `ScheduleWakeupTool` uses,
  proving MUST-FIX 1's fix against the actual hazard named), unresolved-tool-name fail-closed
  behavior, and the empty-batch case.
- **`tests/test_tool_batch_parallel_dispatch.cpp`** (new, 3 claims, real `AgentSession` round-trips):
  (1) two independent `Parallelizable` calls proven to run on different OS threads at the same time
  via a rendezvous (each blocks until it observes the other has started); (2) two
  `ExclusivityGroup<"db-write">` calls proven to NEVER overlap via an instrumented concurrent-count
  high-water mark that never exceeds 1; (3) 64 real round-trips where the first-EMITTED call is
  deliberately the slower one (15ms vs. 1ms) — every single iteration's PHYSICAL completion order
  was observed to be the reverse of emitted order (positively confirming real concurrency, not
  merely inferring it), while `history_`'s appended order was the model's emitted order every time
  (006 §5 / G4's own determinism claim). Not the literal "10⁴ randomized completions" G4's own gate
  text asks for — §6 already named CI-practicality of that exact scale as open; 64 iterations with a
  15x timing margin is the real, executed evidence this pass produces instead.
- **A genuine bug was found and fixed during this pass, by direct execution, not by re-reading the
  design.** The two new integration-test tools' JSON args initially used `"{}"` for an
  all-defaulted `Args` struct; `AE_JSON_SCHEMA`'s generated parser requires every field present
  regardless of its C++ default value, so `tool.invoke()` failed at the schema-parsing step for
  EVERY call in `test_tool_batch_parallel_dispatch.cpp`'s first two claims, before `ToolT::invoke()`
  itself was ever reached — silently producing error `ToolResult`s whose `call_id` order still
  happened to look correct, which is exactly the kind of failure this project's own "prove, don't
  just design" discipline exists to catch. Diagnosed by direct instrumentation (temporary `fprintf`s
  at each stage: batch partitioning, job dispatch, per-thread start timestamps, `run_admitted_call`'s
  admission/deadline/invoke outcome) rather than by inspection, confirming the SCHEDULER'S OWN code
  was already correct (real concurrent thread start times ~900ns apart were observed even while the
  test was failing) — the defect was in the test fixture's own JSON payload, not in `dispatch_tool_
  calls()`, `partition_batch()`, `admit_call()`, or `run_admitted_call()`. Fixed by supplying the
  required field explicitly; all instrumentation was removed before this evidence was recorded.
- **Build/test results**: full rebuild (Ninja + MSVC 14.51, `-std:c++latest`, `/W4 /WX`), zero
  warnings, zero errors, both before and after every change in this pass. Full existing suite
  (256 registered tests, the two environment-gated ones skipped as always) — **100% pass, 0
  failures** — run cleanly, twice: once as a baseline immediately after the `admit_call`/
  `run_admitted_call` extraction (proving the refactor of `invoke_tool()` is behavior-preserving
  before any new call sites were wired), and once more after all three call sites were rewired to
  `dispatch_tool_calls()` (proving the sequential fallback — the path EVERY real batch takes today,
  since no shipped tool yet declares `Parallelizable`/`ExclusivityGroup<Name>` — is byte-for-byte
  unchanged). Both new test binaries (`test_tool_batch_partition`, `test_tool_batch_parallel_
  dispatch`) pass standalone. One unrelated, pre-existing flaky test
  (`test_content_durability_concurrency`, a `FileWorktreeObjectStore` durability test, an entirely
  different subsystem this ADR never touches) failed once during a run contaminated by a concurrent
  rebuild racing the test binary on disk — confirmed not a regression by rerunning it standalone
  (passes) and by a subsequent clean, uncontaminated full suite run (100% pass).

## 8. What this ADR does not claim

- **Does not amend 006 §5's text yet** — §6's own first open question (all-or-nothing vs. per-class
  fan-out) must be decided, and if it changes the rule, the RFC text amendment happens alongside
  that decision, not preemptively here.
- **Does not fix the concurrency-bound mechanism's ultimate source, cancellation, or the
  workflow-embedded case** — all three are named, open, and explicitly deferred (§6), not silently
  assumed solved. `kParallelBatchWorkerCap = 4` is a real, working placeholder constant, not a
  host-configurable knob.
- **Does not claim the bounded worker-pool design is free of every hazard** — SHOULD-FIX 8's
  `blob_sink` concurrent-write proof obligation against `WorktreeObjectStore::put_blob`, and the
  non-reentrancy contract on `emit_run_event_for()`'s new mutex (MUST-FIX 2), are both named
  residuals this implementation accepts rather than eliminates.
- **Does not implement the literal 10⁴-iteration G4 gate text.** §7's 64-iteration test is real,
  executed, positive evidence of the same underlying claim at a smaller, CI-practical scale — §6's
  own open question about whether 10⁴ iterations is CI-practical remains genuinely open, not quietly
  resolved by picking a smaller number.
- **Does not extend fan-out to a workflow-embedded nested session** (§4's own non-goal) — untouched,
  unverified, and explicitly out of scope for this pass.
- **Has not been independently red-teamed a second time.** §3's pass attacked the DESIGN; this
  implementation has not yet been given the same adversarial treatment a shipped ADR's code
  typically receives before "Judged" — per this project's own `design → red-team → prove → judge`
  discipline, `judge` (project-owner sign-off) is the remaining, undone step.
