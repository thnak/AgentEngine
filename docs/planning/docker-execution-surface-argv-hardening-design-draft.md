**UPDATE: promoted.** This design was implemented, empirically verified against a live Docker Desktop
daemon, and independently red-teamed (a second, fresh round, against the real code and daemon — not
just this document) in the same session. See `decisions/ADR-164-docker-execution-surface-argv-hardening.md`
for the executed evidence, the independent red-team's real findings (two: one pre-existing OS-ceiling
disclosure, one real-but-unreachable consistency gap, fixed), and the final Proposed status.

# Closing issue #50 the cheap way — argv-based `DockerCliBackend`, not a transport rewrite

Prompted by issue #50 ("Docker/containerd/Kata execution surfaces shell out via CLI string-building
instead of a structured API — real CWE-88 injection surface, and the char-denylist also blocks safe
agent commands"), and by this session's own live testing where an OpenRouter-driven `cli_chat.cpp`
session reported `%`/`^`/`"` as "banned in this sandbox's shell validator" and worked around the
model's own intended command rather than running it.

**This document narrows the issue's own scope, on the evidence, before proposing a fix.** Issue #50
treats Docker, containerd, and Kata as three instances of the same defect. Reading the current code
shows that is only true for one of them.

## 0. What already exists, and what's actually missing

- **`ContainerdCliBackend`** (`include/agentengine/sandbox/containerd_execution_surface.hpp:410-568`)
  already does this correctly. Every `ctr` invocation — `create()`, `exec()`, `destroy()`,
  `reap_orphans()` — goes through `ctr_cli_detail::run_argv()` (same file, lines 85-213): a real
  `std::vector<std::string>` handed to `posix_spawnp()`, never a shell-interpreted string. The file's
  own top comment (lines 403-409) states this explicitly: "Every invocation is a real argv vector via
  `posix_spawnp()` — never a shell-interpreted string — for the OUTER `ctr` invocation itself." The
  ONLY character-set check applied to `exec()`'s own `command` argument is `reject_embedded_nul()`
  (lines 260-...) — there is no denylist over `%`, `^`, `"`, `$`, or backtick anywhere in this file,
  because there is no host shell in front of `command` for those characters to break out of. `command`
  reaches the CONTAINER's own inner `/bin/sh -c` as one literal argv element (line 473) — a real, but
  already-accepted and correctly-scoped, risk layer one process boundary further in, not a host-side
  hole.
- **`KataBackend`'s `run_ctr()`** (`src/backends/kata/kata_backend_detail.hpp:103`,
  `src/backends/kata/kata_backend.cpp`) is the precedent `ContainerdCliBackend` itself cites as
  "reused verbatim, not reinvented" — the same argv-vector `posix_spawn` shape, same three-step
  `ctr task kill`/`ctr task rm`/`ctr container rm` teardown. Also already correct.
- **`DockerCliBackend`** (`include/agentengine/sandbox/docker_execution_surface.hpp:813-1046`) is the
  one real gap. Every subcommand — `create()` (line 843-846), `copy_to_container()`/
  `copy_from_container()` (lines 885-888, 914-917), `exec()` (lines 934-937), `reap_orphans()`'s own
  listing call (line 980) — builds a `std::ostringstream` into ONE string and hands it to
  `docker_cli_detail::run_capture(std::string const& command)`. On Windows, `run_capture()` does
  `"cmd.exe /c " + command`, raw-concatenated, through `CreateProcessA` (lines 128-146). On POSIX, it
  does `posix_spawn("/bin/sh", {"/bin/sh", "-c", command})` (lines 250-273) — a real argv vector for
  the *outer* spawn, but `command` itself is still one shell-interpreted string, so the host shell is
  still in the loop. Both platforms need a real defense against the caller-supplied portions of that
  string breaking out of the surrounding quoting — hence `docker_cli_reject_shell_breakout()`
  (`"\"%^\r\n"` on Windows, `"\"$\`\\\r\n"` on POSIX, lines 699-702 / 792-795), the denylist issue #50
  is really about, and the one a live model session actually tripped over.
