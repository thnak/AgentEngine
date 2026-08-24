# ADR-088 — KataBackend abuse-corpus (008 §9 G1/G2) promotion-gate evidence, and a rejected
# OOM-classification design

Status: Proposed (design → independent red-team → fix → implement → local verification complete,
2026-08-24; awaiting project-owner Judged sign-off)

## 1. The question

`decisions/ADR-087-sandbox-spec-capability-enforcement.md` closed the `SandboxSpec::capabilities`
gap for `KataBackend`. `kata_backend.hpp`'s own header comment still named, honestly, a real
outstanding gap: no `008-Sandbox-and-Isolation.md` §7/§9 abuse-corpus test exists for this backend
at all, unlike `LinuxNativeJailBackend`/`NativeJailBackend`, which both have one
(`test_native_jail_abuse_corpus_{linux,windows}.cpp`). This ADR is that evidence — and the story of
a design that was investigated, red-teamed, and correctly rejected before it could ship as false
containment evidence.

## 2. What was attempted first, and rejected

The initial plan was to close `KataBackend::exec()`'s missing `exec_outcome_class::oom`
classification (it currently only ever returns `timeout`, `ok`, or `crash` — never `oom`, even when
`SandboxSpec::limits.memory_bytes` is set and the guest OOM killer kills the process) via a
heuristic mirroring `LinuxNativeJailBackend`'s own exit-code-137 fallback
(`linux_native_jail_backend.cpp:488-500`): if a memory cap was set and the exec's exit code was 137
(128+SIGKILL), classify as `oom`.

An independent adversarial red-team pass against this design, run before any code was written,
found it **FATAL** and it was dropped. The reasoning (verbatim from the review, condensed):

> `LinuxNativeJailBackend`'s 128+signal detection rests on a documented Linux kernel `wait()` fact:
> the guest process is PID 1 of its own `CLONE_NEWPID` namespace, and the kernel reports a
> namespace-init process's signal death to its own parent as `WIFEXITED`/`WEXITSTATUS==137` instead
> of `WIFSIGNALED` — a fact about a process that backend's own `waitpid()` directly reaps.
> `KataBackend`'s situation is structurally different: `run_ctr()`'s `waitpid()` reaps the
> **host-side `ctr` CLI process**, not the guest task. `outcome.exit_code` reflects `ctr`'s own
> process exit, three RPC hops removed from the guest's actual death (guest kernel → kata-agent →
> containerd-shim-kata-v2 → containerd daemon → `ctr` CLI). Whether any of those hops preserves
> "killed by SIGKILL" as 137 in the value `ctr` finally exits with is a containerd/kata-agent
> implementation convention, not a POSIX kernel guarantee — plausible, but categorically different
> evidence than the kernel fact native-jail relies on. **Unverified, not safe-by-default.**

Two further, independent problems the same pass found compounded the rejection:

- **No fallback for the direct-SIGKILL case.** `run_ctr()`'s own timeout path already SIGKILLs the
  host `ctr` process directly; when that (or an unrelated host-level OOM) kills `ctr` itself,
  `WIFEXITED` is false and `exit_code` is forced to `-1`, never `137` — the heuristic silently
  misses this class entirely, an asymmetry the original design didn't account for.
- **`memory_capped` (presence-only bool) loses the one fact the heuristic needs.** Gating on "was
  `memory_bytes` set at all" rather than "is the cap actually tighter than the guest's real default"
  means a caller who sets an enormous `memory_bytes` (one that could never plausibly cause an OOM)
  still gets every unrelated 137 misclassified as `oom`. Fixing this would require knowing the
  guest VM's real unconfigured default memory size — which **is not documented anywhere in this
  repository** (checked directly: `src/backends/kata/`, `docs/planning/microvm-first-party-backend-
  design-draft.md`, and `kata_backend.hpp`'s own header comment were all grepped; the "~2GiB
  default" the original design assumed was itself an unsourced claim, not a fact recorded anywhere
  in this codebase). Without it, the abuse-corpus test's own OOM positive control has no way to
  pick a decidable expected outcome ahead of time — it would need an empirical run against a real
  Kata deployment first, which this session cannot reach.

**Decision: `exec_outcome_class::oom` stays unreachable for `KataBackend`.** This is recorded as an
investigated-and-rejected gap, not a silently-unattempted one — matching this project's precedent
for closing an open question in the negative (e.g. the subinterpreter-feasibility spike, ADR-002
§6 item 5) rather than leaving it ambiguous.

