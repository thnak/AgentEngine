#pragma once
// Implements 003-Message-and-Content-Model.md — one content model shared by the agent core, every
// ChatClient, and every protocol surface. Terminology (027 §7): this is `Content`, not `Part` —
// `Part` is A2A's word and stays only in `agentengine::a2a` mapping code.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "agentengine/core/tainted.hpp"

namespace agentengine {

enum class content_origin { user, assistant, tool, system, external };  // ae-naming-lint: allow content_origin — pre-existing M0 scaffolding, reconcile at owning milestone

// `TaintedText` is `Tainted<std::string>` specialized for the text/bytes case (003 §2: "not a
// separate mechanism") — the mechanism itself lives in tainted.hpp, not reimplemented here.
using TaintedText = Tainted<std::string>;  // ae-naming-lint: allow TaintedText — pre-existing M0 scaffolding, reconcile at owning milestone

struct Text {  // ae-naming-lint: allow Text — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string text;  // tainted iff `origin` on the enclosing Content says so

    friend bool operator==(Text const&, Text const&) = default;
};

struct Reasoning {  // ae-naming-lint: allow Reasoning — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string text;
    bool        encrypted = false;  // opaque pass-through only when true (003 §1)
    // Gap-audit finding 20 / 003 §8 Q2 ("exclude from context assembly, never translate"): which
    // backend actually produced this reasoning trace, "vendor:model" (the same runtime string shape
    // `ChatClientId<"vendor:model">` and `ChatClientRegistry::register_client` already use). Stamped
    // by the producing backend's own response parser; empty means "unknown provenance" (any record
    // written before this field existed, or a `text_derived` leak-scan extraction whose whole
    // trustworthiness is already in question — 003 §8 Q2's own "only when it originated from the
    // ChatClientId currently bound" rule treats empty the same as any other non-matching id: excluded,
    // never assumed safe). Appended last (003 §6's field-ordering lesson — see `Usage::
    // cache_write_tokens`'s own comment for why), so every existing positional `Reasoning{text,
    // encrypted}` call site keeps compiling unchanged.
    std::string producer_chat_client_id;

    friend bool operator==(Reasoning const&, Reasoning const&) = default;
};

// Bytes above a threshold move out-of-line; see BlobRef below (003 §3).
struct BlobRef {  // ae-naming-lint: allow BlobRef — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string digest;
    std::string media_type;
    std::size_t size = 0;
    std::string store;  // which blob store seam (019) resolves this digest

    friend bool operator==(BlobRef const&, BlobRef const&) = default;
};

struct Media {  // ae-naming-lint: allow Media — pre-existing M0 scaffolding, reconcile at owning milestone
    std::variant<std::vector<std::byte>, std::string /*uri*/, BlobRef> payload;
    std::string                                                        media_type;

    friend bool operator==(Media const&, Media const&) = default;
};

struct Data {  // ae-naming-lint: allow Data — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string json;  // structured value, serialized; schema id optional
    std::optional<std::string> schema_id;

    friend bool operator==(Data const&, Data const&) = default;
};

// decisions/ADR-023-response-format-codec-seam.md §6 point 4 / 007 §4 amendment: WHERE a call came
// from is load-bearing, not decorative. `vendor_structured` is today's only path -- a backend's own
// wire-format `tool_calls`/`tool_use` field, unchanged trust posture. `text_derived` means
// `apply_response_format_scan()` (core/response_format_leak_scan.hpp; ADR-035 Phase 1 made this
// backend-agnostic -- armed via `OpenAIChatClient::scan_response_format_leaks` for that backend
// directly, or via `AgentSession::set_scan_response_format_leaks()` for any backend/path) pattern-
// matched this call out of free-text `content` that leaked raw response-format tokens (Harmony/
// DeepSeek/Hermes/etc) -- model-supplied text, re-parsed heuristically, never itself an authorization
// decision (007 §4: "no policy-deciding API accepts a tainted value"). `core/tool_pipeline.hpp`'s
// `ToolCallRequest` carries the same enum for exactly this reason -- see its own field comment and
// `invoke_tool`'s step 5, the ONE place this value is ever consulted for an approval decision.
enum class call_provenance { vendor_structured, text_derived };  // ae-naming-lint: allow call_provenance — 007 §4 amendment names this concept normatively; 027 has not been updated to list it

struct ToolCall {  // ae-naming-lint: allow ToolCall — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string call_id;
    std::string tool_name;
    std::string arguments_json;
    content_origin origin = content_origin::assistant;
    // Appended last (003 §6's field-ordering lesson) -- defaults to `vendor_structured` so every
    // existing positional `ToolCall{a,b,c}`/`{a,b,c,d}` call site is unaffected.
    call_provenance provenance = call_provenance::vendor_structured;

