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

#include <concepts>
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
// signature (historical: originally real once Quark's `task<T>` for non-void `T` landed, ADR-047;
// `ae::task<T>` is now `agentengine::rt::task<T>` directly, ADR-037 removed Quark entirely, see
// `core/task.hpp`'s own current banner). A conformer's `chat()` is therefore a coroutine
// (`co_return`s its `result<ChatResponse>` rather than `return`ing it); every caller `co_await`s it
// from inside its own coroutine — `rt::task<T>` has no synchronous "drive to completion" API by
// design (see `rt/task.hpp`'s own banner comment and `task_value_return_test.cpp`'s own precedent),
// so there is no way to call `chat()` from ordinary, non-coroutine code and get a value back inline.
//
// Milestone 5 Phase B4b: `chat_stream`'s return type is `ae::stream<ChatResponseUpdate>` — 004 §1's
// literal signature. `agentengine/core/stream.hpp` now wraps `rt::channel<T,E>` (historical:
// originally Quark's already-Accepted `ReplyStream`/`StreamChannel` credit-controlled ring, RFC
// 024/ADR-018, before ADR-037 replaced the backend; the credit-controlled contract is unchanged).
// Unlike `chat()`, this is NOT
// `ae::task<...>`-wrapped — the return itself is synchronous; a conformer that streams for real (an
// HTTP/SSE backend, Phase D/E) hands the `stream_producer<ChatResponseUpdate>` half off to whatever
// background execution context performs the read loop, and returns the `stream<ChatResponseUpdate>`
// drain handle to the caller immediately (`stream.hpp`'s own file banner has the full design rationale,
// including why `ChatResponseUpdate` — not trivially copyable — is boxed per item on the ring).
// ADR-035 Phase 3: `chat()` is deliberately NOT part of this concept's required shape anymore --
// `chat_stream()` is the only method every conformer must have. This does NOT delete `chat()` from
// any existing conformer (every real backend -- `OpenAIChatClient`/`AnthropicChatClient` -- and
// every test/example fixture in this codebase still has one; nothing here forces removing it) --
// it only WIDENS what can satisfy `ChatClient`: a future backend implementing `chat_stream()` alone,
// with no `chat()` at all, now conforms too, where it previously would not have. Safe with zero
// behavior change for anything that exists today: every current conformer was independently
// verified genuinely streaming-capable (not a type-satisfying stub) ahead of this change, across a
// 46-file conversion pass (ADR-035 Phase 3, part 1) plus the two production call sites that still
// called `chat()` directly (`MemoryProvider::on_turn_end`, `HistoryProvider<Summarize<N,
// SummarizerT>>::on_context`, both now drain `chat_stream()` instead — Phase 3, part 2). The three
// `chat()`-only wrapper templates (`FailoverChatClient`/`ResilientChatClient`/`MiddlewareChatClient`)
// are UNAFFECTED — they still declare and use `chat()`/`chat_stream()` internally as their own
// concrete methods, unrelated to whether this concept requires either one.
template <class T>
concept ChatClient = requires(T client, ChatRequest request, EffectContext& ctx) {
    { client.capabilities() } -> std::same_as<ChatClientCapabilities>;
    { client.chat_stream(request, ctx) } -> std::same_as<stream<ChatResponseUpdate>>;
};

