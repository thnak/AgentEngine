# ADR-141 — Closing `AsyncQuota`'s real, shipped double-spend/double-credit bug in the child-share trio

- **Status:** Proposed — implemented, verified (Windows/MSVC), full rebuild (zero errors) and full
  `ctest` clean (293 total, 1 pre-existing unrelated failure, zero regression), `naming_lint.py` clean.
  **Independent red-team round (see ADR-142) found and fixed two further real gaps in this exact
  redesign, same day.**
- **Date:** 2026-08-30/31.
- **Scope:** `include/agentengine/rt/async_quota.hpp` (`try_consume()`/`release_child_share()`
  signature and logic), `tests/test_identity_authority_grant.cpp` (sections [8]/[9] rewritten to match
  the corrected semantics).
- **Related specs:** `decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md` (the
  original port this hardens), `decisions/ADR-111-ledger-merge-async-quota-gating.md`/`ADR-112` (the
  first, and so far only, real production `AsyncQuota` gating this budget primitive underlies — neither
  currently reaches `allocate_child_share()`/`release_child_share()`, confirmed by this ADR's own
  blast-radius check).

## 1. The question

A final-review pass found `AsyncQuota<Kind>::try_consume()` authorized a child registered via
`allocate_child_share()` to ALSO spend directly against the PARENT quota object, using its own
identity — checked only against the parent's general `remaining_` pool, never against how much was
actually allocated to that specific child (`children_[spender.id()]`). A child holding any registered
share, however small, could therefore draw from the whole parent's unallocated pool, unbounded by its
own allocation. `release_child_share(child, amount)` then blindly re-credited the caller-supplied
`amount` unconditionally, with no knowledge of what had already flowed out through either that direct
channel or the child's own separate returned `AsyncQuota` object. Together, a caller could mint
unlimited quota: allocate a share, spend some of it directly against the parent (never touching the
returned child object at all), release the share, and watch the full original amount come back anyway.
Confirmed not yet reachable through any real production caller (only
`tests/test_identity_authority_grant.cpp` exercised this primitive at the time) — should it be fixed
now, before this API accretes a real caller depending on the broken shape?

## 2. Findings

Yes — a genuine I8 budget-enforcement defect in a load-bearing primitive, not a hypothetical. The two
sub-bugs (unbounded direct-parent draw, and blind full-amount re-credit) are two faces of the same root
design flaw: the trio conflates two decoupled spending channels (the child's own returned object, and
direct calls against the parent using the child's identity) with no reconciliation between them.

## 3. What was built

**`try_consume()` is now owner-only.** The `is_child_share` branch (any registered child may spend
directly against the parent) was removed entirely. A child that was split a share via
`allocate_child_share()` must now spend exclusively through the SEPARATE `AsyncQuota` object that call
returned.

**`release_child_share()` now takes the child's own `AsyncQuota` object BY VALUE** (moved, consuming
it), replacing the old `(IdentityHandle child, std::uint64_t amount)` two-argument shape. It credits
back exactly `child_quota.remaining_` — the object's own real, mutation-tracked state — never a
caller-supplied number. A caller cannot fabricate a bogus "unspent" `AsyncQuota` without going through
that type's own mutex-guarded `try_consume()`, so whatever `remaining_` holds is the truth. Anti-replay
is preserved in spirit: the `children_` ledger entry is still explicitly erased on success, so a second
release for the same child identity fails closed instead of re-crediting twice.

`tests/test_identity_authority_grant.cpp` sections [8]/[9] rewritten: proves the direct-parent-spend
channel now fails closed (`async_quota.unauthorized_spender`), proves spending through the child's own
object still works and correctly decrements only that object (never the parent), and proves
`release_child_share()` credits back only the genuinely-unspent remainder (15 of an original 20-unit
allocation after a 5-unit spend through the child's own object — not the pre-fix double-credited
70/65 coincidence the old test's own numbers happened to mask).

## 4. Verification

Full rebuild (zero errors). Ran `test_identity_authority_grant` directly: pass. Full `ctest`: 293
total, 1 pre-existing unrelated failure, zero regression. `naming_lint.py` clean.

**Sanity-checked the fix's own necessity at the interface level**: reverted `async_quota.hpp` alone
(keeping the rewritten test) and confirmed a genuine COMPILE failure (`error C2660: function does not
take 1 arguments`) against the old two-argument `release_child_share` signature — the new test cannot
even be satisfied by the old, buggy API shape. Restored the fix and reconfirmed a clean rebuild.

Blast-radius check: `allocate_child_share`/`release_child_share`/`try_consume`'s `is_child_share` path
are not called anywhere in `include/`/`src/` outside this file — confirmed via grep — so this is a
genuinely additive-risk-free fix with zero production call sites depending on the old, broken
semantics.

## 5. Not done

- No change to `refund()` — a distinct, already-correct compensating-action verb for a caller undoing
  its OWN just-consumed amount, unaffected by this bug or its fix.
- No attempt to make `AsyncQuota` copyable or otherwise change its move-only nature — the new
  `release_child_share(AsyncQuota)` signature relies on exactly that property to make the anti-replay
  and non-fabrication guarantees hold.

## 6. Residuals

- See ADR-142 for two further gaps this exact redesign still had, found and closed the same day by an
  independent adversarial pass: silent clobbering of a live allocation on a second
  `allocate_child_share()` for the same child identity, and cross-parent confusion when the same child
  identity legitimately holds shares from two independent root quotas.
