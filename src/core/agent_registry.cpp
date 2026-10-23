// Implements rfcs/002-Agent-Model-and-Authoring.md §6 -- the non-template half of core/agent_registry.hpp (ADR-244
// header-split plan, 2026-10-23): the 002 §6 checks, `validate_agent_metadata` and `invoke_agent_tool`, compiled once
// here instead of in each of the ~30 translation units that include the header. Moved verbatim; the header keeps
// every type, the policy extraction and the `register_agent<A>()` template, and declares these functions.
// A validator edit now recompiles this file only.

#include "agentengine/core/agent_registry.hpp"

#include "agentengine/core/agent_choice_options.hpp"
#include "agentengine/core/choice_error.hpp"
#include "agentengine/rt/block_on.hpp"  // ADR-237 §5.2: drive the async pipeline from this sync host edge
#include "agentengine/sandbox/sandbox_backend_registry.hpp"

namespace agentengine {

namespace agent_detail {

result<void> check_required_choices(AgentMetadata const& meta) {
    namespace opt = agent_choice_options;
    if (!meta.max_turns.has_value()) return std::unexpected(choice_missing("max_turns", opt::bound));
    if (!meta.token_budget.has_value()) return std::unexpected(choice_missing("token_budget", opt::bound));
    if (!meta.approval.has_value()) return std::unexpected(choice_missing("approval", opt::approval));
    if (!meta.concurrency.has_value()) return std::unexpected(choice_missing("concurrency", opt::concurrency));
    if (!meta.sandbox_profile.has_value()) {
        return std::unexpected(choice_missing("sandbox_profile", opt::sandbox_profile));
    }
    return {};
}

result<void> check_chat_client_id(std::string_view id) {
    if (id.empty()) {
        return std::unexpected(error{failure_class::contract,
                                      "agent declares no ChatClientId<\"vendor:model\"> binding "
                                      "(002 §3: required, no default)",
                                      "agent.chat_client_id_missing"});
    }
    return {};
}

result<void> check_chat_client_credentials(std::string_view id,
                                                                  ChatClientRegistry const* registry) {
    if (registry == nullptr) return {};
    if (!registry->find(id).has_value()) {
        return std::unexpected(error{failure_class::contract,
                                      "ChatClientId '" + std::string(id) +
                                          "' has no bound backend in the supplied ChatClientRegistry",
                                      "agent.chat_client_id_unregistered"});
    }
    return {};
}

result<void> check_tool_name_collision(std::vector<ToolDescriptor> const& tools) {
    for (std::size_t i = 0; i < tools.size(); ++i) {
        for (std::size_t j = i + 1; j < tools.size(); ++j) {
            if (tools[i].name == tools[j].name) {
                return std::unexpected(
                    error{failure_class::contract,
                          "two declared tools share the name '" + tools[i].name + "'",
                          "agent.tool_name_collision"});
            }
        }
    }
    return {};
}

result<void> check_capability_ceiling(CapabilitySet const& ceiling,
                                                             std::vector<ToolDescriptor> const& tools) {
    for (ToolDescriptor const& tool : tools) {
        for (Capability const& requirement : tool.capability_ceiling) {
            // ADR-217: the pipeline's own admission rule. At run time `invoke_agent_tool` binds the
            // tool under `attenuate(ceiling)`, so a grant-enforcing tool admitted here is bound to
            // (and enforces) the ceiling's caps; every other tool keeps the exact `contains()` rule.
            if (!ceiling_entry_admitted(ceiling, tool, requirement)) {
                return std::unexpected(error{
                    failure_class::policy,
                    "tool '" + tool.name + "' declares a capability the agent's ceiling does not cover",
                    "agent.capability_ceiling_exceeded"});
            }
        }
    }
    return {};
}

result<void> check_sandbox_profile_availability(
        SandboxProfileDescriptor const& desc, SandboxBackendRegistry const& registry) {
    // ADR-244 §2.2 (slice 4b): a registry nothing was registered in is a choice nobody made -- the host registers
    // each backend it chose, and there is no default registry to fall back on. Refused for a concrete-backend
    // profile as well (the registry is where the host states which backends exist).
    if (registry.empty()) {
        return std::unexpected(choice_missing("sandbox_backend", agent_choice_options::sandbox_backend));
    }
    if (!desc.is_strict) return {};
    return registry.resolve_strict(current_platform()).transform([](auto*) { return; });
}

result<void> check_tool_sandbox_profile_compatibility() { return {}; }

result<void> check_output_schema_enforceable(
    std::optional<std::string> const& output_schema_json, ChatClientRegistry const* registry,
    std::string_view chat_client_id) {
    if (!output_schema_json.has_value() || registry == nullptr) return {};
    if (!registry->find(chat_client_id).has_value()) {
        // Already caught by check_chat_client_credentials, which the compiler runs first --
        // defensive redundancy in case call order ever changes, not a second, divergent code path.
        return std::unexpected(error{failure_class::contract,
                                      "ChatClientId '" + std::string(chat_client_id) +
                                          "' has no bound backend to check OutputSchema enforceability against",
                                      "agent.chat_client_id_unregistered"});
    }
    return {};
}

result<void> check_handoff_cycle() { return {}; }

}  // namespace agent_detail

result<void> validate_agent_metadata(AgentMetadata const& meta,
                                                          ChatClientRegistry const* registry,
                                                          SandboxBackendRegistry const& sandbox_registry) {
    using namespace agent_detail;
    // ADR-244 §2.2 (slice 4b): a choice nobody made is a named error before anything about its value.
    if (auto r = check_required_choices(meta); !r) return r;
    // ADR-244 §2.1: zero is refused at every check site -- a turn cap or budget of 0 is not "none allowed" (that
    // has its own mechanism) and not "unset". Both surfaces arrive here (I6), so `at_most(0)` and YAML
    // `max_turns: 0` are refused alike (the CRTP path also refuses it at compile time, `at_most` is consteval).
    if (auto r = check_bound("max_turns", *meta.max_turns); !r) return r;
    if (auto r = check_bound("token_budget", *meta.token_budget); !r) return r;
    if (auto r = check_chat_client_id(meta.chat_client_id); !r) return r;
    if (auto r = check_chat_client_credentials(meta.chat_client_id, registry); !r) return r;
    if (auto r = check_tool_name_collision(meta.tools.descriptors()); !r) return r;
    CapabilitySet const ceiling = CapabilitySet::grant_root(meta.capability_ceiling);
    if (auto r = check_capability_ceiling(ceiling, meta.tools.descriptors()); !r) return r;
    if (auto r = check_sandbox_profile_availability(*meta.sandbox_profile, sandbox_registry); !r) return r;
    if (auto r = check_tool_sandbox_profile_compatibility(); !r) return r;
    if (auto r = check_output_schema_enforceable(meta.output_schema_json, registry, meta.chat_client_id); !r) {
        return r;
    }
    if (auto r = check_handoff_cycle(); !r) return r;
    return {};
}

ToolResult invoke_agent_tool(AgentMetadata const& meta, ToolCallRequest const& request,
                                                   EffectContext& ctx, ApprovalDecider const& approve,
                                                   ToolInvocationAudit* audit_out,
                                                   // ADR-070: appended last, default `{}`, threaded
                                                   // straight through to `invoke_tool()` below --
                                                   // same seam, same fail-closed-when-unset contract.
                                                   PolicyDecider const& policy) {
    auto fail_closed = [&](error const& e) -> ToolResult {
        if (audit_out) {
            audit_out->call_id = request.call_id;
            audit_out->tool_name = request.tool_name;
            audit_out->ok = false;
            audit_out->error_code = e.code;
            audit_out->result_bytes = 0;
            audit_out->duration = std::chrono::steady_clock::duration{};
        }
        return tool_pipeline_detail::make_error_result(request.call_id, e);
    };

    if (!ctx.capabilities) {
        return fail_closed(error{failure_class::policy,
                                  "invoke_agent_tool: caller has no capabilities to attenuate from",
                                  "agent_call.no_caller_capabilities"});
    }
    result<CapabilitySet> attenuated = ctx.capabilities->attenuate(meta.capability_ceiling);
    if (!attenuated) {
        return fail_closed(attenuated.error());
    }
    // ADR-237 §5.2: `invoke_tool` is async; this host helper is a genuinely-sync edge, so block on it.
    return rt::block_on(invoke_tool(meta.tools, *attenuated, request, ctx, approve, audit_out, policy));
}

}  // namespace agentengine
