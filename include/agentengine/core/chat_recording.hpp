#pragma once
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

[[nodiscard]] inline result<json::Value const*> require(json::Value const& obj, std::string_view key) {
    json::Value const* v = obj.find(key);
    if (v == nullptr) {
        return std::unexpected(
            error{failure_class::contract, "missing field: " + std::string(key), "recording.missing_field"});
    }
    return v;
}

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

[[nodiscard]] inline std::string_view failure_class_to_wire_string(failure_class k) noexcept {
    switch (k) {
        case failure_class::transient: return "transient";
        case failure_class::policy: return "policy";
        case failure_class::contract: return "contract";
        case failure_class::resource: return "resource";
        case failure_class::fatal: return "fatal";
    }
    return "fatal";
}

[[nodiscard]] inline result<failure_class> failure_class_from_wire_string(std::string_view s) {
    if (s == "transient") return failure_class::transient;
    if (s == "policy") return failure_class::policy;
    if (s == "contract") return failure_class::contract;
    if (s == "resource") return failure_class::resource;
    if (s == "fatal") return failure_class::fatal;
    return std::unexpected(error{failure_class::contract, "unknown failure_class: " + std::string(s),
                                  "recording.bad_failure_class"});
}

// --- error <-> json ----------------------------------------------------------------------------------

[[nodiscard]] inline json::Value error_to_json(error const& e) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("klass", json::Value::make_string(std::string(failure_class_to_wire_string(e.klass))));
    obj.emplace_back("message", json::Value::make_string(e.message));
    obj.emplace_back("code", json::Value::make_string(e.code));
    obj.emplace_back("native_code", json::Value::make_number(static_cast<double>(e.native_code)));
    return json::Value::make_object(std::move(obj));
}

[[nodiscard]] inline result<error> error_from_json(json::Value const& j) {
    auto klass = failure_class_from_wire_string(recording_detail::opt_string(j, "klass", "fatal"));
    if (!klass) return std::unexpected(klass.error());
    error e;
    e.klass = *klass;
    e.message = recording_detail::opt_string(j, "message");
    e.code = recording_detail::opt_string(j, "code");
    e.native_code = static_cast<int>(recording_detail::opt_u64(j, "native_code"));
    return e;
}

// --- ContentItem / Message <-> json ------------------------------------------------------------------
// The recording profile of the one Message JSON codec (core/message_json.hpp, ADR-202): every ContentItem variant
// alternative, plus ADR-191's `approval` and ADR-192's `deliver_as_instructions` (recorded so a replayed request
// renders the same fences, I5, and the audit shows which approval reached the model, I4; each omitted when unset, so
// older recordings are unchanged). `Message::attribution` is neither written nor read -- the eval screen hashes
// prompts as `message_to_json` renders them (ADR-195 §8); whether recordings should carry it is ADR-202 §9's open
// decision.

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

[[nodiscard]] inline json::Value usage_to_json(Usage const& u) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("input_tokens", json::Value::make_number(static_cast<double>(u.input_tokens)));
    obj.emplace_back("output_tokens", json::Value::make_number(static_cast<double>(u.output_tokens)));
    obj.emplace_back("cached_input_tokens",
                      json::Value::make_number(static_cast<double>(u.cached_input_tokens)));
    obj.emplace_back("reasoning_tokens", json::Value::make_number(static_cast<double>(u.reasoning_tokens)));
    obj.emplace_back("cost_estimate", json::Value::make_number(u.cost_estimate));
    obj.emplace_back("cache_write_tokens", json::Value::make_number(static_cast<double>(u.cache_write_tokens)));
    return json::Value::make_object(std::move(obj));
}

[[nodiscard]] inline Usage usage_from_json(json::Value const& j) {
    Usage u;
    u.input_tokens = recording_detail::opt_u64(j, "input_tokens");
    u.output_tokens = recording_detail::opt_u64(j, "output_tokens");
    u.cached_input_tokens = recording_detail::opt_u64(j, "cached_input_tokens");
    u.reasoning_tokens = recording_detail::opt_u64(j, "reasoning_tokens");
    if (auto const* cost = j.find("cost_estimate"); cost != nullptr && cost->is_number()) {
        u.cost_estimate = cost->as_number();
    }
    u.cache_write_tokens = recording_detail::opt_u64(j, "cache_write_tokens");
    return u;
}

// --- ChatResponse / ChatResponseUpdate <-> json -------------------------------------------------------

[[nodiscard]] inline json::Value chat_response_to_json(ChatResponse const& r) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("message", message_to_json(r.message));
    obj.emplace_back("usage", usage_to_json(r.usage));
    obj.emplace_back("model", json::Value::make_string(r.model));
    obj.emplace_back("fallback_tier", json::Value::make_number(static_cast<double>(r.fallback_tier)));
    // ADR-148: same field-parity discipline as fallback_tier immediately above -- a codec that
    // silently drops a real ChatResponse field is exactly the "don't forget to update it" trap this
    // manual field list already has to guard against by hand.
    obj.emplace_back("route_index", json::Value::make_number(static_cast<double>(r.route_index)));
    return json::Value::make_object(std::move(obj));
}

