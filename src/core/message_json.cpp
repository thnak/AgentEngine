// #120 S3 (ADR-202): the one Message/ContentItem <-> JSON codec's bodies (include/agentengine/core/message_json.hpp).
// Moved from the two copies it replaces -- core/chat_recording.hpp's `recording_detail` codec and
// rt/message_codec.hpp's `message_codec_detail` codec -- which agreed line for line apart from what
// `message_json::Profile` now selects: the delivery marks, `Message::attribution`, the error-code prefix and the
// "kind" type check. ADR-202 §4 lists each difference; tests/core/chat/test_message_json_equivalence.cpp compares
// this file against verbatim copies of both old codecs.

#include "agentengine/core/message_json.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace agentengine::message_json {

namespace {

[[nodiscard]] std::string error_code(Profile profile, std::string_view suffix) {
    std::string code(profile.error_code_prefix());
    code += suffix;
    return code;
}

// Small, self-contained base64 -- the one `ContentItem` payload (`Media`'s `vector<std::byte>`
// alternative) that isn't already text. No third-party dependency for six lines of table lookup.
constexpr std::string_view base64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] std::string base64_encode(std::vector<std::byte> const& bytes) {
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

// Lenient by design, in both old copies alike: '=', '\n' and '\r' are skipped wherever they appear, a trailing
// partial group is dropped, and only a character outside the alphabet is an error.
[[nodiscard]] result<std::vector<std::byte>> base64_decode(std::string_view text, Profile profile) {
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
                error{failure_class::contract, "invalid base64 character", error_code(profile, "bad_base64")});
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

[[nodiscard]] json::Value blob_ref_to_json(BlobRef const& b) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("digest", json::Value::make_string(b.digest));
    obj.emplace_back("media_type", json::Value::make_string(b.media_type));
    obj.emplace_back("size", json::Value::make_number(static_cast<double>(b.size)));
    obj.emplace_back("store", json::Value::make_string(b.store));
    return json::Value::make_object(std::move(obj));
}

// Never fails (every field is optional); kept as a `result` because both old copies returned one.
[[nodiscard]] result<BlobRef> blob_ref_from_json(json::Value const& j) {
    BlobRef b;
    b.digest = detail::opt_string(j, "digest");
    b.media_type = detail::opt_string(j, "media_type");
    b.size = static_cast<std::size_t>(detail::opt_u64(j, "size"));
    b.store = detail::opt_string(j, "store");
    return b;
}

}  // namespace

// --- role / content_origin <-> wire string ----------------------------------------------------------

std::string_view role_to_wire_string(role r) noexcept {
    switch (r) {
        case role::system: return "system";
        case role::user: return "user";
        case role::assistant: return "assistant";
        case role::tool: return "tool";
    }
    return "user";
}

result<role> role_from_wire_string(std::string_view s, Profile profile) {
    if (s == "system") return role::system;
    if (s == "user") return role::user;
    if (s == "assistant") return role::assistant;
    if (s == "tool") return role::tool;
    return std::unexpected(
        error{failure_class::contract, "unknown role: " + std::string(s), error_code(profile, "bad_role")});
}

std::string_view origin_to_wire_string(content_origin o) noexcept {
    switch (o) {
        case content_origin::user: return "user";
        case content_origin::assistant: return "assistant";
        case content_origin::tool: return "tool";
        case content_origin::system: return "system";
        case content_origin::external: return "external";
    }
    return "assistant";
}

result<content_origin> origin_from_wire_string(std::string_view s, Profile profile) {
    if (s == "user") return content_origin::user;
    if (s == "assistant") return content_origin::assistant;
    if (s == "tool") return content_origin::tool;
    if (s == "system") return content_origin::system;
    if (s == "external") return content_origin::external;
    return std::unexpected(error{failure_class::contract, "unknown content_origin: " + std::string(s),
                                  error_code(profile, "bad_origin")});
}

// --- ContentItem <-> json (all 9 variant alternatives) ------------------------------------------------

