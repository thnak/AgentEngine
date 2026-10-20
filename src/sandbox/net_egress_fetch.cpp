// Implements decisions/ADR-241-egress-proxy-async-path-on-rt-http.md: the egress proxy's async path on `rt::http` -- the
// one `fetch` both entry points run (D1 control value, D2 gate, D5 deadline, D6 TLS, D8 the one process-wide I/O context,
// 8.3 the synchronous entry as spawn + latch). The gate, the address policy, the deadline arithmetic and the error table
// are net_egress_proxy.cpp's; this file is the I/O.

#include <algorithm>
#include <chrono>
#include <exception>
#include <latch>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>

#include "agentengine/rt/await_chain.hpp"
#include "agentengine/rt/http.hpp"
#include "agentengine/rt/runtime.hpp"
#include "agentengine/sandbox/detail/net_egress_impl.hpp"
#include "agentengine/sandbox/io_timeout.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"

#ifdef AGENTENGINE_WITH_HTTPS
#include "agentengine/rt/http_tls.hpp"
#endif

namespace agentengine::sandbox::egress_detail {

namespace {

// D8: ONE process-wide egress I/O context -- a leaked 2-lane `rt::Runtime` with its OWN reactor and its OWN DNS pool,
// created lazily once and NEVER destroyed, never joined (not by an engine thread, not at static destruction): the join a
// `~Runtime` does (a running `getaddrinfo`, the lanes, the reactor) must never land on a host lane. It is deliberately
// not the provider plane's runtime or pool, so a stalled egress lookup cannot starve a model call (R2-5). The lanes exist
// because the synchronous entry needs a coroutine home that is not the blocked caller (`Runtime::spawn` is the only
// non-blocking root launcher); async callers resume on their own strand and never use them. Two, not one, so a flood of
// synchronous-edge roots parsing up to 16 MiB each does not queue every continuation (a cancel included) behind one lane.
// Documented leak (R5): reachable from a static pointer, so LSan stays quiet.
struct EgressIoContext {
    rt::Runtime runtime;
    explicit EgressIoContext(rt::RuntimeConfig config) : runtime(std::move(config)) {}
};

std::mutex                 g_context_mutex;
EgressIoContext*           g_context = nullptr;  // intentionally leaked
std::atomic<std::uint64_t> g_context_constructions{0};
std::atomic<int>           g_inject_failures{0};

// Returns the context, creating it on first use. A failed construction (thread spawn, memory) leaves the pointer null so
// the next call retries; the caller gets `resource` / `net.egress_unavailable`.
[[nodiscard]] EgressIoContext* io_context(error* unavailable) {
    std::lock_guard<std::mutex> lock(g_context_mutex);
    if (g_context != nullptr) return g_context;
    try {
        if (g_inject_failures.load(std::memory_order_relaxed) > 0) {
            g_inject_failures.fetch_sub(1, std::memory_order_relaxed);
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                    "injected egress context construction failure");
        }
        rt::RuntimeConfig config;
        config.lanes           = 2;
        config.offload_workers = 1;
        config.dns_workers     = 2;
        g_context              = new EgressIoContext(std::move(config));
        g_context_constructions.fetch_add(1, std::memory_order_relaxed);
        return g_context;
    } catch (...) {
        g_context = nullptr;
        *unavailable = error{failure_class::resource, "the egress I/O context could not be created",
                             "net.egress_unavailable"};
        return nullptr;
    }
}

[[nodiscard]] error canceled_error() {
    return error{failure_class::canceled, "cancelled via stop_token", "net.cancelled"};
}

}  // namespace

std::uint64_t egress_context_constructions() noexcept {
    return g_context_constructions.load(std::memory_order_relaxed);
}
void egress_inject_context_failures(int count) noexcept { g_inject_failures.store(count, std::memory_order_relaxed); }

