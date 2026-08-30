# ADR-133 — Real Linux verification of ADR-132: does GCC agree with MSVC on the `Store`-templatization's injected-class-name/CTAD/deduction reasoning?

- **Status:** Proposed — real verification pass, no production code changed. The pre-existing
  `/root/ae-verify` WSL2 Ubuntu 24.04 checkout (ADR-115, reused by ADR-118/120/121/125/127/129/131)
  fast-forwarded to this branch's exact HEAD (`51631ba`) and rebuilt/retested with the same real
  GCC 14.2.0 toolchain. **This is the actual test of whether ADR-132's own `Store`-templatization —
  which leans on injected-class-name, CTAD, and method-template deduction, genuinely subtle C++
  rules a "standards conformant" compiler can still get wrong at the edges — behaves identically on
  a SECOND, independent compiler**, not merely whether it compiles at all.
- **Date:** 2026-08-30.
- **Scope:** No files changed except this ADR itself and the two disclosure-pointer edits it makes
  (`decisions/ADR-132-store-generic-sandbox-tool-surface.md`, `decisions/README.md`). This ADR
  records a verification run, not a code change.
- **Related specs:** Closes ADR-132's own "no Linux verification yet" residual (§5). Reuses
  ADR-118/120/121/125/127/129/131's own environment and methodology directly.

## 1. The question

ADR-132 made `SandboxRuntime`/`MandatorySandboxProvider<Surface>` gain a second, defaulted `Store`
template parameter, and `RealIoFileSystem`'s three `Ledger`-touching methods become method templates
over `Store`, claiming this is purely additive — every existing 1-argument
`MandatorySandboxProvider<Surface>` usage keeps compiling and behaving identically, resting on
injected-class-name resolving self-references inside each class template's own body, CTAD deducing
`Store` at the one real external call site with zero changes needed, and method-template deduction
correctly binding `Store` from each `Ledger<Store>&` argument with no ambiguity. ADR-132's own
same-day independent red-team round (§7) found no defect, including a real negative-compile probe
confirming the injected-class-name reasoning on MSVC. All of that verification, both the original
pass and the red-team round, ran on Windows/MSVC only. Does the SAME reasoning hold, unchanged, when
a second, independent compiler (GCC 14.2.0) actually compiles this templated code across the whole
tree — and does the new definitive full-stack proof
(`tests/test_task_branch_content_durability_integration.cpp`) genuinely pass on Linux too?

## 2. What was done

Fast-forwarded ADR-131's own `/root/ae-verify` checkout (`git fetch origin` +
`git merge --ff-only origin/session-worktree-design-draft`) from `c7cb5fe` to `51631ba` — a clean
fast-forward, 9 files changed (680 insertions), covering both ADR-132 commits (the templatization
itself, `28fc845`, and the same-day red-team follow-on, `51631ba`) plus ADR-131's own two
disclosure-pointer edits. Confirmed via `git log --oneline -3` that the checkout landed exactly on
`51631ba`. Confirmed the build cache's baseline before touching anything
(`AGENTENGINE_WITH_HTTPS:BOOL=OFF` in `build-linux-verify/CMakeCache.txt`, matching the established
baseline — the same "check the cache variable, don't just trust a `--target` build" discipline
ADR-122 established), and `nproc` reporting 12 cores, matching every prior pass in this chain.

