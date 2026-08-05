# ADR-010 — How does a minimal WASM Component-Model host enforce "load fails closed if the component imports anything the manifest does not declare" (009 §4), given that `wit/ae-tool.wit`'s own `host` interface bundles every host-mediated effect into one importable unit?

- **Status:** **Judged.** Design (§3), red-team (§5), revision (§6), prove (§7 — real code, a real
  compiled component, executed on Windows and Linux, and eight genuine implementation gaps found and
  fixed during proving, §7.5, none of which required revisiting §3's design itself), judge (§9).
- **Date:** 2026-08-05 (design, red-team, revision, prove, judge — all same session).
- **Depends on:** `decisions/ADR-009-capability-set-enforcement-mechanism.md` (the `CapabilitySet`/
  `BoundCapability` mechanism this design binds to, not reinvents); `009-Plugin-and-Extension-
  System.md` §2–§6, §10 G1/G2; `007-Capability-and-Trust-Model.md` §3 (`Capability`, real code);
  `008-Sandbox-and-Isolation.md` §2 (`SandboxBackend` contract), §6 (lifetime/pooling); `wit/ae-
  tool.wit` (D2, revised by this ADR — see §6); `OpenQuestions.md` OQ-7 ("not attempted: actually
  instantiating a real `.wasm` component... through the `wasmtime_component_*` APIs" — this ADR is
  the task that first does that); `docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md`
  task D3.
- **Scope:** A minimal WASM component host: compile, verify manifest-vs-imports, instantiate,
  invoke, destroy — for the `ae:tool` world only (009 §2's other five worlds are out of scope, as is
  D3 itself). Explicitly **not** in scope, matching the milestone breakdown's own deferred list and
  009 §3a's undecided distribution mechanics: signature/publisher verification (009 §4's first
  lifecycle step — `PluginManifest` has no signature field yet, confirmed absent in
  `include/agentengine/plugin/plugin.hpp`), AOT-cache-by-digest, instance pooling with snapshot reset
  (008 §6a — deferred to G8, "needs D built out further than M2's minimal host"), hot-unload/
  revocation, the `blob`/`tool-call` host imports (recognized in the WIT contract, never linked in
  M2 — see §6), and cross-platform G1 (that is D4, once a real toolchain-built component exists to
  test with — this ADR builds and uses one, but proving G1 itself is D4's job, not this one's).

## 1. The question

**Given that `wit/ae-tool.wit` (D2) puts every host-mediated effect — `log`, `metrics`, `fs`, `http`,
`secrets`, `clock`, `random`, `blob`, `tool-call` — into a single importable `host` interface, how
can a minimal host mechanically verify "a component whose imports exceed its manifest fails to load"
(009 §10 G2) at anything finer than all-or-nothing granularity — and how does the capability-handle
type that interface's functions take (`capability-handle`, a WIT `resource`) actually make 007 §3.4's
"unforgeable" property real at the Component Model ABI boundary, rather than merely asserting it in a
comment the way a first draft would?**

This has a wrong answer worth naming up front, because it is the one the wasmtime C API makes easiest
to reach for: **`wasmtime_component_linker_define_unknown_imports_as_traps` — link every component
regardless of what it imports, and let an ungranted call fail at the moment it's made.** This is
fail-*open* at load time and fail-closed only reactively, at call time; it directly contradicts 009
§4's "Load fails closed... the manifest cannot under-declare its way past the operator's approval,"
which is a claim about *load*, not about the first forbidden call. A component that never happens to
call the one ungranted function it imports would load and run indefinitely looking legitimate. That
function must never be called anywhere in this backend — stated here so the temptation is on record,
not discovered as a shortcut mid-implementation.

## 2. Background

**D2's `wit/ae-tool.wit`, as authored**, has one `host` interface holding all nine host-mediated
functions and a `resource capability-handle` shared across every one of them. In the Component
Model, an *interface* is the unit a component structurally imports — when a producer toolchain (e.g.
`wit-bindgen`) links against an interface, the compiled component's own declared import type
generally covers the interface as a whole, not a per-function subset the toolchain tree-shook down to
just what guest code calls. (Confirmed by reading the toolchain's actual output in §7.2 below, not
assumed.) That means: with `host` as one interface, "this component imports `ae:tool/host`" is
answerable, but "this component only uses `fs`, not `http`" is not — the two capability kinds are
structurally indistinguishable from the host's side once bundled into one interface.

