# 008 — Sandbox and Isolation

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 007, 009, 021 · **Gate:** §9 · **Research:** [2026 landscape §6–8](docs/research/2026-standards-landscape.md)

## Goal

Define **one isolation contract** and the named profiles that implement it, so that untrusted
execution — plugins, generated code, third-party tools — has the same capability semantics, the
same limits, the same failure shapes, and the same audit trail across the target platform set (021
§2 — Windows now, Linux next), while the *strength* of the boundary remains a deployment choice.

## 1. The decision, and why

**Isolation is a seam with named profiles, not a hardcoded technology.** No single technique wins
on all of {boundary strength, cold start, portability, ecosystem} — the 2026 trade space is
tabulated in the research record §8 — and the axis a deployment cares about differs by deployment.

Two things are nonetheless **locked**, because leaving them open would leave the whole design open:

- **The WASM Component Model (WASI 0.3) is the plugin ABI** (009). One artifact runs bit-identically
  across the target platform set (021 §2), holds no ambient authority by construction, instantiates
  in microseconds, and
  is language-agnostic — including the large permissively-licensed **C/C++ library ecosystem**,
  which `wasi-sdk` compiles directly. A heavy library is *safer* as a plugin than as a linked host
  dependency, because a plugin holds only the capabilities the host hands it.
- **The Python code interpreter is embedded native CPython, permanently — never WASM, never a
  second runtime** (010 §2). As of July 2026 the rich Python-on-WASM ecosystem is
  **Emscripten**-targeted and requires a JavaScript host, so it cannot be embedded via wasmtime; the
  embeddable **WASI** CPython has no sockets, no threads, no wheel platform tag, and no
  binary-wheel ecosystem, and cannot `import numpy`. That evidence would only ever justify a WASM
  Python as a *second* backend alongside native CPython — and a second backend is exactly what this
  decision rules out regardless of WASI's future maturity: one Python means one behaviour for the
  agent to reason about and one thing to test, not two runtimes whose subtle differences (available
  packages, threading, extension modules) surface as bugs nobody can reproduce on the other one.
- **No `microvm` profile.** An earlier draft of this RFC offered `microvm` (Firecracker/Hyperlight-
  class hardware isolation) as the strongest local boundary for hostile or multi-tenant workloads.
  It is dropped. The original rationale cited a missing macOS backend; that framing is superseded
  now that macOS is out of scope entirely (021 §7 OQ-1) — the real reason is sturdier: Firecracker
  is architecturally Linux+KVM-only (its own FAQ states the host/guest boundary; the VMM core is
  built directly against `KVM_RUN`, no abstraction layer to retarget), so there is **no
  production-grade path to run it on Windows either**, the one platform besides Linux this project
  targets. Porting it would mean writing a new, unaudited VMM, not retargeting a build — exactly
  the second local isolation technology this decision declines to build and cross-platform-maintain
  (`docs/research/2026-microvm-windows-portability.md`, 2026-08-04). A workload that genuinely needs
  hardware-level isolation is better served by the `remote` profile against infrastructure that
  already does this well (e.g. Kubernetes Agent Sandbox CRD backed by Kata/Firecracker on a real
  Linux/KVM host) than by AgentEngine building one of its own. `native-jail` stays a kernel-enforced,
  per-OS boundary — §1b adds interpreter-level mediation as defense in depth on top of it, not a
  claim that it now matches hardware isolation.

The contract below is what makes that split safe: profiles differ in backend, never in semantics.

**Priority: `native-jail` first, the backend seam absorbs the rest.** `native-jail` is the one
profile that is cross-platform today, already has a proven Windows backend
(`decisions/ADR-004-appcontainer-native-jail-windows-backend.md`) and a proven caller-aware import
story for the full native-library Python ecosystem (010, `decisions/ADR-003-caller-aware-import-gating.md`)
— it is the v1 completeness target, not one profile among equally-unfinished others. `wasm` and
`remote` are already specified for when their tradeoffs fit. Anything stronger and Linux-only —
gVisor, Kata, a future microVM backend — is deliberately **not** something the engine commits to
building or maintaining first; §2a makes explicit that the `SandboxBackend` seam is open precisely so
a deployer who needs one can supply it without waiting on this project's roadmap.

### 1a. The WASM runtime requirement

The plugin host requires a runtime providing **all three** of:

1. **The Component Model** — WIT worlds, typed imports/exports. The entire plugin ABI (009) is built
   on it.
