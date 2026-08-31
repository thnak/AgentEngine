# ADR-117 — Widening the real `Capability` variant for task-branch tools

- **Status:** Proposed — implemented, verified against a REAL Docker daemon (Windows/MSVC), full
  project rebuild (zero errors) and full `ctest` clean, `naming_lint.py` clean. Independent red-team
  round completed same day: clean bill of health, no fix needed (see the red-team's own report;
  no §7 was added to this file since nothing real was found). **Linux-verified same day, ADR-118**:
  the widened variant and the new double gate both hold completely under real GCC 14.2.0, independent
  of this environment's own pre-existing, disclosed Docker-CLI-reachability gap.
- **Date:** 2026-08-30.
- **Scope:** `include/agentengine/trust/capability.hpp` (two new `cap::`/`cap::decl::` alternatives,
  two new `capability_kind` enumerators, four exhaustive-switch/`if constexpr` sites extended),
  `include/agentengine/trust/policy_reachability.hpp` (`capability_kind_name()`'s own exhaustive
  switch extended), `include/agentengine/sandbox/mandatory_sandbox_provider.hpp` (the four task-branch
  `Tool<>` conformers gain a real `Capabilities<...>` ceiling; capability-gating comment block
  rewritten), `tests/test_task_branch_tools.cpp` (existing real-pipeline section updated to grant the
  new capabilities; new section added proving the fail-closed property). No other file touched.
- **Related specs:** `decisions/ADR-114-task-branch-tools-promotion.md` §2 (the question this closes,
  named there as real, disclosed follow-on work, not silently skipped), `decisions/ADR-009-capability-
  set-enforcement-mechanism.md` (the enforcement mechanism this widening extends),
  `docs/planning/proofs/task_branch_tool/task_branch_capability.hpp` (the prove-phase design this
  promotes verbatim — two-tag split, fieldless-marker shape, and the exact promotion path, all
  designed and confirmed with the project owner well before this ADR), `src/backends/native_jail/
  session_shell_wiring.hpp` (`RunShellTool`, ADR-096 — the real, shipped precedent for a `Tool<>` that
  already lives under exactly this contract).

## 1. The question

ADR-114 promoted `SandboxRuntime::merge_into()`'s first real caller (`start_task_branch`/
`run_in_task_branch`/`commit_task_branch`/`discard_task_branch`) but deliberately gave all four tools
a ZERO `Capabilities<...>` ceiling, mirroring `RunCommandTool`'s own precedent — because the prove-
phase design's real answer (a two-tag `cap::decl::TaskBranch`/`cap::decl::TaskBranchCommit`
`CapabilitySet` gate) had never been promoted into the real, closed `agentengine::Capability` variant,
and widening that variant was named explicit, deferred, security-critical surgery. Is it now worth
doing, and can it be done without breaking anything that already depends on the old, ungated contract?

## 2. Findings

**The prove-phase design already fully specified the promotion.** `task_branch_capability.hpp`'s own
header comment names the exact shape: two fieldless marker types (`cap::TaskBranch`/
`cap::TaskBranchCommit`, matching the real `cap::Entropy`/`cap::Elicit` precedent — "no further
parameter to narrow"), a matching `cap::decl::` pair, one `to_capability()` overload each, and the
same `Capabilities<cap::decl::TaskBranch>` / `Capabilities<cap::decl::TaskBranch,
cap::decl::TaskBranchCommit>` ceilings the four real tools should carry (start/run/discard need only
`TaskBranch`; commit alone needs both, since a commit can only ever operate on a handle `start` already
produced — enforced by the tool's own declared ceiling, never by a relationship between the two
capability kinds themselves, which stay mutually independent in the variant). This ADR follows that
design verbatim rather than re-deriving it.

**The real cost was never designing the tags — it was the exhaustive-switch surface.** A grep across
`include/` for every place that pattern-matches on the `Capability` variant or `capability_kind` enum
found exactly four sites that needed a new arm: `capability_kind_of()`, `capability_from_kind()`, and
`is_inert_for_text_derived_declassification()` (all three in `capability.hpp`), plus
`policy_reachability.hpp`'s own `capability_kind_name()`. All four are genuinely exhaustive (no
`default:` case, or an `if constexpr` chain with a `static_assert` fallback) — the compiler itself
would have caught a missed site as an unhandled-enumerator warning or a hard `static_assert` failure,
not a silent gap. `agent_library_manifest.hpp`'s `agent_library_registry()` table was checked and found
to need no new row: it is a documentation/discovery surface for chat-facing "modules," not an
exhaustive switch, and task-branch tools are not currently represented there under any name (a
separate, optional follow-on, not required for this widening to be sound).