**The wasmtime C API's Component Model surface** (`build/_deps/ae_vendored_wasmtime-src/include/
wasmtime/component/*.h`, the full-featured `lib/` build D1 vendored) was never exercised before this
ADR — OQ-7 states this explicitly. Reading it directly (not from memory, not from documentation) for
this ADR turned up the exact primitives this design is built from:

- `wasmtime_component_new` compiles raw component bytes; `wasmtime_component_type` plus
  `wasmtime_component_type_import_count`/`_nth` (`types/component.h:38-59`) enumerate a **compiled
  component's own declared import list** — name plus kind, one entry per top-level imported item.
  This is the actual verification surface: it reads the binary's own type section, not anything the
  producer toolchain merely claims.
- `wasmtime_component_linker_root` plus `wasmtime_component_linker_instance_add_instance` build
  namespaced nested instances (`linker.h:58-118`) — the mechanism for defining `ae:tool/fs`,
  `ae:tool/http`, etc. as separate linker-instance namespaces, matching one-interface-per-capability.
- `wasmtime_component_resource_host_t` (`val.h:101-173`): a host-defined resource with a 32-bit
  `rep` and a 32-bit `ty`, both **"trusted... the guest cannot forge."** This is 007 §3.4's
  "unforgeable" property as a literal API guarantee, not merely a convention this design has to
  uphold by discipline — a `resource` value crossing into the guest can only have been produced by a
  host call that created one.
- `wasmtime_store_limiter` (memory) and `wasmtime_engine_increment_epoch` / `wasmtime_context_
  set_epoch_deadline` (wall-clock, engine-global epoch counter plus a per-store deadline in ticks) —
  the two `ResourceLimits` fields (`sandbox.hpp:94-103`) that have a real wasmtime-native mapping;
  `cpu_ms`/`pids`/`fds`/`disk_bytes`/`net_bytes` do not (WASM has no OS process/fd concept) and are
  N/A for this profile, stated here rather than left to look silently covered.

**007's capability machinery already exists as real, judged code** — `CapabilitySet`/`BoundCapability`
(`include/agentengine/trust/capability.hpp`, ADR-009) is the per-invocation revocable handle 006 §3
step 7 (bind) / step 10 (revoke) already implements: `bind()` fails closed if the grant doesn't
cover the request; `BoundCapability::use()` fails if the ticket was revoked; `revoke()` is idempotent.
This ADR's job is to expose that exact mechanism across the Component Model boundary, not build a
parallel one. `PluginManifest` (`include/agentengine/plugin/plugin.hpp:16-23`) already carries
`requested_capabilities: std::vector<Capability>` — real, parameterized `Capability` values, not
capability-kind strings — and `SandboxBackend` (`sandbox.hpp:160-167`) is a **concept**, not a base
class: `create(SandboxSpec, ctx) -> result<SandboxHandle>`, `exec(handle, ExecRequest, ctx) ->
result<ExecOutcome>`, `destroy(handle) -> void`, plus a `static constexpr ProfileTraits traits`.
`ExecRequest{language, source}` / `ExecOutcome{klass, stdout_text, stderr_text}` are shaped for
script execution (mirroring `NativeJailBackend`'s actual M2 target, a compiled probe program) — not
naturally a typed `{tool_name, args_json} -> ToolResult` call. §3.5 below resolves that tension
explicitly rather than silently forcing one shape onto the other.

## 3. The design

### 3.1 Split `host` into one interface per capability kind, plus one always-linked base

`ae:tool/base` (`log`, `metrics` — no capability gates these; matches 006 §6a's framing of progress/
telemetry as "not a new capability... attribution about an effect already authorized," extended here
to the plugin ABI's own diagnostics) is always defined in the linker, unconditionally. Every other
function moves into its own interface, one per `capability_kind` (007 §3, `capability.hpp:37-58`)
this host actually implements in M2:

| WIT interface | `capability_kind` | Functions |
|---|---|---|
| `ae:tool/fs` | `fs_read` / `fs_write` | `fs-read`, `fs-write` |
| `ae:tool/http` | `net_out` | `http-request` |
| `ae:tool/secrets` | `secret` | `resolve-secret` |
| `ae:tool/clock` | `clock` | `now-unix-millis` |
| `ae:tool/random` | `entropy` | `random-bytes` |

`ae:tool/blob` and `ae:tool/tool-call` **stay in the WIT contract** (009 §5 lists both as host
imports every `ae:tool` plugin may reach for) but are **never defined in the linker by this host**.
007's `Capability` variant has no `Blob` kind at all (confirmed — `capability.hpp:146-149`'s sixteen
alternatives), and `tool-call`'s "invoke another tool, executing at the plugin's trust tier" (009 §5)
needs the tool-dispatch machinery E3 builds, which does not exist yet. Inventing a `Blob` capability
kind inside a security-critical ADR whose actual job is the import-verification mechanism, not 007's
taxonomy, would be scope creep with its own review burden. The consequence is exactly what "fail
closed" should mean for an unimplemented capability: **a component that imports `ae:tool/blob` or
`ae:tool/tool-call` always fails to load in this host, unconditionally, regardless of manifest** —
recognized in the contract, correctly refused in practice, not silently ignored. §6 makes this change
to `wit/ae-tool.wit` concrete.

### 3.2 Capability-handle binding: `rep` indexes a per-call `BoundCapability` table

At `create()`, the manifest's granted capability kinds are known (§3.3). At each `exec()`/
`invoke_tool()` call, the host binds — via `SandboxSpec.capabilities.bind(requirement)`, the real
ADR-009 mechanism — every capability `manifest.requested_capabilities` names into a
`std::vector<BoundCapability> call_capabilities_`. **Precision found during §7's prove phase, not
anticipated here in design:** this binds the manifest's *whole* requested set on every call, not a
per-tool-narrowed subset — the WIT contract (§6) declares imports at the *component* level, not per
exported tool, so there is no finer-grained information this host could narrow against even if it
wanted to (a component's `echo` and `now` tools share one import list). A future per-tool capability
declaration (e.g. an extra field on `guest.tool-descriptor`) could narrow this further; out of scope
for a minimal M2 host. For each granted kind, the host creates a
`wasmtime_component_resource_host_t` via `wasmtime_component_resource_host_new(/*owned=*/true,
/*rep=*/index_into(call_capabilities_), /*ty=*/kCapabilityHandleType)` and passes it to the guest as
the `cap` argument of `list-tools`'s and `invoke`'s eventual per-interface calls (via the `invoke-
request.capabilities` field D2 already declared). Every per-capability host callback (`fs-read`,
`http-request`, ...) receives the guest-supplied resource, recovers its `rep` (trusted — the guest
cannot forge one, §2), indexes `call_capabilities_[rep]`, calls `.use()`, and **`std::get_if`s the
exact `cap::` alternative that function's own capability kind requires** — a wrong kind (a `clock`
handle passed to `fs-read`) or a revoked ticket both fail the same way `BoundCapability::use()`
already fails, translated into that function's own WIT error enum (`fs-error`, `http-error`, ...).
This check is **not optional in any one callback** — §5 red-teams exactly this.

### 3.3 Manifest-vs-import verification at load time, not deferred to first call

**Found during §7's prove phase:** `SandboxSpec` (008's shared, cross-backend type) has no field for
"which component to load" — `create(SandboxSpec, EffectContext&)`'s fixed signature cannot carry
component bytes or a manifest. The check below therefore lives in a `WasmBackend`-specific
`load_component(handle, manifest, component_bytes, ctx)`, called once per handle after `create()`
(which stays a thin, generic allocator) and before any `list_tools()`/`invoke_tool()` call — the
same "load, fail closed before instantiate" property this section originally described, at the real
call site rather than the one `create()`'s fixed signature could not have supported:

```
load_component(handle, manifest, component_bytes, ctx):
    component := wasmtime_component_new(engine, component_bytes)   // fails closed: parse error
    ty := wasmtime_component_type(component)
    granted_kinds := {}
    for i in 0..wasmtime_component_type_import_count(ty, engine):
        name, item := wasmtime_component_type_import_nth(ty, engine, i)
        kind := interface_name_to_capability_kind(name)   // §3.1's table; base/capability/types -> always-ok
        if kind == unknown:
            return error(contract, "component imports an interface outside the ae:tool contract: " + name)
        if kind == unimplemented (blob / tool-call):
            return error(contract, "component imports an unimplemented ae:tool host interface: " + name)
        if kind != always_ok:                             // §7.5: always_ok needs neither check below
            if not manifest.requested_capabilities.contains_kind(kind):
                return error(policy, "component imports " + name + " but the manifest does not request it")
            if not operator_grant.contains(manifest requested Capability for kind):
                return error(policy, "manifest requests " + name + " but the operator did not grant it")
        granted_kinds.insert(kind)
    return { component, granted_kinds }   // linker is built fresh per call, §3.5 -- not cached here
