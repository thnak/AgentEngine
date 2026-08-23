# ADR-084 — Now that `native-jail`'s own 008 §9 gate has reached Judged on both platforms
# (ADR-004, ADR-083), does a first-party `KataBackend` (Design A, narrowed per the microVM
# reversal design draft's §7 C2 finding) work as a real `SandboxBackend`, and can it clear an
# independent red-team pass before being treated as more than a spike?

**Status:** Judged (design → build → independent red-team → fix → re-verify complete for the
Slice 1 scope, 2026-08-23). Real, new code — not a formalization of prior work.

**Relates to:** `008-Sandbox-and-Isolation.md` §1 (the locked "no `microvm` profile" decision this
ADR operates under a reopened, narrowed exception to, not a repeal of), `docs/planning/microvm-
first-party-backend-design-draft.md` (the design → red-team → judge record that declined full
Design A/B on 2026-08-23, then recorded reopen condition (i) as met later the same day — §9 of
that document), `decisions/ADR-004-appcontainer-native-jail-windows-backend.md` and `decisions/
ADR-083-linux-native-jail-pivot-root-containment.md` (the two ADRs whose Judged status this one's
own existence depends on — see §1), `decisions/ADR-080-sandbox-backend-registry.md`
(`register_hardware_isolation_backend()`, the structural `named_only` mechanism this backend is
registered through, never `Strict`-eligible by construction), `decisions/ADR-013-mbedtls-vendoring
-and-egress-proxy-design.md` (the exact-pinned, checksummed vendoring precedent this ADR's own
pinning falls short of in one honestly-disclosed way — see §3).

## 1. The question, and why it could now be asked

