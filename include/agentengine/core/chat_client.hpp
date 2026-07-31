#pragma once
// Implements 004-Model-Provider-Plane.md — the seam to every inference API. Terminology (027 §5,
// §7): `ChatClient`, not `Provider` — "provider" stays free for the colloquial vendor sense.

#include <cstdint>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"

namespace agentengine {

// Declared, not probed (004 §2). One bit per capability the degradation rule can act on.
struct ChatClientCapabilities {
    bool streaming = false;
    bool tool_calling = false;
    bool parallel_tool_calls = false;
    bool structured_output_native = false;
    bool json_mode = false;
    bool reasoning = false;
    bool reasoning_encrypted = false;
    bool prompt_caching = false;
    bool multimodal_in_image = false;
    bool multimodal_in_audio = false;
    bool multimodal_in_video = false;
    bool multimodal_in_file = false;
    bool multimodal_out = false;
    bool logprobs = false;
    std::uint64_t context_window = 0;
    std::uint64_t max_output_tokens = 0;
};

struct ChatRequest {
    std::vector<Message> messages;
    // tool declarations, structured-output schema, sampling parameters: 006/003 §4, elided here —
    // vocabulary only.
};

struct ChatResponse {
    Message message;
    Usage   usage;
};

struct ChatResponseUpdate {
    ContentItem delta;
    bool        is_final = false;
};

// concept, not a base class (004 §1) — a backend satisfies this shape; it is never inherited from
// on the hot path (CONVENTIONS.md: "no virtual for policy on the hot path").
template <class T>
concept ChatClient = requires(T client, ChatRequest request, EffectContext& ctx) {
    { client.capabilities() } -> std::same_as<ChatClientCapabilities>;
    { client.chat(request, ctx) };        // ae::task<result<ChatResponse>> — Quark coroutine type
    { client.chat_stream(request, ctx) }; // ae::stream<ChatResponseUpdate>
};

} // namespace agentengine
