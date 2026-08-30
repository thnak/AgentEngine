# ADR-128 — `MandatorySandboxProvider::bind_root_branch()`: automating the root-branch half of task-branch crash recovery

- **Status:** Proposed — implemented, verified (Windows/MSVC), full rebuild (zero errors) and full
  `ctest` clean (288 total, 2 failures, both pre-existing/environment, zero regression), `naming_lint.py`
  clean. **SAME-DAY INDEPENDENT RED-TEAM (§7): one real MUST-FIX found and fixed** — `Ledger::
  reclaim_orphaned_branch()`/`abandon_orphaned_branch()`'s own authorization check was keyed to
  content-digest ACL membership, not true branch ownership, letting any owner who had ever created
  their own fresh root branch reclaim ANY OTHER owner's still-orphaned, never-committed root branch
  (every fresh root/child branch's head tree is the content-addressed digest of an empty `Tree{}`,
  identical for every owner). Empirically confirmed with a real probe, fixed by checking `BranchState::
  created_by_id` directly, re-verified with a full rebuild + full `ctest` + revert-to-confirm-fail
  cycle. §2's own "cross-owner name/ACL mismatch is impossible by construction" claim is corrected by
  this finding — see §7.
- **Date:** 2026-08-30.
- **Scope:** `include/agentengine/sandbox/mandatory_sandbox_provider.hpp` (one new public method,
  `bind_root_branch()`, plus a comment correction on `bind_sandbox()`), `tests/test_root_branch_recovery.cpp`
  (new), `tests/CMakeLists.txt` (one new target registered). **§7's own same-day red-team round added:**
  `include/agentengine/core/ledger.hpp` (`reclaim_orphaned_branch()`/`abandon_orphaned_branch()` now check
  true branch ownership, not content-digest ACL membership -- a real, pre-existing MUST-FIX this ADR's own
  §2 claim surfaced under scrutiny).
- **Related specs:** ADR-126 §5's own "root-branch recovery remains the caller's own, separate,
  already-disclosed responsibility" residual, itself tracing to ADR-102's own `bind_sandbox()` comment
  ("the unbound state owns no branch at all... a `bind_branch_only()` variant is real follow-on work"),
  itself citing ADR-099 §1 item 2.

## 1. The question

ADR-126 closed the CHILD half of task-branch crash recovery: `bind_sandbox()` now automatically
rehydrates orphaned task branches into `task_branches_` from a durable-Store `Ledger`. It explicitly did
NOT touch the ROOT branch itself -- `bind_sandbox()` still takes an already-resolved `BranchHandle<>` as
an explicit parameter, so a host recovering a session after a crash has to know `Ledger`'s own
deterministic root-name format (`"root-" + owner.id() [+ "-" + disambiguator]`) and hand-sequence
`Ledger::orphaned_branches()` / `reclaim_orphaned_branch()` / `create_root_branch()` itself, in the right
order, before it can call `bind_sandbox()` at all. `tests/test_task_branch_durability_recovery.cpp`'s own
Phase B still does exactly this by hand. Is this automatable the same way the child half was, without
inventing any new persistence mechanism or widening any authority?

## 2. Findings

`Ledger::create_root_branch()` and `Ledger::reclaim_orphaned_branch()` already jointly cover the two real
outcomes for "does this owner's root branch already exist in this Ledger's durable state":

- if `owner`'s deterministic root-branch name is currently in `ledger.orphaned_branches()` (a prior
  process, this same owner, left it live-but-unresolved -- a crash or a clean exit, indistinguishable and
  both fine, exactly `Ledger::orphaned_branches()`'s own existing framing), `reclaim_orphaned_branch()`
  hands back a live handle to the SAME branch, independently re-checking `owner`'s own authorization for
  its current head tree and failing closed (a real `ledger.reclaim_unauthorized`/`ledger.not_an_orphan`
  error) if that check fails;
- if it is not, this is a genuine first-ever bind for this owner/disambiguator pair, and
  `create_root_branch()` mints a fresh one, identical to what a host calling that API directly (then
  `bind_sandbox()`) would have done by hand.

