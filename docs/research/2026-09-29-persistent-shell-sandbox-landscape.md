# Persistent shell sessions in agent sandboxes — who keeps a live shell, for how long, and how

Compiled: 2026-09-29. Feeds `decisions/ADR-209-persistent-shell-sessions.md` (revision 4, Proposed).
All external facts below were fetched on 2026-09-29 from the URLs in §Sources; anything not
confirmed from a primary page or source file is marked **UNVERIFIED**. Isolation-technology
pluggability and the general feature matrix are **not** re-surveyed here — see
`docs/research/2026-08-23-microvm-sandbox-backend-landscape.md`,
`docs/research/2026-08-23-sandbox-feature-parity-survey.md` and
`docs/research/2026-08-06-cloudflare-computer-vfs-sandbox-comparison.md`; only facts that changed
since those are restated (§6).

## Question

For each ADR-209 shell tier — **Tier 0** one-shot (fresh process per command), **Tier 1** one live
shell per agent run, **Tier 2** a live shell per session reaped on idle (optionally pause/resume
with process memory), and the **native** (unsandboxed host) variants — which existing sandboxes,
libraries or services match it, and exactly how do they keep (or not keep) shell state?

## Answer

Prior art splits three ways. **(1) One-shot exec, with cwd/env carried host-side or not at all**
is the most common shape: E2B `commands.run`, Modal `Sandbox.exec`, Vercel `runCommand`, Daytona
`process.exec`, Claude Code's Bash tool (cwd carried, env not), Codex `exec_command` (fresh
process per call, `workdir` param), OpenAI's hosted/local shell, mini-swe-agent and container-use
(new container per command, filesystem committed to git). **(2) One live shell per explicit
session**, framed by a sentinel/prompt/exit-code file, is shipped by the Anthropic bash tool
reference (sentinel over pipes, host-chosen lifetime, `restart`), SWE-ReX (pexpect + PS1 marker),
OpenHands (tmux + PS1 JSON, or a persistent PowerShell on Windows), Daytona sessions and Cloudflare
Sandbox SDK sessions (both: long-lived shell fed through stdin, command sourced in the current shell,
exit-code file + `\x01\x01\x01`/`\x02\x02\x02` line prefixes), Runloop named shells, and on Windows
PowerShell `PSSession`. None of these ties the shell's life to an agent *run*; the host picks
(Tier 1 and Tier 2 are the same mechanism with a different close policy — which matches ADR-209's
`ShellLifetime` split). **(3) Surviving idle with live processes** requires a **VM-level** memory
snapshot: E2B pause/resume (~4 s/GiB pause, ~1 s resume), Blaxel standby, microsandbox `--full`
snapshots (merged 2026-09-10) and raw Firecracker snapshots. **Container-level** checkpoint does
**not** keep an exec'd shell: gVisor kills exec'd processes on restore, Docker checkpoint refuses
`-t` containers, Modal's alpha memory snapshots cannot run while an `exec` is live and do not
restore exec'd background processes, Kata has no checkpoint. This supports ADR-209 §10 rejecting
process snapshots for a Docker/containerd-backed live tier. Two findings bear directly on ADR-209
§7: **Daytona and Cloudflare do not kill a timed-out command** (the timeout only stops the caller
waiting, leaving the shell busy), and **OpenHands/Claude Code leave it running** (soft timeout /
move to background); only the Anthropic reference guidance (`killpg` + restart) and mini-swe-agent
kill — ADR-209's "timeout ⇒ kill ⇒ `shell_lost` ⇒ re-open from snapshot" is the strict end of that
spectrum and is consistent with Anthropic's own guidance.

## 1. Summary table

