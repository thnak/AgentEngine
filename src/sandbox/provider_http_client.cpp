// Implements provider_http_client.hpp.

#include "agentengine/sandbox/provider_http_client.hpp"

namespace agentengine::sandbox {

result<NetEgressResponse> perform_provider_https_exchange(
    std::string_view host, std::uint16_t port, NetEgressRequest const& req, std::stop_token stop,
    std::optional<std::uint64_t> byte_cap,
    std::function<result<VerifiedEndpoint>(std::string_view, std::uint16_t)> const& resolver,
    std::string_view ca_bundle_pem_override) {
    auto endpoint = resolver(host, port);
    if (!endpoint) return std::unexpected(endpoint.error());
    return perform_https_exchange(*endpoint, host, req, byte_cap, std::move(stop), ca_bundle_pem_override);
}

}  // namespace agentengine::sandbox