**The genuinely open design question was never "can the variant be widened" — it was "does wiring the
widened variant onto the real tools change existing behavior."** It does, materially: before this ADR,
`invoke_tool()`'s step 4/7 loop (`tool_pipeline.hpp`) had nothing to bind for any of the four tools
(`declared_capabilities()` returned an empty vector), so a session that called `bind_sandbox()` +
`bind_task_branch_tools()` could reach all four verbs through the real `session.start_run() ->
invoke_tool()` pipeline with **no capability grant of any kind** — the dynamic opt-in gate alone was
sufficient. Giving the tools a real ceiling makes this a genuine DOUBLE gate: the existing dynamic
opt-in, unchanged, PLUS a new static requirement that the calling session's own granted `CapabilitySet`
(`AgentSession::set_capabilities()`, which defaults to a genuinely empty `CapabilitySet::grant_root({})`
when never called — never to "unrestricted") actually holds `cap::TaskBranch`/`cap::TaskBranchCommit`.
Confirmed directly, not assumed: `tests/test_task_branch_tools.cpp`'s own pre-existing real-pipeline
section ([6]) passed a session's capabilities as `CapabilitySet::grant_root({})` — genuinely empty —
and that call would now be REJECTED by `invoke_tool()`'s own "no leaked capability" step if left
unchanged. This is a real, disclosed **behavior change** to an API surface ADR-114 shipped, not a purely
additive one.

**Decision: make the change now, not defer it again.** ADR-114 landed the SAME DAY as this ADR, and its
own §5/§6 name zero real production callers of `bind_task_branch_tools()` anywhere outside
`tests/test_task_branch_tools.cpp` itself (confirmed by search, matching that ADR's own disclosed
scope). This is the lowest-risk moment this contract will ever be to change — before any real host
outside this test file has adopted the old, single-gate behavior. Deferring again would only grow the
number of real callers that would eventually need to be migrated onto the double-gate contract, for no
offsetting benefit: the two-tag split, the fieldless-marker shape, and the exact wiring were already
fully designed and reasoned about (§2 above), so there was no remaining design uncertainty to wait out.
The one real cost — updating `tests/test_task_branch_tools.cpp`'s own section [6] to grant both tags —
is a three-line fix to the one file that needed it.

**Why a double gate, not a straight swap.** The existing dynamic gate (`is_bound()` AND
`merge_quota_ != nullptr`) answers a different question than the new static one: "has this
`MandatorySandboxProvider` instance actually been configured with a real sandbox and a real merge
quota at all" versus "has THIS session been granted authority to use task-branch tooling." Removing the
dynamic gate now that a static one exists would be unrelated scope-creep (it also protects against
calling `start_task_branch()` directly, the plain C++ method, which never goes through
`Capabilities<...>` at all — every non-pipeline test section in `test_task_branch_tools.cpp` calls the
provider methods directly, bypassing `Tool<>`/`invoke_tool()` entirely, and is completely unaffected by
this ADR). Both gates stay, each answering the question it already answered.

## 3. What was built