| Tier | Candidate | Shell-state mechanism | Lifetime | Isolation | Embeddable / licence |
|---|---|---|---|---|---|
| 0 | E2B `commands.run` | new `/bin/bash -l -c` per call; `cwd`/`envs` per call | per call; sandbox default 300 s | Firecracker microVM | self-hostable infra; Apache-2.0 |
| 0 | Modal `Sandbox.exec` | new process per call; `workdir`/`env`; optional `pty` | per call; sandbox `timeout=300` s, `idle_timeout=None` | gVisor (VM runtime beta) | hosted only |
| 0 | Vercel Sandbox `runCommand` | argv, one process per call, `cwd`/`env` | per call; sandbox 5 min default, ≤24 h | Firecracker | hosted; SDK Apache-2.0 |
| 0 | Daytona `process.exec` | new shell per call, `Setpgid`, `cwd`/`envs` | per call | container / "dedicated kernel" | frozen OSS v0.190.0 AGPL-3.0; closed since 2026-06 |
| 0 (+cwd) | Claude Code Bash tool | separate process per command; **cwd** carried host-side (inside project dirs only); **env not** (use `CLAUDE_ENV_FILE`) | per command; 2 min default, 10 min max | Seatbelt / bubblewrap(+seccomp) when sandboxed | closed product |
| 0 | Anthropic `srt` | wraps one command string | per command | Seatbelt / bwrap+seccomp / Windows dedicated user + WFP | library; Apache-2.0; research preview |
| 0 (+PTY proc) | Codex CLI `exec_command`/`write_stdin` | fresh process per call (`workdir` param), optional PTY; long runner returns `session_id`; shell env re-created from a "shell snapshot" | process lives until exit/evict; ≤64 procs, LRU | Seatbelt / bwrap+seccomp / Windows restricted token or sandbox users | Apache-2.0 |
| 0 | OpenAI hosted shell / `local_shell` | `commands[]` per call; local ref uses `child_process.exec` | container idle-expires after 20 min | container (tech not named in primary docs) | hosted; local executor is yours |
| 0 | mini-swe-agent | `subprocess.run` / `docker exec` per action, *deliberately* stateless | per action; 30 s default | host or long-lived container | MIT |
| 0 | container-use (Dagger) | **new container** per `environment_run_cmd`; foreground fs changes committed to git ref | per command | container (Dagger) | Apache-2.0; experimental |
| 1/2 | Anthropic bash tool (client tool) | one live bash, sentinel line per command, pipes; `restart: true` | host-chosen ("your application decides") | host's choice | spec + MIT reference code |
| 1/2 | SWE-ReX | pexpect `bash` per named session, PS1 marker; `create/run_in/close_session` | explicit session | Docker/Modal/Fargate/Daytona/local deployments | MIT |
| 1/2 | OpenHands terminal tool | tmux (fallback subprocess PTY) with PS1 JSON `{exit_code, working_dir}`; Windows: persistent PowerShell | explicit session; `reset: true` | local, Docker or K8s agent server | MIT |
| 1/2 | Daytona sessions | one long-lived shell (zsh/bash/sh) per session via stdin pipe; command file **sourced** | explicit session; sandbox auto-stop 15 min | as above | as above |
| 1/2 | Cloudflare Sandbox SDK sessions | one `bash --norc` per session via stdin; `{ cmd }` group, exit file | explicit session; container sleeps after 10 min (`sleepAfter`), **all shell state reset** | container in its own VM | SDK Apache-2.0; hosted runtime |
| 1/2 | Runloop devbox `shell_name` | named "stateful" shell | explicit | UNVERIFIED | hosted |
| 1/2 | Jupyter Kernel/Enterprise Gateway | kernel process holds state (not a shell) | until culled: `cull_idle_timeout` 0 (off); EG recommends 43200 s | platform (K8s, Swarm, YARN) | BSD-3-Clause |
| 1/2 | E2B code-interpreter contexts | per-context interpreter state (Jupyter-backed: UNVERIFIED) | sandbox lifetime | Firecracker | Apache-2.0 |
| 2 (+mem) | E2B pause/resume | VM memory + fs snapshot; running processes kept; also `sandbox.pty` interactive bash | paused: no TTL; auto-pause opt-in (`onTimeout:'pause'`) | Firecracker | Apache-2.0 |
| 2 (+mem) | Blaxel | standby snapshot incl. running processes, <25 ms resume | standby after ~15 s without connection | UNVERIFIED | hosted |
| 2 (+mem) | microsandbox `snapshot --full` | disk + memory + execution state; each `msb exec` is a separate process | named long-lived sandbox | libkrun microVM (KVM/HVF/WHP) | Apache-2.0; beta, feature 3 weeks old |
| 2 (+mem, hosted) | Anthropic code execution tool | container reused by id; checkpointed after ~5 min idle, restorable ≤30 days; shell cwd/env persistence UNVERIFIED | 30 days | not named | hosted |
| 2 (fs only) | Vercel persistent sandboxes, Modal fs snapshots, Docker Sandboxes (`sbx`) | filesystem/packages only, no process state | stop/resume | Firecracker / gVisor / hypervisor microVM | hosted / Docker product |
| native 1/2 | PowerShell `PSSession` | runspace keeps variables, aliases, functions across `Invoke-Command -Session` | IdleTimeout default 7 200 000 ms (2 h); disconnect/reconnect | none (user account; JEA can constrain) | Windows built-in (loopback needs admin) |
| native 1/2 | OpenHands `WindowsTerminal` | persistent PowerShell session | explicit | none | MIT |
| native (session lifecycle) | Microsoft MXC | provision → start → exec → stop → deprovision over ProcessContainer, Windows Sandbox, bwrap, Seatbelt, Hyperlight, … | explicit | README: "no MXC profiles should be treated as security boundaries currently" | MIT |

