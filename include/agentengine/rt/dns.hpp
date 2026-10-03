#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 ("DNS: ... `rt::offload(getaddrinfo)`
// ... ADR-011's resolve-once/connect-to-verified-address rule is unchanged") and §4.6 ("DNS gets its own bounded
// pool"), with §4.4's cancellation rules -- asynchronous name resolution and a connect that resolves once,
// filters through a caller-supplied address policy, and connects only to an address that policy accepted:
//
//   co_await rt::resolve(dns_pool, host, port, stop)                       -> DnsResult (numeric addresses)
//   co_await rt::connect_resolved(reactor, dns_pool, host, port, policy, stop, options) -> ConnectedStream
//
// RESOLVE (§4.6, §6.3):
//   - A NUMERIC host ("127.0.0.1", "::1", "fe80::1%3") skips DNS entirely: `pal::parse_ip_address`, strict
//     inet_pton grammar, no offload, no thread hop. Host names are written without brackets.
//   - A host NAME runs `pal::resolve_host_blocking` (getaddrinfo) on the DNS `OffloadPool` the caller passes --
//     the dedicated, bounded pool of §4.6, never the general one. Its inputs are taken BY VALUE (the host
//     string is moved into the job), so the caller may resume `canceled` and unwind while the lookup runs.
//   - CANCELLATION carries §4.6's residual, as MAF states it for `asyncio.to_thread`: a stop resumes the
//     caller `canceled` promptly, on its home; a lookup already running in the pool cannot be interrupted, so
//     it finishes there and its answer is discarded (`OffloadPool::discarded_results()`). A lookup has no
//     side effect, so the residual is only the worker it occupies until getaddrinfo returns.
//   - Results resume on the caller's home (rt/offload.hpp's rules: ADR-175 `block_on` home or ADR-219 host
//     `Resumer`), never on the DNS worker.
//   - `HostResolver` is a TEST SEAM (a slow or scripted resolver), in the same spirit as ADR-011's injectable
//     `HostEgressProxy::resolver`: production callers leave it empty and get getaddrinfo. It is a value the
//     caller passes explicitly, never ambient state.
//
// CONNECT (ADR-011's rule, unchanged by ADR-237):
//   1. RESOLVE ONCE: one `rt::resolve`; nothing re-resolves the name afterwards, so no second, attacker-timed
//      lookup can sit between the check and the connect.
//   2. FILTER: the caller's `AddressPolicy` is asked about EVERY resolved address before any connect. This is
//      where ADR-011's blocked ranges (guest egress), ADR-016's provider policy, or any SSRF rule plugs in. It
//      is host code, never derived from model output (I3), and runs synchronously on the caller's home thread
//      (it must not block). FAIL CLOSED: no policy at all (an empty function) is `no_policy` before any lookup
//      -- there is no "allow everything" default (I2); a policy that throws counts as a rejection; and when no
//      address passes, nothing is connected (`every_address_rejected`).
//   3. CONNECT TO THE VERIFIED ADDRESS: the accepted addresses are tried IN THE RESOLVER'S ORDER, one at a
//      time, each on a fresh stream with its own per-attempt deadline (`ConnectOptions::attempt_timeout`); a
//      refused / unreachable / timed-out attempt moves to the next accepted address. There is NO happy-eyeballs
//      (RFC 8305) racing of v4 and v6: attempts are sequential, so a black-holed first address costs one
//      `attempt_timeout`. Racing is a later optimisation behind the same function.
//   The per-attempt deadline is a reactor timer that stops a stop_source private to the attempt (§4.4,
//   red-team M5: "each operation gets its own std::stop_source linked to the caller's token; the deadline timer
//   stops the operation's source"). The timer's completion does not request that stop itself: it POSTS the request
//   to the home of the coroutine that started the attempt (its strand, for engine work), where it runs
//   (`detail::HomedCall`, rt/strand_stop_callback.hpp) -- step 5 (§14) closing step 4's recorded deviation, which
//   requested it on the reactor thread before strands existed.
//   A caller stop resumes `canceled` (the in-flight connect is cancelled two-phase, rt/tcp.hpp); an attempt that
//   completed before its cancel landed is reported as connected (round-1 M1's real outcome).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/pal/reactor.hpp"
#include "agentengine/pal/reactor_tcp.hpp"
#include "agentengine/pal/resolver.hpp"
#include "agentengine/rt/offload.hpp"
#include "agentengine/rt/reactor_await.hpp"
#include "agentengine/rt/strand_stop_callback.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/tcp.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow dns_error — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class dns_error : std::uint8_t {
    none,              // at least one address
    canceled,          // the stop token was requested (or the DNS pool shut down) first
    not_found,         // the name does not exist or has no address
    temporary,         // the resolver could not answer now; retryable
    invalid_argument,  // an empty host, an embedded NUL, longer than 253 characters
    resource,          // the resolver ran out of memory or descriptors (a `resource` error, §6.3 G-g)
    other,             // anything else (including a resolver that threw)
};

