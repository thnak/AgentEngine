# ADR-142 — Independent red-team round over ADR-141: two further real gaps in the `AsyncQuota` child-share redesign

- **Status:** Proposed — real, reproduced adversarial findings, both fixed and re-verified same day.
  Full rebuild (zero errors) and full `ctest` clean (293 total, 1 pre-existing unrelated failure, zero
  regression), `naming_lint.py` clean.
- **Date:** 2026-08-30/31.
- **Scope:** `include/agentengine/rt/async_quota.hpp` only (`allocate_child_share()`,
  `release_child_share()`, one new private field `origin_mutex_`).
- **Related specs:** `decisions/ADR-141-async-quota-child-share-double-spend-fix.md` (the redesign this
  adversarially probes).

## 1. The question

ADR-141 closed a real double-spend/double-credit bug by making `try_consume()` owner-only and having
`release_child_share()` take the child's own `AsyncQuota` object by value. Does that redesign hold up
under further adversarial probing — specifically: what happens if `allocate_child_share()` is called
twice for the same child identity before either share is released? And what happens if the same child
`IdentityHandle` legitimately holds shares from two independent root quotas (nothing prevents one agent
identity receiving grants from two separate sessions), and a caller tries to release one against the
wrong parent?

## 2. Findings

Both suspicions confirmed real and reproducible, independently of each other.

**(1) Second allocation to an already-live child silently strands the first share's quota.**
`children_[child.id()] = amount` in `allocate_child_share()` unconditionally overwrites any existing
entry. Reproduced: allocate 40 to a child, then allocate 25 more to the SAME child before releasing the
first — the pre-fix code accepted both allocations (correctly decrementing the parent's `remaining_`
by 65 total) but the ledger entry only ever remembered the SECOND amount (25). Releasing the second
share back succeeds and credits 25; the FIRST share's still-live `AsyncQuota` object (40, genuinely
unspent) then has no ledger entry left to match against and can never be released — its 40 units are
permanently stranded, spendable through neither the parent's own tracked `remaining()` nor any release
path. A real I8 budget-enforcement defect: a leak, not an escalation, but a real, silent loss of usable
parent capacity.

**(2) `release_child_share()` matched only by `child.id()`, not by which parent actually minted the
object.** `children_` is keyed by `IdentityHandle::id()` alone. Reproduced: the SAME child identity
legitimately holds a share from TWO INDEPENDENT root quotas (`quota_x`, `quota_y`) — nothing in the
design prevents this (an agent identity can hold `BranchCost` grants from two separate sessions).
`quota_x.release_child_share(std::move(share_from_quota_y))` succeeded: it found `quota_x`'s OWN live
entry for that child id (coincidentally present from an unrelated allocation), erased THAT entry even
though `quota_x`'s real, still-outstanding share to the child was untouched, and credited `quota_x`'s
`remaining_` with `quota_y`'s leftover amount — crediting the wrong parent while stranding `quota_y`'s
own ledger entry (now un-releasable, since the only live `AsyncQuota` object for it was just consumed
against the wrong parent).

## 3. What was built

**(1)** `allocate_child_share()` now checks `children_.find(child.id())` first and refuses a second
live allocation to the same child identity outright (`async_quota.child_share_already_live`) — a
caller that genuinely wants to split further must release the first share back first.

**(2)** Added a new private field, `AsyncMutex const* origin_mutex_ = nullptr`, on `AsyncQuota`. Every
`AsyncQuota` returned by `allocate_child_share()` is stamped with `mutex_.get()` of the ALLOCATING
PARENT — stable across any later move of the parent `AsyncQuota` itself (the pointee of a
`unique_ptr`, unaffected by moving the owning wrapper, for the exact reason this file's own top comment
already gives for using a `unique_ptr` indirection at all). `release_child_share()` now verifies
`child_quota.origin_mutex_ == mutex_.get()` before touching the ledger, refusing with
`async_quota.release_wrong_parent` otherwise. A root-minted `AsyncQuota` (`mint_root()`) defaults to
`origin_mutex_ = nullptr`, which can never match any real parent's `mutex_.get()` (never itself null),
so a root quota can never be mistakenly "released" into any parent either.

## 4. Verification

Rebuilt (zero errors). Wrote a standalone adversarial probe reproducing both scenarios directly against
the fixed code: (1) confirmed a second `allocate_child_share()` for a live child now fails closed with
`async_quota.child_share_already_live`, and that releasing the first share still works correctly
afterward; (2) confirmed `quota_x.release_child_share(std::move(share_from_quota_y))` now fails closed
with `async_quota.release_wrong_parent`, and that `quota_y`'s own legitimate release of its own share
still succeeds correctly. Probe re-run against a temporarily reverted `async_quota.hpp` first, to
confirm both scenarios were genuinely broken pre-fix (not merely assumed), then against the fix,
confirming both close. Probe file and any temporary CMake scaffolding deleted afterward — `git status`/
`git diff --stat` confirmed only `async_quota.hpp` itself carries changes beyond what ADR-141 already
introduced.

Ran `test_identity_authority_grant` directly: pass, unaffected (its own [8]/[9] sections never exercise
either double-allocation or cross-parent release). Full `ctest`: 293 total, 1 pre-existing unrelated
failure, zero regression. `naming_lint.py` clean.

## 5. Not done

- No new permanent test added to `tests/test_identity_authority_grant.cpp` for either scenario — the
  adversarial probe used to find and verify these two fixes was a temporary, standalone file, deleted
  after use rather than promoted into the permanent suite. A real, disclosed gap: neither
  `async_quota.child_share_already_live` nor `async_quota.release_wrong_parent` has permanent
  regression coverage as of this ADR.
- No change to `allocate_child_share()`'s own budget check (`amount > remaining_`) — unaffected by
  either finding.

## 6. Residuals

- The test-coverage gap named in §5 — a real, cheap follow-on if ever wanted (promoting the temporary
  probe's own two scenarios into `tests/test_identity_authority_grant.cpp` as permanent sections), not
  done in this pass since the task this round was scoped to was adversarial verification of ADR-141,
  not authoring new permanent test surface area.
- Same blast-radius note ADR-141 §4 already made: none of `allocate_child_share`/
  `release_child_share`/`try_consume`'s child-share paths have any real production caller yet, so
  neither of these two closed gaps was ever reachable in shipped behavior — closed now, before this API
  accretes a real caller.
