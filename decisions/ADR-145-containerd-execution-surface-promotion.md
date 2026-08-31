# ADR-145 — Does `ContainerdExecutionSurface` (design-only per `docs/planning/oci-execution-surface-design-draft.md`, proven standalone in the prove-phase tree) promote cleanly into real production code, as the second real `ExecutionSurface` conformer alongside `DockerExecutionSurface`?

- **Status:** Proposed — implemented, verified, and independently red-teamed (2026-08-29), real builds
  and real test runs against a live containerd/runc deployment. The red-team round found two real,
  undisclosed non-mechanical deviations from the prove-phase original (both correctness improvements,
  now accurately disclosed rather than left implied as "unchanged"), one real fix applied same day
  (`host_dir_str`'s embedded-NUL guard), and one real, structurally-unavoidable residual newly
  disclosed (container orphaning on abrupt process death, mirroring `DockerExecutionSurface`'s own
  known residual) — see §5/§7. 18/18 checks pass against a live daemon; full Linux test suite shows
  zero regressions caused by this ADR. **§7's own "not wired into any real tool surface" residual is
  SINCE CLOSED (2026-08-29, same session)**: `tools/containerd_shell_chat.cpp` + `tests/test_composed_
  containerd_providers_live.cpp`, both real, both independently red-teamed and empirically verified
  against a live daemon (`ALL CHECKS PASSED`) — see §7's own updated entry for the full evidence.
- **Date:** 2026-08-29.
- **Scope:** new `include/agentengine/sandbox/containerd_execution_surface.hpp` (header-only, matching
  `docker_execution_surface.hpp`'s own shape — no new CMake library target), new
  `tests/test_containerd_execution_surface.cpp` (Linux-only, `NOT WIN32`-gated in
  `tests/CMakeLists.txt`, no new opt-in flag). **No existing production file changed** — this is a pure
  addition; `DockerExecutionSurface`, `MandatorySandboxProvider<Surface>`, `SandboxRuntime`, and every
  other already-shipped file are untouched.
- **Related specs:** `docs/planning/oci-execution-surface-design-draft.md` (the full design/red-team
  history this ADR promotes: Design C accepted over Podman/raw-`runc` alternatives §2, the bind-mount
  ordering hazard vs. `SandboxRuntime::run()`'s materialize-before-reset sequence empirically PROVEN
  benign §4 finding 1, the C3 "reuse `docker_backend.hpp`'s `reject_chars()` verbatim" claim DISPROVEN
  and replaced by the real POSIX-correct `reject_embedded_nul()` analog §2/§4 finding 2/§5) ·
  `docs/planning/proofs/execution_surface/{containerd_ctr_backend.hpp,containerd_execution_surface.hpp,
  probe_containerd_execution_surface.cpp}` (the prove-phase originals this ADR ports, 16/16 checks
  already passing there) · `decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md` Phase
  3 (the ADR that originally promoted `DockerExecutionSurface` — same naming-port convention reused
  here) · `src/backends/kata/kata_backend.cpp` (the real, shipped `run_ctr()`-shaped argv-vector
  `posix_spawn` pattern and three-step `ctr task kill`/`ctr task rm`/`ctr container rm` teardown
  sequence, reused verbatim, not reinvented) · `decisions/ADR-101-sandbox-backend-tree-refinement-
  reconciliation.md` (already refers to `ContainerdExecutionSurface` as one of `ExecutionSurface`'s
  "two real conformers" in forward-looking language — this ADR is what makes that language accurate).

## 1. The question

The design/prove-phase work already answered the hard architectural questions (bind mount vs. copy,
`ctr run` convenience-flag path vs. `--config` mode, the ordering hazard, the injection-defense shape)
and already has a real, standalone, 16/16-checks-passing C++ conformer
(`docs/planning/proofs/execution_surface/containerd_execution_surface.hpp`). What remains is
mechanical, not architectural: does that prove-phase code port into `include/agentengine/sandbox/`
cleanly, using the real production `agentengine::result`/`agentengine::error`/`SurfaceRunOutcome`
types instead of the prove-phase's own standalone copies, without silently changing behavior in the
process — the same question `DockerExecutionSurface`'s own promotion (ADR-102 Phase 3) already
answered once for the sibling conformer?

**Disproof, if "cleanly":** the ported code fails to compile against the real `ExecutionSurface`
concept; a real run against live containerd behaves differently than the prove-phase probe already
proved (fewer checks pass, or a check that passed there now fails here); or the port silently
introduces a new security/correctness gap the prove-phase's own red-team round didn't already find and
close.

## 2. Design

**Mechanical port, not a redesign.** Followed `docker_execution_surface.hpp`'s own file-top comment as
the template for exactly what kind of naming/type substitution a promotion pass is expected to make
(explicitly enumerated in this ADR's own new file's top comment, not left implicit):
- `probe::ContainerdBackend`/`probe::ContainerdExecutionSurface` → `agentengine::ContainerdCliBackend`/
  `agentengine::ContainerdExecutionSurface` — `ContainerdCliBackend`, not bare `ContainerdBackend`,
  mirroring `DockerCliBackend`'s own naming: this type does not conform to (and is not meant to imply
  conformance to) the real, production `agentengine::SandboxBackend` concept — `KataBackend` is the
  real `SandboxBackend` conformer on this same `ctr` CLI lineage; this class is a much narrower,
  `ExecutionSurface`-shaped wrapper.
- `probe::result<T>`/`probe::error{message, code}` → `agentengine::result<T>`/
  `agentengine::error{failure_class, message, code}` — `policy` for argv-hygiene rejections
  (`reject_unsafe_token()`/`reject_embedded_nul()`), `fatal`/`contract` matching
  `docker_execution_surface.hpp`'s own established convention for the analogous cases.
- `probe::ExecOutcome` → `agentengine::SurfaceRunOutcome` (`execution_surface.hpp`), identical rename
  `DockerExecutionSurface`'s own port already made for the identical reason.

**Most of the rest is unchanged from the prove-phase code**: `run_argv()`'s `posix_spawnp()`+
interleaved `poll()`-drain implementation, the `type=bind,src=...,dst=/workspace,options=rbind:rw`
mount-flag grammar, the `ctr tasks exec --exec-id`/`ctr task kill`+`ctr task rm`+`ctr container rm`
command shapes, and the `drain_to(same host_dir)` no-op / `drain_to(different host_dir)` real-copy
split.

**Two real, undisclosed-until-red-team deviations beyond the claimed three substitutions** (found by
this ADR's own independent red-team round, §5 — recorded here accurately rather than left overclaiming
"identical port"): (1) the prove-phase original's move constructor/move-assignment actually OMITTED
the `ctr_`/`ContainerdBackend` member entirely from the move (a moved-to instance silently got a
default-constructed backend, losing `exec_seq_`'s running count) — this promotion pass's own port adds
`ctr_(std::move(other.ctr_))`/`std::swap(ctr_, other.ctr_)`, correctly matching
`DockerExecutionSurface`'s own established convention (which DOES move/swap its own `docker_` member).
A real, low-severity fix (`exec_seq_` only affects `--exec-id` uniqueness, never running-command
correctness), not a regression, but a genuine behavior change beyond the claimed "mechanical port,
nothing else" scope — corrected here rather than left misdescribed a second time. (2) The `not_reset`
contract-error code string was changed from the prove-phase's generic `"execution_surface.not_reset"`
to a per-class `"containerd_execution_surface.not_reset"`, matching `DockerExecutionSurface`'s own
`"docker_execution_surface.not_reset"` convention — a sensible improvement, also undisclosed until now.

**One real addition beyond the prove-phase probe**: the ported test
(`tests/test_containerd_execution_surface.cpp`) adds a check the original probe never exercised —
`drain_to()` to a DIFFERENT directory than the one bind-mounted (the general `ExecutionSurface`
contract, previously only implemented, never tested — the prove-phase's own §5 named this exact gap:
"`drain_to()` to a directory OTHER than the one bind-mounted falls back to an implemented-but-
unexercised plain host copy"). Closed here: proven, not just implemented.

**CMake wiring**: header-only, no new library target (matching `docker_execution_surface.hpp`'s own
shape — POSIX headers only, no extra system library to link). The test target is `NOT WIN32`-gated
with no additional opt-in flag, deliberately matching `DockerExecutionSurface`'s own tests' posture
(`test_sandbox_runtime`/`test_composed_sandbox_providers_live`: "just fails cleanly if the daemon isn't
reachable," disclosed in ADR-104/105, not hidden behind a special flag) rather than
`test_kata_backend_linux`'s heavier `AGENTENGINE_KATA_SANDBOX_TESTS` opt-in gate — `ctr`/containerd is
a comparably light dependency to `docker`, not the materially heavier KVM/Kata-Containers deployment
that gate exists for.

## 3. Falsifiable claims

| # | Claim |
|---|-------|
| C27 | `containerd_execution_surface.hpp` satisfies the real, unmodified `ExecutionSurface` concept (`static_assert`-checked, matching `docker_execution_surface.hpp`'s own precedent). |
| C28 | The ported production code, run against a real containerd/runc daemon, passes every check the prove-phase probe already proved (bind-mount liveness, zero-`drain_to()`-needed write round-trip, `drain_to(same dir)` no-op, non-zero exit code passed through as a normal result, `reject_embedded_nul()` real rejection, the ordering-hazard re-confirmation) — the promotion changed no observable behavior. |
| C29 | The general `ExecutionSurface` contract the prove-phase left implemented-but-unexercised (`drain_to()` to a DIFFERENT directory) is now proven, not merely implemented. |
| C30 | This ADR's own new files change nothing about any already-shipped production file — `DockerExecutionSurface`, `MandatorySandboxProvider<Surface>`, `SandboxRuntime`, and every other existing conformer/consumer are byte-identical before and after. |

## 4. Executed evidence

- **C27:** `static_assert(ExecutionSurface<ContainerdExecutionSurface>, ...)` at the bottom of
  `containerd_execution_surface.hpp` — compiles cleanly (`make -j12 test_containerd_execution_surface`,
  zero errors).
- **C28/C29:** `test_containerd_execution_surface`, run as root (containerd's default socket
  permissions require it on this install — the same precondition the prove-phase's own probe already
  disclosed) against a real containerd 2.2.2 + runc 1.4.0 deployment (WSL2 Ubuntu, the same environment
  the design/prove phase provisioned): **18/18 checks passed** (16 from the original probe, plus 2 new
  — the `drain_to(different dir)` pair closing C29). Full output:
  ```
  === ContainerdExecutionSurface production test (ADR-145) ===
  [ok]   reset() #1 (fresh container, bind-mounted at host_dir, image auto-pulled if needed)
  [ok]   run(): container reads turn-1 content
  [ok]   run(): container's view of a.txt matches exactly what host_dir held
  [ok]   run(): container writes b.txt
  [ok]   host disk sees the container's write with ZERO drain_to() call
  [ok]   drain_to(same host_dir) succeeds as a no-op
  [ok]   drain_to(same host_dir) does not disturb existing content
  [ok]   drain_to(a DIFFERENT host_dir) succeeds via host-side copy
  [ok]   drain_to(a DIFFERENT host_dir) actually copies the current content there
  [ok]   run(): a non-zero exit code is a normal value, not a result<> error
  [ok]   run(): embedded-NUL command is rejected outright, not silently truncated
  [ok]   the rejected command never reached the container -- workspace untouched
  --- Turn 2: ordering-hazard re-confirmation against the production header ---
  [ok]   host-side remove_all(host_dir) succeeds while turn-1 container is still running
  [ok]   host-side create_directories(host_dir) succeeds immediately after
  [ok]   reset() #2 (destroy old, still-alive container; create fresh one at the recreated path)
  [ok]   fresh turn-2 container starts and reads turn-2 content
  [ok]   fresh container sees NO turn-1 files (a.txt/b.txt) -- no leakage
  [ok]   fresh container sees ONLY turn-2 content
  === 18 checks, 0 failed ===
  ```
  This is the THIRD independent confirmation of the bind-mount ordering hazard being benign for this
  environment (first: `probe_bind_mount_ordering_hazard.sh`'s raw bash/`ctr`-CLI proof; second: the
  prove-phase C++ probe; third: this production header, run through the real
  `include/agentengine/sandbox/` code path for the first time). Also confirmed the disclosed-but-
  untested clean-failure behavior as a non-root user: `ctr run failed ... connect: permission denied`,
  a clear diagnostic, exit code 1, no hang, no partial state.
- **C30:** Full Linux rebuild (`make -j12 -k`) — exactly ONE target failed to build:
  `test_session_builder`, with the ALREADY-DISCLOSED, unrelated `hmac_sha256` undefined-reference
  error (`decisions/ADR-105-sandbox-tool-provider-composed-linux-parity.md` §7) — nothing this ADR
  touched. Full `ctest -E '^test_session_builder$'`: 202/208 passed (208 = 207 pre-existing + this
  ADR's own new test). The 6 failures: the same 5 pre-existing, already-disclosed ones from ADR-105
  (`test_provider_egress_address_policy`, 4 `test_kata_backend_*_linux`) plus exactly ONE new —
  `test_containerd_execution_surface` itself, run as the NORMAL ctest user (not root): fails cleanly
  with the exact disclosed `connect: permission denied` diagnostic this ADR's own §4 already recorded
  from a deliberate manual non-root run, confirming the test's own "requires root or an ACL, fails
  clean otherwise" contract holds under the real `ctest` harness too, not just a manual invocation. No
  file this ADR did not touch behaves any differently before and after — confirmed by the failure list
  being exactly "the same 5, plus this ADR's own new, disclosed-environment-dependent test."

## 5. Red-team round

An independent, fresh `general-purpose` subagent (zero context from the session that made this port)
re-attacked it against the real repo and a real containerd/runc deployment, not by re-reading this
ADR's own prose:

- **Line-by-line diff review**: confirmed the `failure_class` choices (`policy` for every argv-hygiene
  rejection, `fatal` for every `ctr`-invocation failure, `contract` for both `not_reset` guards) all
  correctly match `docker_execution_surface.hpp`'s own established convention for the analogous cases
  — no misclassification found. Found the two real, undisclosed deviations recorded in §2 above (the
  move-ctor/assign fix, the `not_reset` error-code rename) — both real, both improvements, both now
  disclosed accurately rather than left implied as "identical to the prove-phase original."
- **Real-daemon re-verification**: rebuilt independently, reproduced 18/18 as root, reproduced the
  clean `permission denied` failure as a normal user. Additional edge cases tried and found SAFE: two
  consecutive `reset()` calls with no `run()` between them; `run()`/`drain_to()` called before any
  `reset()` (the `not_reset` contract check fires correctly both times, no bypass found). Confirmed the
  header-only, no-new-library-target CMake choice has no ODR problem: compiled two independent
  translation units both including the header, each constructing a `ContainerdExecutionSurface`, linked
  together cleanly.
- **A REAL, CONFIRMED finding**: SIGKILL'ing the host process between a successful `reset()` and the
  destructor ever running leaves a genuinely orphaned, still-running containerd container behind (`ctr
  c ls`/`ctr t ls` showed it, no reclaim mechanism) — the exact same residual class
  `DockerExecutionSurface`'s own header comment already discloses for itself, empirically reproduced
  here for the first time on this conformer, and confirmed to be **structurally unavoidable**: `ctr
  run --help` documents that `--rm` cannot be combined with `--detach`/`-d`, and this conformer must
  use `-d` (a long-lived backgrounded container to `exec` into later) — there is no CLI-level safety
  net available the way `DockerExecutionSurface`'s own (also-leaky) `--rm` at least attempts. This was
  missing from this ADR's own §7 at write time — added below.
- **A real, minor finding, since fixed**: `host_dir_str` (embedded in the `--mount` argv token) had no
  embedded-NUL guard, unlike `id`/`image`/`command` — an inconsistency with this file's own stated
  "reject outright, never truncate" principle (low severity: `create_directories()`'s own OS-level
  truncation at the same byte would already apply independently, so there was no real
  validated-vs-executed divergence the way there is for `command` — but the inconsistency itself was
  real). **Fixed same day**: `reject_embedded_nul()` now also guards `host_dir_str` in `create()`,
  re-verified 18/18 still passing after the fix.
- **Security review (I2/I3)**: tried to construct a real bypass of `reject_unsafe_token()`/
  `reject_embedded_nul()` and could not. Tried `host_dir` containing a literal `=` and a literal `:` —
  both mount cleanly, no grammar confusion found beyond the already-disclosed comma case (which
  reproduces exactly as documented: a clean `ctr` rejection, no silent mis-mount). The design doc's
  core argument (`posix_spawnp`+argv-vector has no host-shell injection surface for the outer `ctr`
  call) held up under direct adversarial testing — no counter-example found.
- **§7 completeness check**: every other existing residual bullet confirmed accurate; the two gaps
  above (SIGKILL orphan, host_dir NUL) were the only real omissions found.

## 6. Decision

Promote `ContainerdExecutionSurface` into production, with the one real fix found by the red-team round
applied (`host_dir_str`'s embedded-NUL guard) and the port's own real, non-mechanical deviations from
the prove-phase original (the move-semantics fix, the error-code rename) disclosed accurately rather
than left implied as "nothing else changed." The port closed one previously-unexercised gap (C29) and
is proven against a live daemon through the real production code path for the first time.
`MandatorySandboxProvider<ContainerdExecutionSurface>` is now usable anywhere
`MandatorySandboxProvider<DockerExecutionSurface>` already is (`Surface` is a plain template parameter
over the `ExecutionSurface` concept, `include/agentengine/sandbox/mandatory_sandbox_provider.hpp:148`
— no wiring change needed there).

## 7. Residual risks

- ~~**Not wired into `tools/cli_chat.cpp`/`tools/sandboxed_shell_chat.cpp` or any real session
  builder**~~ **Closed (2026-08-29, same session)**: `tools/containerd_shell_chat.cpp`, a near-verbatim
  port of `tools/sandboxed_shell_chat.cpp` swapping `MandatorySandboxProvider<DockerExecutionSurface>`
  for `MandatorySandboxProvider<ContainerdExecutionSurface>` (the one template argument — everything
  else, down to the composed-provider structure and the `ComposedQuickstartSessionBuilder` wiring, is
  unchanged), new `agentengine_containerd_shell_chat` CMake target (`AGENTENGINE_WITH_HTTPS AND NOT
  WIN32`). Also closed the deeper half of this residual — proof that the COMPOSITION actually works
  through the real pipeline, not just that a tool file compiles: `tests/test_composed_containerd_
  providers_live.cpp`, a near-verbatim port of `tests/test_composed_sandbox_providers_live.cpp`, run
  as root against a real containerd/runc daemon — **`ALL CHECKS PASSED`**, the first real production
  use of `MandatorySandboxProvider<ContainerdExecutionSurface>` through the actual, unmodified
  `session.start_run() -> invoke_tool()` 10-step pipeline anywhere in this codebase (confirmed via
  `grep -rl "MandatorySandboxProvider<ContainerdExecutionSurface>"` returning only these two new
  files). An independent red-team round `diff -u`'d both new files against their templates line by
  line and found the port genuinely faithful — every non-comment difference is exactly the template
  argument, session/branch-name string literals, scratch-directory paths, and banner text; zero silent
  logic changes (unlike ADR-104's `pclose()` finding or ADR-145's own earlier move-semantics finding,
  this port introduced none). It also independently reproduced every empirical claim (rebuilt both
  targets from scratch; re-ran the composed test as root, `ALL CHECKS PASSED`; ran it twice
  back-to-back with no cleanup between runs, both passed, since containers are named
  `ae_ces_<pid>_<seq>` with no cross-run collision; sanity-checked the tool binary with no
  `OPENAI_API_KEY` set, clean failure); confirmed the CMake link lists are correct and complete by
  tracing what `mandatory_sandbox_provider.hpp`/`sandbox_runtime.hpp` actually need (`agentengine::
  sandbox_io` is genuinely required — `sandbox_runtime.hpp` embeds a real `RealIoFileSystem` member —
  not blindly copied); confirmed both surfaces' default container image
  (`docker.io/library/alpine:latest`, `ContainerdExecutionSurface`'s own constructor default) flows
  through unmodified; and confirmed zero new authority path (same two `FsRead`/`FsWrite` capability
  grants as the Docker sibling, `run_command` still authorizes purely through `IdentityAuthority`/
  `Grant<T>`/`AsyncQuota<T>`, nothing in the diff touches capability/policy logic). It ALSO
  independently reproduced the already-disclosed container-orphan residual (just below) for the
  COMPOSED case specifically, via a real SIGKILL-mid-run test — confirmed not worse than the standalone
  case, same class, same cause. Two real but minor, PRE-EXISTING findings faithfully replicated from
  the template (not introduced by this port, and left as-is rather than fixed here since fixing them
  would mean also touching the already-shipped Docker sibling, a separate, tiny, disclosed cleanup):
  both tools `#include` `trust/secret_quarantine.hpp` but never actually use `QuarantineSecretStore`
  in code (a dead include — `InMemorySecretStore`, the type actually used, lives in `trust/secret.hpp`
  and arrives transitively via `session_builder.hpp`); both tools leave a small empty `/tmp` directory
  behind even on the clean "no API key" failure path, since ledger setup runs before the `build()`
  check that fails. Full Linux suite after this closure: 207/213 (97%) — the 6 failures are the same 5
  pre-existing, already-disclosed, environment-dependent ones plus `test_composed_containerd_
  providers_live` itself failing cleanly as a normal (non-root) `ctest` user, the identical expected
  posture `test_containerd_execution_surface` already has.
- **The bind-mount ordering hazard is proven for ONE real environment** (WSL2 Ubuntu 26.04's ext4-on-
  virtio, containerd 2.2.2, runc 1.4.0) — the design doc's own §5 already disclosed this does not
  generalize to every possible backing filesystem/snapshotter/kernel combination a real deployment
  might target; a future deployment should re-run the equivalent probe against its own real
  environment before relying on this finding.
- **`host_dir` containing a literal comma breaks `ctr`'s own `--mount` flag grammar** (confirmed by the
  design's own probe: `ctr` rejects the invocation cleanly, no container created, no silent
  mis-mount — a correctness limitation, not a security hole) — not defended against here; the one real
  production caller this promotion enables (`RealIoFileSystem::host_root()`, a host-configured scratch
  root, never model/guest-influenced per I2/I3) makes this an operator-configuration concern, not an
  attacker-reachable one, but a future caller passing a genuinely untrusted path should re-examine this.
- **This test requires root (or an unprivileged containerd-socket ACL) to actually run** — a real,
  disclosed environment precondition (containerd's own default socket permissions), not a code gap;
  matches the prove-phase's own identical disclosure. A CI environment wanting this test to run for
  real needs to provision for it explicitly.
- **`docker_execution_surface.hpp`'s own ADR-104/105 shell-injection-hardening findings
  (`docker_cli_reject_empty()`, the leading-dash/whitespace/trailing-backslash fixes) have no analogue
  needed here** — confirmed, not assumed: this conformer's `posix_spawnp()`+argv-vector path has no
  host-shell string-concatenation step for those classes of bug to exist in (this ADR's own §2/design
  doc's §4 finding 2 already established why), so there is nothing to port forward, not an oversight.
- **A container is orphaned, indefinitely, if the host process dies (SIGKILL, abrupt crash) between a
  successful `reset()` and this class's own destructor ever running** — found by this ADR's own §5
  red-team round, empirically reproduced (a real, still-running `ctr` container left behind after a
  SIGKILL test, manually cleaned up afterward). The SAME residual class `DockerExecutionSurface`'s own
  header comment already discloses for itself — but here it is **structurally unavoidable**, not merely
  unfixed: `ctr run --help` documents that `--rm` cannot be combined with `--detach`/`-d`, and this
  conformer must use `-d` to keep a long-lived container alive for later `exec()` calls, so there is no
  CLI-level auto-cleanup safety net available at all (Docker's own `--rm` at least fires on ordinary
  container exit, even though ADR-104's own red-team found that safety net itself unreliable in
  practice). A real fix (persisting instance ids somewhere reclaimable, mirroring `Ledger`'s own
  orphan-branch/A7 design) is real, contained follow-on work, not attempted here.
- **`host_dir_str` had no embedded-NUL guard, unlike `id`/`image`/`command`** — found by this ADR's own
  §5 red-team round, **fixed same day**: `create()` now also runs `reject_embedded_nul(host_dir_str,
  "host_dir")`, re-verified 18/18 still passing. Low severity even before the fix (no real
  validated-vs-executed divergence existed, since `create_directories()`'s own OS-level truncation
  would already apply independently) — fixed anyway for consistency with this file's own stated
  "reject outright, never truncate" principle.
- **This ADR's own C30/§5 rows are marked pending at write time** — must be filled in with real
  evidence (full `ctest` pass count, red-team findings or "none found") before this ADR is ready for
  Judge sign-off, matching this project's own "no self-Judging, evidence before status" discipline.
