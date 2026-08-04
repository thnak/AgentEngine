M2 Phase D — WASM plugin host (009), once Phase C exists to run it in

## Context

Milestone 2 exit criterion (`docs/planning/v1-implementation-roadmap.md`): *"...and one WASM
`ae:tool` component loads and executes (009 §10 G1)."* Depends on Phase C's `SandboxBackend`
existing (the `wasm` profile is one of its concrete backends). Locked decision (CLAUDE.md): the
WASM Component Model (WASI 0.3) is the plugin ABI for tools, skills, providers, memory stores,
filters, and the C/C++ library track (009 §7) — never relitigated without an ADR.

**`wit/` is currently README-only — no `.wit` files exist.** 009's own header names this directory
"the contract of record"; authoring the `ae:tool` world (D2) is a real, previously-invisible task
this project's M2 survey surfaced, not just plumbing.

Wasmtime 47.0.3 (resolving OQ-7) enters the build as a new CMake-optional seam-backend dependency
(`AGENTENGINE_WITH_WASM`, off by default like any CONVENTIONS.md tier-2 heavy dependency), Windows
+ Linux. The `SandboxBackend` contract itself stays std+Quark-only; only the concrete backend under
`src/backends/wasm/` links Wasmtime.

## Tasks

- **D1.** Wasmtime 47.0.3 wired in as `AGENTENGINE_WITH_WASM`, Windows + Linux. **Size: M**
- **D2.** `wit/ae-tool.wit` authored — the `ae:tool` world (009 §2), closing the "contract of
  record is currently empty" gap. **Size: M**
- **D3.** Minimal WASM component host: load, verify manifest-vs-imports (009 §4/§10 G2),
  instantiate under the `wasm` `SandboxBackend` profile, invoke, destroy. **Size: XL**
- **D4.** One real `ae:tool` component (a trivial echo/add tool from a Component-Model-capable
  toolchain) loads and executes identically across platforms — 009 §10 G1, the milestone's other
  named exit-criterion item. **Size: L**
- **D5.** Manifest-capability-mismatch negative proof (miniature 009 §10 G2). **Size: S**

## What's explicitly deferred past this phase (see breakdown doc's full list)

- 009's G4/G4a (warm-invocation/streaming budgets vs. 023, which stays `TBD-baselined` until M8).
- 009's G6 (one real C/C++ library shipped as a plugin, 009 §7).
- 008's G8 (snapshot fidelity, `wasm`-only) — needs Phase D built out further than this minimal
  host.

## Exit criteria

- Wasmtime dependency gated correctly behind `AGENTENGINE_WITH_WASM`, never linked into a default
  build.
- `wit/ae-tool.wit` exists and is the real contract the host and any component compile against.
- A real `ae:tool` component loads, is manifest-verified, executes, and is torn down identically on
  Windows and Linux — the roadmap's named exit-criterion sentence, measured not asserted.
- A manifest/imports mismatch is rejected with a structured, correctly classified error, proven by
  a negative test.
- Full test suite green on Windows and Linux/gcc-14.
