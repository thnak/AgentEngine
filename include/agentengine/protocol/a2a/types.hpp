#pragma once
// Implements 012-A2A-Conformance.md's object model (§0/§A.4 of
// docs/research/2026-a2a-and-agui-detail.md's Part A) -- Milestone 7 Phase D1
// (docs/planning/milestone-7-protocol-conformance-breakdown.md). Wire types only: `Part`, `Message`,
// `Artifact`, `TaskStatus`, `Task`, `task_state`, `a2a_role`, with `to_json()`/`from_json()` proving
// v1.0's own breaking changes from 0.3.x are honoured -- NOT server/client role wiring (that is D2/D3,
// same "envelope first, roles after" split Phase C used for MCP's JSON-RPC layer, C1 before C2/C3).
//
// Terminology (027 §7, content.hpp's own file-top comment): this is `Part`, A2A's own word -- distinct
// from `agentengine::ContentItem` (003's internal content model). A `Task ← Run` / `Part ↔ ContentItem`
// PROJECTION is D2/D3's job (012 §1: "hosting a run as an A2A task is a projection, not a translation
// with a state-machine impedance mismatch"); this file only proves the wire shape itself round-trips.
//
// Two v1.0 breaking changes from 0.3.x this file exists to get right (research doc §A.1):
//   1. `Part`'s `kind` discriminator was REMOVED. The JSON member name (`text`/`raw`/`url`/`data`) IS
//      the discriminator -- there is no separate `"kind"` field alongside it.
//   2. Enums serialize as FULL SCREAMING_SNAKE names (`TASK_STATE_INPUT_REQUIRED`, `ROLE_USER`), never
//      the bare suffix a naive `enum class` stringification would produce.
//
// Unknown `Part` kinds round-trip via `UnknownPart` -- the same "unknown kinds round-trip" precedent
// `core/content.hpp`'s own `Custom` establishes for our internal content model, applied here to A2A's
// own oneof: a discriminator member this file doesn't recognize is preserved verbatim (member name +
// raw JSON value), not dropped, so a peer using a NEWER Part kind than we implement survives a
// round-trip through us unchanged (012 §6's own G2 gate, proven for the `Part` layer here; the fuller
// "every part kind, including unknown ones" corpus is G2's own job at milestone close).

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"

