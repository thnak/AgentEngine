#pragma once
// Implements 003-Message-and-Content-Model.md's durable projection -- closing the "Message/
// ContentItem has no QUARK_SERIALIZE" gap named at agent_session.hpp:154-158/207-213 (Milestone 4
// Phase A4/D1) and interaction.hpp's own header comment. Milestone 6 Phase F
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md) needs this for real: 014 §5's
// "resume restores exactly" means a workflow checkpoint must carry its actual pending/partial
// Message payloads, not just round numbers and Interaction bookkeeping -- unlike a session's
// history (a nice-to-have A4 could defer), a workflow's `pending`/`partial` payloads ARE its state.
//
// WHY A SEPARATE FLAT RECORD TYPE, NOT `QUARK_SERIALIZE` DIRECTLY ON `ContentItem`/`Media`:
// `ContentItem::value` is a 9-way `std::variant`, and `Media::payload` is a nested 3-way one.
// Quark's wire codec (quark/core/describe.hpp, wire.hpp) has NO variant primitive at all -- every
// field's wire type is "a pure function of its static type, never its value" (describe.hpp's own
// words). Two designs were considered and rejected before this one, both real bugs found by
// reading describe.hpp/wire.hpp in full, not style preferences:
//   1. A hand-written `quark_describe(Ar&, ContentItem&)` that branches on `v.value.index()` and
//      only calls `ar.field()` for the active arm's tag. `FingerprintFolder` (describe.hpp)
//      computes a type's fingerprint by calling `quark_describe` ONCE on a single
//      DEFAULT-CONSTRUCTED sample (describe.hpp:264-270) -- a default `ContentItem{}` always holds
//      arm 0 (`Text`), so the folder could only ever see that one branch. Two describers that
//      disagree on how they encode `Media`/`ToolCall`/etc. would fold to the SAME fingerprint,
//      silently defeating "distinct fingerprint on schema drift" (016, and `third_party/quark/
//      OpenQuestions.md`'s existing item 2) -- a new, undetected variant of that same gap.
//   2. The same branching body ALSO cannot safely run on the write/size passes: `tagged_write_object`/
//      `tagged_object_size` take the object by `const&` and `const_cast` it away on the (unenforced)
//      convention that `quark_describe` only reads during those passes (wire.hpp:106,177 comments).
//      A body that reassigns `v.value` -- even to an equal value -- on every pass, including write,
//      is undefined behaviour the moment a caller's real object is genuinely `const`.
//   3. (Ruled out before either of the above: `tagged_read_object` re-invokes the ENTIRE
//      `quark_describe` body once per wire KEY, on the same persistent output object -- see
//      wire.hpp:288-304. A conditional body's "self-healing across calls" trick was traced through
//      by hand and found to corrupt state across the multi-key sequence a real `ContentItem`
//      produces; not reproduced here since design (this file's) sidesteps the whole problem.)
//
// So `ContentItemRecord`/`MediaRecord`/`DataRecord`/`ToolResultRecord`/`MessageRecord` are FLAT
// structs: one always-present, always-described field per variant arm (materialised from whichever
// is active, defaulted otherwise), plus a discriminant. `QUARK_SERIALIZE` generates a strictly
// linear field-list body with zero branching for every one of them -- the same shape every other
// durable record in this codebase already uses (`AgentSessionRecord`, `Interaction`, `RefMoved`),
// so the fingerprint is deterministic and neither pass ever mutates its input. `to_record()`/
// `from_record()` do the variant<->flat mapping in ordinary, non-`quark_describe` C++, mirroring
// `AgentSession::to_record()`/`restore_from_record()`'s own precedent (agent_session.hpp:613-648)
// of keeping the live type and its durable shape crossed in exactly one place.
//
// `Text`/`Reasoning`/`BlobRef`/`ToolCall`/`Citation`/`Error`/`Custom` (content.hpp) have no variant
// of their own, so they are `QUARK_SERIALIZE`'d DIRECTLY -- the macro's generated `quark_describe`
// is found by ADL via the type's own namespace (`agentengine`), regardless of which header the
// macro invocation is textually written in, so this does not require adding a Quark include to
// content.hpp, which stays the protocol-model leaf header 003 describes, decoupled from Quark's
// serialization macro. `Data::schema_id` is the one exception: it is `std::optional<std::string>`,
// and `std::optional<T>` has no `Described` support either (same "not in the closed if-constexpr
// chain" gap variants have) -- `DataRecord` follows this project's own existing sentinel
// convention for "no value here" (`Interaction::expires_at_ns == 0`, interaction.hpp) rather than
// inventing a generic `optional<T>` describer for this one field: an empty `schema_id` string
// means "absent", collapsing the (rare, narrow) case of an explicitly-empty-but-present schema id
// into "absent" too -- named here, not silently assumed lossless.
//
// COST: every `ContentItem`'s durable form carries eight empty sibling arms alongside its one real
// one (and every `Media`'s form carries two empty sibling payload kinds). Each empty arm's own
// fields are cheap defaults (a couple of TLV bytes each), and vectors are cheap-empty (no heap
// allocation) -- accepted deliberately as the price of a fingerprint that is a pure function of
// static type, per this file's whole reason for existing above.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "quark/core/describe.hpp"