Choosing between the two requires only `owner`'s own identity (to compute the deterministic name) and
`ledger.orphaned_branches()` (already a public, read-only accessor) -- no new state, no new authority, and
no change to either underlying `Ledger` method. **A genuinely interesting, unplanned finding while
designing this**: root branches cannot suffer the cross-owner name/ACL mismatch a naive reading of "reclaim
by name" might worry about, BY CONSTRUCTION -- `create_root_branch(owner, disambiguator)` derives the
branch's own name AND grants the initial tree ACL from the SAME `owner.id()` in the same call, so two
different owners can never produce colliding root-branch names with mismatched ACLs through the public
API. This is not a new check this ADR adds; it is an existing structural property of `Ledger`'s own root-
branch naming this pass traced and confirms, not invents. ~~**CORRECTED by §7's own independent red-team
round**: this claim is true for the literal question it asks (branch NAMES never collide across owners),
but a same-day independent review found it does NOT extend to "reclaim is safe" the way this paragraph
implies -- `reclaim_orphaned_branch()`'s pre-existing authorization check was keyed to content-digest ACL
membership, not branch identity, and every never-committed branch (root or child, any owner) shares the
SAME empty-tree digest, letting any owner who had ever created their own branch reclaim a DIFFERENT
owner's orphaned one. Real, proven with a live probe, fixed in `Ledger` itself (`is_branch_owner()`
replacing the digest check) -- see §7 for the full account.~~

**A real, honestly-scoped boundary, not closed by this fix**: `Ledger::orphaned_branches()` only ever
contains what `load_durable_state()` restored at THIS `Ledger`'s own construction -- a branch that is
currently LIVE (bound in this same process, or in some other process that has not exited) is invisible to
it by construction. Calling `bind_root_branch()` a second time for the same owner/disambiguator while the
first branch is still live reaches the create-path and silently produces a duplicate-named branch that
overwrites the live one's own `branches_` entry -- the EXACT SAME pre-existing hazard
`Ledger::create_root_branch()` itself already has and documents (ADR-102 §43.2's own disambiguator fix
narrows collision likelihood, it does not eliminate a genuine same-name re-create). `bind_root_branch()`
makes the CRASH-RECOVERY case correct and convenient; it adds no new safety property for the "still-live"
case that `create_root_branch()` did not already have, and this ADR does not claim otherwise.

## 3. What was built

`MandatorySandboxProvider::bind_root_branch()` (new public method, `mandatory_sandbox_provider.hpp`):
computes `owner`'s deterministic root name exactly the way `Ledger::create_root_branch()` itself does
(duplicated here rather than exposed as a separate `Ledger` accessor -- the same "caller recomputes a
documented deterministic name" shape `recover_orphaned_task_branches()`'s own child-prefix match already
established in this file), checks whether that name is currently orphaned, reclaims or creates
accordingly, and delegates the result straight into the existing, unmodified `bind_sandbox()`. Returns
`agentengine::result<void>` -- the first real failure path (reclaim's ACL check, or a `create_root_branch()`
failure) surfaces directly to the caller instead of being silently swallowed.