## 2. Tier 0 — one-shot

**Mechanism.** A new process per command with `cwd`/`env` passed as call parameters (E2B, Modal,
Vercel, Daytona `exec`, Codex `exec_command`, OpenAI local shell). mini-swe-agent states the
rationale explicitly: stateless `subprocess.run` makes switching to `docker exec` trivial.
container-use goes further: a **new container** per command, with foreground changes committed to a
git ref and background changes explicitly **not** committed.

**"One-shot plus carried cwd/env" is a recognised middle ground.** Claude Code carries `cd` between
separate processes (but only within project/additional dirs; resets otherwise with a visible
notice; subagents never carry it) and does *not* carry `export` (host-side `CLAUDE_ENV_FILE` /
SessionStart hook instead). Codex re-creates the login shell's environment per call from a "shell
snapshot" (detail of what it replays: UNVERIFIED).

**In-repo, per ADR-209 §1/§3/§5a and the 2026-08-23 surveys:** `MandatorySandboxProvider` over
`DockerExecutionSurface` (container destroyed and recreated per command — ADR §1) and, per the
survey/ADR-099 trail, `ContainerdExecutionSurface` and `KataBackend` are Tier 0; `NativeShellProvider`
is native Tier 0 (one process per call at `mount_root`). The **mediated `run_shell`** over
`native_jail` is Tier 0 *plus* a session-scoped `ExecState{cwd, env}` (010 §3a, ADR §5a) — the same
shape as Claude Code's host-carried cwd, but carrying env too. `WasmBackend` and the embedded
CPython interpreter are not shell tiers; per the parity survey, `wasm` has quiescent-point memory
snapshots and `native-jail` loses interpreter state on resume.

## 3. Tier 1 / Tier 2 — a live shell per run or per session

**Nobody scopes a live shell to an agent *run*.** Every product exposes an explicit session (or a
host-owned process) and leaves close policy to the caller or a sandbox-wide idle timer. ADR-209's
`lifetime::PerRun` default plus `on_run_end` has no direct prior art; `PerSession` + engine ceilings
matches the market's "explicit session + idle reap" shape. This is a divergence, not a defect —
it follows from ADR-209 committing every command's effect (§2), which none of the hosted products do.

**How the live shell is driven** (all verified from source):

