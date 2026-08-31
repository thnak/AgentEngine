# ADR-105 — Was `SandboxToolProvider`'s remaining Windows-only build gate (`test_composed_sandbox_providers_live`, the two ADR-096 C2 compile-fail probes, `tools/sandboxed_shell_chat.cpp`) a real platform constraint, or a stale leftover from before ADR-103 achieved parity?

- **Status:** Proposed — implemented, verified, and independently red-teamed (2026-08-29), real builds
  and real test runs on both platforms. Two of the three named gates were STALE (real Linux wins, no
  code change needed beyond CMake); the third surfaced a genuine, previously-undiscovered, unrelated
  gap (disclosed, not fixed here). `test_composed_sandbox_providers_live` passed for real against a
  live Linux Docker daemon. Full Linux `ctest` (207 tests, first time this build has ever been run with
  `AGENTENGINE_WITH_HTTPS=ON`): 202/207 passed; the 5 failures are pre-existing and unrelated to this
  ADR's scope (confirmed via `git stash`), disclosed in §7. Independent red-team round (§5) reproduced
  every claim directly, found zero regressions/bypasses, and surfaced two stale comments (fixed) plus
  an inherited citation misquote (fixed).
- **Date:** 2026-08-29.
- **Scope:** `tests/CMakeLists.txt` (`test_composed_sandbox_providers_live` moved out of its `WIN32`
  gate; the two ADR-096 C2 `compile_fail` probes given a real POSIX deps list), `CMakeLists.txt`
  (`agentengine_sandboxed_shell_chat`'s gate comment corrected — kept `WIN32`-gated, for a different,
  real reason than previously stated). **No production code changed** — every fix in this ADR is a
  build-graph correction against code that was already portable.
- **Related specs:** `decisions/ADR-103-sandbox-tool-provider-linux-parity.md` (§7's own residual
  list, the origin of all three gates this ADR investigates) · `decisions/ADR-104-real-io-filesystem-
  linux-parity.md` (the sibling pass this one directly continues — same day) · `decisions/ADR-005-
  capability-bearer-tokens-cross-process.md` (the deliberately-Windows-only design whose `hmac_sha256`
  primitive turned out to be the REAL blocker for `sandboxed_shell_chat.cpp`, unrelated to
  `SandboxToolProvider`) · `CONVENTIONS.md` ("isolation parity is a gate, not a goal" — this
  ADR's own inverse lesson: don't assume a gate still reflects a real constraint just because it once
  did).

## 1. The question

ADR-103 §7 named three build-graph items as blocked by "SandboxToolProvider's own current platform
scope": `test_composed_sandbox_providers_live`, `tools/sandboxed_shell_chat.cpp`, and the two ADR-096
C2 `compile_fail` probes proving `AgentSession::fork_from()` fails to compile when
`SandboxToolProvider` is composed into a session. But `SandboxToolProvider`'s own file-top comment
(`src/backends/native_jail/sandbox_tool_provider.hpp:39-44`) and its dependency
(`session_shell_wiring.hpp:29-34`) both separately claim portability was achieved BY that same
ADR-103 pass. Which is right — is there still a real Windows-only dependency somewhere in this chain,
or are these three gates stale leftovers from before parity actually landed?

**Disproof, if "these are stale":** any of the three genuinely fails to build/pass on Linux once
un-gated, for a reason actually rooted in `SandboxToolProvider`/`SessionShellSandbox`/
`MediatedFileSystemAdapter` themselves (not an unrelated dependency).

## 2. Design

