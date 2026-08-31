# ADR-103 — Does `SandboxToolProvider`'s `MediatedFileSystemAdapter` (ADR-096) port cleanly to Linux, reusing the already-Judged `open_within_mount_root` (ADR-014) containment primitive, without reopening the class of TOCTOU/mount-escape bug that primitive exists to close?

- **Status:** Proposed — implemented and independently red-teamed (2026-08-28), real builds and real
  test runs on both platforms, including a genuine WSL2 Ubuntu Linux environment (kernel
  6.6.87.2-microsoft-standard-WSL2, gcc 15.2.0) — the first time this repository's `SandboxToolProvider`
  chain has ever actually been built or run on Linux. One real MUST-FIX (a guest-triggerable crash)
  found and fixed same day, empirically re-verified after the fix. Two SHOULD-FIX platform-parity gaps
  found and fixed same day. One real, pre-existing, unrelated gap discovered as a byproduct
  (`RealIoFileSystem`/`SandboxRuntime`, ADR-102 Phases 3-4, unconditionally `#include <windows.h>`) —
  disclosed, not fixed, out of this ADR's scope.
- **Date:** 2026-08-28.
- **Scope:** `src/backends/native_jail/mediated_filesystem_adapter.{hpp,cpp}` (existing Windows
  implementation, made portable) plus a new `mediated_filesystem_adapter_posix.cpp` (Linux
  implementation), `session_shell_wiring.hpp`/`sandbox_tool_provider.hpp` (portable path-type
  signature changes only), a small additive fix to the already-Judged (ADR-014)
  `worktree_mount_fs_posix.cpp`, CMake wiring for both platforms, and six test files made portable.
  **Excludes**: `ContainerdExecutionSurface`/ADR-101 promotion; the whole ADR-102 identity-native
  sandbox stack's own Linux parity (a separate, larger, PRE-EXISTING gap discovered as a byproduct,
  §7); `tools/sandboxed_shell_chat.cpp`/`test_composed_sandbox_providers_live`/the ADR-096 C2
  compile-fail probes (all pull in `MandatorySandboxProvider<DockerExecutionSurface>`, and this
  session's Linux verification environment has no Docker daemon reachable — untested on Linux,
  deliberately not claimed); `test_mediated_shell_runner_no_process_creation`'s own Linux symbol list
  (a separate, bounded task); the embedded-CPython Python-runner composition tests (a materially
  larger, unrelated platform question); `remove(recursive=true)`'s own recursion-depth hazard, shared
  identically with the pre-existing Windows implementation (§6, disclosed not fixed).
