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
};

struct Reasoning {  // ae-naming-lint: allow Reasoning — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string text;
    bool        encrypted = false;  // opaque pass-through only when true (003 §1)
};

// Bytes above a threshold move out-of-line; see BlobRef below (003 §3).
struct BlobRef {  // ae-naming-lint: allow BlobRef — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string digest;
    std::string media_type;
    std::size_t size = 0;
    std::string store;  // which blob store seam (019) resolves this digest
};

struct Media {  // ae-naming-lint: allow Media — pre-existing M0 scaffolding, reconcile at owning milestone
    std::variant<std::vector<std::byte>, std::string /*uri*/, BlobRef> payload;
    std::string                                                        media_type;
};

struct Data {  // ae-naming-lint: allow Data — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string json;  // structured value, serialized; schema id optional
    std::optional<std::string> schema_id;
};

struct ToolCall {  // ae-naming-lint: allow ToolCall — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string call_id;
    std::string tool_name;
    std::string arguments_json;
    content_origin origin = content_origin::assistant;
};

struct ContentItem;  // fwd — ToolResult carries content items recursively  // ae-naming-lint: allow ContentItem — pre-existing M0 scaffolding, reconcile at owning milestone

struct ToolResult {  // ae-naming-lint: allow ToolResult — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string                    call_id;
    std::vector<ContentItem>       content;
    bool                            is_error = false;
};

struct Citation {  // ae-naming-lint: allow Citation — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string source;
    std::size_t span_start = 0;
    std::size_t span_end   = 0;
};

struct Error {  // ae-naming-lint: allow Error — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string message;
};

struct Custom {  // ae-naming-lint: allow Custom — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string type_id;  // namespaced, uri-shaped (003 §1)
    std::string payload_json;
};

// One element of a message (003 §1). Order is meaningful; unknown kinds round-trip via Custom.
struct ContentItem {  // ae-naming-lint: allow ContentItem — pre-existing M0 scaffolding, reconcile at owning milestone
    std::variant<Text, Reasoning, Media, Data, ToolCall, ToolResult, Citation, Error, Custom> value;
    content_origin origin = content_origin::assistant;
    bool           tainted = false;
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
};

} // namespace agentengine
