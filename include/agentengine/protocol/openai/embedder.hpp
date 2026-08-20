#pragma once
// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.2 (the `Embedder`
// concept, core/embedder.hpp) and §2.5 (the concrete conformer): `OpenAIEmbedder`, one
// host-configurable class covering BOTH `api.openai.com` AND `api.openrouter.ai` -- exactly
// mirroring how `OpenAIChatClient` (protocol/openai/chat_client.hpp) already covers both for chat.
// There is no `OpenRouterEmbedder`; OpenRouter is "an OpenAI-compatible host," reached by passing a
// different `host`/`path_prefix` at construction, never by a code branch (ADR-063 §3 claim 4's own
// falsifiable wording: "host/path_prefix as constructor arguments only, no code branch").
//
// Same dependency posture as chat_client.hpp (CONVENTIONS.md's three-tier policy, this file being L4
// protocol code under `protocol/openai/`): guarded behind `AGENTENGINE_WITH_HTTPS`, reusing
// `sandbox::perform_provider_https_exchange` (Phase C's host-initiated HTTPS client) for the network
// exchange and the `SecretStore` seam (`trust/secret.hpp`) for outbound-credential resolution AT THE
// POINT OF USE, never at construction -- `OpenAIEmbedder` holds only a `SecretRef` member, the exact
// behavioral shape `OpenAIChatClient::chat()` already proves (chat_client.hpp:938-940,
// `test_chat_client_credential_resolution.cpp`) and ADR-063 §4's red-team pass explicitly confirmed
// "genuinely sound and factually accurate against real code" for this project's credential
// discipline -- not relitigated here, just followed.
//
// This file deliberately `#include`s chat_client.hpp (both in this SAME `agentengine::openai`
// namespace, same vendor, same OpenAI-compatible wire family -- unlike the openai/anthropic split,
// which duplicates their own one-shot chunked-body decoders because those two ARE genuinely
// different wire dialects) to reuse `detail::decoded_response_body()` and `detail::
// map_http_status_error()` verbatim rather than re-deriving them. `decoded_response_body()` exists
// specifically because a REAL OpenAI-compatible server (confirmed live against OpenRouter,
// chat_client.hpp's own comment) sends `Transfer-Encoding: chunked` on an ORDINARY, non-streaming
// POST response, not only on SSE -- the embeddings endpoint is the same host, same non-streaming
// response shape, so the identical decode step applies verbatim; duplicating that logic into a
// second copy would risk silently reintroducing the exact parse bug chat_client.hpp's own comment
// documents having hit once already. One implementation of the wire contract, not two that could
// drift -- the same principle this project's own files invoke repeatedly for exactly this reason.
//
// `EmbedderCapabilities` (dimensions/max_batch_size) is a CONSTRUCTOR ARGUMENT the caller supplies,
// exactly mirroring `OpenAIChatClient`'s own `ChatClientCapabilities caps` parameter -- declared, not
// probed (004 §2 / 004 §3's rule, restated for embeddings by core/embedder.hpp's own top comment and
// ADR-063 §2.5's last bullet). This file hardcodes NEITHER number: `docs/research/2026-08-19-
// embedding-provider-landscape.md` §3 (added alongside this file, sourced 2026-08-19) confirms
// OpenAI's own published limits (2048 items/request, 1536/3072-dim defaults for the two
// `text-embedding-3-*` models) but explicitly found NO published OpenRouter batch-size limit to cite
// -- baking either number into this class would mean guessing for the host that has no confirmed
// number, and hardcoding a value that is genuinely model/deployment-specific even for OpenAI (a
// caller pointed at a different model, a self-hosted OpenAI-compatible server, or a future model
// gets to declare what THAT endpoint actually enforces, the same reasoning `ChatClientCapabilities`
// already applies).
//
// `embed_batch()` rejects (contract failure, never silently truncates or splits) a request whose
// size exceeds a NONZERO declared `max_batch_size` -- ADR-063 §4 finding 9 ("no sub-batching story
// when ingestion exceeds max_batch_size... nothing says whether the mount-logic or each Embedder
// conformer owns splitting the request") is a named, still-open design gap this conformer does not
// silently resolve on its own by guessing an answer; it fails closed instead, leaving sub-batching to
// whatever future `CorpusSource`/mount-time ingestion logic ADR-063 §7 still owes.
//
// NO Recording/Replay determinism wrapper here, deliberately (ADR-063 §2.2A's named, accepted I5
// tradeoff, core/embedder.hpp's own top comment) -- do not add one; that decision was made in
// conversation and red-teamed, not merely defaulted.
//
// NO `HTTP-Referer`/`X-Title` attribution headers on this class (unlike `OpenAIChatClient`, which
// carries `http_referer_`/`x_title_`): neither OpenRouter's chat docs nor its embeddings docs
// (`docs/research/2026-08-19-embedding-provider-landscape.md` §1, fetched 2026-08-19) state that the
// embeddings endpoint reads or requires these headers, and inventing an unconfirmed wire behavior
// would violate CLAUDE.md's own citation rule ("do not assert what a protocol does from memory").
// ADR-063 §3 claim 4 does not require them either -- only that `OpenAIEmbedder` reach a real
// embedding through OpenRouter, which it does without them. If a real need for app-attribution on
// this endpoint turns up later, add the fields then, sourced, not now, speculatively.

