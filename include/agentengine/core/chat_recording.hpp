#pragma once
// ADR-207 (#120 S7): the function bodies are in src/core/chat_recording.cpp, compiled once. Every declaration and
// comment stays here.
// Also: ADR-191/192 -- records `ContentItem::approval` and `deliver_as_instructions`.
// Also: ADR-202 (#120 S3) -- the Message/ContentItem codec itself is core/message_json.hpp's recording profile; the
// functions below keep their names and signatures as thin wrappers over it.
// Implements 004-Model-Provider-Plane.md §6 ("Recording and replay") — Milestone 5 Phase G1's shared
// codec: the JSON envelope both `RecordingChatClient<Inner>` (core/recording_chat_client.hpp, the
// recorder) and `ReplayChatClient` (core/replay_chat_client.hpp, the player) read and write. Promotes
// `tests/support/recorded_chat_client.hpp`'s hand-authored, test-scoped, 3-content-kind fixture
// format to the real thing (decision 8): every `ContentItem` variant alternative round-trips, both
// unary responses and full ordered streaming chunk sequences (with per-chunk timing) are covered, and
// this is real product code under core/, not test-only scaffolding.
//
// Dependency-tier discipline (CONVENTIONS.md: "Core... std + Quark only, no third-party dependency,
// ever"): built on `agentengine::json::Value` (core/json_value.hpp), the project's own
// dependency-free JSON codec — not nlohmann (that stays test-only, tests/CMakeLists.txt). This is why
// G1 could not simply reuse `RecordedChatClient`'s existing nlohmann-based parser verbatim.
//
// `ChatRequest` recording is intentionally narrower than a full wire capture: `messages` and
// `output_schema_json`/`idempotency_key` round-trip exactly, but `tools` records only
// `{name, description, args_schema_json, reply_schema_json}` — `ToolDescriptor::invoke` is a
// `std::function` closure (capability ceiling, approval mode, and the callable itself), which is
// runtime state, not data, and cannot round-trip through JSON. Named here rather than silently
// dropped; a replayed request's tool table is for human/debug legibility, not for re-invoking tools.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/message_json.hpp"
#include "agentengine/core/tool_descriptor.hpp"

