#pragma once
// ADR-036: the replacement for trying to give `FailoverChatClient`/`ResilientChatClient`/
// `MiddlewareChatClient` real `chat_stream()` parity as three separate wrapper templates. An
// independent red-team pass on that approach found real, unresolved problems tracing back to one
// root cause: retry/failover/middleware are coroutine-shaped concerns (a middleware hook is an
// arbitrary `task<std::monostate>` a caller writes; deciding "retry or fail over" needs to await a
// full attempt's outcome), but `ChatClient::chat_stream()`'s literal signature (004 §1) is a plain,
// non-coroutine function -- there is no safe way to `co_await` a real coroutine from inside it. The
// project owner's own diagnosis, informally: "if we tried to unify around one protocol and let the
// engine's core, not the per-backend wrapper, own tool-calling/middleware/retry, most of that
// friction goes away" -- correct, and this file is that: `OpenAIChatClient`/`AnthropicChatClient`
// stay thin wire-protocol adapters (they already were), and this file gives `AgentSession` a NEW
// shape of "chat client" to plug in -- `ModelCallGatewayLike` (`core/chat_client.hpp`) -- whose
// single `call()` method IS a real coroutine, so a middleware hook is an ordinary, safe `co_await`.
//
// A SECOND piece of project-owner feedback shaped this file's own internal structure, not just
// AgentSession's: don't fold retry+failover+middleware into ONE type either -- `AgentSession`
// already owns enough (the turn loop, tool invocation, approval, budgets, history); a single
// do-everything gateway class would just relocate the god-object problem, not fix it. So this file
// keeps TWO small, separately-testable types, composed the same way `MiddlewareChatClient<Inner,
// Ms...>` already composes over a raw `ChatClient` `Inner` today:
//
//   ModelCallGateway<Primary, Fallback...>        -- retry + circuit-breaking + failover ONLY.
//   MiddlewareModelCallGateway<Inner, Ms...>       -- middleware hooks ONLY, wrapping ANY
//                                                      ModelCallGatewayLike `Inner` (typically a
//                                                      ModelCallGateway<...>, but the concept is
//                                                      what's required, not the concrete type).
//
// A caller wanting just retry+failover uses `ModelCallGateway<Primary, Fallback...>` directly (it
// already satisfies `ModelCallGatewayLike` on its own). A caller wanting middleware too composes:
// `MiddlewareModelCallGateway<ModelCallGateway<Primary, Fallback...>, Ms...>`. This split exists
// specifically because a C++ class template can only have ONE trailing parameter pack -- cramming
// `Fallback...` AND `Ms...` into one template's parameter list is not legal C++, and even if it
// were, it would recreate the exact "one file owns everything" shape the project owner just
// rejected for `AgentSession`. Two small, composable pieces, not one large one.
//
// `ResilientChatClient`/`FailoverChatClient`/`MiddlewareChatClient` (the three original `chat()`-
// only wrapper templates) were REMOVED 2026-08-12 -- this repo had shipped nowhere, so there was no
// deprecation-then-migration cost to justify keeping them once this file gave `AgentSession` a real,
// streaming-capable successor. This file's types reuse their building blocks directly
// (`RetryPolicy`/`BreakerConfig`/`real_jitter` from retry_policy.hpp -- the file `resilient_chat_
// client.hpp` was renamed to once `ResilientChatClient` itself was removed; `ModelCallContext`/
// `MiddlewareTraceHook`/`middleware_detail::run_before`/`run_after`/
// `enforce_backend_tool_call_provenance` from middleware.hpp) rather than reimplementing them --
// one set of correct, already-tested primitives, not two independent copies that could drift.
//
// BUFFER-THEN-DECIDE, THE UNAVOIDABLE COST: every attempt this file makes (whether it succeeds or
// fails) is drained to completion internally, via `chat_stream()` + a poll loop, before `call()`
// ever returns anything to its own caller. This is not a missed optimization -- a real backend's
// failure can surface only AFTER partial content has already been produced (see the ORIGINAL
// `failover_chat_client.hpp`'s own file-top comment for the full reasoning, still correct here);
// showing a caller live tokens from an attempt that might still need to be retried or failed over
// would risk exactly the silent mid-stream backend substitution 004 §4 forbids. So a gateway-routed
// round never fires `run_event_kind::model_delta` -- `AgentSession::run_model_call()`'s own comment
// names this trade explicitly, and `handle()` emits a one-time `run_event_kind::warning` per run
// when a gateway is engaged, matching this codebase's established pattern for named, visible trades
// (ADR-034's identical warning for `stream_model_calls_`).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/chat_stream_drain.hpp"  // DrainedChatStream, drain_chat_stream (ADR-035 Phase 3)
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/middleware.hpp"
#include "agentengine/core/retry_policy.hpp"  // RetryPolicy, BreakerConfig, real_jitter
#include "agentengine/core/stream.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/core/tool_pipeline.hpp"  // IdempotencyKey
#include "agentengine/rt/circuit_breaker.hpp"  // rt::CircuitBreaker, rt::Admit (ADR-037 Phase 2)

