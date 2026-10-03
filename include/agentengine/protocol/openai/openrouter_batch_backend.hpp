#pragma once

// decisions/ADR-235-batch-inference-coalescing.md §3.2: the OpenRouter conformer of the `BatchBackend`
// seam (core/batch_backend.hpp). OpenRouter's Batch API went GA on 2026-09-22
// (docs/research/2026-10-02-batch-inference-provider-limits.md); this speaks its `/api/v1/batches`
// surface with the OpenAI chat-completions item shape, reusing `openai::detail::build_request_body` /
// `parse_chat_completion_response` so a batched item is translated by the SAME code as the synchronous
// `OpenAIChatClient` call it replaces -- one wire translation, not two that could drift.
//
// Wire facts this file relies on (fetched 2026-10-02, https://openrouter.ai/docs/batch-quickstart.md):
//   - POST /api/v1/batches with `endpoint`, `model` serialized BEFORE `requests` (the API stream-parses
//     the body and returns 400 if `requests` comes first); one model per batch; 200 MB payload cap.
//   - GET /api/v1/batches/{id}: status validating -> in_progress -> finalizing -> completed; terminal:
//     completed, failed, expired, cancelled. `results` is inline and non-null ONLY for `completed`; each
//     result has exactly one of `response` (`{status_code, body}`) or `error`.
//   - A request failing OpenRouter's per-request checks fails the WHOLE batch after the 202.
//   - No cancel endpoint. DELETE /api/v1/batches/{id} purges a TERMINAL batch's stored inputs/results
//     (409 while in progress) -- that is `release()`.
//   - Unknown request parameters are silently DROPPED, and image/file parts must be public URLs.
//
// `admit()` is therefore deliberately narrow: only text content, no tools, no output schema, no
// reasoning effort, no client-interaction answers. Anything else runs synchronously, where the
// synchronous client's own capability gates apply -- never a batched request whose parameters the
// vendor might silently drop (ADR-235 §7). Widening this needs live proof per parameter.

#ifdef AGENTENGINE_WITH_HTTPS

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/batch_backend.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/protocol/provider_chat_wire.hpp"
#include "agentengine/sandbox/provider_http_client.hpp"
#include "agentengine/trust/secret.hpp"

namespace agentengine::openai {

namespace detail {

// Per OpenRouter's documented 413 ("Batch input exceeds the 200 MB payload limit").
inline constexpr std::size_t kOpenRouterBatchMaxPayloadBytes = 200ULL * 1024ULL * 1024ULL;
// OpenRouter documents no item cap; this is the engine's own ceiling, matching the smallest documented
// vendor cap (OpenAI/Groq/Together: 50,000) so a group is never one job of unbounded size.
inline constexpr std::size_t kOpenRouterBatchMaxItems = 50000;

// Refuses (contract) a request OpenRouter could reject or silently degrade; on success, the encoded
// size of one `{custom_id, body}` item (custom id budgeted at its longest engine-minted form).
[[nodiscard]] result<std::size_t> openrouter_batch_admit(ChatRequest const& request, std::string const& model);

[[nodiscard]] result<json::Value> build_openrouter_batch_body(std::string const& model,
                                                              std::vector<BatchItemRequest> const& items);

[[nodiscard]] result<std::string> parse_openrouter_batch_id(json::Value const& body);

[[nodiscard]] result<BatchPoll> parse_openrouter_batch_object(json::Value const& body,
                                                              std::string const& producer_chat_client_id);

[[nodiscard]] sandbox::NetEgressRequest openrouter_batch_request(std::string method, std::string path,
                                                                 std::string const& api_key, std::string body);

}  // namespace detail

template <SecretStore Store>
class OpenRouterBatchBackend final : public BatchBackend {  // ae-naming-lint: allow OpenRouterBatchBackend — ADR-235
public:
    OpenRouterBatchBackend(std::string host, std::uint16_t port, std::string model, SecretRef api_key_ref,
                           Store const& store, std::string path_prefix = "/api/v1",
                           detail::Resolver resolver = sandbox::resolve_host, std::string ca_bundle_pem_override = {},
                           sandbox::ProviderTransport transport = sandbox::ProviderTransport::tls)
        : host_(std::move(host)),
          port_(port),
          model_(std::move(model)),
          api_key_ref_(std::move(api_key_ref)),
          store_(store),
          path_prefix_(std::move(path_prefix)),
          resolver_(std::move(resolver)),
          ca_bundle_pem_override_(std::move(ca_bundle_pem_override)),
          transport_(transport) {}

