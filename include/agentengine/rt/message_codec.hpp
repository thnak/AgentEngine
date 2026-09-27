#pragma once
// ADR-037 Phase 3, Slice 2: agentengine::Message/ContentItem <-> JSON, for
// agentengine::rt::WorkflowSupervisor's checkpoint record (workflow_run_state_record.hpp), the AgentSession
// snapshot (agent_session.hpp) and workflow_as_chat_client.hpp -- content.hpp's own "a workflow's pending/partial
// payloads ARE its state" (content_record.hpp's own banner) means Slice 2 cannot narrow these fields away the way
// rt::AgentSession's own Slice 2 narrowed history/state out of AgentSessionRecord.
//
// ADR-202 (#120 S3): this file used to hold a verbatim copy of core/chat_recording.hpp's codec, kept separate so
// that including it did not pull in chat_client.hpp. Both copies are now one codec, core/message_json.hpp, which
// depends only on the content model, JSON and error headers; the functions below keep their names and signatures
// as thin wrappers over its STATE profile:
//   - `Message::attribution` is written and read (ADR-066 §7: it did not previously survive the round trip). The
//     recording profile carries it too since ADR-204, so this is no longer a difference between the two.
//   - ContentItem's `approval` (ADR-191) and `deliver_as_instructions` (ADR-192) are neither written nor read. I3:
//     only AgentSession may set them, when it builds a request, "so no provider, plugin or stored history can
//     grant it" (content.hpp); a checkpoint or snapshot is stored history. message_json.hpp's `Profile` has the
//     full argument.
//   - Error codes are `rt.message_codec.*`.
//
// Gap-15 (2026-08-14) still applies to callers: these functions share their names and argument types with
// core/chat_recording.hpp's recording-profile wrappers in namespace `agentengine`, so an unqualified call from
// namespace `agentengine::rt` that also sees chat_recording.hpp is ambiguous through ADL. Qualify the call.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/message_json.hpp"

namespace agentengine::rt {

[[nodiscard]] inline std::string_view role_to_wire_string(agentengine::role r) noexcept {
    return agentengine::message_json::role_to_wire_string(r);
}
[[nodiscard]] inline agentengine::result<agentengine::role> role_from_wire_string(std::string_view s) {
    return agentengine::message_json::role_from_wire_string(s, agentengine::message_json::Profile::state());
}

[[nodiscard]] inline std::string_view origin_to_wire_string(agentengine::content_origin o) noexcept {
    return agentengine::message_json::origin_to_wire_string(o);
}
[[nodiscard]] inline agentengine::result<agentengine::content_origin> origin_from_wire_string(std::string_view s) {
    return agentengine::message_json::origin_from_wire_string(s, agentengine::message_json::Profile::state());
}

[[nodiscard]] inline agentengine::json::Value content_item_to_json(agentengine::ContentItem const& item) {
    return agentengine::message_json::content_item_to_json(item, agentengine::message_json::Profile::state());
}
[[nodiscard]] inline agentengine::result<agentengine::ContentItem> content_item_from_json(
    agentengine::json::Value const& j) {
    return agentengine::message_json::content_item_from_json(j, agentengine::message_json::Profile::state());
}

[[nodiscard]] inline agentengine::json::Value message_to_json(agentengine::Message const& m) {
    return agentengine::message_json::message_to_json(m, agentengine::message_json::Profile::state());
}
[[nodiscard]] inline agentengine::result<agentengine::Message> message_from_json(agentengine::json::Value const& j) {
    return agentengine::message_json::message_from_json(j, agentengine::message_json::Profile::state());
}

}  // namespace agentengine::rt