2. **WASI 0.3** (released 2026-06-11) — `async func`, `stream<T>`, `future<T>` in the Canonical ABI,
   so guest async maps onto Quark coroutines instead of a thread per call.
3. **WebAssembly 3.0** core features (completed 2025-09-17) — at minimum **exception handling**
   (real C++ libraries assume exceptions, which the C/C++ plugin track needs) and **memory64**
   (removes the 4 GB ceiling for data-heavy codecs and interpreters). GC and tail calls widen which
   languages can author plugins.

Today that is **Wasmtime 46+**, which ships WASI 0.3 with Component Model Async on by default.

**`wasm3` is explicitly not a candidate for this role.** It is an excellent interpreter — the
cold-start winner, tiny, portable down to microcontrollers — but it has only *partial* WASI Preview 2
and Component Model support, which is the one axis that matters here. On the features this design
depends on it has fewer, not more. It may still earn a scoped place for ultra-short guest calls or
constrained deployments, behind the same host interface and behind a gate; that was
[OQ-12](OpenQuestions.md), now resolved (2026-08-03,
`decisions/ADR-008-wasm3-cold-start-vs-wasmtime.md`): a real, measured cold-start comparison on this
project's own trivial-workload shape found wasm3 160-180x faster at p50 and 250-400x at p99 — the
gate this paragraph named is cleared, so wasm3 is now a real candidate for that scoped niche, not a
hypothetical one. This paragraph's rejection of wasm3 **as the primary runtime** is unchanged — the
measurement is silent on Component Model/WASI 0.3 support, which is why the primary-runtime decision
above stands regardless. Whether to actually build the scoped second runtime is a separate decision
the ADR does not make. Evidence:
[`docs/research/2026-wasm-runtime-and-state-persistence.md`](docs/research/2026-wasm-runtime-and-state-persistence.md) §1.

**Where this leaves the engine (OQ-12 gate cleared, 2026-08-04): the measurement above makes wasm3
a real candidate for a scoped niche — it does not, by itself, commit the engine to shipping one.**
The question as framed weighed wasm3's real cold-start/footprint advantages against "two
sandbox-escape surfaces and two conformance stories" as an engine-maintained cost — §2a already
built the mechanism that lets a deployer capture that advantage without forcing the cost onto the
core project: `SandboxBackend` is an open seam, and a deployer with a genuine
ultra-short-guest-call or microcontroller-footprint need can supply a wasm3-backed custom backend
themselves today, to the same §9 gate bar any custom backend must clear. This is the identical move
§1's `native-jail`-first priority already makes for stronger-than-`native-jail` isolation, applied
to the opposite end of the cold-start/footprint axis — the engine backs a small number of profiles
well; the open seam absorbs the rest, at least until an engine-maintained wasm3 backend is
separately proposed and gated. **Whether the project itself builds and maintains a first-party
wasm3 backend — taking on the second conformance story — remains a separate, still-open decision;**
ADR-008 supplies the evidence such a proposal would need, it does not make the call. A deployer
choosing the custom-backend route today inherits the responsibility §2a already states — verifying
their specific plugins only exercise the WASI P2/Component Model subset wasm3 actually implements,
since the plugin ABI's Component Model requirement (above) doesn't relax for them.

### 1b. The sandbox is the whole execution environment, not a wrapper around Python

For `native-jail` — the profile CodeAct and Shell actually run under — the boundary that matters is
not "an OS jail around a Python process." It is one composed unit, and CPython is one component of
it, not the boundary itself:

```
Sandbox (native-jail instance, per session, 010 §3a)
├── Worktree mount           — the VFS the guest sees (025)
├── ShellRunner              — the virtual shell (010 §1a, §2)
├── PythonRunner             — embedded CPython (010 §1a) — a component, not the boundary
├── ResourceLimits           — cpu_ms, wall_ms, memory_bytes, pids, fds, disk_bytes, output_bytes
├── CapabilitySet            — the permission manager (007) every access is checked against
├── NetPolicy                — deny-all default, host-mediated egress
└── Runner/Tool registry     — what a name can resolve to (010 §2, 006, 009)
```

**The mechanism is an allowlist over what can be imported, not a blocklist of dangerous calls.**
Enumerating hazardous entry points — `open`, `socket`, `subprocess`, `ctypes`, and then `mmap`,
`multiprocessing`, `os.fork`/`os.posix_spawn`, `signal`, `resource`, or an arbitrary native
`.pyd`/`.so` extension imported by name — and patching each one is how blocklists rot: the next one
nobody thought of is a matter of when, not if, and a native extension's own C code is invisible to
any Python-level wrapper regardless. Instead:

1. **The importable module set is closed by construction (primary).** At embedding time, only
   modules present in the granted package policy (010 §5) plus a fixed, reviewed stdlib
   safe-subset exist to `import` at all — `sys.modules`/`__import__` see a set the spec granted,
   never an open one with holes patched into it after the fact. A name outside that set raises
   `ImportError`, the ordinary shape a missing package already takes (010 §5), not a caught security
   exception. `ctypes` and arbitrary native extensions are simply absent unless explicitly granted —
   the same "ungranted module is absent" pattern 026 §5 already uses for the `agent` library.
2. **Modules that *are* allowed still have their dangerous behaviour mediated (also primary, not a
   fallback).** `open` resolves against the worktree mount and `FsRead`/`FsWrite` (025 §5) because
   `os`/`builtins` are always in the allowlist; `socket` proxies through the same host-mediated
   egress every other profile uses (§4); running a program is `ShellRunner`/another `Tool` via a
   declared `RunnerCall`/`ToolCall` (010 §1a, 006), never a raw fork, so `subprocess`/`os.system`
   exist as names but not as a way to reach an ungranted effect. A call the capability set does not
   cover raises the exact `PermissionError`/`OSError` 026 §3 already specifies, **before any syscall
   is attempted** — a precise, attributable denial (I4) instead of whatever the kernel happens to
   report.
3. **Kernel-level jail (backstop).** The OS-level boundary (seccomp-BPF/namespaces, AppContainer,
   sandbox profile) remains exactly as specified in §3 — for a bug in layers 1–2, or anything an
   allowed module does that its wrapper did not anticipate. It is the backstop, not the primary
   mechanism, which is why layers 1–2 exist at all: relying solely on the kernel produces a raw,
   unattributable failure far from the capability system; relying solely on interpreter mediation has
   no answer for a bug in the mediation itself.

This is the same idiom `ShellRunner` already uses (010 §2): remove the ambient path at the point of
use rather than grant it and try to contain what it can reach. Applying it to the interpreter, not
only the shell, is what makes "the sandbox" mean the whole execution environment §1b diagrams above,
with CPython as one tenant of it — never a synonym for "the Python process is isolated," which is a
narrower and weaker claim than this RFC makes.

## 2. The contract

```cpp
struct SandboxBackend {                              // concept
    static constexpr ProfileTraits traits;           // declared strength, cold-start class, platforms
    ae::task<result<SandboxHandle>> create(SandboxSpec, EffectContext&);
    ae::task<result<ExecOutcome>>   exec(SandboxHandle&, ExecRequest, EffectContext&);
    ae::task<>                      destroy(SandboxHandle&);
};

struct SandboxSpec {
    CapabilitySet   capabilities;     // 007 — empty unless explicitly granted
    ResourceLimits  limits;           // cpu_ms, wall_ms, memory_bytes, pids, fds, disk_bytes,
                                      // net_bytes, output_bytes
    Mounts          mounts;           // {host path or blob store} → guest path, ro/rw, quota
    NetPolicy       net;              // deny-all default; allowlist of host:port:scheme
    Determinism     determinism;      // clock, RNG, and env visibility
    Lifetime        lifetime;         // per_exec | per_run | per_session, + idle timeout
};
```

**`cpu_ms` is best-effort, not a dependable bound, on Windows `native-jail`.** Measured evidence
(`decisions/ADR-004-appcontainer-native-jail-windows-backend.md` §10): `JOB_OBJECT_LIMIT_JOB_TIME`
fired automatically in only 3 of 11 runs, with 1.38x-8.22x overrun when it did, and no relationship
between the configured budget and the actual overrun. **A caller that needs a dependable CPU/time
bound sets `wall_ms` to the value it actually wants enforced** — that field, not `cpu_ms`, is what
this backend's wall-clock watcher reliably enforces (measured at 500-504ms for a 500ms deadline,
independent of any native kernel limit). This is a per-backend reliability difference, not a
contract violation: §9 G1 still requires identical *outcome classification* across backends (a
CPU-bound guest is always killed and always reported as resource-exceeded), not identical enforcement
latency or an identical trustworthy mechanism per platform.

**Every backend must provide, or it is not a backend:**