`008-Sandbox-and-Isolation.md` §1's locked decision — no `microvm` profile, no first-party
hardware-isolation backend built or maintained first — was relitigated earlier the same day
(2026-08-23) per CLAUDE.md's mandated design → red-team → judge process for contested, locked
decisions. That process's real, 4-lens adversarial red-team found the case for reversal genuinely
credible *in the abstract* — a narrower, steelmanned Design A (one pinned Kata runtime class, not
two; vendored like ADR-013's mbedTLS; permanently `named_only`) answered the security and
maintenance-economics objections (C1-C3) — but declined reversal on one specific, independent,
falsifiable fact: **C5, a sequencing claim** — `native-jail`'s own 008 §9 promotion gate had not
yet reached Judged on both platforms, so building a second, harder isolation technology would be
new scope ahead of already-committed, unfinished scope, exactly the ordering 008 §1's own priority
statement forbids.

That fact stopped being true later the same day. `decisions/ADR-004-...md` closed its own missing
independent red-team pass (§12 there — a fresh reviewer with no prior context found and this same
pass fixed a real BLOCKING ambient-authority handle-inheritance bug) and its LPAC-vs-AppContainer
open question (real execution: LPAC does not close the win.ini/hosts finding), reaching **Judged**.
`decisions/ADR-083-...md` had already closed the Linux-side G2/G3 gap the same day, against a real
WSL2 kernel. `decisions/ADR-082-...md`'s G1-G8 synthesis shows every gate Judged or
correctly-scoped-out on both platforms except G5 (out of scope pending 023's own M8 baseline, not
a `native-jail` gap). Both facts were independently re-verified — not taken on self-report — by
rebuilding and re-running the Windows native-jail suite (6/6 passing) and the new LPAC spike test
directly, before this ADR's own work began. `docs/planning/microvm-first-party-backend-design-
draft.md` §9 records this: **reopen condition (i) reads as met.**

Condition (ii) (a real, sourced demand signal for AgentEngine-owned KataBackend work) was already
treated as answered earlier the same day by explicit project-owner direction — the same standard
of evidence CLAUDE.md already applies to other project-owner-directed decisions elsewhere in this
repository (the HTTP-networking decision, e.g.) — not re-litigated here.

With both conditions met, the project owner directed: **build the narrowed Design A now.** This
ADR is that build's own record.

## 2. Scope — Slice 1, deliberately narrow

Mirrors `decisions/ADR-081-...md`'s own "Slice 1" precedent for the jailed Python worker: prove a
real, working mechanism end-to-end against real infrastructure, name every residual honestly,
rather than attempting full 008 §2 contract parity in one pass. Concretely, Slice 1 is:

- **One pinned Kata Containers release: 4.1.0** (2026-08-21). No formal "LTS" branch exists
  upstream for Kata Containers (checked directly against the project's own GitHub releases API
  during this pass — release cadence is a plain monthly `X.Y.0` sequence, no LTS tag); "pinned
  release with a deliberate upgrade cadence" is this project's own operative interpretation of the
  design draft's "pin one specific Kata LTS release" language, not a claim that Kata itself
  publishes one.
- **One runtime class: `kata-clh` (cloud-hypervisor).** Chosen over `kata-qemu` specifically
  because the design draft's own C2 finding named QEMU's CVE history as the dominant term in the
  "track upstream" surface the maintenance-economics lens flagged; cloud-hypervisor (Rust,
  narrower scope) is the smaller-surface choice consistent with that finding, not merely
  the first option tried.
- **`SandboxSpec::capabilities`/`mounts`/`limits`/`net` are NOT enforced by this backend at all.**
  Named as a REAL GAP in `kata_backend.hpp`'s own header comment, not silently assumed. What
  containment exists today is Kata's own default posture only: no CNI plugin configured means the
  guest VM has no network route out (absence-based, not a policy this backend enforces or could
  relax), and each container gets an ephemeral per-image rootfs with no host directory bind-mounted
  in. This is real containment but it is Kata's, not this backend's own enforced grant — do not
  treat this backend as meeting 008 §2's contract in full.
- **`ExecRequest::source` is a raw shell command line** (`/bin/sh -c <source>`), the same posture
  `LinuxNativeJailBackend`'s own M2 scope already has — not yet Runner-mediated.
- **No GPU passthrough.** The design draft's own C3 finding named this as needing a real
  `capability_kind` this codebase deliberately does not have yet (no consumer, premature
  abstraction) — this Slice does not add one or wire GPU passthrough.
- **No 008 §9 G1-G8 promotion-gate evidence for this backend.** That is real, separate future work.
- Registered — where a host chooses to register it at all; **no call site does yet, by design** —
  only via `register_hardware_isolation_backend()` (ADR-080), so it can never become `Strict`-
  eligible by omission the way a bare `register_backend()` call could.

## 3. What was built and how it was verified

**Infrastructure** (not code — real, once-per-deployment setup, done this pass in a fresh,
dedicated WSL2 Ubuntu-24.04 distro, deliberately separate from the `Ubuntu` distro the native-jail
Linux work already uses, after a real, named conflict risk was raised: a root-cgroup-migration
script that Linux native-jail tests run before each pass would move any process resident directly
in the root cgroup, including a `containerd`/Kata daemon, into a different cgroup mid-test):

- Kata Containers 4.1.0 static release (`kata-static-4.1.0-amd64.tar.zst`, 969,565,384 bytes,
  SHA-256 `3dc6b69c4acb787b967b04b64599a20d02a8beb1a8eaab3084110df9d0b08c96`), downloaded over TLS
  from GitHub's release CDN, extracted to `/opt/kata`. **Honestly disclosed gap relative to
  ADR-013's mbedTLS precedent**: Kata Containers' GitHub release does not publish a separate
  checksum manifest for this artifact (checked directly — no `.sha256sum`/`.asc` sibling asset
  exists for `kata-static-*.tar.zst`, unlike mbedTLS's own published `mbedtls-3.6.7-sha256sum.txt`
  ADR-013 hand-verified against). The SHA-256 above is this project's own recorded pin, verified
  only against TLS-authenticated origin, not cross-checked against an independent upstream
  publication — a real, narrower verification bar than ADR-013's, named rather than glossed over.
- `containerd` 2.2.1 + `runc` 1.3.4 (Ubuntu 24.04 repo packages), configured with a new
  `kata-clh` CRI runtime class (`runtime_type = 'io.containerd.kata-clh.v2'`, pointed at
  `containerd-shim-kata-v2`'s `runtime-rs` build with an explicit `ConfigPath`).
- **A real, load-bearing bug found and fixed during bring-up, not in this backend's own code**:
  the shipped `/opt/kata/share/defaults/kata-containers/runtime-rs/configuration.toml` symlink
  points at `configuration-qemu-runtime-rs.toml` by default — meaning every `ctr run` under
  `io.containerd.kata-clh.v2` silently launched **QEMU**, not cloud-hypervisor, until this was
  caught by directly inspecting the running VMM process (`ps -ef | grep qemu-system` showed QEMU;
  the design's own single-runtime-class choice requires cloud-hypervisor specifically). Setting
  `KATA_CONF_FILE` per-process was tried first and rejected outright by the shim ("only shipped
  Kata configuration files are accepted") — the actual fix was repointing the `runtime-rs/
  configuration.toml` symlink itself to `configuration-clh-runtime-rs.toml`, making cloud-
  hypervisor the real, only default for this deployment. Re-verified directly afterward: `ps -ef`
  during a live container showed `/opt/kata/bin/cloud-hypervisor --api-socket ...`, and the guest
  `uname -r` (`6.18.35`) differs from the WSL2 host kernel (`6.6.87.2-microsoft-standard-WSL2`) —
  a real VM boundary, not runc.

**Code** (`src/backends/kata/kata_backend.{hpp,cpp}`): a `KataBackend` class satisfying
`agentengine::SandboxBackend`, shelling out to the `ctr` CLI (no new C++ dependency vendored — the
one dependency this backend takes is the Kata/containerd deployment itself, already required to
operate it at all) via `posix_spawn` with explicit fd wiring. `create()` runs `ctr run -d --runtime
io.containerd.kata-clh.v2 <image> <id> sleep infinity` (a persistent, otherwise-idle task);
`exec()` runs `ctr tasks exec --exec-id <uuid> <id> /bin/sh -c <source>` against it; `destroy()`
kills and removes the task/container.

**End-to-end evidence** (`tests/test_kata_backend_linux.cpp`, run against the real deployment
above, not mocked): sandbox creation succeeds; the guest kernel is real and distinct from the host
(`uname -r` → `6.18.35`); a state file written by one `exec()` call is read back correctly by a
LATER `exec()` call on the same instance — the real-VM analogue of `test_sandbox_backend_registry
.cpp` item 1's own "closing over a fresh instance per call" regression test, verified here one
level down against the real backend; a nonzero guest exit code is classified
`exec_outcome_class::crash`; `exec()` on an unknown handle fails closed
(`kata_backend.unknown_handle`); `destroy()` is idempotent. All checks pass.

## 4. Independent red-team pass (2026-08-23)

Run by a fresh reviewer with no prior context on this code (spawned specifically for this, the same
"reviewer who did not write it" bar `decisions/ADR-004-...md` §12 named and met earlier the same
day) against the real, shipped `kata_backend.{hpp,cpp}` — not a design sketch. Findings, all
verified by real execution against the live Kata deployment unless noted:

- **Finding 1 — BLOCKING, verified by execution.** `run_ctr()`'s output-draining loop had no size
  cap at all. A single `exec()` with a guest command producing 200MB of stdout landed the entire
  200MB in host process RSS in one call (measured: 3,712 kB → 198,912 kB), with no truncation and
  no error — a host-memory-exhaustion path reachable by an ordinary runaway guest command, not
  requiring any privilege escalation, and distinct from the already-disclosed
  "`SandboxSpec::limits` unenforced" residual (that's about guest-side policy; this is the
  backend's own host-side capture loop, the same class of property
  `LinuxNativeJailBackend::drain_pipe_bounded()`'s own `kDefaultSafetyCapBytes` exists to bound).
- **Finding 2 — BLOCKING, verified by execution.** `run_ctr()`'s `waitpid()` had no deadline at
  all. A guest command that simply runs long (measured directly: `exec("sleep 6")` blocked for
  exactly 6.05 seconds with no independent cap) blocks the calling thread for as long as the
  underlying `ctr` call takes — including inside `destroy()`'s own three sequential calls, meaning
  a wedged shim/containerd call during cleanup, independent of guest behavior, had no bound and
  could hang the "supposed to be reliable" teardown path forever.
- **Finding 3 — MINOR, verified by code reading.** A `pipe2()` partial failure (first pipe opens,
  second fails) leaked the first pipe's two fds on that one return path — every other error path
  in the function correctly closed what it opened; this one didn't.
- **Finding 4 — MINOR/REAL GAP, partially verified by execution.** `destroy()` discarded all three
  `ctr` command results with `(void)` and had zero observability on failure. A deliberately
  introduced `kill -9` of the guest VMM process was tested directly and did NOT reproduce a leak
  (containerd's own shim-exit reconciliation absorbed it cleanly — logged under "what held up," not
  as a confirmed finding) — but the underlying concern (a *different* failure mode, e.g. Finding
  2's hang, leaving zero trace) remained real and unaddressed.
- **Finding 5 — MINOR, verified by code reading, not fixed this pass.** `posix_spawnp` resolves
  `ctr` via `PATH` rather than a fixed/verified path — a wider trust surface than strictly
  necessary, but not a demonstrated exploit (neither `ExecRequest` nor `SandboxSpec` gives a caller
  any influence over `PATH` or the binary name). The red-team's own framing: "a hardening nit, not
  a finding requiring a fix before promotion." Left as a named, deliberately-not-fixed residual.

**What held up** (from the red-team's own report, condensed): no argv/flag-injection path into
`ctr` exists — every `run_ctr()` call uses an explicit argv vector via `posix_spawnp`, never shell-
string concatenation, and `ExecRequest::source` occupies exactly one fixed trailing argv slot with
no way to be reinterpreted as an extra flag. Normal-path fd hygiene (the `pipe2(O_CLOEXEC)` →
`posix_spawn_file_actions_adddup2` → parent-side `close()` sequence) is correct and matches its own
header comment's claims — no equivalent of ADR-004 Finding 6's Windows ambient-handle-inheritance
bug exists here; POSIX's opt-in `CLOEXEC` model structurally prevents that class regardless.
`register_hardware_isolation_backend()`'s `named_only` claim was independently checked against
`sandbox_backend_registry.hpp` directly and confirmed accurate. `instances_`'s lack of
synchronization matches `LinuxNativeJailBackend::instances_`'s own existing, already-named-open
precedent (ADR-080's own Revision 2 finding #6) — not a new gap this file introduces.

## 5. Fixes (this same pass, re-verified by execution, not deferred as residuals)

- **Finding 1, fixed**: `run_ctr()`'s drain logic now caps each stream at `kOutputSafetyCapBytes`
  (16 MiB, matching `LinuxNativeJailBackend::drain_pipe_bounded()`'s own constant), reading via
  `poll()` across both streams interleaved (see the next bullet) rather than sequential per-stream
  `read()`-to-EOF. Re-verified directly: the 200MB repro now caps `stdout_text` at exactly
  16,777,216 bytes and host RSS grows by ~16MB, not ~195MB.
- **Finding 2, fixed**: the same `poll()` loop is bounded by `kProcessTimeoutSeconds` (30s); on
  expiry, the child is `SIGKILL`'d and reaped with a short bounded `WNOHANG` loop rather than an
  unconditional blocking `waitpid()`. Re-verified directly: `exec("sleep 40")` now returns in
  30.05s (not 40+), classified `exec_outcome_class::timeout` (an existing enum value this fix now
  actually uses). Applies uniformly to `exec()` and to each of `destroy()`'s three cleanup calls.
  **A real, honestly-disclosed labeling nuance found during this same re-verification**: the
  Finding-1 repro (200MB stdout, capped correctly) was ALSO classified `timeout` rather than `ok`
  in one run — the guest command's own stdout closed fast once capped, but the paired `ctr`
  process's stderr stream did not reach EOF before the 30s deadline in that run, so the overall
  call was correctly bounded but mis-labeled relative to what "actually happened" (a real, capped
  success, not a genuine hang). Not a safety regression — the call still returned bounded, at the
  correct byte cap — but a real accuracy gap in `exec_outcome_class` selection under this specific
  interaction, left as a named residual for whoever next tunes these constants, not silently
  smoothed over.
- **Finding 3, fixed**: the second `pipe2()`'s failure path now closes the first pipe's two fds
  before returning.
- **Finding 4, fixed within `destroy()`'s own `void` signature constraint**: each of the three
  cleanup steps now checks its own result and logs to stderr on failure (spawn error, timeout, or
  nonzero exit) rather than discarding silently. Full structured observability (an audit hook,
  matching `SandboxBackendResolutionAuditHook`'s own pattern) is named as future work, not built
  here — `destroy()` has no `EffectContext` parameter to report through today.
- **Finding 5, not fixed** — see §4's own framing; recorded here as a deliberate decision, not an
  oversight.

Rebuilt clean (g++-14, `-std=c++23 -Wall -Wextra`, zero warnings) after every fix; the full
`test_kata_backend_linux` suite re-run and still 100% passing after the fixes landed.

## 6. Decision

**Accepted.** `KataBackend` Slice 1 exists, is real (not a mock or a design sketch), was
independently red-teamed by a reviewer with no prior context, and every finding that reviewer
rated BLOCKING is fixed and re-verified by direct execution against the real Kata deployment. It
is reachable only via `register_hardware_isolation_backend()` (`named_only`, structurally never
`Strict`-eligible) — no call site registers it yet, so its existence changes no existing
deployment's behavior.

### 6.1 What this does not claim

Not a `native-jail`-equivalent promotion (008 §9's G1-G8 gates are far broader than this Slice's
scope — no abuse corpus, no capability/mount/limit enforcement, no cross-platform parity beyond
"Linux only," no cold-start measurement). Not GPU passthrough. Not a claim that
`SandboxSpec::capabilities`/`mounts`/`limits`/`net` are honored — they are not, by this backend,
today. Not a claim that Kata Containers publishes an LTS release track (it does not; this
project's own "pinned + deliberate cadence" discipline is what's being applied here, named as such
rather than misattributed to upstream).

### 6.2 Residuals, carried forward explicitly

- Finding 5 (PATH-resolved `ctr`), deliberately not fixed — hardening nit, no demonstrated exploit.
- The Finding-2-fix's own timeout/ok labeling nuance under a capped-output-plus-slow-stderr
  interaction (§5).
- No upstream-published checksum manifest exists for the Kata release tarball to cross-verify
  this project's own recorded SHA-256 against (unlike ADR-013's mbedTLS precedent).
- `destroy()` failure observability is stderr-only, not a structured audit hook.
- `instances_` is unsynchronized — matches existing `LinuxNativeJailBackend` precedent, an
  open architectural decision named by ADR-080 Revision 2 finding #6, not resolved by this ADR.
- 008 §9 G1-G8 promotion-gate evidence for this backend does not exist yet — real, separate future
  work, not claimed here.

### 6.3 What would reopen or extend this

A real consumer needing `SandboxSpec::capabilities`/`mounts`/`limits`/`net` enforcement, GPU
passthrough (which would also need a new `capability_kind`, deliberately not added here), or
promotion-gate evidence — Slice 2+, each its own scoped follow-on, not silently expanded into here.
