# ADR-095 — KataBackend Slice 11: `pids`/`disk_bytes`/`net_bytes` via the `--config` pipeline

Status: Proposed (design → independent red-team (8 findings, 5 BLOCKING) → fix → implement →
WSL compile-verified + one real, live-run-verified fix (the workdir-mount exclusion) → awaiting
project-owner Judged sign-off)

## 1. The question

Can `KataBackend` close its last three unenforced `ResourceLimits` axes — `pids` (ADR-090,
"investigated and deferred"), `disk_bytes`/`net_bytes` (ADR-092, "investigated and deferred") — via a
full redesign/rewrite of `create()`'s resource-limit path, project-owner-authorized 2026-08-24 (not a
patch), reusing the `--config`-mode OCI-spec/netns/nftables pipeline ADR-093 already built for an
unrelated reason (the real `NetPolicy` allowlist)?

Full design, the independent red-team pass's own findings, and every fix applied before
implementation: `docs/planning/kata-backend-config-pipeline-pids-disk-net-redesign-draft.md`. This ADR
is the short-form record; the draft has the full reasoning, citations, and finding log (§11).

## 2. Why ADR-090/ADR-092's own negative decisions are reopened, not wrong

Both were correct given the information available when written. `pids`: ADR-090 found `--config` mode
blocked by a rootfs-preparation gap (`ctr snapshot prepare` needs an internal, undiscoverable chain
ID). `disk_bytes`/`net_bytes`: ADR-092 correctly assigned them to 008 §1b's "interpreter-level
mediation," for which this backend has no guest-side attachment point (raw `/bin/sh -c` exec, no
mediated interpreter). Both premises are stale, not wrong: ADR-093 (a separate investigation, for a
separate reason — the real NetPolicy allowlist) independently found `ctr images mount` solves the
rootfs-prep gap without needing the chain ID at all, and already built a real per-instance
netns+nftables chain — machinery this ADR's `net_bytes` mechanism reuses directly, and whose
side-effect (a real, separately-mountable rootfs directory) is what makes a **backend-owned**
disk-quota mechanism possible without ever touching interpreter-level mediation.

## 3. What this Slice does

- **`pids`**: `build_oci_spec_json()`'s `linux.resources` object gains a `pids.limit` member —
  the identical shape `memory` already has. No new mechanism.
- **`net_bytes`**: a named `nft` quota object (`ae_net_budget`), shared by a rule on BOTH the
  existing per-instance `output` chain and a new `input` chain in the same netns table — total
  bidirectional bytes, not egress-only. Entirely host-side; zero guest cooperation.
- **`disk_bytes`**: a backend-owned, loop-device-backed, fixed-size ext4 filesystem hosts the
  writable half of a real overlay mount (`ctr images mount`'s read-only `View()` output as
  `lowerdir`; the loop filesystem as `upperdir`/`workdir`). Enforcement is a real guest-kernel
  `ENOSPC` once the loop filesystem fills — not a heuristic, the same class of guarantee
  `memory_bytes` already gets from the guest's own OOM path. `fallocate -l` reserves the full
  `disk_bytes` on the HOST eagerly at `create()` time — a considered choice (real, immediate
  admission cost, safer for multi-tenant hosts), not an oversight (design draft §5 point 2).

`disk_bytes == 0`/`net_bytes == 0` (unset, the default): byte-for-byte unchanged behavior — this
Slice's own `lower_dir`/plain-bind-mount restructuring (§4 below) is the one exception, needed for
every instance regardless of whether `disk_bytes` is set, to fix a real snapshot-leak finding (§4).

## 4. Independent red-team pass — 8 findings, 5 BLOCKING, all fixed before implementation

Run against the pre-implementation design draft by a fresh reviewer with no session context,
independently checked against `nft(8)`'s own grammar, the nftables wiki, and `fallocate(2)`/ext4's own
extent-reservation behavior rather than taken on faith (full log: design draft §11):

1. **BLOCKING** — no `lower_dir` tracked separately from `rootfs_dir`; `destroy()`'s existing
   `ctr images unmount` call would silently mistarget the overlay mountpoint once the two diverged,
   leaking a containerd snapshot mount/lease on every `disk_bytes > 0` create/destroy cycle. **Fixed**:
   `Instance::lower_dir` is now always tracked and always the real unmount target — `rootfs_dir` is a
   plain `mount --bind` of `lower_dir` when `disk_bytes` is unset, a real overlay when it is set.
