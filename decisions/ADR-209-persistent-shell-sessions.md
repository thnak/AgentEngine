# ADR-209 — Persistent shell sessions: a live shell per run, sandboxed and native, with developer-chosen lifetimes

- **Status**: **Proposed — design revision 4 (2026-09-28), after red-team passes 1–3 (§13). Pass 3 judged the transaction model structurally sound. Not built.**
- **Date**: 2026-09-28
- **Origin**: project-owner request (2026-09-28): a shell whose `cd`, environment and working state survive from one
  command to the next, for the sandboxed path and natively, on one shared core. Owner decisions in the same
  conversation: **both targets, one core**; **run-scoped by default**; **one provider type per lifetime tier, not a
  mode**; **lifetime is developer-choosable, with several built in**.
- **Touches**: `core/fs_walk.hpp` + the Ledger scan (symlinks never followed — a prerequisite fix Tier 0 needs too,
  §4.1), `core/ledger.hpp` (a read-only head-checkpoint accessor), `sandbox/sandbox_runtime.hpp` (a live-surface run
  verb beside `run()`), a new
  `sandbox/live_shell_sandbox_provider.hpp`, a new `PersistentShellSurface` conformer on `DockerExecutionSurface`, 005 §5
  plus `rt/agent_session_core`/`core/context_assembly.hpp`/`core/composed_context_provider.hpp` (an optional,
  **release-only** run-end hook), `trust/capability.hpp` (`cap::NativeExec` session fields, `cap_covers`,
  `cap::decl::NativeExec`, `to_capability`), the declarative grant schema and the policy-reachability tool (I6), a new
  `src/backends/native_process/native_shell_session_provider.hpp`, a new shared `src/sandbox/persistent_shell/` core.
  `MandatorySandboxProvider` and `NativeShellProvider` are **not changed**.
- **Invariants**: I1, I2 (nothing the shell prints becomes authority; native grant re-checked per command; a lifetime
  policy sees no authority), I3 (cwd/env snapshots are model-derived data), I4 (one principal per commit and per live
  shell), I8 (engine-owned ceilings).

## 1. The problem

*(Corrected 2026-09-29: the mediated `run_shell` is not one-shot — it keeps a session-scoped `ExecState{cwd, env}`
per 010 §3a, with no live process; see §5a. The paths below are the ones with no persistent shell state.)*
Tier 0's `run_command` → `SandboxRuntime::run()` (`sandbox_runtime.hpp:116`):
materialize head → **destroy and recreate the container** (`docker_execution_surface.cpp:953-961`) → one `sh -c` →
drain → scan → commit. `NativeShellProvider` spawns one process per call at `mount_root` with a fixed minimal
environment. `cd`, `export`, virtualenvs and background servers never outlive a call; the sandbox pays a container
create per command.

## 2. The reframe (revision 3)

Revisions 1 and 2 committed a turn's work lazily, at hook points (`on_turn_end`, then `on_context`/`on_run_end`). Both
red-team passes found the same root cause under different paths: **uncommitted work living in a live container between
hook points** — lost or mis-committed through composition (`assemble_context` swallows a contributor's error,
`context_assembly.hpp:201-209`), cancellation between turns (`agent_session_core.cpp:1437` precedes `on_context`),
CodeAct suspension after dispatch, forks from inside a round, flush failures in non-fallible copy operations.

**Revision 3 removes the root cause instead of guarding each path: every `shell_exec` commits its own effect before it
returns, exactly as Tier 0's `run_command` does today.** The live shell changes *where the command runs* (a held
container, a held shell) and *what persists between commands* (cwd, env, processes) — not the transactional contract.
Between tool calls there is no dirty state (except background writers, §6), so there is nothing for a hook, a fork, a
cancellation or a composed provider to lose.

