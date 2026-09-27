#pragma once
// Implements 004-Model-Provider-Plane.md §3 -- Milestone 5 Phase D: the OpenAI-compatible ChatClient
// backend (Chat Completions), 004 §3's default/widest-reach backend (OpenAI, gateways, vLLM/
// llama.cpp/Ollama-style local servers, most vendor compat endpoints). Wire-format field names below
// were sourced directly from the official OpenAI .NET SDK's generated serialization code (the
// `Utf8JsonWriter`/`WritePropertyName` calls are ground truth for the real wire shape, not paraphrased
// from documentation), not guessed.
//
// Reuses Phase C's sandbox::perform_provider_https_exchange (the host-initiated HTTPS client) for the
// actual network exchange and Phase A's SecretStore seam for outbound-credential resolution AT THE
// POINT OF USE (004 §1's rule) -- `OpenAIChatClient` holds only a `SecretRef` member, the same
// behavioral shape `test_chat_client_credential_resolution.cpp`'s reference conformer already proves;
// this is that reference conformer's real, product-code analogue, not a second design.
//
// Capabilities are DECLARED, not probed (004 §3's own rule: "Capability set is per endpoint,
// discovered from config, not assumed") -- the caller constructs this type with whatever
// ChatClientCapabilities its own deployment's config says the target endpoint actually has; nothing
// here inspects a response to infer a capability.
//
// `perform_provider_https_exchange` is a synchronous, blocking call with nothing to suspend on (Phase
// B4a's own decision, milestone doc: "a sync function is freely callable from inside an async
// coroutine body with no co_await needed") -- `chat()`'s coroutine body calls it directly, matching
// that already-established project position rather than introducing a new one. It genuinely blocks
// the calling thread for the full round-trip; a real async I/O integration is future work on
// provider_http_client.hpp itself, not scoped here.
//
// D2 (streaming) scope, named honestly rather than silently claimed complete: `perform_provider_
// https_exchange` has no incremental/chunked-transfer read loop yet -- it blocks until the full HTTP
// response is buffered, then returns. `chat_stream()` below therefore performs one complete
// (blocking) HTTPS exchange on a detached background thread, decodes `Transfer-Encoding: chunked`
// framing if present (a real OpenAI-compatible streaming response's actual wire shape --
// `net_egress_proxy.cpp`'s raw-request builder always sends `Connection: close`, so the server
// closing the connection at the end is the reliable read-loop exit condition either way), splits the
// decoded body into SSE `data: ...` events, and pushes ONE ChatResponseUpdate per event/assembled-
// tool-call as it walks the already-fully-received event list -- so the vendor's own chunk
// BOUNDARIES are preserved faithfully in delivery order (004 §7 G3's own gate: "identical chunk
// boundaries"), and the credit-controlled ring (core/stream.hpp, Phase B4b) still provides genuine
// backpressure between this parsing thread and the consumer -- but the underlying network fetch
// itself is not low-latency incremental. A real incremental read loop is future work on
// provider_http_client.hpp, not scoped here. The background thread is DETACHED, not tracked as a
// member: `OpenAIChatClient` may be shared across many concurrent `chat_stream()` calls (a real
// `ChatClientRegistry` binds one instance per `ChatClientId`, reused across turns), so a single
// thread member that joins-then-replaces would wrongly serialize concurrent streams. The detached
// thread captures every value it needs (host/port/path/model/api-key text, the moved request, the
// moved `stream_producer`) and touches no state owned by `*this`, so its lifetime is fully decoupled
// from the client object's.

#ifdef AGENTENGINE_WITH_HTTPS

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/response_format_leak_scan.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/core/system_channel_fence.hpp"
#include "agentengine/sandbox/incremental_http_body.hpp"
#include "agentengine/protocol/provider_chat_wire.hpp"
#include "agentengine/sandbox/provider_http_client.hpp"
#include "agentengine/trust/secret.hpp"

