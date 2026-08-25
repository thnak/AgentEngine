#pragma once
// docs/planning/default-sandbox-registry-wiring-design-draft.md: real host-callable factory that
// registers every SandboxBackend conformer this build was actually compiled with into a fresh
// SandboxBackendRegistry. Callers never see NativeJailBackend/LinuxNativeJailBackend/WasmBackend/
// KataBackend directly -- the whole point is a host writes build_default_sandbox_registry() once
// and gets whatever this build was compiled with, without branching on _WIN32/AGENTENGINE_WITH_WASM
// itself. Lives under src/sandbox/, not include/agentengine/sandbox/, matching this codebase's own
// convention that SandboxBackend conformers' own headers (src/backends/*/) are never reachable from
// the public include/agentengine/ surface -- constructing them is itself a src-level concern.
//
// HONEST SCOPE (design draft §0): this is real, tested, reusable infrastructure -- it is NOT wired
// into any production execution path. No host in this tree calls register_agent<A>() with a real
// agent type today (verified directly, grep across the whole tree excluding tests/worktrees), so
// this function has, as of this writing, no real caller. "Selection," not "consumption" -- the
// design draft's own named, deliberate scope boundary.

#include "agentengine/sandbox/sandbox_backend_registry.hpp"

namespace agentengine {

// Constructs a fresh SandboxBackendRegistry and registers one long-lived instance of each
// SandboxBackend conformer this build was compiled with (design draft §2c): native-jail
// (NativeJailBackend on Windows, LinuxNativeJailBackend on Linux -- always available, no build
// option), wasm (WasmBackend, only when AGENTENGINE_WITH_WASM was ON at configure time), kata
// (KataBackend, only when AGENTENGINE_BUILD_KATA_BACKEND was ON -- Linux only, registered via
// register_hardware_isolation_backend() so it can never become Strict-eligible by a
// dropped/miscopied argument at some future edit). Every backend uses its own built-in
// constructor defaults -- no config-driven override in this pass (design draft §5 residual).
[[nodiscard]] result<SandboxBackendRegistry> build_default_sandbox_registry(
    SandboxBackendResolutionAuditHook audit_hook = nullptr);

}  // namespace agentengine