[[nodiscard]] inline result<ChatResponse> chat_response_from_json(json::Value const& j) {
    auto message_v = recording_detail::require(j, "message");
    if (!message_v) return std::unexpected(message_v.error());
    auto message = message_from_json(**message_v);
    if (!message) return std::unexpected(message.error());
    ChatResponse r;
    r.message = std::move(*message);
    if (auto const* usage = j.find("usage"); usage != nullptr) r.usage = usage_from_json(*usage);
    r.model = recording_detail::opt_string(j, "model");
    r.fallback_tier = static_cast<std::uint32_t>(recording_detail::opt_u64(j, "fallback_tier"));
    r.route_index = static_cast<std::uint32_t>(recording_detail::opt_u64(j, "route_index"));
    return r;
}

[[nodiscard]] inline json::Value chat_response_update_to_json(ChatResponseUpdate const& u) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("delta", content_item_to_json(u.delta));
    obj.emplace_back("is_final", json::Value::make_bool(u.is_final));
    // Written only when set, so every recording made before this field existed is byte-identical to
    // what this function writes for the same updates today. Round-tripped at all because a replayed
    // stream must reconstruct the SAME message the live one did (I5): drop it, and a replay rebuilds
    // one item per token where the recorded run had one item per block.
    if (u.continues_previous) obj.emplace_back("continues_previous", json::Value::make_bool(true));
    return json::Value::make_object(std::move(obj));
}

[[nodiscard]] inline result<ChatResponseUpdate> chat_response_update_from_json(json::Value const& j) {
    auto delta_v = recording_detail::require(j, "delta");
    if (!delta_v) return std::unexpected(delta_v.error());
    auto delta = content_item_from_json(**delta_v);
    if (!delta) return std::unexpected(delta.error());
    ChatResponseUpdate u;
    u.delta = std::move(*delta);
    u.is_final = recording_detail::opt_bool(j, "is_final");
    u.continues_previous = recording_detail::opt_bool(j, "continues_previous");
    return u;
}

// --- ChatRequest <-> json (scoped -- see file banner) -------------------------------------------------

[[nodiscard]] inline json::Value chat_request_to_json(ChatRequest const& r) {
    std::vector<std::pair<std::string, json::Value>> obj;
    std::vector<json::Value> messages;
    messages.reserve(r.messages.size());
    for (auto const& m : r.messages) messages.push_back(message_to_json(m));
    obj.emplace_back("messages", json::Value::make_array(std::move(messages)));

    std::vector<json::Value> tools;
    tools.reserve(r.tools.size());
    for (auto const& t : r.tools) {
        std::vector<std::pair<std::string, json::Value>> tool_obj;
        tool_obj.emplace_back("name", json::Value::make_string(t.name));
        tool_obj.emplace_back("description", json::Value::make_string(t.description));
        tool_obj.emplace_back("args_schema_json", json::Value::make_string(t.args_schema_json));
        tool_obj.emplace_back("reply_schema_json", json::Value::make_string(t.reply_schema_json));
        tools.push_back(json::Value::make_object(std::move(tool_obj)));
    }
    obj.emplace_back("tools", json::Value::make_array(std::move(tools)));

    if (r.output_schema_json) {
        obj.emplace_back("output_schema_json", json::Value::make_string(*r.output_schema_json));
    }
    if (r.idempotency_key) {
        obj.emplace_back("idempotency_key", json::Value::make_string(*r.idempotency_key));
    }
    return json::Value::make_object(std::move(obj));
}

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

[[nodiscard]] inline json::Value chat_call_recording_to_json(ChatCallRecording const& rec) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("request", chat_request_to_json(rec.request));
    obj.emplace_back("mode", json::Value::make_string(rec.mode == recording_mode::unary ? "unary" : "streaming"));
    obj.emplace_back("duration_ms", json::Value::make_number(static_cast<double>(rec.duration.count())));

    if (rec.mode == recording_mode::unary) {
        if (rec.response) obj.emplace_back("response", chat_response_to_json(*rec.response));
        if (rec.chat_error) obj.emplace_back("chat_error", error_to_json(*rec.chat_error));
    } else {
        std::vector<json::Value> chunks;
        chunks.reserve(rec.chunks.size());
        for (auto const& c : rec.chunks) {
            std::vector<std::pair<std::string, json::Value>> chunk_obj;
            chunk_obj.emplace_back("update", chat_response_update_to_json(c.update));
            chunk_obj.emplace_back("elapsed_ms",
                                    json::Value::make_number(static_cast<double>(c.elapsed_since_start.count())));
            chunks.push_back(json::Value::make_object(std::move(chunk_obj)));
        }
        obj.emplace_back("chunks", json::Value::make_array(std::move(chunks)));
        obj.emplace_back("stream_terminal", json::Value::make_string(rec.stream_terminal));
        if (!rec.stream_error_detail.empty()) {
            obj.emplace_back("stream_error_detail", json::Value::make_string(rec.stream_error_detail));
        }
        if (rec.stream_error.has_value()) {
            obj.emplace_back("stream_error", error_to_json(*rec.stream_error));
        }
    }
    return json::Value::make_object(std::move(obj));
}

