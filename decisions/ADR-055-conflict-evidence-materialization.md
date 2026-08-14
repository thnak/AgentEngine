# ADR-055 — Conflict evidence materialization at `/conflicts/<path>.<agent>`

**Status:** Proposed (2026-08-14). Designed (inherited from
`docs/planning/conflict-evidence-materialization-design-draft.md`'s own already-self-red-teamed
sketch, unchanged except one field-naming correction found while implementing it — see §2),
implemented, and proven (real code + tests, §4); awaiting the project owner's explicit "Judged"
sign-off per this project's governance (`decisions/README.md`; `OpenQuestions.md` OQ-11 — only the
project owner marks an ADR "Judged").

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #14 (the finding this ADR
closes). `docs/planning/conflict-evidence-materialization-design-draft.md` (the design this implements).
`025-Worktree-and-Virtual-Filesystem.md` §4 ("conflict → the merge fails and is surfaced, with both
versions retained at `/conflicts/<path>.<agent>`, and the run's supervising agent or a human resolves
it" — the normative requirement this ADR designs against). `include/agentengine/core/worktree.hpp`'s
`merge_branch_into_parent()`/`MergeConflict` (the existing, real machinery this ADR builds on top of,
unmodified). `core/memory.hpp`'s `memory_ref_name(Principal const&)` (the pattern `conflicts_ref_name()`
is modeled after).

## 1. The question

**Stated so it has a wrong answer:** when `merge_branch_into_parent()` reports a genuine conflict, does
this codebase have any way to durably retain "both versions" the way 025 §4 requires, or does the
conflict information simply vanish the moment the caller's local `BranchMergeOutcome` goes out of
scope?

**Before this ADR: it vanished.** `MergeConflict{path, base, ours, theirs}` was real, already correctly
computed and returned — but nothing ever wrote it anywhere durable. `merge_branch_into_parent()` itself
already, correctly, never calls `commit_ref` on the parent when the merge fails — it returns
`BranchMergeOutcome{std::nullopt, conflicts}` immediately, before any write, an invariant re-verified
directly against current code and confirmed genuinely safe (the audit's own worry about a naive fix
that WOULD write into the parent describes a risk for whoever implements this next, not a bug that
existed).

## 2. A correction to the design draft, found while implementing it

The design draft's own §2c described the branch's identity as available via `"SubWorktree::child_name
(the branch's own, already-known agent id)"`. Checking `worktree.hpp` directly: `SubWorktree` has no
field literally spelled `child_name` — `create_sub_worktree()`'s own PARAMETER is named `child_name`,
but it becomes `SubWorktree::name` in the constructed struct (`SubWorktree{std::move(child_name), ...}`
— the first positional field). The draft's prose was describing the semantic role, not a literal member
name. `materialize_merge_conflicts()` uses `branch.name` — behaviorally identical to what the draft
intended, corrected here only so a reader checking the draft against real code isn't confused by a
member name that doesn't exist.

## 3. The design (as implemented)

Full writeup lives in `worktree.hpp`'s own "Conflict evidence materialization" banner comment;
summarized here.

**(a) `conflicts_ref_name(parent_ref_name) -> std::string`** — a pure, deterministic function:
`parent_ref_name + ":conflicts"`. Deterministic and derived, never caller-supplied, matching `memory_
ref_name`'s own rationale exactly: two different parent Refs can never collide on the same conflicts
Ref by construction (M1 proves this directly, not just by inspection).

