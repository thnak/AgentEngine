// Proves decisions/ADR-244-required-choices-step-1b.md §2.2 / G-C8 (slice 6): the host registers each SandboxBackend it
// chose, by name, and the real compiled-in conformers register with the right names and the right strict_eligibility.
// This replaces test_default_sandbox_registry: `build_default_sandbox_registry()` (a backend by linkage and platform,
// chosen for the host by whatever the build happened to contain) is gone, and `lint_no_default_sandbox_registry` fails
// if the symbol comes back. The registry MECHANISM is proven against fakes by test_sandbox_backend_registry.cpp; this
// file touches the real conformers, the way a host wires them.
//
//   1. native-jail registers, is strict_eligible, wins resolve_strict(current_platform()) and resolves by name.
//   2. (AGENTENGINE_HAVE_WASM_BACKEND only) "wasm" registers and resolves by name, but never outranks native-jail --
//      the strength-40-vs-50 ranking, with two real registered candidates and a compile-time guard.
//   3. (AGENTENGINE_HAVE_KATA_BACKEND only) "kata" registers through register_hardware_isolation_backend(), so it is
//      named_only: resolve_strict()'s winner is never "kata" regardless of its higher strength.
//   4. An empty registry is refused at the point an agent would use it (`choice.missing.sandbox_backend`, proven in
//      test_agent_registry_sandbox_backend_registry): nothing here falls back to a backend the host did not register.

#include <cstdio>
#include <cstdlib>
#include <memory>

#include "../support/kata_images.hpp"

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
#include "agentengine/sandbox/sandbox_backend_registry.hpp"

using namespace agentengine;

#if defined(_WIN32) && defined(AGENTENGINE_HAVE_WASM_BACKEND)
static_assert(wasm::WasmBackend::traits.strength < native_jail::NativeJailBackend::traits.strength,
              "wasm must never outrank the native jail in Strict resolution on this platform");
#elif defined(__linux__) && defined(AGENTENGINE_HAVE_WASM_BACKEND)
static_assert(wasm::WasmBackend::traits.strength < native_jail::LinuxNativeJailBackend::traits.strength,
              "wasm must never outrank the native jail in Strict resolution on this platform");
#endif

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

}  // namespace

int main() {
    SandboxBackendRegistry registry;

    // ---- 1. native-jail: the host registers it; strict_eligible; resolvable both ways. -----------
#if defined(_WIN32)
    auto registered = registry.register_backend("native-jail", std::make_shared<native_jail::NativeJailBackend>());
#elif defined(__linux__)
    auto registered =
        registry.register_backend("native-jail", std::make_shared<native_jail::LinuxNativeJailBackend>());
#endif
    check(registered.has_value(), "the host registers native-jail");
    {
        auto named = registry.resolve_named(HostSandboxSelection{"native-jail"});
        check(named.has_value(), "resolve_named(native-jail) resolves");
        auto strict = registry.resolve_strict(current_platform());
        check(strict.has_value(), "resolve_strict(current_platform()) resolves");
        if (strict.has_value()) {
            check((*strict)->name == "native-jail", "resolve_strict()'s winner is native-jail");
        }
    }

#ifdef AGENTENGINE_HAVE_WASM_BACKEND
    // ---- 2. wasm: registered by the host, strict_eligible, but never outranks native-jail. ------
    {
        check(registry.register_backend("wasm", std::make_shared<wasm::WasmBackend>()).has_value(),
              "the host registers wasm");
        check(registry.resolve_named(HostSandboxSelection{"wasm"}).has_value(), "resolve_named(wasm) resolves");
        auto strict = registry.resolve_strict(current_platform());
        check(strict.has_value() && (*strict)->name == "native-jail",
              "resolve_strict()'s winner stays native-jail with wasm (strength 40) also registered (50)");
    }
#endif

#ifdef AGENTENGINE_HAVE_KATA_BACKEND
    // ---- 3. kata: hardware isolation, named_only -- never a resolve_strict() candidate. --------
    {
        check(registry.register_hardware_isolation_backend("kata", std::make_shared<kata::KataBackend>(ae_test_kata::kRuntime, ae_test_kata::kBusybox)).has_value(),
              "the host registers kata");
        check(registry.resolve_named(HostSandboxSelection{"kata"}).has_value(), "resolve_named(kata) resolves");
        auto strict = registry.resolve_strict(current_platform());
        check(strict.has_value() && (*strict)->name != "kata",
              "resolve_strict() never picks kata -- named_only, regardless of its higher strength");
    }
#endif

    if (g_failures == 0) std::printf("test_real_backend_registration: all checks passed\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
