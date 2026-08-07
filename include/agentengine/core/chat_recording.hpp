#pragma once
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
#include "agentengine/core/tool_pipeline.hpp"

namespace agentengine {

namespace recording_detail {

// Small, self-contained base64 -- the one `ContentItem` payload (`Media`'s `vector<std::byte>`
// alternative) that isn't already text. No third-party dependency for six lines of table lookup.
inline constexpr std::string_view base64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] inline std::string base64_encode(std::vector<std::byte> const& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= bytes.size()) {
        std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                           (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                           static_cast<std::uint32_t>(bytes[i + 2]);
        out += base64_alphabet[(n >> 18) & 0x3F];
        out += base64_alphabet[(n >> 12) & 0x3F];
        out += base64_alphabet[(n >> 6) & 0x3F];
        out += base64_alphabet[n & 0x3F];
        i += 3;
    }
    std::size_t const remaining = bytes.size() - i;
    if (remaining == 1) {
        std::uint32_t n = static_cast<std::uint32_t>(bytes[i]) << 16;
        out += base64_alphabet[(n >> 18) & 0x3F];
        out += base64_alphabet[(n >> 12) & 0x3F];
        out += "==";
    } else if (remaining == 2) {
        std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                           (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
        out += base64_alphabet[(n >> 18) & 0x3F];
        out += base64_alphabet[(n >> 12) & 0x3F];
        out += base64_alphabet[(n >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

[[nodiscard]] inline result<std::vector<std::byte>> base64_decode(std::string_view text) {
    auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int const v = decode_char(c);
        if (v < 0) {
            return std::unexpected(
                error{failure_class::contract, "invalid base64 character", "recording.bad_base64"});
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

[[nodiscard]] inline result<json::Value const*> require(json::Value const& obj, std::string_view key) {
    json::Value const* v = obj.find(key);
    if (v == nullptr) {
        return std::unexpected(
            error{failure_class::contract, "missing field: " + std::string(key), "recording.missing_field"});
    }
    return v;
}

[[nodiscard]] inline std::string opt_string(json::Value const& obj, std::string_view key,
                                             std::string fallback = {}) {
    json::Value const* v = obj.find(key);
    if (v == nullptr || !v->is_string()) return fallback;
    return v->as_string();
}

[[nodiscard]] inline bool opt_bool(json::Value const& obj, std::string_view key, bool fallback = false) {
    json::Value const* v = obj.find(key);
    if (v == nullptr || !v->is_bool()) return fallback;
    return v->as_bool();
}

[[nodiscard]] inline std::uint64_t opt_u64(json::Value const& obj, std::string_view key,
                                            std::uint64_t fallback = 0) {
    json::Value const* v = obj.find(key);
    if (v == nullptr || !v->is_number()) return fallback;
    return static_cast<std::uint64_t>(v->as_number());
}

} // namespace recording_detail

// --- role / content_origin / failure_class <-> wire string -----------------------------------------

[[nodiscard]] inline std::string_view role_to_wire_string(role r) noexcept {
    switch (r) {
        case role::system: return "system";
        case role::user: return "user";
        case role::assistant: return "assistant";
        case role::tool: return "tool";
    }
    return "user";
}

[[nodiscard]] inline result<role> role_from_wire_string(std::string_view s) {
    if (s == "system") return role::system;
    if (s == "user") return role::user;
    if (s == "assistant") return role::assistant;
    if (s == "tool") return role::tool;
    return std::unexpected(error{failure_class::contract, "unknown role: " + std::string(s),
                                  "recording.bad_role"});
}

[[nodiscard]] inline std::string_view origin_to_wire_string(content_origin o) noexcept {
    switch (o) {
        case content_origin::user: return "user";
        case content_origin::assistant: return "assistant";
        case content_origin::tool: return "tool";
        case content_origin::system: return "system";
        case content_origin::external: return "external";
    }
    return "assistant";
}

[[nodiscard]] inline result<content_origin> origin_from_wire_string(std::string_view s) {
    if (s == "user") return content_origin::user;
    if (s == "assistant") return content_origin::assistant;
    if (s == "tool") return content_origin::tool;
    if (s == "system") return content_origin::system;
    if (s == "external") return content_origin::external;
    return std::unexpected(error{failure_class::contract, "unknown content_origin: " + std::string(s),
                                  "recording.bad_origin"});
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

// --- ContentItem <-> json (all 9 variant alternatives) ------------------------------------------------

[[nodiscard]] json::Value content_item_to_json(ContentItem const& item);
[[nodiscard]] result<ContentItem> content_item_from_json(json::Value const& j);

namespace recording_detail {

[[nodiscard]] inline json::Value blob_ref_to_json(BlobRef const& b) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("digest", json::Value::make_string(b.digest));
    obj.emplace_back("media_type", json::Value::make_string(b.media_type));
    obj.emplace_back("size", json::Value::make_number(static_cast<double>(b.size)));
    obj.emplace_back("store", json::Value::make_string(b.store));
    return json::Value::make_object(std::move(obj));
}

[[nodiscard]] inline result<BlobRef> blob_ref_from_json(json::Value const& j) {
    BlobRef b;
    b.digest = opt_string(j, "digest");
    b.media_type = opt_string(j, "media_type");
    b.size = static_cast<std::size_t>(opt_u64(j, "size"));
    b.store = opt_string(j, "store");
    return b;
}

} // namespace recording_detail

[[nodiscard]] inline json::Value content_item_to_json(ContentItem const& item) {
    std::vector<std::pair<std::string, json::Value>> obj;

    if (auto const* t = std::get_if<Text>(&item.value)) {
        obj.emplace_back("kind", json::Value::make_string("text"));
        obj.emplace_back("text", json::Value::make_string(t->text));
    } else if (auto const* r = std::get_if<Reasoning>(&item.value)) {
        obj.emplace_back("kind", json::Value::make_string("reasoning"));
        obj.emplace_back("text", json::Value::make_string(r->text));
        obj.emplace_back("encrypted", json::Value::make_bool(r->encrypted));
    } else if (auto const* m = std::get_if<Media>(&item.value)) {
        obj.emplace_back("kind", json::Value::make_string("media"));
        obj.emplace_back("media_type", json::Value::make_string(m->media_type));
        if (auto const* bytes = std::get_if<std::vector<std::byte>>(&m->payload)) {
            obj.emplace_back("payload_kind", json::Value::make_string("bytes"));
            obj.emplace_back("bytes_base64", json::Value::make_string(recording_detail::base64_encode(*bytes)));
        } else if (auto const* uri = std::get_if<std::string>(&m->payload)) {
            obj.emplace_back("payload_kind", json::Value::make_string("uri"));
            obj.emplace_back("uri", json::Value::make_string(*uri));
        } else {
            auto const& blob = std::get<BlobRef>(m->payload);
            obj.emplace_back("payload_kind", json::Value::make_string("blob_ref"));
            obj.emplace_back("blob_ref", recording_detail::blob_ref_to_json(blob));
        }
    } else if (auto const* d = std::get_if<Data>(&item.value)) {
        obj.emplace_back("kind", json::Value::make_string("data"));
        obj.emplace_back("json", json::Value::make_string(d->json));
        if (d->schema_id) obj.emplace_back("schema_id", json::Value::make_string(*d->schema_id));
    } else if (auto const* tc = std::get_if<ToolCall>(&item.value)) {
        obj.emplace_back("kind", json::Value::make_string("tool_call"));
        obj.emplace_back("call_id", json::Value::make_string(tc->call_id));
        obj.emplace_back("tool_name", json::Value::make_string(tc->tool_name));
        obj.emplace_back("arguments_json", json::Value::make_string(tc->arguments_json));
    } else if (auto const* tr = std::get_if<ToolResult>(&item.value)) {
        obj.emplace_back("kind", json::Value::make_string("tool_result"));
        obj.emplace_back("call_id", json::Value::make_string(tr->call_id));
        std::vector<json::Value> content;
        content.reserve(tr->content.size());
        for (auto const& child : tr->content) content.push_back(content_item_to_json(child));
        obj.emplace_back("content", json::Value::make_array(std::move(content)));
        obj.emplace_back("is_error", json::Value::make_bool(tr->is_error));
    } else if (auto const* c = std::get_if<Citation>(&item.value)) {
        obj.emplace_back("kind", json::Value::make_string("citation"));
        obj.emplace_back("source", json::Value::make_string(c->source));
        obj.emplace_back("span_start", json::Value::make_number(static_cast<double>(c->span_start)));
        obj.emplace_back("span_end", json::Value::make_number(static_cast<double>(c->span_end)));
    } else if (auto const* e = std::get_if<Error>(&item.value)) {
        obj.emplace_back("kind", json::Value::make_string("error"));
        obj.emplace_back("message", json::Value::make_string(e->message));
    } else {
        auto const& cu = std::get<Custom>(item.value);
        obj.emplace_back("kind", json::Value::make_string("custom"));
        obj.emplace_back("type_id", json::Value::make_string(cu.type_id));
        obj.emplace_back("payload_json", json::Value::make_string(cu.payload_json));
    }

    obj.emplace_back("origin", json::Value::make_string(std::string(origin_to_wire_string(item.origin))));
    obj.emplace_back("tainted", json::Value::make_bool(item.tainted));
    return json::Value::make_object(std::move(obj));
}

[[nodiscard]] inline result<ContentItem> content_item_from_json(json::Value const& j) {
    auto kind_v = recording_detail::require(j, "kind");
    if (!kind_v) return std::unexpected(kind_v.error());
    std::string const kind = (*kind_v)->as_string();

    ContentItem item{};

    if (kind == "text") {
        item.value = Text{recording_detail::opt_string(j, "text")};
    } else if (kind == "reasoning") {
        item.value = Reasoning{recording_detail::opt_string(j, "text"),
                                recording_detail::opt_bool(j, "encrypted")};
    } else if (kind == "media") {
        Media media;
        media.media_type = recording_detail::opt_string(j, "media_type");
        std::string const payload_kind = recording_detail::opt_string(j, "payload_kind");
        if (payload_kind == "bytes") {
            auto bytes = recording_detail::base64_decode(recording_detail::opt_string(j, "bytes_base64"));
            if (!bytes) return std::unexpected(bytes.error());
            media.payload = std::move(*bytes);
        } else if (payload_kind == "uri") {
            media.payload = recording_detail::opt_string(j, "uri");
        } else if (payload_kind == "blob_ref") {
            auto const* blob_json = j.find("blob_ref");
            if (blob_json == nullptr) {
                return std::unexpected(
                    error{failure_class::contract, "media blob_ref missing", "recording.missing_field"});
            }
            auto blob = recording_detail::blob_ref_from_json(*blob_json);
            if (!blob) return std::unexpected(blob.error());
            media.payload = std::move(*blob);
        } else {
            return std::unexpected(error{failure_class::contract,
                                          "unknown media payload_kind: " + payload_kind,
                                          "recording.bad_media_payload_kind"});
        }
        item.value = std::move(media);
    } else if (kind == "data") {
        Data data;
        data.json = recording_detail::opt_string(j, "json");
        if (auto const* schema_id = j.find("schema_id"); schema_id != nullptr && schema_id->is_string()) {
            data.schema_id = schema_id->as_string();
        }
        item.value = std::move(data);
    } else if (kind == "tool_call") {
        ToolCall tc;
        tc.call_id = recording_detail::opt_string(j, "call_id");
        tc.tool_name = recording_detail::opt_string(j, "tool_name");
        tc.arguments_json = recording_detail::opt_string(j, "arguments_json");
        item.value = std::move(tc);
    } else if (kind == "tool_result") {
        ToolResult tr;
        tr.call_id = recording_detail::opt_string(j, "call_id");
        tr.is_error = recording_detail::opt_bool(j, "is_error");
        if (auto const* content = j.find("content"); content != nullptr && content->is_array()) {
            for (auto const& child_json : content->as_array()) {
                auto child = content_item_from_json(child_json);
                if (!child) return std::unexpected(child.error());
                tr.content.push_back(std::move(*child));
            }
        }
        item.value = std::move(tr);
    } else if (kind == "citation") {
        Citation c;
        c.source = recording_detail::opt_string(j, "source");
        c.span_start = static_cast<std::size_t>(recording_detail::opt_u64(j, "span_start"));
        c.span_end = static_cast<std::size_t>(recording_detail::opt_u64(j, "span_end"));
        item.value = c;
    } else if (kind == "error") {
        item.value = Error{recording_detail::opt_string(j, "message")};
    } else if (kind == "custom") {
        Custom cu;
        cu.type_id = recording_detail::opt_string(j, "type_id");
        cu.payload_json = recording_detail::opt_string(j, "payload_json");
        item.value = std::move(cu);
    } else {
        return std::unexpected(error{failure_class::contract, "unsupported content kind: " + kind,
                                      "recording.bad_content_kind"});
    }

    if (auto const* origin_json = j.find("origin"); origin_json != nullptr && origin_json->is_string()) {
        auto origin = origin_from_wire_string(origin_json->as_string());
        if (!origin) return std::unexpected(origin.error());
        item.origin = *origin;
    }
    item.tainted = recording_detail::opt_bool(j, "tainted");
    return item;
}

// --- Message <-> json --------------------------------------------------------------------------------

[[nodiscard]] inline json::Value message_to_json(Message const& m) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("role", json::Value::make_string(std::string(role_to_wire_string(m.role))));
    obj.emplace_back("message_id", json::Value::make_string(m.message_id));
    std::vector<json::Value> content;
    content.reserve(m.content.size());
    for (auto const& item : m.content) content.push_back(content_item_to_json(item));
    obj.emplace_back("content", json::Value::make_array(std::move(content)));
    return json::Value::make_object(std::move(obj));
}

[[nodiscard]] inline result<Message> message_from_json(json::Value const& j) {
    auto role_v = role_from_wire_string(recording_detail::opt_string(j, "role", "user"));
    if (!role_v) return std::unexpected(role_v.error());
    Message m;
    m.role = *role_v;
    m.message_id = recording_detail::opt_string(j, "message_id");
    if (auto const* content = j.find("content"); content != nullptr && content->is_array()) {
        for (auto const& item_json : content->as_array()) {
            auto item = content_item_from_json(item_json);
            if (!item) return std::unexpected(item.error());
            m.content.push_back(std::move(*item));
        }
    }
    return m;
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
    return r;
}

[[nodiscard]] inline json::Value chat_response_update_to_json(ChatResponseUpdate const& u) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("delta", content_item_to_json(u.delta));
    obj.emplace_back("is_final", json::Value::make_bool(u.is_final));
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
