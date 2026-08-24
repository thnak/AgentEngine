# Design draft — KataBackend Slice 11: `pids`/`disk_bytes`/`net_bytes` via the `--config` pipeline

**Status:** design → independent red-team → fix complete, 2026-08-24. Ready for implementation. No
live Kata/containerd deployment is available this session (host preparation is scheduled separately)
— implementation will be compile-verify only, same disclosed posture every prior Kata slice already
carries, until a real deployment exists to prove against. Project-owner direction (2026-08-24): a
full redesign/rewrite of `KataBackend::create()`'s resource-limit path is authorized (not a patch) to
close the three remaining `ResourceLimits` residuals this backend still leaves unenforced.

**Red-team pass** (fresh reviewer, no prior session context, run against this exact draft before any
implementation): 8 findings, 5 BLOCKING (including one real I2-relevant gap: a pre-existing,
capability-independent mount-validation opt-out combined with this draft's own new loop-mounted file
to produce a live host-filesystem-corruption path), all fixed in this draft before landing — see §4,
§5, §5a', §5b, §5c, §6, §7, §10 for the findings and fixes inline at the sections they concern. See
§11 for the pass's full finding list. Verdict: the overall mechanisms (`--config`-mode `pids`, the
cross-chain `nft` quota, the loop-backed writable overlay) are sound; this was a fix-and-re-review,
not a structural redesign.

**Relates to:** `008-Sandbox-and-Isolation.md` §1b/§2, `decisions/ADR-090-kata-backend-pids-limit-investigated-and-deferred.md` (reopened by this draft — its own negative finding is superseded, not
wrong), `decisions/ADR-092-kata-backend-disk-net-bytes-investigated-and-deferred.md` (reopened,
`net_bytes` moot-because-deny_all reasoning is stale since ADR-093/Slice 10; `disk_bytes`
interpreter-mediation reasoning is correct but this draft proposes a **different**, backend-level
mechanism 008 §2's own resource-cap axis already permits alongside mediation, not a replacement for
it), `decisions/ADR-093-kata-backend-netpolicy-allowlist-config-cni.md` (the `--config`/OCI-spec
pipeline and per-instance netns/nftables machinery this draft extends, not replaces).

## 1. What changed since ADR-090/ADR-092's negative decisions

Both ADRs correctly found `--config` mode blocked by a rootfs-preparation gap (`pids`) or ruled the
whole approach out of scope (`disk_bytes`/`net_bytes` as interpreter-mediation-only). **ADR-093
already closed the rootfs-prep blocker** for an unrelated reason (`ctr images mount` was the missing
piece, not `ctr snapshot prepare`), and already built a per-instance network namespace with a real
nftables chain. Both premises this draft revisits are therefore stale, not wrong at the time they
were written:

- `pids`: `create()` already emits a hand-authored OCI runtime-spec JSON (`build_oci_spec_json()`,
  `kata_backend.cpp`) with a `linux.resources` object. Adding `pids.limit` is now a same-shape
  addition to a mechanism that already exists for `memory`.
- `net_bytes`: Slice 10 already builds a real per-instance `nft` chain in a real per-instance netns.
  A byte quota is a native `nft` primitive attachable to that SAME chain, entirely host-side — 008
  §1b's "interpreter-level mediation" framing (which correctly says this backend has no guest-side
  attachment point for `open`/`socket` mediation) does not apply here, because this mechanism never
  touches guest code at all; it counts bytes crossing a host-owned netns boundary the guest cannot
  observe or evade, the same trust model `NetPolicy`'s own destination-allowlist already relies on.
- `disk_bytes`: still correctly assessed as unreachable via `ctr images mount`'s own writable path —
  but a **backend-owned, loop-device-backed writable layer with a real ext4 quota** is a distinct
  mechanism `ctr` was never asked to provide, not a retry of the same blocked approach.

## 2. A real, previously-undisclosed finding this draft surfaced: `create()`'s rootfs may not be
   writable today at all

Fetched and read `cmd/ctr/commands/images/mount.go` (containerd, `main` branch) this pass:

```go
if cliContext.Bool("rw") {
    mounts, err = s.Prepare(ctx, key, chainID)
} else {
    mounts, err = s.View(ctx, key, chainID)
}
```

`KataBackend::create()`'s existing call (`kata_backend.cpp`, unchanged since Slice 9) is:

```cpp
run_ctr({"ctr", "images", "mount", image_, rootfs_dir.string()});
```

