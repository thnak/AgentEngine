# `src/backends/wasm`

Implements `agentengine::SandboxBackend` (`include/agentengine/sandbox/sandbox.hpp`) for the
`wasm` profile: wasmtime + WASI 0.3 Component Model, the plugin ABI (**009-Plugin-and-Extension-
System.md**). Software, capability-based isolation with no ambient authority, µs–low-ms cold start,
identical across the target platform set (021 §2 — Windows now, Linux next; macOS is not a target)
— per **008-Sandbox-and-Isolation.md §3**. This is where plugin
(`ae:tool`/`ae:skill`/`ae:provider`/`ae:memory`/`ae:filter`/`ae:codec`) execution and the
deterministic-mode/snapshot machinery (008 §5–§6a) live.

A seam backend (CONVENTIONS.md tier 2): may take the one heavy dependency (wasmtime), behind a
CMake option, never linked into a build that does not select this backend.

Real, tested `SandboxBackend` conformer since Milestone 2 Phase D
(`decisions/ADR-010-wasm-component-host-manifest-capability-binding.md`): `create`/`load_component`/
`list_tools`/`invoke_tool`/`destroy`, capability-verified `ae:tool` component loading, and (M2 Phase
F1, `decisions/ADR-011-first-party-egress-proxy.md`) a real host-mediated egress path for
`http-request`. `wasm_tool_bridge.hpp` (`decisions/ADR-040-wasm-tool-pipeline-bridge.md`) wires a
loaded component's discovered tools into the real `core/tool_pipeline.hpp` `ToolTable`/`invoke_tool`
pipeline, gated behind a `cap::ToolCall` per plugin-qualified tool name. Every piece is
security-critical and went through `design -> red-team -> prove -> judge` per CLAUDE.md before
landing as real code.

Remaining scope gaps, named not silently dropped: no signature/publisher verification, no
AOT-cache-by-digest, no instance pooling, `blob`/`tool-call` host imports recognized in the WIT
contract but never linked (ADR-010 §9), and no production loader anywhere in this repo yet — every
caller today (`tests/test_wasm_backend.cpp`, `tests/test_wasm_tool_bridge.cpp`) already has a loaded
handle by construction (ADR-040 §2's own residual).
