// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 ("DNS") -- the backend of
// `pal::resolve_host_blocking` (pal/resolver.hpp): one blocking getaddrinfo, mapped to numeric
// `pal::IpAddress` values. Lives with the reactor backend because it is the always-built pal backend that
// already links the platform socket library; it uses no Asio type. Called only from an offload worker
// (rt/dns.hpp), never from the reactor thread.

#include "agentengine/pal/resolver.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "agentengine/pal/net.hpp"  // the platform socket headers and ensure_winsock()

#if !defined(_WIN32)
#include <netdb.h>
#endif

namespace agentengine::pal {

namespace {

resolve_error classify(int rc) noexcept {
    switch (rc) {
        case EAI_NONAME: return resolve_error::not_found;
#if defined(EAI_NODATA) && (!defined(EAI_NONAME) || EAI_NODATA != EAI_NONAME)
        case EAI_NODATA: return resolve_error::not_found;
#endif
        case EAI_AGAIN: return resolve_error::temporary;
        case EAI_MEMORY: return resolve_error::resource;
#if defined(EAI_SYSTEM)
        case EAI_SYSTEM:
            return (errno == EMFILE || errno == ENFILE || errno == ENOMEM) ? resolve_error::resource
                                                                         : resolve_error::other;
#endif
        default: return resolve_error::other;
    }
}

}  // namespace

ResolveOutcome resolve_host_blocking(std::string const& host) noexcept {
    ResolveOutcome out;
    if (host.empty() || host.size() > 253 || host.find('\0') != std::string::npos) {
        out.error = resolve_error::invalid_argument;
        return out;
    }
    ensure_winsock();
    ::addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ::addrinfo* res   = nullptr;
    int const   rc    = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || res == nullptr) {
        out.error  = rc != 0 ? classify(rc) : resolve_error::not_found;
        out.native = rc;
        if (res != nullptr) ::freeaddrinfo(res);
        return out;
    }
    try {
        for (::addrinfo const* p = res; p != nullptr; p = p->ai_next) {
            IpAddress a;
            if (p->ai_family == AF_INET && p->ai_addr != nullptr && p->ai_addrlen >= sizeof(::sockaddr_in)) {
                ::sockaddr_in sin{};
                std::memcpy(&sin, p->ai_addr, sizeof(sin));
                std::memcpy(a.bytes.data(), &sin.sin_addr, 4);  // network order
            } else if (p->ai_family == AF_INET6 && p->ai_addr != nullptr &&
                       p->ai_addrlen >= sizeof(::sockaddr_in6)) {
                ::sockaddr_in6 sin6{};
                std::memcpy(&sin6, p->ai_addr, sizeof(sin6));
                a.family = IpAddress::family_kind::v6;
                std::memcpy(a.bytes.data(), &sin6.sin6_addr, 16);
                a.scope_id = sin6.sin6_scope_id;
            } else {
                continue;  // a family the TCP seam cannot connect to
            }
            if (std::find(out.addresses.begin(), out.addresses.end(), a) == out.addresses.end()) {
                out.addresses.push_back(a);
            }
        }
    } catch (...) {  // std::bad_alloc from push_back
        ::freeaddrinfo(res);
        out.addresses.clear();
        out.error = resolve_error::resource;
        return out;
    }
    ::freeaddrinfo(res);
    if (out.addresses.empty()) out.error = resolve_error::not_found;
    return out;
}

}  // namespace agentengine::pal
