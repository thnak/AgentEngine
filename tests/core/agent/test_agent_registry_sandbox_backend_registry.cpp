// Proves docs/planning/sandbox-backend-registry-design-draft.md (Revision 2)'s wiring into
// register_agent<A>(): the second, independent, additive SandboxBackendRegistry const* parameter.
// SandboxBackendRegistry itself is proven separately (test_sandbox_backend_registry.cpp); this file
// proves check_sandbox_profile_availability() actually consults it through the real
// compiler<A,...>::run() call chain (agent_registry.hpp), and that a concrete-backend
// SandboxProfile<P> is provably unaffected either way (its availability is a compile-time fact, not
// a registry lookup -- ADR-012).

#include <cstdio>
#include <memory>
#include <string>

#include "agentengine/core/agent_registry.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

using agentengine::cold_start_class;
using agentengine::EffectContext;
using agentengine::error;
using agentengine::ExecOutcome;
using agentengine::ExecRequest;
using agentengine::platform_id;
using agentengine::ProfileTraits;
using agentengine::result;
using agentengine::SandboxHandle;
using agentengine::SandboxSpec;

// A concrete SandboxBackend conformer, supporting both target platforms -- used to prove the
// is_strict=false path never consults the registry at all.
struct ConcreteBackend {
    static constexpr ProfileTraits traits{5, platform_id::windows_x86_64 | platform_id::linux_x86_64,
                                           cold_start_class::milliseconds};
    result<SandboxHandle> create(SandboxSpec const&, EffectContext&) { return SandboxHandle{}; }
    result<ExecOutcome> exec(SandboxHandle&, ExecRequest const&, EffectContext&) { return ExecOutcome{}; }
    void destroy(SandboxHandle&) {}
};

// The current-platform-supporting backend a real deployment would register for Strict to resolve.
struct AvailableBackend {
    static constexpr ProfileTraits traits{
        30, platform_id::windows_x86_64 | platform_id::linux_x86_64, cold_start_class::milliseconds};
    result<SandboxHandle> create(SandboxSpec const&, EffectContext&) { return SandboxHandle{}; }
    result<ExecOutcome> exec(SandboxHandle&, ExecRequest const&, EffectContext&) { return ExecOutcome{}; }
    void destroy(SandboxHandle&) {}
};

// No SandboxProfile<...> tag -- 002 §3's table default, Strict.
struct StrictAgent
    : agentengine::Agent<StrictAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">> {
    static constexpr std::string_view name = "strict-agent";
    static constexpr std::string_view instructions = "Defaults to SandboxProfile<Strict>.";
};

struct ConcreteAgent
    : agentengine::Agent<ConcreteAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                          agentengine::SandboxProfile<ConcreteBackend>> {
    static constexpr std::string_view name = "concrete-agent";
    static constexpr std::string_view instructions = "Names a concrete backend directly.";
};

}  // namespace

int main() {
    using agentengine::register_agent;
    using agentengine::SandboxBackendRegistry;

    // ---- No SandboxBackendRegistry supplied: Strict stays the honest pre-registry stub, unaffected
    //      by this change -- every pre-existing zero/one-arg call site keeps passing. -------------
    {
        auto meta = register_agent<StrictAgent>();  // zero-arg, exactly like every pre-existing site
        check(meta.has_value(),
              "with no SandboxBackendRegistry supplied, Strict still registers cleanly "
              "(pre-registry always-pass stub, unaffected by this wiring)");
    }

    // ---- A SandboxBackendRegistry supplied, but empty: Strict now fails closed for real. ---------
    {
        SandboxBackendRegistry empty_registry;
        auto meta = register_agent<StrictAgent>(nullptr, &empty_registry);
        check(!meta.has_value(),
              "with a SandboxBackendRegistry supplied and nothing registered, Strict now fails "
              "closed for real (008 §3's 'no fallback -> startup fails'), not a silent pass");
        if (!meta.has_value()) {
            check(meta.error().code == "sandbox_backend_registry.no_strict_candidate",
                  "the real registry error code surfaces through register_agent<A>() unmodified");
        }
    }

    // ---- A SandboxBackendRegistry supplied with a real, current-platform-supporting backend:
    //      Strict resolves and registration succeeds. -------------------------------------------
    {
        SandboxBackendRegistry registry;
        auto instance = std::make_shared<AvailableBackend>();
        auto registered = registry.register_backend("available", instance);
        check(registered.has_value(), "setup: registering a real backend succeeds");

        auto meta = register_agent<StrictAgent>(nullptr, &registry);
        check(meta.has_value(),
              "with a SandboxBackendRegistry supplied and a current-platform-supporting backend "
              "registered, Strict resolves for real and registration succeeds");
    }

    // ---- A concrete SandboxProfile<P> is unaffected by the registry either way -- proven at
    //      compile time already, so an empty registry never rejects it. -------------------------
    {
        SandboxBackendRegistry empty_registry;
        auto meta = register_agent<ConcreteAgent>(nullptr, &empty_registry);
        check(meta.has_value(),
              "a concrete SandboxProfile<P> agent registers cleanly even against an empty "
              "SandboxBackendRegistry -- its availability was already proven at compile time "
              "(ADR-012), so check_sandbox_profile_availability() never consults the registry for "
              "this shape");
        if (meta.has_value()) {
            check(!meta->sandbox_profile.is_strict && meta->sandbox_profile.traits.strength == 5,
                  "the compiled metadata still carries the real, concrete backend's own traits");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_agent_registry_sandbox_backend_registry: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_registry_sandbox_backend_registry: %d FAILURE(S)\n", g_failures);
    return 1;
}
