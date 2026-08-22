# Design draft: `multi_agent::spawn()`/`parallel()`/`pipeline()` — red-teamed once

**Status:** Design draft, red-teamed once (below) — **not an ADR, no code written**. Companion doc:
`dynamic-multi-agent-fanout-gap.md` (gap analysis, the two-primitive split, Primitive 1's original API
sketch, and the five open design questions this pass resolves some of). Matches the same
design → red-team → corrected-design shape `agent-as-workflow-executor-design-draft.md` already
established for the static-graph case.

## Why this red-team pass happened before any code exists

Explicit project-owner direction (2026-08-21): the gap doc's Primitive 1 sketch — `spawn(factory,
request, CapabilitySet capabilities, Principal attributed_to)` — LOOKS I2-compliant on the page
(capabilities are an explicit parameter, not ambient), but the concern raised was that safety was
still phrased as "the calling developer is responsible for X," which is exactly the "convenient-looking
change that breaks I2" pattern `CLAUDE.md` itself warns is the easiest way to go wrong. An independent
adversarial pass (fresh context, real file:line citations, no prior exposure to this doc) confirmed the
concern is real: **three FATAL findings**, all following the same shape — a parameter that *looks*
explicit but has no engine-side check tying it back to what the caller actually held.

## Red-team findings

**FATAL — `CapabilitySet capabilities` has no engine-side ceiling; a careless caller can grant a child
capabilities the parent never held.** `CapabilitySet::grant_root(std::vector<Capability> caps)`
(`include/agentengine/trust/capability.hpp:622-626`) mints a root grant from *any* vector, with no
reference to any existing session's own grant — and the gap doc's sketch takes a bare `CapabilitySet`
value, so nothing stops a dev from writing `spawn(factory, req, CapabilitySet::grant_root({cap::FsWrite{...}}),
attributed_to)` because it's the shortest thing that compiles. Every real engine-side capability path in
this codebase already refuses a bare caller-supplied grant this way: `apply_dispatch_authority`
(`include/agentengine/rt/agent_session.hpp:1315-1337`) sources `effect_context_.capabilities` from
exactly two engine-controlled places (a dispatcher-resolved `RequestAuthority`, or the session's own
`capabilities_` at construction) — never a free function parameter. `CapabilitySet::attenuate()`
(`capability.hpp:694-704`) is the ONLY mechanism that derives a narrower set while failing closed if a
requested capability "isn't subsumed by something in this [parent] set," and the draft doesn't route
through it.

**FATAL — `Principal attributed_to` is caller-fabricated; breaks I4 attribution.** The existing,
shipped `start_background_task()` (`agent_session.hpp:1163-1213`) never takes a `Principal` from its
caller at all — `owner_principal_id` is derived internally from `effect_context_.principal.id`
(`:1187`), itself only ever set by `apply_dispatch_authority()` from a dispatcher-resolved
`RequestAuthority::principal` or the session's own `principal_` (`:1328,1334`). `start_run()`'s
`require_authority_` branch additionally gates a delegated principal through `principal_admitted_for()`
(`:776-780,782-784`, backed by `trust/principal.hpp:122`) before trusting it. The draft's plain
`Principal attributed_to` parameter inverts all of this — nothing rejects a caller mislabeling a
child's spend/effects onto an unrelated principal.

**FATAL — a `SessionFactory` that returns a shared/reused session recreates the exact double-resume
race this project already found once, one layer down.** `agent-as-workflow-executor-design-draft.md:58-74`
documents this bug class for the static-graph case: two concurrent deliveries sharing the same
`AgentSession&` both call `start_run()` (`agent_session.hpp:760`), which does `co_await
session_mutex_.lock()` — a real `AsyncMutex` that genuinely parks a waiter and resumes it from a
*different thread's* `unlock()`, breaking `drive<T>()`'s naive `while(!t.done()) t.resume();` loop
precondition ("nothing it drives genuinely parks"). `multi_agent::parallel()` proposes exactly the
shape that reopens this: N `spawn()`-shaped thunks through a caller-supplied `SessionFactory`, submitted
concurrently via `ThreadPool::submit()` (`rt/thread_pool.hpp:179`). If `SessionFactory` ever memoizes or
returns a shared reference — an easy, natural mistake ("a fresh session per item felt wasteful") — the
identical race recurs.

**MUST-FIX — the Budget check is caller-discretion (the draft's own words), so I8 isn't actually
enforced.** The gap doc states the check "belongs at the call site, before dispatch" — literally
caller-discretion phrasing. Real engine-enforced precedent exists and wasn't followed:
`token_budget_` (`agent_session.hpp:567-571`) is checked automatically inside `run_rounds()` after every
round and fails closed (`run.token_budget_exceeded`, `:1802-1807`) — the caller never has to remember
anything. `Background<max_concurrent>` is likewise checked engine-side against a live count
(`core/tool_pipeline.hpp:693-702`), never trusted to a caller's promise. `parallel()`/`pipeline()` as
drafted have no equivalent gate in their own signatures — nothing stops calling
`multi_agent::parallel(pool, thunks)` with zero budget object anywhere in scope.

**MUST-FIX (I3) — unbounded/model-influenced fan-out count has no engine-side ceiling; same root cause
as the Budget finding.** `items.size()` / `over_field`'s array length directly sets fan-out count.
`ScatterGather::max_concurrency` was captioned "caller-declared ceiling" — again a value nobody checks
against a capability grant. If the array populating `items`/`over_field` traces back to a prior tool
result that echoes model output, the model has de facto chosen the spawn count, capability-mint count,
and aggregate spend, with no engine-side comparison against a ceiling before dispatch. The gap doc's own
critique of MAF ("inherits an open-ended, model-paced fan-out count with no natural place to enforce I8
except after the fact") applies unmodified here — the difference is only *who* computed the count, not
whether a ceiling exists. Same structural fix as the Budget finding closes both cases at once, since a
mandatory pre-dispatch ceiling doesn't care where the count came from.

**Residual, decided now rather than left open — worktree/sandbox scoping default.** The gap doc's open
question 1 left this unresolved. `ADR-032` (`decisions/ADR-032-workflow-executor-worktree-scoping.md`
§4) already resolved the structurally analogous question for static graph nodes: default `worktree_mode`
is `branch` (isolated sub-worktree) **unconditionally**, explicitly rejecting "default to shared,"
because cycles/dynamic routing make it unsound to statically prove two nodes never co-occur in a round.
A runtime-computed fan-out count is *strictly less* provable-safe than that — the number and identity of
concurrent children isn't even known until the array is read — so ADR-032's own reasoning already
dictates the fail-closed default here too, not a fresh judgment call.

## Corrected design

1. **`spawn()` takes the parent session and a narrowing request, not a bare `CapabilitySet`:**

   ```cpp
   template <class ChatClientT, class HistoryProviderT>
   [[nodiscard]] task<result<AgentResponse>> spawn(
       AgentSession<ChatClientT, HistoryProviderT>& parent,
       SessionFactory<ChatClientT, HistoryProviderT> const& factory,  // MUST return a fresh, owned
                                                                       // session -- see fix 3
       StartRun request,
       std::vector<Capability> const& narrower_grant);  // checked via parent.capabilities()->
                                                          // attenuate(narrower_grant) -- fails closed
                                                          // if not a subset of what parent itself holds
   ```

   `attenuate()`'s existing fail-closed behavior (`capability.hpp:694-704`) makes "child exceeds parent"
   a runtime-impossible case instead of a documentation note — the same discipline
   `apply_dispatch_authority` already applies to every other capability-resolution path in this codebase.

2. **No `Principal` parameter.** The child's principal is derived inside `spawn()` from `parent.principal()`,
   or — if delegation is genuinely needed — from a delegate `Principal` validated through
   `principal_admitted_for()` against the parent's own principal, matching `start_run()`'s existing
   admission check exactly rather than inventing a second, unchecked path.

3. **`SessionFactory`'s contract is move-only-fresh, enforced by the type, not a comment.** It must
   return `std::unique_ptr<AgentSession<ChatClientT, HistoryProviderT>>` — never a reference or
   `shared_ptr` to something reusable — mirroring `ThreadPool`'s own type-level anti-aliasing discipline
   (copy AND move constructors both `= delete`d, `thread_pool.hpp:144-147`). A `SessionFactory` signature
   that can only ever hand back a freshly-owned object makes the shared-session double-resume race
   unrepresentable, not merely discouraged.

4. **`parallel()`/`pipeline()` take a mandatory, non-default-constructible ceiling and enforce it inside
   the dispatch loop itself**, modeled directly on `token_budget_`'s existing per-round enforcement shape
   (`agent_session.hpp:567-571,1802-1807`) and `Background<max_concurrent>`'s live-count check
   (`tool_pipeline.hpp:693-702`): once the ceiling is reached, thunk N+1 is refused with a structured
   error (matching `run.token_budget_exceeded`'s naming convention, e.g.
   `multi_agent.fanout_budget_exceeded`) rather than dispatched and reconciled after the fact. This one
   change closes both the Budget finding and the model-influenced-fan-out-count finding simultaneously,
   because the check doesn't care whether a human or a prior model response produced `items.size()`.

5. **`ScatterGather` gets the identical ceiling check**, applied by `WorkflowSupervisor` against
   `over_field`'s resolved array length BEFORE minting any of the N invocations — not after the fact,
   and not left to whoever authored the YAML to bound manually.

6. **Default worktree isolation is `branch`-mode-equivalent, unconditionally, for every dynamically
   spawned child** — stated as the default here rather than left as an open question, per ADR-032's own
   precedent and reasoning above. An explicit, narrower opt-out (shared worktree) can be a later,
   separately-justified addition; it is not the starting default.

## Open design questions — resolved vs. still open (updates the gap doc's list)

- **#1 (worktree/sandbox scoping default)** — **resolved above**: `branch`-mode-equivalent isolation,
  unconditional default, per ADR-032's precedent.
- **#2 (cross-child failure policy)** — **resolved below, round 2**.
- **#3 (`Budget` type's scope)** — **resolved below, round 2** (supersedes the "partially resolved"
  note this bullet used to carry).
- **#4 (`ScatterGather`'s YAML-compiler counterpart)** — **resolved below, round 2**.
- **#5 (attribution on independent scatter/gather child failure)** — **resolved below, round 2**
  (folded into #2's answer — same mechanism closes both).

## Round 2 — resolving #2, #3, #4, #5 (2026-08-22)

Self-reviewed against real code (not an independent fresh-context red-team pass like round 1 — see the
closing note on what still needs one before this becomes an ADR).

### #2 + #5 — cross-child failure policy, and scatter/gather attribution (same mechanism closes both)

**The two primitives need two different answers, because only one of them has a graph to route
through.** `EdgeFailurePolicy` (`workflow/graph.hpp:140-151`) is inherently graph-shaped — its
`fallback` field names a recovery executor **id that the graph validator type-checks against a real
node's output port** (§7's own edge-drawability rule). A bare `multi_agent::parallel()`/`pipeline()`
call has no graph, no executor ids, nothing for a `fallback` field to name — reusing
`EdgeFailurePolicy` verbatim for Primitive 1 doesn't fit the shape it was designed for, it only looks
reusable because the enum's other three members (`fail`/`propagate`/`retry`) don't obviously need a
graph.

**Primitive 1 (native `multi_agent`) — stays exactly as already drafted, plus a named retry seam:**
"a thunk that faults resolves to an error in its slot, the call itself never throws, caller filters"
is the right answer and needs no change — it's the same shape `Workflow`'s own `parallel()` already
uses, and `parallel()`/`pipeline()` don't need graph-shaped policy vocabulary they have nowhere to
route through. What was missing: retry itself. `RetryPolicy` (`core/retry_policy.hpp:34-45`) is
**already a plain, non-graph runtime struct** — proven reusable outside any graph context by
`ModelCallGateway::attempt_with_retry` (`model_call_gateway.hpp:238-248`), which is exactly the shape
a `spawn()`-level retry wrapper needs: an optional `RetryPolicy` parameter, applied around one
`spawn()` call, with no executor-id/fallback concept at all.

**Real, pre-existing gap found while checking this: `is_retryable()` is defined twice, with different
semantics, and nothing reconciles them.** `workflow_supervisor.hpp:1030-1033` treats
`transient || resource` as retryable; `model_call_gateway_detail::is_retryable`
(`model_call_gateway.hpp:102-104`) treats **only** `transient` as retryable — `resource` (the same
class `run.token_budget_exceeded` itself uses, `agent_session.hpp:1806-1807`) is retryable in one path
and not the other, with no comment anywhere explaining the divergence. This is not introduced by this
design — it predates it — but a `spawn()`-level retry wrapper has to pick ONE semantics, and picking
either without noting the other exists would silently bake in an undocumented inconsistency a future
reader has no way to discover. **Decision for this draft:** reuse `workflow_supervisor.hpp`'s broader
`transient || resource` — a resource-exhaustion failure (e.g. a transient rate limit) is exactly the
class a bounded retry exists for, and `ModelCallGateway`'s narrower version already has its own
separate backoff+breaker layer sitting in front of it, which `spawn()`'s bare retry wrapper does not.
**Named as a real, independent, separately-landable fix regardless of this design's fate**: the two
`is_retryable()` definitions should be reconciled (one canonical free function, reused by both
call sites) — the same "don't leave an undiscovered drift for the next reader" instinct
`batch-inference-coalescing-design-draft.md`'s Q4 finding already modeled for
`resume_workflow()`'s missing admission check.

**Contract/policy failures are never retried, full stop — a retry wrapper must gate on this or it
becomes an I2 hazard.** `is_retryable()`'s existing exclusion of `contract`/`policy` failures
(`workflow_supervisor.hpp` comment: "a retry of a denial is an I2 hazard the project takes seriously")
applies unchanged here: `attenuate()` (`capability.hpp:694-704`) fails closed identically on every
attempt, so retrying a capability denial can never succeed — but a retry wrapper that doesn't gate on
failure class would still burn attempts and `Budget` spend retrying an unwinnable denial, and the
noise it produces (repeated identical failures) is exactly the kind of thing that could mask a REAL
transient failure happening elsewhere in the same fan-out. `spawn()`'s retry wrapper must call the
same class-gated check, not a bare "retry N times" loop.

**Primitive 2 (`ScatterGather`) — reuses `EdgeFailurePolicy` directly, because it DOES have a graph.**
A `ScatterGather` node lives on a real `Executor` inside a real `Workflow`, expanded by
`WorkflowSupervisor` into N invocations of the SAME declared node — so the node's own already-declared
outgoing-edge `EdgeFailurePolicy` is the natural, existing mechanism to route an individual element's
failure through, not a second, parallel vocabulary invented just for scatter/gather. This directly
resolves #5:

- **`fail`** — one failed element stops the round, but (matching D1's already-proven behavior,
  `test_rt_workflow_supervisor_failure_policies.cpp` D1: "partial results from BOTH an earlier round
  and same-round sibling work that already succeeded") every OTHER already-succeeded scatter element
  in the SAME gather is preserved in `state_.partial`, not discarded. A `ScatterGather` failure is not
  special-cased into a stricter all-or-nothing shape than an ordinary sibling-node failure already
  gets.
- **`retry`** — the individual failed element is re-invoked, bounded by the SAME `attempts` field,
  gated by the SAME `is_retryable()` check as any other retried executor — no new counter, no new
  gating logic.
- **`propagate`** — this is the one genuinely additive piece: today `propagate` delivers ONE failure
  marker `Message` to ONE downstream target (`failure_marker()`, `workflow_supervisor.hpp`). A
  scatter/gather group needs the gathered array itself to carry a **per-element** marker for the
  failed index, alongside the other elements' real outputs — i.e. the gathered result's shape becomes
  "N slots, each either a real output or a failure marker," not "the whole array is one failure marker
  because one element failed." This is new work (the gather-assembly step needs to build this shape),
  but it is not a new POLICY concept — it is `propagate`'s already-existing per-executor marker,
  applied per-array-slot instead of per-node.
- **`fallback`** — the same named recovery executor runs once per failed element, each invocation
  receiving that element's own failure marker — matching D4's already-proven single-element case,
  repeated N times independently, not a new batch-fallback shape.

This is the direct answer to #5 ("does one failed element fail the whole gathered array, or propagate
per-element") — **per-element**, because reusing `propagate`'s existing per-node marker shape at
per-array-slot granularity is strictly less new machinery than inventing an "all-or-nothing batch
failure" shape from scratch, and it matches this project's own repeated instinct (`batch-inference-
coalescing-design-draft.md` Q3: "reuse `OpenPort` wholesale, no new message type") to extend an
existing proven mechanism rather than mint a second one for a shape that's already 90% covered.

### #3 — `Budget`'s scope, and a concurrency hazard round 1 didn't catch

**Confirmed by grep: no cross-session/cross-run budget-pool type exists anywhere in `include/`** — the
gap doc's claim holds. The only real, enforced precedent is `token_budget_`
(`agent_session.hpp:2065-2066`): a single `std::optional<std::uint64_t>`, scoped to ONE session's ONE
run, checked synchronously inside `run_rounds()` after every round
(`:1801-1808`, `run_tokens_consumed_ += usage.input_tokens + usage.output_tokens`), reset on session
reset (`:1066`). It is trivially race-free because `run_rounds()` never runs concurrently with itself
for one session (`session_mutex_` gates entry, per the double-resume finding round 1 already cited).

**`multi_agent::Budget` cannot copy that safety-by-construction, and round 1's fix 4 glossed over
this.** Fix 4 says "once the ceiling is reached, thunk N+1 is refused... enforce it inside the
dispatch loop itself" — but `parallel()` dispatches **all N thunks at once** (a barrier, per its own
docstring), and `pipeline()` deliberately has **no barrier between stages** ("item A can be in stage 3
while item B is still in stage 1"). Either way, multiple thunks run concurrently on different
`ThreadPool` worker threads and each needs to check-and-debit the SAME shared `Budget` object. A plain
struct with a `std::uint64_t spent` field, checked-then-incremented non-atomically from N worker
threads, is a genuine TOCTOU race — two thunks can each observe "one slot remaining" and both proceed,
overshooting the ceiling by exactly the class of bug I8 exists to prevent. **This is a real gap round
1 did not name** (it modeled the enforcement point on `token_budget_`'s single-threaded shape without
checking whether `multi_agent`'s own dispatch is single-threaded too — it is not).

**Corrected: `Budget` is two independently-synchronized ceilings, not one plain struct, matching
`ThreadPool`'s own existing thread-safety discipline (a mutex-guarded queue, `thread_pool.hpp:183`)
rather than inventing a new concurrency primitive:**

```cpp
class Budget {
public:
    Budget(std::size_t max_spawns, std::uint64_t max_tokens);  // non-default-constructible --
                                                                 // matches fix 4's own requirement
    // Atomic admission check for a WHOLE batch (parallel()'s all-at-once submission) or ONE item
    // (pipeline()'s per-item entry into stage 1). Returns false without reserving anything if the
    // request would exceed max_spawns -- no partial admission of a parallel() batch.
    [[nodiscard]] bool try_reserve_spawns(std::size_t count);
    // Called as each child's AgentResponse::usage lands -- mirrors run_tokens_consumed_'s own
    // accounting unit (input_tokens + output_tokens) exactly, not a separate cost_estimate ledger
    // (Usage already carries both; token-based matches the one enforced precedent that exists today).
    void debit_tokens(Usage const& usage);
    [[nodiscard]] std::size_t remaining_spawns() const;
    [[nodiscard]] std::uint64_t remaining_tokens() const;
private:
    std::atomic<std::size_t>   spawns_reserved_{0};
    std::atomic<std::uint64_t> tokens_spent_{0};
    std::size_t const          max_spawns_;
    std::uint64_t const        max_tokens_;
};
```

`try_reserve_spawns` needs a real compare-and-swap loop (not two separate atomic ops), so a `parallel()`
call with `thunks.size() == 10` against a `Budget` with 5 slots left refuses the WHOLE batch atomically
rather than admitting 5 and silently dropping 5 — `parallel()`'s own already-drafted contract ("the
call itself never throws, caller filters") extends naturally: a refused reservation resolves the whole
call to a structured `multi_agent.fanout_budget_exceeded` error, matching round 1's fix 4 naming, rather
than a partial run.

**Why `max_spawns` and `max_tokens` have to be two separate fields, not one:** round 1's fix 4 folded
the I3 (unbounded/model-influenced fan-out COUNT) finding and the I8 (token budget) finding into "one
change closes both... because the check doesn't care where the count came from" — true for WHERE the
check lives, but not for WHAT it measures. A fan-out of 10,000 near-zero-token children could stay
comfortably under a token ceiling while still being a real I3/resource hazard on its own axis — each
`spawn()` mints a fresh `AgentSession` plus (per this doc's item 6) a `branch`-mode worktree
(ADR-032's own copy-on-write whole-tree cost, §4/M5), so spawn COUNT has a real resource cost
independent of token spend. `max_spawns` and `max_tokens` are checked at different times (count is
knowable before any child runs; tokens are only knowable as results land) and guard different
resources — collapsing them into one field would under-specify one axis or the other.

**Scope: per-call, non-inheriting across nesting — deliberately, matching `EffectContext::capabilities`'s
own accepted shape rather than inventing propagation machinery.** A `Budget` is constructed by the
caller and passed by reference into exactly one `parallel()`/`pipeline()` call (or explicitly reused
across nested calls the caller chooses to share the same reference for). No automatic inheritance from
an outer `Budget` to an inner, recursively-spawned `multi_agent::parallel()` call — an ambient,
implicitly-inherited budget object is structurally the same shape I2 already forbids for capabilities
(authority reachable without being explicitly passed), just applied to spend instead of permissions.
ADR-032 §5 already accepted this exact shape as a named, un-solved residual for
`EffectContext::capabilities` ("a borrowed, non-owning pointer, no natural owner yet... noted... so it
isn't rediscovered from scratch") — `Budget`'s propagation question is the same shape, and gets the
same answer: explicit sharing via a passed reference, not automatic ambient inheritance. This also
answers the "does it interact with existing per-session cost tracking" half of #3: it does not need
to — a spawned child's own `token_budget_` (if its factory sets one) and the fan-out's `Budget` are two
independent ceilings answering different questions ("can this one child's run afford one more round"
vs. "can this whole fan-out afford one more child"), and nothing requires unifying them into one type.

### #4 — `ScatterGather`'s YAML-compiler counterpart, made concrete

Round 1 said "must land atomically... not designed here." Concretely, following the identical pattern
`agent-as-workflow-executor-design-draft.md` §5 already used for `capability_ceiling`:

1. `Executor` (`workflow/graph.hpp`) gains `std::optional<ScatterGather> scatter_gather` — but the gap
   doc's original `ScatterGather{over_field, max_concurrency}` needs a THIRD field found by the same
   reasoning as the `Budget` split above: `max_concurrency` is a throughput knob (how many of the
   admitted N run at once), not a total-count ceiling (how many elements are admitted at all). Round 1
   item 5's own text ("`ScatterGather` gets the identical ceiling check... applied... against
   `over_field`'s resolved array length BEFORE minting any of the N invocations") already implies a
   COUNT ceiling distinct from `max_concurrency` — the gap doc's original two-field sketch never had
   one. Corrected: `ScatterGather{over_field, max_elements, max_concurrency}`, where `max_elements` is
   checked against the resolved array length before minting ANY invocation (fails the node closed,
   through the SAME `EdgeFailurePolicy` machinery above, if exceeded — not a separate error shape), and
   `max_concurrency` bounds how many of the admitted (`<= max_elements`) invocations run concurrently.
2. `compile_executor()` (`yaml_compiler.hpp:60-89`) needs a matching parse branch reading a
   `scatter_gather:` block with `over_field`/`max_elements`/`max_concurrency` sub-fields — today it
   silently drops any field `Executor` doesn't declare (`:80-83`'s own comment, "honestly dropped, not
   fabricated"), so omitting this branch would silently strip `scatter_gather` from every YAML-authored
   graph while the C++ `WorkflowBuilder` form enforced it, the exact I6 divergence
   `test_workflow_graph_validation.cpp:223-225` already exists to prevent.
3. `max_elements`/`max_concurrency` both need a static `>= 1` validator rejection at graph-build time —
   matching `EdgeFailurePolicy::attempts`'s own already-established "a policy field that would be a
   no-op spelled as a policy is a mis-authored graph, rejected, not silently ignored" rule
   (`graph.hpp:142-144`).
4. `over_field`'s JSON-pointer-style path is NOT statically checkable against graph shape (it resolves
   against upstream PAYLOAD data at runtime) — `validate_workflow()` can only check that the field is a
   non-empty string; whether it actually resolves to an array on a given run is a `check_workflow_
   executable()`-time-or-later concern, the same "graph shape vs. runtime data" split `input_type`/
   `output_type`'s own type contract already draws elsewhere in `Executor`.
5. This still depends on `executor_kind::agent`'s runtime bridge landing first (unchanged from round 1)
   — sequencing, not a design question.

### What this round does NOT resolve, and should before an ADR

This was a same-session self-review, not an independent fresh-context adversarial pass like round 1's
own (the one that found the three original FATALs). Two things specifically want that treatment before
an ADR, not just this draft's own reasoning:

1. **The `Budget::try_reserve_spawns` compare-and-swap loop itself** — concurrency-hazard code is
   exactly the category this project's own `CLAUDE.md` names as needing "a real concurrency test under
   `rt::ThreadPool`, not `TestKit`-equivalent synchronous drivers" (the identical bar
   `agent-as-workflow-executor-design-draft.md` §3 item 2 already set for the double-resume hazard) —
   a positive control proving two concurrent `try_reserve_spawns` calls racing for the last slot never
   both succeed, not assumed correct by atomics-are-safe reputation alone.
2. **The `is_retryable()` divergence** — this draft picked `workflow_supervisor.hpp`'s broader
   definition for `spawn()`'s retry wrapper without a red-team pass stress-testing whether that's
   actually the right one to standardize on, or whether `model_call_gateway.hpp`'s narrower one is
   right and `workflow_supervisor.hpp`'s is the stale one. Either way it should become ONE canonical
   function, not two, independent of whether `multi_agent::spawn()` ever ships.

## Round 3 — can the model trigger this, can children self-expand, how does worktree sharing actually
## work for a runtime-computed child (2026-08-22, same-session follow-up questions)

Three direct questions surfaced two things neither round 1 nor round 2 actually answered (recursion
depth, and the real worktree-minting mechanism for children with no static graph identity) — named
honestly below rather than assumed covered by "branch-mode default" or "no auto-inheriting Budget."

### Can the primary/root LLM session call `spawn()`/`parallel()`/`pipeline()` itself?

**No, not directly — this is not an accident, it's the gap doc's own founding distinction.** The gap
doc's entire "why not just port MAF's shape" section rejects exactly this: MAF's
`background_agents_start_task` IS a tool the model calls to decide fan-out; this design's whole
premise is "host-authored, deterministic control flow... not the model" (gap doc, table row 3).
`multi_agent::spawn()`/`parallel()`/`pipeline()` take a `SessionFactory`, a `CapabilitySet`/narrower
grant, and (round 2) a `Budget` — none of these are the shape of arguments an LLM tool call produces
(a JSON payload matching a declared schema), and nothing in either round proposes exposing them as a
`Tool<...>` a model could invoke with model-authored arguments for those parameters.

**What a model CAN do: trigger it indirectly, through an ordinary tool call whose BODY (host C++ code)
calls into `multi_agent`.** A host author can write `Tool<ReviewFilesRequest, ReviewFilesResult>`
whose body internally calls `multi_agent::pipeline()` over `request.file_paths` — the model supplies
`file_paths` (data, via the tool's declared JSON schema, exactly like any other tool argument), never
the `CapabilitySet`/`Principal`/`Budget`, which the tool body constructs from ITS OWN capability
ceiling (mirroring how `Tool<...>::capability_ceiling` already works for any other tool, RFC 009/014
§7). This is precisely the case round 1's MUST-FIX (I3) finding already exists for: "if the array
populating `items`/`over_field` traces back to a prior tool result that echoes model output, the model
has de facto chosen the spawn count" — closed not by forbidding the model from influencing the count,
but by round 2's `Budget::try_reserve_spawns` refusing to admit more than `max_spawns` regardless of
where the count came from. **So: indirect, tool-mediated triggering with a model-influenced fan-out
count is the supported shape; the model directly calling `spawn()` with its own chosen capabilities,
principal, or budget is not designed and would violate I2/I3 if it were.**

### Do spawned children try to self-expand their own branch — i.e., can a child recursively fan out?

**Nothing in the drafted API stops it, and round 2 only answered half of what that implies.** `spawn()`
takes "the parent session" as an ordinary parameter (fix 1, §Corrected design) — a session `spawn()`
itself returned can just as validly be passed as the NEXT call's `parent`. Round 2's `Budget` section
answered the SHARING half (no automatic inheritance across a nested call — a caller who wants a shared
ceiling passes the same `Budget&` reference into both) but did not ask a genuinely separate question:
**is there any ceiling on nesting DEPTH itself**, independent of any one level's own `Budget` being
individually well-formed?

**Currently: no, and this is a real, newly-found gap, not a previously-resolved question re-asked.**
Ten children each independently constructing their own well-formed `Budget{max_spawns: 10, ...}` and
each spawning ten more multiplies total resource consumption (sessions, `branch`-mode worktrees,
threads — round 2's own reasoning for why `max_spawns` and `max_tokens` are separate axes applies
again here) across DEPTH, and no single level's own ceiling bounds the product across levels. This
project's own orchestration tooling (the `Workflow` tool this whole design was explicitly modeled on,
gap doc line "in this very session's toolset") enforces exactly this with a **hard, structural
one-level nesting rule** for its own `workflow()` sub-step: "Nesting is one level only:
`workflow()` inside a child throws." `multi_agent::spawn()` has no equivalent, and — unlike that
tool, which can check nesting depth against its own single always-present call stack — `spawn()` is
an ordinary library function with no engine-side notion of "how many `spawn()` calls deep is this
session already," so the check would have to be a NEW, threaded-through piece of state, not something
reused from elsewhere.

**Two honest options, neither built, matching this draft's own "every real ceiling is engine-enforced,
not caller-discretion" discipline (round 1 fix 4, round 2's `Budget`):**
(a) a hard one-level rule — a session created via `spawn()`'s `SessionFactory` carries a marker (e.g.
`EffectContext`-adjacent, mirroring how `require_authority`/other per-session flags already propagate
via `AgentSessionRecord`) that a nested `spawn()` call checks and refuses closed, matching the
`Workflow` tool's own "throws" behavior exactly; or (b) an explicit `remaining_fanout_depth`
parameter threaded through every `spawn()` call, decremented per level, refusing at 0 — a real,
caller-visible ceiling rather than an invisible one, at the cost of a parameter every `spawn()` call
site has to thread. **Naming this as a new open question (#6) rather than resolving it here** — it
needs the same red-team scrutiny round 1 gave the original three FATALs, since an unbounded-depth
self-expansion is structurally the same class of hazard (unbounded resource consumption with no
engine-side ceiling) those findings already established this project takes seriously.

### How is worktree sharing actually implemented for a dynamically-spawned child?

**Round 1 resolved the DEFAULT MODE (unconditionally `branch`), not the minting MECHANISM — these are
different questions, and only the first one has an answer.** Re-checking ADR-032 itself: `Executor::
worktree_mode` and `mint_executor_worktrees`/`resume_executor_worktrees`
(`workflow/worktree_scoping.hpp:111-208`, confirmed directly) are keyed by a **static `Executor::id`,
known at graph-authoring time, index-parallel to `graph_.executors`** (`for` loop iterating a
fixed-size, pre-declared vector) — and ADR-032 §2/§5 says outright: **"agent/sub_workflow-kind
executor worktree wiring is untouched... those kinds don't exist yet."** `multi_agent::spawn()`'s
children have no static `Executor::id` at all — their count and identity are computed at runtime,
potentially fresh on every call, not once per graph node. So ADR-032's actual minting FUNCTIONS
cannot be called for a `spawn()`-family child as-is; only its underlying PRIMITIVES
(`SubWorktree`/`Mount`, `create_sub_worktree`) and its DESIGN DISCIPLINE (branch-by-default,
existence-check-before-mint, `/`-rejection) are reusable — a genuinely new minting function is needed,
not yet designed beyond "the mode is `branch`."

**What that new function needs, sketched (not designed to ADR quality here):**

```cpp
// Sketch only -- NOT part of the corrected design above, needs its own red-team pass before it is.
[[nodiscard]] result<ExecutorWorktreeGrant> mint_spawn_worktree(
    RS& ref_store, Ref const& parent_ref, std::string const& spawn_identity, sharing_mode mode);