| Implementation | Transport | How a command runs | End-of-command signal | cwd reported? |
|---|---|---|---|---|
| Anthropic reference (docs) | pipes, `setsid` | `{command}\necho {sentinel}` | per-call `__CLAUDE_BASH_DONE_<uuid>__` line | no |
| Anthropic quickstart `bash.py` | pipes, `os.setsid` | `cmd; echo '<<exit>>'` | fixed `<<exit>>`, 0.2 s poll | no |
| SWE-ReX | pexpect PTY, custom `PS1` | bashlex-split, joined with ` ; ` | PS1 marker; exit code via `echo EXITCODESTART$?EXITCODEEND`; `bash -n` pre-check | no |
| OpenHands | tmux pane 256×200 | typed into pane | `PROMPT_COMMAND` sets PS1 to JSON between `###PS1JSON###`/`###PS1END###`; last block wins | **yes** (`working_dir`) |
| Daytona session | stdin pipe, `Setpgid` | command written to `cmd.sh`, run as `{ . cmd.sh; } <in >out 2>err` (sourced) | exit code appended to a file; server polls 50 ms; per-line `\x01\x01\x01`/`\x02\x02\x02` prefixes | no |
| Cloudflare session | stdin pipe to `bash --norc` | `{ cmd }` group (foreground); subshell + FIFOs (background) | `<id>.exit.tmp` → `mv` to `<id>.exit`, `fs.watch` + 50 ms poll; same byte prefixes | no |
| ADR-209 §7 (proposed) | `docker exec` pipes | `eval "$(printf '%b' '<octal>')" </dev/null >"$D/o" 2>&1` | `\036<nonce> <exit> <pwd>\036` trailer | **yes** (+ env diff, §5) |

**Concurrency.** Cloudflare serialises with a per-session mutex; OpenHands **refuses** a new command
while one is running ("is NOT executed… set `is_input` to true" to send `C-c`/`C-z`/`C-d`);
ADR-209 holds `exclusivity_` for the command's deadline. All three agree one shell runs one
foreground command at a time.

**In-repo:** nothing is Tier 1/2 yet; ADR-209 proposes `LiveShellSandboxProvider<…, PerRun|PerSession>`
(sandboxed) and `NativeShellSessionProvider` (pwsh, Job Object).

## 4. Tier 2 with pause/resume — what survives a snapshot

| Mechanism | Does an exec'd live shell survive restore? | Source note |
|---|---|---|
| Firecracker snapshot | yes (whole guest), but vsock connections closed and guest network not guaranteed; restoring one snapshot twice duplicates RNG state | `snapshot-support.md` |
| E2B pause/resume | yes — "all the running processes, loaded variables"; ~4 s/GiB pause, ~1 s resume | persistence docs |
| microsandbox `--full` | memory + execution state; merged 2026-09-10; open bug with `--secret` | PR #1503, issue #1693 |
| Blaxel standby | files + running processes, <25 ms resume | overview docs |
| Anthropic code execution | container checkpointed after ~5 min idle, restorable within 30 days; what process state survives: UNVERIFIED | code-execution docs |
| gVisor `runsc checkpoint` | **no** — "Exec'd processes cannot be stitched back to the original caller and are killed after restore" | gVisor PR #11478 |
| Docker `checkpoint` (CRIU) | experimental; "External terminals (i.e. `docker run -t ..`) aren't supported" | Docker CLI ref |
| Podman checkpoint | root containers only; TTY behaviour UNVERIFIED | podman.io |
| CRIU | PTY state saved only if the master is inside the dumped tree; `--shell-job` for external pty | criu.org |
| Modal memory snapshot (Alpha) | **no** — cannot snapshot while `exec` runs; exec'd background processes not restored; snapshot terminates the sandbox | sandbox-snapshots guide |
| Kata | **no checkpoint/restore** ("The runtime does not provide `checkpoint` and `restore`") | Limitations.md |
| Cloudflare sleep | **no** — "All processes terminate… All shell state resets" (and files deleted) | sandboxes concept doc |

**Lesson:** the only working "live shell survives idle" products snapshot a **whole VM** whose
agent (envd, etc.) re-accepts connections after resume. For a Docker/containerd/Kata-backed tier,
ADR-209's choice — restore **cwd/env as data**, never processes — is what the evidence supports.

