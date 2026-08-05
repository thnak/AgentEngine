// Proves M2 Phase E task E2 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// register_agent<A>() is a real metadata compiler, not an empty CRTP base. Covers the checks E2
// actually implements for real (E4's own scope note names the two headline classes: capability-
// ceiling-mismatch and tool-name-collision) plus the two cheap-to-prove extras this task added
// (ChatClientId presence, Stateless<N> vs. session-state via std::is_empty_v) -- one positive case
// establishing the compiled metadata's default values, and one negative case per real check.

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

// -- a trivial native tool, gated by a single Entropy capability (matches test_tool_pipeline.cpp's
// own EchoTool -- no reason to invent a second shape for the same job) ---------------------------

struct EchoArgs {
    std::string message;
};
AE_JSON_SCHEMA(EchoArgs, message)

struct EchoReply {
    std::string echoed;
};
AE_JSON_SCHEMA(EchoReply, echoed)

struct EchoTool : agentengine::Tool<EchoTool, agentengine::Capabilities<agentengine::cap::decl::Entropy>> {
    static constexpr std::string_view name = "echo";
    static constexpr std::string_view description = "Echo the input message back.";
    using Args = EchoArgs;
    using Reply = EchoReply;

    static agentengine::result<Reply> invoke(Args args, agentengine::EffectContext&) {
        return Reply{"echo: " + args.message};
    }
};

// A second tool sharing EchoTool's own name -- the tool-name-collision negative case's only job.
struct DuplicateNameTool : agentengine::Tool<DuplicateNameTool> {
    static constexpr std::string_view name = "echo";  // deliberately collides with EchoTool::name
    static constexpr std::string_view description = "Also named 'echo'.";
    using Args = EchoArgs;
    using Reply = EchoReply;

    static agentengine::result<Reply> invoke(Args args, agentengine::EffectContext&) {
        return Reply{args.message};
    }
};

// -- agents ----------------------------------------------------------------------------------------

struct WellFormedAgent
    : agentengine::Agent<WellFormedAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                          agentengine::Tools<EchoTool>, agentengine::Capabilities<agentengine::cap::decl::Entropy>> {
    static constexpr std::string_view name = "well-formed";
    static constexpr std::string_view instructions = "Register cleanly; used for the positive case.";
};

struct NoChatClientAgent : agentengine::Agent<NoChatClientAgent> {
    static constexpr std::string_view name = "no-chat-client";
    static constexpr std::string_view instructions = "Declares no ChatClientId<...> at all.";
};

struct NameCollisionAgent
    : agentengine::Agent<NameCollisionAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                          agentengine::Tools<EchoTool, DuplicateNameTool>> {
    static constexpr std::string_view name = "name-collision";
    static constexpr std::string_view instructions = "Two tools both named 'echo'.";
};

// Declares EchoTool (needs Entropy) but grants no capabilities at all -- the agent's ceiling can't
// cover what the tool requires.
struct CapabilityGapAgent
    : agentengine::Agent<CapabilityGapAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                          agentengine::Tools<EchoTool>> {
    static constexpr std::string_view name = "capability-gap";
    static constexpr std::string_view instructions = "Declares EchoTool but grants it no capabilities.";
};

struct StatelessOkAgent
    : agentengine::Agent<StatelessOkAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                          agentengine::Stateless<4>> {
    static constexpr std::string_view name = "stateless-ok";
    static constexpr std::string_view instructions = "Stateless<N> on a genuinely empty type.";
};

// Carries instance data -- Stateless<N> combined with session-state usage, 002 §6's own wording.
struct StatelessViolationAgent
    : agentengine::Agent<StatelessViolationAgent, agentengine::ChatClientId<"anthropic:claude-opus-5">,
                          agentengine::Stateless<4>> {
    static constexpr std::string_view name = "stateless-violation";
    static constexpr std::string_view instructions = "Stateless<N> but carries an instance field.";
    int turn_count = 0;  // session state -- exactly what Stateless<N> claims does not exist
};

}  // namespace

int main() {
    using agentengine::register_agent;

    // -- positive: compiles, and every default from 002 §3's table lands correctly -----------------
    {
        auto meta = register_agent<WellFormedAgent>();
        check(meta.has_value(), "well-formed agent registers successfully");
        if (meta) {
            check(meta->agent_name == "well-formed", "agent_name carried through from static name");
            check(meta->chat_client_id == "anthropic:claude-opus-5", "chat_client_id extracted");
            check(meta->tools.find("echo") != nullptr, "declared tool present in the compiled table");
            check(meta->capability_ceiling.size() == 1, "capability ceiling has exactly the one declared cap");
            check(meta->max_turns == 16, "MaxTurns default is 16 (002 §3 table)");
            check(!meta->token_budget.has_value(), "TokenBudget default is unbounded (nullopt)");
            check(meta->approval == agentengine::approval_mode::policy_driven,
                  "Approval default is policy_driven for an Agent (differs from Tool's never_require)");
            check(meta->concurrency == agentengine::concurrency_mode::sequential,
                  "Concurrency default is sequential");
            check(meta->telemetry == agentengine::telemetry_capture::metadata_only,
                  "Telemetry default is metadata_only");
            check(!meta->stateless_pool_size.has_value(), "Stateless default is off (absent, not 0)");
        }
    }

    // -- negative: ChatClientId missing --------------------------------------------------------------
    {
        auto meta = register_agent<NoChatClientAgent>();
        check(!meta.has_value(), "agent with no ChatClientId<...> is rejected");
        if (!meta) check(meta.error().code == "agent.chat_client_id_missing", "specific diagnostic code");
    }

    // -- negative: tool-name collision (E4's headline class) ------------------------------------------
    {
        auto meta = register_agent<NameCollisionAgent>();
        check(!meta.has_value(), "two tools sharing a name are rejected");
        if (!meta) check(meta.error().code == "agent.tool_name_collision", "specific diagnostic code");
    }

    // -- negative: capability-ceiling-mismatch (E4's other headline class) ---------------------------
    {
        auto meta = register_agent<CapabilityGapAgent>();
        check(!meta.has_value(), "a tool needing an uncovered capability is rejected");
        if (!meta) check(meta.error().code == "agent.capability_ceiling_exceeded", "specific diagnostic code");
    }

    // -- positive: Stateless<N> on a genuinely empty type is fine -------------------------------------
    {
        auto meta = register_agent<StatelessOkAgent>();
        check(meta.has_value(), "Stateless<N> on an empty agent type registers successfully");
        if (meta) check(meta->stateless_pool_size == 4u, "stateless_pool_size carried through");
    }

    // -- negative: Stateless<N> combined with session-state usage -------------------------------------
    {
        auto meta = register_agent<StatelessViolationAgent>();
        check(!meta.has_value(), "Stateless<N> on a type carrying instance data is rejected");
        if (!meta) check(meta.error().code == "agent.stateless_session_state", "specific diagnostic code");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_agent_registry: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_registry: %d FAILURE(S)\n", g_failures);
    return 1;
}
