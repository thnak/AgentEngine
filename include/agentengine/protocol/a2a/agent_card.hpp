#pragma once
// Implements 012-A2A-Conformance.md §2.1 -- Agent Card generation. Milestone 7 Phase D2
// (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// §2.1: "Generated from agent metadata (002 §7) -- never hand-maintained, because a hand-maintained
// card drifts from behaviour... skills derived from the agent's declared tool set... Rule: the card
// advertises only what the conformance suite proves. An unimplemented capability is absent from the
// card, not present and broken."
//
// `to_agent_card()` reads what `AgentMetadata` (core/agent_registry.hpp) ACTUALLY carries today --
// `agent_name` and `tools` (a real `ToolTable`, one `AgentSkill` per registered tool, name/description
// straight from `ToolDescriptor`, the same "one schema source" discipline `protocol/mcp/server.hpp`'s
// own `to_mcp_tool_list_entry()` already uses for `tools/list`) -- and nothing it does not. `002 §7`
// itself says an agent's interop identity is `{agent_id, version}` and that Agent Cards are "generated
// from this same metadata," but `AgentMetadata` (002-owned, M2/M5 vintage) has no `description`,
// `version`, or modality-declaration fields yet -- a real, named gap, not silently invented here by
// repurposing `agent_instructions` as a card description or fabricating a version string. Those fields
// are therefore explicit, caller-supplied `AgentCardIdentity` inputs until 002 grows them for real.
//
// §2.1's own "advertises only what's proven" rule is why `supported_interfaces` defaults EMPTY and
// `capabilities.streaming`/`push_notifications` default `false`: no JSON-RPC/REST binding, no
// streaming, no push-notification delivery exists yet (that is D3+'s own job) -- a card built here
// before any of that lands never claims a capability this codebase cannot yet serve. `AgentSkill.tags`
// is likewise always empty: no tag vocabulary exists anywhere in `core/tool.hpp`/`ToolDescriptor`
// today, named here rather than invented from a tool's name/description text.

#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/protocol/a2a/types.hpp"