#ifdef AGENTENGINE_WITH_HTTPS

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/embedder.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/sandbox/provider_http_client.hpp"
#include "agentengine/trust/secret.hpp"

namespace agentengine::openai {

namespace detail {

// One `POST /embeddings` request body: `{"model": "...", "input": ["...", "...", ...]}` -- the exact
// shape ADR-063 §2.5 names and `docs/research/2026-08-19-embedding-provider-landscape.md` §3 confirms
// against OpenAI's real API reference (`input` accepts a plain array of strings; no `dimensions`/
// `encoding_format` field is sent -- neither is asked for by this conformer's own surface, and
// omitting an optional field is always the safe default over guessing a value for it). `input` is
// sent as an array EVEN for a single string, matching `embed_batch()`'s own always-a-batch contract
// (core/embedder.hpp: "batch-only... no separate single-item embed()") -- there is no code path here
// that would ever want the bare-string form the wire format also permits.
[[nodiscard]] inline json::Value build_embeddings_request_body(std::vector<std::string> const& texts,
                                                                 std::string const& model) {
    std::vector<json::Value> input;
    input.reserve(texts.size());
    for (auto const& text : texts) input.push_back(json::Value::make_string(text));

    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("model", json::Value::make_string(model));
    obj.emplace_back("input", json::Value::make_array(std::move(input)));
    return json::Value::make_object(std::move(obj));
}

// Deliberately minimal compared to `OpenAIChatClient`'s own `build_http_request` -- no
// `HTTP-Referer`/`X-Title` parameters, per this file's own top-comment rationale for omitting them.
[[nodiscard]] inline sandbox::NetEgressRequest build_embeddings_http_request(std::string const& path,
                                                                               std::string const& api_key,
                                                                               std::string body) {
    sandbox::NetEgressRequest req;
    req.method = "POST";
    req.path = path;
    req.headers.emplace_back("Content-Type", "application/json");
    req.headers.emplace_back("Authorization", "Bearer " + api_key);
    req.body = std::move(body);
    return req;
}

// The real response shape (`docs/research/2026-08-19-embedding-provider-landscape.md` §3, fetched
// directly from OpenAI's current API reference, quoted verbatim there):
//   {"object":"list","data":[{"object":"embedding","index":0,"embedding":[<float>,...]},...],
//    "model":"...","usage":{"prompt_tokens":N,"total_tokens":N}}
// `expected_count` is `texts.size()` from the SAME call that built the request -- used two ways:
// (1) a contract check that every requested index actually came back (a provider returning fewer
// items than requested is a malformed response, not silently accepted as "whatever showed up"), and
// (2) to place each `data[i].embedding` at its OWN `data[i].index` rather than assuming array order
// matches request order -- the field exists on the wire specifically so a caller does not have to
// make that assumption, and getting this wrong would silently mis-align a chunk's text with a
// DIFFERENT chunk's vector, a wrong-content bug ADR-063 §2.4's own copy-at-mount design goes out of
// its way to avoid for the text side; the vector side deserves the identical care.
[[nodiscard]] inline result<std::vector<std::vector<float>>> parse_embeddings_response(
    json::Value const& body, std::size_t expected_count) {
    // Defensive, mirroring `parse_chat_completion_response`'s own defensive top-level `error` check:
    // a lenient proxy in front of the real backend (OpenRouter fronts several) could plausibly answer
    // 200 with an error envelope instead of a genuine 4xx/5xx -- `map_http_status_error` (called by
    // the caller for a non-2xx status) does not see this case at all.
    if (auto const* err = body.find("error")) {
        std::string msg = "unknown error";
        if (auto const* m = err->find("message"); m && m->is_string()) msg = m->as_string();
        return std::unexpected(
            error{failure_class::contract, "openai embeddings error: " + msg, "openai_embedder.error"});
    }

    json::Value const* data = body.find("data");
    if (!data || !data->is_array()) {
        return std::unexpected(
            error{failure_class::contract, "response has no 'data' array", "openai_embedder.no_data"});
    }

    std::vector<std::vector<float>> out(expected_count);
    std::vector<bool> filled(expected_count, false);

    for (json::Value const& item : data->as_array()) {
        json::Value const* idx = item.find("index");
        json::Value const* emb = item.find("embedding");
        if (!idx || !idx->is_number() || !emb || !emb->is_array()) {
            return std::unexpected(error{failure_class::contract,
                                          "a data[] item is missing a numeric 'index' or an array "
                                          "'embedding' field",
                                          "openai_embedder.malformed_item"});
        }
        auto const position = static_cast<std::size_t>(idx->as_number());
        if (position >= expected_count) {
            return std::unexpected(error{failure_class::contract,
                                          "data[].index is out of range of this request's own input "
                                          "count -- the response does not correspond to the request "
                                          "that produced it",
                                          "openai_embedder.index_out_of_range"});
        }

        std::vector<float> vector;
        vector.reserve(emb->as_array().size());
        for (json::Value const& component : emb->as_array()) {
            if (!component.is_number()) {
                return std::unexpected(error{failure_class::contract,
                                              "an embedding vector contains a non-numeric element",
                                              "openai_embedder.malformed_vector"});
            }
            vector.push_back(static_cast<float>(component.as_number()));
        }
        out[position] = std::move(vector);
        filled[position] = true;
    }

    for (bool was_filled : filled) {
        if (!was_filled) {
            return std::unexpected(error{failure_class::contract,
                                          "the response 'data' array did not cover every index this "
                                          "request asked for -- a partial embeddings response is "
                                          "treated as a failure, never a silently short result",
                                          "openai_embedder.incomplete_response"});
        }
    }
    return out;
}

}  // namespace detail

// `Store` is any real `SecretStore` (`AgentEngineSecretStore` in production; `InMemorySecretStore` in
// tests) -- the identical template shape `OpenAIChatClient<Store>` already uses.
template <SecretStore Store>
class OpenAIEmbedder {
public:
    // ADR-064 §2 fact 3, re-verified against this class's own embed_batch() body below every time it
    // changes: every step is a plain, synchronous, BLOCKING call (store_.resolve(), perform_provider_
    // https_exchange(), json::parse()) -- the only coroutine keyword used is co_return. This is a
    // STRONGER property than "only nested task<T> awaits" -- this body never suspends at all, by
    // inspection -- so driving it via rt::drive_leaf_task() is sound.
    static constexpr bool synchronous_leaf = true;


