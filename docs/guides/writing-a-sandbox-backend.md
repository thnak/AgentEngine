# Writing a custom `SandboxBackend`

Audience: a consumer/deployer of AgentEngine (first-party or third-party) who wants to plug their
own isolation technology into `SandboxProfile<P>` — a Kubernetes-native orchestrator like
[`kubernetes-sigs/agent-sandbox`](https://github.com/kubernetes-sigs/agent-sandbox), a vendor
sandbox API, gVisor, Firecracker+jailer, or anything else satisfying the contract below. This is
the extension seam **008-Sandbox-and-Isolation.md** §2a names and
`docs/planning/sandbox-backend-registry-design-draft.md` §5 designs: "any type satisfying
`SandboxBackend` works exactly the same way `native-jail` or `remote` does" — you are not editing
AgentEngine's own code to add one.

If you're looking for the *first-party* backends already in this repo, see
[`src/backends/README.md`](../../src/backends/README.md). This guide is for writing your own,
outside or inside this tree.

## 1. The contract

`include/agentengine/sandbox/sandbox.hpp` defines `SandboxBackend` as a **concept, not a base
class** (008 §2 — no virtual dispatch, no vtable, zero-cost per this project's CRTP idiom):

```cpp
template <class T>
concept SandboxBackend = requires(T backend, SandboxSpec spec, SandboxHandle& handle,
                                   ExecRequest request, EffectContext& ctx) {
    { T::traits } -> std::convertible_to<ProfileTraits const&>;
    { backend.create(spec, ctx) } -> std::same_as<result<SandboxHandle>>;
    { backend.exec(handle, request, ctx) } -> std::same_as<result<ExecOutcome>>;
    { backend.destroy(handle) } -> std::same_as<void>;
};
```

Four things, no more:

- `static constexpr ProfileTraits traits` — see §2 below. Required even though it's not a method;
  without it `Profile::Strict` (008 §3) has nothing to rank your backend against.
- `create(SandboxSpec const&, EffectContext&) -> result<SandboxHandle>` — stand up one sandbox
  instance and return an opaque handle. `SandboxHandle::opaque_id` is yours to define; the core
  never interprets it.
- `exec(SandboxHandle&, ExecRequest const&, EffectContext&) -> result<ExecOutcome>` — run one
  request against an already-created instance. Called potentially many times against the same
  handle (`sandbox_lifetime::per_session` is the default in `SandboxSpec`) — **state must persist
  across calls on the same handle**. This was a real, fixed bug class in this codebase (both
  `SandboxBackendRegistry::register_backend()`'s Revision 1 sketch and the registry design draft's
  own red-team catalog it): closing over a fresh, default-constructed instance per call silently
  discards whatever your backend keeps in something like an `instances_` map, and on Windows a
  `JobObjectLimits`-style destructor can kill an in-flight process on the very call that spawned it.
  Keep your own `unordered_map<opaque_id, Instance>` (or equivalent) alive across calls, keyed by
  the handle you returned from `create()`.
- `destroy(SandboxHandle&) -> void` — idempotent, never throws. Called on an already-destroyed or
  unknown handle should be a silent no-op (`LinuxNativeJailBackend`/`KataBackend`'s own convention)
  — `void` means you have no way to surface a cleanup failure through the return type; if you need
  observability, log it (see `kata_backend.cpp`'s `destroy()` for the pattern: failed cleanup steps
  go to stderr so they're visible in host logs, not silently dropped).

Your type does **not** need to derive from anything, register anything with itself, or live in this
repo. A type in your own binary that satisfies this concept is already a valid `SandboxBackend`.

## 2. `ProfileTraits` — declare honestly, not aspirationally

```cpp
struct ProfileTraits {
    std::uint32_t     strength = 0;       // relative isolation strength — see below
    std::uint8_t      platform_mask = 0;  // OR of platform_id flags this backend runs on
    cold_start_class  cold_start = cold_start_class::milliseconds;
};
```

- **`strength`** feeds `Profile::Strict`'s resolution rule (008 §3: highest strength wins, ties
  broken toward broader platform support) — it has no fixed scale, but the existing first-party
  backends anchor it: `wasm` (software capability isolation) < `native-jail` (namespace/cgroup or
  AppContainer/Job-Object process boundary, **50**) < `KataBackend` (real hardware VM boundary via
  cloud-hypervisor, **90**). **Set this to what your backend's *default, unconfigured* isolation
  boundary actually provides — never to what the technology it wraps is theoretically capable of.**
  If your backend's isolation strength depends on deployment-time configuration (e.g. an
  orchestrator like `agent-sandbox` that can be pointed at gVisor *or* Kata *or* a plain
  `RuntimeClass` with no isolation at all via its `Sandbox` CRD), `strength` must reflect what your
  specific, constructed instance is actually configured to enforce — read that config in your
  constructor and set `traits` (or gate registration) accordingly, don't hardcode the best case.
  `KataBackend`'s own header comment is the model for this kind of honest disclosure: it names
  exactly which parts of `SandboxSpec` it does and does not enforce, rather than letting the
  `strength=90` line imply full 008 §2 conformance by itself.
- **`platform_mask`** — OR of `platform_id::windows_x86_64` / `platform_id::linux_x86_64` (021 §2's
  closed two-member target set; there is no third platform to add without an RFC change). If your
  backend needs a control-plane dependency only available on one OS (e.g. a Linux-only orchestrator
  CLI), the mask must say so — see `KataBackend::traits` for the pattern
  (`platform_mask = linux_x86_64` only).
- **`cold_start`** — pick the closest of `zero` / `microseconds_to_low_ms` / `milliseconds` /
  `network_dependent`. A cluster-managed backend (talking to a Kubernetes API server or a vendor
  API) is `network_dependent`, not `milliseconds`, even if the underlying VM boots in milliseconds
  once scheduled — the class describes what a caller should expect end-to-end, not the isolation
  technology's own boot time in isolation.

## 3. What you receive and must return

- **`SandboxSpec`** (`capabilities`, `limits`, `mounts`, `net`, `determinism`, `lifetime`) — 008
  §2's full contract. **Nothing in the `SandboxBackend` concept forces you to enforce every field.**
  `KataBackend` shipped in two disclosed slices specifically because of this: Slice 1 explicitly
  documented that `capabilities`/`mounts`/`limits`/`net` were no-ops (containment came entirely from
  Kata's own defaults), and Slice 2 (`decisions/ADR-086`) closed that gap as real, separate,
  reviewed work — not silently assumed done by Slice 1's mere existence. Do the same: if you don't
  enforce a field yet, say so in your header comment, the same way, rather than letting a caller
  assume `SandboxSpec::limits.memory_bytes` is respected because your `exec()` compiles against it.
- **`ExecRequest`** (`language`, `source`, `preseeded_answers`) — `source` is a caller-trusted
  string (already resolved from whatever mediation layer sits above you — this concept does not
  mediate `open()`/`socket()`/`subprocess` calls itself; that's `010-Python-Code-Interpreter.md`'s
  job, layered *above* your backend, e.g. `python_runner.hpp`/`shell_runner.hpp`).
  `preseeded_answers` is `agent.ask()` replay state (ADR-057 §9) — ignore it entirely if your
  backend never implements `agent.ask()`; that's a legitimate empty, not a gap.
- **`ExecOutcome`** (`klass`, `stdout_text`, `stderr_text`, `result_repr`, `artifacts`,
  `ask_prompt`) — `klass` is one of `ok | timeout | oom | crash | policy_violation |
  escape_attempt | ask_pending`. Distinguishing `timeout` from `oom` from `crash` honestly is real
  work when your backend doesn't control the guest kernel directly — see
  `linux_native_jail_backend.cpp`'s `exec()` for a worked example of disambiguating a cgroup OOM
  kill from a clean crash from a namespace-init 128+signal exit code, none of which alone was
  sufficient evidence. `artifacts`/`ask_prompt` are legitimately empty for most backends — only set
  them if you actually implement worktree-mount harvesting or `agent.ask()` replay.

## 4. Registering your backend

`include/agentengine/sandbox/sandbox_backend_registry.hpp`'s `SandboxBackendRegistry` is
**host-curated only** — same trust tier as `ToolRegistry`/`ChatClientRegistry`: nothing is ever
auto-discovered, scanned for, or self-registered from a plugin manifest (I2 — backend selection is
routing, not authorization). You call `register_backend()` yourself, at host startup, with an
already-constructed instance:

```cpp
auto instance = std::make_shared<MySandboxBackend>(/* your own config */);
registry.register_backend("my-backend", std::move(instance), strict_eligibility::eligible);
```

Two things to get right, both because getting them wrong has a real, disclosed blast radius:

- **Thread-safety is on you.** The registry does not add synchronization around your shared
  instance — it hands the same `shared_ptr<B>` to every caller across every session. If your
  `create`/`exec`/`destroy` mutate shared state (an `instances_` map, a connection pool), you must
  synchronize it yourself. This is a named, open requirement in the design draft (§2a), not
  something the registry silently handles.
- **`strict_eligibility::eligible` vs `named_only`.** `eligible` means your backend competes to win
  `Profile::Strict` resolution for *every* `Strict`-configured agent in the process, the moment it's
  registered — a real, process-wide routing change, not scoped to whoever registered it. For a new,
  unproven, or intentionally high-risk/opt-in backend (a first hardware-isolation integration, a
  backend still missing G1–G8 abuse-corpus evidence), register it `named_only` instead —
  `register_hardware_isolation_backend()` is a convenience overload that *cannot* accept `eligible`
  as an argument, exactly so a call site can't silently widen a hardware-isolation-class backend to
  `Strict` eligibility by omission. `KataBackend` itself is registered this way. Reachable only by
  an explicit `HostSandboxSelection{"my-backend"}` naming it — a host must opt a session in by name,
  not fall into it via `Strict`. Widening to `eligible` later is a distinct, visible decision (call
  `register_backend()` directly), not something that happens by adding a config flag.

## 5. Security posture — this is not optional paperwork

Per `CLAUDE.md`: **"Contested, hot-path, or security-critical designs go through `design →
red-team → prove → judge` and produce an ADR in `decisions/`, not an ad-hoc change."** A
`SandboxBackend` implementation is exactly this category — it is the thing standing between
model-authored code and the host. Every first-party backend in this tree followed that cycle
(`KataBackend`: ADR-080/081/086; Windows `native-jail`: ADR-004) and it caught real, blocking bugs
each time — a dangling-reference UAF, a zero-capability AppContainer child inheriting a live host
handle, a `B{}`-per-call state-loss bug. Writing your own outside this repo doesn't remove the need
for that discipline; it just means you own running it yourself.

Concretely, before treating a new backend as production-ready:

1. **Name your residuals in the header, not just in your head** — which `SandboxSpec` fields you
   enforce today vs. no-op, what your `destroy()` cleanup can silently fail at, what platform/deploy
   preconditions you assume (a running daemon, a reachable API endpoint, a pre-pulled image).
2. **Get an independent red-team pass** — a reviewer who did not write the backend, looking
   specifically for I2 (ambient authority reachable without an explicit capability) and I3 (model
   output influencing a permission decision) violations, per CLAUDE.md's own framing of what a
   "convenient-looking change" gets wrong most often.
3. **Prove containment with a real abuse corpus**, not just a happy-path exec — 008 §9's G1–G8 gate
   shape (or your own equivalent) is the bar every first-party backend in this repo was held to.
4. **Judge and record it** — even outside this repo's own `decisions/` directory, write down what
   was tested, what was found, and what's still a disclosed gap. A security claim without a test
   that could have failed proves nothing (CLAUDE.md's own standard).

## 6. Reference implementations to read, in order of how much to copy

- **`src/backends/kata/kata_backend.hpp`/`.cpp`** — the fullest worked example: shells out to an
  existing CLI (`ctr`) rather than embedding an RPC client ("one dependency per backend"), keeps
  per-instance state in an `instances_` map keyed by `SandboxHandle::opaque_id`, spawns subprocesses
  with explicit fd wiring (no ambient handle inheritance), and its own header comment is the model
  for disclosing exactly what is and isn't enforced yet. If your backend also shells out to a
  control-plane CLI (`kubectl` for `agent-sandbox`, a vendor CLI), start here.
- **`src/backends/native_jail/linux_native_jail_backend.cpp`** — the model for honestly
  disambiguating `ExecOutcome::klass` (timeout vs. oom vs. crash) when you don't have a clean
  "budget exceeded" signal from the isolation layer itself.
- **`src/backends/remote/README.md`**, **`src/backends/wasm/README.md`** — shorter, scope-only
  READMEs for backends that are stubs (`remote`) or fully real (`wasm`) — the pattern to follow for
  documenting your own backend's directory: what profile it implements, what dependency tier it
  takes (CONVENTIONS.md), what's real vs. not yet.
- **`tests/test_sandbox_backend_registry.cpp`** — minimal fake backends (`StatefulBackend`,
  `StrongerNamedOnlyBackend`, `LinuxOnlyBackend`) used purely to exercise the registry's own
  resolution logic. Useful as a *syntactic* skeleton for satisfying the concept, but they are
  intentionally not realistic — they don't spawn anything or enforce `SandboxSpec` at all. Do not
  treat them as a security template.

## 7. Minimal skeleton

Illustrative only — satisfies the concept and nothing else. Replace every body with real
create/exec/destroy logic against your actual isolation technology, and go through §5 before
registering it `eligible`.

```cpp
#include "agentengine/sandbox/sandbox.hpp"
#include <unordered_map>

namespace myorg {

class MySandboxBackend {
public:
    static constexpr agentengine::ProfileTraits traits{
        /*strength=*/40,  // set to what THIS backend's default config actually enforces
        static_cast<std::uint8_t>(agentengine::platform_id::linux_x86_64),
        agentengine::cold_start_class::network_dependent,
    };

    agentengine::result<agentengine::SandboxHandle> create(
            agentengine::SandboxSpec const& spec, agentengine::EffectContext& ctx);
    agentengine::result<agentengine::ExecOutcome> exec(
            agentengine::SandboxHandle& handle, agentengine::ExecRequest const& request,
            agentengine::EffectContext& ctx);
    void destroy(agentengine::SandboxHandle& handle);

private:
    struct Instance { /* your per-sandbox state */ };
    std::unordered_map<std::string, Instance> instances_;  // must survive across exec() calls
};

static_assert(agentengine::SandboxBackend<MySandboxBackend>);

}  // namespace myorg
```

Register it (`named_only` until it's cleared §5):

```cpp
agentengine::SandboxBackendRegistry registry;
auto backend = std::make_shared<myorg::MySandboxBackend>();
registry.register_hardware_isolation_backend("my-backend", std::move(backend));
```
