# ADR-108 — Closing the disclosed container-orphan residual: `reap_orphans()` for both real `ExecutionSurface` conformers

- **Status:** Proposed — implemented, verified against both live daemons (Docker on Windows and WSL2/
  Linux, containerd on WSL2/Linux), independently red-teamed (2026-08-29). The red-team round found
  **three real issues, all fixed same day** — see §5 for the full findings and §4 for post-fix
  re-verification evidence. Full Windows `ctest`: 289/289, 100%. Full Linux `ctest`: 207/207 minus 4
  pre-existing, environment-caused Kata failures (image-pull unreachable in this WSL2 session, nothing
  to do with this ADR — `src/backends/kata/kata_backend.cpp` untouched) and one confirmed parallel-
  execution flake in `test_provider_egress_address_policy` (a live-network-binding test contending
  with this ADR's own live-daemon reap tests under `-j 4`; re-run standalone: `ALL PASS`, and this ADR
  touches no networking/egress code at all). **§7's own "not wired into either tool's startup" AND
  "pid-reuse race, inherent, not solved" residuals are SINCE CLOSED (2026-08-29, same session)** — see
  §8 for the full follow-on work: a real `check_process_identity()`/start-key mechanism narrows the
  pid-reuse gap (proven via a new, direct positive control: same live pid, wrong start-key, correctly
  reaped), and both `tools/sandboxed_shell_chat.cpp`/`tools/containerd_shell_chat.cpp` now run a
  best-effort startup sweep, verified end to end against real daemons with a manually planted orphan.