## 3. A second, separate, previously-undisclosed gap the same red-team pass found

Reading `exec()`'s timeout path closely (required to evaluate the exit-code-137 design at all) the
same review surfaced a real, **BLOCKING** gap unrelated to OOM classification:

`run_ctr()`'s timeout handling (`kata_backend.cpp`, the `outcome.timed_out` branch) calls
`kill(pid, SIGKILL)` where `pid` is the **host-side, `posix_spawn`'d `ctr` CLI process** this
backend directly manages — not the guest-side process that CLI was attached to via
`ctr tasks exec --exec-id <id>`, for which this backend has no host-visible pid at all. Before this
ADR's fix, killing the CLI wrapper on a `wall_ms` timeout let `run_ctr()` return promptly with
`timed_out=true`, but the actual guest workload (e.g. `while true; do :; done`) very likely kept
running **orphaned inside the persistent `sleep infinity` container** until a later `exec()` or
`destroy()` call happened to reap it. `exec_outcome_class::timeout` was being returned without the
workload having actually stopped — a real defect in the 008 §9 G2 containment story for this
backend's single most basic abuse case (infinite loop), found only because writing an honest
abuse-corpus test forced a close read of the exact mechanism being tested.

## 4. The fix (this ADR)

`KataBackend::exec()`'s timeout branch now issues a second, best-effort `ctr` call after
classifying the outcome as `timeout`:

```cpp
auto kill_outcome = run_ctr(
    {"ctr", "tasks", "kill", "--exec-id", exec_id, "--signal", "SIGKILL", inst.container_id});
```

using the **same `--exec-id`** the timed-out call itself minted, targeting the guest-side process
directly rather than only its host-side CLI wrapper. Failure is not silent: a stderr log line names
the handle, exec-id, and container, matching this file's own existing "at least try, log if it
fails, never let a cleanup failure block the real result" posture (`destroy()`'s own established
pattern). Like every other `ctr` CLI surface this file assumes, the exact flag syntax
(`ctr tasks kill --exec-id <id> --signal SIGKILL <container>`) is **not independently re-verified
against a live Kata deployment this session** — no such deployment is reachable here; a wrong
assumption fails into the log line, not into a silently-wrong `timeout` classification (the
classification itself was already decided before this call runs, so a failed kill cannot flip a
real timeout into a false `ok`).

A separate, structurally similar risk was found but deliberately **not** fixed this pass, and is
named rather than silently left implicit: the unbounded-output case's host-side pipe-close (when
`output_bytes` is reached) is not proven to propagate as a guest-visible signal the way the timeout
path's explicit kill now does — a producer process could plausibly also survive orphaned after its
output stops being captured. Scope-bounded to the timeout path, which is the one the red-team
pass actually investigated and found concretely broken; the output-cap path is disclosed as an
open, analogous, unconfirmed risk in `kata_backend.hpp`'s own header comment.

## 5. The abuse-corpus test (`tests/test_kata_backend_abuse_corpus_linux.cpp`)

Three cases, gated identically to the existing Kata tests (`NOT WIN32 AND
AGENTENGINE_BUILD_KATA_BACKEND` + `AGENTENGINE_KATA_SANDBOX_TESTS`, requiring a real
Kata/containerd deployment this session cannot reach — compile-checked, not executed, same
disclosed limitation every Kata test in this tree already carries):

- **Infinite loop / `wall_ms`** — contained, with BOTH the usual "a longer wall_ms budget lets the
  spin run measurably longer" positive control AND a second, stronger check this ADR's own fix
  makes possible: the spinning guest process writes an incrementing heartbeat file to a bind-mounted
  host directory; after the timeout fires, the file's value is read twice, one second apart. If the
  guest-side kill in §4 didn't actually work, the heartbeat would keep incrementing after `exec()`
  returns — this is a real, code-level positive control for the specific defect this ADR fixes, not
  a restatement of the existing native-jail corpus's own weaker "the host call returned in time"
  shape.
- **Unbounded output / `output_bytes`** — captured-stdout-never-exceeds-cap, with a looser-cap
  positive control. Same claim shape as `LinuxNativeJailBackend`'s own corpus (about captured bytes
  only) — does **not** claim the guest producer itself is killed, per the disclosed, unfixed risk in
  §4.
