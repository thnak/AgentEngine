# ADR-238: `Ledger` over an asynchronous `WorktreeObjectStore` -- locking discipline

Status: **Proposed** (2026-10-15), red-teamed twice (section 9) before any code; implemented and proven the same day
(section 10); awaiting the owner's Judge sign-off.
Parent: ADR-237 §5.1 / §14 step 49 (#172). Owner decision (2026-10-15): the store goes `task`-shaped *including*
`Ledger`; no dual blocking face.

## 1. Problem

ADR-237 §5.1 makes `WorktreeObjectStore` (`put_blob`/`get_blob`/`put_tree`/`get_tree`, and the optional
`blob_size`) return `task<result<...>>`. `Ledger<Store>` (core/ledger.hpp, ledger_impl.hpp, ADR-102 Phase 2) is the
identity-scoped ACL/branch/merge layer over that store, and it calls the store **while holding `mutex_`**, a
`std::mutex` that guards all of its state (`branches_`, `blob_acl_`, `tree_acl_`, `pending_abandons_`,
`orphaned_from_restart_`, `branch_seq_`). A `std::mutex` held across a `co_await` is the hazard ADR-237 §14.1 Phase D
names for the A2A server: the coroutine can resume on another thread (unlocking from a non-owner is UB), and on a
one-thread lane a second coroutine that reaches the same lock deadlocks against the suspended holder.
`rt::block_on` is not an escape: it is refused on a lane (`rt.block_on_on_lane`), and ADR-175 removed the
single-resume bridge because destroying a frame that may already be running elsewhere was a use-after-free.

The store call sites inside the lock are exactly eight: `get_blob_safe`, `get_tree_safe`, `create_root_branch`
(`put_tree`), `commit` (`put_tree`), `put_blob_locked` (`put_blob`, reached from `put_blob_safe` and
`BlobWriteBatch::put`), `perform_three_way_merge_locked` (`get_tree` x3) and `commit_merged_tree_locked`
(`put_tree`). Every consumer of the async-shaped ones (`RealIoFileSystem::drain_into_tree`,
`scan_and_drain_into_tree`, `materialize`, called from `SandboxRuntime`) is a coroutine -- but `materialize` holds a
`std::mutex` of its own across its blob reads (section 3.9).

## 2. What the lock was protecting (and what it was not)

**(a) The in-memory state.** Short read-modify-write sections over the maps. Needs a mutex, never needs to be held
across I/O.

**(b) Multi-step operations whose later steps depend on state read in earlier steps.** Only `merge()`: it reads the
parent's head (`ours`), the child's head (`theirs`) and base, loads three trees, merges, writes the merged tree, then
moves the parent's head. Any operation that moved or deleted either head between the read and the move would make the
merge commit a result computed against a stale head -- a lost update. Under the old single lock that was impossible.
`commit`, `reset_to`, `abandon` and `create_root_branch` all move, delete or overwrite a head, so they must not
interleave with a merge's window.

What the lock was **not** protecting: the store. Content addressing makes `put_*` idempotent and `get_*` immutable
reads, and both conformers take their own locks. The old comment "every store_ access is serialized by THIS Ledger's
own mutex_" overstated it.

## 3. Decision

Two locks, with a rule for each, plus rules for reentrancy, cancellation, refusal and faults.

1. **`mutex_` (`std::mutex`) is the state lock: never held across a suspension.** No scope that owns a
   `std::lock_guard` contains a `co_await`. Enforced by structure (a phase is a block or a helper returning a value)
   and by tests T1/T9.
2. **`op_mutex_` (`rt::AsyncMutex`) is the operation lock**, held for the whole body of exactly these operations:
   `create_root_branch`, `commit`, `reset_to`, `merge`, `abandon`, `abandon_orphaned_branch` (the operations that move,
   delete or overwrite a branch head). It is **not** taken by `branch_from` (it moves no head and does no store I/O;
   the red team showed taking it adds refusal and deadlock exposure for nothing), `put_blob_safe`/`BlobWriteBatch`,
   `get_*_safe`, `mark_digest_shared`, `reclaim_orphaned_branch`, or any read accessor.
3. **Order: caller's quota `try_consume` -> `op_mutex_` -> phases -> release `op_mutex_` -> failure refund.** A quota
   wait never holds the op lock; a refund never runs under it. (`AsyncQuota` releases its own mutex before returning, so
   the two cannot cycle.)
4. **Acquisition is the checked form** `co_await op_mutex_.lock(lock_entry::mutating)`, which returns
   `std::expected<Guard, lock_refusal>`. The plain `lock()` throws `lock_refused` when a task descended from a holder
   asks (ADR-237 §4.3); an escaping exception would skip the refund and leave a `merge`'s child unresolved. On refusal:
   refund the quota, register `child` as a reclaimable orphan (merge), return `policy` / `ledger.op_lock_refused`.
5. **No reentrancy.** `AsyncMutex` has no recursive mode: the same task locking twice waits on itself forever
   (`relate(me, me)` classifies as `self` and falls through to the FIFO queue). Each public locker locks once and calls a
   private `*_op_locked` helper that never locks. `abandon_orphaned_branch` and `reap_pending_abandons` use
   `abandon_op_locked` / the public `abandon` respectively and never nest a public locker inside a held lock. A
   planted mutant proves it (T6).
6. **Phases.** `A` (under `mutex_`): authorize, copy every name and digest the later phases need into locals, **do not
   keep a `BranchState*`/reference or read a `BranchHandle`/quota parameter again**. `B` (no `mutex_`): store calls and
   the pure merge. `C` (under `mutex_`): re-find every branch by the copied name, apply ACL inserts and the head move,
   `persist_snapshot_locked()`. Per-entry authorization of a merge result and the owner grants stay in `C` under the
   lock (they read `*_acl_`).
7. **Cancellation.** A coroutine frame destroyed while parked in `B` (the house "drop the handle" idiom, ADR-017) must
   not lose work. `merge` takes `child` by value, and its destructor would queue a real abandon. So `merge` arms an
   RAII `InFlightChild` guard in `A` that marks `child.resolved_` and, if destroyed without reaching `C`'s success
   point, registers the child as a reclaimable orphan (the same recovery every other failed merge path gives). A
   cancelled head-moving operation does **not** refund its quota (a refund is a `co_await`, which a destructor cannot
   do): the budget is over-charged, which is the safe direction for I8 -- named in section 8.
8. **Faults.** A store call that throws (a faulted `task`) and any exception in `C` are caught and converted to a
   `fatal` `ledger.store_faulted` / `ledger.internal_fault` error at the phase boundary, so the normal failure path
   (refund, orphan child, release the guard) runs.
9. **`RealIoFileSystem::materialize`** read every blob while holding its own `sync_mutex_` (a `std::mutex`). It now
   prefetches all blobs first (no lock), then takes `sync_mutex_` for the wipe and the writes. This also moves a failed
   blob read ahead of the wipe, so a rollback that cannot read no longer destroys the working directory first.
10. **`BlobWriteBatch`** stays a single-producer object: its `unpersisted_` flag becomes `std::atomic<bool>`, and the
    contract (one in-flight `put` at a time, the batch outlives its puts) is stated and checked by a debug counter.
11. **`reap_pending_abandons`** re-verifies, under `op_mutex_`, that the branch it is about to erase is still the one
    it snapshotted (same creator, same head tree) before erasing -- the window between snapshot and abandon is wider now.
12. **`blob_count_safe()`** stays synchronous: it reads a conformer-specific `blob_count()` that is not in the concept
    and does no I/O on either conformer.

## 4. Why this is safe (claims the proofs must hold)

- **C1. ACLs are monotonic.** No code path removes an entry from `blob_acl_`/`tree_acl_` (verified by the concurrency
  review and again by the authorization review: only `branches_` and `orphaned_from_restart_` are erased;
  `load_durable_state` runs before any concurrency; `IdentityAuthority` ancestry only grows). An authorization decided
  in `A` still holds in `C`; sharing only widens; admission under the root cap is the one non-monotonic rule and it fails
  closed, re-evaluated in `C` against the live set.
- **C2. Unreferenced stored objects are unreachable.** Reads are gated solely on an ACL entry written in `C`; before,
  `put_tree` already ran ahead of the branch lookup. (Premise: the store honours content addressing -- section 7.)
- **C3. Head-moving operations are mutually exclusive** via `op_mutex_`; readers see only whole `C` sections.
- **C4. `commit` derives turn/parent/self-digest in `C`** from the head read there, so commits to one branch form a
  linear chain.
- **C5. No new authority.** Nothing here widens an ACL or a capability. `is_branch_owner`, `authorized_for` and the
  ownership gates are unchanged and run in the same operations.

## 5. Behaviour that changes, stated

- Sync readers (`head_tree_digest`, `head_checkpoint`, `checkpoint_at`, `orphaned_branches`) no longer block behind a
  mutating operation's store I/O; they observe the state before or after a whole `C` section. Claim corrected from the
  first draft: `persist_snapshot_locked()` still does blocking file I/O under `mutex_`, so they still block behind
  that write.
- `Ledger::get_blob_safe`, `get_tree_safe`, `put_blob_safe` and `BlobWriteBatch::put` become `task`s.
- A merge whose `B` outlasts a stalled store holds `op_mutex_` for that time, blocking the other head-moving
  operations on the **whole ledger**; the old code held a mutex for the same span.
- `materialize` reads all blobs before wiping (section 3.9).

## 6. Proof obligations (tests, each with a planted mutant that must fail)

Infrastructure: a one-lane `Runtime`, and a store wrapper whose calls park on the reactor and can be gated (hold until
the test releases them), so interleavings are forced, not hoped for.

- **T1 no `mutex_` across suspension.** N concurrent `commit`s on one branch. All complete, turn indices are exactly
  1..N, parents chain. Mutant: `put_tree` awaited inside the `mutex_` scope -> lane deadlock (watchdog).
- **T2 merge vs commit.** A commit parked in `put_tree`, a merge started meanwhile. The merge waits, then merges
  against the post-commit parent (the commit's file is in the result). Mutant: no `op_mutex_` -> the file is lost.
- **T3 abandon vs merge.** `abandon(child)` while that child's merge is parked: serialised -- merged-then-abandon-noop
  or abandoned-then-`unknown_branch`, never a merged result for a deleted child. Mutant: `abandon` without the lock.
- **T4 authorization gates the async reads.** An unauthorized `get_blob_safe`/`get_tree_safe` never reaches the store
  (counting store); an authorized one reaches it once. Merge result entries the requester is not authorized for are
  still rejected in `C`.
- **T5 store fault.** A `put_tree` that throws in `commit` refunds the quota, releases `op_mutex_`, and the next
  `commit` succeeds.
- **T6 no self-deadlock.** `abandon_orphaned_branch` completes (and `reap_pending_abandons` under contention).
  Mutant: `abandon_orphaned_branch` calling the public `abandon` while holding the lock -> hangs (watchdog).
- **T7 cancel in `B`.** Destroy a merge task parked in `get_tree`; then `reap_pending_abandons()`; the child still
  exists and is in `orphaned_branches()`. Mutant: no `InFlightChild` guard -> the child is erased.
- **T8 refusal.** An op requested by a task descended from an `op_mutex_` holder returns `ledger.op_lock_refused`,
  refunds, orphans the merge child, and throws nothing. Mutant: plain `lock()` -> an exception escapes.
- **T9 `materialize` holds no `std::mutex` across a Ledger await.** A concurrent `write()` on the same one-lane
  runtime completes while `materialize` is parked in `get_blob_safe`. Mutant: the old placement -> deadlock.
- The existing `ledger` suite (17 files) and the worktree/sandbox suites pass with call-shape changes only.

## 7. Pre-existing findings the review surfaced -- NOT changed here, named for follow-up

"A seam going async must not move behaviour" (ADR-237 §14.1); each of these exists in the synchronous code.

1. **`merge` loads and may reveal trees the requester is not authorized for.** Only `theirs` is authorized before
   `base`, `ours` and `theirs` are read; a conflict error carries `conflicts.front().path` and an unauthorized merged
   entry's path and 12-char digest prefix. Candidate fix: also require `authorized_for(ours)`/`(base)`. Needs its own
   decision (it changes who may merge).
2. **`Ledger` trusts the digest a store returns** (`put_*` results become ACL keys; `get_*` bytes are not re-hashed). A
   lying or request-coalescing async store could grant a writer access to another tree's digest. Candidate fix:
   recompute after `put_*`/`get_*` (a performance cost on the drain path that ADR-102 specifically removed). The
   contract on the concept now states the store must return the digest of the content it stored.
3. **A stale `BranchHandle` re-found by name acts on a re-created branch** (`create_root_branch` uses
   `insert_or_assign` on a deterministic name). Cannot cross principals (owner id is in the name). Candidate fix: a
   per-branch generation number.
4. **`acl_root_cap_exceeded` is an existence/popularity oracle for a known digest, and `create_root_branch` adds every
   owner to `tree_acl_[empty_tree]`, so the 65th distinct owner fails** (default cap 64).
5. **`would_accept_blob_write` can approve two concurrent scans that then collide** (already true; the mutex was
   released between blobs).

## 8. Residuals

- `AsyncMutex` has a documented narrow window (its own banner): a waiter granted but not yet resumed whose frame is
  destroyed leaves the lock held. For a session that is a dead session; for `op_mutex_` it would be a dead ledger.
  Not fixed here (it is `AsyncMutex`'s contract); named so a watchdog or an `AsyncMutex` fix can be scheduled.
- A head-moving operation cancelled after its quota was consumed over-charges the budget (section 3.7).
- File-backed stores still do blocking file I/O inside their `task` bodies (ADR-237 §6 `rt::offload` is not part of
  this decision), so on a lane they block the lane for the I/O, as the other file stores do.
- One `op_mutex_` per ledger serialises unrelated branches' head moves.
- `persist_snapshot_locked()` rewrites the whole snapshot under `mutex_` on every `C` (existing cost).

## 9. Review record

Design -> red-team (two independent agents, adversarial prompts, read-only) -> this revision. Concurrency review: 11
findings, 1 FATAL (reentrant `abandon_orphaned_branch`), 4 MAJOR (cancel in `B`, throwing `lock()`, granted-waiter
wedge, handle reads in `C`), 6 minor; all folded into section 3 except the `AsyncMutex` residual (section 8). Authorization
review: no hole in the phase split; 1 MAJOR new (`materialize` lock, section 3.9), 3 pre-existing (section 7); claims C1,
C2, C3, C5 and the quota/op-lock ordering survived falsification.

## 10. Implementation evidence (2026-10-15)

Implemented as ADR-237 §14 step 49. `tests/core/ledger/test_ledger_async_locking.cpp`, 44 checks, T1-T9 as in section 6
(T3 is "an abandon of another branch is serialised behind an in-flight merge": a second handle to a merging branch
cannot exist, so the same-branch race is reachable only through the reaper, whose re-verification (item 11) is covered
by construction, not by a forced interleaving). T8 needed a real one-lane `Runtime` and `when_all`: the refusal exists
only for a scope-forked child of the holder. All seven planted mutants fail the test (T2, T3, T7, T8 by failed checks;
T1 by crash; T6, T9 by hang under a 40 s watchdog).

Deviations from the text above, all toward fewer moving parts: the error for a refused lock is `policy` /
`ledger.op_lock_refused` (as specified); item 10's debug counter for `BlobWriteBatch` was not added (the `std::atomic`
flag and the documented contract are); the merge's per-entry authorization reads `*_acl_` in a second locked section
(phase A2) between the unlocked reads and the unlocked `put_tree`, rather than in `C`, with the owner grants applied in
`C` -- ACL monotonicity (C1) is what makes that sound. A store fault keeps `ledger.store_faulted` rather than being
re-coded to the call's own `*_failed` code.

