#pragma once
// Implements 004-Model-Provider-Plane.md §3 -- Milestone 5 Phase E: the Anthropic ChatClient backend
// (Messages API, `POST /v1/messages`), 004 §3's "first-class" backend (reasoning parts, prompt
// caching, tool use) for Claude 5 family (Fable 5, Opus 5, Sonnet 5) and Haiku 4.5. Wire-format field
// names below were sourced directly from the official Anthropic C# SDK's generated model code
// (`D:\GitSrc\anthropic-sdk-csharp`, the same rigor Phase D's OpenAI backend used against
// `D:\GitSrc\openai-dotnet`), not paraphrased from documentation.
//
// Structurally mirrors protocol/openai/chat_client.hpp (same detail-namespace-of-pure-functions
// shape, same DETACHED background thread for chat_stream(), same injectable resolver/CA-bundle
// testability seam over Phase C's perform_provider_https_exchange, same "perform_provider_https_
// exchange is a synchronous blocking call, freely callable from inside chat()'s coroutine body,
// matching Phase B4a's own already-established position" reasoning) -- differences below are
// Anthropic's own wire shape, not a second design.
//
// Anthropic-specific translation decisions, named rather than silently assumed:
//
// (1) SYSTEM PROMPT. Anthropic's Messages API has NO `role:"system"` in its `messages[]` array (the
//     SDK's own doc comment: "there is no system role for input messages in the Messages API") --
//     `system` is a distinct top-level request field. Any `role::system` messages in `ChatRequest.
//     messages` are extracted and concatenated into `system`; every other message is translated
//     normally. A `role::tool` AE message (a tool reply) has no Anthropic equivalent role either --
//     Anthropic represents a tool reply as a `role:"user"` message containing a `tool_result` content
//     block, so `role::tool` translates to wire role `"user"`.
//
// (2) TOOL-CALL ARGUMENTS ARE A REAL JSON OBJECT, not a string. Unlike OpenAI's `function.arguments`
//     (a JSON-encoded STRING, confirmed in Phase D's own research), Anthropic's `tool_use.input` is a
//     genuine JSON object on the wire (confirmed: `Dictionary<string,JsonElement>` in the SDK, no
//     stringify/parse round-trip). `ToolCall::arguments_json` (this project's content model) is
//     always a string, so outbound translation parses it into a `json::Value` and emits that object
//     directly; inbound parsing does the reverse (`json::dump` the received object back into
//     `arguments_json`) -- lossless either way, just a different wire shape than Phase D's.
//
// (3) THINKING BLOCKS: response-parsing ONLY, not outbound round-tripping. A `thinking` block
//     requires both `thinking` (text) AND `signature` (an opaque tamper-evidence string) on the wire;
//     `redacted_thinking` carries only opaque `data`, no visible text at all. This project's content
//     model (`agentengine::Reasoning{text, encrypted}`) has no field to carry `signature`/`data` --
//     so a `Reasoning` content item received in a response is translated into `Reasoning{text=
//     thinking-text or empty, encrypted=is-redacted}` for the CALLER to read, but this backend never
//     re-sends a `Reasoning` item back to Anthropic in a later turn's outbound `messages[]` (silently
//     dropped from history translation) -- resending a thinking block without its real signature is
//     either rejected by the API or defeats the tamper-evidence property it exists for, and this
//     project's content model has nowhere to have kept that signature in the first place. Named here
//     rather than silently claimed as full extended-thinking round-tripping.
//
// (4) PROMPT CACHING (`cache_control`), scoped to the two segment boundaries actually visible from
//     `ChatRequest` as received (004 §8 Q2's resolution: "a backend declaring `prompt_caching` inserts
//     its own vendor-specific breakpoints at [005 §3's] existing segment boundaries as backend-
//     internal translation logic"): the extracted `system` text and the LAST tool definition, when
//     `ChatClientCapabilities::prompt_caching` is declared true for this bound instance. `ChatRequest`
//     itself is already a FLATTENED `messages` list by the time it reaches any `ChatClient` (005 §3's
//     `ContextAssemblyResult`/`assemble_context`, core/context_assembly.hpp, merges every
//     contributor's messages into one vector with no per-contributor boundary markers surviving) --
//     so per-message-history cache breakpoints at finer-grained contributor boundaries are NOT
//     reachable from this seam without threading boundary markers through `ContextAssemblyResult` and
//     `ChatRequest` first, which is a real, separate, not-yet-built architectural change, not a gap in
//     this phase's own translation logic. system+tools is exactly Anthropic's own documented
//     best-practice cache-breakpoint placement (the stable prefix), so this is a meaningful, real
//     caching win at the boundary that IS reachable today, not a token gesture.
//
// (5) STRUCTURED OUTPUT uses Anthropic's native `output_config.format` (`{"type":"json_schema",
//     "schema":...}`), confirmed as the SDK's own primary mechanism (distinct from, and newer than,
//     the tool-forcing convention `output_schema_strategy::tool_shaped`, core/chat_client.hpp, still
//     covers via `tool_choice`). No `additionalProperties:false` forcing here unlike Phase D's OpenAI
//     backend -- nothing in the research confirms Anthropic requires or even recognizes that key, so
//     this backend does not assert an unconfirmed requirement onto the schema.
//
// (6) E2: CUMULATIVE-TO-INCREMENTAL USAGE CONVERSION. Every `message_delta.usage` field on the wire is
//     documented (and proven, via the SDK's own `MessageContentAggregator.GetResult` reduce logic) to
//     be the RUNNING TOTAL so far, not a per-event delta -- `output_tokens` is unconditionally
//     overwritten by each event's value, `input_tokens`/cache-token fields are seeded once from
//     `message_start.message.usage` and only overwritten on a `message_delta` that actually carries a
//     non-null value. `accumulate_message_delta_usage` implements exactly this reduce, tested against
//     a literal multi-event SSE sequence. **What this conversion has nowhere to surface**: `chat_
//     stream()`'s `ChatResponseUpdate` carries no `Usage` field at all -- the SAME pre-existing gap
//     Phase D's own OpenAI backend already named (`ChatResponseUpdate{delta, is_final}`, no usage
//     slot). The conversion LOGIC is real and tested; wiring it into a caller-visible per-chunk value
//     needs `ChatResponseUpdate` to grow a field this phase does not add.
//
// (7) M5 RESEARCH FOLLOW-UP (docs/research/2026-08-07-provider-metadata-and-sampling-params-survey.md,
//     "Recommended design" items 1/2/4/5/6/7 -- items 3 (`reasoning_effort`) and 8 (`prompt_cache_key`)
//     explicitly deferred there, not built here): `http_referer`/`x_title` (item 1, OpenRouter-only
//     attribution headers, stamped into every request only when non-empty); `end_user_id` (item 2,
//     Anthropic's `metadata.user_id` abuse-tracking field -- `Metadata` has ONLY that one field per the
//     locally-vendored SDK's `Metadata.cs`; Anthropic has NO native `seed` field at all per Finding 2,
//     so no seed parameter exists here -- a fake no-op would be worse than the honest absence);
//     `ChatResponse.model`/`Usage.cache_write_tokens` (items 4/5, response-parsing-only, see
//     `parse_message_response`); the 4-`cache_control`-blocks-combined hard cap (item 6,
//     `count_cache_control_blocks`, enforced defensively at the end of `build_request_body` even though
//     today's own placements -- at most 2, system + last tool -- can never reach it); and `cache_ttl`
//     (item 7, `"5m"`/`"1h"`/empty-for-server-default, validated at construction time, applied to every
//     `cache_control` object this backend builds).