- This means issue #50's "replace with the Docker Engine REST API / containerd gRPC API" framing is
  scoped to solve a problem that, on inspection, only exists on ONE of the three surfaces named, and
  the smaller mitigation the issue's own "Scoping only" section names as "worth scoping as a cheaper
  first slice" — argv instead of a shell string — is not a stopgap here, it is the SAME architecture
  `ContainerdCliBackend`/`KataBackend` already ship in production. There is no new client machinery to
  build (no gRPC/protobuf, no new outbound transport), no new ADR-061-shaped host/consumer boundary
  question, and no risk of relitigating "does AgentEngine own a socket" — this is a **within-process,
  same-CLI-tool** change: build the same `docker` invocations as argv vectors instead of strings.

## 1. The question, stated so it has a wrong answer

Does closing issue #50 mean **(a)** building real transport clients (Docker Engine REST API over the
daemon socket/named pipe, containerd gRPC over its Unix socket) to replace `docker`/`ctr` CLI
shell-outs entirely across all three surfaces — or **(b)** porting `DockerCliBackend` alone onto the
argv-vector discipline its own sibling `ContainerdCliBackend` and `KataBackend` already use in shipped,
Judged-track code, closing the CWE-88 class at its root without inventing a new transport axis this
codebase doesn't have?

## 2. The competing designs

### Design A (deferred, not rejected) — Docker Engine REST API + containerd gRPC, replacing the CLIs

**Steelman.** This is issue #50's own headline proposal, and it is not wrong on the merits: a real
structured API removes the "did I quote this correctly" question categorically, for both surfaces, and
`KataBackend` already demonstrates that Kata-strength isolation is just a different `runtime_type`
field on the same containerd control plane — a single gRPC client could in principle drive the
Docker-equivalent (`runc`) and Kata-VM (`kata-clh`) tiers together, a genuine architectural
opportunity for a future strength-tiered capability model.

**Deferred because:** it is a materially larger change than the actual defect requires. It needs new
client machinery this codebase has zero of today (no gRPC/protobuf dependency anywhere in the tree;
`provider_http_client.hpp` is a REST/TLS client shaped for OpenAI/Anthropic JSON-over-HTTPS, not a
local Unix-domain-socket gRPC client or a Windows named-pipe HTTP client). It also diverges hard by
platform: the Docker Engine API's Windows transport is a named pipe
(`\\.\pipe\docker_engine`), completely different code from containerd's Linux-only gRPC socket — so
"one structured client" is only true on the containerd/Kata side, and Docker still needs its own
separate implementation regardless. Per CLAUDE.md's own "Feature vs. safety balance" (`ADR-070`): ship
the disciplined, smaller fix behind real evidence now; a transport rewrite is real, valuable, future
work, not a precondition to closing the actual reachable defect. Not rejected outright — recorded here
as the deliberate next step if a future ADR wants the strength-tiered unification issue #50 also
gestures at.

### Design B (rejected) — keep the shell-string architecture, just widen/fix the character sets

**Steelman.** The smallest possible diff: `docker_cli_reject_shell_breakout()` already discloses
exactly which characters are dangerous on each platform (lines 694-702, 783-795) — maybe the fix is
just "figure out which of `%`/`^`/`"` a real agent command actually needs and carve out a narrower,
smarter allowlist inside the quoted string," without touching the spawn mechanism at all.

**Rejected because:** this is fighting the wrong layer. `%`, `^`, and `"` are dangerous ONLY because
`command` sits inside a `cmd.exe`-interpreted string in the first place — they are not inherently
unsafe characters, they are `cmd.exe` metacharacters. No amount of cleverness in the denylist changes
that a legitimate command like `printf "%s\n" "x"` or an arithmetic expression using `^` will always
collide with SOME real `cmd.exe`/`/bin/sh` metacharacter, because the constraint being solved for is
"stay safe inside an outer shell my own value is unavoidably interpreted by" — the wrong problem to be
solving at all once `ContainerdCliBackend` already proves the outer shell doesn't need to exist.
Narrowing a denylist around a structural problem is the "convenient-looking change" CLAUDE.md's own I2
framing warns against papering over, not fixing.

