// Proves ADR-148's `RoutingModelCallGateway<SelectFn, Routes...>` (GitHub issue #16 Finding 1) --
// deterministic, offline, no live model, no network. Reuses the same `ScriptedGatewayBackend`
// fixture shape `test_model_call_gateway.cpp` established.
//
//   G1 -- basic selection: SelectFn picks Route 1 of 2; route_index is stamped, fallback_tier is left
//         at whatever the selected Route's own call() set (0, a plain backend with no failover of its
//         own here).
//   G2 -- an out-of-range SelectFn result is a real, named failure (gateway.route_out_of_range), never
//         UB, never silently clamped to Route 0.
//   G3 -- composition: a Route can itself be a full ModelCallGateway<Primary, Fallback...> -- when
//         Route 1 is selected and its own primary fails over to its own fallback, BOTH route_index
//         (which Route) and fallback_tier (which tier within that Route) are stamped correctly and
//         simultaneously, proving "router-wraps-failover falls out for free" is real, not aspirational.
//   G4 -- capabilities() reports Route 0's capabilities only (the documented, named scope limit).
//   G5 -- composition: a sticky ModelCallGateway used as a Route retains its own sticky state
//         correctly across repeated selections of that same Route by the router -- confirms
//         Route-local state genuinely persists per-Route-instance with zero special-casing needed.

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <string>
#include <tuple>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/model_call_gateway.hpp"
#include "agentengine/core/routing_model_call_gateway.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "support/run_task_sync.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

// Same shape as test_model_call_gateway.cpp's own ScriptedGatewayBackend -- a shared_ptr call_count
// so the gateway's own internal by-value copy is what's actually observed.
struct ScriptedOutcome {
    bool succeed = true;
    std::string text;
    ae::failure_class fail_klass = ae::failure_class::fatal;

    static ScriptedOutcome ok(std::string t) { return ScriptedOutcome{true, std::move(t), {}}; }
    static ScriptedOutcome fail(ae::failure_class k) { return ScriptedOutcome{false, {}, k}; }
};

class ScriptedGatewayBackend {
public:
    std::vector<ScriptedOutcome> outcomes;
    ae::ChatClientCapabilities capabilities_override{};

    ScriptedGatewayBackend() : call_count_(std::make_shared<std::size_t>(0)) {}

    [[nodiscard]] std::size_t call_count() const { return *call_count_; }
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return capabilities_override; }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        std::size_t const call_count = *call_count_;
        if (call_count < outcomes.size()) {
            ScriptedOutcome const& o = outcomes[call_count];
            if (o.succeed) {
                ae::ChatResponseUpdate upd;
                upd.delta.origin = ae::content_origin::assistant;
                upd.delta.value = ae::Text{o.text};
                upd.is_final = true;
                upd.usage = ae::Usage{1, 1, 0, 0, 0.0};
                auto pushed = pair.producer.push(upd);
                (void)pushed;
                pair.producer.close();
            } else {
                pair.producer.fail(ae::error{o.fail_klass, "scripted_failure", "test.scripted_failure"});
            }
        } else {
            pair.producer.fail(
                ae::error{ae::failure_class::fatal, "no more scripted outcomes", "test.no_more_outcomes"});
        }
        ++*call_count_;
        return std::move(pair.consumer);
    }

private:
    std::shared_ptr<std::size_t> call_count_;
};
static_assert(ae::ChatClient<ScriptedGatewayBackend>);

double no_jitter() { return 0.0; }

ae::RetryPolicy fast_retry_policy() {
    ae::RetryPolicy p;
    p.max_attempts = 3;
    p.base_delay = std::chrono::milliseconds(1);
    p.max_delay = std::chrono::milliseconds(1);
    p.jitter_fraction = 0.0;
    return p;
}

[[nodiscard]] ae::EffectContext make_ctx() {
    ae::EffectContext ctx;
    ctx.run_id = "test-run";
    ctx.turn_index = 0;
    return ctx;
}

[[nodiscard]] ae::ChatRequest make_request() {
    ae::ChatRequest req;
    return req;
}

}  // namespace