    // Parameter order mirrors `OpenAIChatClient`'s own constructor exactly (host/port/model/
    // SecretRef/capabilities/store/path_prefix/resolver/ca_bundle_pem_override/..., transport
    // appended last) minus the attribution/sampling fields that class carries and this one does not
    // (see this file's top comment) -- so a reader already familiar with `OpenAIChatClient`'s
    // construction site recognizes this one immediately, and so any FUTURE optional parameter this
    // class gains has an established "append after `ca_bundle_pem_override`, before `transport`"
    // slot to land in without reordering existing call sites, matching that file's own convention.
    OpenAIEmbedder(std::string host, std::uint16_t port, std::string model, SecretRef api_key_ref,
                   EmbedderCapabilities caps, Store const& store, std::string path_prefix = "/v1",
                   detail::Resolver resolver = sandbox::resolve_host,
                   std::string ca_bundle_pem_override = {},
                   sandbox::ProviderTransport transport = sandbox::ProviderTransport::tls)
        : host_(std::move(host)),
          port_(port),
          model_(std::move(model)),
          api_key_ref_(std::move(api_key_ref)),
          capabilities_(caps),
          store_(store),
          path_prefix_(std::move(path_prefix)),
          resolver_(std::move(resolver)),
          ca_bundle_pem_override_(std::move(ca_bundle_pem_override)),
          transport_(transport) {}

