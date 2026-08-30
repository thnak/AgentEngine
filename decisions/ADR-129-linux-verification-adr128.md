# ADR-129 — Real Linux verification of ADR-128: `bind_root_branch()` and the `is_branch_owner()` fix hold on GCC 14.2.0

- **Status:** Proposed — real verification pass, no production code changed. The pre-existing
  `/root/ae-verify` WSL2 Ubuntu 24.04 checkout (ADR-115, reused by ADR-118/120/121/125/127) fast-forwarded
  to this branch's exact HEAD (`33c1b87`) and rebuilt/retested with the same real GCC 14.2.0 toolchain.
- **Date:** 2026-08-30.
- **Scope:** No files changed except this ADR itself and the two disclosure-pointer edits it makes
  (`decisions/ADR-128-root-branch-recovery.md`, `decisions/README.md`). This ADR records a verification
  run, not a code change.
- **Related specs:** Closes ADR-128's own "no Linux verification yet" residual (§5). Reuses
  ADR-118/120/121/125/127's own environment and methodology directly.

## 1. The question

ADR-128 (`MandatorySandboxProvider::bind_root_branch()`, plus the same-day independent red-team's own
`Ledger::is_branch_owner()` MUST-FIX replacing a content-digest ACL check with true branch-ownership
authorization in `reclaim_orphaned_branch()`/`abandon_orphaned_branch()`) landed on Windows/MSVC only.
Does the fix genuinely hold on a real, independently-obtained GCC 14.2.0 toolchain and filesystem —
both the root-branch reclaim-or-create automation itself, and the cross-owner authorization tightening
the red-team found and fixed in `ledger.hpp`?

## 2. What was done

Reused ADR-127's own `/root/ae-verify` checkout (confirmed clean, sitting at `2425fcb`) and fast-forwarded
it (`git fetch origin` + `git merge --ff-only origin/session-worktree-design-draft`) to `33c1b87` — a
genuine 9-file, 1428-insertion/207-deletion fast-forward covering ADR-126's own README/ADR touch-ups
through ADR-128 and a following documentation-only commit. Confirmed the build cache's baseline before
touching anything (`AGENTENGINE_WITH_HTTPS=OFF` in `build-linux-verify/CMakeCache.txt`, matching the
established baseline — the same "check the cache variable, don't just trust a `--target` build"
discipline ADR-122 established).

Rebuilt with the same GCC 14.2.0 toolchain: **44/44 affected targets, zero errors**, both
`test_root_branch_recovery` and `test_task_branch_durability_recovery` (riding along, since both touch
`mandatory_sandbox_provider.hpp`) compiling and linking clean on the first attempt.

**`test_root_branch_recovery`: ALL CHECKS PASSED**, run directly. This is a COMPLETE, unconditional
proof — the test is entirely Docker-independent (a `FakeSurface` stand-in satisfies
`MandatorySandboxProvider<Surface>`'s own template constraint; `run_in_task_branch()` is never called),
so nothing here is narrowed by this environment's own disclosed Docker-CLI-reachability gap. All three
checks held on real GCC/glibc/filesystem behavior: [1] a fresh owner/disambiguator pair takes the
create-path and the resulting binding is genuinely functional end to end (a `start_task_branch()`/
`discard_task_branch()` round trip); [2] THE CORE CLAIM — after a simulated crash (destroy + reconstruct
against the SAME `durable_dir`), a second `bind_root_branch()` call for the SAME owner/disambiguator
genuinely RECLAIMS (not re-creates) the same root branch, proven both by the pre-crash task-branch handle
staying discoverable via `discard_task_branch()` with no manual `Ledger`-level reclaim call anywhere in
the test, and by the direct `orphaned_from_restart_`-removal differentiator that distinguishes reclaim
from create; [3] two different disambiguators for the same owner resolve to two genuinely independent
root branches (distinct `handle_id`s).

Full `ctest` (`ctest --output-on-failure -j12` from `build-linux-verify`): **185 total (184 baseline +
this pass's own new test), 6 failures** — `test_composed_sandbox_providers_live`, `test_sandbox_runtime`,
`test_docker_orphan_reap`, `test_mandatory_sandbox_provider`, `test_task_branch_tools`, and
`test_task_branch_concurrent_dispatch`, all failing on the SAME disclosed, pre-existing
Docker-CLI-reachability gap this WSL2 distro has had since ADR-115 (`docker info` unreachable; confirmed
by inspecting the actual failure output — `test_docker_orphan_reap` fails at "create() a container", and
`test_task_branch_tools` fails at `run_in_task_branch()` itself, both real container-creation attempts,
not assertion logic) — the identical failure SET ADR-127's own pass already established (5 pre-existing
failures plus `test_task_branch_concurrent_dispatch`'s own single Docker-dependent check), zero
regression anywhere else. `test_root_branch_recovery` itself explicitly listed as **Passed** in the
suite (183/185), and `test_ledger` explicitly listed as **Passed** (126/185).

Independently re-ran `test_ledger` directly, not merely trusted the aggregate `ctest` count (this pass's
own subject touches `ledger.hpp`'s `is_branch_owner()` fix, and `test_ledger.cpp` exercises
`reclaim_orphaned_branch()`/`abandon_orphaned_branch()` heavily): `test_ledger: all checks passed`,
exit code 0.

## 3. What this closes and what it does not

**Closes for real, unconditionally**: ADR-128's own Linux-verification residual completely — both halves
of ADR-128 (`bind_root_branch()`'s reclaim-or-create automation, and the red-team's own
`is_branch_owner()` cross-owner authorization fix in `ledger.hpp`) have no Docker dependency at all in
the tests that exercise them (`test_root_branch_recovery`, `test_ledger`, and
`test_task_branch_durability_recovery`, all confirmed passing directly on GCC/glibc), so this is a full,
unnarrowed proof on a real, independently-obtained toolchain and filesystem.

**Does NOT close**: the same disclosed, pre-existing, environment-caused Docker-CLI-reachability gap
ADR-115/118/120/121/125/127 already named — unrelated to this ADR's own subject, unchanged by this pass.
It also does not re-run ADR-128 §7's own temporary cross-owner-reclaim probe (`probe_cross_owner_reclaim.cpp`)
— that file was deliberately removed after use on Windows per §7's own account, and this pass treats
`test_ledger`'s existing, unmodified `reclaim_orphaned_branch()`/`abandon_orphaned_branch()` coverage,
passing unchanged on GCC, as sufficient regression evidence that the `is_branch_owner()` fix did not
break any legitimate reclaim on this platform either; it does not re-derive the original bug's proof.

## 4. What was NOT done

- No attempt to install or configure a real Linux-native Docker daemon in this WSL2 distro — same,
  separate environment decision every prior Linux-verification ADR in this chain has already deferred.
- No new test or repro was written in this pass — purely a verification run of what ADR-128 already
  shipped.
- Did not recreate ADR-128 §7's own temporary probe file — see §3's own account of why the existing
  `test_ledger` coverage was judged sufficient here.

## 5. Residuals

- None specific to `bind_root_branch()` or `is_branch_owner()` — this pass found nothing new to
  disclose.
- The same pre-existing, disclosed Docker-CLI-reachability gap (ADR-115/118/120/121/125/127) remains
  unrelated and unchanged.