#include "agentengine/core/content.hpp"

namespace agentengine {

// ---- Arms with no variant of their own: QUARK_SERIALIZE'd directly on the live content.hpp type.

QUARK_SERIALIZE(Text, (1, text))
QUARK_SERIALIZE(Reasoning, (1, text), (2, encrypted))
QUARK_SERIALIZE(BlobRef, (1, digest), (2, media_type), (3, size), (4, store))
QUARK_SERIALIZE(ToolCall, (1, call_id), (2, tool_name), (3, arguments_json), (4, origin))
QUARK_SERIALIZE(Citation, (1, source), (2, span_start), (3, span_end))
QUARK_SERIALIZE(Error, (1, message))
QUARK_SERIALIZE(Custom, (1, type_id), (2, payload_json))

// ---- `Data`: flattens `optional<string> schema_id` to an empty-means-absent string (see header).

struct DataRecord {  // ae-naming-lint: allow DataRecord — this file's own durable-record family, mirrors AgentSessionRecord's naming
    std::string json;
    std::string schema_id;  // "" means Data::schema_id was std::nullopt

    friend bool operator==(DataRecord const&, DataRecord const&) = default;
};
QUARK_SERIALIZE(DataRecord, (1, json), (2, schema_id))

[[nodiscard]] inline DataRecord to_record(Data const& d) {
    return DataRecord{d.json, d.schema_id.value_or(std::string{})};
}
[[nodiscard]] inline Data from_record(DataRecord const& r) {
    Data d;
    d.json = r.json;
    d.schema_id = r.schema_id.empty() ? std::nullopt : std::optional<std::string>{r.schema_id};
    return d;
}

// ---- `Media`: its own nested 3-way variant<vector<byte>, string, BlobRef> gets the same flat
// treatment as `ContentItem` below, one level in.

struct MediaRecord {  // ae-naming-lint: allow MediaRecord — this file's own durable-record family, mirrors AgentSessionRecord's naming
    std::uint8_t           payload_kind = 0;  // Media::payload's variant index: 0=bytes,1=uri,2=blob
    std::vector<std::byte> payload_bytes;
    std::string            payload_uri;
    BlobRef                payload_blob;
    std::string            media_type;

    friend bool operator==(MediaRecord const&, MediaRecord const&) = default;
};
QUARK_SERIALIZE(MediaRecord, (1, payload_kind), (2, payload_bytes), (3, payload_uri),
                (4, payload_blob), (5, media_type))

[[nodiscard]] inline MediaRecord to_record(Media const& m) {
    MediaRecord r;
    r.payload_kind = static_cast<std::uint8_t>(m.payload.index());
    if (auto const* bytes = std::get_if<std::vector<std::byte>>(&m.payload)) r.payload_bytes = *bytes;
    if (auto const* uri = std::get_if<std::string>(&m.payload)) r.payload_uri = *uri;
    if (auto const* blob = std::get_if<BlobRef>(&m.payload)) r.payload_blob = *blob;
    r.media_type = m.media_type;
    return r;
}
[[nodiscard]] inline Media from_record(MediaRecord const& r) {
    Media m;
    switch (r.payload_kind) {
        case 0: m.payload = r.payload_bytes; break;
        case 1: m.payload = r.payload_uri; break;
        case 2: m.payload = r.payload_blob; break;
        default: m.payload = std::vector<std::byte>{}; break;  // unreachable for a value this
                                                                 // file's own to_record() produced;
                                                                 // fails closed to arm 0 rather than
                                                                 // leaving `payload` uninitialised
                                                                 // for a hand-corrupted record
    }
    m.media_type = r.media_type;
    return m;
}

// ---- `ContentItem`/`ToolResult`: mutually recursive (`ToolResult.content` is
// `vector<ContentItem>`), so `ContentItemRecord` and `ToolResultRecord` are forward-declared
// together, matching content.hpp's own `struct ContentItem;` forward declaration ahead of
// `ToolResult` (content.hpp:56).

struct ContentItemRecord;  // ae-naming-lint: allow ContentItemRecord — this file's own durable-record family, mirrors AgentSessionRecord's naming

struct ToolResultRecord {  // ae-naming-lint: allow ToolResultRecord — this file's own durable-record family, mirrors AgentSessionRecord's naming
    std::string                    call_id;
    std::vector<ContentItemRecord> content;
    bool                            is_error = false;

