#include "sandbox/default_sandbox_registry.hpp"

#include <memory>
#include <utility>

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

// WasmBackend lives in agentengine::wasm (wasm_backend.hpp:37), not top-level agentengine. The
// static_asserts below are a real, cheap compile-time guard for the ranking claim the wasm
// registration comment relies on: wasm must never structurally have a chance at outranking
// native-jail in resolve_strict()'s ranking -- enforced here, not just true by coincidence, so a
// future strength edit to either backend fails loudly instead of silently changing routing.
#if defined(_WIN32) && defined(AGENTENGINE_HAVE_WASM_BACKEND)
static_assert(agentengine::wasm::WasmBackend::traits.strength <
                  agentengine::native_jail::NativeJailBackend::traits.strength,
              "wasm must never outrank native-jail's Strict-resolution priority on this platform");
#elif defined(__linux__) && defined(AGENTENGINE_HAVE_WASM_BACKEND)
static_assert(agentengine::wasm::WasmBackend::traits.strength <
                  agentengine::native_jail::LinuxNativeJailBackend::traits.strength,
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
    // strict_eligible (default): strength 40 vs. native-jail's 50 -- the static_assert above makes
    // "wasm never wins over native-jail" a compile-time-enforced fact, not just true today by
    // coincidence. Disclosed coupling: AGENTENGINE_WITH_WASM=ON turned on for an unrelated reason
    // (e.g. 009's WASM plugin ABI) now unconditionally makes wasm sandbox-eligible too, with no
    // separate per-backend host opt-out -- harmless given the strength guard, but real.
    if (auto r = registry.register_backend("wasm", std::make_shared<wasm::WasmBackend>()); !r) {
        return std::unexpected(r.error());
    }
#endif

#ifdef AGENTENGINE_HAVE_KATA_BACKEND
    // register_hardware_isolation_backend(), NOT register_backend() with a manual named_only
    // argument: KataBackend's own header contract (kata_backend.hpp:373-375) names this exact
    // entry point -- it exists specifically so a hardware-isolation-class backend can never become
    // Strict-eligible through a dropped/miscopied third argument at some future edit of this call
    // site (the prior sandbox-backend-registry-design-draft.md's Revision 2 finding #4: the blast
    // radius of a stronger backend silently winning Strict process-wide for every agent).
    if (auto r = registry.register_hardware_isolation_backend("kata", std::make_shared<kata::KataBackend>());
        !r) {
        return std::unexpected(r.error());
    }
#endif

    return registry;
}

}  // namespace agentengine