#ifdef AGENTENGINE_WITH_HTTPS

#include <cctype>
#include <charconv>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/core/system_channel_fence.hpp"
#include "agentengine/sandbox/incremental_http_body.hpp"
#include "agentengine/protocol/provider_chat_wire.hpp"
#include "agentengine/sandbox/provider_http_client.hpp"
#include "agentengine/trust/secret.hpp"

namespace agentengine::anthropic {

namespace detail {

// See file banner (4): a testability seam mirroring provider_http_client.hpp's own, identical to
// Phase D's OpenAI backend's `Resolver` alias.
using Resolver = agentengine::detail::provider_wire::Resolver;

// (2): a real JSON object on the wire, not a stringified-JSON string -- ToolCall::arguments_json is
// parsed into this shape for outbound translation, and json::dump()'d back for inbound.
[[nodiscard]] json::Value translate_tool_use_input(std::string const& arguments_json);

// Splits `messages` into (system_text, non-system messages) -- see file banner (1). System text from
// multiple role::system messages concatenates in order, joined by a real separator ("\n\n") between
// any two non-empty fragments -- gap-audit finding 17: a bare concatenation left two independently-
// sourced system texts (e.g. an agent's own instructions and an injected memory item, 029 §6) with no
// boundary at all, so one fragment's trailing text and the next fragment's leading text could visually
// (and, worse, to the model reading it) run together as if they were one continuous statement. The
// separator is only inserted BETWEEN fragments (never as a leading/trailing pad), so a single system
// message's own text is emitted byte-for-byte unchanged, preserving the one case this file's own
// existing test already relied on a caller-supplied leading space for.
struct SplitMessages {
    std::string system_text;
    std::vector<Message const*> rest;
};

// `request_code` (ADR-191): the request's code, which an approved lesson's open marker carries; if empty
// and one is, a code is drawn here, so the preamble and the fences always agree.
[[nodiscard]] SplitMessages split_system_messages(std::vector<Message> const& messages,
                                                  std::string request_code = {});

[[nodiscard]] inline std::string_view role_to_wire(role r) noexcept {
    // role::tool has no Anthropic role of its own -- a tool reply travels as a "user" message
    // carrying a tool_result content block (file banner (1)).
    return (r == role::assistant) ? "assistant" : "user";
}

// One AE `Message` -> one Anthropic wire message object (`{"role", "content"}`, content always the
// array-of-blocks form, never the string-collapse shorthand -- simpler to always emit the array, and
// nothing here needs the shorthand). `role::system` messages must be filtered out by the caller
// BEFORE this is called (split_system_messages) -- this function has no system-role handling of its
// own, only user/assistant/tool translation. Reasoning content items are silently dropped from
// outbound translation (file banner (3)).
[[nodiscard]] json::Value translate_message(Message const& m);

// (7): `cache_ttl` validity -- the ONLY two non-empty values Anthropic's `CacheControlEphemeral.Ttl`
// enum accepts (empty means "omit `ttl` entirely, server default of 5 minutes applies"). A pure
// predicate so both the constructor-time contract check and any direct test of it share one definition.
[[nodiscard]] inline bool is_valid_cache_ttl(std::string const& ttl) noexcept {
    return ttl.empty() || ttl == "5m" || ttl == "1h";
}

// (7): one `cache_control` object, shared by both places this backend emits one (the system block and
// the last tool) -- `{"type":"ephemeral"}`, plus `"ttl"` as a sibling when a non-default TTL was
// requested. `cache_ttl` is assumed already validated (is_valid_cache_ttl) by the caller -- this
// function does not re-validate, it only shapes the wire object.
[[nodiscard]] json::Value make_cache_control(std::string const& cache_ttl);

// One `ToolDescriptor` -> `{"name","description","input_schema"}` -- flat, no "function" wrapper
// (confirmed against the SDK's `Tool.cs`: `input_schema` is a top-level sibling of `name`, not nested).
// `cache_ttl` (default empty, (7)) is only consulted when `cache_this_one` is true. Issue #13: same
// re-parse-every-call waste as the OpenAI backend's `translate_tool()` -- `t.args_schema_value_cached`
// (set once at `make_tool_descriptor<T>()` time, `core/tool_pipeline.hpp`) skips the re-parse when
// available, falling back to parsing `args_schema_json` for any hand-built descriptor without it.
[[nodiscard]] result<json::Value> translate_tool(ToolDescriptor const& t, bool cache_this_one,
                                                 std::string const& cache_ttl = {});

// (5): Anthropic's native structured-output mechanism -- `output_config.format`, `{"type":
// "json_schema","schema":...}` -- distinct from OpenAI's `response_format` wrapper name/shape.
[[nodiscard]] result<json::Value> translate_output_config(std::string const& schema_json);

// D1-equivalent: the full `POST /v1/messages` request body. `max_tokens` is REQUIRED by Anthropic
// (unlike OpenAI, where it's optional/deprecated) -- ChatRequest carries no sampling-parameter field
// at all (chat_client.hpp's own file-top comment: "sampling parameters... stay elided"), so this
// falls back to the bound backend's own declared `ChatClientCapabilities::max_output_tokens` when
// nonzero, else a conservative fixed default -- a real, named translation-layer decision, not an
// unstated guess.
inline constexpr std::uint64_t kDefaultMaxTokens = 4096;

// (7): the hard invariant Finding 5 confirms via a real reported API bug -- Anthropic rejects a request
// with `HTTP 400: A maximum of 4 blocks with cache_control may be provided`, counted across system +
// tools + messages COMBINED. A generic recursive walk (not schema-specific) so it stays correct if this
// backend ever grows a THIRD or FOURTH placement site without needing a matching update here: it counts
// every "cache_control" member anywhere in the assembled request body, at any depth. Pure function, no
// network -- independently testable against a hand-built json::Value the public request-building API
// can never itself produce (today's own placements top out at 2).
[[nodiscard]] std::size_t count_cache_control_blocks(json::Value const& v);

inline constexpr std::size_t kMaxCacheControlBlocks = 4;

// ADR-020: map 004 §2's portable ordinal level down to Anthropic's genuinely different native shape.
// Not a spelling change like OpenAI's -- Anthropic takes a token BUDGET, so a level has to become a
// NUMBER, and the only defensible number is a fraction of the very `max_tokens` this same request
// carries. Absolute budgets can't work: they would silently exceed a small `max_tokens`.
//
// Two hard constraints Anthropic itself documents, enforced HERE rather than left to the server:
//   * `budget_tokens >= 1024`
//   * `budget_tokens < max_tokens`
// A gateway may not enforce them -- OpenRouter was measured (ADR-020 §2) returning HTTP 200 for both
// `budget_tokens == max_tokens` and `budget_tokens == 512`, while `api.anthropic.com` rejects both.
// Sending a request that works through the lenient hop and breaks against the strict one is exactly
// the silent-divergence this project's conventions exist to prevent, so the client fails closed.
inline constexpr std::uint64_t kMinThinkingBudgetTokens = 1024;

// Percent of `max_tokens` per level. Deliberately leaves headroom at `high` (75%, not ~100%): the
// remainder is what the visible answer is written from, and a budget that consumed the whole
// allowance would leave a reasoning trace with no room for a reply.
[[nodiscard]] std::uint64_t thinking_budget_for(reasoning_effort effort,
                                                std::uint64_t max_tokens) noexcept;

// Builds the `thinking` object, or fails closed when the vendor's own floor cannot fit under
// `max_tokens` at all (i.e. `max_tokens <= 1024`) -- the one case where no budget satisfies both
// constraints simultaneously, so there is no honest request to send.
[[nodiscard]] result<json::Value> translate_reasoning_effort(reasoning_effort effort,
                                                             std::uint64_t max_tokens);

// `end_user_id`/`cache_ttl` (7): both default-empty, both optional. `end_user_id` non-empty adds
// `metadata.user_id` (Anthropic's abuse-tracking id -- `Metadata` has ONLY this one field). `cache_ttl`
// non-empty is applied to every `cache_control` object this function builds (system block + last tool,
// via `make_cache_control`) -- assumed already validated by the caller (AnthropicChatClient's
// constructor enforces `is_valid_cache_ttl`; this function does not re-check, so it stays directly
// testable with any string, including deliberately invalid ones, without throwing).
[[nodiscard]] result<json::Value> build_request_body(ChatRequest const& request,
                                                     std::string const& model,
                                                     ChatClientCapabilities const& caps,
                                                     bool stream,
                                                     std::string const& end_user_id = {},
                                                     std::string const& cache_ttl = {});

// `http_referer`/`x_title` (7): OpenRouter's own app-attribution header convention (Finding 1) --
// stamped in ONLY when non-empty, both default-empty, so a caller who never sets them gets exactly
// today's three headers and nothing more.
//
// `session_id` (docs/research/2026-08-21-openrouter-session-id-header.md): OpenRouter's own prompt-
// cache sticky-routing key -- NOT `end_user_id`/`user`/`metadata.user_id`, which that vendor uses only
// for abuse-tracking and plays no role in cache routing (confirmed directly against OpenRouter's own
// docs, correcting an earlier claim in this codebase's own comments). Sent as the `x-session-id`
// header, same "stamped only when non-empty" discipline as the two headers above.
[[nodiscard]] sandbox::NetEgressRequest build_http_request(std::string const& path,
                                                           std::string const& api_key,
                                                           std::string const& api_version,
                                                           std::string body,
                                                           std::string const& http_referer = {},
                                                           std::string const& x_title = {},
                                                           std::string const& session_id = {});

// One inbound content block (§2/§7 of the wire-format research) -> zero-or-one AE ContentItem.
// Shared between the non-streaming response parser and the streaming content_block accumulator.
// `producer_chat_client_id` (gap-audit finding 20 / 003 §8 Q2) is stamped onto any `Reasoning` this
// call produces -- defaults empty so every pre-existing call site (positional or not) is unaffected.
[[nodiscard]] std::optional<ContentItem> translate_response_block(
    json::Value const& block, std::string const& producer_chat_client_id = {});

// E1: the non-streaming response. `content[]` (§7), `usage.input_tokens`/`output_tokens`/
// `cache_read_input_tokens` (confirmed exact field names, distinct from OpenAI's `prompt_tokens`/
// `completion_tokens`).
[[nodiscard]] result<ChatResponse> parse_message_response(
    json::Value const& body, std::string const& producer_chat_client_id = {});

[[nodiscard]] error map_http_status_error(std::uint16_t status, std::string const& body);

// (E2) The cumulative-usage reduce, proven against the SDK's own MessageContentAggregator.GetResult
// logic (file banner (6)): output_tokens is unconditionally overwritten by each message_delta event's
// value; input_tokens/cache-token fields are seeded from message_start and only overwritten when a
// later message_delta actually carries a non-null value for that field. Pure function, no network --
// testable against a literal event sequence.
struct AnthropicUsageSnapshot {
    std::optional<std::uint64_t> input_tokens;
    std::uint64_t output_tokens = 0;
    std::optional<std::uint64_t> cache_read_input_tokens;
};

void seed_usage_from_message_start(AnthropicUsageSnapshot& snapshot, json::Value const& usage);

void accumulate_message_delta_usage(AnthropicUsageSnapshot& snapshot, json::Value const& usage);

// D2-equivalent chunked-transfer decoding on the ORDINARY non-streaming `chat()` response used to be
// needed HERE (RFC 9112 §7.1 framing; a real OpenAI-compatible endpoint sends `Transfer-Encoding:
// chunked` on non-streaming responses too, not only SSE, and the same is assumed true of Anthropic-
// compatible endpoints), duplicated rather than shared with protocol/openai/chat_client.hpp per this
// project's own "a second, independent copy rather than a shared header" precedent. As of 2026-08-19
// that gap is closed at the transport layer instead (`net_egress_proxy.cpp`'s
// `dechunk_response_body_if_needed`, decisions/ADR-011-first-party-egress-proxy.md's addendum) --
// `resp->body` below is already plain by the time `chat()` sees it. Running the old per-provider decode
// a second time on an already-dechunked JSON body misparses it as chunk framing and fails closed
// (found the hard way, fixed same day as protocol/openai's identical retirement). The STREAMING path
// (`chat_stream()` / `run_stream_worker` below, `sandbox::perform_provider_streaming_exchange`) is
// unaffected -- it uses the separate incremental `sandbox::ChunkedBodyDecoder`, which the
// transport-layer fix deliberately does not touch.

// Anthropic's SSE uses NAMED events (an `event: <type>` line paired with the following `data: {...}`
// line) -- structurally different from OpenAI's single always-"data:"-only shape (Phase D's own
// split_sse_data_events), confirmed against the SDK's Sse.cs event-type switch. Every Anthropic event
// fits on one line of JSON.
struct SseEvent {
    std::string_view type;
    std::string_view data;
};

[[nodiscard]] std::vector<SseEvent> split_sse_named_events(std::string_view body);

// E2/D2-equivalent: the chunk-to-ChatResponseUpdate translation, factored out of the network call for
// offline testability (mirrors Phase D's `parse_streaming_response_into_updates`). Text deltas emit
// one ChatResponseUpdate per SSE event (real per-chunk fidelity); tool_use input and thinking text
// arrive incrementally across events keyed by `index` and are accumulated per content_block, emitted
// once as a complete item on that block's `content_block_stop`.
// ADR-019: the incremental streaming decoder, Anthropic's named-event counterpart to the OpenAI
// backend's. Fed raw response bytes as they arrive, it returns whichever `ChatResponseUpdate`s are
// complete right now, so a text delta reaches the ring the moment the vendor emitted it.
//
// Same one-item hold-back as the OpenAI accumulator, for the same reason: `is_final` marks the LAST
// update, and "last" is unknowable until the stream ends, so exactly one completed update is held and
// released when the next arrives (or flushed, marked final, by `finish()`).
//
// TOOL_USE AND THINKING BLOCKS STILL EMIT AT THE END, unchanged from the one-shot parser. A
// `tool_use` block's `input` arrives as `partial_json` FRAGMENTS and a partial fragment is not valid
// `arguments_json`; a `thinking` block is likewise accumulated across `thinking_delta`s. Text deltas
// map 1:1 to the vendor's own events and stream immediately. Same ordering, same content -- and
// `parse_streaming_response_into_updates` below is now implemented in terms of THIS type, so the
// streaming and one-shot paths are one decoder rather than two that could drift.
class StreamingUpdateAccumulator {  // ae-naming-lint: allow StreamingUpdateAccumulator — new ADR-019 vocabulary; 027 has not been updated to list it
public:
    // `producer_chat_client_id` (gap-audit finding 20): defaults empty, so every existing positional
    // `StreamingUpdateAccumulator(chunked)` call site (every test in this file included) compiles and
    // behaves identically -- an accumulator constructed without it simply never stamps a producer id
    // onto any `Reasoning` it emits, same as before this field existed.
    explicit StreamingUpdateAccumulator(bool chunked, std::string producer_chat_client_id = {})
        : chunked_(chunked), producer_chat_client_id_(std::move(producer_chat_client_id)) {}

