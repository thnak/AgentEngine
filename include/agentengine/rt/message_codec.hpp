#pragma once
// ADR-037 Phase 3, Slice 2: agentengine::Message/ContentItem <-> JSON, for
// agentengine::rt::WorkflowSupervisor's checkpoint record (workflow_supervisor.hpp) -- content.hpp's
// own "a workflow's pending/partial payloads ARE its state" (content_record.hpp's own banner) means
// Slice 2 cannot narrow these fields away the way rt::AgentSession's own Slice 2 narrowed history/
// state out of AgentSessionRecord.
//
// ADAPTED FROM, NOT DESIGNED HERE: core/chat_recording.hpp already has a complete, proven Message/
// ContentItem <-> JSON codec (role_to_wire_string/content_item_to_json/message_to_json and their
// `_from_json`/`_from_wire_string` inverses) -- the functions below are that same logic, copied
// verbatim rather than reused via #include, for exactly one reason: chat_recording.hpp also includes
// "agentengine/core/chat_client.hpp" (needed there for its own ChatRequest/ChatResponse recording
// functions, which this file does not need) -- pulling that dependency chain into a file
// (workflow_supervisor.hpp) that has no ChatClientT involvement at all (its ExecutorBody is a plain
// std::function) is an unwarranted coupling regardless of what agentengine::task<T> itself resolves
// to today (historical: at the time this file was written, chat_client.hpp transitively pulled in
// the still-Quark-coupled `agentengine::task<T>` alias, core/task.hpp -- that residual is long since
// closed, `task<T>` is Quark-free now, but the coupling argument stands on its own). Duplicating
// ~350 lines of ALREADY-PROVEN,
// unchanged logic here keeps that clean claim intact; #include-ing chat_recording.hpp would not.
// A future consolidation (hoisting this codec somewhere both files can share without either pulling
// in the other's dependencies) is real follow-up work, not done here.
//
// SCOPE: only what content_item_to_json/message_to_json actually need -- role/content_origin wire
// strings, BlobRef, base64 (for Media's byte payload), and the 9-way ContentItem variant. Does NOT
// include chat_recording.hpp's ChatRequest/ChatResponse/ChatCallRecording machinery (irrelevant here)
// or its `agentengine::error`/`failure_class` wire codec (ContentItem's `Error` variant arm is
// content.hpp's own small `Error{message}` content type, unrelated to `agentengine::error`/
// `result<T>` -- no `failure_class` ever needs encoding for a Message payload).

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"

namespace agentengine::rt {

namespace message_codec_detail {

inline constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] inline std::string base64_encode(std::vector<std::byte> const& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= bytes.size()) {
        std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                           (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                           static_cast<std::uint32_t>(bytes[i + 2]);
        out += kBase64Alphabet[(n >> 18) & 0x3F];
        out += kBase64Alphabet[(n >> 12) & 0x3F];
        out += kBase64Alphabet[(n >> 6) & 0x3F];
        out += kBase64Alphabet[n & 0x3F];
        i += 3;
    }
    std::size_t const remaining = bytes.size() - i;
    if (remaining == 1) {
        std::uint32_t n = static_cast<std::uint32_t>(bytes[i]) << 16;
        out += kBase64Alphabet[(n >> 18) & 0x3F];
        out += kBase64Alphabet[(n >> 12) & 0x3F];
        out += "==";
    } else if (remaining == 2) {
        std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                           (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
        out += kBase64Alphabet[(n >> 18) & 0x3F];
        out += kBase64Alphabet[(n >> 12) & 0x3F];
        out += kBase64Alphabet[(n >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

[[nodiscard]] inline agentengine::result<std::vector<std::byte>> base64_decode(std::string_view text) {
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
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                        "invalid base64 character",
                                                        "rt.message_codec.bad_base64"});
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

[[nodiscard]] inline std::string opt_string(agentengine::json::Value const& obj, std::string_view key,
                                             std::string fallback = {}) {
    agentengine::json::Value const* v = obj.find(key);
    if (v == nullptr || !v->is_string()) return fallback;
    return v->as_string();
}
[[nodiscard]] inline bool opt_bool(agentengine::json::Value const& obj, std::string_view key,
                                    bool fallback = false) {
    agentengine::json::Value const* v = obj.find(key);
    if (v == nullptr || !v->is_bool()) return fallback;
    return v->as_bool();
}
[[nodiscard]] inline std::uint64_t opt_u64(agentengine::json::Value const& obj, std::string_view key,
                                            std::uint64_t fallback = 0) {
    agentengine::json::Value const* v = obj.find(key);
    if (v == nullptr || !v->is_number()) return fallback;
    return static_cast<std::uint64_t>(v->as_number());
}

}  // namespace message_codec_detail

[[nodiscard]] inline std::string_view role_to_wire_string(agentengine::role r) noexcept {
    switch (r) {
        case agentengine::role::system:    return "system";
        case agentengine::role::user:      return "user";
        case agentengine::role::assistant: return "assistant";
        case agentengine::role::tool:      return "tool";
    }
    return "user";
}
[[nodiscard]] inline agentengine::result<agentengine::role> role_from_wire_string(std::string_view s) {
    if (s == "system") return agentengine::role::system;
    if (s == "user") return agentengine::role::user;
    if (s == "assistant") return agentengine::role::assistant;
    if (s == "tool") return agentengine::role::tool;
    return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                               "unknown role: " + std::string(s),
                                               "rt.message_codec.bad_role"});
}

[[nodiscard]] inline std::string_view origin_to_wire_string(agentengine::content_origin o) noexcept {
    switch (o) {
        case agentengine::content_origin::user:      return "user";
        case agentengine::content_origin::assistant: return "assistant";
        case agentengine::content_origin::tool:      return "tool";
        case agentengine::content_origin::system:    return "system";
        case agentengine::content_origin::external:  return "external";
    }
    return "assistant";
}
[[nodiscard]] inline agentengine::result<agentengine::content_origin> origin_from_wire_string(
    std::string_view s) {
    if (s == "user") return agentengine::content_origin::user;
    if (s == "assistant") return agentengine::content_origin::assistant;
    if (s == "tool") return agentengine::content_origin::tool;
    if (s == "system") return agentengine::content_origin::system;
    if (s == "external") return agentengine::content_origin::external;
    return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                               "unknown content_origin: " + std::string(s),
                                               "rt.message_codec.bad_origin"});
}

