#pragma once
// agentengine::rt::interaction_to_json/interaction_from_json -- shared JSON codec for
// agentengine::Interaction (core/interaction.hpp). Extracted here because rt/agent_session.hpp and
// rt/workflow_supervisor.hpp each need it for their own record codecs (AgentSessionRecord's
// open_interactions, RunStateRecord's OpenPortRecord::interaction) and previously carried BYTE-FOR-
// BYTE duplicate copies (deliberately, per each file's own former comment -- "small, proven logic,
// low risk to copy," matching message_codec.hpp's precedent) -- that duplication was latent, not
// actually safe: a real ODR violation the instant any single translation unit included both headers
// (which include/agentengine/rt/project_supervisor.hpp, needing both AgentSession and
// WorkflowSupervisor's checkpoint hooks, is the first thing to actually do). Consolidated here
// rather than re-copied a third time.

#include "agentengine/core/error.hpp"
#include "agentengine/core/interaction.hpp"
#include "agentengine/core/json_value.hpp"

namespace agentengine::rt {

[[nodiscard]] inline agentengine::json::Value interaction_to_json(agentengine::Interaction const& i) {
    char const* reason_str = "input";
    switch (i.reason) {
        case agentengine::interaction_reason::input:    reason_str = "input";    break;
        case agentengine::interaction_reason::auth:     reason_str = "auth";     break;
        case agentengine::interaction_reason::approval: reason_str = "approval"; break;
    }
    return agentengine::json::Value::make_object({
        {"interaction_id", agentengine::json::Value::make_string(i.interaction_id)},
        {"run_id", agentengine::json::Value::make_string(i.run_id)},
        {"reason", agentengine::json::Value::make_string(reason_str)},
        {"opened_at_ns", agentengine::json::Value::make_number(static_cast<double>(i.opened_at_ns))},
        {"expires_at_ns", agentengine::json::Value::make_number(static_cast<double>(i.expires_at_ns))},
    });
}

[[nodiscard]] inline agentengine::result<agentengine::Interaction> interaction_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* interaction_id = v.find("interaction_id");
    agentengine::json::Value const* run_id         = v.find("run_id");
    agentengine::json::Value const* reason         = v.find("reason");
    agentengine::json::Value const* opened_at_ns   = v.find("opened_at_ns");
    agentengine::json::Value const* expires_at_ns  = v.find("expires_at_ns");
    if (interaction_id == nullptr || !interaction_id->is_string() || run_id == nullptr ||
        !run_id->is_string() || reason == nullptr || !reason->is_string() || opened_at_ns == nullptr ||
        !opened_at_ns->is_number() || expires_at_ns == nullptr || !expires_at_ns->is_number()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed Interaction record",
                                                    "rt.interaction.record.malformed"});
    }
    agentengine::Interaction out{};
    out.interaction_id = interaction_id->as_string();
    out.run_id          = run_id->as_string();
    std::string const& r = reason->as_string();
    if (r == "input") out.reason = agentengine::interaction_reason::input;
    else if (r == "auth") out.reason = agentengine::interaction_reason::auth;
    else if (r == "approval") out.reason = agentengine::interaction_reason::approval;
    else {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "unknown interaction reason: " + r,
                                                    "rt.interaction.record.malformed"});
    }
    out.opened_at_ns  = static_cast<std::int64_t>(opened_at_ns->as_number());
    out.expires_at_ns = static_cast<std::int64_t>(expires_at_ns->as_number());
    return out;
}

}  // namespace agentengine::rt