- **Related specs:** `decisions/ADR-096-session-sandbox-lifecycle-context-provider-wiring.md`
  (`SandboxToolProvider`'s own design, Windows-only "for this pass") · `decisions/ADR-014-worktree-mount-path-canonicalization.md`
  (Judged; the containment primitive this whole port reuses, both the Windows Phase C2 half and the
  already-Judged Linux Phase C4 half) · `021-Platform-Support-and-Portability.md` §6 G3 (the
  path-escape corpus every containment claim in this codebase is measured against) ·
  `CONVENTIONS.md` ("isolation parity is a gate, not a goal" — cited repeatedly below, since
  several real findings in this pass were exactly a Windows/Linux shape mismatch, not a security
  escape) · `decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md` (a separate,
  adjacent sandbox stack whose own, unrelated Linux gap this pass discovered as a byproduct, §7).

## 1. The question

Can `SandboxToolProvider`'s `run_shell` tool chain — Windows-only since ADR-096 shipped it, per that
ADR's own "021 §2 Windows-now/Linux-next ordering" framing — actually build and run on real Linux,
and can the one genuinely OS-specific piece (`MediatedFileSystemAdapter`, ADR-096's `FileSystemAdapter`
conformer) be ported without weakening the containment guarantee `open_within_mount_root` (ADR-014)
already established and Judged for both platforms?

**Disproof, if true, of "yes cleanly":** a real Linux build either fails to compile, or compiles but a
guest-reachable operation crashes the host process, or a guest-reachable operation escapes the mount
root on Linux in a way the Windows implementation already closed.

## 2. Design

**Scoping research** (a fresh subagent, before any code was written) found the actual Windows-specific
surface was narrower than "the whole native_jail chain": only `mediated_filesystem_adapter.cpp` itself
has real Win32 API calls (`CreateFileW`/`ReadFile`/`WriteFile`/`SetFileInformationByHandle`/
`GetFileInformationByHandleEx`/`GetFinalPathNameByHandleW`). `mediated_shell_parser.cpp`/
`mediated_shell_dispatch.cpp`/`mediated_command_registry.hpp`/`mediated_shell_runner.hpp` are already
portable C++ with zero OS dependency (confirmed by direct reading, not assumed) —
`MediatedShellRunner` is an in-process interpreter over a closed builtin/registered-Runner grammar,
never spawning a real OS process, so none of the Windows-specific process/Job-Object machinery
(`LinuxNativeJailBackend`/`CgroupLimits`, a separate subsystem for the Python-worker jail) is even
relevant here. And the hard part — path containment — was **already done and Judged**:
`open_within_mount_root`'s Linux twin (`core/worktree_mount_fs_posix.hpp/.cpp`, ADR-014 Phase C4) has
existed since an earlier, separate effort. This narrowed the real remaining work to: port ONE file
(`MediatedFileSystemAdapter`), reusing that already-Judged primitive.

**`MediatedFileSystemAdapter`'s declaration became portable**: `root_` changed from Windows-only
`std::wstring` to `std::filesystem::path`, mirroring how `agentengine::worktree_store`'s own
`compute_digest()` stays ONE portable declaration backed by two platform-specific `.cpp` files
(`worktree_digest.cpp`/`worktree_digest_posix.cpp`) — selected at CMake link time, never `#ifdef`'d
inside one `.cpp`, matching this codebase's established one-file-per-platform convention
(`worktree_mount_fs.cpp` vs. `worktree_mount_fs_posix.cpp`). `SessionShellSandbox::create()`/
`SandboxToolProvider`'s own call site followed the same signature change; neither needed any other
logic change, confirming the scoping research's claim that they were already platform-agnostic.

**The new `mediated_filesystem_adapter_posix.cpp`** implements all 10 `FileSystemAdapter` methods
against `open_within_mount_root`, mirroring the Windows file's own structure and TOCTOU-safety
reasoning method-for-method, with genuine POSIX-idiom divergences named rather than forced into a
false Windows-shaped parity (`O_APPEND` giving kernel-atomic append semantics stronger than the
Windows sibling's manual `SetFilePointerEx`; no `\`/`:` character restriction, since those are
ordinary legal POSIX filename characters unlike Windows; `remove()`'s inherent "no delete-via-fd
primitive on POSIX" shape, addressed via a verified-parent-fd `unlinkat`). Two POSIX primitives this
task needed but that did not exist anywhere in this codebase before this file — directory listing and
live on-disk usage — were written directly against `open_within_mount_root`, since neither is itself a
NEW security boundary; both mirror `worktree_mount_fs.cpp`'s own already-Judged `list_within_mount_root`/
`mount_root_usage` policy (symlinks never followed while walking, purely to keep a usage scan
non-cyclic and bounded).

CMake: a `NOT WIN32` branch for `agentengine_mediated_shell_runner` (top-level `CMakeLists.txt`)
building the new POSIX adapter instead of the Windows one, same target name/sources/link-libs
otherwise. Six test targets moved out of an `if(WIN32)` gate to build unconditionally
(`test_mediated_shell_runner_smoke`, `test_mediated_shell_runner_wall_clock_timeout`,
`test_effect_context_sandbox_fs`, `test_session_shell_wiring`, `test_sandbox_tool_provider`,
`test_mediated_shell_runner_hostile_corpus`), each fixed for portability (a hand-read `TEMP` env var
with a Windows-only fallback path, replaced with `std::filesystem::temp_directory_path()`; direct
`.wstring()` construction, replaced with `std::filesystem::path`/`std::string`).

**`test_mediated_shell_runner_hostile_corpus.cpp`'s SH7-SH10 block** — a real, previously-shipped
CRITICAL Windows security fix (a backslash-based mount-escape bug in directory creation) — needed
platform-adapted, not merely ported, expected outcomes: the attack's premise (a backslash smuggling a
`..` walk-up past a `/`-only tokenizer) is Windows-specific by construction, since `\` has no separator
meaning on POSIX. Split into `#ifdef _WIN32`/`#else` branches with genuinely different, both real,
assertions per platform (Windows: rejected as a policy violation; Linux: succeeds as an ordinary,
harmless, oddly-named in-mount directory creation, proven positively via `std::filesystem::exists()`
on the literal name, not merely "did not escape").

## 3. Falsifiable claims

- **C15 (build parity).** The `run_shell` chain — `agentengine_mediated_shell_runner`,
  `SessionShellSandbox`, `SandboxToolProvider`, and the six ported tests — compiles and links cleanly
  on real Linux (WSL2 Ubuntu, gcc 15.2.0), with zero new failures anywhere else in the project.
  *Disproof: a compile/link failure in this chain, or a NEW failure elsewhere in the project caused by
  this change.*
- **C16 (functional parity).** Every one of the six ported tests passes for real on real Linux, not
  merely compiles. *Disproof: any test failure.*
- **C17 (containment holds on Linux).** No guest-reachable operation through the new POSIX adapter can
  resolve, read, write, or create anything outside the declared mount root, and the containment
  mechanism (`open_within_mount_root`) is used for every guest-input-derived filesystem access.
  *Disproof: a concrete escape.*
- **C18 (no new crash/DoS surface).** No guest-reachable operation through the new POSIX adapter can
  crash the host process or exhaust its resources in a way bounded by GUEST-CONTROLLED input (as
  opposed to ordinary, self-limiting resource use). *Disproof: a concrete guest-triggerable crash or
  unbounded-resource repro.*

## 4. The red-team attack (2026-08-28, independent, fresh `general-purpose` subagent, not a fork)

Given real build/shell access on both platforms and explicit instruction to verify empirically, not
just read, the round compiled and ran real standalone repros against the actual built libraries on
BOTH platforms (including a Windows repro cross-check via MSVC, and a real WSL2/gcc repro), rather
than trusting the comments' own claims.

**1. MUST-FIX (C18 disproven, then re-proven after the fix) — `usage()`'s recursive walk was a real,
guest-triggerable crash.** The original `accumulate_usage()` recursed one C++ stack frame PER
directory-tree level, and held TWO simultaneously-open file descriptors per level for the entire
duration of that subtree's traversal — exactly the shape `worktree_mount_fs.cpp::mount_root_usage()`
(the Windows sibling this file claims to mirror) explicitly avoids, via its own documented "an explicit
stack, not recursion... its depth is not bounded by anything this function controls" comment. `usage()`
is not a diagnostic side-path: `mediated_shell_dispatch.cpp`'s `require_fs_write()` calls it on every
quota-capped `mkdir`/`cp`/output-redirect, an entirely ordinary sandbox configuration, and nothing in
`split_mount_path`/`open_within_mount_root` bounds guest-created NESTING DEPTH (only individual segment
shape) — so a guest can legitimately `mkdir` an arbitrarily deep tree via ordinary, individually
harmless calls, then trip this. The round built a real 50,000-level-deep directory tree via fd-relative
`mkdirat`/`openat` (matching how the function itself walks) and reproduced: under the default
`ulimit -n`, `EMFILE`, denying a legitimate operation for no adversarial reason; under a raised
(realistic, container-plausible) `ulimit -n`, a **segfault, reproduced twice**.

**Fixed** by converting `accumulate_usage()` to an explicit stack of mount-relative PATH STRINGS (not
open fds or a recursive call), each re-verified via `open_within_mount_root` only when popped and
closed again before the next pop — at most ONE open directory handle at any time, matching the Windows
sibling's own documented invariant exactly, bounded by directory-tree BREADTH (ordinary, self-limiting
disk usage) rather than nesting depth. Re-verified after the fix, empirically, not just re-read:
rebuilt and reran the identical 50,000-level-deep-tree repro — no crash, no fd exhaustion, a clean
`ENAMETOOLONG` result (the path-string approach's own new, and acceptable, boundary — a real kernel
`PATH_MAX` limit, fails closed with an ordinary error rather than crashing); a second run at a
realistic-but-still-deep 500 levels confirmed `usage()` still SUCCEEDS cleanly and correctly
(`file_count=0, total_bytes=0` for an all-empty-directories tree), not merely fails safely.

