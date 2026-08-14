# ADR-055 — Conflict evidence materialization at `/conflicts/<path>.<agent>`

**Status:** Judged (2026-08-14, project owner sign-off). Designed (inherited from
`docs/planning/conflict-evidence-materialization-design-draft.md`'s own already-self-red-teamed
sketch, unchanged except one field-naming correction found while implementing it — see §2),
implemented, and proven (real code + tests, §4). Re-verified at sign-off review:
`tests/test_worktree_conflict_evidence.cpp` (M1-M6) still passes in full, unchanged since commit
`51083eb`.

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

- **~~Does not build merge-on-join wiring itself.~~ [2026-08-14: CLOSED — see the first Amendment
  below.]**
- **~~Does not design the `/conflicts` mount's own host-policy wiring~~ [2026-08-14: the MOUNT
  CONSTRUCTION primitive is CLOSED — see the second Amendment below; WHICH run/session's capability
  set actually gets granted access, and WHEN, remains a real host-policy decision this codebase still
  does not make anywhere, unchanged.]**
- **Does not solve retention/pruning for the accumulating conflicts Ref** — named explicitly in §4, not
  attempted.
- **Does not change `merge_branch_into_parent()`'s own signature or behavior at all** — `test_worktree_
  merge.cpp`'s existing B2-R1/R2/R3 checks are unmodified and still pass, unaffected by this ADR's
  purely additive new function.

## Amendment (2026-08-14): real merge-on-join wiring

**Status of this amendment: Judged (2026-08-14, project owner sign-off)**, separate from this ADR's
original Judged verdict above (per this project's governance, `decisions/README.md`; `OpenQuestions.md`
OQ-11). Re-verified at sign-off review: `tests/test_rt_workflow_supervisor_merge_on_join.cpp` (J1-J3)
still passes in full, unchanged since commit `82ee6b0`.

At the project owner's explicit direction, this ADR's own §6 residual is closed: a `branch`-mode
workflow executor's worktree now genuinely folds back into its parent, mechanically wired end to end,
for the first time in this codebase — before this amendment, `merge_branch_into_parent()` (and this
ADR's own `materialize_merge_conflicts()`) had zero production call sites; the worktree-scoping and
workflow-execution subsystems were proven in complete isolation from each other.

**The "WHEN" question, answered by the RFC itself, confirmed by direct re-reading before designing
anything**: `025-Worktree-and-Virtual-Filesystem.md` §4, verbatim: *"A `branch` sub-worktree merges
back when its agent completes."* — a per-EXECUTOR-completion event, not tied to `edge_kind::fan_in`
(a message-ROUTING join, a different, easily-confused concept `route_from()` already implements; 025
§4's own merge-on-join has never been about how a reply gets routed onward).

**The design**:
- `rt::WorkflowSupervisor` (`agent_session.hpp`'s sibling, `workflow_supervisor.hpp`) gains a
  `MergeOnJoinHook` (`std::function<result<void>(std::string const& executor_id)>`), the identical
  "caller-injected callback (I2), no ambient authority" shape `checkpoint_hook_` already established —
  fired from the SAME per-executor fold loop that already runs `record_partial()`, for every executor
  whose `worktree_mode == sharing_mode::branch` and whose reply this round was `ok`. `WorkflowSupervisor`
  itself still holds NO `worktree.hpp` type and NO store reference — it passes only the executor's own
  `id` string.
- A cyclic graph can revisit the same executor id across rounds (`mint_executor_worktrees()` mints
  exactly ONE `SubWorktree` per executor id for the whole run, not per visit); rather than trying to
  detect "is this the LAST time this executor runs" — unsound to do cheaply, the identical reason
  ADR-032 §4 gave for why `branch` defaults unconditionally — the hook fires on EVERY completion,
  merging back after each round the executor finishes, keeping divergence windows small rather than
  accumulating them.
- A new `workflow_status::merge_conflict` outcome, and `state_.failed_executor` naming the executor —
  the identical shape `executor_failed`/`routing_failed` already use — for when the hook returns a real
  error (025 §4: "never resolved by guessing... a human resolves it," never auto-retried).
- `workflow/worktree_scoping.hpp` gains `make_merge_on_join_hook(object_store, ref_store,
  run_parent_ref_name, ours_agent_id, wf, grants)` — the "HOW," given `mint_executor_worktrees()`'s
  own already-produced grants (index-parallel to `wf.executors`, the established convention). Builds a
  real `std::function` matching `MergeOnJoinHook`'s signature structurally — this file names no
  `rt::workflow_supervisor.hpp` type at all, keeping `workflow/` from depending on `rt/`. Internally:
  `retry_merge_branch_into_parent` (not the bare single-attempt form — a concurrently-writing `shared`
  sibling makes the stale-parent race a genuinely reachable case here, not hypothetical), and on a real
  conflict, this ADR's own `materialize_merge_conflicts()` before returning the error — evidence is
  durably retained even though the run itself terminates.

**Evidence**: `tests/test_rt_workflow_supervisor_merge_on_join.cpp` (J1-J3, new) — the first test in
this codebase to drive a `branch`-mode executor through `WorkflowSupervisor` to completion:
- **J1** — happy path: a single branch executor's own write folds back into the parent, proven by
  re-reading the parent ref directly afterward, with NO manual `merge_branch_into_parent` call
  anywhere in the test — the supervisor itself did it.
- **J2** — conflict path: a `shared`-mode sibling writes directly to the parent in the SAME round a
  branch executor edits the identical file differently; the run terminates with `merge_conflict`,
  `failed_executor` names the branch, the parent keeps the shared writer's own edit completely
  untouched (never last-writer-wins), and real conflict evidence is durably materialized with both
  sides present.
- **J3** — two independent branch executors completing in the same round both merge cleanly through
  the same hook, proving it isn't a single-branch-per-round assumption.

Full suite: green (this pass), zero regressions — every pre-existing `WorkflowSupervisor`/worktree-
scoping test (`test_rt_workflow_supervisor.cpp`, `test_workflow_worktree_scoping.cpp`,
`test_rt_workflow_supervisor_patterns.cpp`) re-verified passing unchanged.

**What this amendment does not claim**: does not solve the resume-side residual ADR-032 §5 already
named (`SubWorktree::base_digest` isn't durably reconstructed for a resumed branch, so a resumed run's
merge-on-join would need that ancestor re-supplied by a future checkpoint-schema change — unrelated to
and unchanged by this amendment); does not design the `/conflicts` mount's own host-policy wiring or
retention/pruning (both still open, §6 above); does not change `merge_branch_into_parent()`'s own
signature/behavior at all.

## Amendment 2 (2026-08-14): the `/conflicts` mount, and a real reachability bug it caught

**Status of this amendment: Judged (2026-08-14, project owner sign-off)**, separate from this ADR's
original Judged verdict and the first Amendment above (per this project's governance, `decisions/
README.md`; `OpenQuestions.md` OQ-11). Re-verified at sign-off review: `tests/test_worktree_conflict_
evidence.cpp` (M1-M9) still passes in full, unchanged since commit `ecfec15`.

At the project owner's explicit direction, this ADR's own §6 residual — "does not design the
`/conflicts` mount's own host-policy wiring" — is closed for the mount-CONSTRUCTION half: `conflicts_
mount_id(parent_ref_name)`/`conflicts_mount(parent_ref_name)` (`core/worktree.hpp`, right after
`mount_write()`) build a real, guest-visible `Mount` for a parent's own conflicts ref, mirroring
`memory.hpp`'s own `memory_mount_id`/`memory_mount` split EXACTLY — a pure binding, never a
capability, since `Mount`'s own comment already establishes both are "constructed only by host
policy," two separate authorities this project never fuses into one function. WHICH run/session's
capability set actually gets a `cap::FsRead` for this `mount_id`, and WHEN, stays a real host-policy
decision — unchanged, the identical scope `memory_mount()` itself already leaves to its own callers.

**A real, previously-undetected reachability bug, found by writing the FIRST test that ever read
materialized conflict evidence back through the ordinary guest-facing `mount_read()` path** (nothing
before this amendment did — M2's own checks introspected the object store directly via `get_tree()`,
never through a `Mount`). `materialize_merge_conflicts()`'s original implementation built one FLAT,
single-level `Tree` whose entry NAMES were the entire `"<path>.<agent>"` string, `/` and all. Two
independent sources of `/` make this unreachable through `mount_read()`'s ordinary segment-by-segment
walk:
- `MergeConflict::path` is documented, and used elsewhere in this same file, as slash-joined for a
  nested conflict ("a/b/c.txt") — any conflict below the merge root was already affected, independent
  of anything else.
- A workflow executor's own `SubWorktree::name` (the `branch.name` this function's own `theirs` side
  uses as its identity) routinely contains `/` via `worktree_scoping.hpp`'s own `parent_ref_name +
  "/agents/" + executor_id` convention — meaning `theirs`'s evidence for a workflow-driven merge
  (exactly the case Amendment 1's own merge-on-join wiring produces) was silently unreachable through
  `mount_read()` even for a perfectly ordinary, top-level file conflict.

`materialize_merge_conflicts()` is fixed to build a REAL nested `Tree`, reusing this file's own
`detail::set_entry_at_path()`/`detail::ensure_empty_tree()` (the identical mechanism `mount_write()`
already uses for an ordinary write, forward-declared earlier in the file since they were originally
defined later): `MergeConflict::path`'s own `/`-separated segments become real directory levels
(mirroring the original file's own location under `/conflicts`), and the agent identifier (`ours_
agent_id` or `branch.name`) has its own `/` sanitized to `_` before becoming the LEAF's suffix — an
identifier is not itself a navigable path, and should never be mistaken for one.

**Evidence**: `tests/test_worktree_conflict_evidence.cpp`, extended:
- **M7** — `conflicts_mount_id()`/`conflicts_mount()` are deterministic and derived, mirroring M1's
  own proof shape for `conflicts_ref_name()`.
- **M8** — end to end: real conflict evidence (both `ours` and `theirs`, the latter's agent identity
  containing `/`) is read back byte-exact through the ORDINARY `mount_read()` path — the actual
  mechanism a human or supervising-agent host would use — and a capability minted for a different
  `mount_id` is correctly refused. This is the check that caught the bug above: it failed on the
  first implementation, not a hypothetical.
- **M9** — a nested `MergeConflict::path` becomes real nested `Tree` structure, confirmed both by
  reading it back through `mount_read()` and by inspecting the conflicts ref's own root directly (one
  real subdirectory entry, never a flat entry literally named with an embedded `/`).

`tests/test_rt_workflow_supervisor_merge_on_join.cpp`'s own J1-J3 (Amendment 1) re-verified passing
unchanged — their own assertions only checked entry COUNTS, not exact names, so they were unaffected
by the naming fix; re-run explicitly to confirm regardless.

Full suite: green (this pass), zero regressions.

**What this amendment does not claim**: does not decide which session/run's capability set gets
`/conflicts` access, or when (named above, a real, separate host-policy decision); does not solve
retention/pruning for the accumulating conflicts ref (§6, unchanged); does not change `mount_read()`/
`mount_write()`/`set_entry_at_path()`'s own signatures or behavior — only `materialize_merge_
conflicts()`'s internal tree-construction strategy changed, and its own external contract (inputs,
success/failure semantics, "both versions retained") is identical to before this amendment.
