# ADR-167 — Single-agent Plan/Execute mode

**Status:** Proposed — implemented, built and tested against a real compiler in this session's own
tree, self-red-teamed (disclosed as a residual, not laundered as independent), pending project-owner
sign-off and an independent review pass. Closes GitHub issue #54.

## 1. The question

MAF's Harness bundles "Agent modes" (Plan and Execute) at the single-agent level by default
(learn.microsoft.com/en-us/agent-framework/concepts/harness). AgentEngine's closest existing concept,
014 §3's Planner (Magentic) pattern, requires standing up a full workflow graph — executors, edges, a
moderator — to get plan/execute behavior at all. 002 §3's policy table has no entry for it.

Stated so it has a wrong answer: **is single-agent Plan/Execute mode (a) a lighter authoring path
onto 014's existing Planner pattern (a degenerate one-node case), or (b) a mechanism built directly
at the 002/005 layer, composing seams that already exist?** Issue #54's own body predicted (b), on
the strength of 014 §9 Q1's precedent (the single-agent turn loop was deliberately kept out of the
workflow model to avoid paying supervising-actor/typed-edge cost it cannot exercise). This ADR
verifies that prediction against the real code rather than accepting the issue body's framing on
its own authority, and — if (b) — decides which real seam(s) it should be built from.

## 2. Design

**Ground truth checked before designing, not assumed:**

- `include/agentengine/core/agent.hpp`'s own comments (M2 Phase E task E1) disclose that
  `Concurrency`/`Retry`/`Memory`/`Middleware`/`Telemetry`/`Stateless` are **"API surface only, no
  interpretation by `register_agent<A>()` (E2) or any pipeline yet."** `MaxTurns`/`TokenBudget`,
  meanwhile, are consumed as plain **runtime** fields on the real `AgentSession` (`max_turns_`,
  `set_max_turns()`) — 002 §2's "policies are types, not values" framing describes the target
  authoring surface; the actual, working mechanism for most knobs today is runtime configuration on
  `AgentSession`/`session_builder.hpp`, not the CRTP tag pipeline. A bare new `Mode<PlanExecute>` tag
  would join the inert pile, not become real behavior — this rules out option (a)'s literal reading
  ("just a lighter authoring path onto an existing mechanism") in favor of building on whatever IS
  real today.
- Two seams ARE real and wired: `ContextProvider` (005 §5, via `ComposedContextProvider`/
  `assemble_context()` — proven again this session by `TodoProvider`, ADR-166) and `PolicyDecider`
  (ADR-070, via `AgentSession::set_policy_decider()`/`resolve_approval_outcome()` — a host-supplied
  `{caller, tool, arguments_tainted} → {auto_approve, auto_deny, require_approval}` function, already
  the Delegated Decision Seam pattern CLAUDE.md's "Feature vs. safety balance" section names as the
  model for this kind of narrowing-only, host-opt-in mechanism).
- `Middleware`'s TOOL-CALL interception point (which would be the other natural place to gate a tool
  call) is confirmed **still unwired** — 002 §5's own text says so and nothing in this session's
  reading of `middleware.hpp`/`agent_session.hpp` contradicts it. Not a viable seam today.

**The mechanism (`include/agentengine/core/plan_execute_mode.hpp`):**

1. `PlanExecuteMode` — a `ContextProvider` conformer. While its gate is closed, `on_context()`
   injects a gating instruction ("plan first") and always declares a `plan_ready` tool. Once the gate
   opens, the instruction disappears (no more nagging an agent that already passed).
2. `plan_ready`'s invoke closure independently re-checks `TodoProvider::has_planned()` (from #53)
   before honoring the call — I3: a structured tool call is not, by itself, trusted as evidence of
   real planning; only an actual prior `todos_add` is.