json::Value content_item_to_json(ContentItem const& item, Profile profile) {
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
            obj.emplace_back("bytes_base64", json::Value::make_string(base64_encode(*bytes)));
        } else if (auto const* uri = std::get_if<std::string>(&m->payload)) {
            obj.emplace_back("payload_kind", json::Value::make_string("uri"));
            obj.emplace_back("uri", json::Value::make_string(*uri));
        } else {
            auto const& blob = std::get<BlobRef>(m->payload);
            obj.emplace_back("payload_kind", json::Value::make_string("blob_ref"));
            obj.emplace_back("blob_ref", blob_ref_to_json(blob));
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
        for (auto const& child : tr->content) content.push_back(message_json::content_item_to_json(child, profile));
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

    obj.emplace_back("origin",
                     json::Value::make_string(std::string(message_json::origin_to_wire_string(item.origin))));
    obj.emplace_back("tainted", json::Value::make_bool(item.tainted));
    // Recording profile only (I3: the state profile never writes them -- see `Profile`).
    // ADR-191: recorded so a replayed request renders the same fences and preamble (I5 -- the per-request code in an
    // approved request's markers is drawn fresh at serialization and is not part of the request) and the audit shows
    // which approval reached the model (I4). Omitted when empty, so older recordings are unchanged.
    // ADR-192: the same, for text the host told the session to deliver unfenced. Omitted when false.
    if (profile.carries_delivery_marks()) {
        if (!item.approval.empty()) obj.emplace_back("approval", json::Value::make_string(item.approval));
        if (item.deliver_as_instructions) obj.emplace_back("deliver_as_instructions", json::Value::make_bool(true));
    }
    return json::Value::make_object(std::move(obj));
}

result<ContentItem> content_item_from_json(json::Value const& j, Profile profile) {
    json::Value const* kind_v = j.find("kind");
    if (kind_v == nullptr || (profile.rejects_non_string_kind() && !kind_v->is_string())) {
        return std::unexpected(
            error{failure_class::contract, "missing field: kind", error_code(profile, "missing_field")});
    }
    // Recording profile: a non-string "kind" reaches here and `as_string()` throws std::bad_variant_access, exactly
    // as the old recording codec did (ADR-202 §4, D3).
    std::string const kind = kind_v->as_string();

    ContentItem item{};

    if (kind == "text") {
        item.value = Text{detail::opt_string(j, "text")};
    } else if (kind == "reasoning") {
        item.value = Reasoning{detail::opt_string(j, "text"), detail::opt_bool(j, "encrypted")};
    } else if (kind == "media") {
        Media media;
        media.media_type = detail::opt_string(j, "media_type");
        std::string const payload_kind = detail::opt_string(j, "payload_kind");
        if (payload_kind == "bytes") {
            auto bytes = base64_decode(detail::opt_string(j, "bytes_base64"), profile);
            if (!bytes) return std::unexpected(bytes.error());
            media.payload = std::move(*bytes);
        } else if (payload_kind == "uri") {
            media.payload = detail::opt_string(j, "uri");
        } else if (payload_kind == "blob_ref") {
            json::Value const* blob_json = j.find("blob_ref");
            if (blob_json == nullptr) {
                return std::unexpected(
                    error{failure_class::contract, "media blob_ref missing", error_code(profile, "missing_field")});
            }
            auto blob = blob_ref_from_json(*blob_json);
            if (!blob) return std::unexpected(blob.error());
            media.payload = std::move(*blob);
        } else {
            return std::unexpected(error{failure_class::contract, "unknown media payload_kind: " + payload_kind,
                                          error_code(profile, "bad_media_payload_kind")});
        }
        item.value = std::move(media);
    } else if (kind == "data") {
        Data data;
        data.json = detail::opt_string(j, "json");
        if (json::Value const* schema_id = j.find("schema_id"); schema_id != nullptr && schema_id->is_string()) {
            data.schema_id = schema_id->as_string();
        }
        item.value = std::move(data);
    } else if (kind == "tool_call") {
        ToolCall tc;
        tc.call_id = detail::opt_string(j, "call_id");
        tc.tool_name = detail::opt_string(j, "tool_name");
        tc.arguments_json = detail::opt_string(j, "arguments_json");
        item.value = std::move(tc);
    } else if (kind == "tool_result") {
        ToolResult tr;
        tr.call_id = detail::opt_string(j, "call_id");
        tr.is_error = detail::opt_bool(j, "is_error");
        if (json::Value const* content = j.find("content"); content != nullptr && content->is_array()) {
            for (auto const& child_json : content->as_array()) {
                auto child = message_json::content_item_from_json(child_json, profile);
                if (!child) return std::unexpected(child.error());
                tr.content.push_back(std::move(*child));
            }
        }
        item.value = std::move(tr);
    } else if (kind == "citation") {
        Citation c;
        c.source = detail::opt_string(j, "source");
        c.span_start = static_cast<std::size_t>(detail::opt_u64(j, "span_start"));
        c.span_end = static_cast<std::size_t>(detail::opt_u64(j, "span_end"));
        item.value = c;
    } else if (kind == "error") {
        item.value = Error{detail::opt_string(j, "message")};
    } else if (kind == "custom") {
        Custom cu;
        cu.type_id = detail::opt_string(j, "type_id");
        cu.payload_json = detail::opt_string(j, "payload_json");
        item.value = std::move(cu);
    } else {
        return std::unexpected(error{failure_class::contract, "unsupported content kind: " + kind,
                                      error_code(profile, "bad_content_kind")});
    }

    if (json::Value const* origin_json = j.find("origin"); origin_json != nullptr && origin_json->is_string()) {
        auto origin = message_json::origin_from_wire_string(origin_json->as_string(), profile);
        if (!origin) return std::unexpected(origin.error());
        item.origin = *origin;
    }
    item.tainted = detail::opt_bool(j, "tainted");
    // Recording profile only. I3: the state profile never reads them, whatever the stored JSON holds -- a mark
    // restored from stored state would be stored data granting what only AgentSession may set (see `Profile`).
    if (profile.carries_delivery_marks()) {
        if (json::Value const* a = j.find("approval"); a != nullptr && a->is_string()) item.approval = a->as_string();
        item.deliver_as_instructions = detail::opt_bool(j, "deliver_as_instructions");
    }
    return item;
}

