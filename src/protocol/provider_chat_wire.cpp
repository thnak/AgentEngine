// Implements 004-Model-Provider-Plane.md §3/§4 -- ADR-206 (#120 S6): include/agentengine/protocol/
// provider_chat_wire.hpp, the HTTP plumbing shared by the OpenAI and Anthropic ChatClient backends.

#include "agentengine/protocol/provider_chat_wire.hpp"

namespace agentengine::detail::provider_wire {

error http_status_error(std::string_view vendor, std::uint16_t status, std::string const& body) {
    failure_class klass = failure_class::fatal;
    if (status == 429 || status >= 500) {
        klass = failure_class::transient;  // 004 §4: retry applies to Transient only
    } else if (status == 401 || status == 403) {
        klass = failure_class::policy;
    } else if (status >= 400) {
        klass = failure_class::contract;
    }
    std::string message = std::string(vendor) + " http status " + std::to_string(status);
    if (auto parsed = json::parse(body); parsed) {
        if (auto const* err = parsed->find("error")) {
            if (auto const* m = err->find("message"); m && m->is_string()) message = m->as_string();
        }
    }
    return error{klass, message, std::string(vendor) + ".http_" + std::to_string(status)};
}

result<json::Value> exchange_json(std::string_view vendor, std::string_view host, std::uint16_t port,
                                  sandbox::NetEgressRequest const& req, Resolver const& resolver,
                                  std::string_view ca_bundle_pem_override, sandbox::ProviderTransport transport) {
    auto resp = sandbox::perform_provider_https_exchange(host, port, req, {}, std::nullopt, resolver,
                                                         ca_bundle_pem_override, transport);
    if (!resp) return std::unexpected(resp.error());
    if (resp->status < 200 || resp->status >= 300) {
        return std::unexpected(http_status_error(vendor, resp->status, resp->body));
    }
    return json::parse(resp->body);
}

}  // namespace agentengine::detail::provider_wire
