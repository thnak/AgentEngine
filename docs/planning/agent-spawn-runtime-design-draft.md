# `agent.spawn`'s real, wired call path — and OQ-16's manifest wired into a real session

**Status:** Draft, revised after four independent red-team passes (I2, I3, recursion/concurrency,
worktree-isolation + machine-safety — see §9 for the full finding-by-finding disposition). Not yet
implemented, no code written against this document. This is the base a later `prove → judge` pass
turns into `decisions/ADR-0NN-*.md`, per `decisions/README.md`'s required shape.

**Relates to:** `026-Agent-Facing-Runtime-Surface.md` §5 (`agent.spawn`), OpenQuestions.md OQ-14
(named `agent.spawn` the project's own "sharpest case"; its cost/depth halves are separately
Judged in `decisions/ADR-006-agent-spawn-depth-budget-bound.md` and
`decisions/ADR-031-spawn-cost-budget-actor-primitive.md`, both real, tested, and **zero callers**)
and OQ-16 (`trust/agent_library_manifest.hpp`, real, tested, **zero callers**).
`decisions/ADR-059-invoke-agent-tool-capability-attenuation.md` is the closest prior art for §5
below and is reused, not re-derived, wherever this document says so explicitly.

---

## 1. The question

`agent.spawn` has two already-Judged, already-tested primitives (`trust::SpawnBudget` for depth,
`rt::SpawnCostBudget` for a shared token pool) and zero code path that ever calls either of them. No
`Tool<>` conformer exists that a running agent can invoke to spawn a child; no mechanism exists for
constructing a *fresh* `rt::AgentSession` mid-run, from inside a parent's own tool-call loop, and
driving it to completion; no worktree is minted for a dynamically-generated child id; and the one
place this codebase already solved "how much authority does a callee get relative to its caller"
(`invoke_agent_tool()`, ADR-059) is explicitly scoped to a single dispatched tool call against an
already-known target's table, never a full nested run.

**Stated so it has a wrong answer:** can a model-invocable `agent.spawn` tool construct and drive a
brand-new child `rt::AgentSession` to completion, entirely from inside the parent's own synchronous
tool-call loop, such that the child's granted `CapabilitySet` can *never* exceed what the CALLING
session actually holds (I2), the child's identity is always a real, depth-bounded delegation from the
caller (I4/018 §2), both already-proven budget primitives fail the call closed on exhaustion before
any child session is constructed (I8), and the `agent_id`/depth/cost values a model's own tool-call
arguments could smuggle in are never themselves trusted as authority (I3) — using only primitives that
already exist and are already proven, wiring nothing new into the security-critical path except one
reviewable capability-minting function?

If the answer requires trusting anything the model's tool-call JSON supplies as a ceiling, or requires
granting a spawned child the target agent's own declared ceiling *unconditionally* (the exact bug
ADR-059 already found and fixed for `invoke_agent_tool()`), the design is wrong regardless of how
convenient it reads — CLAUDE.md's own words for this class of mistake.

---

## 2. The mechanism, as one pipeline (not six patches)

```
model tool-call: agent.spawn({agent_id, input})
        │
        ▼
[0] GUARD     — ctx.capabilities must be non-null (mirrors invoke_agent_tool's own explicit
        │         null guard, restated here, not left implicit — §9 I3-2). A per-session/
        │         per-principal spawn quota (host-configured, §4.1) is checked here too — a
        │         cheap, local, pre-pool gate. Either failing → fail closed, no side effect.
        ▼
[1] RESOLVE   — agent_id looked up in a host-curated, closed SpawnTargetRegistry.
        │         Unknown id AND ids the caller holds no cap::AgentCall for at all share ONE
        │         uniform error code (agent_spawn.not_available) — §9 I3-3. (§4.1)
        ▼
[2] DEPTH     — caller's *held* cap::AgentCall{agent_id} found and attenuated one level
        │         (trust::SpawnBudget::attenuate_for_spawn(), ADR-006).
        │         No grant, or exhausted → fail closed (same agent_spawn.not_available code
        │         when no grant exists at all; spawn_budget.depth_exhausted once a grant is
        │         known to exist). NO side effect yet. (§4.4a)
        ▼
[3] CEILING   — caller's held CapabilitySet checked (not yet minted) to cover the
        │         target's own declared capability_ceiling, via CapabilitySet::attenuate()
        │         (ADR-009, reused exactly as ADR-059 already established). Not covered
        │         → fail closed (capability.attenuation_not_subsumed — safe to differentiate,
        │         since the caller already knows it holds this id's grant). Still no side
        │         effect. (§4.5)
        ▼
[4] PUMP-ENTER — child_id minted, cost consumed, and the worktree mint request queued, all
        │         as ONE serialized unit on the single-threaded SpawnPump (§4.4c) — not three
        │         independent racy operations. Cost exhausted → fail closed before child_id or
        │         worktree touch anything. *** first real, mutating side effects *** (§4.3, §4.4b, §4.4c)
        ▼
[5] WORKTREE  — the pump's worker thread mints a fresh sub-worktree under a caller-ref-
        │         namespaced, collision-proof child_id (§4.3), branched off the CALLER's own
        │         current worktree ref, with FsRead/FsWrite scoped to what the CALLER itself
        │         already holds (§9 I2-1) — never uncapped over the whole mount.
        │         *** second real, durable side effect, but now race-proof by construction ***
        ▼
[6] MINT      — final child CapabilitySet assembled: [3]'s verified-covered target ceiling,
        │         with any further-spawn AgentCall entries re-rooted to
        │         min([2]'s child depth, the target's own declared per-id depth) — ONLY for
        │         entries that literally appear in the target's declared ceiling, never
        │         injected separately (§9 I2-2) — plus [5]'s scoped FsRead/FsWrite grants
        │         appended. Pure, local. (§4.5)
        ▼
[7] IDENTITY  — child Principal = derive_on_behalf_of(caller_principal, child_id) (018 §2,
        │         already-proven, depth-bounded independently of SpawnBudget).
        ▼
[8] RUN       — fresh rt::AgentSession<...> constructed WITH BACKGROUND-TASK EXECUTION
        │         DISABLED (§9 RC-1), configured with [6]/[7]/instructions and a host-
        │         configured, never-unbounded token_budget (§9 I2-3), driven synchronously to
        │         completion, torn down. (§4.2, §4.6)
        ▼
[9] REPLY     — child's converged text + usage mapped into AgentSpawnReply.
```

