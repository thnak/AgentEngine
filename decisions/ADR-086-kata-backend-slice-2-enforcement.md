# ADR-086 — Does `KataBackend` Slice 2 (ADR-084's own named residual: enforcing
# `SandboxSpec::mounts`/`limits`/`net` instead of silently ignoring them) close that gap for
# real, and can it clear an independent red-team pass before being treated as more than a spike?

**Status:** Judged (build → independent red-team → fix → re-verify complete for the Slice 2
scope, 2026-08-24).

**Relates to:** `decisions/ADR-084-kata-backend-slice-1.md` §6.2/§6.3 (the residual and reopen
condition this ADR closes — "a real consumer needing `SandboxSpec::capabilities`/`mounts`/
`limits`/`net` enforcement... Slice 2+, each its own scoped follow-on"), `src/backends/kata/
kata_backend.{hpp,cpp}` (the file this ADR's own work lives in), `src/backends/native_jail/
linux_native_jail_backend.cpp` (the `BlobRef`-rejection precedent this ADR's own mount handling
was compared against, including where the comparison's safety claim did NOT hold — see §3).

## 1. The question and scope

`decisions/ADR-084-...md` §6.2 named, honestly, four things Slice 1 did not do:
`SandboxSpec::capabilities`/`mounts`/`limits`/`net` unenforced, no GPU passthrough, no
promotion-gate evidence, `ExecRequest::source` not yet Runner-mediated. Project-owner direction
(2026-08-24) scoped Slice 2 to three of those: enforce `MountSpec`, `ResourceLimits`, and
`NetPolicy`. `SandboxSpec::capabilities` (a `CapabilitySet`) and GPU passthrough stay deliberately
out — no `capability_kind` mapping exists for anything this backend could grant beyond what
`mounts`/`net` enforcement already covers, same posture ADR-084 already gave GPU passthrough.

## 2. What was built and how it was verified

All three axes implemented in `KataBackend::create()`, each translated to a real, working
mechanism, not a cosmetic flag:

- **`MountSpec`**: each host-path grant becomes a real `ctr run --mount type=bind,src=<host>,
  dst=<guest>,options=rbind:<ro|rw>` flag — a real virtiofs bind mount into the guest VM.
  `MountSpec::source` as a `BlobRef` fails closed (`kata_backend.blob_mount_unsupported`), the
  same check `LinuxNativeJailBackend::create()` already has for the same case. `MountSpec::
  quota_bytes` is NOT enforced — no quota-aware mount mechanism wired this pass, named as a REAL
  GAP, not silently assumed done. Real end-to-end evidence (`tests/test_kata_backend_slice2_linux
  .cpp`, run against a live Kata deployment, not mocked): a host file's content is readable inside
  the guest through a granted mount; a read-only grant rejects a guest write attempt; a
  read-write grant's guest-side write is visible back on the host filesystem afterward — a real
  live bind, not an ephemeral VM-local copy.
- **`ResourceLimits`**: `memory_bytes` maps to `ctr run --memory-limit`, verified to be REAL VM
  memory sizing, not cosmetic — a 256 MiB cap produced a guest showing ~257 MiB total memory via
  `free -m`, well under the ~2 GiB unconfigured default. `wall_ms`, if nonzero, replaces
  `run_ctr()`'s own fixed 30s default for every `exec()`/`destroy()` call an already-created
  instance makes (verified: a 3000ms cap cut a guest `sleep 10` off at ~3.02s, correctly
  classified `exec_outcome_class::timeout`) — deliberately does NOT bound `create()`'s own VM-boot
  call (§4 finding #3). `output_bytes`, if nonzero, replaces Slice 1's fixed 16 MiB per-stream
  safety cap, mirroring `LinuxNativeJailBackend::drain_pipe_bounded()`'s own "the safety cap, or
  the caller's tighter one" precedent. `cpu_ms`/`pids`/`fds`/`disk_bytes`/`net_bytes` are NOT
  enforced — each named as a REAL GAP in `kata_backend.hpp`'s own header comment with a specific
  reason (`cpu_ms`: a CFS quota is a rate, not a total-time budget, and reinterpreting one as the
  other would misrepresent what's enforced; `pids`: no direct `ctr run` CLI flag exists this pass;
  `fds`/`disk_bytes`/`net_bytes`: no mechanism at all wired).
- **`NetPolicy`**: `deny_all == true` (the only value Slice 1 silently accepted regardless) is now
  an explicitly verified property. A caller requesting `deny_all == false` or a nonempty
  `allowlist` FAILS CLOSED at `create()` (`kata_backend.net_allowlist_unsupported`) — this backend
  has no CNI/egress-proxy wired to honor a real allowlist yet, and silently granting no network
  while a caller believed they requested some would be a correctness footgun even though it
  happens to be safe. Stricter than `LinuxNativeJailBackend`'s own current posture, which silently
  ignores `NetPolicy` entirely — a pre-existing gap this ADR does not fix there, named not fixed.

## 3. Independent red-team pass (2026-08-24)

Run by a fresh reviewer with no prior context on this code (the same "reviewer who did not write
it" bar `decisions/ADR-004-...md` §12 and `ADR-084-...md` §4 both met) against the real, shipped
Slice 2 changes. Findings, verified by real execution against the live Kata deployment unless
noted:

- **Finding 1 — BLOCKING, verified by execution.** `create()` built the `--mount` value by direct
  string concatenation (`"type=bind,src=" + host_path + ",dst=" + guest_path + ",options=..."`)
  — a SINGLE comma-delimited `key=value` string `ctr`'s own parser reads with last-wins semantics
  for repeated keys. A `guest_path` (or `source`) containing an embedded `,src=...,dst=...`
  segment silently overrode the caller's own intended `src`/`dst` values entirely. Reproduced
  directly: `MountSpec{source="/root/sanctioned_src", guest_path=
  "/mnt/intended,src=/etc,dst=/mnt/hijacked", read_write=false}` bind-mounted the HOST's real
  `/etc` into the guest — `cat /mnt/hijacked/passwd` inside the guest returned real host
  `/etc/passwd` content, while `/mnt/intended`, the destination the caller actually authorized,
  never existed. A full I2 violation: ambient authority over an arbitrary host path an attacker
  names, through a spec field this backend claims to enforce. The `options=rbind:<ro|rw>` segment
  itself is NOT similarly injectable — it is always appended last, and `ctr`'s last-wins parsing
  means an embedded `options=` earlier in the string cannot escalate `ro` to `rw`; only the
  source-path hijack was real. Also named: `LinuxNativeJailBackend::create()`'s identical `BlobRef`
  check has no equivalent injection surface at all, because it passes host/guest paths straight
  into a `::mount()` syscall (two separate arguments, no delimited-string grammar) — this ADR's
  own "mirrors that posture" language in the header comment was true only for the `BlobRef` check,
  not for injection safety, and has been corrected to say so explicitly.
- **Finding 2 — REAL GAP, verified by execution, reproduced twice.** A `create()` call that gets
  far enough for containerd to register a container object, but then fails (`spec.limits.
  memory_bytes = 1` reliably triggers this — the Kata agent inside the guest cannot actually
  create the container and `ctr run` exits nonzero) leaked that containerd container object with
  no handle ever reaching the caller and no `Instance` ever tracked — unreachable by any code path
  in this backend, including a later `destroy()` call, since no handle for it ever existed.
  Confirmed via `ctr containers ls` showing the leaked object after the failed call, requiring a
  manual `ctr container rm` to clear; repeated to confirm a second independent leak. Different
  from Slice 1's already-fixed `destroy()` observability gap (ADR-084 Finding 4, which concerned
  an already-tracked instance) — this leaks on the `create()`-failure path itself, before any
  instance exists to observe.
- **Finding 3 — MINOR/judgment call, not a bug, code-reading only.** `wall_ms` does not bound
  `create()`'s own VM-boot `run_ctr()` call — always the fixed 30s default regardless of a
  caller's `spec.limits.wall_ms`. The red-team judged this a real, worth-naming gap in how clearly
  the original header comment scoped the claim (it described `wall_ms` as "a real, host-side
  enforced deadline" without saying it excludes boot), not a defect requiring a code change —
  applying a possibly-sub-second `wall_ms` to boot time itself (cold start is "milliseconds," not
  zero) would make `create()` fail spuriously for realistic small values. Recommended fix: state
  the scope boundary explicitly in the header comment rather than leaving it for a reader to
  infer.

**What held up** (from the red-team's own report, condensed): the `NetPolicy` fail-closed
condition (`!deny_all || !allowlist.empty()`) is correct, including the edge case of a
contradictory `deny_all=true` + nonempty-`allowlist` spec (still fails closed, no silent
allowlist-drop). `MountSpec::quota_bytes` and `SandboxSpec::capabilities` are both fully
unreferenced in the implementation — no half-wired check creating a false impression of
enforcement, matching the header's own disclosure exactly. The `memory_bytes = 1` edge case fails
closed cleanly and fast (883ms), not a hang or crash — only the leak (Finding 2), not the
failure-handling shape, was the problem. Argv construction itself (separate `--mount`/value
elements, `posix_spawnp`, no shell) has no shell-metacharacter injection surface at all — the
vulnerability was entirely in `ctr`'s own comma-delimited value grammar, not argv handling.

## 4. Fixes (this same pass, re-verified by execution)

- **Finding 1, fixed**: any host path or `guest_path` containing a comma is now rejected outright
  at `create()` (`kata_backend.mount_path_invalid`) before building the `--mount` value — `ctr`'s
  mount grammar has no escaping mechanism, so refusing the one delimiter it splits on removes the
  injection surface entirely rather than attempting to escape it. Re-verified directly: the exact
  reproduction payload above now returns `kata_backend.mount_path_invalid` instead of succeeding.
- **Finding 2, fixed**: on a nonzero-exit/failed `create()`, a best-effort `ctr task kill`/`ctr
  task rm`/`ctr container rm` sequence now runs before the error is returned, mirroring
  `destroy()`'s own "at least try, log if it fails, never block on it" posture. Re-verified
  directly: `ctr containers ls`'s line count is unchanged (still just the header row) before and
  after a `memory_bytes=1` failed `create()` call, where it previously left a real, visible leaked
  entry.
- **Finding 3, addressed via documentation**: `kata_backend.hpp`'s header comment and the new
  in-code comment at `create()`'s own `run_ctr(args)` call both now state explicitly that
  `wall_ms` bounds `exec()`/`destroy()` on an already-created instance only, not `create()`'s own
  boot — not left for a reader to infer.

Rebuilt clean (g++-14, `-std=c++23 -Wall -Wextra`, zero warnings) after every fix; both
`test_kata_backend_linux` (Slice 1) and `test_kata_backend_slice2_linux` (Slice 2) re-run and
still 100% passing — no regression from the fixes.

## 5. Decision

**Accepted.** `KataBackend` Slice 2 enforces `MountSpec`/`ResourceLimits`/`NetPolicy` for real,
independently red-teamed, with the one BLOCKING finding (a real I2-violating mount-injection path)
fixed and re-verified by direct execution, not taken on self-report.

### 5.1 Residuals, carried forward explicitly

- `SandboxSpec::capabilities` remains entirely unenforced — no capability-to-mount/network mapping
  exists; Slice 3+ work, not started here.
- `MountSpec::quota_bytes`, `ResourceLimits::{cpu_ms,pids,fds,disk_bytes,net_bytes}` remain
  unenforced, each with its own named reason in `kata_backend.hpp`.
- `wall_ms` does not bound `create()`'s own VM-boot time — a deliberate, now-documented scope
  boundary, not a gap awaiting a fix.
- GPU passthrough, `ExecRequest::source` Runner-mediation, and 008 §9 G1-G8 promotion-gate
  evidence remain out of scope, unchanged from ADR-084.
- `LinuxNativeJailBackend`'s own `NetPolicy` handling (silently ignored entirely) remains
  unfixed — a pre-existing gap in a different file, named here for the record, not addressed.

### 5.2 What would reopen or extend this

A real consumer needing `SandboxSpec::capabilities` enforcement (Slice 3), a quota-aware mount
mechanism, a real CNI/egress-proxy allowlist implementation for `NetPolicy`, or GPU passthrough
(needing a new `capability_kind`, deliberately not added) — each its own scoped follow-on.