// A blocking resolver, run on the DNS pool. Test seam only -- see the file comment.
// ae-naming-lint: allow HostResolver — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
using HostResolver = std::function<pal::ResolveOutcome(std::string const&)>;

// ae-naming-lint: allow DnsResult — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct DnsResult {
    dns_error                   error = dns_error::none;
    std::vector<pal::IpAddress> addresses;  // resolver order; non-empty iff ok()
    std::uint16_t               port    = 0;
    bool                        numeric = false;  // the host was a numeric address: no lookup happened
    int                         native  = 0;      // the resolver's own code, when there was one

    [[nodiscard]] bool ok() const noexcept { return error == dns_error::none; }
};

// Decides whether an address may be connected to. Host code, never model output (I3). See the file comment.
// ae-naming-lint: allow AddressPolicy — ADR-237 §6.3 / ADR-011: new reactor vocabulary, 027 §4 row added when the ADR is Judged
using AddressPolicy = std::function<bool(pal::IpAddress const&)>;

// ae-naming-lint: allow connect_error — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class connect_error : std::uint8_t {
    none,                    // connected
    canceled,                // the caller's stop (or a reactor / DNS pool shutdown)
    no_policy,               // no AddressPolicy was given: refused before any lookup (fail closed, I2)
    resolve_failed,          // the lookup failed; `ConnectedStream::dns` says how
    every_address_rejected,  // the policy accepted none of the resolved addresses; nothing was connected
    connect_failed,          // every accepted address was tried and failed; `attempts` says how
};

// ae-naming-lint: allow ConnectAttempt — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct ConnectAttempt {
    pal::IpAddress address;
    pal::tcp_error error     = pal::tcp_error::none;
    bool           timed_out = false;  // the per-attempt deadline ended it (`error` is then `canceled`)
};

// ae-naming-lint: allow ConnectOptions — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct ConnectOptions {
    std::chrono::milliseconds attempt_timeout{10'000};  // per address; <= 0: no per-attempt deadline
    HostResolver              resolver{};               // test seam; empty: getaddrinfo
};

// ae-naming-lint: allow ConnectedStream — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct ConnectedStream {
    connect_error                   error = connect_error::none;
    std::shared_ptr<pal::TcpStream> stream;   // connected iff ok()
    pal::IpAddress                  address;  // the verified address it is connected to (iff ok())
    std::uint16_t                   port = 0;
    DnsResult                       dns;            // what the single lookup returned
    std::size_t                     rejected = 0;   // resolved addresses the policy refused
    std::vector<ConnectAttempt>     attempts;       // in order, one per accepted address tried

    [[nodiscard]] bool ok() const noexcept { return error == connect_error::none; }
};

namespace dns_detail {

[[nodiscard]] constexpr dns_error from(pal::resolve_error e) noexcept {
    switch (e) {
        case pal::resolve_error::none: return dns_error::none;
        case pal::resolve_error::not_found: return dns_error::not_found;
        case pal::resolve_error::temporary: return dns_error::temporary;
        case pal::resolve_error::invalid_argument: return dns_error::invalid_argument;
        case pal::resolve_error::resource: return dns_error::resource;
        case pal::resolve_error::other: return dns_error::other;
    }
    return dns_error::other;
}

[[nodiscard]] inline bool valid_host(std::string const& host) noexcept {
    return !host.empty() && host.size() <= 253 && host.find('\0') == std::string::npos;
}

// The per-attempt deadline: a reactor timer whose expiry posts "stop the attempt's private stop_source" to the
// attempt's home (§4.4 M5; file comment). `arm()` runs on that home, before the timer starts.
class AttemptDeadline final : public pal::ReactorOp {
public:
    void arm() {
        call_ = detail::HomedCall::arm([source = source_]() mutable { source.request_stop(); });
    }
    void on_complete(pal::op_status status) noexcept override {
        if (status != pal::op_status::completed) return;  // cancelled (the attempt ended) or reactor shutdown
        fired_.store(true, std::memory_order_release);
        if (call_) call_->fire();  // reactor thread: only posts; the stop runs on the attempt's home
    }
    [[nodiscard]] bool            fired() const noexcept { return fired_.load(std::memory_order_acquire); }
    [[nodiscard]] std::stop_token token() const noexcept { return source_.get_token(); }
    [[nodiscard]] std::stop_source source() const noexcept { return source_; }

private:
    std::stop_source                   source_;
    std::atomic<bool>                  fired_{false};
    std::shared_ptr<detail::HomedCall> call_;  // set before the timer starts; read by on_complete only
};

// Links the caller's token to the attempt's source.
struct ForwardStop {
    std::stop_source target;
    void operator()() noexcept { target.request_stop(); }
};

// Cancels a started deadline timer however the attempt ends -- a frame destroyed mid-connect included -- so a
// timer never outlives its attempt by more than the cancel's post. Its completion touches only the op.
struct DeadlineGuard {
    pal::Reactor*                    reactor;
    std::shared_ptr<AttemptDeadline> op;
    bool                             started = false;
    DeadlineGuard(pal::Reactor* r, std::shared_ptr<AttemptDeadline> o) noexcept : reactor(r), op(std::move(o)) {}
    DeadlineGuard(DeadlineGuard const&)            = delete;
    DeadlineGuard& operator=(DeadlineGuard const&) = delete;
    ~DeadlineGuard() {
        if (started) reactor_detail::Canceler{reactor, op}();
    }
};

inline task<ConnectAttempt> connect_attempt(pal::Reactor* reactor, pal::TcpStream* stream, pal::IpAddress address,
                                            std::uint16_t port, std::stop_token caller,
                                            std::chrono::milliseconds limit) {
    auto                              deadline = std::make_shared<AttemptDeadline>();
    DeadlineGuard                     guard{reactor, deadline};
    std::stop_callback<ForwardStop> const link(caller, ForwardStop{deadline->source()});  // fires inline if stopped
    if (limit.count() > 0) {
        deadline->arm();  // on this coroutine's home (step 5): where the expiry's stop request will run
        reactor->start_timer(deadline, pal::Reactor::clock::now() + limit);
        guard.started = true;
    }
    TcpResult const r = co_await tcp_connect(*stream, address, port, deadline->token());
    ConnectAttempt  out;
    out.address   = address;
    out.error     = r.error;
    out.timed_out = r.error == pal::tcp_error::canceled && deadline->fired() && !caller.stop_requested();
    co_return out;
}

}  // namespace dns_detail

