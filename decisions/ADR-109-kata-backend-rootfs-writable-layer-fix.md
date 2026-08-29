# ADR-109 — `KataBackend`'s first real deployment run: the rootfs-writable-layer fix that unblocked every default `create()` call

- **Status:** Proposed — implemented, verified against a real live deployment, independently red-teamed
  (2026-08-29). This is the FIRST time any `KataBackend` code (ADR-084 through ADR-101ish, spanning
  Slices 1-11) has ever run against a real Kata Containers deployment — `docs/planning/kata-backend-ci-
  runner-setup.md` itself disclosed this had never happened. A real Kata Containers 4.1.0 (`kata-clh`
  runtime, cloud-hypervisor VMM) deployment was provisioned in this session's own WSL2 Ubuntu dev
  environment (real `/dev/kvm` available, unlike the previously-blocked Proxmox-LXC CI-runner candidate)
  specifically to run this test suite for real for the first time. The red-team round found **one
  BLOCKING and one real-but-high-confidence issue in this same day's own fix**, both fixed before
  landing — see §5. Full real test results, both before and after the red-team fixes: `test_kata_backend
  _linux` 8/8 ALL PASS, `test_kata_backend_slice2_linux` 22/22 ALL PASS, `test_kata_backend_abuse_corpus_
  linux` 25/26 (1 separate, disclosed, not-yet-diagnosed residual — §7), `test_kata_backend_slice9_10_
  linux` 12/14 (2 failures are `cnitool`/CNI plugins never being installed in this environment,
  explicitly out of scope per the provisioning runbook's own §4 scoping — not a code defect). Full
  project `ctest`: zero regressions outside the Kata suite itself.
- **Date:** 2026-08-29.
- **Scope:** `src/backends/kata/kata_backend.cpp`/`.hpp` (the rootfs-mounting and mount-point-
  pre-creation logic inside `create()`/`destroy()`, plus a new unconditional `guest_path` traversal
  guard), `tests/test_kata_backend_slice2_linux.cpp` (2 stale error-code assertions corrected),
  `tests/test_kata_backend_slice9_10_linux.cpp` (1 test-sequencing cleanup bug fixed). No other file
  touched — `build_oci_spec_json()`'s own declared mount/capability/resource shape is unchanged; this
  ADR is about how the rootfs those mounts target gets prepared, not what the OCI spec itself says.
- **Related specs:** `docs/planning/kata-backend-ci-runner-setup.md` (the provisioning runbook this
  session followed to stand up the real deployment; its own text disclosed no session before this one
  had ever run any of this against a real host) · `docs/planning/kata-backend-config-pipeline-pids-disk-
  net-redesign-draft.md` (SLICE 11's own design, whose explicit "disk_bytes == 0 stays byte-for-byte
  unchanged" scope decision this ADR's own red-team round caught an early version of this fix silently
  violating — see §5 finding 1) · `decisions/ADR-093-kata-backend-netpolicy-allowlist-config-cni.md`
  (SLICE 10, the NetPolicy/CNI mechanism this ADR's own §7 confirms is real but untested against a live
  CNI install in this pass) · every prior Kata ADR (084 through the most recent Slice-11 one) whose own
  claims this pass is the first real, live confirmation or refutation of.

## 1. The question

`KataBackend` has a dozen prior ADRs' worth of design, red-teamed reasoning, and unit-level proof —
but, per its own provisioning runbook's explicit disclosure, had never actually been exercised against
a real, live Kata Containers deployment. Does the subsystem's own core claim — "a real Kata sandbox
starts successfully" — actually hold once a real VM, a real guest agent, and a real containerd/`ctr`
CLI are in the loop, or does years of design-and-red-team-without-execution conceal a real, previously
invisible defect?

## 2. What was found (the bug, empirically diagnosed, not guessed)

`KataBackend::create()` builds a complete OCI runtime spec and invokes `ctr run --config <spec.json>
<id>` — deliberately bypassing containerd's own image/snapshotter-based bundle machinery (SLICE 9's own
reasoning, to support `NetPolicy`/pids/resources fields the convenience-flag path has no CLI exposure
for). Before this fix, when `SandboxSpec::limits.disk_bytes` was unset (0 — the default, and every
existing caller's actual behavior), `rootfs_dir` was a **plain, read-only bind mount** of `lower_dir`
(`ctr images mount`'s own target, which mounts image content read-only by design).

**First hypothesis, disproven by direct testing**: that Kata's virtiofs rootfs-sharing mechanism needed
containerd's own snapshot-mount-list (conveyed via the standard `Create()` RPC), which `--config` mode
never supplies. Empirically tested via a real, manually-constructed `ctr run --config` invocation
against a real, populated rootfs directory: this worked past the point the real bug occurred, disproving
the hypothesis.

**The real cause, found by systematic field-by-field bisection** of a hand-reconstructed OCI spec
against the live deployment: Kata's Rust guest agent (`containerd-shim-kata-v2`, `runtime-rs`) does
**not** auto-create a missing mount-point directory before mounting something there — unlike `runc`,
which does. `busybox:latest`'s own image ships **only** `/dev` as a pre-existing top-level directory;
`build_oci_spec_json()`'s own default mount set additionally declares `/proc`, `/sys`, `/run`,
`/dev/pts`, `/dev/shm`, `/dev/mqueue` — every one of which failed guest-side `create_container` with a
bare `ENOENT`, for **every** default (no-disk-quota) `create()` call, with any image that doesn't happen
to ship every one of those directories pre-created. A real, populated rootfs plus a writable overlay
with the missing directories manually pre-created was confirmed, empirically, to resolve it (a real
`sleep infinity` task reached `RUNNING` state and was killable normally) before any code was written.

## 3. The fix

1. `rootfs_dir` is now **always** a real overlay mount with a writable upper layer, never a plain bind
   mount. `disk_bytes > 0` (unchanged in substance): the existing loop-device-backed, fixed-size ext4
   filesystem provides the writable layer — real quota enforcement, unaffected. `disk_bytes == 0`: a
   **size-capped** (`size=4m`) `tmpfs` provides the writable layer instead — see §5 finding 1 for why
   capped, not unsized.
2. `create()` now pre-creates, under the freshly-mounted `rootfs_dir`, every mount-point directory it is
   about to declare in the OCI spec: its own fixed default set (matching `build_oci_spec_json()`'s own
   list exactly) and every caller-supplied `MountSpec::guest_path`. `/etc/hosts` (a file bind-mount
   destination, used only when `NetPolicy` grants real network access) is handled separately — the
   parent `/etc` directory is created, then an empty file is written at `/etc/hosts` only if nothing (or
   the wrong type) is already there.
3. `destroy()`'s teardown correspondingly unifies: it now **always** unmounts the overlay (`rootfs_dir`)
   and the writable layer (`quota_root`, whichever kind was mounted there), and only attempts a
   loop-device detach when `disk_quota_active` (narrowed to mean specifically "the loop-device-backed
   ext4 layer was used", not "any writable layer exists" — every instance has one now).

## 4. Verification

- `test_kata_backend_linux`: 8/8 ALL PASS (was 0 — every case failed at `create()` before this fix).
- `test_kata_backend_slice2_linux`: 22/22 ALL PASS, including real, live proofs this pass is the first
  to ever exercise: a genuine virtiofs bind mount round-trips content host→guest and guest→host; a
  `memory_bytes` cap really resizes the VM (measured guest total memory ≈ requested cap, not the
  ~2 GiB unconfigured default); an `fds` cap's `RLIMIT_NOFILE` genuinely propagates to a *later*
  `ctr tasks exec` call, not just the container's initial process (SLICE 7's own previously-disclosed
  open question, answered for real here); a `wall_ms` cap genuinely bounds a real 10-second guest sleep
  to the configured budget, not `run_ctr()`'s own fixed 30-second default.
- `test_kata_backend_abuse_corpus_linux`: 25/26. The one failure (a CPU-spin containment check) is a
  separate, real, first-time-ever-surfaced finding — disclosed, not chased down in this pass (§7).
- `test_kata_backend_slice9_10_linux`: 12/14 after also fixing one real, pre-existing test-sequencing
  bug (§6) — the 2 remaining failures are both `cnitool`/CNI-plugin-dependent (`ip netns add` +
  `cnitool add` + `nft` rules), and `cnitool`/`/opt/cni/bin` were never installed in this environment,
  matching the provisioning runbook's own explicit "§4, needed only for `NetPolicy::allowlist`-exercising
  tests" scoping — a real, disclosed environment gap, not a code defect this ADR claims to have checked.
- Full project `ctest` (WSL2, root, 214 tests): zero regressions outside the Kata suite itself before OR
  after the red-team fixes.
- Windows: unaffected — every file this ADR touches is Linux-only (`NOT WIN32`-gated), confirmed via a
  clean Windows reconfigure/build showing zero work to do.

## 5. Red-team round

A genuinely independent, fresh-agent adversarial pass (not this session's own self-review) found **two
real issues, both fixed same day**, holding this whole design lineage's own established pattern: every
independent pass on Kata-adjacent code has found something real.

1. **BLOCKING — the first version of this fix silently reopened a resource-budget question the
   project's own design doc had explicitly deferred.** The original fix used an *unsized* `tmpfs` for
   the `disk_bytes == 0` writable layer. Linux's kernel default for an unsized `tmpfs` is 50% of
   physical RAM, and because that layer is shared into the guest via Kata's own virtiofs (a **host-side**
   daemon, entirely outside the guest's own memory cgroup), it was a guest-reachable, zero-capability-
   gated, up-to-50%-of-host-RAM write primitive for **every** default `create()` call — composing across
   concurrently-created instances with no coordination. `docs/planning/kata-backend-config-pipeline-
   pids-disk-net-redesign-draft.md` explicitly scoped `disk_bytes == 0` as "byte-for-byte unchanged" and
   named reopening its writability as separate, owner-decided follow-on work — landing an unbounded
   writable layer silently, inside an unrelated ENOENT bugfix, would have been exactly the "contested...
   design landed without design → red-team → prove → judge" mistake CLAUDE.md's own discipline exists to
   catch, and a real I8 (budgets enforced) regression versus the *actual* prior behavior (a read-only
   bind mount enforced a budget of literally zero writable bytes, not "no cap, same as before"). **Fixed
   same day**: the tmpfs is now capped at `size=4m` — generous for its own stated purpose (a handful of
   empty directories plus one empty file) but nowhere near enough to matter as a host-RAM DoS vector.
   Re-verified: all four Kata suites' own results unchanged after the cap (still 8/8, 22/22, 25/26,
   12/14) — nothing depended on unbounded capacity.
2. **Real, high-confidence — a host-side directory-traversal gap in the new mount-point pre-creation
   loop.** `fs::path::relative_path()` (used to combine `rootfs_dir` with a caller-supplied
   `MountSpec::guest_path` before calling `fs::create_directories()`) strips only the leading
   root-name/root-directory — it does **not** strip embedded `..` components (empirically verified via a
   standalone compiled probe, not assumed). `authorize_spec()` (`sandbox.hpp`) only rejects a `.`/`..`
   component in `guest_path` when the caller holds a `cap::SandboxMount` grant — every existing call
   site today holds none at all (that function's own documented "opt-out preserved" shape) — so nothing
   upstream defended `guest_path` in the common, default case before this fix. A `MountSpec{.guest_path
   = "/../../../../etc/cron.d/x"}` would have made the new pre-creation loop create real directories
   **outside `rootfs_dir`, on the host**, as whatever privilege this process runs under. `guest_path` is
   caller-supplied host-side configuration, never model output (I3 not directly implicated), but this is
   a new, real host-filesystem-write primitive this fix introduced, inconsistent with this exact file's
   own established defense-in-depth pattern for the identical risk class on `host_path`
   (`targets_own_workdir()`, unconditional, not capability-gated). **Fixed same day**: an unconditional
   `has_dot_or_dotdot_component(guest_path)` rejection, mirroring `targets_own_workdir()`'s own posture
   exactly — never gated on a capability grant this backend's own directory-creation side effect does
   not depend on.

**Also found, real-but-minor, fixed anyway (cheap and directly actionable):** the `/etc/hosts`
pre-creation check used a bare `fs::exists()`, which does not distinguish file from directory — if an
image ever shipped `/etc/hosts` as a directory, the check would have wrongly reported "already there"
and `create()` would have succeeded with the wrong destination type in place, surfacing later as a
confusing guest-side `ENOTDIR` instead of a clear host-side pre-flight error. Fixed via `fs::status()` +
`fs::is_regular_file()`/`fs::is_directory()`, which tell the two cases apart for real.

**Checked and found clean by the red-team round:** the `disk_quota_active` meaning-narrowing (every
remaining use across both files re-checked, none stale); `destroy()`'s new unconditional teardown
ordering against `cleanup_partial()`'s own reverse-acquisition-order discipline (no leak on any traced
path); the hardcoded default mount-point set against `build_oci_spec_json()`'s own declared list (exact
match, no drift).

## 6. Byproduct fixes: two stale test expectations, first exposed by this pass's own real execution

- `test_kata_backend_slice2_linux.cpp`: two cases asserted `kata_backend.net_allowlist_unsupported`, a
  Slice-2-era error code SLICE 10 (ADR-093) superseded with a real, capability-gated NetPolicy mechanism
  — the code now returns `kata_backend.net_capability_required` for the exact same scenario (zero
  `cap::SandboxNetOut` grants). `net_allowlist_unsupported` no longer appears anywhere in
  `kata_backend.cpp` (grep-confirmed). Never caught before because this assertion had never actually run
  against a live deployment — `create()` always failed earlier, at daemon-connection, with a different,
  generic error before today.
- `test_kata_backend_slice9_10_linux.cpp`: the disk-quota "positive control" case (a small write meant
  to prove the quota mechanism itself works, not just that a bigger write fails) ran immediately after a
  case that deliberately overflows the same 8 MiB quota via `dd`. `dd` does not clean up its own partial
  output on `ENOSPC` failure, so the failed fill's leftover bytes (potentially most of the 8 MiB budget)
  were still consuming the quota when the "positive control" ran next — racing against stale state, not
  a fresh budget. Fixed by removing the failed fill's output before the positive-control write.

## 7. Decision

Land the rootfs-writable-layer fix, its red-team-found follow-on fixes, and the two byproduct test
corrections. This closes the specific, empirically-diagnosed ENOENT defect that blocked every default
`KataBackend::create()` call from ever working against a real deployment, without reopening the
disk-quota subsystem's own deliberately-deferred writability-budget question.

## 8. Residuals

- **`test_kata_backend_abuse_corpus_linux`'s one remaining failure, NOT diagnosed in this pass:** "the
  guest process's own heartbeat file stops changing after the `wall_ms` timeout fires" — the measured
  heartbeat values were both empty (`v1= v2=`, consistently reproduced, not a flake) rather than
  differing, suggesting the write never reached the host-visible bind mount at all in this specific
  timing window (plausibly `virtio_fs_cache = "auto"`'s own write-back caching semantics interacting
  with a forceful guest-side kill, though not confirmed). This is a SEPARATE concern from the
  rootfs-creation bug this ADR fixes — the same bind-mount mechanism is proven working elsewhere in this
  same session's own test runs (slice2's real read/write bind-mount proofs) — and was not chased down
  here, named accurately rather than silently left implied as understood.
- **CNI/`cnitool` never installed in this environment** — 2 of `test_kata_backend_slice9_10_linux`'s own
  checks stay unverified against a live CNI setup, matching the provisioning runbook's own explicit §4
  scoping ("needed only for `NetPolicy::allowlist`-exercising tests"). Real, tractable follow-on work if
  a future session wants full NetPolicy-allowlist coverage, not attempted here.
- **The pid-reuse-adjacent multi-namespace concern named in ADR-108 is unrelated to this ADR** — noted
  here only to avoid confusion: this fix touches a completely different subsystem (`KataBackend`, VM
  isolation) from ADR-108's `DockerCliBackend`/`ContainerdCliBackend` (container-process isolation).
- **This is the FIRST real deployment run for the whole Kata subsystem** — every prior Kata ADR's own
  claims about behavior this session's tests didn't specifically re-exercise (e.g. `pids` cgroup
  enforcement under real fork-bomb load, per `test_kata_backend_abuse_corpus_linux`'s own "fork-bomb-
  documentation" case, which deliberately documents-not-tests that gap) remain exactly as previously
  disclosed — this ADR narrows "the rootfs the whole subsystem depends on now actually works", it does
  not re-certify every other prior claim in the Kata lineage.