**Idle/lifetime defaults in the market** (for sizing `LiveShellLimits`, which ADR-209 §8 makes
required with no default): E2B sandbox 300 s (max 1 h Base / 24 h Pro); Modal 300 s max lifetime,
no idle timeout by default; Vercel 5 min (≤24 h); Daytona auto-stop 15 min; Cloudflare `sleepAfter`
10 min; OpenAI containers 20 min idle; Anthropic code execution ~5 min to checkpoint, 30-day expiry;
Jupyter culling off by default (EG recommends 12 h); PSSession 2 h. Nobody defaults to "unbounded
and live" — Modal's `idle_timeout=None` is still capped by its 300 s lifetime.

## 5. Native variants (Windows)

- **PowerShell `PSSession`** is the established native Tier 1/2 pattern: state (variables, aliases,
  functions) persists across `Invoke-Command -Session`; disconnected sessions keep running;
  default output buffering `Block` **suspends** the command when the buffer fills (`Drop`
  recommended); IdleTimeout default 2 h (min 60 s). Loopback use needs an elevated PowerShell — so
  ADR-209's plain held `pwsh` child is the practical embedding, not remoting.
- **OpenHands `WindowsTerminal`** is a persistent PowerShell session — independent prior art for
  "pwsh only" on Windows.
- **Job Objects**: `KILL_ON_JOB_CLOSE` terminates all associated processes when the last handle
  closes; children escape only with `BREAKAWAY_OK`/`SILENT_BREAKAWAY_OK` — confirms ADR-209 §9's
  "no breakaway" setting is the right lever. Whether WSL2 Linux processes launched via `wsl.exe`
  from inside a job survive job close: **UNVERIFIED** by any primary source; WSL keeps a distro alive
  for `instanceIdleTimeout` (default 15 s) after the last client, which is consistent with ADR-209
  §9's disclosed escape. Recommend an empirical test (run `wsl.exe -- sleep 1000` inside a
  KILL_ON_JOB_CLOSE job, close it, check `wsl ps`) before stating it as fact.
- **Stronger Windows boundaries available today:** Codex's native Windows sandbox (`elevated` —
  dedicated lower-privilege sandbox users + ACLs + firewall; `unelevated` — restricted token + ACLs),
  `srt`'s Windows mode (dedicated `srt-sandbox` user + WFP rules), Windows 11 "agent workspace"
  (separate standard account in a separate Windows session; preview, admin-enabled), AppContainer
  (Win32 app isolation is preview, 24H2, MSIX). **Windows Sandbox** is not usable for a shell tool:
  `wsb exec` "has no support for process I/O", one instance at a time, and it is unavailable on
  Home edition. Its persistence feature covers guest-initiated reboots, not host restart.
- **Microsoft MXC** (MIT, announced 2026-06-02) is the nearest Windows analogue of ADR-209's surface:
  a session lifecycle over many backends, adopted by GitHub Copilot CLI for process isolation — but
  its README says no profile is a security boundary yet. **LiteBox** (library OS, pre-stable) and
  **Hyperlight** (function-call micro-VMs, no shell, `snapshot()`/`restore()` of VM memory) have no
  shell concept and do not fit a shell tier.

## 6. Facts changed since the 2026-08 surveys

- Modal memory snapshots: docs now say **Alpha** (survey said "early preview"), with the limits in §4.
- Daytona closed-source: confirmed, announced 2026-06-11; last open release v0.190.0 (AGPL-3.0);
  community fork "Nightona". `process.exec` default timeout: docs say 10 s, source applies none
  unless set (SDK default `None`) — docs and source disagree.
- Codex Linux sandbox is now `bwrap` + seccomp by default (`codex sandbox landlock` is an alias);
  WSL1 unsupported from 0.115; native Windows sandbox has `elevated`/`unelevated` modes.
- `srt` gained a Windows mode (dedicated user + WFP).
- Vercel Sandbox maximum raised to 24 h (2026-06-16); persistent (fs-only) sandboxes are the default.
- New entrants for this question: microsandbox full snapshots, Docker Sandboxes (`sbx`, hypervisor
  microVMs incl. Windows 11 via WHP), kubernetes-sigs/agent-sandbox (stateful pod, pause/resume,
  gVisor/Kata), Microsoft MXC.

## 7. Lessons for ADR-209