1. **Empty-by-default authority.** Nothing is reachable that the spec did not grant (**I2**).
2. **Enforced resource limits**, including a hard wall-clock kill that cannot be evaded by the
   guest, and enforcement of output size (an unbounded stdout is a denial-of-service on the host).
3. **Structured outcomes, never host faults.** Timeout, OOM, crash, policy violation, and escape
   attempt are `ExecOutcome` values. No exception, no signal, no host crash crosses the seam.
4. **Full teardown.** Destroy reclaims memory, files, processes, sockets, and handles. Leaked
   guests are a defect class with a dedicated test.
5. **Attribution.** Every exec emits a span + audit record with capabilities used (**I4**).
6. **Recordable.** Inputs, outputs, exit status, and artifact digests are recordable for replay
   (**I5**).
7. **Cancellation.** A `stop_token` terminates the guest within a bounded, measured time.

**No backend is permitted to weaken the contract by configuration.** A profile that cannot enforce a
limit does not offer it; it does not offer it and ignore it.

## 2a. Custom backends — the seam is open, not a closed set

§3's table is what the engine ships, not the exhaustive set of what `SandboxBackend` can be. Nothing
about the concept in §2 is engine-private, and the mechanism to use a custom one is already there
with no new machinery: an agent selects its profile via the compile-time `SandboxProfile<P>` policy
(002, §3 below), and `P` is any type satisfying the `SandboxBackend` concept — a deployer's own type
wrapping gVisor, Kata, a future microVM backend, or anything else that fits their environment works
exactly the same way `native-jail` or `remote` does, because from the engine's point of view there is
no difference.

- **A custom backend is host-trust-tier code, not sandboxed code.** It runs unsandboxed as part of the
  host, the same trust tier as first-party native tools (007 §6 T0) — the engine does not attempt to
  contain the thing that creates and manages containment. Supplying one is an operator/deployer trust
  decision, exactly like linking in any other native dependency.
- **The bar to meet is the same bar §9 sets for the built-in profiles**, not a lower one for being
  third-party: G1's cross-backend outcome-parity requirement, G2's containment corpus with a positive
  control, G3's no-ambient-authority probe, and the rest. The engine cannot enforce this on code it
  did not write, so this is stated as the standard a custom backend must be held to, not a check the
  engine runs automatically — the same posture 007 §7 already takes toward third-party plugin trust
  (declared, operator-approved, not runtime-verified).
- **No special status for engine-shipped profiles beyond having already cleared that bar.**
  `native-jail`, `wasm`, and `remote` are not privileged by the contract — they are simply the three
  the project has done the work (and, for `native-jail`, the ADR-proven measurement) to back.

## 3. Profiles

The four the engine ships and backs with a promotion gate (§9) — not the exhaustive set §2a permits.

| Profile | Backend | Boundary | Cold start | Platforms | Intended use |
|---|---|---|---|---|---|
| **`wasm`** | wasmtime, WASI 0.3 components | Software; capability-based, no ambient authority | µs–low ms | Win · Linux, identical (021 §2 — macOS not targeted) | Plugins (default, 009), deterministic execution, replay, hostile multi-tenant *plugin* code that fits the WASI surface |
| **`native-jail`** | OS process jail + interpreter-level mediation (§1b) | Kernel-enforced, per-OS, plus mediated at the point of use | ms | Win (AppContainer + Job Object + restricted token) · Linux (namespaces + seccomp-BPF + cgroups v2 + no_new_privs), next | The code interpreter and shell (010), full-ecosystem Python, on trusted-tenant deployments — the default |
| **`remote`** | Kubernetes **Agent Sandbox** CRD, or a vendor sandbox API | Cluster-side, backend-decoupled — may itself use hardware isolation | network + backend | Any (client side) | Production scale-out, cluster-managed lifecycle, pause/resume, and hostile/untrusted-multi-tenant workloads that need a stronger boundary than `native-jail` offers — this profile, not one we build, is where that strength comes from |
| **`none`** | In-process | **No boundary** | 0 | All | First-party trusted code only; **refuses to load T2/T3 code** (007 §6) |

**Profile resolution.** An agent declares `SandboxProfile<P>` (002). At startup, the engine resolves
`P` to an available backend on this platform. If unavailable:

- with a declared fallback chain (e.g. `native-jail → remote`), it takes the first available and
  **records the downgrade** in startup diagnostics, run traces, and metrics;