namespace agentengine {

namespace model_call_gateway_detail {

// Replaces `quark::monotonic_now_ns()` (ADR-037): a plain monotonic ns-since-epoch reading,
// std-only, no PAL indirection needed here -- this file's only use is feeding `rt::CircuitBreaker`'s
// `now_ns` parameter, which only ever compares two readings from the SAME clock for elapsed time,
// never a real wall-clock timestamp.
[[nodiscard]] inline std::int64_t monotonic_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ADR-036 (ADR-037 second pass: `DrainedChatStream::failure` is now `ae::error` directly, so this is
// simply the SAME `failure_class::transient` scoping `chat()`'s own retry logic already uses -- 004
// §4: retry applies to Transient only. Deliberately conservative, matching "policy/contract/resource/
// fatal aren't retried" on the `chat()` side; no separate coarser vocabulary to re-derive this from
// anymore -- the old `quark::errc`-keyed split this replaced is gone with `classify_drained_failure`.
[[nodiscard]] inline bool is_retryable(failure_class klass) noexcept {
    return klass == failure_class::transient;
}

}  // namespace model_call_gateway_detail

// Retry (F1-shaped, reusing `RetryPolicy` verbatim) + circuit-breaking (F2-shaped, reusing
// `BreakerConfig` verbatim, one REAL `rt::CircuitBreaker` (ADR-037 Phase 2 -- a faithful,
// Quark-free port of `quark::CircuitBreaker`'s exact state machine, see rt/circuit_breaker.hpp's
// own banner) PER backend -- 004 §4/decision 6's breaker key is {provider, model, secret}, so a
// chain of N distinct backends needs N distinct
// breakers, never one shared across the whole chain) + failover (tries Primary, then each Fallback
// in declaration order, first success wins, stamps `ChatResponse::fallback_tier` -- same contract
// `FailoverChatClient::chat()` already established) -- all through ONE real coroutine, `call()`.
//
// `Fallback...` may be empty (unlike `FailoverChatClient`, which requires >=1 -- that type's whole
// purpose IS failover; here, "just retry+breaker, no failover" is a legitimate, common
// configuration, so zero fallbacks is allowed).
template <class Primary, class... Fallback>
// ae-naming-lint: allow ModelCallGateway — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class ModelCallGateway {
    static_assert(ChatClient<Primary>,
                  "ModelCallGateway's Primary must satisfy the ChatClient concept (004 §1)");
    static_assert((ChatClient<Fallback> && ...),
                  "ModelCallGateway's every Fallback must satisfy the ChatClient concept (004 §1)");

public:
    using JitterSource = std::function<double()>;