    [[nodiscard]] result<std::vector<ChatResponseUpdate>> feed(std::string_view bytes);

    // ADR-177: true when the body was chunk-framed and the peer went away BEFORE the terminating
    // 0-chunk. That is an HTTP framing violation, not a short answer -- the transport hands a close
    // to its caller as an ordinary end-of-body ("the terminal 0-chunk is the CALLER's to notice",
    // net_egress_proxy.cpp), and until a caller notices, a connection cut mid-answer looks exactly
    // like a stream that ended. It then surfaces as "completed with no usage", a contract failure
    // nothing retries, when the truth is a transient one. Only meaningful when chunked: an unchunked
    // SSE body legitimately ends at connection close and carries no such signal.
    // A stream that already delivered `message_stop` is COMPLETE whatever the framing did afterwards
    // (some proxies close without the final 0-chunk after a whole answer); failing it would discard a
    // finished, billed response and pay for it again.
    [[nodiscard]] bool truncated() const noexcept {
        return chunked_ && !chunked_decoder_.complete() && !message_stop_seen_;
    }

    [[nodiscard]] std::vector<ChatResponseUpdate> finish();

private:
    struct PendingBlock {
        bool seen = false;
        std::string kind;
        std::string tool_id;
        std::string tool_name;
        std::string tool_input_json;  // accumulated partial_json fragments
        std::string thinking_text;
        bool redacted = false;
    };