**Investigation, not construction.** Grepped every file in `SandboxToolProvider`'s real dependency
chain (`session_shell_wiring.hpp`, `mediated_command_registry.hpp`, `mediated_shell_dispatch.cpp`,
`mediated_shell_parser.cpp`) for `_WIN32`/`windows.h`/`CreateProcess` — none found (the one
`CreateProcess` hit in `mediated_command_registry.hpp` is a comment explaining the function
*deliberately has no path to it*, not a real reference). Confirmed at the CMake level:
`agentengine_mediated_shell_runner` (top-level `CMakeLists.txt:114-147`) has had a real `NOT WIN32`
branch since ADR-103, building from `mediated_filesystem_adapter_posix.cpp` instead of the Windows
`.cpp` — same public surface, same target name, on both platforms. This confirmed the hypothesis:
`SandboxToolProvider` itself has been portable since ADR-103; these three gates were never updated
to match.

**Empirical un-gating, one target at a time**, against a real WSL2 Ubuntu build with a real Docker
daemon reachable (the same environment ADR-104 verified against, same day):

1. **`test_composed_sandbox_providers_live`** — moved out of the `if(WIN32)` block (mirroring the
   precedent already set for `test_mediated_shell_runner_no_process_creation` in the ADR-104 follow-on
   work). Built and linked on the first try, no code change needed. Confirms: `SandboxToolProvider`
   and `MandatorySandboxProvider<DockerExecutionSurface>` composed together, real
   `ComposedContextProvider<Ms...>`, real `AgentSession::start_run()`, on Linux, for the first time.

2. **The two ADR-096 C2 `compile_fail` probes** — un-gated the same way, but these ALSO needed a real
   fix (not just removing the gate): the Windows branch's `try_compile()` dependency list hardcoded
   Windows-only source files (`mediated_filesystem_adapter.cpp`, `worktree_digest.cpp`,
   `worktree_mount_fs.cpp`) and linked `bcrypt` directly. Added a Linux `else()` branch with the real
   POSIX twins (`mediated_filesystem_adapter_posix.cpp`, `worktree_digest_posix.cpp`,
   `worktree_mount_fs_posix.cpp`) and no extra link library (Linux's `compute_digest` is a
   self-contained FIPS 180-4 implementation, no system crypto library needed) — the `try_compile()`
   calls themselves were de-duplicated to run unconditionally against whichever deps list the platform
   branch set, rather than existing as two near-identical Windows-only and (previously nonexistent)
   Linux copies. Both probes now run as part of every Linux `cmake` configure, matching the Windows
   `message(STATUS ...)` line: "ADR-096 C2 compile-fail proof: OK" now prints on Linux too.

3. **`tools/sandboxed_shell_chat.cpp` / `agentengine_sandboxed_shell_chat`** — un-gated the same way
   first, to test the hypothesis. Failed to LINK, but not for the reason the stale comment gave: a real,
   unrelated undefined reference to `agentengine::trust::hmac_sha256`. That symbol is defined only in
   `src/trust/hmac.cpp`, built only inside `agentengine_capability_token`
   (`CMakeLists.txt:595-604`), which is `if(WIN32)`-only with **no `else()` branch at all** — ADR-005's
   own deliberate scope (Windows CNG/BCrypt for cross-process capability tokens), never given a Linux
   twin. `tools/sandboxed_shell_chat.cpp` pulls this in transitively through
   `trust/secret_quarantine.hpp`'s `QuarantineSecretStore::quarantine()` (a header-only inline
   function; the ODR-use, and therefore the link failure, only manifests where a translation unit
   actually calls it) — a core turn-boundary secret-quarantine feature with **no connection to
   SandboxToolProvider at all**. This appears to be a previously-undiscovered gap: nothing else in this
   codebase's Linux build currently calls `QuarantineSecretStore::quarantine()`, so no other target has
   ever hit this undefined symbol. **Reverted the gate removal** for this one target — re-gated
   `WIN32`-only, with the comment corrected to name the REAL reason (see §7).

## 3. Falsifiable claims

