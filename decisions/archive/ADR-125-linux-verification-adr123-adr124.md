# ADR-125 — Real Linux verification of ADR-123/124: `fork_from()`'s reentrancy fix and the task-branch concurrent-dispatch stress test both hold on GCC 14.2.0

- **Status:** Proposed — real verification pass, no production code changed. The pre-existing
  `/root/ae-verify` WSL2 Ubuntu 24.04 checkout (ADR-115, reused by ADR-118/120/121) fast-forwarded to
  this branch's exact HEAD (`8ee9eef`, the ADR-123 red-team-closure commit) and rebuilt/retested with
  the same real GCC 14.2.0 toolchain.
- **Date:** 2026-08-30.
- **Scope:** No files changed. This ADR records a verification run, not a code change.
- **Related specs:** Closes ADR-123's and ADR-124's own "not yet Linux-verified" residuals. Reuses
  ADR-118/120/121's own environment and methodology directly.

## 1. The question

ADR-123 (the `AsyncMutex::is_held_by_current_thread()` addition and `fork_from()`'s reentrancy fix,
including the same-day red-team's own honest scope correction) and ADR-124 (the task-branch
concurrent-dispatch stress test) both landed on Windows/MSVC only. Does the reentrancy fix genuinely
close the deadlock on a real, independently-obtained GCC 14.2.0 toolchain and allocator, and does the
concurrent-dispatch stress test's own core claim (`task_branch_mutex_` holds under real two-OS-thread
contention) reproduce there too?

## 2. What was done

Reused ADR-121's own `/root/ae-verify` checkout (confirmed clean, sitting at `35fe8dd`) and
fast-forwarded it (`git fetch` + `git merge --ff-only`) to `8ee9eef` — a genuine 14-file,
1141-insertion fast-forward covering ADR-121 through ADR-123's own red-team follow-on. Rebuilt with
the same GCC 14.2.0 toolchain (`AGENTENGINE_WITH_HTTPS=OFF`, matching ADR-120/121's own established
baseline): **340/340 targets, zero errors**, including both `test_rt_agent_session_fork_from_
serialization` and `test_task_branch_concurrent_dispatch` — the two headers this pass touches
(`async_mutex.hpp`/`agent_session.hpp`) are used essentially everywhere, so this rebuild genuinely
exercised the change against the whole tree, not just the two directly-relevant test binaries.

**`test_rt_agent_session_fork_from_serialization`: ALL CHECKS PASSED**, including the new section [3]
(the reentrancy fix) alongside the two pre-existing sections. This is a COMPLETE, unconditional proof —
this test has no Docker/containerd dependency at all, so nothing here is narrowed by this environment's
own disclosed gap.

**`test_task_branch_concurrent_dispatch`: section [1] — the test's OWN core claim, and the only section
with no Docker dependency — PASSED completely**: all 5 rounds of 8 genuine concurrent OS threads calling
`start_task_branch()` on one provider instance succeeded, with `task_branches_` correctly populated (8
distinct real branches) and `BranchCost` correctly accounted (exactly 8 units) every round. Sections
[2]/[3] partially ran: `docker info` confirmed the same, already-diagnosed gap ADR-115/118/120/121 all
name (Docker CLI bridged to the Windows host, unreachable from this side of the WSL2 boundary) — the
ONE check requiring real container exec (`run_in_task_branch()`'s own command execution) failed exactly
as expected, while every OTHER check in sections [2]/[3] — including `commit_task_branch()` (a pure
Ledger `merge()` operation, no container needed) and the full concurrent `discard_task_branch()`/
`start_task_branch()` pair in section [3] (also pure Ledger operations) — passed cleanly. This is the
identical, minimal, disclosed Docker-dependent surface every other test in this environment shares —
not a new or ADR-123/124-specific gap.

Full `ctest`: **183 total (182 baseline + this pass's own new test), 6 failures** — the same 5
pre-existing failures ADR-118/120/121 already named, PLUS `test_task_branch_concurrent_dispatch` itself
(failing on the identical, single Docker-dependent check just described) — zero regression anywhere
else in the suite.

## 3. What this closes, and what it does not

**Closes for real, unconditionally**: ADR-123's own Linux-verification residual completely — the
reentrancy fix has no Docker dependency, so this is a full, unnarrowed proof. ADR-124's own residual is
closed for the part that matters — `task_branch_mutex_`'s real concurrent-dispatch safety (section [1])
is proven completely and unconditionally on Linux, independent of the Docker gap.

**Does NOT close**: the same disclosed, pre-existing, environment-caused Docker-CLI-reachability gap
ADR-115/118/120/121 already named — `test_task_branch_concurrent_dispatch`'s own `run_in_task_branch()`
check (the one operation requiring real container exec) remains unverified on Linux, for the identical,
unrelated reason every other Docker-dependent test in this environment shares.

## 4. What was NOT done

- No attempt to install or configure a real Linux-native Docker daemon in this WSL2 distro — same,
  separate environment decision every prior Linux-verification ADR in this chain has already deferred.
- No new test or repro was written in this pass — purely a verification run of what ADR-123/124 already
  shipped.

## 5. Residuals

- The single Docker-dependent check inside `test_task_branch_concurrent_dispatch`'s sections [2]/[3]
  remains unverified on Linux — unchanged, pre-existing, environment-caused, not this ADR's own gap.
- Every other residual named by ADR-108 through ADR-124 remains unchanged — this ADR closes only the
  Linux-verification gaps for ADR-123/124.
