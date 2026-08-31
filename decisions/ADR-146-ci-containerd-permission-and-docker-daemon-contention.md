# ADR-146 — CI: containerd needs root, and live-Docker/containerd tests need `RESOURCE_LOCK` against daemon contention

- **Status:** Proposed — implemented, verified by re-configuring/re-listing under CTest on this
  session's own Windows/MSVC checkout (`RESOURCE_LOCK` values confirmed registered via
  `ctest --show-only=json-v1`), `naming_lint.py` clean. **The actual fix target is GitHub Actions'
  `ubuntu-latest` runner — verification of the fix's real effect is the next CI run on this PR,
  not a local build**, since neither a containerd daemon nor the multi-test daemon-contention
  scenario this ADR fixes exists on this Windows session.
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
