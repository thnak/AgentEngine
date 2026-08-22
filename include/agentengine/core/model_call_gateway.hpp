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
// BUFFER-THEN-DECIDE, THE UNAVOIDABLE COST FOR `call()` SPECIFICALLY: every attempt `call()` makes
// (whether it succeeds or fails) is drained to completion internally, via `chat_stream()` + a poll
// loop, before `call()` ever returns anything to its own caller. This is not a missed optimization --
// a real backend's failure can surface only AFTER partial content has already been produced (see the
// ORIGINAL `failover_chat_client.hpp`'s own file-top comment for the full reasoning, still correct
// here); showing a caller live tokens from an attempt that might still need to be retried or failed
// over would risk exactly the silent mid-stream backend substitution 004 §4 forbids. So `call()` never
// fires `run_event_kind::model_delta` -- this is `call()`'s OWN, permanent contract, not a
// gateway-wide limitation anymore.
//
// `call_stream()` (unified-streaming-design-draft.md §3, Piece A) is the commit-gated alternative:
// pushes each chunk to the caller live as it arrives, tracking whether anything has been shown yet
// (`any_pushed`) -- once true, a mid-attempt failure is terminal (no retry, no fallback tier, matching
// `call()`'s own single-attempt failure contract for that case); until then, retry/failover stay
// invisible to the caller exactly as `call()`'s own buffered path already guarantees. This does NOT
// reopen the silent-substitution risk `call()`'s buffering exists to prevent -- pre-commit chunks are
// never exposed to the caller under either method, only post-commit ones, and post-commit means
// exactly one tier ever supplied what the caller sees. `AgentSession::run_model_call()` picks between
// `call()`/`call_stream()` via `stream_model_calls_` AND `ModelCallGatewayStreamLike<ChatClientT>`
// (only `ModelCallGateway` itself implements `call_stream()` today -- `MiddlewareModelCallGateway`/
// `ContentReplayGateway` do not, a named residual); `start_run()` emits its own gateway warning only
// for the narrower case that still applies (a gateway-typed session that does NOT get streaming),
// matching this codebase's established pattern for named, visible trades (ADR-034's identical warning
// for `stream_model_calls_`).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
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

    // unified-streaming-design-draft.md §3 (Piece A), Rev 7. Mirrors `ChatClient::chat_stream()`'s OWN
    // contract shape exactly (`chat_client.hpp:193-197`) -- a plain, non-coroutine function returning
    // `stream<ChatResponseUpdate>` immediately, backed by a detached thread, NOT a `task<>` the caller
    // `co_await`s while pushes happen synchronously on the same chain (the 4th red-team pass's Finding 2:
    // that shape deadlocks past the channel's default 256-item capacity, since `push()` genuinely blocks
    // until a consumer drains it). `ModelCallGatewayStreamLike` (`chat_client.hpp`) is how a caller
    // detects this method exists -- optional, not part of `ModelCallGatewayLike` itself (Finding 4/5).
    //
    // LIFETIME CONTRACT, disclosed rather than structurally enforced (matching this codebase's own
    // established precedent for the identical class of hazard -- `tool_pipeline.hpp::background_task()`'s
    // detached thread avoids capturing `this` via a `weak_ptr`, BUT that pattern requires the target to
    // already be `shared_ptr`-held; `ModelCallGateway` is a plain value type, normally embedded directly
    // in `AgentSession::chat_client_`, so the same `weak_ptr` fix does not apply without a larger,
    // out-of-scope ownership change): the detached thread below captures `this` and touches `breakers_`/
    // `primary_`/`fallbacks_`/`retry_policy_`/`jitter_` for as long as it runs. The CALLER must ensure
    // this `ModelCallGateway` instance (and transitively, its owning `AgentSession`) outlives every
    // in-flight `call_stream()` call's eventual completion or cancellation. Dropping/cancelling the
    // returned `stream<ChatResponseUpdate>` requests the thread stop ASAP (`stream_producer::stop_token()`,
    // checked between attempts, and `push()` itself returning `terminated` mid-chunk) but does NOT
    // synchronously guarantee the thread has exited by the time `cancel()`/the destructor returns --
    // destroying the owning session immediately after cancelling risks a real use-after-free on the
    // detached thread's next loop iteration. Named here because neither red-team pass traced this
    // specific hazard; `prove`'s own tests should exercise cancel-then-destroy timing directly.
    //
    // THREAD-SAFETY INVARIANT for the shared `breakers_` state (Finding 6-new, 5th red-team pass):
    // `rt::CircuitBreaker` (`rt/circuit_breaker.hpp`) is documented single-writer, no internal
    // synchronization -- safe here ONLY because `AgentSession::session_mutex_` already serializes every
    // `run_model_call()` invocation (I1: one session, one executor), so at most one of `call()`'s
    // coroutine-driven access or `call_stream()`'s detached-thread access to `breakers_[i]` is ever live
    // at a time for a given gateway instance. This invariant is the CALLER's responsibility (this class
    // does not and cannot enforce it) -- a hand-built `ModelCallGateway` driven from two genuinely
    // concurrent threads outside `AgentSession`'s own serialization would violate it.
    //
    // Code review finding (2026-08-22): `ctx_copy` below MUST NOT carry `report_progress`/
    // `bound_capabilities` onto the detached thread as-is. `report_progress` is a `std::function` that,
    // whenever a live `invoke_tool()` bracket is open (`rt/agent_session.hpp`'s three bracket sites),
    // captures `[this, call_id]` back into the OWNING `AgentSession` -- calling it off-thread would reach
    // `emit_run_event()`'s unlocked `run_event_seq_by_run_` map, the exact unlocked data race ADR-060 §4
    // already found and fixed once for `tool_pipeline.hpp::background_task()`'s own identical by-value-
    // EffectContext-onto-detached-thread shape (see that function's own comment, `tool_pipeline.hpp:664-
    // 679`). `bound_capabilities` is a raw, non-owning pointer the same file revokes at step 10 --
    // carrying it stale risks a dangling read. Neither is live TODAY at this specific call site (both are
    // only ever bound inside an `invoke_tool()` bracket, and `run_model_call()` -- the only caller of
    // `call_stream()` -- never runs inside one), but that is an unenforced cross-file invariant, not a
    // structural guarantee; reset unconditionally here, matching `tool_pipeline.hpp:679`'s own precedent,
    // rather than relying on callers never changing that invariant.
    [[nodiscard]] stream<ChatResponseUpdate> call_stream(ChatRequest request, EffectContext& ctx) {
        request.idempotency_key = IdempotencyKey{ctx.run_id, ctx.turn_index, 0, 0}.to_string();
        EffectContext ctx_copy = ctx;  // never hold a reference back into the caller's frame cross-thread
        ctx_copy.report_progress = [](ContentItem) {};
        ctx_copy.bound_capabilities = nullptr;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        std::thread(
            [this, request = std::move(request), ctx = std::move(ctx_copy),
             producer = std::move(pair.producer)]() mutable {
                std::stop_token const stop = producer.stop_token();
                stream_tier<0>(request, ctx, producer, stop);
            })
            .detach();
        return std::move(pair.consumer);
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

    // unified-streaming-design-draft.md §3 (Piece A). Compile-time tier recursion, streaming shape --
    // structurally mirrors `try_tier<Tier>` above, but plain synchronous code (no `co_await` needed:
    // `chat_stream()` is already a plain, non-coroutine call per the `ChatClient` concept), driven
    // entirely from `call_stream()`'s own detached thread. `std::optional<error>` return: `nullopt` means
    // the producer was already closed (success) or failed (terminal, reached after `any_pushed` became
    // true) -- nothing more for the caller to do. A real `error` means this tier's retries were exhausted
    // with nothing ever pushed -- `stream_tier<Tier>` decides whether to try the next tier or, if this was
    // the last one, fail the producer with THIS real error (never a generic placeholder).
    template <std::size_t Tier>
    void stream_tier(ChatRequest const& request, EffectContext& ctx,
                      stream_producer<ChatResponseUpdate>& producer, std::stop_token const& stop) {
        if constexpr (Tier == 0) {
            std::optional<error> const failure =
                stream_attempt_with_retry(primary_, breakers_[0], request, ctx, producer, stop);
            if (!failure.has_value()) return;
            if constexpr (sizeof...(Fallback) == 0) {
                producer.fail(*failure);
            } else {
                stream_tier<1>(request, ctx, producer, stop);
            }
        } else {
            auto& backend = std::get<Tier - 1>(fallbacks_);
            std::optional<error> const failure =
                stream_attempt_with_retry(backend, breakers_[Tier], request, ctx, producer, stop);
            if (!failure.has_value()) return;
            if constexpr (Tier < sizeof...(Fallback)) {
                stream_tier<Tier + 1>(request, ctx, producer, stop);
            } else {
                producer.fail(*failure);
            }
        }
    }

    // unified-streaming-design-draft.md §3 (Piece A). The streaming sibling of `attempt_with_retry()`
    // above -- reuses the SAME leaf helpers (`compute_backoff`, `model_call_gateway_detail::is_retryable`/
    // `monotonic_now_ns`, `drained_failure_to_agent_error`) and the SAME breaker/backoff/deadline
    // bookkeeping, but cannot reuse `attempt_with_retry()`'s own body verbatim -- that function is built
    // around one synchronous, fully-buffering `drain_chat_stream()` call with no per-chunk hook for live
    // pushing (5th red-team pass, Finding 3: flagged as a real, non-trivial duplication, not "relocated
    // unchanged" as an earlier draft of this design implied). `any_pushed` is Finding 2's own original
    // commit gate from `model-call-gateway-routing-design-draft.md`: once true, a mid-attempt failure is
    // TERMINAL (`producer.fail(...)`, `nullopt` returned -- no further retry, no fallback tier), matching
    // `call()`'s own single-attempt failure contract for that case via a different, streaming-capable
    // path. `producer.push()` returning anything other than `ok` means the caller dropped/cancelled the
    // stream -- stop immediately, `nullopt` (nothing more to do; there is no caller left to deliver to).
    template <class Backend>
    std::optional<error> stream_attempt_with_retry(Backend& backend, rt::CircuitBreaker& breaker,
                                                      ChatRequest const& request, EffectContext& ctx,
                                                      stream_producer<ChatResponseUpdate>& producer,
                                                      std::stop_token const& stop) {
        std::uint32_t attempts_used = 0;
        bool any_pushed = false;
        for (;;) {
            if (stop.stop_requested()) return std::nullopt;  // caller already gone; nothing to deliver

            rt::Admit const admit = breaker.on_send(model_call_gateway_detail::monotonic_now_ns());
            if (admit == rt::Admit::Shed) {
                error shed_error{failure_class::transient, "circuit open, admission shed",
                                  "gateway.circuit_open"};
                if (any_pushed) {
                    producer.fail(shed_error);
                    return std::nullopt;
                }
                return shed_error;
            }

            stream<ChatResponseUpdate> s = backend.chat_stream(request, ctx);
            std::optional<Usage> last_usage;
            bool terminated_by_consumer = false;
            while (!s.done()) {
                while (std::optional<ChatResponseUpdate> upd = s.next()) {
                    if (upd->is_final && upd->usage.has_value()) last_usage = upd->usage;
                    if (producer.push(*upd) != stream_push::ok) {
                        terminated_by_consumer = true;
                        break;
                    }
                    any_pushed = true;
                }
                if (terminated_by_consumer) break;
                if (!s.done()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (terminated_by_consumer) return std::nullopt;

            // Same fail-closed-on-missing-usage rule `attempt_with_retry()` already enforces for `call()`
            // -- 004 §5's TokenBudget<N> depends on a true per-call token count.
            bool const succeeded = s.terminal() == stream_terminal::closed && last_usage.has_value();
            ++attempts_used;
            breaker.on_result(succeeded, model_call_gateway_detail::monotonic_now_ns());

            if (succeeded) {
                producer.close();
                return std::nullopt;
            }

            error const failure = (s.terminal() == stream_terminal::closed)
                ? error{failure_class::fatal, "streaming call reported no token usage",
                        "gateway.usage_unavailable"}
                : s.fail_error();

            if (any_pushed) {
                producer.fail(drained_failure_to_agent_error(failure, "gateway.attempt_failed"));
                return std::nullopt;
            }

            bool const retryable = model_call_gateway_detail::is_retryable(failure.klass);
            if (!retryable || attempts_used >= retry_policy_.max_attempts) {
                return drained_failure_to_agent_error(failure, "gateway.attempt_failed");
            }

            std::chrono::milliseconds const delay = compute_backoff(attempts_used - 1);
            if (ctx.deadline.time_since_epoch().count() != 0) {
                auto const now = std::chrono::steady_clock::now();
                if (now >= ctx.deadline) {
                    return drained_failure_to_agent_error(failure, "gateway.attempt_failed");
                }
                auto const remaining = ctx.deadline - now;
                if (delay > remaining) {
                    return drained_failure_to_agent_error(failure, "gateway.attempt_failed");
                }
            }
            // Same pre-existing blocking-`sleep_for` residual `attempt_with_retry()` already carries
            // (see that function's own comment) -- not newly introduced here, not fixed here either.
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