| # | Claim |
|---|-------|
| C23 | `test_composed_sandbox_providers_live` builds and links on Linux with no production-code change, using the existing `agentengine::mediated_shell_runner`/`agentengine::sandbox_io` targets. |
| C24 | Both ADR-096 C2 `compile_fail` probes run on a real Linux `cmake` configure and print the same "OK" status line Windows does — the `fork_from()`-must-fail-to-compile claim and its positive control both hold against the real POSIX `SandboxToolProvider` dependency chain, not just the Windows one. |
| C25 | `agentengine_sandboxed_shell_chat`'s Linux link failure is caused by `hmac_sha256`/`QuarantineSecretStore`, NOT by anything in the `SandboxToolProvider`/`MandatorySandboxProvider<DockerExecutionSurface>` composition itself. |
| C26 | This ADR's own changes cause zero regressions: every `ctest` failure on a full Linux run is pre-existing and reproducible identically with this ADR's diff `git stash`d out. |

## 4. Executed evidence

- **C23:** `make -j12 test_composed_sandbox_providers_live` in a real WSL2 Ubuntu build
  (`build-linux`, `AGENTENGINE_WITH_HTTPS=ON`) — `[100%] Built target
  test_composed_sandbox_providers_live`, first try, no source change.
- **C24:** Reconfigured the same build dir from scratch after the `tests/CMakeLists.txt` change —
  configure log shows `-- ADR-096 C2 compile-fail proof: OK (composing SandboxToolProvider into a
  session makes fork_from() fail to compile; the same session type minus that call compiles and links
  cleanly)`, printed unconditionally (was previously only printed inside the `if(WIN32)` block).
