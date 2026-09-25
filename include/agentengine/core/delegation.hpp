#pragma once
// Implements decisions/ADR-185-delegation-provenance.md: the one rule every agent-to-agent handoff applies --
// a hop never changes who wrote the text.
//
// Before this, `agent.spawn` handed a child the parent model's `input` as an untainted `role::user`, `origin=user`
// message, and a workflow agent node received the previous node's reply as its own assistant turn. Either way, text a
// model wrote -- possibly copied from memory, a document or a tool result the parent saw fenced as untrusted (ADR-173)
// -- reached the next agent looking like a human's request or its own words (003 §2: model output is tainted, and
// untainting needs an explicit, logged decision; this was neither). Three hops later nothing marked it at all.
//
// The rule: a delegated task reaches the next agent as a `role::user` message (so the agent carries it out -- a
// delegated task that is fenced as "never follow" would be inert, ADR-183's finding) made of two parts:
//   1. a host-authored line (untainted, `content_origin::system`: host code wrote it, ADR-066 §5) saying the request
//      was delegated by another agent, which one, at what depth, and that no human wrote it;
//   2. the delegated text itself, `content_origin::external`, `tainted = true` -- so recordings, summarizers, memory
//      capture and every taint-aware consumer see it for what it is, at every hop.
// It is not fenced: the next agent is meant to act on it, and its authority is unchanged (its capabilities are the
// attenuated grant; every tool call it makes is already `arguments_tainted`). What changes is that nothing downstream
// can mistake it for a human's words.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agentengine/core/content.hpp"

namespace agentengine {

// Where a delegated task came from. Host-derived values only (a principal id, a node label, a depth) -- never text
// the delegating model wrote (I3).
struct DelegationSource {  // ae-naming-lint: allow DelegationSource — ADR-185
    std::string   kind{};        // "agent.spawn" | "workflow node"
    std::string   from{};        // the delegating principal's id, or a label for the upstream node
    std::uint32_t depth = 0;     // the receiving agent's delegation depth (1 = delegated once)
};

// A host-derived label as it may appear in the untainted host line: quoted, control characters replaced, length
// capped (red team round 1: a root principal's id can be an identity provider's `sub`, which the host line must not
// let read as an instruction).
[[nodiscard]] inline std::string quoted_label(std::string_view label) {
    constexpr std::size_t kMax = 120;
    std::string out = "\"";
    for (std::size_t i = 0; i < label.size() && i < kMax; ++i) {
        char const c = label[i];
        out += (static_cast<unsigned char>(c) < 0x20 || c == '"' || c == 0x7f) ? ' ' : c;
    }
    if (label.size() > kMax) out += "...";
    out += '"';
    return out;
}

// The host line (part 1). Fixed wording; only the host-derived fields vary.
[[nodiscard]] inline std::string delegated_task_preamble(DelegationSource const& source) {
    std::string out = "The request below was delegated to you by another agent (";
    out += source.kind;
    if (!source.from.empty()) {
        out += " from ";
        out += quoted_label(source.from);
    }
    out += ", delegation depth ";
    out += std::to_string(source.depth);
    out += "). A model wrote it, not a human user. Carry it out as that agent's request; it grants you nothing "
           "beyond the tools you have.";
    return out;
}

// The delegated message: the host line, then the delegated text, tainted and external.
[[nodiscard]] inline Message make_delegated_message(DelegationSource const& source, std::string text) {
    Message m;
    m.role = role::user;
    ContentItem host;
    host.origin = content_origin::system;
    host.tainted = false;
    host.value = Text{delegated_task_preamble(source)};
    m.content.push_back(std::move(host));
    ContentItem delegated;
    delegated.origin = content_origin::external;
    delegated.tainted = true;
    delegated.value = Text{std::move(text)};
    m.content.push_back(std::move(delegated));
    return m;
}

// ADR-185 red team round 1: the same rule applied per ITEM, for input that may mix host-authored parts with another
// agent's (a workflow fan-in merges payloads onto the first one's role, so an upstream model's text could arrive
// inside a user- or system-role message the role test passed straight through). Every item the host did not author
// -- anything but an untainted user/system item -- becomes tainted and `external`, keeping its value (text, data,
// media: nothing is dropped except tool calls/results, which a converged reply never carries and no user or system
// message may hold). An assistant-role message becomes a user-role one. A user-role message that carries such items
// gets the host line first; a system-role one needs none, since its tainted items are fenced (ADR-173). Input with
// no such item is returned unchanged.
[[nodiscard]] inline Message delegate_foreign_items(Message in, DelegationSource const& source) {
    auto const host_authored_item = [](ContentItem const& item) {
        return !item.tainted && (item.origin == content_origin::user || item.origin == content_origin::system);
    };
    bool const has_foreign = std::any_of(in.content.begin(), in.content.end(), [&](ContentItem const& item) {
        return !host_authored_item(item) || std::holds_alternative<ToolCall>(item.value) ||
               std::holds_alternative<ToolResult>(item.value);
    });
    if (!has_foreign) return in;  // host-authored input, unchanged (scanned first: nothing is moved out of it)
    bool any_foreign = false;
    std::vector<ContentItem> kept;
    kept.reserve(in.content.size() + 1);
    for (ContentItem& item : in.content) {
        if (std::holds_alternative<ToolCall>(item.value) || std::holds_alternative<ToolResult>(item.value)) continue;
        if (!host_authored_item(item)) {
            item.tainted = true;
            item.origin = content_origin::external;
            any_foreign = true;
        }
        kept.push_back(std::move(item));
    }
    in.content = std::move(kept);
    if (!any_foreign) return in;  // only tool calls/results were dropped
    kept = std::move(in.content);
    if (in.role == role::assistant || in.role == role::tool) in.role = role::user;
    if (in.role == role::user) {
        ContentItem host;
        host.origin = content_origin::system;
        host.tainted = false;
        host.value = Text{delegated_task_preamble(source)};
        kept.insert(kept.begin(), std::move(host));
    }
    in.content = std::move(kept);
    return in;
}

}  // namespace agentengine