1. **Kill on timeout is the minority, and the right one for a committing tier.** Anthropic's bash
   tool guidance: own process group, on timeout `os.killpg(pid, SIGKILL)` then `restart()`, because
   "a hung command blocks the session forever because its sentinel line never arrives." Daytona and
   Cloudflare only stop *waiting* (source: `execute.go`, `session.ts`) and the shell stays busy;
   OpenHands reports exit `-1` and keeps it running (soft 30 s no-output timeout; `C-c` via
   `is_input`); Claude Code moves the command to the background; SWE-ReX raises and leaves it to an
   explicit `sendintr()`/`kill -9 %1` interrupt. ADR-209 §7 (`kill -KILL -1` → `shell_lost` → re-open
   with snapshot cwd/env) matches Anthropic's guidance. **Divergence to flag:** nobody else re-opens
   *and restores cwd/env* automatically after a timeout — others either restart clean or keep the
   hung command. Consider whether an OpenHands-style "no new output for N s" soft signal is worth a
   follow-on (it lets a long `npm install` report progress rather than hit a hard deadline).
2. **Sourcing a file beats eval-of-a-string for framing robustness.** Daytona and Cloudflare write the
   command to a file and run it as a `{ . file; }` / `{ cmd }` group with redirected stdio, then signal
   completion with an **exit-code file written after output is flushed** (Cloudflare: write
   `.exit.tmp` then atomic `mv`). This removes quoting entirely and puts completion outside the output
   stream, so a command cannot forge it by printing. ADR-209 §7's in-band `\036nonce` trailer is
   forgeable (ADR admits, residual 5); an out-of-band exit file in `$D` would still be forgeable by
   the command (same uid) but not by merely printing — a cheap narrowing. The 64 KiB `printf '%b'`
   argument concern also disappears if the command is `docker cp`'d or streamed into `$D/cmd`.
3. **Report cwd in the framing, like OpenHands.** OpenHands' PS1 JSON carries `working_dir` (and the
   Python interpreter path) every command; ADR-209 §5 goes further (env diff). Agreement with the one
   other design that does this. OpenHands also keeps only the **last** marker block to resist
   interleaved/forged output — the same defensive parse ADR-209 should use for its trailer.
4. **Pipes, not a PTY, for a non-interactive agent shell.** Anthropic's reference and Daytona/Cloudflare
   use pipes; PTYs appear where interactivity is the point (SWE-ReX pexpect, OpenHands tmux, E2B
   `pty`, Codex `tty: true`, Modal `pty`). ADR-209's `</dev/null` choice mirrors the Anthropic
   docs' "No interactive commands" constraint. If interactivity is ever wanted, Codex's
   `exec_command` → `session_id` → `write_stdin` model (process-scoped, 64-process LRU cap,
   1 MiB output cap) is the proven shape — distinct from the shell itself.
5. **Background processes are the universally weak spot.** container-use does not commit background
   changes; Cloudflare runs background commands in a subshell (no state persistence) and its own
   design doc warns children may not be killed; E2B background output only reaches the starting
   client (redirect to files). ADR-209 §6 committing background writes at the next command is
   stronger than any of these, and the torn-drain measurement (C12) has no prior art to lean on.
6. **Restart must be budgeted.** Anthropic exposes `restart: true` freely; ADR-209 charges
   `LiveShellOpen` per restart (§8). No product surveyed bounds restarts — a divergence in the safe
   direction, consistent with I8.
7. **Idle reap values cluster at 5–20 minutes** (E2B, Daytona, Cloudflare, OpenAI, Anthropic);
   hosts sizing `LiveShellLimits::max_idle` have that as a market reference.
8. **Do not promise process survival across idle on the container tiers.** §4: every container-level
   checkpoint path breaks exec'd shells; only whole-VM snapshots work. If a Tier 2 "resume with live
   processes" is ever wanted, it belongs to a microVM surface (E2B/microsandbox/Firecracker class),
   i.e. the `remote` profile per the locked "no `microvm` profile" decision.