    [[nodiscard]] std::string group_key() const override {
        // The credential REFERENCE is part of the key (ADR-235 §3.2): a restored run never polls a job
        // through a backend bound to a different secret. The account behind the reference is not fingerprinted
        // (ADR-235 §7 residual) -- resolving the secret here would need an EffectContext.
        return "openrouter:" + host_ + ":" + std::to_string(port_) + path_prefix_ + ":chat.completions:" + model_ +
               ":key=" + api_key_ref_.name;
    }

    [[nodiscard]] BatchLimits limits() const override {
        return BatchLimits{detail::kOpenRouterBatchMaxItems, detail::kOpenRouterBatchMaxPayloadBytes, 1};
    }

    [[nodiscard]] result<std::size_t> admit(ChatRequest const& request) const override {
        return detail::openrouter_batch_admit(request, model_);
    }

    [[nodiscard]] result<std::string> submit(std::vector<BatchItemRequest> const& items, EffectContext& ctx) override {
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) return std::unexpected(lease.error());
        auto body = detail::build_openrouter_batch_body(model_, items);
        if (!body) return std::unexpected(body.error());
        auto req = detail::openrouter_batch_request("POST", path_prefix_ + "/batches", lease->reveal_text(),
                                                    json::dump(*body));
        auto parsed = exchange(req);
        if (!parsed) return std::unexpected(parsed.error());
        return detail::parse_openrouter_batch_id(*parsed);
    }

    [[nodiscard]] result<BatchPoll> poll(std::string const& job_id, EffectContext& ctx) override {
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) return std::unexpected(lease.error());
        auto req = detail::openrouter_batch_request("GET", path_prefix_ + "/batches/" + job_id, lease->reveal_text(), {});
        auto parsed = exchange(req);
        if (!parsed) return std::unexpected(parsed.error());
        return detail::parse_openrouter_batch_object(*parsed, "openai:" + model_);
    }

    [[nodiscard]] result<void> cancel(std::string const&, EffectContext&) override {
        return std::unexpected(error{failure_class::contract,
                                     "OpenRouter's Batch API has no cancel endpoint; the job runs to its terminal "
                                     "state (release() can purge it afterwards)",
                                     "openrouter_batch.cancel_unsupported"});
    }

    [[nodiscard]] result<void> release(std::string const& job_id, EffectContext& ctx) override {
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) return std::unexpected(lease.error());
        auto req = detail::openrouter_batch_request("DELETE", path_prefix_ + "/batches/" + job_id, lease->reveal_text(), {});
        auto parsed = exchange(req);
        if (!parsed) return std::unexpected(parsed.error());
        return {};
    }

private:
    [[nodiscard]] result<json::Value> exchange(sandbox::NetEgressRequest const& req) const {
        return agentengine::detail::provider_wire::exchange_json("openrouter", host_, port_, req, resolver_,
                                                                 ca_bundle_pem_override_, transport_);
    }

    std::string host_;
    std::uint16_t port_;
    std::string model_;
    SecretRef api_key_ref_;
    Store const& store_;
    std::string path_prefix_;
    detail::Resolver resolver_;
    std::string ca_bundle_pem_override_;
    sandbox::ProviderTransport transport_;
};

}  // namespace agentengine::openai

#endif  // AGENTENGINE_WITH_HTTPS