    // Gap-audit finding 20 / 003 §8 Q2: this is the SINGLE choke point every ContentItem this
    // accumulator produces passes through, regardless of which construction site built it (a
    // `thinking`/`redacted_thinking` block reconstructed here in `finish()`, or any future content
    // kind) -- stamping `producer_chat_client_id` here once, rather than at each construction site
    // individually, means a future third construction site can never forget to stamp it.
    //
    // 004 §1 amendment (`ChatResponseUpdate::continues_previous`): `text_block` is the content-block
    // index a live `text_delta` item came from, and empty for everything else. A text item continues
    // the previous item only when the previous item was text from the SAME block. That is the whole
    // point of asking the producer: `tool_use` and `thinking` blocks are held until `finish()`, so the
    // text of block 0 and the text of block 2 either side of a tool call arrive here back to back,
    // and adjacency alone would glue two blocks the non-streamed reply keeps separate.
    void release(std::vector<ChatResponseUpdate>* out, ContentItem item,
                 std::optional<std::size_t> text_block = std::nullopt);

    // One content item out of `items_from_block()`, with the block a live text fragment belongs to.
    struct BlockItem {
        ContentItem item;
        std::optional<std::size_t> text_block;
    };

    PendingBlock& ensure_index(std::size_t index) {
        if (index >= pending_by_index_.size()) pending_by_index_.resize(index + 1);
        return pending_by_index_[index];
    }

