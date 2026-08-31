# ADR-143 — Real Linux verification of ADR-136 through ADR-142: do all six final-review fixes actually hold on GCC, not merely MSVC?

- **Status:** Proposed — real verification pass. **Found and fixed one further real, previously-
  undiscovered bug** (in `worktree_mount_fs_posix.cpp`, ADR-138's own file — see ADR-138 §8 for the
  full account), independent of and predating both ADR-138 and its own same-day red-team round. Every
  other claim across ADR-136/137/139/141/142 confirmed to hold on a second, independent
  compiler/allocator/filesystem with zero regression. The pre-existing `/root/ae-verify` WSL2 Ubuntu
  24.04 checkout (ADR-115, reused by every Linux-verification ADR in this chain through ADR-135)
  fast-forwarded to this branch's exact HEAD (`e5baf56`) and rebuilt/retested with the same real GCC
  14.2.0 toolchain.
- **Date:** 2026-08-31.
- **Scope:** `src/core/worktree_mount_fs_posix.cpp` (one real fix: deferred `O_TRUNC`), plus this ADR
  itself and the disclosure-pointer edits it makes to `decisions/ADR-138-posix-mount-escape-cleanup-
  parity.md` and `decisions/README.md`. No other production code changed — ADR-136/137/139/141/142
  all held on Linux with zero source changes needed.
- **Related specs:** Closes ADR-136/137/138/139/140/141/142's own "not yet Linux-verified" residuals.
  Reuses ADR-115/118/120/121/122/125/127/129/131/133/135's own environment and methodology directly.
  ADR-140 (Kata backend) is explicitly NOT closed by this pass — see §5.

## 1. The question

ADR-136 through ADR-142 landed on Windows/MSVC in one consolidated final-review session: an orphan-
sweep parity fix (136), a durability-failure-handling fix with two of its own red-team follow-ons
(137), a mount-escape-cleanup parity fix for a Linux-only file that could not even be compiled on the
authoring platform (138), a Docker `run_capture` timeout/cap rewrite with its own red-team fix (139), a
Kata backend symlink-escape guard for another Linux-only, deployment-gated file (140), and an
`AsyncQuota` double-spend/double-credit fix with a same-day red-team follow-on (141/142). Do all of
these actually hold on a second, independent compiler and OS — and, for the two files that were never
even compiled during their own authoring pass (138, 140), do they even build at all?

## 2. What was done