// --- Message <-> json --------------------------------------------------------------------------------

json::Value message_to_json(Message const& m, Profile profile) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("role", json::Value::make_string(std::string(message_json::role_to_wire_string(m.role))));
    obj.emplace_back("message_id", json::Value::make_string(m.message_id));
    std::vector<json::Value> content;
    content.reserve(m.content.size());
    for (auto const& item : m.content) content.push_back(message_json::content_item_to_json(item, profile));
    obj.emplace_back("content", json::Value::make_array(std::move(content)));
    // State profile only (ADR-066 §7 residual). Omit-when-absent, matching `Data::schema_id`'s convention above --
    // `nullopt` means "never routed through assemble_context()" (content.hpp's own `Message::attribution` comment),
    // which must round-trip back to `nullopt`, not a zeroed/default `ContributorProvenance`. The recording profile
    // never writes it: its output is hashed by the eval screen (ADR-195 §8) and must not change (ADR-202 §9).
    if (profile.carries_attribution() && m.attribution.has_value()) {
        std::vector<std::pair<std::string, json::Value>> attribution_obj;
        attribution_obj.emplace_back("contributor_index",
                                     json::Value::make_number(static_cast<double>(m.attribution->contributor_index)));
        attribution_obj.emplace_back("contributor_type", json::Value::make_string(m.attribution->contributor_type));
        obj.emplace_back("attribution", json::Value::make_object(std::move(attribution_obj)));
    }
    return json::Value::make_object(std::move(obj));
}

result<Message> message_from_json(json::Value const& j, Profile profile) {
    auto role_v = message_json::role_from_wire_string(detail::opt_string(j, "role", "user"), profile);
    if (!role_v) return std::unexpected(role_v.error());
    Message m;
    m.role = *role_v;
    m.message_id = detail::opt_string(j, "message_id");
    if (json::Value const* content = j.find("content"); content != nullptr && content->is_array()) {
        for (auto const& item_json : content->as_array()) {
            auto item = message_json::content_item_from_json(item_json, profile);
            if (!item) return std::unexpected(item.error());
            m.content.push_back(std::move(*item));
        }
    }
    if (profile.carries_attribution()) {
        if (json::Value const* attribution = j.find("attribution"); attribution != nullptr && attribution->is_object()) {
            ContributorProvenance provenance;
            provenance.contributor_index = static_cast<std::size_t>(detail::opt_u64(*attribution, "contributor_index"));
            provenance.contributor_type = detail::opt_string(*attribution, "contributor_type");
            m.attribution = provenance;
        }
    }
    return m;
}

}  // namespace agentengine::message_json
