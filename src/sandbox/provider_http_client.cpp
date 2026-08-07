// Implements provider_http_client.hpp.

#include "agentengine/sandbox/provider_http_client.hpp"

namespace agentengine::sandbox {

result<NetEgressResponse> perform_provider_https_exchange(
    std::string_view host, std::uint16_t port, NetEgressRequest const& req, std::stop_token stop,
    std::optional<std::uint64_t> byte_cap,
    std::function<result<VerifiedEndpoint>(std::string_view, std::uint16_t)> const& resolver,
    std::string_view ca_bundle_pem_override, ProviderTransport transport) {
    auto endpoint = resolver(host, port);
    if (!endpoint) return std::unexpected(endpoint.error());
    // ADR-016 §3: the two transports differ only in the transport. Same request bytes, same
    // byte-cap-enforced read loop, same no-redirect posture, same stop_token cancellation --
    // `perform_http_exchange` and `perform_https_exchange` are deliberately structured that way
    // (net_egress_proxy.hpp's own note on the pair), so there is one branch here and nothing else.
    if (transport == ProviderTransport::plaintext_http) {
        return perform_http_exchange(*endpoint, host, req, byte_cap, std::move(stop));
    }
    return perform_https_exchange(*endpoint, host, req, byte_cap, std::move(stop), ca_bundle_pem_override);
}

}  // namespace agentengine::sandbox
