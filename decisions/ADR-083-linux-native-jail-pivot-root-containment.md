# ADR-083 — Does a real `pivot_root` + bind-mount jail close `LinuxNativeJailBackend`'s filesystem
# and process-visibility gap (008 §9 G2/G3's Linux half) without reopening I2/I3 or regressing the
# existing Linux native-jail suite?

**Status:** Judged (design → red-team → prove complete, this session, 2026-08-23 — same day as
`decisions/ADR-082-native-jail-promotion-gate-008-9.md`'s G6/G7 closures). Design and self-red-team
phase: `docs/planning/linux-native-jail-pivot-root-containment-design-draft.md` (written earlier this
session, before a Linux build/test environment was available). Prove phase: this ADR, run against a
real Linux 6.6 kernel (WSL2/Ubuntu) obtained specifically to close this gap — cgroups v2 fully
mounted (`cpuset`/`cpu`/`io`/`memory`/`hugetlb`/`pids`), real seccomp-BPF support, and working
namespace creation under `CAP_SYS_ADMIN` (root).

**Relates to:** `decisions/ADR-082-native-jail-promotion-gate-008-9.md` (this ADR closes that
document's own named residual — "G2/G3's shared Linux gap" — the one item left after G6/G7's
same-day closure), `008-Sandbox-and-Isolation.md` §1b/§2/§3/§9 (G2 containment, G3 no ambient
authority, the `native-jail` profile's Linux half), `decisions/ADR-004-appcontainer-native-jail-
windows-backend.md` (the Windows side's already-Judged AppContainer filesystem containment this
closes the Linux parity gap against), `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap
#10 (the finding both the design draft and this ADR address), `docs/planning/milestone-2-tools-
capabilities-sandbox-breakdown.md` task C3/C5 (the Linux abuse-corpus and no-ambient-authority tests
this ADR updates rather than leaves as documented gaps).

## 1. The question

**Stated so it has a wrong answer:** `008-Sandbox-and-Isolation.md` §9 G2 requires "every §7 abuse
case is contained" and G3 requires "a probe guest enumerating filesystem, network, env, and processes
finds exactly the granted set and nothing else, on each backend." Before this ADR,
`LinuxNativeJailBackend` met G3 on the env and network axes but not on filesystem or process
enumeration: `CLONE_NEWNS` gave the guest a *private copy* of the host's mount table, not a
*restricted* one — any host path readable by the invoking user was readable from inside the guest,
by absolute path, and `CLONE_NEWPID`'s fresh PID namespace was paired with the host's own
unremounted `/proc`, so the guest's process enumeration showed the host's real process list. Both
gaps were explicitly, honestly tracked as open (`test_native_jail_abuse_corpus_linux.cpp`'s and
`test_native_jail_ambient_authority_linux.cpp`'s own header comments; `ADR-082 §4`) rather than
silently assumed closed — but they were still real, unclosed containment gaps.

**The design question:** can a real `pivot_root` + bind-mount jail be built inside the child, after
`clone()` and before `execve`, that (a) restricts the guest's visible filesystem to exactly the
granted `MountSpec` set, (b) gives the guest a namespace-local `/proc`, (c) enforces read-only grants
as genuinely read-only (not just labeled so), (d) never leaks a mount into the *host's* own mount
table, and (e) does all of this without reopening I2 (no ambient authority — an implicit, ungranted
toolchain mount would be exactly that) or I3 (nothing about the guest's own claims is trusted),
using only kernel primitives this codebase already reaches for elsewhere (no new third-party
dependency, matching `seccomp_filter.hpp`'s own "pure OS API" posture)?

## 2. Design and self-red-team (already complete before this ADR)

Full design and red-team record: `docs/planning/linux-native-jail-pivot-root-containment-design-
draft.md`. Summary, not restated in full:

- **MS_PRIVATE first, unconditionally.** `mount(nullptr, "/", nullptr, MS_REC|MS_PRIVATE, nullptr)`
  before any other mount/`pivot_root` call — a systemd-managed host's root mount defaults to
  `MS_SHARED`, and skipping or misordering this step would leak every subsequent bind mount into the
  *host's* own mount table (self-red-team MUST-FIX 1).
- **A dedicated tmpfs jail root**, not the cgroup directory or any host directory reused for another
  purpose — avoids the `rmdir`/`EBUSY` collision hazard a naive implementation sharing the cgroup
  directory would hit.
- **Exactly the granted `MountSpec` set, bind-mounted in** — a read-only grant gets a *required
  second* `MS_BIND|MS_REMOUNT|MS_RDONLY` call, because the initial `MS_BIND` call silently ignores
  `MS_RDONLY` passed in the same call (self-red-team MUST-FIX 2). No implicit `/bin`/`/lib`/`/usr`
  mount of any kind.
- **`pivot_root`, not `chroot`** — `chroot` alone is escapable via `fchdir` from a pre-existing fd to
  outside the new root; `pivot_root` detaches the old root from the mount tree structurally.
- **A fresh `procfs` mount, not a bind-mounted one**, mounted after `pivot_root` from a process
  already inside the new `CLONE_NEWPID` namespace — namespace-local by construction.

## 3. The shipped design (this ADR's own implementation)

`src/backends/native_jail/linux_native_jail_backend.{hpp,cpp}`:

- **`setup_jail()`** (new, `linux_native_jail_backend.cpp`) implements §2 verbatim, called from
  `child_entry()` after the sync-pipe handshake (cgroup membership already confirmed by the parent)
  and **before** `install_seccomp_filter()` — `mount`/`umount2`/`pivot_root` are all three in that
  filter's own denylist (`seccomp_filter.cpp:31`), so this trusted jail-construction code must finish
  before the filter goes on and the untrusted guest command runs.
- **`Instance` gained**: the full `MountSpec` grant set (`mounts`, rebuilt into the jail on every
  `exec()` call — a mount namespace is per-process, not persisted on the handle), `cwd_guest_path`
  (the first read-write mount's *guest* path, replacing the pre-pivot_root design's use of the *host*
  path — the host path stops resolving once the guest's root is the jail), and `exec_seq` (a
  per-instance counter minting a fresh, unique `jail_root` directory name on every `exec()` call).
- **`jail_root` lifecycle brackets the child's, host-side**: `mkdir()`'d by the *parent* before
  `clone()` (so the id is real host state even if the child dies before starting its own setup), and
  `rmdir()`'d by the parent after the child is fully reaped, on every exit path (clone failure,
  cgroup-join failure, normal completion, and timeout-kill). Because `MS_PRIVATE` runs first inside
  the child, none of the child's own mounts ever reach the host's mount table — from the host's side,
  `jail_root` is an ordinary, always-empty directory the whole time; the `rmdir` is real cleanup, not
  racing a lazy unmount.
- **`hostile_child_posix.cpp`** gained `probe_read <path>` / `probe_write <path>` modes (fs-escape
  and read-only-bind-mount probes — the Linux analogues of the env/net/proc probes it already had).
- **`tests/helpers/native_jail_linux_toolchain_mounts.hpp`** (new, test-only): once the jail only
  contains the granted set, `/bin/sh` and the hostile-child test binary's own shared-library
  dependencies are no longer ambiently visible — every existing Linux native-jail test's `SandboxSpec`
  now needs an explicit, read-only toolchain grant (`/bin`, `/lib`, `/lib64`, `/usr`, and the
  directory containing the test binary) for `/bin/sh -c "<hostile child> ..."` to resolve at all,
  exactly the "no implicit mount, needs its own explicit grant" consequence §2 predicted.
- **New test**, `test_native_jail_fs_containment_linux.cpp`: the Linux analogue of
  `test_native_jail_abuse_corpus_windows.cpp`'s Case 4 (fs-escape) plus a dedicated read-only-bind
  positive/negative pair (MUST-FIX 2) — both named in the design draft §4 as what proving this design
  would need.
- **`test_native_jail_ambient_authority_linux.cpp`'s axis 3** (process enumeration) flipped from
  documenting a known gap to asserting real containment — the fresh `/proc` mount closes it as a
  side effect of the same `setup_jail()` call, matching the design draft's own observation that the
  fs and process-visibility gaps shared one root cause.
- **`test_native_jail_teardown_cycles_linux.cpp`** gained a 4th census axis (`jail_dirs`) proving the
  per-exec `mkdir`/`rmdir` bracket doesn't leak across 300 cycles — the concrete, falsifiable form of
  the design draft's MUST-FIX 1 verification item for the one piece of that leak surface visible from
  the host side.

## 4. Prove phase (this ADR's own evidence, run directly against a real Linux kernel)

**Environment obtained for this ADR:** WSL2 (Ubuntu, kernel 6.6.87.2), a real (lightweight-VM-backed)
Linux kernel, not a container atop the host kernel — cgroups v2 fully mounted with every controller
this backend needs, seccomp-BPF fully supported, namespace creation working under root. The repo was
cloned into WSL's native ext4 filesystem (not `/mnt/c`, which is slow and has cross-filesystem
hardlink/permission quirks observed directly when first attempted). Missing build deps
(`libseccomp-dev`, `pkg-config`, `python3-dev`) installed via `apt`. Full tree (652 targets)
configured and built clean with `cmake`/`ninja`/gcc 15.2, zero errors, before any of this ADR's
changes — establishing a real baseline, not assumed.

**Baseline (this session's commit `66b1d1d`, before this ADR's changes), full suite:**
```
100% tests passed, 0 tests failed out of 155 (1 platform-skipped)
```

**After this ADR's changes, full suite, run in parallel (`ctest -j 4`) as root (needed for
`CAP_SYS_ADMIN`, namespace creation, and cgroup delegation-root writes):**
```
100% tests passed, 0 tests failed out of 161 (1 platform-skipped)
```
161 = 155 baseline + 6 gated Linux native-jail tests (`AGENTENGINE_LINUX_SANDBOX_TESTS=ON`, off by
default, run here explicitly) — `test_native_jail_backend_linux`, `_abuse_corpus_linux`,
`_parity_linux`, `_ambient_authority_linux`, `_teardown_cycles_linux` (all five pre-existing, now
updated for the toolchain-grant requirement, run clean under the new jail), plus the new
`test_native_jail_fs_containment_linux`.

**A real pre-existing bug surfaced and fixed by running the full suite under `-j 4`, not scoped to
this ADR's own containment work but caused by adding a 6th consumer of it**: every Linux native-jail
test constructs its `LinuxNativeJailBackend` with the *same* default host paths
(`/sys/fs/cgroup/agentengine`, `/tmp/agentengine-native-jail`) and mints ids from a *process-local*
counter starting at 0 — two test *binaries* running concurrently under `ctest -j` could independently
compute the identical `linux_native_jail-0` id and collide creating the same cgroup/jail-root
directory. Observed directly: `mkdir(/sys/fs/cgroup/agentengine/linux_native_jail-0) failed: File
exists`, cascading into spurious `create()` failures in three sibling tests. Fixed with `RUN_SERIAL
TRUE` on all six gated tests in `tests/CMakeLists.txt`, the same treatment
`test_native_jail_teardown_cycles_windows` already has for an analogous shared-resource reason.
Confirmed fixed: the `-j 4` run above is 100% green with the property in place.

**`test_native_jail_fs_containment_linux`, measured output, this pass:**

| Claim | Probe | Measured | Verdict |
|---|---|---|---|
| A host path outside every granted `MountSpec` is unreadable from inside the jail, even by absolute path. | `probe_read <secret file outside every grant>` | `READ_DENIED err=2` (ENOENT — the path does not exist inside the jail's own mount table at all). | **CORRECT** (G2). |
| The denial above is real containment, not a broken guest. | `probe_read` the identical mechanism against a path *inside* the granted `/work` mount. | `READ_OK bytes=26 data=granted_and_readable_24680`. | **CORRECT** (positive control). |
| A read-only-granted mount is genuinely read-only (MUST-FIX 2). | `probe_write` an existing file under a `read_write=false` grant. | `WRITE_DENIED err=30` (**EROFS** — the required second `MS_REMOUNT|MS_RDONLY` call is genuinely present, not just the first `MS_BIND` call, which alone would leave it writable). | **CORRECT**. |
| The RO denial is real, not a no-op probe. | Host-side re-read of the target file after the denied write attempt. | Content byte-for-byte unchanged. | **CORRECT**. |
| The write mechanism itself works when granted read-write. | `probe_write` the identical file shape under a `read_write=true` grant. | `WRITE_OK bytes=20`, and the write is visible reading the file from the **host** side afterward (a real bind mount, not an isolated tmpfs-only copy). | **CORRECT** (positive control). |

**`test_native_jail_ambient_authority_linux` axis 3 (process enumeration), measured output, this
pass:**
```
measured: contained probe_proc stdout=PROC_VISIBLE total=2 target=no
ok: the guest's /proc is a fresh, namespace-local mount ... cannot enumerate a host PID it was never granted
ok: positive control: the guest's own /proc is NOT empty (it sees at least itself)
```
`total=2`, not `total=0` — the fresh `/proc` mount genuinely mounted and shows the guest's own tiny
PID-namespace-local process set, not an empty/broken mount silently passing the negative check for
the wrong reason.

**MUST-FIX 1, verified directly against the real host, not merely argued from the design**: host
`/proc/mounts` line count measured before and after running the 300-cycle teardown test plus the new
fs-containment test (hundreds of jail creations/teardowns, each performing multiple bind mounts and a
`pivot_root`):
```
host /proc/mounts entries: before=41 after=41
MUST-FIX 1 VERIFIED: no host mount leak
```

**`test_native_jail_teardown_cycles_linux`'s new `jail_dirs` census, measured, this pass:**
```
census: fds 6 -> 6, rss_kb 4224 -> 4352, cgroup_dirs 43 -> 43, jail_dirs 0 -> 0
```
Zero growth across 300 create/exec/destroy cycles on all four censused resources, including the new
one this ADR added.

## 5. Decision

**Adopted.** `LinuxNativeJailBackend`'s `setup_jail()` is the real, kernel-enforced filesystem and
process-visibility containment `008 §9 G2` (the filesystem half) and `G3` (the process-enumeration
axis) required on Linux, closing `ADR-082`'s last-named residual. I2 is not reopened: the guest
receives exactly the granted `MountSpec` set and nothing implicit; no ambient host path, toolchain
included, is ever reachable without an explicit grant. I3 is not reopened: nothing about the jail's
correctness depends on anything the guest process claims — every check in §4 is an external,
host-side observation (`errno`, host-side file re-reads, host `/proc/mounts`).

**Residual risks, named rather than implied:**
- **The id-collision bug is fixed at its root, same-day follow-on.** `create()`'s id is now
  `"linux_native_jail-" + getpid() + "-" + counter` (was counter-only, process-local, not
  host-unique) — a PID cannot repeat among processes alive at the same instant, closing the
  collision `RUN_SERIAL` had only serialized around. Verified directly: two copies of
  `test_native_jail_backend_linux` launched concurrently (no `RUN_SERIAL`, no `ctest`) both pass.
  `RUN_SERIAL` is kept on the gated tests as defense in depth against a reused PID from a
  since-exited process racing a still-running one's leftover directory, not because the collision
  is still possible in the common case.
- **The toolchain-mount test helper (`add_shell_toolchain_mounts`) grants host `/bin`/`/lib`/
  `/lib64`/`/usr` at identical guest paths** — acceptable for this M2 raw-shell-exec test scope
  (`linux_native_jail_backend.hpp`'s own "not yet Runner-mediated" scope note, unchanged by this
  ADR), explicitly not a production filesystem-visibility policy; 010's interpreter-level mediation
  is the real production boundary, not this backend's raw `/bin/sh -c` path.
  `MountSpec::guest_path`'s own doc comment calls for "never runtime-revealing" guest paths, which
  this test helper does not honor — flagged here as a test-only exception, not proposed as the
  pattern a real caller should follow.
  `jail_root_base_`'s content-size cap (`tmpfs size=67108864` — 64MiB) is a generous-but-finite
  Machine Safety bound on the jail's own scaffolding (mount points, `/.old_root`, `/proc`'s mount
  point), not a resource-limit axis a `SandboxSpec` can configure — bind-mounted content itself is
  unaffected by this cap (a bind mount is not tmpfs-backed storage).
- **`cpu_ms`/`disk_bytes`/`net_bytes`/`fds` enforcement remains `cgroup_limits.hpp`'s separate job**,
  unchanged and untouched by this ADR, same layering the Windows side already has between
  `app_container_profile.hpp` and `job_object_limits.hpp`.
- **008 §9 G1 (cross-platform outcome-classification parity)** is unaffected by this ADR — the
  `setup_jail()` failure path is `_exit(125)` inside the child, which the existing wait loop already
  classifies the same as any other guest-side setup failure (`crash`), not a new outcome class.
- **Linux parity for the *jailed Python worker process* (ADR-081's Windows-only design) is still a
  separate, unclosed gap** (`docs/planning/linux-jailed-python-worker-design-draft.md`,
  design-phase-only) — this ADR closes the raw-shell-exec `LinuxNativeJailBackend` path's own
  filesystem/process containment; it does not build a Linux jailed-worker process for
  `execute_code`, a structurally different, still-open question.