    // `fallbacks` as an explicit `std::tuple<Fallback...>` (not a trailing parameter pack) is what
    // lets `retry_policy`/`breaker_config` follow it in the constructor's own parameter list --
    // a class template may have only one trailing pack, and a constructor parameter pack must be
    // the LAST parameter for positional construction to work at all (this is exactly why
    // `MiddlewareModelCallGateway` below is a SEPARATE type instead of a second pack on this one).
    explicit ModelCallGateway(Primary primary, std::tuple<Fallback...> fallbacks,
                               RetryPolicy retry_policy = {}, BreakerConfig breaker_config = {},
                               JitterSource jitter = &resilient_chat_client_detail::real_jitter)
        : primary_(std::move(primary)),
          fallbacks_(std::move(fallbacks)),
          retry_policy_(retry_policy),
          jitter_(std::move(jitter)) {
        if (retry_policy_.max_attempts == 0) retry_policy_.max_attempts = 1;
        breakers_.reserve(1 + sizeof...(Fallback));
        for (std::size_t i = 0; i < 1 + sizeof...(Fallback); ++i) {
            breakers_.emplace_back(
                breaker_config.fail_threshold,
                std::chrono::duration_cast<std::chrono::nanoseconds>(breaker_config.open_duration).count());
        }
    }

    // Primary's capabilities only -- same reasoning as `FailoverChatClient::capabilities()`'s own
    // file-top comment (merging/intersecting across a heterogeneous chain is wrong in either
    // direction; plan against what actually answers most of the time).
    [[nodiscard]] ChatClientCapabilities capabilities() const { return primary_.capabilities(); }

    [[nodiscard]] task<result<ChatResponse>> call(ChatRequest request, EffectContext& ctx) {
        // ONE idempotency key for the WHOLE logical call (every retry AND every fallback tier),
        // matching `ResilientChatClient::chat()`'s own "stable across every attempt" contract --
        // `ChatRequest::idempotency_key` is not yet read by any real backend adapter (grepped;
        // presently inert), so this is forward-looking, not load-bearing today.
        request.idempotency_key = IdempotencyKey{ctx.run_id, ctx.turn_index, 0, 0}.to_string();
        result<ChatResponse> outcome = co_await try_tier<0>(request, ctx);
        co_return outcome;
    }

private:
    // Compile-time recursion over tiers: 0 = Primary, I = the (I-1)th Fallback. Each tier's own
    // bounded retry loop runs first; on success, `fallback_tier` is stamped (OVERWRITING whatever
    // the raw backend itself may have left, exactly `FailoverChatClient`'s own established rule --
    // this type is the one authoritative source of that field). On exhaustion, falls through to the
    // next tier; the DEEPEST (last) tier's own failure is what ultimately propagates all the way
    // back up (no separate "last error" bookkeeping needed -- each level's `co_return co_await
    // try_tier<I+1>(...)` already forwards exactly that).
    template <std::size_t Tier>
    task<result<ChatResponse>> try_tier(ChatRequest const& request, EffectContext& ctx) {
        if constexpr (Tier == 0) {
            result<ChatResponse> attempt = co_await attempt_with_retry(primary_, breakers_[0], request, ctx);
            if (attempt.has_value()) {
                attempt->fallback_tier = 0;
                co_return attempt;
            }
            if constexpr (sizeof...(Fallback) == 0) {
                co_return attempt;
            } else {
                co_return co_await try_tier<1>(request, ctx);
            }
        } else {
            auto& backend = std::get<Tier - 1>(fallbacks_);
            result<ChatResponse> attempt =
                co_await attempt_with_retry(backend, breakers_[Tier], request, ctx);
            if (attempt.has_value()) {
                attempt->fallback_tier = static_cast<std::uint32_t>(Tier);
                co_return attempt;
            }
            if constexpr (Tier < sizeof...(Fallback)) {
                co_return co_await try_tier<Tier + 1>(request, ctx);
            } else {
                co_return attempt;  // the last tier -- its own failure is the whole call's outcome
            }
        }
    }

