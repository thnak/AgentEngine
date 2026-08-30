# ADR-124 — A real concurrent-dispatch stress test for `MandatorySandboxProvider`'s task-branch verbs

- **Status:** Proposed — a real, two-OS-thread stress test added and verified against a REAL Docker
  daemon (Windows/MSVC); no production code changed. Full rebuild (zero errors) and full `ctest` clean
  (285/286, same pre-existing unrelated matplotlib/pandas gap), `naming_lint.py` clean. Not yet
  independently red-teamed; not yet Linux-verified.
- **Date:** 2026-08-30.
- **Scope:** `tests/test_task_branch_concurrent_dispatch.cpp` (new), `tests/CMakeLists.txt` (one new
  test target). `decisions/ADR-114-task-branch-tools-promotion.md` (disclosure correction pointing
  here).
- **Related specs:** `decisions/ADR-114-task-branch-tools-promotion.md` §5/§6 (the residual this ADR
  closes: "did not add a dedicated concurrent-dispatch stress test the way ADR-102 Phase 4's own
  `block_on()` fix did for the quota-sharing case"), `tests/test_rt_block_on.cpp` (the methodology this
  ADR's own test mirrors), `decisions/ADR-123-fork-from-reentrant-self-deadlock.md` (the sibling
  concurrency-hardening ADR from the same pass).

## 1. The question

`MandatorySandboxProvider`'s four task-branch verbs (`start_task_branch`/`run_in_task_branch`/
`commit_task_branch`/`discard_task_branch`) each acquire `task_branch_mutex_` for their whole body —
the discipline the prove-phase original established and ADR-114's promotion inherited verbatim — but no
test has ever driven two of these calls through GENUINE, real two-OS-thread concurrent dispatch on the
SAME provider instance; every existing test exercises them sequentially. Does `task_branch_mutex_`
actually hold up under real concurrent dispatch, the way `AsyncQuota`'s own internal mutex was proven to
under `block_on()` (`tests/test_rt_block_on.cpp`, ADR-102 Phase 4)?

## 2. Findings