**(b) `materialize_merge_conflicts(object_store, ref_store, parent_ref_name, ours_agent_id, branch,
conflicts) -> result<void>`** — a NEW function, called by `merge_branch_into_parent()`'s own CALLER
(never by that function itself, keeping its existing, already-relied-upon "returns conflicts, touches
nothing on failure" contract completely unchanged). For each `MergeConflict`: writes an entry pointing
at `ours`'s digest (when present) at `<path>.<ours_agent_id>` and one pointing at `theirs`'s digest
(when present) at `<path>.<branch.name>`, inside the CONFLICTS ref's own tree — genuinely separate from
`expected_parent`'s own Ref, so the parent is provably untouched regardless of what this function does
(M2 proves this directly: the parent ref is re-read after materialization and found byte-identical to
its pre-materialization state).

**A design detail not spelled out in the draft, found necessary during implementation**: entries reuse
the ALREADY-STORED digests directly rather than re-writing fresh blobs. `object_store` is the SAME
content-addressed store the parent/branch trees already live in, so a `TreeEntry{name, digest, is_tree}`
pointing at an existing digest already IS "both versions retained" — no `get_blob`/`put_blob` round-trip
needed, and the approach is correct regardless of whether a conflicting entry is a blob or (a
blob-vs-tree type fork) itself a tree, since `is_tree` carries through unchanged.

**(c) The real committer-identity source (closing the audit's own "misattributes ours/theirs" note)**:
`theirs`'s identity is `branch.name` — already real and known. `ours`'s identity is NOT knowable from
inside the merge machinery itself (the parent side could be the top-level session, another
already-merged sibling, anything), so `ours_agent_id` is a REQUIRED, explicit parameter the CALLER
supplies, never inferred or guessed — the structural fix for the audit's own finding that the previous
framing implicitly assumed one side's identity could be derived from the merge inputs alone, which it
cannot be, safely.

## 4. Self-red-team findings (design draft §3, verified still correct against the real implementation)

**Writing conflict evidence from INSIDE `merge_branch_into_parent()` was considered and rejected.** It
would need a new `ours_agent_id` parameter threaded into a function whose existing contract (`test_
worktree_merge.cpp`'s own passing tests) is already relied upon with its current signature, and would
couple two genuinely separate concerns (computing a merge, durably recording its failure evidence) into
one function. Keeping materialization as the caller's own explicit second step is both more flexible
(a caller that only wants the conflict LIST, e.g. a dry-run precondition check, doesn't pay for a
second commit) and lower-risk to the already-proven function underneath it.

**A single shared `:conflicts` Ref per parent, not one per conflict-event, is a deliberate choice with
a named consequence — proven directly (M5), not merely asserted.** Every failed merge attempt against
the SAME parent accumulates into the SAME conflicts Ref (new commits, tree grows) — M5 proves the first
attempt's evidence is not silently dropped when a second, later attempt against the same parent
commits its own evidence. This is honest and matches "both versions retained" — nothing is silently
overwritten UNLESS the exact same `<path>.<agent>` key repeats (a genuinely repeat conflict at the
identical location, where replacing stale evidence with fresh evidence is the correct behavior, not a
bug) — but means the conflicts Ref has no built-in retention/pruning, the same class of residual
`decisions/README.md`'s own ADR-038 entry already names for passivation/archival generally. Named here,
not silently assumed away.

## 5. Evidence

`tests/test_worktree_conflict_evidence.cpp` (M1-M6, new):
- **M1** — `conflicts_ref_name()` is deterministic (same parent → same name) and collision-free across
  distinct parents.
- **M2** — end-to-end: a REAL `merge_branch_into_parent()` conflict (parent and branch both edit `a.txt`
  differently, the identical fixture `test_worktree_merge.cpp`'s own B2-R2 uses) is materialized — both
  sides' real content digests land at the correct `<path>.<agent>` keys, retrievable through the
  conflicts ref's own tree, and the parent ref is re-read afterward and found completely untouched.
- **M3** — an add/add divergence (no `base` entry at all) materializes both sides correctly.
- **M4** — an edit/delete fork materializes only the side that actually still exists — no fabricated
  entry for the deleted side.
- **M5** — a second failed merge against the same parent accumulates into the same ref; the first
  attempt's evidence survives alongside the second's.
- **M6** — an empty conflicts list is a no-op — no conflicts ref is created at all when there's nothing
  to materialize.

Full suite: green (this pass), zero regressions.

## 6. What this ADR does not claim

- **Does not build merge-on-join wiring itself.** `merge_branch_into_parent()` still has zero
  production call sites in this codebase (confirmed by grep, unchanged by this ADR) —
  `workflow/worktree_scoping.hpp`'s own top comment already states explicitly that it does NOT
  implement merge-on-join (025 §4) for a `branch` executor's worktree; WHEN a branch folds back into its
  parent remains a separate, not-yet-answered question. This ADR designs what happens to conflict
  evidence ONCE a real merge-on-join call site exists to produce it — it does not build that call site.
- **Does not design the `/conflicts` mount's own host-policy wiring** — which run/session gets one, and
  when, matches `Mount`'s own "constructed only by host policy" framing, a separate, later decision.
- **Does not solve retention/pruning for the accumulating conflicts Ref** — named explicitly in §4, not
  attempted.
- **Does not change `merge_branch_into_parent()`'s own signature or behavior at all** — `test_worktree_
  merge.cpp`'s existing B2-R1/R2/R3 checks are unmodified and still pass, unaffected by this ADR's
  purely additive new function.