    [[nodiscard]] EmbedderCapabilities capabilities() const { return capabilities_; }

    [[nodiscard]] task<result<std::vector<std::vector<float>>>> embed_batch(
        std::vector<std::string> const& texts, EffectContext& ctx) const {
        // A zero-item batch performs no effect at all: no secret resolution, no network call. This is
        // not merely an optimization -- I2's "no ambient authority" posture is best satisfied by never
        // touching a capability a call has no actual use for, rather than resolving a credential that
        // would go completely unused.
        if (texts.empty()) co_return std::vector<std::vector<float>>{};

        if (capabilities_.max_batch_size != 0 && texts.size() > capabilities_.max_batch_size) {
            co_return std::unexpected(error{
                failure_class::contract,
                "embed_batch() called with " + std::to_string(texts.size()) +
                    " texts, exceeding this backend's declared max_batch_size (" +
                    std::to_string(capabilities_.max_batch_size) +
                    "); sub-batching a request that exceeds a declared provider limit is the "
                    "caller's own responsibility (ADR-063 §4 finding 9: an unresolved lifecycle gap "
                    "this conformer does not silently paper over by truncating or splitting on its "
                    "own)",
                "openai_embedder.batch_too_large"});
        }

        // Resolution happens HERE, inside embed_batch(), against EffectContext -- never at
        // construction (004 §1 / 018 §4, the same rule `OpenAIChatClient::chat()` follows,
        // chat_client.hpp:938-940, and ADR-063 §4's red-team pass confirmed sound for this project).
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) co_return std::unexpected(lease.error());

        auto body = detail::build_embeddings_request_body(texts, model_);
        auto req = detail::build_embeddings_http_request(path_prefix_ + "/embeddings",
                                                           lease->reveal_text(), json::dump(body));
        auto resp = sandbox::perform_provider_https_exchange(host_, port_, req, {}, std::nullopt,
                                                               resolver_, ca_bundle_pem_override_,
                                                               transport_);
        if (!resp) co_return std::unexpected(resp.error());

        // Reuses `OpenAIChatClient`'s own one-shot decoder -- see this file's top comment for why
        // that is a deliberate reuse, not an accidental cross-file coupling.
        auto decoded_body = detail::decoded_response_body(*resp);
        if (!decoded_body) co_return std::unexpected(decoded_body.error());
        if (resp->status < 200 || resp->status >= 300) {
            co_return std::unexpected(detail::map_http_status_error(resp->status, *decoded_body));
        }

        auto parsed = json::parse(*decoded_body);
        if (!parsed) co_return std::unexpected(parsed.error());
        co_return detail::parse_embeddings_response(*parsed, texts.size());
    }

private:
    std::string host_;
    std::uint16_t port_;
    std::string model_;
    SecretRef api_key_ref_;
    EmbedderCapabilities capabilities_;
    Store const& store_;
    std::string path_prefix_;
    detail::Resolver resolver_;
    std::string ca_bundle_pem_override_;
    sandbox::ProviderTransport transport_;
};

// `OpenAIEmbedder` is templated on `Store` (identical reason `OpenAIChatClient` is), so a bare
// `Embedder<OpenAIEmbedder>` cannot be written -- concept-checking a template requires a concrete
// instantiation. `InMemorySecretStore` (trust/secret.hpp) is a real, compiled `SecretStore`
// conformer (test-only, but structurally real, not a mock of the concept), the same choice
// `test_chat_client_credential_resolution.cpp` makes for the analogous `ChatClient` check -- this is
// this file's own version of `vector_index.hpp`'s bottom `static_assert` convention, adapted for a
// templated conformer rather than a concrete class.
static_assert(Embedder<OpenAIEmbedder<InMemorySecretStore>>,
              "OpenAIEmbedder must satisfy the real Embedder concept (ADR-063 §2.2/§2.5)");

}  // namespace agentengine::openai

#endif  // AGENTENGINE_WITH_HTTPS
