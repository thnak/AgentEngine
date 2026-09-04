# ADR-171 — `DockerCliBackend`/`DockerExecutionSurface` emitted `docker run` with no isolation flags at all. What can honestly be closed without pretending the result is a `SandboxBackend`?

- **Status:** Proposed — implemented, proven by real test execution against a live Docker daemon
  (37 checks, containment verified by reading the container's own cgroup values from inside it, with
  a positive control), full 278-test suite re-run at 100%, pending project-owner sign-off.
- **Date:** 2026-09-04.
- **Scope:** `include/agentengine/sandbox/docker_execution_surface.hpp` (modified — new
  `ContainerIsolation`, `docker_isolation_argv()`, `container_isolation_from()`;
  `DockerCliBackend::create()` and `DockerExecutionSurface`'s constructor take isolation, defaulted
  deny-all; move ctor/assignment carry the new member; a "what this file is, and is not" scope
  statement), `include/agentengine/sandbox/execution_surface.hpp` (modified — the concept-level scope
  statement issue #63 step 2 asked for), `008-Sandbox-and-Isolation.md` (amended — §2a),
  `tests/test_docker_isolation.cpp` (new), `tests/CMakeLists.txt` (additive wiring).
  **No existing test file changed, and no existing call site needed editing.**
- **Related specs:** GitHub issue #63 (the defect this closes) · GitHub issue #66 (the containerd
  sibling, split out) · `008-Sandbox-and-Isolation.md` §2 (the backend contract), §2a (custom
  backends — amended here), §3 (profile resolution), §4 (egress is always host-mediated), §9 (the
  promotion gate this does **not** clear) · `decisions/ADR-099-*`/`ADR-102` (the phase that ported
  this surface, and the project-owner direction keeping it outside `SandboxBackend`) ·
  `decisions/ADR-165-docker-execution-surface-argv-hardening.md` (the argv port this builds on) ·
  `decisions/ADR-139-docker-run-capture-timeout-and-cap.md` (host-side `wall_ms`/`output_bytes`
  enforcement, the two limits no container flag expresses) · `include/agentengine/sandbox/net_egress_proxy.hpp`
  (the real host-mediated egress path an allowlist must use instead).

## 1. The question

Issue #63 made two claims. Both were verified against `main` before any code was written, and both
were true.

**(1) The surface is outside 008's `SandboxBackend` contract by design.** True, and unchanged by this
ADR — `ExecutionSurface`'s own banner says so, matching explicit project-owner direction (ADR-099 §7).

**(2) It enforced none of 008 §2's mandatory contract.** Also true, and worse than "not a backend":

```
$ grep -n -- '--network|--memory|--pids-limit|--cpus' include/agentengine/sandbox/docker_execution_surface.hpp
(no matches)
```

`create()` emitted `docker run -d --rm -w /workspace --name <n> <image> sh -c "..."`. Every container
it produced therefore ran with Docker's defaults: **unfiltered bridge egress**, unbounded memory,
unbounded pids, unbounded CPU, and the default Linux capability set. The gap was not confined to
`DockerExecutionSurface` as the issue's §2 framed it — `DockerCliBackend`, the lower-level wrapper
that `tools/sandboxed_shell_chat.cpp` and friends also reach, had it too.