    friend bool operator==(ToolResultRecord const&, ToolResultRecord const&) = default;
};

struct ContentItemRecord {  // ae-naming-lint: allow ContentItemRecord — this file's own durable-record family, mirrors AgentSessionRecord's naming
    // ContentItem::value's variant index: 0=Text,1=Reasoning,2=Media,3=Data,4=ToolCall,
    // 5=ToolResult,6=Citation,7=Error,8=Custom -- must track content.hpp's own declared order
    // (content.hpp:81) exactly, since `to_record()`/`from_record()` below index by it directly.
    std::uint8_t kind = 0;
    Text         text;
    Reasoning    reasoning;
    MediaRecord  media;
    DataRecord   data;
    ToolCall     tool_call;
    ToolResultRecord tool_result;
    Citation     citation;
    Error        error;
    Custom       custom;
    content_origin origin = content_origin::assistant;
    bool           tainted = false;

    friend bool operator==(ContentItemRecord const&, ContentItemRecord const&) = default;
};

QUARK_SERIALIZE(ToolResultRecord, (1, call_id), (2, content), (3, is_error))
QUARK_SERIALIZE(ContentItemRecord, (1, kind), (2, text), (3, reasoning), (4, media), (5, data),
                (6, tool_call), (7, tool_result), (8, citation), (9, error), (10, custom),
                (11, origin), (12, tainted))

[[nodiscard]] ToolResultRecord to_record(ToolResult const&);
[[nodiscard]] ToolResult from_record(ToolResultRecord const&);

[[nodiscard]] inline ContentItemRecord to_record(ContentItem const& c) {
    ContentItemRecord r;
    r.kind = static_cast<std::uint8_t>(c.value.index());
    if (auto const* v = std::get_if<Text>(&c.value)) r.text = *v;
    if (auto const* v = std::get_if<Reasoning>(&c.value)) r.reasoning = *v;
    if (auto const* v = std::get_if<Media>(&c.value)) r.media = to_record(*v);
    if (auto const* v = std::get_if<Data>(&c.value)) r.data = to_record(*v);
    if (auto const* v = std::get_if<ToolCall>(&c.value)) r.tool_call = *v;
    if (auto const* v = std::get_if<ToolResult>(&c.value)) r.tool_result = to_record(*v);
    if (auto const* v = std::get_if<Citation>(&c.value)) r.citation = *v;
    if (auto const* v = std::get_if<Error>(&c.value)) r.error = *v;
    if (auto const* v = std::get_if<Custom>(&c.value)) r.custom = *v;
    r.origin = c.origin;
    r.tainted = c.tainted;
    return r;
}
[[nodiscard]] inline ContentItem from_record(ContentItemRecord const& r) {
    ContentItem c;
    switch (r.kind) {
        case 0: c.value = r.text; break;
        case 1: c.value = r.reasoning; break;
        case 2: c.value = from_record(r.media); break;
        case 3: c.value = from_record(r.data); break;
        case 4: c.value = r.tool_call; break;
        case 5: c.value = from_record(r.tool_result); break;
        case 6: c.value = r.citation; break;
        case 7: c.value = r.error; break;
        case 8: c.value = r.custom; break;
        default: c.value = Text{}; break;  // fails closed to arm 0, same rationale as
                                            // MediaRecord::from_record's default case above
    }
    c.origin = r.origin;
    c.tainted = r.tainted;
    return c;
}

[[nodiscard]] inline ToolResultRecord to_record(ToolResult const& t) {
    ToolResultRecord r;
    r.call_id = t.call_id;
    r.content.reserve(t.content.size());
    for (auto const& item : t.content) r.content.push_back(to_record(item));
    r.is_error = t.is_error;
    return r;
}
[[nodiscard]] inline ToolResult from_record(ToolResultRecord const& r) {
    ToolResult t;
    t.call_id = r.call_id;
    t.content.reserve(r.content.size());
    for (auto const& item : r.content) t.content.push_back(from_record(item));
    t.is_error = r.is_error;
    return t;
}

// ---- `Message`: role/content/message_id, flat modulo `content`'s recursion into `ContentItemRecord`.

struct MessageRecord {  // ae-naming-lint: allow MessageRecord — this file's own durable-record family, mirrors AgentSessionRecord's naming
    role                            msg_role = role::user;  // `role` alone would shadow content.hpp's
                                                              // `enum class role` as a member name
    std::vector<ContentItemRecord> content;
    std::string                    message_id;

    friend bool operator==(MessageRecord const&, MessageRecord const&) = default;
};
QUARK_SERIALIZE(MessageRecord, (1, msg_role), (2, content), (3, message_id))

[[nodiscard]] inline MessageRecord to_record(Message const& m) {
    MessageRecord r;
    r.msg_role = m.role;
    r.content.reserve(m.content.size());
    for (auto const& item : m.content) r.content.push_back(to_record(item));
    r.message_id = m.message_id;
    return r;
}
[[nodiscard]] inline Message from_record(MessageRecord const& r) {
    Message m;
    m.role = r.msg_role;
    m.content.reserve(r.content.size());
    for (auto const& item : r.content) m.content.push_back(from_record(item));
    m.message_id = r.message_id;
    return m;
}

}  // namespace agentengine