9. **Native: prior art isolates by *identity*, ADR-209 by *job*.** Codex elevated, `srt` Windows and
   the Windows agent workspace all run under a separate low-privilege account; ADR-209 §9's held
   `pwsh` runs as the host user inside a Job Object (lifetime/resource containment, not an authority
   boundary) — correctly stated as a widening of `cap::NativeExec`. Flag for the Judge: every
   comparable native Windows shell sandbox found adds a principal boundary; ADR-209 does not.

## Sources (all accessed 2026-09-29)

Anthropic
- https://platform.claude.com/docs/en/agents-and-tools/tool-use/bash-tool
- https://raw.githubusercontent.com/anthropics/claude-quickstarts/main/computer-use-demo/computer_use_demo/tools/bash.py
- https://platform.claude.com/docs/en/agents-and-tools/tool-use/code-execution-tool
- https://code.claude.com/docs/en/tools-reference
- https://code.claude.com/docs/en/sandboxing
- https://github.com/anthropic-experimental/sandbox-runtime

OpenAI
- https://github.com/openai/codex (`codex-rs/core/src/unified_exec/mod.rs`, `process_manager.rs`, `codex-rs/core/src/tools/handlers/shell_spec.rs`)
- https://developers.openai.com/codex/sandbox.md
- https://learn.chatgpt.com/docs/windows/windows-sandbox.md
- https://developers.openai.com/api/docs/guides/tools-shell
- https://developers.openai.com/api/docs/guides/tools-local-shell.md
- https://developers.openai.com/api/docs/guides/tools-code-interpreter.md
- Codex Ctrl-C (`\x03`) for non-TTY processes, PR #26734 — title seen in search only, **UNVERIFIED**

Agent frameworks
- https://raw.githubusercontent.com/SWE-agent/SWE-ReX/main/src/swerex/runtime/local.py
- https://github.com/OpenHands/software-agent-sdk (`openhands-tools/openhands/tools/terminal/`)
- https://raw.githubusercontent.com/SWE-agent/mini-swe-agent/main/src/minisweagent/environments/local.py
- https://raw.githubusercontent.com/SWE-agent/mini-swe-agent/main/src/minisweagent/environments/docker.py
- https://raw.githubusercontent.com/jupyter-server/kernel_gateway/main/README.md
- https://jupyter-enterprise-gateway.readthedocs.io/ (config-culling page)
- https://github.com/dagger/container-use ; https://raw.githubusercontent.com/dagger/container-use/main/mcpserver/tools.go

Hosted sandboxes
- https://raw.githubusercontent.com/e2b-dev/E2B/main/packages/python-sdk/e2b/sandbox_sync/commands/command.py
- https://raw.githubusercontent.com/e2b-dev/E2B/main/packages/python-sdk/e2b/sandbox/main.py
- https://raw.githubusercontent.com/e2b-dev/infra/main/packages/envd/internal/services/process/handler/handler.go
- https://raw.githubusercontent.com/e2b-dev/infra/main/packages/envd/internal/services/process/start.go
- https://github.com/e2b-dev/infra
- https://docs.e2b.dev/commands/background.md ; https://docs.e2b.dev/sandbox/pty.md ; https://docs.e2b.dev/code-interpreting/contexts.md ; https://docs.e2b.dev/sandbox ; https://docs.e2b.dev/sandbox/persistence ; https://docs.e2b.dev/sandbox/auto-resume.md
- https://deepwiki.com/e2b-dev/code-interpreter (secondary; Jupyter backing **UNVERIFIED**)
- https://github.com/daytonaio/daytona ; https://www.daytona.io/dotfiles/updates/daytona-is-going-closed-source ; https://github.com/nightona-co/nightona ; https://www.daytona.io/docs/en/sandboxes/
- https://raw.githubusercontent.com/daytonaio/daytona/v0.190.0/apps/daemon/pkg/toolbox/process/execute.go
- https://raw.githubusercontent.com/daytonaio/daytona/v0.190.0/apps/daemon/pkg/session/create.go ; …/session/execute.go ; …/session/delete.go ; …/common/get_shell.go
- https://raw.githubusercontent.com/daytonaio/daytona/v0.190.0/libs/sdk-python/src/daytona/common/daytona.py
- https://modal.com/docs/reference/modal.Sandbox ; https://modal.com/docs/guide/sandbox ; https://modal.com/docs/guide/sandbox-snapshots ; https://modal.com/docs/guide/security ; https://modal.com/docs/guide/vm-sandboxes
- https://developers.cloudflare.com/sandbox/concepts/sessions/ ; https://developers.cloudflare.com/sandbox/concepts/sandboxes/ ; https://developers.cloudflare.com/containers/platform-details/architecture/
- https://raw.githubusercontent.com/cloudflare/sandbox-sdk/main/packages/sandbox-container/src/session.ts ; …/docs/SESSION_EXECUTION.md ; …/packages/sandbox-container/src/config.ts
- https://vercel.com/docs/vercel-sandbox ; https://vercel.com/docs/sandbox/sdk-reference ; https://vercel.com/docs/sandbox/concepts/persistent-sandboxes ; https://vercel.com/docs/sandbox/concepts/snapshots ; https://vercel.com/changelog/vercel-sandbox-can-now-run-for-up-to-24-hours
- https://raw.githubusercontent.com/vercel/sandbox/main/packages/vercel-sandbox/src/api-client/api-client.ts ; …/src/sandbox.ts
- https://docs.runloop.ai/docs/devboxes/execute-commands
- https://docs.blaxel.ai/Sandboxes/Overview (isolation tech **UNVERIFIED**)