### Design C (accepted) — port `DockerCliBackend` onto `ContainerdCliBackend`'s own argv discipline

Build a real `std::vector<std::string>` argv for every `docker` subcommand `DockerCliBackend` issues,
spawn `docker`/`docker.exe` directly (no `cmd.exe`/`/bin/sh` in front of the OUTER invocation at all),
and pass `command` to the CONTAINER's own `sh -c` as one literal, unparsed argv element — exactly the
shape `ContainerdCliBackend::exec()` already ships (line 467-479). Concretely:

- **New shared primitive, `docker_cli_detail::run_argv()`.** POSIX side is close to a straight lift of
  `ctr_cli_detail::run_argv()` (containerd_execution_surface.hpp:85-213) — same
  `posix_spawnp()`/pipe/poll/timeout/output-cap shape, same `kProcessTimeoutSeconds`/
  `kOutputSafetyCapBytes` constants `docker_execution_surface.hpp` already carries (ADR-139, lines
  75-85). Windows side reuses this codebase's OWN already-tested MS-CRT argv-quoting algorithm —
  `agentengine::native_process::detail::quote_one_argument`/`build_command_line`
  (`src/backends/native_process/native_process_spawn.hpp:75-78`, implemented and independently
  unit-tested in `native_process_spawn.cpp`/`tests/test_native_process_spawn.cpp`) — to build a
  correctly-quoted `lpCommandLine`, THEN spawns it via `CreateProcessA`/`W` with
  **`lpApplicationName = nullptr`** so Win32's own standard PATH search resolves `docker` -> `docker.exe`
  directly, with **no `cmd.exe /c` wrapper at all**. All of the existing Job-Object timeout-kill
  machinery (`docker_execution_surface.hpp:150-238`, ADR-104) is kept, just re-targeted: it currently
  exists BECAUSE `cmd.exe` is a real parent process a plain `TerminateProcess` can't see past (the
  function's own header comment names the exact orphaning bug this caused); removing that middleman
  process means the Job Object now binds `docker.exe` itself as the top-level process — strictly
  fewer moving parts, not a weaker guarantee. (`native_process::spawn_native_process()` itself is NOT
  reused wholesale — its contract requires an already-PATH-resolved absolute `program_path`
  (`native_process_spawn.hpp:36-42`, an I2 "no ambient authority" requirement specific to
  ADR-071's unsandboxed native-execution providers). Only the pure, side-effect-free quoting helpers
  are reused; the new `run_argv()` keeps the bare-name-plus-PATH-search posture `ctr_cli_detail`'s own
  `posix_spawnp()` already established as this subsystem's accepted precedent — see §4 finding 2.)
- **`DockerCliBackend::create/exec/copy_to_container/copy_from_container/destroy/reap_orphans`** all
  rebuild their argument list as a `std::vector<std::string>` instead of an `std::ostringstream`, and
  call `run_argv()` instead of `run_capture()`. `exec()`'s generated call becomes
  `{"docker", "exec", inst.container_id, "sh", "-c", command}` — `command` as one argv element, never
  concatenated into a quoted string.
- **The denylist over `command` shrinks to `reject_embedded_nul()` alone**, matching
  `ctr_cli_detail::reject_embedded_nul()`'s own already-accepted reasoning verbatim: there is no host
  shell left to break out of, so `docker_cli_reject_shell_breakout()`'s whole character set
  (`"%^\r\n"` / `"$\`\\\r\n"`) stops being a real defense and starts being pure, unjustified
  over-blocking — the exact live-tested symptom this document opened with. This is the actual fix for
  issue #50's usability half.
- **`image`/`host_path`/`container_path` keep a real check, just a narrower one.** These are still
  individual argv elements `docker`'s OWN CLI flag/argument parser reads — `docker_cli_reject_leading_dash()`
  (lines 593-602, "a value entirely alnum/`-` satisfies every character-set check yet is emitted as a
  bare token positioned exactly where `docker` parses its own flags") is a REAL argv-level defense,
  independent of any shell, and stays unchanged. The Windows-only whitespace-splitting check
  (`docker_cli_reject_unsafe_for_unquoted_arg`, lines 680-692) becomes unnecessary once these values
  are individual argv elements rather than space-joined tokens in one command-line string — argv
  elements can legitimately contain spaces without splitting (`quote_one_argument` handles that
  correctly by construction) — so it is dropped, not weakened. `docker_cli_reject_empty()` (empty
  values still real, token-shift is only a shell-string-concatenation artifact, but rejecting an empty
  required argument outright is still cheap and correct) can also be dropped once every argv slot is
  filled positionally rather than joined with literal spaces a caller could collapse.
- **`ExecutionSurface`/`SandboxRuntime<Surface>` — unchanged, and this is worth stating explicitly.**
  Issue #50 asks for "an API that's easy for developers to switch between backends" as if that were
  still missing. It already exists: `SandboxRuntime` is templated on `Surface`
  (`include/agentengine/sandbox/sandbox_runtime.hpp:99`), and `DockerExecutionSurface`/
  `ContainerdExecutionSurface` both satisfy the same `ExecutionSurface` concept
  (`reset(host_dir)`/`run(command)`/`drain_to(host_dir)`) already. This design changes ONLY
  `DockerCliBackend`'s internals; `DockerExecutionSurface`'s own public methods
  (`docker_execution_surface.hpp:1046-1127`) do not change shape at all. No new abstraction is being
  proposed here because none is missing — re-litigating that would be solving a problem this codebase
  already solved.

## 3. The decision

Design C. Argv-based `DockerCliBackend`, ported directly from `ContainerdCliBackend`'s own shipped
shape, no new transport, no new dependency, `ExecutionSurface` unchanged. Design A (REST/gRPC) is
recorded as real future work, not blocking this fix, and not something this document is asking to be
judged now.

## 4. Red-team round (self-run this session — design-only, no code exists yet to attack)

- **Finding 1 — the "remove `cmd.exe`" claim needed to be checked against what the Job Object was
  actually protecting against, not just asserted as a simplification.** The existing header comment
  (`docker_execution_surface.hpp:94-111`) documents a REAL, previously-reproduced bug: an early fix
  that only did `TerminateProcess` on the top-level process left `docker.exe` running as an orphaned
  host process, because `cmd.exe` was a real intermediate parent `TerminateProcess` couldn't reach
  past. Removing `cmd.exe` from the picture doesn't remove the need for the Job Object — it changes
  what the Job Object's top-level member is. Verified this is still correct: `CreateJobObjectA` +
  `AssignProcessToJobObject` + `CREATE_SUSPENDED`-before-`ResumeThread` binds whatever process is
  actually spawned to the job BEFORE it can run, regardless of whether that process is `cmd.exe` or
  `docker.exe` directly — the mechanism doesn't assume a `cmd.exe` layer exists, it assumes "job-bind
  before first instruction," which holds either way. **Not yet closed**: the SAME live-Docker-Desktop
  probe the original fix used (`run_capture("docker exec <id> sh -c \"tail -f /dev/null\"",
  timeout_seconds=5)`, confirmed via `docker top` that the contained process was gone) needs to be
  re-run against the argv-based, `cmd.exe`-free spawn before this can be called proven rather than
  reasoned — this document does not claim that proof, see §5.
- **Finding 2 — bare-name PATH resolution needed an explicit justification, not a silent carry-over.**
  A first pass at this design considered resolving `docker`/`docker.exe` to an absolute path before
  spawning (matching `native_process::NativeExecRequest`'s own I2-motivated "never a bare name" rule,
  `native_process_spawn.hpp:36-42`). Checked against what this subsystem's OWN already-Judged precedent
  actually does: `ctr_cli_detail::run_argv()` calls `posix_spawnp()` — the `p`-suffixed variant that
  DOES search `PATH` — for `ctr` itself, unconditionally, in already-shipped ADR-145 code. Requiring
  pre-resolution for `docker` while `ctr`/`ctr`'s own sibling function does not would be an
  inconsistency this design introduces, not a gap it closes; `native_process`'s stricter rule is
  specific to ADR-071's deliberately weaker-isolation native-automation capability class (a different,
  already-disclosed threat model — the caller there IS meant to be running arbitrary host binaries by
  name, so pinning the resolution step matters more), not a general rule this subsystem's own container
  CLIs are bound by. Kept as bare-name + PATH search on both platforms, matching `ctr_cli_detail`'s own
  precedent exactly — a consistency argument, not a new judgment call.
- **Finding 3 — `docker_cli_reject_leading_dash()` and `docker_cli_reject_empty()` must survive the
  port; only the shell-breakout denylist and the whitespace-splitting check are made obsolete by argv.**
  Double-checked each of the four existing checks individually against "does removing the host shell
  make this specific check redundant" rather than treating the whole denylist as one unit: leading-dash
  is an argv/CLI-flag-parsing concern, unrelated to shell quoting, stays; empty-value token-shift is a
  string-concatenation-specific artifact, becomes moot once values are separate argv slots rather than
  space-joined, can be dropped (though keeping it costs nothing and fails a genuinely-invalid empty
  required argument earlier and more clearly — a judgment call for whoever implements this, not a
  security requirement either way).
- **Finding 4 — `command` still reaches a real inner shell (`sh -c`) inside the container, and that is
  correct, not a residual left unaddressed.** This mirrors `ContainerdCliBackend::exec()`'s own already-
  accepted risk layer exactly (that file's own comment, lines 461-466): the CONTAINER's own `sh -c`
  interpreting `command` is the intended, documented behavior an `ExecutionSurface::run(command)` call
  is FOR — a model asking to run a pipeline (`grep foo | wc -l`) needs a real shell somewhere to
  interpret it, and that shell being INSIDE the sandboxed container (not on the host) is exactly the
  trust boundary this whole subsystem exists to enforce. This design does not add or remove anything
  about that layer; it only removes the redundant, ALSO-shell-interpreted HOST layer that never needed
  to be there.
- **Finding 5 — blast radius on existing tests confirmed small, not merely assumed.** `run_capture(std::string)`
  is called directly, with static, non-attacker-influenced strings, from real test files
  (`tests/test_docker_orphan_reap.cpp:75,87,170`, `tests/test_sandbox_runtime.cpp:61`) for host-side
  setup/assertions outside the code path under test — this design does not remove or change
  `run_capture()` itself, only adds `run_argv()` alongside it and repoints `DockerCliBackend`'s own
  internal call sites, so those tests are unaffected by construction, not merely expected to still
  pass. Real, already-shipped live-Docker coverage that WOULD need to be re-run against the ported
  code before promotion: `tests/test_composed_sandbox_providers_live.cpp`,
  `tests/test_mandatory_sandbox_provider.cpp`, `tests/test_task_branch_tools.cpp`,
  `tests/test_task_branch_concurrent_dispatch.cpp` — all real, Docker-backed, not mocked.
- **Finding 6 (self-correction) — this document's own §2 Design A "deferred, not rejected" framing was
  checked against CLAUDE.md's actual rule and adjusted.** An earlier draft of this section called
  Design A "rejected" outright; re-reading CLAUDE.md's "Feature vs. safety balance" section
  (`ADR-070`) — "ship first behind [a] seam... harden later, as a follow-on ADR, not as a precondition
  to shipping" — makes "deferred" the accurate word: Design A is not wrong, it is simply not what this
  specific, reachable defect requires to close, and nothing in this document should be read as
  foreclosing it.

## 5. What this document does NOT establish

- **UPDATE (prove phase run, same session): the design is now implemented and empirically verified
  against a live Docker Desktop daemon on Windows — real code exists, this is no longer design-only.**
  `docker_cli_detail::run_argv()` (both platforms), the unified `docker_cli_reject_argv_value()`/
  `docker_cli_reject_embedded_nul()` checks, and the full `DockerCliBackend` port all landed in
  `include/agentengine/sandbox/docker_execution_surface.hpp`. Evidence, not reasoning:
  - `tests/test_docker_orphan_reap.cpp` — 14/14 checks pass (create()/destroy()/reap_orphans(), all
    argv-based now) against a live daemon.
  - `tests/test_sandbox_runtime.cpp` — full suite passes, including a rewritten check [6]: the old
    scenario (`echo "this double-quote trips the shell guard"` expecting rejection) is now, correctly,
    no longer rejected at all — replaced with an embedded-NUL-byte command, the one check that
    survives, matching `ctr_cli_detail::reject_embedded_nul()`'s own posture exactly.
  - `tests/test_composed_sandbox_providers_live.cpp`, `tests/test_mandatory_sandbox_provider.cpp`,
    `tests/test_task_branch_tools.cpp`, `tests/test_task_branch_concurrent_dispatch.cpp` — all pass
    unchanged, real Docker-backed regression coverage for `exec()`/`create()`/`copy_to_container()`/
    `copy_from_container()`/`destroy()` all now argv-based.
  - **New, permanent test**: `tests/test_docker_run_argv_timeout.cpp` (Windows-only, wired into
    `tests/CMakeLists.txt`) directly answers Finding 1 below — see its own updated status.
  - `agentengine_cli_chat`, `agentengine_sandboxed_shell_chat`, `agentengine_durable_sandboxed_shell_chat`
    (the three real tool binaries that `#include` this header) all rebuild cleanly against the ported
    API — `DockerCliBackend`'s own public method signatures never changed shape, only their internals.
- **Finding 1's Job-Object-retargeting reasoning is now empirically proven, not merely reasoned.**
  `tests/test_docker_run_argv_timeout.cpp` re-runs the exact scenario that validated the ORIGINAL
  `cmd.exe`-based fix (`docker exec <id> sh -c "tail -f /dev/null"` under a short timeout) against the
  NEW, `cmd.exe`-free `run_argv()` path, on a real Docker Desktop daemon: the call returned in ~3.0s
  (matching the 3s timeout given, not the real 30s default — proving the Job Object kill actually
  fired, not that the call happened to finish naturally), `exit_code == -1` as expected, and a
  `CreateToolhelp32Snapshot()`-based process count showed ZERO `docker.exe`/`com.docker.cli.exe`
  processes before AND after — no orphan, matching this design's own §2 reasoning exactly. 6/6 checks
  pass. This closes the single highest-value open question this document's first version named.
- **POSIX-side `run_argv()` is presented here as "close to a straight lift" of
  `ctr_cli_detail::run_argv()`, not verified line-by-line identical.** The two files' constants
  (`kProcessTimeoutSeconds`/`kOutputSafetyCapBytes`) already match by design (ADR-139 explicitly ported
  them from the containerd file), but `docker_execution_surface.hpp`'s current POSIX `run_capture()`
  merges stdout+stderr into one stream while `ctr_cli_detail::run_argv()` keeps them separate
  (`ProcessOutcome` has both `stdout_text` and `stderr_text`) — an actual API-shape decision (does
  `docker_cli_detail::run_argv()` return one merged stream, matching `SurfaceRunOutcome`'s existing
  single-field shape, or two, matching `ctr_cli_detail::ProcessOutcome`'s richer one) that a prove
  phase needs to settle, not assumed resolved by this document.
- **Design A (REST/gRPC transport) is not designed here at all** — this document only establishes that
  it is not required to close issue #50's actually-reachable defect, and records it as real,
  disclosed future work per §2.
- **containerd/Kata are Linux-only and untouched by this document** — this design changes nothing
  about them; they were already correct. This document does not re-verify that claim beyond the direct
  code reads in §0.
