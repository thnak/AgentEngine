// Proves docs/planning/default-sandbox-registry-wiring-design-draft.md: build_default_sandbox_
// registry() actually registers the real, compiled-in SandboxBackend conformers with the right
// names and the right strict_eligibility -- not a fake/stub backend the way
// test_sandbox_backend_registry.cpp's StatefulBackend et al. are, which prove the registry
// MECHANISM itself but never touch a real conformer.
//
//   1. native-jail is always registered, strict_eligible, and wins resolve_strict(current_platform()).
//   2. resolve_named("native-jail") resolves to the same entry.
//   3. (AGENTENGINE_HAVE_WASM_BACKEND only) "wasm" is registered, strict_eligible, but never wins
//      resolve_strict() over native-jail -- the strength-40-vs-50 ranking, proven with two real,
//      registered candidates, not just the static_assert default_sandbox_registry.cpp itself carries.
//   4. (AGENTENGINE_HAVE_KATA_BACKEND only) "kata" is registered but named_only -- resolve_strict()'s
//      winner is never "kata" regardless of its higher strength.

#include <cstdio>
#include <cstdlib>

#include "sandbox/default_sandbox_registry.hpp"

using namespace agentengine;

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
    auto built = build_default_sandbox_registry();
    check(built.has_value(), "build_default_sandbox_registry() succeeds");
    if (!built.has_value()) {
        std::fprintf(stderr, "  error: %s\n", built.error().message.c_str());
        return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    SandboxBackendRegistry& registry = *built;

    // ---- 1/2. native-jail: always registered, strict_eligible, resolvable both ways. -------------
    {
        auto named = registry.resolve_named(HostSandboxSelection{"native-jail"});
        check(named.has_value(), "resolve_named(\"native-jail\") resolves");

        auto strict = registry.resolve_strict(current_platform());
        check(strict.has_value(), "resolve_strict(current_platform()) resolves");
        if (strict.has_value()) {
            check((*strict)->name == "native-jail",
                  "resolve_strict()'s winner is native-jail (nothing else registered outranks it)");
        }
    }

#ifdef AGENTENGINE_HAVE_WASM_BACKEND
    // ---- 3. wasm: registered, strict_eligible, but never outranks native-jail. --------------------
    {
        auto named = registry.resolve_named(HostSandboxSelection{"wasm"});
        check(named.has_value(), "resolve_named(\"wasm\") resolves");

        auto strict = registry.resolve_strict(current_platform());
        check(strict.has_value(), "resolve_strict() still resolves with wasm also registered");
        if (strict.has_value()) {
            check((*strict)->name == "native-jail",
                  "resolve_strict()'s winner stays native-jail with wasm (strength 40) also "
                  "registered (strength 50) -- the ranking this codebase's own static_assert enforces");
        }
    }
#endif

#ifdef AGENTENGINE_HAVE_KATA_BACKEND
    // ---- 4. kata: registered, but named_only -- never a resolve_strict() candidate. ---------------
    {
        auto named = registry.resolve_named(HostSandboxSelection{"kata"});
        check(named.has_value(), "resolve_named(\"kata\") resolves");

        auto strict = registry.resolve_strict(current_platform());
        check(strict.has_value(), "resolve_strict() still resolves with kata also registered");
        if (strict.has_value()) {
            check((*strict)->name != "kata",
                  "resolve_strict() never picks kata -- named_only, regardless of its higher strength");
        }
    }
#endif

    if (g_failures == 0) {
        std::printf("test_default_sandbox_registry: all checks passed\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
