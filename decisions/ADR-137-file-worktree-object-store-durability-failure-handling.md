# ADR-137 — `FileWorktreeObjectStore` fails closed on a real write/rename failure, instead of silently reporting success

- **Status:** Proposed — implemented, verified (Windows/MSVC), full rebuild (zero errors) and full
  `ctest` clean (293 total, 1 pre-existing unrelated failure, zero regression), `naming_lint.py` clean.
  **Independent red-team round (§7) found and fixed two further real defects**, one a genuine
  regression this ADR's own first draft introduced.
- **Date:** 2026-08-30/31.
- **Scope:** `include/agentengine/core/file_worktree_object_store.hpp` only.
- **Related specs:** `decisions/ADR-130-content-durability-conformer.md` (the original port this
  hardens), `decisions/ADR-132-store-generic-sandbox-tool-surface.md`,
  `decisions/ADR-134-durable-sandboxed-shell-chat.md` (the real production consumer whose own
  durability claim depends on this store actually failing closed).

## 1. The question

A final-review pass found `put_blob()`/`put_tree()` computed a `std::error_code` from
`std::filesystem::rename()` and never checked it, and never checked `std::ofstream::write()`'s own
failure state either — a real write or rename failure (disk full, permission denied, an AV lock, a
path-length limit) still returned `Ok(digest)`. Unlike `Ledger::persist_snapshot_locked()`, whose own
comment explicitly accepts a best-effort rename failure (it only loses durability atop an
already-successful in-memory mutation), `FileWorktreeObjectStore` **is** the primary and only
durability layer for content — a caller believing `Ok(digest)` means "durable" has no way to know
otherwise, and a later `get_blob(digest)` on that digest would report `worktree.blob_not_found`,
indistinguishable from real corruption. A real I4 attributability gap. Should this store fail closed
instead?

## 2. Findings

Straightforward in principle — check both failure states, return a real error, best-effort clean up
the temp file. The complication (found only after building the fix and running the existing
concurrency test) is that this store's `put_blob()`/`put_tree()` are explicitly designed to be called
from MULTIPLE independent `FileWorktreeObjectStore` instances sharing one on-disk `root_` (the whole
point of the design — proving cross-process durability, ADR-130 §1). The original, error-ignoring
shape was implicitly safe under that concurrency model because a losing racer's rename failure was
simply discarded; adding real cleanup-on-failure is not automatically safe under the same model. See
§7 for the two real defects this exposed, both closed same-day.

## 3. What was built

`put_blob()`/`put_tree()`: after `std::ofstream::write()`, check the stream's own failure state
(`!out`) and return `worktree.blob_write_failed`/`worktree.tree_write_failed` (best-effort removing the
temp file) rather than silently proceeding. After `std::filesystem::rename()`, check the returned
`std::error_code` and return the same error class on failure (see §7 for the refinement this needed).

## 4. Verification

Full rebuild (zero errors). Ran the existing durability test suite directly:
`test_content_durability_cross_process`, `test_content_durability_concurrency`,
`test_identity_durability_precondition`, `test_task_branch_content_durability_integration`,
`test_worktree_object_store` — all pass. Full `ctest`: 293 total, 1 pre-existing unrelated failure,
zero regression. `naming_lint.py` clean.

Sanity-checked the fix's own necessity by reverting it and confirming a temporary adversarial probe
(pointing the store at a location a write would genuinely fail against) produced the pre-fix silent
`Ok(digest)` before the fix, and the correct `worktree.blob_write_failed`/`worktree.tree_write_failed`
after — probe deleted afterward, `git diff --stat` confirmed clean.

## 5. Not done

- No change to `Ledger::persist_snapshot_locked()`'s own, deliberately different, best-effort
  durability posture — that function's own comment already gives the reasoning for why it stays
  best-effort (branch/ACL bookkeeping durability is a separate concern from content durability).
- No retry logic on a write/rename failure — a caller that wants retry semantics builds it on top of
  this store's own fail-closed `result<T>`, not inside the store itself.

## 6. Residuals

- None beyond what §7 already discloses and closes.

## 7. Independent red-team round (same day, this session's own consolidated final-review pass)

**Adversarial probing found and fixed two real defects, one a genuine regression this ADR's own first
draft introduced, one a pre-existing gap this pass's own scrutiny of the surrounding code surfaced.**

