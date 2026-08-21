# Dynamic multi-agent fan-out — going past MAF's fixed-roster background agents

**Status:** Gap analysis + first design sketch. **Not an ADR, no code written.** Per the project's
standing discipline for this class of doc (`agent-as-workflow-executor-gap.md`,
`batch-inference-coalescing-gap.md`): document the gap and a candidate design, do not implement.
Written from an explicit user direction (2026-08-21): MAF's `BackgroundAgentsProvider` "seems only good
for well defined flow with known list of agents" — the ask is a primitive closer to a host-authored
script that fans out over a runtime-computed work list, the shape this session's own orchestration
tooling (`Workflow`) already demonstrates, not a fixed pre-registered agent roster.

**Design draft, red-teamed once:** `dynamic-multi-agent-fanout-design-draft.md`. A same-session
adversarial pass, triggered by an explicit project-owner concern ("we allow consumer dev will handle
risk instead of let engine handle all things"), found the Primitive 1 sketch below LOOKS I2-compliant
(capabilities/budget/principal are explicit parameters) but was actually still caller-discretion in
practice on three FATAL axes: a `CapabilitySet` parameter with no engine-side ceiling against the
parent's own grant, a caller-fabricated `Principal` that breaks I4 attribution, and a `SessionFactory`
contract that could reintroduce the exact concurrent double-resume race
`agent-as-workflow-executor-design-draft.md` already found once for the static-graph case — plus two
MUST-FIX findings (budget and fan-out count are both caller-checked, not engine-enforced, the latter
also an I3 concern if the count traces back to model output). Read that doc before treating the sketch
below as anything but a starting point the red-team pass already moved past.

**Prior research this draft builds on:** `docs/research/2026-08-21-maf-background-execution-concepts.md`
(MAF's four long-running-work primitives, none of which is this), `docs/research/2026-08-21-maf-provider-list.md`,
`agent-as-workflow-executor-gap.md` + `agent-as-workflow-executor-design-draft.md` (the closest existing
AgentEngine design surface — embedding ONE agent as ONE node in a statically-declared `Workflow` graph;
red-teamed twice, still unimplemented), `batch-inference-coalescing-gap.md` (N independent one-shot
calls sharing a vendor batch job — a different axis, orthogonal to this one).

## Naming the actual gap precisely

Three distinct "more than one agent, not fully synchronous" shapes exist or are proposed. Conflating
them is the mistake worth avoiding up front:

| Shape | Node/agent count | Who decides fan-out | AgentEngine status |
|---|---|---|---|
| Static workflow graph (`014`) | Fixed at graph-authoring time (C++ `WorkflowBuilder` or YAML) | The graph author, ahead of time | Declared/validated today; `executor_kind::agent` (an agent AS a graph node) is designed but unimplemented — see `agent-as-workflow-executor-*` |
| MAF's `BackgroundAgentsProvider` | Task count unbounded, but the **agent roster is fixed** (`[webSearchAgent, codeAnalysisAgent]`, bound when the parent agent is constructed) | The **model**, via `background_agents_start_task` tool calls, choosing among the pre-registered names | No AgentEngine equivalent; not proposed here as the target shape either — see "Why not just port MAF's shape" below |
| **This gap**: dynamic fan-out | Computed at **run time** from data (N files in a diff, N search results, N workflow items) — unknown at authoring time, unbounded by any pre-registered roster | **Host-authored, deterministic control flow** (a loop/`pipeline`/`parallel` over a runtime list), not the model | Does not exist in any form |

The third row is what "dynamic workflow like Claude have" names: `Workflow`'s own `agent()` /
`parallel()` / `pipeline()` primitives, in this very session's toolset, spawn a variable, runtime-
computed number of subagents from a deterministic script, not from a fixed registered list and not from
model-decided delegation.

## Why not just port MAF's shape (fixed roster, model-decided dispatch)

Named directly since it's the natural first instinct and the user already flagged it as insufficient:

1. **A fixed roster can't express "one reviewer per changed file" or "one summarizer per search hit"**
   — the whole class of workload the user is pointing at — because the agent identities have to be
   pre-registered before the count of files/hits is known.
2. **Model-decided dispatch sits at exactly the seam I3 is strictest about.** MAF's
   `background_agents_start_task` is a tool the MODEL calls to decide what work happens and how much of
   it. That is fine under AgentEngine's own rules too — I3 says model output is never *authority* over a
   *permission* decision, not that a model can never trigger a tool call; every tool call already flows
   through the same capability-gated tool pipeline regardless of who invoked it. But it does mean a
   MAF-shaped port inherits an open-ended, model-paced fan-out count with no natural place to enforce
   I8 (budgets) except after the fact, per-task. A host-authored `pipeline()`/`parallel()` script, by
   contrast, computes its fan-out count from DATA the host already has (`items.length`), so a budget
   ceiling (I8) can be checked BEFORE dispatch, not discovered by watching the model spend it.
3. **MAF's roster is registered once, at agent-construction time** (`ChatClientAgentOptions.AIContextProviders`)
   — a static binding, not meaningfully more dynamic than `014`'s own static graph, just phrased as
   tools instead of graph edges.

None of this means model-decided delegation is worthless — it is a real, different, smaller feature
(closer to "let the model choose which of a few known specialist agents to consult"). It is just not
the shape asked for here, and conflating the two would produce a design that satisfies neither well.

## Two complementary primitives, not one

The design splits along the same line RFC 014 §7 and 009 already draw elsewhere in this codebase: a
**native C++ surface** for code that wants full runtime dynamism, and a **declarative surface** for the
subset of that dynamism that can be expressed as data — because I6 ("declarative and native surfaces are
equivalent") and the locked decision ("v1 authoring surfaces are C++ CRTP and declarative YAML/JSON...
no scripting layer") both apply here and a naive "just embed something like the `Workflow` tool's JS
runtime" answer would violate both at once.

### Primitive 1 — native: a `multi_agent` helper library over `AgentSession`

C++ is already Turing-complete and already a first-class authoring surface (the locked-decision list
says so directly) — an ordinary `for` loop already IS dynamic fan-out. What's actually missing is not
dynamism, it's a *convenience/safety* layer so every caller doesn't hand-roll the same
`ThreadPool::submit()` + `drive<T>()` + capability-threading boilerplate, with the same chances to get
I1/I2/I8 wrong each time. Proposed shape, modeled directly on `Workflow`'s own `agent()`/`parallel()`/
`pipeline()` but as ordinary C++ functions over real, existing primitives:

```cpp
namespace agentengine::rt::multi_agent {

// One dynamically-spawned child: a fresh AgentSession, minted from a caller-supplied factory (not a
// pre-registered roster), with an EXPLICIT capability subset -- never the parent's ambient set (I2).
template <class ChatClientT, class HistoryProviderT>
[[nodiscard]] task<result<AgentResponse>> spawn(
    SessionFactory<ChatClientT, HistoryProviderT> const& factory,   // how to build ONE child session
    StartRun request,
    CapabilitySet capabilities,        // explicit grant for THIS child -- not inherited (I2)
    Principal attributed_to);          // I4: whose spend/effects this is, for provenance + budget

// Barrier: run N spawn()-shaped thunks concurrently via a caller-supplied rt::ThreadPool, await all.
// Mirrors Workflow's parallel() semantics exactly: a thunk that faults resolves to an error in its
// slot, the call itself never throws -- caller filters.
template <class T>
[[nodiscard]] task<std::vector<result<T>>> parallel(
    ThreadPool& pool, std::vector<std::function<task<result<T>>()>> thunks);

// No-barrier pipeline: each item flows through stage1, stage2, ... independently -- item A can be in
// stage 3 while item B is still in stage 1 (mirrors Workflow's pipeline() over-barrier default).
template <class Item, class... Stages>
[[nodiscard]] task<std::vector<result<...>>> pipeline(
    ThreadPool& pool, std::vector<Item> items, Stages&&... stages);

}  // namespace agentengine::rt::multi_agent
```

This is a **library, not a new runtime primitive** — every building block it needs already exists and is
proven: `ThreadPool::submit(task<void>) -> std::future<JobOutcome>` (`rt/thread_pool.hpp:179`),
`AgentSession::start_run()` (`rt/agent_session.hpp:760`, `rt::task<result<AgentResponse>>`), the
`drive<T>()` bridge already proven in `examples/16_group_chat_live.cpp:54-58`. The one genuinely new
piece is threading an explicit, per-spawn `CapabilitySet` and `Principal` through — mirroring the
capability-sourcing answer `agent-as-workflow-executor-design-draft.md` already worked out for the
static-graph case (item i's grant comes from what the CALLER populates, never from ambient scope or
from anything the model wrote).

**Budget (I8):** unlike MAF's model-paced dispatch, `parallel()`/`pipeline()`'s caller knows the fan-out
count (`items.size()`) before a single child spawns — a budget ceiling check belongs at the call site,
before dispatch, not as a post-hoc watch. This wants a real `Budget` type (`total`, `spent()`,
`remaining()`, matching `Workflow`'s own `budget` global almost exactly) that `spawn()` debits as each
child's `AgentResponse::usage` comes back — genuinely new, not reused from anywhere in this codebase
today (grep confirms no existing cross-session budget-pool type).

**What this does NOT need to solve that the static-graph design already had to:** no checkpoint/resume
story is owed here at the SAME layer — a `multi_agent::parallel()` call is an ordinary (if long-running)
C++ function call inside whatever `AgentSession`/`WorkflowSupervisor`/tool body invoked it; if THAT outer
context is checkpointed, the fan-out just re-runs from scratch on resume, same as any other
non-`Backgroundable` tool body today. Naming this explicitly rather than assuming it, per this project's
own "an honest documented limitation beats a silently assumed one" pattern from the static-graph draft's
item 1.

### Primitive 2 — declarative: a scatter/gather `Executor` modifier, not a new graph shape

I6 does not require every native capability to have a declarative twin on day one (RFC 014's own
`executor_kind` enum grew incrementally) — but a purely-native `multi_agent` library would leave YAML-
authored workflows permanently unable to express "one agent per upstream item," which is a real, durable
gap worth naming even if it's not built now. The shape that stays data (not a scripting layer): a new,
optional `Executor` field —

```cpp
struct ScatterGather {
    std::string over_field;      // JSON-pointer-style path into the upstream payload naming an array
    std::size_t max_concurrency; // caller-declared ceiling, not the pool's whole worker count by default
};
std::optional<ScatterGather> scatter_gather;  // on Executor, alongside the existing capability_ceiling
```

At runtime, `WorkflowSupervisor` would expand ONE `agent`-kind node with a populated `scatter_gather`
into N invocations of the SAME declared node body, one per element of `over_field`'s array, gathering N
results back into one array before the node's outgoing edges fire — the classic map/scatter-gather
operator, expressible as plain graph + field data (an integer/string + a field name), never as arbitrary
code. This is deliberately narrower than `multi_agent::pipeline()`'s full generality (no multi-stage
per-item pipeline, no per-item distinct prompts beyond templating `over_field`'s element into the node's
existing prompt-construction path) — matching this project's own "narrower than the full ask, not
silently unscoped" discipline from the batch-coalescing gap doc's own closing section.

**This depends on `executor_kind::agent`'s runtime bridge landing first** (`agent-as-workflow-executor-*`,
still unimplemented) — scatter/gather over a node that can't yet run at all is not independently
buildable; sequencing this AFTER that work, not in parallel with it, is a real scheduling constraint,
not just tidiness.

## Open design questions a future ADR needs to answer (not answered here)

1. **Does `spawn()`'s child get its own worktree/sandbox scope**, matching `ADR-032`'s per-workflow-
   executor worktree scoping, or does a dynamically-spawned child share the parent's? Unlike a static
   graph node (whose worktree-scoping question ADR-032 already resolved), a runtime-computed fan-out
   count means the number of worktrees needed is also runtime-computed — a real resource-provisioning
   question, not just a policy one.
2. **Cross-child failure policy.** `Workflow`'s own `parallel()` resolves a faulted thunk to `null` in
   its slot rather than aborting the whole barrier. Does `multi_agent::parallel()` copy that (partial
   results, caller filters) or does AgentEngine's own existing `failure_policy`/retry/fallback machinery
   (`test_rt_workflow_supervisor_failure_policies.cpp`) get reused instead — and if reused, does it
   compose cleanly outside a `WorkflowSupervisor`-owned round, since `spawn()` is proposed as usable from
   plain tool-body code, not only from inside a workflow executor?
3. **The `Budget` type's scope.** Per-call (one `parallel()` invocation), per-session, per-run, or
   nestable (a `parallel()` inside a `parallel()`'s child, matching `Workflow`'s own one-level-nesting
   rule for its `workflow()` sub-step) — and how does it interact with any EXISTING per-session/per-run
   cost tracking this codebase already has, if any (not verified in this pass — a real prerequisite check
   before designing the type, not assumed absent).
4. **`ScatterGather`'s YAML-compiler counterpart** must land atomically with the C++ field, exactly the
   same I6 discipline `agent-as-workflow-executor-design-draft.md`'s item 3 already establishes for
   `capability_ceiling` — named here so it isn't rediscovered as a surprise later.
5. **Attribution when a scatter/gather child fails independently of its siblings** — does one failed
   element fail the whole gathered array (matching a batch job's per-`custom_id` result shape, where
   failure is per-item, not per-job), or does a failed element propagate as a per-element `error` inside
   an otherwise-successful gathered result? The batch-coalescing gap doc's own question 3 (fan-out result
   shape) is the closest existing precedent to reuse rather than re-derive.

## What NOT to design around

An embedded general-purpose scripting layer (JS-like, matching what the `Workflow` tool itself runs on)
is explicitly out of scope — it would directly reopen the locked "v1 authoring surfaces are C++ CRTP and
declarative YAML/JSON... Python/.NET bindings deferred" decision, which a change of this size cannot
relitigate implicitly by architectural drift. Primitive 1 gets the same *dynamism* by being ordinary
C++ (already Turing-complete, already a first-class surface); Primitive 2 gets the same dynamism for the
declarative surface by staying a bounded, named operator (map-over-array) rather than arbitrary code —
neither needs a new language runtime.
