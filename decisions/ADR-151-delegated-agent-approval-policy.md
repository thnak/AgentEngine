# ADR-151 — A reference `PolicyDecider` for delegated/spawned-agent calls, corrected from GitHub issue #30

**Status:** Proposed — designed, corrected from that design by an independent red-team pass (§2),
implemented, and proven (real code + tests, §4). **Awaiting project-owner judgment** — per this
project's `design → red-team → prove → judge` discipline (CLAUDE.md), this ADR is not "Judged" until
the project owner signs off; recorded here in Proposed state so the evidence trail is complete and
reviewable before that sign-off.

**Relates to:** GitHub issue #30 (opens this line of work: `PolicyDecider` already receives
`Principal::on_behalf_of`/`delegation_depth`, so a delegation-aware auto-approve policy was fully
EXPRESSIBLE today with zero engine changes, but no reference implementation existed anywhere in this
codebase). `decisions/ADR-070-host-configurable-responsibility-boundary.md` (owns the Delegated
Decision Seam's five required properties, §4 below checks this ADR's additions against every one of
them; §4a's "what must NOT become a seam" list, unaffected here). `007-Capability-and-Trust-Model.md`
(I2/I3, untouched). `018-Identity-Authorization-and-Secrets.md` §2 (`on_behalf_of`/`delegation_depth`,
`derive_on_behalf_of()`). `decisions/ADR-079-agent-spawn-runtime-and-capability-minting.md` (the
`agent.spawn` mechanism this ADR adds two new opt-in fields to, `rt/agent_spawn.hpp`/`rt/agent_spawn_
child_run.hpp`).
`include/agentengine/trust/delegated_approval_policy.hpp` (new), `include/agentengine/rt/agent_spawn.hpp`
(extended: `SpawnTargetDescriptor::approval_decider`/`policy_decider`), `include/agentengine/rt/
agent_spawn_child_run.hpp` (extended: `ChildSpawnRequest::approval_decider`/`policy_decider`,
`run_child_agent_session()`'s wiring), `tests/test_agent_tool_invocation.cpp` (extended, case 6),
`tests/test_rt_agent_spawn.cpp` (extended, T8-T9), `examples/24_delegated_agent_approval.cpp` (new).

## 1. The question

**Stated so it has a wrong answer:** is "a delegated/spawned agent's own `policy_driven` tool calls
should not need human approval by default" something AgentEngine's approval API already supports, or
a real gap needing new engine mechanism — and if it's expressible today, why does nothing in this
codebase demonstrate it?

**Before this ADR:** GitHub issue #30 found, by direct code reading (`core/tool.hpp`, `core/
tool_pipeline.hpp`, `core/agent_registry.hpp`, `rt/agent_spawn_child_run.hpp`, `trust/principal.hpp`):
`PolicyDecider = std::function<policy_decision(Principal const&, ToolDescriptor const&, bool
arguments_tainted)>` (ADR-070) already receives the caller's full `Principal`, which already carries
`on_behalf_of` (the immediate parent's id, never empty for a delegated/spawned principal —
`derive_on_behalf_of()`'s own contract, `trust/principal.hpp`) and `delegation_depth`. So a policy
like `caller.on_behalf_of.empty() ? require_approval : auto_approve` was fully expressible with **zero
engine changes** — but grepping `tests/`/`examples/` found no such `PolicyDecider` anywhere, and
`rt/agent_spawn.hpp`/`rt/agent_spawn_child_run.hpp` wired `derive_on_behalf_of()` for identity but did
nothing with approval at all: a spawned child's own further tool calls had no path to receive a
`PolicyDecider`/`ApprovalDecider` distinct from whatever the host happened to configure for the whole
top-level session (which a spawned child, being a freshly-constructed, separate `AgentSession`, never
inherits — confirmed by reading `run_child_agent_session()`, which calls neither `set_approval_
decider()` nor `set_policy_decider()` on the child it constructs).

Per ADR-070 §3, this must never become an engine-level DEFAULT (I2/I3 stay untouched) — the issue's
own scope was a reference/example gap: a documented, opt-in `PolicyDecider` implementation, plus a
test proving it end-to-end, plus (the issue's own "possibly") a named, reusable helper.

## 2. A correction to the design draft, from an independent red-team pass

An independent, adversarial review (a fresh session, no prior context, briefed only with CLAUDE.md,
the issue text, and the relevant source files) was run against a first draft of this design before any
implementation code was written, per CLAUDE.md's `design → red-team → prove → judge` discipline. It
found two MUST-FIX defects that would have shipped a reference example demonstrating nothing:

**MUST-FIX 1 — `arguments_tainted` is a live-path constant, not a discriminator.** The draft's
`if (arguments_tainted) return require_approval;` fast-path gate assumed this parameter varies
meaningfully per call. Tracing every real call site that ever threads a LIVE `PolicyDecider`
(`rt/agent_session.hpp`'s suspend-for-approval pre-check at two sites, and its one `invoke_tool()`
call site — all three build their `ToolCallRequest` via `core/tool_call_extraction.hpp`'s
`tool_call_request_of()`) found `arguments_tainted` hard-coded `true`, unconditionally, for every
genuinely model-originated call — the ONLY kind of call that ever reaches a `policy_driven` decision
at all (the three "already resolved by a real human" `invoke_tool()` call sites never consult
`PolicyDecider` in the first place). A policy gating on `arguments_tainted` would therefore return
`require_approval` unconditionally in production, making its `on_behalf_of`/`delegation_depth` branches
dead code — a reference implementation that looks delegation-aware but isn't.

**Resolution:** the reference policy (§3) does not consult `arguments_tainted` at all. Its own file
banner states the constant plainly, so a future reader doesn't reintroduce the same mistake by
"fixing" what looks like an oversight. This is not a weakening of I3: every call this policy ever sees
is, by construction, model-originated (that is what `policy_driven` mode and the `PolicyDecider` seam
are FOR); ignoring a parameter that never varies for the calls this mechanism actually receives is
honest, not lax.

**MUST-FIX 2 — for a spawned child, `require_approval` silently degenerates to denial, not deferral.**
The draft's own inline comment claimed a `require_approval` verdict "falls back to whatever
`ApprovalDecider`/suspend behavior the host already has." True for a top-level session; FALSE for a
spawned child: `run_child_agent_session()` drives the child SYNCHRONOUSLY to completion and destroys
it before returning (that file's own top comment) — it can never genuinely suspend for a later human
answer (ADR-029's `Interaction` mechanism needs a session that outlives the call that opened it). With
no `ApprovalDecider` wired for the child, the ordinary step-5 fallback (`bool approved = approve &&
approve(...)`) is unconditionally `false` — an outright denial. Combined with MUST-FIX 1 (every real
call reaches `require_approval` for a non-delegated caller), the honest consequence of shipping the
draft as originally scoped (`PolicyDecider`-only propagation) would be: a spawned child's
`policy_driven` calls either always auto-approve (if delegated) or always silently fail (if not) — no
actual review path exists either way.

**Resolution:** propagate BOTH `ApprovalDecider` and `PolicyDecider` down to a spawned child (§3), not
`PolicyDecider` alone — giving a host that wants a genuine, synchronous approve/deny path for
delegated-but-uncovered calls the same tool it already has for a top-level session. The residual (a
spawned child still can never truly SUSPEND for a later human answer, with or without this ADR) is
named explicitly, in the reference policy's own file banner, in `run_child_agent_session()`'s new
comment, and in `examples/24_delegated_agent_approval.cpp`'s own inline comment — not fixed here (it
would need a mechanism this codebase does not have, well beyond this ADR's scope).

**Should-fix, also incorporated:** the red-team additionally found the original draft's plumbing shape
(a `PolicyDecider` parameter threaded through `perform_agent_spawn()` and a new `AgentSpawnToolProvider`
constructor parameter, shared by every target registered under one provider) inconsistent with this
codebase's own established per-TARGET scoping (`SpawnTargetDescriptor::spawn_cost`/`worktree_mode`/
`child_token_budget` are all per-target, set at `register_target()` time). Moved both new deciders onto
`SpawnTargetDescriptor` instead (§3) — this is simpler AND more correct: a host with both a low-risk
and a high-risk spawn target under one registry can now give them different delegation policies, and
`perform_agent_spawn()`/`AgentSpawnToolProvider` need no new parameters at all (the resolved `target`
is already available where `child_request` is built).

The red-team's full report also confirmed, independently, that: `derive_on_behalf_of()`'s `kind`/
`on_behalf_of`/`delegation_depth` computation is entirely host-side and untouched by anything
model-controlled (`AgentSpawnArgs::agent_id`/`input` never reach `Principal` construction); which
`PolicyDecider` gets consulted never depends on the model's own spawn-call arguments (`AgentSpawnTool
Provider`/`perform_agent_spawn()` always resolve the SAME per-target decider regardless of what
`agent_id` the model named — the model only ever SELECTS among already-registered targets, never
supplies or influences a decider's logic, I3); step 4/7's capability bind runs before step 5
structurally, unchanged, so this policy can never widen what a delegated caller could already do; no
new cross-thread state is touched (`ChildSpawnRequest`'s two new fields are plain by-value
`std::function` copies through a single-threaded construct-then-drive-then-destroy path, matching
RC-1's already-proven precondition); and `agent.spawn` itself (`AgentSpawnTool`) is, and remains,
`never_require` — untouched by and irrelevant to this ADR.

## 3. The design as implemented

**The reference policy** (`include/agentengine/trust/delegated_approval_policy.hpp`):
`agentengine::trust::approve_delegated_calls(std::optional<std::uint32_t> max_depth = std::nullopt) ->
PolicyDecider`. `auto_approve` iff `caller.on_behalf_of` is non-empty AND (`max_depth` unset, or
`caller.delegation_depth <= *max_depth`); `require_approval` otherwise. Never returns `auto_deny` — a
deliberate choice, documented in the file banner: this policy only ever narrows toward "ask", never
introduces a new denial surface a host didn't already have. `max_depth` is an independent, SOFTER
ceiling from `kMaxDelegationDepth` (`trust/principal.hpp`'s own hard, structural bound on delegation
depth itself, currently 8) — a caller past `max_depth` still runs, just without this policy's fast
path.

**Two independent wiring points**, neither one inherited from the other:
1. `AgentSession::set_policy_decider()` (pre-existing, ADR-070) — for the HOST's own top-level
   session's `policy_driven` calls.
2. `SpawnTargetDescriptor::approval_decider`/`policy_decider` (new, `rt/agent_spawn.hpp`) — per spawn
   TARGET, threaded by `perform_agent_spawn()` into the new `ChildSpawnRequest::approval_decider`/
   `policy_decider` fields (`rt/agent_spawn_child_run.hpp`), which `run_child_agent_session()` now
   passes to the freshly-constructed child via `set_approval_decider()`/`set_policy_decider()`. Both
   default `{}`, reproducing exactly today's pre-ADR-151 behavior (ADR-070 property 2: fails
   closed/safe when unset) — a target that doesn't opt in is completely unaffected.

`examples/24_delegated_agent_approval.cpp` demonstrates both side by side, deliberately contrasted: a
non-delegated top-level caller still genuinely suspends for a real human (wiring point 1, this policy
recognizes it as NOT delegated and changes nothing); a spawned child's own `policy_driven` call
auto-approves with no human involved (wiring point 2, the child's Principal is a real, structurally
derived delegation via the unchanged `derive_on_behalf_of()` call `run_child_agent_session()` already
made before this ADR).

## 4. Evidence

`tests/test_agent_tool_invocation.cpp` case 6 (4 sub-cases, through the REAL `invoke_agent_tool()`
entry point, not a synthetic `resolve_approval_outcome()` unit test): 6a non-delegated caller ->
denied (no blanket approval); 6b delegated caller (real `derive_on_behalf_of()` output) with
`arguments_tainted=true` -> auto-approves, `ApprovalDecider` tripwire never consulted, audit records
success; 6c delegation_depth exceeding `max_depth` -> falls back to denial, same as non-delegated; 6d
delegation_depth == max_depth (the pinned boundary) -> still auto-approves, removing off-by-one
ambiguity.

`tests/test_rt_agent_spawn.cpp` T8-T9, through the REAL `perform_agent_spawn()`/`AgentSpawnTool` call
path (not a hand-built `ToolCallRequest`) — a spawned child's own live turn loop genuinely issues a
`policy_driven` tool call via its own scripted model output (so `arguments_tainted` really is the
production-constant `true` MUST-FIX 1 named, not a test-chosen value): T8, with `SpawnTargetDescriptor::
policy_decider = approve_delegated_calls()`, the child's tool actually ran (real invocation, not merely
"no error"), no `ApprovalDecider` ever configured. T9, with no `policy_decider` wired for the target,
the same call is denied — the outer `perform_agent_spawn()` still succeeds (the child's own turn loop
absorbs the denied call as an ordinary tool-result error, not a hard run failure), but the tool itself
never ran — proving ADR-151 changes nothing for a target that doesn't opt in.

`examples/24_delegated_agent_approval.cpp` — both wiring points, run end to end, offline and
deterministic (matching every other example's own discipline).

Windows/MSVC build: `test_agent_tool_invocation`, `test_rt_agent_spawn`, `test_agent_spawn_worktree`,
`test_rt_agent_spawn_child_run`, `test_rt_spawn_pump_concurrency` (every test file that includes either
modified header), and `agentengine_example_24_delegated_agent_approval` all compile with zero errors
and pass with zero failures. `python tools/naming_lint.py`: clean, no new suppressions needed
(`approve_delegated_calls` is a free function, not a type name the 027 vocabulary tables govern).

Full project rebuild (`cmake --build . --config Debug`, all targets): zero compile errors. Full
`ctest -C Debug`: **296/297 passed** — the one failure is `test_reference_agent_task_corpus`, the
same long-documented, unrelated matplotlib-not-installed environment gap this repo's other recent
ADRs (e.g. ADR-148) already record, confirmed unrelated by inspection of its own failure output (a
pandas/matplotlib artifact-rendering check, nothing in this ADR's own call path). A follow-up
independent code-review pass (fresh session, no prior context) re-derived every claim in this ADR
directly from the on-disk code and re-ran the full build/test/lint sequence itself — zero
discrepancies found; its one note (a missed `std::move()` on the two new `ChildSpawnRequest` decider
fields in `run_child_agent_session()`, a micro-optimization, not a correctness issue) was applied.

## 5. What this ADR does not claim

- **Not an engine default.** Per ADR-070 §3/§4a, `approve_delegated_calls()` is one opt-in reference
  implementation a host may or may not adopt, at either or both wiring points. Nothing in
  `resolve_approval_outcome()`, `tool_call_requires_approval()`, or any tool's own declared
  `approval_mode` changed.
- **Does not fix the spawned-child suspend gap.** A spawned child still cannot genuinely defer a
  `policy_driven` call to a later human answer — this was true before ADR-151 and remains true after
  it; ADR-151 only makes the FALLBACK behavior (denial, not silent auto-approval) reachable and
  documented, via the new `SpawnTargetDescriptor::approval_decider` seam, for a host that wants a real,
  synchronous approve/deny path instead of blanket denial.
- **Does not touch `text_derived` declassification, `Tainted<T>`, or the middleware declassifying
  closure.** None of ADR-070 §4a's "must not become a seam" list is affected; `resolve_approval_
  outcome()`'s own `provenance != text_derived` gate, unchanged, still runs before `policy` is ever
  consulted.
- **Does not change what a delegated caller can already do.** Step 4/7's capability bind/check runs
  unconditionally before step 5 in `invoke_tool()`, unchanged — this policy can only decide whether an
  ALREADY-authorized call also needs a human to look at it first, never widen what's authorized.
- **`max_depth`'s bound is soft, not structural.** `kMaxDelegationDepth` (currently 8,
  `trust/principal.hpp`) is the real, hard ceiling on how deep delegation can go at all, enforced by
  `derive_on_behalf_of()` itself failing closed past it — independent of and unrelated to this policy's
  own `max_depth` parameter, which only bounds how deep AUTO-APPROVAL specifically reaches.
- **A `PolicyDecider`/`ApprovalDecider` misconfigured by a host** (e.g. `approve_delegated_calls()`
  wired with no `max_depth` onto a target with deep legitimate recursion) has fully replicated
  `never_require` for every delegated call to that target, by the host's own explicit choice — this is
  the intended shape of the Delegated Decision Seam (ADR-070 §7's own residual, restated here), not a
  defect this ADR introduces.
