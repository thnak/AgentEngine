# AgentEngine implementation conventions — the coding contract

This file binds all code (human- or agent-written) to the RFC specs. **Every change must follow
the specs (`001`–`024`) and the proven decisions (`decisions/ADR-*`).** When code and a spec
disagree, **the spec wins**; if the spec is genuinely wrong, fix the spec first (RFC-style, backed
by an ADR), then the code — never silently diverge. This rule is inherited from Quark and is not
negotiable here either.

## Target & scope

- **Cross-platform is a v1 requirement, not a later port.** Windows 11 / x86-64 and Linux / x86-64
  are both first-class. The engine is developed on Windows and Linux simultaneously; a change that
  builds on only one is not done. **macOS is not a target platform** (021 §7 OQ-1, resolved
  2026-08-04): Quark has no macOS PAL backend, nobody has claimed the work of building one, and
  every macOS claim in this project was aspirational rather than CI-backed. Dropped rather than
  carried as permanent debt.
- **All OS specifics go through a seam.** Quark's PAL (spec 019) for scheduler/IO/clock/net; the
  Sandbox backend interface (008) for isolation; the plugin host (009) for WASM. AgentEngine core
  contains **no** `#ifdef _WIN32` outside those seams.
- **Isolation parity is a gate, not a goal.** Any capability the sandbox seam exposes must be
  enforceable on all three OSes or it does not enter the seam. Backends may differ in *strength*
  (documented per profile in 008); they may not differ in *contract*.

## Language & dependencies

- **C++23, std-first core.** `std::expected`, `<coroutine>`, `std::stop_token`, `std::pmr`,
  concepts + deducing-this. Matches Quark, which the core sits directly on.
- **Error model: `ae::result<T> = std::expected<T, ae::error>`.** No exceptions for control flow.
  Hot paths are `noexcept`. Exceptions may surface only from cold setup paths.
- **Dependency posture — the three tiers.**
  1. **Core (`include/agentengine/core`)**: std + Quark only. No third-party dependency, ever.
  2. **Seam backends (`src/backends/*`)**: may take a heavy dependency (wasmtime, an HTTP client,
     a JSON library, a TLS stack), one dependency per backend, behind a CMake option, never
     linked into a build that does not select that backend.
  3. **Plugins (WASM components)**: may depend on anything that compiles to WASI. This is the
     designated home for the open-source C/C++ ecosystem — see 009. A library that would be a
     heavy host dependency should be evaluated as a plugin first.
- **No .NET / managed-runtime vocabulary** in names, comments, or design. MAF supplies the *shape*
  of the developer model, not its spelling.
- **No RTTI, no reflection, no `virtual` for policy on the hot path.** Policies are CRTP template
  parameters resolved to metadata at startup (002). Type erasure is permitted only at declared
  seams (provider, sandbox backend, store) and never inside a turn's hot loop.

## Security rules (these are tested, not trusted — 007, 008)

- **The empty-capability default is a compile-time default.** A sandbox constructed without an
  explicit capability set has no filesystem, no network, no environment, no subprocess, no clock
  entropy. There is no constructor that grants "everything".
- **No capability may be derived from model output.** Any code path where a string that originated
  in a model response reaches a capability-granting API is a defect. There is a static check
  (tainted-string type) plus a test for this per RFC 007.
- **Every effect is attributed at the point of effect**, not reconstructed afterwards: the
  `EffectContext` is a required parameter, not an ambient thread-local.
- **Secrets never enter a message, a log, a span attribute, or a checkpoint.** They are resolved
  through a `SecretRef` at the point of use and redacted by construction (018).
- **A sandbox escape or capability-widening bug is a release blocker**, ranked above any
  correctness or performance regression.

## Hot-path rules (023)

- **Zero heap allocations in the per-token streaming path** (measured with a hooked allocator).
- **A run's steady state costs one Quark activation**, not a thread, and not a fiber.
- **Session footprint is budgeted**: idle sessions per GB is a benchmarked number, not an estimate.
- Sandbox creation is amortized: profiles declare whether they pool, snapshot, or cold-start, and
  each declares a measured p50/p99 (023).

## Layout

```
include/agentengine/
  core/            L2 agent core: agent, session, run, tool plane, provider seam, middleware
  workflow/        L3 orchestration: graph, edges, checkpointing
  protocol/        L4 surfaces: mcp/, a2a/, agui/, openai/
  trust/           L1 capability model, principal, policy, audit
  sandbox/         L1 sandbox seam (backend-agnostic contract only)
  plugin/          L1 WASM component host + WIT bindings
  detail/          private internals — not user-facing
src/
  backends/        sandbox backends (wasm/, native_jail/, remote/) + provider clients
  ...              non-template translation units
third_party/quark/ Quark submodule — unmodified, never patched in tree
wit/               WIT worlds defining the plugin ABI (009) — versioned, the contract of record
plugins/           first-party plugin sources (build to .wasm components)
schema/            JSON Schema for the declarative format (015) + protocol fixtures
tests/             correctness gate, one per load-bearing invariant (CTest)
conformance/       protocol conformance suites (011, 012, 013) — the I7 gate
bench/             budget gate (023)
samples/           runnable programs over the public surface
decisions/         ADRs — design → red-team → prove → judge records
docs/research/     dated research records with sources
NNN-*.md           the RFC specs (authoritative)
```

## Naming & files

- Namespace `agentengine`, with the sanctioned alias `namespace ae = agentengine;`. Private helpers
  in `agentengine::detail`. Protocol code in `agentengine::mcp`, `::a2a`, `::agui`.
- **Types** `PascalCase`, **functions/members/variables** `snake_case`, **macros** `AE_UPPER`,
  **policies** `PascalCase` template types (`Sandbox<Strict>`, `MaxTurns<12>`).
- **Every source file names the spec(s)/ADR(s) it implements** in a top comment, e.g.
  `// Implements 008-Sandbox §Capability set — native-jail backend; ADR-00x.`
- Header guards: `#pragma once`.

## Protocol code rules

- **Wire types are generated or schema-checked, never hand-drifted.** Each protocol RFC names the
  upstream schema artifact and its revision (e.g. MCP `2026-07-28` `schema.json`); a build step
  validates our types against it.
- **The protocol revision is a build-visible constant**, and the conformance suite is tagged with
  it. Supporting two revisions means two constants and two suites, not a runtime `if`.
- **Never let a protocol type leak into the core.** L4 translates to L2 vocabulary at the boundary;
  `agentengine::core` contains no `mcp::` or `a2a::` type.

## Build & test — machine safety

The Quark dev-box rule applies verbatim to this repo, because the Quark suite runs here too:

- Build with **`-j4` max** (`cmake --build build -j4`); TSan builds `-j1`. Never `-j$(nproc)`.
- Pin tests and benchmarks to ≤ 4 cores (`taskset -c 0-3` on Linux; the equivalent affinity mask on
  Windows). Never spawn `hardware_concurrency()` threads.
- **Sandbox tests are hostile by design** and must be pinned and resource-capped: a test that
  proves a fork bomb is contained must not be able to take the dev box with it.
- Compile clean under **MSVC 19.4x**, **g++ 14+**, and **clang 20+**, `-std=c++23 /W4 -Wall -Wextra`.
- Correctness tests run under **ASan/UBSan**, and **TSan** for anything with cross-thread edges.
- **A load-bearing invariant without a test, a hot path without a bench, or a protocol claim
  without a conformance run, is not done.**