- **C25:** `make -j12 agentengine_sandboxed_shell_chat` (before reverting the gate) failed with:
  `undefined reference to 'agentengine::trust::hmac_sha256(unsigned char const*, unsigned long,
  unsigned char const*, unsigned long)'`, traced via the linker's own function context
  (`QuarantineSecretStore::quarantine`, `trust/secret_quarantine.hpp:146`) to `hmac.cpp`'s exclusive
  membership in the `WIN32`-only `agentengine_capability_token` library — confirmed via direct
  `grep`/read of `CMakeLists.txt:595-604` and `src/trust/hmac.cpp` (a straightforward BCrypt-based
  HMAC-SHA256, ADR-005's own deliberate scope, no Linux twin exists).
- **C26:** Full `build-linux` rebuild (`make -j12 -k`, `AGENTENGINE_WITH_HTTPS=ON`) — exactly ONE
  target failed to build: `test_session_builder`, with the IDENTICAL `hmac_sha256` undefined-reference
  error as `agentengine_sandboxed_shell_chat`. Confirmed pre-existing and unrelated to this ADR's own
  changes: `git stash`d this ADR's entire diff (`CMakeLists.txt`/`tests/CMakeLists.txt`) and rebuilt
  `test_session_builder` alone against the untouched tree — identical failure, byte-for-byte same
  linker error. This means the `hmac_sha256`/`QuarantineSecretStore` gap named in C25 is WIDER than
  originally scoped: it blocks every `AGENTENGINE_WITH_HTTPS`-gated Linux target that touches
  `QuarantineSecretStore`, not just `sandboxed_shell_chat.cpp` — and had never been exercised before,
  because this repo's Linux build directory had never previously been configured with
  `AGENTENGINE_WITH_HTTPS=ON` (confirmed: its `CMakeCache.txt` read `OFF` before this ADR's own
  investigation turned it on to test `agentengine_sandboxed_shell_chat`). `git stash pop` restored
  this ADR's changes before continuing. Full `ctest -E '^test_session_builder$'` run: **202/207
  passed (98%)**. `test_composed_sandbox_providers_live` (this ADR's own primary target) **passed
  against a real live Docker daemon** — the first time this exact test has run successfully on Linux.
  The 5 failures, all pre-existing and structurally unrelated to anything this ADR touched: 4 are
  `test_kata_backend_*_linux` (real Kata Containers/network-namespace/cgroup operations failing --
  this WSL2 environment has no working Kata runtime, a known, already-disclosed environment gap, see
  `project_kata_ci_runner_labmaymok_status` memory) and 1 is `test_provider_egress_address_policy`
  (ADR-016's own egress-policy suite, also newly exposed by turning `AGENTENGINE_WITH_HTTPS` on for
  the first time on Linux — a real, distinct finding: `G3: an octal-encoded address is not a dotted
  quad` fails, meaning one of the two address resolvers may accept an octal-encoded IP literal that
  the other correctly rejects, a potential SSRF-adjacent parsing discrepancy. Unrelated to
  `SandboxToolProvider`/`MandatorySandboxProvider`, ADR-016's own scope, not investigated further
  here — named as a fresh, disclosed finding for a future ADR-016 follow-on pass, not fixed in this
  one). Two tests reported "Skipped" via the pre-existing `SKIP_RETURN_CODE 77` mechanism
  (`test_shell_runner_no_process_creation`/`test_mediated_shell_runner_no_process_creation`), an
  already-established, unrelated pattern (see ADR-104's own follow-on work on these same two tests).

## 5. Red-team round

An independent, fresh `general-purpose` subagent (zero context from the session that made these
changes) re-attacked C23-C26 directly against the real repo, not by re-reading this ADR's own prose:

- **CMake correctness**: independently traced the `if(WIN32)`/`else()`/`endif()` nesting in both
  files hunk-by-hunk — confirmed balanced and correct, confirmed the untouched
  `AGENTENGINE_BUILD_PYTHON_RUNNER` block's own `if`/`endif()` pair is unaffected. Verified (not
  assumed) that an empty `LINK_LIBRARIES` list on the `NOT WIN32` `try_compile()` branch is handled
  correctly by CMake — the positive-control probe still succeeded, and the "OK" status line still
  printed.
- **C23/C24 re-reproduced independently**: rebuilt and re-ran `test_composed_sandbox_providers_live`
  against real Docker on its own WSL2 session — passed. Reconfigured from scratch — the "ADR-096 C2
  compile-fail proof: OK" line printed on Linux.
- **Explicitly hunted for a "third reason" `agentengine_sandboxed_shell_chat` might fail** (beyond
  `hmac_sha256`) by un-gating it independently and building directly: confirmed `hmac_sha256` is the
  ONLY failure — every other dependency (`session_builder.hpp`, `composed_context_provider.hpp`,
  `docker_execution_surface.hpp`, `mandatory_sandbox_provider.hpp`, `sandbox_tool_provider.hpp`)
  compiles cleanly on Linux. C25 holds under direct test, not just by analogy to
  `test_session_builder`.
- **C26 independently reproduced**, including the `git stash` claim: stashed this ADR's own diff,
  rebuilt `test_session_builder` against the untouched tree, got the byte-identical `hmac_sha256`
  linker error, popped the stash, confirmed the working tree matched exactly.
- **Security review (I2/I3)**: confirmed zero production code changed (only the two `CMakeLists.txt`
  files) — no new authority reachable, the same already-portable code is merely exercised on a second
  platform now.
- **Real findings, not previously disclosed**: two stale top-of-file comments in source files this
  ADR's own CMake changes didn't touch — `tests/test_composed_sandbox_providers_live.cpp`'s own
  "REQUIRES: Windows (SandboxToolProvider's own current platform scope)" comment (now factually wrong:
  the test passes on Linux) and `tools/sandboxed_shell_chat.cpp`'s identical claim (the Windows-only
  OUTCOME is still correct, but the stated REASON is the same stale one this ADR corrected everywhere
  else). Both **fixed** as a direct result of this red-team round (see their own file history). Also
  caught a citation misquote inherited from ADR-103/104 (`CONVENTIONS.md`'s actual text is "Isolation
  parity is a gate, not a goal.", not "...not identical shape") — **fixed** in all three ADRs.
- **Process disclosure, self-reported by the red-team agent**: while probing for a third `hmac_sha256`
  alternative, it ran `git checkout -- CMakeLists.txt` to undo its own temporary test edit, which
  reverted the WHOLE file and discarded this ADR's legitimate uncommitted `CMakeLists.txt` diff. Caught
  immediately via `git diff`/`git status`, reconstructed the exact original diff by hand, and verified
  byte-identical restoration (matching blob hash) before reporting. Independently re-verified by the
  coordinating session afterward: confirmed intact, matching what was written before the red-team ran.

No bypass, no regression, no over-broad rejection, no C23-C26 discrepancy found.

## 6. Decision

Un-gate `test_composed_sandbox_providers_live` and the two ADR-096 C2 `compile_fail` probes
permanently — they were stale gates over already-portable code, not a real platform constraint. Leave
`agentengine_sandboxed_shell_chat` `WIN32`-gated, but for the corrected reason
(`hmac_sha256`/`QuarantineSecretStore`, not `SandboxToolProvider`). Do not attempt an
`hmac_sha256` Linux port as part of this ADR — that is ADR-005's own deliberate scope, a distinct
security-critical crypto-primitive question, not a `SandboxToolProvider` parity question, and deserves
its own dedicated design/red-team/prove pass if pursued.

## 7. Residual risks

- **`hmac_sha256` has NO Linux implementation at all, and blocks more than `sandboxed_shell_chat.cpp`**
  — widened during this ADR's own C26 evidence gathering: `test_session_builder` (an old,
  long-existing test with no connection to `SandboxToolProvider`) fails identically. Both trace to
  `QuarantineSecretStore::quarantine()` (`trust/secret_quarantine.hpp`, a core turn-boundary
  secret-quarantine feature) calling `agentengine::trust::hmac_sha256`, which exists only inside the
  `WIN32`-only `agentengine_capability_token` library (ADR-005's own deliberate BCrypt/CNG scope, no
  `else()` branch). Every `AGENTENGINE_WITH_HTTPS`-gated Linux target that touches
  `QuarantineSecretStore` is affected — this had simply never been exercised before because this
  repo's Linux build directory had never been configured with `AGENTENGINE_WITH_HTTPS=ON` until this
  ADR's own investigation turned it on. A from-scratch HMAC-SHA256 construction (RFC 2104, built on
  the existing `worktree_digest_posix.cpp` SHA-256 primitive) is the likely shape of a fix, verified
  against RFC 4231's known-answer test vectors as a positive control — named for whoever picks it up
  next, not silently left for them to rediscover, and NOT attempted here (ADR-005's own scope, a
  distinct security-critical crypto-primitive question).