```

- **`spawn_identity` needs its own naming convention**, since there is no author-typed `Executor::id`
  to reuse — the natural fit is the same composed-string convention `batch-inference-coalescing-
  design-draft.md` Q4 already established for `custom_id` (`run_id + ":" + round_number + ":" +
  executor_id`): something like `parent_session_id + ":spawn:" + monotonic_counter`, host-generated,
  not model-authored.
- **ADR-032 finding #4's `/`/`..`-rejection must still apply unconditionally**, even though this id is
  host-generated rather than author-typed — a host could still template runtime data (a file path, a
  loop variable) into the identity string for readability without realizing the mount-id namespace has
  this constraint; the check doesn't get to assume "host-generated" implies "already safe."
- **The existence-check-before-mint discipline (ADR-032 finding #2, `already_minted`) still applies**,
  but its precondition changes: `mint_executor_worktrees` assumes each executor is minted AT MOST ONCE
  per run because the graph has a fixed executor count; a `spawn()`-family caller that runs
  `pipeline()` twice over different item lists in the same outer session needs a genuinely fresh
  `spawn_identity` each time (e.g. the monotonic counter above), or the second call's mint attempt
  fails closed against the first — which is correct behavior, but only if the identity scheme actually
  guarantees freshness, which nothing checks today.
- **Checkpoint/resume for a dynamically-spawned child's worktree is not designed at all** — round 1
  §"What this does NOT need to solve" already named the SESSION-level version of this honestly ("if
  the outer context is checkpointed, the fan-out just re-runs from scratch on resume"), but a
  `branch`-mode worktree that a re-run would then mint AGAIN under a NEW `spawn_identity` (since the
  counter restarts) leaves the FIRST attempt's branch orphaned, not merged, not cleaned up — a real,
  separate resource-leak question distinct from the "history is lost" limitation already accepted for
  the session itself.

**Naming this as a new open question (#7)**, distinct from the already-resolved #1 (default mode):
*"what is the actual minting mechanism and identity scheme for a runtime-computed child's worktree,
given ADR-032's own functions are graph-shaped and explicitly don't cover this case"* — not designed
here, and — like #6 above — needs its own red-team pass, not assumed safe by extension of ADR-032's
already-proven (but structurally different) mechanism.

## Round 4 — does a spawned child share context/history with its parent? (2026-08-22)

**No, not automatically — this is a direct, already-decided consequence of fix 3, just not stated in
these terms before.** `SessionFactory` "MUST return a fresh, owned session... never a reference or
shared_ptr to something reusable" (fix 3) means every spawned child gets a brand-new `AgentSession`
with empty `history_` and a freshly-default-constructed `HistoryProviderT`. `StartRun`
(`agent_session.hpp:294-301`) carries exactly `{input, caller, authority}` — one `Message`, nothing
history-shaped. **There is no export path even if one were wanted**: `AgentSessionRecord` (the
checkpoint record) does not serialize `history_` at all (already noted in
`agent-as-workflow-executor-design-draft.md`'s item 1) — so there isn't even a checkpoint-shaped
mechanism to copy a parent's transcript into a child; the only thing that ever reaches a child is
whatever ONE `Message` the host-authored calling code hand-builds as `StartRun.input`.

**What CAN be shared, only via explicit host wiring at construction time, and it's knowledge-sharing,
not history-sharing:** `ContextProvider` composition (`ComposedContextProvider<Ms...>`,
`composed_context_provider.hpp:34-92`) lets a session's one `HistoryProviderT` slot hold N
contributors. A `SessionFactory` can construct a child pointed at the SAME underlying store a parent
also reads from (`memory_provider.hpp`, `vector_rag_context_provider.hpp`) — real, existing
infrastructure, reusable as-is. But this shares a common BACKING STORE (memory/RAG), not the parent's
live, in-session turn-by-turn `history_`, and it has to be baked into the factory's construction of the
type itself: `history_provider_` is "a plain, default-constructed value member with no
emplace_*/accessor pair to reach in and configure it after construction"
(`history_and_skills_provider.hpp`'s own comment) — so this can never be wired in after the fact, only
at the moment the `SessionFactory` builds the child.

**Newly-found open question (#8), not covered by any prior round:** if a host hand-constructs a
child's `StartRun.input` by copying content FROM the parent's own history or tool results (the only
real way to hand a child "what I found so far"), does the copied content's taint marker survive the
copy? `ToolResult` content items are `tainted = true` by convention (`tool_pipeline.hpp:594`, "the
primary prompt-injection vector," 006 §7) specifically so downstream code treats them with appropriate
suspicion — a host that copies tainted content into a fresh child's input, in a fresh session with no
memory of where that content came from, needs that taint to travel with it, or the child processes
what is structurally external, untrusted content as if it were its own trusted input. Not designed or
even scoped here — named so it isn't silently assumed safe.

**A real, already-proven precedent exists for the SHAPE this fix should take, found while answering a
follow-up question in this same session: `MemoryProvider::memory_item_to_message()`
(`core/memory_provider.hpp:327-338`).** It already solves "carry externally-sourced content across a
boundary (there: a prior turn/session's write, into the CURRENT turn's context) without letting it
pass as trusted" — `content_origin::external` + `tainted = true`, a confidence label derived ONLY from
a trusted structured field (never from the content itself), and `neutralize_forged_provenance_markers()`
run over the untrusted content BEFORE the real label is prepended, specifically to stop the content
from forging a higher-trust label once concatenated with other labeled items (029 §6, gap-audit
finding 17). A future fix for #8 should reuse this exact four-part pattern (external origin, tainted,
structured-field-only label, anti-forgery neutralization) for whatever content a host copies from a
parent's history into a child's `StartRun.input`, rather than inventing a second taint-carrying
mechanism — the same "extend a proven mechanism instead of minting a new one" instinct this whole
design draft already leans on repeatedly (Q3's `OpenPort` reuse, `propagate`'s per-slot reuse above).
`VectorRagContextProvider` (ADR-063) already reuses the identical mechanism for RAG citations via its
own `rag:` marker family — a third, independent case this pattern already generalizes across, one
more reason to reuse it a fourth time here rather than not.

## Round 5 — final self-check: where two individually-correct rounds don't compose (2026-08-22)

A full re-read specifically looking for cross-round inconsistencies — not new topics, places where two
already-"resolved" answers quietly conflict once put together. Four found, none previously named.

1. **Does a `spawn()` retry attempt debit `Budget.max_spawns` again?** Round 2's retry wrapper
   (`RetryPolicy` around one `spawn()` call) and round 2's `Budget.max_spawns` were designed in the
   same round but never checked against each other. Every retry attempt still goes through `spawn()`,
   which mints a genuinely fresh session + (fix 6) a fresh `branch`-mode worktree per fix 3's own
   contract — the real resource cost `max_spawns` exists to cap. If a retried attempt does NOT debit
   `try_reserve_spawns` again, `RetryPolicy::max_attempts` silently multiplies actual session/worktree
   creation beyond the declared ceiling — the exact shape of hazard `max_spawns` was built to close,
   reopened at the retry seam. **Not resolved here**: the honest answer is retries must debit like any
   other spawn (a retry is not exempt from the resource cost that motivated the ceiling), but this
   needs to be stated as part of the `Budget`/retry contract, not left implicit.
2. **`ScatterGather` never got the `max_tokens`-equivalent ceiling round 2 §3 argued for on its own
   terms.** Round 2 §3's own reasoning for splitting `Budget` into `max_spawns` + `max_tokens` ("guard
   different resources... collapsing them would under-specify one axis") applies identically to
   `ScatterGather` — but round 2 §4's corrected `ScatterGather{over_field, max_elements,
   max_concurrency}` only gained a COUNT ceiling (`max_elements`), never a token-spend ceiling. A
   `ScatterGather` node bounded to `max_elements: 10` can still spend an unbounded number of tokens
   across those 10 real model calls, with no engine-side comparison against anything — the identical
   I8 gap round 1's fix 4 closed for Primitive 1, left open for Primitive 2. This is the same class of
   omission finding #4/#5 in round 1 targeted, just not carried through consistently into round 2's own
   later section.
3. **Concurrent writes to a SHARED `MemoryProvider`/`AppendLogStore` from multiple, concurrently-running
   `branch`-mode children is an untested combination of two separately-fine decisions.** Round 1 makes
   every dynamically-spawned child's WORKTREE isolated (`branch`, unconditional). Round 4 shows a host
   CAN point multiple children's `HistoryProviderT` slot at the SAME underlying `MemoryProvider`
   store (shared knowledge, explicit wiring). Put together: N concurrently-running children, each with
   an ISOLATED worktree for file I/O but a SHARED memory/`AppendLogStore` backing ref for
   `write_memory_item()` calls from their own `on_turn_end()` hooks — this is a genuinely different
   access pattern than `MemoryProvider`'s existing, presumably single-session-at-a-time usage. Whether
   `rt::AppendLogStore`'s write path is safe under concurrent writers from independent sessions is **not
   verified one way or the other in this pass** — named as an unverified assumption, not asserted broken,
   because round 2's `Budget` finding shows exactly this kind of gap (a mechanism proven fine under its
   original single-caller usage, unchecked under this design's genuinely new concurrent-multi-session
   usage) is a real, recurring pattern in this exact design, not a one-off.
4. **`spawn_identity`'s counter (round 3's `parent_session_id + ":spawn:" + monotonic_counter` sketch)
   has no named owner.** `AgentSession` has no such counter member today. If two independent
   `multi_agent::parallel()` calls run concurrently against the SAME parent session (two different tool
   calls, or a tool body that fans out twice), and each maintains its own local counter starting at 0,
   their `spawn_identity` strings collide — reopening finding #2's `already_minted` fail-closed path as
   a spurious, wrong failure rather than the genuine double-mint protection it exists for. Needs an
   explicit owner (a real, thread-safe counter on `AgentSession` itself, or a caller-supplied
   disambiguating prefix per `parallel()`/`pipeline()` call) — not designed here, folded into open
   question #7 rather than given a new number, since it's the same "identity scheme" gap #7 already
   names, just a sharper edge of it found on this pass.

None of these four are dispatched with a fix here — they are exactly the kind of composition-level
finding an independent red-team pass over the full accumulated design (rounds 1-5) should either
confirm, sharpen, or refute, which is the next step for this doc rather than a further self-review
round.

## Round 6 — independent, fresh-context red-team of rounds 1-5 (2026-08-22)

An independent pass (fresh context, no prior exposure to this doc, via the `Agent` tool) verified
essentially every real `file:line` citation in rounds 1-5 against current source and reported it all
accurate. Two FATALs and three MUST-FIXes survived a follow-up manual re-verification (citations
re-checked directly against the files below before recording here) — this design is **not safe as
drafted**, specifically for the Tier-3 admission mode and the token half of `Budget`.

**FATAL — fix 1's `spawn()` sources the wrong capability grant for a `require_authority_` (Tier-3,
ADR-061) session, reopening round 1's own FATAL #1 one layer down.** Fix 1 attenuates against
`parent.capabilities()`. `capabilities()` (`agent_session.hpp:593-595`) returns
`capabilities_.get()` — the session's static, set-once grant. But `apply_dispatch_authority()`
(`:1315-1337`) shows the LIVE per-call grant a Tier-3 session actually dispatches under is
`effect_context_.capabilities = authority->capabilities` (`:1328-1329`, sourced from a
dispatcher-resolved `RequestAuthority` that can legitimately differ call to call) — `capabilities_`
is only ever the live grant in the non-Tier-3 `else` branch (`:1334-1335`). **Confirmed: there is no
public accessor for `effect_context_.capabilities` anywhere in the file** — every reference is an
internal member read. So `spawn()` as drafted, called from inside a Tier-3 session's tool body,
attenuates a child's grant against the session's static baseline instead of the authority actually
governing the call it's running inside of — exactly the "parameter that looks explicit but isn't
tied to what was actually held for this invocation" shape round 1's FATAL #1 was written to kill,
for precisely the deployment mode (Tier-3) this codebase has invested the most machinery in.

**FATAL — round 2's own corrected `Budget` never enforces `max_tokens_`; it only tallies it.**
`debit_tokens(Usage const&)` returns `void`; `remaining_tokens()` exists but nothing in the drafted
dispatch flow ever calls it before admitting the next spawn — only `try_reserve_spawns` (the
spawn-COUNT axis) gates admission. This directly contradicts fix 4's own mandate ("once the ceiling
is reached, thunk N+1 is refused... rather than dispatched and reconciled after the fact") for one of
`Budget`'s two declared axes, inside the very type built to satisfy I8. Self-confirmed on re-reading
round 2's own code sketch above — the gap is real, not a misreading by the red-team pass.

**MUST-FIX — retry attempts and `spawn_identity`'s `already_minted` guard are on an unresolved
collision course.** Round 3's `spawn_identity = parent_session_id + ":spawn:" + monotonic_counter`
explicitly imports ADR-032 finding #2's existence-check-before-mint discipline
(`worktree_scoping.hpp:130-141`, `worktree_scoping.already_minted`). If retry (round 2) reruns
`spawn()` under the SAME logical identity (the natural reading — round 5 finding #4 only discussed
identity stability ACROSS separate `parallel()` calls, never within one item's own retry), a retried
attempt mints under the identical identity as the failed first attempt and fails closed against the
already-minted guard — silently disabling retry for every `spawn()` using the unconditional
(round 1 fix 6) default `branch`-mode worktree. Sharper than round 5 finding #1 (which asked whether
retries debit `Budget`): fix 1's own `spawn()` signature carries no `Budget` parameter at all — only
`parallel()`/`pipeline()` do — so a `RetryPolicy` wrapper "applied around one `spawn()` call" (round 2)
has no reference to a `Budget` to debit even if the design committed to counting it, without a
signature change neither round proposes.

**MUST-FIX — round 4's shared-`MemoryProvider` sharing pattern triggers an already-self-documented
race, not a merely-unverified one.** `write_memory_item()`'s own comment
(`core/memory.hpp:288-296`, verified verbatim) states directly: `write_seq` is predicted from
`ref_store.last_seq()` before `mount_write`'s own commit, "assum[ing] no OTHER writer commits to the
SAME ref between this read and `mount_write`'s own commit... true for every caller in this codebase
TODAY (one principal's memory worktree, written sequentially by that principal's own turn loop), not
a structurally enforced guarantee against a FUTURE CONCURRENT WRITER." Round 4's own accepted pattern
(multiple children's `HistoryProviderT` pointed at the same backing `MemoryProvider` store) is
exactly the future concurrent writer this comment already names — N `branch`-isolated (worktree-wise)
children each calling `on_turn_end()` concurrently reintroduces this TOCTOU for real, corrupting the
`write_seq` total-order invariant `rank_memory_items`'s own tie-break sort
(`memory_provider.hpp:210-217`) depends on. Round 5 filed this as "unverified"; it is not — the race
condition is already named in the exact code this design proposes exercising concurrently for the
first time.

**Residual — `Budget` has no reset/release/lifecycle story, unlike the precedent it claims to
mirror.** `spawns_reserved_`/`tokens_spent_` only grow; nothing decrements on child completion or
resets the object. `token_budget_` (the cited precedent) resets on session reset
(`agent_session.hpp:1066`). Round 2 covers reuse ACROSS nested calls (share the same `Budget&`
deliberately) but never reuse across many SEQUENTIAL, non-nested `parallel()`/`pipeline()` calls in a
long-lived host loop — such a `Budget` permanently exhausts after `max_spawns` children EVER, no
matter how long ago most of them completed, with no lifecycle question even named.

**What this means for the doc's status:** this design is not safe to promote to an ADR as currently
corrected — the two FATALs (wrong capability source under Tier-3; `Budget`'s token axis is
decorative) need an actual fix, not just a named gap, before the next round. The three MUST-FIXes and
one residual can reasonably stay as scoped follow-on work, matching how round 1's own MUST-FIXes
(items 4/5 in that round) were carried forward rather than blocking the corrected design outright —
but the two FATALs are structurally the same severity as round 1's original three, which DID block a
"corrected design" from being written until they were addressed. A round 7 owes this doc the same
treatment round 1 gave its own three FATALs: an actual fix, not a deferral.

## Round 7 — fixing round 6's two FATALs, plus a third of the identical shape found while fixing them
## (2026-08-22)

**Found while designing the fix: fix 2 (`Principal`, round 1) has the IDENTICAL bug as FATAL #1, not
just FATAL #2's separate `Budget` bug.** `principal()` (`agent_session.hpp:690`) returns
`principal_` — session-level, static — but three separate call sites in the same file
(`:947,1561,1670`) carry the comment "`effect_context_.principal`, not `principal_` — per-request, not
session-level" as a standing, repeated convention. `apply_dispatch_authority()` confirms why:
`effect_context_.principal = authority->principal` under Tier-3 (`:1328`), only falling back to
`principal_` in the non-Tier-3 branch (`:1334`). Fix 2's "derived inside `spawn()` from
`parent.principal()`" is exactly as wrong, for exactly the same reason, as fix 1's `parent.capabilities()`
— both read the STATIC accessor when the LIVE per-call value is what actually governs a Tier-3
dispatch. One fix closes both.

### The fix: `spawn()` takes the caller's live `EffectContext const&` as an explicit parameter

Neither `capabilities()` nor `principal()` should be called at all — `AgentSession` has no public
accessor for `effect_context_` (confirmed, round 6), and adding one would just be a new, wrong-by-default
foot-gun (anything reaching it outside the bracket `apply_dispatch_authority()` maintains would read a
stale value). The actual fix reuses a seam this project already resolved once, for the structurally
identical problem: `agent-as-workflow-executor-design-draft.md`'s own capability-sourcing resolution —
"the actual granted `CapabilitySet` a node's `AgentSession` runs with comes from whatever `EffectContext`
the CALLER populates... the existing seam, finally used, no new API surface." `spawn()`'s caller (a tool
body, a workflow executor body — anywhere `spawn()` is meant to be called from) already legitimately
holds its OWN call-scoped `EffectContext&` — every real `ToolDescriptor::invoke` in this codebase already
receives one (`memory_provider.hpp`'s own `make_recall_tool_descriptor()`:
`[...](json::Value const&, EffectContext&) -> result<json::Value>`). `spawn()` should take that same
object, read-only, instead of reaching into `parent`:

```cpp
template <class ChatClientT, class HistoryProviderT>
[[nodiscard]] task<result<AgentResponse>> spawn(
    AgentSession<ChatClientT, HistoryProviderT>& parent,
    EffectContext const& parent_call_ctx,  // the LIVE grant/principal for THIS call -- e.g. exactly
                                            // the EffectContext& a tool body's own invoke(args, ctx)
                                            // already receives. NEVER parent.capabilities()/
                                            // .principal() -- both are the session-level STATIC
                                            // baseline, wrong under Tier-3/ADR-061 (round 6 FATAL #1,
                                            // and the identical bug just found in fix 2 above).
    SessionFactory<ChatClientT, HistoryProviderT> const& factory,
    StartRun request,
    std::vector<Capability> const& narrower_grant,
    std::optional<Principal> delegate_principal = std::nullopt);