namespace agentengine::openai {

namespace detail {

[[nodiscard]] std::string_view role_to_wire(role r) noexcept;

// One AE `Message` -> one OpenAI wire message object. Handles the three content shapes this project's
// own content model can express today (Text, ToolCall, ToolResult) -- Reasoning/Media/Citation/Custom
// are not translated outbound yet (Chat Completions itself has no reasoning-trace field on inbound
// messages either, confirmed against the SDK's request-message serializers: only `content`/
// `tool_calls`/`tool_call_id`/`name`/`refusal`/`audio` exist). A tool-role AE Message's `call_id`
// comes from its `ToolResult` content item -- the only place a call id lives in the content model, and
// the shape `core/tool_pipeline.hpp`'s own step-9 "normalize" produces (a `Data` item wrapping the
// tool's JSON reply, tagged `content_origin::tool`).
// `request_code` (ADR-191): this request's code, which an approved block's open marker carries; empty unless the
// request carries an approved lesson.
[[nodiscard]] json::Value translate_message(Message const& m, std::string_view request_code = {});

// A `role::tool` AE `Message` carrying N>1 `ToolResult` content items -- the natural shape a caller
// resolving N parallel tool calls in one turn would build -- must become N separate wire
// `{role:"tool", tool_call_id, content}` objects: OpenAI's Chat Completions contract requires one
// message per `tool_call_id` (confirmed against the SDK's own request-message shape), unlike
// Anthropic's Messages API, which legitimately bundles multiple `tool_result` blocks into a single
// `user`-role message (protocol/anthropic/chat_client.hpp's own `translate_message`). `translate_
// message` above tracks only ONE `tool_call_id` variable, so feeding it a multi-ToolResult Message
// silently collapses to the LAST result's id, with every earlier ToolResult's text merged under that
// wrong id -- a real correlation bug, not merely a hygiene one, that a real provider surfaces as
// "tool_call_id ... not found" or an unresolved-tool-call 400 (the assistant turn's OTHER tool_calls
// entries are left with no matching reply message at all). Every other message shape (user/assistant/
// system, or a role::tool message with 0-1 ToolResult items) is unaffected -- still exactly one wire
// message via `translate_message` unchanged.
[[nodiscard]] std::vector<json::Value> translate_message_to_wire(Message const& m,
                                                                 std::string_view request_code = {});

// D3: one `ToolDescriptor` -> `{"type":"function","function":{"name","description","parameters"}}`
// (confirmed field names/nesting against the SDK's `InternalChatFunctionDefinition` serializer).
// `args_schema_json` already IS the tool's JSON Schema text (006's own real per-run tool table, no
// second provider-facing declaration shape) -- passed through as raw JSON, matching the SDK's own
// `WriteRawValue` passthrough for `parameters`/`schema` (it does no client-side schema validation
// either). Issue #13: `build_request_body` calls this once per tool on EVERY `chat()`/`chat_stream()`
// call, but the schema text is fixed for the tool's lifetime -- `t.args_schema_value_cached` (set once
// at `make_tool_descriptor<T>()` time, `core/tool_pipeline.hpp`) skips the re-parse when available,
// falling back to parsing `args_schema_json` for any hand-built descriptor that predates the cache.
[[nodiscard]] result<json::Value> translate_tool(ToolDescriptor const& t);

// Issue #14: `json_schema.hpp`'s generator represents an optional (`std::optional<U>`) field by
// omitting it from `required` -- correct for the plain tool-call schema shape it's shared with, but
// OpenAI's Structured Outputs `strict:true` contract requires EVERY property to be listed in
// `required`, with optionality instead expressed as a nullable union (`anyOf: [<T>, {"type":"null"}]`)
// on that property's own schema. This walks the schema tree (recursing through `properties` and array
// `items`, since a nested struct or array-of-structs can carry its own optional fields) and, for every
// object node with a `properties` key: adds every property name to `required`, wraps any property that
// wasn't already required in the nullable-union shape, and forces `additionalProperties: false` (the
// same requirement D4 already enforced, but only at the top level -- nested objects need it too).
[[nodiscard]] json::Value make_nullable(json::Value fragment);

[[nodiscard]] json::Value make_strict_schema(json::Value const& node);

// D4: 004 §3's other named checklist item -- "structured-output shaping that forces
// `additionalProperties: false` into the JSON Schema before it reaches the provider." `schema_json`
// is 003 §4's `OutputSchema<T>` text (`schema::json_schema_of<T>()`, an object schema with no
// `additionalProperties` key of its own -- confirmed against `json_schema.hpp`'s `ObjectBuilder`).
// Wraps as OpenAI's Structured Outputs `response_format` (`{"type":"json_schema","json_schema":
// {"name","schema","strict"}}`, confirmed field names/order against the SDK's
// `InternalResponseFormatJsonSchemaJsonSchema` serializer). `name` is required on the wire but 003 §4
// carries none -- "response" is a fixed, non-semantic placeholder (the schema body, not its name, is
// what OpenAI actually validates against). `make_strict_schema` (above) reshapes the whole tree for
// `strict:true`'s all-properties-required contract before it's embedded (issue #14).
[[nodiscard]] result<json::Value> translate_output_schema(std::string const& schema_json);

// 004 §2's 2026-08-07 amendment (ADR-020): map the portable ordinal level down to OpenAI's own flat
// string enum. `off` -> `"none"`, OpenAI's own spelling for the same request. This backend's native
// shape happens to be a near-match, which is exactly why the CAPABILITY GATE below lives in
// `build_request_body` and not here -- a pure spelling function has nothing to fail against.
[[nodiscard]] std::string_view translate_reasoning_effort(reasoning_effort effort) noexcept;

// D1: the full `POST /v1/chat/completions` request body. `stream` is a caller-supplied flag (never
// probed from `ChatRequest`) because `chat()` and `chat_stream()` are the two distinct callers, each
// wanting a different value.
//
// `end_user_id`/`seed` (Milestone 5 research follow-up, docs/research/2026-08-07-provider-metadata-and-
// sampling-params-survey.md, "Recommended design" items 1/2): backend-constructor-local, NOT portable
// `ChatRequest` vocabulary (004 §1's own sampling-parameter elision stands -- these are an abuse-
// tracking id and a best-effort determinism hint, not a sampling knob). Both optional; `end_user_id`
// empty means "omit `user` from the body entirely" (never send an empty-string user id), `seed`
// unset means "omit `seed` entirely" (never fabricate a value).
//
// `caps` (ADR-020): APPENDED LAST, defaulted, for the one thing this function must now refuse --
// 004 §2's degradation rule applied to `reasoning_effort`. Defaulting it to an all-false capability
// set is safe rather than surprising: the gate can only fire when a caller ASKS for a reasoning
// level, so every pre-existing call site (none of which can have set the field) is unaffected.
[[nodiscard]] result<json::Value> build_request_body(
    ChatRequest const& request, std::string const& model, bool stream,
    std::string const& end_user_id = {}, std::optional<std::int64_t> seed = std::nullopt,
    ChatClientCapabilities const& caps = {});

// `http_referer`/`x_title` (same research-doc follow-up, "Recommended design" item 1): an OpenRouter-
// specific app-attribution convention (`HTTP-Referer`/`X-Title` HTTP headers, NOT a JSON body field --
// no other surveyed backend has an equivalent, confirmed Finding 1). Stamped in only when non-empty --
// an empty string means "don't send this header at all," never a fabricated empty header value.
//
// `session_id` (docs/research/2026-08-21-openrouter-session-id-header.md): OpenRouter's own prompt-
// cache sticky-routing key -- NOT `end_user_id`/`user`, which that vendor uses only for abuse-tracking
// and plays no role in cache routing (confirmed directly against OpenRouter's own docs, correcting an
// earlier claim in this codebase's own comments). Sent as the `x-session-id` header, same "stamped
// only when non-empty" discipline as the two headers above.
[[nodiscard]] sandbox::NetEgressRequest build_http_request(std::string const& path,
                                                           std::string const& api_key,
                                                           std::string body,
                                                           std::string const& http_referer = {},
                                                           std::string const& x_title = {},
                                                           std::string const& session_id = {});

[[nodiscard]] error map_http_status_error(std::uint16_t status, std::string const& body);

// ADR-037 (second pass): `classify_http_status_stream_error` -- the streaming path's own separate,
// coarser status classifier -- is GONE. It existed only because `quark::error::detail` was a
// non-owning `std::string_view`, and this classifier fed `producer.fail()` sites running on a
// DETACHED background thread whose stack (including any parsed error-body string) is gone the instant
// the function returns, while a `stream<T>` consumer may read `fail_error()` well after that --
// reusing `map_http_status_error`'s dynamic, body-derived message there would have been a real
// dangling-view read. `agentengine::error::message` OWNS its text, so that hazard no longer exists:
// `run_stream_worker` below calls `map_http_status_error(status, {})` directly (an empty body, since
// the streaming path's error document was never captured as a separate buffer -- see that call site's
// own comment), getting the exact SAME status-code-driven retryable/policy/contract split `chat()`'s
// own non-streaming path already uses, from ONE function instead of two that could drift.

// Factored out so the streaming path (below) parses a trailing `usage`-only SSE chunk with the
// EXACT same field mapping as the non-streaming response body -- one implementation of the wire
// contract, not two that could drift (this file's own D2 precedent for `StreamingUpdateAccumulator`
// vs `parse_streaming_response_into_updates`).
[[nodiscard]] Usage parse_usage_object(json::Value const& usage);

// D1: the non-streaming response. Field names confirmed against the SDK's `ChatCompletion`/
// `ChatTokenUsage` serializers -- `choices[0].message.content`/`.tool_calls[]`, `usage.prompt_tokens`/
// `.completion_tokens`/`.prompt_tokens_details.cached_tokens`/`.completion_tokens_details.
// reasoning_tokens`. No `reasoning`/`reasoning_content` field exists on a VANILLA Chat Completions
// response message (confirmed: the SDK's response-message deserializer has no such branch) -- but
// OpenRouter's own (non-vanilla-OpenAI) Chat Completions-shaped surface DOES send one, confirmed live
// (issue #49: `message.reasoning`, a plain string, observed directly against
// `deepseek/deepseek-v3.2-exp` with `reasoning: {enabled: true}` in the request) -- see
// `reasoning_field_of` below, shared with the streaming delta parser.
//
// `producer_chat_client_id` (gap-audit finding 20 / 003 §8 Q2): defaults empty, matching
// `AnthropicChatClient`'s own `parse_message_response` convention, so a pre-existing positional
// `parse_chat_completion_response(body)` call site (every test in this file included) still compiles
// and behaves identically -- a call without it simply never stamps a producer id onto any `Reasoning`
// it emits.
[[nodiscard]] std::string_view reasoning_field_of(json::Value const& v);

[[nodiscard]] result<ChatResponse> parse_chat_completion_response(
    json::Value const& body, std::string const& producer_chat_client_id = {});

// ADR-023 Phase 1+2 (decisions/ADR-023-response-format-codec-seam.md §6 points 3-4). Opt-in only --
// `OpenAIChatClient`'s `scan_response_format_leaks` constructor flag gates whether this is ever
// called at all (default off, per the ADR's Finding 6: scanning is operator-armed, never
// content-triggered).
//
// ADR-035 Phase 1: `apply_response_format_scan` itself relocated to `core/response_format_leak_
// scan.hpp` so `AgentSession::run_model_call()` can apply it backend-agnostically (Anthropic
// included) regardless of streaming or `chat()` -- this call site is unchanged in behavior, just
// now calls the shared implementation instead of a private copy of it (one implementation, not two
// that could drift).

// D2: `Transfer-Encoding: chunked` framing (RFC 9112 §7.1) on the ORDINARY non-streaming `chat()`
// response (confirmed live against a real OpenAI-compatible endpoint, OpenRouter's `api.openrouter.ai`
// -- not only SSE sends it) used to need decoding HERE, because `perform_https_exchange` (Phase C)
// returned the raw, still chunk-framed bytes verbatim. As of 2026-08-19 that gap is closed at the
// transport layer instead (`net_egress_proxy.cpp`'s `dechunk_response_body_if_needed`, decisions/
// ADR-011-first-party-egress-proxy.md's addendum) -- `resp->body` below is already plain by the time
// `chat()` sees it, for every caller of the buffered (non-streaming) exchange functions. The
// per-provider decode duplicated here and in protocol/anthropic/chat_client.hpp was retired rather than
// kept as a second, now-redundant decode step: running it twice on an already-dechunked JSON body
// misparses the JSON as chunk framing and fails closed (found the hard way, fixed same day). The
// STREAMING path (`chat_stream()` / `run_stream_worker` below) is unaffected -- it uses the separate
// incremental `sandbox::ChunkedBodyDecoder` (`StreamingUpdateAccumulator`), which the transport-layer
// fix deliberately does not touch (see that function's own comment for why).

// SSE framing (only `data:` lines matter for an OpenAI-compatible stream -- `event:`/`id:`/`:comment`
// lines, if any, are ignored). Every OpenAI event fits on one line (compact single-line JSON), so no
// multi-line data accumulation is needed.
[[nodiscard]] std::vector<std::string_view> split_sse_data_events(std::string_view body);

// D2: the actual chunk-to-ChatResponseUpdate translation, factored out of the network call so it is
// testable against a literal canned SSE/chunked body with no live server involved. `raw_body` is
// exactly `NetEgressResponse::body` (still `Transfer-Encoding: chunked`-framed if `is_chunked`).
//
// Streaming tool-call ARGUMENT FRAGMENTS arrive incrementally across chunks, keyed by a per-choice
// `index` (confirmed against the SDK's `StreamingChatToolCallUpdate` -- `index` is required precisely
// so a consumer can correlate parallel tool-call fragments). Since this function already has every
// event in hand (Phase C's HTTP layer buffers the whole response, see file banner), fragments are
// accumulated in one pass and each tool call is emitted as ONE complete `ChatResponseUpdate` once
// fully assembled, appended after every text delta -- text deltas map 1:1 to the vendor's own SSE
// chunks (real per-chunk fidelity preserved), tool calls do not (unavoidable: a partial JSON-string
// fragment is not a valid `ToolCall::arguments_json` on its own).
// ADR-019: the incremental streaming decoder. Fed raw response bytes as they arrive off the socket,
// it returns whichever `ChatResponseUpdate`s are complete RIGHT NOW, so `chat_stream()` can push each
// text delta onto the ring the moment the vendor emitted it rather than after the whole completion
// has been received.
//
// ONE-ITEM HOLD-BACK, and why. `ChatResponseUpdate::is_final` marks the LAST update of a stream, and
// "last" is not knowable until the stream ends. Rather than weaken that contract (or emit a trailing
// empty update to carry the flag), this holds exactly one completed update back: `feed()` releases
// update N when N+1 becomes available, and `finish()` flushes the held one with `is_final` set. One
// item of lag, which is invisible next to a model's own inter-token latency, in exchange for
// `is_final` meaning exactly what it did before.
//
// TOOL CALLS STILL EMIT AT THE END, unchanged: a tool call's `arguments` arrive as JSON-string
// FRAGMENTS across many chunks (correlated by `index`), and a partial fragment is not a valid
// `ToolCall::arguments_json`. Text deltas map 1:1 to the vendor's own chunks and stream immediately;
// tool calls are assembled and appended by `finish()`. That is the same ordering and the same content
// the one-shot parser always produced -- see `parse_streaming_response_into_updates` below, which is
// now implemented in terms of THIS type precisely so the streaming and non-streaming paths cannot
// drift apart.
class StreamingUpdateAccumulator {  // ae-naming-lint: allow StreamingUpdateAccumulator — new ADR-019 vocabulary; 027 has not been updated to list it
public:
    // `producer_chat_client_id` (gap-audit finding 20, issue #49): defaults empty, matching
    // `AnthropicChatClient::StreamingUpdateAccumulator`'s own identical convention, so every
    // pre-existing positional `StreamingUpdateAccumulator(chunked)` call site (every test in this file
    // included) compiles and behaves identically -- an accumulator constructed without it simply never
    // stamps a producer id onto any `Reasoning` it emits, same as before this field existed.
    explicit StreamingUpdateAccumulator(bool chunked, std::string producer_chat_client_id = {})
        : chunked_(chunked), producer_chat_client_id_(std::move(producer_chat_client_id)) {}

