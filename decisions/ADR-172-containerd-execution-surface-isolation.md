# ADR-172 — `ContainerdCliBackend` had ADR-171's gap too. Is the Docker fix transferable, or does `ctr` need different answers?

- **Status:** Proposed — implemented, proven against a **live containerd 2.2.2 / runc on WSL2 Ubuntu**
  (39 checks, containment verified from the kernel's own records, with a positive control), the
  pre-existing containerd suite re-run green, pending project-owner sign-off.
- **Date:** 2026-09-04.
- **Scope:** `include/agentengine/sandbox/containerd_execution_surface.hpp` (modified — new
  `ContainerdIsolation`, `kDefaultDroppedCapabilities`, `containerd_isolation_argv()`,
  `containerd_isolation_from()`; `ContainerdCliBackend::create()` and `ContainerdExecutionSurface`'s
  constructor take isolation, defaulted conservative; move ctor/assignment carry the new member; a
  "what this file is, and is not" scope statement), `tests/test_containerd_isolation.cpp` (new),
  `tests/CMakeLists.txt` (additive wiring, inside the existing `NOT WIN32` block).
  **No existing test file changed, and no existing call site needed editing.**
- **Related specs:** GitHub issue #66 (the defect this closes), split from issue #63 ·
  `decisions/ADR-171-docker-execution-surface-isolation.md` (the Docker sibling this deliberately does
  **not** copy) · `008-Sandbox-and-Isolation.md` §2 (the backend contract), §2a (amended by ADR-171),
  §4 (egress is always host-mediated), §9 (the gate this does **not** clear) ·
  `decisions/ADR-145-containerd-execution-surface-promotion.md` (the port this builds on) ·
  `src/backends/kata/kata_backend.cpp` (the real `SandboxBackend` on this same `ctr` lineage, via the
  OCI spec rather than convenience flags) · `include/agentengine/sandbox/net_egress_proxy.hpp`.

## 1. The question

ADR-171 closed the missing-isolation gap for Docker and split the identically-shaped containerd
sibling out as issue #66, on the stated grounds that `ctr`'s flag syntax differs and no live
containerd daemon was available to hold a fix to the same evidence standard.

The second half turned out to be wrong: **WSL2 Ubuntu on this machine runs a live containerd 2.2.2
with `ctr` present, and the project builds and runs there under g++ 15.2.** So the fix could be held
to ADR-171's standard after all, and was. The first half turned out to be *more* true than expected —
the Docker mapping is not transferable, and three of its four axes need different answers.

The baseline was the same: `ContainerdCliBackend::create()` emitted
`ctr run -d --mount ... <image> <id> sleep infinity` with **no memory, CPU or capability flags at
all**.

## 2. Four axes, three of which differ from Docker

Every claim below was established by running `ctr` against the live daemon and reading the result
back — never from its `--help` text.

**Network — containerd was never in Docker's pre-ADR-171 state.** A flagless `ctr run` already gives
the container its own netns holding only `lo` (verified: `ls /sys/class/net` inside reports exactly
`lo`). So `host_network` defaults false and emits **nothing**: the guarantee comes from containerd,
and this type's job is to *keep* it, not to establish it. And the opt-in is materially worse than
Docker's: `--net-host` shares the **host's** network namespace — the container reaches anything the
host can, loopback-only services included — where Docker's `bridge` is a NAT'd bridge. A reader
transferring intuition from ADR-171 would assume parity that does not exist, so the field's own
comment says so.

**Capabilities — `--cap-drop ALL` does not exist.** `ctr` rejects it outright: *"capabilities must be
specified with 'CAP_' prefix"*. Dropping everything means enumerating containerd's own 14-entry OCI
default set (`kDefaultDroppedCapabilities`). That list's *completeness* is not something an offline
test can establish, which is exactly why L3 reads `CapBnd` from inside a real container and asserts
`0000000000000000`. C1 additionally asserts every entry carries the `CAP_` prefix, because `ctr`
fails the **whole** `create()` on a single bad name — one typo would disable containment entirely
rather than degrade it by one capability.

**Memory and CPU — these do transfer**, with different spellings (`--memory-limit <bare bytes>`, no
unit suffix; `--cpus`). Both are real: L4 reads the host-side cgroup and finds `memory.max` and
`cpu.max` exactly as requested.

**pids — there is no limit available on this path at all.** `ctr run` has no `--pids-limit`, and this
class deliberately never uses `--config`/OCI-spec mode (its own file banner). L4 asserts the
consequence out loud: `pids.max` really does read `max`.

## 3. The design decision that follows: refuse, don't ignore

Docker's `container_isolation_from()` refuses one thing (a `NetPolicy` allowlist). The containerd
analogue refuses **two**, and the second is the interesting one:

**A non-zero `ResourceLimits::pids` is refused.** Accepting it and emitting nothing would leave a
caller believing a fork bomb has a ceiling it does not have — strictly worse than saying no. The
error names a surface that *can* enforce it (the Docker one, or a real `SandboxBackend`), so the
refusal is actionable rather than a dead end. C6 pairs this with a positive control: the same call
*without* a pids limit must still map, or the test would be passing because the mapping refuses
everything.

**A `NetPolicy` allowlist is refused**, for ADR-171's reason with higher stakes: since the only
network opt-in here is `--net-host`, silently honoring an allowlist would hand the container the
host's network namespace.

Everything else follows ADR-171 unchanged: an unset (zero) limit maps to the conservative default and
never to "unlimited"; `cpu_ms` is not mapped to `--cpus` (a CPU-time *budget* is not a bandwidth
*share*); the flags are spliced **before** the image, because `ctr run [flags] IMAGE ID [COMMAND]`
means anything after the image is positional and a flag placed there would be eaten as the id or the
container's argv.

**The line not crossed is identical.** This is still not a `SandboxBackend`: `create()` takes no
`EffectContext` and consults no `CapabilitySet`, so 008 §2 rule 1 (empty-by-default authority, I2)
has nothing to check against — a property of the `ExecutionSurface` *concept*, not a missing flag.
The header now says so, matching the Docker sibling and 008 §2a's amended rule.

## 4. Evidence

`tests/test_containerd_isolation.cpp`, **39 checks, all passing** against live containerd 2.2.2.

| | claim |
|---|---|
| C1 | the defaults: no network flag, real memory/CPU ceilings, all 14 capability drops, every one `CAP_`-prefixed |
| C2 | opting in emits `--net-host`; disabling the drop omits all 14 rather than emitting an empty value |
| C3 | `--cpus` is integer-derived, so byte-stable and locale-independent |
| C4 | the 008 §2 mapping, with unset-means-conservative-default |
| C5 | a `NetPolicy` allowlist is refused, with a stable code and an actionable message |
| C6 | **a pids limit is refused** — plus a positive control that the mapping is not simply refusing everything |
| L1 | **live**: a default container has only `lo`, read from inside its own netns |
| L2 | **positive control**: `host_network` genuinely produces `eth0`, so L1 is falsifiable |
| L3 | **live**: `CapBnd` inside the container is exactly `0000000000000000` — the 14-name list is complete for this containerd/kernel pair |
| L4 | **live**: the host-side cgroup reports the requested `memory.max`/`cpu.max`, **and** `pids.max == max`, asserting the disclosed gap rather than describing it |
| L5 | **live**: `ContainerdExecutionSurface` threads the caller's isolation into `reset()`'s own `create()` — proven via the `host_network` opt-in, which is observable from inside without needing the id the surface generates privately — while the bind mount still works |

**One difference from ADR-171's evidence worth stating rather than glossing:** the Docker test reads
cgroup values *from inside* the container; `ctr run` does not mount cgroupfs into the container, so
L4 reads them **host-side** at `/sys/fs/cgroup/default/<id>`. Same kernel record, different vantage.
Network and capabilities are still verified from inside.

**A real bug in the first draft of the test, caught by it failing rather than by review:** the
`flag_value_is` helper finds only the *first* occurrence of a flag, which is wrong for `--cap-drop`,
which repeats 14 times — asking it about any drop but `CAP_CHOWN` reported a false negative. Fixed
with an `any_flag_value_is` helper plus the every-entry-is-prefixed scan.

**Build and suite.** WSL2 Ubuntu, g++ 15.2, Ninja, Debug, under this project's `-Werror` warnings
target: zero warnings, zero errors. `ctest -R containerd`: **3/3 passed**, including the pre-existing
`test_containerd_execution_surface` (31 checks) and `test_composed_containerd_providers_live`, both
now running their containers with the new isolation applied and both unchanged. Windows-side full
`ctest -j8`: 278/278 (this file is `NOT WIN32`-gated, so the Windows suite is unaffected by
construction; one unrelated `-j8` load flake in `test_rt_workflow_supervisor` appeared on the first
run and passed both standalone and on a clean second full run).

## 5. What this explicitly does NOT do

- **Does not promote anything to a `SandboxBackend`**, and does not clear 008 §9's gate.
- **Does not add `CapabilitySet` mediation** — the actual reason this is not a backend (§3).
- **Cannot bound pids on this path.** Refused rather than faked (§3). Closing it for real means
  `--config`/OCI-spec mode, which this class deliberately does not use, or writing the container's
  cgroup `pids.max` directly from the host — a real technique, but one whose cgroup-path derivation
  varies by driver (cgroupfs vs systemd) and would have been fragile in a way the rest of this ADR is
  not. Named as future work, not attempted.
- **Does not use `--cni`.** CNI networking would be a middle ground between "isolated" and "host
  namespace", but nothing in this tree provisions CNI plugins, so offering it would be a flag that
  fails at runtime on every machine here.
- **Does not honor `SandboxSpec::mounts`** — this surface takes exactly one bind mount at
  `/workspace`.
- **Proven on exactly one containerd/kernel pair** (containerd 2.2.2 on WSL2). L3's completeness
  claim for the capability list is scoped to that pair; a different containerd whose OCI default set
  differs would need the list rechecked, which is why L3 asserts the resulting `CapBnd` rather than
  the list's contents.
- **Was not run through the full `design → red-team → prove → judge` process**, for ADR-171's reason:
  it adds containment to code that had none and mints no new authority.

## 6. Promotion gate

**G1 (met).** A container produced by the default code path has no interface but loopback, a zeroed
capability bounding set, and kernel-enforced memory/CPU ceilings — each verified from the kernel's own
record — while the same code path with an explicit opt-in demonstrably reaches the host's network.
Falsifiable and it does fail: reverting the splice in `create()` fails L1/L3/L4, and reverting the
surface's `isolation_` threading fails L5.

**G2 (met).** Purely additive: every existing call site compiles unchanged and is now contained, and
the pre-existing containerd suite passes with no edits.

**G3 (open, for the project owner).** Same question ADR-171 left open, now with a second surface
behind it: do these become real `SandboxBackend` conformers — which needs `ExecutionSurface`'s verbs
to take an `EffectContext` and then 008 §9 in full — or do they stay permanently scoped? Both headers
now state the scoped answer; ADR-171 and ADR-172 make it *safe*, not *final*.
