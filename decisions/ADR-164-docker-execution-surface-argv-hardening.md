# ADR-164 — Does porting `DockerCliBackend` onto a real argv vector (never a host shell string) close issue #50 without regressing anything the old shell-quoting denylist protected?

- **Status:** Proposed — implemented, verified by real test execution against a live Docker Desktop
  daemon, independently red-teamed (see §5: no CWE-88 bypass, no regression against the old denylist's
  actual protections; one real pre-existing OS-ceiling disclosure and one real-but-unreachable
  consistency gap found and fixed), and independently code-reviewed (see §5a: one real, live
  Windows-path-encoding regression found and fixed, with a new positive-control test), pending
  project-owner sign-off.
- **Date:** 2026-09-02.
- **Scope:** `include/agentengine/sandbox/docker_execution_surface.hpp` (modified — `DockerCliBackend`'s
  six methods, the validation-helper set, a new `docker_cli_detail::run_argv()` per platform, a new
  `docker_cli_detail::path_to_utf8()` per platform — see §5a), `tests/test_sandbox_runtime.cpp`
  (modified — one test scenario updated to match the new, correct behavior),
  `tests/test_docker_run_argv_timeout.cpp` (new, Windows-only), `tests/test_docker_non_ascii_path.cpp`
  (new — §5a's own positive control), `tests/CMakeLists.txt` (additive wiring for both new tests).
  **No other production file changed** — `DockerExecutionSurface`'s own public methods, the
  `ExecutionSurface` concept, and `SandboxRuntime` are untouched.
- **Related specs:** GitHub issue #50 (the defect this closes) ·
  `docs/planning/docker-execution-surface-argv-hardening-design-draft.md` (the design/red-team document
  this ADR promotes) · `include/agentengine/sandbox/containerd_execution_surface.hpp`/`decisions/
  ADR-145-containerd-execution-surface-promotion.md` (the already-shipped argv-vector precedent this
  port follows) · `src/backends/native_process/native_process_spawn.cpp` (the MS-CRT argv-quoting
  algorithm this port's Windows side duplicates, per its own documented reasoning) ·
  `decisions/ADR-104-real-io-filesystem-linux-parity.md`/`ADR-139-docker-run-capture-timeout-and-cap.md`
  (the Job-Object timeout-kill machinery this port reuses, retargeted).

## 1. The question

`DockerCliBackend` (the only one of the three execution surfaces issue #50 named that actually had the
defect — see the design doc's §0 for the code-read that narrowed the issue's own scope) built every
`docker` CLI invocation as a single, host-shell-interpreted command string, defended by a character
denylist that also rejected legitimate agent commands containing `"`/`%`/`^` — a live-tested,
real-user-visible defect, not merely a theoretical one. Does replacing that string-plus-denylist
architecture with a real `std::vector<std::string>` argv, spawned directly (no `cmd.exe`/`/bin/sh` in
front of the outer `docker` invocation), close issue #50 — both the CWE-88 host-injection class and the
over-blocking usability defect — without regressing anything the old denylist actually protected, and
without changing any consumer-visible API?

## 2. Design

Full reasoning, competing-designs comparison (Docker Engine REST API deferred as real future work, a
narrower-denylist patch rejected as fighting the wrong layer), and a first, design-only red-team round
are in `docs/planning/docker-execution-surface-argv-hardening-design-draft.md`. Summary of what was
actually built, in this session, against the real repo:

- **New `docker_cli_detail::run_argv()`, both platforms**, in `docker_execution_surface.hpp`. POSIX:
  `posix_spawnp()` over a real argv vector (mirroring `ctr_cli_detail::run_argv()`,
  `containerd_execution_surface.hpp:85-213`, but merging stdout+stderr into one stream to match
  `SurfaceRunOutcome`'s existing single-field shape). Windows: `CreateProcessW` with
  `lpApplicationName = nullptr` (PATH search, matching `posix_spawnp()`'s own posture — not a new grant
  of ambient authority, see the design doc's own red-team Finding 2), command line built by a
  locally-duplicated copy of the MS-CRT argv-quoting algorithm (`quote_one_argument`/
  `build_command_line`, same algorithm as `native_process/native_process_spawn.cpp`'s own, duplicated
  rather than linked against because that file's own build target is gated behind the opt-in,
  off-by-default `AGENTENGINE_WITH_NATIVE_PROCESS` option — linking against it would make
  `DockerExecutionSurface`'s always-built header silently depend on an unrelated capability class), and
  the SAME Job-Object timeout-kill machinery `run_capture()` already carried (ADR-104/139), retargeted
  to bind the now-top-level spawned process (`docker.exe` itself) directly instead of `cmd.exe`.
- **Every `DockerCliBackend` method ported**: `create()`, `copy_to_container()`, `copy_from_container()`,
  `exec()`, `destroy()`, `reap_orphans()` all build a real argv vector and call `run_argv()` instead of
  `run_capture(std::ostringstream(...).str())`. `run_capture()` itself is KEPT, unchanged — real test
  files (`tests/test_docker_orphan_reap.cpp`, `tests/test_sandbox_runtime.cpp`) call it directly with
  static, non-attacker-influenced strings for host-side setup/assertions outside the code path under
  test — but it is no longer used by any production call site in this file.
- **Validation collapsed and unified**: `docker_cli_reject_unsafe_for_shell`/
  `docker_cli_reject_unsafe_for_unquoted_arg`/`docker_cli_reject_shell_breakout`/
  `docker_cli_win_double_trailing_backslashes` (all four, both platform variants) are REMOVED —
  none of them were still a real defense once there is no host shell for the caller-
  supplied value to break out of. What remains, unified (no `#ifdef` needed — the risk was never
  platform-specific): `docker_cli_reject_embedded_nul()` (NUL-byte only, applied to `exec()`'s `command`,
  matching `ctr_cli_detail::reject_embedded_nul()`'s own identical, already-shipped posture) and
  `docker_cli_reject_argv_value()` (NUL + leading-dash + empty, applied to `image`/`host_path`/
  `container_path` — leading-dash is a real, still-relevant `docker` CLI flag-injection concern
  independent of any shell; empty/NUL are fail-fast correctness checks, no longer security boundaries).
- **One test updated to match the new, correct behavior**: `tests/test_sandbox_runtime.cpp`'s check [6]
  used to assert that `echo "this double-quote trips the shell guard"` gets REJECTED — that assertion
  encoded exactly the over-blocking defect issue #50 reports. Replaced with a command containing an
  embedded NUL byte (the one check that survives), preserving the same underlying invariant under test
  (RunCost is refunded when `run()` is rejected before ever attempting the command).
- **New test, `tests/test_docker_run_argv_timeout.cpp`** (Windows-only): directly re-runs the historical
  `cmd.exe`-orphaning scenario (`docker exec <id> sh -c "tail -f /dev/null"` under a short timeout)
  against the new, `cmd.exe`-free `run_argv()` path, against a real Docker Desktop daemon.

## 3. Falsifiable claims

| # | Claim |
|---|-------|
| C1 | Every `DockerCliBackend` method builds a real argv vector for the OUTER `docker` invocation — no caller-supplied value is ever concatenated into a string a host shell re-parses. `exec()`'s `command` reaches the CONTAINER's own inner `sh -c` as ONE literal argv element. |
| C2 | A command containing `"`, `%`, `^`, backtick, or `$(...)` — the exact class issue #50 reports as wrongly blocked — is no longer rejected by `DockerCliBackend::exec()`. |
| C3 | The full, real, live-Docker-backed regression suite (`test_docker_orphan_reap`, `test_sandbox_runtime`, `test_composed_sandbox_providers_live`, `test_mandatory_sandbox_provider`, `test_task_branch_tools`, `test_task_branch_concurrent_dispatch`) passes unchanged in behavior, with `test_sandbox_runtime`'s one intentionally-updated scenario the only test-source change. |
| C4 | Removing the `cmd.exe` layer from the Windows spawn path does not weaken the Job-Object timeout-kill guarantee ADR-104/139 established — a non-terminating `exec()` command is still killed on timeout with no orphaned host `docker.exe` process left behind. |
| C5 | No consumer-visible API changed: `DockerExecutionSurface`'s public methods, the `ExecutionSurface` concept, `SandboxRuntime<Surface>`, and the three real tool binaries that `#include` this header (`agentengine_cli_chat`, `agentengine_sandboxed_shell_chat`, `agentengine_durable_sandboxed_shell_chat`) are all unaffected. |

## 4. Executed evidence

- **C1/C2:** confirmed by direct code read of every `DockerCliBackend` method
  (`docker_execution_surface.hpp`, `class DockerCliBackend`) — every subcommand is built as a
  `std::vector<std::string>` literal, no `std::ostringstream`/string concatenation remains in any
  production call site. `docker_cli_reject_embedded_nul()` is the only check left on `command`; it does
  not reference `"`, `%`, `^`, backtick, or `$` at all.
- **C3:** built (`cmake --build build --config Debug --target <name>`, Visual Studio 18 2026/MSVC
  generator) and run for real against a live Docker Desktop daemon on Windows (`docker version 29.7.2`):
  ```
  test_docker_orphan_reap:                    14/14 checks, 0 failed
  test_sandbox_runtime:                       ALL CHECKS PASSED
  test_composed_sandbox_providers_live:       ALL CHECKS PASSED
  test_mandatory_sandbox_provider:            ALL CHECKS PASSED
  test_task_branch_tools:                     ALL CHECKS PASSED
  test_task_branch_concurrent_dispatch:       ALL CHECKS PASSED
  ```
  All six built with zero compiler warnings/errors on the first pass after the port. The one
  intentional test-source change (`test_sandbox_runtime.cpp` check [6]) is documented in §2 above and
  in that file's own updated top comment/inline comments.
- **C4:** `tests/test_docker_run_argv_timeout.cpp`, new, run against the same live daemon:
  ```
  === run_argv() timeout-kill leaves no orphaned docker.exe (no cmd.exe layer) ===
  [ok]   CreateToolhelp32Snapshot() succeeds
  [ok]   create() succeeds
  [info] run_argv() returned after 3030 ms (exit_code=-1)
  [ok]   run_argv() returned promptly, proving the Job-Object timeout kill fired
  [ok]   timed-out run_argv() reports exit_code == -1
  [info] docker.exe-family processes: before=0, after timeout-kill=0
  [ok]   no extra orphaned docker.exe-family process left behind by the timeout kill
  [ok]   destroy() cleanup succeeds
  === 6 checks, 0 failed ===
  ```
  A 3-second timeout against a genuinely non-terminating `tail -f /dev/null` returned in ~3.0s (not the
  real 30s default), proving the Job Object kill actually fired rather than the call finishing
  naturally, with zero `docker.exe`/`com.docker.cli.exe` processes observed before and after via a real
  `CreateToolhelp32Snapshot()` process enumeration.
- **C5:** `agentengine_cli_chat`, `agentengine_sandboxed_shell_chat`, `agentengine_durable_sandboxed_shell_chat`
  (the three real tool binaries `#include`-ing this header, `AGENTENGINE_WITH_HTTPS`/
  `AGENTENGINE_BUILD_PYTHON_RUNNER` both ON in the build tree used) all rebuilt cleanly with zero
  changes required at their own call sites. `grep` confirms no other production file references any of
  the four removed validation functions (only historical comments in
  `containerd_execution_surface.hpp` and this file's own top comment mention their old names, both
  descriptive text, not live dependencies).

## 5. Red-team round

An independent, fresh `general-purpose` subagent (zero context from the session that made this port)
re-attacked it against the real repo and a real, live Docker Desktop 4.89 daemon directly — read
`docker_execution_surface.hpp` in full (1245 lines) against `containerd_execution_surface.hpp`'s shipped
precedent and `native_process_spawn.cpp`'s quoting algorithm, rebuilt and re-ran all 7 real test targets
independently, then wrote and ran three standalone probes issuing real `docker create/exec/cp/destroy`
calls:

- **Real-daemon test re-verification**: all 7 targets rebuilt clean (zero warnings) and passed for
  real, matching §4's own claimed numbers exactly (`test_docker_orphan_reap` 14/14;
  `test_sandbox_runtime`/`test_composed_sandbox_providers_live`/`test_mandatory_sandbox_provider`/
  `test_task_branch_tools`/`test_task_branch_concurrent_dispatch` all "ALL CHECKS PASSED";
  `test_docker_run_argv_timeout` 6/6, 3017ms timeout-kill, 0 orphaned `docker.exe` before/after).
- **Quoting-algorithm diff**: `docker_execution_surface.hpp`'s `quote_one_argument()`/
  `build_command_line()` confirmed BYTE-FOR-BYTE identical logic to `native_process_spawn.cpp`'s own —
  only comments differ. Not merely claimed, directly diffed.
- **Adversarial probes, real containers**: `exec()` with a command containing literal `"`, `%`, `^`,
  backtick, `$(id)` — ran unmodified inside the container, every character survived verbatim (C2
  confirmed for real, not just by code inspection). Embedded-NUL command cleanly rejected
  (`docker_cli_backend.unsafe_argv_value`). `image = "--privileged"` and `image = "-"` both cleanly
  rejected. Real Windows drive-letter host paths (`C:\Users\...`, including a trailing backslash)
  through both `copy_to_container()`/`copy_from_container()` round-tripped correctly — no `ctr
  --mount`-style comma/colon grammar problem exists for `docker cp` (not a comma-delimited flag
  grammar the way `ctr --mount` is). UTF-8 (CJK + emoji) through `exec()` round-tripped correctly.
  Embedded newline in `command` — both lines ran inside the container's own `sh`, no host effect. A
  command that is itself flag-shaped (`echo -n ...`) ran fine, correctly not confused with a `docker`
  flag (it is `sh -c`'s own positional argument, never something `docker`'s own parser scans).
- **Finding A — real, PRE-EXISTING (not a regression introduced by this port), disclosed not blocking.**
  A command whose total quoted command line exceeds Windows' own ~32K `CreateProcessW` limit fails
  outright with `exit_code == -1` and no distinguishing error. Bisected: passes at 32013 chars, fails at
  40013. Verified the OLD `cmd.exe`-prefixed path hit the identical OS ceiling at an even SMALLER
  effective size (the `"cmd.exe /c "` prefix ate into the same budget) — not a regression. **Disclosed**
  in the Windows `run_argv()`'s own header comment (docker_execution_surface.hpp) and §7 below.
- **Finding B — real, but not currently reachable; a defense-in-depth consistency gap, since fixed.**
  `create()`'s own returned `container_id` was used as a bare/prefix argv element at every downstream
  call site (`exec()`/`destroy()`/`copy_to_container()`/`copy_from_container()`) WITHOUT going through
  `docker_cli_reject_leading_dash()` the way `image`/`host_path`/`container_path` already do. Not
  reachable today (`docker run -d`'s own documented output contract guarantees a hex id, which can
  never start with `-`, and the value is never attacker/model-influenced), but a real inconsistency
  against this file's own stated "every value that becomes a bare/flag-position argv element gets
  checked" principle. **Fixed same session**: `create()` now checks `container_id` once, at the source,
  before returning `Instance` — covers all four downstream call sites by construction. Re-verified: all
  7 targets above still pass unchanged after the fix.
- **False alarm ruled out**: `docker_cli_reject_leading_dash()` applied to `container_path` in
  `copy_to_container()`/`copy_from_container()` checks the raw value, but the actual argv element is
  always `container_id + ":" + container_path`, which can never start with `-` regardless — the check
  is inert there, not unsafe, just scoped to a value distinct from the literal argv element it
  ultimately becomes part of.
- **Comment audit**: no stale/contradicted historical comments found — every reference to the four
  removed validation functions is framed correctly as "was removed", never as live code.
- **Overall verdict**: "the port holds up under adversarial review. No CWE-88 bypass, no crash/hang/
  leak, no old-denylist-era protection regressed — every metacharacter class the old denylist wrongly
  blocked now runs correctly and safely." Findings A and B are real but neither is attacker-reachable
  nor a regression; both disclosed (B additionally fixed) before Judge sign-off.

## 5a. Independent code review round (`/code-review`, real live-Windows encoding bug found and fixed)

A separate, independent `/code-review` pass over the full working-tree diff (not the §5 subagent —
a distinct review, run after §5, against the code §5 had already approved) traced the encoding contract
of every value fed into the new `run_argv()` and found a real, live, previously-undetected regression:

- **Finding — real, confirmed by direct analysis, not a false alarm.** `copy_to_container()`/
  `copy_from_container()` built their argv element from `host_path.string()`. On Windows,
  `std::filesystem::path::string()` narrows via the process's ACTIVE CODE PAGE (`GetACP()`) — on the
  machine this port was built and tested on, confirmed via `[System.Text.Encoding]::Default.CodePage`
  to be **1252** (Windows-1252), NOT UTF-8 (the console's own OEM code page happened to read 65001,
  which is a DIFFERENT setting — `chcp` vs `GetACP()` — and does not affect this). The new Windows
  `run_argv()`'s own `widen()` explicitly treats its input as UTF-8. The OLD `run_capture()`/
  `CreateProcessA` path was internally ANSI-consistent (an ACP string handed to an ANSI API) — this
  port's own switch to `CreateProcessW`/`widen()` made the mismatch real for the first time: any
  `host_path` containing a non-ASCII byte would silently mis-decode (`MultiByteToWideChar(CP_UTF8, ...)`
  with `dwFlags=0` does not hard-fail on ill-formed sequences, it substitutes/garbles), corrupting the
  generated `docker cp` argv element. This codebase already has the correct, established fix for this
  EXACT class of bug elsewhere (`native_process/native_providers.hpp`'s own `detail::narrow()`, paired
  with converting from a path's `wstring()`, never its ACP-narrowed `string()`) — this port's Windows
  side just hadn't reused that pattern.
- **Fixed same session**: new `docker_cli_detail::path_to_utf8()` (both platforms — Windows converts
  `p.native()`/`wstring()` via `WideCharToMultiByte(CP_UTF8, ...)`, matching `widen()`'s own documented
  contract exactly; POSIX is a pure passthrough, since POSIX paths have no ACP-vs-UTF-8 distinction to
  begin with). `copy_to_container()`/`copy_from_container()` now validate and embed the SAME
  UTF-8-correct bytes, both call sites.
- **New, permanent positive-control test**: `tests/test_docker_non_ascii_path.cpp` — a real host
  directory named with Vietnamese, CJK, and an emoji character, round-tripped through BOTH
  `copy_to_container()` (verified via a real `exec()` read-back) and `copy_from_container()` (verified
  via reading the drained file back off real disk), against the same live Docker Desktop daemon. **9/9
  checks passed** after the fix. The active code page check above (1252, not UTF-8) confirms this test
  genuinely exercises the mismatch on this machine, not a latent no-op.
- **Re-verification**: all 8 real Docker-backed tests (`test_docker_orphan_reap`,
  `test_sandbox_runtime`, `test_composed_sandbox_providers_live`, `test_mandatory_sandbox_provider`,
  `test_task_branch_tools`, `test_task_branch_concurrent_dispatch`, `test_docker_run_argv_timeout`, and
  the new `test_docker_non_ascii_path`) rebuilt clean and passed after this fix; the three real tool
  binaries rebuilt with zero errors.

## 6. Decision

Port `DockerCliBackend` onto the real argv-vector discipline `ContainerdCliBackend`/`KataBackend`
already established in shipped, Judged-track code. This closes issue #50's actually-reachable defect
(the Windows/POSIX host-shell-string CWE-88 surface, and the exact live-tested over-blocking symptom)
at its structural root rather than patching the denylist, with zero new dependency, zero new transport,
and zero change to any consumer-facing API — `MandatorySandboxProvider<DockerExecutionSurface>` and
every tool built on it work unchanged, now against a strictly narrower, more accurate defense surface.

## 7. Residual risks

- **Design A (Docker Engine REST API / containerd gRPC) is deliberately deferred, not built.** This ADR
  closes the CWE-88 host-injection class at the CLI-invocation layer; a future strength-tiered
  capability model unifying Docker/Kata behind one structured transport remains real, disclosed future
  work (design doc §2 Design A), not a precondition this ADR waited on.
- **`create()`'s inner `sh -c "mkdir -p /workspace && sleep infinity"` is still a shell string** — by
  design, not oversight: it is the CONTAINER's own inner shell reading it as one argv element, the same
  accepted risk layer `ContainerdCliBackend`/`KataBackend` already carry for their own inner-shell
  commands, never a host-shell-parsed string.
- **POSIX side of this port has not been built or run** — this repo's own established, disclosed
  posture for Linux-only/cross-platform gaps when no Linux environment is available in-session (matching
  `AGENTENGINE_WITH_NATIVE_PROCESS`'s own identical disclosed POSIX gap). The POSIX `run_argv()` is a
  close structural mirror of `ctr_cli_detail::run_argv()` (already-shipped, Linux-proven code), reducing
  but not eliminating this risk. A future Linux CI/dev-environment pass should build and run the full
  Docker-backed suite there before this ADR is treated as fully cross-platform-proven.
- **The container-orphan-on-abrupt-host-death residual `DockerExecutionSurface`'s own header comment
  already discloses (ADR-104/145) is unchanged by this port** — `reap_orphans()` still exists as the
  same explicit, caller-invoked maintenance operation it always was; nothing here narrows or widens it.
- **Red-team Finding A (pre-existing, not a regression): a command whose total quoted `docker`
  command line exceeds Windows' own ~32K `CreateProcessW` limit fails silently** — `exit_code == -1`,
  empty output, indistinguishable from any other spawn failure. Empirically bisected (passes at 32013
  chars, fails at 40013) and confirmed the OLD `cmd.exe`-prefixed path hit the identical OS ceiling at
  an even SMALLER effective size, so this is a pre-existing platform ceiling neither version ever
  surfaced distinctly, not something this port introduced. Disclosed in `run_argv()`'s own Windows-side
  header comment. A future pass wanting a distinguishing error here would need `CreateProcessW`'s own
  `GetLastError()` value threaded through `SurfaceRunOutcome`, which today only carries `exit_code`/
  `stdout_text` — a real, larger API change, not attempted in this ADR.
- **Red-team Finding B, fixed same session**: `create()`'s returned `container_id` now passes through
  `docker_cli_reject_leading_dash()` before being returned, closing a real-but-unreachable consistency
  gap (see §5) at its source rather than leaving it implicit in `docker run -d`'s own output contract.
