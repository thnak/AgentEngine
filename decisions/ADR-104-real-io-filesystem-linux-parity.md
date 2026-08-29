# ADR-104 — Does `RealIoFileSystem`/`DockerExecutionSurface` (ADR-102 Phase 3) port cleanly to Linux, closing the gap ADR-103 §7 disclosed but deliberately left out of scope, without opening a POSIX-specific host command-injection hole in the process?

- **Status:** Proposed — implemented and independently red-teamed (2026-08-29), real builds and real
  test runs on both platforms (Windows/MSVC and WSL2 Ubuntu/gcc). Two real, empirically-proven
  findings from the independent red-team round, both pre-existing on Windows too (not introduced by
  this port), fixed on both platforms same day and re-verified. Zero regressions on either platform's
  full test suite, confirmed before and after the fixes.
- **Date:** 2026-08-29.
- **Scope:** `include/agentengine/sandbox/real_io_filesystem.hpp` (made portable: `write_verified()`/
  `read_verified()` split from inline-in-header to platform-specific out-of-line definitions), new
  `src/sandbox/real_io_filesystem.cpp` (Windows implementation, moved verbatim) and
  `src/sandbox/real_io_filesystem_posix.cpp` (new Linux implementation), `include/agentengine/sandbox/
  docker_execution_surface.hpp` (made portable in place: `_popen`/`_pclose` → `#ifdef`-selected
  `popen`/`pclose`; new POSIX-specific shell-injection defenses), CMake wiring for a new
  `agentengine_sandbox_io` library (both platforms) and its six real consumer targets.
  **Excludes**: `ContainerdExecutionSurface`/ADR-101 promotion; `remove(recursive=true)`'s own
  recursion-depth hazard (ADR-103 §7, still open, unrelated to this ADR's files); Docker-DAEMON-level
  runtime verification of the ported code on Linux (no Docker daemon/binary reachable in this
  session's WSL2 environment — the exact same disclosed gap ADR-103 §7 named for
  `tools/sandboxed_shell_chat.cpp`/`test_composed_sandbox_providers_live`, now additionally true for
  `test_sandbox_runtime`/`test_mandatory_sandbox_provider` specifically because THIS ADR makes them
  compile on Linux for the first time — compiled and standalone-proven, not Docker-daemon-proven).
- **Related specs:** `decisions/ADR-103-sandbox-tool-provider-linux-parity.md` (§7's own "Real,
  contained, but SEPARATE follow-on work" naming of exactly this gap; same reused pattern:
  `open_within_mount_root`'s already-Judged POSIX half, portable-declaration/two-platform-`.cpp`
  split) · `decisions/ADR-014-worktree-mount-path-canonicalization.md` (Judged; the containment
  primitive `write_verified()`/`read_verified()` build on, unchanged by this ADR) ·
  `decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md` (Phase 3, the ADR that
  originally shipped `RealIoFileSystem`/`DockerExecutionSurface` as Windows-only) ·
  `CONVENTIONS.md` ("isolation parity is a gate, not identical shape" — the POSIX shell-injection
  defenses below are the clearest instance of this principle in either ADR-103 or this one: same
  CONTRACT, deliberately different MECHANISM, because the interpreting shell genuinely differs).

## 1. The question

Can the two remaining pieces ADR-103 §7 named but explicitly left unfixed —
`RealIoFileSystem::write_verified()`/`read_verified()` (real, verified-handle-based host I/O) and
`DockerCliBackend`/`DockerExecutionSurface` (a real `docker` CLI wrapper shelling out via `popen`) —
actually build and run correctly on Linux, and can the platform-specific shell-out mechanism be ported
without silently trading the Windows `cmd.exe`-tuned host-injection defense for a POSIX `/bin/sh`
attack surface it was never designed to cover?

**Disproof, if true, of "yes cleanly":** a real Linux build either fails to compile; or a
guest/model-reachable value can break out of the generated shell command and run attacker-controlled
code on the HOST (not merely inside the container) on Linux even though the equivalent Windows value
is correctly rejected; or the port introduces a regression on Windows (which was NOT supposed to
change behavior at all for its own, already-shipped half).

## 2. Design

**Scoping.** Following the real Linux build ADR-103 performed (`ninja -k 0`, full project), exactly
three targets failed to even compile: `test_sandbox_runtime`, `test_mandatory_sandbox_provider`,
`test_mandatory_sandbox_provider_composed`. All three trace through
`include/agentengine/sandbox/real_io_filesystem.hpp`'s unconditional `#include
"agentengine/core/worktree_mount_fs.hpp"` (Windows-only, pulls in `<windows.h>`), exactly as ADR-103
§7 predicted. Attempting the mechanical fix (split `write_verified()`/`read_verified()` per platform,
the only two methods on that class touching Win32 directly — everything else is already portable
`std::filesystem`/`Ledger`/`AsyncMutex` code, unchanged) surfaced a SECOND, un-predicted blocker one
`#include` boundary away: `docker_execution_surface.hpp`'s `docker_cli_detail::run_capture()` calls
Windows CRT `_popen`/`_pclose` directly, which don't exist on Linux at all — this file is a real
consumer of the same three test targets (`#include`d directly by their own `.cpp` files, not via
`real_io_filesystem.hpp`).

**`RealIoFileSystem` split** followed ADR-103's own established pattern exactly: portable declaration
in the header (now with NO platform-specific `#include` at all — neither Windows' `worktree_mount_fs.hpp`
nor Linux's `worktree_mount_fs_posix.hpp` leaks into the header; each platform `.cpp` includes only
its own), Windows body moved verbatim into `src/sandbox/real_io_filesystem.cpp` (byte-identical logic,
zero behavior change), new `src/sandbox/real_io_filesystem_posix.cpp` built on
`open_within_mount_root`'s already-Judged POSIX half (ADR-014 Phase C4) plus `::write`/`::fstat`/
`::read` with EINTR-retry and short-transfer loops (POSIX's `write`/`read` are not guaranteed
full-transfer the way `WriteFile`/`ReadFile` are). `O_CREAT|O_TRUNC|O_WRONLY` is the POSIX analogue of
`GENERIC_WRITE`+`CREATE_ALWAYS` — same "always start from a clean, empty file" contract.

**`docker_execution_surface.hpp`'s shell-out mechanism was made portable IN PLACE** (`#ifdef _WIN32`/
`#else` inside the one file, not a new platform-split `.cpp`, since every affected function is a
small, header-only `inline` helper): `run_capture()` selects `_popen`/`_pclose` vs. `popen`/`pclose`.
The harder part is the shell-injection defense: the Windows character blacklist
(`"&|<>^%\r\n"` for `image`/`host_path`/`container_path`; `"%^\r\n"`... plus `"` for `command`) is
explicitly tuned for `cmd.exe`'s grammar. `popen`/`pclose` on Linux invoke `/bin/sh` — a
structurally different shell (backtick/`$()` command substitution, `;` sequencing with no `cmd.exe`
analogue) — so reusing the Windows blacklist verbatim would have been a real, silent command-injection
regression, not a parity win. Two NEW, POSIX-specific functions were written instead, keyed to how
each value is actually embedded in the generated string:
- `docker_cli_reject_unsafe_for_shell` (POSIX): a POSITIVE ALLOWLIST (alnum + `. _ - / : @`) for
  `image`/`host_path`/`container_path`, all of which are interpolated UNQUOTED into the generated
  `sh -c` string at least once — an unquoted POSIX shell word treats almost every non-alphanumeric
  character as meaningful, so a blacklist of "every dangerous character" is the wrong shape here; an
  allowlist matching Docker's own reference-name grammar is the right one (CLAUDE.md: "security
  claims need positive controls").
