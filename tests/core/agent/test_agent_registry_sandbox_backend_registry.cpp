// Proves docs/planning/sandbox-backend-registry-design-draft.md (Revision 2)'s wiring into
// register_agent<A>(): the SandboxBackendRegistry parameter, required since ADR-244 slice 4b (no `nullptr` default;
// an empty registry is `choice.missing.sandbox_backend`).
// SandboxBackendRegistry itself is proven separately (test_sandbox_backend_registry.cpp); this file
// proves check_sandbox_profile_availability() actually consults it through the real
// compiler<A,...>::run() call chain (agent_registry.hpp), and that a concrete-backend
// SandboxProfile<P> is provably unaffected either way (its availability is a compile-time fact, not
// a registry lookup -- ADR-012).

#include <cstdio>
#include <memory>
#include <string>

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/sandbox/sandbox_backend_registry.hpp"

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
using agentengine::task;

// A concrete SandboxBackend conformer, supporting both target platforms -- used to prove the
// is_strict=false path never consults the registry at all.
struct ConcreteBackend {
    static constexpr ProfileTraits traits{5, platform_id::windows_x86_64 | platform_id::linux_x86_64,
                                           cold_start_class::milliseconds};
    task<result<SandboxHandle>> create(SandboxSpec, EffectContext&) { co_return SandboxHandle{}; }
    task<result<ExecOutcome>> exec(SandboxHandle&, ExecRequest, EffectContext&) { co_return ExecOutcome{}; }
    task<void> destroy(SandboxHandle&) { co_return; }
};

// The current-platform-supporting backend a real deployment would register for Strict to resolve.
struct AvailableBackend {
    static constexpr ProfileTraits traits{
        30, platform_id::windows_x86_64 | platform_id::linux_x86_64, cold_start_class::milliseconds};
    task<result<SandboxHandle>> create(SandboxSpec, EffectContext&) { co_return SandboxHandle{}; }
    task<result<ExecOutcome>> exec(SandboxHandle&, ExecRequest, EffectContext&) { co_return ExecOutcome{}; }
    task<void> destroy(SandboxHandle&) { co_return; }
};

// No SandboxProfile<...> tag -- 002 §3's table default, Strict.
struct StrictAgent
    : agentengine::Agent<StrictAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                         agentengine::MaxTurns<agentengine::at_most(16)>,
                         agentengine::TokenBudget<agentengine::unbounded()>,
                         agentengine::Approval<agentengine::approval_mode::policy_driven>,
                         agentengine::Concurrency<agentengine::concurrency_mode::sequential>,
                         agentengine::SandboxProfile<agentengine::Strict>> {
    static constexpr std::string_view name = "strict-agent";
    static constexpr std::string_view instructions = "Defaults to SandboxProfile<Strict>.";
};

struct ConcreteAgent
    : agentengine::Agent<ConcreteAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                         agentengine::SandboxProfile<ConcreteBackend>,
                         agentengine::MaxTurns<agentengine::at_most(16)>,
                         agentengine::TokenBudget<agentengine::unbounded()>,
                         agentengine::Approval<agentengine::approval_mode::policy_driven>,
                         agentengine::Concurrency<agentengine::concurrency_mode::sequential>> {
    static constexpr std::string_view name = "concrete-agent";
    static constexpr std::string_view instructions = "Names a concrete backend directly.";
};

}  // namespace

int main() {
    using agentengine::register_agent;
    using agentengine::SandboxBackendRegistry;

    // ---- ADR-244 slice 4b (flipped): the registry is required, and an EMPTY one is a choice nobody made. Before,
    //      a null registry made Strict "not evaluated" and an empty one failed with the resolver's own code.
    {
        SandboxBackendRegistry empty_registry;
        auto meta = register_agent<StrictAgent>(nullptr, empty_registry);
        check(!meta.has_value() && meta.error().code == "choice.missing.sandbox_backend",
              "an empty SandboxBackendRegistry fails choice.missing.sandbox_backend, naming how to register one");
        if (!meta.has_value()) {
            check(meta.error().message.find("register_backend") != std::string::npos,
                  "... and the message lists the option (register a backend)");
        }
    }

    // ---- A registry with entries, none of them able to win Strict: the resolver's own error, unmodified. ------
    {
        SandboxBackendRegistry registry;
        auto registered = registry.register_backend("named-only", std::make_shared<AvailableBackend>(),
                                                    agentengine::strict_eligibility::named_only);
        check(registered.has_value(), "setup: registering a named-only backend succeeds");
        auto meta = register_agent<StrictAgent>(nullptr, registry);
        check(!meta.has_value() && meta.error().code == "sandbox_backend_registry.no_strict_candidate",
              "Strict against a registry with no strict-eligible backend fails closed (008 §3's 'no fallback -> "
              "startup fails'), with the resolver's own code");
    }

    // ---- A real, current-platform-supporting backend: Strict resolves and registration succeeds. ------------
    {
        SandboxBackendRegistry registry;
        auto instance = std::make_shared<AvailableBackend>();
        auto registered = registry.register_backend("available", instance);
        check(registered.has_value(), "setup: registering a real backend succeeds");

        auto meta = register_agent<StrictAgent>(nullptr, registry);
        check(meta.has_value(),
              "with a current-platform-supporting backend registered, Strict resolves for real and registration "
              "succeeds");
    }

    // ---- A concrete SandboxProfile<P> never consults WHICH backends are registered (its availability was proven at
    //      compile time, ADR-012), but the registry must exist: empty is refused, non-empty registers. ----------
    {
        SandboxBackendRegistry empty_registry;
        auto refused = register_agent<ConcreteAgent>(nullptr, empty_registry);
        check(!refused.has_value() && refused.error().code == "choice.missing.sandbox_backend",
              "a concrete SandboxProfile<P> agent against an EMPTY registry is refused too (ADR-244 slice 4b: the "
              "registry is where the host states which backends exist)");

        SandboxBackendRegistry registry;
        auto registered = registry.register_backend("elsewhere", std::make_shared<AvailableBackend>(),
                                                    agentengine::strict_eligibility::named_only);
        check(registered.has_value(), "setup: registering an unrelated backend succeeds");
        auto meta = register_agent<ConcreteAgent>(nullptr, registry);
        check(meta.has_value(),
              "a concrete SandboxProfile<P> agent registers against a non-empty registry that does not hold its own "
              "backend (ADR-012: availability is a compile-time fact, not a registry lookup)");
        if (meta.has_value()) {
            check(!meta->sandbox_profile->is_strict && meta->sandbox_profile->traits.strength == 5,
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
