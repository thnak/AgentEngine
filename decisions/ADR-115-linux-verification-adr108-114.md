# ADR-115 — Real Linux verification of ADR-108–114: full build clean, `Ledger::merge()` gate fully proven, containerd path re-confirmed

- **Status:** Proposed — real verification pass, no production code changed. A fresh `git clone` of
  this branch's exact HEAD (`bfdfe6e`) built and tested against real GCC 14.2.0 on a genuine Ubuntu
  24.04 WSL2 instance, entirely separate from the environment every prior Windows/MSVC verification in
  this branch's history used.
- **Date:** 2026-08-30.
- **Scope:** No files changed. This ADR records a verification run, not a code change — the "what was
  built" section of ADR-108 through ADR-114 is unaffected.
- **Related specs:** Closes the "no Linux verification" residual named by ADR-109 §5, ADR-111 §7,
  ADR-112 §5, and ADR-114 §5/§7 (all four). Also re-confirms ADR-106/108's own containerd/`reap_orphans()`
  work against a genuinely fresh environment, independent of whatever state any prior WSL2 session
  left behind.

## 1. The question

Every ADR from ADR-109 through ADR-114 landed on Windows/MSVC only, each disclosing "no Linux
verification" as an explicit residual. This branch's own prior Linux work (ADR-103 through ADR-110)
used a WSL2 environment that accumulated real, working state over many sessions — but that state's
exact provenance was unclear, and it was not a proper `git`-tracked checkout of this branch (verified:
`git status` inside the pre-existing `/root/ae` directory in the `Ubuntu-24.04` WSL2 distro returned
"not a git repository"). Does this branch's newest work (ADR-108 through ADR-114, including the
same-day ADR-114 follow-on fixes) actually hold up on a real, independently-obtained Linux checkout —
or has it only ever been exercised on Windows?

## 2. What was done

