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

// 004 §2's 2026-08-07 amendment (ADR-020). The ONE sampling-adjacent knob carved out of §1's
// elision, because it is the only one with a defensible portable shape: an ORDINAL LEVEL each
// backend maps down to its own native mechanism, never a vendor field passed through.
//
// `minimal` (OpenAI's fifth level) is deliberately absent — Ollama has no equivalent, so admitting
// it would make this OpenAI's enum with a rename, exactly the "one vendor's shape leaking onto every
// other backend" trap 004 §8 Q2 already avoided for prompt caching. Its absence is what makes this
// an abstraction rather than a pass-through. `off` is a real request ("disable reasoning"), distinct
// from ChatRequest's `nullopt` ("no opinion — send no field, take the vendor default"); every
// surveyed backend can express both.
enum class reasoning_effort { off, low, medium, high };  // ae-naming-lint: allow reasoning_effort — 004 §2's amendment names this concept normatively; 027 has not been updated to list it

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
    // Milestone 5 Phase F1 (004 §4: "a retried call carries a stable idempotency key so a provider
    // that supports it does not double-charge or double-execute"). Set once by a retrying wrapper
    // (`ResilientChatClient`, core/resilient_chat_client.hpp) before its first attempt and reused
    // verbatim across every retry of the SAME logical call -- never regenerated per attempt, which is
    // what makes it stable. A plain, backend-agnostic field rather than an HTTP header baked in here:
    // neither locally vendored SDK (openai-dotnet, anthropic-sdk-csharp) documents a client-supplied
    // idempotency header for Chat Completions/Messages as of this research (checked directly in
    // source, not assumed), so no backend wires this into a request header yet -- it exists so one
    // can, without another field-ordering migration, the moment a verified provider mechanism exists.
    std::optional<std::string> idempotency_key;
    // 004 §2's 2026-08-07 amendment (ADR-020). APPENDED LAST, never inserted earlier -- the same
    // field-ordering discipline `Usage::cache_write_tokens` learned the hard way (003 §6's own dated
    // amendment): inserting a field mid-struct silently breaks every positional aggregate
    // initialization of it. `nullopt` (the default) means "send no reasoning field at all", so every
    // pre-existing caller and both backends behave bit-for-bit as before.
    // (Qualified: the member name deliberately matches the enum's, so the type must be spelled with
    // its namespace here -- the member would otherwise hide the type inside the class scope.)
    std::optional<agentengine::reasoning_effort> reasoning_effort;
};

struct ChatResponse {
    Message message;
    Usage   usage;
    // Milestone 5 research follow-up (docs/research/2026-08-07-provider-metadata-and-sampling-params-
    // survey.md Finding 4): the model that ACTUALLY answered -- both OpenAI's and Anthropic's own wire
    // responses report this (confirmed directly against a real live call: an `openrouter/auto` request
    // came back "model":"google/gemini-3.5-flash-lite"), and it's invisible to the caller today. Matters
    // whenever routing isn't pinned to one model (auto-routing, fallback lists, any gateway that can
    // silently substitute a backend) and for correlating cache-hit behavior with which concrete backend
    // served a call. Empty string when a backend doesn't report one (never fabricated from the request's
    // own `model` field -- that's what was ASKED for, not what answered).
    std::string model;
    // Milestone 5 Phase F3 (004 §4: "Failover... must appear in the trace and in the response
    // metadata"). 0 = the primary backend answered; N>0 = the Nth fallback in a `FailoverChatClient`'s
    // configured order answered instead, after the primary (and any earlier fallback) failed. Full
    // run-trace recording is out of scope here -- no trace sink exists yet (needs 016, M8), the same
    // scoping Phase B5 already applied to its own output-schema-strategy trace note -- this field is
    // the "response metadata" half of the gate, real and tested now.
    std::uint32_t fallback_tier = 0;
};

struct ChatResponseUpdate {  // ae-naming-lint: allow ChatResponseUpdate — pre-existing M0 scaffolding, reconcile at owning milestone
    ContentItem delta;
    bool        is_final = false;
    // ADR-034 (AgentSession's opt-in streaming turn loop). Appended last, additive (003 §6's own
    // field-ordering lesson) -- every pre-existing `ChatResponseUpdate{delta, is_final}` call site
    // keeps compiling unchanged. Populated only on the terminal update (is_final == true), and only
    // when the backend actually reported usage for this call; nullopt means "this backend/call
    // provided none" -- a caller that needs usage (AgentSession's streaming loop, 004 §5's
    // TokenBudget<N>) must treat nullopt as a hard failure, never silently as zero cost.
    std::optional<Usage> usage = std::nullopt;
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

// ADR-036: the alternate shape `AgentSession::ChatClientT` may satisfy instead of `ChatClient` above
// -- `core/model_call_gateway.hpp`'s `ModelCallGateway<Primary, Fallback...>` is the one conformer,
// deliberately NOT a `ChatClient` itself (it has no `chat()`/`chat_stream()` at all; retry, failover,
// and middleware hooks live entirely inside its single `call()`, which needs to be a real coroutine
// so a middleware hook can be `co_await`ed directly -- `chat_stream()`'s plain, non-coroutine
// signature cannot safely host that, see `model_call_gateway.hpp`'s own top comment for the full
// reasoning). `AgentSession::run_model_call()` picks between the two shapes with `if constexpr`; a
// raw single backend (the common case, every pre-ADR-036 conformer) is completely unaffected.
template <class T>
concept ModelCallGatewayLike = requires(T gateway, ChatRequest request, EffectContext& ctx) {
    { gateway.capabilities() } -> std::same_as<ChatClientCapabilities>;
    { gateway.call(request, ctx) } -> std::same_as<task<result<ChatResponse>>>;
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
