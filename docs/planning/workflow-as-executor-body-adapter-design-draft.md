# Design draft: a `Workflow`-as-participant adapter (adjacent to issue #35)

## 1. The question

AgentEngine has no supported, reusable way to take a whole built `Workflow` (plus its `ExecutorBody`
set) and reuse it as a single participant somewhere else — inside another
`MagenticWorkflowBuilder`/`GroupChatBuilder`-shaped graph, or anywhere else a plain `ExecutorBody`
slot exists. Today the only paths are (a) hand-write a bespoke closure per call site that drives the
inner `WorkflowSupervisor` manually — with all the correctness hazards that entails, see §10 finding
1 — or (b) wait on issue #33 (`executor_kind::sub_workflow` as a first-class graph NODE, unbuilt, its
own much larger open-question set). Grounded against MAF's real
`python/samples/03-workflows/agents/sequential_workflow_as_agent.py` (`agent-framework` clone, commit
`4c0bff8`): `workflow.as_agent()` is explicitly the OTHER direction from `WorkflowExecutor`/
`SubworkflowBinding` (issue #33's own mechanism) — "no graph node, no message-type wiring", terminal
output only, reusable as one ordinary participant.

**Scope note, corrected after red-team (§10 finding 2):** the first draft of this document claimed to
close issue #35 ("no adapter lets a built Workflow satisfy the Agent/ChatClient interface"). On a
closer read, #35 as filed is centered on a **`ChatClient`-shaped** adapter — its own References
section names `Agent`/`ChatClient` (not `ExecutorBody`), and its own "Why this matters" section
explicitly names "hand-writing a bespoke `ExecutorBody`/`ChatClient` shim per call site" as the
*inadequate status quo*, not the target. What this document designs — a correctly-synchronized,
reusable `ExecutorBody` adapter — is real, valuable, adjacent work (it turns "everyone hand-rolls
their own possibly-buggy shim" into "one proven-correct library function," which is exactly what the
red-team's own finding-1 race condition demonstrates is needed), but it is **not** what #35 asked for
and does not close it. This will be filed as its own, separate GitHub issue when implemented; #35
stays open for the `ChatClient`-shaped adapter, which has its own, different open questions (a
multi-turn `ChatRequest` against something with no "turn" concept) genuinely deferred, not attempted
here — matching ADR-149's own restraint in deferring issue #28 item 5.

## 2. The precedent this mirrors, and where it must diverge

`agent_session_as_executor_body()` wraps a real `rt::AgentSession<...>` as an `ExecutorBody` for an
`executor_kind::agent` node. It is templated on the session's own type, returns a special
`AgentExecutorBodyTag` (a structural marker `WorkflowSupervisor::initialize()` checks for, because
`executor_kind::agent` gets its own capability-ceiling validation path, and because that marker also
drives the agent-kind-only "at most one concurrent delivery" quarantine in `execute()`). None of that
applies here in the same shape:

- `WorkflowSupervisor` is a concrete, non-template class — the new adapter needs no template
  parameters at all.
- This adapter is NOT bound to a new `executor_kind`. From the OUTER graph's point of view, the node
  this body is attached to is an ordinary `executor_kind::function` node — exactly matching MAF's "no
  graph node" framing. No new `executor_kind`, no new field on `Executor`, no change to
  `check_workflow_executable()`.
- **Because it is an ordinary `function`-kind node, it does NOT inherit the agent-kind-only
  concurrency quarantine** (`execute()`'s gather-time dedup is scoped explicitly to
  `executor_kind::agent`). Red-team finding 1 (§10) found this matters a great deal — the adapter must
  provide its OWN concurrency safety, not borrow the outer supervisor's.
- `AgentSession` accumulates real conversation history across repeated calls WITHIN one outer run
  (`agent_session_as_executor_body`'s own documented "reuse across rounds is intentional" contract).
  A `WorkflowSupervisor`'s `run_workflow()` unconditionally resets `state_`/`ports_`/`rounds_` at its
  own top (confirmed by direct read, `rt/workflow_supervisor.hpp`) — so this adapter has the OPPOSITE
  contract: **every call is an independent, fresh, complete run of the inner workflow from its own
  `start`, with zero memory of any earlier call.** This is not a limitation to work around; it is
  MAF's own `as_agent()` semantics ("one call in, one final output out") and is documented as the
  adapter's actual contract, not a residual.

## 3. The adapter (revised after red-team — see §10 findings 1, 3, 4)

New file `include/agentengine/rt/workflow_as_executor.hpp`. Two entry points, both returning
`result<ExecutorBody>` (construction can now fail — see §4):

```cpp
[[nodiscard]] result<ExecutorBody> workflow_as_executor_body(
    std::shared_ptr<WorkflowSupervisor> inner);   // PRIMARY surface — see §10 finding 4

[[nodiscard]] result<ExecutorBody> workflow_as_executor_body(WorkflowSupervisor& inner);
    // advanced/reference form — ONLY when the caller already independently guarantees inner's
    // lifetime; see the LIFETIME CONTRACT paragraph this gets in the real file, with the two
    // red-team-supplied negative examples (factory-returns-a-body; vector<WorkflowSupervisor>
    // reallocation) spelled out as wrong, not just an abstract "must outlive" statement.
```

Both overloads:

1. **Refuse at construction** (return an error, build nothing) if `inner->graph()` (or `inner.graph()`
   for the reference overload) contains ANY `executor_kind::request_port` executor — see §4's
   red-team-driven revision; this is what eliminates the state-loss hazard finding 3 identified,
   rather than merely documenting it.
2. Otherwise, build a closure that captures (a) the supervisor — by `shared_ptr` in the primary
   overload (so the returned `ExecutorBody` owns a real reference-counted keep-alive, safe to store,
   copy, and return from a factory), or by reference in the advanced overload, and (b) a fresh
   `std::shared_ptr<std::mutex> call_mutex` — see §10 finding 1, this is NEW versus the first draft
   and is what actually makes the adapter safe, not `WorkflowSupervisor::run_mutex_`.

Body (both overloads, differing only in how `inner` is captured):

```cpp
return [inner, call_mutex](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
    std::lock_guard<std::mutex> guard(*call_mutex);   // see §10 finding 1
    WorkflowResult r = drive(inner->run_workflow(RunWorkflow{in}));
    if (r.status == workflow_status::completed) return ExecutorOutcome{r.output};
    return std::unexpected(error{failure_class::contract, /* names r.status, see §4 */,
                                  "rt.workflow_as_executor.inner_run_not_completed." + status_tag(r.status)});
};
```

The SAME hand-rolled `drive<T>()` "resume until done" loop every `rt::` file duplicates
(`agent_workflow_executor.hpp`'s own copy, `examples/10`/`19`/`20`'s own copy) — proven safe for
`WorkflowSupervisor::run_workflow()` specifically by every existing example/test in this codebase
that already drives it this way from a plain, non-coroutine call site, **now additionally guarded by
`call_mutex` so `drive()` is never entered concurrently against the same `inner`** (see §10 finding 1
for exactly why that guard, not `run_mutex_` itself, is load-bearing here).

**ROUTING NOTE** (closes §10 finding 6 / the original punch-list item 4): `ExecutorOutcome{r.output}`
carries empty `routes`, exactly mirroring `agent_session_as_executor_body`'s own "no routing concept"
disclosure — a `switch_case`/`multi_selection` edge OUT of a wrapped node can never fire from this
adapter alone. `r.output`'s `Message`/`ContentItem` metadata (`origin`, `tainted`) passes through
unchanged, neither stripped nor reinterpreted, so no I3 concern: nothing the inner workflow produced
is smuggled into the outer graph with more authority than it already carried.

## 4. What happens when the inner run does NOT complete (revised after red-team — see §10 finding 3)

`ExecutorBody`'s contract is one synchronous call, one `result<ExecutorOutcome>` return — there is no
way to express "still pending" back to the outer graph. The first draft fail-closed uniformly on
every non-`completed` status, including `suspended`, and called that "disclosed." Red-team finding 3
(§10) found this actually **silently and permanently loses the inner workflow's open interaction the
next time `run_workflow()` resets `state_`** — a real state-loss hazard, not merely an unsupported
case.

**Fixed by refusing earlier, not by documenting harder**: both `workflow_as_executor_body()`
overloads now reject construction outright — a `failure_class::contract` error, error code
`rt.workflow_as_executor.request_port_unsupported` — if the wrapped graph contains any
`executor_kind::request_port` node at all. This means `suspended` can **never** actually occur through
this adapter (the only source of a `suspended` status is a `request_port` firing), so the state-loss
hazard is eliminated structurally, the same "refused graph is recoverable; a quietly reinterpreted one
is not" discipline `check_workflow_executable()` already applies elsewhere in this codebase
(`workflow/graph.hpp`).

The remaining non-`completed` statuses (`bound_max_rounds`/`bound_deadline`/`bound_token_budget`/
`bound_max_stalls`/`bound_max_resets`, `routing_failed`, `merge_conflict`, `invalid`) carry NO
lingering state hazard — the next call to the same adapter starts a fully independent fresh run
regardless (§2's contract), so there is nothing to lose. These still fail closed as a single
`failure_class::contract` error (a caller cannot meaningfully retry or route around a broken inner
workflow from the outer graph's own edge-policy machinery, which is `failure_class`/error-code keyed,
not message-text keyed) — but the error CODE now embeds the specific inner status
(`rt.workflow_as_executor.inner_run_not_completed.<status>`), so a caller inspecting the error
programmatically CAN tell "hit a configured bound" apart from "routing failed inside the inner graph"
apart from "the inner graph itself is malformed," even though the `failure_class` stays uniformly
`contract` for all of them.

**Still named, disclosed, NOT fixed here**: nested `request_port` proxying (mirroring MAF's
`WorkflowHostExecutor::_pendingResponsePorts`) is real, separate, unbuilt work — but it is no longer a
silent-loss hazard to work around; it is simply a graph shape this adapter refuses up front until
that future extension exists.

## 5. Capability sourcing (I2)

The adapter's returned `ExecutorBody` receives an `EffectContext&` from the OUTER graph (every
`ExecutorBody` does, by signature) but **does not use it at all** — the parameter is present only
because the signature requires it, and is intentionally ignored. The inner workflow's own executors
run under whatever `EffectContext`s the CALLER already passed to `inner.initialize(..., contexts,
...)` before construction — entirely decoupled from whatever capabilities the outer node happens to
carry.

This is a deliberate choice, not an oversight. `agent_session_as_executor_body()` can thread the
outer `ctx` straight into the wrapped session because an `AgentSession` is ONE entity with ONE
capability set — a 1:1 substitution. An embedded `Workflow` has N executors, each with its own
`capability_ceiling`; there is no natural 1:1 mapping from one outer `EffectContext` onto that shape,
and forcing one would either silently widen some inner nodes or silently narrow others in a way
nothing here could predict.

**Strengthened after red-team (§10 finding 5)**: this isn't only an analogy to "a plain hand-written
`ExecutorBody` lambda already has this same property" — it is a directly-citable, pre-existing fact of
this codebase. `check_workflow_executable(Workflow const&, contexts)` (`workflow/graph.hpp`) only
engine-enforces `capability_ceiling` for `executor_kind::agent` nodes — a `function`-kind node's
`capability_ceiling` (a field `TypedExecutor` exposes generally, not gated to agent-kind) is **never
engine-enforced for any function-kind node today, with or without this adapter**. So "the outer node's
visible ceiling doesn't reflect what a wrapped inner workflow can actually do" is not a hole this
adapter creates — it is a pre-existing, general property of every function-kind node in this
codebase, which this adapter's own file banner will cite directly rather than resting purely on
analogy.

A host that WANTS the outer node's grant to influence the inner workflow arranges that explicitly at
construction time (e.g. building `inner_contexts` from the same `CapabilitySet` the outer graph will
later use) — this adapter does not do it automatically, and does not need to for a correct v1.

## 6. Concurrency (rewritten after red-team — see §10 finding 1)

The first draft claimed a second concurrent call against the same `inner` "queues rather than
corrupts state" via `WorkflowSupervisor::run_mutex_`. **This was wrong and has been retracted.**
Red-team finding 1 (§10) traced the actual mechanism: `AsyncMutex::lock()`'s contended path parks the
*awaiting coroutine's own handle* in `waiters_`, to be resumed later ONLY by the matching `unlock()`'s
own trampoline — never by an unrelated caller blindly calling `.resume()` on it. This adapter's
`drive<T>()` loop (`while (!t.done()) t.resume();`) has no idea what the coroutine is actually
suspended on; if a second delivery lands on the same wrapped node in the same outer round (an entirely
ordinary graph topology — e.g. two edges converging on one node — nothing agent-kind-only about
`function`-kind nodes prevents this), the second thread's `drive()` loop resumes the FIRST call's
already-parked coroutine handle out from under it, both proceed to run `execute()` against the same
`state_`/`ports_`/`rounds_` concurrently — a genuine data race, and later a second, spurious resume of
a possibly-already-destroyed coroutine frame when the real `unlock()` trampoline eventually runs.

**Fixed, not merely documented**: the adapter now owns its OWN plain `std::mutex` (`call_mutex`,
§3), held for the full duration of one `drive(inner->run_workflow(...))` call, entirely independent
of `WorkflowSupervisor::run_mutex_`'s coroutine machinery. This runs on an ordinary `ThreadPool`
worker thread (never inside a suspended coroutine frame itself), so blocking on a real OS mutex here
is safe: a second concurrent delivery to the same wrapped node genuinely blocks until the first
call's ENTIRE `drive()` loop finishes, then proceeds with its own fully independent fresh run — no
race, no double-resume, and the "every call is fresh" contract (§2) is preserved exactly, just
serialized rather than corrupted when two deliveries land on the same node in one round.

## 7. What this draft does NOT claim

- No `ChatClient`-shaped adapter (deferred — different open questions, named in §1; that is the
  actual subject of issue #35, left open).
- No nested `request_port` proxying — and, after §4's revision, no graph containing a `request_port`
  node is even accepted by this adapter until that future extension exists.
- No cross-call state/history for the inner workflow (by design — §2's "fresh every call" contract,
  not a gap).
- No new `executor_kind`, no change to `check_workflow_executable()`, no change to
  `WorkflowBuilder`/`MagenticWorkflowBuilder` — a caller wires this exactly like any other
  hand-written `ExecutorBody` into an existing `bodies` vector, by index, today.
- Does not touch issue #33 at all — that gap (a `sub_workflow`-kind graph NODE with typed message
  passing, intermediate-result visibility, and its own checkpoint-composition questions) remains
  fully open and unrelated in mechanism to this adapter.
- No compile-time or runtime check that the outer `TypedExecutor<In,Out>` declared for the wrapping
  node actually corresponds to what the inner workflow's `start` consumes or `output_selection`
  produces (§10 finding 7) — this adapter returns a type-erased `ExecutorBody` exactly like any
  hand-written closure would, so that correspondence is the author's manual responsibility, same as
  it already is for every existing function-kind node in this codebase. Named explicitly here so it
  is not discovered the hard way.

## 8. Files (planned)

- **New**: `include/agentengine/rt/workflow_as_executor.hpp` (`workflow_as_executor_body()`, both
  overloads)
- **New**: `tests/test_rt_workflow_as_executor.cpp` — at minimum: a completed inner run's output
  reaches the outer graph unchanged; a SECOND call to the same adapter starts fully fresh (no
  leftover state from the first); construction is REFUSED for a graph containing a `request_port`
  node (proving §4's fix, not just its absence of a crash); an inner-workflow failure
  (`routing_failed`/a bound) fails closed with a status-specific error code; a REAL concurrency proof
  — two genuinely concurrent deliveries to the same wrapped node (e.g. via an outer fan-out) complete
  correctly and serially rather than racing, proving §6's fix under actual contention, not just
  reasoning about it; the owning (`shared_ptr`) overload used from a factory function that returns the
  `ExecutorBody` after its own local `WorkflowSupervisor` goes out of scope, proving §10 finding 4's
  fix; an end-to-end proof of the actual headline use case — a whole `MagenticGraph` (or any built
  `Workflow`) wrapped and used as ONE `.participant()` inside an OUTER `MagenticWorkflowBuilder` (or
  plain `WorkflowBuilder`) graph, driven through a real `WorkflowSupervisor::run_workflow()` call on
  the OUTER graph, proving the adapter is genuinely composable, not just unit-tested in isolation.
  Before writing this last one, confirm `MagenticWorkflowBuilder`'s own `.manager()`/`.participant()`
  impose no extra constraint beyond ordinary `TypedExecutor` (§10 finding 8 — not deeply audited by
  the red-team, worth a direct check first).
- **New**: `examples/21_workflow_as_participant.cpp` — the worked, runnable proof of that same
  composition, offline (no live model needed, matching examples 10/13/14/20's own style).

## 9. Punch list sent to red-team (original, kept for the record)

1. Is "fail closed on non-`completed` status" actually the right default, or should `suspended`
   specifically get different treatment?
2. Does "zero implicit capability flow across the boundary" (§5) hold up?
3. Is capturing `inner` by reference the right lifetime contract given this adapter is likely to be
   constructed and immediately handed into a `bodies` vector that could outlive its building scope?
4. Any I3 concern in returning the inner workflow's raw `r.output` unchanged?
5. Does the concurrency caveat (§6) actually hold under real contention?

## 10. Red-team round 1 findings and resolutions

Independent red-team pass (fresh agent, zero prior context, briefed on this draft plus
`agent_workflow_executor.hpp`, `workflow_supervisor.hpp`, `workflow/graph.hpp`, issues #35/#33, and
CLAUDE.md's I1-I8 invariants) found 4 MUST-FIX findings and 4 MINOR/RESIDUAL findings, all resolved
before implementation:

1. **MUST-FIX, most severe** — the original §6 concurrency claim ("`run_mutex_` queues rather than
   corrupts") was traced to be actively wrong: a second concurrent delivery to the same wrapped node
   (ordinary outer-graph topology, not caller misuse) resumes another call's already-parked
   `AsyncMutex` waiter handle out from under it via the adapter's own blind `drive()` loop — a genuine
   data race, plausibly followed by a double-resume of a destroyed coroutine frame. **Fixed**: the
   adapter now owns its own `std::mutex`, independent of `run_mutex_`, serializing concurrent calls
   safely (§6, rewritten).
2. **MUST-FIX** — this draft originally claimed to close issue #35; #35 as filed is centered on a
   `ChatClient`-shaped adapter, not an `ExecutorBody` one, and its own text names the latter as the
   inadequate status quo. **Resolved**: reframed (§1) as separate, adjacent work, to be filed as its
   own new issue rather than closing #35; #35 stays open.
3. **MUST-FIX** — fail-closed on `suspended` (bundled with every other non-`completed` status) was
   understated as "disclosed, not fixed"; it actually silently and permanently orphans the inner
   workflow's open interaction the next time the same adapter is called, since `run_workflow()`
   unconditionally resets `ports_`. **Fixed**: construction now refuses any graph containing a
   `request_port` node outright (§4, rewritten) — `suspended` can no longer occur through this
   adapter at all, eliminating the state-loss hazard structurally rather than documenting it.
4. **MUST-FIX** — `WorkflowSupervisor` is immovable (embeds `AsyncMutex`, which deletes copy and
   thereby suppresses the implicit move ctor too), so the reference-capture overload cannot be
   rescued by "build it, then move it" — and the adapter's own headline use case (a reusable,
   composable participant, possibly built by a factory function) actively invites exactly the
   dangling-reference and container-reallocation traps that break reference capture. **Fixed**: added
   a `shared_ptr`-owning overload as the PRIMARY documented surface (§3); the reference overload
   remains available for callers with an already-guaranteed-stable `WorkflowSupervisor`, with an
   explicit LIFETIME CONTRACT paragraph and the red-team's own two negative examples spelled out in
   the real file.
5. MINOR — the I2 "zero implicit capability flow" argument was analogy-only; strengthened with a
   direct citation (`check_workflow_executable()` never engine-enforces `capability_ceiling` for
   function-kind nodes at all, adapter or not) — §5, revised.
6. MINOR — no explicit "ROUTING NOTE" mirroring the sibling adapter's own I3 disclosure. Added to §3.
7. MINOR — no explicit statement that the outer `TypedExecutor<In,Out>`'s compile-time type pairing
   has zero visibility into whether it actually matches the wrapped inner workflow's real input/output
   shape. Added to §7.
8. MINOR — `MagenticWorkflowBuilder`'s own `.manager()`/`.participant()` surface wasn't deeply audited
   for extra constraints beyond ordinary `TypedExecutor`. Named as a pre-flight check in §8 before the
   composability test/example is written, not assumed clear.
