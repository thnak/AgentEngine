// Proves decisions/archive/ADR-012-sandbox-profile-template-parameter-kind.md: SandboxProfile<P>'s template-
// parameter kind (a real SandboxBackend, or the Strict resolution selector -- never an enum value)
// round-trips correctly through register_agent<A>() into AgentMetadata::sandbox_profile.
//
//   1. (ADR-244 slice 4b) No SandboxProfile<...> tag is a compile error -- tests/compile_fail's
//      agent_rejects_* gates -- so there is no "default Strict" left to prove; the declaration says it.
//   2. SandboxProfile<Strict> declared explicitly -> is_strict=true, compiled in as a stated value.
//   3. SandboxProfile<ConformingBackend> declared -> is_strict=false and traits == the backend's own
//      real, compile-time ProfileTraits -- not a copy that could drift from what the type itself
//      declares.
//   4. check_sandbox_profile_availability() (agent_registry.hpp) never rejects either shape when no
//      SandboxBackendRegistry is supplied (every register_agent<A>() call below is zero-arg): the
//      concrete-backend case is already proven safe at compile time (SandboxProfileArg<P>), and the
//      Strict case stays the honest "not evaluated" pre-registry stub -- both compiled agents
//      register cleanly. The real, registry-backed Strict resolution path (docs/planning/sandbox-
//      backend-registry-design-draft.md) is proven separately in test_sandbox_backend_registry.cpp
//      and test_agent_registry_sandbox_backend_registry.cpp.

#include <cstdio>
#include <string>

#include "agentengine/core/agent_registry.hpp"
#include "../support/stub_sandboxes.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

struct ConformingBackend {
    static constexpr agentengine::ProfileTraits traits{
        7, agentengine::platform_id::windows_x86_64 | agentengine::platform_id::linux_x86_64,
        agentengine::cold_start_class::milliseconds};

    agentengine::task<agentengine::result<agentengine::SandboxHandle>> create(agentengine::SandboxSpec,
                                                             agentengine::EffectContext&) {
        co_return agentengine::SandboxHandle{};
    }
    agentengine::task<agentengine::result<agentengine::ExecOutcome>> exec(agentengine::SandboxHandle&,
                                                         agentengine::ExecRequest,
                                                         agentengine::EffectContext&) {
        co_return agentengine::ExecOutcome{};
    }
    agentengine::task<void> destroy(agentengine::SandboxHandle&) { co_return; }
};
static_assert(agentengine::SandboxBackend<ConformingBackend>);
static_assert(agentengine::SandboxProfileArg<ConformingBackend>);
static_assert(agentengine::SandboxProfileArg<agentengine::Strict>);
static_assert(!agentengine::SandboxProfileArg<int>);

struct StrictAgent : agentengine::Agent<StrictAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                                        agentengine::SandboxProfile<agentengine::Strict>,
                                        agentengine::MaxTurns<agentengine::at_most(16)>,
                                        agentengine::TokenBudget<agentengine::unbounded()>,
                                        agentengine::Approval<agentengine::approval_mode::policy_driven>,
                                        agentengine::Concurrency<agentengine::concurrency_mode::sequential>> {
    static constexpr std::string_view name = "strict-agent";
    static constexpr std::string_view instructions = "Declares SandboxProfile<Strict> explicitly.";
};

struct ConcreteProfileAgent
    : agentengine::Agent<ConcreteProfileAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                         agentengine::SandboxProfile<ConformingBackend>,
                         agentengine::MaxTurns<agentengine::at_most(16)>,
                         agentengine::TokenBudget<agentengine::unbounded()>,
                         agentengine::Approval<agentengine::approval_mode::policy_driven>,
                         agentengine::Concurrency<agentengine::concurrency_mode::sequential>> {
    static constexpr std::string_view name = "concrete-profile-agent";
    static constexpr std::string_view instructions = "Declares SandboxProfile<ConformingBackend>.";
};

}  // namespace

int main() {
    using agentengine::register_agent;

    // -- 2/4. explicit SandboxProfile<Strict> -> is_strict -------------------
    {
        auto meta = register_agent<StrictAgent>(nullptr, ae_test::sandboxes());
        check(meta.has_value(), "StrictAgent registers cleanly");
        if (meta) {
            check(meta->sandbox_profile->is_strict,
                  "SandboxProfile<Strict> declared -> is_strict is true");
        }
    }

    // -- 3/4. a real conforming backend -> is_strict=false, real traits extracted -----------------
    {
        auto meta = register_agent<ConcreteProfileAgent>(nullptr, ae_test::sandboxes());
        check(meta.has_value(), "ConcreteProfileAgent registers cleanly");
        if (meta) {
            check(!meta->sandbox_profile->is_strict,
                  "SandboxProfile<ConformingBackend> declared -> is_strict is false");
            check(meta->sandbox_profile->traits.strength == ConformingBackend::traits.strength &&
                      meta->sandbox_profile->traits.platform_mask == ConformingBackend::traits.platform_mask &&
                      meta->sandbox_profile->traits.cold_start == ConformingBackend::traits.cold_start,
                  "the extracted traits exactly match ConformingBackend::traits, not a stale copy");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_sandbox_profile_kind: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_sandbox_profile_kind: %d FAILURE(S)\n", g_failures);
    return 1;
}
