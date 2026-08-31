# ADR-131 — Real Linux verification of ADR-130: the red-team's `std::system()` quoting fix genuinely holds on GCC 14.2.0

- **Status:** Proposed — real verification pass, no production code changed. The pre-existing
  `/root/ae-verify` WSL2 Ubuntu 24.04 checkout (ADR-115, reused by ADR-118/120/121/125/127/129)
  fast-forwarded to this branch's exact HEAD (`c7cb5fe`) and rebuilt/retested with the same real
  GCC 14.2.0 toolchain. **This is the actual, real test of whether ADR-130 §7's own same-day
  red-team fix (making the cross-process self-relaunch tests' Windows-`cmd.exe`-specific outer
  quote wrap conditional on `_WIN32`) genuinely works** — the red-team's own account was a direct
  `sh -c` string reproduction, not a real Linux build of this repository; this pass is that build.
- **Date:** 2026-08-30.
- **Scope:** No files changed except this ADR itself and the two disclosure-pointer edits it makes
  (`decisions/ADR-130-content-durability-conformer.md`, `decisions/README.md`). This ADR records a
  verification run, not a code change.
- **Related specs:** Closes ADR-130's own "no Linux verification yet" residual (§5). Reuses
  ADR-118/120/121/125/127/129's own environment and methodology directly.

## 1. The question

ADR-130 (`agentengine::FileWorktreeObjectStore`, closing the content-durability half of the gap
ADR-102/126/128 each named and declined to close) landed on Windows/MSVC only. Its own same-day
independent red-team round (§7) found and fixed a real, Linux-blocking defect: both new
cross-process tests self-relaunch via `std::system()` as a genuinely separate OS process, and both
originally wrapped the whole command string in one extra, redundant pair of quotes — a real,
empirically-necessary workaround for a `cmd.exe`-specific misparse on Windows, but whose own
original comment incorrectly claimed the same wrap was also a harmless no-op on POSIX `/bin/sh -c`.
The red-team proved that claim false by reproducing the exact command-string construction against a
real `sh -c` (Git Bash's own `/bin/sh` on Windows) and fixed it by making the extra wrap
`#ifdef _WIN32`-only. That verification was a string reproduction against a POSIX-compatible shell
on Windows, not an actual Linux build and run of this repository. Does the fix genuinely hold when
`std::system()` really does invoke `/bin/sh -c` on real Linux, and do the rest of ADR-130's claims
(the concurrency probe's content-safety and same-digest-race sections, the identity-precondition
proof) hold on a second, independent allocator and filesystem?

## 2. What was done

Fast-forwarded ADR-129's own `/root/ae-verify` checkout (`git fetch origin` +
`git merge --ff-only origin/session-worktree-design-draft`) from `33c1b87` to `c7cb5fe` — a genuine,
clean fast-forward, 10 files changed (1702 insertions), covering ADR-129's own README/ADR touch-ups
through both ADR-130 commits (the port itself, `1a4b6b2`, and the red-team follow-on,
`c7cb5fe`). Confirmed via `git log --oneline -3` that the checkout landed exactly on `c7cb5fe`.
Confirmed the build cache's baseline before touching anything
(`AGENTENGINE_WITH_HTTPS:BOOL=OFF` in `build-linux-verify/CMakeCache.txt`, matching the established
baseline — the same "check the cache variable, don't just trust a `--target` build" discipline
ADR-122 established).

Rebuilt with the same GCC 14.2.0 toolchain: `cmake --build build-linux-verify -j12` completed in
**23 build steps, zero errors** (an incremental build against ADR-129's own already-built tree, so
only the genuinely new/changed work — the three new test targets and `libagentengine_sandbox_io.a`
— needed rebuilding). All three new test binaries — `test_content_durability_cross_process`,
`test_content_durability_concurrency`, `test_identity_durability_precondition` — compiled and linked
clean on the first attempt.

### The `std::system()` self-relaunch verification (the specific thing this pass exists to test)

