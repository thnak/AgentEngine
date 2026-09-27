#pragma once
// #120 S3 (decisions/ADR-202-one-message-json-codec.md): the ONE Message/ContentItem <-> JSON codec
// (003-Message-and-Content-Model.md's content model). It replaces two drifted copies that each carried a
// full implementation: core/chat_recording.hpp's (chat-call recordings, 004 §6, and the eval screen's prompt
// hash, ADR-195 §8) and rt/message_codec.hpp's (workflow checkpoints and the AgentSession snapshot, ADR-037,
// ADR-066). Both headers keep their public functions as thin wrappers over this one, each fixed to one
// profile, so no caller changed. Bodies live in src/core/message_json.cpp (library agentengine_message_json).
//
// The two copies agreed on everything except what `Profile` below selects; ADR-202 §4 lists every difference
// found and how each is kept. Dependencies: content.hpp, json_value.hpp, error.hpp and std only (layer V).

#include <cstdint>
#include <string>
#include <string_view>

#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"

namespace agentengine::message_json {

// Which of the codec's two callers a call serves. Exactly two values exist, built only by the two named
// factories below; the constructor is private, so no seam can build a third combination (for example
// "state, but with delivery marks").
//
// - `recording()` -- chat-call recordings (core/chat_recording.hpp: RecordingChatClient, ReplayChatClient)
//   and the eval screen's prompt hash (eval/eval_tier1_screen.hpp). Writes and reads ContentItem's ADR-191
//   `approval` and ADR-192 `deliver_as_instructions` delivery marks: a replayed request must render the same
//   fences (I5) and the audit must show which approval reached the model (I4). Error codes `recording.*`.
//
// - `state()` -- durable engine state that is read back into a live session or workflow: workflow
//   checkpoints (rt/workflow_run_state_record.hpp), the AgentSession snapshot (rt/agent_session.hpp),
//   workflow_as_chat_client.hpp. Error codes `rt.message_codec.*`.
//   I3: the delivery marks are NEITHER written NOR read. content.hpp: they are set ONLY by `AgentSession` when
//   it builds a request, after re-verifying the text against its host-owned approved-lesson registry, "so no
//   provider, plugin or stored history can grant it". Stored state is exactly that: a checkpoint or snapshot
//   file can be written by anyone who can write its store, and its history carries model output. If this
//   profile read `approval` back, a crafted or model-influenced store entry would restore a mark that tells
//   the serializers to name text an approved lesson, or (with `deliver_as_instructions`) to send it unfenced
//   -- stored data deciding how the model is told to trust text, which I3 forbids. Not writing them keeps the
//   state format free of a field no reader may honour. tests/core/chat/test_message_json_equivalence.cpp
//   holds the positive control.
//
// Both profiles write (when present) and read `Message::attribution` (ADR-066), identically: state since ADR-066
// §7, recordings since ADR-204 (the I4 gap ADR-202 §9 left open). It is not a profile difference, so there is no
// query for it -- a query that answers the same for both values would only invite flipping it back for one. It
// is provenance data, not a mark: no consumer decides authority from it (ADR-204 §5), so reading it back from a
// stored file needs no I3 guard.
class Profile {  // ae-naming-lint: allow Profile — #120 S3 (ADR-202) codec profile, internal to the Message JSON codec
public:
    [[nodiscard]] static constexpr Profile recording() noexcept { return Profile{id::recording}; }
    [[nodiscard]] static constexpr Profile state() noexcept { return Profile{id::state}; }

    // ADR-191 `approval` and ADR-192 `deliver_as_instructions`: written (when set) and read.
    [[nodiscard]] constexpr bool carries_delivery_marks() const noexcept { return id_ == id::recording; }
    // A present but non-string "kind" is a `missing_field` error. The recording copy never checked the type and
    // reads it with `as_string()`, which throws std::bad_variant_access (ADR-202 §4, D3) -- kept, not fixed here.
    [[nodiscard]] constexpr bool rejects_non_string_kind() const noexcept { return id_ == id::state; }
    // Prefix of every error code this codec returns: "recording." or "rt.message_codec.".
    [[nodiscard]] constexpr std::string_view error_code_prefix() const noexcept {
        return id_ == id::recording ? std::string_view{"recording."} : std::string_view{"rt.message_codec."};
    }

    friend constexpr bool operator==(Profile, Profile) noexcept = default;

private:
    enum class id : std::uint8_t { recording, state };
    constexpr explicit Profile(id i) noexcept : id_(i) {}
    id id_;
};

// Shared field readers (were duplicated in both copies; chat_recording.hpp's envelope code uses them too).
namespace detail {

[[nodiscard]] inline std::string opt_string(json::Value const& obj, std::string_view key, std::string fallback = {}) {
    json::Value const* v = obj.find(key);
    if (v == nullptr || !v->is_string()) return fallback;
    return v->as_string();
}

[[nodiscard]] inline bool opt_bool(json::Value const& obj, std::string_view key, bool fallback = false) {
    json::Value const* v = obj.find(key);
    if (v == nullptr || !v->is_bool()) return fallback;
    return v->as_bool();
}

[[nodiscard]] inline std::uint64_t opt_u64(json::Value const& obj, std::string_view key, std::uint64_t fallback = 0) {
    json::Value const* v = obj.find(key);
    if (v == nullptr || !v->is_number()) return fallback;
    return static_cast<std::uint64_t>(v->as_number());
}

}  // namespace detail

// role / content_origin <-> wire string. Unknown strings are a `contract` error with code
// `<prefix>bad_role` / `<prefix>bad_origin`.
[[nodiscard]] std::string_view role_to_wire_string(role r) noexcept;
[[nodiscard]] result<role> role_from_wire_string(std::string_view s, Profile profile);
[[nodiscard]] std::string_view origin_to_wire_string(content_origin o) noexcept;
[[nodiscard]] result<content_origin> origin_from_wire_string(std::string_view s, Profile profile);

// ContentItem <-> JSON, all 9 variant alternatives; ToolResult children use the same profile.
[[nodiscard]] json::Value content_item_to_json(ContentItem const& item, Profile profile);
[[nodiscard]] result<ContentItem> content_item_from_json(json::Value const& j, Profile profile);

// Message <-> JSON.
[[nodiscard]] json::Value message_to_json(Message const& m, Profile profile);
[[nodiscard]] result<Message> message_from_json(json::Value const& j, Profile profile);

}  // namespace agentengine::message_json