    // The bounded retry loop for ONE tier (one backend). Admission-gated before every attempt
    // (including the first), same as `ResilientChatClient::chat()`; a shed never consumes a
    // `retry_policy_.max_attempts` slot and is never spin-waited out within this call (same
    // reasoning: looping would either busy-loop or duplicate the breaker's own cooldown clock) --
    // it simply exhausts this tier immediately, falling through to the next one (or the final
    // failure, if this was the last tier).
    template <class Backend>
    task<result<ChatResponse>> attempt_with_retry(Backend& backend, rt::CircuitBreaker& breaker,
                                                    ChatRequest const& request, EffectContext& ctx) {
        std::uint32_t attempts_used = 0;
        for (;;) {
            rt::Admit const admit = breaker.on_send(model_call_gateway_detail::monotonic_now_ns());
            if (admit == rt::Admit::Shed) {
                co_return std::unexpected(
                    error{failure_class::transient, "circuit open, admission shed", "gateway.circuit_open"});
            }

            DrainedChatStream drained = drain_chat_stream(backend.chat_stream(request, ctx));
            // A Closed terminal with no reported usage is treated as a failure here, not a success
            // -- 004 §5's TokenBudget<N> depends on a true per-call token count, so this gateway
            // never lets a call with unknown cost masquerade as free (same fail-closed rule
            // AgentSession::run_model_call() applies on its own streaming branch).
            bool const succeeded = drained.ok && drained.usage.has_value();
            if (drained.ok && !drained.usage.has_value()) {
                drained.failure = error{failure_class::fatal, "streaming call reported no token usage",
                                         "gateway.usage_unavailable"};
            }
            ++attempts_used;
            breaker.on_result(succeeded, model_call_gateway_detail::monotonic_now_ns());

            if (succeeded) {
                co_return ChatResponse{std::move(drained.accumulated), *drained.usage};
            }

            bool const retryable = model_call_gateway_detail::is_retryable(drained.failure.klass);
            if (!retryable || attempts_used >= retry_policy_.max_attempts) {
                co_return std::unexpected(
                    drained_failure_to_agent_error(drained.failure, "gateway.attempt_failed"));
            }

            std::chrono::milliseconds const delay = compute_backoff(attempts_used - 1);
            if (ctx.deadline.time_since_epoch().count() != 0) {
                auto const now = std::chrono::steady_clock::now();
                if (now >= ctx.deadline) {
                    co_return std::unexpected(
                        drained_failure_to_agent_error(drained.failure, "gateway.attempt_failed"));
                }
                auto const remaining = ctx.deadline - now;
                if (delay > remaining) {
                    co_return std::unexpected(
                        drained_failure_to_agent_error(drained.failure, "gateway.attempt_failed"));
                }
            }
            // Code review finding (2026-08-12), recorded as a named residual rather than fixed here:
            // this blocks the calling (actor worker) thread for `delay`, with no co_await/suspension
            // point across the whole retry loop -- on a shared thread pool this can stall other
            // actors scheduled on the same worker. NOT a hazard newly introduced by this file:
            // `ResilientChatClient::chat()` (removed 2026-08-12; see retry_policy.hpp's own top
            // comment) already did the identical blocking `sleep_for` inside a `task<>` coroutine,
            // predating ADR-036. A real fix needs a coroutine-suspending timer primitive that doesn't
            // exist anywhere in `rt::task<T>` today (historical: originally "Quark's task<>," before
            // ADR-037 replaced the underlying coroutine type; the gap itself -- confirmed still live,
            // see the blocking `sleep_for` below -- was never specific to which task type), and would
            // touch both files -- new
            // infrastructure, not a same-pass edit; left for separate design → red-team → prove work
            // per this project's own governance for
            // concurrency-critical changes.
            std::this_thread::sleep_for(delay);
        }
    }

    // Identical formula to `ResilientChatClient::compute_backoff` (same file-top rationale: bounded
    // exponential with jitter, respecting the remaining deadline) -- duplicated rather than shared
    // because it's a private, three-line pure function closed over this type's own
    // `retry_policy_`/`jitter_`, not worth a shared header for.
    [[nodiscard]] std::chrono::milliseconds compute_backoff(std::uint32_t retry_index) const {
        double const exp_factor = std::pow(2.0, static_cast<double>(retry_index));
        double const base_ms = static_cast<double>(retry_policy_.base_delay.count()) * exp_factor;
        double const capped_ms =
            std::min(base_ms, static_cast<double>(retry_policy_.max_delay.count()));
        double const r = jitter_ ? jitter_() : 0.0;
        double jittered_ms = capped_ms * (1.0 + retry_policy_.jitter_fraction * r);
        if (jittered_ms < 0.0) jittered_ms = 0.0;
        return std::chrono::milliseconds(static_cast<std::int64_t>(jittered_ms));
    }