- **Fork bomb / `pids`** — deliberately **not** a containment case. `ResourceLimits::pids` has no
  mechanism wired for this backend at all (Slice 2's own unchanged gap). Silently omitting the case
  (as the original Slice-2/3 header comments implicitly did) would make this corpus non-comparable
  to native-jail's own four-case shape without saying so; a literal unbounded fork bomb against a
  guest with no `pids` cap risks hanging or crashing the whole guest VM, a real machine-safety
  concern (CLAUDE.md: "hostile tests are themselves resource-capped"). The case instead runs a
  bounded, `wall_ms`-capped probe (50 background children) and asserts only that `exec()` returns a
  result — no containment claim is made or tested, and the test's own comment says so explicitly.
- **OOM / `memory_bytes`** — not covered, per §2's rejection.

## 6. Residuals, carried forward explicitly

- `exec_outcome_class::oom` remains unreachable for `KataBackend` — investigated and rejected (§2),
  not merely unattempted. Closing it for real would need either a host-observable signal this
  codebase does not currently have access to across the VM boundary (e.g. parsing kata-agent's own
  structured exit reporting, if any exists, or a documented containerd Task API convention verified
  against a real deployment) or accepting a materially different, better-scoped heuristic than the
  one rejected here.
  - Two of `exec()`'s error paths report `crash` for what may in fact be an OOM-driven signal-kill:
    an unattributed `exit_code == -1` currently falls into the pre-existing generic
    `exit_code == 0 ? ok : crash` branch (since `-1 != 0`), which is unaffected and unchanged by
    this ADR — named here rather than left implicit alongside the OOM gap it's related to.
- The unbounded-output guest-process orphan risk (§4) is disclosed, not fixed, this pass.
- `ResourceLimits::pids`/`fds`/`disk_bytes`/`net_bytes` remain unenforced for `KataBackend`
  (Slice 2's own unchanged gap) — the fork-bomb case documents this rather than closing it.
- The `--exec-id`-targeted `ctr tasks kill` flag syntax this ADR's own fix depends on is not
  independently re-verified against a live Kata deployment this session (none reachable) — carried
  forward from every other `ctr` CLI assumption already in this file, not a new class of risk this
  ADR introduces.
- No real Kata/containerd/`kata-clh` deployment was reachable anywhere in this session (a Windows
  dev box; WSL Ubuntu is available with a real Linux g++/cmake/ninja toolchain and was used for
  compile verification, see §7, but has no Kata Containers deployment of its own) — the new test
  remains **un-executed**, same disclosed limitation Slices 1-3's own Kata tests already carry.

## 7. Verification performed this session

- **Design → independent red-team → fix, before implementation** — an adversarial subagent review
  (resumed once after an interruption) attacked the exit-code-137 design across 8 specific angles
  (heuristic soundness, race conditions with `destroy()`, the `memory_capped` bool's information
  loss, the positive control's decidability, shell/argv injection, the fork-bomb-omission tradeoff,
  other undisclosed gaps found by reading the code, and the compile-verification plan's own failure
  modes) before any code existed. Findings: 1 FATAL (the heuristic itself, dropped), 2 MUST-FIX
  (the `destroy()` race and the `memory_capped` bool, both moot once the heuristic was dropped),
  1 BLOCKING (the guest-process-orphan-on-timeout gap, fixed in §4), 1 NON-ISSUE (confirmed no
  injection hazard — `request.source` reaches `/bin/sh -c` as one `posix_spawn` argv element the
  whole way, never re-tokenized by a host-side shell), plus the fork-bomb-omission judgment call
  (§5) and the implementation-trap note about `Instance`'s positional aggregate init (moot — no new
  `Instance` field was added, since `memory_capped` was never implemented).
- **Compile verification**: this repo's `build-linux` CMake cache (already configured against this
  same checkout via WSL Ubuntu's real g++/cmake/ninja, mounted at `/mnt/d/GitSrc/AgentEngine`) was
  reconfigured with `-DAGENTENGINE_BUILD_KATA_BACKEND=ON -DAGENTENGINE_KATA_SANDBOX_TESTS=ON`
  (explicitly on the command line, not relying on a stale cached value) and the changed/new files
  built against a real Linux toolchain — more verification than Slices 1-3's own Kata files
  received in-session, though still short of execution against a live deployment.
- **Full local Windows `ctest` regression run**: confirms zero regressions in every test unaffected
  by this change (this backend is Linux-only and gated out of the Windows build entirely, so this
  run's purpose is confirming the rest of the tree is untouched, not exercising the new code).