`bind_sandbox()`'s own comment (the one carrying ADR-102's original "`bind_branch_only()` is real
follow-on work" note) corrected: strikethrough plus a precise scope note distinguishing what this ADR
closes (the durability-shaped half: reattach to an existing root by identity alone) from what it does not
(the fuller ADR-099 §1 item 2 claim that an entirely never-bound session should still own a branch -- a
session-lifecycle question, out of this ADR's own scope).

`tests/test_root_branch_recovery.cpp` (new, Docker-independent -- same `FakeSurface` stand-in
`test_task_branch_durability_recovery.cpp` already established):
- **[1]** a fresh owner/disambiguator pair takes the create-path, and the resulting binding is genuinely
  functional end to end (a `start_task_branch()`/`discard_task_branch()` round trip through the normal
  tool surface), not merely "returns success" with no usable state behind it.
- **[2] THE CORE CLAIM**: after a simulated crash (mirroring `test_task_branch_durability_recovery.cpp`'s
  own "destroy + reconstruct against the SAME `durable_dir`" methodology), a second `bind_root_branch()`
  call for the SAME owner/disambiguator reattaches to the SAME branch -- proven two ways: (a) a task-
  branch handle minted before the crash is still discoverable via `discard_task_branch()` through the
  normal tool surface, with NO manual `Ledger`-level reclaim call anywhere in this test (the automation
  this ADR's whole point is); (b) a DIRECT differentiator -- `reclaim_orphaned_branch()` erases the name
  from `Ledger`'s own `orphaned_from_restart_` set as part of a genuine reclaim, while
  `create_root_branch()` never touches that set at all, so the root name is asserted to be genuinely
  ABSENT from `ledger.orphaned_branches()` after the call. (b) is the check that actually distinguishes
  reclaim from create -- see §4's own account of why (a) alone was found, empirically, not to.
- **[3]** two different disambiguators for the same owner resolve to two genuinely independent root
  branches (a positive control against an implementation that collapsed them).

`tests/CMakeLists.txt`: new `test_root_branch_recovery` target registered, mirroring
`test_task_branch_durability_recovery`'s own registration exactly.

## 4. Verification

Built and ran the new test directly: **ALL CHECKS PASSED**.

**A real, honest miss in this test's own first draft, found and fixed during the mandatory sanity check**:
the first version's check [2] used only the "pre-crash handle is discoverable via `discard_task_branch()`"
proof (a), reasoning that a wrongly-always-creating `bind_root_branch()` would leave `task_branches_`
empty and that discard would fail. Reverting `is_orphan` to always-`false` (forcing the create-path
unconditionally) and rebuilding to confirm the expected failure instead produced a full, unexpected PASS
-- because `recover_orphaned_task_branches()` (ADR-126) matches child orphans by NAME PREFIX alone, and
the root's deterministic name is identical whether it was reclaimed or freshly re-created under the same
name; the child-branch orphan it already owns is therefore rediscovered either way, making proof (a)
blind to exactly the distinction this ADR needed to test. Root-caused, not just patched around: added
proof (b) above, the `orphaned_from_restart_`-removal differentiator, which the two code paths genuinely
disagree on. Re-ran the SAME `is_orphan`-always-`false` break with (b) added: **check [2]'s new assertion
failed exactly as expected, and only it** (`git diff --stat` confirmed the exact expected 63-insertion
diff before restoring). Restored the fix, rebuilt, reran: full pass again. This is disclosed here plainly
because it is a real methodological lesson (a "does the recovered state look right" proof and a "did the
right code PATH run" proof are not automatically the same claim, and this codebase's own layered recovery
mechanisms -- child-orphan matching by name, root reattachment by name -- can make the former true even
when the latter is false), not merely a note that the test now passes.

Full project rebuild (`cmake --build . --config Debug`, all 314 targets): **zero errors**. Full `ctest`:
**288 total (287 baseline + this pass's own new test), 2 failures** -- `test_reference_agent_task_corpus`
(the same, already-established, pre-existing pandas/matplotlib environment gap this whole design line has
repeatedly confirmed, re-run in isolation to confirm: identical failure, unrelated to this change) and
`test_rt_spawn_cost_budget` (failed only inside the full-suite run; re-run in isolation immediately after,
100% pass -- a flaky, timing-sensitive full-suite artifact, not a real regression this change introduced;
this file/target was not touched). `python tools/naming_lint.py`: clean, no new exported vocabulary
requiring a table entry (`bind_root_branch()` is a method, not an exported type).

## 5. What was NOT done

- **No independent red-team pass yet.** This is new logic in `mandatory_sandbox_provider.hpp`, the same
  file ADR-102/114/117/119/126 have each already hardened through a same-day adversarial round -- expected
  next step, not optional polish.
- **No Linux verification yet.** Same established next-step pattern as every other ADR in this design
  line (ADR-118/120/121/125/127).
- **The fuller ADR-099 §1 item 2 claim remains open** -- an entirely never-bound session (before ANY
  `bind_sandbox()`/`bind_root_branch()` call, in any process) still owns no branch at all. This ADR closes
  the crash-recovery-shaped half of the disclosed gap, not the session-lifecycle-shaped half; see
  `bind_sandbox()`'s own corrected comment for the precise line drawn.
- **The still-live double-bind hazard is inherited, not fixed.** `bind_root_branch()` called twice for the
  same owner/disambiguator while the first branch is still live (not orphaned) silently overwrites it via
  `create_root_branch()`'s own pre-existing behavior -- the EXACT SAME hazard that API already has, not a
  new one. Not attempted here; see §2's own account of why detecting "live elsewhere" is not something
  `Ledger::orphaned_branches()` can answer.
- **No `AgentSession`-level integration.** Same scope boundary ADR-126 §5 already drew for the child half:
  whether/how a real host wires a full "session resumed after a crash" flow (calling `bind_root_branch()`,
  re-minting quotas, resuming normal operation) remains entirely the host's own responsibility.

## 6. Residuals

- Everything named in §5 not otherwise closed.
- `bind_root_branch()` duplicates `Ledger::create_root_branch()`'s own root-name format string rather than
  querying it from `Ledger` directly (no such read-only accessor exists) -- the same "caller recomputes a
  documented deterministic name" coupling `recover_orphaned_task_branches()` already accepted for child
  names; a future `Ledger` refactor changing that format would need to update both call sites, not just
  one.

## 7. Independent red-team round (same day)

**Scope of the pass.** A fresh, independent review (no prior context beyond this ADR and the real diff)
worked adversarially through: (1) whether `bind_root_branch()` can ever obtain/reclaim/create a branch
without an explicit, already-possessed `IdentityHandle owner`; (2) §2's own "cross-owner name/ACL
mismatch is impossible by construction" claim, verified against `Ledger::create_root_branch()`'s and
`reclaim_orphaned_branch()`'s real source rather than trusted from this ADR's own account; (3) whether
the disclosed "still-live double-bind" hazard is worse than disclosed, or cheaply guardable; (4) the
`orphaned_branches()`-snapshot-then-act TOCTOU window between the check and the reclaim/create call; (5)
whether `tests/test_root_branch_recovery.cpp`'s checks [1]/[3] are as decisive as [2] (already
self-corrected once in §4); (6) error-handling/`failure_class` correctness on both failure paths; (7)
lifetime/reference handling and the name-computation string match.

**(1) I2/no-ambient-authority: no MUST-FIX found.** `bind_root_branch()` takes `owner` as an explicit
parameter and computes `root_name` from `owner.id()` alone; it cannot target a DIFFERENT owner's branch
without already holding that owner's own `IdentityHandle` (an ambient-authority question that, if real,
lives at whatever call site minted/handed over that `IdentityHandle` -- entirely outside this method's
own reach, the same boundary `bind_sandbox()` itself already has for its own `owner` parameter).

**(2) MUST-FIX found and fixed: the "impossible by construction" claim is false as a safety property,
even though the narrower literal claim it rests on is true.** §2 is correct that `create_root_branch()`
derives a root branch's NAME from `owner.id()`, so two different owners' root-branch NAMES never
literally collide. But `reclaim_orphaned_branch()`'s (and `abandon_orphaned_branch()`'s) own
authorization check was NOT keyed to branch identity at all -- it was `authorized_for(tree_acl_,
it->second.head_tree_digest, requested_by)` (`ledger.hpp`, pre-fix lines ~815/844), i.e. "is
`requested_by` in the ACL set for this branch's CURRENT TREE CONTENT's digest." `create_root_branch()`'s
freshly-minted, never-committed root branch has its head tree at the digest of an empty `Tree{}` --
CONTENT-ADDRESSED, and therefore IDENTICAL across every owner, since an empty tree carries no
owner-specific content. `insert_acl_root_bounded()` adds EVERY owner who has ever created ANY fresh
root/child branch into that SAME shared `tree_acl_[empty_tree_digest]` entry. The consequence: any owner
B who has ever created their own, entirely unrelated root branch was already "authorized_for" that
shared digest, and could reclaim owner A's still-orphaned, never-committed root branch using only B's own
legitimately-held identity -- a real cross-owner authority bypass, not a hypothetical, and directly
contrary to `reclaim_orphaned_branch()`'s own documented "fails closed if `requested_by` is not
authorized" guarantee and `sandbox_runtime.hpp`'s own load-bearing claim ("never widens authority:
`reclaim_orphaned_branch()` itself requires `requested_by` be ALREADY authorized for the orphaned
branch's current head tree"). Not new code this ADR introduced -- `reclaim_orphaned_branch()` predates
ADR-128 -- but `bind_root_branch()` is exactly the kind of caller that leans on this property implicitly
(a host now calls it automatically instead of hand-sequencing the lower-level API itself, per §1), and
§2's own explicit safety claim is what this pass was told to verify and found false.