// The pre-Phase-3 full shape (`capabilities()` + `chat()` + `chat_stream()`), kept as its own concept
// for `RecordingChatClient` (core/recording_chat_client.hpp), the one remaining `chat()`-only wrapper
// template whose body unconditionally calls a wrapped instance's `.chat(...)` -- code review finding
// (2026-08-12): after `ChatClient` above was relaxed to drop `chat()`, `RecordingChatClient`'s own
// gating on `ChatClient<Inner>` stopped guaranteeing what its body actually needs, so a
// chat_stream()-only backend (now a legal `ChatClient` conformer) plugged into it would pass silently
// and then fail deep inside `.chat()` with an opaque "no member named chat" template error instead of
// the intended, named diagnostic. `LegacyChatClient` is what `RecordingChatClient` gates on instead --
// it is exactly what `ChatClient` used to require, given a name so it survives the relaxation as an
// explicit, checkable requirement rather than silently reading `ChatClient` and hoping every conformer
// still happens to have `chat()`. `ReplayChatClient` does NOT need this concept -- it doesn't wrap
// another instance's `.chat(...)` at all, it implements both methods itself. `FailoverChatClient`/
// `ResilientChatClient`/`MiddlewareChatClient` originally needed this concept too (and briefly used
// it, 2026-08-12) but were REMOVED the same day -- this repo had shipped nowhere, so once ADR-036's
// `ModelCallGateway`/`MiddlewareModelCallGateway` gave `AgentSession` a real, streaming-capable
// successor, there was no deprecation-then-migration cost to justify keeping the three `chat()`-only
// wrappers around.
template <class T>
concept LegacyChatClient = requires(T client, ChatRequest request, EffectContext& ctx) {
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

// Gap-audit finding 20 / 003 §8 Q2 ("a Reasoning item is included in a turn's assembled context only
// when it originated from the ChatClientId currently bound"). Optional, duck-typed detection
// (matching `agent_registry.hpp`'s own `has_agent_description`/`has_agent_version` precedent, ADR-044)
// rather than a new member added to the `ChatClient`/`ModelCallGatewayLike` concepts' required shape
// -- widening either of those would force every conformer (every mock/test fixture in this codebase)
// to grow a method it has no real identity to report, for a check most of them don't need. A
// `ChatClientT` that doesn't satisfy this concept (a mock, or a `ModelCallGateway` composition with
// no single real backend identity of its own) simply gets no cross-provider filtering at all --
// `AgentSession::run_rounds()`'s own `if constexpr` gate on this concept degrades to "unchanged
// behavior," never a compile error and never a silently-wrong filter.
template <class T>
concept HasProducerChatClientId = requires(T const& t) {
    { t.producer_chat_client_id() } -> std::convertible_to<std::string>;
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

// Gap-audit finding 19, Phase 1 (fail-closed capability gate + symmetric drop-signal — Phase 2, real
// wire encoding, is blocked on RFC 019's blob-store seam, which doesn't exist anywhere in the tree).
// Every real backend's outbound `translate_message()` silently drops `Media` content it has no wire
// shape for (confirmed directly in both `protocol/openai/chat_client.hpp` and `protocol/anthropic/
// chat_client.hpp` — the translation loop simply has no branch for `Media`, so it vanishes with no
// signal at all, even when `ChatClientCapabilities` declares the matching `multimodal_in_*` bit
// FALSE, meaning the drop was never even a real capability mismatch worth silently tolerating — it
// was always going to be dropped, unconditionally, by every backend that exists today). This function
// makes that failure LOUD instead: called once, before a request ever reaches a backend, it refuses
// to proceed at all when the request carries `Media` content the bound backend hasn't declared
// support for — the caller finds out via a real, attributable error, not a response that quietly
// answers as if it never saw the attachment.
enum class media_category { image, audio, video, file };  // ae-naming-lint: allow media_category — new gap-19 vocabulary; 027 has not been updated to list it

// MIME-type-prefix categorization — matches `ChatClientCapabilities`'s own four `multimodal_in_*`
// bits exactly. Anything not recognized as image/audio/video falls to `file`, the same catch-all
// 004 §2's own capability bitset uses for "everything else."
[[nodiscard]] inline media_category categorize_media_type(std::string_view media_type) noexcept {
    if (media_type.starts_with("image/")) return media_category::image;
    if (media_type.starts_with("audio/")) return media_category::audio;
    if (media_type.starts_with("video/")) return media_category::video;
    return media_category::file;
}

[[nodiscard]] inline bool media_capability_declared(media_category cat,
                                                       ChatClientCapabilities const& caps) noexcept {
    switch (cat) {
        case media_category::image: return caps.multimodal_in_image;
        case media_category::audio: return caps.multimodal_in_audio;
        case media_category::video: return caps.multimodal_in_video;
        case media_category::file: return caps.multimodal_in_file;
    }
    return false;
}

namespace chat_client_detail {

// Recurses into `ToolResult::content` too — a tool reply carrying an image is exactly as
// unencodable outbound as one arriving directly in a Message, and the ORIGINAL gap-19 finding named
// "Media nested inside ToolResult::content" as unaddressed by either phase; walking the SAME
// recursive shape `translate_message()`'s own tool-result loop already uses closes that specific
// residual as a direct consequence of writing this check correctly, not as separately scoped work.
[[nodiscard]] inline result<void> check_media_capability(std::vector<ContentItem> const& items,
                                                            std::string const& message_id,
                                                            ChatClientCapabilities const& caps) {
    for (ContentItem const& item : items) {
        if (auto const* media = std::get_if<Media>(&item.value)) {
            media_category const cat = categorize_media_type(media->media_type);
            if (!media_capability_declared(cat, caps)) {
                return std::unexpected(error{
                    failure_class::contract,
                    "message '" + message_id + "' carries Media content (media_type='" +
                        media->media_type +
                        "') but the bound ChatClient does not declare the matching "
                        "multimodal_in_* capability -- refusing to send a request that would "
                        "silently drop it",
                    "chat_client.multimodal_capability_missing"});
            }
        } else if (auto const* tool_result = std::get_if<ToolResult>(&item.value)) {
            auto nested = check_media_capability(tool_result->content, message_id, caps);
            if (!nested) return nested;
        }
    }
    return {};
}

}  // namespace chat_client_detail

[[nodiscard]] inline result<void> validate_outbound_media_capabilities(
    ChatRequest const& request, ChatClientCapabilities const& caps) {
    for (Message const& m : request.messages) {
        auto r = chat_client_detail::check_media_capability(m.content, m.message_id, caps);
        if (!r) return r;
    }
    return {};
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