**(1) Regression: shared, digest-derived temp filenames made the new cleanup-on-failure logic actively
unsafe under concurrent same-digest writes.** The first draft's temp filename was `<digest>.tmp` —
identical across every concurrent writer of the SAME content, exactly the scenario
`tests/test_content_durability_concurrency.cpp`'s own `[1b]` section exercises (16 threads x 20
iterations). Under the ORIGINAL, error-ignoring code this was harmless (nothing ever deleted the shared
temp file). Adding real cleanup-on-failure (`std::filesystem::remove(temp, ...)` on a write/rename
failure) made it unsafe: one racing writer's failure-path cleanup could delete a SIBLING writer's
still-in-flight temp file, cascading into failures where the final blob file sometimes never got
created at all — reproduced directly (`[1b]`'s own "the final on-disk blob file exists after every
iteration" check genuinely failed, and a naive re-run hung/timed out entirely). **Fixed** by giving
every temp filename a real per-call-unique suffix (`next_temp_file_suffix()`, a monotonic atomic
counter seeded from a nanosecond timestamp, mirroring `docker_execution_surface.hpp`'s own established
`g_next_container_seq` pattern) instead of deriving it from the digest alone — eliminating cross-writer
temp-file interference entirely, independent of digest collisions. Verified: `[1b]` passes stably
across 8+ consecutive direct runs after the fix (0/8 failures), versus roughly 60% failing before it.

**(2) A second, deeper regression the fix above did not itself close: two independent
`FileWorktreeObjectStore` INSTANCES (each with its own, uncoordinated `mutex_`) racing a rename onto
the SAME final digest-named `path` for identical content.** Reproduced directly against real Windows
filesystem semantics: `MoveFileExW` (what `std::filesystem::rename` uses under MSVC's STL) denies a
rename onto a destination another thread's rename just populated with `ERROR_ACCESS_DENIED`, unlike
POSIX's silently-atomic-replace semantics this code was originally written assuming — a real,
previously undiscovered cross-platform correctness gap, confirmed via a dedicated probe (833/1000
racing renames of identical content from two independent store instances succeeded outright, 167 hit
exactly this "access denied" path; 0 corruption in any case). Made the new hard-failure behavior a real
regression for `create_root_branch()`'s own always-identical empty-tree case
(`tests/test_content_durability_concurrency.cpp`'s `[2c]` section, which constructs exactly two
independent `Ledger<FileWorktreeObjectStore>` instances sharing one durable root) — `[2c]` genuinely
failed 7/10 runs after (1)'s fix alone, versus 0/8 on the pre-ADR-137 baseline. **Fixed**: on a rename
failure, read back the destination and recompute its digest — content-addressing guarantees that if
`path` now holds bytes matching this digest, a sibling writer's rename already won with byte-identical
content (this digest is a pure function of `bytes`, so it does not matter which writer's rename
physically succeeded), so this call correctly reports success instead of a spurious error; only a
genuine content mismatch or continued absence is still reported as a real failure. Deliberately verifies
CONTENT via a full readback-and-recompute, not bare `exists()`, which cannot distinguish "a sibling
writer won" from "an unrelated file happens to already sit at this digest-named path." Verified:
`test_content_durability_concurrency` (full suite, both `[1]`/`[1b]` and `[2]`/`[2b]`/`[2c]`) passes
stably across 10+ consecutive runs after this fix.

**(3) A separate, pre-existing dedup-check gap, found while re-tracing the store's own logic, unrelated
to (1)/(2) but closed in the same pass.** `put_blob()`'s original dedup check was bare
`!std::filesystem::exists(path)`, which treats ANY filesystem object at the digest-named path —
including a directory — as "already have this blob," silently skipping the write and reporting success
with content that was never actually placed on disk anywhere. Proved with a directory-landmine probe (a
directory pre-created at the exact digest-named path). **Fixed** by switching to
`std::filesystem::is_regular_file(path)`, forcing that case through the real write+rename path instead,
where it now fails closed correctly (a directory occupying the digest name means `rename()` itself
fails, correctly reported). Verified: the landmine probe now fails closed as expected; every other
requested test (`test_content_durability_cross_process`, `test_identity_durability_precondition`,
`test_worktree_object_store`, `test_task_branch_content_durability_integration`) still passes.

All temporary probe files and CMake scaffolding used for this round were deleted; `git status`/`git
diff --stat` confirmed only the two intended files (`file_worktree_object_store.hpp` here,
`async_quota.hpp` under the same round's ADR-141/142 work) carry changes beyond what was already
tracked.