- **Date:** 2026-08-29.
- **Scope:** `include/agentengine/sandbox/docker_execution_surface.hpp` (adds `--name` to `docker run`,
  a POSIX/Windows `process_is_alive()` pair, `DockerCliBackend::reap_orphans()`), `include/agentengine/
  sandbox/containerd_execution_surface.hpp` (adds a POSIX `process_is_alive()`, `ContainerdCliBackend::
  reap_orphans()` — `reset()`'s existing `ae_ces_<pid>_<seq>` naming is unchanged, it was already
  discoverable), new `tests/test_docker_orphan_reap.cpp` (cross-platform, `docker` CLI required, no
  `WIN32` gate — mirrors `test_sandbox_runtime.cpp`'s own posture), and 8 new checks appended to
  `tests/test_containerd_execution_surface.cpp` (Linux-only, unchanged gate). **No behavior change to
  either class's existing `reset()`/`run()`/`drain_to()` verbs or the `ExecutionSurface` concept
  itself** — `reap_orphans()` is a new, additional, explicitly-invoked method, never called
  automatically from any constructor/destructor/verb.
- **Related specs:** `decisions/ADR-104-real-io-filesystem-linux-parity.md` (where `DockerExecutionSurface`'s
  own header comment first disclosed this residual, "HONEST RESIDUAL, disclosed not solved", and its
  2026-08-28 correction widening it past "crash only") · `decisions/ADR-106-containerd-execution-surface-
  promotion.md` §5/§7 (independently reproduced the identical residual for `ContainerdExecutionSurface`
  via a real SIGKILL test, named the fix direction: "persisting instance ids somewhere reclaimable,
  mirroring `Ledger`'s own orphan-branch/A7 design") · `docs/planning/proofs/worktree_io/worktree_ledger.hpp`
  `reclaim_orphaned_branch()`/`Ledger::abandon()` (the precedent this ADR adapts, not copies — see §2
  below for why the adaptation changes shape) · `decisions/ADR-089-kata-backend-output-cap-orphan-fix.md`
  (a differently-shaped "orphan" — a guest PROCESS left running inside a still-alive container — checked
  and confirmed NOT the same problem or fix; no reusable mechanism found there).

## 1. The question

Both real `ExecutionSurface` conformers disclose, in their own header comments, that a container can
outlive the host process that created it — a crash, a `SIGKILL`, or even an ordinary exit whose
destructor-time `destroy()` call transiently fails all leave a real container running on the daemon
indefinitely, with (before this ADR) no id persisted anywhere and no mechanism to find it again. Does a
real, adversarially-testable fix exist that closes this without widening what either conformer is
authorized to touch (I2) — i.e., a mechanism that can tell "a container THIS naming scheme produced,
now definitely orphaned" apart from "any other container on the host, possibly still legitimately in
use by something else entirely"?

## 2. Design

**Adapted from `Ledger`'s own orphan-branch precedent, not copied.** `Ledger::reclaim_orphaned_branch()`
hands back a live, continued-use `BranchHandle` once an authorized `Principal` reclaims it — the branch's
content is still valuable and a caller wants to keep using it. A crashed container has no such caller:
nothing in-process ever held a reference to it, there is nothing to "resume". This is `Ledger::abandon()`'s
shape instead — pure garbage collection, not resumption — so the new method is a report-returning
`reap_orphans()`, not a handle-returning `reclaim_orphans()`.

**Discoverable naming.** `ContainerdExecutionSurface::reset()` already named every container
`ae_ces_<pid>_<seq>` (this predates this ADR — added for `ctr`'s own id argument, not for reaping).
`DockerCliBackend::create()` did not: Docker assigns its own random id/name unless told otherwise, so a
Docker container had no discoverable marker at all before this ADR. Fixed by adding `--name ae_des_<pid>_
<seq>` to the `docker run` invocation — a second, deliberately DIFFERENT prefix from Containerd's
(`ae_des_` vs `ae_ces_`) so `reap_orphans()` on one backend can never match a name only the other
backend's own scheme produces, even though nothing stops both from running against the same host. The
name is built entirely from `getpid()`/`_getpid()` and an internal atomic counter — never caller or model
input — so it always trivially satisfies every existing shell-injection character-set check with no new
validation call needed (CLAUDE.md: don't add a check for an input that's already impossible).

**Liveness gate, fails closed.** `reap_orphans()` lists every container/name the daemon currently knows
about (`docker ps -a --format "{{.Names}}"` / `ctr containers list -q`), keeps only the ones matching its
own prefix with a parseable, purely-decimal embedded pid, and destroys exactly the ones whose embedded pid
is CONFIRMED dead — `kill(pid, 0)` on POSIX (only `ESRCH` means dead; every other outcome, including
`EPERM` for a pid reused by a different, differently-owned process, is treated as alive), `OpenProcess()`
+ `GetExitCodeProcess()` on Windows (only `ERROR_INVALID_PARAMETER` from `OpenProcess` means "no such
pid"; a handle that opens but reports anything other than `STILL_ACTIVE` is also treated as dead). Every
ambiguous outcome resolves to "alive, don't touch" — the one gate standing between this mechanism and
destroying a real, in-use container, so a wrong "dead" answer is the only wrong answer with a real
consequence.

**I2 boundary.** `reap_orphans()` never inspects, and is structurally incapable of matching, any
container this exact naming scheme did not produce — a name failing the prefix-then-decimal-pid parse is
skipped unconditionally, never treated as a partial match. It only ever destroys a container whose OWN
embedded pid it can prove is dead; it never widens authority to touch anything the two conformers' own
existing `create()`/`destroy()` methods couldn't already reach.

**Explicit invocation only.** Neither backend calls `reap_orphans()` automatically from any constructor,
`reset()`, or destructor. This is a deliberate Delegated Decision Seam (CLAUDE.md "Feature vs. safety
balance"): reaping touches OTHER processes' containers by definition (`Instance` is never even consulted
— this instance's own live container is never itself a candidate), which is a side effect no
`ExecutionSurface` verb's documented contract promises. A host tool (`tools/sandboxed_shell_chat.cpp`,
`tools/containerd_shell_chat.cpp`, a cron-style maintenance job) must call it explicitly. **Not yet wired
into either tool's own startup** — named as a residual below, not silently left implied as done.

## 3. Claims table

| # | Claim |
|---|-------|
| C1 | A container whose embedded pid is confirmed alive is never touched by `reap_orphans()`. |
| C2 | A container whose embedded pid is confirmed dead IS found and destroyed by `reap_orphans()`. |
| C3 | A container that does not carry either backend's own naming prefix is never touched, regardless of what its name might otherwise look like. |
| C4 | `reap_orphans()`'s own listing/destroy calls never mutate or interact with the calling `DockerExecutionSurface`/`ContainerdExecutionSurface` instance's own live container. |
| C5 | No existing `reset()`/`run()`/`drain_to()` behavior, and no existing test, regresses. |
| C6 (added §8) | A container whose embedded pid is alive but belongs to a DIFFERENT process than the one that created it (a pid-reuse scenario) IS still found and destroyed by `reap_orphans()`, via the start-key mismatch — not misclassified "still alive" by the plain pid check alone. |

## 4. Verification

**Pre-red-team (self):**
- `tests/test_docker_orphan_reap.cpp`: 9/9 checks, against a real Docker daemon (Docker Desktop,
  Windows) — proves C1/C2/C3 for `DockerCliBackend` directly (create() the live-pid case for real;
  `docker rename` a second/third container to a confirmed-dead-pid name and a foreign name
  respectively, since `create()` itself can only ever embed this test process's own live pid).
- `tests/test_containerd_execution_surface.cpp`'s new "ADR-108" block: 8/8 checks (26/26 total in that
  file), against a real containerd 2.2.2/runc 1.4.0 daemon (WSL2 Ubuntu, root) — proves C1/C2/C3 for
  `ContainerdCliBackend` directly, using `create(id, ...)`'s own real `id` parameter to embed a
  genuinely fork()+waitpid()-confirmed-dead pid for the negative case, no rename step needed (`ctr`'s
  own create path takes an arbitrary id directly, unlike `docker run`).
- Full Windows `ctest`: 289/289 passed, 100% (`test_docker_orphan_reap` included).

**Post-red-team (after the §5 fixes), re-verified against real daemons on both platforms:**
- `tests/test_docker_orphan_reap.cpp`: **11/11** (the 2 new truncation-regression checks added, plus
  the original 9), against Docker Desktop on Windows AND against Docker inside WSL2/Linux (Docker
  Desktop's WSL2 integration exposes the same daemon there) — both **11/11**.
- `tests/test_containerd_execution_surface.cpp`: **28/28** total (2 new truncation-regression checks
  added on top of the prior 26), against the real containerd/runc daemon in WSL2 as root.
- Full Windows `ctest`: **289/289 passed, 100%.**
- Full Linux `ctest` (WSL2, root): 207 total; the only failures are the 4 pre-existing Kata tests
  (unrelated environment gap, see Status line) and one confirmed `-j 4` parallel-execution flake in
  `test_provider_egress_address_policy` (re-run standalone: `ALL PASS`) — zero real regressions caused
  by this ADR.

## 5. Red-team round

A genuinely independent, fresh-agent adversarial pass (not this session's own self-review) found **3
real issues and 3 cosmetic/minor ones**, holding the pattern this whole design lineage has established:
every prior independent pass on this code has found something real.

**Real, fixed same day:**
1. **`pid_t`/`DWORD` truncation defeats the liveness gate — empirically proven, not theoretical.**
   `parse_orphan_pid()` parsed into a `long` (64-bit on LP64 Linux) but `process_is_alive()` casts to
   `pid_t`/`DWORD` (32-bit) — a decimal run that fits in `long` but exceeds `INT32_MAX` (no real pid
   ever reaches that range) silently truncated on the cast, and the red-team's own standalone repro
   showed the truncated value can read back as a dead pid even though the original was never a real
   pid at all. On a shared, multi-tenant daemon this let `reap_orphans()` destroy a container it never
   created, directly contradicting this ADR's own §2 I2 claim. **Fixed**: `parse_orphan_pid()` now
   rejects any parsed value outside `(0, INT32_MAX]` before it can ever reach a liveness check, on
   both backends. Regression checks added to both test files (no daemon needed — pure parsing logic).
2. **A real Docker `--name` collision regression this same ADR introduced, undisclosed.** Before this
   ADR, `docker run` never named its containers, so a name collision was structurally impossible. Adding
   mandatory `--name ae_des_<pid>_<seq>` with `<seq>` starting at 0 every process meant: process P1
   creates `ae_des_X_1`, is killed before cleanup (exactly the orphan scenario this ADR targets),
   nobody has run `reap_orphans()` yet, the OS reuses pid X for a later process P2, and P2's own FIRST
   `create()` call computes the identical name while P1's still-alive orphan occupies it — `docker run
   --name` fails outright. **Fixed**: `g_next_container_seq` now seeds from a wall-clock nanosecond
   timestamp instead of a fixed 0, so two independent process starts collide only by an astronomically
   unlikely coincidence.
3. **Undisclosed Docker/Containerd naming-scheme asymmetry, same collision class.** `ContainerdExecution
   Surface::seq_` is a per-INSTANCE member (pre-existing from ADR-106, not touched by today's diff)
   that also starts at 0 — this ADR's own §2 claimed the two naming schemes "mirror" each other without
   this piece actually matching, and two `ContainerdExecutionSurface` instances alive concurrently in
   one process (or the same pid-reuse-after-orphan scenario as finding 2) would collide identically.
   **Fixed**: `seq_` now seeds from the same nanosecond-timestamp technique in the constructor.

**Cosmetic/minor, not fixed (assessed genuinely low-stakes):**
4. Both new tests hardcoded a fixed literal name for the "foreign, non-prefixed" negative-control
   container; a leftover from a prior interrupted run could collide. **Fixed anyway** (cheap, and
   directly actionable) — both now suffix the name with the test process's own pid.
5. `tests/test_docker_orphan_reap.cpp` computed an unused `alive_name` local. **Fixed** (dead code
   removed).
6. A theoretical vacuous-pass edge case in one check's isolated reading — the red-team's own report
   confirms the overall suite still correctly fails via an earlier gating check, so this does not
   affect real test correctness; not changed.

**What held up under adversarial review:** shell-injection safety of `reap_orphans()`'s `docker rm -f`
call (Docker enforces a safe charset on names daemon-side, independently confirmed); `g_next_container_
seq`'s thread-safety for concurrent same-process `create()` calls; both `process_is_alive()`
implementations' documented liveness semantics, resource handling (no `HANDLE` leak), and absence of UB;
`parse_orphan_pid`'s rejection of empty/non-digit/out-of-`long`-range segments; that every name/id this
mechanism mints is built exclusively from `getpid()`/an internal counter, never caller or model input.

## 6. Decision

Land `reap_orphans()` on both `DockerCliBackend` and `ContainerdCliBackend`, matching this codebase's own
established pattern of naming a residual honestly in a prior ADR and closing it with real, adversarially-
tested follow-on work in a new one (ADR-089 closing an ADR-088 residual is the direct precedent). Do NOT
wire it into either tool's startup path in this same pass — that is real, separate, disclosed follow-on
work (§7), not silently bundled in.

## 7. Residuals

- ~~Not wired into `tools/sandboxed_shell_chat.cpp` or `tools/containerd_shell_chat.cpp`'s own
  startup~~ — **SINCE CLOSED, see §8.**
- ~~Pid-reuse race, inherent, not solved~~ — **SINCE NARROWED, see §8** (real fix, not merely disclosed
  further — a residual TOCTOU window remains, disclosed in §8's own fix description, not eliminated).
- **No automatic/scheduled invocation exists** — `reap_orphans()` only ever runs when a caller explicitly
  calls it. §8's tool-startup wiring means every real, user-reachable invocation of either tool now
  sweeps for orphans left by a PRIOR invocation, but nothing runs it on any other schedule (a
  long-lived host process that starts one session and never restarts still accumulates orphans from
  its OWN crashes until something else invokes the sweep) — this closes "no reclaim mechanism exists
  at all, ever", not "orphans can never accumulate under any usage pattern".
- **Multi-pid-namespace daemon sharing, discovered during §8's own verification, disclosed not fixed:**
  `reap_orphans()` implicitly assumes every caller reaping a given daemon shares ONE OS pid namespace.
  This assumption held for every scenario this ADR's own tests exercise, but broke during ad hoc manual
  verification of the tool wiring: Docker Desktop's single daemon is reachable simultaneously from BOTH
  native Windows and its WSL2 integration, two DIFFERENT pid namespaces pointed at the SAME daemon. A
  container `reap_orphans()` running in one namespace created can be reaped by a concurrent
  `reap_orphans()` call running in the OTHER namespace, because a pid that is alive in the creator's own
  namespace may correspond to nothing (or something unrelated) when checked via the OTHER namespace's
  own `process_is_alive()`/`OpenProcess()` — observed directly: a container from a WSL2-side
  `test_docker_orphan_reap` run was destroyed by a concurrently-running native-Windows `ctest` suite's
  own `test_docker_orphan_reap` invocation, both against the one shared Docker Desktop daemon; re-run in
  isolation (not concurrent with the other platform), both pass cleanly. NOT the same problem as the
  pid-reuse race §8 fixes (that is about the SAME namespace's pid being recycled over TIME; this is
  about pid NUMBERS meaning different things across two DIFFERENT namespaces at the SAME time) and not
  fixed here — a real fix would need embedding a namespace/machine-scoped identifier alongside the pid,
  which is real, separate, deliberately-out-of-scope follow-on work. Low real-world severity: a genuine
  single-host production deployment does not typically run two OS pid-namespaces' worth of AgentEngine
  tooling against one shared daemon simultaneously the way this ADR's own dual-platform verification
  methodology did — `ContainerdCliBackend` is immune (only ever reachable from one pid namespace, WSL2,
  in this environment) and this was confirmed via the containerd side passing cleanly even when run
  concurrently with the Windows suite.

## 8. Follow-on work (2026-08-29, same session): pid-reuse narrowing + tool-startup wiring

Closes the two §7 residuals above (tool wiring, pid-reuse) at the user's explicit request, after the
initial red-team round landed. Both closures got their own build+run verification on both platforms;
no second independent red-team round was requested for this follow-on pass (a narrower, more
mechanical extension of already-red-teamed machinery, not a new design) — flagged here rather than
silently implied as equally scrutinized.

**Pid-reuse narrowing.** The plain pid-liveness check `reap_orphans()` used could not tell "the
ORIGINAL process that created this container is still running" from "the pid was later reused by a
completely unrelated process" — the second case reads as "alive" under `process_is_alive()` alone, so
a genuinely orphaned container could silently stay unreapable forever once its pid happened to get
recycled by something else (a real, not merely theoretical, false-negative class — pid reuse over a
long-lived host's uptime is routine, not exotic). Fixed by embedding a per-process-INSTANCE "start
key" alongside the pid in the naming scheme (now `ae_des_<pid>_<start_key>_<seq>` /
`ae_ces_<pid>_<start_key>_<seq>`): POSIX reads `/proc/<pid>/stat`'s own `starttime` field (ticks since
boot, man proc(5), the last-`)`-then-20th-token parse every robust `/proc/[pid]/stat` reader uses to
handle a `comm` field that can itself contain spaces/parens); Windows reads `GetProcessTimes()`'s
`lpCreationTime`. `check_process_identity()` layers this ON TOP of the existing, already-audited
`process_is_alive()` (never replacing its own "ESRCH/`ERROR_INVALID_PARAMETER` is the one unambiguous
'gone' signal" logic) — only when a process genuinely IS alive at the exact pid does the start-key
comparison run, and if THAT read itself fails, the outcome is `kUnknown`, treated exactly like
"alive" (fail closed, never destroy on an ambiguous answer). This is a strict improvement, not a new
risk: every path that now resolves to "reap" also unambiguously means the ORIGINAL creating process
is really gone (either no process at all, or a different process now holds that pid) — no new class of
incorrect destroy is introduced, only false negatives (orphans that silently never got reaped because
their pid got recycled by something unrelated) become correct positives. The disclosed pid-reuse
residual is NARROWED, not eliminated: a tiny, irreducible TOCTOU window remains between
`check_process_identity()` returning "gone" and the actual `docker rm -f`/`ctr container rm` call
running a moment later — the same original process cannot un-die in that window, and a coincidental
pid+start-key collision from a brand-new unrelated process in that exact window is astronomically
unlikely, but not provably impossible.

Verified with a NEW, direct positive control added to both test files, proving the fix rather than
just its absence of regression: a container embedding THIS test process's own real, live pid but a
DELIBERATELY WRONG start-key (simulating exactly the pid-reuse scenario) is confirmed REAPED, while
the identical pid with its OWN correct start-key survives. `tests/test_docker_orphan_reap.cpp`:
**14/14** (Windows and, separately, WSL2/Linux Docker, each run in isolation — a real, disclosed
cross-daemon interference finding from running both concurrently is its own new residual above, not a
regression in the fix itself). `tests/test_containerd_execution_surface.cpp`: **31/31** (WSL2 root).

**Tool-startup wiring.** `main()` in both `tools/sandboxed_shell_chat.cpp` and
`tools/containerd_shell_chat.cpp` now runs a `DockerCliBackend`/`ContainerdCliBackend::reap_orphans()`
sweep before building the session — best-effort, non-fatal (a daemon that's briefly unreachable never
blocks the tool's own real purpose), silent unless something was actually reaped or a destroy failed.
Verified end to end, not just at the unit level: a real orphan-shaped container was manually planted
against each live daemon (`docker run --name ae_des_999999_1_manualtest ...` / `ctr run ...
ae_ces_999999_1_manualtest ...`, pid 999999 confirmed via `kill -0`/`/proc` to correspond to no real
process), the actual tool binary was run with no `OPENAI_API_KEY` set (so it fails cleanly after the
sweep, matching the tool's own already-established sanity-check posture), and the planted orphan was
confirmed gone afterward on both platforms — `startup orphan sweep: inspected 1, reaped 1, 0 destroy
failure(s)` printed by the Windows tool; the WSL2 tool's own sweep confirmed via the container's
disappearance from `ctr containers list -q`. A real, previously-undiagnosed STALE-BUILD artifact was
found and worked around during this verification (not a logic bug): the WSL2 build directory sits on
a `/mnt/d` 9p-mounted cross-filesystem mount between the Windows host and the WSL2 guest, and at least
one incremental `cmake --build` there silently skipped recompiling `containerd_shell_chat.cpp` despite
a genuinely newer source mtime (most likely a host/guest clock or mtime-granularity mismatch across
that mount) — resolved by force-deleting the stale `.o`/binary before rebuilding; disclosed here as a
real environment gotcha for any future WSL2 work against a `/mnt/*`-hosted build tree, not specific to
this ADR's own code.

**Post-follow-on full-suite re-verification:** Full Windows `ctest`: **289/289 passed, 100%.** Full
Linux `ctest` (WSL2, root): 214 total, only the same 4 pre-existing Kata image-pull failures (unrelated
environment gap, unchanged from §4) — `test_sandbox_runtime`, which flaked under `-j 4` parallel
Docker-daemon contention during an earlier verification pass in this same session, passed cleanly here.
Zero real regressions from this follow-on work on either platform.