3. `make_plan_execute_policy_decider(gate, inner, is_planning_safe)` — a `PolicyDecider` factory.
   While the gate is closed, any `policy_driven` tool call that isn't "planning-safe" (default bar:
   `effect_class::pure` and an empty `capability_ceiling` — the same notion of "harmless enough"
   `tool_pipeline_detail::is_auto_declassifiable_text_derived_call` already uses in this codebase for
   a different question, reused as precedent, not by calling that function) is `auto_deny`'d. Once
   open, it defers entirely to `inner` (or `require_approval`, i.e. "no opinion, use whatever would
   have happened with no `PolicyDecider` wired at all" — a genuine no-op, not a new default).

**The shared-state problem this design had to solve, found while implementing, not anticipated up
front:** `ComposedContextProvider` copies whatever `ProviderT` value a host constructs into its own
internally-owned `shared_ptr<ProviderT>` (`context_assembly.hpp`'s `make_context_provider_descriptor`).
A `PlanExecuteMode` holding a bare `TodoProvider const&`/pointer captured at construction time would
be watching a copy that immediately diverges from the live one running inside the session — `plan_
ready` would see whatever `ever_used` was at construction, frozen, never the session's real state.
Fixed by pulling `TodoProvider`'s three data members into one `std::shared_ptr<TodoState>` (ADR-167
commit 1, `todo_provider.hpp`) and `PlanExecuteMode`'s own gate into a `std::shared_ptr<GateState>` —
both obtained via accessor calls made ONCE, before either provider is copied anywhere, so every later
copy (inside `ComposedContextProvider`, inside a `PolicyDecider` closure, wherever) still shares the
one live heap object. `gate_handle()` returns `shared_ptr<GateState const>` deliberately — read-only,
so nothing outside `plan_ready`'s own invoke closure can flip the gate directly (caught by the
compiler during this session's own test-writing: an early test draft tried `gate->executing = true`
directly and failed to compile, exactly as intended).

## 3. Competing designs (steelmanned)

**Design A (adopted): `ContextProvider` + `PolicyDecider` composition**, as above. Real teeth (denies
actual tool calls, not just suggests via instructions), composes with an existing host policy via
`inner`, needs no changes to `AgentSession`'s turn loop.

**Design B: a degenerate one-node Planner (Magentic) workflow, as issue #54's option (a) framed it.**
Steelmanned: it would reuse 014's existing cyclic-moderator machinery and its own replan/stall
handling, and a workflow author who already knows 014 gets a familiar mental model. Rejected on the
same grounds 014 §9 Q1 already established for the *general* single-agent-as-workflow question,
applied here specifically: a moderator executor, typed edges, and `WorkflowSupervisor` round-tracking
are real, non-zero machinery a single agent's "call todos_add, then plan_ready" loop never exercises
— paying that cost to get a lighter authoring path is backwards, not lighter. 014 §9 Q1 amended (this
session) to say so directly, not just by analogy.

**Design C: a new, wired `Agent<>`-level `TOOL-CALL` middleware interception point, with
`PlanExecuteMode` as a `Middleware` instead of a `PolicyDecider`.** Steelmanned: `Middleware` is the
policy vocabulary's own named interception concept for exactly this kind of per-call gating, and 002
§5 already documents its intended shape. Rejected for this pass: 002 §5's own text is explicit that
wiring the TOOL-CALL interception point through `AgentSession`'s turn loop is "separately-scoped,
larger work this pass does not attempt" — building it as a precondition for #54 would mean this issue
silently absorbs that separately-scoped work instead of closing its own, narrower gap. `PolicyDecider`
already does the job with zero new turn-loop plumbing. If TOOL-CALL middleware is wired later,
`PlanExecuteMode`'s gating logic could migrate there without changing its `ContextProvider` half or
its external contract (`gate_handle()`/`executing()`) — noted as a residual, not built here.

## 4. Falsifiable claims

- **C1** — `PlanExecuteMode` satisfies `ContextProvider` and composes through `assemble_context()`
  the same way `TodoProvider` does. *Disproof:* `static_assert` fails to compile, or assembly drops
  its contribution.
- **C2** — The gate cannot be opened by model text alone; `plan_ready` requires real prior evidence
  (`TodoProvider::has_planned()`). *Disproof:* calling `plan_ready` with zero prior `todos_add` calls
  succeeds anyway.
- **C3** — A copy of `TodoProvider`/`PlanExecuteMode` (simulating `ComposedContextProvider`'s own
  internal copy) still shares live state with a handle captured from the pre-copy original.
  *Disproof:* a mutation made through the copy is invisible to the pre-copy handle.
- **C4** — The `PolicyDecider` this mechanism produces auto-denies non-planning-safe `policy_driven`
  tool calls while the gate is closed. *Disproof:* such a call returns anything other than
  `auto_deny`.
- **C5** — Composition with an inner `PolicyDecider` only ever narrows what `inner` would have
  allowed, never widens it. *Disproof:* an `inner` that would `auto_approve` still gets through while
  the gate is closed.
- **C6** — Once open, the composed decider is a genuine no-op relative to "no `PolicyDecider` wired
  at all" for tools with no `inner` opinion, and fully delegates to `inner` when one is given.
  *Disproof:* post-open behavior differs from the pre-`PolicyDecider` baseline, or `inner` is
  bypassed.
