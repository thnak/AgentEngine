# ADR-140 — `KataBackend`'s SLICE 12 mount-point pre-creation gets a real host-side symlink-escape guard

- **Status:** Proposed — implemented (Linux-only file, `if(NOT WIN32 AND AGENTENGINE_BUILD_KATA_
  BACKEND)` gated, so this pass could not compile or execute it on this Windows session).
  **Independent red-team round (§7): full static trace, clean bill of health, no bug found.** Real
  compilation/execution verification deferred to the Linux-verify pass this ADR names as still
  outstanding.
- **Date:** 2026-08-30/31.
- **Scope:** `src/backends/kata/kata_backend.cpp` only (the three SLICE 12 mount-point pre-creation
  call sites inside `create()`).
- **Related specs:** `decisions/ADR-109-kata-backend-live-deployment.md` (SLICE 12's own origin, the
  read-only-rootfs/missing-mount-point fix this pre-creation logic implements),
  `src/backends/native_jail/real_filesystem_adapter.cpp` (the sibling symlink-escape guard this
  mirrors in shape).

## 1. The question

A final-review pass found that SLICE 12's mount-point pre-creation loops (`fs::create_directories()`
for the fixed `/proc,/dev,/sys,...` set and every caller-supplied `MountSpec::guest_path`; an
`std::ofstream` for `/etc/hosts`) resolve paths against `rootfs_dir`, whose LOWER layer is `image_` — an
arbitrary OCI registry reference this file's own `/etc/hosts` block comment already calls "not
trusted-by-construction." `fs::create_directories`/`ofstream` follow symlinks by default. A malicious
image shipping, say, `/etc` as a symlink to an absolute host path would make these calls create
directories or write file content OUTSIDE `rootfs_dir`, on the real host filesystem, at whatever
privilege this process runs under. The existing lexical `.`/`..` rejection on caller-supplied
`guest_path` does not cover this — a symlink is not a literal `..` path component. Should this be
closed the same way `real_filesystem_adapter.cpp`'s own sibling symlink-escape guard already closes the
identical bug class for `LinuxNativeJailBackend`?

## 2. Findings

Yes, and the existing sibling's own "walk to the longest existing ancestor, canonicalize it, verify
containment" shape applies directly, simplified: this file's own case needs no case-folding/UNC
handling (Linux paths are case-sensitive, no drive letters), so a smaller, self-contained lambda
suffices rather than reusing the sibling's own case-folding-heavy implementation verbatim.

## 3. What was built

A new `escapes_rootfs_via_symlink(target)` lambda, called before each of the three
`create_directories`/`ofstream` call sites: walks `target` up to its longest EXISTING ancestor,
canonicalizes that ancestor (`fs::canonical`, which follows symlinks), and verifies via
`lexically_relative()` that it is still contained within a once-computed `rootfs_canonical`. If nothing
along the path exists yet, it is trivially safe (nothing to escape through — `create_directories` will
build real, non-symlink directories). On escape, `create()` fails closed via the existing
`cleanup_partial()` + `std::unexpected` pattern already used by every other error path in this
function, with a new `kata_backend.mount_point_escape` code.

## 4. Verification

**Not compiled or executed this pass** — `src/backends/kata/kata_backend.cpp` is gated
`if(NOT WIN32 AND AGENTENGINE_BUILD_KATA_BACKEND)` and does not build on this Windows session, and this
session has no live Kata/containerd deployment to exercise `create()` against even on Linux. Verified
by hand instead (see §7 for the full account): the ancestor-walk loop's termination, `fs::canonical`
error-handling fail-closed behavior, `lexically_relative()`'s correctness against a sibling-prefix
false-positive/negative (`/tmp/rootfs` vs `/tmp/rootfs2`), and confirmation the guard is actually
called at all three real call sites (no missed site).

## 5. Not done

- No live Kata/containerd deployment available to exercise `create()` against a real malicious image
  this pass — the guard's logic is verified by hand-trace only, not by an actual attack reproduction.
- No explicit TOCTOU defense between `escapes_rootfs_via_symlink()`'s own check and the subsequent
  `create_directories`/`ofstream` call — see §7's own note on this; judged low real risk (a statically
  extracted image layer, not a live racing process) but not yet stated as an explicit, disclosed
  residual the way ADR-138's own POSIX fix states its own narrower TOCTOU gap.

## 6. Residuals

- **Not yet compiled or run against a real Kata Containers deployment or a real malicious OCI image**
  — a dedicated Linux-verify pass (with a live Kata/containerd environment, which this session's own
  established WSL2 verification environment has historically lacked full reach into per ADR-109/110's
  own disclosed residuals) should specifically: (a) confirm the file compiles clean on GCC, (b) if a
  live deployment is reachable, build a concrete repro (an image with `/etc` shipped as a symlink to an
  absolute host path) and confirm `create()` fails closed with `kata_backend.mount_point_escape` rather
  than writing outside `rootfs_dir`.
- The disclosed, low-risk TOCTOU gap named in §5.

## 7. Independent red-team round (same day, this session's own consolidated final-review pass)

**Static-only review (execution not possible on this platform for this file) traced the fix fully —
clean bill of health, no bug found.**

Confirmed by hand: the ancestor-walk loop is bounded (terminates at a filesystem root, which always
exists, so the loop cannot run unbounded even for a `target` with zero existing ancestors); `fs::
canonical`'s own error path (`walk_ec`) fails closed (treated as an escape, not silently ignored);
`lexically_relative()`'s emptiness/`..`-prefix check correctly rejects the naive-substring class of bug
(`/tmp/rootfs` vs `/tmp/rootfs2` sibling directories are correctly distinguished, since
`lexically_relative` is component-based, not a string prefix compare); all three real call sites (the
fixed directory list, the `spec.mounts` loop, and the `/etc`+`/etc/hosts` block) are confirmed guarded
before their respective `create_directories`/`ofstream` calls; the one `ofstream` NOT guarded by this
function (`workdir/"hosts"`, used for the network-namespace hosts file, a few lines below the SLICE 12
block) is correctly out of scope, since `workdir` is never under the untrusted `rootfs_dir` at all.

One minor gap noted, not fixed this pass: unlike ADR-138's own POSIX fix, this guard's own comment does
not explicitly disclose the check-then-act TOCTOU window between `escapes_rootfs_via_symlink()`'s own
check and the actual `create_directories`/`ofstream` call that follows it — judged low real risk (a
statically extracted OCI image layer is not a live process racing this exact window the way a
concurrent attacker process would be) but recorded here, in §5/§6, as an explicit residual rather than
left silently undisclosed, matching this session's own established honesty convention for narrower
theoretical gaps.

Explicit Linux-execution needs, named for the dedicated verify pass per this ADR's own §6: confirm
`fs::canonical`'s symlink-following behavior and `lexically_relative`'s containment-check reasoning
hold identically on the real target kernel/filesystem, and, if a live Kata/containerd deployment is
reachable in that environment, attempt the concrete malicious-image repro named in §6.