    // `chunk_out`: unified-streaming-design-draft.md §1 (Piece B), Findings 26/27 -- companion
    // emission path symmetric with the OpenAI-side accumulator, keyed off this backend's own
    // `PendingBlock`/SSE-named-events vocabulary instead of OpenAI's `PendingToolCall`/JSON-array
    // one. Unlike OpenAI, Anthropic's wire protocol gives a real per-index completion signal
    // (`content_block_stop`) -- `is_final` is a resolved answer here, not an open question.
    [[nodiscard]] std::vector<BlockItem> items_from_block(std::string const& block,
                                                            std::vector<ChatResponseUpdate>* chunk_out);

    [[nodiscard]] std::optional<Usage> captured_usage() const;

    bool chunked_;
    bool message_stop_seen_ = false;
    std::string producer_chat_client_id_;
    sandbox::ChunkedBodyDecoder chunked_decoder_;
    sandbox::SseEventFramer framer_;
    std::vector<PendingBlock> pending_by_index_;
    std::optional<ContentItem> held_;
    bool held_continues_previous_ = false;  // `continues_previous` for the update `held_` becomes
    std::optional<std::size_t> last_released_text_block_;  // block of the last released item, if text
    AnthropicUsageSnapshot usage_snapshot_;
};

// The one-shot parse, for a body genuinely fully in hand -- implemented on top of the incremental
// accumulator (ADR-019) so the two paths are one decoder, not two.
[[nodiscard]] result<std::vector<ChatResponseUpdate>> parse_streaming_response_into_updates(
    std::string_view raw_body, bool is_chunked, std::string const& producer_chat_client_id = {});

// The detached background worker -- see protocol/openai/chat_client.hpp's own identical rationale for
// why detached, not a tracked thread member (a bound instance is shared across concurrent streaming
// calls via ChatClientRegistry).
void run_stream_worker(std::string host, std::uint16_t port, std::string path, std::string api_key,
                       std::string api_version, std::string model, ChatClientCapabilities caps,
                       ChatRequest request, stream_producer<ChatResponseUpdate> producer,
                       Resolver resolver, std::string ca_bundle_pem_override,
                       std::string http_referer, std::string x_title, std::string end_user_id,
                       std::string cache_ttl, sandbox::ProviderTransport transport,
                       std::stop_token stop, std::string session_id);

}  // namespace detail

// The real, product-code `ChatClient` conformer for 004 §3's first-class Anthropic backend. `Store`
// is any real `SecretStore` (`AgentEngineSecretStore` in production; `InMemorySecretStore` in tests).
template <SecretStore Store>
class AnthropicChatClient {
public:
    // (7): four OPTIONAL trailing parameters, appended strictly at the END of the existing list, never
    // inserted -- every existing positional-argument construction call site (tests/test_anthropic_chat_
    // client_live.cpp has several) keeps compiling unchanged. `http_referer`/`x_title` (research doc
    // item 1) and `end_user_id` (item 2) have no validity constraint of their own -- any string,
    // including empty (the "don't send this header/field at all" default), is accepted verbatim.
    // `cache_ttl` (item 7) DOES have a real constraint (Anthropic's `CacheControlEphemeral.Ttl` enum has
    // exactly two non-empty values) -- checked here, at construction, a cold setup path where
    // CONVENTIONS.md permits an exception to surface (this project's error model, `ae::result<T>`, has
    // no return channel from a constructor to route a rejection through instead).
    AnthropicChatClient(std::string host, std::uint16_t port, std::string model, SecretRef api_key_ref,
                         ChatClientCapabilities caps, Store const& store, std::string path_prefix = "/v1",
                         std::string api_version = "2023-06-01",
                         detail::Resolver resolver = sandbox::resolve_host,
                         std::string ca_bundle_pem_override = {}, std::string http_referer = {},
                         std::string x_title = {}, std::string end_user_id = {}, std::string cache_ttl = {},
                         sandbox::ProviderTransport transport = sandbox::ProviderTransport::tls,
                         // docs/research/2026-08-21-openrouter-session-id-header.md: OpenRouter's own
                         // prompt-cache sticky-routing key, sent as the `x-session-id` header -- NOT
                         // `end_user_id` above, which that vendor does not use for cache routing at
                         // all. Appended last, same "never insert earlier" convention as every
                         // optional param above.
                         std::string session_id = {})
        : host_(std::move(host)),
          port_(port),
          model_(std::move(model)),
          api_key_ref_(std::move(api_key_ref)),
          capabilities_(caps),
          store_(store),
          path_prefix_(std::move(path_prefix)),
          api_version_(std::move(api_version)),
          resolver_(std::move(resolver)),
          ca_bundle_pem_override_(std::move(ca_bundle_pem_override)),
          http_referer_(std::move(http_referer)),
          x_title_(std::move(x_title)),
          end_user_id_(std::move(end_user_id)),
          cache_ttl_(std::move(cache_ttl)),
          transport_(transport),
          session_id_(std::move(session_id)) {
        if (!detail::is_valid_cache_ttl(cache_ttl_)) {
            throw std::invalid_argument(
                "AnthropicChatClient: cache_ttl must be \"\" (server default), \"5m\", or \"1h\"");
        }
    }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return capabilities_; }