    // Feeds raw (still chunk-framed, if the response was chunked) bytes. Returns updates ready now.
    [[nodiscard]] result<std::vector<ChatResponseUpdate>> feed(std::string_view bytes);

    // ADR-177: true when the body was chunk-framed and the peer went away BEFORE the terminating
    // 0-chunk. That is an HTTP framing violation, not a short answer -- the transport hands a close
    // to its caller as an ordinary end-of-body ("the terminal 0-chunk is the CALLER's to notice",
    // net_egress_proxy.cpp), and until a caller notices, a connection cut mid-answer looks exactly
    // like a stream that ended. It then surfaces as "completed with no usage", a contract failure
    // nothing retries, when the truth is a transient one. Only meaningful when chunked: an unchunked
    // SSE body legitimately ends at connection close and carries no such signal.
    // A stream that already delivered its own terminal event (`[DONE]`) is COMPLETE whatever the framing
    // did afterwards: some proxies close without the final 0-chunk after a whole answer, and failing
    // that would throw away a finished, billed response and pay for it again.
    [[nodiscard]] bool truncated() const noexcept {
        // `captured_usage_` counts too: with `include_usage` the usage chunk is the LAST thing a provider
        // sends, so a stream that delivered it is whole even if a sloppy gateway then closed without
        // `[DONE]` or the final chunk -- and usage is exactly what the session needs to call it complete.
        return chunked_ && !chunked_decoder_.complete() && !done_seen_ && !captured_usage_.has_value();
    }