    friend bool operator==(ToolCall const&, ToolCall const&) = default;
};

struct ContentItem;  // fwd — ToolResult carries content items recursively  // ae-naming-lint: allow ContentItem — pre-existing M0 scaffolding, reconcile at owning milestone

struct ToolResult {  // ae-naming-lint: allow ToolResult — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string                    call_id;
    std::vector<ContentItem>       content;
    bool                            is_error = false;

    friend bool operator==(ToolResult const&, ToolResult const&) = default;
};

struct Citation {  // ae-naming-lint: allow Citation — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string source;
    std::size_t span_start = 0;
    std::size_t span_end   = 0;

    friend bool operator==(Citation const&, Citation const&) = default;
};

struct Error {  // ae-naming-lint: allow Error — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string message;

    friend bool operator==(Error const&, Error const&) = default;
};

struct Custom {  // ae-naming-lint: allow Custom — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string type_id;  // namespaced, uri-shaped (003 §1)
    std::string payload_json;

    friend bool operator==(Custom const&, Custom const&) = default;
};

// One element of a message (003 §1). Order is meaningful; unknown kinds round-trip via Custom.
struct ContentItem {  // ae-naming-lint: allow ContentItem — pre-existing M0 scaffolding, reconcile at owning milestone
    std::variant<Text, Reasoning, Media, Data, ToolCall, ToolResult, Citation, Error, Custom> value;
    content_origin origin = content_origin::assistant;
    bool           tainted = false;

    friend bool operator==(ContentItem const&, ContentItem const&) = default;
};

enum class role { system, user, assistant, tool };  // ae-naming-lint: allow role — pre-existing M0 scaffolding, reconcile at owning milestone

struct Usage {  // ae-naming-lint: allow Usage — pre-existing M0 scaffolding, reconcile at owning milestone
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
    std::uint64_t cached_input_tokens = 0;
    std::uint64_t reasoning_tokens = 0;
    double        cost_estimate = 0.0;
    // 003 §6 amendment (2026-08-07): the symmetric counterpart to cached_input_tokens (a cache READ) --
    // tokens spent establishing a new cache entry (Anthropic's cache_creation_input_tokens, OpenRouter's
    // usage.prompt_tokens_details.cache_write_tokens). See docs/research/2026-08-07-provider-metadata-
    // and-sampling-params-survey.md Finding 5. Appended LAST, not inserted among the original five
    // fields: several existing call sites positionally aggregate-initialize `Usage{a,b,c,d,e}` (5
    // values) -- inserting a field earlier silently shifts every value after it onto the wrong member
    // (confirmed the hard way: a positional `Usage{1,1,0,0,0.0}` narrowed a `double` into this field's
    // `uint64_t` slot and failed to compile once tried mid-struct). Appending preserves every existing
    // 5-value call site exactly as originally written, with this field defaulting to 0.
    std::uint64_t cache_write_tokens = 0;
};

struct Message {
    agentengine::role        role;
    std::vector<ContentItem> content;
    std::string              message_id;

    friend bool operator==(Message const&, Message const&) = default;
};

} // namespace agentengine
