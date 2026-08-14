# Conflict evidence materialization (`/conflicts/<path>.<agent>`) — design draft

**Status: designed, not implemented.** Matches this session's own gap-4/gap-7/gap-10 precedent: a
primitive whose real caller-side wiring doesn't exist yet gets a real design + self-red-team pass
here, landing as document-only work, per CLAUDE.md's `design → red-team → prove → judge` discipline
applied honestly rather than forcing code onto plumbing that isn't there yet.

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #14. `025-Worktree-and-
Virtual-Filesystem.md` §4 ("conflict → the merge fails and is surfaced, with both versions retained
at `/conflicts/<path>.<agent>`, and the run's supervising agent or a human resolves it" — the
normative requirement this draft designs against). `core/worktree.hpp`'s `merge_branch_into_parent()`/
`MergeConflict` (the existing, real machinery this draft builds on top of, unmodified).
`core/memory.hpp`'s `memory_ref_name(Principal const&)` (the pattern this draft's own
`conflicts_ref_name(...)` is modeled after).

## 1. Re-grounding: this gap is bigger than "pick a storage location"

The audit's own recommended approach: "materializing evidence into the parent Ref via `commit_ref`
directly contradicts an already-passing test... and misattributes ours/theirs — needs a different
storage location and a real committer-identity source." Re-verified directly: `merge_branch_into_
parent()` (`worktree.hpp`) already, correctly, never calls `commit_ref` on the parent when
`!merged->ok()` — it returns `BranchMergeOutcome{std::nullopt, conflicts}` immediately, before any
write. That invariant is genuinely safe today; the audit's own worry (a naive fix that DOES write into
the parent) is a real risk for whoever implements this next, not a bug that exists now.

**What re-grounding additionally found, beyond the audit's own framing:** `merge_branch_into_parent()`
has **zero production call sites anywhere in the codebase** — confirmed by grep across `src/`; the
only callers are `tests/test_worktree_merge.cpp`/`test_worktree_branch_concurrency.cpp` and its own
retry wrapper. `workflow/worktree_scoping.hpp`'s own top comment states this explicitly: "It does NOT
implement merge-on-join (025 §4) for a `branch` executor's worktree — WHEN a branch folds back into
its parent is a separate question this file does not answer." So gap 14 isn't "conflict evidence has
nowhere to go," it's "the whole merge-ON-JOIN call site this evidence would be attached to doesn't
exist in production yet either" — a bigger, structurally earlier gap than the audit's own framing
(which reads as if merges are already happening and only the conflict-surfacing half is missing).

**A second structural finding, changing where conflict evidence even CAN live:** there is no
"session root tree" that `/work`/`/skills/*`/`/agents/*` are subtrees of. `Mount{mount_id, ref_name,
subtree_path}` (`worktree.hpp`'s own comment: "a `/work` mount and an `/input` mount might point at
the SAME ref... or at entirely different refs. Never derived from a guest-supplied path — constructed
only by host policy") is a flat binding; every real mount-producer in this codebase (`memory.hpp`'s
`memory_mount`, `skill_provider.hpp`'s per-skill mounts, `worktree_scoping.hpp`'s per-executor
`/agents/<id>` mounts) points at its OWN independent Ref, not a shared parent tree. `/conflicts/
<path>.<agent>` therefore cannot be assumed to live "under" `expected_parent`'s own tree the way the
RFC's guest-visible path might suggest — it needs its own, separately-committed Ref, mounted at
`/conflicts` by whichever host policy already mounts `/work`/`/agents/*` for that run.

## 2. The design

**(a) `conflicts_ref_name(parent_ref_name) -> std::string`**, a pure, deterministic function modeled
directly on `memory.hpp`'s own `memory_ref_name(Principal const&)` pattern: `parent_ref_name +
":conflicts"`. Deterministic and derived, never caller-supplied (matching `memory_ref_name`'s own
documented rationale for why an aliasing-prone caller-chosen id is a real cross-something leakage
hazard, not a hypothetical one) — two different parent Refs can never collide on the same conflicts
Ref by construction.

**(b) `materialize_merge_conflicts(object_store, ref_store, parent_ref_name, ours_agent_id,
branch, conflicts) -> result<void>`**, a NEW function, called by `merge_branch_into_parent()`'s own
caller (not by `merge_branch_into_parent()` itself — keeping that function's own contract, "returns
conflicts, touches nothing on failure," completely unchanged) immediately after receiving a non-empty
`conflicts` list. For each `MergeConflict{path, base, ours, theirs}`: writes `ours`'s blob (when
present) at `<path>.<ours_agent_id>` and `theirs`'s blob (when present) at `<path>.<branch.child_
name>` inside the CONFLICTS ref's own tree (via `commit_ref(ref_store, conflicts_ref_name(parent_ref_
name), new_tree_digest)`) — a genuinely separate Ref, so `expected_parent`'s own Ref is provably
untouched regardless of what this function does (structural, not merely convention).

**(c) The real committer-identity source, closing the audit's own "misattributes ours/theirs" note**:
`theirs`'s identity is already real and available — `SubWorktree::child_name` (the branch's own,
already-known agent id). `ours`'s identity is NOT knowable from inside the merge machinery itself
(the parent side could be the top-level session, another already-merged sibling, anything) — so
`ours_agent_id` is a REQUIRED, explicit parameter the CALLER supplies, never inferred or guessed. This
is the structural fix the audit's own "misattributes ours/theirs" finding was pointing at: the
previous framing implicitly assumed one side's identity could be derived from the merge inputs alone,
which it cannot be, safely.

## 3. Self-red-team findings

**Writing conflict evidence from INSIDE `merge_branch_into_parent()` was considered and rejected.**
It would need a NEW parameter (`ours_agent_id`) threaded into a function whose existing contract
(`test_worktree_merge.cpp`'s own passing tests) is already relied upon with its current signature —
and would couple two genuinely separate concerns (computing a merge, and durably recording its
failure evidence) into one function, making a future caller that wants the conflict LIST without
paying for a second commit (e.g., a dry-run precondition check) impossible to write cheaply. Keeping
materialization as the CALLER's own explicit second step, only invoked when the caller actually wants
durable evidence, is both more flexible and lower-risk to the already-proven function underneath it.

**A single shared `:conflicts` Ref per parent, not one per conflict-event, is a deliberate choice with
a named consequence.** Every failed merge attempt against the SAME parent accumulates into the SAME
conflicts Ref (new commits, tree grows). This is honest and matches "both versions retained" — nothing
is silently overwritten — but means the conflicts Ref has no built-in retention/pruning, the same
class of residual `decisions/README.md`'s own ADR-038 entry already names for passivation/archival
generally. Named here rather than silently assumed away.

## 4. What this draft does not claim

- **No code.** `conflicts_ref_name`/`materialize_merge_conflicts` are proposals, not real declarations.
- **Does not build merge-on-join wiring itself** — `worktree_scoping.hpp`'s own already-named gap
  ("WHEN a branch folds back... is a separate question") is the real precondition this draft's own
  design depends on being answered first; this draft only designs what happens to conflict evidence
  ONCE a real merge-on-join call site exists to produce it.
- **Does not design the `/conflicts` mount's own host-policy wiring** (which run/session gets one,
  when) — matching `Mount`'s own "constructed only by host policy" framing, a separate, later decision.
- **Does not solve retention/pruning for the accumulating conflicts Ref** (§3) — named, not attempted.