// Resolves `host` (see the file comment): a numeric host at once, a name on `dns_pool`. `port` is carried into
// the result. `resolver` is the test seam; leave it empty.
[[nodiscard]] inline task<DnsResult> resolve(OffloadPool& dns_pool, std::string host, std::uint16_t port,
                                             std::stop_token stop = {}, HostResolver resolver = {}) {
    DnsResult r;
    r.port = port;
    if (stop.stop_requested()) {
        r.error = dns_error::canceled;
        co_return r;
    }
    if (auto const numeric = pal::parse_ip_address(host)) {
        r.numeric = true;
        r.addresses.push_back(*numeric);
        co_return r;
    }
    if (!dns_detail::valid_host(host)) {
        r.error = dns_error::invalid_argument;
        co_return r;
    }
    if (!resolver) resolver = [](std::string const& h) { return pal::resolve_host_blocking(h); };
    // By value (§4.6): the resolver and the host are moved into the job; nothing in this frame is referenced.
    offload_result<pal::ResolveOutcome> o = co_await offload(dns_pool, stop, std::move(resolver), std::move(host));
    switch (o.status) {
        case offload_status::canceled: r.error = dns_error::canceled; co_return r;
        case offload_status::faulted: r.error = dns_error::other; co_return r;
        case offload_status::completed: break;
    }
    r.error     = dns_detail::from(o.value->error);
    r.native    = o.value->native;
    r.addresses = std::move(o.value->addresses);
    if (r.ok() && r.addresses.empty()) r.error = dns_error::not_found;
    co_return r;
}

// Resolve once, filter through `policy`, connect to a verified address (see the file comment).
[[nodiscard]] inline task<ConnectedStream> connect_resolved(pal::Reactor& reactor, OffloadPool& dns_pool,
                                                            std::string host, std::uint16_t port,
                                                            AddressPolicy policy, std::stop_token stop = {},
                                                            ConnectOptions options = {}) {
    ConnectedStream out;
    out.port = port;
    if (!policy) {  // fail closed before any lookup (I2: no implicit "allow everything")
        out.error = connect_error::no_policy;
        co_return out;
    }
    out.dns = co_await resolve(dns_pool, std::move(host), port, stop, std::move(options.resolver));
    if (!out.dns.ok()) {
        out.error = out.dns.error == dns_error::canceled ? connect_error::canceled : connect_error::resolve_failed;
        co_return out;
    }
    std::vector<pal::IpAddress> accepted;
    for (pal::IpAddress const& a : out.dns.addresses) {
        bool allow = false;
        try {
            allow = policy(a);
        } catch (...) {  // a policy that throws rejects (fail closed)
            allow = false;
        }
        if (allow) {
            accepted.push_back(a);
        } else {
            ++out.rejected;
        }
    }
    if (accepted.empty()) {
        out.error = connect_error::every_address_rejected;
        co_return out;
    }
    for (pal::IpAddress const& a : accepted) {
        if (stop.stop_requested()) {
            out.error = connect_error::canceled;
            co_return out;
        }
        std::shared_ptr<pal::TcpStream> s = pal::make_tcp_stream(reactor);
        ConnectAttempt const at =
            co_await dns_detail::connect_attempt(&reactor, s.get(), a, port, stop, options.attempt_timeout);
        out.attempts.push_back(at);
        if (at.error == pal::tcp_error::none) {
            out.stream  = std::move(s);
            out.address = a;
            out.error   = connect_error::none;
            co_return out;
        }
        if (at.error == pal::tcp_error::canceled && !at.timed_out) {  // the caller's stop, or a reactor shutdown
            out.error = connect_error::canceled;
            co_return out;
        }
    }
    out.error = connect_error::connect_failed;
    co_return out;
}

}  // namespace agentengine::rt
