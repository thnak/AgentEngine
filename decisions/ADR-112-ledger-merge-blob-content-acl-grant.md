# ADR-112 — `Ledger::merge()` closes its own "structural, not content-wise" blob-level ACL gap

- **Status:** Proposed — implemented, verified against the real `InMemoryWorktreeObjectStore`
  (Windows/MSVC), full project rebuild and `ctest` clean outside the same 5 pre-existing,
  environment-caused failures ADR-111 already named (Docker daemon not running; unrelated Python
  worker/matplotlib gap) — none touch `Ledger`/`merge()`. Not yet independently red-teamed by a fresh
  agent, and not yet re-verified on Linux.
- **Date:** 2026-08-29.
- **Scope:** `include/agentengine/core/ledger.hpp` (`Ledger<Store>::merge()` body only — no signature
  change, no new Kind tag, no other file touched) and `tests/test_ledger.cpp` (case [4] extended with
  two new checks: a positive proof the new grant works, and a negative control proving it doesn't
  over-widen to an unrelated identity).
- **Related specs:** `include/agentengine/core/ledger.hpp`'s own `merge()` — specifically the
  "SCOPE LIMIT, disclosed" comment this ADR replaces, written when `merge()`'s tree-level parent-owner
  grant first landed (ADR-102 Phase 2's own port, itself fixing a real, live-reproduced
  `ledger.tree_access_denied` MUST-FIX finding) and left this exact residual named but unattempted.

## 1. The question

`merge()` already grants `parent_state.created_by_id` (the parent branch's own owner) root ACL access
to the merged tree's own digest, so the "orchestrator spawns a sub-agent, the sub-agent merges its
work back, the orchestrator resumes" flow doesn't permanently lock the orchestrator out of its own
branch. But that grant is **tree-digest-level only**: the merged tree's own entries (blobs the child
alone wrote, or nested subtrees) stay under only the child's own ACL root. The parent owner could list
the merged tree's structure (`get_tree_safe()`) but not fetch the actual bytes of a blob the child
alone contributed (`get_blob_safe()` still failed `ledger.blob_access_denied`) — named as a disclosed,
unattempted residual since ADR-102 Phase 2. Is "the orchestrator resumes" now fully restored, or is
this still only a structural fix?

## 2. The fix

Extends the SAME existing `if (parent_state.created_by_id != requested_by.id())` grant block with a
per-entry loop: for every entry in the just-merged tree, grant `parent_state.created_by_id` root
access to that entry's own digest too (`tree_acl_` if `entry.is_tree`, `blob_acl_` otherwise) — the
identical `insert_acl_root_bounded()` call already used for the top-level tree digest, just applied
once per entry. Reasoning is unchanged from the existing tree-level grant's own comment: the parent
branch's owner already owns the branch this merge lands on, so granting them content-level access to
what just became their own branch's new state does not widen authority to a stranger — it completes
the same category of grant already made one level up, for content that is now genuinely part of their
own resource.

A snapshot of the merged entries (`merged_entries_for_owner_grant`) is taken just before
`store_.put_tree(std::move(merged.merged))` moves `merged.merged` out from under the function — but
**only** when the grant will actually run (`parent_state.created_by_id != requested_by.id()`), to
avoid an unnecessary copy on the common case where the merge requester already owns the branch.

**Deliberately best-effort, not fail-closed**: if a per-entry grant hits
`ledger.acl_root_cap_exceeded` (a digest already at its configured cap of distinct authorized roots —
rare in practice), that single grant is silently skipped rather than rejecting the whole,
already-successful merge. The tree-level grant — the one property `merge()`'s own callers actually
depend on structurally — has already succeeded by this point; unwinding that decision to fail the
entire merge over one capped blob would trade a narrow, pre-existing content-access residual for a far
more surprising full-merge rejection. A capped entry is left exactly as inaccessible to the parent
owner as it already was before this fix — no regression, not a new hazard, just not fully closed for
that one digest (an already-authorized principal can still use `mark_digest_shared()` on it directly).

## 3. Verification

`tests/test_ledger.cpp` case [4] extended, not just re-run:

- **Positive proof**: after the existing tree-level checks (`get_tree_safe()`/`head_tree_digest()` for
  `owner` post-merge), a new `ledger.get_blob_safe(*blob2_r, owner)` call is asserted to succeed —
  `blob2` is `child_identity`'s own write, never touched by `owner` directly, and previously failed
  `ledger.blob_access_denied` even after the tree-level fix.
- **Negative control**: `ledger.get_blob_safe(*blob2_r, unrelated)` is asserted to still fail with
  `ledger.blob_access_denied` — proving the new grant is scoped to `parent_state.created_by_id`
  specifically, not accidentally opened to every caller.
- **Sanity-checked the test itself, not just the fix**: temporarily disabled the new per-entry loop
  (replaced its body with a no-op) and re-ran — the new positive-proof check failed exactly as
  expected (`FAIL: ADR-112: the PARENT branch's own creator can now fetch the actual BYTES...`),
  confirming the test genuinely exercises the fix rather than passing vacuously. Restored and
  re-verified all checks pass again before landing.

Full local verification (Windows/MSVC): `test_ledger` passes standalone; full project rebuild
(`cmake --build . --config Debug`) completes with zero new errors; full `ctest` reports the same
246/251 (98%) ADR-111 already established, with the identical 5 pre-existing, environment-caused
failures (none touch `Ledger`/`merge()`); `python3 tools/naming_lint.py` reports clean (no new
exported vocabulary — this ADR adds no new type, only extends an existing method's body).

## 4. What was NOT done

- **No independent red-team pass yet.** Given this exact lineage's own repeated finding — every
  independent pass on this design finds something real (ADR-108, ADR-109, ADR-111's own MUST-FIX all
  demonstrate this) — this should not be assumed clean merely because the change is small and the
  existing suite stayed green.
- **No Linux verification.** Windows/MSVC only this pass.
- **Nested-subtree entries are logically covered by the same loop (`entry.is_tree` routes to
  `tree_acl_`) but not separately exercised by a dedicated test** — every existing `test_ledger.cpp`
  scenario uses flat, single-level trees (`is_tree=false` entries only). The loop's own logic doesn't
  distinguish blob vs. subtree grants beyond which ACL table to use, so this is a reasoned, not
  independently proven, extension.

## 5. Residuals

- **Awaiting an independent red-team round and Linux re-verification**, per §4.
- **A digest at its ACL root cap stays inaccessible to the parent owner** (§2's own best-effort
  disclosure) — an already-authorized principal's `mark_digest_shared()` remains the only recourse;
  not attempted as a design change here.
- **This grant only ever covers digests reachable from THIS merge's own resulting tree** — content the
  child wrote but which did not survive into the final merged result (e.g., a file the child later
  deleted before merging) never becomes reachable to the parent owner through this mechanism. This is
  an intentional boundary (content that isn't part of the branch's new state was never meant to become
  reachable through it), not an oversight, but named explicitly rather than left implicit.
- **Every residual ADR-111 already named remains unchanged**: `merge_into()`'s own pre-existing
  concurrency hazard (ADR-102 Phase 3 §22), `merge_into()` still having no real first caller anywhere
  in the tree, and the Docker-daemon-dependent test failures in this environment.
