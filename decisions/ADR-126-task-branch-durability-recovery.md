# ADR-126 — Closing `MandatorySandboxProvider`'s `active_`/`task_branches_` durability gap

- **Status:** Proposed — implemented, verified (Windows/MSVC, Docker-independent), full rebuild (zero
  errors) and full `ctest` clean (286/287, same pre-existing unrelated matplotlib/pandas gap),
  `naming_lint.py` clean. **SAME-DAY INDEPENDENT RED-TEAM (§7): one real MUST-FIX found and fixed** (a
  grandchild-shaped orphan could be misfiled into `task_branches_` as a direct child, contrary to the
  original code's own comment — the I2/cross-owner-authorization boundary itself checked out clean).
  Re-verified 286/287 and a clean full rebuild after the fix. **Linux-verified, ADR-127** (2026-08-30,
  same day): a complete, unconditional pass on real GCC 14.2.0 -- no Docker dependency at all, so
  every phase, including the red-team's own grandchild-exclusion regression, is fully proven there too.
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

~~**Root-branch recovery remains a separate, still-open, explicitly-not-attempted residual.**
`bind_sandbox()` takes an already-resolved `BranchHandle` as an explicit parameter — recovering the
ROOT branch itself (as opposed to its children) after a crash is the caller's own responsibility, and
this ADR's own test does that one step by hand (`ledger.reclaim_orphaned_branch(root_name, owner)`
before calling `bind_sandbox()`), matching what a real host's own recovery flow would need to do too.
This is the SAME gap ADR-102's own disclosed "the unbound state owns no branch at all... a `bind_
branch_only()` variant is real follow-on work" already named — not something this ADR attempts to
close, and not conflated with the narrower `task_branches_`-specific gap it does close.~~ **Closed by
ADR-128**: `MandatorySandboxProvider::bind_root_branch()` now automates exactly the by-hand step this
ADR's own test performed, for real production callers.

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
- ~~No Linux verification.~~ **Closed by ADR-127** — a complete, unconditional pass (this test has no
  Docker dependency at all).
- **Content durability remains unclosed** — a real, separate, materially larger piece of work (a
  durable object-store conformer for blob/tree content, not merely branch/ACL bookkeeping), already
  disclosed by `tests/test_ledger.cpp` itself and explicitly out of this ADR's own scope.
- ~~Root-branch recovery remains the caller's own responsibility~~ **Closed by ADR-128** —
  `MandatorySandboxProvider::bind_root_branch()` automates the reclaim-or-create decision for a durably-
  tracked owner's own root branch, the crash-recovery-shaped half of ADR-102's own "`bind_branch_only()`
  is real follow-on work" gap. `bind_sandbox()`'s own explicit `BranchHandle` parameter is itself
  unchanged (`bind_root_branch()` is a new, additional entry point, not a replacement).
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

## 7. Independent red-team round (same day)

**Scope of the pass.** A fresh, independent review (no prior context beyond this ADR and the real diff)
worked adversarially through the I2/I4 questions §5 flagged as not yet done: (1) whether recovery could
materialize a capability without an explicit host grant, (2) whether `reclaim_orphaned_child()`'s own
ACL check genuinely blocks cross-owner leakage and fails silently-safe when it does, (3) whether
`child_prefix` prefix-matching is sound against a determined adversary or an unlucky collision given
`Ledger::branch_from()`'s exact deterministic naming, (4) real build/test verification independent of
this ADR's own account, (5) whether the "best-effort, not fatal to the others" partial-recovery
behavior can lose a reclaimed branch in the window between `Ledger::reclaim_orphaned_branch()`'s success
and this method's own `task_branches_.insert_or_assign()`.

**(1)/(2) I2/cross-owner leakage: correctly scoped, no MUST-FIX found.** `owner_` is the same
host-supplied `IdentityHandle` the current `bind_sandbox()` call already received — recovery only ever
narrows already-possessed authority (finding a durable record of THIS root's own prior children), never
mints new authority, matching `ADR-070`'s Delegated Decision Seam framing. Traced `Ledger::
reclaim_orphaned_branch()` (`include/agentengine/core/ledger.hpp:965-988`) directly: it fails closed on
`ledger.not_an_orphan` if the name was never a real orphan, and fails closed on `ledger.
reclaim_unauthorized` if `requested_by` is not `authorized_for()` the branch's own current head tree
digest (`authorized_for()`, line 1033, is the SAME per-content-digest ACL check — set membership plus
`IdentityAuthority` ancestry — every other read in this design already uses; nothing about the recovery
path weakens or bypasses it). Constructed the adversarial two-owner scenario by hand (owner A's root
"root-3", owner B's root "root-31", each with real children, shared durable Ledger, simulated crash):
because `Ledger::create_root_branch()` names roots as `"root-" + to_string(owner.id())` with an OPTIONAL
`"-" + disambiguator` (`ledger.hpp:422-424`) and `Ledger::branch_from()` names children as
`parent.name() + "/child-" + id + "-" + seq` (`ledger.hpp:605-607`), the mandatory literal `"-"`
separator between a root's numeric id and any disambiguator means a `/` can NEVER appear immediately
after a root's own id substring from `create_root_branch()` alone — so no unrelated root or its children
can ever produce a name starting with `"root-3/child-"` unless it is genuinely a descendant of THIS
exact root branch entry. Confirmed a rejection is silent-and-safe: `reclaim_orphaned_branch()`'s failure
path returns a plain `agentengine::error` with no side effect on `orphaned_from_restart_` (the name stays
orphaned, available for a legitimate reclaim later) and `recover_orphaned_task_branches()`'s own loop
just `continue`s — no timing signal, no error propagated to the caller of `bind_sandbox()`, no partial
state. Cross-owner leakage via this path is not reachable.

**(3) MUST-FIX found and fixed: `child_prefix` matching was not actually "direct children only," contrary
to its own comment.** The ORIGINAL `recover_orphaned_task_branches()` (`include/agentengine/sandbox/
mandatory_sandbox_provider.hpp`, the `bind_sandbox()`-appended method) matched an orphan by prefix alone
(`orphan_name.compare(0, child_prefix.size(), child_prefix) != 0`) and its own comment claimed a
grandchild "would carry a DIFFERENT prefix and is correctly left alone." Tracing `Ledger::branch_from()`
directly (`ledger.hpp:605-607`) shows this is FALSE as stated: a child is named `parent.name() +
"/child-" + id + "-" + seq` UNCONDITIONALLY, including when `parent` is itself already a child — so a
grandchild's real name is `"<root>/child-A-B/child-C-D"`, which DOES start with `"<root>/child-"` (a bare
prefix check cannot distinguish "direct child" from "any descendant, however deep"). The comment's safety
claim rested entirely on an UNENFORCED fact living in a DIFFERENT method (`start_task_branch()` always
calls `runtime_->spawn_child_branch()` on `runtime_`'s own bound branch, never on a `task_branches_`
entry — confirmed by reading `spawn_child_branch()`'s own two callers), not on anything
`recover_orphaned_task_branches()` itself checks, and `Ledger::branch_from()` places no restriction on
chaining for any OTHER caller of the same `Ledger`. This is a real I4 attribution/scope defect: had a
grandchild-shaped orphan ever existed (a future feature, or any other lower-level `Ledger` caller), it
would have been silently misfiled into `task_branches_` as if this root had created it directly — not a
cross-owner leak (the ACL check in (1)/(2) still gates it to the SAME owner), but a real violation of this
method's own documented "direct children only" scope.
**Fix**: after the prefix match, additionally require no further `/` in the matched remainder — a direct
child's own suffix is exactly `<id>-<seq>` (never containing `/`); any deeper descendant always does.
**Proof it was real**: added Phase D (`tests/test_task_branch_durability_recovery.cpp`, checks [5]/[6]) —
builds a genuine grandchild via direct `Ledger::branch_from()` calls (bypassing the tool surface
entirely, the same way any other lower-level caller could), simulates a crash, reconstructs, and asserts
the grandchild is correctly left an unrecovered orphan while the true direct child is still recovered
normally. Reverting only the new `orphan_name.find('/', child_prefix.size()) != std::string::npos`
guard line (prefix check left in place) and rebuilding reproduced two genuine, expected `FAIL`s (checks
[5]) — confirming the test catches the exact defect, not a tautology — then the guard was restored and a
full pass reconfirmed.

**(4) Build/test verification, independent of this ADR's own account.** Built and ran
`test_task_branch_durability_recovery` directly: **ALL CHECKS PASSED** (including the new [5]/[6] cases).
Independently repeated this ADR's own sanity check (not merely trusted it): commented out the
`recover_orphaned_task_branches()` call in `bind_sandbox()`, rebuilt, reran — checks [2] and both halves
of [3] genuinely **FAILED** as expected — then restored and reran, confirming a full pass again (`git
diff --stat` on the production file was empty after restoring, confirming a clean revert with no leftover
artifacts). Rebuilt and reran `test_task_branch_tools`, `test_task_branch_concurrent_dispatch`,
`test_mandatory_sandbox_provider`, `test_mandatory_sandbox_provider_composed`, and (a real Docker daemon
was reachable — confirmed via `docker info` first) `test_composed_sandbox_providers_live`: **all pass
unchanged**. Full project rebuild: zero errors (same pre-existing, unrelated MSVC warnings only). Full
`ctest`, run twice (once before this round's fix, once after): **286/287 both times**, the one failure
being the same pre-existing, unrelated `test_reference_agent_task_corpus` matplotlib/pandas gap — nothing
newly broken by either this ADR's original change or this round's fix. `python tools/naming_lint.py`:
clean, no new exported vocabulary, both times.

**(5) Partial-recovery "lost branch" window: not a real gap.** Traced `Ledger::reclaim_orphaned_branch()`
directly (`ledger.hpp:965-988`): it holds `mutex_` for its entire body, and erases the name from
`orphaned_from_restart_` SYNCHRONOUSLY as part of the same locked critical section that returns the fresh
`BranchHandle` — there is no `co_await` inside it, so no other coroutine can interleave between the erase
and the return. `reclaim_orphaned_child()` (`sandbox_runtime.hpp`) calls it as a plain synchronous call,
not an awaited one. The only real question is what happens across an ACTUAL process crash (not a
same-process race) between one orphan's successful reclaim and `recover_orphaned_task_branches()`'s own
`task_branches_.insert_or_assign()`: `orphaned_from_restart_` is never itself persisted — it is
recomputed FRESH by `load_durable_state()` at every `Ledger` construction from durable branch state alone
(`ledger.hpp:1141`) — so a hard crash at ANY point in this loop reverts all of this method's in-memory
work uniformly; the next restart's `load_durable_state()` will find that branch's durable state
unchanged (nothing in this method persists anything of its own) and correctly re-list it as an orphan
again. Recovery is naturally idempotent across a real crash — matching §6's own "best-effort, not
transactional" framing, and never the corrupted/lost state the partial-recovery question raised. The one
theoretical non-crash window (an exception thrown by `task_branches_.insert_or_assign()`'s own allocation
between a successful reclaim and the insert, which would queue the freshly-reclaimed `BranchHandle` for
abandonment via its own destructor) is the same pre-existing `unordered_map`-insert-after-acquire pattern
this whole file already uses everywhere (e.g. `start_task_branch()`'s own `task_branches_.insert_or_assign()`
after `spawn_child_branch()`) — not a new risk introduced by this ADR, and not fixed here.

**Verdict**: one real MUST-FIX (the grandchild-prefix scope defect in finding (3) above), fixed and
proven; everything else in items (1), (2), (4), (5) checked out clean under genuine adversarial
construction, not assumed.