[[nodiscard]] json::Value content_item_to_json(agentengine::ContentItem const& item);
[[nodiscard]] agentengine::result<agentengine::ContentItem> content_item_from_json(
    agentengine::json::Value const& j);

namespace message_codec_detail {

[[nodiscard]] inline agentengine::json::Value blob_ref_to_json(agentengine::BlobRef const& b) {
    std::vector<std::pair<std::string, agentengine::json::Value>> obj;
    obj.emplace_back("digest", agentengine::json::Value::make_string(b.digest));
    obj.emplace_back("media_type", agentengine::json::Value::make_string(b.media_type));
    obj.emplace_back("size", agentengine::json::Value::make_number(static_cast<double>(b.size)));
    obj.emplace_back("store", agentengine::json::Value::make_string(b.store));
    return agentengine::json::Value::make_object(std::move(obj));
}
[[nodiscard]] inline agentengine::result<agentengine::BlobRef> blob_ref_from_json(
    agentengine::json::Value const& j) {
    agentengine::BlobRef b;
    b.digest     = opt_string(j, "digest");
    b.media_type = opt_string(j, "media_type");
    b.size       = static_cast<std::size_t>(opt_u64(j, "size"));
    b.store      = opt_string(j, "store");
    return b;
}

}  // namespace message_codec_detail

