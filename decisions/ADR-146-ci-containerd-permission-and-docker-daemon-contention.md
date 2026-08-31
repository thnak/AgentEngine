# ADR-146 — CI: containerd needs root, and live-Docker/containerd tests need `RESOURCE_LOCK` against daemon contention

- **Status:** Proposed — implemented and REAL-CI-CONFIRMED for everything this ADR set out to fix:
  the `RESOURCE_LOCK` fix (dropped 5 non-deterministic failures to 0), the containerd-socket
  permission fix (`permission denied` gone), and the `ctr` image-pull gap (both containerd tests now
  pass 100%, disproving an earlier "shared composition bug" theory — see §9). **One test remains red
  and is NOT fixed by this ADR: `test_composed_sandbox_providers_live`** (Docker-specific, composed-
  provider-specific). Investigated extensively (§9: real native-Linux Docker repro, CI-matching
  full-suite runs under matched core constraints, 45+ local runs) with NO successful local
  reproduction — genuinely appears to be a low-frequency, CI-environment-specific race whose exact
  trigger was not identified. Diagnostic instrumentation added (§9) so the next real CI failure is
  directly diagnosable. Disclosed as an open, investigated-but-unresolved residual, not silently
  folded into a false "done."
- **Date:** 2026-08-31.
- **Scope:** `.github/workflows/ci.yml` (`linux` job's `Test` step only), `tests/CMakeLists.txt`
  (`RESOURCE_LOCK` property additions to 8 existing `add_test()` registrations, no new tests, no
  test logic changes).
- **Related specs:** `decisions/ADR-145-containerd-execution-surface-promotion.md` (ex-ADR-106, the
  containerd conformer whose tests this CI leg first exercises for real), `decisions/ADR-108-
  execution-surface-orphan-reap.md` (`test_docker_orphan_reap`, whose positive control is the
  concrete failure this ADR's second fix closes), `decisions/ADR-139-docker-run-capture-timeout-
  and-cap.md` (the Docker-daemon-dependent tests this ADR now serializes against each other).

## 1. The question

The current PR's first real Linux CI run (`ubuntu-latest`, gcc-14, `ctest -j 4`, unfiltered) failed
two ways, both new (never previously exercised in `ci.yml` — `test_containerd_execution_surface`/
`test_composed_containerd_providers_live` don't exist on `main` yet, so `main`'s own green CI never
ran them):

1. `test_containerd_execution_surface` / `test_composed_containerd_providers_live` both failed with
   `dial unix /run/containerd/containerd.sock: connect: permission denied` — a known, disclosed
   requirement (ADR-145 §own text already says "Run as root (containerd's default socket
   permissions require it)"), just never wired into CI before now.
2. A second, initially-undiagnosed failure: `test_composed_sandbox_providers_live` also failed in
   the same run. A follow-up run (same job, unmodified code, re-triggered by an unrelated push)
   failed WORSE — 5 tests, not 1: the same two containerd tests, plus
   `test_composed_sandbox_providers_live`, `test_sandbox_runtime`, and `test_docker_orphan_reap`.
   `test_docker_orphan_reap`'s own POSITIVE CONTROL check ("the matching-start-key container was
   NOT reaped") failed — the exact assertion this session's own ADR-108 red-team round added
   specifically to prove the pid-reuse fix doesn't over-reap. `test_sandbox_runtime` failed on
   "turn 1 `run()` succeeds" — the SAME failure signature ADR-139 §3a's own Windows CRT-quoting bug
   produced, but this is Linux/POSIX code, ruling that specific bug out.

Should CI grant the containerd tests root, and is the second failure a real logic regression or
something else?

## 2. Findings

**(1) is exactly what it looks like** — `ubuntu-latest` pre-configures the runner user into
`docker`'s group (confirmed: `test_docker_orphan_reap` reached a real daemon in the SAME run before
also failing for an unrelated reason — see below), but never grants equivalent access to
`/run/containerd/containerd.sock`, which is root-only by default and has no analogous group-based
opt-out. Nothing to root-cause further; `ADR-145`'s own text called this exact requirement out when
the containerd suite was first verified.

**(2) is daemon contention, not a logic bug.** All five failing tests in the worse run are real,
live-daemon E2E tests (`docker`, `docker`, `docker`, `ctr`, `ctr`) with NO `RESOURCE_LOCK` or
equivalent serialization between them — under `ctest -j 4` on a 2-4-core runner, CTest is free to
(and did) schedule several of them against the SAME shared daemon at once. Evidence this is
contention, not a regression:
- The runs are non-deterministic in WHICH tests fail and HOW MANY (1 vs. 5, same code, two
  different pushes) — a real logic bug in this session's own new code would fail the same way every
  time.
- `test_docker_orphan_reap`'s failed check is a POSITIVE control on the CURRENT test process's OWN
  pid+start-key, computed and checked entirely within that one test binary — nothing about it
  should depend on what other test binaries are doing, UNLESS the shared daemon itself is slow/
  contended enough that a `docker inspect`/`docker rename` call in this test's own sequence
  observes stale or delayed state while 3-4 OTHER test binaries are simultaneously issuing their own
  `docker run`/`exec`/`inspect`/`rm` calls against the identical daemon.
- `test_sandbox_runtime`'s "turn 1 `run()` succeeds" failure matches "the underlying `docker`
  invocation itself failed or timed out," consistent with daemon-side queuing/latency under
  concurrent load, not a code path this session's own Windows verification (which has run this
  exact `DockerExecutionSurface`/`SandboxRuntime` code against a live daemon repeatedly, ADR-139
  most recently) has ever reproduced non-concurrently.
- None of the 8 tests newly locked below have ever been given a `RESOURCE_LOCK` or run serially in
  CI before now — this is a pre-existing gap in the harness, not something this session's own
  recent changes introduced; it simply had no way to surface until `ci.yml` grew enough live-daemon
  tests running together for contention to become likely.

## 3. What was built

**`.github/workflows/ci.yml`** (`linux` job): split the single `Test` step into two:
- The main step runs the full suite EXCLUDING the two containerd tests
  (`-E '^(test_containerd_execution_surface|test_composed_containerd_providers_live)$'`),
  unprivileged, exactly as before otherwise.
- A new `Test (containerd, root)` step runs ONLY those two, under `sudo`
  (`-R '^(test_containerd_execution_surface|test_composed_containerd_providers_live)$'`).

Root is deliberately scoped to just these two tests, not the whole suite: several tests in this
codebase (the escape-corpus suites) specifically assert that a POSIX permission check REJECTS an
operation — running under root would bypass those checks and silently flip that signal from
"correctly denied" to "not tested," a worse regression than the one being fixed.

**`tests/CMakeLists.txt`**: added `RESOURCE_LOCK docker_daemon` to the six Docker-daemon-dependent
tests (`test_composed_sandbox_providers_live`, `test_sandbox_runtime`, `test_docker_orphan_reap`,
`test_mandatory_sandbox_provider`, `test_task_branch_tools`, `test_task_branch_concurrent_dispatch`)
and `RESOURCE_LOCK containerd_daemon` to the two containerd-daemon-dependent tests
(`test_containerd_execution_surface`, `test_composed_containerd_providers_live`). CTest's own
`RESOURCE_LOCK` semantics: tests sharing a lock name are never scheduled concurrently regardless of
`-j`, while every other test in the suite keeps running at full parallelism — this fixes the
contention directly rather than working around it by dropping `-j` for the whole job (which would
slow down all 189+ unrelated tests to fix 8).

Two separate lock names, not one: the Docker-locked and containerd-locked tests hit two genuinely
different daemons, so there's no real contention between the two GROUPS, only within each — and
since the containerd tests already run in their own separate, later `sudo`-elevated CTest
invocation (a fresh CTest process with only those two tests matched), they'd serialize against each
other via `-R`'s own tiny 2-test result set regardless; the explicit lock keeps that true if a
future test is ever added to the containerd-tagged group.

## 4. Verification

CMake reconfigure clean (`cmake .` in the existing Windows build tree, zero errors/warnings beyond
a pre-existing unrelated vendored-mbedtls deprecation notice). `ctest --show-only=json-v1 -C Debug`
confirmed all 6 Windows-visible `RESOURCE_LOCK` properties registered exactly as written (the two
containerd ones are inside `if(NOT WIN32)` and don't exist in this Windows configure — expected,
unverifiable here, will show up in the next Linux CI run). `naming_lint.py` clean. No production
code or test logic touched — this is CI/build-metadata only, so a full rebuild+`ctest` run adds no
signal beyond what reconfigure+list already confirmed.

**Real end-to-end verification is necessarily the next CI run on this PR** (this session has no
containerd daemon and no reproducible multi-core Docker-daemon-contention rig locally) — flagged
here rather than silently deferred.

## 5. Not done

- Did not investigate whether the SAME contention risk exists for the `windows-msvc` job's own
  `ctest -j 4` step — `RESOURCE_LOCK` is set in `tests/CMakeLists.txt`, which is platform-generic,
  so the fix already applies there too if Windows CI runners ever exercise these same live-Docker
  tests; no separate Windows-specific change was needed or made.
- Did not add a CI-level retry/backoff for daemon calls, or increase the containerd/Docker
  per-test `TIMEOUT` beyond what already exists — the working theory is that serialization alone
  removes the contention; if the next CI run still shows daemon-related flakiness with tests now
  serialized, that would point at a different cause (e.g. genuine per-call latency on this runner
  class) needing separate investigation, not assumed here.

## 6. Residuals

- If GitHub ever moves `ubuntu-latest` to a runner class where `docker` group membership is not
  pre-provisioned, `sudo`-scoping this list would need to grow to cover the Docker-daemon tests
  too — not needed today (`test_docker_orphan_reap` already proved unprivileged Docker access
  works on the current runner image).
- `RESOURCE_LOCK` values are plain strings CTest doesn't validate against anything else; a future
  test added under `if(NOT WIN32)` that talks to `ctr` but is added without the `containerd_daemon`
  lock (or a Docker test added without `docker_daemon`) would silently reintroduce the exact
  contention this ADR fixes — no automated check enforces the pairing, same class of residual as
  every other "must remember to tag it" convention in this codebase.

## 7. Real CI results (post-push) and a real bug found in this ADR's own first fix

The pushed commit's actual CI run confirmed the `RESOURCE_LOCK` fix directly: all 5 previously
non-deterministic failures (`test_composed_sandbox_providers_live`, `test_sandbox_runtime`,
`test_docker_orphan_reap`, plus the two containerd tests, which by then were excluded into their own
step) dropped to exactly ONE — `test_composed_sandbox_providers_live`, now failing consistently and
fast (0.91s, not a timeout), no longer alongside the others. `test_sandbox_runtime`/
`test_docker_orphan_reap`/`test_mandatory_sandbox_provider`/`test_task_branch_tools`/
`test_task_branch_concurrent_dispatch` all passed clean once serialized — daemon contention
confirmed as the real cause for those 4, not a logic bug, exactly as §2 reasoned.

**A real bug in THIS ADR's own first version was found from that same run**: because the main `Test`
step failed (due to the still-unrelated `test_composed_sandbox_providers_live` failure), GitHub
Actions' default step-gating SKIPPED the new `Test (containerd, root)` step entirely rather than
running it — so §1's actual fix target (the containerd permission gap) was never exercised by that
run at all, silently. Fixed by adding `if: always()` to that step (§3's code now reflects this).
Caught by checking the job's raw log for evidence the containerd step ran, not by assuming a green
partial result meant the intended fix worked — this session's own "verify FOR REAL, don't assume"
discipline applied to this ADR's own change, not just production code.

**`test_composed_sandbox_providers_live`'s own remaining failure is NOT part of this ADR's fix** —
traced far enough to rule out several hypotheses (not image-pull-related: other tests share the same
`alpine:latest` image on the same daemon and pass; not the POSIX `run_capture()` rewrite in general:
the exact same function is exercised successfully by 5 sibling tests in the same run; not the docker
command sequence itself: reproduced byte-identical against a live Docker daemon from this Windows
session and it returns the expected output) but not yet root-caused — the composed-provider-specific
angle (this is the only test that composes `SandboxToolProvider` and `MandatorySandboxProvider` in
ONE session and drives both through ONE `invoke_tool()`-pipeline `start_run()`, and the RunCommandTool
closure's `agentengine::rt::block_on(runtime_->run(...))` call is a plausible but unconfirmed
suspect) is disclosed here, not fixed, and needs either live-Linux-Docker repro or further tracing
this session did not complete before this ADR was written.

## 8. The `if: always()` fix's own CI run: permission gap CONFIRMED FIXED, one new gap found

The `if: always()` fix's own CI run proved the containerd step now actually executes (§7's bug is
closed): `test_containerd_execution_surface` no longer fails with `permission denied` on the socket
— §1's actual target is fixed. It now fails differently: `ctr: image "docker.io/library/alpine:
latest": not found`. A real, different, previously-masked gap: `docker` (used by every Docker-daemon
test, all passing) pulls that exact image fine on this runner, but `ctr` — probably because
`ubuntu-latest`'s Docker uses its own containerd namespace (`moby`), distinct from the `default`
namespace plain `ctr` targets — never sees it. Fixed by adding an explicit `ctr image pull` step
before the containerd `Test` step (§3's code now reflects this).

**A new, useful signal from this same run**: `test_composed_containerd_providers_live` failed with
the IDENTICAL check-level signature as `test_composed_sandbox_providers_live` ("`run_command`
genuinely executed... " / "the real Ledger checkpoint... " both FAIL, everything else in the test
passes) — but `ContainerdExecutionSurface`/`ctr_cli_detail::run_argv()` is a completely separate,
independently-implemented code path from `DockerExecutionSurface`/`docker_cli_detail::run_capture()`
(different file, different daemon, different CLI). Two independent implementations failing the exact
same way, only when composed with `SandboxToolProvider` in the same session, is real evidence the
open residual (§7) is NOT specific to either execution surface's own implementation — it narrows
toward something in the composition/pipeline machinery both share (`ComposedContextProvider`,
`invoke_tool()`, or the nested `agentengine::rt::block_on()` pattern §7 already named as a suspect).
Whether the containerd-composed test's failure was ALSO partly caused by the same image-pull gap
(masked identically, since neither composed test's own `check()` calls print the underlying surface
error) or is purely the shared composition bug is exactly what the next CI run (after this section's
own fix) will show — expected to narrow, not close, the open residual either way.

## 9. The image-pull fix's own CI run DISPROVED the "shared composition bug" theory

That CI run (§8's fix) came back with `test_composed_containerd_providers_live` PASSING clean —
100%, 0 failures on the containerd side. This disproves §8's "shared composition-layer bug" theory
outright: the containerd-composed test's earlier failure WAS entirely the same image-pull gap as its
standalone sibling, masked identically because neither composed test's own `check()` surfaces the
underlying surface error. `test_composed_sandbox_providers_live` (Docker) is now the ONLY remaining
red test in the entire CI matrix, and the theory space narrows back to something Docker-specific,
when composed with `SandboxToolProvider`, on `ubuntu-latest` specifically.

**Extensive real-repro effort, all of it negative** (i.e., could not reproduce):
- Added instrumentation to the test itself (`tests/test_composed_sandbox_providers_live.cpp`) that
  dumps every tool-role reply's actual content — error message, JSON, or text — on failure only, so
  the next real CI failure is diagnosable from the log directly rather than needing another guess.
  Building this surfaced and fixed a real bug in the instrumentation's own first draft:
  `ContentItem::value` on a tool-role message holds a `ToolResult` wrapper, not `Error`/`Data`/`Text`
  directly (missed on the first pass, caught because the fixed version's dry run against a genuine
  local failure — see below — printed nothing until corrected).
- Installed a REAL native Linux Docker engine (`docker.io` via `apt`, not Docker Desktop's Windows-
  hosted translation layer) inside the project's own established WSL2 Ubuntu-24.04 Linux-verify
  environment specifically so this investigation would run against the same class of daemon
  `ubuntu-latest` uses, not a cross-platform approximation.
- Built the exact test in Release mode with gcc-14 (CI's own compiler/build-type), ran it 15 times
  against real Docker: 0 failures.
- Ran it another 20 times pinned to 2 CPUs with `stress-ng --cpu 2` saturating them concurrently
  (deliberately CPU-starved, approximating a resource-constrained hosted runner): 0 failures.
- Built and ran the ENTIRE CI-excluded-containerd test suite (187 tests) via `ctest -j4`, pinned to 4
  CPUs (matching a standard GitHub-hosted runner's core count) — the most faithful local
  reproduction of the actual CI invocation achievable — 4 separate full-suite runs: 100% passed, 0
  failures, every time.
- **One genuine local failure DID occur** (Windows, this session's own long-lived Docker Desktop
  daemon, after many hours of heavy repeated container churn from this same investigation): the test
  failed with the identical signature (fast, ~0.9s, no timeout), and the container it created was
  left running afterward with a completely empty `/workspace` — proving the actual `docker exec`
  command never wrote anything inside the container, not a result-capture bug. Removing a STALE,
  already-orphaned container left over from an earlier run (created by an interrupted prior
  invocation, never cleaned up — `DockerExecutionSurface` has no automatic cleanup on object
  destruction, a pre-existing disclosed residual, not new) made the failure go away, and it could not
  be reproduced again afterward by deliberately recreating similar conditions (10 extra live
  containers present did not trigger it; two back-to-back runs of the same test with no cleanup in
  between did not trigger it).

**Conclusion, honestly stated**: this looks like a genuine, low-frequency race or transient daemon-
level hiccup — CI-side evidence (3 consecutive fresh-VM failures) shows it is NOT rare on
`ubuntu-latest` specifically, while 45+ real-Docker runs across two materially different local
environments (native Linux/WSL2, Windows Docker Desktop) reproduced it exactly once, under
conditions (accumulated local daemon state from hours of unrelated churn) that don't exist on a
fresh CI VM. The mechanism is NOT proven to be identical between the one local occurrence and the
systematic CI occurrences — they share a symptom (fast failure, empty/missing output) but the
CI-specific trigger (Docker version, storage driver, cgroup driver, or some other `ubuntu-latest`-
specific characteristic none of this session's local environments share) was not identified.

**What this ADR leaves behind, honestly**: the diagnostic instrumentation (kept, inert on the
passing path, verified via naming_lint and repeated local runs) is the load-bearing artifact for
whoever hits this next — the next real CI failure's log will show the actual reply content instead
of a bare "FAIL" with no context. Root-causing further from here needs either interactive access to
a failing `ubuntu-latest` run (this session had none) or accepting the diagnostic's next real capture
as the next lead. Not fixed. Not quarantined. Disclosed, with the actual investigative trail, so a
future session does not have to re-derive that image-pull, composition-layer, and general-resource-
contention are all ruled out before making progress.

## 10. §9's diagnostic delivered a real answer — and a genuinely new, specific lead

The push landed on a fresh CI run and, this time, the diagnostic instrumentation worked exactly as
designed: `test_composed_sandbox_providers_live` failed again, but now with the ACTUAL underlying
error visible for the first time —

```
DIAG: tool-role message error: docker cp (to container) failed: docker: 'docker cp' requires 2 arguments
```

This is a real, concrete finding, not a guess: `SandboxRuntime::run()`'s step 3 (`surface.reset()`)
seeds a fresh container by `docker cp`-ing the branch's materialized staging directory into
`/workspace`, and on this specific CI run that `docker cp` invocation was malformed enough that
Docker's own CLI parser saw the wrong argument count. `copy_to_container()`
(`docker_execution_surface.hpp`) already runs `host_path`/`container_path` through a strict allowlist
before embedding them (rejects space, quotes, and every shell metacharacter), so whatever produced
this was either an edge case that allowlist doesn't catch (an EMPTY `host_path`, an EMPTY
`inst.container_id`, or some interaction with the trailing `"/."` convention `reset()` appends) or
something in `run_capture()`'s own command delivery to `/bin/sh -c` — genuinely ambiguous from the
error text alone, since `r.stdout_text` never included the command it actually ran.

Closed that ambiguity directly rather than guessing further: `copy_to_container()`/
`copy_from_container()` now include the exact assembled command string in their own error message
(`"... (command: " + cmd.str() + ")"`) — safe to log verbatim, since every value embedded in `cmd` has
already passed the same allowlist check that gates whether the function proceeds at all. Full rebuild
clean, `naming_lint.py` clean, full `ctest` 294/294 minus the one pre-existing, disclosed, unrelated
`test_reference_agent_task_corpus` environment gap — zero regression from this change. Pushed; the
next CI failure (if it recurs) will show the literal `docker cp` command that was generated, which
should make the actual mechanism unambiguous instead of theorized.