namespace agentengine::a2a {

namespace json = agentengine::json;

// research doc §A.5: `TASK_STATE_` x {UNSPECIFIED, SUBMITTED, WORKING, COMPLETED, FAILED, CANCELED,
// INPUT_REQUIRED, REJECTED, AUTH_REQUIRED}. Terminal: COMPLETED/FAILED/CANCELED/REJECTED. Interrupted
// (non-terminal): INPUT_REQUIRED/AUTH_REQUIRED.
enum class task_state {
    unspecified,
    submitted,
    working,
    completed,
    failed,
    canceled,
    input_required,
    rejected,
    auth_required,
};

[[nodiscard]] inline bool is_terminal(task_state s) noexcept {
    switch (s) {
        case task_state::completed:
        case task_state::failed:
        case task_state::canceled:
        case task_state::rejected:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline std::string_view to_wire_string(task_state s) noexcept {
    switch (s) {
        case task_state::unspecified:     return "TASK_STATE_UNSPECIFIED";
        case task_state::submitted:       return "TASK_STATE_SUBMITTED";
        case task_state::working:         return "TASK_STATE_WORKING";
        case task_state::completed:       return "TASK_STATE_COMPLETED";
        case task_state::failed:          return "TASK_STATE_FAILED";
        case task_state::canceled:        return "TASK_STATE_CANCELED";
        case task_state::input_required:  return "TASK_STATE_INPUT_REQUIRED";
        case task_state::rejected:        return "TASK_STATE_REJECTED";
        case task_state::auth_required:   return "TASK_STATE_AUTH_REQUIRED";
    }
    return "TASK_STATE_UNSPECIFIED";
}

[[nodiscard]] inline result<task_state> task_state_from_wire_string(std::string_view s) {
    if (s == "TASK_STATE_UNSPECIFIED") return task_state::unspecified;
    if (s == "TASK_STATE_SUBMITTED") return task_state::submitted;
    if (s == "TASK_STATE_WORKING") return task_state::working;
    if (s == "TASK_STATE_COMPLETED") return task_state::completed;
    if (s == "TASK_STATE_FAILED") return task_state::failed;
    if (s == "TASK_STATE_CANCELED") return task_state::canceled;
    if (s == "TASK_STATE_INPUT_REQUIRED") return task_state::input_required;
    if (s == "TASK_STATE_REJECTED") return task_state::rejected;
    if (s == "TASK_STATE_AUTH_REQUIRED") return task_state::auth_required;
    return std::unexpected(error{failure_class::contract, "unrecognized task_state: " + std::string(s),
                                  "a2a.unknown_task_state"});
}

enum class a2a_role { unspecified, user, agent };  // ae-naming-lint: allow a2a_role -- 012's own wire vocabulary, distinct from core/content.hpp's `role`

[[nodiscard]] inline std::string_view to_wire_string(a2a_role r) noexcept {
    switch (r) {
        case a2a_role::unspecified: return "ROLE_UNSPECIFIED";
        case a2a_role::user:        return "ROLE_USER";
        case a2a_role::agent:       return "ROLE_AGENT";
    }
    return "ROLE_UNSPECIFIED";
}

[[nodiscard]] inline result<a2a_role> a2a_role_from_wire_string(std::string_view s) {
    if (s == "ROLE_UNSPECIFIED") return a2a_role::unspecified;
    if (s == "ROLE_USER") return a2a_role::user;
    if (s == "ROLE_AGENT") return a2a_role::agent;
    return std::unexpected(
        error{failure_class::contract, "unrecognized role: " + std::string(s), "a2a.unknown_role"});
}

namespace detail {

// The same base64 idiom `core/chat_recording.hpp`'s own `recording_detail::base64_encode`/
// `base64_decode` already establish for the one non-text `ContentItem` payload -- reproduced here
// rather than pulled in through an unrelated recording-codec header for what is otherwise a
// self-contained, dependency-free six-line table lookup (CONVENTIONS' own "core... no third-party
// dependency" posture extends to not manufacturing cross-feature header coupling for one function).
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
                error{failure_class::contract, "invalid base64 character", "a2a.bad_base64"});
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

}  // namespace detail

struct TextPart { std::string text; };
struct RawPart { std::vector<std::byte> bytes; };  // wire: base64 string under the "raw" member
struct UrlPart { std::string url; };
struct DataPart { json::Value data; };
// Forward-compat: an unrecognized discriminator member -- see file-top comment.
struct UnknownPart {
    std::string member_name;
    json::Value  raw_value;
};

// §A.4: "a oneof of exactly one of text/raw/url/data ... no `kind` field in v1.0 ... plus common
// `metadata?`, `filename?`, `mediaType?`.`
struct Part {
    std::variant<TextPart, RawPart, UrlPart, DataPart, UnknownPart> value;
    std::optional<json::Value> metadata;
    std::optional<std::string> filename;
    std::optional<std::string> media_type;
};

// §A.4: `Message = {messageId, contextId?, taskId?, role, parts[] (>=1), metadata?, extensions[]?,
// referenceTaskIds[]?}`.
struct Message {
    std::string                     message_id;
    std::optional<std::string>      context_id;
    std::optional<std::string>      task_id;
    a2a_role                        role = a2a_role::unspecified;
    std::vector<Part>               parts;
    std::optional<json::Value>      metadata;
    std::vector<std::string>        extensions;
    std::vector<std::string>        reference_task_ids;
};

// §A.4: `Artifact = {artifactId, name?, description?, parts[] (>=1), metadata?, extensions[]?}`.
struct Artifact {
    std::string                 artifact_id;
    std::optional<std::string>  name;
    std::optional<std::string>  description;
    std::vector<Part>           parts;
    std::optional<json::Value>  metadata;
    std::vector<std::string>    extensions;
};

// §A.4: `TaskStatus = {state, message?, timestamp?}`.
struct TaskStatus {
    task_state                   state = task_state::unspecified;
    std::optional<Message>       message;
    std::optional<std::string>   timestamp;
};

// §A.4: `Task = {id, contextId?, status, artifacts[]?, history[]?, metadata?}`.
struct Task {
    std::string                  id;
    std::optional<std::string>   context_id;
    TaskStatus                   status;
    std::vector<Artifact>        artifacts;
    std::vector<Message>         history;
    std::optional<json::Value>   metadata;
};

namespace detail {

[[nodiscard]] inline json::Value strings_to_json(std::vector<std::string> const& v) {
    std::vector<json::Value> items;
    items.reserve(v.size());
    for (auto const& s : v) items.push_back(json::Value::make_string(s));
    return json::Value::make_array(std::move(items));
}

[[nodiscard]] inline result<std::vector<std::string>> strings_from_json(json::Value const* field) {
    std::vector<std::string> out;
    if (!field) return out;
    if (!field->is_array()) {
        return std::unexpected(
            error{failure_class::contract, "expected a JSON array of strings", "a2a.malformed_array"});
    }
    for (json::Value const& item : field->as_array()) {
        if (!item.is_string()) {
            return std::unexpected(
                error{failure_class::contract, "array element is not a string", "a2a.malformed_array"});
        }
        out.push_back(item.as_string());
    }
    return out;
}

}  // namespace detail

[[nodiscard]] inline json::Value to_json(Part const& p) {
    std::vector<std::pair<std::string, json::Value>> members;
    if (auto const* t = std::get_if<TextPart>(&p.value)) {
        members.emplace_back("text", json::Value::make_string(t->text));
    } else if (auto const* r = std::get_if<RawPart>(&p.value)) {
        members.emplace_back("raw", json::Value::make_string(detail::base64_encode(r->bytes)));
    } else if (auto const* u = std::get_if<UrlPart>(&p.value)) {
        members.emplace_back("url", json::Value::make_string(u->url));
    } else if (auto const* d = std::get_if<DataPart>(&p.value)) {
        members.emplace_back("data", d->data);
    } else {
        auto const& unk = std::get<UnknownPart>(p.value);
        members.emplace_back(unk.member_name, unk.raw_value);
    }
    if (p.metadata) members.emplace_back("metadata", *p.metadata);
    if (p.filename) members.emplace_back("filename", json::Value::make_string(*p.filename));
    if (p.media_type) members.emplace_back("mediaType", json::Value::make_string(*p.media_type));
    return json::Value::make_object(std::move(members));
}

[[nodiscard]] inline result<Part> part_from_json(json::Value const& v) {
    if (!v.is_object()) {
        return std::unexpected(
            error{failure_class::contract, "Part must be a JSON object", "a2a.malformed_part"});
    }
    Part part;
    // §A.4: the JSON member name IS the discriminator (v1.0 removed `kind`) -- the first member that
    // isn't one of the three common fields is the oneof's own key, whatever it is named.
    bool found_discriminator = false;
    for (auto const& [key, value] : v.as_object()) {
        if (key == "metadata") { part.metadata = value; continue; }
        if (key == "filename") {
            if (!value.is_string()) {
                return std::unexpected(error{failure_class::contract, "\"filename\" must be a string",
                                              "a2a.malformed_part"});
            }
            part.filename = value.as_string();
            continue;
        }
        if (key == "mediaType") {
            if (!value.is_string()) {
                return std::unexpected(error{failure_class::contract, "\"mediaType\" must be a string",
                                              "a2a.malformed_part"});
            }
            part.media_type = value.as_string();
            continue;
        }
        if (found_discriminator) {
            return std::unexpected(error{failure_class::contract,
                                          "Part carries more than one oneof member", "a2a.malformed_part"});
        }
        found_discriminator = true;
        if (key == "text") {
            if (!value.is_string()) {
                return std::unexpected(
                    error{failure_class::contract, "\"text\" must be a string", "a2a.malformed_part"});
            }
            part.value = TextPart{value.as_string()};
        } else if (key == "raw") {
            if (!value.is_string()) {
                return std::unexpected(
                    error{failure_class::contract, "\"raw\" must be a base64 string", "a2a.malformed_part"});
            }
            auto bytes = detail::base64_decode(value.as_string());
            if (!bytes) return std::unexpected(bytes.error());
            part.value = RawPart{std::move(*bytes)};
        } else if (key == "url") {
            if (!value.is_string()) {
                return std::unexpected(
                    error{failure_class::contract, "\"url\" must be a string", "a2a.malformed_part"});
            }
            part.value = UrlPart{value.as_string()};
        } else if (key == "data") {
            part.value = DataPart{value};
        } else {
            // Forward-compat: a discriminator this build doesn't recognize -- preserved, not dropped.
            part.value = UnknownPart{key, value};
        }
    }
    if (!found_discriminator) {
        return std::unexpected(error{failure_class::contract,
                                      "Part carries no oneof member (text/raw/url/data)",
                                      "a2a.malformed_part"});
    }
    return part;
}

[[nodiscard]] inline json::Value to_json(Message const& m) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("messageId", json::Value::make_string(m.message_id));
    if (m.context_id) members.emplace_back("contextId", json::Value::make_string(*m.context_id));
    if (m.task_id) members.emplace_back("taskId", json::Value::make_string(*m.task_id));
    members.emplace_back("role", json::Value::make_string(std::string(to_wire_string(m.role))));
    std::vector<json::Value> parts_json;
    parts_json.reserve(m.parts.size());
    for (Part const& p : m.parts) parts_json.push_back(to_json(p));
    members.emplace_back("parts", json::Value::make_array(std::move(parts_json)));
    if (m.metadata) members.emplace_back("metadata", *m.metadata);
    if (!m.extensions.empty()) members.emplace_back("extensions", detail::strings_to_json(m.extensions));
    if (!m.reference_task_ids.empty())
        members.emplace_back("referenceTaskIds", detail::strings_to_json(m.reference_task_ids));
    return json::Value::make_object(std::move(members));
}

[[nodiscard]] inline result<Message> message_from_json(json::Value const& v) {
    if (!v.is_object()) {
        return std::unexpected(
            error{failure_class::contract, "Message must be a JSON object", "a2a.malformed_message"});
    }
    json::Value const* id_field = v.find("messageId");
    if (!id_field || !id_field->is_string()) {
        return std::unexpected(error{failure_class::contract, "\"messageId\" must be a string",
                                      "a2a.malformed_message"});
    }
    json::Value const* role_field = v.find("role");
    if (!role_field || !role_field->is_string()) {
        return std::unexpected(
            error{failure_class::contract, "\"role\" must be a string", "a2a.malformed_message"});
    }
    auto role_val = a2a_role_from_wire_string(role_field->as_string());
    if (!role_val) return std::unexpected(role_val.error());

    json::Value const* parts_field = v.find("parts");
    if (!parts_field || !parts_field->is_array() || parts_field->as_array().empty()) {
        return std::unexpected(error{failure_class::contract, "\"parts\" must be a non-empty array",
                                      "a2a.malformed_message"});
    }
    Message m;
    m.message_id = id_field->as_string();
    m.role       = *role_val;
    for (json::Value const& item : parts_field->as_array()) {
        auto p = part_from_json(item);
        if (!p) return std::unexpected(p.error());
        m.parts.push_back(std::move(*p));
    }
    if (auto const* cid = v.find("contextId"); cid && cid->is_string()) m.context_id = cid->as_string();
    if (auto const* tid = v.find("taskId"); tid && tid->is_string()) m.task_id = tid->as_string();
    if (auto const* md = v.find("metadata")) m.metadata = *md;
    auto ext = detail::strings_from_json(v.find("extensions"));
    if (!ext) return std::unexpected(ext.error());
    m.extensions = std::move(*ext);
    auto refs = detail::strings_from_json(v.find("referenceTaskIds"));
    if (!refs) return std::unexpected(refs.error());
    m.reference_task_ids = std::move(*refs);
    return m;
}

[[nodiscard]] inline json::Value to_json(Artifact const& a) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("artifactId", json::Value::make_string(a.artifact_id));
    if (a.name) members.emplace_back("name", json::Value::make_string(*a.name));
    if (a.description) members.emplace_back("description", json::Value::make_string(*a.description));
    std::vector<json::Value> parts_json;
    parts_json.reserve(a.parts.size());
    for (Part const& p : a.parts) parts_json.push_back(to_json(p));
    members.emplace_back("parts", json::Value::make_array(std::move(parts_json)));
    if (a.metadata) members.emplace_back("metadata", *a.metadata);
    if (!a.extensions.empty()) members.emplace_back("extensions", detail::strings_to_json(a.extensions));
    return json::Value::make_object(std::move(members));
}