**No `--rw` flag.** Per the source above, this takes the `View()` branch — a read-only snapshot
view, not `Prepare()`'s writable one. `build_oci_spec_json()` nonetheless sets `root.readonly =
false` (`kata_backend.cpp:388`), i.e. the OCI spec **claims** a writable rootfs while the actual
mount backing it may not be one. This is disclosed here as a real, plausible, **not experimentally
confirmed** defect (no live deployment reachable to prove which way `View()`'s mounts actually
behave for the configured snapshotter) — the same "assumed, not verified" posture every `ctr` CLI
claim in this file already carries, named explicitly rather than silently inherited.

**Scope decision for this draft**: the `disk_bytes == 0` (unset) path is left **byte-for-byte
unchanged** — same single `ctr images mount` call, same possibly-read-only rootfs, same undisclosed
behavior a caller gets today. This finding is named as a separate, pre-existing gap for the
project-owner to decide whether to fix independently of `disk_bytes` — bundling an unrelated
behavior change into this pass for every caller (not just those requesting a disk quota) would widen
blast radius beyond what was authorized. Only the `disk_bytes > 0` path (a caller who opted in)
touches this at all, and it does so by construction (see §5) rather than by flipping `--rw` on the
existing call.

## 3. `pids` — trivial addition to the existing OCI resources object

`build_oci_spec_json()`'s `resources_members` already conditionally adds a `memory` member when
`in.memory_bytes > 0`. Add the identical shape for `pids`:

```cpp
if (in.pids > 0) {
    resources_members.emplace_back(
        "pids", Value::make_object({{"limit", Value::make_number(static_cast<double>(in.pids))}}));
}
```

`OciSpecInputs` gains a `std::uint32_t pids` member, populated from `spec.limits.pids` at the same
call site `memory_bytes`/`fds` already are (`create()`'s `OciSpecInputs` construction). No new
deployment precondition, no new cleanup path, no new failure mode beyond what `memory_bytes` already
has (a limit rejected by the guest kernel surfaces as a nonzero `ctr run` exit, already handled by
the existing `create_failed` path). **Lowest-risk change in this draft.**

Whether `linux.resources.pids.limit` is honored by `kata-agent` inside the guest the way it is by
runc on a shared host kernel is **not independently verified against a live deployment this
session** — disclosed, not assumed, matching every other resource-limit claim this file already
carries (`fds`' own SLICE 7 disclosure is the direct precedent).

## 4. `net_bytes` — an `nft` quota object shared across the netns's egress AND ingress chains

**Open design question this draft resolves, flagged explicitly for red-team scrutiny**: does
`net_bytes` mean egress-only bytes, or total bidirectional bytes? `ResourceLimits::net_bytes`'s own
doc comment (`sandbox.hpp`) does not say. This draft reads it as **total bidirectional** — the
intuitive reading of "network byte budget," and the more conservative (harder-to-evade) one: a
workload that receives a large response but sends almost nothing would not be captured by an
egress-only counter, and a large download is exactly the kind of host-resource consumption a byte
budget exists to bound.

Netfilter's `output` hook (Slice 10's existing chain) only sees packets **originating** in the netns
— it does not see inbound response traffic, which traverses the `input` hook instead. A single
`output`-only quota therefore under-counts. Fix: a **named quota object**, shared by a rule on each
of two chains in the same `inet ae_netpolicy` table:

```
nft add table inet ae_netpolicy                      # unchanged
nft add chain inet ae_netpolicy output { type filter hook output priority 0; policy drop; }
nft add chain inet ae_netpolicy input  { type filter hook input  priority 0; policy accept; }
nft add quota inet ae_netpolicy ae_net_budget { over <net_bytes> bytes }

