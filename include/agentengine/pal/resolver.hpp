#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 ("DNS") and §4.6 -- the
// BLOCKING name-resolution primitive the async resolver (rt/dns.hpp) runs on its dedicated DNS offload pool.
// It is the one place a host name becomes addresses; everything above it handles numeric `pal::IpAddress`
// values only (ADR-011: resolve once, verify, connect to the verified address).
//
// Std-only, like the rest of pal/: the OS call (`getaddrinfo`) is in the backend,
// src/backends/reactor_asio/resolver_getaddrinfo.cpp.
//
// RULES:
//   - BLOCKS. Never call it on a lane, the reactor thread or a host UI thread: `rt::resolve` runs it on the
//     DNS `OffloadPool` (§4.6: its own bounded pool, so a burst of ~30 s glibc lookups cannot stall file I/O).
//   - ONE LOOKUP, NO POLICY. It returns every address the system resolver produced, in the resolver's order
//     (RFC 6724 sorting on both OSes), de-duplicated, IPv4 and IPv6. Filtering -- ADR-011's blocked ranges,
//     ADR-016's provider path -- is the caller's (rt::connect_resolved's address policy).
//   - Windows: `GetAddrInfoExW` with overlapped completion would make this truly asynchronous (§6.3); not done
//     yet -- both OSes go through the offload pool, with §4.6's residual (a canceled lookup still finishes).

#include <cstdint>
#include <string>
#include <vector>

#include "agentengine/pal/reactor_tcp.hpp"

namespace agentengine::pal {

// ae-naming-lint: allow resolve_error — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class resolve_error : std::uint8_t {
    none,              // at least one address
    not_found,         // the name does not exist, or has no address (EAI_NONAME / EAI_NODATA / WSAHOST_NOT_FOUND)
    temporary,         // the resolver could not answer now (EAI_AGAIN / WSATRY_AGAIN): retryable
    invalid_argument,  // an empty name, an embedded NUL, longer than 253 characters
    resource,          // out of memory or descriptors inside the resolver (EAI_MEMORY, EAI_SYSTEM+EMFILE)
    other,             // anything else; `native` carries the resolver's code
};

// ae-naming-lint: allow ResolveOutcome — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct ResolveOutcome {
    resolve_error          error = resolve_error::none;
    std::vector<IpAddress> addresses;  // non-empty iff error == none
    int                    native = 0;  // the getaddrinfo return code, when there was one
};

// One blocking `getaddrinfo(host, AF_UNSPEC, SOCK_STREAM)`. Never throws. Defined by the backend.
[[nodiscard]] ResolveOutcome resolve_host_blocking(std::string const& host) noexcept;

}  // namespace agentengine::pal
