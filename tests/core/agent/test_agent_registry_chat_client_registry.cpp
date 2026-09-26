// Milestone 5 Phase B6 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md):
// register_agent<A>()'s additive ChatClientRegistry parameter — check_chat_client_credentials and
// check_output_schema_enforceable run for real when a registry is supplied (and stay the pre-M5
// always-pass stub, proven already by test_agent_registry.cpp, when it isn't). Separate file from
// test_agent_registry.cpp: this one needs its own OutputSchema<T>-bearing agent types and doesn't
// touch that file's existing E2/E4 coverage.

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

struct WeatherResult {
    std::string summary;
    int temperature_f = 0;
};
AE_JSON_SCHEMA(WeatherResult, summary, temperature_f)

struct RegisteredAgent
    : agentengine::Agent<RegisteredAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">> {
    static constexpr std::string_view name = "registered";
    static constexpr std::string_view instructions = "Bound to a ChatClientId the registry knows.";
};

struct UnregisteredAgent
    : agentengine::Agent<UnregisteredAgent, agentengine::ChatClientId<"openai:gpt-5">> {
    static constexpr std::string_view name = "unregistered";
    static constexpr std::string_view instructions = "Bound to a ChatClientId the registry does NOT know.";
};

struct StructuredOutputAgent
    : agentengine::Agent<StructuredOutputAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                          agentengine::OutputSchema<WeatherResult>> {
    static constexpr std::string_view name = "structured-output";
    static constexpr std::string_view instructions = "Declares OutputSchema<WeatherResult>.";
};

}  // namespace

int main() {
    using agentengine::ChatClientCapabilities;
    using agentengine::ChatClientRegistry;
    using agentengine::output_schema_strategy;
    using agentengine::register_agent;

    ChatClientCapabilities native_caps;
    native_caps.structured_output_native = true;
    ChatClientRegistry registry;
    registry.register_client("anthropic:claude-opus-5", native_caps);
    // Deliberately no entry for "openai:gpt-5" -- UnregisteredAgent's own negative case.

    // ---- with no registry: pre-M5 stub behavior is unchanged (both checks always pass) -----------
    {
        auto meta = register_agent<UnregisteredAgent>();  // zero-arg call, exactly like every pre-M5 site
        check(meta.has_value(),
              "with no registry supplied, an unregistered ChatClientId still registers (pre-M5 "
              "always-pass stub, unaffected by Phase B6)");
    }

    // ---- with a registry: a registered ChatClientId passes ---------------------------------------
    {
        auto meta = register_agent<RegisteredAgent>(&registry);
        check(meta.has_value(), "a ChatClientId present in the supplied registry registers successfully");
    }

    // ---- with a registry: an unregistered ChatClientId is rejected -------------------------------
    {
        auto meta = register_agent<UnregisteredAgent>(&registry);
        check(!meta.has_value(),
              "with a registry supplied, a ChatClientId absent from it is now rejected");
        if (!meta.has_value()) {
            check(meta.error().code == "agent.chat_client_id_unregistered", "specific diagnostic code");
        }
    }

    // ---- OutputSchema<T> + a registry: strategy is chosen and recorded ----------------------------
    {
        auto meta = register_agent<StructuredOutputAgent>(&registry);
        check(meta.has_value(), "an agent with OutputSchema<T> registers against a capable ChatClient");
        if (meta.has_value()) {
            check(meta->output_schema_json.has_value() &&
                      meta->output_schema_json->find("summary") != std::string::npos,
                  "output_schema_json is real JSON Schema text derived from WeatherResult");
            check(meta->output_schema_strategy_chosen.has_value() &&
                      *meta->output_schema_strategy_chosen == output_schema_strategy::native,
                  "the chosen strategy reflects the registered ChatClient's real capabilities "
                  "(structured_output_native -> native)");
        }
    }

    // ---- OutputSchema<T> with NO registry: nothing to enforce against, honestly not evaluated -----
    {
        auto meta = register_agent<StructuredOutputAgent>();  // zero-arg, no registry
        check(meta.has_value(), "OutputSchema<T> with no registry still registers (nothing to check against yet)");
        if (meta.has_value()) {
            check(meta->output_schema_json.has_value(),
                  "output_schema_json is still compiled from the declared OutputSchema<T> "
                  "regardless of whether a registry was supplied");
            check(!meta->output_schema_strategy_chosen.has_value(),
                  "output_schema_strategy_chosen stays nullopt (not evaluated, not a guessed default) "
                  "when no registry was supplied to check it against");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_agent_registry_chat_client_registry: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_registry_chat_client_registry: %d FAILURE(S)\n", g_failures);
    return 1;
}