    // End of stream: flushes the held-back update and every assembled tool call, marking the very
    // last one final. Safe to call on a stream that produced nothing (returns empty).
    [[nodiscard]] std::vector<ChatResponseUpdate> finish();

private:
    struct PendingToolCall {
        std::string id;
        std::string name;
        std::string arguments;
        bool seen = false;
    };

    // Emits the previously-held item (never final -- something came after it) and holds this one.
    // Gap-audit finding 20 / 003 §8 Q2: the single choke point every `ContentItem` this accumulator
    // produces passes through -- stamping `producer_chat_client_id` here once, rather than at each
    // construction site, mirrors `AnthropicChatClient::StreamingUpdateAccumulator::release()` exactly.
    //
    // 004 §1 amendment (`ChatResponseUpdate::continues_previous`): also the one place that knows what
    // was released immediately before this item. On this wire a choice has exactly ONE `content`
    // string and ONE reasoning string, each streamed as consecutive `delta` fragments, and tool calls
    // are only released from `finish()` after both. So a `Text` fragment released straight after a
    // `Text` fragment is the next piece of the same string, and likewise for `Reasoning` -- the
    // continuation is a fact of the wire format here, not a guess from adjacency. Anything else
    // (the first fragment, a switch between reasoning and text, a tool call) starts a new item.
    void release(std::vector<ChatResponseUpdate>* out, ContentItem item);