- `docker_cli_reject_shell_breakout` (POSIX): a blacklist of exactly `" $ `` \ \r \n` for `command`
  (exec()'s own argument, embedded inside escaped double quotes) — inside POSIX double quotes, ONLY
  those four characters retain shell meaning, so this stays a blacklist (not an allowlist) precisely
  so `command` can keep using `&|<>(){}` for the CONTAINER's own inner `sh`, matching the Windows
  version's original intent (reject only what breaks the OUTER quoting, not everything).

## 3. Falsifiable claims

- **C19 (build parity).** `real_io_filesystem.hpp`/`docker_execution_surface.hpp` and every real
  consumer target (`agentengine_cli_chat`, `agentengine_sandboxed_shell_chat`, `test_sandbox_runtime`,
  `test_mandatory_sandbox_provider`, `test_mandatory_sandbox_provider_composed`,
  `test_composed_sandbox_providers_live`) compile and link cleanly on real Linux, with zero new
  failures anywhere else in the project and zero regression on Windows. *Disproof: a compile/link
  failure in this chain on either platform, or a NEW failure elsewhere.*
- **C20 (RealIoFileSystem POSIX I/O correctness).** `write_verified()`/`read_verified()` on Linux
  perform a real, byte-exact round trip (including null/high bytes and empty files), genuinely
  truncate on overwrite, and reject both a lexical `..` escape and a REAL symlink-based escape via
  `open_within_mount_root`. *Disproof: a byte mismatch, a stale-data leak past truncation, or a
  successful escape.*
- **C21 (POSIX host-shell injection is actually blocked, not merely believed to be).** No value
  reachable through `DockerCliBackend`'s public API can cause the HOST's `/bin/sh` (invoked via
  `popen`) to execute attacker-chosen code, when the guard functions run — proven with a POSITIVE
  control (the guard rejects real injection payloads) AND a NEGATIVE control (the identical payload,
  with the guard bypassed, actually achieves code execution on this platform) — the negative control
  is what makes the positive control meaningful rather than assumed. *Disproof: a payload that passes
  the guard and achieves host code execution.*
- **C22 (no Windows regression).** The Windows half of both files (byte-identical `write_verified()`/
  `read_verified()`; the unchanged `_popen`-based blacklist functions) produces IDENTICAL behavior to
  before this ADR, for every existing Windows caller. *Disproof: any Windows test that passed before
  this ADR and fails after, or a documented behavior change for a Windows-only caller.*

## 4. The red-team attack (2026-08-29, independent, fresh `general-purpose` subagent, not a fork)

Given the same "empirically prove, don't just read" brief as ADR-103's own round, and full real
build/shell access on both platforms plus the prior session's own already-compiled proofs to
independently re-verify (not just trust).

**1. REAL, EMPIRICALLY PROVEN — leading-dash flag/argument injection (CWE-88), pre-existing on BOTH
platforms, not introduced by this port.** Neither the Windows blacklist nor the new POSIX allowlist
anchors "must not start with `-`": a value that is entirely allowlisted/non-blacklisted characters
(e.g. `--privileged`) passes cleanly and is emitted as a bare, unquoted token positioned exactly where
the real `docker` CLI parses its OWN flags (`docker run -d --rm -w /workspace <image> ...`) — a
different, in some ways worse class of bug than a shell-quoting escape, since it needs no shell
metacharacter at all. Proven two ways: against the real `docker.exe` CLI client on the Windows host
(daemon unreachable, but CLI flag-parsing happens before the daemon dial) — `docker run ... --privileged
...` proceeds past arg-parsing to a daemon-dial error, while a bogus `--not-a-real-flag` is rejected
immediately with `unknown flag`, proving `--privileged` really is parsed as a flag, not a value; same
pattern reproduced against `docker cp --archive ...` for `host_path`/`copy_to_container()`. And
directly against the real, compiled `DockerCliBackend::create()` in WSL2 with a PATH-shimmed `docker`
logging its own `argv`: `create("--privileged")` was accepted by the (pre-fix) guard and the shim's
log showed `--privileged` landing in the argument slot docker itself would parse as a flag.
**Reachability today: NOT live** — the one production caller, `MandatorySandboxProvider::bind_sandbox()`,
always hardcodes `image="alpine:latest"`/`container_path="/workspace"` and derives `host_path` from
`std::filesystem::temp_directory_path()`, never from attacker/model input — but this is a shared,
reusable function whose own comment claimed to match Docker's real reference grammar, which does
require an alnum start; fixed anyway, on both platforms, since a future caller making these values
configurable would otherwise inherit a live vulnerability silently.

**Fixed** by a new, shared `docker_cli_reject_leading_dash()` (rejects a leading `-` specifically —
NOT "must start with alnum", which would have broken the real, legitimate `/workspace`-shaped
`container_path`/`host_path` values that also flow through the same function), wired into
`docker_cli_reject_unsafe_for_shell` on BOTH the Windows and POSIX branches. Re-verified: the
standalone repro now shows `--privileged`/`-v` rejected, while `/workspace` and `alpine:latest` still
pass (an explicit regression check, since the first version of this fix — requiring an alnum start —
was caught locally, before ever reaching the red-team, as breaking exactly this case).

**2. REAL, low-impact, empirically proven non-exploitable — embedded NUL truncation in the `command`
blacklist.** `docker_cli_reject_shell_breakout` never rejected an embedded NUL byte; since every
built command string is eventually passed through `.c_str()` into `popen`/`_popen`, a NUL silently
truncates the string the SHELL actually sees, which can differ from what the guard approved. Proven,
via a compiled repro, that this specific gap is NOT exploitable for RCE against the one real caller
(`command`): a NUL always lands strictly inside the still-open `sh -c "..."` region this file
constructs, so the truncated string always has an unterminated double quote, and `dash` (confirmed via
`readlink -f /bin/sh` on the target WSL2 distro) fails closed with a syntax error rather than running
anything. **Fixed anyway**, as defense-in-depth (a value the guard "approved" silently differing from
what actually ran is a real correctness gap independent of whether today's specific caller happens to
make it harmless) — `docker_cli_reject_chars()` now rejects any embedded NUL, once, for every caller
on both platforms, before the existing character-set check runs.

**Checked, ruled out (traced/proven, not just read):**
- The one payload class that DOES pass `docker_cli_reject_shell_breakout` (bare `;`, no quote-breaking
  character) was driven, unguarded, directly through the real `popen()`-based `run_capture()` — no
  host-side marker file was created, confirming it really is inert to the HOST shell as intended
  (its only effect is inside the CONTAINER's own inner `sh`, which is `command`'s whole purpose).
- POSIX double-quote semantics (only `` ` ``, `$`, `"`, `\` retain meaning inside double quotes) were
  checked against the real interpreting shell on the target system (`dash`, not `bash` — matters for
  exact lexer behavior), not assumed from POSIX spec text alone.
- The `"cd /workspace && " + command` prefix `DockerExecutionSurface::run()` adds, and the `" 2>&1"`
  suffix `run_capture()` adds, were both checked for reopening a bypass — neither does; both are
  applied around/after the guard's own check, outside the double-quoted region `command` itself sits
  in.
- CMake completeness: every real `#include` site of both headers across the whole tree (excluding
  `docs/planning/proofs/`, not built by the main CMake tree) was grepped and cross-checked against
  every consuming target's `target_link_libraries` — all six real consumers correctly link
  `agentengine::sandbox_io` on both platforms; no missing link site found.
- `RealIoFileSystem` POSIX I/O itself: EINTR-retry correctness on both `write`/`read` loops, no TOCTOU
  reintroduced (`fstat`+`read` act on the same already-verified fd from `open_within_mount_root`,
  never a re-parsed path), `SafeFileHandlePosix` RAII closes on every error path, short-read handling
  fails closed on concurrent truncation without a buffer overrun on concurrent growth — no defect
  found.

## 5. Executed evidence

- Real Windows/MSVC full rebuild: 287/287 targets link clean (0 errors), both before and after the
  red-team's two fixes. Full `ctest` run: 284/287 passing both times; the 3 failures
  (`test_composed_sandbox_providers_live`/`test_sandbox_runtime`/`test_mandatory_sandbox_provider`) are
  the pre-existing, explicitly-documented "REQUIRES a running Docker daemon" tests — confirmed via
  `docker version` that Docker Desktop's daemon is unreachable on this host (`open
  //./pipe/dockerDesktopLinuxEngine: The system cannot find the file specified`), an environmental
  condition unrelated to this change, identical failure set before and after the fixes (no new
  Windows regression).
- Real WSL2 Ubuntu (kernel 6.6.87.2-microsoft-standard-WSL2, gcc 15.2.0) full rebuild via `ninja -k 0`:
  zero `error:`/`FAILED:` lines in the complete build log, both before and after the fixes — the first
  time `test_sandbox_runtime`/`test_mandatory_sandbox_provider`/`test_mandatory_sandbox_provider_composed`
  have ever compiled on Linux. Full `ctest` run: 171/173 passing both times (`test_shell_runner_no_process_creation`
  skipped, a pre-existing, unrelated, already-disclosed ADR-103 §7 residual); the 2 failures
  (`test_sandbox_runtime`/`test_mandatory_sandbox_provider`) traced to their real root cause, not
  assumed: `docker` is not on `PATH` in this WSL2 distro at all (confirmed via `which docker`
  returning nothing and Docker Desktop's own WSL-integration message), so `reset()`'s `docker create`
  fails first, `instance_` stays empty, and `run()` short-circuits on `docker_execution_surface.not_reset`
  before ever reaching the shell-injection guard this ADR added — a purely environmental, cascading
  failure, not a code defect (confirmed by reading the exact failing assertions against the exact
  short-circuit logic, not merely observed).
- Two standalone repros, compiled and run against the real code (not simulated): a
  `RealIoFileSystem` POSIX I/O proof linked against the real built `libagentengine_sandbox_io.a`/
  `libagentengine_worktree_store.a` (12/12 checks: byte-exact round trip incl. `0x00`/`0xFF`,
  empty-file handling, `O_TRUNC` truncation, lexical `..` rejection, REAL symlink-escape rejection);
  a `docker_execution_surface.hpp` shell-guard proof (25/25 checks after the red-team's fixes,
  20/20 before) including the C21 positive+negative control pair and both round-2 fixes'
  regression checks (`/workspace`/`alpine:latest` still pass; `--privileged`/`-v`/embedded-NUL now
  rejected).

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| C19 — build parity | **CORRECT** | Real WSL2 full-project `ninja -k 0`, zero errors; real MSVC full rebuild, 287/287 targets, both before and after fixes. |
| C20 — RealIoFileSystem POSIX I/O correctness | **CORRECT** | Standalone repro against the real built library, 12/12 checks, including a real symlink-escape rejection (not just lexical). |
| C21 — POSIX host-shell injection actually blocked | **CORRECT, after two real fixes** | Positive+negative control pair proved the base defense holds; the red-team's own two findings (leading-dash flag injection, NUL truncation) were real but pre-existing/non-exploitable-for-`command` respectively, both fixed and re-verified (25/25). |
| C22 — no Windows regression | **CORRECT** | Windows `write_verified()`/`read_verified()` moved verbatim (byte-identical); full Windows `ctest` run identical (284/287, same 3 Docker-required failures) before and after every change in this ADR. |

## 7. Residual risks

- **Docker-daemon-level runtime verification on Linux remains open** — this ADR gets
  `test_sandbox_runtime`/`test_mandatory_sandbox_provider` to COMPILE and their non-Docker-dependent
  logic (the shell-injection guard, the I/O layer) EMPIRICALLY PROVEN via standalone repros, but
  neither test's own real Docker-container assertions have ever run on Linux in this environment (no
  `docker` binary on `PATH` in this WSL2 distro). Identical in kind to ADR-103 §7's own disclosed gap
  for `tools/sandboxed_shell_chat.cpp`/`test_composed_sandbox_providers_live` — not newly introduced,
  just now additionally true for two more targets this ADR made compile for the first time. **WINDOWS
  SIDE SINCE VERIFIED (2026-08-29)**: once Docker Desktop's daemon was reachable on this host, all
  four Docker-dependent tests (`test_sandbox_runtime`, `test_mandatory_sandbox_provider`,
  `test_mandatory_sandbox_provider_composed`, `test_composed_sandbox_providers_live`) ran for real —
  this session's own `docker_execution_surface.hpp` shell-injection-guard rewrite (§2-§4 above),
  including the leading-dash and NUL-byte fixes, driven through real `docker run`/`exec`/`cp`/`rm`
  calls for the first time — and passed, 287/287 on the full Windows suite. The Linux half of this
  residual remains open (WSL2 Docker integration, a Docker Desktop setting, not a code gap).
- **`docker_cli_reject_leading_dash`'s fix is narrower than a full CLI-argument-injection defense** —
  it blocks a LEADING dash specifically (the concrete, proven attack shape), not every conceivable way
  a crafted value could confuse `docker`'s own argument parser. Judged sufficient for the values this
  function actually gates today (none of which are attacker/model-reachable in the current, single
  production caller) — a future caller passing genuinely untrusted `image`/`host_path`/`container_path`
  values should re-examine this before relying on it as a complete defense.
- **`remove(recursive=true)`'s shared recursion-depth hazard (ADR-103 §7)** — different files from this
  ADR's own scope, so left untouched here; **SINCE FIXED (2026-08-29)**, same day, as a small, separate,
  bounded follow-on (see ADR-103 §7's own updated entry) — noted here for anyone reading this ADR's
  residuals list in isolation.
- **The Windows-side `docker_cli_reject_unsafe_for_shell`/`docker_cli_reject_shell_breakout` blacklist
  itself was not re-audited character-by-character against `cmd.exe`'s full grammar in this pass** —
  out of scope (this ADR's job was Linux parity, and the Windows behavior is explicitly required to
  stay unchanged, C22); the leading-dash finding was the one exception fixed on both platforms because
  it was found live, proven live, and directly analogous to a POSIX-side finding already being fixed
  in the same pass.