```

- **Capability**: fails closed (`multi_agent.no_live_capabilities`) if `parent_call_ctx.capabilities`
  is null — matching `run_rounds()`'s own `empty_caps` fallback pattern
  (`CapabilitySet::grant_root({})`, `agent_session.hpp:1660`) rather than dereferencing a possibly-null
  `shared_ptr` — then `parent_call_ctx.capabilities->attenuate(narrower_grant)`, unchanged from fix 1's
  own fail-closed reasoning, just sourced correctly now.
- **Principal**: `parent_call_ctx.principal` directly by default (the live principal governing THIS
  call, Tier-3 or not) — or, if `delegate_principal` is supplied, validated through
  `principal_admitted_for(*delegate_principal, parent_call_ctx.principal)`
  (`agent_session.hpp:776,859`) before being trusted, matching `start_run()`'s own existing admission
  check exactly (fix 2's original intent, now sourced from the right field).
- This is additive to what round 1 already established (`attenuate()`'s fail-closed narrowing, no bare
  `Principal` parameter) — it corrects WHERE the two inputs come from, not the shape of the check
  applied to them.

### The fix for `Budget.max_tokens`: an honest admission gate, not a true reservation

Round 6 confirmed `debit_tokens()` only tallies. The reason it was never wired into admission is real,
not an oversight to paper over: unlike spawn COUNT (`items.size()`, known before any child runs),
**token cost is unknowable before a child actually runs** — there is nothing to "reserve" the way
`try_reserve_spawns` reserves a count slot. The honest fix is a coarser, but real, engine-enforced gate:
`parallel()`/`pipeline()` must check `remaining_tokens() > 0` (already-spent tally, from completed
siblings' real `usage`) as part of the SAME admission decision as `try_reserve_spawns`, refusing to
dispatch ANY further child once the running total has already crossed `max_tokens_` — a "stop starting
new work once we're already over" backstop, not a per-child pre-commitment. Both checks belong in ONE
combined, single-lock-guarded function rather than two independently-atomic ones (checking two separate
atomics for a joint admission decision is itself a TOCTOU risk between the two reads — the same class of
bug round 2 found in the original unguarded `Budget`, one level subtler):

```cpp
class Budget {
public:
    Budget(std::size_t max_spawns, std::uint64_t max_tokens);
    // ONE combined, mutex-guarded admission check for a WHOLE parallel() batch or ONE pipeline() item.
    // Refuses (reserves nothing) if EITHER axis is already exhausted -- spawn count checked against
    // the reservation about to be made, tokens checked against the running tally SO FAR (not this
    // request's own future cost, which cannot be known before it runs).
    [[nodiscard]] bool try_reserve(std::size_t spawn_count);
    void debit_tokens(Usage const& usage);  // called as each child's real usage lands
    [[nodiscard]] std::size_t remaining_spawns() const;
    [[nodiscard]] std::uint64_t remaining_tokens() const;
private:
    std::mutex mutex_;  // guards BOTH counters together -- ThreadPool's own "mutex-guarded queue"
                         // precedent (thread_pool.hpp:183), not two independent atomics whose
                         // combination could still race between reads.
    std::size_t   spawns_reserved_ = 0;
    std::uint64_t tokens_spent_    = 0;
    std::size_t const   max_spawns_;
    std::uint64_t const max_tokens_;
};
```

**Named explicitly so a future reader doesn't expect symmetry that doesn't exist:** `max_spawns` is a
true pre-commitment (a reservation that can never be exceeded, by construction). `max_tokens` is a
looser "no new work starts once we're already over" backstop — a single child can still push the total
past `max_tokens_` before that overshoot is observed (its own usage isn't known until it completes),
the same fundamental shape `token_budget_`'s own per-round check already has for one session
(`agent_session.hpp:1801-1808`, checked AFTER `usage` arrives, not before). This design does not
invent a better guarantee than the codebase's own existing, accepted precedent already provides for
the single-session case — it applies the identical shape at the fan-out level.

### What round 7 does NOT fix — MUST-FIXes from round 6 stay open, on purpose

Fixing FATAL #1/#2 (this round) changes `spawn()`'s signature; fixing `Budget` (this round) changes its
internals. Neither touches round 6's three MUST-FIXes (retry-vs-`already_minted` collision,
retry-has-no-`Budget`-to-debit, concurrent `MemoryProvider` writes) or its one residual
(`Budget` lifecycle) — those remain real, open, and are NOT closed by this round. Worth naming one
new connection, not a fix: since `spawn()`'s signature is being touched anyway (this round), threading
a `Budget*` (nullable — a bare `spawn()` call outside `parallel()`/`pipeline()` has nothing to debit
against) through it would make the "retry has no `Budget` to debit" MUST-FIX cheap to close in a FUTURE
round, but doing so now, un-red-teamed, would be exactly the kind of scope creep this draft's own
discipline (fix things one confirmed finding at a time, not speculatively) argues against — named as a
low-cost future opportunity, not implemented here.

## Round 8 — wrapping `multi_agent` as a `ContextProvider`: instructions, child framing, and result
## handoff (2026-08-22, user proposal)

A direct proposal from the project owner: expose this feature to the MAIN session as a real
`ContextProvider` — one that carries its own instruction for the parent, while each spawned child gets
a fixed, hardcoded instruction plus a per-call instruction passed in. This closes a real gap no prior
round asked about at all: **rounds 1-7 designed how a child gets spawned, capped, and isolated, but
never how the model-facing surface actually looks, or how results get back to the model that asked for
them.** Both are answered by real, existing mechanisms already proven elsewhere in this codebase — not
new machinery.

### The main session's side: `FanOutProvider`, a `ContextProvider` mirroring `MemoryProvider`'s exact shape

`ContextContribution` (`core/context_provider.hpp:45-49`) already has THREE fields, not one:
`instructions` (`std::optional<TaintedText>` — a dedicated, type-level-tainted instruction slot,
`core/tainted.hpp:22-41`, distinct from `messages`), `messages`, and `tools`. `MemoryProvider` already
proves the "contribute a callable tool from inside a `ContextProvider`" pattern for real
(`make_recall_tool_descriptor()`, `memory_provider.hpp:340-370`, pushed via
`contribution.tools.push_back(...)`). A `FanOutProvider` conformer is the direct union of both
already-proven shapes:

```cpp
template <class ChatClientT, class HistoryProviderT>
class FanOutProvider {
public:
    static constexpr std::string_view name = "fan_out";  // ADR-066 §3 naming convention

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext&) {
        ContextContribution c;
        // Host/engine-authored, not model-facing input echoed back -- ADR-066 §5's "host-authored
        // synthesized content is entitled to claim ::system"-equivalent posture, EXCEPT `instructions`
        // is `TaintedText` unconditionally by TYPE (tainted.hpp) regardless of authorship -- no
        // ContextProvider gets an exemption from that, matching 029 §6's "memory gets no exemption"
        // instinct applied one level up, to the field itself rather than to one provider's content.
        c.instructions = TaintedText{
            "You may delegate independent sub-tasks over a list of items to separate sub-agents via "
            "the 'delegate_review' tool. Each item is reviewed in isolation; results are gathered back "
            "as a single reply. Use this for genuinely independent, parallelizable work only."};
        c.tools.push_back(make_delegate_tool_descriptor());
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
private:
    ToolDescriptor make_delegate_tool_descriptor() const { /* below */ }
    SessionFactory<ChatClientT, HistoryProviderT> child_factory_;
    // ... narrower_grant, Budget, RetryPolicy -- all HOST-configured at FanOutProvider construction
    // time, never model-supplied (I2/I3, unchanged from every prior round).
};
```

This is additive, not a replacement for round 3's original answer ("a host can write its own bespoke
`Tool<ReviewFilesRequest, ReviewFilesResult>`") — `FanOutProvider` is a reusable, generic default (one
tool, one shape: items + a task instruction in, results out) a host can compose in directly, matching
`MemoryProvider`/`HistoryProvider`'s own already-established "real, generic, reusable `ContextProvider`
conformer" pattern, while a host wanting a narrower or differently-shaped tool still writes their own,
same as before.

### The child's side: exactly the split the proposal asked for, mapped onto real, already-established
### channels — nothing new needed

- **Hardcoded instruction** → the child's OWN small, fixed `ContextProvider` contribution, baked into
  the `SessionFactory` at construction time — the identical shape `SkillsProviderT` already uses
  ("contributes exactly one system message," `history_and_skills_provider.hpp:47-52`) and the identical
  constraint round 4 already found (`history_provider_` "has no accessor to reconfigure after
  construction" — this MUST be baked in when the factory builds the child, not injected later). Content
  is fixed prose the host writes once (e.g. "you are one of several independently-dispatched
  sub-agents; you will receive exactly one task and one item; report a concise, structured finding").
- **Per-call instruction** → `StartRun.input` (`agent_session.hpp:294-301`), the ONE thing round 4
  already established flows from caller to child. The `delegate_review` tool's `invoke()` body builds
  this per-child from whatever the MODEL supplied as this call's task description + one item —
  ordinary tool-argument data, not itself requiring taint treatment (it's the calling model's own
  legitimate delegation, the same trust level as any other tool argument) — UNLESS that argument data
  itself carries forward content copied from an earlier, already-tainted tool result (e.g. "review
  these 5 files a prior search tool found"), in which case round 4's still-open **#8 finding becomes
  load-bearing here, concretely, not hypothetically**: `delegate_review`'s own `invoke()` must apply
  the `MemoryProvider::memory_item_to_message()` precedent (external origin + tainted + labeled +
  anti-forged, `memory_provider.hpp:327-338`) to whatever carried-forward tainted content it forwards
  into a child's `StartRun.input`. This design doesn't close #8 — it makes concrete exactly the one
  place `FanOutProvider` itself would need that fix, once #8 has one.

### A genuinely useful side effect: this closes open question #6 for the MODEL-driven path specifically

Round 3's open question #6 (self-expanding recursive fan-out) asked whether a spawned child could
itself trigger further fan-out, and left two unbuilt options (a hard one-level marker, or a threaded
`remaining_fanout_depth` counter). **A cleaner answer falls out of this round's own design, for exactly
the path round 3 was worried about (the MODEL triggering it): a spawned child's own `SessionFactory`
simply never composes `FanOutProvider` into that child's `ContextProvider` stack — meaning the child's
model has no `delegate_review`-shaped tool to call at all, so it structurally cannot trigger further
fan-out, the same "impossible by construction, not merely discouraged" bar fix 3's move-only-fresh
`SessionFactory` contract already set for the double-resume race.** This is a real, useful narrowing,
but not a full closure of #6 — stated precisely, not overclaimed: it closes the MODEL-triggered path
(the only path round 3 could describe, since round 3 itself established the model can only ever reach
`multi_agent` indirectly through a contributed tool). It does NOT constrain a HOST who writes their own
separate, bespoke tool that internally calls `multi_agent::spawn()` again from within a child's own
tool body, unrelated to whether `FanOutProvider` itself is composed in — that remains exactly as open
as round 3 left it, a host-code discipline question, not an engine-enforced one. #6 should be updated
to note this narrower, precise scope rather than being marked resolved.

### Result handoff: the actually-new question this whole design never asked before round 8, and the
### answer is "nothing new is needed"

No prior round designed how a `parallel()`/`pipeline()` call's aggregate result reaches the model that
(indirectly) triggered it. The answer, once the feature is wrapped as an ordinary tool (this round's
own framing): **`delegate_review`'s `invoke()` body calls `multi_agent::pipeline(...)`, gets back
`std::vector<result<AgentResponse>>` (round 1's own "error in its slot, caller filters" contract,
unchanged), and the tool author's own C++ code assembles ONE reply value matching the tool's declared
JSON reply schema** — extracting each successful child's `AgentResponse.message` text (or
`structured_output_json` if the child session declared an `OutputSchema<T>`, `agent_session.hpp:330`),
and marking failed slots however the tool's own reply shape names (e.g. a parallel `failed_indices`
array). That reply is returned as an ordinary `result<json::Value>` from `invoke()`, which
`invoke_tool()` (`tool_pipeline.hpp:460`) turns into a completely ordinary `ToolResult` — tainted per
the SAME existing, unmodified convention every tool result already carries
(`tool_pipeline.hpp:594`) — and flows back to the model exactly the way any other tool's result
already does. **No new engine mechanism, no new event kind, no new message shape is needed for "gộp
kết quả / chuyển giao"** — the aggregation is ordinary tool-author code, and the handoff channel is the
one every tool call already uses. This is worth stating plainly precisely because every other round in
this doc found a REAL gap needing new design — this is the one piece that, once framed as an ordinary
tool, needs none.

## Round 9 — independent red-team of round 7 (2026-08-22)

Fresh-context pass scoped to round 7 specifically (round 6's own findings already independently
reviewed once). Verdict: **the core capability/principal fix holds up correctly; the `Budget` fix is
weaker than round 7's own text claims, and round 7 wrongly named a caller class as already-safe
without checking it.** Both findings below re-verified directly against source before recording.

**MUST-FIX — round 7 names "a workflow executor body" as a legitimate `spawn()` caller without
checking whether its `EffectContext` is actually live; it is not.** Round 7's own text (above) claims
"`spawn()`'s caller (a tool body, a workflow executor body...) already legitimately holds its OWN
call-scoped `EffectContext&`," backing this only with a `ToolDescriptor::invoke` example — never
checking the executor-body half of its own claim. **Confirmed false for that half.** `ExecutorBody`
(`workflow_supervisor.hpp:196-197`) receives an `EffectContext&` sourced from `contexts_`, populated
**once**, at `initialize()` (`:538-543`) — that function's own comment states directly: "a missing/short
entry is filled with a fresh `EffectContext{}`... nothing in this codebase yet populates it
meaningfully." Nothing resembling `apply_dispatch_authority()` re-derives it per round or per delivery
— it is frozen for the executor's entire lifetime across every round it ever runs. This is the
STRUCTURALLY IDENTICAL bug round 6/7 just fixed for `AgentSession`'s own `capabilities()`/`principal()`
— a static baseline standing in for a live per-call grant — one layer over, in a caller round 7
explicitly cited as already safe. Not exploitable TODAY only because the default `EffectContext{}` has
null `capabilities`, which trips `spawn()`'s own fail-closed check — but the moment any host populates
`contexts_` with a real grant (which round 3/4's own "`executor_kind::agent`'s runtime bridge" work
plainly anticipates needing), that grant is frozen at `initialize()` time forever, never refreshed.
Round 7 should not have named this caller class as safe; it needs its own fix (an executor-body-side
authority-refresh seam, not designed here) before `spawn()` can be called from a workflow executor body
at all. (Separately confirmed FINE: `background_task()`'s detached-thread path copies `EffectContext`
synchronously before detaching, and structurally cannot hold an `AgentSession&` to pass as `spawn()`'s
`parent` at all since `captures_session_state` tools can never be backgrounded — this path cannot reach
`spawn()`, safe or not.)

**MUST-FIX — the `max_tokens` backstop is completely inert WITHIN one `parallel()` call's own batch,
not merely "looser" than `token_budget_` as round 7's text claims.** `parallel()` dispatches all N
thunks in ONE barrier (round 2's own text: "`parallel()` dispatches all N thunks at once"), and
`try_reserve(spawn_count)` is called exactly ONCE, for the whole batch, before any of its own N
children have run — so `remaining_tokens()` at that check can only reflect PREVIOUSLY completed,
already-debited calls, never anything from the batch being admitted right now. A single `parallel()`
call whose own N children collectively blow through `max_tokens_` is not caught by this mechanism AT
ALL — the backstop only ever bites a SUBSEQUENT `parallel()`/`pipeline()` call sharing the same
`Budget&`. `token_budget_`'s own precedent has no equivalent gap (checked by the same coroutine, once
per round, before the NEXT round of the SAME run — no "whole batch admitted before any of it is
measured" case exists for one session). Round 7's "refus[ing] to dispatch ANY further child once the
running total has already crossed `max_tokens_`" overstates what the mechanism does for `parallel()`
specifically — true for `pipeline()` (per-item, no barrier), false for `parallel()`'s own batch.

**Residual — `multi_agent::parallel()`/`pipeline()`'s entire viability atop `ThreadPool::submit()`
rests on a precondition `ThreadPool`'s own file banner already names as unproven, and no round states
it as load-bearing.** `thread_pool.hpp:21-43` (verified verbatim): `run_job()` resumes a job's
coroutine EXACTLY ONCE and FAILS it if not `done()` afterward — genuine external suspension (real
`AsyncMutex` contention under a DIFFERENT thread's `unlock()`, a `channel<T>` under real contention) is
explicitly NOT supported, and the file says so in its own words: "a genuinely separate, harder
problem... needs its own dedicated design → red-team → prove pass before anything depends on it." Today
this holds only by ACCIDENT (the file's own admission: "never actually triggered... happens to complete
in exactly one `resume()`" for every real caller today), resting on `AsyncMutex`'s uncontended fast path
(`async_mutex.hpp:120-124`) plus both shipped chat clients making a synchronous, blocking HTTP call with
nothing to suspend on. `spawn()`'s full call graph (a fresh session, a fresh `HistoryProviderT`, a real
`ChatClient::chat()`) happens to resolve in one `resume()` TODAY for the same reason — but no round of
this design states that as a load-bearing precondition, and nothing would catch a future
`HistoryProviderT`/tool body/`ChatClient` that introduces genuine async suspension inside a
`spawn()`-driven job: per `ThreadPool`'s own documented behavior, that job would silently FAULT rather
than deadlock, but whatever `Budget` reservation it held would never be released (round 6's already-open
`Budget` lifecycle residual compounds here). The same "proven fine under original single-caller usage,
unchecked under this design's new usage" pattern round 6 already flagged for `MemoryProvider` — a third
occurrence of it in this same document, not a one-off.

**Confirmed correct, re-verified independently:** the core `EffectContext const& parent_call_ctx` fix
for the `ToolDescriptor::invoke` path — `InvokeFn` (`tool_pipeline.hpp:65`) genuinely receives a live,
freshly-bracketed `effect_context_` by reference from `AgentSession`'s real dispatch loop
(`agent_session.hpp:983,1611,1913`); `principal_admitted_for()`'s parameter order matches round 7's
proposed call exactly (`trust/principal.hpp:122`, existing use at `:776,859`); no public
`effect_context()` accessor exists anywhere (re-confirmed, zero hits).

## Round 10 — fixing round 9's two MUST-FIXes (2026-08-22)

### Fix A: narrow `spawn()`'s callable-from contract — a workflow executor body is NOT a safe caller
### today, and the fix is restriction, not extension

Round 9 found `ExecutorBody`'s `EffectContext` (from `contexts_`, `workflow_supervisor.hpp:538-543`)
frozen once at `initialize()`, never refreshed per round. Looking closer at WHEN `initialize()` runs
settles what kind of value `contexts_[i]` actually is, and it isn't a bug in disguise — it's a
category mismatch round 7 made by treating it as the same kind of thing as `EffectContext::capabilities`.
`agent-as-workflow-executor-design-draft.md:530-543` establishes `initialize()` runs BEFORE `run_id_`
is even assigned (`run_id_` is set inside `handle(Ask<RunWorkflow,...>)`, strictly after
`initialize()`) — so `contexts_[i]` cannot possibly be derived from anything resembling a per-run,
still less a per-round, dispatched authority; it is populated at SUPERVISOR-WIRING time, before any run
exists. That makes it structurally closer to a `Tool`'s own static `capability_ceiling` (a host-declared
ceiling, set once, at construction) than to `AgentSession::effect_context_.capabilities` (a genuinely
dynamic, per-dispatch value that a Tier-3 session's `apply_dispatch_authority()` can populate
differently call to call). Round 7's mistake was treating the two as interchangeable because both are
spelled `EffectContext`.

**The fix: `spawn()`'s documented contract is narrowed, not `WorkflowSupervisor` extended.** `spawn()`
may be called from a `ToolDescriptor::invoke` body (confirmed safe, round 9: genuinely live, freshly
bracketed per call) but **NOT from inside a plain `ExecutorBody`** using `contexts_[i]` as if it were a
live per-call `parent_call_ctx` — that would silently launder a wiring-time ceiling as if it were a
live grant, the same "parameter that looks explicit but isn't tied to what was actually held for this
invocation" shape every prior FATAL in this document has been about. A host wanting to call `spawn()`
from within a workflow needs one of two real, NOT-designed-here prerequisites: (a) `WorkflowSupervisor`
gains its own genuine per-run/per-delivery authority-refresh mechanism analogous to
`apply_dispatch_authority()` (real, separate work — `RunWorkflow`/`ResumeWorkflow`/`ContinueWorkflow`
have no `RequestAuthority`-equivalent field today, confirmed by inspection of their fields as cited
elsewhere in this doc, so this is not a small addition); or (b) the workflow author instead exposes
`spawn()` through an ordinary CONTRIBUTED TOOL (round 8's own `FanOutProvider` shape) invoked from
INSIDE an `agent`-kind executor's own `AgentSession` turn loop, where `ToolDescriptor::invoke`'s
already-proven-safe `EffectContext&` sourcing applies unchanged — reusing round 8's design rather than
inventing a new safe path for raw `ExecutorBody` callers. Matches this project's own accepted
resolution shape for an honestly-scoped gap (`agent-as-workflow-executor-design-draft.md`'s own "Either
[real work or] a documented, TESTED limitation is acceptable — silently assuming coverage is not"):
this restriction should ship with a real, structural check (`spawn()` fails closed with a
distinguishing error if handed an `EffectContext` that did not come from `apply_dispatch_authority()`'s
own bracket — not fully designable here without deeper `AgentSession` changes, but the DIRECTION is:
detect and refuse, not merely document and hope) rather than being left as prose alone.

### Fix B: `max_tokens`'s within-batch gap in `parallel()` is a documented, honest asymmetry — not
### patched with a new estimation mechanism

Building a pre-flight token ESTIMATE (a caller-supplied "roughly how expensive is each child" hint,
checked against `max_tokens_` before dispatching a whole `parallel()` batch) was considered and
rejected here: an estimate can only ever be a heuristic (real per-child cost is genuinely unknowable
before the child runs, the same fact that made a true reservation impossible in round 7), and
introducing a new, unverified estimation mechanism whose OWN accuracy nobody has tested would be
exactly the kind of speculative machinery `CLAUDE.md` warns against building for a hypothetical need
before a real one is proven — a second unverified mechanism stacked on the first, not a fix.

**The honest fix: document the asymmetry explicitly, and route token-sensitive large fan-outs to
`pipeline()` instead of `parallel()`, rather than pretending `parallel()`'s `max_tokens` protects what
it structurally cannot.** `pipeline()` already has NO barrier ("item A can be in stage 3 while item B
is still in stage 1," round 1's own gap-doc citation) — each item's admission into its own first stage
is a SEPARATE `try_reserve` call, which DOES observe every prior item's real, already-debited usage
before admitting the next one, giving `pipeline()` genuine within-batch protection `parallel()`
structurally cannot have (since `parallel()`'s whole point is dispatching all N at once, before any of
them have produced anything to observe). **This is a real, load-bearing consequence for
`FanOutProvider`'s own default implementation (round 8): it should build `delegate_review` on
`multi_agent::pipeline()`, not `parallel()`, whenever the number of items is large enough that
within-batch token protection matters** — named here as the first concrete design consequence this
asymmetry has on round 8's own generic tool, not left as an abstract caveat. For call sites that
genuinely need `parallel()`'s all-at-once semantics (e.g. tight latency requirements where a barrier
handshake is unacceptable), `max_tokens` should be documented plainly as protecting ONLY the sequence
of calls sharing one `Budget&`, never a single `parallel()` batch's own internal overshoot — the same
"honestly disclosed residual" bar `ADR-071` sets for a different tradeoff (native unsandboxed process
execution) applied here to a scheduling tradeoff instead of a sandboxing one.

## Round 11 — independent red-team of round 10 (2026-08-22)

Fresh-context pass scoped to round 10. Verdict: **fix A's escape hatch (b) reopens the exact problem
it was presented as an alternative to; fix B's "use `pipeline()` instead" does not deliver the claimed
protection.** Both re-verified directly before recording. One citation-hygiene defect also found and
corrected below.

**MUST-FIX — round 10's escape hatch (b) is not an independent safe path; it is option (a)'s SAME
unbuilt prerequisite, one level down, wrongly presented as already usable.** Round 10 offered "route
`spawn()` through a `FanOutProvider`-contributed tool invoked from inside an `agent`-kind executor's
own `AgentSession` turn loop" as safe TODAY, contrasted with option (a) (a real `WorkflowSupervisor`
authority-refresh mechanism) as unbuilt work. This doesn't hold: `agent_session_as_executor_body()` (the
function that would construct that internal session) does not exist anywhere in the tree (confirmed by
grep). The cited companion doc's own capability-sourcing resolution for `executor_kind::agent`
(`agent-as-workflow-executor-design-draft.md:137-157`) states its internal session's capabilities would
be seeded from "whatever `EffectContext` the CALLER populates into `contexts[i]` **at `initialize()`
time**" — the IDENTICAL once-at-wiring-time mechanism round 10 just excluded for the direct path, now
just laundered through session construction instead. And `AgentSession::require_authority_` defaults
`false` (`agent_session.hpp:2064`) with nothing proposing Tier-3 for this internal session, so
`apply_dispatch_authority()`'s non-Tier-3 branch would set `effect_context_.capabilities = capabilities_`
— the SAME static value — on every single call, forever (`:1334-1335`). Round 9's "genuinely live,
freshly-bracketed" finding confirmed the BRACKET MECHANISM re-derives a value each call; it never
established the underlying VALUE is dynamic — for a non-Tier-3 session it structurally cannot be, since
nothing populates it from anywhere but the same `capabilities_` every time. **Corrected: options (a)
and (b) are not two independent prerequisites — they are the SAME missing prerequisite
(`WorkflowSupervisor` needs genuine per-run/per-delivery authority refresh) applying to two different
call shapes.** Until it exists, `spawn()` has exactly ONE safe caller class TODAY: a `ToolDescriptor::
invoke` body running inside an ORDINARY `AgentSession`'s own turn loop, never embedded inside anything
`WorkflowSupervisor` drives. Round 10's "either (a) or (b)" framing should read "neither, until the one
underlying fix exists."

**MUST-FIX — round 10's "use `pipeline()` instead of `parallel()` for token-sensitive large fan-outs"
does not deliver the claimed protection, and is weakest exactly where it was recommended.**
`ThreadPool::submit()` "returns a `std::future<JobOutcome>` the caller can wait on **without blocking
the calling thread for the job's own duration**" and "many threads may call `submit()` concurrently"
(`thread_pool.hpp:120-123`, verified verbatim). An admission loop calling `try_reserve` then `submit()`
per item, with nothing throttling between iterations, runs in microseconds per item while a real
`spawn()`-driven child (a fresh session, a real `ChatClient::chat()` call) takes seconds — so for a
genuinely LARGE fan-out of real model calls (exactly the case round 10 targeted), the admission loop
races through most or all N items' `try_reserve` calls before the FIRST item's usage is ever debited.
`pipeline()`'s "no barrier" property (round 1's own gap-doc citation: "item A can be in stage 3 while
item B is still in stage 1") is a design FEATURE for overlap — but it means `pipeline()` reproduces
`parallel()`'s "whole batch admitted before anything is measured" gap in practice, just bounded by "how
fast a `for` loop can call `submit()`" rather than by anything token-aware. Recommending `pipeline()`
specifically for LARGE, token-sensitive fan-outs targets exactly the case where this mitigation is
weakest, not strongest.

**Corrected fix for both, together — an explicit, separate in-flight concurrency throttle, not a
primitive-choice workaround.** The real fix neither `parallel()` nor `pipeline()` structurally provides
on its own: bound how many children may be dispatched-but-not-yet-debited AT ONCE, independent of
`max_spawns`/`max_tokens` (which bound the TOTAL, not the CONCURRENT-UNMEASURED count). A
`max_in_flight` parameter — a counting semaphore `pipeline()`'s (and, if used this way, `parallel()`'s
own batched variant's) admission loop acquires before `submit()` and releases only after that child's
`debit_tokens()` call lands — caps the blast radius of the "admitted before measured" gap to
`max_in_flight` children's worth of unmeasured spend, rather than the whole batch. This does not
eliminate the fundamental gap (per-child cost is still unknowable before it runs, unchanged from round
7's own honest framing) but makes it BOUNDED rather than proportional to batch size — a real, buildable,
not-yet-designed improvement. **`FanOutProvider` (round 8) should not rely on "use `pipeline()`" alone**
for large fan-outs — it needs this throttle, or an explicit, disclosed acceptance of the residual risk
at whatever batch size a host configures it for.

**Residual — a citation-hygiene defect in round 10 itself, corrected here.** Round 10 attributed the
"`contexts_` is set before `run_id_` exists" claim to `agent-as-workflow-executor-design-draft.md:530-543`
and named a function `handle(Ask<RunWorkflow,...>)` — neither exists in that 220-line doc or in
`workflow_supervisor.hpp`. The actual source of that claim is `ADR-032`'s own §3 finding #3 (quoted
verbatim earlier in THIS document's own opening context), and the real, current function is
`run_workflow(RunWorkflow request)` (`workflow_supervisor.hpp:598-610`, setting `run_id_` at `:603`) —
`handle(Ask<RunWorkflow,...>)` reads like the pre-ADR-037 Quark-actor-era message-handler name ADR-032
itself was written against, not current `rt::` code. **The underlying fact is independently confirmed
correct** against current source by both this document's own earlier reading and this round's
re-verification (`initialize()` at `:538-547` precedes `run_workflow()` at `:598-610`) — this is a
mis-citation, not a substantive error — but worth correcting given this document's whole method rests
on real, checked `file:line` citations, and it's the first citation across 11 rounds found to be wrong.

## Round 12 — fixing round 11's two MUST-FIXes (2026-08-22)

### Fix A: formally narrow `multi_agent`'s v1 scope to its one confirmed-safe caller class

Round 11 already reached the correct factual conclusion (no safe path exists yet from anything
`WorkflowSupervisor` drives); what was still missing was making that the DESIGN's own stated scope,
not just a red-team finding sitting in the round history. **`multi_agent::spawn()`/`parallel()`/
`pipeline()`'s v1 contract is revised: the ONLY supported caller is a `ToolDescriptor::invoke` body
running inside an ordinary `AgentSession`'s own turn loop — never a plain `ExecutorBody`, and never
(until it exists) an `agent`-kind executor's internal session.** This is a real narrowing of what round
1 originally scoped ("usable from plain tool-body code, not only from inside a workflow executor" —
open question #2's own framing already anticipated BOTH being valid; only one is, today). Extending to
either workflow-embedded shape needs a genuine, separate `WorkflowSupervisor`-level design effort (a
real per-run/per-delivery authority-refresh mechanism `RunWorkflow`/`ResumeWorkflow`/`ContinueWorkflow`
do not have today, confirmed field-by-field in round 9) — named as a real prerequisite, deliberately
NOT sketched further here: half-designing a `WorkflowSupervisor` authority model as a side effect of
fixing `multi_agent` would be exactly the "design for a hypothetical future requirement" CLAUDE.md
warns against, when the narrower, honestly-scoped v1 (one confirmed-safe caller) is sufficient to be
useful and correct on its own.

### Fix B: a real `max_in_flight` concurrency throttle, independent of `max_spawns`/`max_tokens`

The gap round 11 found (both primitives can admit far more children than have been measured, because
`ThreadPool::submit()` doesn't block the admission loop) needs a genuinely different kind of ceiling
than `Budget`'s existing two — not a TOTAL count or a TOTAL spend, but how many children may be
**dispatched-but-not-yet-debited at once**. Unlike round 10's rejected token-ESTIMATE idea (rejected for
being a second unverified, heuristic mechanism), a plain counting semaphore has no accuracy question at
all — it bounds a count, exactly, the same way `max_spawns` does; it introduces no new uncertainty, only
a new axis to bound.

```cpp
class Budget {
public:
    Budget(std::size_t max_spawns, std::uint64_t max_tokens, std::size_t max_in_flight);
    [[nodiscard]] bool try_reserve(std::size_t spawn_count);  // unchanged from round 7/10
    // BLOCKS the CALLING thread (an ordinary std::counting_semaphore::acquire()) until a slot is free.
    // Called from pipeline()'s/parallel()'s own DRIVING loop -- the code that calls ThreadPool::
    // submit() -- NEVER from inside a job already submitted TO the pool. This is deliberate: a
    // blocking wait here is an ordinary thread block, not a coroutine suspending mid-body, so it
    // never touches round 9's own residual (ThreadPool's "genuine external suspension" hazard,
    // thread_pool.hpp:21-43) at all -- that hazard is about a SUBMITTED JOB's own coroutine parking
    // across threads; this throttle lives one level up, in the code that decides whether to submit
    // another job yet.
    void acquire_in_flight_slot();
    // Called exactly once debit_tokens() lands for that child -- folded INTO debit_tokens() itself
    // (one call site, not two the caller could forget to pair) rather than a separate release() a
    // caller must remember to call independently.
    void debit_tokens(Usage const& usage);  // now also calls the semaphore's release() internally
    [[nodiscard]] std::size_t remaining_spawns() const;
    [[nodiscard]] std::uint64_t remaining_tokens() const;
private:
    std::mutex mutex_;                              // guards spawns_reserved_/tokens_spent_ together
    std::size_t   spawns_reserved_ = 0;
    std::uint64_t tokens_spent_    = 0;
    std::counting_semaphore<> in_flight_slots_;      // initialized to max_in_flight
    std::size_t const   max_spawns_;
    std::uint64_t const max_tokens_;
};
```

Dispatch shape (both primitives): `try_reserve(1)` (fails closed, unchanged) → `acquire_in_flight_slot()`
(blocks if `max_in_flight` children are already dispatched-but-undebited) → `pool.submit(...)`, where the
submitted job itself calls `debit_tokens()` on completion, which releases the slot. **This bounds the
blast radius of round 11's gap to `max_in_flight` children's worth of unmeasured spend, for BOTH
primitives, rather than the whole batch** — `max_in_flight = 1` degrades to fully sequential (zero
residual risk, zero concurrency, a real and available choice for a genuinely cost-sensitive caller);
larger values trade risk for overlap, a real, disclosed, host-tunable tradeoff rather than a silently
assumed-safe default. This does not manufacture a stronger guarantee than the domain allows (per-child
cost is still unknowable before it runs, unchanged from round 7) — it bounds EXPOSURE to that
uncertainty instead of leaving it proportional to however large a batch happens to be. `parallel()`'s
own "barrier: awaits all thunks before returning" promise (round 1) is about when results become
available, not about unconditionally submitting all N at once — a `max_in_flight` throttle is
compatible with it, not a violation of it: with `max_in_flight >= N`, `parallel()`'s behavior is
byte-for-byte unchanged from before this round.

**`FanOutProvider` (round 8) should set a real, host-chosen `max_in_flight`** rather than relying on
primitive choice alone (round 10's now-retracted "use `pipeline()`" advice) — this is the actual,
load-bearing mitigation round 8's own default tool implementation needs.

## Round 13 — can `multi_agent`'s children share a vendor batch job for the ~50% discount?
## (2026-08-22, user question)

Real, dated research already exists in this repo answering the vendor mechanics precisely:
`docs/research/2026-08-13-vendor-batch-inference-apis.md` (fetched live from OpenAI's and Anthropic's
own docs, 2026-08-13). The short answer: **`multi_agent`'s children are structurally the right SHAPE
for batch sharing (N independent, concurrently-dispatched calls), but its current in-process,
`ThreadPool`-driven design is the wrong MECHANISM to carry it — and this project already built the
right mechanism, for a different, narrower case.**

### The vendor facts (confirmed, cited, not from memory)

Both OpenAI's and Anthropic's batch APIs: one HTTP call submits N independent requests, **async,
polling only (no webhook), turnaround up to 24 hours, flat ~50% discount vs. the synchronous API**
(`vendor-batch-inference-apis.md:14-33`). Critically: **"each batch item is one complete, independent
request/response pair; there is no live round-trip"** — an agentic tool-calling loop "would need to
submit EACH ROUND as its own separate batch item, each incurring the full ~1-hour-or-more turnaround...
a 5-round tool-calling turn would take on the order of 5 HOURS, not 5 API round-trips"
(`:46-52`). The research doc's own conclusion: batch sharing "is real and buildable for the
single-shot case (N independent `chat()` calls, no tool loop)... it does not, by itself, make a
multi-round agentic `AgentSession` cheaper" (`:60-64`).

### Why `multi_agent` as designed (rounds 1-12) cannot carry this

Two independent, already-established findings in THIS document compound here:

1. **Batch turnaround (minutes to 24h) is exactly the "genuine external suspension" round 9's own
   residual already found `ThreadPool` cannot support.** `thread_pool.hpp:21-43` (cited round 9):
   `run_job()` resumes a coroutine EXACTLY ONCE and FAILS it if not `done()` — real, long-duration
   async suspension (a coroutine parking now, resumed LATER by a vendor's polling result, possibly from
   a different thread or a different process entirely after a restart) is explicitly not what this
   type does, by its own file banner's admission ("needs its own dedicated design → red-team → prove
   pass before anything depends on it"). Every `spawn()`-driven child today resolves in one `resume()`
   only because both shipped chat clients make a synchronous, blocking HTTP call — swapping that for
   "submit to a batch, then wait up to 24h for a poll to resolve" is precisely the kind of composition
   round 9 named as unsafe, not a smaller version of the same problem.
2. **Even ignoring the suspension mechanism, `spawn()`'s own children are NOT single-shot by design.**
   `spawn()` drives a full `StartRun`/`run_rounds()` `AgentSession` conversation — round 8's own
   `FanOutProvider`/`delegate_review` example child may well need a tool call mid-loop (e.g. reading the
   file it's reviewing). Per the vendor facts above, ANY child that makes more than one model round
   is disqualified from batch eligibility outright, or pays a per-round 24h-class turnaround multiplied
   by round count — worse than not batching at all. A batch-eligible `multi_agent` child would need to
   be restricted to a single, tool-free round — a real, separate constraint on `FanOutProvider`'s own
   default shape, not automatically satisfied by "it's independent of its siblings."

### This project already built the right mechanism — for a narrower, different case

`batch-inference-coalescing-design-draft.md` (Q3, already resolved in that doc) answers the EXACT
suspend-resume problem correctly: "batch-eligible pending deliveries become literal `OpenPort`s" —
reusing `request_port`/`Interaction`'s real, DURABLE suspend-resume machinery (ADR-029, proven to
survive a process restart — `test_rt_workflow_checkpoint_g2.cpp`'s 20/20 result, cited in that doc),
never `ThreadPool`. That is the structurally correct backbone for "wait up to 24 hours for a vendor
result" — `ThreadPool`+`drive()` is not, and was never meant to be (round 9's own finding). But that
mechanism is scoped, by its own explicit design, to **N `agent`-kind workflow-fan-out nodes converging
in one superstep round of one `WorkflowSupervisor` run** — not to `multi_agent::spawn()`'s own
plain-C++, non-workflow use case. And `agent`-kind executors don't exist yet (confirmed repeatedly
across this very document, most recently round 12's own fix A) — so the mechanism that WOULD make batch
sharing safe is itself gated on unbuilt prerequisites, the identical ones round 12 already found gate
workflow-embedded `spawn()` at all.

**Honest conclusion: `multi_agent`'s v1 (rounds 1-12) should not attempt vendor batch-discount sharing
at all — the right backbone for it already exists in this codebase's design space, but it lives in
`batch-inference-coalescing`'s `OpenPort`-based mechanism, gated on the same unbuilt `agent`-kind
executor prerequisite round 12 already named, not in `ThreadPool`.** If batch-discount sharing for
independent fan-out children is wanted later, the coherent path is extending
`batch-inference-coalescing`'s already-designed mechanism to cover this shape once `agent`-kind
executors land — not retrofitting a SECOND, parallel suspend-resume mechanism into `multi_agent`
itself. Named here as a real, deliberately-NOT-pursued direction, matching this document's own
repeated discipline of naming a limitation precisely rather than either ignoring it or half-building a
fix for it.

## Round 14 — fixing round 6's three remaining MUST-FIXes before implementation (2026-08-22)

### Fix 1 + 2 together: retry gets its own per-attempt identity, closing both the `Budget` and
### `already_minted` collisions at once

Both gaps (does a retry debit `max_spawns` again; does a retry collide with `already_minted`) have the
same root cause and the same fix: round 2's "`RetryPolicy` wrapper applied around one `spawn()` call"
was never given its own function, so it had no identity scheme and no `Budget` reference to act
through. **Corrected: retry is a separate, explicit function, `spawn_with_retry()`, not a bare loop
around `spawn()` a caller writes by hand:**

```cpp
template <class ChatClientT, class HistoryProviderT>
[[nodiscard]] task<result<AgentResponse>> spawn_with_retry(
    AgentSession<ChatClientT, HistoryProviderT>& parent,
    EffectContext const& parent_call_ctx,
    SessionFactory<ChatClientT, HistoryProviderT> const& factory,
    StartRun request,
    std::vector<Capability> const& narrower_grant,
    std::string const& base_spawn_identity,  // whatever round 3/open-#7's eventual per-logical-item
                                              // scheme produces -- THIS fix only adds a suffix on top
                                              // of it, it does not invent the base scheme (#7 stays open)
    RetryPolicy const& policy,
    Budget& budget);  // MANDATORY here -- unlike bare spawn(), retry's whole point is resource reuse
                       // under a real ceiling, so there is no "budget-less" retry call