    // Gap-audit finding 20 / 003 §8 Q2: this bound instance's own real identity, "vendor:model" --
    // the same runtime string shape `ChatClientId<"vendor:model">`/`ChatClientRegistry` already use.
    // Deliberately derived from THIS instance's own construction parameters, not threaded in from an
    // agent's compile-time `ChatClientId<...>` policy tag: what determines whether a `Reasoning`
    // trace is safe to echo back is which backend/model actually produced it, a real runtime
    // property of this object, not a separate compile-time configuration string that could in
    // principle diverge from it. Not part of the `ChatClient` concept's required shape (optional,
    // duck-typed detection via `HasProducerChatClientId`, chat_client.hpp) -- every other conformer
    // (mocks, test fixtures) is unaffected by this method existing.
    [[nodiscard]] std::string producer_chat_client_id() const { return "anthropic:" + model_; }

    [[nodiscard]] task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext& ctx) const {
        // Resolution happens HERE, inside chat(), against EffectContext -- never at construction
        // (004 §1 / 018 §4, the same rule test_chat_client_credential_resolution.cpp proves).
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) co_return std::unexpected(lease.error());

        auto body = detail::build_request_body(request, model_, capabilities_, /*stream=*/false,
                                                end_user_id_, cache_ttl_);
        if (!body) co_return std::unexpected(body.error());

        auto req = detail::build_http_request(path_prefix_ + "/messages", lease->reveal_text(),
                                                api_version_, json::dump(*body), http_referer_, x_title_,
                                                session_id_);
        // ADR-206: the POST, the non-2xx mapping and the JSON parse are shared with the other backend.
        auto parsed = agentengine::detail::provider_wire::exchange_json("anthropic", host_, port_, req, resolver_,
                                                                        ca_bundle_pem_override_, transport_);
        if (!parsed) co_return std::unexpected(parsed.error());
        co_return detail::parse_message_response(*parsed, producer_chat_client_id());
    }

    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest request, EffectContext& ctx) const {
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) {
            // Forward the SecretStore's own real error -- matches chat()'s own non-streaming path,
            // which already returns lease.error() unchanged.
            pair.producer.fail(lease.error());
            return std::move(pair.consumer);
        }
        // ADR-017: read the token BEFORE moving the producer (see the OpenAI backend's identical note
        // -- unspecified argument evaluation order would otherwise let this read a moved-from producer).
        std::stop_token stop = pair.producer.stop_token();
        std::thread(&detail::run_stream_worker, host_, port_, path_prefix_ + "/messages",
                    lease->reveal_text(), api_version_, model_, capabilities_, std::move(request),
                    std::move(pair.producer), resolver_, ca_bundle_pem_override_, http_referer_, x_title_,
                    end_user_id_, cache_ttl_, transport_, std::move(stop), session_id_)
            .detach();
        return std::move(pair.consumer);
    }

private:
    std::string host_;
    std::uint16_t port_;
    std::string model_;
    SecretRef api_key_ref_;
    ChatClientCapabilities capabilities_;
    Store const& store_;
    std::string path_prefix_;
    std::string api_version_;
    detail::Resolver resolver_;
    std::string ca_bundle_pem_override_;
    std::string http_referer_;
    std::string x_title_;
    std::string end_user_id_;
    std::string cache_ttl_;
    sandbox::ProviderTransport transport_;
    std::string session_id_;
};

}  // namespace agentengine::anthropic

#endif  // AGENTENGINE_WITH_HTTPS