[[nodiscard]] inline result<Artifact> artifact_from_json(json::Value const& v) {
    if (!v.is_object()) {
        return std::unexpected(
            error{failure_class::contract, "Artifact must be a JSON object", "a2a.malformed_artifact"});
    }
    json::Value const* id_field = v.find("artifactId");
    if (!id_field || !id_field->is_string()) {
        return std::unexpected(error{failure_class::contract, "\"artifactId\" must be a string",
                                      "a2a.malformed_artifact"});
    }
    json::Value const* parts_field = v.find("parts");
    if (!parts_field || !parts_field->is_array() || parts_field->as_array().empty()) {
        return std::unexpected(error{failure_class::contract, "\"parts\" must be a non-empty array",
                                      "a2a.malformed_artifact"});
    }
    Artifact a;
    a.artifact_id = id_field->as_string();
    for (json::Value const& item : parts_field->as_array()) {
        auto p = part_from_json(item);
        if (!p) return std::unexpected(p.error());
        a.parts.push_back(std::move(*p));
    }
    if (auto const* n = v.find("name"); n && n->is_string()) a.name = n->as_string();
    if (auto const* d = v.find("description"); d && d->is_string()) a.description = d->as_string();
    if (auto const* md = v.find("metadata")) a.metadata = *md;
    auto ext = detail::strings_from_json(v.find("extensions"));
    if (!ext) return std::unexpected(ext.error());
    a.extensions = std::move(*ext);
    return a;
}

