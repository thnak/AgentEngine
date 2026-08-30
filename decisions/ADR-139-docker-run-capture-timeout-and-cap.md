# ADR-139 — `DockerExecutionSurface`'s `run_capture()` gets a real wall-clock timeout and output cap, matching its containerd sibling

- **Status:** Proposed — implemented, verified FOR REAL against a live Docker daemon (Windows/MSVC),
  full rebuild (zero errors) and full `ctest` clean (293 total, 1 pre-existing unrelated failure, zero
  regression), `naming_lint.py` clean. **Independent red-team round (§7) found and fixed one real,
  reproducible Windows-only process-leak defect, confirmed live against a real Docker daemon both
  before (leak present) and after (leak gone) the fix.**
- **Date:** 2026-08-30/31.
- **Scope:** `include/agentengine/sandbox/docker_execution_surface.hpp` only
  (`docker_cli_detail::run_capture()`, both platform arms, completely rewritten).
- **Related specs:** `decisions/ADR-106-containerd-execution-surface.md`/`ADR-107` (`ctr_cli_detail::
  run_argv()`, the sibling this now has parity with — same 30s timeout, same 1 MiB cap, cites the same
  CLAUDE.md machine-safety rule), `decisions/ADR-104-real-io-filesystem-linux-parity.md` (the POSIX
  `pclose()` exit-status bug this file's own comment already documents, unaffected by this rewrite).

## 1. The question

A final-review pass found `run_capture()` — used by EVERY `docker` CLI invocation this codebase makes
(`create()`, `exec()`, `copy_to_container()`, `copy_from_container()`, `destroy()`,
`reap_orphans()`) — had no wall-clock timeout and no output-size cap, unlike its sibling
`ctr_cli_detail::run_argv()` (containerd_execution_surface.hpp), which explicitly cites CLAUDE.md's
machine-safety rule for both. Since `exec()` carries model-supplied `RunCommandArgs::command`, a
non-terminating contained process (`tail -f`, `yes`, a backgrounded daemon) hung the calling coroutine
forever (`popen`/`_popen` blocks until the pipe hits EOF, i.e. until the child exits) while holding
`SandboxRuntime::exclusivity_`, and unbounded stdout grew this host process's memory without limit — a
real, reachable I8 gap. Should `run_capture()` get the same real timeout/cap treatment its sibling
already has?

## 2. Findings

Both platform implementations needed a genuine rewrite, not a parameter added to the existing
`popen`/`_popen` call — neither gives access to the child's process handle/pid, which is required to
actually kill a hung child on a deadline. See §7 for a real Windows-only gap the first rewrite still
left open (killing the immediate child is not the same as killing the whole process tree on Windows).

## 3. What was built

**Constants**: `kProcessTimeoutSeconds = 30`, `kOutputSafetyCapBytes = 1u << 20` (1 MiB) — matching
`ctr_cli_detail`'s own values exactly.

**POSIX**: replaced `popen`/`pclose` with `posix_spawn("/bin/sh", {"/bin/sh","-c",command})` + a real
anonymous pipe (stdout AND stderr `dup2`'d to the SAME write end, reproducing the previous
`(command + " 2>&1")` merge behavior) + a `poll()`-based bounded read loop with a deadline, mirroring
`ctr_cli_detail::run_argv()`'s own proven shape (single stream here, since `SurfaceRunOutcome` has only
one text field). On timeout or output-cap-hit, `kill(pid, SIGKILL)` + a bounded `waitpid`; on natural
EOF, a plain blocking `waitpid` (mirroring `run_argv()`'s own "bounded reap even in the non-timeout
path" comment).

**Windows**: replaced `_popen`/`_pclose` with `CreateProcessA` + an anonymous pipe (stdout AND stderr
both set to the same write handle) + a `PeekNamedPipe`-based bounded read loop with a deadline (a short
`WaitForSingleObject(pi.hProcess, 20)` poll when no data is immediately available). `command` is
concatenated onto `"cmd.exe /c "` **raw** — see §3a for why this specific decision mattered.

**§3a — a real bug found and fixed during THIS implementer's own build/test pass, before red-team**:
the first draft wrapped `command` in the Microsoft CRT argv-quoting algorithm (the same one
`native_process_spawn.cpp`'s own `detail::quote_one_argument` implements) before handing it to
`cmd.exe /c`, reasoning by analogy that this was what `_popen` did internally. This was WRONG and a
real, executed regression: `cmd.exe`'s own `/c`-remainder parsing does NOT apply CRT-style
backslash-before-quote unescaping to what it finds there — a completely different, much simpler
grammar. Re-quoting a `command` string that already contains its OWN literal `"..."` (e.g. `docker exec
<id> sh -c "<cmd>"`, built by this file's own callers) turned every embedded quote into a literal
backslash-quote PAIR `cmd.exe` then passed straight through to `docker`/`sh` as two literal characters,
silently corrupting every quoted argument. Confirmed via direct test failure:
`test_sandbox_runtime`/`test_docker_orphan_reap`/`test_mandatory_sandbox_provider`/
`test_task_branch_tools`/`test_composed_sandbox_providers_live`/`test_task_branch_concurrent_dispatch`
all failed identically (turn 1 `run()` itself failing) against a real Docker daemon. Fixed by removing
the re-quoting entirely — `"cmd.exe /c " + command` raw, reproducing what `_popen` actually does
internally (it does NOT CRT-quote the command string) and matching this file's own pre-existing,
already-tested shell-quoting discipline (`docker_cli_win_double_trailing_backslashes`,
`docker_cli_reject_shell_breakout`), which was always designed and tested against a raw concatenation.
Re-verified: all six tests pass clean after the fix.

## 4. Verification

Full rebuild (zero errors). Ran all six affected real-Docker tests directly and confirmed genuine pass
against a live daemon (`docker info` confirmed reachable): `test_sandbox_runtime`,
`test_docker_orphan_reap`, `test_mandatory_sandbox_provider`, `test_task_branch_tools`,
`test_composed_sandbox_providers_live`, `test_task_branch_concurrent_dispatch`. Full `ctest`: 293
total, 1 pre-existing unrelated failure, zero regression. `naming_lint.py` clean.

## 5. Not done

- No change to the POSIX `pclose()`-return-value bug this file's own comment already documents and
  works around (unrelated — that fix already applies to the `pclose()` semantics this rewrite no
  longer uses at all; the new POSIX code uses `waitpid`/`WIFEXITED`/`WEXITSTATUS` directly, the correct
  decoding, from the start).
- No change to `SurfaceRunOutcome`'s own shape (still one merged `stdout_text` field, no separate
  stderr) — matches the existing, unmodified contract every caller already depends on.

## 6. Residuals

- Killing the CLIENT-side `docker`/`sh` process on timeout does not itself signal the CONTAINER's own
  server-side exec'd process to stop (e.g. `tail -f` inside the container can outlive the killed
  client) — symmetric with `ctr_cli_detail`'s own accepted model (client kill ≠ remote-process kill),
  covered by the existing `reap_orphans()` container-level sweep, not something this ADR is scoped to
  close.
- See §7 for the Windows Job Object fix and its own residual (POSIX has no equivalent open residual —
  `posix_spawn` + `SIGKILL` genuinely terminates the immediate child and, for the shell-only spawn
  shape used here, has no grandchild process to leak).

## 7. Independent red-team round (same day, this session's own consolidated final-review pass)

**Verdict: the core design (30s timeout + 1 MiB cap) is sound, confirmed via real tests — but a real,
reproducible Windows-only resource leak was found and fixed.**

**Finding: `TerminateProcess()` on timeout only killed `cmd.exe`, not its real child.** Windows has no
`exec()`-style process-image replacement — `cmd.exe` stays alive as a genuine parent of whatever it
runs (e.g. `docker.exe`). On timeout, the original `TerminateProcess(pi.hProcess, ...)` killed only
`cmd.exe`, leaving the actual `docker.exe` client (and, transitively, the containerized process it was
waiting on) running forever. Proved directly against a real Docker Desktop daemon:
`run_capture("docker exec <id> sh -c \"tail -f /dev/null\"", timeout=5)` returned in 5s (confirming the
coroutine-hang half of the original bug WAS fixed) but left a live `docker.exe` host process and a live
`tail` process inside the container (confirmed via `docker top`) — a real, undisclosed I8 leak, just
moved one level down from "hung coroutine" to "orphaned process tree."

**Fixed**: added a Windows Job Object (`CREATE_SUSPENDED` + `AssignProcessToJobObject()` BEFORE
`ResumeThread()`, so `cmd.exe` can never spawn a child before it is job-bound) configured with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`: `TerminateJobObject()` on timeout now kills `cmd.exe` AND every
real descendant it ever spawned. Falls back to the original single-process `TerminateProcess` if job
creation/assignment fails (rare), rather than aborting the call outright.

Verified necessary and sufficient: re-ran the same live-Docker probe against the pre-fix code (1 leaked
`docker.exe` process, confirmed by process enumeration) and the post-fix code (0 leaked `docker.exe`
processes, identical command/timeout). Rebuilt and re-ran all six requested test binaries before and
after the fix — all pass both times — plus confirmed `agentengine_cli_chat.exe` starts cleanly and runs
its new ADR-136 startup orphan sweep without crashing or hanging.

**Other things checked and confirmed correct, no fix needed**: `SurfaceRunOutcome::stdout_text` is
documented as merged-only, and both new implementations genuinely `dup2`/redirect stderr onto the same
pipe as stdout — verified true merge, not silent drop. Output-cap and deadline enforcement in both read
loops are correctly bounded, no bypass found. POSIX `waitpid`/`kill(SIGKILL)` sequencing matches the
proven `ctr_cli_detail::run_argv` precedent exactly, including its same low-probability
`poll()`-error-not-`EINTR` edge case (pre-existing and shared, not a new regression). The
`tools/cli_chat.cpp` startup sweep (ADR-136) reviewed alongside this round: faithful copy of the
existing sibling pattern, silent when nothing needs reaping, does not block/crash startup.

All temporary probe files, containers, and processes created during this round were confirmed cleaned
up (`git status --short` clean beyond the intended diff).
