#pragma once
// Implements 012-A2A-Conformance.md §1's "Message → Part[] ... is the content model of 003. The
// mapping layer is thin by construction" -- the `agentengine::Message`/`ContentItem` (003, internal)
// <-> `agentengine::a2a::Message`/`Part` (012, wire) projection. Milestone 7 Phase D3
// (docs/planning/milestone-7-protocol-conformance-breakdown.md).
//
// Deliberately narrow on the `ContentItem` side: `Text`/`Data`/`Error` are the three kinds this
// codebase's own pipelines actually produce today (`protocol/mcp/server.hpp`'s own file-top comment
// names the identical set for the same reason -- `tool_pipeline.hpp` never constructs a `Reasoning`/
// `Media`/`ToolCall`/`ToolResult`/`Citation`/`Custom` item, and the M1-era `AgentSession` turn loop's
// own `ChatResponse` -> `Message` path is likewise text-shaped in every conformer this codebase has).
// Every OTHER kind falls back to `UnknownPart` -- REUSING the exact forward-compat mechanism
// `protocol/a2a/types.hpp` already built for a wire peer's own unrecognized `Part` kind, rather than
// inventing a second "we don't have a real mapping for this" shape. This is honestly narrower than
// 012 §6's own G2 gate ("round-trip fidelity... preserves every part") -- that gate is milestone-close
// work over a real corpus, not claimed here.
//
// Role mapping is lossy in one well-named direction: `agentengine::role` has four values (system/
// user/assistant/tool), A2A's `a2a_role` has three (unspecified/user/agent) with no system/tool
// equivalent -- `system`/`tool` both map to `a2a_role::unspecified` (never silently coerced to `user`
// or `agent`, which would misattribute who said what), and the reverse direction only ever produces
// `role::user`/`role::assistant` (an inbound A2A message is always somebody ELSE speaking to us, never
// system/tool -- 012's own client/server roles never receive a system or tool-authored `Message` over
// the wire).

#include <string>
#include <utility>

#include "agentengine/core/content.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/protocol/a2a/types.hpp"

namespace agentengine::a2a {

[[nodiscard]] inline a2a_role to_a2a_role(agentengine::role r) noexcept {
    switch (r) {
        case agentengine::role::user:      return a2a_role::user;
        case agentengine::role::assistant: return a2a_role::agent;
        case agentengine::role::system:
        case agentengine::role::tool:
        default:
            return a2a_role::unspecified;
    }
}

// See file-top comment: an inbound A2A `Message` is always a peer speaking to us (user) or an agent
// reply we are re-wrapping (agent) -- `unspecified` defaults to `user`, the conservative "external
// content, treat it as caller input" reading (003 §2), never silently promoted to `assistant`.
[[nodiscard]] inline agentengine::role from_a2a_role(a2a_role r) noexcept {
    return r == a2a_role::agent ? agentengine::role::assistant : agentengine::role::user;
}

[[nodiscard]] inline Part content_item_to_part(ContentItem const& item) {
    Part p;
    if (auto const* t = std::get_if<Text>(&item.value)) {
        p.value = TextPart{t->text};
    } else if (auto const* d = std::get_if<Data>(&item.value)) {
        auto parsed = json::parse(d->json);
        // A `Data` payload this codebase itself produced is always well-formed JSON (it came from
        // `json::dump()` in the first place, tool_pipeline.hpp step 9) -- but a wire round trip is
        // not the place to newly introduce a throwing assumption, so a parse failure still degrades
        // to a legible `TextPart` of the raw string rather than dropping the content.
        if (parsed.has_value()) {
            p.value = DataPart{std::move(*parsed)};
        } else {
            p.value = TextPart{d->json};
        }
    } else if (auto const* e = std::get_if<Error>(&item.value)) {
        p.value = TextPart{e->message};
    } else {
        // Reasoning/Media/ToolCall/ToolResult/Citation/Custom: no real producer in this codebase's
        // pipelines today (file-top comment) -- preserved via the SAME forward-compat shape
        // `types.hpp` already built for an unrecognized WIRE `Part` kind, not a second mechanism.
        p.value = UnknownPart{"agentengineContent", json::Value::make_string("unmapped content kind")};
    }
    return p;
}

[[nodiscard]] inline ContentItem part_to_content_item(Part const& p) {
    ContentItem item;
    item.origin  = content_origin::external;  // 003 §2: content arriving over a protocol boundary
    item.tainted = true;                      // -- a peer agent is exactly as trusted as a web page (012 §3)
    if (auto const* t = std::get_if<TextPart>(&p.value)) {
        item.value = Text{t->text};
    } else if (auto const* d = std::get_if<DataPart>(&p.value)) {
        item.value = Data{json::dump(d->data), std::nullopt};
    } else if (auto const* u = std::get_if<UrlPart>(&p.value)) {
        item.value = Media{u->url, p.media_type.value_or(std::string{})};
    } else if (auto const* r = std::get_if<RawPart>(&p.value)) {
        item.value = Media{r->bytes, p.media_type.value_or(std::string{})};
    } else {
        auto const& unk = std::get<UnknownPart>(p.value);
        item.value = Custom{"a2a:" + unk.member_name, json::dump(unk.raw_value)};
    }
    return item;
}

[[nodiscard]] inline Message to_a2a_message(agentengine::Message const& m, std::optional<std::string> task_id,
                                             std::optional<std::string> context_id) {
    Message wire;
    wire.message_id = m.message_id;
    wire.task_id     = std::move(task_id);
    wire.context_id  = std::move(context_id);
    wire.role        = to_a2a_role(m.role);
    wire.parts.reserve(m.content.size());
    for (ContentItem const& item : m.content) wire.parts.push_back(content_item_to_part(item));
    return wire;
}

[[nodiscard]] inline agentengine::Message from_a2a_message(Message const& wire) {
    agentengine::Message m;
    m.message_id = wire.message_id;
    m.role        = from_a2a_role(wire.role);
    m.content.reserve(wire.parts.size());
    for (Part const& p : wire.parts) m.content.push_back(part_to_content_item(p));
    return m;
}

}  // namespace agentengine::a2a
