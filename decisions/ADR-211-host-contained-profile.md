# ADR-211 — `host-contained`: the host runs each session in a container, the engine runs commands natively inside it

**Status:** Proposed (2026-09-29). Design, one red-team round, and a prove phase against real containers
are done (§6, §7). Awaiting Judge. The engine-side implementation (§4, §9) is not built yet.
**Implements / amends:** 008 §2a (custom execution), `ExecutionSurface` (`sandbox/execution_surface.hpp`,
including the `drain_to()` mirror contract from issue #143), ADR-070 (Delegated Decision Seam).
**Related:** ADR-071 (native providers, Windows-only, not confined — issue #144), ADR-209 (persistent
shell sessions), ADR-171 (what an `ExecutionSurface` does not inherit), ADR-011 (egress).
**Proof:** `docs/planning/proofs/host_contained/` — `contained.cpp` (launcher, broker, engine stand-in)
and `run.sh` (the correct configuration plus every control, each in a resource-capped container).

## 1. The question

The project owner proposed (2026-09-29) a host that starts **one container per session**, runs the agent
application inside it, and lets the engine run shell commands natively there — "without sandbox, because
it is already inside a container".

Today that deployment has no correct configuration. `none` (008 §3) is unimplemented and refuses T3 code,
which a model-written command is. ADR-071's native providers are Windows-only and not confined.
`DockerExecutionSurface`/`ContainerdExecutionSurface` drive a container runtime from outside, and inside a
per-session container there is no daemon to drive — handing the session one would hand it the host.

The idea is sound: the container is a real boundary, and it protects **the host**. What it does not protect
is **the engine from its own commands**, and that is what this ADR adds.

## 2. What the container alone does not give

If the engine and the commands it runs share a container **and a uid**:

| The command can | Because | Breaks |
|---|---|---|
| read the model API key and every credential the engine holds | `/proc/<pid>/environ`, the engine's config files, same-uid `ptrace` where Yama allows it | **I2** |
| read, forge or erase the ledger, audit log, approval state | same uid owns them | **I4** |
| change what the engine does next (config, signals, a planted binary) | same uid | **I3** |
| starve the engine of processes or memory | one `pids`/memory budget, no per-principal limit | **I8** |
| use the engine's network reach, including its loopback listeners | one network namespace | **I2** |

**Measured, not argued** (§7, the `--same-uid` controls): with one uid the executor read the secret from the
engine's environment and its private file, appended to an existing ledger record, and — the moment the
per-command reap ran — killed the engine outright.

So the design keeps the container as the host's boundary and adds the cheapest isolation Linux has that
needs no privilege at run time: **a second uid**, plus the handful of mechanisms that make a second uid
actually sufficient inside one container.

## 3. Prior art

- **Agent sandboxes of 2026** (Docker Sandboxes, E2B, Daytona, Sprites, Claude Code / Codex dev containers;
  `docs/research/2026-09-29-new-sandbox-types-2026.md`): all put the *commands* in a box and keep the
  credential holder **outside** it or behind a proxy. None run the credential holder and the model's shell as
  one principal. This ADR keeps that property while moving both into one container.
- **Privilege separation** (OpenSSH, Chromium's broker/renderer, Postfix): a credential holder talks over a
  pre-opened channel to an unprivileged worker under another uid; the worker cannot reach the holder's
  memory or files. This ADR is that pattern, sized for one container.
- **`ContainerdExecutionSurface`** (this repo): the in-place shape — the command's workspace IS staging and
  `drain_to()` is a no-op, which issue #143's `drain_to()` contract makes correct. This surface is another
  in-place surface.

## 4. The design

### 4a. Two principals, fixed before the engine starts

| | uid | owns | can |
|---|---|---|---|
| **engine** `E` | non-zero, host-chosen | the application, secrets, ledger store, audit log, config, a private `TMPDIR` | read/write the workspace; talk to the broker |
| **executor** `X` | non-zero, `≠ E`, **used by no other process on the host** | nothing outside the workspace | read/write the workspace, `/tmp`, `/dev/shm`; whatever the image leaves world-readable; **no network** |

Group `W` contains both. The engine runs with **umask `077`**: its own files are private by default. The
workspace directory is `E:W`, mode `2770`.

"Used by no other process on the host" is a real requirement, found by measurement: `RLIMIT_NPROC` counts
every process of a uid in the user namespace, and without user-namespace remapping that is the **host's**.
With `X` = a real host user (uid 1000 under WSL), the executor could not fork at all. A host must pick an
unused uid or run the container with userns-remap; the attestation (§4e) states which.

### 4b. `agentengine_contained_launch` — the only process that ever holds privilege

The container entrypoint, shipped by this project. Starts as root and, before any application code runs:

1. Installs the executor's network rules (§4f). This is the only reason it may need `CAP_NET_ADMIN`.
2. Creates **one `socketpair(AF_UNIX, SOCK_SEQPACKET)`**. No socket file exists, so nothing can connect to the
   broker except the process handed the other end.
3. Forks the **broker**: `oom_score_adj = 1000`, then `setgroups([W])`, `setresgid`, `setresuid(X)`,
   `PR_SET_NO_NEW_PRIVS`, **`PR_SET_DUMPABLE 0`**, **`PR_SET_CHILD_SUBREAPER`**, `clearenv()`.
4. Sets its own `oom_score_adj` below the executor's, drops to `E` the same way, and **forks** the host
   application with the engine's end of the pair in `AE_CONTAINED_BROKER_FD` and the broker's pid in
   `AE_CONTAINED_BROKER_PID`.
5. **Stays as PID 1, running as `E`**: an init that only waits, reaps the engine's orphans, and exits with the
   engine's status.

After step 4 no process in the container holds a capability. The engine never needs `CAP_SETUID`.

Two of these were found by measurement, not design:

- **The engine must not be PID 1.** `kill(-1)` never signals init, so with the engine as PID 1 "the engine
  survives the executor's `kill(-1)`" passed whatever the uids were — a vacuous claim (red-team, weak claim
  P7). With the launcher as PID 1, the same-uid control shows the engine **killed** (§7).
- **The broker must be the child subreaper.** Without it, a double-forked daemon reparented to PID 1, was
  killed by the reap, and stayed as an executor-owned zombie in the engine's own process tree (C3 measured
  1 left). As subreaper, the broker inherits every orphan and collects it.