The issue asked only that this be *tracked*, with two options: promote toward a real backend (owing
008 §9's full gate), or say explicitly in the header that it is permanently scoped. So the real
question is: **what can honestly be closed now**, short of a promotion nobody has authorized?

## 2. What this does — and the line it does not cross

Two things, deliberately separated:

**(a) Real, kernel-enforced containment, fail-closed by default.** `ContainerIsolation` is a plain
value type whose *defaults are the point*: a default-constructed one denies the network outright
(`--network none`), drops every Linux capability, forbids privilege escalation, and caps memory
(512 MiB), pids (128) and CPU (1.0). Every existing `create()` / `DockerExecutionSurface{...}` call
site keeps compiling and is now contained — **nothing has to opt in to be safe**, and loosening any
axis is an explicit, reviewable field assignment. That inversion is what makes the header's scope
statement enforceable rather than advisory; a comment asking callers to remember is exactly what was
already there and was not enough.

The numeric ceilings are conservative defaults a host raises, not tuned values. 006 §7's
"a fixed byte constant applied uniformly is an anti-pattern" warning is about deriving a per-call
budget from a model's context window; it is not an argument for leaving a container unbounded.

**(b) The bridge from 008's own vocabulary, written now so its gaps are visible.**
`container_isolation_from(ResourceLimits, NetPolicy)` maps what `docker run` can genuinely enforce,
and refuses what it cannot:

- **A `NetPolicy` allowlist is REFUSED, not silently widened.** 008 §4's rule is that egress is
  always host-mediated; `--network bridge` grants unfiltered egress, which is not an allowlist by any
  reading. Mapping a three-entry allowlist onto full bridge access would be the single most dangerous
  thing this function could do, so it fails closed with a `failure_class::policy` error whose message
  names the real alternative (`sandbox/net_egress_proxy.hpp`).
- **`ResourceLimits::cpu_ms` is NOT mapped to `--cpus`.** They answer different questions: `cpu_ms` is
  a total CPU-time *budget*, `--cpus` is a bandwidth *share*. Treating one as the other produces a
  limit that looks enforced and is not.
- **An unset (zero) limit maps to the conservative default, never to "unlimited."** Zero is exactly
  what an untouched `SandboxSpec` carries, so this is the direction that matters.
- `wall_ms`/`output_bytes` are enforced host-side by `run_argv()` (ADR-139), not by any container
  flag; `fds`/`disk_bytes`/`net_bytes` have no honest `docker run` equivalent and are named unmapped.

**The line not crossed: this is still not a `SandboxBackend`, and container flags cannot make it
one.** `create()` takes no `EffectContext` and consults no `CapabilitySet`, so 008 §2 rule 1
(empty-by-default authority, I2) has nothing to check against. That is a *structural* property of the
`ExecutionSurface` concept — its three verbs take no capability parameter at all — not a missing
flag. Both headers now say this outright, and 008 §2a carries the general rule, because the natural
assumption is the inverse one: **proximity in the source tree is not conformance.**

## 3. Evidence

`tests/test_docker_isolation.cpp`, 37 checks. The offline half needs no daemon; the live half proves
the kernel actually applied the limits.

| | claim |
|---|---|
| I1 | a **default-constructed** `ContainerIsolation` produces deny-all argv — network, memory, pids, cpu, `--cap-drop ALL`, `--security-opt no-new-privileges` |
| I2 | opting in changes exactly what was opted into; disabling the two boolean flags **omits** them rather than emitting `--cap-drop ''` (malformed, not looser) |
| I3 | `--cpus` is formatted from integer milli-CPU, so the argv is byte-stable and locale-independent — `std::to_string(double)` is neither |
| I4 | the 008 §2 vocabulary maps, and an **unset** limit falls back to the conservative default, never to "unlimited" |
| I5 | **a `NetPolicy` allowlist is refused**, as a `policy` failure, with a stable code and a message naming `net_egress_proxy` |
| L1 | **live**: a default container has only `lo` — read from `/sys/class/net` **inside its own network namespace** |
| L2 | **positive control**: the same code path with `network_enabled` **does** get `eth0`, so L1 is a falsifiable result and not a probe that always reports "denied" |
| L3 | **live**: `memory.max`, `pids.max` and `cpu.max` inside the container are exactly the requested values — the kernel enforced them, they were not merely passed |
| L4 | **live**: `DockerExecutionSurface` (the type `SandboxRuntime` actually drives, whose container is created inside `reset()`) carries the **caller's** ceilings, not the type's defaults — and the surface's real job, materializing the tree, still works under containment |

L3 and L4 are the checks that matter. A flag on a command line proves the flag was passed; reading
`/sys/fs/cgroup/pids.max` from inside proves a fork bomb has a hard ceiling — and proves it without
detonating one, which is also what CLAUDE.md's machine-safety rule asks for.

**A real bug found and fixed while wiring this, not by review:** `DockerExecutionSurface`'s move
constructor and move assignment both enumerate their members by hand. Adding `isolation_` without
touching them would have left a moved-to surface silently reverting to the deny-all defaults —
discarding a host's deliberate opt-in on move. The safe direction, still wrong, and exactly the class
of silent divergence that file's own moved-from `instance_` comment already exists because of. Both
now carry the member explicitly.

**Build and suite.** Clang 21 / Ninja / Debug under this project's `-Werror` warnings target: zero
warnings, zero errors. Full `ctest -j8`: **278/278 passed** — including every other live-Docker test
(`test_sandbox_runtime`, `test_mandatory_sandbox_provider`, `test_docker_orphan_reap`,
`test_docker_non_ascii_path`, `test_composed_sandbox_providers_live`, `test_task_branch_tools`), all
of which now run their containers with the network denied. That they pass unchanged is itself the
evidence that nothing in this tree needed container egress, which is why deny-all could be the
default rather than an opt-in.

## 4. What this explicitly does NOT do

- **Does not promote anything to a `SandboxBackend`**, and does not clear 008 §9's gate (G1 parity,
  G2 containment corpus, G3 no-ambient-authority probe, G4 teardown). L1–L4 are G2-shaped evidence for
  one backend on one host; they are not the corpus, and G1/G3/G4 are untouched.