[[nodiscard]] inline json::Value to_json(TaskStatus const& s) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("state", json::Value::make_string(std::string(to_wire_string(s.state))));
    if (s.message) members.emplace_back("message", to_json(*s.message));
    if (s.timestamp) members.emplace_back("timestamp", json::Value::make_string(*s.timestamp));
    return json::Value::make_object(std::move(members));
}

[[nodiscard]] inline result<TaskStatus> task_status_from_json(json::Value const& v) {
    if (!v.is_object()) {
        return std::unexpected(error{failure_class::contract, "TaskStatus must be a JSON object",
                                      "a2a.malformed_task_status"});
    }
    json::Value const* state_field = v.find("state");
    if (!state_field || !state_field->is_string()) {
        return std::unexpected(
            error{failure_class::contract, "\"state\" must be a string", "a2a.malformed_task_status"});
    }
    auto state_val = task_state_from_wire_string(state_field->as_string());
    if (!state_val) return std::unexpected(state_val.error());
    TaskStatus s;
    s.state = *state_val;
    if (auto const* msg = v.find("message")) {
        auto parsed = message_from_json(*msg);
        if (!parsed) return std::unexpected(parsed.error());
        s.message = std::move(*parsed);
    }
    if (auto const* ts = v.find("timestamp"); ts && ts->is_string()) s.timestamp = ts->as_string();
    return s;
}

