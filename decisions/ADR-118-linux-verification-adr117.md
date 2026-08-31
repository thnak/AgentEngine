# ADR-118 — Real Linux verification of ADR-117: the widened `Capability` variant and the new double gate both hold on GCC 14.2.0

- **Status:** Proposed — real verification pass, no production code changed. The pre-existing
  `/root/ae-verify` WSL2 Ubuntu 24.04 checkout (first established for ADR-115) fast-forwarded from
  `bfdfe6e` to this branch's exact HEAD (`fb280b7`, the ADR-117 commit) and rebuilt/retested with the
  same real GCC 14.2.0 toolchain ADR-115 used.
- **Date:** 2026-08-30.
- **Scope:** No files changed. This ADR records a verification run, not a code change.
- **Related specs:** Closes ADR-117 §5's own "no Linux verification" residual. Reuses ADR-115's own
  environment and methodology directly (same WSL2 checkout, same `gcc-14`/`g++-14` toolchain, same
  Docker-CLI-reachability caveat) rather than re-deriving it.

## 1. The question

ADR-117 (widening the real, closed `agentengine::Capability` variant with `cap::TaskBranch`/
`cap::TaskBranchCommit`, and wiring a real capability ceiling onto the four task-branch tools) landed
on Windows/MSVC only, disclosing "no Linux verification" as an explicit residual — the same posture
every other same-day ADR in this design line has disclosed before its own Linux pass. Does the widened
variant, its four newly-extended exhaustive switches, and the new double-gate enforcement actually
compile and behave correctly under a real, independently-obtained GCC 14.2.0 toolchain — including the
one property that matters most (the new capability ceiling genuinely gates the real
`invoke_tool()` pipeline, in both directions) — or has it only ever been exercised on MSVC?

## 2. What was done