    // One SSE event block -> zero or more content items, updating tool-call accumulation state.
    // `chunk_out`: unified-streaming-design-draft.md §1 (Piece B) -- as `PendingToolCall::arguments`
    // grows, a companion `ChatResponseUpdate{.tool_call_argument_chunk = {...}}` is pushed directly
    // here (raw, unrepaired, best-effort display fragment), alongside (not instead of) continuing to
    // accumulate into `pending_by_index_` exactly as before. `finish()` is unchanged -- same one real
    // `json::parse`, same authoritative buffer, zero change to the dispatch-tier invariant.
    [[nodiscard]] std::vector<ContentItem> items_from_block(std::string const& block,
                                                              std::vector<ChatResponseUpdate>* chunk_out);

    bool chunked_;
    bool done_seen_ = false;
    std::string producer_chat_client_id_;
    sandbox::ChunkedBodyDecoder chunked_decoder_;
    sandbox::SseEventFramer framer_;
    std::vector<PendingToolCall> pending_by_index_;
    std::optional<ContentItem> held_;
    bool held_continues_previous_ = false;  // `continues_previous` for the update `held_` becomes
    std::optional<Usage> captured_usage_;  // ADR-034: from a stream_options.include_usage trailing chunk
};

// D2: the one-shot parse, for a body that genuinely is fully in hand. Implemented ON TOP of the
// incremental accumulator (ADR-019) rather than beside it: these two used to be one function, and
// keeping them as two independent implementations of the same wire contract would be exactly the
// drift this project's own conventions warn about. Same inputs, same outputs, one decoder.
[[nodiscard]] result<std::vector<ChatResponseUpdate>> parse_streaming_response_into_updates(
    std::string_view raw_body, bool is_chunked, std::string const& producer_chat_client_id = {});

// A testability seam, not a security bypass -- mirrors `provider_http_client.hpp`'s own injectable
// `resolver` parameter exactly, letting a test bind an arbitrary `Host:` name to the ephemeral
// loopback port its own server just opened without a real DNS lookup. Defaults to the real
// `sandbox::resolve_host` (ADR-016: the host-initiated provider resolver, which does real DNS and
// the resolve-once-connect-to-a-literal discipline but applies NO blocked-range filter -- a
// deployment's own llama.cpp/vLLM/Ollama endpoint on loopback or RFC 1918 is the ordinary case here,
// not an SSRF attempt; the guest path keeps `resolve_and_validate`). Production code never
// constructs `OpenAIChatClient` with a non-default resolver.
using Resolver = agentengine::detail::provider_wire::Resolver;

// The detached background worker (see file banner for why detached, not a tracked member). Every
// parameter is owned by value -- no reference back to the `OpenAIChatClient` instance that spawned it.
// `ca_bundle_pem_override` mirrors `perform_provider_https_exchange`'s own testability seam verbatim
// (empty in production -- the vendored CA bundle applies; a test's self-signed leaf isn't in it).
// `http_referer`/`x_title`/`end_user_id`/`seed`/`transport`/`session_id` are `OpenAIChatClient`'s own
// optional constructor fields, threaded through by value exactly like every other captured parameter
// here.
void run_stream_worker(std::string host, std::uint16_t port, std::string path, std::string api_key,
                       std::string model, ChatRequest request,
                       stream_producer<ChatResponseUpdate> producer, Resolver resolver,
                       std::string ca_bundle_pem_override, std::string http_referer,
                       std::string x_title, std::string end_user_id,
                       std::optional<std::int64_t> seed, sandbox::ProviderTransport transport,
                       std::stop_token stop, ChatClientCapabilities caps, std::string session_id);

}  // namespace detail

// The real, product-code `ChatClient` conformer for 004 §3's default OpenAI-compatible backend.
// `Store` is any real `SecretStore` (`AgentEngineSecretStore` in production; `InMemorySecretStore` in
// tests, matching `test_chat_client_credential_resolution.cpp`'s own pattern).
template <SecretStore Store>
class OpenAIChatClient {
public:
    // `http_referer`/`x_title`/`end_user_id`/`seed` (Milestone 5 research follow-up, docs/research/
    // 2026-08-07-provider-metadata-and-sampling-params-survey.md "Recommended design" items 1/2): ALL
    // optional, APPENDED after `ca_bundle_pem_override` -- never inserted earlier in this list. Every
    // existing construction call site in tests/protocol/openai/test_openai_chat_client_live.cpp uses positional
    // arguments; inserting a parameter anywhere but the end would silently misalign every one of them
    // (the same class of bug `Usage::cache_write_tokens`'s own placement note in core/content.hpp
    // documents, deliberately avoided here the same way). `transport` (ADR-016) is appended after all
    // of them for exactly the same reason.
    //
    // `transport` defaults to TLS and should stay there. `plaintext_http` is for a local llama.cpp/
    // vLLM/Ollama server that has no certificate to present -- on that transport the `Authorization:
    // Bearer` header this client sends is readable on the wire, which is fine on loopback and a
    // credential disclosure anywhere else. See sandbox/provider_http_client.hpp's `ProviderTransport`.
    OpenAIChatClient(std::string host, std::uint16_t port, std::string model, SecretRef api_key_ref,
                      ChatClientCapabilities caps, Store const& store, std::string path_prefix = "/v1",
                      detail::Resolver resolver = sandbox::resolve_host,
                      std::string ca_bundle_pem_override = {}, std::string http_referer = {},
                      std::string x_title = {}, std::string end_user_id = {},
                      std::optional<std::int64_t> seed = std::nullopt,
                      sandbox::ProviderTransport transport = sandbox::ProviderTransport::tls,
                      // ADR-023 Phase 1: off by default -- scanning `content` for raw response-format
                      // leaks (Harmony/DeepSeek/Hermes/`<think>`) is operator-armed, never
                      // content-triggered (the ADR's Finding 6). Appended last, same "never insert
                      // earlier" convention this constructor's own file-top comment already documents
                      // for every optional param above.
                      //
                      // OQ-23 (OpenQuestions.md), docs/planning/oq23-undeclared-tool-call-leak-design-
                      // draft.md: `AgentSession::run_model_call()` (rt/agent_session.hpp) refuses,
                      // rather than silently passing through, a raw wire-format leak matching a live
                      // tool name when `capabilities().tool_calling` is declared true and THIS flag is
                      // left at its default `false` for the endpoint. That protection is NOT available
                      // to a caller invoking `chat()` on this `OpenAIChatClient` directly, bypassing
                      // `AgentSession` -- a scope gap named, not fixed, by that design (§6): calling
                      // `chat()` here with `tool_calling` declared and this flag unarmed still returns
                      // an ordinary, unscanned response, exactly as before that design existed.
                      bool scan_response_format_leaks = false,
                      // docs/research/2026-08-21-openrouter-session-id-header.md: OpenRouter's own
                      // prompt-cache sticky-routing key, sent as the `x-session-id` header -- NOT
                      // `end_user_id` above, which that vendor does not use for cache routing at all.
                      // Appended last, same convention as every optional param above.
                      std::string session_id = {})
        : host_(std::move(host)),
          port_(port),
          model_(std::move(model)),
          api_key_ref_(std::move(api_key_ref)),
          capabilities_(caps),
          store_(store),
          path_prefix_(std::move(path_prefix)),
          resolver_(std::move(resolver)),
          ca_bundle_pem_override_(std::move(ca_bundle_pem_override)),
          http_referer_(std::move(http_referer)),
          x_title_(std::move(x_title)),
          end_user_id_(std::move(end_user_id)),
          seed_(seed),
          transport_(transport),
          scan_response_format_leaks_(scan_response_format_leaks),
          session_id_(std::move(session_id)) {}

