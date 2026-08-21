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
- **#2 (cross-child failure policy)** — **still open**; this pass found nothing bearing on
  partial-results-vs-`failure_policy` reuse.
- **#3 (`Budget` type's scope)** — **partially resolved**: it must be engine-enforced *inside*
  `parallel()`/`pipeline()`'s dispatch loop (fix 4 above), modeled on `token_budget_`'s existing
  per-run precedent, not caller-checked-before-dispatch. The exact scope (per-call / per-session /
  nestable, matching `Workflow`'s own one-level `workflow()` nesting rule) remains open.
- **#4 (`ScatterGather`'s YAML-compiler counterpart)** — **still open**; an I6 concern orthogonal to this
  security pass.
- **#5 (attribution on independent scatter/gather child failure)** — **still open**; a result-shape
  question, not a safety violation.

## What survives from the gap doc's original sketch, unchanged

The two-primitive split (native `multi_agent` library + a bounded, non-scripting declarative
`ScatterGather` field) and the underlying building blocks (`ThreadPool::submit()`, the `drive<T>()`
bridge, `AgentSession::start_run()`) are NOT rejected — every fix above narrows or removes a parameter's
freedom, it doesn't change the overall shape or reach for new machinery.
