#pragma once
// Implements decisions/archive/ADR-241-egress-proxy-async-path-on-rt-http.md §5 ("No `resolver` field; one seam pair,
// test-only"): `TestEgressProxy`, the ONLY way to run the egress proxy's shared gate and fetch with a non-default host
// resolver, address policy or CA override.
//
// It is a SEPARATE type from `HostEgressProxy`, so the production type has no setter, no friend and no seam member to
// flip (a friend function was not a seal: any translation unit can define one). A deployer who includes THIS header is
// choosing a delegated decision (ADR-070) with their eyes open -- host code is trusted -- but nothing in a production
// binary can arrive here by accident. tools/egress_seam_lint.py keeps this header and
// `sandbox/detail/net_egress_impl.hpp` out of every file under include/, src/ and examples/ except `src/sandbox/`,
// `sandbox/testing/` and `sandbox/detail/` (ADR-241 G15). Tests under tests/ may include it.
//
// It conforms to `NetEgressBackend` (the tools take any conformer), counts gate calls PER INSTANCE (no process-global
// counter, so parallel or live rows cannot disturb it), and runs exactly the implementation `HostEgressProxy` runs.

#include <chrono>
#include <string>

#include "agentengine/sandbox/detail/net_egress_impl.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"

namespace agentengine::sandbox {

// ae-naming-lint: allow TestEgressProxy — ADR-241 §5: test-only type, defined only in this testing header
struct TestEgressProxy {
#ifdef AGENTENGINE_WITH_HTTPS
    static constexpr NetEgressTraits traits{"test-egress-proxy", true};
#else
    static constexpr NetEgressTraits traits{"test-egress-proxy", false};
#endif

    // What a test may replace. Defaults are the production values, so a `TestEgressProxy{}` differs from
    // `HostEgressProxy{}` only in the counters below.
    rt::HostResolver          host_resolver   = egress_detail::production_host_resolver();
    rt::AddressPolicy         address_policy  = egress_detail::egress_address_policy;
    std::string               tls_ca_override;
    // A permit-all `address_policy` still refuses every non-v4 family unless a test sets this: the OS dual-stack connect
    // would otherwise reach a v4 listener through a v4-mapped answer (measured, ADR-241 S2). Production never has it.
    bool                      allow_non_v4    = false;
    std::chrono::milliseconds total_timeout   = kDefaultEgressTotalTimeout;

    mutable egress_detail::GateObserver observer;

    [[nodiscard]] int gate_calls() const noexcept { return observer.gate_calls.load(); }
    // Wire bytes the client read for the last completed exchange (counted on the client, ADR-241 G9).
    [[nodiscard]] std::uint64_t last_wire_bytes() const noexcept { return observer.last_wire_bytes.load(); }

    [[nodiscard]] task<result<NetEgressResponse>> fetch(NetEgressRequest req, cap::NetOut granted,
                                                        EgressCallControl control) const {
        return egress_detail::egress_fetch(std::move(req), std::move(granted), std::move(control), seams(),
                                           total_timeout, &observer);
    }

    [[nodiscard]] result<NetEgressResponse> fetch_blocking(NetEgressRequest req, cap::NetOut granted,
                                                           EgressCallControl control) const {
        return egress_detail::egress_fetch_blocking(std::move(req), std::move(granted), std::move(control), seams(),
                                                    total_timeout, &observer);
    }

private:
    [[nodiscard]] egress_detail::EgressSeams seams() const {
        return egress_detail::EgressSeams{host_resolver, address_policy, tls_ca_override, allow_non_v4};
    }
};

static_assert(NetEgressBackend<TestEgressProxy>);

}  // namespace agentengine::sandbox