nft add rule inet ae_netpolicy output oif lo accept
nft add rule inet ae_netpolicy input  iif lo accept
nft add rule inet ae_netpolicy output quota name ae_net_budget drop
nft add rule inet ae_netpolicy input  quota name ae_net_budget drop
nft add rule inet ae_netpolicy output ip daddr <ip> tcp dport <port> accept   # per allowlist entry, unchanged shape
```

**Red-team finding (BLOCKING), fixed before landing**: the first draft of this section wrote the
quota-add command as `nft add quota inet ae_netpolicy ae_net_budget over <net_bytes> bytes`, with no
`{ }` around the quota spec. Checked against `nft(8)`'s own grammar (`add quota [family] table name {
[over|until] bytes BYTE_UNIT ... }` — braces are mandatory even in single-line CLI form, confirmed
against the man page's own worked example): this is a syntax error, `nft` would reject it, the
`create()` call correctly fails closed via the existing `nft_setup_failed` path (not a security
bypass), but it means `net_bytes` would ship **completely non-functional** in every real deployment
— and the exact command this draft's own §9 test plan proposed to assert argv-by-argv would have
made that failure invisible (the assertion would pass while asserting the broken command). Fixed:
`{"...", "nft", "add", "quota", "inet", "ae_netpolicy", "ae_net_budget", "{", "over",
<net_bytes-as-string>, "bytes", "}"}` as separate argv tokens — matching this file's own established
convention for chain definitions (`kata_backend.cpp:697-699` already tokenizes `"{"`/`";"`/`"}"` as
separate argv entries for exactly this reason). The quota-reference rules (`quota name
ae_net_budget drop`) need no braces — confirmed against the same grammar, only the quota *definition*
requires them.

**Ordering matters and is the actual mechanism, not a style choice**: nftables evaluates rules
top-to-bottom per chain and stops at the first terminal verdict (`accept`/`drop`). The quota-drop
rule MUST be added before the per-destination `accept` rules in the `output` chain — `quota over`
only matches once the shared counter has already exceeded the budget, so while under budget it
simply does not match and falls through to the real allowlist rules below it; once exceeded, it
matches and terminates the chain with `drop` before the allowlist rule below is ever reached.
Independently verified against the nftables wiki (not just asserted): a named quota object is
table-scoped and referenceable by name from rules in different chains of the same table, and its
counter accumulates for every packet a `quota name` statement evaluates against regardless of
match/no-match — the cross-chain, shared-budget design in this section is real, not merely assumed.

**Correction (the first draft overclaimed the `input` chain's safety here)**: an `input` chain with
policy `accept` and no source/connection-tracking filter is **not** structurally limited to "a
response to a connection `output` already permitted" — it accepts any inbound packet reaching the
guest's netns interface from anywhere on the CNI network, subject only to the shared byte quota.
This is **not a regression this draft introduces** — Slice 10 today registers no `input` chain at
all, so inbound traffic is already completely unfiltered by netfilter in the shipped backend — but
the first draft's "by construction" language asserted a guarantee this mechanism does not actually
provide, which a future reader could wrongly rely on when reasoning about `NetPolicy`'s scope. Stated
correctly: `input`'s only job here is feeding the shared byte counter; it enforces no destination or
source restriction of its own, before or after this draft, and this draft does not change that
pre-existing exposure — closing it (a `ct state established,related` rule, or a real source
allowlist) is a separate, out-of-scope follow-on, not silently assumed solved by this section.

This is entirely host-side, inside the netns Slice 10 already creates — zero guest cooperation.
**New deployment precondition**, named for consistency with §5a's own disclosure standard (the first
draft omitted this): `nft`'s named-quota-object support requires a kernel/nftables build with quota
support (`nft_quota`) — a real, usually-present-by-default bar beyond Slice 10's own
`table`/`chain`/`rule ... accept` usage, which needs no such module. `net_bytes == 0` (unset) skips
both new lines (`in.net_bytes > 0` guard, mirroring every other optional-limit field in this file) —
byte-for-byte unchanged for a caller that doesn't opt in, including callers who *do* request network
access via `deny_all=false`/`allowlist` but never set `net_bytes` (Slice 10's existing behavior,
unaffected).

**Disclosed residual**: `quota over` is a monotonically-accumulating counter for the lifetime of the
`nft` table (i.e. for the lifetime of the netns/container, since the table is destroyed with the
netns) — there is no per-`exec()`-call reset. This matches `ResourceLimits::net_bytes`'s own most
natural reading (a budget for the whole sandboxed session, `sandbox_lifetime`, not per individual
`exec()` call) and is the same lifetime scope `disk_bytes` below uses, but is named explicitly since
`wall_ms`/`output_bytes` are the opposite (per-call) — this file's three enforced-limit axes do not
all share one temporal scope, and a reader should not assume they do.

## 5. `disk_bytes` — a backend-owned, loop-device-backed, size-bounded writable overlay layer

**Mechanism** (precedented: Docker's own historical `overlay2 --storage-opt size=` feature and a
long-standing community pattern for size-bounding a writable layer without depending on an
XFS-project-quota-mounted host filesystem — a real host-configuration precondition this backend
cannot assume, unlike a self-contained loop-mounted image file):

1. `create()` still calls `ctr images mount <image> <lower_dir>` (View, read-only, **exactly** as
   today's single call semantically was) to obtain the image's read-only content. **Red-team finding
   (BLOCKING), fixed**: the first draft left `rootfs_dir` playing double duty — the `ctr images
   mount` target AND the final overlay mountpoint — with no field recording `lower_dir` separately.
   `destroy()`'s existing `ctr images unmount <rootfs_dir>` call would then target a path that was
   NEVER the actual `ctr images mount` target once the overlay exists at that same path, silently
   failing to release the real snapshot mount/lease — a containerd snapshot leaked on every
   `disk_bytes > 0` create/destroy cycle. Fixed: `lower_dir` is now a **distinct** path
   (`<workdir>/lower`, never equal to `rootfs_dir`) for every instance, `disk_bytes` set or not (see
   §6) — `destroy()`/`cleanup_partial()` always unmount `lower_dir`, never `rootfs_dir`, closing the
   gap this draft's own §2 disclosure did not previously separate. When `disk_bytes == 0`, `rootfs_dir`
   is a bind-mount alias of `lower_dir` (`mount --bind <lower_dir> <rootfs_dir>`) rather than the same
   path reused twice — a one-line addition that keeps the "byte-for-byte unchanged behavior" promise
   (§2/§3) while giving `destroy()` a single, uniform teardown shape regardless of whether the
   `disk_bytes > 0` branch ever ran for a given instance.
2. When `spec.limits.disk_bytes > 0`, additionally:
   - `fallocate -l <disk_bytes> <workdir>/upper.img` — **correction (the first draft
     mischaracterized this as lazy/sparse)**: verified against `fallocate(2)` and ext4's own behavior,
     `fallocate -l SIZE FILE` (no `--keep-size`) is an **eager** reservation — ext4 marks the extents
     `EXT_UNWRITTEN` and reflects the space as consumed in free-space accounting immediately; only the
     *content* write is deferred, not the *reservation*. `truncate -s` is the actual lazy/sparse
     primitive, not this. This draft deliberately keeps `fallocate -l` (eager), not `truncate -s`,
     stated as a considered choice now rather than an accidental one: eager reservation is the safer
     multi-tenant behavior — N concurrent `disk_bytes`-requesting instances immediately consume `N ×
     disk_bytes` of real host disk at `create()` time, so an operator sizing a host around this
     backend's own declared budgets gets an honest, immediate `ENOSPC` at `create()` time if
     oversubscribed, rather than a later, harder-to-attribute `ENOSPC` once several lazily-allocated
     quotas collide inside the guest. `disk_bytes` therefore behaves as a real, host-disk-reserving
     admission cost at `create()` time, not merely a guest-visible ceiling — named explicitly since a
     caller/operator reading `ResourceLimits::disk_bytes` as "only consumed as used" would be wrong.
   - `mkfs.ext4 -q <workdir>/upper.img` — format it. ext4, not a faster/simpler filesystem: the
     writable rootfs layer needs POSIX permissions/symlinks/hardlinks/special files, which a
     workload's own `mkfs.ext4`-formatted layer supports and a FAT-family filesystem does not.
   - `losetup -f --show <workdir>/upper.img` — attach a free loop device, **capturing stdout** to
     learn which `/dev/loopN` was assigned (a genuinely new pattern in this file — see §5b for the
     concurrency fix this needed after red-team).
   - `mount <loop_dev> <workdir>/quota_root` — mount the loop-backed ext4 filesystem.
   - `mkdir <workdir>/quota_root/upper <workdir>/quota_root/work` — overlayfs requires `upperdir`
     and `workdir` as two empty directories on the **same** filesystem (kernel requirement, not this
     design's choice) — both must live inside the just-mounted loop filesystem, not beside it.
   - `mount -t overlay overlay -o lowerdir=<lower_dir>,upperdir=<workdir>/quota_root/upper,workdir=<workdir>/quota_root/work <rootfs_dir>`
     — the actual rootfs the OCI spec's `root.path` points at is now a REAL overlay: read-only image
     content from `lower_dir`, writable content capped by the ext4 filesystem's own real size.
3. `disk_bytes == 0` (unset): `rootfs_dir` is a plain bind-mount of `lower_dir` (§ point 1's fix) —
   no loop device, no ext4, no overlay; behaviorally identical to today's single-mount rootfs.

**Enforcement is real, kernel-level, not a heuristic**: once the guest's writes fill the
loop-mounted ext4 filesystem, the guest's own `write()`/`open(O_CREAT)` calls get `ENOSPC` from the
guest kernel exactly as they would on any full disk — the same class of guaranteed, unconditional
enforcement `memory_bytes`already gets from the guest kernel's own OOM path, not a probabilistic or
best-effort signal.

**Disclosed, not oversold**: ext4 reserves filesystem metadata overhead (inode tables, the
default ~5% root-reserved-blocks percentage) — actual usable space is somewhat below the literal
`disk_bytes` value, not exactly equal to it. This draft does not propose compensating for the
overhead (e.g. sizing the image slightly larger than `disk_bytes`) — a caller requesting an exact
byte-for-byte budget does not exist in this codebase today, and silently padding the size would make
`disk_bytes` mean something other than what it says.

### 5a'. `MountSpec` host-path exclusion against the backend's own workdir — closes a real,
   capability-independent host-corruption path (red-team finding, BLOCKING, fixed before landing)

**Finding**: `authorize_spec()` (`sandbox.hpp:210-230`) only validates mount paths when the caller
holds at least one `cap::SandboxMount` grant — its own documented "opt-out preserved" shape means a
caller holding **zero** `SandboxMount` grants at all skips that check entirely
(`!mount_grants.empty()` guard, `sandbox.hpp:211-212`). `KataBackend::create()`'s own mount
validation (`kata_backend.cpp:514-530`, unchanged by earlier slices) only rejects a `BlobRef` source
and a literal `,` — nothing stops a caller supplying, say, `MountSpec{source="/run/agentengine-kata",
guest_path="/hostrun", read_write=true}` with **no** `SandboxMount` grant at all. Before this draft
that was inert (worst case, the guest could see/tamper with this backend's own bookkeeping
directory). Once `disk_bytes > 0` exists, `<workdir>/upper.img` is the **live backing file of an
actively loop-mounted, actively overlay-mounted** ext4 filesystem. A guest process writing through
such a bind mount straight into `upper.img` bypasses the loop/block layer and ext4's own I/O path
entirely — a well-known way to corrupt a filesystem that is concurrently mounted through a different
path (the loop driver's and ext4's own page-cache state go incoherent against the out-of-band write)
— a materially worse outcome than merely exceeding the quota, reachable with **zero capability
grants**, a real I2-relevant gap §7's original checklist incorrectly asserted did not exist.

**Fix**: `create()` gains an unconditional check, independent of `authorize_spec()`/`cap::SandboxMount`
entirely (the backend's own workdir was never meant to be nameable by a caller at all, regardless of
what the caller's capability set holds) — reject any `MountSpec` whose host path is equal to,
is an ancestor of, or is contained within `/run/agentengine-kata` outright
(`kata_backend.workdir_mount_forbidden`), before any resource acquisition begins, using the same
path-component-lexical check `sandbox_detail::has_dot_or_dotdot_component`/this backend's own `,`
check already establish as this file's idiom for "reject the injection surface outright" rather than
attempt a runtime containment fix.

### 5b. Loop-device allocation under concurrency (red-team finding, BLOCKING/contract, resolved
   rather than left open)

`losetup -f --show` uses the kernel's `LOOP_CTL_GET_FREE` to find a candidate minor, then
`LOOP_SET_FD` to bind it — the bind step is exclusive per-device, so two concurrent `create()` calls
racing for the "same" nominally-free device cannot both actually attach to it (one gets `EBUSY`) —
**confirmed not a silent-corruption/double-use risk**. It IS a real gap against this project's own
documented contract, though: `sandbox_backend_registry.hpp`'s own thread-safety comment requires a
registered backend to "tolerate concurrent create/exec/destroy calls from unrelated sessions" — a
design that lets a purely load-driven race spuriously **fail an entirely unrelated caller's**
`create()` does not "tolerate" that race, it just fails closed instead of corrupting state, which is
necessary but not sufficient. **Resolved** (not left as an open question for a later reader): the
`fallocate`→`mkfs.ext4`→`losetup -f --show`→`mount` sequence for a single instance's disk-quota setup
is serialized behind a `std::mutex` private to `KataBackend` (a new member, alongside `instances_`) —
these four steps are short relative to VM boot time (milliseconds to low tens of milliseconds even
under contention), so serializing just this slice of `create()` (not the whole method, and never
`exec()`/`destroy()`, which never touch loop-device allocation) costs negligible throughput while
making the "tolerate concurrent calls" contract actually true rather than merely usually-true. A
retry-on-`EBUSY` loop was considered and rejected: it depends on correctly classifying `losetup`'s
own error text/exit code as "device was raced," which is a weaker, more fragile guarantee than simply
not racing in the first place.

### 5c. Cleanup ordering — `destroy()` and `create()`'s own failure-path unwind

Six new/changed host resources are acquired, in this order, only when `disk_bytes > 0`: `lower_dir`
mount (§5 point 1, now always separate from `rootfs_dir`) → sparse file → loop device → loop mount →
overlay mount. Teardown MUST run in exact reverse order — an early `losetup -d` while the overlay
mount is still active would either fail (device busy) or, worse on some kernels, silently detach a
device still backing a live mount, corrupting or wedging that mount:

```
umount <rootfs_dir>              # overlay mount, must go first (guest process is already dead by
                                  # this point in destroy()'s existing sequence, see below)