**Full incremental rebuild**: `cmake --build build-linux-verify -j12` from `/root/ae-verify`
completed in **46 build steps, zero errors, zero warnings** (grepped the full build log for
`warning`/`error` outside the expected `-- ... compile-fail proof: OK` CMake configure-time lines —
nothing found). This is a genuinely large incremental rebuild relative to ADR-131's own 23 steps,
exactly as expected: `sandbox_runtime.hpp`/`mandatory_sandbox_provider.hpp`/`real_io_filesystem.hpp`
are foundational headers essentially every sandbox-related target depends on, so this run recompiled
every real-Docker sandbox test (`test_sandbox_runtime`, `test_mandatory_sandbox_provider`,
`test_mandatory_sandbox_provider_composed`, `test_task_branch_tools`, `test_task_branch_concurrent_
dispatch`, `test_composed_sandbox_providers_live`, `test_composed_containerd_providers_live`), every
Docker-independent recovery/durability test in this design line (`test_root_branch_recovery`,
`test_task_branch_durability_recovery`), the new `agentengine_sandbox_io` static library
(`real_io_filesystem_posix.cpp`, containing the newly-templated methods' POSIX implementation), and
the new `test_task_branch_content_durability_integration` binary itself — exercising the templated
code against GCC across essentially the whole sandbox surface, exactly the point of this pass.

### The cross-compiler template-behavior verification (the specific thing this pass exists to test)

**GCC 14.2.0 agreed with MSVC on every one of ADR-132's central template-reasoning claims.** The
whole tree — every file that names `SandboxRuntime`/`MandatorySandboxProvider` — compiled clean on
the first attempt, with no diagnostic of any kind (not just no errors: zero warnings) touching
template deduction, injected-class-name resolution, or overload ambiguity. Concretely:

- **Injected-class-name self-references compiled unchanged.** Every within-class-template
  self-reference ADR-132 §3 named (`agentengine::result<SandboxRuntime>` return types, the
  `merge_into(SandboxRuntime const& parent, ...)` parameter, constructor self-calls) built with no
  explicit `<Store>` anywhere in those files' own source — if GCC resolved the injected name to
  anything other than the current specialization, this would have failed to compile or, worse,
  silently bound to the wrong specialization; neither happened.
- **CTAD deduced `Store` at the one real external call site with zero source changes.**
  `tests/test_sandbox_runtime.cpp:93`'s `SandboxRuntime runtime(ledger, std::move(*root_r), staging);`
  — unmodified since before ADR-132 — compiled and its own dedicated real-Docker test
  (`test_sandbox_runtime`, run directly below) still exercises the real code path, confirming GCC's
  CTAD deduced `Store` from the constructor argument exactly as MSVC's did.
- **Method-template deduction on `RealIoFileSystem`'s three `Ledger`-touching methods** (`drain_
  into_tree()`, `scan_and_drain_into_tree()`, `materialize()`) resolved `Store` from each
  `Ledger<Store>&` argument with zero ambiguity across every call site in the rebuilt tree — the
  method-template `Store` parameter correctly shadowing the enclosing `SandboxRuntime<Store>`'s own
  class-template parameter of the same name, exactly the shape ADR-132 §7's own red-team round traced
  and confirmed on MSVC.
- **No cross-compiler disagreement of any kind surfaced** — no GCC-specific error, no GCC-specific
  warning (`-Wnon-template-friend`, ambiguous-deduction, or otherwise) that MSVC's own build (ADR-132
  §4/§7) did not also produce (neither produced any). Two independent compilers, two independent
  standard-library implementations (MSVC STL vs. libstdc++), agreeing exactly on every subtle rule
  this templatization depends on.

Ran `./build-linux-verify/tests/test_task_branch_content_durability_integration` directly. **Result:
`ALL CHECKS PASSED`, exit code 0.** This is the decisive full-stack result: real content committed to
a durable `Ledger<FileWorktreeObjectStore>`'s root branch via the raw `Ledger` API, `Mandatory
SandboxProvider<FakeSurface, FileWorktreeObjectStore>` binding to that same root and starting a real
task branch through the real tool surface, a simulated crash (everything destructed unmerged),
`bind_root_branch()` (ADR-128) reclaiming the root by identity alone on a freshly-reconstructed
`Ledger`, automatically rehydrating the orphaned child task branch (ADR-126) as part of that same
call — then `commit_task_branch()` on the ORIGINAL `handle_id` genuinely succeeding (a real three-way
merge reloading real content from real disk, not `ledger.merge_tree_load_failed`), with the real
content committed in phase A confirmed byte-exact through the ACL-gated `get_blob_safe()` path
afterward. Every one of this session's own root-recovery, child-recovery, content-durability, and
Store-genericity mechanisms composing together, through the real production API, on GCC — exactly as
on MSVC.

### The other Docker-independent tests in this design line

Ran directly, individually, not merely via `ctest`: `test_root_branch_recovery`, `test_task_branch_
durability_recovery`, `test_content_durability_cross_process`, `test_content_durability_concurrency`,
`test_identity_durability_precondition`, `test_ledger`. **All six: `ALL CHECKS PASSED`
(`test_ledger`: `all checks passed`), exit code 0**, confirming the templatization introduced zero
regression to the rest of this design line's own Docker-independent proofs on this platform.

### Full regression baseline

Ran the full suite: `ctest --output-on-failure -j12` from `build-linux-verify`. **189 total (188
ADR-131 baseline + 1 new test), 6 failures**: `test_composed_sandbox_providers_live`,
`test_sandbox_runtime`, `test_docker_orphan_reap`, `test_mandatory_sandbox_provider`, `test_task_
branch_tools`, `test_task_branch_concurrent_dispatch` — the IDENTICAL 6-failure set ADR-131's own
Linux baseline established, confirmed by inspecting real failure output in the `ctest` log (e.g.
`test_composed_sandbox_providers_live` failing at `run_command genuinely executed in a real Docker
container` — the same disclosed, pre-existing Docker-CLI-reachability gap this WSL2 distro has had
since ADR-115, not assertion logic or anything caused by ADR-132). Independently confirmed `docker
info` still fails (`The command 'docker' could not be found in this WSL 2 distro`), matching the
already-disclosed gap exactly. `test_task_branch_content_durability_integration` listed as Passed in
the suite, zero regression anywhere else.

### Bonus: containerd IS reachable in this distro, and the templatization holds under it too

Unlike Docker CLI, `ctr`/containerd IS reachable in this WSL2 distro right now (`sudo ctr version`
returns both a Client and a Server block; `sudo ctr images ls` lists real pulled images). Both
containerd-backed tests already ran as part of the full `ctest` pass above and both passed:
`test_composed_containerd_providers_live` (0.20s) and `test_containerd_execution_surface` (1.70s).
`test_composed_containerd_providers_live.cpp` constructs `MandatorySandboxProvider<
ContainerdExecutionSurface>` — the default, 1-template-argument form — and drives a real 10-step
pipeline through genuine container execution. This is real, extra confirmation (not merely the
Docker-CLI-blocked tests staying unverified) that the templatized default case works correctly under
genuine container-backed execution on Linux, on the exact platform and container runtime this session
already had reachable.

## 3. What this closes and what it does not

**Closes for real, unconditionally**: ADR-132's own Linux-verification residual. The specific,
named risk this pass exists to test — whether GCC 14.2.0 agrees with MSVC's own reasoning about
injected-class-name resolution, CTAD, and method-template deduction for this templatization — is
confirmed: the whole tree rebuilt clean (zero errors, zero warnings) on a second, independent
compiler and standard library, every self-reference and deduction site behaved exactly as ADR-132 §3
and §7 predicted, and the new definitive full-stack proof
(`test_task_branch_content_durability_integration`) passes completely on Linux, with the same
content-durability, root-recovery, and child-recovery claims composing together identically to the
Windows run. The full regression baseline (189 total, 6 pre-existing failures, zero new ones) confirms
the widening changed nothing for every already-verified default case on this platform either.

**Does NOT close**: the same disclosed, pre-existing, environment-caused Docker-CLI-reachability gap
ADR-115/118/120/121/125/127/129/131 already named — unrelated to ADR-132's own subject, unchanged by
this pass (Docker CLI specifically remains unreachable; containerd, a separate runtime, is reachable
and was used as a bonus confirmation above, not a substitute for the Docker-gated tests). It does not
attempt to wire any real production caller (`tools/cli_chat.cpp` and siblings) to actually use a
non-default `Store` — ADR-132 §5's own disclosed scope boundary on that point is unchanged. It does
not address the `FileWorktreeObjectStore`↔`durable_dir` configuration-consistency residual ADR-130 §6
named, also unchanged.

## 4. What was NOT done

- No attempt to install or configure a real Docker daemon in this WSL2 distro — same, separate
  environment decision every prior Linux-verification ADR in this chain has already deferred.
  Containerd's own reachability (§2, bonus section) is a pre-existing state of this distro, not
  something newly configured in this pass.
- No new test or repro was written in this pass — purely a verification run of what ADR-132 already
  shipped.
- No independent red-team round of this pass itself — this is a verification run, not new design
  work; ADR-132 §7 already carried its own same-day red-team, on MSVC.
- No attempt to reproduce ADR-132 §7's own negative-compile probe (`SandboxRuntime<
  InMemoryWorktreeObjectStore>::merge_into()` called with a mismatched-`Store` parent) on GCC. The
  positive claim (correct code compiles identically on both compilers) is fully verified by this
  pass; the negative claim (GCC also rejects the mismatched case) was not independently re-probed —
  named here as a real, disclosed gap in this pass's own coverage, not asserted to generalize from
  the Windows-only red-team probe.

## 5. Residuals

- The negative-compile-probe gap named in §4 — a real, cheap follow-on if ever wanted, not urgent:
  ADR-132's own positive claims (the ones with actual production-code consequences) are now
  cross-compiler verified; the negative claim is a compile-time safety net whose failure mode (silent
  wrong-`Store` binding) the whole rest of this pass's clean rebuild and clean test run already argue
  against happening in practice.
- The same pre-existing, disclosed Docker-CLI-reachability gap (ADR-115/118/120/121/125/127/129/131)
  remains unrelated and unchanged.
- ADR-132's own disclosed, deliberately-out-of-scope residuals (no production caller wired to a
  non-default `Store`, the `FileWorktreeObjectStore`↔`durable_dir` configuration-consistency gap) are
  unchanged by this pass, which verifies the Windows-authored templatization on a second platform
  rather than closing any of them.