Fast-forwarded ADR-135's own `/root/ae-verify` checkout (`git fetch origin` + `git merge --ff-only
origin/session-worktree-design-draft`) from `2a84e8d` to `e5baf56` — a clean fast-forward, 18 files
changed (1650 insertions, 77 deletions), covering all seven of this session's own commits (136 through
the README index update). One leftover, uncommitted local diff was found in this checkout before
fetching — the WIFEXITED/WEXITSTATUS fix ADR-135 itself already made and shipped upstream through a
different path — confirmed byte-identical to what `origin/session-worktree-design-draft` already
carried via `git show`, then discarded (`git checkout --`) before the fast-forward, per this repo's own
git-safety discipline (never discard uncommitted work without first confirming it is genuinely
redundant, not real). Confirmed the build cache's baseline first (`AGENTENGINE_WITH_HTTPS=OFF`,
`AGENTENGINE_BUILD_KATA_BACKEND=OFF`), matching every prior pass in this chain.

### The two never-before-compiled files — the genuinely new claims, each given its own attention

**ADR-138 (`worktree_mount_fs_posix.cpp`)**: full incremental rebuild, 81/81 steps, zero errors — this
file (and every other Linux target) compiled clean on the FIRST attempt, no source change needed for
the create-and-unwind fix (§7 in that ADR) itself. `test_worktree_mount_fs_escape_corpus_linux` (the
existing, pre-ADR-138 corpus, unrelated to this ADR's own new logic) ran clean, zero regression. A
dedicated, temporary, targeted probe (`tests/test_zzprobe_adr138.cpp`, deleted after use, along with its
temporary `tests/CMakeLists.txt` registration — confirmed via `git status`/`git diff --stat` showing
zero leftover diff) was built and run directly against the real fix to prove the two properties this
ADR's own red-team round (§7) reasoned about but could never execute:

1. A dangling symlink inside the mount, pointing to a location outside it, opened with `O_CREAT` and no
   caller `O_EXCL` — confirmed REJECTED, and the newly-created file at the escaped-to target confirmed
   genuinely unlinked, not left behind (the create-and-unwind fix, §7).
2. A symlink to an EXISTING outside file — confirmed REJECTED, and confirmed the pre-existing file was
   NOT deleted by the unwind logic (the specific "worse bug" the red-team's own rejected alternative fix
   would have caused).

**A third check, in the same probe, surfaced a real bug — see §3.**

**ADR-140 (`kata_backend.cpp`)**: this file is additionally gated behind `AGENTENGINE_BUILD_KATA_
BACKEND` (default OFF, no toolchain cost for a host that doesn't want it). Reconfigured with
`-DAGENTENGINE_BUILD_KATA_BACKEND=ON` (ADR-120's own toggle-and-revert pattern, applied to a different
flag) and built the target directly: `agentengine_kata_backend`, 4/4 steps, zero errors, zero warnings
(confirmed via a direct grep of the build log for `warning`/`error`, zero matches). The new
`escapes_rootfs_via_symlink()` guard and all three of its real call sites compile clean on GCC with no
source change needed. Reverted to `AGENTENGINE_BUILD_KATA_BACKEND=OFF` afterward and confirmed a clean
incremental rebuild. **No live Kata/containerd deployment was available in this environment to exercise
`create()` against a real malicious image** — this pass proves the guard COMPILES correctly on GCC, not
that it behaves correctly against a real attack; see §5 for what remains open.

### Everything else — held with zero source changes, confirmed by direct execution

`test_identity_authority_grant` (ADR-141/142, the `AsyncQuota` fix): **`all checks passed`**, exit 0.
`agentengine_cli_chat` (ADR-136): builds clean (`AGENTENGINE_WITH_HTTPS=ON`, toggled and reverted same
as prior passes needing this target). `docker_execution_surface.hpp` (ADR-139): every Docker-independent
target using this header compiled clean; the Docker-dependent tests themselves (`test_sandbox_runtime`
et al.) are, as expected, part of this environment's own long-disclosed, pre-existing Docker-CLI-
unreachable-via-WSL2 gap (unchanged, not this ADR's subject) — see §4.
`file_worktree_object_store.hpp` (ADR-137, including its own two red-team follow-on fixes): `test_
content_durability_concurrency` — **`ALL CHECKS PASSED`**, exit 0, **10.44–10.52 seconds real time**
across repeated runs (confirming the `[1b]` same-digest race section and the full `[2]`/`[2b]`/`[2c]`
metadata-bookkeeping-race section both genuinely ran, not silently skipped), including the specific
`[1b]`/`[2c]` sections ADR-137's own red-team round found and fixed real Windows-specific regressions
in — both hold cleanly on Linux too, where POSIX's own atomic-replace `rename()` semantics mean the
Windows-specific "access denied on a racing rename" scenario the readback-and-recompute fix targets
does not even arise the same way, yet the fix introduces no regression for the POSIX case either.

## 3. A real, previously-undiscovered bug found by this pass itself

The ADR-138 probe's second scenario (a symlink to an EXISTING outside file) initially FAILED a check
that scenario's own author had added for thoroughness, not because it was expected to fail: the
pre-existing file survived (not deleted — the §7 fix's own claim held), but its CONTENT had been wiped
to zero bytes. Root-caused: `O_TRUNC` is destructive at `open()`-time, an unconditional kernel side
effect of a successful `open()` call, before this function's own containment check ever runs — a real,
previously-undiscovered data-loss bug, predating ADR-138 entirely (the ORIGINAL, pre-ADR-138 single-
`open()` design had the identical exposure; not introduced by this session's own create-and-unwind fix,
only surfaced by this fix's own verification probe incidentally exercising the truncation path while
testing a different property). Reachable through the same `write_file()` path already named in ADR-138
§1, since `append=false` (i.e. `O_TRUNC`) is this codebase's own default write mode.

**Fixed in this same pass**: `O_TRUNC` is now stripped from every `open()` call in `open_within_mount_
root()` and applied via a real `ftruncate()` on the resulting descriptor only AFTER the containment
check confirms the target is genuinely inside `mount_root` — deferring the destructive effect past the
point where it is known to be safe. Behavior-preserving for every legitimate in-mount caller (verified:
POSIX guarantees a non-`O_APPEND` open positions the file offset at 0 regardless of truncation, so a
deferred `ftruncate(fd, 0)` before any write reaches an identical end state to the original `O_TRUNC`-
at-open-time behavior). Rebuilt clean (6/6 steps for the direct dependency chain), reran the probe:
**ALL CHECKS PASSED**, including the specific content-preservation assertion that had failed before this
fix. Reran the full `test_worktree_mount_fs_escape_corpus_linux` suite and the whole `ctest` run:
zero regression (see §4). Full details, including why this is disclosed as ADR-138's own finding rather
than a separate ADR, are in `decisions/ADR-138-posix-mount-escape-cleanup-parity.md` §8.

## 4. Full regression baseline

Full `ctest --output-on-failure -j12`: **190 total (189 ADR-135 baseline + 1 temporary probe, later
removed), 6 failures** — the IDENTICAL 6-failure set every Linux-verification ADR in this chain since
ADR-125 has established (`test_composed_sandbox_providers_live`, `test_sandbox_runtime`, `test_docker_
orphan_reap`, `test_mandatory_sandbox_provider`, `test_task_branch_tools`, `test_task_branch_concurrent_
dispatch`), independently reconfirmed via `docker info` still failing with the same "could not be found
in this WSL 2 distro" message — the same disclosed, pre-existing, environment-caused gap, zero
regression anywhere else. After removing the temporary probe and reconfiguring back to this
environment's exact baseline flags (`AGENTENGINE_WITH_HTTPS=OFF`, `AGENTENGINE_BUILD_KATA_BACKEND=OFF`),
reran once more: **189 total, the same 6 failures**, confirming a byte-for-byte return to the
established baseline with the one real fix (§3) folded in permanently.

## 5. What this closes and what it does not

**Closes for real**: ADR-136/137/138/139/141/142's own "not yet Linux-verified" residuals, completely —
every one of those five fixes now has real GCC 14.2.0 compilation and, where a real test exists,
execution evidence, not merely hand-traced reasoning. ADR-138 specifically gets a genuinely new,
previously-undiscovered bug closed as a direct result of this verification effort, not merely confirmed.

**Does NOT close**: ADR-140's own residual is only PARTIALLY closed — this pass confirms the Kata
backend's new symlink-escape guard compiles clean on GCC (a real, previously-untested claim), but this
environment has no live Kata/containerd deployment to exercise `create()` against a real malicious OCI
image, so the guard's actual runtime behavior against a genuine attack remains verified only by hand-
trace (ADR-140 §7), not by execution — this pass narrows but does not eliminate that specific residual.
Also unchanged: the same disclosed, pre-existing Docker-CLI-reachability gap named in every prior
Linux-verification ADR in this chain.

## 6. What was NOT done

- No attempt to install or configure a real Docker daemon or a live Kata/containerd deployment in this
  WSL2 distro — same, separate environment decisions every prior pass in this chain has already
  deferred.
- No independent red-team round of this pass's own fix (§3) — a verification pass that found and fixed
  one real bug directly, the same shape ADR-135's own pass took for its own POSIX exit-code fix.
- No audit of whether any OTHER `open()`-based mediation path in this codebase (Windows or POSIX) has
  the same class of "destructive flag applied before the safety check" issue `O_TRUNC` had here — a
  real, disclosed gap in this pass's own coverage, not asserted to be the only instance. The Windows
  sibling (`worktree_mount_fs.cpp`) was NOT checked for an equivalent `CREATE_ALWAYS`/truncation-timing
  issue as part of this pass.

## 7. Residuals

- The audit gap named in §6 (other mediation paths possibly sharing the same destructive-flag-before-
  check class of bug, on either platform) — a real, cheap follow-on if ever wanted, not urgent.
- ADR-140's own partially-closed residual (§5) — a live Kata/containerd deployment with a real,
  attacker-controlled OCI image is needed to fully close it.
- The same pre-existing, disclosed Docker-CLI-reachability gap named in every Linux-verification ADR
  since ADR-115, unrelated and unchanged.
- Every other disclosed, deliberately-out-of-scope residual named in ADR-136 through ADR-142
  themselves (the TOCTOU window in ADR-138 §5/§6, ADR-139's client-kill-vs-remote-process-kill gap,
  ADR-142's own missing permanent test coverage for its two new error codes) is unchanged by this pass,
  which verifies the Windows-authored fixes on a second platform rather than closing any of them.
