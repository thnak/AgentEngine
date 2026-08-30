# ADR-126 — Closing `MandatorySandboxProvider`'s `active_`/`task_branches_` durability gap

- **Status:** Proposed — implemented, verified (Windows/MSVC, Docker-independent), full rebuild (zero
  errors) and full `ctest` clean (286/287, same pre-existing unrelated matplotlib/pandas gap),
  `naming_lint.py` clean. Not yet independently red-teamed; not yet Linux-verified.
- **Date:** 2026-08-30.
- **Scope:** `include/agentengine/sandbox/mandatory_sandbox_provider.hpp` (`bind_sandbox()` gets one new
  call; a new private `recover_orphaned_task_branches()` method), `tests/test_task_branch_durability_
  recovery.cpp` (new), `tests/CMakeLists.txt` (one new test target). `decisions/ADR-114-task-branch-
  tools-promotion.md` (disclosure correction pointing here).
- **Related specs:** `decisions/ADR-114-task-branch-tools-promotion.md` §5/§6 (the residual this ADR
  closes, inherited unchanged from the prove-phase original's own finding 6), `include/agentengine/
  core/ledger.hpp` (`orphaned_branches()`/`reclaim_orphaned_branch()`/`load_durable_state()` — the
  already-existing, already-proven durability/recovery primitives this fix reuses, never re-derives),
  `include/agentengine/sandbox/sandbox_runtime.hpp` (`reclaim_orphaned_child()` — the same primitive
  `commit_task_branch()`'s own conflict-retry path already relies on), `tests/test_ledger.cpp` (the
  established "destroy + reconstruct against the SAME `durable_dir`" crash-simulation methodology this
  ADR's own test mirrors, and the same file's own prior disclosure of the content-vs-metadata
  durability split this ADR's own test had to precisely rediscover).

## 1. The question

ADR-114 §5/§6 disclosed, inherited unchanged from the prove-phase original's own finding 6: "`active_`'s
table has no durability of its own — a process crash mid-task-branch strands it from this tool surface's
own verbs even in a durable-`Store` `Ledger` configuration, though the underlying branch itself survives
and remains reclaimable via the lower-level orphan-reclaim API." Is this fixable without inventing any
new persistence mechanism, given the Ledger already durably tracks exactly the information needed?

## 2. Findings

**The fix needs no new persistence mechanism at all — the Ledger already remembers everything
required.** `Ledger::branch_from()` names children deterministically (`"<parent>/child-<id>-<seq>"`),
so a task branch's own parent is recoverable from its NAME alone, with no separate lineage index. Every
branch with a durable `durable_dir` configured is restored into `orphaned_from_restart_` by `load_
durable_state()` at Ledger CONSTRUCTION time — precisely every branch that had a live, unresolved
`BranchHandle` when the previous process ended, whether that end was clean or a crash (the Ledger
cannot tell the difference, and does not need to). This is EXACTLY the set `task_branches_` should
contain at any given moment; the only piece missing was `MandatorySandboxProvider` actually asking for
it.

**The fix is small and additive, reusing an already-proven primitive.** `SandboxRuntime::
reclaim_orphaned_child()` — the SAME method `commit_task_branch()`'s own merge-conflict-retry path
already calls — is the correct, minimal reclaim primitive: it wraps `Ledger::reclaim_orphaned_branch()`
(which independently re-checks `owner_`'s own ACL authorization for each candidate, the identical gate
every other read in this design already goes through) and hands back a fresh, live `SandboxRuntime`.
`bind_sandbox()` now calls a new `recover_orphaned_task_branches()` at the end of its own body: walk
`ledger_->orphaned_branches()`, keep only names carrying THIS root branch's own deterministic child
prefix, and `reclaim_orphaned_child()` each one into `task_branches_`. A reclaim failure for any one
candidate (a genuine auth mismatch, or losing a race to some other, unrelated reclaim of the same
orphan) is not fatal to the others — each is attempted independently, matching this method's own
best-effort, convenience-only framing (a host that needs the AUTHORITATIVE list already has direct
access to `Ledger::orphaned_branches()`/`reclaim_orphaned_branch()`).

**A real, precise scope boundary, found empirically while writing the test, not assumed in advance.**
The first version of the new test used `commit_task_branch()` to prove recovery, and it genuinely
failed — not with `task_branch_unknown_handle` (which would mean recovery itself was broken), but with
a real `ledger.merge_tree_load_failed` from `merge_into()`'s own attempt to load base/ours/theirs tree
CONTENT. Tracing this confirmed a real, DIFFERENT, and already-disclosed limitation: `tests/test_
ledger.cpp`'s own header comment already states that `durable_dir` persists Ledger's branch/ACL
BOOKKEEPING only — a durable object-STORE conformer (real blob/tree content durability) was never
ported. A recovered task branch's own METADATA (its existence, its ACL, its place in `task_branches_`)
survives a simulated crash correctly; its own real tree content, held only in the in-memory `store_`,
does not, and this fix was never going to change that (content durability is a materially larger,
separate piece of work). The test was redesigned to prove the METADATA-durability claim this fix
actually makes with `discard_task_branch()` (a pure branch-table erase, needing no tree content at all)
— and, rather than discard the finding, to ALSO assert `commit_task_branch()` on a second recovered
handle fails with the EXACT expected `ledger.merge_tree_load_failed` code (not `task_branch_unknown_
handle`), turning the initial debugging surprise into a permanent, precise regression test that
distinguishes "recovery didn't work" from "a different, already-disclosed limitation" by evidence, not
assumption.

**`BranchCost` accounting is deliberately, correctly NOT re-charged on recovery.** `AsyncQuota` is
well-established, pre-existing in-process state, never claimed durable anywhere in this codebase — a
fresh quota after a restart has no memory of what was spent before the crash. Recovering N orphaned
branches into `task_branches_` does not re-consume `BranchCost` for them (`reclaim_orphaned_child()`
touches no quota at all), which is the correct behavior given the existing, accepted, unrelated
non-durability of `AsyncQuota` itself — not a new gap this fix introduces or should attempt to close.

**Root-branch recovery remains a separate, still-open, explicitly-not-attempted residual.**
`bind_sandbox()` takes an already-resolved `BranchHandle` as an explicit parameter — recovering the
ROOT branch itself (as opposed to its children) after a crash is the caller's own responsibility, and
this ADR's own test does that one step by hand (`ledger.reclaim_orphaned_branch(root_name, owner)`
before calling `bind_sandbox()`), matching what a real host's own recovery flow would need to do too.
This is the SAME gap ADR-102's own disclosed "the unbound state owns no branch at all... a `bind_
branch_only()` variant is real follow-on work" already named — not something this ADR attempts to
close, and not conflated with the narrower `task_branches_`-specific gap it does close.

## 3. What was built

`include/agentengine/sandbox/mandatory_sandbox_provider.hpp`: `bind_sandbox()` gets one new call,
`recover_orphaned_task_branches()`, appended after all its existing reassignment work (so it correctly
operates on the FRESH `runtime_`/`owner_`, never the prior binding's). The new private method itself:
prefix-matches `ledger_->orphaned_branches()` against `runtime_->branch_name() + "/child-"`, reclaims
each match via `SandboxRuntime::reclaim_orphaned_child()`, and inserts successes into `task_branches_`.

`tests/test_task_branch_durability_recovery.cpp` (new, Docker-independent — never calls `run_in_task_
branch()`, using a `FakeSurface` stand-in mirroring `tests/test_mandatory_sandbox_provider_composed.cpp`'s
own fixture): mirrors `tests/test_ledger.cpp`'s own established "destroy + reconstruct against the SAME
`durable_dir`" crash-simulation methodology. Phase A binds against a durable Ledger, starts TWO task
branches, then lets everything go out of scope (the same "at least as strong as a real crash" reasoning
`test_ledger.cpp` itself already established, since a real process exit runs no destructors either).
Phase B reconstructs a fresh `Ledger` against the SAME `durable_dir`, hand-reclaims the root branch (the
one step outside this fix's own scope), binds a fresh provider, and proves: (a) `discard_task_branch()`
on the first original `handle_id` succeeds through the normal tool surface with no direct Ledger-level
orphan call for the child branch anywhere in the check; (b) `commit_task_branch()` on the second
original `handle_id` genuinely reaches real merge logic (not `task_branch_unknown_handle`) but fails on
the separate, precisely-identified content-durability gap. Phase C confirms an ordinary, non-recovered
`discard_task_branch()` call and its `BranchCost` refund are both completely unaffected by this fix.

## 4. Verification

Built and ran the new test directly: **ALL CHECKS PASSED**. Sanity-checked the same way this design
line always does: temporarily commented out the new `recover_orphaned_task_branches()` call in `bind_
sandbox()`, rebuilt, and reran — all three of the core recovery assertions genuinely **FAILED** (the
discard-recovery check, the "handle is genuinely found" check, and the "fails on the precise expected
content-durability error" check all failed, the last two because with no recovery at all the call fails
closed with `task_branch_unknown_handle` instead) — then restored the fix and reran, confirming a full
pass again. `git diff` on the production file confirmed a clean, purely additive change with no leftover
sanity-check artifacts.

Rebuilt and reran every other test that calls `bind_sandbox()` (`test_task_branch_tools`, `test_task_
branch_concurrent_dispatch`, `test_mandatory_sandbox_provider`, `test_composed_sandbox_providers_live`,
`test_mandatory_sandbox_provider_composed`), against a REAL Docker daemon where applicable: **all pass
unchanged**, confirming the new recovery call is a genuine no-op for the ordinary, non-crash-recovery
case (an empty `ledger.orphaned_branches()` list, the overwhelmingly common case, costs one cheap,
empty iteration).

Full project rebuild: zero errors. Full `ctest`: **286/287**, the one failure being the same,
unrelated, pre-existing matplotlib/pandas gap this whole design line has repeatedly confirmed —
nothing newly broken. `python tools/naming_lint.py`: clean, no new exported vocabulary.

## 5. What was NOT done

- **No independent red-team pass yet.** This is new logic in `mandatory_sandbox_provider.hpp`, a file
  this design line has already hardened through several real red-team rounds (ADR-102, ADR-114,
  ADR-117, ADR-119) — a fresh pass is the expected next step, not optional polish.
- **No Linux verification.**
- **Content durability remains unclosed** — a real, separate, materially larger piece of work (a
  durable object-store conformer for blob/tree content, not merely branch/ACL bookkeeping), already
  disclosed by `tests/test_ledger.cpp` itself and explicitly out of this ADR's own scope.
- **Root-branch recovery remains the caller's own responsibility** — `bind_sandbox()`'s own explicit
  `BranchHandle` parameter is unchanged; this ADR does not attempt ADR-102's own separately-disclosed
  "`bind_branch_only()` is real follow-on work" gap.
- **No AgentSession-level integration** — this ADR closes the gap at `MandatorySandboxProvider`'s own
  layer (the tool surface can find its recovered branches again); whether/how a real host wires a full
  "session resumed after a crash" flow (reclaiming the session's own root branch, re-minting quotas,
  calling `bind_sandbox()`) remains entirely the host's own responsibility, unchanged by this ADR.

## 6. Residuals

- Everything named in §5 not otherwise closed.
- `recover_orphaned_task_branches()` is best-effort, not transactional: if reclaiming branch N of M
  orphans fails partway through (an auth mismatch, say), branches 1..N-1 are already recovered into
  `task_branches_` and stay there — a real, disclosed, but low-consequence asymmetry (the caller simply
  sees fewer recovered handles than existed, never a corrupted or partially-applied state), matching
  the "attempted independently, not fatal to the others" design already stated in §2.
