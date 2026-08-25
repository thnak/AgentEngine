# Design draft: `build_default_sandbox_registry()` — real host-callable backend registration

**Status:** Design draft, Revision 2 — not yet an ADR. Revision 2 folds in three independent
red-team passes (correctness/CMake plumbing, security I2/I3, scope/overclaim), all against the real
current code, not the draft's paraphrase. Findings, all fixed in this revision:

- **MUST-FIX (correctness)**: `WasmBackend` lives in `agentengine::wasm`, not top-level
  `agentengine` (`src/backends/wasm/wasm_backend.hpp:37,59`) — Revision 1's §2c sample wrote
  `std::make_shared<WasmBackend>()` unqualified inside `namespace agentengine`, which does not
  resolve and would not compile. Fixed below: `wasm::WasmBackend`.
- **MUST-FIX-equivalent (security)**: `KataBackend`'s own header contract
  (`src/backends/kata/kata_backend.hpp:373-375`) states it must be reached only via
  `register_hardware_isolation_backend()` — a dedicated entry point
  (`sandbox_backend_registry.hpp:150-154`) built specifically because a plain `register_backend()`
  call site that drops its third `strict_eligibility` argument silently makes a hardware-isolation
  backend `Strict`-eligible process-wide (Revision 2 finding #4 of the *prior* registry draft).
  Revision 1's §2c called `register_backend("kata", ..., strict_eligibility::named_only)` directly —
  syntactically safe today, but bypassing the exact type-level guarantee the codebase built to
  prevent this landmine on a future edit. Fixed below: `register_hardware_isolation_backend(...)`.
- **SHOULD-FIX (correctness)**: the Windows wasm test-verification plan named no equivalent of the
  `ENVIRONMENT "PATH=${AGENTENGINE_WASMTIME_DLL_DIR};$ENV{PATH}"` every existing wasm-linked test
  sets (`tests/CMakeLists.txt:1724-1725` et al.) — without it the new test binary fails to start on
  Windows (`wasmtime.dll` not found). Fixed in §3/§2a below.
- **SHOULD-FIX (correctness)**: `AGENTENGINE_HAVE_WASM_BACKEND`/`AGENTENGINE_HAVE_KATA_BACKEND`
  don't need `PUBLIC` visibility — only `default_sandbox_registry.cpp` itself reads them; `PRIVATE`
  avoids leaking build config into every future consumer's compile flags for no reason. Fixed in §2a.
- **MINOR (security), added as a real guard**: nothing enforced wasm's strength staying below
  native-jail's going forward (no `static_assert`) — a future strength edit to either backend could
  silently flip `Strict` resolution. Added a compile-time guard in §2c.
- **MINOR (security), disclosed rather than fixed**: `AGENTENGINE_WITH_WASM=ON` turned on for
  unrelated reasons (e.g. tool-pipeline WASM plugins, 009's plugin ABI) now unconditionally makes
  wasm sandbox-eligible too, with no separate per-backend host opt-out — harmless today given the
  strength math (now `static_assert`-enforced), but a real coupling, named in §4.
- **Scope/overclaim pass**: independently re-verified every §0 claim (register_agent call sites,
  SandboxBackendRegistry being real/tested, §3's bounded verification scope) against the real
  tree — all confirmed true, no overclaim found. One should-fix folded in: §3 now states plainly
  that the Kata CMake wiring is *compiled by no one this pass*, not merely "inspected," so a future
  session doesn't conflate the two.

**Relates to:** `docs/planning/sandbox-backend-registry-design-draft.md` (the registry mechanism
itself — `SandboxBackendRegistry`, `register_backend()`, `resolve_strict()`/`resolve_named()` — is
real, implemented, tested code, already merged; this draft is its own §0's explicitly named,
deliberately deferred follow-up: "real production *consumption* of a resolved backend still does
not exist anywhere... this design makes backend *selection* real and runtime-configurable; it does
not, by itself, make backend *consumption* real"). `SandboxBackend` concept (`sandbox.hpp:356`),
already conformed-to by all four real backends via `static_assert`.

## 0. What this draft is and is not

Scoped narrowly, per explicit project-owner direction this session: build one real, tested,
host-callable factory function that registers the compiled-in `SandboxBackend` conformers into a
fresh `SandboxBackendRegistry` — the mechanical "how does a backend get into the registry at all"
half of the still-open gap. This draft does **not** attempt:

- Wiring any real execution path (`AgentSession`, `SandboxToolProvider`, the Python-worker path) to
  actually call `registry.resolve_*()` and use what comes back — that is "consumption," named as a
  separate, larger, higher-risk piece of work in the prior design draft's §0 and confirmed out of
  scope for this pass by the project owner directly.
- Adding a `register_agent<A>()` call site to `tools/cli_chat.cpp` — verified directly (grep across
  the whole tree, worktree/test dirs excluded) that **no real production code calls
  `register_agent<A>()` today**; the only non-test call site is `tools/policy_reachability_fixture.hpp`,
  a CI policy-check dev tool (`register_agent<EchoAgent>()`, no registry argument). `tools/cli_chat.cpp`
  — the closest thing to a real host in this tree — wires tools directly and never touches the
  `Agent<>`/`register_agent<A>()` machinery at all. So this factory function has, honestly, no real
  host to plug into yet; it is built as real, tested, reusable infrastructure a future host adopts,
  not as end-to-end wiring. Named explicitly rather than glossed over, matching this project's own
  "selection without consumption" self-critique pattern (component-role-audit tracker Findings M/N,
  cited by the prior draft).

## 1. A real constraint the naive version of this gets wrong

The four `SandboxBackend` conformers are not just platform-split, they are **separate, independently
opt-in CMake targets**:

- `agentengine_native_jail_backend` (Windows) / `agentengine_linux_native_jail_backend` (Linux) —
  always built for the current platform, no option gate.
- `agentengine_wasm_backend` — gated behind `AGENTENGINE_WITH_WASM` (`CMakeLists.txt:302`), **OFF by
  default**, pulls a real vendored wasmtime C API dependency (network fetch) when enabled.
- `agentengine_kata_backend` — gated behind `AGENTENGINE_BUILD_KATA_BACKEND` (`CMakeLists.txt:223`),
  **OFF by default**, Linux-only (`CMakeLists.txt:237`'s own `FATAL_ERROR` on `WIN32`), requires a
  live containerd + Kata Containers deployment to be useful at all.

A single header that unconditionally `#include`s all four backend headers would fail to compile for
any build with the defaults (`AGENTENGINE_WITH_WASM=OFF`, `AGENTENGINE_BUILD_KATA_BACKEND=OFF`) —
the wasm/kata backend headers and their transitive dependencies (`wasm.h`, wasmtime types) simply
are not on the include path unless those options are on. This must be its own CMake target with
conditional linking and matching preprocessor feature-detection macros, not a plain header.

Also confirmed (grep): no `include/agentengine/**` header ever includes anything under `backends/`
— `SandboxBackend` conformers' own headers live under `src/backends/*/`, each with its own
`target_include_directories(... PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")`, reached via
`#include "backends/..."` relative to that root — never from the public `include/agentengine/`
surface. This factory function follows that same convention: it lives under `src/sandbox/`, not
`include/agentengine/sandbox/`.

## 2. The design

### 2a. New CMake target: `agentengine_default_sandbox_registry`

```cmake
# ---- Default sandbox-backend registry wiring (src/sandbox/) -- always built for the current
# platform's native-jail leg; wasm/kata legs compiled in only when their own options are ON,
# matching each backend's own opt-in gate exactly (never silently assumes a heavy/Linux-only
# dependency the rest of the build didn't ask for).
add_library(agentengine_default_sandbox_registry STATIC
  src/sandbox/default_sandbox_registry.cpp)
target_include_directories(agentengine_default_sandbox_registry PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(agentengine_default_sandbox_registry PUBLIC agentengine::core)
target_link_libraries(agentengine_default_sandbox_registry PRIVATE agentengine_warnings)
if(WIN32)
  target_link_libraries(agentengine_default_sandbox_registry PUBLIC agentengine::native_jail_backend)
elseif(UNIX)
  target_link_libraries(agentengine_default_sandbox_registry PUBLIC agentengine::linux_native_jail_backend)
endif()
if(AGENTENGINE_WITH_WASM)
  target_link_libraries(agentengine_default_sandbox_registry PUBLIC agentengine::wasm_backend)
  target_compile_definitions(agentengine_default_sandbox_registry PRIVATE AGENTENGINE_HAVE_WASM_BACKEND)
endif()
if(NOT WIN32 AND AGENTENGINE_BUILD_KATA_BACKEND)
  target_link_libraries(agentengine_default_sandbox_registry PUBLIC agentengine::kata_backend)
  target_compile_definitions(agentengine_default_sandbox_registry PRIVATE AGENTENGINE_HAVE_KATA_BACKEND)
endif()
add_library(agentengine::default_sandbox_registry ALIAS agentengine_default_sandbox_registry)
```

Red-team correctness finding: only `default_sandbox_registry.cpp` itself reads these two macros
(§2b's declaration header carries no `#ifdef`), so `PRIVATE` is correct — `PUBLIC` would leak build
config into every future consumer's compile flags for no reason.

The new test target (§3) must set the same `ENVIRONMENT "PATH=${AGENTENGINE_WASMTIME_DLL_DIR};$ENV{PATH}"`
every existing wasm-linked test sets on Windows (`tests/CMakeLists.txt:1724-1725` et al., since
`agentengine_wasmtime_vendor` is a `SHARED IMPORTED` `wasmtime.dll` with no rpath on Windows) —
omitted from Revision 1, a real gap the correctness red-team caught: without it the new test binary
fails to even start on Windows once linked against `agentengine::wasm_backend`.

Placed after all four backend option blocks in `CMakeLists.txt` (so every `agentengine::*_backend`
alias it references already exists by the time this target is declared) — matching the file's own
existing convention of declaring optional feature libraries only after their dependencies.

The public macros (`AGENTENGINE_HAVE_WASM_BACKEND`/`AGENTENGINE_HAVE_KATA_BACKEND`) are new
vocabulary — grepped, nothing in this codebase defines feature-detection macros of this shape today
(everything else gates entire translation units out of the build graph rather than `#ifdef`-ing
inside one shared file). This is the one new pattern this draft introduces, and it exists because
`default_sandbox_registry.cpp` is the first file in this tree whose job is specifically "know about
every compiled-in backend, whichever subset that turns out to be."

### 2b. `src/sandbox/default_sandbox_registry.hpp` — declaration only, no backend headers visible

```cpp
#pragma once
#include "agentengine/sandbox/sandbox_backend_registry.hpp"

namespace agentengine {

// Constructs a fresh SandboxBackendRegistry and registers every SandboxBackend conformer this
// build was actually compiled with — one long-lived instance per backend, exactly the shape
// SandboxBackendRegistry::register_backend() requires (sandbox-backend-registry-design-draft.md
// Revision 2, findings #1/#2). Real, host-callable code; NOT wired into any production execution
// path yet (see this file's own design draft §0 for the honest scope boundary).
[[nodiscard]] result<SandboxBackendRegistry> build_default_sandbox_registry(
    SandboxBackendResolutionAuditHook audit_hook = nullptr);

}  // namespace agentengine
```

Callers never see `NativeJailBackend`/`WasmBackend`/`KataBackend` at all — the whole point of this
factory is that a host writes `build_default_sandbox_registry()` once and gets whatever this build
was actually compiled with, without its own code branching on `#ifdef _WIN32`/`AGENTENGINE_WITH_WASM`
itself.

### 2c. `src/sandbox/default_sandbox_registry.cpp` — the actual registration list

```cpp
#include "sandbox/default_sandbox_registry.hpp"

#if defined(_WIN32)
#include "backends/native_jail/native_jail_backend.hpp"
#elif defined(__linux__)
#include "backends/native_jail/linux_native_jail_backend.hpp"
#endif
#ifdef AGENTENGINE_HAVE_WASM_BACKEND
#include "backends/wasm/wasm_backend.hpp"
#endif
#ifdef AGENTENGINE_HAVE_KATA_BACKEND
#include "backends/kata/kata_backend.hpp"
#endif

// Red-team correctness finding: WasmBackend lives in agentengine::wasm (wasm_backend.hpp:37),
// not top-level agentengine -- Revision 1 wrote an unqualified std::make_shared<WasmBackend>()
// here, which would not compile. Also a real, cheap guard the security red-team asked for:
// wasm must never structurally have a chance at outranking native-jail in resolve_strict()'s
// ranking (§2c's own registration below relies on this staying true) -- enforced at compile time,
// not just true by coincidence today.
#if defined(_WIN32) && defined(AGENTENGINE_HAVE_WASM_BACKEND)
static_assert(wasm::WasmBackend::traits.strength < native_jail::NativeJailBackend::traits.strength,
              "wasm must never outrank native-jail's Strict-resolution priority on this platform");
#elif defined(__linux__) && defined(AGENTENGINE_HAVE_WASM_BACKEND)
static_assert(wasm::WasmBackend::traits.strength < native_jail::LinuxNativeJailBackend::traits.strength,
              "wasm must never outrank native-jail's Strict-resolution priority on this platform");
#endif

namespace agentengine {

result<SandboxBackendRegistry> build_default_sandbox_registry(
        SandboxBackendResolutionAuditHook audit_hook) {
    SandboxBackendRegistry registry(std::move(audit_hook));

#if defined(_WIN32)
    if (auto r = registry.register_backend("native-jail", std::make_shared<native_jail::NativeJailBackend>());
        !r) {
        return std::unexpected(r.error());
    }
#elif defined(__linux__)
    if (auto r = registry.register_backend("native-jail",
                                            std::make_shared<native_jail::LinuxNativeJailBackend>());
        !r) {
        return std::unexpected(r.error());
    }
#endif

#ifdef AGENTENGINE_HAVE_WASM_BACKEND
    // strict_eligible (default): strength 40 vs. native-jail's 50 -- never wins Strict resolution
    // over native-jail on a platform where both are registered (the static_assert above makes this
    // a compile-time-enforced fact, not just true by coincidence today), only competes when
    // native-jail isn't (there is no such build configuration today, since native-jail has no off
    // switch, but resolve_strict()'s own ranking makes this the correct posture regardless).
    // Disclosed coupling (security red-team): AGENTENGINE_WITH_WASM=ON turned on for an unrelated
    // reason (e.g. 009's WASM plugin ABI) now unconditionally makes wasm sandbox-eligible too, with
    // no separate per-backend host opt-out -- harmless given the strength guard above, but real.
    if (auto r = registry.register_backend("wasm", std::make_shared<wasm::WasmBackend>()); !r) {
        return std::unexpected(r.error());
    }
#endif

#ifdef AGENTENGINE_HAVE_KATA_BACKEND
    // register_hardware_isolation_backend(), NOT register_backend() with a manual named_only
    // argument (security red-team's real finding, Revision 1's actual bug): KataBackend's own
    // header contract (kata_backend.hpp:373-375) names this exact entry point -- it exists
    // specifically so a hardware-isolation-class backend can never become Strict-eligible through a
    // dropped/miscopied third argument at some future edit of this call site (Revision 2 finding #4
    // of the prior registry draft: the blast radius of a stronger backend silently winning Strict
    // process-wide). This is a type-level guarantee, not a convention this file has to remember to
    // uphold by writing the right literal every time.
    if (auto r = registry.register_hardware_isolation_backend("kata", std::make_shared<kata::KataBackend>());
        !r) {
        return std::unexpected(r.error());
    }
#endif

    return registry;
}

}  // namespace agentengine
```

**Backend constructor arguments**: every conformer's own defaulted constructor is used as-is
(`LinuxNativeJailBackend`'s `delegated_cgroup_root`/`jail_root_base` defaults,
`KataBackend`'s `runtime_type`/`image`/CNI defaults, `WasmBackend`'s default ctor,
`NativeJailBackend`'s default ctor) — this draft adds no new host-config surface for overriding
them (e.g. reading `AGENTENGINE_KATA_IMAGE` from an env var). A real config-driven override path is
future work, out of scope here; naming it as a residual rather than silently deciding it's
unnecessary.

**Registering `KataBackend` even though this build usually has no real containerd/Kata deployment
behind it**: deliberate, not an oversight. "Registered" only means `resolve_named("kata")` can find
it; `create()` still runs its own real precondition checks (containerd socket reachable, runtime
class configured, etc.) and fails closed exactly as it does today without a registry — a deployer
who names `"kata"` on a host without the real deployment gets the same failure they'd get calling
`KataBackend::create()` directly, not a new failure mode this draft introduces.

## 3. Verification plan

- Default build (this session's actual environment: Windows, `AGENTENGINE_WITH_WASM=OFF`,
  `AGENTENGINE_BUILD_KATA_BACKEND=OFF`, the untouched defaults): confirms the native-jail-only leg
  compiles, links, and a new test proves `resolve_strict(current_platform())` and
  `resolve_named("native-jail")` both resolve to the registered `NativeJailBackend` — and that
  `AGENTENGINE_HAVE_WASM_BACKEND`/`AGENTENGINE_HAVE_KATA_BACKEND` are correctly undefined (no
  attempt to reference `WasmBackend`/`KataBackend` symbols that don't exist in this build).
- Reconfigure with `-DAGENTENGINE_WITH_WASM=ON` (real network fetch of the pinned, checksummed
  wasmtime release, per project-owner go-ahead this session): confirms the wasm leg actually
  compiles and links against the real `WasmBackend`, and that the same test's wasm-gated assertions
  (`resolve_named("wasm")` succeeds; `resolve_strict()`'s winner is still `"native-jail"`, not
  `"wasm"`, proving the strength-40-vs-50 ranking behaves as designed with two real, registered
  candidates) pass for real, not just by code inspection. The new test target must carry the same
  `set_tests_properties(... PROPERTIES ENVIRONMENT "PATH=${AGENTENGINE_WASMTIME_DLL_DIR};$ENV{PATH}")`
  every existing wasm-linked test already sets on Windows (`tests/CMakeLists.txt:1724-1725` et al.)
  — `agentengine_wasmtime_vendor` is a `SHARED IMPORTED` `wasmtime.dll` with no rpath on Windows;
  without this the new test binary fails to even start once linked against `agentengine::wasm_backend`.
- Kata leg: Linux-only, `AGENTENGINE_BUILD_KATA_BACKEND` requires a live containerd+Kata deployment
  neither available on this Windows dev box. The CMake wiring (§2a's kata block) is checked by eye
  against the exact gating pattern the real `agentengine_kata_backend` target already uses (`NOT
  WIN32 AND AGENTENGINE_BUILD_KATA_BACKEND`, `CMakeLists.txt:227`) — stated plainly, per the
  scope/overclaim red-team: this means **compiled by no one this pass**, not merely "inspected and
  therefore equivalent to verified." A disclosed residual, not a claimed-covered gap; a future
  session with real Linux+Kata CI should build-verify it before treating it as settled.
- `tools/naming_lint.py`: `default_sandbox_registry.hpp`/`.cpp` introduce no new
  `agentengine`-namespace top-level types (the one function, `build_default_sandbox_registry`, is a
  plain lowercase function name, not new vocabulary needing an `ae-naming-lint: allow`).
- Full existing `ctest` suite: must stay green — this draft adds one new test file and one new
  optional CMake target, touches no existing header/source file.

## 4. I2/I3 check

Same posture the prior registry draft's §3 already established, unchanged by this draft: this
factory is host-only code, called explicitly by whatever future host adopts it, never reachable
from model output or a tool call. It introduces no new authority — every backend it registers was
already a real, already-conforming `SandboxBackend` type; this draft only makes constructing and
registering the compiled-in set mechanical instead of hand-written per host. `KataBackend`'s
`named_only` registration is itself an I2-shaped decision (§2c above) — a stronger-`strength`,
infrastructure-dependent backend must be asked for explicitly, never silently win routing for every
agent just because this build happened to compile it in.

## 5. Residuals, named not silently assumed complete

- No production host calls this function yet (§0) — selection infrastructure, not consumption
  wiring, and not yet reachable from any real running program.
- No config-driven override of backend constructor arguments (§2c) — every backend uses its own
  built-in defaults.
- Kata leg is code-inspection-verified only this pass, not build-verified (§3) — no Linux
  environment with a real Kata deployment available in this session.
- `AGENTENGINE_HAVE_WASM_BACKEND`/`AGENTENGINE_HAVE_KATA_BACKEND` are new feature-detection macros,
  the first of their kind in this codebase — if a future backend needs the same treatment, reuse
  this pattern rather than inventing a third one.