**Proof it was real.** Wrote a temporary standalone probe (`tests/probe_cross_owner_reclaim.cpp`, removed
after use -- not part of the permanent suite): owner A creates a root branch and the process "crashes"
(same destroy + reconstruct against the same `durable_dir` methodology this whole design line already
uses); a freshly reconstructed `Ledger` confirms A's root is a real orphan; owner B -- who has never
touched A's branch -- creates their OWN, unrelated root branch (ordinary, expected use); B then calls
`ledger.reclaim_orphaned_branch("root-<A>", B)` directly. Pre-fix: **succeeded** (`CONFIRMED BUG: owner B
successfully reclaimed owner A's orphaned root branch ('root-1') using only B's own identity`). This
reachably breaks the crash-recovery model this whole design line depends on: a real deployment shape is
one host process, one shared `Ledger`, many owners/sessions -- exactly what `orphaned_branches()` letting
a host "reattach by owner identity across many owners" is FOR.

**Fix.** `Ledger::reclaim_orphaned_branch()` and `Ledger::abandon_orphaned_branch()`
(`include/agentengine/core/ledger.hpp`) now check TRUE branch ownership -- a new private
`is_branch_owner(BranchState const&, IdentityHandle const&)` comparing `requested_by.id()` against the
branch's own recorded `BranchState::created_by_id` (the SAME field `reap_pending_abandons()` already
treats as this branch's authoritative owner for its own internal cleanup path), with the identical
ancestor-of-owner allowance `authorized_for()` already extends elsewhere in this class -- instead of the
digest-ACL check. `created_by_id` round-trips through `persist_snapshot_locked()`/`load_durable_state()`
unchanged, so the fix costs nothing across a genuine crash-recovery reconstruction. `authorized_for()`
itself is untouched -- every OTHER caller (content reads, commit-time reference checks, merge grants) is
correctly asking "can this principal access this CONTENT," which is the right question there; only the
two orphan-recovery methods were asking the wrong question.

