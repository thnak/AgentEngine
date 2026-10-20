# ADR-209 — Persistent shell sessions: a live shell per run, sandboxed and native, with developer-chosen lifetimes

- **Status**: **Proposed — revision 5 (2026-10-01): implemented (GitHub issue #146, build steps 2–8), every claim
  run with its positive control, self red-team done (§15). Awaiting the project owner's Judge (step 9).** Revision 4
  (2026-09-28) was the design after red-team passes 1–3 (§13); where building it forced a change, §15.2 records it.
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

### 5a. Reconciliation with 010 §3a `ExecState` (found after pass 3, 2026-09-29) — DECIDED in §15.3: option (b)

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

*(Revision 5: the in-band trailer below was replaced by §14 item 1's out-of-band file transport, and the static
pause binary by a builtins-only `sh` keeper — §15.2 items 1–2. Kept as written for the record.)*

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

*(Revision 5: "`pwsh` only" became "the PowerShell family — `pwsh` or Windows PowerShell", with a containment probe at
every open that refuses a build whose children escape the Job Object; the Store/MSIX `pwsh` measurably does — §15.2
item 12.)*

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
   next command or check point (§15.5 M4), never mid-command; no identity boundary (§14 item 3 not evaluated).
7. A `PerSession` shell in an untouched session lives until the session object is destroyed.
8. The Ledger tree holds regular files only: after §4.1, symlinks are skipped and listed; empty dirs and modes are
   lost. All of these vanish on any re-open (venv interpreter links included).
9. Not in v1: task-branch tools in the live tier; `ContainerdExecutionSurface`; `bash`/`cmd.exe` natively; prewarm.
10. A timeout, the model's own `kill -1`, or `exit` ends every process in the shell; cwd/env survive via the snapshot.
11. `StorageBytes` is charged per full tree, not delta; `exclusivity_` is held for a command's deadline; cancellation
    cannot interrupt a running command (both inherited from Tier 0).
12. A reset issued directly on the Ledger (not through the provider) restores files but not cwd/env.
13. Lifetime ceilings are checked lazily, at `shell_exec`/`on_context`/`on_run_end`/destroy, never by a host timer
    (§15.5): an idle environment keeps running, bounded by its container/job limits but not in wall time, until the
    next check point.

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

## 14. Prior-art check (2026-09-29) — open items for the next revision

From `docs/research/2026-09-29-persistent-shell-sandbox-landscape.md` and `docs/research/2026-09-29-new-sandbox-types-2026.md`:

1. **Out-of-band completion signal.** Daytona and Cloudflare sessions write the command to a file, source it in the
   live shell, and signal completion with an exit-code file written after output is flushed — so printing cannot fake
   completion and no quoting is needed. Cheaper and sturdier than §7's in-band trailer; adopt for the sh transport
   unless the prove phase finds a reason not to.
2. **`PerRun` has no direct precedent.** Every live-shell product found ties a shell to an explicit session (Anthropic
   bash tool, SWE-ReX, OpenHands, Daytona, Cloudflare, `PSSession`). Keep `PerRun` as the default (owner decision) but
   say so, and keep `PerSession` first-class.
3. **Native identity boundary.** Every comparable native sandbox (Codex's Windows sandbox, `srt`, the Windows agent
   workspace, MXC session isolation) runs the agent under a separate low-privilege identity; §9's held `pwsh` in a Job
   Object has none. Evaluate Microsoft Execution Containers (preview, Build 2026) as the native tier's isolation layer
   before building §9 — it is OS-provided, so it does not conflict with the no-second-local-isolation decision.
4. Confirms §10: whole-process restore exists only at the VM-snapshot level (E2B, Firecracker, Perplexity SPACE);
   container checkpointing breaks live shells (gVisor, Docker checkpoint, Kata has none). Timeout behaviour in most
   products is "stop waiting, leave it running"; §7's kill-then-restore-cwd/env is stricter than all of them.

## 15. Revision 5 — implementation (2026-10-01, GitHub issue #146)

### 15.1 What was built

Branch `adr209-persistent-shell-impl`, one commit per build step (prerequisites #142–#145 verified on `main`):

| Step | What | Tests (all green) |
|---|---|---|
| 2 | `Ledger::head_checkpoint()`, `SandboxRuntime::run_live()`, the shared core (`persistent_shell.hpp`: `ShellSnapshot`, `LiveExecOutcome`, `PersistentShellSurface`, replay) | `test_sandbox_runtime_live` (H1–H2, R1–R10) |
| 3 | release-only `on_run_end` (`HasOnRunEnd`, `RunEndView`, `bound_on_run_end`, descriptor + `ComposedContextProvider` forwarding) | `test_rt_agent_session_run_end_hook` (E1–E9) |
| 4 | `DockerPersistentShellSurface` | `test_persistent_shell_core` (offline), `test_docker_persistent_shell_surface` (daemon, V1–V10, V6b) |
| 5 | `LiveShellSandboxProvider`, `ShellLifetime` (`lifetime::PerRun`/`PerSession`), `LiveShellLimits`, `AsyncQuota<LiveShellOpen>`, the reset mapping | `test_live_shell_sandbox_provider` (P1–P16) |
| 6 | `cap::NativeExec::{live_session, session_wall_ms_cap, max_processes}`, `cap::decl::NativeExec` params, `trust::parse_native_exec_grant`, reachability fixture | `test_native_exec_capability` (N10–N13), `test_policy_reachability` |
| 7 | `NativePwshSession`, `NativeShellSessionProvider` | `test_native_shell_session_provider` (W0–W7, N1–N8; real processes) |
| 8 | claims C1–C17 | `test_live_shell_sandbox_provider_live` (daemon, L0–L7) plus the above |

### 15.2 Design changes the implementation forced

1. **Out-of-band file transport (§14 item 1 adopted; replaces §7's `printf %b`/`eval`/trailer).** A server loop in
   the container reads a nonce from a FIFO, checks `c.<nonce>` with `/bin/sh -n` (a syntax error in a *sourced*
   file would kill the shell), sources it with fd 3 closed and stdin from `/dev/null`, and publishes
   `rc`/`pwd`/`environ` as `x.<nonce>` by rename. The client is one `docker exec -i` per command with the command on
   **stdin** (never argv: no `execve` limit, no quoting). Its reply is `AE1 <xlen> <olen> <blen>` followed by the
   three parts, parsed with every length bounded. A missing or malformed header is `shell_lost`. A syntax error is
   exit 2 and the shell survives (V3). The native tier uses the same model (files, `[ScriptBlock]::Create`, base64).
2. **PID 1 is a builtins-only `sh` keeper, not a static bind-mounted binary.** It opens a FIFO read-write on fd 3,
   unlinks it, and loops `read -t 5` + `wait`: it never forks after start (§7 (c)), reaps every ≤5 s (b), and, as a
   namespace init with no SIGKILL handler, survives a SIGKILL from inside (a). There is no binary to ship or mount. The
   server refuses an `sh` without `read -t` (the keeper would spin) with exit 98. **Measured while planting:** busybox
   `sh -c` execs its last command, so the Tier 0 shape `sleep infinity` *also* survives the in-container SIGKILL; what
   it lacks is reaping (20 zombies). V6b, not V6, is the keeper's discriminating check.
3. **sh replay is POSIX single quoting** (`'` becomes `'\''`), not `printf '%b'` octal literals: simpler, and exactly
   injection-safe (nothing is special inside single quotes). The pwsh replay stays base64.
4. **Timeout:** `docker exec <id> sh -c 'kill -KILL -1'`, then `shell_lost`; the partial output is fetched after the
   kill; if that exec cannot start, `docker rm -f` and `container_lost`. A killed process stays a zombie until the
   keeper's next tick (V5 counts live processes only).
5. **The snapshot is a diff against the open-time environment.** The denylist was widened to `PWD`, `OLDPWD`,
   `SHLVL`, `_`, `CDPATH`, `PSModulePath` and the prefixes `LD_`, `PS`, `BASH_FUNC_`, `DYLD_`; non-identifier names
   never enter. A cwd over 4 KiB is dropped, never cut (a cut path names another directory).
6. **A new class `DockerPersistentShellSurface`.** `DockerExecutionSurface` is unchanged apart from `run_argv`
   reporting a deadline kill and `DockerCliBackend::create` taking an optional PID 1 script.
7. **`shell_exec`'s static ceiling is `cap::decl::RunCommand`**, the same authority as Tier 0's `run_command` (run a
   command in this session's own sandbox), held differently. The dynamic gate is unchanged: owner-only `RunCost`.
8. **`ShellExecReply` has 16 fields** (`AE_JSON_SCHEMA`'s limit): the cwd is not echoed (the command can print it),
   `commit_error` is `"<code>: <message>"`, and truncation is `output_bytes > output.size()`.
9. **A charged `LiveShellOpen` is not refunded when the open then fails.** The attempt created and destroyed an
   environment, which is what that budget bounds. `RunCost` *is* refunded (nothing ran).
10. **`on_run_end` runs with the sandbox event sink bound** (bracketed like a tool call, ADR-170), so a failed release
    is a `sandbox_exec_finished{stage:"release", ok:false}` event of the run that completed (E9), emitted after that
    run's `run_finished`. Releases checked from `on_context()` have no sink and are counted in `release_failures()`.
11. **There was no declarative capability path at all.** `trust::parse_native_exec_grant()` is now the one place a
    YAML/JSON document becomes a `cap::NativeExec` (unknown keys, wrong types and out-of-range numbers are refused);
    `trust::live_session_grant_usable()` is §9's use-site rule. The reachability fixture holds a declarative and a
    CRTP live grant (both GRANTED) and a one-shot grant (DENIED), giving I6 parity.
12. **Native: the PowerShell family plus a containment probe.** MEASURED: the Store/MSIX `pwsh` 7.6 starts every child
    process OUTSIDE the Job Object it runs in (`IsProcessInJob` false; the active-process limit never applied; a
    `Start-Process` child survived `TerminateJobObject`), with no breakaway flag set. Windows PowerShell 5.1 is
    contained. Every open therefore starts a probe child and refuses the shell (`native_shell_session.job_not_enforced`)
    unless the probe is inside the job; the host chooses `pwsh` or `powershell`. `max_processes` counts the shell and
    must be ≥2 for the probe; the job's `ActiveProcesses` also counts the console host. The minimal environment gained
    a fixed `PATHEXT` (no `cmd`/`ping` by bare name otherwise); output is forced to UTF-8 and a BOM is stripped.
13. **Native snapshots are "the last one"** (the native tier has no checkpoints); a re-open replays it.
14. **The native per-command re-check is `contains(the grant the running session was opened under)`.** A narrowed
    grant (smaller `session_wall_ms_cap`/`max_processes`/memory) is treated as a revocation: refuse and kill the job (N4).
15. **§14 item 3 (Microsoft Execution Containers) was not evaluated.** The native tier ships with no identity boundary
    (the shell runs as the host user), as disclosed in §12 item 6.
16. **Empty directories vanish on a re-open** (§12 item 8), found live: a `cd` into an empty directory created by an
    earlier command falls back to `/workspace` after a re-open, because the Ledger tree has no directory entries.
17. **`reset_to_turn(n)` uses the existing `Ledger::checkpoint_at(n)`** (ADR-102) to map the appended checkpoint to
    turn *n*'s snapshot (snapshots are keyed by checkpoint `self_digest` in a table bounded to 256).

### 15.3 §5a decided: option (b), scoped

§5a recommended (a), "when both serve one worktree". As built, they never do: the live sandbox tier owns its own
`SandboxRuntime`, branch and container (§3: "two sandboxes, two branches, nothing shared"), and the native tier runs
on the host. A shared `cwd` would name a directory the other environment does not have. 010 §3a is amended: "every
`Runner`" means every runner over the same execution environment; each live tier keeps its own `{cwd, env}`
(`ShellSnapshot`); background processes in the live tiers are governed by §6, not 006 §6b.

### 15.4 Claims, tests, positive controls

Every control below was planted and observed to fail the named check, then removed.

| # | Checked by | Control and result |
|---|---|---|
| C1 | L1 (venv in `python:3.12-alpine`, 4 links skipped and listed, both commits ok), L0, V2, P2 | Tier 0 pair: export not carried (L0); scan following links: the venv commit fails (L1) |
| C2 | L2 (one container, 5 commands across turns) | re-open per command: L2 fails |
| C3 | L3, R3, P9 | uncleared staging: the deleted file comes back (L3) |
| C4 | P15 (real `AgentSession`, cancel between turns), P16 (model failure) | `run_live` deferring its commit: P15/P16 fail |
| C5 | P7 (incl. an equal tree), R4 | no reset mapping: P7 fails; no lost-shell mapping: P8 fails |
| C6 | R10, P10, P10b (a delegate in the owner's OPEN shell) | no `RunCost` gate: P10/P10b fail |
| C7 | V5 (incl. no live survivor), V6, V6b, V7, L6 | `sleep infinity` keeper: V6b (not V6, §15.2 item 2); host-side-only kill: V5's survivor check. *Not run:* the `sleep & wait` keeper plant (the keeper never forks, by construction) |
| C8 | V3, P1–P3 | no `sh -n`: V3. *Not run:* an uncapped-read plant (the reply parser's bounds are unit-tested in `test_persistent_shell_core`) |
| C9 | Q1–Q3, V9 (sh), W6 (pwsh) | `\'` escaping: Q1; quote-literal pwsh replay: W6 (the broken replay does not parse, so nothing is restored) |
| C10 | P3–P6, P12, P14, E7 | no run-end check: P3/P4/P12/P14; uncharged open: P6 |
| C11 | E1–E9 | no sink bracket: E9 |
| C12 | L5 (measurement) | a mid-write drain committed 4–5 of the first 10 lines a 1 line/s writer produced; torn sets are real; `docker pause` was not implemented, so §12 item 2 stands |
| C13 | N1, N10–N13, reachability | no monotone clause: N10 and the reachability oracle; `live_session` alone as the tool gate: N1; no per-command re-check: N3/N4 |
| C14 | V10 (argv log), W7 | snapshot cwd as `-w`: V10; snapshot cwd as the native spawn cwd: W7 |
| C15 | V7 (64-pid container), W5 (job process limit) | no job process limit: W5 (12 of 12 started). *Not run for Docker:* removing the container's pids limit under a fork bomb would load the shared Docker VM other work uses (CLAUDE.md machine safety) |
| C16 | L4, V8 | container reuse: L4 and V8 |
| C17 | R5, P9 | commit failure returned as an error: R5 |
| — | W0 | no containment probe: the Store `pwsh` opens with children outside the job |

Also found while proving: V7 was flaky (a `sleep 60` tail can itself fail to fork under pid exhaustion and end the
command early); the tail is now a builtin busy loop.

### 15.5 Adversarial self red-team (2026-10-01) — findings and fixes

A hostile, read-only review of the finished code (container escape, quota bypass, I2 widening via the native
session, leaked processes). No FATAL finding. Five MAJOR, all fixed. Each fix has a test, and each test was shown
to fail with its fix planted back out:

| # | Finding | Fix | Test | Control → result |
|---|---|---|---|---|
| M1 (I8) | A command the surface refused *after* `run_live` opened the environment (a NUL, or >256 KiB, on first use or `restart`) returned before the provider bookkept the open. No ceiling, policy or destructor check saw the container as held, and its background processes outlived every run. The "create" audit event was also missing (I4). | `open_hook` marks the attempt; `shell_exec` bookkeeps any open that left a live surface, before returning the refusal. | P17 | old bookkeeping (`outcome->reopened` only) → P17 ×2 |
| M2 (I8, C7) | The timeout kill ran the container's own `sh`, which the command (root in its container) can replace. A fake `sh` exiting 0 made the host report `shell_lost` and drain, while every process kept running. | After the in-container kill, the host checks with `docker top <id> -o pid,stat` (ps on the Docker host; nothing runs in the container) that only PID 1 is alive (zombies excluded, 5 tries). If not, `docker rm -f` and `container_lost`. | V11 (`#!/bin/busybox true` planted at `/usr/local/bin/sh`, then a busy loop) | trust the kill's exit status → V11 fails |
| M3 (I2) | The native shell was spawned with `bInheritHandles=TRUE` and no handle list. Every inheritable handle in the host (another session's `docker exec` pipes, sockets) was duplicated into a shell that lives for hours. native_jail_backend.cpp records this exact defect as fixed. | `STARTUPINFOEXW` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` = {NUL in, NUL out}. | W8 (a host pipe holding a secret, opened by handle value from pwsh) | no `EXTENDED_STARTUPINFO_PRESENT` → W8 reads the secret |
| M4 (I2) | Revocation was only checked inside `exec()`. A revoked grant stops the tool being offered, so no `exec()` comes, and an idle shell with its background processes kept running under a revoked grant. | `on_context` and `on_run_end` end a held shell whose opening grant is no longer contained in what the context holds (`end_if_revoked`). | N9 (both hooks), N4 (narrowing now caught at `on_context`; the next command runs in a fresh shell bounded by the narrower grant) | no `on_context` check → N4, N9, N11 fail |
| M5 (I8) | `cpu_ms_cap` was carried in the grant and compared by the re-check, but never applied to the held shell. | Applied as `JOB_OBJECT_LIMIT_JOB_TIME` (best-effort: job_object_limits.hpp's measured finding) AND checked from the host (`JobObjectBasicAccountingInformation`, user + kernel) after every command and at every check point. Unreadable accounting fails closed. It is a budget **per shell**: a re-open, charged to `LiveShellOpen`, starts a fresh one. | N12 (3000 ms cap, an 8 s spin) | both removed → N12 fails; host check alone → passes; kernel limit alone → passed in the run measured (it killed the shell), but it is not relied on |

MINOR findings:

- **Fixed.** Another identity could end the opener's native shell: the coverage check ran before the principal
  check. The principal check now runs first. N10 (a grantless stranger); control: old order → N10/N11.
- **Fixed.** A revoked shell's snapshot was replayed into the next shell. It is now dropped on revocation, and a
  snapshot only replays into a shell the same principal opens. N11.
- **Fixed.** Release events from `on_context` were dropped (no-op sink). The turn-start `on_context` call is now
  bracketed like a tool call. E10; control: no bracket → E10. (`resume_tool_table`'s `on_context`, on an
  approval resume, is still unbracketed: a release there is counted in `release_failures()` only.)
- **Fixed.** A lost container whose removal failed was marked closed and forgotten. `container_lost` now goes
  through `release()`, which keeps the handle and retries. P18; control: `timers_.closed()` → P18 ×2.
- **Fixed.** A `session_wall_ms_cap`/`wall_ms_cap` above INT64_MAX wrapped negative. Live grants above 2^62 ms are
  unusable (`trust::kMaxLiveSessionWallMs`), and `wall_ms_cap` saturates. N12 in test_native_exec_capability;
  control: the bound removed → fails.
- **Fixed.** The env denylist is now case-insensitive and covers .NET injection points (`DOTNET_*`, `CORECLR_*`,
  `COR_*`, `COMPLUS_*`). S2; control: prefixes removed → S2.
- **Fixed.** The native provider was movable while its tool descriptor captured `this`. It is now non-movable.
- **Fixed, without a discriminating test.** `run_in_flight_`/`pending_run_end_` are reset in `clear_core_state`,
  `fork_core_from` and `restore_from_record`. No test discriminates it: a fork is a fresh object, and a restored
  interaction is closed with nothing run and no terminal event (ADR-196 §7). So a restored suspended run never fires
  `on_run_end`, and its environment is released by the ceilings or the destructor.
- **Disclosed, not fixed.**
  - Run-end events are emitted after `run_finished`/`run_failed`/`run_canceled` (§15.2 item 10).
  - The live sandbox provider stays movable (`ComposedContextProvider` engages it by value). Its descriptor captures
    `this`, so it must not be moved while a suspended round holds a descriptor.
  - W0 proves the probe refuses a real escaping build (the Store `pwsh`), not a planted one.

**Ceilings are checked lazily** (red-team cross-cutting note, disclosed as §12 residual 13). `max_idle`, `max_alive`,
`max_runs`, PerRun and `session_wall_ms_cap` are evaluated at `shell_exec`, `on_context`, `on_run_end` and destroy.
No host timer enforces them. A run suspended on an approval that never comes, or a PerSession shell nobody touches,
keeps its environment and background processes until the next check point or the provider's destruction. Bounded
meanwhile: the container's pids/memory/CPU limits, or the job's process, memory and (now) CPU limits. Not bounded:
wall time. A host reaper thread is the follow-on.

### 15.6 CI

`test_docker_persistent_shell_surface` and `test_live_shell_sandbox_provider_live` need a daemon and are in both of
`ci.yml`'s exclusion regexes. `test_native_shell_session_provider` is built only with
`AGENTENGINE_WITH_NATIVE_PROCESS=ON`, which no CI leg sets; it exits 77 (ctest SKIP) when no `powershell` is on PATH.