- **C7** — Planning-safe (pure, capability-free) tool calls remain available during the closed phase
  by default; a host-supplied `is_planning_safe` predicate can widen that floor per tool.
  *Disproof:* a pure/capability-free call is denied while closed, or the custom predicate is ignored.

## 5. Red-team round (self-conducted, disclosed — not independently fresh)

Findings that changed the shipped design from the first draft, not a rubber stamp:

1. **Chicken-and-egg on the guidance instruction.** An early sketch reused `TodoProvider`'s own
   adaptive instruction-suppression logic directly for the gate's guidance. That's wrong: `TodoProvider`
   suppresses its instructions until AFTER first use, but the plan-first instruction needs to be
   visible BEFORE the model has ever planned, or the model never learns the gate exists. Fixed by
   making `PlanExecuteMode`'s own instruction INVERTED relative to `TodoProvider`'s — present while
   closed, gone once open — a genuinely different provider with its own state, not a thin wrapper
   reusing `TodoProvider`'s adaptivity.
2. **Read-only reconnaissance during planning.** A strict "deny everything until open" gate would
   block even harmless, capability-free lookups an agent might reasonably want before committing to a
   plan. Resolved by making the "planning-safe" bar (`pure` + empty `capability_ceiling`) the default
   floor, not an absolute one — a host can widen it per tool via the `is_planning_safe` constructor
   parameter (C7), rather than this ADR guessing every host's right answer.
3. **`never_require` tools never reach the decider at all.** `resolve_approval_outcome()` only
   consults `policy_decider_` for `tool.approval == policy_driven`; `TodoProvider`'s own five tools
   are `never_require` (ADR-166) and so are structurally invisible to any `PolicyDecider` — this is
   WHY the gate cannot be built purely as a `PolicyDecider` watching for `todos_add` calls; it must
   observe `TodoState` directly instead. This shaped the whole shared-state design in §2, not a
   footnote.
4. **DoS via repeated denial.** A broken model that never plans could spin, retrying denied tool
   calls. Checked against `MaxTurns`/`TokenBudget` (already-real, already-enforced hard bounds on
   `AgentSession`): the spin is wasteful, not unbounded — this gate introduces no new unbounded-loop
   class, it just makes an existing bounded budget get spent faster in the failure case. Disclosed as
   a residual (§7), not treated as newly closed.
5. **`gate_handle()` read-only-ness, confirmed by the compiler, not just by design intent.** An early
   test draft (`gate->executing = true`, simulating "open the gate" as a shortcut) failed to compile
   against `shared_ptr<GateState const>` — the type system enforcing exactly the "only `plan_ready`'s
   own invoke closure may open the gate" property this design intends, caught as a build error during
   this session, not asserted from a comment.

## 6. Executed evidence

```
$ cd build && cmake --build . --target test_todo_provider   # regression: TodoState refactor
[... clean build ...]
$ ./tests/test_todo_provider.exe
test_todo_provider: all checks passed

$ cmake --build . --target test_plan_execute_mode
[... clean build, zero warnings under -Wall -Wextra -Werror ...]
$ ./tests/test_plan_execute_mode.exe
test_plan_execute_mode: all checks passed

$ cmake --build . --target test_memory_provider   # regression: shared context_provider.hpp/
                                                    # context_assembly.hpp/tool_pipeline.hpp seam
$ ./tests/test_memory_provider.exe
test_memory_provider: OK
```