2. **BLOCKING** — the pre-existing `cleanup_partial()` lambda already ran `fs::remove_all(workdir)`
   *before* its own `ctr images unmount` call (remove-then-unmount); naively appending new teardown
   steps after it would delete/corrupt a live writable overlay before detaching it, once `rootfs_dir`
   could be a real writable mount. **Fixed**: every unmount/detach now runs strictly before
   `fs::remove_all` in both `cleanup_partial()` (create() failure path) and `destroy()`.
3. **BLOCKING** — `fallocate -l` was mischaracterized as lazy/sparse. Verified against `fallocate(2)`:
   it is an **eager** reservation. **Fixed**: disclosure corrected; the eager mechanism is kept
   deliberately (the safer multi-tenant choice), not swapped for `truncate -s`.
4. **BLOCKING** — the `nft add quota` command as drafted omitted the mandatory `{ }` braces around
   the quota spec (`nft(8)`'s own grammar) — a real syntax error that would have shipped `net_bytes`
   completely non-functional while the draft's own proposed test would have asserted the broken
   command and appeared to pass. **Fixed**: correct braced argv tokens, matching this file's own
   existing chain-definition convention.
5. **BLOCKING, I2-relevant** — a pre-existing, capability-independent gap (`authorize_spec()` skips
   its own `cap::SandboxMount` coverage check entirely for a caller holding zero grants at all;
   `create()`'s own mount validation never rejected a host path targeting this backend's own workdir)
   combined with this Slice's new live loop-mounted file to produce a real host-filesystem-corruption
   path reachable with **zero capability grants**. **Fixed**: `create()` now unconditionally rejects
   any `MountSpec` host path equal to, an ancestor of, or contained within `/run/agentengine-kata`
   (`kata_backend.workdir_mount_forbidden`), independent of any capability grant. **This fix is the
   one piece of this Slice independently proven by real test execution this session** (§6) — every
   other claim here is compile-verified only, no live Kata deployment reachable.
6. **BLOCKING, contract** — `losetup -f` (find a free loop device) has a TOCTOU race under concurrent
   `create()` calls; confirmed benign for corruption (the kernel's own `LOOP_SET_FD` exclusivity
   prevents double-attach) but real against `sandbox_backend_registry.hpp`'s own documented "tolerate
   concurrent create/exec/destroy calls from unrelated sessions" contract (a purely load-driven race
   could spuriously fail an unrelated caller). **Fixed**: a `KataBackend`-private `std::mutex` serializes
   only the `fallocate`/`mkfs.ext4`/`losetup -f`/loop-mount sequence, not `create()` as a whole.
7. **MINOR** — the `nft` named-quota kernel/build precondition (`nft_quota`) was undisclosed,
   inconsistent with this draft's own disclosure standard for `losetup`/`mkfs.ext4`. **Fixed**: named.
8. **MINOR, mischaracterization** — the first draft's "`input` traffic is by construction only ever a
   response to an `output`-permitted connection" claim overstated the mechanism (a pre-existing,
   unchanged exposure — Slice 10 already has no `input` filtering at all — not a new one this Slice
   introduces). **Fixed**: corrected wording.

