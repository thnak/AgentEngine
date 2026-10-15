// Implements 004-Model-Provider-Plane.md §3/§4 -- ADR-206 (#120 S6): include/agentengine/protocol/
// provider_chat_wire.hpp, the HTTP plumbing shared by the OpenAI and Anthropic ChatClient backends.

#include "agentengine/protocol/provider_chat_wire.hpp"

#include <utility>

#include "sse_stream_pump.hpp"  // make_http_context (the TLS connector stays in src/)

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

task<result<rt::HttpResponse>> exchange_async(std::string host, std::uint16_t port, sandbox::NetEgressRequest req,
                                             ProviderReactorOptions reactor, std::string ca_bundle_pem_override,
                                             sandbox::ProviderTransport transport, std::stop_token stop) {
    bool const      https = transport != sandbox::ProviderTransport::plaintext_http;
    rt::HttpRequest hreq;
    hreq.scheme = https ? rt::http_scheme::https : rt::http_scheme::http;
    hreq.host   = std::move(host);
    hreq.port   = port;
    hreq.method = std::move(req.method);
    hreq.target = std::move(req.path);
    for (auto& h : req.headers) hreq.headers.push_back(std::move(h));
    hreq.body             = std::move(req.body);
    rt::HttpContext hctx  = make_http_context(reactor, ca_bundle_pem_override, https);
    rt::HttpResponse resp = co_await rt::http_request(hctx, std::move(hreq), stop);
    if (!resp.ok()) co_return std::unexpected(http_error_to_error(resp.error, resp.code, resp.message));
    co_return resp;
}

}  // namespace agentengine::detail::provider_wire