namespace agentengine {

namespace recording_detail {

// The field readers are the Message JSON codec's own (core/message_json.hpp, ADR-202); the envelope code below
// uses them as before. `require` stays here: its `recording.missing_field` code is this envelope's.
using message_json::detail::opt_bool;
using message_json::detail::opt_string;
using message_json::detail::opt_u64;

[[nodiscard]] result<json::Value const*> require(json::Value const& obj, std::string_view key);

} // namespace recording_detail

// --- role / content_origin / failure_class <-> wire string -----------------------------------------
// role and content_origin: the recording profile of the one Message JSON codec (core/message_json.hpp, ADR-202).

[[nodiscard]] inline std::string_view role_to_wire_string(role r) noexcept {
    return message_json::role_to_wire_string(r);
}

[[nodiscard]] inline result<role> role_from_wire_string(std::string_view s) {
    return message_json::role_from_wire_string(s, message_json::Profile::recording());
}

[[nodiscard]] inline std::string_view origin_to_wire_string(content_origin o) noexcept {
    return message_json::origin_to_wire_string(o);
}

[[nodiscard]] inline result<content_origin> origin_from_wire_string(std::string_view s) {
    return message_json::origin_from_wire_string(s, message_json::Profile::recording());
}

[[nodiscard]] std::string_view failure_class_to_wire_string(failure_class k) noexcept;

[[nodiscard]] result<failure_class> failure_class_from_wire_string(std::string_view s);

// --- error <-> json ----------------------------------------------------------------------------------

[[nodiscard]] json::Value error_to_json(error const& e);

[[nodiscard]] result<error> error_from_json(json::Value const& j);

// --- ContentItem / Message <-> json ------------------------------------------------------------------
// The recording profile of the one Message JSON codec (core/message_json.hpp, ADR-202): every ContentItem variant
// alternative, plus ADR-191's `approval` and ADR-192's `deliver_as_instructions` (recorded so a replayed request
// renders the same fences, I5, and the audit shows which approval reached the model, I4; each omitted when unset, so
// older recordings are unchanged). `Message::attribution` is written when present and read when an object (ADR-204,
// closing ADR-202 §9's I4 gap: a recorded request says which context provider contributed which message). An older
// recording without it reads back with none. The eval screen hashes prompts as `message_to_json` renders them
// (ADR-195 §8), so an attributed prompt's design hash changed at ADR-204; an unattributed one's did not.

[[nodiscard]] inline json::Value content_item_to_json(ContentItem const& item) {
    return message_json::content_item_to_json(item, message_json::Profile::recording());
}

[[nodiscard]] inline result<ContentItem> content_item_from_json(json::Value const& j) {
    return message_json::content_item_from_json(j, message_json::Profile::recording());
}

[[nodiscard]] inline json::Value message_to_json(Message const& m) {
    return message_json::message_to_json(m, message_json::Profile::recording());
}

[[nodiscard]] inline result<Message> message_from_json(json::Value const& j) {
    return message_json::message_from_json(j, message_json::Profile::recording());
}

// --- Usage <-> json ------------------------------------------------------------------------------------

[[nodiscard]] json::Value usage_to_json(Usage const& u);

[[nodiscard]] Usage usage_from_json(json::Value const& j);

// --- ChatResponse / ChatResponseUpdate <-> json -------------------------------------------------------

[[nodiscard]] json::Value chat_response_to_json(ChatResponse const& r);

[[nodiscard]] result<ChatResponse> chat_response_from_json(json::Value const& j);

[[nodiscard]] json::Value chat_response_update_to_json(ChatResponseUpdate const& u);

[[nodiscard]] result<ChatResponseUpdate> chat_response_update_from_json(json::Value const& j);

// --- ChatRequest <-> json (scoped -- see file banner) -------------------------------------------------

[[nodiscard]] json::Value chat_request_to_json(ChatRequest const& r);

// Deliberately no `chat_request_from_json`: nothing in this milestone re-issues a recorded request as
// a live `ChatRequest` (recorded `tools` lost `invoke`/`capability_ceiling` on the way to JSON, see
// file banner -- reconstructing a `ChatRequest` from a recording would silently fabricate an empty
// `invoke` for every tool, which is worse than not offering the function at all). The recording keeps
// the request for human/debug legibility; `ReplayChatClient` (core/replay_chat_client.hpp) replays the
// RESPONSE side only and, like `RecordedChatClient` before it, ignores the live caller's own request.

// --- ChatCallRecording ---------------------------------------------------------------------------------

enum class recording_mode { unary, streaming }; // ae-naming-lint: allow recording_mode — 004 §6 vocabulary, no RFC term yet assigned

struct RecordedChunk { // ae-naming-lint: allow RecordedChunk — 004 §6 vocabulary, no RFC term yet assigned
    ChatResponseUpdate update;
    std::chrono::milliseconds elapsed_since_start{0};
};

// The full recording envelope for one `ChatClient::chat()` or `chat_stream()` call (004 §6: "request,
// response or full ordered chunk sequence, timing, and usage"). `response`/`chat_error` are mutually
// exclusive and only meaningful when `mode == unary`; `chunks`/`stream_terminal`/`stream_error` are
// only meaningful when `mode == streaming` -- matching `ChatClient`'s own two-call-shape split
// (chat_client.hpp), not a redesign of it.
struct ChatCallRecording { // ae-naming-lint: allow ChatCallRecording — 004 §6 vocabulary, no RFC term yet assigned
    ChatRequest request;
    recording_mode mode = recording_mode::unary;

    std::optional<ChatResponse> response;
    std::optional<error> chat_error;

    std::vector<RecordedChunk> chunks;
    std::string stream_terminal; // "closed" | "cancelled" | "deadline_exceeded" | "failed" | "" (unset)
    std::string stream_error_detail; // meaningful only when stream_terminal == "failed"
    // ADR-177: the stream's whole `error` (class + code + message), not just its message. A retry
    // decision reads the class and code, so a recording that kept only the text could not reproduce
    // the decision on replay (I5) -- a replayed failure would come back as `fatal` and never retry.
    // Absent in recordings written before this field existed; the replay player then falls back to
    // its old, class-less reconstruction rather than guessing a class.
    std::optional<error> stream_error;

    std::chrono::milliseconds duration{0};
};

[[nodiscard]] json::Value chat_call_recording_to_json(ChatCallRecording const& rec);

[[nodiscard]] result<ChatCallRecording> chat_call_recording_from_json(json::Value const& j);

// --- File I/O --------------------------------------------------------------------------------------

[[nodiscard]] result<void> write_chat_call_recording(std::filesystem::path const& path,
                                                     ChatCallRecording const& rec);

[[nodiscard]] result<ChatCallRecording> read_chat_call_recording(std::filesystem::path const& path);

} // namespace agentengine