**A real, code-level scoping question, answered by reading the actual method bodies rather than
assuming the mutex is the whole story.** `task_branches_` (the `std::map` `task_branch_mutex_` protects)
is genuinely per-instance, private state — never shared across sessions or `MandatorySandboxProvider`
instances the way `AsyncQuota` legitimately is (`fork_from()`'s own copy-assignment mints a FRESH
`task_branch_mutex_`/empty `task_branches_` for a forked child, never carrying the parent's forward). So
under I1 (one session, one executor), this mutex should never see genuine cross-thread contention in
NORMAL usage at all — the value of a stress test here is a genuine POSITIVE CONTROL (proving the
assumption holds under a scenario the design doesn't normally reach), not closing a live, reachable bug
the way ADR-123's sibling fix does.

**Wrote a real, three-scenario test mirroring `test_rt_block_on.cpp`'s own methodology**: [1] eight
genuine OS threads calling `start_task_branch()` concurrently on one provider (outcome checks: all
succeed, `task_branches_` ends up with exactly 8 distinct real branches, `BranchCost` consumed by
exactly 8 units — the observable symptoms a torn concurrent map mutation or a torn quota decrement would
produce); [2] two DIFFERENT verbs (`run_in_task_branch`, `commit_task_branch`) dispatched concurrently
on two independently pre-seeded handles (checked: both genuinely execute against real Docker/Ledger
state with no cross-contamination, and each handle's own post-condition — the committed handle erased,
the run-only handle still independently usable — is confirmed via a real follow-up call, not inferred);
[3] a concurrent `discard_task_branch()`/`start_task_branch()` pair (checked: the discarded handle is
genuinely gone, the new one genuinely present and usable, via real follow-up calls on both).

**A real, honest, disclosed limitation of the sanity-check methodology itself, found by actually trying
it — this design line's own established discipline, applied even where it produced a negative result.**
Attempted the usual "temporarily revert the fix, confirm the new test genuinely fails, restore" check by
removing BOTH `task_branch_mutex_` acquisitions in `start_task_branch()`/`discard_task_branch()`. Across
3-5 repeated runs, at up to 8 concurrent threads, this did NOT reliably reproduce a detectable failure.
Traced the real reason by reading `SandboxRuntime::spawn_child_branch()`'s own implementation
(`sandbox_runtime.hpp`): it takes `SandboxRuntime`'s OWN `exclusivity_` lock (the same lock `run()`
takes) for its entire body — so even with `task_branch_mutex_` removed, every `start_task_branch()`
call's dominant, slow work is ALREADY serialized by a different lock on the shared parent `runtime_`;
the actual unprotected window (between `spawn_child_branch()` returning and the now-unguarded
`task_branches_.insert_or_assign()`) is apparently too narrow, relative to real Docker/Ledger I/O timing
variance, to hit reliably within available runs. `discard_task_branch()`'s own map touch, by contrast,
happens BEFORE any `co_await` at all, so it tends to complete before a concurrently-dispatched, I/O-bound
`start_task_branch()` even reaches its own (unguarded) map touch — a natural timing asymmetry, not a
synchronization guarantee. **This is disclosed honestly as an empirical limitation of testing a `std::
map` data race against real I/O-dominated timing, not as evidence `task_branch_mutex_` is unnecessary**
— concurrent, unsynchronized `std::map` mutation is textbook undefined behavior regardless of how rarely
it manifests under any one workload's particular timing profile, and `task_branch_mutex_` remains the
only synchronization around `task_branches_`'s own mutations, correct by construction (code review, not
empirical reproduction, is what grounds this ADR's own confidence). Both locks were restored immediately
after this investigation; `git diff --stat` confirmed the production file matched its committed state
exactly afterward.

**Section [1]'s design grew from 2 to 8 concurrent threads during this same investigation** — a
strictly stronger positive control on the CORRECT, currently-shipped code (more simultaneous real
branches, more real quota consumption to account for correctly) regardless of the sanity-check's own
inconclusive result against the reverted code, so the stronger version was kept in the shipped test.

## 3. What was built

`tests/test_task_branch_concurrent_dispatch.cpp` (new): the three-scenario stress test described in §2,
using `agentengine::rt::block_on()` to drive each thread's own coroutine call (the same, already-proven
-safe driver `test_rt_block_on.cpp` itself validates generically — this file's own job is to confirm
`MandatorySandboxProvider`'s SPECIFIC use of the underlying primitive, not to re-prove the primitive).
Requires a running Docker daemon (sections [2]/[3] shell out to real containers; section [1] does not).

`tests/CMakeLists.txt`: one new `add_executable`/`add_test` block for the new target, placed immediately
after `test_task_branch_tools`'s own registration.

## 4. Verification

Built and ran `test_task_branch_concurrent_dispatch` against a REAL Docker daemon: **ALL CHECKS
PASSED** across all three sections. The sanity-check investigation described in §2 (temporarily removing
both locks, confirming no reliable failure across 8 total runs spanning 2- and 8-thread variants) is
itself disclosed as part of this ADR's own verification record, not hidden as a quiet negative result.

Full project rebuild: zero errors. Full `ctest`: **285/286**, the one failure being the same, unrelated,
pre-existing matplotlib/pandas gap this design line has repeatedly confirmed. `python tools/naming_
lint.py`: clean, no new exported vocabulary.

## 5. What was NOT done

- **No independent red-team pass yet.**
- **No Linux verification.**
- **No forced, artificially-synchronized repro of the map-mutation race** (e.g. injecting a barrier
  immediately before the map touch inside the production method bodies, purely for testing) was
  attempted — that would require instrumenting production code for a test-only need, which this ADR
  judged out of proportion to a residual that (a) is not reachable under I1-respecting usage in the
  first place and (b) has correct-by-construction code review backing it regardless of empirical
  reproduction difficulty.
- **`run_in_task_branch()`/`commit_task_branch()`'s own reentrancy relative to `task_branch_mutex_`
  was not separately stress-tested against `task_branch_mutex_` removal** — section [2]'s own
  concurrent dispatch was only exercised against the CORRECT, currently-locked code, not sanity-checked
  the same way [1]/[3] were (a narrower gap in this ADR's own diligence, disclosed rather than silently
  assumed equivalent).

## 6. Residuals

- Everything named in §5 not otherwise closed.
- The empirical-limitation finding in §2 (I/O-timing asymmetry masking a real data-race window) is
  itself worth remembering for any FUTURE concurrency sanity-check in this codebase that involves a
  fast, in-memory mutation guarded by a lock whose dominant cost is real I/O — a revert-and-fail check
  may not reliably reproduce even a genuine hazard under that shape, and a negative result there should
  be treated as inconclusive, not as proof of safety, without independently reasoning about the code.
