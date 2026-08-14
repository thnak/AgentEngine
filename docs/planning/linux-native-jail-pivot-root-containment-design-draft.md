# Design draft: Linux `native-jail` filesystem and process containment (`pivot_root` + bind mounts)

**Status:** Design draft, self-red-teamed once (below) — **not an ADR, no code written, not built or
executed**. This machine is Windows-only for this session (no Linux dev/test environment available),
so unlike this pass's other two closed gaps (ADR-040, ADR-041 — both real code, built, tested,
175/175 green), this document stops at design + red-team. Per the same project-owner posture already
applied to OQ-19/OQ-20 (`OpenQuestions.md`): document what implementation needs, do not implement
without the ability to prove it. Seeds a future ADR once a Linux build/test environment is available
for this milestone. Companion: `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #10 (the
finding this document addresses).

## 0. Correcting the audit before designing against it

Gap #10's one-line recommended approach: *"Build pivot_root+bind-mount containment, but the cited
path-validation primitive is ADR-014's already-rejected, TOCTOU-vulnerable design — must use the
accepted open-then-verify primitive, fix a repeated-exec `rmdir` bug that breaks a currently-passing
test, and narrow the blanket `/usr` bind-mount."*

Re-grounded directly against current code (this pass, not carried over from the 2026-08-10 snapshot):

- **The current-state claim is accurate and not stale.** `src/backends/native_jail/
  linux_native_jail_backend.cpp` calls `clone()` with `CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWNS |
  CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD` and nothing else — no `mount()`, `pivot_root()`, or
  `chroot()` anywhere in the file. The header's own comment says so directly: `CLONE_NEWNS` gives the
  guest a *private copy* of the mount table, but nothing restricts which existing host paths remain
  visible inside it. `008-Sandbox-and-Isolation.md:437-449` states the same thing as current,
  non-stale spec text: zero filesystem/process containment on Linux `native-jail` today. This is
  confirmed independent of ADR-037's churn (ADR-037 only removed Quark; this backend predates and
  postdates that removal unchanged).
- **The "blanket `/usr` bind-mount" claim does not correspond to anything in this codebase and never
  did.** Grepped the whole tree (RFC 008, every ADR, `docs/research/*.md`) for `/usr`, `bind-mount`,
  `pivot_root`, `MS_BIND` — the only `/usr` references anywhere are `PATH=/usr/bin:/bin` environment
  strings (`linux_native_jail_backend.cpp:116`), not a bind mount. No design anywhere proposes
  bind-mounting `/usr`. This reads as generic forward-looking advice from whoever wrote the audit row
  (a typical minimal `pivot_root` jail needs *something* providing `/usr`/`/lib`/`/bin` for a working
  toolchain, and the row is pre-emptively warning not to make that mount too broad once it exists) —
  not a citation of a real bug. **Dropped from this design's scope as a "fix," kept as a design
  constraint below** (§2 step 2 addresses what actually gets mounted, deliberately narrow from the
  start rather than narrowed after the fact).
- **The `rmdir` bug is real but not yet a live bug** — it is a real *hazard this design would
  introduce if built naively*, not a currently-firing defect. `cgroup_limits.cpp:104`,
  `CgroupLimits::destroy_now()`: `::rmdir(path_.c_str())`, explicitly comment `// best-effort; a
  non-empty cgroup fails harmlessly (caller's bug)` — silently ignores failure.
  `tests/test_native_jail_teardown_cycles_linux.cpp` runs 300 create/exec/destroy cycles and asserts
  the delegated cgroup root's directory-entry count returns exactly to baseline — it currently passes
  *because there is nothing to leak yet* (no mounts exist). §2 step 5 below addresses this directly
  rather than carrying it forward as an open risk.
- **The accepted path-validation primitive already exists and is already proven**, just uncalled:
  `core/worktree_mount_fs_posix.hpp`/`.cpp`'s `open_within_mount_root` POSIX analogue (ADR-014 §9
  addendum, "Phase C4, Linux parity closed") — `open()` (which resolves symlinks transparently)
  followed by `readlink("/proc/self/fd/N")` to re-verify what the descriptor actually resolves to,
  proven in `tests/test_worktree_mount_fs_escape_corpus_linux.cpp` (21 checks). The rejected shape
  (ADR-014 §3 Design A, kept only as a permanent regression control under `redteam::`) is a lexical
  canonicalize-then-string-check-then-reopen — exactly what naive bind-mount-source validation would
  reach for if this primitive weren't reused. **Scoped honestly below (§2 step 1a)**: this primitive
  answers a guest-relative-path-to-safe-handle question; a `MountSpec`'s bind-mount *source* is host
  config, not guest input, so it does not need this check the way a guest `open()` call does — the
  citation matters only if some future caller lets a bind-mount source be even partially
  guest-influenced, which nothing in this design proposes.

## 1. What "parity with Windows" actually requires

RFC 008 §9 G3 already passes on Windows (`NativeJailBackend`/AppContainer genuinely restricts
`CreateToolhelp32Snapshot` process enumeration and unmounted-path reads). The Linux side needs the
same two properties, by different mechanisms:

1. **Filesystem**: the guest process's visible filesystem is restricted to exactly the mounts this
   sandbox instance was granted — nothing else on the host is reachable, not even by absolute path.
2. **Process visibility**: `/proc` inside the sandbox shows only processes in the guest's own PID
   namespace (already created via `CLONE_NEWPID`), not the host's real process list.

## 2. The design

All of this runs in the **child**, after `clone(CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWNS |
CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD)` returns there, and before `execve()` of the guest command —
matching where the existing seccomp-BPF filter installation and cgroup join already happen in this
file's real, current control flow.

1. **Make the new mount namespace's root private before touching anything else.** `mount(nullptr,
   "/", nullptr, MS_REC | MS_PRIVATE, nullptr)`. This is not optional and not obviously implied by
   `CLONE_NEWNS` alone — see §3 finding 1, the single most important correction this design's own
   red-team pass made to a naive first draft.
   1a. **Bind-mount sources are host config** (each granted `MountSpec::source`, decided by the
       calling C++ code from a real worktree/vendored-tree path), not guest-relative strings — so
       they are used directly as real filesystem paths, the same trust tier `mount_root_usage()`
       (the Windows quota-scan primitive, ADR-040) already treats its own `mount_root` argument as.
       `open_within_mount_root`'s POSIX analogue is the primitive to reach for only if some future
       change lets a bind-mount source be even partially guest-influenced; nothing here proposes that.
2. **Build the new root under a fresh, private, per-invocation directory** — not nested inside (or
   equal to) the per-invocation cgroup directory `cgroup_limits.cpp` already owns and `rmdir()`s at
   teardown (the exact collision that would trigger finding 3's `EBUSY` hazard if it happened). A
   `tmpfs` mount at that directory (`mount("tmpfs", jail_root, "tmpfs", 0, "size=...")`) gives a
   real, isolated mount point to pivot into, with no host-disk footprint to clean up beyond the
   mount itself.
3. **Bind-mount exactly the granted set, nothing implicit.** For each `MountSpec`: `mount(source,
   jail_root + guest_path, nullptr, MS_BIND, nullptr)`, then — for a read-only grant — a **required
   second call**, `mount(source, jail_root + guest_path, nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY,
   nullptr)`. A bind mount's initial `MS_BIND` call silently ignores `MS_RDONLY` passed in the same
   call (a well-documented Linux `mount(2)` behavior, not an edge case) — omitting the remount step
   would make every "read-only" grant actually writable, the identical "safe-looking flag that
   doesn't do what it says" shape this project's red-team passes keep finding elsewhere (finding 2).
   No implicit `/usr`/`/lib`/`/bin` mount of any kind — if a guest command needs a working toolchain
   beyond what an explicitly granted mount provides, that is its own, separate, explicitly-granted
   `MountSpec`, matching Windows's own vendored-interpreter-tree precedent (ADR-004 §3 step 1) rather
   than an implicit host passthrough.
4. **`pivot_root`, not `chroot`.** `chroot()` alone is escapable by a process holding an open file
   descriptor to a directory outside the new root before the call (`fchdir` after `chroot` reaches
   outside it) — a known, old class of container escape. `pivot_root()` fully detaches the old root
   from the mount tree, closing that class structurally:
   - `mount(jail_root, jail_root, nullptr, MS_BIND | MS_REC, nullptr)` (a directory must be a mount
     point for `pivot_root` to accept it as the new root — self-bind-mounting achieves that cheaply).
   - `mkdir(jail_root + "/.old_root", ...)`.
   - `chdir(jail_root)`, then `syscall(SYS_pivot_root, ".", ".old_root")`.
   - `chdir("/")` (now inside the new root).
   - `umount2("/.old_root", MNT_DETACH)` — lazily detaches the old root; nothing under it remains
     reachable from the guest, including anything the guest process might have an open fd into that
     was NOT explicitly re-opened after the pivot (an open fd to something under the old root stays
     valid for that process, matching ordinary Unix fd semantics, but nothing NEW is openable there).
   - `rmdir("/.old_root")` (now empty, safe to remove from inside the new root).
5. **Fresh `/proc`, not a bind-mounted one.** `mount("proc", jail_root + "/proc", "proc", 0,
   nullptr)`, done *after* the `pivot_root` above (mounting `procfs` fresh, from a process that is
   inside the new `CLONE_NEWPID` PID namespace, gives a namespace-local view by construction — this
   is the standard, documented way every real container runtime gets PID-namespace-scoped `/proc`;
   bind-mounting the host's existing `/proc` instead would keep showing the host's real process
   list, since a bind mount does not reinterpret `procfs`'s own namespace-awareness).
6. **Teardown ordering, addressing the `rmdir` hazard directly.** Because the jail root (step 2) is a
   dedicated `tmpfs` mount, separate from the cgroup directory, and because a Linux mount namespace
   is automatically and fully torn down by the kernel when the last process using it exits (no
   explicit per-bind-mount `umount()` needed from the host side once step 1's `MS_PRIVATE` prevents
   any leak into the host's own namespace), `CgroupLimits::destroy_now()`'s existing `rmdir()` on the
   *cgroup* directory is never touched by anything this design adds — the hazard the audit named
   only existed as a hypothetical for a naive implementation that reused the cgroup directory as the
   jail root, which step 2 rules out by construction, not by a defensive check. Still to be proven
   once a Linux environment is available (§4): the "wait for full process exit, including reaped
   zombie state, before treating the mount namespace as gone" ordering `test_native_jail_teardown_
   cycles_linux.cpp`'s existing 300-cycle assertion would need to keep passing under.

## 3. Self-red-team findings

**MUST-FIX 1 — mount propagation leak if step 1 (`MS_PRIVATE`) is skipped or misordered.** Modern
Linux (any systemd-managed host, which is most realistic deployment targets) marks the root mount as
`MS_SHARED` by default. A new mount namespace created by `CLONE_NEWNS` inherits that propagation
setting unless explicitly changed — meaning every bind mount and the `pivot_root` call itself in
steps 2-5 would propagate back into the **host's own mount namespace** if step 1 is skipped, run
after any mount already happened, or is non-recursive (`MS_PRIVATE` without `MS_REC` only affects the
root mount point itself, not mount points nested under other filesystems already mounted there). This
is not a sandbox-escape (`MS_SHARED` propagation is child-to-parent for this direction, not
parent-to-child), but it is a real, cumulative host-visible-mount-pollution bug: every sandboxed
invocation would leave real mount entries in the host's own `/proc/mounts` that never get cleaned up
by the ordinary per-process mount-namespace teardown the design otherwise relies on (§2 step 6),
because a `MS_SHARED`-propagated mount is *also* the host's own mount, not just the child's — exactly
the kind of "the assumed automatic cleanup mechanism doesn't actually apply here" failure this
project's red-team passes exist to catch before it ships. **Step 1 must run first, unconditionally,
before any other mount/pivot_root call in this whole sequence** — stated as an ordering invariant, not
merely listed as step 1.

**MUST-FIX 2 — the two-step read-only bind mount.** Already folded into §2 step 3 above, called out
again here because it is the single easiest line to get wrong: `mount(src, dst, nullptr, MS_BIND |
MS_RDONLY, nullptr)` in one call silently succeeds and is silently **writable** — `MS_RDONLY` is
ignored on the initial bind. Any implementation of this design must be reviewed specifically for
whether it performs the required *second*, `MS_REMOUNT`-flagged call, and the future proof phase (§4)
must include a positive-control test that writes through a nominally-read-only granted mount and
fails only if the remount step is genuinely present — matching this project's own "a gate without a
positive control proves nothing" discipline (022 §5, and the pattern already used for every case in
`test_native_jail_abuse_corpus_windows.cpp`).

**REAL RISK, not fixed by this design — `/proc` mount races against concurrent teardown.** Step 5's
fresh `/proc` mount happens inside the child, which is racing against nothing else *inside* that
process, but the host-side `wait_or_kill`/cgroup-teardown path (whatever the future Linux equivalent
of `job_object_limits.cpp`'s destructor pattern turns out to be) must not attempt any cleanup of the
child's cgroup or process-tree state until the child has either called `execve()` successfully or
exited — an early-teardown race while the child is mid-setup (between `clone()` returning and the
`pivot_root` completing) could interact with the seccomp filter or cgroup join in ways this design
does not analyze, because it is a general lifecycle-ordering question the existing `create()`/`exec()`
split already has to answer today, independent of anything this design adds. **Named as an open
question for whoever implements this, not answered here.**

**Not a finding, a scope check: does this design's mount-namespace containment interact with the
already-installed seccomp-BPF filter or cgroup limits?** No new interaction found. `seccomp_filter.hpp`
filters syscalls by number/argument, not by mount-namespace state; `cgroup_limits.hpp`'s resource
accounting is PID/cgroup-membership-based, unaffected by what the process's mount table looks like.
This design is additive to both, not entangled with either.

## 4. What proving this would need (not attempted here)

- A Linux build/test environment (this session has none) — `cmake`/`ninja`/a real Linux kernel with
  namespace support, matching this project's existing `test_native_jail_backend_linux.cpp`/
  `test_native_jail_teardown_cycles_linux.cpp` harness shape.
- A Linux analogue of `test_native_jail_abuse_corpus_windows.cpp`'s Case 4 (fs-escape) — an ungranted
  host path (e.g. `/etc/shadow` or a test-created secret file outside any `MountSpec`) must be
  unreadable from inside the jail, with a positive control proving the *same* mechanism can also
  allow a granted mount (matching this project's own positive-control discipline throughout).
- A read-only-bind-mount positive/negative control pair (MUST-FIX 2).
- A `/proc`-namespace-locality check: `ps`/`readdir("/proc")` inside the jail must show only the
  guest's own process(es), not the host's real process table — the Linux analogue of Windows's
  already-proven `CreateToolhelp32Snapshot` restriction (008 §9 G3).
- Re-running `test_native_jail_teardown_cycles_linux.cpp`'s existing 300-cycle assertion against the
  new mount/pivot_root code path, to confirm §2 step 6's ordering claim holds under repetition, not
  merely once.
- A check for MUST-FIX 1 specifically: after N sandbox creations and teardowns, `/proc/mounts` on the
  **host** must show no leaked entries from any of them — the concrete, falsifiable form of "the
  `MS_PRIVATE` step actually prevented propagation," not merely "the design says it should."

## 5. What this document does not claim

No code exists. No claim that this design is complete once written — MUST-FIX 1 and 2 are corrections
to what would otherwise have been the natural first-draft implementation, not residual risks in a
finished one; a future implementer should treat §2 as already including both fixes, not as a base to
apply them to. Not a decision about `disk_bytes`/`net_bytes`/`fds` enforcement (Linux `native-jail`'s
resource-limit story is `cgroup_limits.hpp`'s job, already separate from this filesystem/process
containment question, same separation the Windows `native-jail` backend already has between
`app_container_profile.hpp` and `job_object_limits.hpp`). Not a decision about whether Linux
`native-jail` promotion (008 §9 G1-G8) is otherwise ready — this closes only the filesystem/process
containment gates (G2/G3's Linux half), not the whole gate set.
