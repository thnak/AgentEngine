// Proves decisions/ADR-012-sandbox-profile-template-parameter-kind.md: SandboxProfile<P>'s template-
// parameter kind (a real SandboxBackend, or the Strict resolution selector -- never an enum value)
// round-trips correctly through register_agent<A>() into AgentMetadata::sandbox_profile.
//
//   1. No SandboxProfile<...> tag declared at all -> the 002 §3 table default (Strict) is compiled
//      in, not left unset.
//   2. SandboxProfile<Strict> declared explicitly -> the same is_strict=true result as the default
//      (proves the two spellings -- absence and explicit Strict -- are genuinely equivalent, not
//      just coincidentally both "true" today).
//   3. SandboxProfile<ConformingBackend> declared -> is_strict=false and traits == the backend's own
//      real, compile-time ProfileTraits -- not a copy that could drift from what the type itself
//      declares.
//   4. check_sandbox_profile_availability() (agent_registry.hpp) never rejects either shape: the
//      concrete-backend case is already proven safe at compile time (SandboxProfileArg<P>), and the
//      Strict case has nothing to resolve against yet (no Engine-level backend registry, M2 scope) --
//      both compiled agents register cleanly.

#include <cstdio>
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

struct ConformingBackend {
    static constexpr agentengine::ProfileTraits traits{
        7, agentengine::platform_id::windows_x86_64 | agentengine::platform_id::linux_x86_64,
        agentengine::cold_start_class::milliseconds};

    agentengine::result<agentengine::SandboxHandle> create(agentengine::SandboxSpec const&,
                                                             agentengine::EffectContext&) {
        return agentengine::SandboxHandle{};
    }
    agentengine::result<agentengine::ExecOutcome> exec(agentengine::SandboxHandle&,
                                                         agentengine::ExecRequest const&,
                                                         agentengine::EffectContext&) {
        return agentengine::ExecOutcome{};
    }
    void destroy(agentengine::SandboxHandle&) {}
};
static_assert(agentengine::SandboxBackend<ConformingBackend>);
static_assert(agentengine::SandboxProfileArg<ConformingBackend>);
static_assert(agentengine::SandboxProfileArg<agentengine::Strict>);
static_assert(!agentengine::SandboxProfileArg<int>);

struct DefaultProfileAgent
    : agentengine::Agent<DefaultProfileAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">> {
    static constexpr std::string_view name = "default-profile-agent";
    static constexpr std::string_view instructions = "Declares no SandboxProfile<...> tag at all.";
};

struct StrictAgent : agentengine::Agent<StrictAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                                         agentengine::SandboxProfile<agentengine::Strict>> {
    static constexpr std::string_view name = "strict-agent";
    static constexpr std::string_view instructions = "Declares SandboxProfile<Strict> explicitly.";
};

struct ConcreteProfileAgent
    : agentengine::Agent<ConcreteProfileAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                          agentengine::SandboxProfile<ConformingBackend>> {
    static constexpr std::string_view name = "concrete-profile-agent";
    static constexpr std::string_view instructions = "Declares SandboxProfile<ConformingBackend>.";
};

}  // namespace

int main() {
    using agentengine::register_agent;

    // -- 1/4. no tag declared -> Strict default, registers cleanly -------------------------------
    {
        auto meta = register_agent<DefaultProfileAgent>();
        check(meta.has_value(), "DefaultProfileAgent registers cleanly");
        if (meta) {
            check(meta->sandbox_profile.is_strict,
                  "no SandboxProfile<...> tag declared -> is_strict defaults to true (002 §3 table default)");
        }
    }

    // -- 2/4. explicit SandboxProfile<Strict> -> the same result as the default -------------------
    {
        auto meta = register_agent<StrictAgent>();
        check(meta.has_value(), "StrictAgent registers cleanly");
        if (meta) {
            check(meta->sandbox_profile.is_strict,
                  "SandboxProfile<Strict> declared explicitly -> is_strict is true, same as the default");
        }
    }

    // -- 3/4. a real conforming backend -> is_strict=false, real traits extracted -----------------
    {
        auto meta = register_agent<ConcreteProfileAgent>();
        check(meta.has_value(), "ConcreteProfileAgent registers cleanly");
        if (meta) {
            check(!meta->sandbox_profile.is_strict,
                  "SandboxProfile<ConformingBackend> declared -> is_strict is false");
            check(meta->sandbox_profile.traits.strength == ConformingBackend::traits.strength &&
                      meta->sandbox_profile.traits.platform_mask == ConformingBackend::traits.platform_mask &&
                      meta->sandbox_profile.traits.cold_start == ConformingBackend::traits.cold_start,
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