[[nodiscard]] inline agentengine::json::Value content_item_to_json(agentengine::ContentItem const& item) {
    using agentengine::Text;
    using agentengine::Reasoning;
    using agentengine::Media;
    using agentengine::Data;
    using agentengine::ToolCall;
    using agentengine::ToolResult;
    using agentengine::Citation;
    using agentengine::Error;
    using agentengine::Custom;
    using agentengine::BlobRef;
    namespace mcd = message_codec_detail;

    std::vector<std::pair<std::string, agentengine::json::Value>> obj;

    if (auto const* t = std::get_if<Text>(&item.value)) {
        obj.emplace_back("kind", agentengine::json::Value::make_string("text"));
        obj.emplace_back("text", agentengine::json::Value::make_string(t->text));
    } else if (auto const* r = std::get_if<Reasoning>(&item.value)) {
        obj.emplace_back("kind", agentengine::json::Value::make_string("reasoning"));
        obj.emplace_back("text", agentengine::json::Value::make_string(r->text));
        obj.emplace_back("encrypted", agentengine::json::Value::make_bool(r->encrypted));
    } else if (auto const* m = std::get_if<Media>(&item.value)) {
        obj.emplace_back("kind", agentengine::json::Value::make_string("media"));
        obj.emplace_back("media_type", agentengine::json::Value::make_string(m->media_type));
        if (auto const* bytes = std::get_if<std::vector<std::byte>>(&m->payload)) {
            obj.emplace_back("payload_kind", agentengine::json::Value::make_string("bytes"));
            obj.emplace_back("bytes_base64",
                             agentengine::json::Value::make_string(mcd::base64_encode(*bytes)));
        } else if (auto const* uri = std::get_if<std::string>(&m->payload)) {
            obj.emplace_back("payload_kind", agentengine::json::Value::make_string("uri"));
            obj.emplace_back("uri", agentengine::json::Value::make_string(*uri));
        } else {
            auto const& blob = std::get<BlobRef>(m->payload);
            obj.emplace_back("payload_kind", agentengine::json::Value::make_string("blob_ref"));
            obj.emplace_back("blob_ref", mcd::blob_ref_to_json(blob));
        }
    } else if (auto const* d = std::get_if<Data>(&item.value)) {
        obj.emplace_back("kind", agentengine::json::Value::make_string("data"));
        obj.emplace_back("json", agentengine::json::Value::make_string(d->json));
        if (d->schema_id) obj.emplace_back("schema_id", agentengine::json::Value::make_string(*d->schema_id));
    } else if (auto const* tc = std::get_if<ToolCall>(&item.value)) {
        obj.emplace_back("kind", agentengine::json::Value::make_string("tool_call"));
        obj.emplace_back("call_id", agentengine::json::Value::make_string(tc->call_id));
        obj.emplace_back("tool_name", agentengine::json::Value::make_string(tc->tool_name));
        obj.emplace_back("arguments_json", agentengine::json::Value::make_string(tc->arguments_json));
    } else if (auto const* tr = std::get_if<ToolResult>(&item.value)) {
        obj.emplace_back("kind", agentengine::json::Value::make_string("tool_result"));
        obj.emplace_back("call_id", agentengine::json::Value::make_string(tr->call_id));
        std::vector<agentengine::json::Value> content;
        content.reserve(tr->content.size());
        for (auto const& child : tr->content) content.push_back(content_item_to_json(child));
        obj.emplace_back("content", agentengine::json::Value::make_array(std::move(content)));
        obj.emplace_back("is_error", agentengine::json::Value::make_bool(tr->is_error));
    } else if (auto const* c = std::get_if<Citation>(&item.value)) {
        obj.emplace_back("kind", agentengine::json::Value::make_string("citation"));
        obj.emplace_back("source", agentengine::json::Value::make_string(c->source));
        obj.emplace_back("span_start", agentengine::json::Value::make_number(static_cast<double>(c->span_start)));
        obj.emplace_back("span_end", agentengine::json::Value::make_number(static_cast<double>(c->span_end)));
    } else if (auto const* e = std::get_if<Error>(&item.value)) {
        obj.emplace_back("kind", agentengine::json::Value::make_string("error"));
        obj.emplace_back("message", agentengine::json::Value::make_string(e->message));
    } else {
        auto const& cu = std::get<Custom>(item.value);
        obj.emplace_back("kind", agentengine::json::Value::make_string("custom"));
        obj.emplace_back("type_id", agentengine::json::Value::make_string(cu.type_id));
        obj.emplace_back("payload_json", agentengine::json::Value::make_string(cu.payload_json));
    }

    obj.emplace_back("origin",
                     agentengine::json::Value::make_string(std::string(origin_to_wire_string(item.origin))));
    obj.emplace_back("tainted", agentengine::json::Value::make_bool(item.tainted));
    return agentengine::json::Value::make_object(std::move(obj));
}

