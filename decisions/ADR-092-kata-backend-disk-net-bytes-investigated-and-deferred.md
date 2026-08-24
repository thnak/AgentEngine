# ADR-092 — KataBackend: `ResourceLimits::disk_bytes`/`net_bytes` investigated, deferred

Status: Proposed (investigation complete, decided in the negative; awaiting project-owner sign-off,
mirroring ADR-090's own "investigated and deferred" precedent for `ResourceLimits::pids`)

## 1. The question

`kata_backend.hpp`'s residual list has carried `ResourceLimits::disk_bytes`/`net_bytes` as
"unenforced and uninvestigated" since Slice 2 (ADR-086), unlike `pids` (ADR-090) and `fds` (ADR-091),
neither of which had been checked against either the project's own spec or containerd's real CLI
surface. This ADR is that investigation, run to a real conclusion for both fields.

## 2. What 008-Sandbox-and-Isolation.md actually assigns these fields to

Unlike `pids`/`fds` — which are Linux kernel resource-control primitives with a natural
process/cgroup-level enforcement point `ctr run`'s convenience flags can reach — `disk_bytes` and
`net_bytes` are explicitly assigned by the spec to a *different* layer entirely.
`src/backends/native_jail/cgroup_limits.hpp`'s own header comment already states this plainly for
the sibling backend: "`disk_bytes`, `net_bytes`, `output_bytes`, `fds` are explicitly NOT enforced
here — 008 §1b assigns those to interpreter-level mediation... or, for `output_bytes`, to the bounded
pipe-drain the backend itself performs." `008-Sandbox-and-Isolation.md` §1b confirms this directly
(quoted, §1b's own capability table): `disk_bytes`/`net_bytes`-shaped containment (`FsRead`/`FsWrite`
usage tracking, `NetOut` byte accounting) is delivered via **interpreter-level mediation of `open`/
`socket`** — i.e. AgentEngine's own `MediatedPythonRunner`/`ShellRunner` intercepting the actual
syscalls a script makes, at the point of use, inside a process AgentEngine itself controls — not a
static, backend-level, create-time resource cap the way `memory_bytes`/`pids`/`fds` are.

## 3. Why that mechanism has no attachment point in `KataBackend`'s execution model

`native-jail`'s own table row in 008 §1b names it explicitly as "OS process jail **+**
interpreter-level mediation (§1b)" — two layers, combined. `KataBackend::exec()` has only the first
half of that pair, and not even that: it runs `ctr tasks exec --exec-id <id> <container> /bin/sh -c
<source>` — a bare shell, executing directly inside the guest VM, with no AgentEngine-owned
interpreter process mediating its `open()`/`socket()` calls at all. The `MediatedPythonRunner`/
`HandleRelay` machinery (ADR-081/ADR-085) that gives native-jail's own `disk_bytes`/`net_bytes`
mediation an attachment point exists entirely on the HOST side, wrapping a jailed CPython process
AgentEngine itself spawns and controls; nothing analogous runs inside a Kata guest — there is no
AgentEngine-owned process inside the VM for a future mediation layer to instrument, only the guest's
own unmodified `/bin/sh`.

Building one would mean constructing an entirely new guest-side subsystem: a mediating agent baked
into the guest image (or injected via a mount) that every `exec()` call would need to route through
instead of a bare shell, replicating `MediatedPythonRunner`'s whole architecture across the VM
boundary — a wholly new component, not a resource-limit wiring change, and squarely out of scope for
an incremental slice.

## 4. A backend-level (not interpreter-level) approximation was also checked, and is blocked the
   same way `pids` was

Before concluding, this investigation also checked whether a coarser, VM/snapshot-level disk quota
(analogous to how `memory_bytes`/`fds` map to real `--memory-limit`/`--rlimit-nofile` flags,
independent of interpreter mediation) might be a reasonable degraded substitute for `disk_bytes`.
It is not currently available either way: `ctr run`'s full convenience-flag surface (checked
exhaustively for ADR-090/091 against `platformRunFlags` and `ContainerFlags`) has no disk-quota flag
at all — `--blockio-config-file`/`--blockio-class` govern I/O bandwidth/priority (cgroup `blkio`),
not a size cap. Any real disk-size limit would need the identical `--config`-based full OCI spec
rewrite ADR-090 already investigated and rejected for `pids` — the same `--config`-exclusivity,
missing-rootfs-preparation, and unresolvable chain-ID-discovery blockers apply identically here, not
a new problem.

## 5. `net_bytes` is additionally moot right now, independent of mediation

Even setting mediation aside, `KataBackend::create()` already fails closed on any `NetPolicy` beyond
`deny_all` (Slice 2/ADR-086, unchanged since) — there is currently no network path inside a Kata
guest to meter in the first place. `net_bytes` enforcement is not an independent gap right now; it
collapses into the separately-named "real `NetPolicy` allowlist mechanism (CNI)" gap
(`kata_backend.hpp`'s own residual list) — closing that gap is the actual prerequisite, not this
field.

## 6. Decision

**`ResourceLimits::disk_bytes`/`net_bytes` stay unenforced for `KataBackend`.** Both are
investigated-and-deferred now, not merely unattempted:

- `disk_bytes`: blocked by two independent, compounding reasons — 008 §1b's own designed mechanism
  (interpreter-level mediation) has no attachment point in this backend's raw-guest-shell execution
  model, and the backend-level fallback (a VM/snapshot quota) is blocked by the identical
  `--config`-rewrite obstacles ADR-090 already found for `pids`.
- `net_bytes`: blocked by the same mediation-attachment-point problem, and additionally moot today
  since this backend currently grants no network access at all regardless.

Real enforcement of either needs one of: a genuinely new guest-side mediation subsystem (§3, large,
out of scope), reopening the `--config`/embedded-client architecture question ADR-090 already
declined to reopen incidentally, or (for `net_bytes` specifically) first closing the separate CNI
gap and then re-evaluating whether byte metering is tractable once network access exists at all.

## 7. What changed in this pass

No source code changed. `kata_backend.hpp`'s residual list is updated to mark both fields
investigated-and-deferred, with a pointer to this ADR, rather than "unenforced and uninvestigated."

## 8. Residuals, carried forward explicitly

- The three other named KataBackend gaps are unchanged by this ADR: the real `NetPolicy` allowlist
  mechanism (CNI) — now explicitly the actual prerequisite for `net_bytes`, not just a parallel gap;
  `ExecRequest::source` Runner-mediation; GPU passthrough (deliberately out of scope).
- This investigation is source/spec-level analysis, the same evidentiary posture ADR-090 used for
  `pids` — no live Kata/containerd deployment was available to empirically confirm any of it, though
  the core finding (008 §1b's own architecture, and `cgroup_limits.hpp`'s own precedent) rests on
  this project's own spec and shipped code, not external/third-party source reading alone.