**Independently verified, not just asserted, during the same pass**: nftables' cross-chain named
quota sharing is real; the shared quota object's own `{ }`-bracketed definition syntax; `fallocate
-l`'s eager-vs-`truncate -s`'s-lazy distinction. Overall red-team verdict: the mechanisms are sound;
this was a fix-and-re-review, not a structural redesign.

## 5. What changed in this pass

- `src/backends/kata/kata_backend.hpp` — new SLICE 11 header-comment section (scope, the three
  mechanisms, the red-team findings and fixes); `Instance` gains `lower_dir`/`loop_device`/
  `disk_quota_active`; `KataBackend` gains a private `disk_quota_setup_mutex_`.
- `src/backends/kata/kata_backend.cpp` — `OciSpecInputs` gains `pids`; `build_oci_spec_json()` adds
  the `pids.limit` resources member; new helpers `targets_own_workdir()`/`trim_trailing_newline()`;
  `create()`'s mount-validation loop gains the unconditional workdir-exclusion check; `create()`'s
  rootfs acquisition is rewritten around a separately-tracked `lower_dir` plus either a plain bind
  mount (`disk_bytes == 0`) or the full loop-device/ext4/overlay stack (`disk_bytes > 0`), serialized
  behind the new mutex; `create()`'s `nft_cmds` construction gains the `input` chain + shared quota
  object when `net_bytes > 0`; `cleanup_partial()` reordered (unmount/detach before `remove_all`) and
  extended for the new resources; `destroy()` extended with the matching reverse-order teardown and
  now unmounts `lower_dir` (never `rootfs_dir`).
- `tests/test_kata_backend_slice2_linux.cpp` — 3 new cases proving the workdir-mount-exclusion fix:
  exact-root match, contained-within match, and a sibling-path negative control (proves the check is
  path-component-aware, not a naive substring match). **These 3 cases pass for real in this session**
  (§6) — they run before any resource acquisition, so unlike every other case in this backend's test
  suite they do not require a live Kata/containerd deployment.
- `tests/test_kata_backend_slice9_10_linux.cpp` — 3 new cases (`pids` smoke test; `disk_bytes` real
  ENOSPC-vs-fits-cleanly pair; `net_bytes` real request-exceeds-256-byte-budget case) — all require a
  live deployment, not run this session, same disclosed posture as every prior Kata test.
- `docs/planning/kata-backend-config-pipeline-pids-disk-net-redesign-draft.md` — the full design,
  red-team pass, and every fix, kept as the permanent record (not deleted after landing).

## 6. Verification this session

No live Kata/containerd/`cnitool`/`nft`/`losetup` deployment was reachable this session (root
privilege itself is unavailable in the WSL environment used — confirmed by a baseline `git stash`
comparison: every pre-existing Kata test already failed with the identical `Permission denied` under
`/run/agentengine-kata` *before* this Slice's changes, so this is a pre-existing environment
limitation, not a regression this pass introduced).

- **Real, live-run-verified**: the 3 new workdir-mount-exclusion cases in
  `test_kata_backend_slice2_linux.cpp` (finding 5's fix) — these run before any resource is acquired
  and pass for real in this session, the exact same reason the pre-existing `BlobRef`/`,`-rejection
  cases in that same file have always passed without a live deployment. This is the one piece of this
  ADR's claims backed by actual execution, not compile inference alone.
- **Compile-verified only** (everything else — `pids`/`net_bytes`/`disk_bytes` themselves, the
  `lower_dir` restructuring, the mutex, the teardown reordering): WSL rebuild of
  `agentengine_kata_backend` and all four Kata test binaries clean (`test_kata_backend_linux`,
  `test_kata_backend_slice2_linux`, `test_kata_backend_abuse_corpus_linux`,
  `test_kata_backend_slice9_10_linux`); full local `ctest` run, 156/160 passing — the 4 failures are
  the same 4 Kata test binaries that already failed identically at baseline (confirmed via
  `git stash`/rebuild/rerun before restoring this Slice's changes), zero new failures. Windows build
  unaffected by construction: `AGENTENGINE_BUILD_KATA_BACKEND=OFF` in the Windows `CMakeCache.txt`,
  this backend entirely absent from that build graph.

## 7. Honestly disclosed residuals, not eliminated

- Every mechanism this Slice adds beyond the workdir-mount-exclusion fix (§6) is unexecuted against a
  real deployment — the exact `nft` cross-chain quota behavior, `ctr images mount`'s real `View()`
  semantics for the pre-existing `disk_bytes == 0` path (a separately-named, NOT fixed, real
  possible-defect this Slice's own design draft §2 surfaced — the rootfs may not actually be writable
  today regardless of this Slice), whether `losetup -f --show`'s stdout format is exactly as assumed,
  and whether `linux.resources.pids.limit` is honored by `kata-agent` the way it is by runc on a
  shared kernel, all remain genuinely open until a real host exists to prove them against.
- `disk_bytes == 0`'s possible pre-existing read-only-rootfs gap (design draft §2) is named, not
  fixed, by explicit scope decision — an unrelated behavior change for every caller, not just those
  opting into `disk_bytes`, is out of scope for this authorized redesign.
- `ResourceLimits::cpu_ms` (rate-vs-total-budget gap, unrelated to `--config` mode), `MountSpec::
  quota_bytes` (a distinct, still-separate, still-unenforced per-mount cap — confirmed NOT conflated
  with `disk_bytes` by this Slice), `exec_outcome_class::oom` (ADR-088, investigated and rejected for
  unrelated reasons), GPU passthrough, and `cap::Exec` (ADR-094 Finding 2) are all unchanged,
  unaffected by this pass.
- ext4's own filesystem metadata overhead means actual usable `disk_bytes` space is somewhat below
  the literal requested value — not compensated for, stated rather than silently rounded.

## 8. Decision

Implementation lands as Proposed. The design→red-team→fix cycle is complete and the one piece
provably testable without a live deployment (the workdir-mount-exclusion I2 fix) is proven by real
execution. Every other claim awaits a real Kata/containerd host — scheduled separately by the
project owner — before this ADR can move to Judged.