Toolchain: Ninja + `clang++` (this session's configured `build/` tree, Windows). **Not run:** the
full test suite (too slow for a scoped addition, matching ADR-166 §6's identical disclosure); any
Linux/GCC/MSVC build (single-platform-session gap, same disclosed posture as ADR-165 §7/ADR-166 §7).

## Per-claim verdicts

| # | Claim | Verdict |
|---|-------|---------|
| C1 | Satisfies `ContextProvider`, composes via `assemble_context()` | **CORRECT** — `static_assert` compiles; R1/R4 pass. |
| C2 | Gate cannot open on model text alone | **CORRECT** — R2 passes (`plan_execute.no_plan` on zero prior planning). |
| C3 | Shared state survives `ComposedContextProvider`-style copying | **CORRECT** — R5 passes; this is the central claim the whole `TodoState`/`GateState` design exists to make true. |
| C4 | Auto-deny non-planning-safe calls while closed | **CORRECT** — R6a passes. |
| C5 | Composition only narrows an inner decider, never widens | **CORRECT** — R8a passes (an always-`auto_approve` inner is still overridden while closed). |
| C6 | Open-gate behavior is a genuine no-op / full delegation | **CORRECT** — R6b/R7/R8b pass. |
| C7 | Planning-safe default + host-widenable floor | **CORRECT** — R6b (default) and R9 (custom predicate) both pass. |

No claim resolved **INCONCLUSIVE** or **WRONG** in this pass. Honest limitations are structural, not
claim failures — see §7.

## 7. Decision and residual risks

**Decision:** Adopt Design A — `PlanExecuteMode` (`ContextProvider`) + `make_plan_execute_policy_decider`
(`PolicyDecider` factory), composed with `TodoProvider` (#53), as specified in
`include/agentengine/core/plan_execute_mode.hpp`. No new `Agent<>` policy tag. 002 §3 and 014 §9 Q1
updated to point at this mechanism rather than either a new inert tag or the Planner pattern.

**Residual risks, disclosed:**

- **Independently reviewed (2026-09-03).** A fresh reviewer (no context from this ADR's own
  implementation) checked out this branch cold, built and ran `test_plan_execute_mode` on their own
  machine, and specifically attacked the gate-bypass, shared-state, decider-correctness, and
  stacking-cleanliness questions. Every claim in §'s per-claim verdict table held — no code defect
  found; `gate_handle()`'s read-only guarantee was independently reverified (a direct-mutation attempt
  through it was independently confirmed to fail to compile), and the `feature/todo-provider` stacking
  base was confirmed to match `origin/feature/todo-provider`'s exact tip with no drift.
- **Real finding, promoted from a design-rationale aside to an explicit residual: the gate is opt-in
  PER TOOL, and a host can leave it silently toothless.** `resolve_approval_outcome()`
  (`tool_pipeline.hpp`) only ever consults a `PolicyDecider` for a tool declared
  `approval_mode::policy_driven` — a tool left at `never_require` (a common, easy-to-reach-for
  default for a tool with no capability reach) bypasses this gate's `PolicyDecider` unconditionally,
  regardless of gate state, and the independent review confirmed this directly by compiling a
  throwaway `never_require` tool through `resolve_approval_outcome()` and observing it proceed without
  ever invoking the decider. §5 finding 3 already explained *why* the gate has to work this way
  (`TodoProvider`'s own tools are deliberately `never_require`, which is what forced the shared-state
  `TodoState`/`GateState` design in the first place) — what was missing was flagging the SAME
  mechanism as a footgun for a host's OWN execute-phase tools, not just explaining why `TodoProvider`'s
  tools are exempt. Mitigated, not eliminated, by the new `count_gated_execute_tools()` diagnostic
  (`plan_execute_mode.hpp`, proven by R10a/R10b in `test_plan_execute_mode.cpp`) — a host should call
  it once against their own `ToolTable` after composing `PlanExecuteMode` and confirm it returns > 0,
  or the gate has nothing real to enforce. This is a coverage check, not an automatic fix:
  `count_gated_execute_tools()` cannot change what `approval_mode` a host declared on their own tools,
  it can only tell them they forgot to declare it correctly.
- **DoS-by-spin under `MaxTurns`/`TokenBudget`** (§5 finding 4) — bounded, not unbounded, but not
  specifically measured or mitigated beyond relying on those existing budgets.
- **No `Agent<>`-declarative authoring surface.** This is host/session-builder-level composition
  (construct `TodoProvider`+`PlanExecuteMode`, wire `set_policy_decider`), not something an agent
  author writes as a CRTP policy on `Agent<Derived, Policies...>`. Matches the honest state of most of
  002 §3's own table today (§2's design, not a gap unique to this ADR) — a future declarative
  `Mode<PlanExecute>` sugar, if ever built, should compile down to exactly this composition rather
  than inventing new turn-loop behavior.
- **Default `is_planning_safe` bar may be too strict for some hosts** (§5 finding 2) — mitigated by
  the `is_planning_safe` constructor parameter (C7), not eliminated; every host must still make its
  own call about which tools are safe during planning.
- **If/when 002 §5's TOOL-CALL middleware interception point is wired**, Design C (§3) becomes viable
  and may be a better long-term home for the deny logic — not built here, noted as future work, not a
  precondition to shipping this.
- **Windows/clang++/Ninja only this session** — same disclosed single-platform gap as ADR-165 §7/
  ADR-166 §7.
- **Does not persist gate/plan state across a checkpoint restart** — inherits `TodoProvider`'s own
  ADR-166 §7 disclosure (`TodoState` has the same no-state-bag-mechanism limitation `GateState` now
  also has, structurally, not by omission).
