# A real, OCI-standard `ExecutionSurface` conformer — a fresh design, not a Docker-CLI-syntax swap

Prompted directly by the project owner: the identity-native design's A3 execution-surface work
(`docs/planning/identity-native-sandbox-worktree-design.md` §36) has exactly one real `ExecutionSurface`
conformer, and it is Docker-CLI-specific rather than grounded in the OCI standard this codebase's own
shipped, Judged-track code (`KataBackend`) already builds on. Design document only, per explicit user
direction — no `ContainerdExecutionSurface` C++ conformer. Also per explicit user direction: treat this
the same way the identity-native design itself was built — a fresh, first-principles design, not a
minimal-diff retrofit of what already exists.

**Revision note**: this document's first version also deferred all environment provisioning and live
proof. §4/§5 now include one deliberate exception — after a design-only red-team round found a real,
load-bearing risk in the bind-mount architecture that reasoning alone couldn't settle, `containerd`+
`runc` were provisioned into the pre-existing WSL2 environment specifically to answer that ONE
question empirically, per explicit user direction to pursue it. This does not mean the conformer
itself is now being built — see §5's own honest scope statement.

## 0. What already exists, and what's actually missing

`DockerExecutionSurface` (`docs/planning/proofs/execution_surface/docker_execution_surface.hpp`) wraps
`docs/planning/proofs/docker_sandbox/docker_backend.hpp`, which shells out to the real `docker` CLI
via `_popen` (a single, shell-interpreted command string — the reason `docker_backend.hpp` needed its
own `reject_chars()` denylist fix after a real code-review pass found unescaped values could break out
of the surrounding quoting). §36's own residual: *"whether the three-verb `ExecutionSurface` shape
generalizes past Docker to a `native_jail`-shaped mediated-syscall backend... unverified — only one
conformer exists."*