`include/agentengine/trust/capability.hpp`: `cap::TaskBranch{}`/`cap::TaskBranchCommit{}` (fieldless
runtime markers) and `cap::decl::TaskBranch{}`/`cap::decl::TaskBranchCommit{}` (fieldless compile-time
declaration tags) added verbatim from the prove-phase design; both runtime types added as new
alternatives 20/21 of the `Capability` variant; `capability_kind::task_branch`/
`capability_kind::task_branch_commit` added to the enum; `capability_kind_of()` gets two new
`if constexpr` arms; `capability_from_kind()` gets two new `switch` arms; `to_capability()` gets two
new overloads (one per `cap::decl::` tag); `subsumes_payload()` gets two new overloads, both
unconditionally `true` (matching the `cap::Entropy`/`cap::Elicit` precedent — a fieldless marker has no
payload to narrow); `is_inert_for_text_derived_declassification()` gets two new arms, both `false`
(task-branch start/run execute commands, even if isolated on a child branch; commit mutates the
session's own main branch — neither is provably inert).

`include/agentengine/trust/policy_reachability.hpp`: `capability_kind_name()`'s exhaustive switch gets
two new arms (`"task_branch"`/`"task_branch_commit"`).

`include/agentengine/sandbox/mandatory_sandbox_provider.hpp`: `StartTaskBranchTool`/
`RunInTaskBranchTool`/`DiscardTaskBranchTool` now declare `Tool<..., Capabilities<cap::decl::
TaskBranch>>`; `CommitTaskBranchTool` declares `Tool<..., Capabilities<cap::decl::TaskBranch,
cap::decl::TaskBranchCommit>>`. The capability-gating comment block immediately above the four structs
is rewritten to describe the new double-gate contract in full, including the exact rejection error
(`tool.capability_not_held`, from `tool_pipeline.hpp`'s own step 4/7) and an explicit note that this is
a disclosed behavior change from this file's own ADR-114 original.

`tests/test_task_branch_tools.cpp`: section [6]'s `held` `CapabilitySet` now grants both
`cap::TaskBranch{}` and `cap::TaskBranchCommit{}` (previously empty — the exact grant that would now
fail). A new section [7] proves the fail-closed direction: a session that calls `bind_sandbox()` +
`bind_task_branch_tools()` (the dynamic gate, satisfied) but is given an explicitly empty
`CapabilitySet` gets `start_task_branch` rejected as a real `role::tool` error result through the real
pipeline — `start_run()` itself still completes normally (the rejection is an ordinary error `ToolResult`,
not a thrown/aborted run), no `handle_id` ever appears anywhere in history (the real verb never ran),
and the `BranchCost` quota is untouched (the rejection happens strictly before `call_sandbox()`, so
nothing is spent on a call that never executes).

## 4. Verification

`tests/test_task_branch_tools.cpp` rebuilt and run against a REAL Docker daemon (Windows/MSVC):
**ALL CHECKS PASSED**, including the updated section [6] (now granting both tags) and the new section
[7] (the fail-closed proof). Sanity-checked the new test the same way this design line always does:
temporarily reverted `StartTaskBranchTool`'s ceiling back to `Tool<StartTaskBranchTool>` (no
`Capabilities<...>`, the exact ADR-114 shape) and confirmed all three of section [7]'s new assertions
genuinely FAIL against that pre-fix code (the rejection never happens, a `handle_id` leaks into history,
and the call reaches `call_sandbox()` for real) — then restored the fix and re-confirmed a full pass.

Full project rebuild: zero errors. Full `ctest`: unchanged pass rate from before this change (the one
pre-existing failure is the same unrelated matplotlib/pandas Python-worker gap ADR-111/112/114 already
named — confirmed still the only failure, nothing newly broken). `naming_lint.py`: clean, no new
findings — the two new `cap::`/`cap::decl::` types follow the identical naming shape as every existing
fieldless capability marker (`Entropy`/`Elicit`), which the lint already accepts with no special-case
entries.

Searched the whole `tests/` tree for any test that iterates `capability_kind` exhaustively, asserts a
fixed count of `Capability` alternatives, or otherwise hard-codes "19 alternatives" as a live
assertion (not prose): none found. `agent_library_manifest.hpp`'s `agent_library_registry()` was
checked directly and confirmed to need no change (§2).

## 5. What was NOT done

- **No independent red-team pass yet**, unlike every other same-day ADR in this design line
  (ADR-108/109/111/114/116). This IS real, security-critical surgery to the enforcement mechanism
  itself (widening the closed variant every `CapabilitySet`/`subsumes()`/`bind()` call site depends
  on) — a red-team round is the expected next step before this is considered closed, not optional
  polish.
- ~~No Linux verification.~~ **Closed by ADR-118** (2026-08-30, same day): rebuilt and retested on real
  GCC 14.2.0 (the same WSL2 checkout ADR-115 used, fast-forwarded to this ADR's exact commit). The
  widened variant and every extended exhaustive switch compile clean, and — the part that actually
  matters — the new double gate is proven completely and unconditionally in both directions (granted
  -> allowed, withheld -> rejected with no side effects) through the real `invoke_tool()` pipeline,
  independent of the same pre-existing Docker-CLI-reachability gap ADR-115 already disclosed for this
  environment (which still leaves the Docker-command-execution half of `test_task_branch_tools`
  unverified on Linux — a narrower, pre-existing residual, not new to this ADR).
- **`agent_library_manifest.hpp`'s discovery registry was not given a "task-branch" module row.** Named
  in §2/§4 as checked-and-optional, not a gap silently introduced — no existing module row names any of
  the four task-branch tools by any name today, so this is pre-existing scope, not something this ADR
  regressed.
- **`RunCommandTool` itself was NOT given a capability ceiling.** Out of scope: this ADR closes the
  ADR-114 §2 residual specifically named for the task-branch tools, not a broader reconsideration of
  `RunCommandTool`'s own, separately-decided (ADR-102 Phase 4) zero-ceiling design.

## 6. Residuals

- Everything named in §5 not otherwise closed.
- The two new `capability_kind` enumerators are additive to a real, closed, security-load-bearing enum
  — any FUTURE exhaustive switch over `capability_kind`/`Capability` written without first grepping for
  the existing four sites this ADR extended risks silently missing the same class of arm this ADR had
  to add in four places. Not a new risk this ADR introduces (the same risk existed for every prior
  capability kind), but worth naming for whoever adds the next one.
