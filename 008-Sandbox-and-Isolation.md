# 008 — Sandbox and Isolation

**Status:** Draft · **Depends on:** 007, 009, 021 · **Gate:** §9 · **Research:** [2026 landscape §6–8](docs/research/2026-standards-landscape.md)

## Goal

Define **one isolation contract** and the named profiles that implement it, so that untrusted
execution — plugins, generated code, third-party tools — has the same capability semantics, the
same limits, the same failure shapes, and the same audit trail on Windows, Linux, and macOS, while
the *strength* of the boundary remains a deployment choice.

## 1. The decision, and why

**Isolation is a seam with named profiles, not a hardcoded technology.** No single technique wins
on all of {boundary strength, cold start, portability, ecosystem} — the 2026 trade space is
tabulated in the research record §8 — and the axis a deployment cares about differs by deployment.

Two things are nonetheless **locked**, because leaving them open would leave the whole design open:

- **The WASM Component Model (WASI 0.3) is the plugin ABI** (009). One artifact runs bit-identically
  on all three OSes, holds no ambient authority by construction, instantiates in microseconds, and
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
  It is dropped: it has no macOS backend, and a workload that genuinely needs hardware-level
  isolation is better served by the `remote` profile against infrastructure that already does this
  well (e.g. Kubernetes Agent Sandbox CRD backed by Kata/Firecracker) than by AgentEngine building
  and cross-platform-maintaining a second local isolation technology of its own. `native-jail`
  stays a kernel-enforced, per-OS boundary — §1b adds interpreter-level mediation as defense in
  depth on top of it, not a claim that it now matches hardware isolation.

The contract below is what makes that split safe: profiles differ in backend, never in semantics.

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
constrained deployments, behind the same host interface and behind a gate; that is
[OQ-12](OpenQuestions.md), not a plan. Evidence:
[`docs/research/2026-wasm-runtime-and-state-persistence.md`](docs/research/2026-wasm-runtime-and-state-persistence.md) §1.

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

## 3. Profiles

| Profile | Backend | Boundary | Cold start | Platforms | Intended use |
|---|---|---|---|---|---|
| **`wasm`** | wasmtime, WASI 0.3 components | Software; capability-based, no ambient authority | µs–low ms | Win · Linux · macOS, identical | Plugins (default, 009), deterministic execution, replay, hostile multi-tenant *plugin* code that fits the WASI surface |
| **`native-jail`** | OS process jail + interpreter-level mediation (§1b) | Kernel-enforced, per-OS, plus mediated at the point of use | ms | Win (AppContainer + Job Object + restricted token) · Linux (namespaces + seccomp-BPF + cgroups v2 + no_new_privs) · macOS (sandbox profile + resource limits) | The code interpreter and shell (010), full-ecosystem Python, on trusted-tenant deployments — the default |
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
kernel jail's ACL model for reads.

Each has a test in the hostile suite. Per CONVENTIONS, hostile tests are themselves resource-capped
so they cannot take the dev box down.

## 8. Observability

Per exec: profile, backend, cold/warm, create/exec/destroy durations, peak memory, CPU ms, bytes
in/out, egress hosts contacted, capabilities used, outcome class. Metrics carry the profile so a
downgrade shows up as a graph change, not as a surprise in an incident review.

## 9. Promotion gate

- **G1 (parity)** — one hostile test corpus runs against every available backend on Windows and
  Linux (plus `wasm` + `native-jail` on macOS) and produces the **same outcome classification** for
  every case. Semantics identical, strength documented.
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
- **Q3** — Whether the egress proxy should be a first-party component or a host-provided seam.
- **Q4** — Whether capabilities should cross to the `remote` profile as bearer tokens (007 Q1).
- **Q5** — GPU access from a sandbox (needed for local inference plugins) has no good story in any
  profile and is currently out of scope.