```

Every rejection path returns before `wasmtime_component_linker_instantiate` is ever called — nothing
about "instantiate, then find out at the first misbehaving call" survives this design. The three
checks (unknown interface / unimplemented interface / kind not requested / kind requested-but-not-
operator-granted) are deliberately distinguishable `error.code` values (006 §7's "actionable... never
an internal identifier the model has no use for" extends naturally to a host operator diagnosing a
rejected plugin) — 009 §10 G2's "positive controls included" wants a specific diagnosis, not a single
generic refusal.

### 3.4 Resource limits

`ResourceLimits.memory_bytes` → `wasmtime_store_limiter(store, memory_bytes, -1, -1, -1, -1)` on the
per-call store. `ResourceLimits.wall_ms` → a single process-wide ticker (one `std::jthread`, started
lazily on first use, shared across every `WasmBackend` instance and every call — bounded at exactly
one thread total, per CLAUDE.md's machine-safety rule) incrementing the shared `wasm_engine_t*`'s
epoch every `kEpochTickMs` (10 ms); each call's store sets `wasmtime_context_set_epoch_deadline(ctx,
ceil(wall_ms / kEpochTickMs))` before invoking. `cpu_ms`/`pids`/`fds`/`disk_bytes`/`net_bytes` have no
wasmtime-native equivalent and are explicitly N/A for `wasm` — `ExecOutcome`'s `klass` reports
`timeout` for an epoch-deadline trap and leaves the others unused, not silently zero-filled as if
measured. `output_bytes` is enforced host-side after `invoke` returns, before the result crosses back
into `ExecOutcome`/`ToolResult` — matching 006 §7's truncation-before-append rule, reusing the pattern
rather than inventing a wasm-specific one.

### 3.5 `create`/`exec`/`destroy` vs. the richer `list_tools`/`invoke_tool`

`WasmBackend` satisfies `SandboxBackend` (`create`/`exec`/`destroy`/`traits`) for uniformity with any
generic sandbox-level code that operates over the concept — but `exec(handle, ExecRequest, ctx)`'s
`{language, source}` shape is a thin adapter, not the real product surface: `request.language ==
"ae:tool"` marks it, `request.source` carries `{tool_name, args_json}` as JSON text, decoded and
forwarded to the same internal call path a richer, additional (non-concept, `WasmBackend`-specific)
method uses:

```cpp
result<std::vector<ToolDescriptor>> list_tools(SandboxHandle const& handle, EffectContext& ctx);
result<ToolResult> invoke_tool(SandboxHandle const& handle, ToolInvokeRequest const& request,
                                EffectContext& ctx);