**2. SHOULD-FIX (C15/C17-adjacent parity gap, not a security escape) — `exists()`'s error
classification was narrower than the Windows original.** Only `native_code == ENOENT` was treated as
"doesn't exist" -> `false`; the Windows original treats ANY non-policy/non-contract failure as `false`,
unconditionally. The round compiled and ran both real implementations against the identical scenario
(querying `"blocker/child"` where `"blocker"` already exists as a regular file, not a directory):
Windows returned a clean `false`; Linux propagated a hard error (`ENOTDIR`, `native_code=20`). Not a
security bypass (both fail SAFE, just differently-shaped) but a real "isolation parity is a gate, not
identical shape" (CONVENTIONS.md) violation, in a method that is part of the shipped public
`FileSystemAdapter` contract even though no shell builtin currently reaches it (no `test`/`[` builtin
exists yet in `mediated_shell_dispatch.cpp`). **Fixed**: broadened to match the Windows catch-all
exactly.

**3. SHOULD-FIX, same root cause, lower severity — `make_directory(parents=true)`'s not-found
classification was narrower too.** Only `ENOENT` was accepted as "not found yet"; the Windows original
explicitly checks both `ERROR_FILE_NOT_FOUND` and `ERROR_PATH_NOT_FOUND` (the `ENOTDIR` analog). Traced
the concrete "intermediate segment is a file" scenario on both platforms: both still end up failing
overall either way (Windows via a later `create_one_directory` parent-open failure, POSIX via the
direct probe failure) — the fix makes the ERROR PATH match across platforms, not the final pass/fail
verdict. **Fixed**: also accept `ENOTDIR`.

