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
- **The Python code interpreter does not default to WASM** (010). As of July 2026 the rich
  Python-on-WASM ecosystem is **Emscripten**-targeted and requires a JavaScript host, so it cannot
  be embedded via wasmtime; the embeddable **WASI** CPython has no sockets, no threads, no wheel
  platform tag, and no binary-wheel ecosystem. A WASI-Python interpreter today cannot
  `import numpy`, which is most of the reason to have one.

The contract below is what makes that split safe: profiles differ in backend, never in semantics.

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
| **`wasm`** | wasmtime, WASI 0.3 components | Software; capability-based, no ambient authority | µs–low ms | Win · Linux · macOS, identical | Plugins (default), deterministic execution, replay, hostile multi-tenant code that fits the WASI surface |
| **`native-jail`** | OS process jail | Kernel-enforced, per-OS | ms | Win (AppContainer + Job Object + restricted token) · Linux (namespaces + seccomp-BPF + cgroups v2 + no_new_privs) · macOS (sandbox profile + resource limits) | Full-ecosystem Python interpreter (010) on trusted-tenant deployments |
| **`microvm`** | KVM (Linux) / WHP (Windows); Firecracker-class or Hyperlight-class | Hardware, own kernel or micro-guest | ~2 ms (micro-guest) – hundreds of ms (full kernel) | Win · Linux; macOS **not supported** | Hostile input + full ecosystem; strongest local boundary |
| **`remote`** | Kubernetes **Agent Sandbox** CRD, or a vendor sandbox API | Cluster-side, backend-decoupled | network + backend | Any (client side) | Production scale-out, cluster-managed lifecycle, pause/resume |
| **`none`** | In-process | **No boundary** | 0 | All | First-party trusted code only; **refuses to load T2/T3 code** (007 §6) |

**Profile resolution.** An agent declares `SandboxProfile<P>` (002). At startup, the engine resolves
`P` to an available backend on this platform. If unavailable:

- with a declared fallback chain (e.g. `microvm → native-jail`), it takes the first available and
  **records the downgrade** in startup diagnostics, run traces, and metrics;
- with no fallback, **startup fails**. Silently running unisolated because the preferred isolation
  was unavailable is the single worst failure mode in this design, and it is prohibited.

**`Profile::Strict`** is the named alias meaning "the strongest profile available on this platform,
never `none`" and is the default for every agent (002 §3).

## 4. Capability enforcement per backend

| Capability | `wasm` | `native-jail` | `microvm` | `remote` |
|---|---|---|---|---|
| `FsRead`/`FsWrite` | preopened dirs (no path escape by construction) | bind/junction mounts + path canonicalization + FS restrictions | virtio-fs / injected image | volume grant |
| `NetOut` | host-mediated: guest has no sockets; egress via a host proxy that enforces the allowlist | network namespace + proxy; Windows: WFP/AppContainer capability | tap + host proxy | network policy |
| `Secret` | never mounted; resolved host-side at point of use, injected per-exec only if granted | same | same | same |
| `ToolCall` | host function import (WIT) | RPC over a controlled channel | vsock/virtio channel | API callback |
| `Clock`/`Entropy` | virtualizable (determinism mode) | limited virtualization | limited | backend |
| `Exec` (nested) | denied | denied | denied | denied |

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

- **Default lifetime is `per_exec`**: a fresh boundary per execution, and **in-memory guest state
  never persists across executions**. Persistence across executions is via the workspace mount —
  external state, explicitly granted — matching the CodeAct contract MAF settled on.
- `per_run` / `per_session` lifetimes exist for cost reasons and **must be declared**, because a
  reused guest is a cross-execution information channel.
- **Pooling and snapshot-restore** are performance mechanisms that must not weaken §6's first bullet:
  a pooled instance is reset to a snapshot taken before any untrusted input, never merely "cleaned".

## 7. Failure and abuse

Handled, tested, and named:

fork bombs · OOM · infinite loops · unbounded output · filesystem quota exhaustion · symlink and
`..` path escape · TOCTOU on mounts · DNS-rebinding around an egress allowlist · SSRF to link-local
metadata endpoints (`169.254.169.254` and friends — **blocked by default in every profile**) ·
guest→host time manipulation · resource exhaustion of the host by many concurrent sandboxes.

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

## 10. Open questions

- **Q1** — macOS `microvm`: Virtualization.framework is plausible but unproven here. Until proven,
  macOS strict = `native-jail`, and the parity table says so.
- **Q2** — Hyperlight is attractive (<2 ms, Windows WHP + Linux KVM/mshv, CNCF Sandbox) but its own
  project describes it as experimental and not production-grade. Adopt as an *option* behind the
  `microvm` profile; do not make it the default until a gate proves it.
- **Q3** — Whether the egress proxy should be a first-party component or a host-provided seam.
- **Q4** — Whether capabilities should cross to the `remote` profile as bearer tokens (007 Q1).
- **Q5** — GPU access from a sandbox (needed for local inference plugins) has no good story in any
  profile and is currently out of scope.
