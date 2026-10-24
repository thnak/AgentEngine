// Implements decisions/archive/ADR-241-egress-proxy-async-path-on-rt-http.md: the egress proxy's async path on `rt::http` -- the
// one `fetch` both entry points run (D1 control value, D2 gate, D5 deadline, D6 TLS, D8 the one process-wide I/O context,
// 8.3 the synchronous entry as spawn + latch). The gate, the address policy, the deadline arithmetic and the error table
// are net_egress_proxy.cpp's; this file is the I/O.

#include <algorithm>
#include <chrono>
#include <exception>
#include <latch>
#include <map>
#include <tuple>
#include "agentengine/rt/sleep.hpp"
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


// ---------------------------------------------------------------------------------------------------------------
// Admission (ADR-241 8.2). ONE std::mutex guards the global count, the two unattributed counters and both maps, and all
// three levels are admitted in ONE step, all-or-nothing: nothing is incremented until every level has been checked, so
// there is no run-level increment to roll back and no transient over-count other callers could mistake for "busy".
// The primitive is a plain `std::mutex` + `std::map` + atomics for the limits -- the codebase has no semaphore or
// keyed-bucket primitive and a counting semaphore cannot express three levels in one atomic step. The mutex is never held
// across a `co_await` (try_admit/release are straight-line and short); the bounded wait is the caller's loop around it.
// The leaked singleton (like the I/O context) means a slot released during process teardown still has a live object.
// ---------------------------------------------------------------------------------------------------------------
using RunKey       = std::tuple<std::string, std::string, std::string>;  // (tenant, root_id, root_run_id)
using PrincipalKey = std::tuple<std::string, std::string>;               // (tenant, root_id)

// What a slot owns: COPIES of the keys, never map iterators (a limit lowered at run time, or a map rebalance, cannot
// strand it). Release looks the entries up again under the lock.
struct HeldAdmission {
    bool         active           = false;
    bool         run_unattributed = false;
    bool         pr_unattributed  = false;
    RunKey       run;
    PrincipalKey principal;
};

class Admission {
public:
    [[nodiscard]] bool try_admit(AdmissionKey const& key, HeldAdmission* out) {
        auto&                        limits = egress_limits();
        std::lock_guard<std::mutex>  lock(mutex_);
        bool const run_unattr = key.root_run.empty();
        bool const pr_unattr  = key.principal.empty();
        RunKey const       run_key{key.tenant, key.principal, key.root_run};
        PrincipalKey const pr_key{key.tenant, key.principal};

        if (global_ >= limits.global.load(std::memory_order_relaxed)) return false;
        std::uint32_t run_count = unattributed_run_;
        if (!run_unattr) {
            auto const it = runs_.find(run_key);
            run_count     = it == runs_.end() ? 0 : it->second;
        }
        if (run_count >= (run_unattr ? limits.unattributed_run.load(std::memory_order_relaxed)
                                     : limits.per_run.load(std::memory_order_relaxed))) {
            return false;
        }
        std::uint32_t pr_count = unattributed_principal_;
        if (!pr_unattr) {
            auto const it = principals_.find(pr_key);
            pr_count      = it == principals_.end() ? 0 : it->second;
        }
        if (pr_count >= (pr_unattr ? limits.unattributed_principal.load(std::memory_order_relaxed)
                                   : limits.per_principal.load(std::memory_order_relaxed))) {
            return false;
        }
        // Every level has room: admit all three together. The only operations that can throw (the two map node
        // allocations) run FIRST and touch no counter; if the second throws, the first node is erased again, so a
        // `bad_alloc` here leaves no leaked slot and no empty entry (red-team r5 m1). After this block nothing can throw.
        std::map<RunKey, std::uint32_t>::iterator       run_it;
        std::map<PrincipalKey, std::uint32_t>::iterator pr_it;
        bool                                            run_inserted = false;
        if (!run_unattr) std::tie(run_it, run_inserted) = runs_.try_emplace(run_key, 0u);
        if (!pr_unattr) {
            try {
                pr_it = principals_.try_emplace(pr_key, 0u).first;
            } catch (...) {
                if (run_inserted) runs_.erase(run_it);
                throw;
            }
        }
        ++global_;
        if (run_unattr) ++unattributed_run_; else ++run_it->second;
        if (pr_unattr) ++unattributed_principal_; else ++pr_it->second;
        out->active           = true;
        out->run_unattributed = run_unattr;
        out->pr_unattributed  = pr_unattr;
        out->run              = run_key;
        out->principal        = pr_key;
        return true;
    }

    void release(HeldAdmission const& held) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (global_ > 0) --global_;
        if (held.run_unattributed) {
            if (unattributed_run_ > 0) --unattributed_run_;
        } else if (auto const it = runs_.find(held.run); it != runs_.end()) {
            if (--it->second == 0) runs_.erase(it);  // erase-when-zero, under the same lock that admits
        }
        if (held.pr_unattributed) {
            if (unattributed_principal_ > 0) --unattributed_principal_;
        } else if (auto const it = principals_.find(held.principal); it != principals_.end()) {
            if (--it->second == 0) principals_.erase(it);
        }
    }

    [[nodiscard]] AdmissionSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return AdmissionSnapshot{global_, runs_.size(), principals_.size(), unattributed_run_, unattributed_principal_};
    }

private:
    mutable std::mutex                     mutex_;
    std::uint32_t                          global_                 = 0;
    std::uint32_t                          unattributed_run_       = 0;
    std::uint32_t                          unattributed_principal_ = 0;
    std::map<RunKey, std::uint32_t>        runs_;
    std::map<PrincipalKey, std::uint32_t>  principals_;
};

Admission& admission() {
    static Admission* const instance = new Admission();  // intentionally leaked, like the I/O context
    return *instance;
}