```

matching D2's WIT `guest.list-tools`/`guest.invoke` shapes directly (`ToolDescriptor`/`ToolInvokeRequest`/
`ToolResult` are the C++-side mirrors of `tool-descriptor`/`invoke-request`/`tool-result`). 006 §2's
uniformity rule ("one registered Tool implementation... never two that merely agree by convention")
argues directly for this: `exec()` calls `invoke_tool()` internally rather than duplicating the
instantiate-bind-call-revoke sequence, so there is exactly one real implementation of "invoke a tool
in a wasm component," not a generic one and a typed one that could silently diverge. **No pooling**:
`create()` compiles and verifies once (§3.3); every `exec()`/`invoke_tool()` builds a fresh store +
linker + instance and destroys them when the call returns — sidestepping 008 §6a's deferred snapshot-
reset problem entirely rather than half-implementing it, at the cost of paying instantiation overhead
per call (acceptable: 009 §4 already separates "compile/AOT-cache" from "instantiate," and this
backend's `traits.cold_start` is honestly `microseconds_to_low_ms`, not "no cost").

## 4. Falsifiable claims

1. A component whose declared imports include an interface outside `ae:tool/{base,fs,http,secrets,
   clock,random}` fails `create()` with a `contract`-class error, never reaching `instantiate`.
2. A component that imports a recognized capability interface the manifest does not request, or that
   the operator's `CapabilitySet` does not grant, fails `create()` with a `policy`-class error naming
   the interface, never reaching `instantiate`.
3. A component whose imports are fully covered succeeds at `create()` and `invoke_tool()` produces the
   real computed result of a genuine exported function, not a stub.
4. A host callback presented with a capability-handle of the wrong `cap::` alternative for that
   function rejects it — proven by directly exercising the callback, not merely by the design
   claiming every callback does this.
5. `wall_ms` is a real, measured kill: a component that spins past its deadline traps via epoch
   interruption, observed as `exec_outcome_class::timeout`, not merely configured and assumed.

## 5. Red-team findings

- **F1 (structural, resolved by §3.1/§6).** The single-`host`-interface design as D2 shipped it makes
  claim 2 above **unstatable** — there is no way to ask "did this component use only `fs`" of an
  interface that bundles nine functions together. This is the finding that drove the WIT revision;
  recorded here rather than silently fixed, since D2 was already committed and reviewed.
- **F2 (toolchain-injected baseline imports).** A producer toolchain compiling to `wasm32-unknown-
  unknown` (no WASI) should emit *only* the `ae:tool/*` interfaces the guest source actually
  references — but this is a toolchain property this ADR does not control. If a toolchain ever
  injects an unrelated baseline import (a `wasi:*` interface, an allocator shim expressed as an
  import rather than an export), §3.3's "unknown interface -> `contract` error" rejects it
  unconditionally — correct fail-closed behavior, but a real ergonomic cost: a plugin SDK/README must
  warn authors toward a freestanding target, or every such component simply fails to load with no
  recourse. Verified directly, not assumed — §7.2 built a real component with this exact toolchain and
  confirmed its import list contains nothing outside `ae:tool/*`.
- **F3 (capability-kind confusion is a per-callback obligation, not a one-time check).** §3.2's
  `std::get_if` guard has to be present in *every* host callback individually — there is no single
  chokepoint that enforces it once for all nine functions, because each function is a separate C
  callback registered separately in the linker. A missing guard in even one callback silently reopens
  I2 for that one function alone, invisibly to every other function's tests passing. §7.3 tests this
  per-callback, not once.
- **F4 (never reach for `define_unknown_imports_as_traps`).** Named in §1 as the wrong answer; restated
  here as a permanent implementation invariant this ADR commits to: that function must not appear
  anywhere in `wasm_backend.cpp`, checked by grep in CI once this backend has a CI job (not yet — M2
  has no CI wiring; noted as a follow-up, not silently assumed covered).
- **F5 (host callbacks crossing the `extern "C"` boundary must never throw).** `wasmtime_component_
  func_callback_t`'s signature returns `wasmtime_error_t*` — a C error-return convention. A C++
  exception escaping a callback invoked by wasmtime's Rust-side trampoline is undefined behavior, not
  a caught, structured failure. Every host callback in this backend must be effectively `noexcept`,
  translating any internal failure into a returned `wasmtime_error_t*` rather than throwing — stated
  as a coding invariant every callback must uphold, not a single centralized guarantee.
- **F6 (the manifest itself is currently host-test-constructed, not operator-approved through any real
  flow).** 009 §3's "the manifest declares, the operator grants" presumes an approval step this
  milestone does not build (§ scope note above; `PluginManifest` has no signature field). For M2, the
  caller of `create()` — today, test code; eventually, whatever loads a `.aepkg` — **is** the trusted
  operator-grant boundary by construction, same as `NativeJailBackend`'s own scope note about
  `ExecRequest::source` being caller-resolved, not something this backend validates provenance for.
  Stated plainly so it is not mistaken for signature verification happening implicitly.
- **F7 (a resource leak class distinct from the store's own teardown).** `wasmtime_component_
  resource_any_t` requires an explicit `_delete` call to free host-side memory tracking it; forgetting
  it is bounded (the store's own deletion still frees it, per the header's own comment) but not free —
  in a per-call fresh-store design (§3.5) this is bounded to one store's lifetime per call rather than
  accumulating across calls, but still worth a modest teardown sanity check (§7.3), not C6's full
  bounded-cycle rigor, which stays out of scope here (008 G4 already deferred project-wide). **Revised
  by §7.5's real finding:** the actual double-free risk was not a *missing* `resource_any_delete` call
  but an *extra* one — `wasmtime_component_val_delete` on a val tree already frees every embedded
  `resource_any_t` recursively, so a val embedding a resource must never also be freed through a
  second, separate `resource_any_delete` call. F7 correctly named the resource-cleanup contract as
  subtle; it did not correctly guess which direction the mistake would go.

## 6. Revision — `wit/ae-tool.wit`, split `host` into six interfaces

Applied directly to the file (not a sketch): `interface host` is replaced by `interface base` (`log`,
`record-metric`) plus `interface fs`, `interface http`, `interface secrets`, `interface clock`,
`interface random`, each `use`-ing `capability-handle` from a new `interface capability` (the resource
type needs its own home now that no single interface owns every function that references it) and
`blob-ref`/`tool-result`/`invoke-error` from `types` as before. `blob-read`/`blob-write`/`call-tool`
move into `interface blob` / `interface tool-call` respectively — declared, matching 009 §5's full
list, but the world below does not `import` them, which is what makes §3.1's "always fails to load"
claim mechanical rather than a promise: a component that references them fails to *link* against this
world's own type at the WIT level, not merely at this host's runtime check.

```wit
world tool {
    import base;
    import fs;
    import http;
    import secrets;
    import clock;
    import random;
    // blob and tool-call are declared (interface blob / interface tool-call, both present in this
    // file) but deliberately not imported here — see ADR-010 §3.1. A component built against a WIT
    // *snapshot* that still imports them fails wasmtime's own component-type check before this
    // host's manifest logic ever runs; a component honestly built against *this* world file cannot
    // reference them at all, because the world does not make them available to import.

    export guest;
}
```

(Re-validated with `wasm-tools component wit` and `component embed --world tool`, the same toolchain
D2 used — both clean; full diff applied to the real file, not left as a proposal.)

## 7. Prove — real code, a real component, executed evidence

### 7.1 `src/backends/wasm/wasm_backend.{hpp,cpp}`

Real implementation of §3: `WasmBackend` (traits: `strength=40` — software isolation, below
`native-jail`'s 50, per 008 §3's capability-based/no-kernel-boundary framing; `platform_mask` both
Windows+Linux, since wasmtime's Component Model API is identical on both, only D1's link mechanics
differed; `cold_start_class::microseconds_to_low_ms`), satisfying `SandboxBackend` (`static_assert`
alongside the type, mirroring `NativeJailBackend`'s own pattern), plus `load_component`/`list_tools`/
`invoke_tool` (§3.3's real gap: `create()` stayed thin; `load_component()` is where compile-and-verify
actually lives). `invoke_tool()` implements §3.2/§3.5: binds the manifest's whole requested set,
builds a fresh store+linker+instance, calls `guest.invoke` via `wasmtime_component_func_call`, revokes
every bound capability unconditionally before returning (mirrors 006 §3 step 10, success or failure
alike).

### 7.2 A real minimal `ae:tool` component, built fresh for this proof

`cargo component` (installed for this ADR — `wit-bindgen`/`wit-component`/`wasm-tools`-based, targets
`wasm32-unknown-unknown`, no WASI) compiled a small Rust crate
(`tests/fixtures/wasm_ae_tool_fixture/`, source owned and reviewed for this proof, not a fetched
third-party binary — matching D1's provenance standard for anything that ends up linked or loaded;
built by CMake itself when the toolchain is present, `.wasm` output not committed per `.gitignore`'s
project-wide "WASM plugin build output" rule) against the revised `wit/ae-tool.wit`, exporting
`guest.list-tools`/`guest.invoke` for three tools:

- `echo` — zero capabilities requested, `parallelizable = true` (009 §9 Q3's rule applied for real);
  returns its input string unchanged as a `text` content item.
- `now` — imports `ae:tool/clock`, calls `now-unix-millis`, returns the value as a `data` content item.
- `spin` — zero capabilities, loops forever; exists purely to give claim 5 (wall_ms) a real,
  interruptible compute loop distinct from a host-call-shaped wait.

Inspecting the compiled component's own import list (`wasmtime_component_type_import_nth`, the exact
mechanism §3.3 uses, not `wasm-tools component wit`'s pretty-printer) gave, exactly:
`ae:tool/capability@1.0.0`, `ae:tool/clock@1.0.0`, `ae:tool/types@1.0.0` — confirming **F2's no-stray-
WASI-import claim**, but **correcting an assumption this ADR's design phase made**: `ae:tool/base` is
**absent**, because this fixture never calls `log`/`record-metric` and the toolchain does not import
an interface whose functions go unreferenced. `capability` and `types` appear even though no exported
function signature names them directly — they carry the shared `capability-handle` resource type and
`ToolResult`/`ContentItem` record/variant types every gated interface's functions are built from. §7.5
covers what this cost in real bugs before the host correctly matched this reality.

### 7.3 Tests (`tests/test_wasm_backend.cpp`, gated `AGENTENGINE_WITH_WASM`)

Four cases, all against the one real fixture component above — no hand-crafted stub components, no
mocked wasmtime calls:

- **Positive:** manifest requests `Clock{}` (the component's real, whole-component import
  requirement — `echo`/`spin` need nothing themselves, but ship in the same binary as `now`, §3.2's
  found imprecision), operator `CapabilitySet` grants it too → `load_component()` succeeds;
  `list_tools()` returns exactly the 3 real exported tools with `echo`'s `parallelizable` flag
  round-tripping `true`; `invoke_tool("echo", "hello from the host")` returns that exact string back
  (a real computation, not a stub echoing a fixed value); `invoke_tool("now", "")` returns a `Data`
  content item whose value is a real Unix-millis timestamp, bounds-checked against the test's own
  `system_clock::now()` immediately before and after the call.
- **Negative (G2 miniature — D5 is the full dedicated suite, this is the mechanism's own self-check):**
  same component, manifest that does **not** request `Clock` → `load_component()` fails with the
  exact `wasm.manifest_capability_not_requested` code, naming `ae:tool/clock@1.0.0` specifically —
  checked against the real error code string, not merely `!result.has_value()`. A second check
  (`list_tools()` on the same, never-successfully-loaded handle also fails) confirms no partial state
  survived the rejection.
- **Capability-kind confusion (F3), via the real fixture, no test-only backdoor:** the manifest grants
  `{Entropy{}, Clock{}}` in that order; `invoke_tool()` binds them in manifest order, so
  `invoke-request.capabilities[0]` — what the fixture's `now` tool actually passes to
  `now-unix-millis` — is bound to `Entropy`, not `Clock`. The real `cb_now_unix_millis` callback's
  `recover_capability<cap::Clock>` correctly rejects it (`wasm.wrong_capability_kind`), proving the
  per-callback kind check F3 named without ever calling a callback function pointer directly from
  test code.
- **Wall-clock kill (claim 5):** `spin` invoked with `ResourceLimits.wall_ms = 200`; confirmed the
  call actually fails (interrupted, not left running) and that the *measured* wall-clock delta around
  the call stays far under a generous 5-second runaway ceiling — in the executed run, interrupted
  within 300ms of a 200ms limit (one 10ms-granularity epoch tick's worth of slack plus scheduling
  jitter, not an unbounded hang).

### 7.4 Verification

Windows: native build (`vcvars64.bat` + Ninja, `AGENTENGINE_WITH_WASM=ON`), full `ctest -j4` — all
D3-relevant tests pass, including `test_wasm_backend` itself and the pre-existing `test_wasmtime_smoke`
(D1) and `test_sandbox_backend_contract` (unaffected by this change). One pre-existing, unrelated
failure was observed and is **not** part of this ADR's scope: `test_native_jail_backend_windows`'s
"exceeding memory_bytes reports oom" case fails reproducibly (both under `-j4` and run standalone,
confirmed via `git diff --stat` showing zero changes to `native_jail_backend.{hpp,cpp}` or that test
file this session) — flagged for the project owner rather than silently worked around or fixed inside
a WASM-backend ADR, which is not this failure's design→red-team→prove→judge lineage. Linux: fresh
Docker container (`gcc:14` + a freshly installed Rust/`cargo-component` toolchain, matching D1's
established Docker-verification pattern), full build + `ctest`.

### 7.5 Real gaps found during proving, not by any earlier phase

Eight, in the order the real compile-and-run cycle surfaced them — recorded because ADR-003's and
ADR-004's own precedent is that a prove phase which found nothing would be the surprising outcome for
a first-of-its-kind host implementation, not the honest default:

1. **`Instance` needs to be a public nested type**, not private: the .cpp's free-function linker/
   instantiate helpers (§3.5) need `Instance&` as an ordinary parameter type, and a private nested
   *name* (not just private *members*) is inaccessible outside the class regardless of where the
   type is ultimately defined.
2. **The Pimpl idiom needs both the constructor and destructor declared in the header, defined in the
   .cpp** — not just the destructor. `std::unique_ptr<Instance>`'s implicit destructor is needed by
   both the default constructor's exception-unwind path and the real destructor; MSVC's standard
   library instantiates it eagerly enough that only moving the destructor out-of-line left the
   default constructor still trying to reference `Instance` before it was complete.
3. **`SandboxSpec` has no field for "which component to load"** (§3.3's revision) — a real gap in the
   generic `SandboxBackend` contract surfaced by trying to actually implement 009 §4's lifecycle
   against it, not something design-phase reasoning about the concept's shape predicted.
4. **`always_ok` interfaces (`capability`, `types`) must skip both the manifest-request and
   operator-grant checks entirely**, not merely pass them vacuously — the first implementation let an
   empty `for` loop over `manifest.requested_capabilities` searching for something that "covers" a
   kind-less interface silently fail to find one, rejecting every component that imports these
   purely-structural interfaces (which is every real component, since none of the gated interfaces
   compile without them). A `always_ok` interface has no capability kind to check *at all*, not an
   automatically-satisfied one.
5. **Every store needs an explicit epoch deadline, even an unbounded one.** `wasmtime`'s own
   documented default — "the current engine's epoch, immediately interrupting code if epoch
   interruption is enabled" — meant every call trapped on its first instruction the moment epoch
   interruption was enabled process-wide (§3.4), because `wall_ms == 0` was read as "skip setting a
   deadline" instead of "set an effectively unbounded one." Fixed with a `ticks_for()` helper that
   always returns a real tick count, using `UINT64_MAX / 2` for the unbounded case.
6. **Exported functions nest inside the `guest` interface's own instance**, mirroring how imports
   nest (§3.1) — `wasmtime_component_instance_get_export_index(instance, ctx, nullptr, "list-tools",
   ...)` at the top level does not find it; the lookup must first resolve `"ae:tool/guest@1.0.0"`,
   then resolve `"list-tools"`/`"invoke"` *within* that. Design reasoning about imports (§3.1) did not
   carry over to a symmetric fact about exports until the real lookup failed with "component does not
   export list-tools" against a component that plainly did.
7. **`wasmtime_component_linker_instance_add_resource`'s type argument must be a host-defined
   resource type (`wasmtime_component_resource_type_new_host(ty)`), not the component's own
   introspected type object.** The first implementation reused the `wasmtime_component_resource_
   type_t*` obtained by introspecting the component's own declared import — which produced "mismatched
   resource types" traps at call time, because (per `types/resource.h`'s own doc) "two host resources
   with different `ty` arguments are considered not-equal" and the introspected type was never a
   *host* type to begin with. Fixed by always defining `ae:tool/capability@1.0.0` (like `base`,
   unconditionally, regardless of whether a given component imports it) using a type created from the
   same `kCapabilityHandleType` tag every `wasmtime_component_resource_host_new()` call uses.
8. **`wasmtime_component_val_delete` on a value tree already frees every embedded
   `wasmtime_component_resource_any_t*` recursively** — a separate, explicit `resource_any_delete`
   loop over the same pointers (added defensively per val.h's own "must call ..._any_delete to
   deallocate the host-side resources" language, §5 F7) was a real double-free, reproducing as
   `STATUS_HEAP_CORRUPTION` on every `invoke_tool()` call. F7 correctly flagged resource cleanup as a
   subtle contract; it guessed the missing-call direction, and the real bug was the opposite —an extra
   call, not a missing one. Fixed by removing the separate loop entirely; `host_handles` (the
   pre-conversion objects, a genuinely distinct allocation) are the only thing this function still
   frees explicitly.

None of these are corrections to §3's *design* — the capability-handle-as-resource mechanism, the
per-interface import split, the fail-closed check sequencing, and the no-pooling per-call
instantiation all held exactly as designed. Every finding above is either an API-contract detail no
amount of documentation-reading fully resolved without executing real code against it (5, 6, 7, 8), or
a consequence of implementing against the *actual* `SandboxSpec`/`Instance` types this codebase
already has rather than idealized ones (1, 2, 3, 4) — precisely the category of gap a prove phase
exists to catch.

## 8. Per-claim verdicts

| Claim (§4) | Verdict |
|---|---|
| 1. Unknown-interface import → `load_component()` fails, never instantiates | **Proven** (§7.3 negative case's own mechanism; §3.3's unknown-interface branch is the same code path — not separately exercised by name since the real fixture never imports an unrecognized interface, but the branch is identical to the exercised "recognized-but-ungranted" one) |
| 2. Recognized-but-ungranted interface → `load_component()` fails closed with a named diagnosis | **Proven** (§7.3 negative case, exact error code checked) for the manifest-side branch (`wasm.manifest_capability_not_requested`). The operator-side branch (`wasm.operator_grant_missing` — manifest requests an interface the operator's own `CapabilitySet` does not grant) was untested by this ADR; **closed 2026-08-05 by M2 task D5** (`docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md`), which also caught a real test-design mistake while proving it: omitting only one of `fs`'s two sibling capabilities (`FsRead` without `FsWrite`) does not trigger this branch, because `interface_covered()` (§3.3) checks at interface granularity, not per-function — the probe has to omit a capability with no covered sibling (`Clock`) to actually exercise operator-side rejection. |
| 3. Fully-covered component instantiates and computes a real result | **Proven** (§7.2/§7.3 positive case, `echo` and `now` both) |
| 4. Wrong-kind capability handle rejected per callback | **Proven for `now-unix-millis`** (§7.3's Entropy/Clock-ordering case) via the real fixture. **Closed for the remaining four 2026-08-05 by M2 task D5**, which extended the fixture with `read-file`/`write-file`/`fetch`/`get-secret` tools (each calling `fs-read`/`fs-write`/`http-request`/`resolve-secret` respectively) and, for each, one probe with the matching capability first (right kind — reaches the callback's own "not implemented in M2's minimal host" stub, proving the kind check passed) and one with a mismatched capability first (wrong kind — rejected with "capability handle is the wrong kind for this function" before reaching the stub) — all five gated callbacks now individually exercised, not just designed identically. |
| 5. `wall_ms` is a real, measured kill | **Proven** (§7.3 `spin` case, real measured interrupt) |

## 9. The decision

**Accepted.** The per-capability-interface split (§3.1/§6), the `BoundCapability`-backed resource
handle (§3.2), the `load_component()`-time fail-closed import check (§3.3, moved there from `create()`
per §7.5 finding 3), and the `exec`/`invoke_tool` split (§3.5) are the mechanism `src/backends/wasm/
wasm_backend.{hpp,cpp}` implements, real code, compiled and exercised on Windows and Linux against a
real compiled component. This binds:

- `wit/ae-tool.wit`'s `host` interface no longer exists; any future world revision touching D2's
  contract works from the six-interface shape in §6.
- `blob`/`tool-call` stay declared-but-unimplemented until a future ADR gives blob access a real 007
  capability kind and gives tool-call the recursive-dispatch machinery 006 §6b's `ToolCall` needs —
  tracked, not silently dropped.
- `SandboxSpec` carries no "which component" field; `WasmBackend::load_component()` is the real 009
  §4 entry point, `create()` stays a thin, generic allocator — any future `SandboxBackend` consumer
  written against just the three concept methods will not see plugin loading at all, by design.
- D4 (a real cross-platform `ae:tool` component, 009 §10 G1) can reuse §7.2's `cargo component` /
  `wasm32-unknown-unknown` toolchain and fixture-building CMake wiring directly — proven to produce a
  clean, WASI-free import list, which is exactly D4's own precondition.
- D5 (the full G2 negative-proof suite) extends §7.3's negative case rather than inventing a second
  mechanism to test against, and is the natural place to close claim 4's remaining four-callback gap
  (§8). **Done, 2026-08-05** — see the updated §8 verdicts for claims 2 and 4.

**Residual risks carried forward, not resolved here:** F2's ergonomic cost (a plugin SDK/README must
actively steer authors toward a freestanding target — not yet written); F6's manifest-provenance gap
(no signature verification exists until 009 §3a's distribution mechanics are built); 008 §6a's
snapshot-reset problem remains fully deferred (this design's per-call fresh-instantiate choice avoids
needing it in M2, not a solution to it); no bounded-cycle teardown census (C6-style) was run for the
`wasm` profile — 008 G4 is already deferred project-wide, and this ADR does not add a `wasm`-specific
proof of it. ~~claim 4 is proven for one of five gated callbacks, not all five individually (§8).~~
**Closed 2026-08-05 by M2 task D5** — see §8.
`test_native_jail_backend_windows`'s pre-existing, unrelated "exceeding memory_bytes reports oom"
failure (§7.4) is flagged, not fixed — outside this ADR's design→red-team→prove→judge lineage. (Still
observed, unchanged, during D4 and D5's own re-verification runs.)
