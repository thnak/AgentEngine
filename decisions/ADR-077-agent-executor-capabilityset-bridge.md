# ADR-077 — An agent-executor's `CapabilitySet` comes from `WorkflowSupervisor::initialize()`'s per-node `contexts`, and `executor_kind::agent` is now real

**Status:** Judged (2026-08-23, approved by project owner — explicit, dated instruction to lift the
2026-08-13 "document only, do not implement yet" direction and proceed to implementation this
session). Design → red-team (twice) → prove, all complete. **Resolves `OpenQuestions.md` OQ-19.**

**Relates to:** `decisions/ADR-070-host-configurable-responsibility-boundary.md` (its own §7 named
the direction this ADR would resolve toward, as a cross-reference, not a resolution — see that ADR's
own text and OQ-19's note); `007-Capability-and-Trust-Model.md` (I2/I3, `CapabilitySet`);
`014-Workflow-and-Orchestration.md` §1/§3/§7 (`executor_kind::agent`, orchestration patterns, I6).
Design record, kept as the full uncompressed history including the FIRST draft's two FATAL findings:
`docs/planning/agent-as-workflow-executor-design-draft.md`. Gap analysis and MAF precedent research:
`docs/planning/agent-as-workflow-executor-gap.md`, `docs/research/2026-08-13-maf-agent-as-workflow-executor.md`.

## 1. The question

`executor_kind::agent` (014 §3/§7) was real at the graph-declaration layer but structurally refused
at execution — `check_workflow_executable()` (`workflow/graph.hpp`) rejected it outright, because no
runtime bridge wrapped a real `AgentSession` as a workflow node, and — the harder question — nothing
said where that node's `CapabilitySet` would come from. MAF's own shipped design (the project's
developer-model prior art) gives zero precedent: it has no in-process capability/authority system
analogous to `CapabilitySet`/`EffectContext::capabilities` at all. A future ADR had to decide: does an
agent-executor's capability ceiling come from the workflow's own declaration, a per-executor
binding-time grant, or something derived from the invoking run's principal?

## 2. Non-negotiable constraints (not themselves contested)

- **I2 (no ambient authority):** an agent-kind node must receive its `CapabilitySet` explicitly, from
  a caller-populated seam — never inferred from the graph, the model, or an ambient default.
- **I3 (model output is never authority):** nothing about which capabilities an agent-kind node runs
  under may be derived from anything the model itself produced.
- **I6 (declarative and native surfaces are equivalent):** whatever new field/gate this design adds
  must not silently diverge between the C++ `WorkflowBuilder` form and the YAML-compiled form.
- **ADR-070 §7's own steer, not a resolution:** the eventual answer should be a per-executor
  binding-time grant, attenuation-bounded by the workflow's own declared ceiling — closer to the
  Delegated Decision Seam pattern than to a principal-derived (ambient-authority-adjacent) design.

## 3. Why a second red-team pass happened before any code existed

The first draft (`agent-as-workflow-executor-design-draft.md` §1) looked small: "no core-seam change
needed, only `check_workflow_executable()`'s blanket refusal needs to loosen." An independent
adversarial pass found this **false in two places that matter, both FATAL as originally scoped**:

1. **Checkpoint/resume would silently lose the session, or the whole conversation.**
   `WorkflowSupervisor::restore_from_record()` never touches `bodies_` — a resumed run gets
   caller-resupplied fresh bodies, matched to `graph_.executors` purely by array index. The first
   draft's whole justification for reusing one `AgentSession` across a cyclic node's repeated visits
   (accumulated history) evaporates the moment that run is ever checkpointed and resumed.
2. **A genuine concurrent double-resume race.** Two ordinary (non-`fan_in`) edges converging on the
   SAME agent-kind node in one round produce two separate `Delivery` entries, submitted concurrently
   onto different worker threads. If both closures capture the same `AgentSession&`, both call
   `start_run()`, which genuinely parks a contended waiter and resumes it from a DIFFERENT thread — a
   concrete double-resume race against the naive "resume until done" drive loop this design's bridge
   needs, not a hypothetical one.

Plus two MUST-FIX findings (a bare `ExecutorBody` has no structural way to tell an agent-backed body
from an ordinary function closure; a new `capability_ceiling` field risks I6 drift if its YAML
counterpart doesn't land in the same change) and one RESIDUAL (the runtime-refusal path had zero test
coverage — a stale comment in `test_workflow_graph_validation.cpp` pointed at a file that no longer
exists). Full findings: design draft §2.

## 4. The resolved design (second red-team pass, §5 of the design draft)

**Capability sourcing.** `WorkflowSupervisor::initialize(graph, bodies, contexts)` already accepted a
per-executor-index `contexts` parameter that nothing meaningfully populated. Resolved:
(a) `Executor` (`workflow/graph.hpp`) gains a static, graph-declared `capability_ceiling` field
(`std::vector<agentengine::Capability>`), mirroring `Tool<...>::declared_capabilities()`'s own
empty-by-default shape — naming WHAT a node needs; (b) the actual GRANTED `CapabilitySet` a node's
`AgentSession` runs under comes from whatever `EffectContext` the caller populates into
`contexts[i]` at `initialize()` time — the existing, previously-unused seam; (c)
`check_workflow_executable()` gained a genuinely new overload taking `contexts` too, verifying
`contexts[i]`'s granted capabilities satisfy `graph.executors[i].capability_ceiling` before accepting
the graph as executable.

**Checkpoint/resume — accepted and tested limitation, not a design gap.** `restore_from_record()`
never touches `bodies_`; a resumed run gets whatever fresh `AgentSession` the caller binds via a fresh
`agent_session_as_executor_body()` call. History does NOT survive a checkpoint/resume cycle — the same
already-accepted gap `AgentSessionRecord` itself has (no `history_` field). Proven directly by T8
(§6 below), not merely documented.

**Concurrent-same-node hazard — quarantine the specific hazardous delivery, not the whole round.**
Detected at gather time, before any dispatch (`exec_deliveries` is built once, fully, so this cannot
be evaded across the retry-attempt loop): the second-and-later `Delivery` this round targeting the
SAME agent-kind `executor_index` is synthetically failed (`contract`-class, never dispatched, never
retried), routed through the EXISTING failure-policy/retry/fallback machinery exactly like any other
real executor failure — not a new whole-round-abort mechanism. The FIRST delivery still runs
normally, and every OTHER unrelated delivery in the round is unaffected.

**Structural marker.** `AgentExecutorBodyTag` — a fixed, non-templated functor type (independent of
the adapter's own `ChatClientT`/`StateT`/`HistoryProviderT`) that `agent_session_as_executor_body()`
returns and that `WorkflowSupervisor::initialize()` requires for every agent-kind slot, checked via
`std::function`'s own type erasure (`bodies_[i].target<AgentExecutorBodyTag>() != nullptr`) — not a
data-only check on the graph's declared `kind`, which a caller could satisfy by mistake with an
ordinary function closure.

**YAML atomicity.** `Executor::capability_ceiling` has no YAML parser yet — parsing a capability LIST
from YAML needs the same per-kind registry work `agent_yaml_compiler.hpp`'s own `spec.capabilities`
already documents as a separate, not-yet-built gap. Rather than silently dropping an authored
`capability_ceiling:` key (this compiler's own established convention for a field `Executor` doesn't
declare), `compile_executor()` refuses it loudly with `yaml_compiler.executor_capability_ceiling_unsupported` — a refused graph is recoverable, a quietly reinterpreted one is not. The
empty-ceiling case (no key at all) compiles identically on both surfaces (G9, §6).

## 5. What was rejected

- **Principal-derived capability sourcing** (an agent-kind node's ceiling comes from the invoking
  run's own principal) — rejected per ADR-070 §7's own steer: closer to ambient authority than a
  host-explicit grant, and gives a workflow author no way to express "this specific node needs less
  than the run's principal generally holds."
- **A `TaggedExecutorBody{ExecutorBody body; bool is_agent_backed;}` wrapper struct** "alongside" the
  existing `ExecutorBody` type — FATAL as scoped: every real call site
  (`examples/04,09,10,13,14,15,16,17`, every `test_rt_workflow_supervisor*.cpp`) passes a plain
  `std::vector<ExecutorBody>` to `initialize()`; introducing a second element type breaks every
  existing caller's signature. Fixed via `AgentExecutorBodyTag` + `std::function::target<T>()`
  instead, which changes zero existing call sites.
- **Aborting the whole round on a detected duplicate delivery** — rejected as strictly harsher than
  the existing `broke` failure path: today, when routing fails mid-round, every OTHER
  `exec_delivery` in that round still runs and gets recorded; aborting before any dispatch would
  discard unrelated, unaffected nodes' legitimate work in the same round.
- **A defaulted `Executor::operator==`** once `capability_ceiling` was added — `agentengine::Capability`'s payload structs have no `operator==` of their own, so a defaulted comparator would have
  been silently DELETED, taking `Workflow`'s own defaulted comparator (014 §7's diffing property)
  down with it. Fixed with a hand-written `Executor::operator==` that compares every pre-existing
  field and excludes `capability_ceiling` (named in-code, not silently gapped) rather than adding
  `operator==` to every `cap::*` struct in the shared, foundational `trust/capability.hpp` — a larger,
  separate change this ADR does not need.

## 6. Falsifiable claims and proof

Implemented across `include/agentengine/workflow/graph.hpp` (new `capability_ceiling` field +
contexts-aware `check_workflow_executable()` overload + hand-written `Executor::operator==`),
`include/agentengine/workflow/yaml_compiler.hpp` (explicit refusal), `include/agentengine/rt/workflow_supervisor.hpp` (`AgentExecutorBodyTag`, the structural-marker check in `initialize()`, the
duplicate-delivery quarantine in `execute()`), and a new `include/agentengine/rt/agent_workflow_executor.hpp` (`agent_session_as_executor_body()`, driven via the same hand-rolled "resume until
done" loop `examples/16_group_chat_live.cpp` already established — deliberately NOT `rt::drive_leaf_task()`, whose own top comment names this exact `AgentSession::session_mutex_` cross-thread-resume
hazard as why its `synchronous_leaf` contract excludes `AgentSession`).

| # | Claim | Verdict | Basis |
|---|---|---|---|
| P1 | The graph-layer gate (contexts-aware `check_workflow_executable`) accepts a satisfied ceiling, rejects an unsatisfied or absent one, and leaves `sub_workflow` refused unconditionally. | **CORRECT** | `tests/test_workflow_agent_executor_gate.cpp` G1–G6, all passing. |
| P2 | `TypedExecutor`'s `capability_ceiling` escape hatch round-trips through `describe()`, matching `worktree_mode`'s own precedent. | **CORRECT** | G7. |
| P3 | The YAML compiler refuses an authored `capability_ceiling:` key loudly (never silently drops it), and is unaffected — identical to the C++ default — when the key is absent (I6). | **CORRECT** | G8, G9. |
| P4 | `AgentExecutorBodyTag`'s structural marker is a real positive control: non-null for a genuinely agent-backed body, null for an ordinary function closure satisfying the same call signature. | **CORRECT** | `tests/test_rt_agent_workflow_executor.cpp` T1. |
| P5 | A graph declaring an agent-kind node bound to a non-agent-backed body refuses to run (`workflow_status::invalid`) rather than silently executing it as a plain function — closing the stale-comment test-coverage gap the first red-team pass found. | **CORRECT** | T2. |
| P6 | A real end-to-end dispatch through a scripted `AgentSession` produces that session's own response as the workflow's output. | **CORRECT** | T3. |
| P7 | An unsatisfied `capability_ceiling` refuses the run before it starts (`initialize()`-time, not a race discovered mid-run). | **CORRECT** | T4. |
| P8 | An agent-kind node revisited across rounds WITHIN one run reuses the SAME `AgentSession`, accumulating real conversation history — proven by history length, not merely asserted. | **CORRECT** | T5. |
| P9 | Two non-`fan_in` edges converging on the same agent-kind node in one round dispatch the underlying `AgentSession` exactly ONCE — never a concurrent double-call — under BOTH the default `fail` policy (round fails cleanly, no hang/crash) and a `propagate` policy (round completes normally, the duplicate's marker is silently absorbed by the pre-existing same-round `deliver_once()` dedup, and the survivor's REAL payload reaches the downstream sink) — the design's own central "every OTHER delivery completes normally" claim, proven under two distinct failure policies, not asserted. | **CORRECT** | T6 (`fail`), T7 (`propagate`). |
| P10 | Checkpoint/resume gives a resumed run's agent-kind node a fresh, history-less `AgentSession` — the pre-checkpoint session's conversation does not carry over. | **CORRECT** | T8: `session2.history().size() == 2` (only the post-resume exchange), against a `session1` that independently shows `history().size() == 2` for its own single pre-checkpoint exchange. |

**Regression check:** `test_rt_agent_workflow_executor.exe` (13 checks) and
`test_workflow_agent_executor_gate.exe` (13 checks) both built and ran clean, standalone, against the
real headers (MSVC 19.51, `/std:c++latest`) — full `ctest` was not re-run this session (this build
tree has `AGENTENGINE_WITH_HTTPS=OFF`, which also gates the pre-existing default-suite block these two
new tests were added alongside, so neither the new tests nor several pre-existing ones in the same
block — e.g. `test_workflow_graph_validation` — currently register as CTest targets in THIS
particular build cache; both new executables were built and run directly instead, and the CMakeLists
entries mirror the exact working pattern of their neighbors in the same block).

## 7. Residual risks and deferred work (not closed by this ADR)

- **`sub_workflow` stays unimplemented** — out of OQ-19's scope, which was specifically about
  `CapabilitySet` sourcing for `agent`-kind nodes.
- **`Capability` payload structs still have no `operator==`** — `Executor::operator==` works around
  this by hand-comparing every field except `capability_ceiling`; a future change wanting real
  structural diffing over declared capability ceilings needs to add real equality to
  `trust/capability.hpp`'s `cap::*` structs first (a separate, contained change).
- **YAML authoring of `capability_ceiling` remains unbuilt**, refused loudly rather than silently
  dropped — real follow-on work, needing the same per-kind capability registry
  `agent_yaml_compiler.hpp`'s own `spec.capabilities` gap already names.
- **An agent-kind node cannot select a `switch_case`/`multi_selection` route** — `AgentResponse` has
  no routing concept, so `agent_session_as_executor_body()`'s `ExecutorOutcome` always carries empty
  `routes`. A real "model output selects a route" capability (which would need to cross I3's boundary
  carefully — a route label is engine-checked structure, never free-form model output to trust
  directly) is separate, not-yet-built work.
- **Full project regression (`ctest`) was not re-run this session** — see §6's build-cache note. The
  two new suites were verified directly; a future session with an HTTPS-enabled build should confirm
  they also register and pass through the normal CTest path.
- **Concurrent-same-node quarantine is scoped to `agent`-kind executors only** — an ordinary
  `function`-kind node receiving two concurrent same-round deliveries is unchanged, pre-existing
  behavior (no known hazard there; out of this ADR's scope to touch).