Checkpoint / local sandboxes
- https://gvisor.dev/docs/user_guide/checkpoint_restore/ ; https://github.com/google/gvisor/issues/11439 ; https://github.com/google/gvisor/pull/11478 ; https://github.com/google/gvisor/issues/11064
- https://docs.docker.com/reference/cli/docker/checkpoint/
- https://podman.io/docs/checkpoint ; https://docs.podman.io/en/latest/markdown/podman-container-checkpoint.1.html
- https://criu.org/TTYs ; https://criu.org/Advanced_usage
- https://raw.githubusercontent.com/firecracker-microvm/firecracker/main/docs/snapshotting/snapshot-support.md
- https://github.com/kata-containers/kata-containers/blob/main/docs/Limitations.md ; https://github.com/kata-containers/kata-containers/issues/13653
- https://github.com/superradcompany/microsandbox ; https://docs.microsandbox.dev/cli/snapshot-commands ; https://github.com/superradcompany/microsandbox/pull/1503 ; https://github.com/superradcompany/microsandbox/issues/1693
- https://docs.docker.com/ai/sandboxes/ ; https://docs.docker.com/ai/sandboxes/architecture/ ; https://docs.docker.com/ai/sandboxes/install/
- https://github.com/kubernetes-sigs/agent-sandbox ; https://github.com/abshkbh/arrakis

Windows
- https://learn.microsoft.com/en-us/windows/security/application-security/application-isolation/windows-sandbox/
- https://learn.microsoft.com/en-us/windows/security/application-security/application-isolation/windows-sandbox/windows-sandbox-cli
- https://learn.microsoft.com/en-us/windows/win32/secauthz/appcontainer-isolation ; https://learn.microsoft.com/en-us/windows/win32/secauthz/app-isolation-overview
- https://support.microsoft.com/en-us/windows/experimental-agentic-features-a25ede8a-e4c2-4841-85a8-44839191dfb3 ; https://learn.microsoft.com/en-us/windows/security/book/operating-system-agentic-security
- https://blogs.windows.com/windowsdeveloper/2026/06/02/windows-platform-security-for-ai-agents/ ; https://github.com/microsoft/mxc
- https://github.com/microsoft/litebox ; https://github.com/hyperlight-dev/hyperlight
- https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/about/about_pssessions ; https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/about/about_remote_disconnected_sessions ; https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/new-pssessionoption ; https://learn.microsoft.com/en-us/powershell/scripting/security/remoting/jea/overview
- https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_limit_information
- https://learn.microsoft.com/en-us/windows/wsl/wsl-config ; https://github.com/microsoft/WSL/issues/8161 ; https://github.com/microsoft/WSL/issues/2151 (WSL-escapes-job: **UNVERIFIED**, inference only)