    Primary primary_;
    std::tuple<Fallback...> fallbacks_;
    RetryPolicy retry_policy_;
    std::vector<rt::CircuitBreaker> breakers_;  // index 0 = primary's, index I = fallbacks_[I-1]'s
    JitterSource jitter_;
};

// Middleware hooks ONLY, wrapping ANY `ModelCallGatewayLike` `Inner` -- typically a
// `ModelCallGateway<Primary, Fallback...>`, but the CONCEPT is what's required, not that concrete
// type (a caller could compose two of these, or wrap a hand-written test fixture, etc.). Reuses
// `middleware.hpp`'s `ModelCallContext`/`middleware_detail::run_before`/`run_after`/
// `enforce_backend_tool_call_provenance` VERBATIM -- this is `MiddlewareChatClient::chat()`'s own
// body, unchanged, just calling `inner_.call(...)` instead of `inner_.chat(...)`. Because `call()`
// is a real coroutine (unlike `chat_stream()`), a middleware hook's `co_await` here is completely
// ordinary -- no "resume once and hope it never suspends" hack, no leaked-frame use-after-free risk
// (the exact hazard that made the original wrapper-template streaming-parity attempt unsafe).
template <class Inner, class... Ms>
// ae-naming-lint: allow MiddlewareModelCallGateway — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class MiddlewareModelCallGateway {
    static_assert(ModelCallGatewayLike<Inner>,
                  "MiddlewareModelCallGateway's Inner must satisfy ModelCallGatewayLike -- wrap a "
                  "ModelCallGateway<Primary, Fallback...> (or another ModelCallGatewayLike type), "
                  "never a raw ChatClient (use MiddlewareChatClient for that, unchanged)");

public:
    explicit MiddlewareModelCallGateway(Inner inner, Ms... middlewares)
        : inner_(std::move(inner)), middlewares_(std::move(middlewares)...) {}

    MiddlewareModelCallGateway& set_trace_hook(MiddlewareTraceHook hook) {
        trace_hook_ = std::move(hook);
        return *this;
    }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return inner_.capabilities(); }

    task<result<ChatResponse>> call(ChatRequest request, EffectContext& ctx) {
        ModelCallContext mctx{std::move(request), std::nullopt, std::nullopt};
        std::size_t stopped_at = 0;

        {
            std::monostate const ignored =
                co_await middleware_detail::run_before<0>(middlewares_, mctx, stopped_at, trace_hook_);
            (void)ignored;
        }

        std::optional<ChatResponse> raw_backend_response;
        if (!mctx.settled()) {
            auto real = co_await inner_.call(std::move(mctx.request), ctx);
            if (real.has_value()) {
                raw_backend_response = *real;
                mctx.response         = *real;
            } else {
                mctx.failure = real.error();
            }
        }

        {
            std::monostate const ignored =
                co_await middleware_detail::run_after<0>(middlewares_, mctx, stopped_at, trace_hook_);
            (void)ignored;
        }

        if (mctx.response.has_value()) {
            middleware_detail::enforce_backend_tool_call_provenance(mctx.response->message,
                                                                     raw_backend_response);
            co_return *mctx.response;
        }
        if (mctx.failure.has_value()) co_return std::unexpected(*mctx.failure);
        co_return std::unexpected(
            error{failure_class::fatal,
                  "middleware chain produced neither a response nor a failure", "gateway.middleware_unsettled"});
    }

private:
    Inner              inner_;
    std::tuple<Ms...>  middlewares_;
    MiddlewareTraceHook trace_hook_;
};

}  // namespace agentengine