A genuinely fresh `git clone --branch session-worktree-design-draft` of the real GitHub remote
(`https://github.com/thnak/AgentEngine`) into a new directory (`/root/ae-verify`), never touching the
pre-existing, ambiguous-provenance `/root/ae` — landed at this branch's exact tip (`bfdfe6e`, the
ADR-114 follow-on commit), confirmed via `git log`. Configured with real GCC 14.2.0 (`gcc-14`/`g++-14`
were already installed alongside a default `gcc`/`g++` 13.3.0 that fails this project's own `021 §5`
compiler-floor gate — a real, useful finding in itself: the default toolchain in this distro is below
this project's stated floor, only a non-default, explicitly-invoked compiler satisfies it) via CMake +
Ninja.

**Full project build: 764/764 targets, zero errors**, the first time this exact branch tip has been
built end-to-end on Linux at all (every prior Linux verification in this branch's history was of an
earlier, narrower slice — ADR-103 through ADR-110's own targets, before ADR-111 through ADR-114
existed).

**`test_ledger`: ALL CHECKS PASSED**, no daemon dependency at all — this is the direct, complete
closure of ADR-111's `AsyncQuota<MergeCost>` gate and ADR-112's blob-content ACL grant on real Linux,
including case [12]'s quota-exhaustion/orphan-reclaim proof and the MUST-FIX regression case [4]/[5]
both ADRs added.

**`test_containerd_execution_surface`: 31/31 checks passed**, against a REAL, already-running
`containerd` daemon in this WSL2 distro (root, confirmed via `ps aux`) — including the FULL
ADR-108 `reap_orphans()` positive/negative-control suite (a matching-start-key container survives, a
wrong-start-key-but-live-pid container is correctly reaped — the exact pid-reuse fix ADR-108's own
follow-on added — a confirmed-dead-pid container is reaped, a non-prefixed container is untouched).
The only reason this didn't pass on the first attempt was a missing local image cache (`ctr images
pull docker.io/library/alpine:latest` — 8.6s, confirming outbound network access works fine) —
environment state, not a code defect; re-run afterward, clean 31/31.

**`test_mandatory_sandbox_provider_composed`: ALL CHECKS PASSED**, no daemon needed (its own file-top
comment: proves declaration-time composition against a fake surface, not real execution) — this
DOES exercise this session's own ADR-114-follow-on `discard_all_active_task_branches_and_refund()`
code path indirectly (every `bind_sandbox()`/`fork_from()` call in this test now runs through the
patched code), with zero regressions.

**7 real, expected, environment-caused failures — all Docker-CLI-reachability, not code defects**:
`test_sandbox_runtime`, `test_docker_orphan_reap`, `test_mandatory_sandbox_provider`,
`test_task_branch_tools`, `test_composed_sandbox_providers_live`, `test_composed_containerd_providers_live`
(fails on ITS `DockerExecutionSurface` half only — its containerd half's own checks pass clean, matching
`test_containerd_execution_surface`'s own success), and `test_containerd_execution_surface` itself on
its FIRST run (before the image was cached). The Docker-shaped failures share one root cause: this
WSL2 distro's `docker` on `PATH` resolves to the WINDOWS Docker Desktop binary bridged in at
`/mnt/c/Program Files/Docker/Docker/resources/bin/docker` (confirmed via `which docker`) — a real
binary, but one that dials a Windows named pipe no Linux process can reach, not a broken or absent
Docker installation. This is a materially different, and more clearly diagnosable, root cause than
ADR-105's own prior finding ("`docker` is not on `PATH` in this WSL2 distro at all") — here it IS on
`PATH`, it simply cannot reach a real daemon from this side of the WSL2 boundary. Installing a genuine
Linux-native Docker daemon inside this WSL2 distro (or enabling Docker Desktop's WSL integration for
it) was judged out of scope for this verification pass — a real, more invasive environment change,
not attempted without separately deciding to make it.

## 3. What this closes, and what it does not

**Closes for real**: ADR-111/ADR-112's own "no Linux verification" residual — `test_ledger` needs no
container runtime at all, so this is a complete, unconditional closure, not a partial one.
ADR-106/ADR-108's containerd-path work is re-confirmed end-to-end on a fresh environment (31/31,
including the ADR-108 follow-on's own pid-reuse fix).

**Does NOT close**: ADR-114's own "no Linux verification" residual only partially — the task-branch
tools' own logic (start/run/commit/discard, the MUST-FIX/SHOULD-FIX fixes from ADR-114 §7) is proven
to COMPILE cleanly on Linux (a real, new fact — this exact code had never been attempted on this
platform before this pass) and its non-Docker-specific sibling
(`test_mandatory_sandbox_provider_composed`) passes for real, but `test_task_branch_tools.cpp` itself
is hardcoded to `MandatorySandboxProvider<DockerExecutionSurface>` and could not be run for real here
— this WSL2 distro has no reachable Linux-native Docker daemon. Given `MandatorySandboxProvider<Surface>`
is generic over `ExecutionSurface` and `test_containerd_execution_surface` already re-proves the
SAME underlying `Ledger`/`SandboxRuntime`/`reap_orphans()` machinery works correctly under a real
container runtime on this exact Linux environment, this residual is now narrower and better-understood
than before this pass — but not eliminated. `test_sandbox_runtime` and `test_docker_orphan_reap`
(Docker-specific, pre-existing, unrelated to this session's own new work) share the identical,
disclosed root cause.

## 4. What was NOT done

- No attempt to install or configure a real Linux-native Docker daemon in this WSL2 distro — a bigger,
  separate environment decision, not made as a side effect of a verification pass.
- No new test file written to exercise `MandatorySandboxProvider<ContainerdExecutionSurface>`'s own
  task-branch tools specifically (would need a `test_task_branch_tools`-shaped file parameterized over
  `ContainerdExecutionSurface` instead of `DockerExecutionSurface`) — real, disclosed follow-on work if
  a Docker-reachable Linux environment remains unavailable going forward.
- The default `gcc`/`g++` (13.3.0, below this project's own 021 §5 floor) being the distro default
  rather than `gcc-14`/`g++-14` is disclosed but not fixed (a `update-alternatives` change or similar
  host-configuration decision, out of scope for a verification-only pass).

## 5. Residuals

- `test_sandbox_runtime`/`test_docker_orphan_reap`/`test_mandatory_sandbox_provider`/
  `test_task_branch_tools`/`test_composed_sandbox_providers_live`'s Docker-specific half remain
  unverified on Linux in this pass — real, disclosed, environment-caused (no reachable Linux-native
  Docker daemon), not a code defect, and narrower in scope than before this pass given
  `test_containerd_execution_surface`'s own full, fresh 31/31 re-confirmation of the same underlying
  machinery.
- Every other residual named by ADR-108 through ADR-114 remains unchanged — this ADR only closes the
  Linux-verification gap, not any of the other named, disclosed follow-on work (the `Capability`-variant
  widening question, the `ComposedContextProvider` cross-session hazard, `ctr images mount`'s own
  separate, previously-disclosed environment defect in a DIFFERENT WSL2 distro, etc.).