```

For attempt `N` (0-based, `N < policy.max_attempts`): construct `spawn_identity = base_spawn_identity +
":attempt:" + N`, call `budget.try_reserve(1)` (fails closed if the ceiling is already hit — a retry is
not exempt, closing MUST-FIX 1's own question directly: **yes, every attempt debits `max_spawns`,
because every attempt is a genuinely new session + `branch`-mode worktree mint, the same real resource
cost the ceiling exists to bound**), then call `spawn()` with that attempt's own fresh identity feeding
whatever worktree-minting mechanism `SessionFactory` uses internally. A fresh identity per attempt makes
the `already_minted` collision (MUST-FIX 2) structurally unreachable — attempt 1 and attempt 2 mint
under DIFFERENT identities, so the existence-check-before-mint guard never sees a duplicate. On success,
`budget.debit_tokens(response.usage)`. On a failure classified `is_retryable()` (round 2's own gate,
`workflow_supervisor.hpp`'s broader `transient || resource` definition, decided round 2) with attempts
remaining, loop; on a `contract`/`policy` failure, or attempts exhausted, return the error immediately —
unchanged from round 2's own gating discipline, just now with somewhere real to run.

**Named residual, not closed here:** a FAILED attempt (one that never produces an `AgentResponse`) has
no `Usage` to debit tokens against — if the vendor actually billed for tokens generated before the
failure, this codebase has no mechanism to know that amount, so `Budget.tokens_spent_` under-counts by
whatever a failed attempt actually cost the vendor. This mirrors a real-world accounting gap, not a
design defect in `spawn_with_retry()` itself — `AgentResponse`/`Usage`'s own shape has nothing to carry
partial-failure token counts, and inventing one would be speculative machinery for a case this
codebase's real chat clients don't currently surface either.

### Fix 3: shared, WRITE-capable `MemoryProvider` sharing across concurrently-running children is
### explicitly OUT of v1 scope, not patched

`write_memory_item()`'s own existing comment (`core/memory.hpp:288-296`, cited round 6) already states
the concurrent-writer hazard is a real, acknowledged gap in an ALREADY-SHIPPED, tested primitive — fixing
`write_memory_item()` itself (a compare-and-retry loop against `last_seq()`, or a real atomic
sequence source at the ref-store level) is genuine, separate work touching a file this design doesn't
own, and would need its OWN red-team pass before trusting it, exactly the same "don't half-fix someone
else's shipped primitive as a side effect" discipline this document applied to `is_retryable()`'s
divergence (round 2) rather than silently picking a winner and editing both call sites.

**Corrected scope: round 4's "children share a `MemoryProvider`" pattern is v1-supported ONLY for
READ-ONLY sharing** (a `MemoryProvider` instance constructed with no write path reachable by concurrent
children — e.g. every child's own `cap::FsWrite` grant for that mount withheld, or the shared instance
never has `on_turn_end()`'s extraction wired for children specifically, only for the parent) **or for
provider kinds that never call `write_memory_item()` at all** (`VectorRagContextProvider`,
`HistoryProvider` composed read-only). Write-capable `MemoryProvider` sharing across concurrently
dispatched children is a real, named, NOT-YET-SAFE combination — `FanOutProvider` (round 8) must not
default to it, and a host who wires it anyway is knowingly outside what this design has verified, not
silently exposed to an undocumented race. Closing it for real is separate work against
`core/memory.hpp` itself, not bundled into this document's own implementation.

## Round 15 — v1 implemented and proven (2026-08-22)

Real code + real tests now exist for the round-12-narrowed v1 scope: `include/agentengine/rt/
multi_agent.hpp` (`Budget`, `spawn()`, `spawn_with_retry()`, `parallel()`) and `tests/
test_rt_multi_agent.cpp` (19 real positive/negative controls, 236 checks, all passing; includes a
genuine multi-threaded `Budget` contention test -- 200 real-thread races for one slot, none doubly
admitted -- and a real `ThreadPool`-backed `parallel()` run proving `max_in_flight` bounds OBSERVED
concurrent overlap, not merely configured value). No regression in `test_rt_thread_pool` or
`test_rt_agent_session_tier3_authority`.

One real bug surfaced by writing the tests, not by further reading: the first `spawn_with_retry()`
fixture assumed a flaky chat client's own failure counter would persist across retry attempts --
impossible given fix 3's own "genuinely fresh session every attempt" contract, since a fresh
`SessionFactory` call also constructs a fresh chat client with no memory of prior attempts. Fixed by
sharing the failure counter externally (a `std::shared_ptr<std::atomic<int>>`), matching how a real
vendor's own rate-limit state lives outside any one client object -- not a production defect, but
exactly the kind of thing this document's own repeated instinct (test the design, don't just read it
as correct) exists to catch, one more time, in the actual "prove" phase this time.

Per `decisions/README.md`, this is the evidence a future ADR needs to cite -- the design/red-team
rounds above are the "design → red-team" half; this round is the first real "prove" evidence. Still
open before a real ADR: round 6's un-revisited concurrent-`MemoryProvider`-write restriction (not
exercised by this implementation, since `FanOutProvider`/shared context is out of this pass's scope),
and the two structurally-unenforced restrictions this file's own banner documents rather than checks
(the executor-body caller restriction, round 12; `EffectContext` provenance, round 10's residual).

## What survives from the gap doc's original sketch, unchanged

The two-primitive split (native `multi_agent` library + a bounded, non-scripting declarative
`ScatterGather` field) and the underlying building blocks (`ThreadPool::submit()`, the `drive<T>()`
bridge, `AgentSession::start_run()`) are NOT rejected — every fix above narrows or removes a parameter's
freedom, it doesn't change the overall shape or reach for new machinery.