umount <workdir>/quota_root      # the loop-backed ext4 mount
losetup -d <loop_dev>            # detach the loop device (recorded on the Instance at create() time,
                                  # NOT re-discovered by scanning losetup -a at destroy() time -- a
                                  # concurrent instance's own loop device must never be guessed at)
ctr images unmount <lower_dir>   # the real snapshot mount/lease this instance actually holds --
                                  # NEVER <rootfs_dir> (see §5 point 1's fix)
rm -rf <workdir>                 # sparse file, mount points, and the workdir itself
```

**Red-team finding (BLOCKING), fixed**: the first draft's `cleanup_partial()` sketch simply appended
these new steps to the EXISTING lambda without checking that lambda's own current step order. Read
verbatim from the shipped code (`kata_backend.cpp:615-625`), the existing `cleanup_partial()` already
runs `fs::remove_all(workdir, ec)` **before** `ctr images unmount` — i.e. remove-then-unmount, the
opposite of the reverse-acquisition-order rule this section itself states. For the read-only
`disk_bytes == 0` path that was latent-but-mostly-harmless (removing files under a read-only view
mount just yields per-file `EROFS`, silently swallowed by `remove_all`'s error-code overload). Once
`rootfs_dir`/`<workdir>/quota_root` can be **live writable mounts**, the same call would recurse into
them (`std::filesystem::remove_all` has no "stay on one filesystem" guard, unlike `rm
--one-file-system`) and either delete real guest-written content or hit `EBUSY` trying to remove a
still-mounted directory, before the new unmount/detach steps below it in a naively-appended lambda
ever ran. **Fixed**: `cleanup_partial()` is reordered so every unmount/detach (overlay → loop-ext4 →
`losetup -d` → `ctr images unmount <lower_dir>`) happens strictly before `fs::remove_all(workdir)`,
not merely before whatever new lines get appended after the pre-existing (wrongly-ordered)
`remove_all` call.

**`destroy()`'s existing step ordering already kills the task/container BEFORE this file's own
`ctr images unmount` call** — the new overlay/loop teardown above is inserted at that same point
(after task kill, before the `lower_dir` unmount), since a live guest process still has the overlay
mount open via virtiofs and unmounting underneath it would be the same class of "unmount while still
referenced" hazard named above, one layer up the stack (host virtiofs share → guest, not host mount →
host mount, but the same ordering discipline applies). `destroy()`'s own existing sequence already
places `fs::remove_all` AFTER its unmount call (unlike `cleanup_partial()`'s pre-existing bug above) —
confirmed by re-reading `kata_backend.cpp:890-899` — so `destroy()` itself needs the new teardown
steps inserted in the right place, not reordered.

**`create()`'s own failure-path (`cleanup_partial`, now fixed per above) needs the identical
reverse-order unwind** for every disk-quota resource acquired before the failure — today's
`cleanup_partial` already has this exact acquired-so-far-only shape for netns/CNI (§ existing code);
the new resources need the same treatment: guarded by their own acquired-so-far booleans (matching
`netns_created`/`cni_added`'s existing pattern) so a failure at, say, the overlay-mount step correctly
unwinds the loop mount and loop device it already acquired without attempting to unwind the overlay
mount that never succeeded.

**New deployment preconditions** (mirrors this file's own existing precedent for naming these
explicitly — `cnitool`/CNI plugins/`nft` for Slice 10): `losetup`, `mkfs.ext4` (`e2fsprogs`), and
loop-device kernel module availability (`CONFIG_BLK_DEV_LOOP`, present on essentially every
distribution kernel by default, named anyway rather than silently assumed) must exist on the host.
`fallocate`/`mount`/`umount` are assumed already present (coreutils/util-linux, the same baseline
this file already assumes for `ip`).

## 6. `OciSpecInputs`/`Instance` changes

```cpp
struct OciSpecInputs {
    // ... existing members unchanged ...
    std::uint32_t pids;   // NEW -- 0 = unset, same "0 means don't add this resources member" idiom
                           // memory_bytes/fds already use.
};