    [[nodiscard]] ChatClientCapabilities capabilities() const { return capabilities_; }

    // Gap-audit finding 20 / 003 §8 Q2: identifies THIS bound instance, "vendor:model", the same
    // runtime string `parse_chat_completion_response`/`run_stream_worker` below stamp onto any real
    // `Reasoning` item this backend produces (issue #49: a vanilla Chat Completions response never
    // has one, but OpenRouter's own extension `reasoning`/`reasoning_content` field does) -- so
    // `AgentSession`'s cross-provider exclusion correctly recognizes and excludes an Anthropic-origin
    // (or a different OpenAI-compatible model's) `Reasoning` item once a session switches to this
    // backend. See `AnthropicChatClient::producer_chat_client_id()` for the full rationale this
    // mirrors.
    [[nodiscard]] std::string producer_chat_client_id() const { return "openai:" + model_; }

    [[nodiscard]] task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext& ctx) const {
        // Resolution happens HERE, inside chat(), against EffectContext -- never at construction
        // (004 §1 / 018 §4, the same rule test_chat_client_credential_resolution.cpp proves).
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) co_return std::unexpected(lease.error());

        auto body = detail::build_request_body(request, model_, /*stream=*/false, end_user_id_, seed_,
                                                 capabilities_);
        if (!body) co_return std::unexpected(body.error());