**Verification.** Rebuilt and reran the probe: **fails as expected** (`NOT CONFIRMED: reclaim failed ...
ledger.reclaim_unauthorized`). Rebuilt and reran `test_root_branch_recovery`, `test_ledger`, and
`test_task_branch_durability_recovery` (the three suites exercising `reclaim_orphaned_branch()` most
directly): all pass unchanged -- every existing legitimate reclaim in this codebase already reclaims with
the SAME identity that created the branch, so tightening the check to true ownership changes no existing
passing behavior. **Sanity check**: `git stash`'d only the `ledger.hpp` fix, rebuilt the probe, reran --
the exact same `CONFIRMED BUG` reproduced deterministically; `git stash pop` restored the fix, rebuilt,
reconfirmed a clean probe pass. Full project rebuild (`cmake --build . --config Debug`, all targets):
**zero errors**. Full `ctest`: **288 total, 1 failure** (`test_reference_agent_task_corpus`, the same
pre-existing pandas/matplotlib environment gap this whole design line has repeatedly confirmed;
`test_rt_spawn_cost_budget` did not flake in this run) -- zero regressions from the fix. `python
tools/naming_lint.py`: clean (`is_branch_owner()` is a private method, not exported vocabulary). The
temporary probe file and its `tests/CMakeLists.txt` entry were removed after use; `git diff --stat` shows
only the permanent fix.