This codebase already has a second, more mature, OCI-native container-tooling lineage that A3's own
work never drew on: `KataBackend` (`src/backends/kata/kata_backend.{hpp,cpp}`, `decisions/ADR-084`
through `ADR-093`, Judged/Proposed real production code) shells out to `ctr` (containerd's own CLI) via
`posix_spawn` with a real argv vector — never a shell string, so there is no shell-metacharacter
injection surface to defend against in the first place, a stronger posture than `docker_backend.hpp`'s
own denylist fix. This repo also has two dated, sourced research notes
(`docs/research/2026-08-24-containerd-ctr-run-config-vs-convenience-flags.md`,
`...-cni-kata-ordering.md`) that fetched and read containerd's own real source
(`cmd/ctr/commands/run/run_unix.go`, `run.go`, `commands.go`) to pin down exactly what `ctr run`'s
convenience flags and `--config` mode each do.

## 1. The question, stated so it has a wrong answer

Does closing A3's own "only one conformer, does the shape generalize" residual, and making the
execution-surface layer genuinely OCI-standard rather than Docker-proprietary, mean **(a)** building a
second conformer that still copies files in and out of a container the way `DockerExecutionSurface`
does, just against a different CLI's syntax — or **(b)** designing a second conformer fresh, on its own
merits, using this codebase's own already-proven `ctr`/OCI lineage, discovering along the way whether
the `ExecutionSurface` concept's own three verbs (`reset`/`run`/`drain_to`) actually required a
copy-based implementation at all, or whether that was only ever an artifact of Docker being the first
conformer anyone happened to write?

## 2. The competing designs

### Design A (rejected) — Podman, Docker-CLI-compatible

**Steelman.** Podman is explicitly marketed as the OCI-native alternative to Docker: daemonless,
directly OCI Runtime Spec + OCI Image Spec compliant, and its CLI is close enough to Docker's own that
`docker_backend.hpp`'s existing command-building logic could largely be reused with different flag
names.

**Rejected because:** zero prior experience anywhere in this codebase. Adopting it would make THREE
distinct container-tooling dependency classes real in this project at once (`docker`, `ctr`, `podman`)
when `ctr` is already this project's own established second one, with 6+ real ADRs and 2 sourced
research notes worth of hard-won knowledge about exactly how its CLI maps to the OCI spec, ready to
reuse directly. Podman would require redoing that whole investigation from scratch for no compensating
benefit — this design doesn't need anything podman offers that `ctr` doesn't already give it.

### Design B (rejected) — raw `runc`, no containerd at all

**Steelman.** The purest possible OCI-Runtime-Spec-only implementation: no daemon, no image-management
layer, just a `config.json` bundle and `runc create`/`start`/`kill`/`delete`. Nothing could be more
directly "the OCI standard" than talking to a reference OCI runtime with nothing else in between.

**Rejected because:** `runc` alone has no concept of OCI images at all — pulling, unpacking, and
snapshotting an image into a rootfs bundle is exactly the hard part containerd's `ctr images
pull`/`ctr images mount` already solves, and that this repo's own research already investigated deeply
at the containerd-source level. Going to raw `runc` would mean reimplementing OCI Distribution Spec
image-fetching from nothing, discarding real, already-invested research for a purity argument this
design doesn't actually need to make (containerd, and everything it drives, already IS the OCI
standard end to end — `ctr` is not a step away from OCI the way Docker's own daemon/CLI historically
was perceived to be).

### Design C (accepted) — a `ctr`-based conformer, built fresh, using the SIMPLER of `ctr`'s own two real modes

`KataBackend` was forced into `ctr run --config <spec.json>` specifically because it needs pids-limit
enforcement, VM-boot/CNI ordering control, and Kata-runtime annotations — none of which the
`ExecutionSurface` concept (`execution_surface.hpp`) has any dimension for at all: its three verbs are
`reset(host_dir)`/`run(command)`/`drain_to(host_dir)`, no `ResourceLimits`, no `NetPolicy`, no
lifecycle beyond "give me an isolated place, run one command, give me back what changed." Per this
repo's own sourced research (`...-config-vs-convenience-flags.md` §2), `ctr run`'s **convenience-flag
path** (i.e. NOT `--config`) already does image pull, unpack, and snapshot-rootfs creation
automatically, and already supports a `--mount` flag (confirmed present in containerd's own
`commands.go` `ContainerFlags`, alongside `--memory-limit`/`--net-host`/`--gpus`, though this repo's
research never needed to quote `--mount`'s own exact value grammar, since `KataBackend` never uses it —
named as a real, open verification item below, not assumed).

This means a plain-container `ExecutionSurface` conformer needs **no hand-authored OCI spec JSON at
all** — a real simplification `KataBackend`'s own forced complexity obscured until this design actually
tried the simpler path:

- **Image + rootfs**: handled automatically by `ctr run`'s own convenience-flag path — no separate
  `ctr images pull`/`ctr images mount` step needed (unlike Kata, which needs the rootfs pre-materialized
  specifically because `--config` mode skips all of that).
- **The bind mount, not a copy**: `--mount` (Docker/containerd-convention key-value flag — real
  syntax TBD, see §5) pointed directly at the real host staging directory
  (`RealIoFileSystem::host_root()`) as `/workspace`, read-write. This is the actual architectural
  finding this design surfaces, not a minor implementation detail: **`reset()`'s copy-in and
  `drain_to()`'s copy-out both disappear entirely.** A bind-mounted container writes directly to the
  real host directory the whole time it runs — there is no separate "pull the bytes back" step because
  they were never anywhere else. `execution_surface.hpp`'s own concept comment ("`T::drain_to(host_dir)`
  -- pull everything the surface's own view currently holds back onto real disk") is satisfied
  TRIVIALLY by a bind-mount conformer (already true by construction) — a real degree of freedom the
  concept always had, that `DockerExecutionSurface` being the only conformer ever built never had
  reason to expose. Also a closer architectural match to this codebase's own `NativeJailBackend`/
  `MountSpec` bind-mount philosophy than Docker's copy-based approach is.
- **Run**: `ctr tasks exec --exec-id <id> <container_id> <command...>` — the real, already-shipped
  `ctr tasks exec` verb `kata_backend.cpp` itself uses twice (lines 1065, 1089) for exactly this
  purpose, run against a long-lived, already-`ctr run -d`-started container (mirroring
  `DockerExecutionSurface`'s own `docker exec`-against-a-detached-container shape, and `KataBackend`'s
  own `-d` convention at line 1001).
- **Teardown**: the real, already-shipped three-step sequence `kata_backend.cpp` uses twice (lines
  1011-1013, 1140-1142) — `ctr task kill`, `ctr task rm`, `ctr container rm` — reused verbatim, not
  reinvented. **Confirmed by design red-team (§4 finding 3), not merely an unexplained quirk**:
  singular `ctr task kill`/`ctr task rm` (the top-level container's own primary task) and plural `ctr
  tasks exec`/`ctr tasks kill --exec-id` (a distinct, `--exec-id`-addressed exec'd sub-process within
  an already-running task) are two different, deliberately-distinct real operations, not two
  interchangeable spellings of the same one — an earlier version of this document hedged this as an
  unexplained inconsistency to preserve rather than a confirmed, correct pattern to follow; keep both
  forms exactly as `kata_backend.cpp` uses them for the corresponding verb.
- **Never a shell string for the OUTER `ctr` invocation itself** — every `ctr` invocation via a real
  argv vector and `posix_spawn` (`run_ctr()`'s own already-proven pattern, `kata_backend.cpp:94-128`,
  confirmed by direct read: no shell anywhere in that function), not `_popen` over a concatenated
  command string. This structurally removes the injection surface `docker_backend.hpp`'s own
  `reject_chars()`/`reject_shell_breakout()` was added to patch AFTER THE FACT, by construction, from
  the first line of code — for the image name, container id, mount spec, and every other `ctr`-level
  argument.
  **Correction (design red-team, see §4 finding 2): the `command` argument itself is a real, different
  case, not a lesser one.** `kata_backend.cpp:1065` runs `ctr tasks exec ... /bin/sh -c
  <request.source>` — a genuine, second, INNER shell, the identical shape `docker_backend.hpp:150-155`
  already uses (`docker exec <id> sh -c "<command>"`) for the identical reason (letting the caller's
  command use pipes/redirects/etc., matching what `ExecutionSurface::run(command)`'s own contract
  promises). This conformer needs the EXACT SAME defense `docker_backend.hpp` already built and
  proved for this exact problem — `reject_chars()`/`reject_shell_breakout()` reused verbatim against
  `command` before it reaches the inner `sh -c`, not a new mechanism, and not something the outer
  argv-vector/`posix_spawn` discipline makes unnecessary the way the first draft of this document
  claimed. The outer/inner distinction is real and worth keeping straight: `posix_spawn` closes host-
  side injection through `ctr`'s OWN arguments; it does nothing for what happens after containerd
  hands `command` to a shell one process boundary further in.

**Falsifiable claims (Design C):**
- **C1**: no hand-authored OCI runtime-spec JSON is needed for this conformer, unlike `KataBackend`.
  *Disproof: implementing this conformer turns out to require `--config` mode after all — e.g. because
  the convenience-flag path's automatic image/rootfs handling can't be combined with a bind mount the
  way this design assumes.*
- **C2**: a bind-mount-based conformer satisfies the EXACT SAME `ExecutionSurface` concept
  (`execution_surface.hpp`) `DockerExecutionSurface` already conforms to, with no concept changes.
  *Disproof: `reset()`/`run()`/`drain_to()`'s existing signatures or documented contract turn out to
  assume a copy-based implementation somewhere this design missed.*
- **C3 (revised post-red-team, §4 finding 2)**: routing every OUTER `ctr` argument through a real argv
  vector (never a shell string) closes host-side shell injection through `ctr`'s own arguments, by
  construction — but does NOT eliminate the need for `docker_backend.hpp`'s own
  `reject_chars()`/`reject_shell_breakout()`, reused verbatim, against the `command` string that still
  reaches a genuine INNER `sh -c` one process boundary further in (`ctr tasks exec ... sh -c
  <command>`, matching `kata_backend.cpp:1065`'s own real shape). *Disproof: either the outer
  `ctr`-invocation path concatenates caller-influenced values into a shell-interpreted string somewhere
  (would reopen the closed class), or the inner `command` string reaches `sh -c` without the reused
  `reject_chars()`-equivalent check (would leave the still-open class unpatched).*

  **DISPROVEN, by the real implementation (`containerd_ctr_backend.hpp`/`containerd_execution_surface.hpp`,
  2026-08-28) — recorded here per this track's own "spec vs. code disagree, fix the spec" rule
  (CLAUDE.md), not silently left stale.** The real code passes `command` as ONE exact `argv[]` element
  to `posix_spawn` (`kata_backend.cpp`'s own `run_ctr()` pattern, extended to `exec` the same way) —
  never concatenated into a string a HOST shell parses. Re-examined against `docker_backend.hpp`'s own
  `reject_shell_breakout()` comment: that defense exists specifically because `_popen` hands a
  concatenated string to a real host shell (`cmd.exe`), which a literal `"` can break out of — a defense
  against HOST-side string concatenation, not against the container's own inner shell per se.
  `posix_spawn`+argv never concatenates anything into a host-parsed string, so that specific injection
  class genuinely does not exist on this path — confirmed empirically (`grep -c
  "reject_chars|reject_shell_breakout" kata_backend.cpp` → 0, the real, shipped precedent this design
  already cites carries no such defense either, by the identical reasoning) and independently verified
  by a second, adversarial red-team round. **What C3's disproof condition actually triggered was real —
  the inner `command` string does reach `sh -c` with no `reject_chars()`-equivalent — but does NOT
  reopen the class C3 was written to prevent**, because that class requires a host-side shell to break
  out of, which never exists here. The REAL, POSIX-correct analog built instead:
  `reject_embedded_nul()` (argv truncation-at-NUL — the actual class of bug an argv-vector call can
  still have, proven to reject rather than silently truncate). The container's own inner `/bin/sh -c`
  interpretation of `command` is the SAME accepted-risk boundary `kata_backend.cpp`'s own `ExecRequest::
  source` documentation already establishes ("trusted to have already resolved and mediated") — not a
  new hole this conformer introduces. C3 itself is retired as originally stated; the real property this
  conformer establishes is: no HOST-side shell-injection surface exists anywhere in the outer `ctr`
  invocation OR the inner command delivery, by construction (argv-only throughout), and the inner
  container-shell interpretation is an already-accepted, already-documented risk layer this conformer
  inherits rather than introduces.

## 3. The decision

**Design C is accepted**: a new, Linux-only `ContainerdExecutionSurface` (name TBD at implementation
time — `platform_mask = linux_x86_64` only, matching `KataBackend`'s own real scope; `ctr` only
meaningfully talks to a local containerd Unix socket, so this conformer's own C++ code would need to
compile and run AS a Linux binary, not a Windows binary talking to a remote daemon the way
`DockerBackend`'s Windows-native `_popen`-over-Docker-Desktop's-client-daemon-split currently does),
built on `ctr run`'s convenience-flag path with a bind mount, never `--config` mode, reusing
`KataBackend`'s own real `run_ctr()`-shaped argv-vector `posix_spawn` pattern for every invocation.

**This authorizes the design, not an implementation.** No file under `docs/planning/proofs/` or
anywhere else changes as a result of this document by itself, per explicit user direction this pass.

## 4. Independent red-team round (design-only — no code exists yet to attack)

One independent, adversarial pass, run against this draft's first version and the real, cited sources
directly (not this draft's paraphrase of them).

- **Finding 1 — a genuine ordering hazard between `SandboxRuntime::run()`'s fixed step sequence and a
  bind-mount conformer, that Docker's copy-based conformer structurally never had to face.**
  `sandbox_runtime.hpp`'s `run()` calls `io_fs_.materialize()` (step 2) unconditionally BEFORE
  `surface.reset()` (step 3) — confirmed by direct read of both files. `real_io_filesystem.hpp`'s
  `materialize()` does `remove_all(host_root_)` then `create_directories(host_root_)` — a full
  delete-and-recreate, not an in-place rewrite (confirmed, lines 299-301). For
  `DockerExecutionSurface`, this is harmless: a container's filesystem is never aliased to
  `host_root_`. For a bind-mount `ctr` conformer, the PREVIOUS turn's container may still be alive and
  still bind-mounted to `host_root_` at the exact moment `materialize()`'s `remove_all`/recreate
  fires — `reset()` (which would destroy that old container) hasn't run yet, it's the very next step.
  **UPDATE (2026-08-27, real, empirical proof — no longer reasoning): a real containerd 2.2.2/runc
  1.4.0 deployment was provisioned (Ubuntu 26.04, WSL2 — this repo's own established
  `KataBackend`-precedent environment, nothing existed here before this pass) specifically to test
  this. `docs/planning/proofs/execution_surface/probe_bind_mount_ordering_hazard.sh` reproduces the
  exact sequence — a real container bind-mounted at a host path, a host-side `rm -rf`+`mkdir` while
  that container is still running, then destroy-and-replace — and the real, recorded result (quoted
  verbatim in that file's own header comment) confirms the reasoning WAS right, precisely: the
  host-side recreate succeeds cleanly (no error, no hang) while the old container survives; the old
  container's bind-mounted view becomes a genuinely EMPTY, orphaned directory (not stale-with-old-
  content, not leaking the new content either — `rm -rf`'s own recursive unlink empties the orphaned
  inode the mount still references); destroying the old container and starting a fresh one at the
  recreated path sees ONLY the fresh content, no leakage either direction; a write from the fresh
  container round-trips correctly onto the real host disk. **This is now a proven result for this
  containerd/runc/kernel combination, not an assumption** — see §5 for the honest scope of what this
  one real run does and does not generalize to.
- **Finding 2 (REAL correction, folded into §2/§3 above): the "never a shell string" claim overstated
  what `posix_spawn` actually closes.** `kata_backend.cpp:1065` genuinely runs `ctr tasks exec ...
  /bin/sh -c <command>` — a real, second, inner shell, identical in shape to
  `docker_backend.hpp:150-155`'s own `sh -c "<command>"`. The outer `ctr`-invocation argv-vector
  discipline closes host-side injection through `ctr`'s OWN arguments; it does nothing for the inner
  shell `command` itself reaches. Corrected: this conformer needs `docker_backend.hpp`'s own
  `reject_chars()`/`reject_shell_breakout()` reused verbatim against `command`, not a claim that the
  problem doesn't exist here.
- **Finding 3 (REAL correction, folded into §2/§3 above): `ctr task` vs `ctr tasks` is not an
  unexplained inconsistency to merely preserve.** Confirmed via both real call sites
  (`kata_backend.cpp:1009-1013`, `1140-1142`): singular `ctr task kill`/`ctr task rm` control the
  top-level container's own primary task; plural `ctr tasks exec`/`ctr tasks kill --exec-id` address a
  distinct, `--exec-id`-scoped exec'd sub-process. Two different, deliberately correct operations, not
  two spellings of one.
- **Podman/runc rejection (§2, Designs A/B)**: confirmed reasoned but under-argued technically — the
  rejection rests on precedent-reuse ("this codebase already has `ctr` experience"), not on a
  capability `ctr` has that podman or raw `runc` genuinely lack for this specific use case. Not fatal
  (precedent-reuse is a legitimate, if not the strongest possible, argument — this document's own
  §34.10/A9 already establishes "reuse is real, disclosed reasoning, not required to be the ONLY
  possible reasoning" as an accepted standard elsewhere in this design), but named honestly as the
  weaker of this draft's arguments rather than dressed up as a technical necessity it isn't.
- **Everything else checked** — C1's premise (the convenience-flag path's automatic image/rootfs
  handling), and every other citation against `kata_backend.cpp`/the sourced research doc — confirmed
  accurate.

## 5. What this document does NOT establish

- **Finding 1's ordering hazard is now proven, not merely reasoned (see §4's update) — but only for
  ONE real environment**: WSL2 Ubuntu 26.04's own filesystem stack backing `/tmp` (ext4-on-virtio, per
  WSL2's own architecture), containerd 2.2.2, runc 1.4.0. This does NOT establish the same result for
  every filesystem a real deployment's staging directory might sit on (a network filesystem, a
  different overlay/snapshotter configuration, a different kernel version) — the underlying mechanism
  (a bind mount holding its own dentry reference independent of the path's own directory-entry churn)
  is standard Linux VFS behavior, not a WSL2-specific quirk, but this pass verified exactly one
  concrete instance of it, not the general case across every possible backing filesystem. A real
  implementation should still re-run `probe_bind_mount_ordering_hazard.sh`-equivalent verification
  against whatever real deployment filesystem it actually targets, not assume this one result travels
  automatically.
- **UPDATE (2026-08-28): a real, standalone C++ conformer now exists and is proven live.**
  `docs/planning/proofs/execution_surface/containerd_ctr_backend.hpp`/`containerd_execution_surface.hpp`
  satisfy the real `ExecutionSurface` concept (`static_assert`-checked) and were compiled (`g++ 15.2.0`,
  `-std=c++23`, inside WSL2 — no `clang++` present there) and run for real against live
  containerd/runc: 16/16 checks pass, including a live bind-mount round trip with zero `drain_to()`
  call needed (a container write lands on real host disk immediately) and a second, C++-side
  reconfirmation of the ordering-hazard finding just below. Independently red-teamed; verdict solid,
  one doc-sync gap (C3, above) found and fixed here. **Still NOT established**: integration with the
  real `Ledger`/`SandboxRuntime`/`RealIoFileSystem` stack (this conformer is proven standalone, the
  same bar `probe_docker_sandbox.cpp` originally proved Docker's own mechanics at, before
  `SandboxRuntime` unified anything — POSIX siblings for the Windows-specific real production APIs
  that stack currently assumes DO exist in this repo, `worktree_digest_posix.cpp`/
  `worktree_mount_fs_posix.{hpp,cpp}`, making that integration plausible for a future pass, not
  attempted here); `drain_to()` to a directory OTHER than the one bind-mounted falls back to an
  implemented-but-unexercised plain host copy; a host path containing a literal comma breaks `ctr`'s
  own `--mount` flag grammar (confirmed by direct test: `ctr` rejects the invocation cleanly, no
  container created, no silent mis-mount — a correctness limitation, not a security hole).
- **UPDATE (2026-08-27): the environment gap named in this bullet's original version is closed.**
  `containerd` 2.2.2 + `runc` 1.4.0 were apt-installed into the pre-existing WSL2 "Ubuntu" distro
  (Ubuntu 26.04 LTS) — the exact environment class `KataBackend`'s own real proofs already used —
  confirmed live (`ctr version`, a real `docker.io/library/alpine:latest` OCI Distribution Spec pull).
  This closes the provisioning gap for THIS design's own targeted ordering-hazard probe; it does not
  by itself mean a full `ContainerdExecutionSurface` conformer is ready to build and prove — that is
  still real, larger, not-yet-started work.
- **`--mount`'s exact flag-value grammar — CONFIRMED, empirically, not merely plausible.**
  `--mount type=bind,src=<host_dir>,dst=/workspace,options=rbind:rw` was run for real against
  containerd 2.2.2 (`probe_bind_mount_ordering_hazard.sh`, steps 1 and 7) and produced exactly the
  bind-mounted, read-write container view this design assumed — closing what was originally an
  unverified-from-memory assumption with a real, reproduced result instead of a citation of
  `commands.go`'s parser source (which this pass still did not read directly — the empirical run
  answers the practical question just as well for this design's purposes, but a future implementation
  wanting the parser-level guarantee `--config`'s own exclusivity finding had should still fetch it).
- **Whether this conformer's bind-mount approach interacts safely with `SandboxRuntime::run()`'s own
  materialize→reset→run→drain→scan→commit sequence (§36) is reasoned about here, not proven.** In
  particular: whether a live bind mount could let the contained process's writes become visible to
  `RealIoFileSystem`'s own staging area before a turn's `run()` call is considered complete, in any way
  that matters, is a real question a red-team round should check (§4) — not assumed safe by the
  architecture's own elegance.
- **Resource limits, network policy, and every other `SandboxSpec`-shaped dimension** are out of scope
  for this conformer for the same reason they were out of scope for `DockerExecutionSurface` — the
  `ExecutionSurface` concept itself has no such dimensions; a future conformer needing them is a
  different, larger design question this document does not attempt.