        auto req = detail::build_http_request(path_prefix_ + "/chat/completions", lease->reveal_text(),
                                                json::dump(*body), http_referer_, x_title_, session_id_);
        // ADR-206: the POST, the non-2xx mapping and the JSON parse are shared with the other backend.
        auto parsed = agentengine::detail::provider_wire::exchange_json("openai", host_, port_, req, resolver_,
                                                                        ca_bundle_pem_override_, transport_);
        if (!parsed) co_return std::unexpected(parsed.error());
        auto response = detail::parse_chat_completion_response(*parsed, producer_chat_client_id());
        if (!response) co_return std::unexpected(response.error());
        if (scan_response_format_leaks_) {
            response->message =
                agentengine::apply_response_format_scan(std::move(response->message), request.tools);
        }
        co_return response;
    }

    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest request, EffectContext& ctx) const {
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) {
            // Forward the SecretStore's own real error (already correctly classified, typically
            // failure_class::policy for a denied grant) instead of a synthetic stand-in -- matches
            // chat()'s own non-streaming path exactly, which already returns lease.error() unchanged.
            pair.producer.fail(lease.error());
            return std::move(pair.consumer);
        }
        // ADR-017: read the token BEFORE moving the producer into the thread. Argument evaluation
        // order is unspecified, so `pair.producer.stop_token()` written inline alongside
        // `std::move(pair.producer)` could legally run after the move -- on a moved-from producer,
        // whose stop_source is empty, yielding a token that never fires.
        std::stop_token stop = pair.producer.stop_token();
        std::thread(&detail::run_stream_worker, host_, port_, path_prefix_ + "/chat/completions",
                    lease->reveal_text(), model_, std::move(request), std::move(pair.producer), resolver_,
                    ca_bundle_pem_override_, http_referer_, x_title_, end_user_id_, seed_, transport_,
                    std::move(stop), capabilities_, session_id_)
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
    detail::Resolver resolver_;
    std::string ca_bundle_pem_override_;
    std::string http_referer_;
    std::string x_title_;
    std::string end_user_id_;
    std::optional<std::int64_t> seed_;
    sandbox::ProviderTransport transport_;
    bool scan_response_format_leaks_;
    std::string session_id_;
};

}  // namespace agentengine::openai

#endif  // AGENTENGINE_WITH_HTTPS
