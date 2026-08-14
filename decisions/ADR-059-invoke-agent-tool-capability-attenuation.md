# ADR-059 — `invoke_agent_tool()` must attenuate against the caller's own held capabilities

**Status:** Proposed. Red-teamed and implemented; the fix survived red-team as originally proposed
(no correction needed). Awaiting the project owner's judge pass (`OpenQuestions.md` OQ-11 — only the
owner marks an ADR Judged).

**Relates to:** `026-Agent-Facing-Runtime-Surface.md` §5 (`agent.spawn` — "inherits an attenuated
capability set... a spawned agent can never exceed its parent," the constraint this ADR closes a real
violation of), `include/agentengine/core/agent_registry.hpp` (`invoke_agent_tool()`, the function this
ADR fixes), `include/agentengine/trust/capability.hpp` (`CapabilitySet::attenuate()`, the already-
proven ADR-009 primitive this fix reuses), `include/agentengine/trust/spawn_budget.hpp` (`SpawnBudget`,
ADR-006, depth — orthogonal to this fix, untouched), `include/agentengine/rt/spawn_cost_budget.hpp`
(`rt::SpawnCostBudget`, ADR-031/ADR-037 — orthogonal, untouched). **Explicitly scoped narrower than
"build real `agent.spawn`"** — see §3.

## 1. The question

`invoke_agent_tool()` (`core/agent_registry.hpp:505-510`) is the one function that dispatches a tool
call against a target agent's compiled metadata — today the only real glue between `AgentMetadata` and
`tool_pipeline.hpp`'s actual invocation pipeline, and the mechanism a real `agent.spawn`/agent-to-agent
call path would eventually build on:

```cpp
[[nodiscard]] inline ToolResult invoke_agent_tool(AgentMetadata const& meta, ToolCallRequest const& request,
                                                   EffectContext& ctx, ApprovalDecider const& approve = {},
                                                   ToolInvocationAudit* audit_out = nullptr) {
    CapabilitySet const ceiling = CapabilitySet::grant_root(meta.capability_ceiling);
    return invoke_tool(meta.tools, ceiling, request, ctx, approve, audit_out);
}
```

