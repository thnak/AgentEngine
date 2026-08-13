# Design draft: closing `executor_kind::agent`'s runtime bridge

**Status:** Design draft, red-teamed once (below) — **not an ADR, no code written**. Seeds a future
ADR once a milestone claims this work; per explicit project-owner direction (2026-08-13): document
what implementation needs, do not implement yet. Companion docs: `agent-as-workflow-executor-gap.md`
(current-state gap analysis), `docs/research/2026-08-13-maf-agent-as-workflow-executor.md` (MAF's own
shipped design, read directly from `D:\GitSrc\agent-framework`).

## Why a second red-team pass already, before any code exists

This project's own discipline (`CLAUDE.md`) is design → red-team → prove → judge for anything
contested or security-adjacent. The first draft of this design (below, §1) looked small — "no
core-seam change needed, `ExecutorBody` already supports this by capture-by-reference, only
`check_workflow_executable()`'s blanket refusal needs to loosen." An independent adversarial pass
against real code found that claim is **false in two places that matter**, both FATAL as originally
scoped. Recording the failed first draft alongside the fix, matching every other ADR in
`decisions/README.md`, so a future implementer doesn't re-discover the same two bugs.

## 1. The first draft (rejected as originally scoped — see §3 for the corrected version)

A standalone adapter, no changes to `rt/workflow_supervisor.hpp` or `workflow/graph.hpp`:

```cpp
template <class ChatClientT, class HistoryProviderT>
[[nodiscard]] rt::ExecutorBody agent_session_as_executor_body(
    rt::AgentSession<ChatClientT, HistoryProviderT>& session,
    CapabilitySet const& capability_ceiling,
    Principal caller);
```