Ran `./build-linux-verify/tests/test_content_durability_cross_process` directly. **Result: `ALL
CHECKS PASSED`, exit code 0.** This is the decisive result: the test's reader role genuinely invoked
`std::system()` on real Linux, which really does run the command through `/bin/sh -c` on this
platform (not Git Bash's own bundled `/bin/sh`, as the red-team's own reproduction used, but this
WSL2 distro's real Ubuntu 24.04 `/bin/sh`, `dash`) — and the writer role process was correctly
located, launched as a genuinely separate OS process, and exited cleanly with real content committed
to real disk. Had the red-team's `#ifdef _WIN32` fix been wrong or incomplete, the specific,
predicted failure mode was a `sh`-level parse collapse of the executable path and every argument
into one bad, nonexistent command name (`No such file or directory`, `std::system()` returning a
nonzero exit reflecting shell exit 127) — nothing resembling that occurred; the process ran to
completion and every one of its checks (root+child content committed by process 1, both branches
recovered as real orphans by process 2, `merge()` succeeding on disk-recovered content rather than
`ledger.merge_tree_load_failed`, and the merged content reading back byte-exact through
`get_blob_safe()`) passed.

Ran `./build-linux-verify/tests/test_identity_durability_precondition` directly, which uses the
identical self-relaunch pattern (`run_child()`, same `#ifdef _WIN32`-conditional wrap) twice per
scenario (owner role, then attacker role) across two scenarios. **Result: `ALL CHECKS PASSED`, exit
code 0.** Both proofs held on real Linux: the vulnerable configuration (in-memory-only
`IdentityAuthority` in both processes) produced a genuine cross-principal leak — the attacker's own
honestly-minted identity (id 1, the fresh in-process allocator restarting at 1 exactly as on
Windows) successfully read the real owner's real secret content through `get_blob_safe()`; the
correctly-configured scenario (both processes sharing the same durable `identity_dir`) correctly,
genuinely failed closed with `ledger.blob_access_denied`, the attacker's id correctly advancing to 2
against the durable high-water-mark rather than recycling.

Both tests together exercise the fixed quoting logic four total times (one self-relaunch in the
cross-process test, two owner/attacker pairs — four child launches — in the identity-precondition
test) against real `/bin/sh` / `dash`, all four succeeding.

### The remaining new tests and full regression baseline

Ran `./build-linux-verify/tests/test_content_durability_concurrency` directly. **Result: `ALL CHECKS
PASSED`, exit code 0**, taking **50.8 seconds real time** — noticeably longer than the sub-second
runtime of every other test in this suite, confirming the red-team's own new `[1b]` same-digest race
section (16 threads × 20 iterations, each racing 3 MiB of identical content through `put_blob()`)
genuinely built and ran rather than being silently skipped; a near-instant finish would have been the
tell that it hadn't. Section `[1]` (8 threads × 25 blobs, 200 total, cross-instance content safety)
and section `[2]`/`[2c]` (the disclosed, unfixed metadata-bookkeeping race under genuine concurrent
`Ledger` construction, exactly one of two branches surviving) both reproduced their expected outcomes
on real GCC/glibc/filesystem behavior, matching the Windows results exactly.

Ran the full suite: `ctest --output-on-failure -j12` from `build-linux-verify`. **188 total (185
baseline from ADR-129 + 3 new tests), 6 failures**: `test_composed_sandbox_providers_live`,
`test_sandbox_runtime`, `test_docker_orphan_reap`, `test_mandatory_sandbox_provider`,
`test_task_branch_tools`, `test_task_branch_concurrent_dispatch` — the IDENTICAL 6-failure set
ADR-129's own baseline established, confirmed not merely by name but by re-running
`test_docker_orphan_reap` and `test_task_branch_tools` directly and inspecting their real failure
output: `test_docker_orphan_reap` fails at "reap setup: create() a container," and
`test_task_branch_tools` fails across every check that depends on `run_in_task_branch()` actually
executing a container — both real container-creation attempts hitting the same disclosed,
pre-existing Docker-CLI-reachability gap this WSL2 distro has had since ADR-115, not assertion logic
or anything caused by ADR-130. All three new tests (`test_content_durability_cross_process`,
`test_content_durability_concurrency`, `test_identity_durability_precondition`) listed as Passed in
the suite, zero regression anywhere else.

Independently re-ran `test_ledger` directly, not merely trusted the aggregate `ctest` count: `test_ledger:
all checks passed`, exit code 0 — a cheap, independent sanity check that nothing in the wider
incremental rebuild regressed, even though `test_ledger.cpp` itself was untouched by ADR-130.

## 3. What this closes and what it does not

**Closes for real, unconditionally**: ADR-130's own Linux-verification residual — the specific,
named risk (the red-team's own quoting fix, verified only by a string reproduction against a
Windows-hosted POSIX shell, not an actual Linux build) is now confirmed against real `/bin/sh` on
real Ubuntu 24.04: both self-relaunching tests pass completely, with four total child-process
launches through the fixed quoting path all succeeding. The concurrency probe's full claim set
(content-safety, the new same-digest race, and the disclosed-not-fixed metadata race) and the
identity-precondition proof's full claim set (both the vulnerable-configuration leak and the
correctly-configured fail-closed case) all reproduce on a second, independent allocator and
filesystem, not merely asserted to generalize from the Windows results.

**Does NOT close**: the same disclosed, pre-existing, environment-caused Docker-CLI-reachability gap
ADR-115/118/120/121/125/127/129 already named — unrelated to ADR-130's own subject, unchanged by
this pass. It also does not attempt any Linux build/verification of the `SandboxRuntime`/
`MandatorySandboxProvider` `Store`-templatization follow-on ADR-130 §2 explicitly scoped out — that
work does not exist yet on any platform. It does not re-run ADR-130 §4's own temporary
`MergeCost`-timing probe (`probe_mergecost_timing.cpp`, already deleted per that ADR's own
"temporary probe, capture evidence, remove" account) — no cross-platform timing comparison was
attempted here.

## 4. What was NOT done

- No attempt to install or configure a real Linux-native Docker daemon in this WSL2 distro — same,
  separate environment decision every prior Linux-verification ADR in this chain has already
  deferred.
- No new test or repro was written in this pass — purely a verification run of what ADR-130 already
  shipped.
- Did not attempt the `MergeCost` real-I/O timing measurement a second time on Linux (ADR-130 §3's
  own number was explicitly disclosed as an informal, single-machine, Windows/NTFS measurement, not a
  cross-platform benchmark) — out of scope for a verification pass, real follow-on work if a rigorous
  cross-platform number is ever wanted.
- No independent red-team round of this pass itself — this is a verification run, not new design
  work.

## 5. Residuals

- None specific to `FileWorktreeObjectStore`, the cross-process self-relaunch mechanism, or the
  concurrency/identity-precondition proofs — this pass found nothing new to disclose; every claim
  ADR-130 made reproduced exactly as predicted on a second, independent toolchain and filesystem.
- The same pre-existing, disclosed Docker-CLI-reachability gap (ADR-115/118/120/121/125/127/129)
  remains unrelated and unchanged.
- ADR-130's own disclosed, deliberately-out-of-scope residuals (no `SandboxRuntime`/
  `MandatorySandboxProvider` integration, no garbage collection, no cross-process file locking beyond
  what exists, the metadata-bookkeeping concurrency hazard demonstrated-not-fixed, the informal
  `MergeCost` measurement) are all unchanged by this pass, which verifies the Windows-authored work
  on a second platform rather than closing any of them.