struct Instance {
    // ... existing members unchanged ...
    std::string lower_dir;          // NEW -- always set (disk_bytes > 0 or not, §5 point 1's fix):
                                     // the REAL `ctr images mount` target. destroy()/cleanup_partial
                                     // always `ctr images unmount` THIS path, never rootfs_dir.
    std::string loop_device;        // NEW -- disk_bytes > 0 only; empty otherwise. Recorded at
                                     // create() time so destroy() detaches the EXACT device this
                                     // instance attached, never re-discovered.
    bool disk_quota_active = false; // NEW -- mirrors net_created's existing "only reverse what was
                                     // actually acquired" pattern.
};
```

`KataBackend` itself (not `Instance`) also gains one new private member: `std::mutex
disk_quota_setup_mutex_;` — §5b's fix, serializing only the `fallocate`/`mkfs.ext4`/`losetup -f`/
`mount` sequence across concurrent `create()` calls, held for the shortest span that actually needs
it (released before `ctr run --config` itself, which does not need serialization).

## 7. Contract obligations this draft must not weaken (008 §2, restated as a checklist for the
   red-team pass and for implementation)

- **"No backend is permitted to weaken the contract by configuration."** A caller that sets
  `disk_bytes`/`net_bytes`/`pids` and gets a `create()` that silently ignores the field (rather than
  either enforcing it for real or failing closed) would violate this — every new code path below
  must fail `create()` closed (a distinct diagnostic code, matching this file's own established
  `kata_backend.*_failed` naming) on any step failure, never fall through to an unquota'd/uncapped
  container.
- **I4 (attribution)**: no change needed — `create()`'s existing failure/success shape already
  surfaces through the same `result<SandboxHandle>` every other slice uses.
- **I2 (no ambient authority)**: none of the three new mechanisms grant anything beyond what
  `SandboxSpec::limits` itself already authorizes; `authorize_spec()` (§ existing code, unchanged by
  this draft) continues to gate `mounts`/`net` exactly as before — `pids`/`disk_bytes`/`net_bytes`
  are resource caps, not new reachable authority, the same category `memory_bytes`/`fds`/`wall_ms`
  already occupy without a capability check of their own. **Correction (red-team finding, this claim
  was wrong as first written)**: the first draft asserted this held unconditionally. It does not —
  the *interaction* between a pre-existing, already-permissive gap (a `MountSpec` host path is
  unvalidated against this backend's own workdir whenever the caller holds zero `cap::SandboxMount`
  grants, `authorize_spec()`'s own documented opt-out) and this draft's *newly introduced* live
  loop-mounted file at `<workdir>/upper.img` combine into a real, capability-independent host
  filesystem corruption path — see §5a' for the finding and its fix (an unconditional, backend-owned
  path exclusion, not gated by the caller's capability set at all). This is the one place this draft
  introduces something reachable without an explicit grant, and it is closed, not merely disclosed.

## 8. Explicitly out of scope for this draft

- Fixing §2's disclosed `disk_bytes == 0` read-only-rootfs finding — named, not fixed, per §2's own
  scope decision.
- `cpu_ms` (unchanged rate-vs-total-budget gap, ADR-086's own original finding, unrelated to
  `--config` mode).
- `exec_outcome_class::oom` (ADR-088, investigated and rejected for unrelated reasons — a guest OOM
  under the new `disk_bytes`/`pids` limits is not a new signal source this draft creates).
- Reopening `disk_bytes == 0`'s writability question, or the GPU-passthrough/`cap::Exec` gaps
  ADR-094 already named as separate follow-ons.

## 9. Test plan (compile-verified only this session, no live deployment — named, not hidden)

- `pids`: extend `tests/test_kata_backend_slice9_10_linux.cpp` (or a new file, TBD at implementation
  time) with a case asserting the OCI spec JSON `create()` writes contains the expected
  `linux.resources.pids.limit` value when `spec.limits.pids > 0`, and omits the member when unset —
  a pure JSON-shape assertion, runnable without a live deployment (the same "prove the artifact we
  actually hand to `ctr`, not just that we called it" idiom this file's tests already use for mounts/
  memory in Slice 9).
- `net_bytes`: assert the exact `nft` command sequence `create()` issues when `net_bytes > 0`
  (argv-by-argv, matching Slice 9/10's existing test style for the allowlist rules) — including rule
  ORDER (quota-drop before the per-destination accept rules), since §4 established order is the
  actual enforcement mechanism, not incidental.
- `disk_bytes`: assert the exact `fallocate`/`mkfs.ext4`/`losetup`/`mount` command sequence and
  ordering, and that `cleanup_partial`/`destroy()` issue the exact reverse-order teardown sequence on
  both the failure path and the normal path — including `ctr images unmount <lower_dir>` (never
  `rootfs_dir`, §5 point 1's fix) on both paths.
- `MountSpec` exclusion (§5a' fix): a case asserting `create()` rejects a `MountSpec` host path equal
  to, an ancestor of, or contained within `/run/agentengine-kata` with
  `kata_backend.workdir_mount_forbidden`, **with zero `cap::SandboxMount` grants held** — the exact
  zero-grant condition the finding depends on, not merely "any rejection happens."
- Loop-device mutex (§5b fix): a case (or, if a real concurrency test is impractical without a live
  deployment, a compile-time/code-inspection note in the test file) confirming the
  `disk_quota_setup_mutex_` scope covers exactly `fallocate`→`mkfs.ext4`→`losetup -f`→`mount` and
  nothing wider — a lock scoped too broadly would silently serialize unrelated `create()` calls that
  never touch disk quotas at all, a real throughput regression this test should guard against too.
- All three (plus the `lower_dir`/bind-mount change in §5 point 1, which touches every instance
  regardless of whether `disk_bytes` is set): a caller that sets none of `pids`/`disk_bytes`/
  `net_bytes` produces the SAME final rootfs content and OCI spec shape as today, even though the
  underlying mechanism now always does one extra bind-mount step — a regression guard against this
  draft's own `lower_dir` fix accidentally changing observable behavior for callers who opt into
  nothing.

## 10. Open questions from the original draft — resolved by the red-team pass

*(Renumbered from the original draft's §10; each is now closed with a verdict, not left open — see
§11 for the pass's own findings this table doesn't otherwise cover.)*

1. **Resolved, correct as designed.** Independently verified against the nftables wiki: a named
   quota object is table-scoped and shareable across rules in different base chains of the same
   table, accumulating on every evaluation regardless of match outcome. The syntax used to *define*
   the quota was wrong (missing required `{ }` braces, §4's fix); the underlying cross-chain sharing
   mechanism itself was correct as designed.
2. **Resolved, no defect found.** `losetup -f --show`'s single-line stdout (the device path, one
   trailing newline) is well within `run_ctr()`'s existing capture path and cap; the caller must trim
   the trailing newline before using the value as a path component (an ordinary, low-risk parsing
   step, not a new hazard) — implementation must not skip this, but it is not a design-level gap.
3. **Resolved, no defect found; now stated explicitly rather than left implicit.** Mounting a
   loop-backed filesystem and an overlay filesystem both require `CAP_SYS_ADMIN`-class host
   privilege, the same tier `ip netns`/the existing bind-mount-adjacent operations in this file
   already assume this backend's host process runs with — no new privilege tier introduced, named
   explicitly now (§5c's deployment-precondition paragraph) rather than silently assumed.
4. **Resolved — see §5b.** Confirmed benign for corruption/double-use (the kernel's own
   `LOOP_SET_FD` exclusivity prevents that), but real against this project's own "tolerate concurrent
   calls" contract; fixed with a scoped mutex rather than left as an open question.
5. **Resolved: correctly two separate, non-conflated gaps.** `ResourceLimits::disk_bytes` (this
   draft's subject — the whole sandbox instance's writable-layer budget) and `MountSpec::quota_bytes`
   (a per-mount cap on an explicitly bind-mounted host path, still unenforced, unchanged by this
   draft) are genuinely different authorities at different scopes — this draft does not touch
   `MountSpec::quota_bytes` at all, and does not claim to. Left as a real, still-open, separately
   trackable gap, same as before this draft.

## 11. Full red-team finding log (fresh reviewer, no prior session context, 2026-08-24)

Run against this draft's first version, before any implementation, independently against the
shipped `kata_backend.{hpp,cpp}` (not against this draft's own prose alone) and independently
sanity-checked against real nftables/`fallocate(2)` documentation rather than taken on faith. All
eight are fixed or resolved at the sections named above; logged here verbatim by classification for
the historical record this file's own convention (§1's citation trail) already establishes:

1. **BLOCKING** — no `lower_dir` tracked; `destroy()`'s existing unmount call silently mistargeted,
   leaking a containerd snapshot on every `disk_bytes > 0` lifecycle. Fixed: §5 point 1, §6.
2. **BLOCKING** — `cleanup_partial()`'s existing (pre-this-draft) step order is remove-then-unmount,
   not unmount-then-remove; naively appending new teardown steps would delete/corrupt a live writable
   overlay before detaching it. Fixed: §5c.
3. **BLOCKING** — `fallocate -l` was mischaracterized as lazy/sparse; it is an eager reservation.
   Fixed (disclosure corrected, mechanism kept as the better multi-tenant choice): §5 point 2.
4. **BLOCKING** — the `nft add quota` command as drafted omits mandatory `{ }` braces, a real `nft`
   syntax error that would make `net_bytes` completely non-functional while appearing tested. Fixed:
   §4.
5. **BLOCKING (I2-relevant)** — a pre-existing, capability-independent mount-validation opt-out
   combined with this draft's own new loop-mounted file produces a live host-filesystem-corruption
   path reachable with zero capability grants. Fixed: §5a', §7 correction.
6. **BLOCKING (contract)** — `losetup -f` TOCTOU across concurrent `create()` calls is benign for
   corruption but violates the registry's own documented "tolerate concurrent calls" contract if left
   as a spurious-failure risk. Resolved with a scoped mutex, not left open: §5b.
7. **MINOR** — the `nft` named-quota kernel/build precondition was undisclosed, inconsistent with
   this draft's own disclosure standard for `losetup`/`mkfs.ext4`. Fixed: §4.
8. **MINOR / mischaracterization** — the first draft's "`input` traffic is by construction only ever
   a response to an `output`-permitted connection" claim overstated what the mechanism actually
   guarantees (a pre-existing, unchanged exposure, not a new one). Fixed: §4.

**Independently verified, not just asserted, during the same pass**: nftables' cross-chain named
quota sharing is real (checked against the nftables wiki); a named quota object's own `{ }`-bracketed
definition syntax (checked against `nft(8)`); `fallocate -l`'s eager-vs-`truncate`'s-lazy distinction
(checked against `fallocate(2)` and ext4's own extent-reservation behavior). Overall verdict from the
pass, concurred with here: the draft's core mechanisms are sound and did not require a structural
redesign — every finding was a fix within the existing design's shape, now applied.