Wraps `session.start_run(StartRun{...})` (real, `rt::task<result<AgentResponse>>`,
`include/agentengine/rt/agent_session.hpp`), driven via the exact `drive<T>()` pattern
`examples/16_group_chat_live.cpp:54-58` already uses for `ChatClient::chat()`, maps `AgentResponse` to
`rt::ExecutorOutcome`. Mint one `AgentSession` per workflow RUN (never shared across runs, matching
MAF's `declareCrossRunShareable: false`), reused across multiple visits to the same node within one
run so a cyclic reflection loop accumulates real conversation history. `check_workflow_executable()`
loosens from a blanket `agent`-kind refusal to accepting it once the node has a real bound body and a
declared capability ceiling (new `Executor::capability_ceiling` field, mirroring
`Tool<...>::capability_ceiling`).

## 2. Red-team findings against the first draft

An independent pass (fresh context, real file:line citations, no prior exposure to this doc) found:

**FATAL — checkpoint/resume silently loses the session, or the whole conversation.**
`WorkflowSupervisor::restore_from_record()` (`rt/workflow_supervisor.hpp:607-630`) restores only
`RunStateRecord` (pending deliveries, partial outputs, ports) — it never touches `bodies_`, which is
caller-supplied fresh at `initialize()` and matched to `graph_.executors` purely by array index, with
zero structural link to a checkpoint record. `tests/test_rt_workflow_checkpoint_g2.cpp`'s own pattern
(line 9's comment: "a genuinely NEW `WorkflowSupervisor` instance") confirms this is the intended
shape: a resumed run gets brand-new bodies from the caller, not reconnected ones. Worse, even a
correctly-reconnected `AgentSession` has amnesia: `AgentSessionRecord` carries no `history_` field —
`examples/12_session_checkpoint.cpp:139-143` names this directly as an accepted, already-known gap
("conversation history is NOT restored by this snapshot"). So the first draft's whole justification
for reusing one `AgentSession` across a cyclic node's repeated visits (accumulated history) evaporates
the moment that run is ever checkpointed and resumed — silently, not with an error.

**FATAL — same real bug class ADR-039's own red-team found, one level down: a genuine concurrent
double-resume race.** `execute()`'s delivery loop (`rt/workflow_supervisor.hpp:725-753`) has no
merge-by-executor-index for ordinary edges — only `edge_kind::fan_in`-tagged edges get that treatment
(confirmed directly: `examples/09_concurrent_workflow.cpp`'s "aggregator ran EXACTLY ONCE" claim is
credited to the fan_in edge kind specifically, not a general rule). Two ordinary `direct` edges from
different upstream nodes converging on the SAME agent-kind node in one round produce TWO separate
`Delivery` entries, both submitted concurrently via `pool_.submit()` onto different worker threads
before either is awaited (the file's own "decision 5" comment). If both closures capture the same
`AgentSession&` (the first draft's explicit design), both call `start_run()`, which does
`co_await session_mutex_.lock()` (`agent_session.hpp:407`) — a REAL `AsyncMutex` that genuinely parks
a contended waiter and resumes it from a DIFFERENT thread's `unlock()` (`async_mutex.hpp:139-140,
173-187`). But `drive<T>()` — the exact bridge the first draft proposes reusing — is only safe because
nothing it drives "genuinely parks" (`examples/16_group_chat_live.cpp:17-19`'s own comment states this
as the precondition). Two concurrent deliveries to one agent-kind node breaks that precondition for
real: `drive()`'s `while(!t.done()) t.resume();` loop keeps calling `.resume()` on a coroutine handle
simultaneously queued for a cross-thread resume from `unlock()` — a concrete double-resume race, not a
hypothetical one.

**MUST-FIX — the `Executor::capability_ceiling` field addition risks I6 drift.**
`workflow/yaml_compiler.hpp`'s `compile_executor()` already, deliberately, silently drops any YAML
field `Executor` doesn't declare (`yaml_compiler.hpp:80-83`'s own comment: "honestly dropped, not
fabricated"). Adding the field to the C++ struct without a matching YAML-parsing change in the SAME
pass reproduces exactly the "declarative and native surfaces diverge" hazard
`test_workflow_graph_validation.cpp:223-225`'s own decision 6 already exists to prevent — a
YAML-authored agent node's declared ceiling would silently vanish while the C++ `WorkflowBuilder` form
enforces it.

**MUST-FIX — nothing distinguishes a real agent-backed body from an ordinary function pretending to
be one.** `ExecutorBody` is a bare, uninspectable `std::function` (`workflow_supervisor.hpp:156-157`).
`initialize()` binds `bodies_[i]` to `graph_.executors[i]` purely by array position — nothing couples
a declared `kind` to what the callable actually does. Loosening `check_workflow_executable()` to
"accept `agent`-kind if there's a body and a ceiling" checks graph DATA, never the bound body itself,
so a caller can (by mistake, not even maliciously) satisfy the check with an ordinary function closure
that never touches an `AgentSession` at all — reopening the exact "silently run as a plain function"
hazard M6 built the blanket refusal to prevent in the first place, just moved one layer down.

**RESIDUAL, confirmed independently** (already named in the companion gap doc, re-confirmed by this
pass): `check_workflow_executable()`'s refusal path has zero test coverage in the real suite — the
comment in `test_workflow_graph_validation.cpp:176-178` points at a file
(`test_workflow_request_port.cpp`) that no longer exists; only `validate_workflow()`'s ACCEPTANCE of
the graph is tested, never the runtime REFUSAL.

## 3. What a real design has to resolve (not resolved here — this is the punch list, not the design)

1. **Checkpoint/resume story for agent-kind nodes**, one of two honest answers, not assumed: (a) real
   work — add `history_` serialization to `AgentSessionRecord`/the session store seam, and a real
   `bodies_`-to-checkpoint reconnection mechanism in `WorkflowSupervisor`; or (b) a documented,
   TESTED limitation — resuming a checkpointed run with agent-kind nodes starts those nodes with fresh,
   history-less sessions, proven by a real test that would fail if that silently changed. Either is
   acceptable; silently assuming composition (the first draft's mistake) is not.
2. **A structural answer to the concurrent-same-node hazard**, not a documentation-only warning: either
   (a) `check_workflow_executable()` (or `validate_workflow()`) statically rejects any graph where an
   agent-kind node is reachable by more than one non-fan_in edge converging in the same round — a real,
   checkable graph property; or (b) the bridge itself stops using the naive `drive()` resume loop for
   any body that can genuinely suspend, which needs a thread-aware waiter (a materially bigger change,
   name the cost honestly if this is the direction); or (c) `AgentSession::start_run()` gets a real
   per-call admission check that fails closed (not deadlocks, not races) on re-entrant concurrent
   invocation, proven by a real concurrency test under `rt::ThreadPool`, not `TestKit`-equivalent
   synchronous drivers (matching this project's own M6-era "forcing function" precedent for exactly
   this class of bug).
3. **`Executor::capability_ceiling` (or whatever the accepted capability-sourcing answer turns out to
   be) lands atomically with its `yaml_compiler.hpp` counterpart**, in the same change, with
   `check_workflow_executable()`'s new gate proven identical over both a C++-built and a YAML-compiled
   graph (I6, tested, not asserted).
4. **A structural marker distinguishing an agent-backed `ExecutorBody` from an ordinary one** — a
   tagged wrapper type `agent_session_as_executor_body()` returns and that binding an agent-kind slot
   requires (checkable via `std::function::target<T>()` or an explicit tagged variant), not a bare
   `std::function` plus a data-only check on the graph declaration.
5. **Fix the pre-existing test-coverage gap as a real, small, separately-landable prerequisite**: a
   test that actually builds a graph with an `agent`-kind executor, calls
   `WorkflowSupervisor::initialize()`/`run_workflow()`, and asserts `workflow_status::invalid` —
   closing the stale-comment gap named in §2's RESIDUAL finding. This does not depend on any of items
   1-4 above and can land on its own, independent of when (or whether) the rest of this design proceeds.

## 5. Resolving the §3 punch list — second red-team pass (2026-08-13)

Worked through all five items plus the original capability-sourcing question (from the research doc,
"MAF gives zero precedent for this"). Red-teamed once more before recording as resolved.

**Capability sourcing — reuse existing, unused infrastructure, don't invent new.**
`WorkflowSupervisor::initialize(Workflow graph, std::vector<ExecutorBody> bodies,
std::vector<EffectContext> contexts = {})` (`rt/workflow_supervisor.hpp:483-492`) ALREADY accepts a
per-executor-index `contexts` parameter — `contexts_.resize(graph_.executors.size())` — that nothing
currently populates meaningfully (the file's own comment says so directly). Confirmed by red-team:
only one read site (`:751`) and one write site (`initialize()` itself) exist for `contexts_` in the
whole file, nothing else assumes it stays default, and — unlike the ADR-028 `Backgroundable`+
`captures_session_state` hazard (a reference outliving its owner via a DETACHED thread) — `execute()`'s
dispatch loop always synchronously `.get()`s every job before the round returns
(`workflow_supervisor.hpp:758`), so a `contexts_[idx]`'s borrowed `capabilities`/`bound_capabilities`
pointer never outlives an ordinary synchronous call the way a detached-thread capture could. **Design:**
(a) `Executor` gains a static, graph-declared `capability_ceiling` field (mirroring
`Tool<...>::capability_ceiling`, I2's "declared statically" discipline) naming WHAT a node needs; (b)
the actual granted `CapabilitySet` a node's `AgentSession` runs with comes from whatever `EffectContext`
the CALLER populates into `contexts[i]` at `initialize()` time — the existing seam, finally used, no
new API surface; (c) `check_workflow_executable()` needs a genuinely NEW overload taking `contexts` too
(its current signature, `graph.hpp:431`, sees only the graph) to verify `contexts[i]`'s granted
capabilities actually satisfy `graph.executors[i].capability_ceiling` before accepting the graph as
executable — a real, small, additive signature change, not yet built. `initialize()`'s own comment
(`:475-482`) goes stale the moment this ships and needs to state the borrowed-pointer lifetime contract
explicitly (matching `effect_context.hpp:18,25`'s own inline statement of that contract).

**Item 1 (checkpoint/resume) — confirmed lower-risk than it looked, accept and test the limitation.**
Red-team confirmed directly: `restore_from_record()` never touches `bodies_`, and the only real
checkpoint-resume precedent (`test_rt_workflow_checkpoint_g2.cpp`) always resupplies fresh bodies to a
genuinely new `WorkflowSupervisor` instance before restoring — a stale `AgentSession&` inside an old
closure is structurally UNREACHABLE on resume, not merely unlikely. Resolution stands as originally
scoped: document and test that resuming a checkpointed run gives each `agent`-kind node a fresh,
history-less session (matching `AgentSessionRecord`'s own already-accepted no-`history_` gap,
`examples/12_session_checkpoint.cpp:139-143`), proven by a real test that fails if this silently changes
— not a design gap needing new serialization work.

**Item 2 (concurrent same-node hazard) — corrected: quarantine the specific hazardous delivery, don't
abort the whole round.** First resolution pass proposed failing the ENTIRE round closed the moment a
duplicate `executor_index` is found among a round's gathered `exec_deliveries`. Red-team found this is
**strictly harsher than the existing `broke` failure path**: today, when routing fails mid-round, EVERY
OTHER `exec_delivery` in that round still runs and gets recorded into `state_.partial`
(`workflow_supervisor.hpp:780-787`) — only downstream routing aborts. Aborting the whole round before
any dispatch would silently discard unrelated, unaffected nodes' legitimate work in the same round, and
none of the six existing `workflow_status` values (`:188-196`) honestly describes "nothing ran, a
duplicate delivery was detected." **Corrected design:** detect the duplicate at gather time (still
before dispatch, still evades the race — red-team confirmed `exec_deliveries` is built once, fully,
before any `pool_.submit()`, so a gather-time check cannot be evaded across the retry-attempt loop), but
instead of aborting the round, feed ONLY the hazardous entries into the SAME per-delivery
failure-outcome channel `JobOutcome`/`ExecuteReply` already uses for a real executor failure — i.e., the
duplicate deliveries fail individually (attributable to the specific node, a specific error code, e.g.
`workflow.duplicate_delivery_same_round`), routed through the EXISTING failure-policy/retry/fallback
machinery `tests/test_rt_workflow_supervisor_failure_policies.cpp` already proves works, while every
OTHER unrelated delivery in that round dispatches and completes normally. No new `workflow_status`
value needed — this reuses the executor-failure shape, not the whole-run-invalid shape.

**Item 3 (`Executor::capability_ceiling` + YAML atomicity) — unchanged from the first pass**, folded
into the capability-sourcing design above: the field, its YAML-compiler counterpart, and
`check_workflow_executable()`'s extended gate must land in the same change, proven identical over both
a C++-built and YAML-compiled graph (I6).

**Item 4 (structural marker for an agent-backed body) — FATAL in the first resolution, fixed.**
Wrapping `ExecutorBody` in a new `TaggedExecutorBody{ExecutorBody body; bool is_agent_backed;}` struct
"alongside" the existing type is not additive as scoped: red-team confirmed grepping every real call
site (`examples/04,09,10,13,14,15,16,17`, every `test_rt_workflow_supervisor*.cpp`) shows all of them
pass plain `std::vector<ExecutorBody>` to `initialize()` — introducing a second element type either
breaks `initialize()`'s signature for every existing caller, or gets stripped away before ever reaching
`bodies_[i]`, leaving nothing for the new gate to inspect. **Corrected:** don't change the vector's
element type at all. Give `agent_session_as_executor_body()`'s returned callable a FIXED,
non-templated functor type (independent of the adapter's own `ChatClientT`/`HistoryProviderT` template
parameters — e.g. a plain `AgentExecutorBodyTag` class with `operator()` matching `ExecutorBody`'s
signature), and check it via `std::function`'s own built-in type erasure:
`bodies_[i].target<AgentExecutorBodyTag>() != nullptr`. `std::vector<ExecutorBody> bodies` stays
byte-identical — zero existing call site changes. **Named caveat, not yet closed:** `.target<T>()` has
ZERO precedent anywhere in this codebase (confirmed by grep across `include/`/`src/`) — it needs its
own positive-control test (confirm it correctly returns non-null for a real tagged body AND null for an
ordinary one) before being trusted as an enforcement gate, not assumed correct by C++ standard-library
reputation alone.

**Item 5 (test-coverage gap fix)** — unchanged, still independently landable, no design dependency.

## 6. What survives from the first draft, unchanged

The core mechanism shape — a standalone adapter function driving `AgentSession::start_run()` via the
same `drive<T>()` bridge already proven for `ChatClient::chat()`, requiring no change to
`rt/workflow_supervisor.hpp`'s general execution model — is NOT rejected, only its safety envelope
needs the fixes in §3. MAF's own "one session per run, reused across turns within it" shape (§1, from
the research doc) also survives; §3 item 2 constrains WHEN that shared session can safely be reached
concurrently, it doesn't reject session reuse itself.