What this keeps from the owner's goal: shell state persists; one container per run instead of per command; state is
restored when the durable head moved. What it gives back: the per-command drain + commit (Tier 0's cost) stays. An
incremental drain (changed files only) is the follow-on that recovers most of it (§10).

## 3. Types

| Tier | Type | Tool |
|---|---|---|
| 0 one-shot (unchanged) | `MandatorySandboxProvider<Surface, Store>` | `run_command`, task-branch tools |
| live sandbox | `LiveShellSandboxProvider<Surface, Store, Lifetime = lifetime::PerRun>` | `shell_exec` |
| native one-shot (unchanged) | `NativeShellProvider` | `native_shell_run` |
| native live | `NativeShellSessionProvider<Lifetime = lifetime::PerRun>` (**`pwsh` only**, §9) | `native_shell_session_exec` |

The live sandbox type owns its own `SandboxRuntime`, branch and surface. `MandatorySandboxProvider` cannot take a
lifetime parameter: its second parameter is `Store` (ADR-132, `:367`), and its task-branch tools run `run()` on its one
`surface_` (`:930`), whose `reset()` destroys the container. Composing Tier 0 and a live provider is allowed: two
sandboxes, two branches, nothing shared. No task-branch tools in the live tier in v1.

## 4. The sandbox command path

`SandboxRuntime` gains `run_live(LiveSurface&, command, author, quotas)` beside `run()` (it owns `io_fs_`, which is
private, `:353`). Under `exclusivity_`, per `shell_exec`:

1. **Charge `RunCost`** before anything runs (Tier 0's rule; same owner-only quota semantics, `async_quota.hpp:131`).
   **Who can use the shell:** `RunCost` is owner-only (`async_quota.hpp:131-137`), so step 1 already refuses every
   identity but the quota owner — exactly as Tier 0 does. A live shell therefore only ever runs commands for one
   identity, which is what makes §6's attribution hold. A delegate admitted via `on_behalf_of` cannot use `shell_exec`
   (a named refusal; nothing runs). Per-principal quotas would need their own ADR. Revision 3's "principal change →
   kill" machinery was unreachable behind this check and is removed (pass 3 #5).
2. **Sync check.** No live shell, or the shell is `shell_lost`/`desynced`, or the head **checkpoint** (`self_digest`,
   via a new read-only accessor) ≠ `live_synced_checkpoint_` → **open** (§4.2). Syncing on the checkpoint rather than
   the tree catches a reset whose target tree equals the current one (pass 3 #2).
4. **Run** the framed command (§7) with the per-command deadline.
5. **Drain → scan → commit** — the same steps 5–7 as `run()`, into a **wiped** staging directory (draining into the old
   one would resurrect deleted files: `drain_to` is an additive `docker cp`, `:1054-1073`). Record the snapshot under the
   new checkpoint's Ledger `turn_index`. `live_synced_tree_ = committed tree`.
6. Errors are the **tool's** errors, returned through the tool result. **A failed commit after the command ran** is
   reported with the command's `exit_code` and output **plus** `commit_error` (`ok: false`) — unlike Tier 0, which
   drops the output (`sandbox_runtime.hpp:171-178`) — so the model knows a `git clone` or `curl -X POST` really happened
   (pass 3 #7). The shell is marked `desynced`; the next command re-opens from head, and its reply says the unrecorded
   changes were discarded.

### 4.1 Prerequisite: the scan must not follow symlinks

`for_each_regular_file_recursive` classifies with `entry.is_regular_file()` (`core/fs_walk.hpp:66`), which follows
symlinks; the read then goes through `open_within_mount_root`, which resolves first and checks containment after
(`worktree_mount_fs_posix.hpp:84-90`). So `python -m venv .venv` (`.venv/bin/python -> /usr/bin/python3`) makes every
commit fail on a Linux host, a dangling link is silently skipped, and the pass/fail difference tells the container
whether a host path exists (pass 3 #1). **Tier 0 has this today.** Fix, as its own commit plus a GitHub issue for Tier
0: classify with `symlink_status`, never follow, never fail on a link; links are **skipped and listed** in the reply
(the Ledger tree has no link kind; adding one is a follow-on). Consequence, disclosed: a venv works inside a live shell,
and after any re-open its interpreter links are gone (`python -m venv --copies` avoids this).

### 4.2 What "open" means

Every open — first use, desync, reset, `shell_lost`, `container_lost`, `restart: true` — is one operation: charge
`LiveShellOpen`; **destroy any existing container and create a fresh one** (seeding is an additive extract,
`docker_execution_surface.cpp:1046`, so reusing a container would resurrect files a reset or discard removed — pass 3
#3); materialize head; seed; start the shell; replay the snapshot for this checkpoint (§5);
`live_synced_checkpoint_ = head`. There is no partial re-sync.

### 4.3 Shell lost, container alive

`kill -1` from inside kills the shell too (only PID 1 and the caller are spared), as does `exit` inside `eval` or a
trailer missing at the deadline. All of these are `shell_lost`: processes are gone, and the next command opens (§4.2),
restoring cwd/env from the snapshot taken after the **last completed** command. A timeout therefore always ends
background processes; cwd/env survive it. Disclosed (§12).

The live provider exposes no `runtime()` accessor. Forks (`fork_from`), `reset_to_turn`, rebind and move need no flush:
the head already holds every completed command. `reset_to_turn` just moves the head; step 3 restores on the next command.

## 5. Snapshots

After **every** command the trailer reports `cwd` and the environment diff (pass 3 #11). Snapshots are keyed by the
committed checkpoint's `self_digest`. `Ledger::reset_to` appends a **new** checkpoint holding the old tree
(`ledger_impl.hpp:199-207`), so the provider's own `reset_to_turn(n)` records "new checkpoint → turn n's snapshot" and
marks the shell desynced (pass 3 #2). A reset issued behind the provider's back is caught by the checkpoint sync check
and restores with no snapshot (fresh cwd/env) — disclosed. Only one identity ever uses a live shell (§4), so a snapshot
never crosses principals.

- Replayed inside the shell as data: sh — each value passed as a `printf '%b'` octal-escaped literal into `cd --` /
  `export`, with a sentinel character appended inside the command substitution and stripped afterwards so trailing
  newlines survive; pwsh — base64 decoded by `[Convert]::FromBase64String` (never a quoted literal: pwsh single quotes also close
  on U+2018–U+201B). Never used host-side (no `docker exec -w`, no native spawn cwd, no host env block, no grant check).
- Not replayed: `IFS`, `ENV`, `BASH_ENV`, `LD_*`, `PS*`, `SHELLOPTS`, `BASHOPTS`, `PROMPT_COMMAND`.
- Caps: `cwd` ≤ 4 KiB; ≤ 64 variables / 32 KiB. In memory, bounded LRU. Never restored: processes, functions, aliases.

### 5a. OPEN — reconciliation with 010 §3a `ExecState` (found after pass 3, 2026-09-29)

010 §3a already specifies a session-scoped `ExecState{cwd, env}` (`include/agentengine/sandbox/runner.hpp:17`), held
by the sandbox and shared **by reference** by every `Runner`, so a `cd` in the shell is the `os.getcwd()` of the next
`execute_code` and vice versa. The mediated `run_shell` implements it (`session_shell_wiring.hpp:103`: "cwd/env
survive across calls"). §1's "every shell path is one-shot" was wrong for the mediated shell, and this ADR's §5
snapshot is a second, parallel `{cwd, env}` carrier — which the spec forbids ("exactly one notion of the current
directory of this session"). The spec wins until amended. To decide before building:

- **(a) Adopt:** the live shell's snapshot *is* the session's `ExecState` — read on open, written after every command
  — when the live provider and the interpreter serve the same worktree. Needs a path mapping (the container sees
  `/workspace`, the interpreter's mount is `/work`) and a rule for values the other side cannot represent.
- **(b) Scope:** amend 010 §3a so "every `Runner`" means every runner over the same execution environment; a live
  container shell has its own `ExecState`, and a session using both says so.

Recommendation: (a) when both are configured for one worktree, since it is the property 010 §3a calls "one machine";
(b) only if the path mapping proves lossy. Either way, 010 §3a's own open item — background processes "not fully
specified here — see Q6" — is what §6 below answers for the live tier, and 010 must cite this ADR.

## 6. Background processes

A command may leave a process running (`npm run dev &`). Its later writes are committed by **whichever later
`shell_exec` drains** — attributed to that command's principal, which is always the quota owner — the only identity that can
run a command in a live shell (§4) — so no process started by A is ever committed as B (pass 2 #6, pass 3 #5). A drain racing an active writer can capture a
torn file set; `docker pause` for the drain is investigated in the prove phase (C12), otherwise disclosed. Writes after
the last command of a run are lost at close. Disclosed (§12).

## 7. Framing and deadlines

- **sh:** `{ eval "$(printf '%b' '<\0ddd-escaped command>')"; } </dev/null >"$D/o" 2>&1; printf '\n\036<N> %d %s\036\n' "$?" "$(pwd)"`
  — `$D` is a `mktemp -d` (0700) directory made at shell start and marked `readonly`; `printf` must be a builtin (probed at open, so a 64 KiB
  command never hits an `execve` argument limit). NUL in a command is rejected up front. The open-time probe checks
  `sh`, builtin `printf`, `mktemp`, `kill`; missing → the open fails with a named error.
- **pwsh:** the command is sent base64 and **dot-sourced** (`. ([ScriptBlock]::Create(...))`) so assignments persist;
  `$LASTEXITCODE` reset before each command.
- A per-command 128-bit nonce is a literal in the trailer. **The framing is not a security boundary** — the command can
  shadow `printf`/`pwd` with functions, replace `$D/o` with a FIFO, read the shell's saved stdin, forge a trailer or
  write to an inherited descriptor (pass 3 #10). That only makes the model's own reply wrong or its own session slow: a
  trailer missing at the deadline is `shell_lost` (§4.3), whose re-open is charged to the budget the model is spending.
  Nothing host-side trusts the trailer. The host-side read is capped independently (`output_discipline.hpp`).
- **Deadline (container).** PID 1 must (a) survive `SIGKILL` sent from inside the namespace — the kernel drops
  signals to a namespace init that has no handler for them, `SIGKILL` included — (b) reap orphans, and (c) **never fork
  in steady state**, so pid exhaustion cannot kill it (pass 3 #8). `tini` fails (a) because it exits with its child
  (pass 2 #3); a `sleep & wait` shell loop fails (c). Design: a small static pause-and-reap binary shipped with the
  engine and bind-mounted read-only (the Kubernetes pause-container pattern, minus its exit-on-`TERM`). On timeout:
  `docker exec <id> kill -KILL -1` → `shell_lost` (§4.3); files remain, and the command's partial writes are drained
  and committed by step 5 with `timed_out`. If the `exec` cannot even start (pids exhausted) → `docker rm -f` →
  `container_lost`, reported; the next command opens. Destroy stays `docker rm -f` (the keeper ignores `TERM`, so
  `docker stop` would wait out its timeout).

## 8. Lifetimes, ceilings, budgets

```cpp
struct LifetimeView { lifetime_event event; std::optional<run_end_reason> reason;
                      std::chrono::milliseconds idle_for, alive_for; std::uint32_t runs_since_open; };
template <class L> concept ShellLifetime = requires(L const& l, LifetimeView const& v) {
    { l.close(v) } -> std::same_as<bool>;   // may only close sooner than the engine's ceilings
};
```

The policy decides only **when a live shell closes**. It sees plain data, no authority, no handle. Built-ins:
`lifetime::PerRun` (close at run end), `lifetime::PerSession` (keep across runs). Opening is always lazy (first
`shell_exec`) — no prewarm in v1: a prewarm in `resume_tool_table` opens even when the resume cascades into another
suspension (pass 2 #11).

Engine-owned, whatever the policy says:
- **`LiveShellLimits{max_idle, max_alive, max_runs}`** — a required constructor argument, no unbounded default. Checked
  at every `shell_exec` and `on_context()`, and — because a composed provider is unreachable by the host
  (`containerd_shell_chat.cpp:110-113`) — also by **`on_run_end`**, and by the destructor. A `PerSession` shell in a
  session nobody touches again survives until the session object is destroyed; disclosed (§12), bounded by the
  session's own lifetime.
- **`AsyncQuota<LiveShellOpen>`**, charged per open (including re-opens after principal change, desync, container loss,
  `restart: true`). A host-sized budget like `RunCost`: `AsyncQuota` has no refill, so `PerRun` consumes one per run
  that uses the shell; the host sizes it for the session, as it already sizes `RunCost`. Documented, not hidden.
- `RunCost` per command; `StorageBytes` per commit (unchanged). **Disclosed:** each commit charges the whole tree's
  listing, not the delta (`ledger_impl.hpp:116`), and a live shell invites many small commands (`cd`, `ls`), so
  `StorageBytes` drains faster than under Tier 0. The incremental-drain follow-on (§10) is also the fix for this.
- **Locking, disclosed:** `exclusivity_` is held for a command's whole deadline, so `reset_to_turn` or a fork waits for
  a running command, and cancellation cannot interrupt one (the tool body uses `block_on`, as Tier 0 does,
  `mandatory_sandbox_provider.hpp:788`).

### 8.1 `on_run_end` (engine change, release-only)

Optional `on_run_end(RunEndView{reason}, EffectContext&) -> task<std::monostate>` — **release-only**: it never commits
(nothing is dirty) and cannot lose work. Wiring: a virtual `bound_on_run_end` on `AgentSessionCore`, an optional closure
on `ContextProviderDescriptor`, forwarded by `ComposedContextProvider`.

**It fires only on true run completion** (pass 3 #6): `final`, `max_turns`, `failed`, `canceled`, `exception` — and
only for a run that actually **started**. The fire point is "the core emitted `run_started` for this run", not "admission
passed", because refusals also happen after admission (`apply_dispatch_authority`, `run.approval_pending`,
`resolve_interaction`'s unknown-id and CodeAct branches). **Never on `suspended`**: the run is not over, and closing at
every approval pause would kill dev servers and spend an open per approval. It fires from the two public entry points,
`start_run` and `resolve_interaction`; the CodeAct/hook resolvers are private and called from `resolve_interaction`
(pass 2 #18). `start_background_task` needs nothing, because `shell_exec` is not `Backgroundable`.

**Release failures** (`docker rm` fails): the provider keeps the instance handle and retries at the next release point
and in the destructor; ADR-139's `ae_des_*` orphan reaping is the last net. Reported as a run event, never silent.

**Fork/copy of a live provider:** the copy gets a child branch (as Tier 0) and **no** live shell — it opens its own on
first use. A live container is never shared between two provider instances (I1).

## 9. Native live shell

- **`pwsh` only.** `bash` on this platform can resolve to WSL, whose Linux processes are outside the Job Object
  (pass 2 #13); Git Bash is a possible later addition once proven inside the job. `cmd.exe` cannot be framed. This does **not**
  close the WSL escape: from inside `pwsh`, `wsl.exe -e …` still starts Linux processes outside the job (pass 3 #9) —
  disclosed alongside WMI and Task Scheduler.
- The shell runs in a Job Object: `KILL_ON_JOB_CLOSE`, no breakaway, memory and active-process limits.
- **This is a widening of `cap::NativeExec`, stated as one:** longer life, background processes across commands, and the
  loss of the one-shot path's per-call `cwd = mount_root` and argv path check (`native_providers.hpp:193-205`). Processes
  started out-of-job by the OS on the shell's behalf (WMI, Task Scheduler) are not contained — the same class ADR-071 §6
  item 3 already discloses.
- **Opt-in is a monotone grant flag, not "fields set".** `cap_covers` treats an unset parent cap as uncapped
  (`capability.hpp:543-548`), so "tool appears when both caps are set" could be satisfied by any attenuated grant setting
  `UINT64_MAX`. So `cap::NativeExec` gains `bool live_session = false` (a child may set it only if the parent has it) and
  `session_wall_ms_cap`/`max_processes`. `NativeExec` is a plain aggregate, so "invalid" cannot be enforced at
  construction (pass 3 #9): a live grant missing either cap is **rejected where it is used** — the provider contributes
  no tool, and the per-command re-check refuses. `cap::decl::NativeExec` gains the three as template parameters
  (`to_capability` emits caps-free grants by design today, `capability.hpp:453-456`); the declarative grant schema and
  the policy-reachability tool carry them too (I6).
- The grant is re-verified before every command; revoked → refuse and kill the job. An in-flight command is not
  interrupted by revocation alone (no revoke callback); `session_wall_ms_cap` bounds it.
- A run event records each native live-shell start (ADR-070 audit).

## 10. Rejected alternatives and follow-ons

- **Lazy per-turn/per-run commit** (revisions 1–2) — the root cause of both passes' FATALs (§2).
- A lifetime parameter on `MandatorySandboxProvider`; a mode flag; a closed set of lifetimes (§3, owner direction).
- `--read-only` + tmpfs + `tar` streaming for disk bounds — needs a safe host-side extractor for a hostile archive
  (pass 2 #12); not in v1.
- **Follow-on (not this ADR): incremental drain** — list changed/deleted paths inside the container and copy only those,
  reusing the Ledger's content addressing for the rest. This is where the per-command cost goes away.
- Process snapshots (`docker commit`/CRIU); host-side parsing of `cd`/`export`.

## 11. Claims (to prove) — each with a discriminating positive control

| # | Claim | Control (must fail) |
|---|---|---|
| C1 | `cd`/env persist across `shell_exec`; `python -m venv .venv && . .venv/bin/activate`, then `python -c …` in the next command, works **and every commit succeeds** (links skipped and listed). | Tier 0 `run_command` pair → not persisted. Plant: scan with `is_regular_file` → the commit after `venv` fails. |
| C2 | One container per run for a 3-turn, 5-command run. | Plant: open per command → 5. |
| C3 | Every `shell_exec` commits its effect before returning, including **deletions**; a failed commit is the tool's error and forces a re-open. | Plant: drain into unwiped staging → deleted file reappears. Plant: swallow commit error → reply `ok` with Ledger unchanged. |
| C4 | **Nothing is lost on any run exit** — cancel between turns, failure, CodeAct `agent.ask` suspension after a `shell_exec`, fork from a tool body, composed provider: the Ledger holds every command whose reply was returned. | Plant: defer commit to `on_context` → cancel-between-turns loses the write. |
| C5 | Provider `reset_to_turn(n)` → next command sees turn n's tree **and** turn n's cwd/env — including when turn n's tree equals the current tree (cwd-only commands in between). | Plant: sync on tree digest → no re-open, wrong cwd. Plant: look the snapshot up under the appended checkpoint → no cwd/env. |
| C6 | A non-owner identity (e.g. an `on_behalf_of` delegate) is refused by `shell_exec` before anything runs, with a named error. | Plant: charge `RunCost` after the command → the delegate's command runs. |
| C7 | Timeout and the model's own `kill -KILL -1`: PID 1 lives, the **shell is lost** (asserted), the next command re-opens with the last snapshot's cwd/env, partial writes committed; under pid exhaustion (fork bomb at `--pids-limit`, watchdog-guarded) the fallback `docker rm -f` reports `container_lost`. | Plant: PID 1 = `sh -c "… sleep infinity"` → container gone. Plant: `sleep & wait` keeper under pid exhaustion → keeper dies. Plant: kill only the host `docker exec` client → `sleep` survives. |
| C16 | Every open destroys and recreates the container: after a reset that deletes `f`, `f` is absent in the container and never re-committed. | Plant: reuse the container on re-open → `f` reappears and is committed under the next command's author. |
| C17 | A commit failing after the command ran (`StorageBytes` exhausted) returns exit code + output + `commit_error`; the next command reports the discard. | Plant: Tier 0's error shape → output lost. |
| C8 | Framing: unbalanced quote, heredoc, forged trailer, inherited-fd write, binary output, 10 MiB output, `exit`, NUL rejected, 64 KiB command → no hang, bounded host memory, next command correct. | Plant: raw command without the wrapper → unbalanced quote hangs to the deadline. Plant: uncapped read → memory above cap. |
| C9 | Snapshot replay is injection-safe: cwd/env values with `'`, U+2019, newline, `$(…)` are restored literally in sh and pwsh. | Plant: single-quote escaping in pwsh → payload executes. |
| C10 | `LiveShellOpen` bounds re-opens (`restart: true` loop refused once spent); `LiveShellLimits` close at the ceiling via `shell_exec`, `on_context`, `on_run_end` — **composed** as well as bare. | Plant: no descriptor forwarding → composed shell outlives `max_alive`. Plant: uncharged restarts → unbounded. |
| C11 | `on_run_end` fires once per completed run that emitted `run_started` (final/max_turns/failed/canceled/exception), never on `suspended`, never on a refusal (admission, dispatch authority, `run.approval_pending`, unknown interaction id). | Plant: fire on "admission passed" → a bogus `resolve_interaction` closes a suspended run's shell. Plant: fire on `suspended` → an approval pause kills the dev server. |
| C12 | Background writer: its writes are committed by the next command under the shell starter's principal; torn-drain behaviour measured (with/without `docker pause`). | — (measurement; the result decides claim vs residual). |
| C13 | Native: tool absent unless the grant has `live_session` + both caps; a child cannot set `live_session` over a parent without it; revoke → refused + job killed; declarative and CRTP grants equivalent. | Plant: "caps set" as the opt-in → an attenuated `UINT64_MAX` grant enables it. |
| C14 | I3: a snapshot cwd of `/etc` / `C:\Windows` never reaches a host spawn cwd or `docker exec -w` (instrumented). | Plant: pass it → test fails. |
| C15 | Containment (memory-capped test + external watchdog): fork bomb / memory hog in the live container and the native job stay within caps. | Caps removed in a throwaway watchdog-guarded run → the cap is what stopped it. |

## 12. Residuals (proposed as accepted)

1. Per-command drain + commit cost remains (until the incremental-drain follow-on).
2. Background writers: writes after a run's last command are lost at close; a drain may capture a torn set (C12).
3. Processes, functions, aliases not restored; snapshots in memory only.
4. Disk outside `/workspace` is bounded only by the container lifetime (`PerRun`) and the host's storage driver.
5. The framing trailer is forgeable by the command; only the model's own view is affected.
6. Native: no worktree confinement; out-of-job spawns (WMI, Task Scheduler, `wsl.exe`); revocation effective at the
   next command.
7. A `PerSession` shell in an untouched session lives until the session object is destroyed.
8. The Ledger tree holds regular files only: after §4.1, symlinks are skipped and listed; empty dirs and modes are
   lost. All of these vanish on any re-open (venv interpreter links included).
9. Not in v1: task-branch tools in the live tier; `ContainerdExecutionSurface`; `bash`/`cmd.exe` natively; prewarm.
10. A timeout, the model's own `kill -1`, or `exit` ends every process in the shell; cwd/env survive via the snapshot.
11. `StorageBytes` is charged per full tree, not delta; `exclusivity_` is held for a command's deadline; cancellation
    cannot interrupt a running command (both inherited from Tier 0).
12. A reset issued directly on the Ledger (not through the provider) restores files but not cwd/env.

## 13. Red-team history

**Pass 1** (revision 1): 2 FATAL, 11 MAJOR, 7 factual errors — task-branch `run()` destroying the live container, flush
with no error channel, hook not reaching composed providers, `self_digest`-keyed snapshots, incomplete barrier list,
principal not fixed per run, `kill -1` killing the container, native widening, disk, additive drain. Revision 2 fixed
the structure (new type, fallible hooks) and kept lazy commits.

**Pass 2** (revision 2): 3 FATAL, 11 MAJOR, 4 MINOR. The FATALs: `assemble_context` swallows the flush error in a
composed provider; cancellation between turns and flush failure discard work the model already saw; `--init`'s `tini`
exits with its child so `kill -1` still kills the container. Majors included CodeAct suspension after dispatch, flushes
inside non-fallible copies, owner-only quotas vs "flush under the old principal", admission-denied callers closing
shells, torn background drains, non-hard ceilings, lifetime-budget quota, hostile tar extraction, WSL `bash` outside the
Job Object, `cap_covers` making "caps set" a forgeable opt-in, pwsh quote breakout across principals, `%b`/NUL/arg-limit
edge cases, snapshot key collisions, double-firing wrapper. Verified closed by pass 2: pass-1 #1, #4, #13.

**Pass 3** (revision 3): verdict — **the transaction model is structurally sound**; no new root-cause flaw. 1 FATAL for
the headline claim (the scan follows symlinks, so a venv fails every commit — inherited from Tier 0, verified by the
author at `fs_walk.hpp:66`), 7 MAJOR (reset appends a checkpoint so snapshot lookup and tree sync both miss; re-open into
a reused container resurrects discarded files; shell death with the container alive unmodelled; principal check
unreachable behind owner-only `RunCost`; `on_run_end` fired after post-admission refusals and on suspension;
commit-failure drops the command's output and `StorageBytes` charges full trees; keeper dies under pid exhaustion; native
`live_session` unenforceable on an aggregate and WSL still escapes from `pwsh`), 3 MINOR (framing self-subversion,
newline-stripping in replay and env every N commands, locking/cancel/fork/release gaps). **Revision 4** answers each:
§4.1 symlink prerequisite, §4.2 open = fresh container, §4.3 `shell_lost`, checkpoint-keyed sync and snapshots with the
provider's reset mapping (§5), owner-only use stated and the dead principal machinery removed (§4), `on_run_end` on
`run_started` completion only (§8.1), commit-failure reply carries output (§4 step 6), non-forking keeper plus `rm -f`
fallback (§7), use-site validation and decl template params plus WSL disclosure (§9), `readonly` D and missing trailer
= `shell_lost` (§7), env every command and newline-safe replay (§5), locking/fork/release rules (§8, §8.1); new claims
C16, C17, revised C1, C5, C6, C7, C11.

**Revision 3's answer** is §2's reframe (per-command commit removes every FATAL whose root is dirty state between hook
points: pass 2 #1, #2, #4, #5, #9's sync, #18's event contradiction) plus targeted fixes: keeper PID 1 (#3), starter
principal + identity-key comparison + per-principal replay (#6, #14), admission-gated release-only hook on public entries
(#7, #18), ceilings checked in the hook and destructor with the unreachable-host case disclosed (#10), host-sized open
budget documented and no prewarm (#11), tar design dropped (#12), `pwsh`-only native + monotone `live_session` flag + I6
parity (#13), `%b` with builtin probe and NUL rejection + env denylist (#15), `turn_index` keys (#16), `docker pause`
measured or disclosed (#8).