**(3) Still-live double-bind hazard: severity assessment confirmed accurate, no cheap guard found.** The
disclosed hazard (a second `bind_root_branch()` call for a still-LIVE, not-yet-orphaned
owner/disambiguator silently overwrites it via `create_root_branch()`) is real and not worse than
disclosed: `Ledger::orphaned_branches()` is populated ONLY by `load_durable_state()` at construction, by
design (§2), so nothing this Ledger instance does in-process can make a live branch visible there --
there is no missed field or counter that would let `bind_root_branch()` detect "live elsewhere" cheaply.
A genuine fix would need a new mechanism entirely (a lease/heartbeat record, or a durable "currently
bound by process X" marker) -- real, disclosed follow-on design work, not something this pass could
retrofit as a small guard without widening this ADR's own scope. Confirmed this is the EXACT SAME
pre-existing shape `create_root_branch()` itself already has (not a new hazard `bind_root_branch()`
introduces) by reading `create_root_branch()` directly: `branches_.insert_or_assign(name, ...)`
unconditionally overwrites any existing entry under that name, live or not, with no existence check at
all -- true for every caller of that method, not just this one.

**(4) TOCTOU between the `orphaned_branches()` snapshot and the reclaim/create call: real window, benign
outcome, not exploitable.** `bind_root_branch()` snapshots `ledger.orphaned_branches()`, then calls a
SEPARATE `reclaim_orphaned_branch()` or `create_root_branch()` based on that snapshot -- genuinely two
lock acquisitions, not one atomic decision, so another thread/process sharing the same `Ledger` could
mutate `orphaned_from_restart_` in between. Traced both directions: if the snapshot said "orphaned" but
another caller reclaims it first, `reclaim_orphaned_branch()`'s own `orphaned_from_restart_.contains()`
recheck (under its own lock) now correctly fails closed with `ledger.not_an_orphan` -- a real, correctly-
classified error surfaces to `bind_root_branch()`'s caller, not silent corruption or a stale handle. The
reverse direction (snapshot said "not orphaned," so the create-path runs) cannot be raced into "actually
orphaned" by a third party in between, since nothing else can retroactively orphan a branch that was
never orphaned to begin with; the only live hazard reachable via the create-path is the already-disclosed
§5/(3) still-live-overwrite one, not a new TOCTOU-specific one. This is the SAME snapshot-then-act shape
`recover_orphaned_task_branches()` (ADR-126) already has for the child half (`orphaned_branches()`, then a
separate `reclaim_orphaned_child()` per candidate) and ADR-126 §7's own round already traced this same
"fails closed, no silent corruption" property for that method -- `bind_root_branch()` inherits the
identical, already-accepted risk profile, not a new or worse one.

**(5) Test scrutiny: [1] and [3] are decisive for what they each claim; neither needed a fix.** [1] proves
the create-path produces a GENUINELY functional binding (a full `start_task_branch()`/
`discard_task_branch()` round trip through the normal tool surface), not merely "returns success with no
usable state behind it" -- decisive, and the right kind of proof (matches the exact "does this proof
distinguish success from failure" standard §4's own self-correction applied to [2]). [3] proves two
disambiguators for the same owner mint genuinely INDEPENDENT branches (distinct `handle_id`s from two
independently bound providers on the same `Ledger`) -- decisive as the positive control it claims to be
(an implementation that collapsed both disambiguators to the same name would fail this), though it does
not additionally prove that discarding one provider's task branch leaves the other's completely
unaffected -- a slightly stronger version of the same claim, not something [3] currently asserts and not
required for what §3 documents this check as proving.

**(6) Error handling: correctly propagated, nothing swallowed.** `bind_root_branch()`'s
`if (!resolved.has_value()) return std::unexpected(resolved.error());` forwards whichever real,
correctly-classified `agentengine::error` either failure path produced (`ledger.reclaim_unauthorized`/
`ledger.not_an_orphan`/`ledger.unknown_branch` -- `failure_class::policy` or `::contract` -- from
`reclaim_orphaned_branch()`, or `create_root_branch()`'s own `ledger.put_tree_failed`/ACL-cap error)
unmodified to the caller; no exception handling, no default-value fallback, and no code path in
`bind_root_branch()` itself that could discard an error silently.

**(7) Lifetime and name computation: no issues found.** `bind_root_branch()` forwards its `Ledger<>&`/
`AsyncQuota<T>&` references straight into `bind_sandbox()`, which stores them as raw pointers exactly the
way every other `bind_sandbox()` caller already does -- no new lifetime shape introduced. The root-name
computation (`"root-" + std::to_string(owner.id())`, then `"-" + disambiguator` if non-empty) was
compared byte-for-byte against `create_root_branch()`'s own computation (`ledger.hpp`) -- identical, no
off-by-one or separator mismatch.