- with no fallback, **startup fails**. Silently running unisolated because the preferred isolation
  was unavailable is the single worst failure mode in this design, and it is prohibited.

**`Profile::Strict`** is the named alias meaning "the strongest profile available on this platform,
never `none`" and is the default for every agent (002 §3).

**Resolving `Profile::Strict`.** The table above sits on incomparable axes (software
capability-based, kernel-enforced, cluster-delegated) — its boundary column alone gives no order, so
"strongest" needs an explicit rule rather than an eyeballed reading. Each backend's `ProfileTraits`
(§2) declares a `strength` rank; `Profile::Strict` resolves to the highest-`strength` profile whose
platform list includes the current platform (`none` excluded by definition regardless of rank), with
ties — two available profiles at the same declared strength — broken toward whichever has the
broader, more-proven platform support, since a rank that doesn't discriminate should not be allowed
to make the choice arbitrary.

**What `SandboxProfile<P>` does and does not govern.** This table is what an agent's
`SandboxProfile<P>` selects among for its own script-executing tools — it does **not** redirect
plugins, which run in `wasm` unconditionally (009 §6), or the code interpreter, which is locked to
`native-jail` permanently (§1, CLAUDE.md). See 002 §3 for the full statement of that boundary and
002 §6 for the validation rule it implies (a tool whose backend requirement is incompatible with the
agent's declared profile fails registration).

## 4. Capability enforcement per backend

| Capability | `wasm` | `native-jail` | `remote` |
|---|---|---|---|
| `FsRead`/`FsWrite` | preopened dirs (no path escape by construction) | bind/junction mounts + path canonicalization + FS restrictions, **and** interpreter-level mediation of `open` (§1b) | volume grant |
| `NetOut` | host-mediated: guest has no sockets; egress via a host proxy that enforces the allowlist | network namespace + proxy; Windows: WFP/AppContainer capability, **and** interpreter-level mediation of `socket` (§1b) | network policy |
| `Secret` | never mounted; resolved host-side at point of use, injected per-exec only if granted | same | same |
| `ToolCall` | host function import (WIT) | `RunnerCall`/`ToolCall` dispatch (§1b, 010 §1a) — never a raw fork | API callback |
| `Clock`/`Entropy` | virtualizable (determinism mode) | limited virtualization | backend |
| `Exec` (nested) | denied | denied — `subprocess`/`os.system`/`os.exec*` do not exist as a way to run something (§1b) | denied |

**Two rules the table exists to make explicit:**

- **Egress is always host-mediated.** No profile hands a guest a raw socket. This is the only way
  `NetOut<host>` means the same thing everywhere, and it is what makes egress auditable.
- **Nested sandbox creation is denied in every profile.** A guest cannot create a guest.

## 4a. Remote callback authentication (resolves OQ-3 for this RFC's part of it)

The `remote` row in §4's table is deliberately vague — "volume grant," "network policy," "API
callback" — because a cluster-side backend materializes mounts and network policy through its own
control plane (a CRD spec, a vendor API call) at `Exec` creation, out of band from anything crossing
the guest boundary at runtime. What that leaves genuinely open is narrower than "how do capabilities
cross a process boundary" in general: **when a `remote` sandbox instance calls back to the host** —
for `Secret` resolution, `ToolCall` dispatch, or any effect this RFC's "egress is always
host-mediated" rule routes through the host — **how does the host know which `Exec` invocation, and
therefore which already-computed `SandboxSpec`, that callback belongs to**, given the backend is not
a process the host directly mediates syscalls for the way `native-jail` is?

**Decision: `RemoteExecToken` is the self-verifying, macaroon-style bearer token ADR-005 proved out
(Design A), not an opaque lookup key into a host-side registry.** An earlier draft of this section
argued for the opaque-reference alternative (Design B) specifically to avoid taking on a crypto
dependency and a caveat-parsing forgery surface; `decisions/ADR-005-capability-bearer-tokens-cross-
process.md` (007 §10 Q1, executed, red-teamed, ASan-clean) settled that argument the other way for
capabilities crossing a process boundary generally, and `RemoteExecToken` is that mechanism applied
to this RFC's specific callback-authentication problem, not a separate bespoke protocol.

```
RemoteExecToken = CapabilityToken{
    capability_kind = remote_exec_callback,
    param           = { exec_id, run_id },
    caveats         = [ ExpiresAt(expires_at) ],
    signature       = HMAC-chain over the above (root key held only by the minting host)
}
```

(`include/agentengine/trust/capability_token.hpp`, `src/trust/capability_token.cpp` — the same
`mint_root`/`attenuate`/`verify` triple ADR-005 §3 Design A specifies.)

- Minted by the host at the same point it constructs the `SandboxSpec` for an `Exec`, handed to the
  remote backend alongside (not instead of) the out-of-band spec delivery above.
- **`verify` is a pure local computation.** The host recomputes the HMAC chain from its own root key
  and the token's own `(capability_kind, param, caveats)` and compares against `signature` in
  constant time — no round trip to any external party is structurally required to establish
  authenticity (ADR-005 claim A5), unlike the opaque-reference design this section previously
  specified. A bit-flipped signature, a tampered `exec_id`/`run_id`, or a widened/stripped
  `ExpiresAt` caveat all fail `verify` the same way ADR-005 §5/§6's red-team corpus (R-A1–R-A6)
  proved for the general mechanism.
- **Immediate revocation on `destroy` still needs a liveness check, because a self-verifying token
  structurally cannot be "unminted" early (ADR-005 §6 claim B3, §8).** `destroy` (§2's backend
  contract, full teardown) removes the exec's entry from the host's own live-exec table the instant
  teardown completes; a callback presenting a token that verifies cryptographically but names an
  `exec_id` no longer live is still rejected. This is the same liveness bookkeeping the previous
  design used, layered on top of a token that also carries real, self-verifying authority rather
  than none — the host is both the minting and the verifying party here, so this liveness check
  costs nothing beyond what teardown accounting already does; it is not a reintroduction of Design
  B's registry-as-the-source-of-authority, only of its revocation property, exactly as ADR-005 §8
  recommends for capabilities that need immediate revocation.
- **`expires_at`** is bound to the `SandboxSpec.lifetime` already declared for that exec, as a
  bound on how long a token can outlive a liveness check that might be delayed or lost, not as the
  primary revocation mechanism — the live-exec check above is.

**Scope:** this mechanism is specific to `SandboxBackend` instances (008) and generalizes to any
future host-external execution surface built the same way (a remote-hosted plugin instance, 009,
should it ever exist, is the same shape — compute the host doesn't syscall-mediate, calling back
over a channel that always reaches the host). It is not a change to `Capability` (007 §3): those
handles remain host-process-only, unforgeable by private construction, and never serialized or
transmitted. `RemoteExecToken` authenticates a callback's origin; it is never the source of truth
for what that origin may do.

## 5. Determinism and replay

The `wasm` profile supports a **deterministic mode**: virtual clock, seeded RNG, no ambient env, no
egress — making execution a pure function of `(component, inputs, capabilities)`. Combined with
content-addressed inputs (003 §3), the outcome is cacheable by digest and replayable offline (I5).

Other profiles are recordable but not deterministic; replay serves recorded outputs. The spec is
honest about the difference rather than claiming determinism it cannot enforce.

## 6. Lifetime, pooling, and state

The isolation boundary that matters is **between sessions**, not between executions within one
session. A session is one principal, one conversation, one capability set; keeping a live sandbox
for its duration is what makes a conversation feel continuous rather than amnesiac.

- **Default lifetime is `per_session`**, bound to the session's Quark actor: created on first use,
  retained while the session is active, destroyed when the session ends. `per_run` and `per_exec`
  remain available and are the right choice for one-shot or adversarial workloads.
- **Cross-session reuse is prohibited in every profile.** A guest instance is never handed to a
  different session or principal. This is the line that pooling may not cross.
- **Pooled instances are reset to a snapshot taken before any untrusted input** — never merely
  "cleaned". A pool is a performance mechanism, not a state-sharing mechanism.
- **Files do not depend on any of this.** Persistence across turns, restarts, and profiles is the
  **worktree** (025), which is durable engine state mounted into the sandbox. Destroying a sandbox
  loses no files, on any profile.

### 6a. Surviving passivation

Quark passivates idle sessions (ADR-028/034). What happens to in-memory interpreter state then is
**profile-dependent, and the difference is stated rather than papered over**:

| | `wasm` (plugins, 009) | `native-jail` (interpreter and shell, 010) |
|---|---|---|
| Files across passivation | Worktree — always | Worktree — always |
| In-memory state across passivation | **Snapshot and restore** | **Lost**; session resumes with a clean interpreter and shell (010 §3a) |

The `wasm` snapshot is **only taken at quiescent points** — between executions, when no guest stack
exists — at which moment a component's heap *is* its linear memory, so a snapshot is a memory dump
plus store/table/global state. Wasmtime provides no built-in snapshot API (the upstream requests
date to 2021–2022 and remain open), so this is ours to implement; it is portable because linear
memory is linear memory on every OS. It is a real property of the `wasm` profile, available to
**plugins** that use it (009).

There is no portable equivalent for native processes: CRIU is Linux-only and, by its own
documentation, frequently fails on live network connections and device-mapped workloads. We
therefore do not offer a cross-platform contract we cannot keep.

**This is an accepted, permanent limitation for the interpreter and shell, not a gap to close by
picking a different backend for them.** §1's decision already rules out a WASM Python runtime for
the reasons stated there; this is the cost of that decision made explicit rather than left as a
surprise discovered later. A session's Python/shell state does not survive passivation on any
profile it can run under — only the worktree does. Evidence for the wasm-side property:
[`docs/research/2026-wasm-runtime-and-state-persistence.md`](docs/research/2026-wasm-runtime-and-state-persistence.md) §2.

## 7. Failure and abuse

Handled, tested, and named:

fork bombs · OOM · infinite loops · unbounded output · filesystem quota exhaustion · symlink and
`..` path escape · TOCTOU on mounts · DNS-rebinding around an egress allowlist · SSRF to link-local
metadata endpoints (`169.254.169.254` and friends — **blocked by default in every profile**) ·
guest→host time manipulation · resource exhaustion of the host by many concurrent sandboxes ·
**read-access leak via inherited OS-default ACEs on `native-jail`/Windows** — a curated set of host
files (e.g. `win.ini`, `drivers\etc\hosts`) carry `ALL APPLICATION PACKAGES`/`ALL RESTRICTED
APPLICATION PACKAGES` read grants by Windows' own default, independent of any capability this
profile grants or withholds (`decisions/ADR-004-appcontainer-native-jail-windows-backend.md` §6.1) —
this is exactly why §1b makes interpreter-level `open()` mediation primary rather than relying on the
kernel jail's ACL model for reads · **no filesystem containment yet on `native-jail`/Linux** — the
M2 Phase C build gives the guest a private mount namespace (`CLONE_NEWNS`) but nothing populates it
with a restricted view (no `pivot_root`/`chroot`/bind-mount jail), so the guest can read/write
anything the invoking user can, anywhere on the host; namespaces/cgroups/seccomp contain process
count, memory, and syscalls, but not paths. Tracked gap, not silently dropped
(`docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md` C3's writeup, GitHub issue
tracking the follow-up); the fs-escape-attempt abuse case below is proven on Windows only until this
closes.

Each has a test in the hostile suite. Per CONVENTIONS, hostile tests are themselves resource-capped
so they cannot take the dev box down.

## 8. Observability

Per exec: profile, backend, cold/warm, create/exec/destroy durations, peak memory, CPU ms, bytes
in/out, egress hosts contacted, capabilities used, outcome class. Metrics carry the profile so a
downgrade shows up as a graph change, not as a surprise in an incident review.

## 9. Promotion gate

- **G1 (parity)** — one hostile test corpus runs against every available backend on every platform
  in the current target set (021 §2 — Windows now, Linux next) and produces the **same outcome
  classification** for every case. Semantics identical, strength documented.
- **G2 (containment)** — every §7 abuse case is contained, with a measured kill time, and a
  positive control (limits deliberately disabled) demonstrably fails — so the test is not vacuous.
- **G3 (no ambient authority)** — a probe guest enumerating filesystem, network, env, and processes
  finds exactly the granted set and nothing else, on each backend.
- **G4 (teardown)** — 10⁵ create/exec/destroy cycles leak no memory, no handles, no processes, no
  temp files (ASan + handle/pid census).
- **G5 (cold start)** — measured p50/p99 create+exec per profile against the 023 budgets.
- **G6 (downgrade visibility)** — an unavailable profile with no fallback fails startup; with a
  fallback, the downgrade appears in diagnostics, trace, and metrics. Proven by a negative test.
- **G7 (session boundary)** — a guest instance is never reused across sessions or principals; a
  canary written in session A is unreachable from session B on every profile, including under
  pooling and snapshot-restore. Positive control: a deliberately shared pool is caught.
- **G8 (snapshot fidelity)** — under `wasm`, a stateful plugin instance (009) passivated
  mid-conversation and reactivated resumes with its linear memory intact (globals, any in-guest
  state) and a worktree byte-identical to the pre-passivation state; a snapshot attempted at a
  non-quiescent point is refused rather than producing a corrupt image. Does not apply to
  `native-jail`'s interpreter/shell state, which §6a states plainly does not survive passivation.

## 10. Open questions

- ~~**Q1** — macOS `microvm`: Virtualization.framework is plausible but unproven here.~~
  **Resolved:** `microvm` is dropped as a profile entirely (§1); this question no longer applies.
- ~~**Q2** — Hyperlight as an option behind the `microvm` profile.~~ **Resolved:** `microvm` is
  dropped as a profile entirely (§1); Hyperlight is not adopted. A workload that would have reached
  for it belongs on the `remote` profile instead.
- ~~**Q3** — Whether the egress proxy should be a first-party component or a host-provided seam.~~
  **Resolved, both — a real first-party default, and a seam a deployer may replace (2026-08-04):**
  this can't be a pure "deployer supplies it" seam with an insecure/no-op default the way Quark's
  `SecureTransport` crypto exception is, because §7/§9 G2 already commit this RFC to SSRF and
  DNS-rebinding defense being *tested, contained-by-default* properties of the engine ("blocked by
  default in every profile") — a claim that's either false or untestable on a base install with no
  reference implementation behind it. Unlike TLS, egress mediation's core job (host/port/scheme
  allowlist enforcement, blocking RFC 1918/link-local/metadata ranges, re-resolving and re-checking
  addresses post-connect against DNS rebinding) is ordinary host-side socket/DNS logic — it doesn't
  need a vetted third-party crypto library the way `SecureTransport` does, so there's no equivalent
  "don't roll your own" argument forcing a thin-adapter-only shape. It also has to be cross-cutting
  by §4's own rule ("egress is always host-mediated," uniformly across `wasm`/`native-jail`/`remote`),
  so leaving it purely to each deployer would fragment the one property that's supposed to mean the
  same thing everywhere. Ships first-party, gate-tested (§9 G2); stays a seam underneath (the same
  `SandboxBackend`-is-a-concept pattern §2a already uses) for a deployer who wants to substitute a
  corporate proxy, an existing SSRF appliance, or an mTLS-terminating gateway.
- ~~**Q4** — Whether capabilities should cross to the `remote` profile as bearer tokens (007 Q1).~~
  **Resolved — see 007 §10 Q1 and `decisions/ADR-005-capability-bearer-tokens-cross-process.md`.**
  Capabilities do cross a process boundary, as narrowly-scoped, self-verifying bearer tokens: a
  macaroon-style HMAC-chain construction with caveats such as `ExpiresAt`/`PathPrefix`, executed and
  red-teamed, ASan-clean (`include/agentengine/trust/capability_token.hpp`,
  `src/trust/capability_token.cpp`). §4a's `RemoteExecToken` is this mechanism applied to the
  `remote` profile's callback-authentication problem specifically.
- ~~**Q5** — GPU access from a sandbox (needed for local inference plugins) has no good story in any
  profile and is currently out of scope.~~ **Resolved, out of scope for `wasm`/`native-jail` by
  construction, not a gap to close there — `remote` absorbs it (2026-08-04):** the third application
  of a pattern already used twice in §1 (microVM-class hardware isolation, wasm3's footprint niche):
  the engine backs a small number of profiles well and lets the open seam absorb what those profiles
  structurally can't do. GPU device passthrough into a sandboxed guest is exactly that kind of
  workload — a `remote`-profile case (§3's table already scopes it as "backend-decoupled... may
  itself use hardware isolation") against infrastructure that already solves GPU device access
  (Kubernetes GPU device plugins, cloud GPU instances), not something `wasm`/`native-jail`'s
  capability model should grow bespoke plumbing for. No speculative `Gpu<...>` capability is added to
  007 §3 — that would be exactly the unfalsifiable, no-implementation-behind-it design CLAUDE.md
  warns against. If a concrete `remote` backend later needs to expose GPU access, that's the
  backend's own `SandboxSpec`/mounts shape to extend (§2a's custom-backend seam already permits this
  without an engine change), not a v1 commitment. Local (in-process) GPU inference stays out of
  scope entirely for v1 — point the agent's `ChatClientId` at a remote-hosted inference server via
  the ordinary OpenAI-compatible backend (004 §3) instead. Same duplicate resolution written into
  010 §10 Q4.