- **NEW, disclosed finding, out of scope: `test_provider_egress_address_policy`'s `G3` check fails on
  Linux** — `an octal-encoded address is not a dotted quad and does not resolve as a hostname either`.
  This suggests one of ADR-016's two address resolvers (guest-path vs. provider-path) may accept an
  octal-encoded IP literal (e.g. `0177.0.0.1`) that the other correctly rejects — a potential
  SSRF-adjacent parsing discrepancy. Found purely as a byproduct of turning `AGENTENGINE_WITH_HTTPS`
  on for a Linux build for the first time (same root cause as the `hmac_sha256` gap above: never
  exercised on Linux before). Entirely unrelated to `SandboxToolProvider`/
  `MandatorySandboxProvider<DockerExecutionSurface>` and this ADR's own scope — not investigated
  further here. Named for a future ADR-016 follow-on pass.
- **4 pre-existing `test_kata_backend_*_linux` failures**, confirmed environment-caused (this WSL2
  distro has no working Kata Containers/KVM runtime — matches this project's own already-recorded
  `project_kata_ci_runner_labmaymok_status` finding that no available host can currently run Kata) —
  unrelated to this ADR, not investigated further.
- **The `ContainerdExecutionSurface`/ADR-101 promotion** and the Windows-side shell-injection
  blacklist re-audit remain separate, untouched, previously-named residuals (ADR-104 §7) — out of this
  ADR's scope.