- **Does not add `CapabilitySet` mediation** — the actual reason this is not a backend (§2). Closing
  it means changing `ExecutionSurface`'s own verb signatures, which is a concept change, not a flag.
- **Does not honor `SandboxSpec::mounts`** — this surface moves data with `docker cp`.
- **Does not fix the identically-shaped `ContainerdCliBackend`.** Grepping it for isolation flags
  still returns nothing. Not fixed here because `ctr`'s flag syntax differs (some options live in the
  OCI spec rather than a CLI flag) and this session had no live containerd daemon to hold the fix to
  the same evidence standard — shipping untested flags that merely look right is what that standard
  exists to prevent. Split out as **GitHub issue #66** rather than silently fixed or silently ignored.
- **Does not make the surface selectable via `SandboxProfile<P>`** (008 §3). Still unreachable
  through profile resolution, by the same project-owner direction.
- **Does not enforce a network allowlist.** It refuses one (§2). A host needing filtered egress uses
  `sandbox/net_egress_proxy.hpp`.
- **Was not run through the full `design → red-team → prove → judge` process.** The change adds
  containment to code that had none and mints no new authority; the adversarial work that did happen
  is §3's move-semantics bug and the deliberate refusal in I5.

## 5. Promotion gate

**G1 (met).** A container produced by the default code path has no network interface but loopback and
carries kernel-enforced memory/pids/CPU ceilings — verified from inside the container — while the same
code path with an explicit opt-in demonstrably gets `eth0`. Falsifiable and it does fail: reverting
the splice in `create()` fails L1/L3, and reverting the surface's `isolation_` threading fails L4.

**G2 (met).** Purely additive: every existing call site compiles unchanged and is now contained, and
the full pre-existing suite — including six other live-Docker tests — passes with no edits.

**G3 (open, for the project owner).** Should this surface be promoted toward a real
`remote`-profile-shaped `SandboxBackend` (issue #63's step 1), which would require giving
`ExecutionSurface`'s verbs an `EffectContext` and then clearing 008 §9 in full — or does it stay
permanently scoped, as both headers now state? Not decided here. ADR-171 makes the permanently-scoped
answer *safe*; it does not make it final.