namespace agentengine::a2a {

namespace json = agentengine::json;

// §A.3: `AgentInterface = {url, protocolBinding ("JSONRPC" | "GRPC" | "HTTP+JSON" | a URI),
// protocolVersion, tenant?}`.
struct AgentInterface {
    std::string url;
    std::string protocol_binding;
    std::string protocol_version;
    std::optional<std::string> tenant;
};

// §A.3: `AgentSkill = {id, name, description, tags (all required), examples?, inputModes?,
// outputModes?, securityRequirements?}`.
struct AgentSkill {
    std::string              id;
    std::string              name;
    std::string              description;
    std::vector<std::string> tags;
    std::vector<std::string> examples;
    std::vector<std::string> input_modes;
    std::vector<std::string> output_modes;
};

// §A.3: `AgentCapabilities = {streaming?, pushNotifications?, extensions[], extendedAgentCard?}`.
struct AgentCapabilities {
    bool                      streaming = false;
    bool                      push_notifications = false;
    std::vector<std::string>  extensions;
    bool                      extended_agent_card = false;
};

struct AgentCard {
    std::string                    name;
    std::string                    description;
    std::vector<AgentInterface>    supported_interfaces;  // ordered, first = preferred (§A.3)
    std::string                    version;
    AgentCapabilities              capabilities;
    std::vector<std::string>       default_input_modes;
    std::vector<std::string>       default_output_modes;
    std::vector<AgentSkill>        skills;
    // Optional §A.3 fields NOT built: provider, documentationUrl, securitySchemes,
    // securityRequirements, signatures (§4a's JWS signing), iconUrl.
};

// What `AgentMetadata` (002, agent_registry.hpp) does not yet carry, per this file's own top comment.
// `default_input_modes`/`default_output_modes` are MIME-type-shaped strings (e.g. "text/plain") --
// no modality-declaration surface exists on `Tool`/`Agent` today, so these are caller-supplied rather
// than derived from anything real.
struct AgentCardIdentity {
    std::string               description;
    std::string               version;
    std::vector<std::string>  default_input_modes;
    std::vector<std::string>  default_output_modes;
    // Empty by default -- §2.1's own "advertises only what's proven" rule: a caller with a real
    // binding built (D3+) supplies its own real interface entries; nothing here fabricates one.
    std::vector<AgentInterface> supported_interfaces;
};

[[nodiscard]] inline AgentCard to_agent_card(AgentMetadata const& meta, AgentCardIdentity const& identity) {
    AgentCard card;
    card.name        = meta.agent_name;
    card.description = identity.description;
    card.version      = identity.version;
    card.supported_interfaces = identity.supported_interfaces;
    card.default_input_modes  = identity.default_input_modes;
    card.default_output_modes = identity.default_output_modes;
    // capabilities/skills.securityRequirements are left at their honest false/empty defaults -- see
    // file-top comment.
    for (ToolDescriptor const& d : meta.tools.descriptors()) {
        AgentSkill skill;
        skill.id          = d.name;
        skill.name        = d.name;
        skill.description = d.description;
        card.skills.push_back(std::move(skill));
    }
    return card;
}

// `detail::strings_to_json()` is `types.hpp`'s own -- reused as-is (sibling file, same
// `agentengine::a2a` namespace, same feature), not redefined here.

[[nodiscard]] inline json::Value to_json(AgentInterface const& i) {
    std::vector<std::pair<std::string, json::Value>> members{
        {"url", json::Value::make_string(i.url)},
        {"protocolBinding", json::Value::make_string(i.protocol_binding)},
        {"protocolVersion", json::Value::make_string(i.protocol_version)}};
    if (i.tenant) members.emplace_back("tenant", json::Value::make_string(*i.tenant));
    return json::Value::make_object(std::move(members));
}

[[nodiscard]] inline json::Value to_json(AgentSkill const& s) {
    std::vector<std::pair<std::string, json::Value>> members{
        {"id", json::Value::make_string(s.id)},
        {"name", json::Value::make_string(s.name)},
        {"description", json::Value::make_string(s.description)},
        {"tags", detail::strings_to_json(s.tags)}};
    if (!s.examples.empty()) members.emplace_back("examples", detail::strings_to_json(s.examples));
    if (!s.input_modes.empty()) members.emplace_back("inputModes", detail::strings_to_json(s.input_modes));
    if (!s.output_modes.empty())
        members.emplace_back("outputModes", detail::strings_to_json(s.output_modes));
    return json::Value::make_object(std::move(members));
}

[[nodiscard]] inline json::Value to_json(AgentCapabilities const& c) {
    std::vector<std::pair<std::string, json::Value>> members{
        {"streaming", json::Value::make_bool(c.streaming)},
        {"pushNotifications", json::Value::make_bool(c.push_notifications)},
        {"extensions", detail::strings_to_json(c.extensions)},
        {"extendedAgentCard", json::Value::make_bool(c.extended_agent_card)}};
    return json::Value::make_object(std::move(members));
}

[[nodiscard]] inline json::Value to_json(AgentCard const& card) {
    std::vector<json::Value> interfaces_json;
    interfaces_json.reserve(card.supported_interfaces.size());
    for (AgentInterface const& i : card.supported_interfaces) interfaces_json.push_back(to_json(i));

    std::vector<json::Value> skills_json;
    skills_json.reserve(card.skills.size());
    for (AgentSkill const& s : card.skills) skills_json.push_back(to_json(s));

    return json::Value::make_object(
        {{"name", json::Value::make_string(card.name)},
         {"description", json::Value::make_string(card.description)},
         {"supportedInterfaces", json::Value::make_array(std::move(interfaces_json))},
         {"version", json::Value::make_string(card.version)},
         {"capabilities", to_json(card.capabilities)},
         {"defaultInputModes", detail::strings_to_json(card.default_input_modes)},
         {"defaultOutputModes", detail::strings_to_json(card.default_output_modes)},
         {"skills", json::Value::make_array(std::move(skills_json))}});
}

}  // namespace agentengine::a2a