[[nodiscard]] inline result<ChatCallRecording> chat_call_recording_from_json(json::Value const& j) {
    ChatCallRecording rec;
    if (auto const* request_json = j.find("request"); request_json != nullptr) {
        // messages/output_schema_json/idempotency_key round-trip; `tools` is intentionally lossy for
        // the reason chat_request_to_json's neighbour comment gives, so only messages/schema/key are
        // rehydrated here -- a replayed recording's `request.tools` therefore always reads back empty,
        // which is fine, since nothing reads it back into a live call (see the note above).
        if (auto const* messages = request_json->find("messages"); messages != nullptr && messages->is_array()) {
            for (auto const& m_json : messages->as_array()) {
                auto m = message_from_json(m_json);
                if (!m) return std::unexpected(m.error());
                rec.request.messages.push_back(std::move(*m));
            }
        }
        if (auto const* schema = request_json->find("output_schema_json");
            schema != nullptr && schema->is_string()) {
            rec.request.output_schema_json = schema->as_string();
        }
        if (auto const* key = request_json->find("idempotency_key"); key != nullptr && key->is_string()) {
            rec.request.idempotency_key = key->as_string();
        }
    }

    std::string const mode_str = recording_detail::opt_string(j, "mode", "unary");
    rec.mode = (mode_str == "streaming") ? recording_mode::streaming : recording_mode::unary;
    rec.duration = std::chrono::milliseconds(static_cast<std::int64_t>(recording_detail::opt_u64(j, "duration_ms")));

    if (rec.mode == recording_mode::unary) {
        if (auto const* response_json = j.find("response"); response_json != nullptr) {
            auto response = chat_response_from_json(*response_json);
            if (!response) return std::unexpected(response.error());
            rec.response = std::move(*response);
        }
        if (auto const* error_json = j.find("chat_error"); error_json != nullptr) {
            auto err = error_from_json(*error_json);
            if (!err) return std::unexpected(err.error());
            rec.chat_error = std::move(*err);
        }
    } else {
        if (auto const* chunks_json = j.find("chunks"); chunks_json != nullptr && chunks_json->is_array()) {
            for (auto const& chunk_json : chunks_json->as_array()) {
                auto update_v = recording_detail::require(chunk_json, "update");
                if (!update_v) return std::unexpected(update_v.error());
                auto update = chat_response_update_from_json(**update_v);
                if (!update) return std::unexpected(update.error());
                RecordedChunk chunk;
                chunk.update = std::move(*update);
                chunk.elapsed_since_start =
                    std::chrono::milliseconds(static_cast<std::int64_t>(recording_detail::opt_u64(chunk_json, "elapsed_ms")));
                rec.chunks.push_back(std::move(chunk));
            }
        }
        rec.stream_terminal = recording_detail::opt_string(j, "stream_terminal");
        rec.stream_error_detail = recording_detail::opt_string(j, "stream_error_detail");
        if (auto const* se = j.find("stream_error"); se != nullptr && se->is_object()) {
            auto err = error_from_json(*se);
            if (!err) return std::unexpected(err.error());
            rec.stream_error = std::move(*err);
        }
    }

    return rec;
}

// --- File I/O --------------------------------------------------------------------------------------

[[nodiscard]] inline result<void> write_chat_call_recording(std::filesystem::path const& path,
                                                              ChatCallRecording const& rec) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(
            error{failure_class::fatal, "cannot open recording file for write: " + path.string(),
                  "recording.write_open_failed"});
    }
    std::string const text = json::dump(chat_call_recording_to_json(rec));
    out << text;
    if (!out) {
        return std::unexpected(error{failure_class::fatal, "write failed: " + path.string(),
                                      "recording.write_failed"});
    }
    return {};
}

[[nodiscard]] inline result<ChatCallRecording> read_chat_call_recording(std::filesystem::path const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(error{failure_class::fatal, "recording not found: " + path.string(),
                                      "recording.read_open_failed"});
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto parsed = json::parse(buffer.str());
    if (!parsed) return std::unexpected(parsed.error());
    return chat_call_recording_from_json(*parsed);
}

} // namespace agentengine