**Why this order, explicitly (the task's own hardest ordering question):**
Both budget checks happen strictly before *either* minting step, and between the two budget checks,
depth (`[2]`, pure/local, no shared state touched) comes before cost (`[4]`, mutates a shared pool) —
cheapest, least-consequential check first. Cost is still spent before the worktree is minted, not
after, for the same reason as before (an in-memory token with no refund, ADR-031 §7, is a more benign
leftover than a durable orphaned ref) — but red-team (§9 RC-3) correctly points out this reasoning
only ever compared the two *mutating* steps to each other, not to the much more common case of the
child's *own run* failing (LLM error, tool error, `max_turns` hit) *after* both mutations already
committed. That case was already implied by "no child session is ever constructed" being false once
step `[8]` starts — it is now named explicitly: **every started child run that fails or errors still
permanently costs one token from the shared pool, by ADR-031's own no-refund-by-design choice.** This
document does not change ADR-031 to add refunds (that would be reimplementing a Judged primitive, the
opposite of item 4's instruction); it bounds the blast radius instead via the per-session/per-principal
spawn quota at step `[0]` (§4.1) and accepts the rest as a named residual (§8, §9 RC-3). If step `[5]`
fails after `[4]` succeeded, the spawn attempt still fails closed and no child session is ever
constructed — the caller gets an error, not a partially-authorized child. `[6]` (capability assembly)
deliberately runs *after* `[5]`, since it needs `[5]`'s minted mount id/grants as an input; `[3]` (the
static ceiling-coverage *check*, side-effect-free) runs *before* both mutating steps precisely because
it is pure and cheap — no reason to spend a cost token or touch the ref store for a request the ceiling
check would have rejected anyway.

---

## 3. Item 5 — child capability minting: the three designs

This is where a red-team should spend the most time. `cap::AgentCall` is explicitly marked in
`trust/capability.hpp` (line ~262) as **not** auto-attenuable the way other kinds are — "recursive
authority, cascades to another agent" — and this section states, operationally, exactly what that
means: it means a spawn's capability minting can *never* be a bare `attenuate()` call against the
caller's whole held set the way an ordinary tool's ceiling is bound (`invoke_tool()` step 4/7,
`held.bind(requirement)`); it needs a dedicated function because the target's own *declared* further
recursion allowance and the caller's *live, already-consumed* depth budget are two independent
numbers that must both bound the result, and because the mechanism must manufacture brand-new
worktree authority that was never anybody's "held" capability in the first place.

### Design A — child gets the target's own declared ceiling, unconditionally

`AgentMetadata::capability_ceiling` (compiled by `register_agent<TargetAgentType>()`) is granted to
the child via `CapabilitySet::grant_root(target_metadata.capability_ceiling)` directly — the target
agent author already declared what it needs, and `register_agent<A>()` already validated that
declaration is internally consistent (every tool's own ceiling is covered by the agent's declared
ceiling, `check_capability_ceiling`).

**Steelman:** simplest to implement and to reason about in isolation — "an agent's authority is
exactly what its own author declared, full stop," which is arguably the most natural reading of
002 §1's authoring model, and it sidesteps ADR-059 §4 R3's own named residual (the declarative
`cap::decl::*` surface has no syntax for a *capped* ceiling entry, so attenuation against a capped
caller-held grant can spuriously reject an otherwise-legitimate spawn).

**Why it is rejected:** this is exactly the bug ADR-059 found and fixed for `invoke_agent_tool()` —
"a caller holding almost nothing could invoke an agent declaring a broad ceiling and the callee would
receive that FULL ceiling anyway... a textbook I2 violation." A model-supplied `agent_id` string
choosing which target's ceiling gets minted, combined with a caller that never actually held that
authority itself, is ambient authority reachable without an explicitly passed capability — precisely
what I2 forbids, and precisely the class of change CLAUDE.md says is wrong "regardless of how well it
reads." **Rejected outright**, not offered even as a host-configurable option, for the same reason
CLAUDE.md gives no casual path to relaxing I2.

### Design B — child gets the caller's held set, attenuated down to the target's declared ceiling (RECOMMENDED)

Exactly §2 steps `[3]`/`[6]` above: `caller_held.attenuate(target_metadata.capability_ceiling)` must
succeed (every entry the target declares wanting must already be `contains()`-covered by what the
CALLER holds) before anything is minted; the final grant is the target's own declared ceiling —
never wider than that — with `cap::AgentCall` entries re-rooted to the tighter of (the caller's live
remaining chain depth) and (the target's own declared per-id depth), plus the freshly-minted worktree
grants appended. This is ADR-059's already-Judged discipline ("never grant the target's declared
ceiling directly; attenuate the CALLER's own HELD set down to it... bounded on both sides at once"),
extended with the recursion-specific re-rooting step ADR-059 itself never needed (single tool calls
have no notion of "further recursion").

**Steelman:** reuses a mechanism this codebase has already designed, red-teamed, and shipped (ADR-009's
`attenuate()`, ADR-059's calling discipline) rather than inventing a second one; the child is
*structurally* incapable of ending up with more authority than its own caller, for every capability
kind at once, not just `AgentCall` — closing the exact hole Design A reopens.

**Named cost, honestly, not hidden:** ADR-059 §4 R3's residual applies here identically — because
`cap::decl::*` has no capped-ceiling declaration syntax, every `AgentMetadata.capability_ceiling`
entry a real agent can declare today is uncapped on every scalar axis (`size_cap_bytes`,
`quota_bytes`, etc.), so in practice a caller can only satisfy step `[3]` by holding an
*equally-uncapped* grant for the same kind+mount/host. A host that has deliberately hand-capped a
caller's grant (e.g. a byte quota) will see spawn fail for a target whose (uncapped-by-construction)
declared ceiling asks for that same kind — correctly fail-closed per ADR-009's already-Judged WIDENING
rule, but a genuine, inherited limitation of the surrounding declaration system, not new here.

### Design C — child gets an independent, host-declared grant; the caller only needs a boolean gate

`SpawnTargetDescriptor` carries its own fully-formed `CapabilitySet` baked in at *registration* time
(host-authored, static — a "service-account" shape). At spawn time, the only check against the
caller is whether it holds *any* `cap::AgentCall{agent_id}` at all (a boolean presence check, not a
full attenuation) — the caller's other holdings are irrelevant to what the child receives.

**Steelman:** conceptually simpler than Design B (no dependency on `attenuate()`'s capped-ceiling
limitation at all — the host just states the child's authority directly, the same way a native tool's
own `capability_ceiling` is trusted by construction, `tool_registry.hpp`'s own `native` provenance
precedent: "the compiler is the trust boundary... no outer_grant check runs"). It matches a real,
legitimate deployment shape: spawn used purely as **controlled task decomposition into a fixed,
host-trusted set of known-safe sub-agents**, where every caller holding the bare gating grant is
*meant* to unlock the full, independently-vetted sub-agent — the same trust shape as calling a
well-known internal microservice, where the caller doesn't need to independently hold everything that
service needs.

**Why it is rejected as the v1 default:** stripped of the "service account" framing, this is the
*identical* ambient-authority shape Design A has — the caller's own holdings do not bound what the
child receives, only a single boolean flag does, and a low-privilege caller holding nothing but that
one gating capability still causes the target to run with independently-granted, potentially broad
authority. It also does not fit `ADR-070`'s Delegated Decision Seam pattern (the one sanctioned way
this codebase lets a host trade away some engine-enforced safety) — that seam requires "narrows or
decides among already-possessed authority only, never mints/widens it," and Design C's whole point is
minting authority independent of what's already possessed. **Not offered even as a host toggle in
this design** — CLAUDE.md requires a *new* ADR with project-owner sign-off to relax I2, not a
per-deployment configuration flag decided inside this document.

**Decision (confirmed after red-team): Design B.** Design A is rejected outright (same reasoning
ADR-059 already used and Judged). Design C was confirmed by the I2 lens to be I2-equivalent to Design
A (a caller's own holdings never bound what the child receives, only a boolean gate does) and stays
rejected for the same reason.

None of the four red-team passes found a flaw in Design B's *core* mechanism (`attenuate()`-bounded
coverage, re-rooted `AgentCall` depth) — the worked-by-hand cross-agent-id arithmetic in the I3 pass
and the single-id re-rooting proof in the recursion pass both came back clean; C4/C5's own falsifiable
claims survive unchanged. Every finding that actually lands (§9) is in the *wiring* around Design B,
not in Design B's coverage-and-attenuation discipline itself: the freshly-minted worktree grant
appended in step `[6]` was being treated as exempt from any coverage check ("new, not inherited") when
in `branch` mode it is not exempt in the way that reasoning assumed (§9 I2-1); `child_depth_budget`'s
exact scope needed to be pinned down in prose, not just in the one shown code path (§9 I2-2); and the
child's own resource ceiling (LLM tokens) turned out to be a dimension Design B's capability vocabulary
doesn't yet model at all (§9 I2-3). All three are fixed within Design B's own frame below, not by
switching designs — none of them is evidence that a caller-attenuation-bounded mint is the wrong shape,
only that this document's first pass didn't finish threading it through every side effect the mint
touches.

---

## 4. Exact files, signatures, and what each piece does

### 4.1 `include/agentengine/rt/agent_spawn.hpp` — the tool surface + orchestration (items 1, 2)

```cpp
#pragma once
// Implements 026-Agent-Facing-Runtime-Surface.md §5 (agent.spawn) / OpenQuestions.md OQ-14's
// "sharpest case" — the real, wired call path ADR-006/ADR-031 were proven standalone against but
// never connected to. See docs/planning/agent-spawn-runtime-design-draft.md for the full design.

namespace agentengine::rt {

struct AgentSpawnArgs {
    std::string agent_id;  // host-curated registry key ONLY — never trusted as authority (I3);
                            // an id the registry doesn't recognize fails closed at step [1]
    std::string input;     // plain text, becomes the child's one user-role input message
};
AE_JSON_SCHEMA(AgentSpawnArgs, agent_id, input)
// Deliberately NO depth/budget/ceiling field anywhere on this struct — there is nothing here for
// model-written code to even attempt to widen (I3): every numeric bound this mechanism enforces
// comes from the CALLER's already-held CapabilitySet/SpawnBudget, never from the call's own args.

struct AgentSpawnReply {
    std::string output;
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
};
AE_JSON_SCHEMA(AgentSpawnReply, output, input_tokens, output_tokens)

// Poison-sentinel CRTP shape, matching ScheduleWakeupTool's own established precedent
// (rt/agent_session.hpp) exactly — this static invoke() must never actually run; it exists only so
// AgentSpawnTool satisfies Tool<Derived,...>'s contract for make_tool_descriptor_with_invoke<...>()
// to extract Args/Reply/schemas from. No Capabilities<...> policy tag declared here either, for the
// SAME reason ScheduleWakeupTool declares none: enforcement is a LIVE, per-call, per-agent_id check
// (§2 steps [2]/[3]) a static compile-time ceiling entry cannot express (it would have to name every
// spawnable agent_id and its own budget at compile time, which the model's own call selects at
// runtime from a HOST-curated set, not a compile-time-fixed one).
struct AgentSpawnTool : agentengine::Tool<AgentSpawnTool> {
    static constexpr std::string_view name = "agent.spawn";
    static constexpr std::string_view description =
        "Run a sub-agent (from a host-curated, closed set) on the given input and return its "
        "converged result. Depth- and cost-budgeted; fails closed if either is exhausted or you "
        "hold no grant for the named agent.";
    using Args = AgentSpawnArgs;
    using Reply = AgentSpawnReply;
    static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::contract,
            "agent.spawn invoked without its host-bound dispatch closure -- unreachable",
            "agent_spawn.unreachable_static_invoke"});
    }
};

// One request→response bundle handed to a host-authored ChildRunner (below) — everything the child
// needs, already computed by perform_agent_spawn() before this is ever called. The ChildRunner never
// re-derives capabilities/identity itself; it only constructs+configures+drives the concrete
// AgentSession<ChatClientT,...> type this particular target needs.
struct ChildSpawnRequest {
    agentengine::Message input;
    agentengine::CapabilitySet capabilities;
    agentengine::Principal principal;
    std::string instructions;   // target's own agent_instructions + OQ-16 push_side_summary(capabilities)
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t token_budget;  // == target->child_token_budget (§4.1, §9 I2-3); never unbounded
};

// Type-erased "construct a fresh, concretely-typed child AgentSession, wire it, drive it to
// completion, tear it down" thunk — the SAME closure-based erasure ToolDescriptor::invoke already
// performs over a concrete ToolT (core/tool_pipeline.hpp), applied here over a concrete
// ChatClientT/StateT/HistoryProviderT. Host-authored ONLY, at registry-build time — never anything
// derived from model output (I3).
using ChildRunner = std::function<agentengine::result<agentengine::AgentResponse>(ChildSpawnRequest)>;

struct SpawnTargetDescriptor {
    agentengine::AgentMetadata metadata;             // register_agent<TargetAgentType>()'s output
    std::uint64_t spawn_cost = 1;                     // HOST-configured; never model-supplied (I3)
    agentengine::sharing_mode worktree_mode = agentengine::sharing_mode::branch;  // 025 §3
    // HOST-configured LLM cost ceiling for the CHILD's own run — never std::nullopt (§9 I2-3).
    // Deliberately not defaulted to "unbounded"; a host registering a target must pick a number.
    std::uint64_t child_token_budget = 50'000;
    ChildRunner run_child;
};

// HOST-configured, per-caller-Principal soft ceiling on spawn attempts, checked at step [0] —
// BEFORE the shared, non-refundable rt::SpawnCostBudget pool is ever touched (§9 RC-3/WT-5). Bounds
// how much of the WHOLE deployment's shared pool any one caller can burn, adversarially or by
// ordinary child-run failure; it does not itself refund anything (ADR-031's own no-refund-by-design
// choice is unchanged) — it only bounds the blast radius of one caller against everyone else sharing
// the pool. Host-configured, never model-supplied.
struct SpawnQuota {
    std::uint64_t max_spawns_per_principal = 100;   // lifetime-of-process default; host tunes per deployment
};

// HOST-CURATED ONLY, never auto-discovered — the identical discipline core/tool_registry.hpp's
// ToolRegistry already establishes ("nothing is ever added except by an explicit register_target()
// call the host itself makes"). Structurally answers "could the model widen the reachable target
// set" the same way ToolRegistry answers namespace squatting: nothing here self-registers.
class SpawnTargetRegistry {
public:
    agentengine::result<void> register_target(std::string agent_id, SpawnTargetDescriptor descriptor);
    [[nodiscard]] SpawnTargetDescriptor const* find(std::string_view agent_id) const;
private:
    std::unordered_map<std::string, SpawnTargetDescriptor> targets_;
};

// The orchestration function — §2's nine steps, in order. `ctx` is the CALLER's own EffectContext
// (the tool-pipeline-supplied one, reaching this via AgentSpawnTool's custom_invoke closure);
// `ctx.capabilities` is the ONLY real ceiling this function ever trusts. `child_id` MUST be
// deterministic (I5) and is minted by the SpawnPump inside step [4], never supplied by this
// function's own caller — see §4.3's derive_spawn_child_id() for the exact formula (§9 I3-1/WT-1).
// child_id is no longer a caller-supplied parameter (§9 I3-1/WT-1 fix) — SpawnPump::submit() mints
// it internally at step [4], from host/engine-only inputs (caller_worktree_ref, ctx.principal, and
// the pump's own serialized sequence counter), never from args or any other caller-supplied value.
template <agentengine::rt::AppendLogStore StoreT>
[[nodiscard]] agentengine::result<AgentSpawnReply> perform_agent_spawn(
    AgentSpawnArgs const& args, agentengine::EffectContext& ctx, SpawnTargetRegistry const& registry,
    SpawnPump<StoreT>& pump, agentengine::Ref const& caller_worktree_ref);

}  // namespace agentengine::rt
```

`AgentSpawnTool` is wired into a session's declared tool surface via a small `ContextProvider`
conformer (`AgentSpawnToolProvider`, same file), reusing the EXISTING composition seam
(`core/composed_context_provider.hpp`) rather than editing `rt/agent_session.hpp`'s `run_rounds()` a
second time (`ScheduleWakeupTool`'s own injection site). `on_context()` returns the one
`make_tool_descriptor_with_invoke<AgentSpawnTool>(...)`-built descriptor **iff**
`ctx.capabilities->contains_kind(capability_kind::agent_call)` — the same "never advertise a tool the
model could never successfully call" precedent `ScheduleWakeupTool`'s own injection site already
established for `cap::Schedule`. The per-target-`agent_id` check happens later, inside
`perform_agent_spawn()`'s own step `[2]` — advertising the tool at all only needs *some* AgentCall
grant to exist, not a scan of which specific ids are currently reachable.

**Step `[0]` guard, restated explicitly (§9 I3-2):** `perform_agent_spawn()` requires
`ctx.capabilities != nullptr`; a null pointer fails closed with `agent_spawn.no_capabilities` before
`RESOLVE` even runs. This mirrors `invoke_agent_tool()`'s own explicit guard
(`agent_registry.hpp:561-565`, ADR-059) — restated here in the function's own contract rather than left
as an implicit property of "we reuse ADR-059's discipline," since the two functions take the ceiling
input differently (`EffectContext::capabilities*` vs. a bound `CapabilitySet const&`) and the null case
does not translate itself.

**Error-code collapsing, restated explicitly (§9 I3-3):** for any `agent_id` the caller holds **no**
`cap::AgentCall` grant for at all, steps `[1]` (unknown to the registry) and `[2]` (known, but no grant)
return the SAME error code, `agent_spawn.not_available` — a model cannot distinguish "this agent
doesn't exist" from "it exists but you were never granted it," closing the enumeration oracle a
differentiated code would otherwise give it over the host's closed registry. Once the caller is
*proven* to hold an `AgentCall` grant naming that exact id (i.e. it already knows this id exists — its
own held capability, not new information), step `[3]`'s `capability.attenuation_not_subsumed` stays
distinct; it only discloses a relationship between the caller's OWN holdings and a target it is already
permitted to know about, not information about a different agent's shape.

### 4.2 The nested-run drive mechanism (item 2)

`perform_agent_spawn()`'s step `[8]` calls `target.run_child(ChildSpawnRequest{...})`, whose
host-authored body follows the identical shape `rt::agent_workflow_executor.hpp`'s own
`agent_executor_detail::drive<T>()` already established for exactly this class of problem — a plain,
non-coroutine call site synchronously resuming a `rt::task<T>` until `done()`:

```cpp
// Inside a host-authored ChildRunner (per SpawnTargetDescriptor), for one concrete ChatClientT:
agentengine::result<agentengine::AgentResponse> run_child(ChildSpawnRequest req) {
    AgentSession<ConcreteChatClientT> child;
    child.initialize(req.principal.id, req.principal, /*token_budget=*/req.token_budget,
                      /*max_turns=*/std::optional<std::uint64_t>{25});  // a finite default,
                      // matching QuickstartSessionBuilder's own §7 red-team finding: an unbounded
                      // child turn loop is a real availability hazard, not merely a convenience gap.
                      // token_budget is now ALWAYS the host-configured
                      // SpawnTargetDescriptor::child_token_budget (§4.1, §9 I2-3), never nullopt —
                      // ChildSpawnRequest carries it as req.token_budget, computed by
                      // perform_agent_spawn() from the target descriptor, not by this function.
    child.emplace_chat_client(/* whatever this target's ChatClientT needs */);
    child.set_capabilities(&req.capabilities);
    child.set_static_instructions(req.instructions);  // §4.6
    child.set_background_execution_disabled(true);    // NEW, additive-only (§9 RC-1) — every
                      // Backgroundable tool this child's own ToolTable exposes runs its step-8
                      // invoke() SYNCHRONOUSLY instead of detaching a std::thread, closing the
                      // use-after-free this section's precondition would otherwise miss entirely.

    auto t = child.start_run(StartRun{req.input});
    while (!t.done()) t.resume();     // safe ONLY under the precondition below
    return t.take_value();
}
```

**The load-bearing precondition, named exactly as `agent_workflow_executor.hpp`'s own comment names
it for its identical pattern:** `AgentSession::session_mutex_` (`rt::AsyncMutex`) genuinely parks a
contended waiter and resumes it from a *different thread* under real cross-session contention
(`rt/async_mutex.hpp`'s own file banner) — a naive "resume until done" loop cannot survive that. This
is safe here for the same reason it is safe in `agent_workflow_executor.hpp`: the child session is
**freshly constructed, referenced by nothing else, for the duration of this one call** — nobody else
can ever contend `child`'s own `session_mutex_`, so `co_await session_mutex_.lock()` always takes the
uncontended fast path (`await_ready()` returns `true` immediately, no real suspension). The entire
recursive spawn tree this produces (parent blocks on child; child, if it itself spawns, blocks on
*its* child) executes depth-first on **one OS thread** — there is no concurrency to survive **only
because `set_background_execution_disabled(true)` above removes the one mechanism
(`start_background_task()`'s detached `std::thread` for a `Backgroundable` step-8 invoke) that could
otherwise introduce a second thread of control touching this exact `child` object concurrently with
the parent's own driving loop (§9 RC-1).** Without that call, a spawned child whose own tool table
includes any ordinary `Backgroundable` tool can legitimately reach `done()==true` and return while its
own detached worker thread is still executing — and `run_child()` then destroys `child` out from under
it, a genuine use-after-free. This was the single easiest thing for the recursion/concurrency red-team
to falsify (formerly open question 3, §7); it is now closed by construction, not merely asserted.

### 4.3 `include/agentengine/core/agent_spawn_worktree.hpp` — sub-worktree minting (item 3)

```cpp
#pragma once
// Implements 025-Worktree-and-Virtual-Filesystem.md §3's "a session may run several agents, each on
// its own subtree" for a DYNAMICALLY-generated child id — workflow/worktree_scoping.hpp's own
// mint_executor_worktrees() only works for statically-known ids drawn from a whole Workflow graph
// (ADR-032 §5); this is the same POLICY + MINTING layer, keyed by one freshly-generated id instead.
// See docs/planning/agent-spawn-runtime-design-draft.md for the full design.

namespace agentengine {

// Mirrors workflow::ExecutorWorktreeGrant exactly (same fields, same readonly caveat — Mount/
// mount_read have no pinned-digest concept yet, ADR-032 §5 — reused, not re-derived).
struct SpawnWorktreeGrant {
    SubWorktree sub;
    std::optional<Mount> mount;
    std::optional<cap::FsRead> read;
    std::optional<cap::FsWrite> write;
};

// child_id derivation (§9 WT-1/I3-1) — the formula §4.1's forward reference always meant to point
// here. child_id = to_hex(BLAKE3(caller_ref.name || 0x00 || caller_principal.id || 0x00 ||
// spawn_seq)), where spawn_seq is a HOST-maintained, per-caller-ref, monotonically-increasing
// counter minted ONLY inside the SpawnPump's single worker thread (§4.4c) — never read from,
// derived from, or influenced by AgentSpawnArgs::agent_id or ::input in any way. Every input to
// this hash is either host/engine state (caller_ref.name, caller_principal.id) or a
// pump-serialized counter; there is nothing here for a model to smuggle a path-splice attempt
// into (closes §9 WT-6's check_child_id concern structurally: a hex digest cannot contain '/').
// Deterministic per (caller, sequence number) — I5 — and, because spawn_seq is minted under the
// pump's single-writer serialization, collision-free across every concurrent caller sharing one
// SpawnPump instance, closing §9 WT-1's "no derivation stated" and (combined with mint below)
// §9 WT-2's TOCTOU race.
[[nodiscard]] std::string derive_spawn_child_id(Ref const& caller_ref,
                                                 Principal const& caller_principal,
                                                 std::uint64_t spawn_seq);

// Defense-in-depth mirror of workflow::worktree_scoping.hpp's check_executor_id (§9 WT-6) — run
// even though derive_spawn_child_id() above can only ever produce a fixed-length hex digest, for
// the identical "the failure mode of skipping this is a security-relevant string splice into a
// worktree ref name" reason that file's own comment gives. Rejects anything containing '/', '..',
// or a character outside [0-9a-f].
[[nodiscard]] result<void> check_child_id(std::string_view child_id);

// MUST be called only from the SpawnPump's single worker thread (§4.4c) — never directly from a
// caller's own tool-pipeline thread. Fails closed if a worktree already exists under this exact
// child_id (mirrors mint_executor_worktrees's own precondition-2 discipline); this check is now
// actually race-free, not merely asserted, because the pump serializes every spawn-worktree mint
// in the process onto one thread (§9 WT-2).
template <rt::AppendLogStore S>
[[nodiscard]] result<SpawnWorktreeGrant> mint_spawn_worktree(
    S& ref_store, Ref const& caller_ref, std::string const& child_id, sharing_mode mode,
    CapabilitySet const& caller_held);

}  // namespace agentengine
```

**Mount id, namespaced under the caller (§9 WT-3):** `"/agents/dynamic-spawn/" + caller_ref.name +
"/" + child_id` — NOT bare `"/agents/spawn/" + child_id` as first drafted. `mint_executor_worktrees`
gets its own collision-freedom from prefixing every ref name with `run_parent_ref.name` (itself
already unique per run, its precondition 1); the first draft of this mechanism dropped that prefix
and reproduced a flat, global, cross-session-collidable ref namespace — exactly the class of bug the
precedent it claimed to mirror was built to close. The `"dynamic-spawn"` segment is also structurally
disjoint from `mint_executor_worktrees`'s own `"/agents/" + executor_id` namespace: `check_executor_id`
already forbids `/` in an `executor_id`, so `"/agents/" + executor_id` can never have more than one
path segment after `/agents/`, while every dynamic-spawn mount has at least two (`dynamic-spawn` and
`caller_ref.name`) — the two namespaces cannot alias no matter what either id is.

**Worktree grant scoping — the central §9 I2-1 fix.** `branch` (copy-on-write off the caller's
current tree) stays the recommended default in `SpawnTargetDescriptor::worktree_mode` — not `shared`
(scope creep the model's own tool call shouldn't grant itself by default) and not `scratch`/`readonly`
(a host that wants either can still set it explicitly per target). But the grant `mint_spawn_worktree`
returns for `branch` mode is **no longer unconditionally uncapped** the way `grant_for()`'s own
`path_prefix=""` grant is. `branch` mode seeds the new mount at the caller's own CURRENT tree
content — full content, not empty scratch — so an uncapped `FsRead{path_prefix=""}`/
`FsWrite{path_prefix=""}` over that mount hands the child read+write over everything in the caller's
tree, including content the caller's OWN held capabilities never covered (a leftover file from another
mount, another turn, another tool). The fix: for `branch` mode, `mint_spawn_worktree` takes
`caller_held` and builds the returned `read`/`write` grants by **intersecting** with what the caller
itself already holds — for each `cap::FsRead`/`cap::FsWrite` entry `caller_held` has on the caller's
*own* current mount, emit the matching entry remapped onto the new mount id, same `path_prefix`, same
`quota_bytes`/`file_count_cap` (never widened to `nullopt`). A caller holding no `FsRead`/`FsWrite` on
its own mount produces a child grant of `std::nullopt` for that axis — same safety as `scratch` mode,
by construction, not by omission. `scratch`/`readonly` modes are unaffected (the mount starts empty;
there is nothing inherited to over-grant).

### 4.4 Budget wiring (item 4) — no new primitives, only call sites

**4.4a — `trust::SpawnBudget` (depth, ADR-006), in a new small header
`include/agentengine/trust/agent_spawn_capability.hpp`:**

```cpp
// One new accessor on CapabilitySet (trust/capability.hpp), mirroring find_background()/
// find_schedule()'s own already-established "pure lookup, not subsumes()-based" shape exactly —
// the live remaining_depth() of what's actually held is a different question than "is a requested
// ceiling covered," the same distinction those two functions' own comments already draw.
[[nodiscard]] std::optional<cap::AgentCall> CapabilitySet::find_agent_call(std::string const& agent_id) const;

namespace agentengine::trust {

// §2 step [2]. Fails closed if the caller holds no cap::AgentCall for this exact agent_id
// (agent_spawn.not_granted) or the held budget is exhausted (spawn_budget.depth_exhausted, ADR-006's
// own error code, unchanged). Pure — no shared/durable state touched.
[[nodiscard]] result<SpawnBudget> check_and_consume_spawn_depth(
    CapabilitySet const& caller_held, std::string const& target_agent_id);

}  // namespace agentengine::trust
```

**4.4b — `rt::SpawnCostBudget` (cost, ADR-031):** no new type at all. `perform_agent_spawn()`'s step
`[4]` submits a `ConsumeSpawnTokens{target->spawn_cost}` request to the SpawnPump (§4.4c) rather than
calling `SpawnCostBudget::consume()` and driving it with its own thread's busy-loop, closing the race
the earlier draft only *named* as an open question. **Construction scope, stated explicitly (§9
WT-4):** exactly ONE `SpawnCostBudget` instance exists per host process, constructed once by host
bootstrap code (not `session_builder.hpp`, and never per-session) — this is the only scope under which
ADR-031's own cross-session-shared-pool intent, and this document's own C3 claim, mean anything. A
fresh instance per session is a misuse this design explicitly forbids, not an implementation detail
left to the builder's discretion.

**4.4c — `SpawnPump`: the single-threaded serialization point (NEW, closes §9 RC-2/WT-1/WT-2).**

```cpp
namespace agentengine::rt {

// One dedicated worker thread, owned by the host process (constructed alongside the single
// SpawnCostBudget instance, §4.4b), with a bounded request queue. EVERY call site that would
// otherwise touch shared spawn-time state directly — SpawnCostBudget::consume(), child_id
// minting (derive_spawn_child_id() + the spawn_seq counter), and mint_spawn_worktree()'s
// read-then-write over the ref_store — submits a request and blocks the CALLING thread on a
// std::future/condition_variable for the result. The pump's own worker thread is the ONLY thread
// that ever calls resume() on any of these coroutines, or touches spawn_seq, or reads-then-writes
// the ref_store for a spawn mint — by construction, not by an assumed precondition.
// Templated on the SAME ref_store type mint_spawn_worktree() itself is templated on — the pump owns
// (a reference to) the one host-process-wide ref_store instance alongside the one SpawnCostBudget
// instance (§9 WT-4's construction-scope statement applies to the pump as a whole, not just the cost
// pool half of it).
template <rt::AppendLogStore StoreT>
class SpawnPump {
public:
    struct SpawnMintRequest {
        std::uint64_t cost;
        Ref caller_ref;
        Principal caller_principal;
        CapabilitySet const* caller_held;
        sharing_mode worktree_mode;
    };
    struct SpawnMintResult {
        SpawnTokenGrant token_grant;
        std::string child_id;
        SpawnWorktreeGrant worktree_grant;
    };
    SpawnPump(SpawnCostBudget& cost_pool, StoreT& ref_store);
    // Blocks the calling (arbitrary) thread until the pump's worker thread has processed this
    // request; internally: consume() first (cheapest to fail, and the "spend before worktree"
    // ordering from §2 is preserved inside the pump, not reordered by it), THEN child_id
    // derivation, THEN mint_spawn_worktree(). Exhaustion or mint failure ⇒ fail closed, no
    // partial state committed for this request.
    [[nodiscard]] result<SpawnMintResult> submit(SpawnMintRequest req);
private:
    SpawnCostBudget& cost_pool_;
    StoreT& ref_store_;
    std::uint64_t spawn_seq_ = 0;       // touched ONLY on the worker thread
    // ... request queue, worker thread, condition_variable — standard producer/consumer shape
};

}  // namespace agentengine::rt
```

This directly closes what the earlier draft only flagged as **open question 1** (§7): the busy-loop
was unsafe not merely in some hypothetical future multi-threaded host, but *today*, because
`AgentSession::start_background_task()` already detaches a real `std::thread` for any `Backgroundable`
tool's step 8 — a second thread of control this codebase already has, not a speculative one. Routing
`consume()` exclusively through the pump's single worker thread means a detached background-tool
thread that itself triggers a spawn (directly, or via a tool that itself calls `perform_agent_spawn()`)
can never race the calling thread's own busy-loop over the SAME `AsyncMutex`-guarded coroutine — there
is only ever one thread that resumes it, closing the double-resume/phantom-`Guard` hazard the
recursion/concurrency red-team traced through `async_mutex.hpp`'s own `LockAwaiter::await_resume()`.
The pump is a small, self-contained addition (one new type, no change to `SpawnCostBudget`,
`SpawnBudget`, or `AsyncMutex` themselves) — it does not reimplement either Judged primitive, it only
disciplines how the *call site* drives them.

### 4.5 Capability minting (item 5) — `mint_child_spawn_capabilities`

Same header as §4.4a:

```cpp
namespace agentengine::trust {

struct ChildSpawnGrant {
    CapabilitySet capabilities;
    // Carried alongside PURELY as caller-side bookkeeping/observability (e.g. for logging what
    // depth headroom this spawn consumed) — it is NEVER separately merged into `capabilities`
    // outside the re-rooting loop below (§9 I2-2). A target whose declared `capability_ceiling`
    // contains no `cap::AgentCall` entry for itself grants the child ZERO further-spawn authority,
    // full stop, regardless of how much headroom `child_depth_budget` still has. Self-recursion
    // is opt-in by the TARGET's own author, declared in its own ceiling — never ambient because a
    // caller's live chain happened to have depth left.
    SpawnBudget child_depth_budget;
};

// §2 steps [3]+[6]. Design B (§3). Fails closed (capability.attenuation_not_subsumed, ADR-009's own
// existing code, unchanged) the moment ANY entry in target_metadata.capability_ceiling isn't
// contains()-covered by caller_held — before anything below is assembled.
[[nodiscard]] result<ChildSpawnGrant> mint_child_spawn_capabilities(
    CapabilitySet const& caller_held, AgentMetadata const& target_metadata,
    SpawnWorktreeGrant const& worktree_grant, SpawnBudget const& child_depth_budget);

}  // namespace agentengine::trust
```

Algorithm, exactly:

```cpp
result<ChildSpawnGrant> mint_child_spawn_capabilities(caller_held, target_metadata, worktree_grant,
                                                        child_depth_budget) {
    // Verify coverage FIRST — attenuate()'s own all-or-nothing rule, reused unmodified (ADR-009).
    // The returned CapabilitySet is discarded; we only need the success/fail verdict, since a
    // successful attenuate() proves final_grants below (copied straight from
    // target_metadata.capability_ceiling) is already fully caller-covered, entry for entry.
    auto covered = caller_held.attenuate(target_metadata.capability_ceiling);
    if (!covered) return unexpected(covered.error());

    std::vector<Capability> final_grants;
    for (Capability const& c : target_metadata.capability_ceiling) {
        if (auto const* ac = std::get_if<cap::AgentCall>(&c)) {
            // Re-root to the TIGHTER of: the live chain depth inherited from the caller
            // (child_depth_budget, already decremented by check_and_consume_spawn_depth), and the
            // target's own declared per-id ceiling (a fresh mint_root(M), an upper bound stated by
            // the target agent's OWN author, independent of any live chain). Narrowing an
            // already-covered entry further can never violate the coverage attenuate() just proved.
            // INVARIANT (§9 I2-4, stated explicitly, not merely emergent from this one formula):
            // this computation is keyed by `ac->agent_id`, INDEPENDENTLY, once per loop iteration
            // — never a single scalar shared across every AgentCall entry in the ceiling. A target
            // declaring further-spawn authority for TWO different agent_ids gets two independently
            // tightened entries; neither can ever borrow the other's headroom. C5b (§6) is the
            // multi-id regression test this invariant needs and the single-id C5 doesn't exercise.
            SpawnBudget const tighter =
                (child_depth_budget.remaining_depth() <= ac->budget.remaining_depth())
                    ? child_depth_budget : ac->budget;
            final_grants.push_back(cap::AgentCall{ac->agent_id, tighter});
        } else {
            final_grants.push_back(c);   // verbatim -- already proven covered above
        }
    }
    // Freshly host-minted authority for a mount that never existed before this call. UNLIKE the
    // first draft of this function, worktree_grant is no longer assumed exempt from any check
    // against caller_held merely because the mount id is new (§9 I2-1) — for `scratch`/`readonly`
    // modes the mount starts empty so an uncapped grant is genuinely safe (nothing inherited to
    // over-expose); for `branch` mode, mint_spawn_worktree() (§4.3) has ALREADY intersected these
    // grants with what caller_held covers on the caller's own mount before returning them here —
    // by the time this function sees worktree_grant, it is already bounded, not merely trusted to
    // be.
    if (worktree_grant.read)  final_grants.push_back(*worktree_grant.read);
    if (worktree_grant.write) final_grants.push_back(*worktree_grant.write);

    return ChildSpawnGrant{CapabilitySet::grant_root(std::move(final_grants)), child_depth_budget};
}
```

### 4.6 OQ-16 wired into a real session (item 6)

**Session-level (`include/agentengine/rt/agent_session.hpp`, additive only):**

```cpp
// New, small, opt-in — every existing session unaffected until it's called, matching every other
// set_*() on this class (set_turn_middleware_hook, set_policy_decider, ...).
void set_static_instructions(std::string text) noexcept { static_instructions_ = std::move(text); }
```

`run_rounds()`'s existing instructions-materialization block (the one already turning
`contribution->instructions` into a `role::system` message, `agent_session.hpp` ~line 2215) gets one
small, additive change: when `static_instructions_` is non-empty, push a **second** `role::system`
message built from it, unconditionally untainted (host/engine-authored — a `CapabilitySet`-derived
string never touches model output, so no `TaintedText` declassification step is needed the way
`contribution->instructions` needs one). This reuses the already-established, already-safe precedent
that file's own comment names: "a second, independent `role::system` message from another contributor
coexists fine (both real backends already concatenate every `role::system` message they see)."

**Builder-level (`include/agentengine/core/session_builder.hpp`), one line added at the exact point
both `QuickstartSessionBuilder::build()` and `ComposedQuickstartSessionBuilder::build()` already call
`session->set_capabilities(capabilities.get());`:**

```cpp
session->set_capabilities(capabilities.get());
session->set_static_instructions(agentengine::trust::push_side_summary(*capabilities));  // OQ-16
```

`trust::push_side_summary()` is called nowhere else in the tree today (OQ-16's own "zero callers"
finding) — this is the actual wiring this item asks for. No change needed to
`trust/agent_library_manifest.hpp` itself: its `"spawn"` row is already gated on
`capability_kind::agent_call`, so a session holding any `cap::AgentCall` grant already gets
`"agent.spawn: Run a sub-agent and get its result."` in its own summary text with zero further work.

The SAME one line, using `child_grant->capabilities` in place of `*capabilities`, is what
`ChildRunner::run_child()` (§4.2) uses to build `req.instructions` — a spawned child gets an accurate,
freshly-computed manifest of *its own* granted surface, not a copy of its parent's.

**Explicitly out of scope, named per the task:** an embedded CPython `agent` module binding
(`dir(agent)`/`help(agent)` sourced from this same registry, 026 §5a) is real future work this
document does not build — 026 is still Draft, and `agent_library_manifest.hpp`'s own file banner
already names this as the next step once it exists.

---

## 5. What the child NEVER inherits (I2/I3, stated explicitly for red-team to attack)

1. **The target's own declared ceiling, unconditionally** — Design A, rejected (§3).
2. **The parent's raw held `CapabilitySet`** — only the subset both the caller holds *and* the target
   declares wanting (§4.5), narrower on both sides at once (ADR-059's own already-Judged property).
3. **The parent's own worktree** — a `shared`-mode grant is never the default; the child gets a fresh,
   separately-mounted sub-worktree (§4.3), branched, not aliased, unless a host explicitly configures
   `shared` for a specific target.
4. **The parent's raw `Principal`** — always a *derived* principal (`derive_on_behalf_of`), always
   `principal_kind::agent`, never re-labeled `human`/`service`, independently depth-bounded by
   `kMaxDelegationDepth` regardless of what `SpawnBudget` itself allows.
5. **A reference to the shared `SpawnCostBudget`/host `SpawnTargetRegistry` objects themselves** — the
   child receives only *derived values* (an already-consumed `SpawnTokenGrant`, an already-attenuated
   `CapabilitySet`); it can trigger *further* `consume()` calls through the same shared pool for its
   own sub-spawns, but cannot bypass or directly manipulate the pool.
6. **The parent's own `ToolTable`** — the child's tools are exactly `target_metadata.tools`, nothing
   the parent itself can call leaks across.
7. **Any authority derived from the model's own tool-call arguments** — `AgentSpawnArgs` carries only
   `agent_id` (checked against a host-curated closed registry, §4.1) and `input` (plain text handed to
   the child as a user message, never parsed as a capability/budget request). There is no field on
   this struct a model could populate to request a wider ceiling, a deeper budget, or a different
   worktree mode — every one of those is either host-configured (`SpawnTargetDescriptor`) or derived
   from the CALLER's own already-held state (§4.4/§4.5), never from the call itself.

---

## 6. Falsifiable claims (for a later prove step)

- **C1 (fails closed on unknown target):** `perform_agent_spawn()` with an `agent_id` not in the
  registry never constructs a child session, never touches the cost pool, never touches the ref store.
- **C2 (depth is enforced, not just checked) — scoped to targets that do NOT self-declare an
  `AgentCall` entry in their own `capability_ceiling`:** N nested real spawns against a caller-held
  `cap::AgentCall{budget=N}` succeed; spawn N+1 fails with `spawn_budget.depth_exhausted`, and no
  child session, worktree, or cost-token spend happens for that N+1st attempt (§2's ordering claim).
  This claim is now explicitly scoped (§9 RC-4) because it does NOT hold unmodified for a
  *self-recursive* target — see C2b.
- **C2b (self-recursive re-rooting terminates at the coverage bound, not at the caller's live
  headroom — intended behavior, not a defect, §9 RC-4):** a target `T` that declares
  `AgentCall<"T", M>` in its own ceiling can only be re-spawned by a chain whose live depth at each
  hop is `>= M` (coverage, §4.5's `attenuate()` call requires the full declared entry to be held) —
  so a caller with root budget `H` and a target declaring `M > 1` terminates the chain once live
  depth drops below `M`, which can be well short of `H`. This is Design B's coverage discipline
  applied consistently ("never grant MORE than declared, either") and is accepted as the correct,
  intended semantics, not fixed by loosening coverage. Hosts registering self/mutually-recursive
  targets must grant callers enough initial depth headroom for the intended recursion depth
  (`>= M × intended_hops`), or the target's author must declare a smaller `M`.
- **C3 (cost pool is truly shared and race-proof under this mechanism):** M concurrent spawn attempts,
  submitted from arbitrary/concurrent threads (no longer required to be single-OS-thread-sequential —
  the SpawnPump, §4.4c, is what makes this claim provable now instead of merely assumed), against a
  pool of K < M tokens never grant more than K total.
- **C4 (no ceiling widening, exhaustively):** for every capability kind `cap::*`, a child spawned by a
  caller holding a strictly narrower grant than the target declares wanting never receives the
  target's request (mirrors ADR-009 §5's own six-axis widening suite, extended to this call path).
- **C5 (AgentCall re-rooting picks the tighter bound):** a target declaring `AgentCall<"C", 10>` spawned
  by a caller whose own live chain depth is 2 produces a child holding `AgentCall{"C", budget=1}`
  (one less than 2, never 9).
- **C5b (re-rooting is per-agent_id, not a shared scalar, §9 I2-4):** a target declaring BOTH
  `AgentCall<"cheap_helper", 3>` and `AgentCall<"privileged_ops", 3>`, spawned by a caller holding
  live chain depth 5 for `"cheap_helper"` and live chain depth 1 for `"privileged_ops"`, produces a
  child holding `AgentCall{"cheap_helper", budget=3}` AND `AgentCall{"privileged_ops", budget=1}` —
  neither entry's tightened value is ever influenced by the other's inputs. This is the multi-id
  regression test C5's single-id example cannot catch.
- **C6 (worktree isolation):** a child with `worktree_mode::branch` writing to its own mount never
  becomes visible on the caller's own worktree ref until an explicit merge-on-completion step (not
  built by this design — named as residual, §8) runs; a child crash/error leaves the caller's own
  tree untouched.
- **C6b (branch-mode grant is caller-bounded, §9 I2-1):** a caller holding
  `cap::FsRead{mount_id="root", path_prefix="/workspace"}` and nothing broader spawns a `branch`-mode
  target against a caller worktree whose tree also contains `/scratch/unrelated.json` (content outside
  `/workspace`); the child's minted `cap::FsRead` on the new mount has `path_prefix="/workspace"`, and
  attempting to read `/scratch/unrelated.json` through the child's own granted capability fails — even
  though the file is physically present in the branched mount's content.
- **C7 (I4 attribution):** every audit record for an effect the child performs carries a `Principal`
  whose `on_behalf_of` chain traces back to the original caller through `derive_on_behalf_of`'s own
  already-proven mechanism.
- **C8 (child use-after-free is impossible with backgrounding disabled, and IS reproducible without
  it, §9 RC-1):** a spawned child whose own `ToolTable` includes an ordinary `Backgroundable` tool,
  invoked during its own run, never leaves a detached thread executing past `run_child()`'s own
  return — verified under ASan/TSan. A NEGATIVE control (a build with
  `set_background_execution_disabled(true)` removed) is expected to reproduce a use-after-free under
  the same test, proving the fix is load-bearing and not merely asserted.
- **C9 (pump serialization closes the AsyncMutex hazard, §9 RC-2/WT-2):** two concurrent callers on
  separate OS threads, each submitting a spawn request (touching the same `SpawnCostBudget` and the
  same ref_store) through the SAME `SpawnPump` instance, never corrupt `AsyncMutex::held_`/`waiters_`
  and never alias two callers' worktree mints onto the same `child_id` — verified under TSan. A
  NEGATIVE control bypassing the pump (calling `consume()`/`mint_spawn_worktree()` directly from both
  threads) is expected to reproduce the corruption/aliasing this design's earlier draft only predicted
  from reading the source, confirming C8/C9 close what §7's old open questions 1 and 3 left open.

---

## 7. Open questions — status after red-team

The four questions the first draft flagged are resolved as follows; see §9 for the finding-by-finding
trace each resolution is grounded in.

1. **~~Is the synchronous busy-loop drive of `SpawnCostBudget::consume()` safe?~~ CLOSED.** It was not
   safe, and not merely hypothetically — `AgentSession::start_background_task()` already detaches a
   real `std::thread` today (RC-2, §9). Fixed by the SpawnPump (§4.4c): every call site submits to one
   dedicated worker thread instead of driving the coroutine with its own thread. `ToolDescriptor::invoke`
   does NOT need to become a coroutine; the pump absorbs the concurrency instead.
2. **~~Is Design B's `cap::AgentCall` re-rooting rule right?~~ CLOSED, with the semantics named
   explicitly.** The adversarial self-recursion case this question asked a red-team to build was built
   (RC-4, §9) and confirms the rule is SAFE (never over-grants — a target cannot re-declare its way to
   more depth than the live caller chain independently allows) but has a real, previously-unstated
   AVAILABILITY consequence: a self-recursive target's own declared per-hop `M` can terminate a chain
   well short of the caller's real root budget. §6's C2/C2b now state this precisely; it is accepted as
   intended semantics (a target's author gets to bound its own recursion), not loosened.
3. **~~Does the "fresh child, uncontended `session_mutex_`" argument survive a hostile child?~~
   CLOSED.** It does not survive unmodified — a `Backgroundable` tool inside the child's own run
   introduces exactly the second thread this question worried about, and the earlier draft's argument
   never accounted for it (RC-1, §9: a genuine use-after-free, not merely a contended mutex). Fixed by
   `set_background_execution_disabled(true)` on every spawned child (§4.2) — no detached thread is ever
   created inside a child session, so the "only ever driven by one thread" property this argument
   depends on is now actually true, not merely aspirational.
4. **~~Is `mint_child_spawn_capabilities` correct for an EMPTY declared ceiling?~~ CLOSED — vacuous
   success confirmed correct, not a gap.** `attenuate()` against an empty `narrower` list vacuously
   succeeds regardless of what the caller holds, matching the same `std::all_of`-over-empty-range shape
   ADR-023 already relies on elsewhere in this codebase. This is the CORRECT behavior: a target
   declaring it needs nothing further should be spawnable by anyone holding its bare `AgentCall` gate,
   and (per §9 I2-2) an empty ceiling also means the child receives zero further-spawn authority no
   matter how much `child_depth_budget` headroom the caller had — the two properties compose correctly
   without a special case.

No open questions remain from the first draft. §9 lists the findings the four red-team passes raised
independently of these four (some overlapping, most new); several of those remain named residuals
rather than closed, and are not to be confused with these four, which are fully resolved.

---

## 8. Residuals, named up front (not solved by this design)

Each residual below is accepted in this project's own ADR-070/ADR-071 style: named explicitly, bounded
by an already-shipped or explicitly-designed mitigation where one exists, and never silently dropped.

- **No merge-on-completion for a `branch`-mode child worktree.** `workflow/worktree_scoping.hpp`'s own
  `make_merge_on_join_hook()` exists for the graph-shaped case; a dynamic spawn's own equivalent
  (merge the child's branch back into the caller's live tree once the child converges, or discard on
  error) is not designed here — a spawned child's filesystem work stays isolated on its own branch
  ref unless a future pass wires an equivalent hook for this call path.
- **No reclamation/GC of spawn-minted worktree refs (§9 WT-7).** `core/worktree.hpp` has no
  delete/prune entry point at all, for ANY caller, not just this one; every successful spawn leaves a
  permanent ref in the durable store for the life of the process. Mitigated, not solved, by the
  per-principal `SpawnQuota` (§4.1) bounding how many refs any one caller can mint and by the shared
  `SpawnCostBudget`'s own hard ceiling on total spawns process-wide; a real fix needs a ref-store
  deletion primitive this design does not add (out of scope — it would touch `core/worktree.hpp`
  itself, a change with a much larger blast radius than this document's file list).
- **No refund on ordinary (non-adversarial) child-run failure, and no live-concurrency ceiling
  distinct from lifetime spend (§9 RC-3/WT-5).** `SpawnCostBudget` is by-design non-refundable
  (ADR-031 §7); every child run that fails after step `[4]` — an LLM backend error, a tool error, the
  child's own `max_turns`/`token_budget` being hit — permanently costs one token, same as a successful
  run, with no live-vs-total distinction the way `background_task()`'s `max_concurrent` gate has for
  ordinary backgrounded tools. This is accepted as inherent to reusing ADR-031's own Judged, no-refund
  design rather than reimplementing it (item 4's own instruction). Mitigated by the per-principal
  `SpawnQuota` (§4.1, §9 RC-3) bounding one caller's blast radius against the shared pool, and named
  here as an operational residual: hosts must size `SpawnCostBudget::total_tokens` and monitor its
  remaining balance, the same way any non-renewing resource pool needs operational sizing.
- **No cancellation.** A child spawned mid-round cannot be canceled independently of the parent's own
  run; the parent blocks until the child's `start_run()` fully resolves.
- **`ChildRunner`'s own `ChatClientT` selection is fully host-authored, static per registered target**
  — nothing here lets the model choose a *different backend* for a spawned child than whatever the
  host wired into that `agent_id`'s `SpawnTargetDescriptor` at registration time. This is deliberate
  (I3: a model-chosen backend/model string is not something this design trusts), named so it is not
  mistaken for an oversight.
- **The embedded CPython `agent` module binding for `dir()`/`help()`** (026 §5a, item 6's own explicit
  scope line) — real future work, not attempted here.
- **Wall-clock/deadline budgeting for spawn** — ADR-006/ADR-031 both already name this as open; this
  design does not close it either (the child's own `deadline` field on `ChildSpawnRequest` is threaded
  through from the caller's `ctx.deadline`, but nothing here adds a NEW, spawn-specific wall-clock cap).

---

## 9. Red-team findings and disposition

Four independent passes attacked this document: **I2** (no ambient authority), **I3** (model output is
data, never authority), **recursion/concurrency** (RC), and **worktree-isolation + machine-safety**
(WT). Every finding is listed below with its disposition — **Closed** (the design above now prevents
it) or **Accepted residual** (named explicitly, mitigated where possible, in this project's own
ADR-070/ADR-071 style — never silently dropped).

| ID | Severity | Finding (one line) | Disposition |
|----|----------|---------------------|--------------|
| **I2-1** | Critical | `branch`-mode worktree grant was uncapped over the caller's WHOLE tree, not scoped to what the caller's own held `FsRead`/`FsWrite` covered | **Closed.** §4.3: `mint_spawn_worktree` now intersects the branch-mode grant with `caller_held`'s own FS entries, remapped onto the new mount; a caller with no FS grant gets none. C6b (§6) is the regression test. |
| **I2-2** | High | `ChildSpawnGrant::child_depth_budget` looked like it might be injected into the child's grants independent of the target's declared ceiling, bypassing a target author's choice to decline self-recursion | **Closed** — clarified, not a code change: §4.5's struct comment now states explicitly this field is bookkeeping only; further-spawn authority exists iff it is a literal entry in `target_metadata.capability_ceiling`. Reaffirmed by the resolved open question 4 (§7) and I2-2's cross-reference in the §2 pipeline diagram. |
| **I2-3** | Medium | Child session's LLM token budget was `std::nullopt` (unbounded) — no capability bounds spawn's real-world compute/API-spend effect | **Closed.** §4.1: `SpawnTargetDescriptor::child_token_budget` is a required, host-configured, non-optional field (default 50,000, never unbounded); §4.2's `run_child()` uses it instead of `nullopt`. |
| **I2-4** | Medium | `min()` re-rooting formula's per-`agent_id` independence was an emergent property of the code shown, not a stated invariant or a multi-entry test | **Closed** — invariant now stated explicitly in §4.5's algorithm comment, and C5b (§6) is the multi-id regression test C5 didn't cover. |
| **I3-1** | Medium | `child_id`'s derivation was forward-referenced to a section (§4.6) that never actually stated it | **Closed.** §4.3 now states the exact formula (`BLAKE3` over caller ref name, caller principal id, and a pump-serialized sequence counter) — no model-supplied input anywhere in it. Same finding as WT-1; one fix closes both. |
| **I3-2** | Low | The `ctx.capabilities` null-guard `invoke_agent_tool()` uses (ADR-059) was never restated for `perform_agent_spawn`, even though the document claims to reuse that discipline | **Closed.** §2 step `[0]` and §4.1 now state the guard explicitly: null capabilities fail closed with `agent_spawn.no_capabilities` before `RESOLVE` runs. |
| **I3-3** | Low | Three distinct error codes across steps `[1]`/`[2]`/`[3]` let a low-privilege caller differentially probe the host's closed registry and other agents' declared ceiling shape | **Closed.** §4.1/§2: unknown-id and no-grant-held now share one uniform code (`agent_spawn.not_available`); only once a caller is proven to already hold a grant for that exact id (information it already has) does step `[3]`'s code stay distinct. |
| **RC-1** | Critical | A spawned child's `Backgroundable` tool could detach a `std::thread` that outlives the stack-local `child` session `run_child()` destroys — a genuine use-after-free | **Closed.** §4.2: every spawned child is constructed with `set_background_execution_disabled(true)` (new, additive `AgentSession` method) — step 8 always runs synchronously inside a child, no detached thread is ever created. C8 (§6) is the positive+negative regression test, to be run under ASan/TSan per the task's own build convention. |
| **RC-2** | High | The single-OS-thread precondition for busy-looping `SpawnCostBudget::consume()` was already false today (`start_background_task()` is real and wired), and violating it corrupts `AsyncMutex` internal state, not just races a counter | **Closed.** §4.4c: the new `SpawnPump` routes every `consume()`/child_id/worktree-mint call through one dedicated worker thread, regardless of which thread submits the request — no caller thread ever resumes the coroutine itself. C9 (§6) is the regression test, to be run under TSan. |
| **RC-3** | High | Spending the shared, non-refundable cost token BEFORE the child is known to succeed turns ordinary child-run failure (not just the rare worktree-mint race) into a cheap, cross-tenant exhaustion vector against the whole shared pool | **Accepted residual**, per ADR-031's own Judged no-refund-by-design choice (not reimplemented here, per item 4's instruction). Mitigated by the new per-principal `SpawnQuota` (§4.1) bounding any one caller's share of the pool; the underlying non-renewing-resource operational risk is named explicitly in §8 rather than left implicit, with a stated host sizing/monitoring obligation. |
| **RC-4** | Medium | A self-recursive target's own declared `AgentCall<Self, M>` ceiling entry forces the coverage check to demand live depth `>= M` at every hop, terminating the chain well short of the caller's real root budget — contradicting the original C2 | **Closed by clarification, not by loosening coverage.** §6 splits C2 (non-self-referential targets) from new C2b (self-referential — states and accepts the tighter, correct bound); §7 open question 2 is resolved as "confirmed safe, availability consequence now named." Hosts registering self-recursive targets must provision caller depth accordingly. |
| **WT-1** | High | Same root cause as I3-1 — `child_id` derivation was promised but never written down anywhere in the document | **Closed** — see I3-1. |
| **WT-2** | Critical | `mint_spawn_worktree`'s "fails closed if a worktree already exists" claim was a racy check-then-act over `commit_ref`, which has no compare-and-swap — two concurrent spawns could alias one child's mount onto another's ref, a silent cross-child data leak | **Closed.** §4.3/§4.4c: `mint_spawn_worktree` is now callable ONLY from the `SpawnPump`'s single worker thread — the read-then-write is race-free because there is structurally only one writer, not because concurrent callers are assumed not to exist. C9 (§6) is the regression test. |
| **WT-3** | High | The spawn worktree's mount id (`"/agents/spawn/" + child_id`) had no caller-scoping prefix, unlike `mint_executor_worktrees`'s own `run_parent_ref.name`-prefixed scheme — a flat, global, cross-session-collidable namespace | **Closed.** §4.3: mount id is now `"/agents/dynamic-spawn/" + caller_ref.name + "/" + child_id`, with an explicit disjointness argument against `mint_executor_worktrees`'s own namespace. |
| **WT-4** | High | `SpawnCostBudget`'s construction scope/lifetime (one host-wide pool vs. one per session) was never stated, and the design's own width-bound claims are meaningless if it's minted per-session | **Closed** — stated explicitly in §4.4b: exactly one instance per host process, constructed by host bootstrap code, never by `session_builder.hpp` or per-session. |
| **WT-5** | Medium | No live-concurrency ceiling distinct from lifetime spend, and no refund even after a child completes and frees its resources — a well-behaved, heavily-spawning deployment can permanently exhaust the pool | **Accepted residual** — same disposition as RC-3 (they are the same underlying ADR-031 no-refund property viewed from two angles); named together in §8 with the same `SpawnQuota` mitigation. |
| **WT-6** | Medium | No `check_executor_id`-equivalent defense-in-depth existed for `child_id` before it is spliced into a worktree mount id/ref name | **Closed.** §4.3: `derive_spawn_child_id()`'s output is structurally a fixed-length hex digest (no model input feeds it at all, so no splice is possible by construction), and `check_child_id()` is added anyway as an explicit defense-in-depth mirror of `check_executor_id`, per this project's own "defensive even when provably unreachable" precedent. |
| **WT-7** | Medium | Every spawn leaves a permanent ref in the durable store with no delete/prune/GC path anywhere in `core/worktree.hpp` — dynamic, model-triggerable, repeatable spawn turns a latent limitation into an active slow-drip storage-exhaustion vector | **Accepted residual** — named explicitly in §8, mitigated by `SpawnQuota` and the shared cost pool's own hard ceiling; a real fix needs a ref-store deletion primitive outside this document's file list (`core/worktree.hpp` itself), out of scope for an `agent.spawn`-scoped change. |

**Summary:** 11 of 18 findings are closed by a concrete change to this document (a new field, a new
type, a restated invariant, or a corrected formula); 5 are accepted, named residuals inherited from
already-Judged primitives this design is instructed to reuse rather than reimplement (ADR-031's
no-refund choice, and `core/worktree.hpp`'s lack of a deletion primitive), each carrying a stated
mitigation; 2 (I3-1/WT-1 and RC-4's underlying self-recursion mechanics) are closed by writing down a
specification/invariant that was previously only implied. None of the 18 findings is dropped silently.
