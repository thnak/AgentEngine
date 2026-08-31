#pragma once
// ADR-148: Finding 1 of `docs/planning/model-call-gateway-routing-design-draft.md` (GitHub issue #16),
// implemented and corrected from that draft's own self-red-team by an independent red-team pass (see
// ADR-148 §2 for the full record). `RoutingModelCallGateway<SelectFn, Routes...>` is a THIRD small,
// composable `ModelCallGatewayLike` type, alongside `ModelCallGateway`/`MiddlewareModelCallGateway`
// (`model_call_gateway.hpp`) -- it picks ONE of several already-`ModelCallGatewayLike` `Routes` per
// call, via a caller-supplied `SelectFn`, rather than retrying/failing over across them. Each `Route`
// already satisfying `ModelCallGatewayLike` means a route can itself be a full
// `ModelCallGateway<Primary, Fallback...>` -- router-wraps-failover composition falls out for free, no
// special-casing needed, the same way `MiddlewareModelCallGateway<Inner, Ms...>` already wraps ANY
// `ModelCallGatewayLike` `Inner` today (ADR-036 §3).
//
// SCOPE, NAMED NOT SILENTLY ASSUMED: this type is for routing among Routes that declare the SAME
// relevant `ChatClientCapabilities` -- cost/latency/provider selection, or (the concrete use case
// GitHub issue #16's own follow-up comment names) the same backend wrapped at different
// `reasoning_effort` levels, routed by predicted task complexity. It is NOT capability-based content
// routing (e.g. "send image-bearing requests to a vision-capable Route, text-only elsewhere") --
// `capabilities()` below reports Route 0's declared capabilities only (matching `ModelCallGateway::
// capabilities()`'s own established "primary only" precedent for a heterogeneous chain -- that
// method's own comment cites `FailoverChatClient::capabilities()`'s identical reasoning), and
// `AgentSession::run_model_call()` gates on `chat_client_->capabilities()` BEFORE `select_()` ever
// runs (`validate_outbound_media_capabilities()`, `rt/agent_session.hpp`) -- a request needing a
// capability only a LATER Route declares would be rejected at that gate first, never reaching
// selection. Making `AgentSession`'s pre-dispatch gates router-aware is real, separate, invasive work
// this ADR does not attempt (an independent red-team pass found this gap directly; see ADR-148 §2).
//
// `SelectFn`'s required shape is DELIBERATELY narrower than the design draft's own sketch --
// `(ChatRequest const&) -> convertible_to<std::size_t>`, NOT `(ChatRequest const&, EffectContext&)` --
// found necessary by the same red-team pass: `EffectContext::deadline` (`effect_context.hpp`) is a
// `std::chrono::steady_clock::time_point`, a real, unrecorded wall/steady-clock read that is NOT
// reproducible across an I5 replay boundary (`effect_context.hpp`'s own adjacent comment on `run_id`
// names exactly this class of hazard: "an unrecorded wall-clock read here would be exactly the kind
// of untracked nondeterminism I5 forbids"). A `SelectFn` that cannot see `EffectContext` at all cannot
// read `deadline` (or anything else nondeterministic hanging off it) -- this makes "the base router
// needs zero new I5 recording plumbing, because replaying the exact same recorded `ChatRequest`
// reproduces the exact same selected index" a real, `static_assert`-checkable STRUCTURAL guarantee
// not a documented-but-unenforced convention a future `SelectFn` author could silently violate. A
// future selection signal genuinely needing `EffectContext` (or a live, non-deterministic input like
// an embedding call -- the draft's own `SemanticRoutingModelCallGateway<EmbedderT, Routes...>` sketch,
// explicitly NOT built here, see ADR-148 §2) needs its own type with its own I5 recording story, not a
// widening of this one's contract.

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/task.hpp"

namespace agentengine {

// Deferred bulk reconciliation against 027 §2-4, same posture ModelCallGateway/
// MiddlewareModelCallGateway already carry (ADR-025 §4c).
template <class SelectFn, class... Routes>
// ae-naming-lint: allow RoutingModelCallGateway — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class RoutingModelCallGateway {
    static_assert((ModelCallGatewayLike<Routes> && ...),
                  "RoutingModelCallGateway's every Route must satisfy ModelCallGatewayLike -- wrap a "
                  "ModelCallGateway<Primary, Fallback...> (or another ModelCallGatewayLike type), "
                  "never a raw ChatClient (compose via ModelCallGateway<Route> first if needed)");
    static_assert(sizeof...(Routes) >= 1, "RoutingModelCallGateway needs at least one Route");
    static_assert(
        requires(SelectFn& fn, ChatRequest const& request) {
            { fn(request) } -> std::convertible_to<std::size_t>;
        },
        "RoutingModelCallGateway's SelectFn must be callable as select(request) -> convertible to "
        "std::size_t -- deliberately NOT given EffectContext&, see file banner (I5 replay safety)");

public:
    explicit RoutingModelCallGateway(SelectFn select, std::tuple<Routes...> routes)
        : select_(std::move(select)), routes_(std::move(routes)) {}

    // Route 0's capabilities only -- see file banner for why this is a real, named scope limit, not
    // an oversight.
    [[nodiscard]] ChatClientCapabilities capabilities() const { return std::get<0>(routes_).capabilities(); }

    [[nodiscard]] task<result<ChatResponse>> call(ChatRequest request, EffectContext& ctx) {
        std::size_t const index = select_(request);
        result<ChatResponse> outcome = co_await call_route<0>(index, request, ctx);
        co_return outcome;
    }

private:
    // Runtime index -> compile-time tuple access. Unlike `ModelCallGateway::try_tier<Tier>` (which
    // cascades forward through a KNOWN-ordered chain and never needs to detect "index out of range" --
    // every tier 0..sizeof...(Fallback) is tried in turn, the last tier's own failure IS the answer),
    // this walks a fixed-size tuple looking for an arbitrary, externally-supplied runtime value that
    // may not be valid at all -- so it genuinely needs one extra instantiation past the last real
    // Route specifically to catch that case (the same shape a hand-rolled `std::visit` dispatch table
    // needs), not a mechanical copy of `try_tier`'s own recursion shape.
    template <std::size_t I>
    task<result<ChatResponse>> call_route(std::size_t index, ChatRequest const& request, EffectContext& ctx) {
        if constexpr (I < sizeof...(Routes)) {
            if (I == index) {
                result<ChatResponse> outcome = co_await std::get<I>(routes_).call(request, ctx);
                // route_index stamped here; fallback_tier is left exactly as the selected Route's own
                // call() set it -- orthogonal fields, see ChatResponse::route_index's own comment.
                if (outcome.has_value()) outcome->route_index = static_cast<std::uint32_t>(I);
                co_return outcome;
            }
            co_return co_await call_route<I + 1>(index, request, ctx);
        } else {
            // select_() returned an index >= sizeof...(Routes) -- a real, named contract failure, never
            // UB and never silently clamped to Route 0 (which would silently misattribute route_index).
            co_return std::unexpected(error{failure_class::contract,
                                             "RoutingModelCallGateway: select() returned an out-of-range "
                                             "route index",
                                             "gateway.route_out_of_range"});
        }
    }

    SelectFn select_;
    std::tuple<Routes...> routes_;
};

}  // namespace agentengine
