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

## Known issue found during Phase E verification (2026-08-05) — Linux-only intermittent segfault

Phase D itself is done (D1-D5) and this is not a regression from anything in Phase E (`test_wasm_backend`/`wasm_backend.cpp`/the Rust fixture are byte-for-byte what D5 committed as `6e56a28`) — flagged here, not fixed here, because fixing it means editing `src/backends/wasm/wasm_backend.cpp`, a security-critical component that went through Phase D's own design→red-team→prove→judge cycle (ADR-010) and should get the same rigor for a change, not a drive-by patch from an unrelated task.

**Finding**: `test_wasm_backend`, run standalone and repeatedly on a fresh Linux container (the same `gcc:14` + no-cargo methodology D3-D5/E2 all use), segfaults on roughly **6 of 20 runs (~30%)**. Every observed failure's captured stdout shows *every* assertion already printed `ok:` — including the very last of D5's 8 gated-callback probes (`resolve-secret/wrong-kind`) — with the crash landing immediately after, before the test's own final summary line prints. This points at a use-after-free/double-free during per-probe teardown (`SandboxHandle`/`Instance` destruction, or the wasmtime resource-handle cleanup path) rather than anything in the assertions themselves — consistent with ADR-010 §7.5's own documented bug class from D3 ("`wasmtime_component_val_delete` already frees embedded resource pointers recursively"), now possibly recurring given D5 grew the test from 1 to 9 handle-creating probes per run, which would raise the odds of hitting an intermittent teardown race that a smaller D3/D4 run mostly didn't trigger.

**Not reproduced on Windows**: 20/20 standalone runs of `test_wasm_backend.exe` on Windows native passed clean. Windows and Linux link wasmtime differently (DLL vs. static archive, D1's own finding) — consistent with a platform-specific memory-safety bug in the teardown path, not a logic error the assertions themselves would catch on either platform.

**Not a duplicate of the already-known native_jail flake** (this doc's own C-phase sibling, `docs/issues/m2-phase-c-native-jail-sandbox.md`, and every Phase C/D/E verification pass's own writeup): that one is an OOM-detection assertion mismatch under `ctest -j4` resource contention on `test_native_jail_backend_windows`, Windows-only, no crash. This is a Linux-only process crash after all assertions already passed — a different defect class entirely.

**Scope decision**: tracked here, not investigated further as part of M2 Phase E (task E2, `register_agent<A>()`) — unrelated surface, and `wasm_backend.cpp` changes need their own red-team pass per CLAUDE.md. Should be picked up before Phase D is treated as fully closed for production use, or whenever Phase F's cross-cutting security work next touches the WASM backend.