**Checked, ruled out (traced/rebuilt, not just read):**
- `remove()`'s target/parent TOCTOU (verify-target, verify-parent, then `unlinkat(parent_fd, leaf, ...)`
  against a raw leaf-name string): every constructible race (swap the leaf for a symlink/dir/file
  between the initial `fstat` and the final `unlinkat`) either fails safely (`ENOTDIR`/`EISDIR`/
  `ENOTEMPTY`) or at worst deletes a different entry still strictly inside the already-verified parent —
  never anything outside the mount. `leaf` structurally can never contain `/` (guaranteed by
  `split_leaf`'s own construction), and `unlinkat`/`AT_REMOVEDIR` never dereference a symlink at the
  final path component.
- `create_one_directory`'s NUL-only leaf validation (vs. Windows' NUL+`\`+`:`): confirmed `leaf` can
  never contain `/`, `.`/`..` leaves are explicitly rejected, and the only place a `..`/absolute
  smuggling attempt could hide (`parent`) goes through the shared, already-Judged `validate_segments`.
  No POSIX-specific escape found.
- `worktree_mount_fs_posix.cpp`'s `native_code` addition: `error` is a plain aggregate defaulting
  `native_code` to 0; grepped every existing caller in the tree — none is sensitive to the new trailing
  field. Genuinely additive, as claimed.
- The hostile-corpus platform split: rebuilt and ran the Windows branch for real (29/29 checks, clean
  exit); hand-traced the Linux branch's three claims against the real `create_one_directory`/
  `split_leaf` code (none of the three payloads contain `/`, so each becomes one literal whole-string
  leaf created directly under the mount root) — accurate. Confirmed `sibling_marker`/`temp_dir` are
  scoped inside the `#ifdef _WIN32` block's own braces, never referenced from `#else`.
- CMake `NOT WIN32` branch: read in full; mirrors the `WIN32` branch's target name/sources/includes/
  link-libs exactly; the deliberately-unported test targets remain correctly inside their own
  `if(WIN32)` block; no duplicate targets, no typo'd conditions.

**Minor, noted, not scored** (pre-existing, shared identically by both platforms, not introduced this
session): `create_one_directory` on both platforms treats "already exists" as unconditional success
without checking the existing entry is actually a directory — `mkdir` on a path that already exists as
a *file* silently no-ops instead of erroring. Not attributable to this port; a real, if minor,
pre-existing gap.

## 5. Executed evidence

- Real WSL2 Ubuntu build (kernel 6.6.87.2-microsoft-standard-WSL2, gcc 15.2.0, cmake 4.2.3, ninja
  1.13.2): the six ported test targets compile clean on the FIRST attempt, before any red-team fixes,
  and pass 6/6 for real (0.50-0.56s total across three separate runs — before the fixes, and twice
  after).
- A real, full Linux project build (`ninja -k 0`, keep going past failures): exactly 3 failing targets
  (`test_sandbox_runtime`/`test_mandatory_sandbox_provider`/`test_mandatory_sandbox_provider_composed`),
  all tracing to the SAME, pre-existing, unrelated root cause (`real_io_filesystem.hpp` unconditionally
  `#include`s the Windows-only `worktree_mount_fs.hpp`) — none of these three files were touched this
  session; disclosed in full in §7, not fixed here. A real, full Linux `ctest` run excluding those three
  was 170/170 passing, zero regressions, run three times (once before the fixes, twice after).
- Real MSVC (Windows) build and `ctest` run: the six ported tests still pass 6/6 after every round of
  changes (including after the red-team's fixes, which touched Linux-only code); one full project
  rebuild, clean.
- Real, targeted crash-fix verification (not merely re-reading the fix): a standalone repro, compiled
  against the real built `libagentengine_mediated_shell_runner.a`, built a real 50,000-level-deep
  directory tree and called `usage()` — before the fix, this is the exact shape the red-team round
  segfaulted on; after the fix, it returns a clean `ENAMETOOLONG` error (no crash), and a separate
  500-level-deep run confirms `usage()` still succeeds correctly for a realistic-but-deep tree.

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| C15 — build parity | **CORRECT** | Real WSL2 build, six targets clean, zero new failures anywhere else in the project (confirmed via `ninja -k 0`). |
| C16 — functional parity | **CORRECT** | Real `ctest` run, six tests, 6/6 passing, three separate runs. |
| C17 — containment holds on Linux | **CORRECT** | Red-team's own traced/rebuilt TOCTOU analysis of `remove()`/`create_one_directory`; no escape found. |
| C18 — no new crash/DoS surface | **CORRECT, after one real fix** | `usage()`'s recursion WAS a real, twice-reproduced segfault — fixed, then independently re-verified via a real 50,000-level-deep repro (no crash) and a real 500-level-deep repro (correct success). |

## 7. Residual risks

- **`RealIoFileSystem`/`SandboxRuntime` (ADR-102 Phases 3-4) have NO real Linux parity today,
  discovered as a byproduct of this pass, not fixed here.** `include/agentengine/sandbox/
  real_io_filesystem.hpp` unconditionally `#include`s the Windows-only `core/worktree_mount_fs.hpp`
  (never `<windows.h>`-guarded), so `test_sandbox_runtime`/`test_mandatory_sandbox_provider`/
  `test_mandatory_sandbox_provider_composed` fail to even COMPILE on Linux. This is not a Linux-specific
  regression from anything in this ADR — none of the three broken files were touched this session — it
  is a genuine, previously-undiscovered gap that simply went unnoticed because nobody had attempted a
  real Linux build of this codebase's `sandbox/` tree before now. Despite these tests not being
  `WIN32`-gated in CMake (which would have made the gap visible), the underlying header itself never
  actually supported Linux. Real, contained, but SEPARATE follow-on work: a `real_io_filesystem_posix`
  (or equivalent) implementation, almost certainly following this exact ADR's own pattern (reuse the
  already-Judged `open_within_mount_root` POSIX primitive) — named here for whoever picks it up next,
  not silently left for them to rediscover the hard way.
- **`remove(path, recursive=true)`'s own recursion-depth hazard is the SAME structural class the MUST-FIX
  closed for `usage()`, left unfixed, disclosed.** The Windows original `remove()` this file's own
  `remove()` deliberately mirrors also recurses one C++ stack frame per directory-tree level — not a
  Linux-specific regression, but a real, shared, pre-existing hazard this pass's own MUST-FIX finding
  makes newly credible (rather than theoretical) for BOTH platforms. Converting it needs the identical
  explicit-stack treatment on both the Windows and Linux files for real parity — genuinely out of this
  pass's Linux-parity scope (it would mean editing the already-shipped Windows file too), named as
  real, contained follow-on work. **SINCE FIXED (2026-08-29)**: converted to the identical iterative,
  explicit-path-string-stack pattern on both platforms, independently red-teamed (no new defect found;
  the round additionally closed a Windows-side verification gap left open at fix time, and sharpened —
  without changing — this section's own pre-existing symlink-follow disclosure, now in this function's
  own top-of-file comment).
- **Docker-dependent consumers remain untested on Linux**: `tools/sandboxed_shell_chat.cpp`,
  `tests/test_composed_sandbox_providers_live.cpp`, and the two ADR-096 C2 `compile_fail` probes all
  compose `SandboxToolProvider` alongside `MandatorySandboxProvider<DockerExecutionSurface>`, and this
  session's WSL2 verification environment has no Docker daemon reachable (Docker Desktop's WSL
  integration is not enabled for this distro) — untested on Linux, deliberately not claimed either way.
  **WINDOWS SIDE SINCE VERIFIED (2026-08-29)**: once the project owner started Docker Desktop on this
  host, `test_composed_sandbox_providers_live` (the real `ComposedContextProvider<SandboxToolProvider,
  MandatorySandboxProvider<DockerExecutionSurface>>` composition) ran for real against a live daemon
  and passed — the full Windows suite went 287/287, 100%, for the first time in this whole multi-phase
  effort's history. The Linux half of this residual (WSL2 Docker integration) remains open — a Docker
  Desktop setting outside this session's control, not a code gap.
- **`test_mediated_shell_runner_no_process_creation`'s own Windows-specific symbol list** (`CreateProcessA`/
  `CreateProcessW`/etc., checked via `llvm-nm --undefined-only` against the built artifact) needs its
  own, separately-reasoned Linux symbol list (`fork`/`execve`/`posix_spawn`/`vfork`/`clone`) to mean
  anything on that platform — a real, bounded, but genuinely separate task (which exact libc/syscall
  wrapper symbols constitute "process creation" on Linux is its own judgment call), not attempted here.
- **`create_one_directory`'s EEXIST-without-type-check gap** (§4's "minor, noted, not scored" finding):
  pre-existing on both platforms, not introduced or worsened by this pass, named for the record.
- **The embedded-CPython Python-runner composition tests** (`AGENTENGINE_BUILD_PYTHON_RUNNER`) remain
  entirely out of scope — a materially larger, unrelated platform-parity question.