### 4c. The broker — spawns, caps, reaps, repairs, as `X`

Per request `{id, command, wall_ms, cpu_s, as_bytes, nproc, fsize_bytes, output_cap}`:

- runs `/bin/sh -c command` in a new session, `cwd` = the staging root, `umask 0007`, an **explicit minimal
  environment** (never the broker's, which is cleared), the rlimits set;
- `RLIMIT_NPROC` bounds every `X` process at once, so a fork bomb stops at the cap and never reaches the
  engine's own ability to fork;
- collects output up to `output_cap`, enforces `wall_ms`;
- **after every request, reaps:** `kill(-1, SIGKILL)` as `X` signals every process `X` may signal except the
  broker itself — including anything that double-forked or `setsid`'d — and a blocking `waitpid` loop
  collects them (all are the subreaper's children by then);
- **then repairs:** walks the staging tree without following links and, for every entry `X` owns, restores
  group access (directories `2770`, files `g+rw`) and group `W`. The executor can `chmod 700` its own
  directory to lock the engine out of it; measured, that made the engine's delete fail with
  `Permission denied` until this step was added. Only `X` can fix `X`'s files, and after the reap no
  executor process remains to race it;
- **then sweeps** `X`-owned entries out of `/tmp` and `/dev/shm`, which would otherwise outlive the command
  and, on tmpfs, hold memory.

**The broker is part of the executor principal.** An executor process can kill it (measured: C9). It cannot
`ptrace` it or read its `/proc`, because the broker is not dumpable (measured: P8, with a dumpable-broker
control that the preflight refuses). What the broker reports is command output, which I3 already treats as
data — but **whether the reap happened is not the broker's word to give** (§4d).

### 4d. `ContainedExecutionSurface` — the only way in

Not a `SandboxBackend` and not registered in `SandboxBackendRegistry`, which takes only `SandboxBackend`s
(red-team #1: the first draft said "registered `named_only`", which the registry cannot express, and a surface
is a compile-time template argument of `SandboxRuntime`, never looked up). So `Strict` cannot reach it by
construction, and **the adoption call itself is the gate**:

```cpp
result<ContainedExecutionSurface> adopt_contained_broker(HostContainmentAttestation const&, EffectContext&);
```

- reads `AE_CONTAINED_BROKER_FD` / `_PID` once, unsets them, marks the fd `FD_CLOEXEC` (nothing the engine
  runs later inherits it), and fails on a second call;
- requires the attestation (§4e), runs the preflight (§4e), and **records both** as one audit event (I4) —
  a host cannot get a surface without all three;
- returns a **move-only** surface bound to its first staging root: `reset()` with a different root fails.
  One surface, one `SandboxRuntime`, one session (I1). Two sessions sharing `X` would reap each other's
  commands.

`ExecutionSurface` operations:

- `reset(root)` — `root` must be the bound staging root, a subdirectory of the workspace (never the mount
  point itself: `materialize()` deletes and recreates its root, red-team #2). Re-opens the tree the engine
  just materialized to the group (`2770`/`0660`: the engine owns those files, so it can; with umask `077`
  they would otherwise be unreadable to commands — C11), then runs the **per-request checks**;
- `run(command)` — one broker request, then the per-request checks;
- `drain_to(root)` — no-op for the bound root (in place); anything else is refused.

**Per-request checks, by the engine, not the broker** — before reporting any result:

1. the reply arrived within `wall_ms + margin` (`SO_RCVTIMEO`) and carries the request's `id`;
2. **no process with uid `X` exists other than the broker**, counted by the engine from `/proc/*/status`
   (world-readable). This is C3's evidence — the first draft took the broker's own count;
3. the staging root is still `E:W 2770`.

Any failure **disables the surface permanently**: every later `run()` fails, the tool reports an error, and
nothing ever runs unsplit. A killed or stopped broker is exactly this case (C9).

Everything built on `ExecutionSurface` works unchanged — `SandboxRuntime::run()`, `MandatorySandboxProvider`'s
`run_command` (Tier 0), rollback. The "native shell" the owner asked for is `run(command)`: a real `/bin/sh`
in the real container, with the ledger checkpointing every command. **Not in v1:** task branches (each needs
its own staging root and runtime, and one surface serves exactly one); a construction under this profile
that asks for one fails.

### 4e. Attestation and preflight

**`HostContainmentAttestation`** — host code, never model output (ADR-070 property 4): who runs the
container; runtime and image digest; `E`, `X`, `W` and whether `X` is host-unique or userns-remapped; the
container's memory and pids limits and whether `memory.oom.group` is set; **the paths of every secret the
host put in the container**. The engine cannot see out of the container, so it does not verify the
container itself; it records the attestation on every adoption (ADR-070 property 5).

**Preflight** — what the engine does not take on trust. Any failure refuses adoption; there is no fallback.

| P | How | Must |
|---|---|---|
| **P0** | the engine reads the broker's uid from `/proc/<broker>/status` — **before any command runs** | differ from the engine's, and neither is 0 |
| P1 | `id -u` via the broker | equal the attested `X` |
| P2 | read the engine's own `0600` canary **and every attested secret path** | fail |
| P3 | read `/proc/<engine>/environ` and `/proc/<engine>/mem` | fail |
| P4 | create, read, append to an existing record in the ledger and audit directories | fail |
| P5 | create a nested tree in the workspace; the engine deletes it | succeed |
| P6 | connect to a listener the engine opens on loopback (v4 and v6) | fail |
| P8 | read `/proc/<broker>/environ` | fail (broker not dumpable) |
| P9 | `fs.protected_hardlinks`, `fs.protected_symlinks` | both 1 |
| P10 | `memory.oom.group` of the container's cgroup | 0, or attested |

Every probe appends a marker and must show it: "the output lacks the secret" must never pass because the
probe did not run. **Measured:** the first harness run passed C1 and C2 on an empty reply from an
already-dead broker.

**P0 exists because of a measurement.** With `X = E`, the broker's reap after the first probe killed the
engine, so a preflight that learns `X` by running `id -u` never reaches its own refusal. The engine reads the
broker's uid itself, from the kernel, before sending anything.

A secret the host mounts world-readable (Docker secrets default `0444`, Kubernetes secret volumes `0644`,
red-team #10) fails P2 and refuses adoption; the host must mount it `0400` owned by `E`. A secret passed in
the engine's **argv** is world-readable in `/proc/<pid>/cmdline` and cannot be protected; the adoption docs
say so.

### 4f. Network: the executor has none

The launcher installs, for both `iptables` and `ip6tables`, `OUTPUT -m owner --uid-owner X -j REJECT`. The
rule sees the socket's uid, so it covers every address — including **loopback**.

That is needed even with `--network none`. **Measured:** `--network none` still has loopback, and without the
rule the executor connected to a listener the engine opened on `127.0.0.1` (§7, "N1 without rules"). So the
host chooses only whether the *container* has an external route (the engine's own egress); the executor's
rules are mandatory either way, and P6 checks them.

Not in v1: executor egress through ADR-011's proxy. ADR-011's proxy is an in-process `fetch()`, not a
listener (red-team #4), so there is nothing to allow a connection to. A host that needs executor egress
needs a new ADR.

## 5. Falsifiable claims

Every claim has a control that must fail it; `run.sh` fails if any control passes.

| # | Claim | Control that must fail it |
|---|---|---|
| C1 | The executor cannot read the engine's secret (private file, environment) | `--same-uid`: P0 refuses; `--leak-canary`: P2 refuses |
| C2 | The executor cannot create, read, or append to the ledger's existing records | `--same-uid` |
| C3 | Nothing a command starts outlives it, **counted by the engine** | `--no-reap`: 1 process left |
| C4 | During an executor fork bomb the **engine can still fork** (10/10) and serves the next request | `--no-nproc`: 0/10 |
| C5 | The executor cannot connect to the engine's loopback listener; the engine can | no rules: connects |
| C6 | A misconfigured container refuses adoption before any command runs | `--same-uid` (P0), no broker fd |
| P5 | After the executor `chmod 700`s its own tree, the engine can still delete it | `--no-fixup`: `Permission denied` |
| P8 | The executor cannot read the broker's `/proc` | `--broker-dumpable`: P8 refuses |
| C9 | The executor can kill the broker but not the engine; later requests fail closed | `--same-uid --force`: engine killed |
| C10 | Executor files in `/tmp` and `/dev/shm` do not outlive the command | `--no-tmp-sweep` |
| C11 | Commands can edit what the engine materialized, though the engine writes with umask `077` | `--no-normalize` |

Not proven here, owed by the implementation: **C7** (adoption without an attestation, or with a failed
preflight, returns no surface) and **C8** (`SandboxRuntime` over this surface commits writes and deletions
exactly — the #143 D5 shape, live). Both are engine-side C++ tests.

## 6. Red-team (round 1, 2026-09-29)

An adversarial pass over the first draft. It stopped early, so its output is design findings rather than
attack sequences. Separately, the prove phase found five problems by measurement. All are addressed above.

| # | Finding | Severity | Resolution |
|---|---|---|---|
| R1 | The registry takes only `SandboxBackend`s; a host could adopt the broker and skip attestation and preflight | MAJOR | adoption is the gate (§4d) |
| R2 | `materialize()` deletes and recreates its root, losing `E:W 2770` and failing on a mount point | MAJOR | staging root is a subdirectory; `reset()` re-establishes modes (§4d) |
| R3 | Task branches and a second runtime do not fit one surface; two sessions sharing `X` break I1/I4 | MAJOR | move-only, bound, one runtime; task branches not in v1 |
| R4 | ADR-011's proxy is not a listener; the N1 Unix-socket path does not exist | MAJOR | executor has no network in v1 (§4f) |
| R5 | The reap is the executor's own word; stop or kill the broker and nothing reaps | MAJOR | engine-side per-request checks; broker loss disables the surface (§4d) |
| R6 | Orphans reparent to the engine as PID 1 and stay as zombies | MAJOR | subreaper + launcher as PID 1 (measured, §4b) |
| R7 | Engine code hardcodes `/tmp` paths the executor shares | MAJOR | must-fix before shipping (§9) |
| R8 | The scan trusts executor-controlled files (type, links, size) | MAJOR | scan runs only after the engine-verified reap, so the tree is quiescent; P9; a total-bytes cap on the scan (§9) |
| R9 | The executor can `chmod` its files to lock the engine out | MAJOR | broker repair step (measured, P5) |
| R10 | Real secret mounts are world-readable, argv is public, the broker inherits the environment | MAJOR | attested secret paths probed by P2; broker `clearenv()`; engine umask `077` |
| R11 | tmpfs files outlive the reap; `memory.oom.group` kills the whole container | MAJOR | `/tmp`, `/dev/shm` sweep (C10); P10 |
| R12 | `execute_code`, the PDF worker and the native jail run as `E` and bypass the split | MAJOR | out of this profile's scope, stated (§8) |
| R13 | The broker channel protocol is unspecified | MINOR | request `id`, one in flight, engine-side size limit and timeout, `FD_CLOEXEC` (§4c, §4d) |
| M1 | The executor uid must be unused on the host (`RLIMIT_NPROC` is per uid, host-wide) | MAJOR | §4a requirement; attested |
| M2 | `--network none` still has loopback | MAJOR | rules mandatory in both modes (§4f) |
| M3 | With `X = E`, the first reap kills the engine before the preflight can refuse | MAJOR | P0 reads the broker's uid before any command |
| M4 | Probes passed vacuously on an empty reply | MAJOR | run marker on every probe |
| M5 | A fork-bomb check of "engine works after" cannot fail | MINOR | C4 measures the engine forking *during* the bomb |

Weak claims in the first draft, now fixed: P7/C9 (vacuous with the engine as PID 1); C3 (the broker's own
count); C2 (only file creation); P6 (one host canary, no loopback).

## 7. Prove phase (2026-09-29)

`docs/planning/proofs/host_contained/run.sh`, on WSL2 (kernel 6.6.87.2, Yama `ptrace_scope` 1,
`fs.protected_*` 1), Docker, Alpine, every container capped at `--pids-limit 256 --memory 512m --cpus 1`.
One static binary plays launcher, broker and engine.

```
PASS  correct configuration, N2
PASS  correct configuration, N1 + rules
PASS  finding: N1 without rules leaks loopback
PASS  control: X = E refused before any command
PASS  control: readable canary refused
PASS  control: dumpable broker refused
PASS  control: no reap breaks C3
PASS  control: no nproc breaks C4
PASS  control: no fixup breaks P5
PASS  control: no N2 rules breaks C5
PASS  control: no broker fd refused
PASS  control: X = E, forced past preflight: the reap kills the engine
PASS  control: no reset() normalize breaks C11
PASS  control: no /tmp sweep breaks C10
run.sh exit=0
```

The correct configuration passes 11/11 checks. What it does not prove is listed in §5 (C7, C8) and §9.
Preflight rows the harness does **not** exercise, owed by the implementation's tests: P2 over attested
secret paths (the harness probes only its own canary), P6 over IPv6 (the rule is installed, the probe is
IPv4), P9, and P10.

## 8. Residuals (disclosed, not closed)

- **The container itself** — kernel exploits, runtime bugs, the host's own limits — is the host's claim,
  recorded by the attestation, not verified.
- **Anything world-readable in the image** is readable by the executor. P2 checks the attested secret paths,
  not every file a host might have put somewhere.
- **The executor can kill or stop the broker.** That disables the surface for the rest of the session
  (fail closed, measured); it is a self-inflicted denial of service, never a bypass.
- **Engine code paths that are not an `ExecutionSurface`** — `execute_code`'s mediated interpreter, the PDF
  worker, the native jail — keep their own boundaries and run as `E`. This profile does not cover them; a
  host that enables them in the same container gets what those subsystems already provide, no more.
- **Memory.** `RLIMIT_AS` is per process; `nproc × as_bytes` plus tmpfs bounds the executor, and
  `oom_score_adj` points the OOM killer at it first. A container limit below that bound can still pressure
  the engine; the attestation states the limit.
- **Same-kernel side channels** between the two uids are not addressed.

## 9. Owed before this profile ships

1. The engine-side implementation: `agentengine_contained_launch`, the broker, `ContainedExecutionSurface`,
   `adopt_contained_broker()`, the preflight — with C7 and C8 as C++ tests, and `run.sh`'s scenarios as a
   Linux CI job (daemon-requiring, so hand-added to `ci.yml`'s lists).
2. Remove the hardcoded `/tmp` paths (`tools/extract_pdf_text.hpp:255`,
   `backends/native_jail/linux_native_jail_backend.hpp:89`) in favour of the engine's private `TMPDIR` (R7).
3. A total-bytes cap on `RealIoFileSystem`'s scan, failing closed, so a command that fills the workspace
   cannot make the engine read unbounded data before any quota is charged (R8).
4. A second red-team round on the implementation, not the design.

## 10. Decision

Adopt the design in §4 as the `host-contained` profile, pending Judge. The container is the host's boundary;
the second uid, the broker and the engine's own per-request checks are the engine's. Neither substitutes for
the other.
