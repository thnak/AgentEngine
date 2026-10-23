# ADR-127 — Real Linux verification of ADR-126: the `active_`/`task_branches_` durability recovery holds on GCC 14.2.0

- **Status:** Proposed — real verification pass, no production code changed. The pre-existing
  `/root/ae-verify` WSL2 Ubuntu 24.04 checkout (ADR-115, reused by ADR-118/120/121/125) fast-forwarded
  to this branch's exact HEAD (`2425fcb`, the ADR-126 red-team-closure commit) and rebuilt/retested
  with the same real GCC 14.2.0 toolchain.
- **Date:** 2026-08-30.
- **Scope:** No files changed. This ADR records a verification run, not a code change.
- **Related specs:** Closes ADR-126's own "not yet Linux-verified" residual. Reuses ADR-118/120/121/125's
  own environment and methodology directly.

## 1. The question

ADR-126 (the `MandatorySandboxProvider::bind_sandbox()` durability-recovery fix, including the same-day
red-team's own grandchild-exclusion MUST-FIX) landed on Windows/MSVC only. Does recovery genuinely hold
on a real, independently-obtained GCC 14.2.0 toolchain and filesystem — including the red-team's own
adversarial grandchild-orphan regression case?

## 2. What was done

Reused ADR-125's own `/root/ae-verify` checkout (confirmed clean, sitting at `8ee9eef`) and
fast-forwarded it (`git fetch` + `git merge --ff-only`) to `2425fcb` — a genuine 9-file, 845-insertion
fast-forward covering ADR-125 through ADR-126's own red-team follow-on. Rebuilt with the same GCC 14.2.0
toolchain (`AGENTENGINE_WITH_HTTPS=OFF`, matching the established baseline): **41/41 affected targets,
zero errors**, `test_task_branch_durability_recovery` compiling and linking clean on the first attempt.

**`test_task_branch_durability_recovery`: ALL CHECKS PASSED**, run directly. This is a COMPLETE,
unconditional proof — the test is entirely Docker-independent (a `FakeSurface` stand-in satisfies
`MandatorySandboxProvider<Surface>`'s own template constraint; `run_in_task_branch()` is never called),
so nothing here is narrowed by this environment's own disclosed Docker-CLI-reachability gap. Every
phase passed on real GCC/glibc/filesystem behavior: the "destroy + reconstruct against the SAME
`durable_dir`" crash simulation, recovery of two genuine orphaned child branches, `discard_task_branch()`
on a recovered handle succeeding through the normal tool surface, `commit_task_branch()` on a second
recovered handle correctly reaching real merge logic and failing on the precise, already-disclosed
content-durability gap (not `task_branch_unknown_handle`), an ordinary non-recovered discard's own
`BranchCost` refund, and — the same-day red-team's own MUST-FIX regression — a genuine grandchild
orphan (constructed via direct `Ledger::branch_from()` calls, bypassing the tool surface) correctly
staying an unrecovered orphan while a true direct child recovers normally.

Full `ctest`: **184 total (183 baseline + this pass's own new test), 6 failures** — the identical 6
failures ADR-125's own pass already established (5 pre-existing Docker-CLI-reachability failures plus
`test_task_branch_concurrent_dispatch`'s own single expected Docker-dependent check) — zero regression
anywhere else, and `test_task_branch_durability_recovery` itself explicitly listed as **Passed**.

## 3. What this closes, and what it does not

**Closes for real, unconditionally**: ADR-126's own Linux-verification residual completely — the
durability-recovery fix, including the red-team's own grandchild-exclusion guard, has no Docker
dependency at all, so this is a full, unnarrowed proof on a real, independently-obtained toolchain and
filesystem.

**Does NOT close**: the same disclosed, pre-existing, environment-caused Docker-CLI-reachability gap
ADR-115/118/120/121/125 already named — unrelated to this ADR's own subject, unchanged by this pass.

## 4. What was NOT done

- No attempt to install or configure a real Linux-native Docker daemon in this WSL2 distro — same,
  separate environment decision every prior Linux-verification ADR in this chain has already deferred.
- No new test or repro was written in this pass — purely a verification run of what ADR-126 already
  shipped.

## 5. Residuals

- None specific to `MandatorySandboxProvider`'s durability recovery — this pass found nothing new to
  disclose.
- The same pre-existing, disclosed Docker-CLI-reachability gap (ADR-115/118/120/121/125) remains
  unrelated and unchanged.