[[nodiscard]] inline json::Value to_json(Task const& t) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("id", json::Value::make_string(t.id));
    if (t.context_id) members.emplace_back("contextId", json::Value::make_string(*t.context_id));
    members.emplace_back("status", to_json(t.status));
    if (!t.artifacts.empty()) {
        std::vector<json::Value> arr;
        arr.reserve(t.artifacts.size());
        for (Artifact const& a : t.artifacts) arr.push_back(to_json(a));
        members.emplace_back("artifacts", json::Value::make_array(std::move(arr)));
    }
    if (!t.history.empty()) {
        std::vector<json::Value> arr;
        arr.reserve(t.history.size());
        for (Message const& m : t.history) arr.push_back(to_json(m));
        members.emplace_back("history", json::Value::make_array(std::move(arr)));
    }
    if (t.metadata) members.emplace_back("metadata", *t.metadata);
    return json::Value::make_object(std::move(members));
}

[[nodiscard]] inline result<Task> task_from_json(json::Value const& v) {
    if (!v.is_object()) {
        return std::unexpected(
            error{failure_class::contract, "Task must be a JSON object", "a2a.malformed_task"});
    }
    json::Value const* id_field = v.find("id");
    if (!id_field || !id_field->is_string()) {
        return std::unexpected(
            error{failure_class::contract, "\"id\" must be a string", "a2a.malformed_task"});
    }
    json::Value const* status_field = v.find("status");
    if (!status_field) {
        return std::unexpected(
            error{failure_class::contract, "\"status\" is required", "a2a.malformed_task"});
    }
    auto status_val = task_status_from_json(*status_field);
    if (!status_val) return std::unexpected(status_val.error());

    Task t;
    t.id     = id_field->as_string();
    t.status = std::move(*status_val);
    if (auto const* cid = v.find("contextId"); cid && cid->is_string()) t.context_id = cid->as_string();
    if (auto const* arts = v.find("artifacts"); arts && arts->is_array()) {
        for (json::Value const& item : arts->as_array()) {
            auto a = artifact_from_json(item);
            if (!a) return std::unexpected(a.error());
            t.artifacts.push_back(std::move(*a));
        }
    }
    if (auto const* hist = v.find("history"); hist && hist->is_array()) {
        for (json::Value const& item : hist->as_array()) {
            auto m = message_from_json(item);
            if (!m) return std::unexpected(m.error());
            t.history.push_back(std::move(*m));
        }
    }
    if (auto const* md = v.find("metadata")) t.metadata = *md;
    return t;
}

}  // namespace agentengine::a2a