[[nodiscard]] inline agentengine::result<agentengine::ContentItem> content_item_from_json(
    agentengine::json::Value const& j) {
    using agentengine::Text;
    using agentengine::Reasoning;
    using agentengine::Media;
    using agentengine::Data;
    using agentengine::ToolCall;
    using agentengine::ToolResult;
    using agentengine::Citation;
    using agentengine::Error;
    using agentengine::Custom;
    namespace mcd = message_codec_detail;

    agentengine::json::Value const* kind_v = j.find("kind");
    if (kind_v == nullptr || !kind_v->is_string()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "missing field: kind",
                                                    "rt.message_codec.missing_field"});
    }
    std::string const kind = kind_v->as_string();

    agentengine::ContentItem item{};

    if (kind == "text") {
        item.value = Text{mcd::opt_string(j, "text")};
    } else if (kind == "reasoning") {
        item.value = Reasoning{mcd::opt_string(j, "text"), mcd::opt_bool(j, "encrypted")};
    } else if (kind == "media") {
        Media media;
        media.media_type = mcd::opt_string(j, "media_type");
        std::string const payload_kind = mcd::opt_string(j, "payload_kind");
        if (payload_kind == "bytes") {
            auto bytes = mcd::base64_decode(mcd::opt_string(j, "bytes_base64"));
            if (!bytes) return std::unexpected(bytes.error());
            media.payload = std::move(*bytes);
        } else if (payload_kind == "uri") {
            media.payload = mcd::opt_string(j, "uri");
        } else if (payload_kind == "blob_ref") {
            agentengine::json::Value const* blob_json = j.find("blob_ref");
            if (blob_json == nullptr) {
                return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                            "media blob_ref missing",
                                                            "rt.message_codec.missing_field"});
            }
            auto blob = mcd::blob_ref_from_json(*blob_json);
            if (!blob) return std::unexpected(blob.error());
            media.payload = std::move(*blob);
        } else {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                        "unknown media payload_kind: " + payload_kind,
                                                        "rt.message_codec.bad_media_payload_kind"});
        }
        item.value = std::move(media);
    } else if (kind == "data") {
        Data data;
        data.json = mcd::opt_string(j, "json");
        if (agentengine::json::Value const* schema_id = j.find("schema_id");
            schema_id != nullptr && schema_id->is_string()) {
            data.schema_id = schema_id->as_string();
        }
        item.value = std::move(data);
    } else if (kind == "tool_call") {
        ToolCall tc;
        tc.call_id        = mcd::opt_string(j, "call_id");
        tc.tool_name      = mcd::opt_string(j, "tool_name");
        tc.arguments_json = mcd::opt_string(j, "arguments_json");
        item.value = std::move(tc);
    } else if (kind == "tool_result") {
        ToolResult tr;
        tr.call_id   = mcd::opt_string(j, "call_id");
        tr.is_error  = mcd::opt_bool(j, "is_error");
        if (agentengine::json::Value const* content = j.find("content");
            content != nullptr && content->is_array()) {
            for (auto const& child_json : content->as_array()) {
                auto child = content_item_from_json(child_json);
                if (!child) return std::unexpected(child.error());
                tr.content.push_back(std::move(*child));
            }
        }
        item.value = std::move(tr);
    } else if (kind == "citation") {
        Citation c;
        c.source     = mcd::opt_string(j, "source");
        c.span_start = static_cast<std::size_t>(mcd::opt_u64(j, "span_start"));
        c.span_end   = static_cast<std::size_t>(mcd::opt_u64(j, "span_end"));
        item.value = c;
    } else if (kind == "error") {
        item.value = Error{mcd::opt_string(j, "message")};
    } else if (kind == "custom") {
        Custom cu;
        cu.type_id      = mcd::opt_string(j, "type_id");
        cu.payload_json = mcd::opt_string(j, "payload_json");
        item.value = std::move(cu);
    } else {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "unsupported content kind: " + kind,
                                                    "rt.message_codec.bad_content_kind"});
    }

    if (agentengine::json::Value const* origin_json = j.find("origin");
        origin_json != nullptr && origin_json->is_string()) {
        auto origin = origin_from_wire_string(origin_json->as_string());
        if (!origin) return std::unexpected(origin.error());
        item.origin = *origin;
    }
    item.tainted = message_codec_detail::opt_bool(j, "tainted");
    return item;
}

[[nodiscard]] inline agentengine::json::Value message_to_json(agentengine::Message const& m) {
    std::vector<std::pair<std::string, agentengine::json::Value>> obj;
    obj.emplace_back("role", agentengine::json::Value::make_string(std::string(role_to_wire_string(m.role))));
    obj.emplace_back("message_id", agentengine::json::Value::make_string(m.message_id));
    std::vector<agentengine::json::Value> content;
    content.reserve(m.content.size());
    for (auto const& item : m.content) content.push_back(content_item_to_json(item));
    obj.emplace_back("content", agentengine::json::Value::make_array(std::move(content)));
    return agentengine::json::Value::make_object(std::move(obj));
}

[[nodiscard]] inline agentengine::result<agentengine::Message> message_from_json(
    agentengine::json::Value const& j) {
    auto role_v = role_from_wire_string(message_codec_detail::opt_string(j, "role", "user"));
    if (!role_v) return std::unexpected(role_v.error());
    agentengine::Message m;
    m.role       = *role_v;
    m.message_id = message_codec_detail::opt_string(j, "message_id");
    if (agentengine::json::Value const* content = j.find("content");
        content != nullptr && content->is_array()) {
        for (auto const& item_json : content->as_array()) {
            auto item = content_item_from_json(item_json);
            if (!item) return std::unexpected(item.error());
            m.content.push_back(std::move(*item));
        }
    }
    return m;
}

}  // namespace agentengine::rt