**The bug:** `ceiling` is built entirely from `meta.capability_ceiling` — the TARGET agent's own
compiled, declared ceiling — via `grant_root()`, the function this codebase reserves specifically for
minting a **root** grant with no parent to check against (`capability.hpp:550`'s own comment: "the one
explicitly-named, greppable entry point" for that). The `EffectContext& ctx` parameter this function
already receives carries `ctx.capabilities` — the CALLER's own actually-held `CapabilitySet const*`
(`effect_context.hpp:18`) — and it is never read. **The target's own ceiling is granted unconditionally,
regardless of what the caller invoking it actually holds.**

This is a real, structural ambient-authority hole matching exactly the shape I2 forbids: if this
function is ever the mechanism behind `agent.spawn` (026 §5's own explicit design constraint: "a
spawned agent can never exceed its parent"), a caller holding almost nothing could invoke an agent
declaring a broad ceiling and the callee would receive that FULL ceiling anyway — authority reachable
without an explicitly passed capability, the textbook I2 violation.

**Stated so it has a wrong answer:** can `invoke_agent_tool()` grant only the intersection of what the
caller actually holds and what the target declares — reusing `CapabilitySet::attenuate()`, the
already-proven ADR-009 primitive built for exactly this narrowing shape — without breaking the one
existing test that exercises this function today, and without silently widening any OTHER existing
attenuation rule this codebase already relies on?

## 2. The fix under red-team

Replace the `grant_root()` line with an attenuation against the caller's own held set:

```cpp
if (!ctx.capabilities) {
    error e{failure_class::policy, "invoke_agent_tool: caller has no capabilities to attenuate from",
            "agent_call.no_caller_capabilities"};
    return finish-as-error(...);  // exact shape TBD against tool_pipeline.hpp's own error-construction idiom
}
result<CapabilitySet> attenuated = ctx.capabilities->attenuate(meta.capability_ceiling);
if (!attenuated) {
    return finish-as-error(attenuated.error());  // caller doesn't cover the target's own declared ceiling
}
return invoke_tool(meta.tools, *attenuated, request, ctx, approve, audit_out);
```

`CapabilitySet::attenuate()` (`capability.hpp:609-618`) already exists, already proven (ADR-009): it
checks every capability in the requested narrower list is `contains()`-covered by the parent set, and
either returns a new, correctly-narrowed `CapabilitySet` or fails. Reusing it here means the target's
own declared ceiling becomes the REQUESTED narrower set, checked against the CALLER's held set as the
parent — the caller can never grant the callee more than it itself holds, and the callee still never
gets more than its own declared ceiling either (both bounds enforced by the same call).

**Where a red-team should aim:**
- `attenuate()`'s own documented rule — "a capped parent and an uncapped request is a WIDENING attempt,
  never an implicitly-fine omission" (`capability.hpp:74`'s comment, and the real bug Milestone 3
  Phase G4 found and fixed for a DIFFERENT call site under the identical rule, `Internal_open`'s
  write-mode gate). Does `meta.capability_ceiling` — a target agent's OWN compiled ceiling, which may
  declare capabilities with NO cap set (an uncapped request) — collide with a caller's held capability
  that IS capped (e.g. a `FsWrite` with a real `quota_bytes`), the exact shape that bug was? If so, is
  that the CORRECT fail-closed behavior here (the target's ceiling asking for literally unrestricted
  access when the caller only holds a capped grant should fail, matching G4's own resolution), or does
  it produce a surprising rejection for an ordinary, legitimate case that needs a different fix?
- The existing test, `tests/test_agent_tool_invocation.cpp`, calls `invoke_agent_tool()` with a
  **default-constructed `EffectContext`** — `ctx.capabilities` is `nullptr`. This positive test
  currently expects success. Under this fix it must fail (no caller capabilities to attenuate from) —
  correctly, matching this codebase's own established idiom ("null capabilities_ denies per-call
  rather than aborting the run," ADR-027's own named residual for the identical null-check shape
  elsewhere in `AgentSession`). This is a **deliberate, correct behavior change to an existing test**,
  not a regression to paper over — the test needs a real, covering `CapabilitySet` constructed and
  attached to `ctx.capabilities`, changing what it actually proves (from "the glue works" to "the glue
  works AND enforces attenuation").
- Does anything else in this codebase call `invoke_agent_tool()` with an `EffectContext` that does NOT
  represent a real caller's held set (i.e., is this test's shape a one-off, or a pattern other code
  might rely on)? Confirmed already (§1's own grep): zero other callers exist in production code, but a
  red-team should re-confirm this directly rather than trust the earlier finding secondhand.
- Is `CapabilitySet::attenuate()`'s error shape (a generic `result<CapabilitySet>` failure) rich enough
  to distinguish "caller holds nothing at all for this capability kind" from "caller holds it but with
  a narrower cap than the target needs" — does 026 §5/007 need either distinction surfaced, or is a
  single fail-closed error sufficient for this pass (matching this ADR's own narrow scope)?

## 3. Deliberately out of scope

This ADR does **not** build a real `agent.spawn` tool, a nested `AgentSession` run, sub-worktree
wiring for a spawned child, or wire `SpawnBudget`/`rt::SpawnCostBudget` to any call path. It fixes one
function's capability-granting bug so that WHENEVER a real spawn/invoke mechanism is eventually built
on top of `invoke_agent_tool()`, it inherits correct attenuation by construction rather than needing
to remember to add it later. `agent.spawn` itself remains unwired and unbuilt after this ADR, exactly
as before — a materially larger, separately-scoped effort (per the Milestone 3 Phase G2 scoping
precedent already on record for this exact module).

## 4. The red-team attack

Read directly (not the ADR's own paraphrase above) before attacking: `include/agentengine/trust/
capability.hpp` in full, `include/agentengine/core/agent_registry.hpp` in full,
`tests/test_agent_tool_invocation.cpp`, and the M3 Phase G4 `find_fs_write` writeup in
`docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md` lines ~1259-1267.

**R1 — bug shape confirmed exactly as claimed.** `invoke_agent_tool()`
(`core/agent_registry.hpp:505` before this pass, function body at the old lines 508-509) built
`ceiling` from `CapabilitySet::grant_root(meta.capability_ceiling)` — the TARGET's own compiled
ceiling, minted via `grant_root`, the function `capability.hpp:558`'s own comment names as "the one
explicitly-named, greppable entry point" for minting root grants with no parent to check against.
`ctx.capabilities` (`effect_context.hpp:18`, the CALLER's own held set) was read nowhere in the old
function body. **Verdict: fatal, confirmed.** This is exactly the I2 ambient-authority shape CLAUDE.md
names as the class of bug that "is wrong regardless of how well it reads" — reachable today only
through the one test below, but structurally present in the one function 026 §5's `agent.spawn`
would build on.

**R2 — zero production callers, re-confirmed directly (not secondhand).** Grepped
`invoke_agent_tool` across the full tree: 14 files match. Of those, the only files containing an
actual *call* (not a comment) are `tests/test_agent_tool_invocation.cpp` and `tests/CMakeLists.txt`
(test registration). Every other hit is a comment:
`src/backends/native_jail/tool_bridge.hpp:7` explicitly says the bridge does NOT go through
`invoke_agent_tool` ("that wrapper binds the AGENT's own `capability_ceiling`... exactly the wrong
authority source here (I2)" — written before this fix, already correctly avoiding the bug by
construction); `include/agentengine/core/run_event.hpp:16` says outright "`invoke_tool` is called
only from `agent_registry.hpp::invoke_agent_tool`, never from `AgentSession::handle`" — confirming
`AgentSession`'s real turn loop does not reach this function at all today.
**Verdict: confirmed-clean, re-confirmed.** Fixing this function closes nothing exploitable TODAY —
this is purely forward-looking hardening for when a real `agent.spawn`/MCP-tool-exposure path is
built on top of it (`run_event.hpp:18` itself names "Phase C's MCP tool exposure calling
invoke_agent_tool" as the anticipated future caller). Said honestly, not inflated: no live
vulnerability is closed by this patch; a structural landmine for the NEXT thing built on this glue
is.

**R3 — the capped-vs-uncapped WIDENING rule, attacked directly.** This is the main attack the ADR's
own §2 flagged, and it deserved the closest look before trusting the fix as written.

Mechanically: `cap_covers()` (`capability.hpp:401-407`) treats a capped `parent` (the CALLER's held
capability under the new fix) matched against an uncapped `requested` (the TARGET's declared ceiling
entry) as a widening attempt and rejects — `!parent_cap.has_value()` covers anything, but
`parent_cap.has_value() && !requested_cap.has_value()` returns `false` unconditionally.

Checked whether `meta.capability_ceiling` — the TARGET's compiled ceiling — can ever be capped on the
scalar axes (`size_cap_bytes`, `quota_bytes`, `file_count_cap`, `byte_cap`, `cpu_ms_cap`,
`wall_ms_cap`, `memory_bytes_cap`) for an agent declared with today's DSL. Read every
`cap::decl::*` declaration tag (`capability.hpp:268-319`) and every `to_capability()` overload that
compiles a tag into a runtime `Capability` (`capability.hpp:327-383`):
- `cap::decl::FsRead<Mount>`, `cap::decl::FsWrite<Mount>`, `cap::decl::NetOut<Host>`,
  `cap::decl::Exec<Profile>` — **none of these declaration templates has a cap/quota NTTP parameter
  at all.** There is no syntax in the current declarative surface for an agent author to declare a
  numerically-bounded ceiling entry.
- Every matching `to_capability()` overload constructs the runtime capability with `std::nullopt` (or
  an empty vector, for `NetOut::method_restrictions`) on every scalar cap field — `cap::FsRead{mount,
  "", std::nullopt}`, `cap::FsWrite{mount, "", std::nullopt, std::nullopt}`, `cap::NetOut{{host},
  std::nullopt, {}}`, `cap::Exec{profile, std::nullopt, std::nullopt, std::nullopt}`.
- `capability_ceiling_of<Policies...>()` (`agent_registry.hpp:193-202`) is exactly the fold over
  `tool_detail::policy_capabilities<P>::get()`, which is built from these same `to_capability()`
  results. The file's own comment on `to_capability()` (`capability.hpp:326`) says the conversion's
  job is "only 'turn the declared identity into a real Capability', not policy" — the operator-
  narrowing compiler that would ever produce a capped entry does not exist yet.

**Conclusion: this is a structural fact, not merely "typical."** Every `AgentMetadata.capability_
ceiling` entry compiled by `register_agent<A>()` today, for every declarable agent, is uncapped on
every scalar axis. So under the fix, `attenuate()` rejects on the numeric axis **only when the
CALLER's own held capability for that same kind+mount/host is itself capped** (`parent_cap.
has_value()`) — since the target's request-side value is always `std::nullopt`.

Is that a defect in the fix, or is it the intended, already-proven behavior working exactly as
designed? Checked `capability.hpp:73-75`'s own comment and cross-checked against ADR-009 §5's R-C3
attack, which explicitly red-teamed "an uncapped request against a capped parent" as one of the SIX
independent widening axes `test_capability_enforcement.cpp` proves rejected (ADR-009 §4 claim C3,
§7 verdict: CORRECT). This is not new behavior introduced by ADR-059 — it is `attenuate()`'s own
already-judged, already-proven semantics, reused unmodified. Relitigating it here would mean
relitigating a judged ADR without new evidence, which CLAUDE.md's governance model does not sanction
casually.

Whether it breaks the fix's own *practical* purpose turns on what "realistic caller" means today.
Since no capped ceiling can currently be *declared* by an agent, and no operator-narrowing compiler
exists to produce one either, a caller's own held `CapabilitySet` — if built the same way (from its
own agent's declared `Capabilities<...>` ceiling, or hand-`grant_root()`'d by a host mirroring that
shape) — is uncapped on the same axes too, and `attenuate()` succeeds cleanly (proven directly:
`tests/test_agent_tool_invocation.cpp` case 4, `attenuated->size() == meta->capability_ceiling.
size()`). The rejection only fires when a host has deliberately hand-constructed a caller's
`CapabilitySet` with an explicit numeric cap the target's (uncapped) ceiling request then can't be
covered by — which is exactly a caller that CANNOT vouch for the full, unbounded access the target's
declared ceiling nominally asks for on that axis, so failing closed there is correct, not surprising.

**Verdict: the WIDENING rule does not break the fix's basic purpose for any realistic case reachable
under today's declarative capability surface — confirmed-clean, no correction to `attenuate()` or to
the fix's approach needed.** Named as a residual (§7, new bullet): the declarative `cap::decl::*`
surface has no syntax for a capped ceiling entry, so in practice a caller today can only satisfy
`attenuate()` on the scalar axes by holding an equally-uncapped grant for the same kind+mount/host —
this is a real, load-bearing limitation of the SURROUNDING capability-declaration system (not of this
fix), worth a future ADR if/when capped declarative ceilings are added, but out of THIS fix's scope
exactly as §3 already draws the line.

**R4 — the existing test's behavior change, confirmed correct.** `tests/test_agent_tool_invocation.
cpp`'s original two cases both used a default-constructed `EffectContext` (`ctx.capabilities ==
nullptr`). Under the fix both would now fail at the null-capabilities gate rather than exercising
what they were written to prove (the tool's own logic running; the pipeline's own unknown-tool
error). Confirmed this by reading the pre-fix test directly — no assumption. Fixed per §2's own
plan: both cases (and three new ones) now attach a real, covering `CapabilitySet` via `ctx.
capabilities`, and case 2 specifically asserts `audit.error_code == "tool.unknown_name"` (not just
`result.is_error`) so it can't silently pass for the wrong reason (rejected at the attenuation gate
instead of reaching the real pipeline).

**R5 — error-shape sufficiency.** `attenuate()`'s single generic `capability.
attenuation_not_subsumed` failure does not distinguish "caller holds nothing for this kind" from
"caller holds it but narrower than needed." Checked whether 026 §5/007 need that distinction
surfaced now: no consumer of `invoke_agent_tool()`'s error exists yet (R2) to need it, and the ADR's
own §7 already named this a deliberate, scoped-out residual. **Verdict: confirmed-sufficient for this
pass** — the one distinction that IS load-bearing (a caller with literally no capabilities at all vs.
an ordinary attenuation rejection) is already surfaced via the separate `agent_call.
no_caller_capabilities` code the fix adds, which is enough for a caller to tell "you gave me nothing"
apart from "what you gave me wasn't enough."

**No other real issues found** reading the actual code (as opposed to hypothesizing): `invoke_tool()`
itself (`tool_pipeline.hpp`) is untouched by this fix and its own ten-step pipeline, bind/revoke, and
audit machinery are unaffected; `tool_pipeline_detail::make_error_result()` is the same helper
`make_denial_result()` already uses from within this same file's neighborhood (ADR-029 precedent),
so reusing it directly here for the two new early-return paths matches an established idiom rather
than inventing a new one.

**Overall verdict: the fix as proposed in §2 survives red-team as-is — no named correction, no
different approach needed.** Implemented with two small, non-substantive additions beyond §2's sketch
(both mechanical, not design changes): (1) `audit_out` is populated on both new early-return paths
the same way `invoke_tool()`'s own internal `finish` lambda already populates it on ITS early-return
paths, so a caller journaling audits sees consistent shape regardless of which layer rejected the
call; (2) the null-capabilities and attenuation-rejected paths are given genuinely distinct error
codes (`agent_call.no_caller_capabilities` vs. `attenuate()`'s own `capability.
attenuation_not_subsumed`), matching §2's own sketch verbatim rather than collapsing them.

## 5. Executed evidence

**Rebase.** `git merge-base HEAD main` (before this pass) was `940c2cc` — 5 commits behind `main`
(`main` tip `758ac67`, ADR-057/ADR-058). `git diff 940c2cc main -- include/agentengine/core/
agent_registry.hpp include/agentengine/trust/capability.hpp include/agentengine/core/effect_context.hpp
include/agentengine/core/tool_pipeline.hpp` showed only an ADDITIVE, unrelated field on
`EffectContext` (`codeact_preseeded_answers`, ADR-057 §9) — `agent_registry.hpp`, `capability.hpp`,
and `tool_pipeline.hpp` were byte-identical between the two points. `git stash && git rebase main &&
git stash pop` completed with `Successfully rebased and updated refs...` and no conflicts, confirming
the task's own prediction.

**Windows, MSVC (Visual Studio 18 Community, `vcvarsall.bat x64`), Ninja, Debug, default `build/`
tree** (fresh configure — no prior `build/` existed in this worktree):

```
CONFIGURE_EXIT=0
...
NINJA_EXIT=0
```

Full `ninja -j 8`: clean build, zero errors. The only warnings touching files this pass built are
pre-existing and unrelated to this change: `C4834` (discarding `[[nodiscard]]`) throughout
`include/agentengine/rt/agent_session.hpp` and `core/composed_context_provider.hpp`, `C4996`
(`getenv`) in three unrelated shell-runner/worktree test files, `C4702` (unreachable code) in
`test_middleware_model_call_gateway.cpp` — none in `agent_registry.hpp` or
`test_agent_tool_invocation.cpp`.

**Targeted test first**, direct binary invocation (`build/tests/test_agent_tool_invocation.exe`):

```
test_agent_tool_invocation: ALL PASS
SINGLE_TEST_EXIT=0
```

All 5 case blocks' `check()` calls passed silently (the harness only prints `FAIL: <what>` lines on
failure — none printed), covering: case 1 (positive, covering caller), case 2 (negative, pipeline's
own `tool.unknown_name` reached through the attenuation gate), case 3 (ADR-059 R1: caller holding
less than the ceiling rejected with `capability.attenuation_not_subsumed`), case 4 (ADR-059 R3: extra
caller capabilities don't block, and `attenuated->size() == meta->capability_ceiling.size()` — no
leak-through), case 5 (ADR-059 R4: `ctx.capabilities == nullptr` rejected with
`agent_call.no_caller_capabilities`).

**Full sweep**, `ctest --output-on-failure -j 4` from the same `build/` tree:

```
116/154 Test #116: test_agent_tool_invocation .......................   Passed    0.21 sec
...
100% tests passed, 0 tests failed out of 154

Total Test time (real) =  27.38 sec
CTEST_EXIT=0
```

**154/154 tests passed, 0 failed, 0 skipped** — every test in the default (non-Python-gated) tree,
not just the ones this change touched, confirming no regression anywhere else in the tree from this
fix (`invoke_agent_tool()` has no other production callers per R2, so this is expected, but the full
sweep is real evidence of it, not an assumption).

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| R1 (bug shape matches the draft's own claim) | **CORRECT** | `agent_registry.hpp`, pre-fix lines 505-510, read directly |
| R2 (zero production callers, purely forward-looking) | **CORRECT** | grep across full tree; `tool_bridge.hpp:7`, `run_event.hpp:16/18`; full 154/154 sweep shows no other call site broke or needed a change |
| R3 (WIDENING rule doesn't break the fix's realistic case) | **CORRECT** | `cap::decl::*`/`to_capability()` structural read (no cap NTTP param exists on any declaration tag); ADR-009 §5 R-C3 precedent (already-judged, unmodified); test case 4's direct `attenuated->size()` assertion passed |
| R4 (existing test needed a real covering CapabilitySet) | **CORRECT** | test file fixed; case 2 asserts `audit.error_code == "tool.unknown_name"` and passed, proving the negative case reaches the real pipeline post-fix |
| R5 (error-shape sufficiency for this pass) | **CORRECT** | dedicated `agent_call.no_caller_capabilities` code added and exercised (case 5, passed) |
| Fix survives red-team as-is (no different approach needed) | **CORRECT** | §4's analysis + full build/test evidence above, zero regressions |
| New tests actually prove attenuation: (a) less-than-ceiling caller rejected | **CORRECT** | case 3, `capability.attenuation_not_subsumed`, passed |
| New tests: (b) exactly-enough caller succeeds | **CORRECT** | case 1 (re-purposed from the original positive test), passed |
| New tests: (c) extra caller capabilities don't leak through | **CORRECT** | case 4, `attenuated->size() == meta->capability_ceiling.size()`, passed |
| New tests: (d) `ctx.capabilities == nullptr` fails closed with a distinct code | **CORRECT** | case 5, `agent_call.no_caller_capabilities`, passed |
| No regression to the rest of the tree | **CORRECT** | full sweep 154/154, 100% pass, 0 skipped |

## 7. Residuals to name up front

- `SpawnBudget`(depth)/`rt::SpawnCostBudget`(cost) stay unwired — this fix touches capability
  attenuation only, not recursion or cost bounds.
- No richer error taxonomy for "which specific capability the caller lacked" — a single fail-closed
  error this pass, unless red-team finds that insufficient for a real caller to act on.
- `invoke_agent_tool()` still performs exactly ONE tool call against the target's table — it is not,
  and this ADR does not make it, a real nested-run mechanism.
- **The declarative `cap::decl::*` surface has no syntax for a capped ceiling entry** (no
  `cap::decl::FsWrite<Mount, QuotaBytes>`-shaped tag exists) — found during §4's red-team, not
  anticipated in the original design. In practice this means a caller can only satisfy the new
  `attenuate()` gate on scalar axes (byte caps, quotas) by holding an equally-uncapped grant for the
  same kind+mount/host; a host that hand-grants a caller a numerically-capped capability will always
  fail to invoke a target whose (uncapped-by-construction) declared ceiling needs that same kind —
  correctly, per `attenuate()`'s own already-judged WIDENING rule (ADR-009), but worth a dedicated
  ADR of its own if/when a capped declarative ceiling syntax is added. Not built here — out of this
  ADR's own §3 scope.