int main() {
    // ---- G1: basic selection -- SelectFn picks Route 1 of 2 ---------------------------------------
    {
        ScriptedGatewayBackend route0_backend;
        route0_backend.outcomes = {ScriptedOutcome::ok("route 0 should never answer")};
        ScriptedGatewayBackend route1_backend;
        route1_backend.outcomes = {ScriptedOutcome::ok("route 1 answered")};

        ae::ModelCallGateway<ScriptedGatewayBackend> route0(route0_backend, std::make_tuple(),
                                                              fast_retry_policy(), ae::BreakerConfig{}, &no_jitter);
        ae::ModelCallGateway<ScriptedGatewayBackend> route1(route1_backend, std::make_tuple(),
                                                              fast_retry_policy(), ae::BreakerConfig{}, &no_jitter);

        auto select_route1 = [](ae::ChatRequest const&) -> std::size_t { return 1; };
        ae::RoutingModelCallGateway<decltype(select_route1), ae::ModelCallGateway<ScriptedGatewayBackend>,
                                     ae::ModelCallGateway<ScriptedGatewayBackend>>
            gw(select_route1, std::make_tuple(std::move(route0), std::move(route1)));

        auto ctx = make_ctx();
        auto r = ae::test_support::run_task_sync<ae::result<ae::ChatResponse>>(gw.call(make_request(), ctx));
        AE_CHECK(r.has_value(), "G1: the call converges via the selected route");
        if (r.has_value()) {
            AE_CHECK(text_of(r->message) == "route 1 answered", "G1: Route 1's text is returned, not Route 0's");
            AE_CHECK(r->route_index == 1, "G1: route_index is stamped to 1 -- the selected route");
            AE_CHECK(r->fallback_tier == 0,
                     "G1: fallback_tier is 0 -- Route 1's own (single-backend) chain answered at its own tier 0");
        }
        AE_CHECK(route0_backend.call_count() == 0, "G1: Route 0's backend was never touched");
        AE_CHECK(route1_backend.call_count() == 1, "G1: Route 1's backend was called exactly once");
    }

    // ---- G2: an out-of-range select() result is a real, named failure -----------------------------
    {
        ScriptedGatewayBackend route0_backend;
        route0_backend.outcomes = {ScriptedOutcome::ok("unreachable")};
        ae::ModelCallGateway<ScriptedGatewayBackend> route0(route0_backend, std::make_tuple(),
                                                              fast_retry_policy(), ae::BreakerConfig{}, &no_jitter);

        auto select_out_of_range = [](ae::ChatRequest const&) -> std::size_t { return 5; };
        ae::RoutingModelCallGateway<decltype(select_out_of_range), ae::ModelCallGateway<ScriptedGatewayBackend>>
            gw(select_out_of_range, std::make_tuple(std::move(route0)));

        auto ctx = make_ctx();
        auto r = ae::test_support::run_task_sync<ae::result<ae::ChatResponse>>(gw.call(make_request(), ctx));
        AE_CHECK(!r.has_value(), "G2: an out-of-range route index is a real failure, not UB");
        if (!r.has_value()) {
            AE_CHECK(r.error().klass == ae::failure_class::contract,
                     "G2: the failure is classified failure_class::contract");
            AE_CHECK(r.error().code == "gateway.route_out_of_range",
                     "G2: the failure carries the named, specific error code");
        }
        AE_CHECK(route0_backend.call_count() == 0,
                 "G2: Route 0's backend was NEVER touched -- an out-of-range index is not silently "
                 "clamped to route 0");
    }

    // ---- G3: composition -- a Route that is itself a ModelCallGateway<Primary, Fallback...> --------
    // ---- stamps BOTH route_index AND fallback_tier correctly and simultaneously -------------------
    {
        ScriptedGatewayBackend route0_backend;
        route0_backend.outcomes = {ScriptedOutcome::ok("route 0 should never answer")};
        ae::ModelCallGateway<ScriptedGatewayBackend> route0(route0_backend, std::make_tuple(),
                                                              fast_retry_policy(), ae::BreakerConfig{}, &no_jitter);

        ScriptedGatewayBackend route1_primary;
        route1_primary.outcomes = {ScriptedOutcome::fail(ae::failure_class::contract)};  // fails, non-retryable
        ScriptedGatewayBackend route1_fallback;
        route1_fallback.outcomes = {ScriptedOutcome::ok("route 1's OWN fallback answered")};
        ae::ModelCallGateway<ScriptedGatewayBackend, ScriptedGatewayBackend> route1(
            route1_primary, std::make_tuple(route1_fallback), fast_retry_policy(), ae::BreakerConfig{}, &no_jitter);

        auto select_route1 = [](ae::ChatRequest const&) -> std::size_t { return 1; };
        ae::RoutingModelCallGateway<decltype(select_route1), ae::ModelCallGateway<ScriptedGatewayBackend>,
                                     ae::ModelCallGateway<ScriptedGatewayBackend, ScriptedGatewayBackend>>
            gw(select_route1, std::make_tuple(std::move(route0), std::move(route1)));

        auto ctx = make_ctx();
        auto r = ae::test_support::run_task_sync<ae::result<ae::ChatResponse>>(gw.call(make_request(), ctx));
        AE_CHECK(r.has_value(), "G3: the call converges via Route 1's own internal failover");
        if (r.has_value()) {
            AE_CHECK(text_of(r->message) == "route 1's OWN fallback answered",
                     "G3: Route 1's own fallback's text is returned");
            AE_CHECK(r->route_index == 1, "G3: route_index is 1 -- Route 1 was the SELECTED route");
            AE_CHECK(r->fallback_tier == 1,
                     "G3: fallback_tier is 1 -- WITHIN Route 1's own chain, its fallback (not its "
                     "primary) is what actually answered -- router-wraps-failover composition proven, "
                     "not just claimed to fall out for free");
        }
    }

    // ---- G4: capabilities() reports Route 0's capabilities only (named scope limit) ---------------
    {
        ScriptedGatewayBackend route0_backend;
        route0_backend.capabilities_override.tool_calling = true;
        route0_backend.capabilities_override.multimodal_in_image = false;
        ScriptedGatewayBackend route1_backend;
        route1_backend.capabilities_override.tool_calling = false;
        route1_backend.capabilities_override.multimodal_in_image = true;

        ae::ModelCallGateway<ScriptedGatewayBackend> route0(route0_backend, std::make_tuple(),
                                                              fast_retry_policy(), ae::BreakerConfig{}, &no_jitter);
        ae::ModelCallGateway<ScriptedGatewayBackend> route1(route1_backend, std::make_tuple(),
                                                              fast_retry_policy(), ae::BreakerConfig{}, &no_jitter);
        auto select_route0 = [](ae::ChatRequest const&) -> std::size_t { return 0; };
        ae::RoutingModelCallGateway<decltype(select_route0), ae::ModelCallGateway<ScriptedGatewayBackend>,
                                     ae::ModelCallGateway<ScriptedGatewayBackend>>
            gw(select_route0, std::make_tuple(std::move(route0), std::move(route1)));

        ae::ChatClientCapabilities caps = gw.capabilities();
        AE_CHECK(caps.tool_calling && !caps.multimodal_in_image,
                 "G4: capabilities() reports Route 0's declared capabilities, not Route 1's (or a "
                 "union/intersection) -- this type's documented scope limit, proven not just written");
    }

    // ---- G5: composition -- a sticky Route retains its own state across repeated selections --------
    {
        ScriptedGatewayBackend route1_primary;
        route1_primary.outcomes = {
            ScriptedOutcome::fail(ae::failure_class::contract),
            ScriptedOutcome::ok("route1 primary would answer if ever tried again"),
        };
        ScriptedGatewayBackend route1_fallback;
        route1_fallback.outcomes = {
            ScriptedOutcome::ok("route1 fallback answer 1"),
            ScriptedOutcome::ok("route1 fallback answer 2"),
        };
        ae::ModelCallGateway<ScriptedGatewayBackend, ScriptedGatewayBackend> route1(
            route1_primary, std::make_tuple(route1_fallback), fast_retry_policy(), ae::BreakerConfig{},
            &no_jitter, /*sticky=*/true);

        ScriptedGatewayBackend route0_backend;
        route0_backend.outcomes = {ScriptedOutcome::ok("route 0"), ScriptedOutcome::ok("route 0")};
        ae::ModelCallGateway<ScriptedGatewayBackend> route0(route0_backend, std::make_tuple(),
                                                              fast_retry_policy(), ae::BreakerConfig{}, &no_jitter);

        auto select_route1 = [](ae::ChatRequest const&) -> std::size_t { return 1; };
        ae::RoutingModelCallGateway<decltype(select_route1), ae::ModelCallGateway<ScriptedGatewayBackend>,
                                     ae::ModelCallGateway<ScriptedGatewayBackend, ScriptedGatewayBackend>>
            gw(select_route1, std::make_tuple(std::move(route0), std::move(route1)));

        auto ctx1 = make_ctx();
        auto r1 = ae::test_support::run_task_sync<ae::result<ae::ChatResponse>>(gw.call(make_request(), ctx1));
        AE_CHECK(r1.has_value() && r1->route_index == 1 && r1->fallback_tier == 1,
                 "G5: call 1 selects Route 1, which fails over to its own fallback internally");

        auto ctx2 = make_ctx();
        auto r2 = ae::test_support::run_task_sync<ae::result<ae::ChatResponse>>(gw.call(make_request(), ctx2));
        AE_CHECK(r2.has_value() && text_of(r2->message) == "route1 fallback answer 2",
                 "G5: call 2 (same route selected again) converges directly via Route 1's OWN fallback "
                 "again -- that Route instance's sticky state persisted across the router's repeated "
                 "selection of it, zero special-casing needed in RoutingModelCallGateway itself");
        AE_CHECK(route1_primary.call_count() == 1,
                 "G5: Route 1's OWN primary was never re-attempted for call 2 -- its sticky pinning "
                 "survived being wrapped inside a RoutingModelCallGateway");
    }

    std::cout << (g_failures == 0 ? "test_routing_model_call_gateway: OK\n"
                                  : "test_routing_model_call_gateway: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