// RAII: releases on every exit, including a coroutine frame dropped while parked.
class AdmissionSlot {
public:
    AdmissionSlot() = default;
    explicit AdmissionSlot(HeldAdmission h) : held_(std::move(h)) {}
    AdmissionSlot(AdmissionSlot&& o) noexcept : held_(std::move(o.held_)) { o.held_.active = false; }
    AdmissionSlot& operator=(AdmissionSlot&&) = delete;
    AdmissionSlot(AdmissionSlot const&)            = delete;
    AdmissionSlot& operator=(AdmissionSlot const&) = delete;
    ~AdmissionSlot() {
        if (held_.active) admission().release(held_);
    }
    void adopt(HeldAdmission h) noexcept { held_ = std::move(h); }
    [[nodiscard]] bool active() const noexcept { return held_.active; }

private:
    HeldAdmission held_;
};

[[nodiscard]] error busy_error() {
    return error{failure_class::resource, "egress is busy; retry after a short delay", "net.egress_busy"};
}
}  // namespace

AdmissionSnapshot egress_admission_snapshot() { return admission().snapshot(); }

std::shared_ptr<void> egress_admission_try(AdmissionKey const& key) {
    HeldAdmission held;
    if (!admission().try_admit(key, &held)) return nullptr;
    return std::shared_ptr<void>(new AdmissionSlot(std::move(held)), [](void* p) { delete static_cast<AdmissionSlot*>(p); });
}

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
    co_return co_await egress_fetch_gated(std::move(req), *target, std::move(granted), std::move(control), std::move(seams),
                                          total_timeout, observer);
}

task<result<NetEgressResponse>> egress_fetch_gated(NetEgressRequest req, GatedEgressTarget target, cap::NetOut granted,
                                                   EgressCallControl control, EgressSeams seams,
                                                   std::chrono::milliseconds total_timeout, GateObserver* observer) {
    (void)observer;
    (void)granted;

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

    // 4b. Admission (ADR-241 8.2): AFTER the gate (a rejected call never waits or charges a bucket) and BEFORE resolve.
    //     All three levels are admitted in one step; on exhaustion wait up to 1 s (capped by the call's deadline and stop),
    //     retrying every 25 ms on the egress reactor, then `net.egress_busy`. The slot is held until this frame ends.
    auto const started = std::chrono::steady_clock::now();
    AdmissionSlot slot;
    {
        HeldAdmission held;
        if (admission().try_admit(control.admission, &held)) {
            slot.adopt(std::move(held));
        } else {
            auto wait_end = started + std::chrono::seconds(1);
            if (control.deadline != std::chrono::steady_clock::time_point{} && control.deadline < wait_end) {
                wait_end = control.deadline;
            }
            for (;;) {
                if (std::chrono::steady_clock::now() >= wait_end) {
                    if (control.deadline != std::chrono::steady_clock::time_point{} &&
                        std::chrono::steady_clock::now() >= control.deadline) {
                        co_return std::unexpected(error{failure_class::resource, "the call's deadline passed while waiting for egress admission", "net.deadline_exceeded"});
                    }
                    co_return std::unexpected(busy_error());
                }
                auto const status = co_await rt::sleep_for(io->runtime.reactor(), std::chrono::milliseconds(25), merged.get_token());
                if (status == rt::sleep_status::canceled || merged.stop_requested()) co_return std::unexpected(canceled_error());
                if (admission().try_admit(control.admission, &held)) {
                    slot.adopt(std::move(held));
                    break;
                }
            }
        }
    }
    // The wait spent part of the call's total: the request gets what is left.
    auto const spent = std::chrono::steady_clock::now() - started;
    if (spent >= *budget) {
        co_return std::unexpected(error{failure_class::resource, "the call's total time ran out while waiting for egress admission", "net.deadline_exceeded"});
    }
    auto const request_budget = std::chrono::duration_cast<std::chrono::milliseconds>(*budget - spent);

    // 5. The request, bounded (D4): the byte cap is the BODY's wire bytes; heads share min(64 KiB, cap) so a head larger
    //    than a tiny cap fails while it is being read; the request-level total derives from both (ADR-241 S1).
    std::uint64_t const cap =
        std::min<std::uint64_t>(granted.byte_cap.limit().value_or(kHardResponseCeilingBytes), kHardResponseCeilingBytes);
    rt::HttpContext ctx{.options = {.total_timeout_ms = agentengine::Bound<std::uint64_t>::unbounded()}};  // set to the request budget below
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
    ctx.options.total_timeout_ms         = agentengine::Bound<std::uint64_t>::at_most(
        static_cast<std::uint64_t>(std::max(request_budget, std::chrono::milliseconds{1}).count()));
    ctx.options.idle_timeout             = std::chrono::milliseconds(io_timeout_ms());
    ctx.options.limits.max_header_bytes  = static_cast<std::size_t>(std::min<std::uint64_t>(64u * 1024u, cap));
    ctx.options.limits.max_body_bytes    = cap;
    ctx.options.connect.resolver         = std::move(seams.host_resolver);

    rt::HttpRequest request;
    request.scheme  = target.https ? rt::http_scheme::https : rt::http_scheme::http;
    request.host    = target.host;  // the allowlisted NAME: what TLS verifies, never an address
    request.port    = target.port;
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
    // The gate runs HERE, once, before the context is touched: a gate-rejected call constructs no I/O context (G13b).
    if (observer != nullptr) observer->gate_calls.fetch_add(1, std::memory_order_relaxed);
    auto const gated = egress_gate(req, granted);
    if (!gated) return std::unexpected(gated.error());
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
            egress_fetch_gated(std::move(req), *gated, std::move(granted), std::move(control), std::move(seams),
                               total_timeout, observer),
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