task<result<NetEgressResponse>> egress_fetch(NetEgressRequest req, cap::NetOut granted, EgressCallControl control,
                                             EgressSeams seams, std::chrono::milliseconds total_timeout,
                                             GateObserver* observer) {
    // 1. The gate -- first, before anything else can wait, resolve or charge a bucket. A rejected call has touched
    //    nothing: no I/O context, no resolver, no socket (G13b).
    if (observer != nullptr) observer->gate_calls.fetch_add(1, std::memory_order_relaxed);
    auto const target = egress_gate(req, granted);
    if (!target) co_return std::unexpected(target.error());

    // 2. The deadline (D5): the configured total narrowed by the call's own deadline. An expired one fails here.
    auto const budget = effective_total_timeout(total_timeout, control.deadline, std::chrono::steady_clock::now());
    if (!budget) {
        co_return std::unexpected(error{failure_class::resource, "the call's deadline had already passed",
                                        "net.deadline_exceeded"});
    }

    // 3. The stop (D1): the caller's token AND the root's. `rt::scope_stop_token()` is what Runtime shutdown requests and
    //    what a `when_all` sibling failure requests; neither reaches `ctx.cancellation`. `merged` is what rt::http gets.
    std::stop_source merged;
    std::stop_callback const on_caller(control.stop, [&merged] { merged.request_stop(); });
    std::stop_callback const on_scope(rt::scope_stop_token(), [&merged] { merged.request_stop(); });
    if (merged.stop_requested()) co_return std::unexpected(canceled_error());

    // 4. The I/O context -- fetched before the first co_await (D8).
    error           unavailable;
    EgressIoContext* const io = io_context(&unavailable);
    if (io == nullptr) co_return std::unexpected(unavailable);

    // 5. The request, bounded (D4): the byte cap is the BODY's wire bytes; heads share min(64 KiB, cap) so a head larger
    //    than a tiny cap fails while it is being read; the request-level total derives from both (ADR-241 S1).
    std::uint64_t const cap =
        std::min<std::uint64_t>(granted.byte_cap.value_or(kHardResponseCeilingBytes), kHardResponseCeilingBytes);
    rt::HttpContext ctx;
    ctx.reactor  = &io->runtime.reactor();
    ctx.dns_pool = &io->runtime.dns_pool();
    // Whatever policy the seam carries is narrowed to IPv4 unless a test opted into v6 by name; an empty one stays empty
    // (fails closed, no_policy, before any lookup).
    if (seams.address_policy && !seams.allow_non_v4) {
        ctx.policy = [inner = std::move(seams.address_policy)](pal::IpAddress const& a) {
            return a.family == pal::IpAddress::family_kind::v4 && inner(a);
        };
    } else {
        ctx.policy = std::move(seams.address_policy);
    }
#ifdef AGENTENGINE_WITH_HTTPS
    ctx.tls = rt::tls_connector({.ca_bundle_pem_override = std::move(seams.tls_ca_override)});
#endif
    ctx.options.total_timeout            = *budget;
    ctx.options.idle_timeout             = std::chrono::milliseconds(io_timeout_ms());
    ctx.options.limits.max_header_bytes  = static_cast<std::size_t>(std::min<std::uint64_t>(64u * 1024u, cap));
    ctx.options.limits.max_body_bytes    = cap;
    ctx.options.connect.resolver         = std::move(seams.host_resolver);

    rt::HttpRequest request;
    request.scheme  = target->https ? rt::http_scheme::https : rt::http_scheme::http;
    request.host    = target->host;  // the allowlisted NAME: what TLS verifies, never an address
    request.port    = target->port;
    request.method  = std::move(req.method);
    request.target  = std::move(req.path);
    request.headers = std::move(req.headers);
    request.body    = std::move(req.body);  // moved, not copied, down the chain (R3-9)

    rt::HttpResponse response = co_await rt::http_request(std::move(ctx), std::move(request), merged.get_token());
    if (observer != nullptr) observer->last_wire_bytes.store(response.wire_bytes, std::memory_order_relaxed);
    if (!response.ok()) co_return std::unexpected(egress_error(response));

    NetEgressResponse out;
    out.status  = response.status;
    out.headers = std::move(response.headers);
    out.body    = std::move(response.body);
    co_return out;
}

namespace {

// What the synchronous entry waits on. Shared with the root so neither side's lifetime matters to the other.
struct BlockingSlot {
    std::optional<result<NetEgressResponse>> value;
    std::latch                               done{1};
};

// The root `egress_fetch_blocking` spawns. A free coroutine over by-value parameters (ADR-237 §14: no lambda
// coroutines with captures). The latch is counted down as the LAST statement, and also when the fetch throws (an
// uncounted latch would park the WASM thread forever).
task<void> blocking_root(task<result<NetEgressResponse>> inner, std::shared_ptr<BlockingSlot> slot) {
    try {
        slot->value = co_await std::move(inner);
    } catch (...) {
        slot->value = std::unexpected(error{failure_class::fatal, "the egress fetch threw an unexpected exception",
                                            "net.egress_faulted"});
    }
    slot->done.count_down();
}

}  // namespace

result<NetEgressResponse> egress_fetch_blocking(NetEgressRequest req, cap::NetOut granted, EgressCallControl control,
                                                EgressSeams seams, std::chrono::milliseconds total_timeout,
                                                GateObserver* observer) {
    error            unavailable;
    EgressIoContext* const io = io_context(&unavailable);
    if (io == nullptr) return std::unexpected(unavailable);

    auto slot = std::make_shared<BlockingSlot>();
    std::stop_token const caller_stop = control.stop;
    rt::RootHandle handle;
    try {
        // `fetch` itself merges rt::scope_stop_token() -- the root's -- so `handle.request_stop()` below reaches the
        // in-flight operation. Spawn FIRST, then register the stop callback: a stop already requested fires inline on
        // registration, which is correct, but only once `handle` exists.
        handle = io->runtime.spawn(blocking_root(
            egress_fetch(std::move(req), std::move(granted), std::move(control), std::move(seams), total_timeout,
                         observer),
            slot));
    } catch (...) {
        return std::unexpected(error{failure_class::resource, "the egress I/O context could not start the request",
                                     "net.egress_unavailable"});
    }
    {
        std::stop_callback const forward(caller_stop, [&handle] { handle.request_stop(); });
        // The calling thread parks here exactly as the old blocking exchange parked it (ADR-241 8.3, R3-4): nothing the
        // root needs runs on this thread, so there is no lane deadlock, and the wait is bounded by
        // min(total_timeout, control.deadline) plus the stop above.
        slot->done.wait();
    }
    return std::move(*slot->value);
}

}  // namespace agentengine::sandbox::egress_detail

namespace agentengine::sandbox {

task<result<NetEgressResponse>> HostEgressProxy::fetch(NetEgressRequest req, cap::NetOut granted,
                                                       EgressCallControl control) const {
    return egress_detail::egress_fetch(std::move(req), std::move(granted), std::move(control),
                                       egress_detail::production_seams(), total_timeout, nullptr);
}

result<NetEgressResponse> HostEgressProxy::fetch_blocking(NetEgressRequest req, cap::NetOut granted,
                                                          EgressCallControl control) const {
    return egress_detail::egress_fetch_blocking(std::move(req), std::move(granted), std::move(control),
                                                egress_detail::production_seams(), total_timeout, nullptr);
}

}  // namespace agentengine::sandbox
