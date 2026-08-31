# ADR-135 — Real Linux verification of ADR-134: does `tools/durable_sandboxed_shell_chat.cpp` actually build and pass on GCC, not merely on MSVC?

- **Status:** Proposed — real verification pass. One real defect found and fixed, but it is in the
  NEW TEST's own cross-platform exit-code handling (`tests/test_durable_sandboxed_shell_chat_cross_
  process.cpp`), not in `tools/durable_sandboxed_shell_chat.cpp` itself, which built and ran correctly
  on Linux on the first attempt with zero changes needed. The pre-existing `/root/ae-verify` WSL2
  Ubuntu 24.04 checkout (ADR-115, reused by every Linux-verification ADR in this chain through
  ADR-133) fast-forwarded to this branch's exact HEAD (`2a84e8d`) and rebuilt/retested with the same
  real GCC 14.2.0 toolchain.
- **Date:** 2026-08-30.
- **Scope:** `tests/test_durable_sandboxed_shell_chat_cross_process.cpp` (one real fix: POSIX
  `std::system()` exit-status decoding), plus this ADR itself and the two disclosure-pointer edits it
  makes (`decisions/ADR-134-durable-sandboxed-shell-chat.md`, `decisions/README.md`). No production
  code (`tools/durable_sandboxed_shell_chat.cpp` itself) changed.
- **Related specs:** Closes ADR-134's own "no Linux verification yet" residual (§4). Reuses
  ADR-115/118/120/121/122/125/127/129/131/133's own environment and methodology directly, including
  ADR-120's own toggle-`AGENTENGINE_WITH_HTTPS`-and-revert pattern for building an HTTPS-gated target.

## 1. The question

ADR-134 added `tools/durable_sandboxed_shell_chat.cpp` — the first real, user-reachable production
tool wired to durable content storage (ADR-128/130/132) — and its own file comment claims it is
"portable on both platforms from the start," unlike its sibling `tools/sandboxed_shell_chat.cpp`,
which needed a real Linux-parity fix (ADR-105/107) before it could drop its own `WIN32` gate. That
portability claim had never actually been executed on Linux before this pass — **this is a genuinely
new claim being tested for the first time**, not a re-confirmation of an already-proven template
mechanism the way ADR-133 re-confirmed ADR-132's cross-compiler template reasoning. ADR-134's own
same-day red-team round also added `tests/test_durable_sandboxed_shell_chat_cross_process.cpp`, a
real two-process regression test asserting a decisive signal (`ledger_state.snapshot`'s own
last-write-time staying unchanged across a second invocation, proving reclaim over re-create) — does
that new test's own POSIX code path (the `unsetenv()`/`setenv()` branch, and the un-doubled
`std::system()` quoting reused from `test_content_durability_cross_process.cpp`) actually work
correctly on a real `/bin/sh`, not merely reuse a pattern already proven for a different call shape?

## 2. What was done

Fast-forwarded ADR-133's own `/root/ae-verify` checkout (`git fetch origin` + `git merge --ff-only
origin/session-worktree-design-draft`) from `51631ba` to `2a84e8d` — a clean fast-forward, 8 files
changed (868 insertions), covering both ADR-134 commits (`f1abbf2`, the tool itself, and `2a84e8d`,
the same-day red-team's new cross-process test) plus ADR-133's own two disclosure-pointer edits.
Confirmed via `git log --oneline -3` that the checkout landed exactly on `2a84e8d`. Confirmed the
build cache's baseline before touching anything (`AGENTENGINE_WITH_HTTPS:BOOL=OFF` in
`build-linux-verify/CMakeCache.txt`, matching every prior pass in this chain), and `nproc` reporting
12 cores.

Read `CMakeLists.txt`'s own new registration (line ~965-976): `agentengine_durable_sandboxed_shell_
chat` is gated `if(AGENTENGINE_WITH_HTTPS)` only — no additional `NOT WIN32`/`WIN32` condition, unlike
its Linux-only sibling `agentengine_containerd_shell_chat` a few lines below it — confirming the
target is meant to build on both platforms once HTTPS is on, exactly as ADR-134's own comment claims.
Reconfigured with `-DAGENTENGINE_WITH_HTTPS=ON` (ADR-120's own toggle-and-revert pattern) to make the
new tool and test targets buildable at all.

### The tool build itself — the genuinely new claim (its own paragraph, as it should get)

`cmake --build build-linux-verify --target agentengine_durable_sandboxed_shell_chat -j12`: **133/133
build steps, zero errors, on the first attempt.** No source change was needed anywhere in
`tools/durable_sandboxed_shell_chat.cpp` — its `host_home_dir()`'s only `_WIN32` branch (the
`USERPROFILE`/`HOME` split) compiled correctly on the `#else` (POSIX) arm, `agentengine::pal::env_
var()` linked cleanly, and every dependency the file actually includes (`agentengine::core`,
`agentengine::provider_http_client`, `agentengine::worktree_store`, `agentengine::sandbox_io`, plus
MbedTLS transitively via `agentengine::provider_http_client`) built without incident. A targeted
touch-and-rebuild of just this one translation unit, greped for `warning`/`error`, produced zero
matches — genuinely zero diagnostics, not merely zero fatal ones. **The tool's own top-comment
portability claim is now actually verified, not merely asserted: it needed no fix of any kind to
build on Linux/GCC 14.2.0.**

### The new cross-process test — one real, POSIX-specific defect found and fixed

`cmake --build build-linux-verify --target test_durable_sandboxed_shell_chat_cross_process -j12`:
4/4 steps, zero errors — the test itself built cleanly. Running it directly
(`./build-linux-verify/tests/test_durable_sandboxed_shell_chat_cross_process`) surfaced a real
failure: both `[1] first invocation exits with the documented, clean quickstart_builder.no_store
failure (exit code 1)` and the equivalent `[2]` check failed, while every other check (tree count,
snapshot existence, and — decisively — the snapshot-mtime-unchanged core claim itself) passed.

**Root-caused, not patched around**: `run_once()`'s `exit1 == 1` / `exit2 == 1` comparisons assume
`std::system()` returns the child's exit code directly, which is true on Windows but NOT on POSIX —
POSIX `std::system()` returns the raw `wait()` status, which encodes the exit code in bits 8–15.
Confirmed empirically on this real WSL2/GCC 14.2.0 host with a two-line C probe: `system("exit 1")`
returned raw status `256`, not `1`. This is a genuinely different failure mode than the one
`test_content_durability_cross_process.cpp` already red-teamed for POSIX (ADR-130 §7) — that sibling
test only ever compares `std::system()`'s return value against `0`, which happens to be encoding-
agnostic (a raw wait status is `0` if and only if the child exited normally with code `0`, same as
Windows' direct return), so its own POSIX fix (making the double-quote wrap `_WIN32`-only) never had
to confront this. This test is the first in the design line to assert a *specific nonzero* exit code,
and the pattern-reuse from the sibling test did not cover that case — exactly the kind of
call-site-specific gap the task briefing for this pass called out as worth re-checking rather than
assuming.

**Fixed** in `tests/test_durable_sandboxed_shell_chat_cross_process.cpp`: added `#include
<sys/wait.h>` on the POSIX arm and rewrote `run_once()`'s POSIX branch to decode the raw status via
`WIFEXITED`/`WEXITSTATUS`, mapping a `std::system()` launch failure (`-1`) or a not-normally-exited
child to `-1` (which never spuriously equals the expected `1`). The Windows arm is unchanged — its
`std::system()` already returns the exit code directly. Rebuilt (4/4 steps, zero errors) and reran:
**`ALL CHECKS PASSED`, exit code 0**, confirmed stable across two consecutive runs. The un-doubled
`std::system()` quoting for the self-relaunch (no `_WIN32` wrap needed on the POSIX arm, matching
`test_content_durability_cross_process.cpp`'s own already-red-teamed reasoning) and the `unsetenv()`/
`setenv()` calls both worked correctly on the first attempt — no defect found in either of those two,
only in the exit-code comparison.

### Revert to baseline and full regression

Reconfigured back to `-DAGENTENGINE_WITH_HTTPS=OFF` (confirmed via `grep` on `CMakeCache.txt`).
**Full incremental rebuild**: `cmake --build build-linux-verify -j12` — **23 build steps, zero
errors, zero warnings** (grepped the full build log for `warning`/`error` outside the expected
`-- ... compile-fail proof: OK` configure-time lines — nothing found). Confirmed via `ctest -N` that
neither `agentengine_durable_sandboxed_shell_chat` nor `test_durable_sandboxed_shell_chat_cross_
process` is registered in this HTTPS-off configuration, as expected (both are gated behind
`AGENTENGINE_WITH_HTTPS`).

Ran the full suite: `ctest --output-on-failure -j12`. **189 total, 6 failures**: `test_composed_
sandbox_providers_live`, `test_sandbox_runtime`, `test_docker_orphan_reap`, `test_mandatory_sandbox_
provider`, `test_task_branch_tools`, `test_task_branch_concurrent_dispatch` — the IDENTICAL 6-failure
set and identical 189 total ADR-133's own last-known-good Linux baseline established (unchanged
because this pass added no new HTTPS-off test to the suite; the new test only exists when
`AGENTENGINE_WITH_HTTPS=ON`). Independently reconfirmed `docker info` still fails with the same
"could not be found in this WSL 2 distro" message, matching the already-disclosed, pre-existing gap
exactly. **This baseline confirms zero regression from everything else in this session's work
landing on this platform — it does NOT itself prove the new test passes on Linux; that is proven
separately, directly, above, with `AGENTENGINE_WITH_HTTPS=ON`.**

## 3. What this closes and what it does not

**Closes for real**: ADR-134's own Linux-verification residual, for both halves. The tool itself
(`agentengine_durable_sandboxed_shell_chat`) builds clean on Linux/GCC 14.2.0 with zero source changes
— a genuinely new claim, verified for the first time here, not merely re-confirmed. The new
regression test (`test_durable_sandboxed_shell_chat_cross_process`) needed one real, POSIX-specific
fix (exit-status decoding) to pass on Linux; after that fix, it passes completely and stably, proving
the same crash-recovery reclaim-over-create claim ADR-134 §6 established on Windows now also holds on
a second, independent OS/compiler/allocator. The full HTTPS-off regression baseline (189 total, 6
pre-existing failures, zero new ones) confirms this session's other recent work introduced no
regression on this platform either.

**Does NOT close**: the same disclosed, pre-existing, environment-caused Docker-CLI-reachability gap
ADR-115/118/120/121/122/125/127/129/131/133 already named — unrelated to ADR-134's own subject,
unchanged by this pass. It does not exercise the actual interactive chat loop (`Bundle::ask()`) on
Linux either — no `OPENAI_API_KEY` was available in this verification environment, the same honestly
disclosed gap ADR-134 §1 already named for the Windows pass, unchanged here. It does not attempt to
install or configure a real Docker daemon in this WSL2 distro.

## 4. What was NOT done

- No attempt to install or configure a real Docker daemon in this WSL2 distro — same, separate
  environment decision every prior Linux-verification ADR in this chain has already deferred.
- The actual interactive chat loop was not exercised on Linux either, for the same reason ADR-134 §1
  itself disclosed on Windows: no `OPENAI_API_KEY` in this verification environment. `Bundle::ask()`
  is unmodified, already-shipped machinery, unrelated to what this pass verifies.
- No independent red-team round of this pass itself — this is a verification (and one narrowly
  scoped bug-fix) run, not new design work.
- The POSIX exit-status-decoding fix was applied and verified only for this one test's own `run_once()`
  helper. No audit was done of whether any OTHER test in this repository makes the same mistake
  (comparing a POSIX `std::system()` return value against a specific nonzero constant) — a real,
  disclosed gap in this pass's own coverage, not asserted to be the only instance.

## 5. Residuals

- The audit gap named in §4 (other tests possibly sharing the same POSIX exit-status mistake) — a
  real, cheap follow-on if ever wanted, not urgent: every other `std::system()`-based cross-process
  test in this design line (`test_content_durability_cross_process.cpp`,
  `test_identity_durability_precondition.cpp`) only ever compares against `0`, which this pass already
  confirmed is encoding-agnostic, so the known instances are not at risk — only an unaudited "any other
  test anywhere in the tree" case remains open.
- The same pre-existing, disclosed Docker-CLI-reachability gap (ADR-115 and every Linux-verification
  ADR since) remains unrelated and unchanged.
- ADR-134's own disclosed, deliberately-out-of-scope residuals (no `run_shell`/native-jail tool
  composed alongside this one, no automatic garbage collection or cross-process locking for the
  durable state directory, not safe to run twice concurrently against the same durable root) are
  unchanged by this pass, which verifies the Windows-authored tool and test on a second platform
  rather than closing any of them.
