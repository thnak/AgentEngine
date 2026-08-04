# `src/backends/wasm`

Implements `agentengine::SandboxBackend` (`include/agentengine/sandbox/sandbox.hpp`) for the
`wasm` profile: wasmtime + WASI 0.3 Component Model, the plugin ABI (**009-Plugin-and-Extension-
System.md**). Software, capability-based isolation with no ambient authority, µs–low-ms cold start,
identical on Windows/Linux — per **008-Sandbox-and-Isolation.md §3**. This is where plugin
(`ae:tool`/`ae:skill`/`ae:provider`/`ae:memory`/`ae:filter`/`ae:codec`) execution and the
deterministic-mode/snapshot machinery (008 §5–§6a) live.

A seam backend (CONVENTIONS.md tier 2): may take the one heavy dependency (wasmtime), behind a
CMake option, never linked into a build that does not select this backend. No real logic yet —
enforcement is security-critical and goes through `design -> red-team -> prove -> judge` per
CLAUDE.md before it is real code.
