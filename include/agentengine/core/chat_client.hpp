#pragma once
// Implements 004-Model-Provider-Plane.md — the seam to every inference API. Terminology (027 §5,
// §7): `ChatClient`, not `Provider` — "provider" stays free for the colloquial vendor sense.
//
// Milestone 5 Phase B1/B2 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md):
// un-elides ChatRequest.tools/output_schema_json (006's real ToolDescriptor, core/tool_pipeline.hpp
// — there is no second, provider-facing declaration shape, the same finding
// core/context_provider.hpp's own top comment already made for ContextContribution.tools) and
// completes ChatClientCapabilities to 004 §2's full declared bitset. Sampling parameters
// (temperature, top_p, ...) stay elided — no RFC section this project has built against names a
// concrete shape for them yet, and no M5 gate needs one; naming this here rather than silently
// dropping it, matching this project's own "narrower than the RFC's full text, not silently
// assumed complete" discipline.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/core/tool_pipeline.hpp"

namespace agentengine {

// Declared, not probed (004 §2). One bit/field per capability the degradation rule (§2) can act on
// — the full bitset, `stop_sequences`/`seed`/`token_counting`/`batch` added in Phase B2 (the M0
// scaffolding version had only 14 of the 18 named).
struct ChatClientCapabilities {  // ae-naming-lint: allow ChatClientCapabilities — pre-existing M0 scaffolding, reconcile at owning milestone
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
    bool stop_sequences = false;
    bool seed = false;
    bool token_counting = false;
    bool batch = false;
    std::uint64_t context_window = 0;
    std::uint64_t max_output_tokens = 0;
};

struct ChatRequest {  // ae-naming-lint: allow ChatRequest — pre-existing M0 scaffolding, reconcile at owning milestone
    std::vector<Message> messages;
    // 006's real per-run tool table entry — the exact type ContextContribution.tools already reuses
    // (core/context_provider.hpp's own top comment), not a second, provider-facing declaration shape.
    std::vector<ToolDescriptor> tools;
    // 003 §4's OutputSchema<T> contract, compiled to its JSON Schema text (schema::json_schema_of<T>,
    // the same mechanism ToolDescriptor::args_schema_json already uses) — nullopt when the agent
    // declared no OutputSchema<T>. Which of §4's three enforcement strategies (native/tool-shaped/
    // parse-and-repair) applies is a backend-side decision against ChatClientCapabilities
    // (agent_registry.hpp's select_output_schema_strategy, Phase B5), not carried here.
    std::optional<std::string> output_schema_json;
    // sampling parameters: elided (see file-top comment).
};

struct ChatResponse {
    Message message;
    Usage   usage;
};

struct ChatResponseUpdate {  // ae-naming-lint: allow ChatResponseUpdate — pre-existing M0 scaffolding, reconcile at owning milestone
    ContentItem delta;
    bool        is_final = false;
};

// concept, not a base class (004 §1) — a backend satisfies this shape; it is never inherited from
// on the hot path (CONVENTIONS.md: "no virtual for policy on the hot path").
//
// Milestone 5 Phase B4a: `chat`'s return type is `ae::task<result<ChatResponse>>` — 004 §1's literal
// signature, real now that Quark's `task<T>` for non-void `T` landed (ADR-047, submodule bumped to
// `dcb191f`). A conformer's `chat()` is therefore a coroutine (`co_return`s its `result<ChatResponse>`
// rather than `return`ing it); every caller `co_await`s it from inside its own coroutine (an async
// actor handler, or another nested `ae::task<T>`) — `quark::task<T>` has no synchronous "drive to
// completion" API by design (see `quark/core/task.hpp`'s banner comment and
// `task_value_return_test.cpp`'s own precedent), so there is no way to call `chat()` from ordinary,
// non-coroutine code and get a value back inline.
//
// Milestone 5 Phase B4b: `chat_stream`'s return type is `ae::stream<ChatResponseUpdate>` — 004 §1's
// literal signature, real now that `agentengine/core/stream.hpp` wraps Quark's already-Accepted
// `ReplyStream`/`StreamChannel` credit-controlled ring (RFC 024/ADR-018). Unlike `chat()`, this is NOT
// `ae::task<...>`-wrapped — the return itself is synchronous; a conformer that streams for real (an
// HTTP/SSE backend, Phase D/E) hands the `stream_producer<ChatResponseUpdate>` half off to whatever
// background execution context performs the read loop, and returns the `stream<ChatResponseUpdate>`
// drain handle to the caller immediately (`stream.hpp`'s own file banner has the full design rationale,
// including why `ChatResponseUpdate` — not trivially copyable — is boxed per item on the ring).
template <class T>
concept ChatClient = requires(T client, ChatRequest request, EffectContext& ctx) {
    { client.capabilities() } -> std::same_as<ChatClientCapabilities>;
    { client.chat(request, ctx) } -> std::same_as<task<result<ChatResponse>>>;
    { client.chat_stream(request, ctx) } -> std::same_as<stream<ChatResponseUpdate>>;
};

// 003 §4's three enforcement strategies, in the order the degradation rule (004 §2) prefers them.
enum class output_schema_strategy { native, tool_shaped, parse_and_repair };  // ae-naming-lint: allow output_schema_strategy — 003 §4/004 §2 name this concept normatively; 027 has not been updated to list it

// Milestone 5 Phase B5: the degradation rule itself — "when an agent needs a capability the bound
// ChatClient lacks, the engine applies a DECLARED fallback... it never invents a fallback that
// changes semantics without saying so" (004 §2). Pure function of the bound backend's declared
// capabilities, so it's the same decision for every caller, never re-derived ad hoc per call site.
// Always returns a strategy: `parse_and_repair` (003 §4's own "last resort") needs no special
// capability bit at all, so a real capability set can never fail to produce SOME fallback under
// today's three-strategy design — 004 §2's "if no fallback exists, register_agent<A>() fails at
// startup" clause is therefore honestly unreachable via this function as currently specified, named
// here rather than faked with an artificial failure case; agent_registry.hpp's
// check_output_schema_enforceable is where a future capability bit that removed even
// parse_and_repair's own precondition (freeform text output) would get a place to fail.
[[nodiscard]] inline output_schema_strategy select_output_schema_strategy(
    ChatClientCapabilities const& caps) noexcept {
    if (caps.structured_output_native) return output_schema_strategy::native;
    if (caps.tool_calling) return output_schema_strategy::tool_shaped;
    return output_schema_strategy::parse_and_repair;
}

// Milestone 5 Phase B6: the smallest useful slice of an eventual Engine-level ChatClient registry —
// agent_registry.hpp's own top comment already named the gap precisely: "needs a real ChatClient
// registry, 004 — Milestone 2 builds no such registry; there is no Engine type yet to hold one."
// Maps a declared `ChatClientId` (002 §3's compile-time string tag, in its runtime form —
// `agent_detail::chat_client_id_of<Policies...>()`) to the CAPABILITIES of whatever backend is
// actually bound to it, which is all `register_agent<A>()`'s own validation
// (`check_chat_client_credentials`/`check_output_schema_enforceable`) needs to check against — not
// a bound, callable instance itself (an eventual Engine's registry would also own that instance's
// lifetime; no `Engine` type exists yet to do so, so this stays capability-only until one does).
class ChatClientRegistry {
public:
    void register_client(std::string chat_client_id, ChatClientCapabilities caps) {
        entries_[std::move(chat_client_id)] = caps;
    }

    [[nodiscard]] std::optional<ChatClientCapabilities> find(std::string_view chat_client_id) const {
        auto it = entries_.find(std::string(chat_client_id));
        if (it == entries_.end()) return std::nullopt;
        return it->second;
    }

private:
    std::unordered_map<std::string, ChatClientCapabilities> entries_;
};

} // namespace agentengine