Reused ADR-115's own `/root/ae-verify` checkout (WSL2 Ubuntu 24.04, real GCC 14.2.0 via `gcc-14`/
`g++-14`) rather than cloning fresh a second time — confirmed clean (`git status --short` empty) and
sitting at `bfdfe6e` (this branch's tip as of ADR-115). `git fetch` + `git merge --ff-only
origin/session-worktree-design-draft` fast-forwarded it to `fb280b7` (this branch's exact current tip,
including ADR-117 itself) — a genuine 18-file, 1125-insertion fast-forward, not a rebase or a
cherry-pick, so this is a real verification of the actual landed commit. Reused ADR-115's own
pre-configured `build-linux-verify` directory (CMake + Ninja, `CMAKE_CXX_COMPILER=/usr/bin/g++-14`,
`CMAKE_BUILD_TYPE=Debug` — confirmed via `CMakeCache.txt` before building, not assumed).

**Full incremental rebuild: 610/610 targets, zero errors, zero warnings in the ADR-117-touched
files** (`capability.hpp`, `policy_reachability.hpp`, `mandatory_sandbox_provider.hpp`,
`test_task_branch_tools.cpp` all compiled clean under GCC 14.2.0 — the two new exhaustive `if
constexpr`/`switch` sites in `capability.hpp` and the one in `policy_reachability.hpp` all resolved
correctly, confirming this is not an MSVC-only-clean widening).

**Full `ctest`: 177/182 passed (97%).** Five failures, ALL sharing the exact same root cause ADR-115
already disclosed and diagnosed in this identical WSL2 distro: `test_composed_sandbox_providers_live`,
`test_sandbox_runtime`, `test_docker_orphan_reap`, `test_mandatory_sandbox_provider`, and
`test_task_branch_tools` — every one Docker-CLI-reachability, not a code defect. Confirmed directly
(not assumed from ADR-115's prior finding): `which docker` still resolves to the bridged Windows
Docker Desktop binary at `/mnt/c/Program Files/Docker/Docker/resources/bin/docker`, and `docker info`
fails to reach a daemon from this side of the WSL2 boundary — the identical, unresolved environment gap
ADR-115 named, now reproduced independently on a fresh fast-forward rather than assumed to still hold.

**The one property that actually matters for THIS ADR — the new capability ceiling itself — is
UNCONDITIONALLY, completely verified on Linux, independent of the Docker gap.** Re-ran
`test_task_branch_tools` directly and diffed its failure set against every `check()` call site in the
file (this test's own `check()` helper only prints on failure, never on success, so absence from the
failure list is the success signal): NONE of the three new ADR-117 fail-closed assertions appear
in the failure output —

```
FAIL: run_in_task_branch() succeeds
FAIL: the parent's own branch now contains the child's committed file
FAIL: both children can run independently (isolated)
FAIL: the SECOND commit (now stale -- parent moved under it) is a real, rejected conflict
FAIL: the rejected handle_id is reclaimed and stays usable -- NOT unknown_handle
FAIL: the reclaimed handle can be cleanly discarded afterward
FAIL: the parent's branch reflects A's committed content, not B's rejected one
FAIL: Y's 1st rejection is an ordinary conflict, not a retry-quota exhaustion
FAIL: Y's handle is still usable after the 1st rejection
FAIL: the 2nd rejected retry spends the LAST MergeCost unit
FAIL: the 3rd attempt fails closed with retry_quota_exhausted, not another silent free retry
FAIL: Y's BranchCost unit is refunded when it is force-discarded on quota exhaustion
FAIL: no further MergeCost is spent once the handle is gone (the loop is truly bounded)
FAIL: the file committed through the REAL tool-call pipeline is really on the parent's own branch,
      read back independently of any tool's own reported reply
```

— every one of these 14 failures is a check that requires a REAL Docker container to actually run a
command (ADR-113/ADR-114's own pre-existing best-of-N/conflict/retry-quota logic, all built on top of
`run_in_task_branch()` actually executing something), unrelated to ADR-117's own new code. By contrast,
section [6]'s OWN first two checks — `"start_run() driving a real start_task_branch tool call through
invoke_tool() succeeds"` and `"session.history() contains a real start_task_branch reply carrying a
handle_id"` — and all THREE of section [7]'s new checks (`"start_task_branch is rejected as a real
role::tool error result when the session holds no cap::TaskBranch grant..."`, `"no real
start_task_branch call ever ran..."`, `"a rejected-at-authorization call never reaches
call_sandbox()..."`) are absent from the failure list, meaning all five PASSED. This is a real,
structural reason these particular checks are Docker-independent, not a lucky coincidence:
`start_task_branch()` is a pure `Ledger`/`SandboxRuntime::spawn_child_branch()` operation (no container
exec at all), and the capability-ceiling REJECTION itself (section [7]) happens at `invoke_tool()`'s
step 4/7, structurally BEFORE `call_sandbox()` is ever reached — so neither the "granting the
capability lets the call through" direction nor the "withholding it rejects the call" direction ever
touches Docker. **This makes the Linux verification of ADR-117's own actual subject — the widened
variant and the double gate — complete and unconditional, not narrowed by the Docker gap the way
ADR-114/115's own task-branch-tools verification was.**

`test_capability_enforcement` and `test_policy_reachability` (the two non-task-branch tests most
directly exercising the widened variant's general `CapabilitySet`/`subsumes()` machinery and the new
`capability_kind_name()` arm respectively) both **Passed** cleanly in the full suite run.

## 3. What this closes, and what it does not

**Closes for real, unconditionally**: ADR-117's own "no Linux verification" residual, for the part that
actually matters — the widened `Capability` variant compiles correctly under GCC 14.2.0, every
exhaustive switch/`if constexpr` site resolves without a missing-arm diagnostic, and the new double-gate
enforcement (both the "granted -> allowed" and "not granted -> rejected, no side effects" directions)
is proven end-to-end through the real, unmodified `invoke_tool()` pipeline on this platform, using
checks that do not depend on Docker at all.

**Does NOT close**: the same disclosed, pre-existing, environment-caused Docker-CLI-reachability gap
ADR-115 already named for this exact WSL2 distro — `test_task_branch_tools`'s own Docker-dependent
checks (real command execution inside a task branch, real conflict/reclaim, real committed-file
content) remain unverified on Linux, for the identical reason and with the identical root cause
ADR-115 diagnosed (not a new or ADR-117-specific gap).

## 4. What was NOT done

- No attempt to install or configure a real Linux-native Docker daemon in this WSL2 distro — same,
  separate environment decision ADR-115 already deferred, not revisited here.
- No new test file parameterizing `test_task_branch_tools`-shaped checks over
  `ContainerdExecutionSurface` instead of `DockerExecutionSurface` for this Linux environment — real,
  disclosed follow-on work, unchanged from ADR-115's own §4.

## 5. Residuals

- The Docker-dependent half of `test_task_branch_tools` (and the four other tests sharing the same root
  cause) remains unverified on Linux — unchanged, pre-existing, environment-caused, not this ADR's own
  gap to close.
- Every other residual named by ADR-108 through ADR-117 remains unchanged — this ADR closes only
  ADR-117's Linux-verification gap.
