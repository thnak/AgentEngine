#pragma once
// Implements 003-Message-and-Content-Model.md — one content model shared by the agent core, every
// ChatClient, and every protocol surface. Terminology (027 §7): this is `Content`, not `Part` —
// `Part` is A2A's word and stays only in `agentengine::a2a` mapping code.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace agentengine {

enum class content_origin { user, assistant, tool, system, external };

// Taint is a type-level marker (003 §2), not a bit someone remembers to check. `TaintedText` is
// the accessor that does not implicitly convert to the plain `std::string_view` capability APIs
// take; the conversion itself is the security-critical primitive and is not sketched here (same
// deferral as CapabilitySet — this is vocabulary, the mechanism needs its own review).
class TaintedText {
public:
    explicit TaintedText(std::string value) : value_(std::move(value)) {}
    [[nodiscard]] std::string const& unsafe_view() const noexcept { return value_; }

private:
    std::string value_;
};

struct Text {
    std::string text;  // tainted iff `origin` on the enclosing Content says so
};

struct Reasoning {
    std::string text;
    bool        encrypted = false;  // opaque pass-through only when true (003 §1)
};

// Bytes above a threshold move out-of-line; see BlobRef below (003 §3).
struct BlobRef {
    std::string digest;
    std::string media_type;
    std::size_t size = 0;
    std::string store;  // which blob store seam (019) resolves this digest
};

struct Media {
    std::variant<std::vector<std::byte>, std::string /*uri*/, BlobRef> payload;
    std::string                                                        media_type;
};

struct Data {
    std::string json;  // structured value, serialized; schema id optional
    std::optional<std::string> schema_id;
};

struct ToolCall {
    std::string call_id;
    std::string tool_name;
    std::string arguments_json;
    content_origin origin = content_origin::assistant;
};

struct ContentItem;  // fwd — ToolResult carries content items recursively

struct ToolResult {
    std::string                    call_id;
    std::vector<ContentItem>       content;
    bool                            is_error = false;
};

struct Citation {
    std::string source;
    std::size_t span_start = 0;
    std::size_t span_end   = 0;
};

struct Error {
    std::string message;
};

struct Custom {
    std::string type_id;  // namespaced, uri-shaped (003 §1)
    std::string payload_json;
};

// One element of a message (003 §1). Order is meaningful; unknown kinds round-trip via Custom.
struct ContentItem {
    std::variant<Text, Reasoning, Media, Data, ToolCall, ToolResult, Citation, Error, Custom> value;
    content_origin origin = content_origin::assistant;
    bool           tainted = false;
};

enum class role { system, user, assistant, tool };

struct Usage {
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
    std::uint64_t cached_input_tokens = 0;
    std::uint64_t reasoning_tokens = 0;
    double        cost_estimate = 0.0;
};

struct Message {
    agentengine::role        role;
    std::vector<ContentItem> content;
    std::string              message_id;
};

} // namespace agentengine
