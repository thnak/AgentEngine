#pragma once
// Implements 004-Model-Provider-Plane.md §4 (Reliability) -- Milestone 5 Phase F1+F2
// (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, Phase F's "Design decisions
// (2026-08-07, before implementation, IN PROGRESS)" block -- READ THAT BLOCK, it is the
// authoritative design this file follows, not re-derived here).
//
// F1 (retry) and F2 (circuit breaking) are ONE wrapper, `ResilientChatClient<Inner>`, not two
// composed layers: the breaker must see the outcome of every attempt the retry loop makes, and a
// breaker admission check gates each attempt before the loop retries it. Two separate wrapper
// layers would either duplicate the attempt loop or need to leak breaker state between layers.
//
// `ChatClient` is a concept, never a base class (chat_client.hpp's own top comment: "never
// inherited from on the hot path") -- so this is a template wrapper composing over an `Inner`
// template parameter, exactly the shape every existing conformer already uses
// (`OpenAIChatClient<Store>`/`AnthropicChatClient<Store>`), never a virtual/type-erased decorator.
//
// `RetryPolicy`/`BreakerConfig` are plain RUNTIME constructor structs, not compile-time template
// parameters: retry timing and breaker thresholds are operational knobs in the same spirit as
// 004 §5's "pricing tables are configuration, not code," and every existing backend already takes
// its own operational config (host/port/model) as runtime constructor args, not template params.
//
// BREAKER KEY: 004 §4/decision 6 say the breaker key is `{provider, model, SecretRef}`. One
// `OpenAIChatClient`/`AnthropicChatClient` instance is already constructed bound to exactly one
// provider+model+SecretRef triple, so wrapping it needs no runtime key/map at all: this wrapper's
// own single `quark::CircuitBreaker` member IS that key, by construction.
//
// CLOCKS: Quark's governance/deadline primitives are clock-free (`governance.hpp`'s own file
// banner: "every time-dependent method takes an explicit monotonic now_ns"). This file keeps the
// two clock domains separate, never conflated:
//   - breaker/backoff bookkeeping ("what time is it, for CircuitBreaker::on_send/on_result")
//     uses `quark::monotonic_now_ns()`.
//   - "how much deadline budget is left" uses `std::chrono::steady_clock::now()` vs
//     `EffectContext::deadline` directly -- both are `steady_clock`, so the subtraction is valid
//     with no PAL bridging needed. The zero-value sentinel check
//     (`ctx.deadline.time_since_epoch().count() != 0`) is the exact idiom `core/tool_pipeline.hpp`
//     (around line 296) already uses for "no deadline set."

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <utility>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/core/tool_pipeline.hpp"  // IdempotencyKey

#include "quark/core/deadline.hpp"    // quark::monotonic_now_ns()
#include "quark/core/governance.hpp"  // quark::CircuitBreaker, quark::Admit

namespace agentengine {

// F1 (004 §4): "bounded exponential with jitter, respecting the remaining deadline." A plain
// runtime constructor struct, not a template parameter (see file-top comment).
struct RetryPolicy {
    // Total attempts INCLUDING the first, real (non-shed) attempt. 1 == no retries at all.
    std::uint32_t max_attempts = 3;
    std::chrono::milliseconds base_delay{100};
    std::chrono::milliseconds max_delay{5000};
    // Backoff formula (this file's own design choice -- 004 §4 names "bounded exponential with
    // jitter" but no exact algorithm): for the Nth retry (0-based), the unjittered delay is
    // `min(max_delay, base_delay * 2^N)`; jitter then scales that by `1 + jitter_fraction * r`
    // where `r` is drawn from `JitterSource` in [-1.0, 1.0] -- i.e. +/- `jitter_fraction` of the
    // computed exponential delay, never negative after clamping. 0.2 == +/-20%.
    double jitter_fraction = 0.2;
};

// F2 (004 §4 / 022 §3): a plain runtime constructor struct, not a template parameter (same
// reasoning as `RetryPolicy` above). Forwarded verbatim into the wrapper's own
// `quark::CircuitBreaker` member.
struct BreakerConfig {
    std::uint32_t fail_threshold = 5;
    std::chrono::milliseconds open_duration{30000};
};

namespace resilient_chat_client_detail {

// The real jitter source: a thread-local PRNG (never shared mutable state across threads/actors),
// seeded from `std::random_device`. Returns a value uniformly in [-1.0, 1.0].
[[nodiscard]] inline double real_jitter() {
    thread_local std::mt19937_64 engine{std::random_device{}()};
    thread_local std::uniform_real_distribution<double> dist(-1.0, 1.0);
    return dist(engine);
}

}  // namespace resilient_chat_client_detail

// `ResilientChatClient<Inner>` itself conforms to the `ChatClient` concept (chat_client.hpp) --
// `capabilities()`/`chat()`/`chat_stream()` all forward to `Inner`, with F1 retry + F2 circuit
// breaking wrapped around `chat()`, and F2 admission (only) wrapped around `chat_stream()`.
template <class Inner>
class ResilientChatClient {
public:
    // Injectable jitter/random source -- default real (`resilient_chat_client_detail::real_jitter`
    // above), overridable in tests for determinism. Mirrors this codebase's established
    // testability-seam pattern, `sandbox/provider_http_client.hpp`'s injectable `resolver`
    // parameter (that file's own comment: "a testability seam, not a security bypass" -- same
    // reasoning here: production code never passes a non-default jitter source, a test does, so it
    // can assert exact backoff timing/attempt counts without real randomness in the loop).
    using JitterSource = std::function<double()>;

    explicit ResilientChatClient(Inner inner, RetryPolicy retry_policy = {},
                                  BreakerConfig breaker_config = {},
                                  JitterSource jitter = &resilient_chat_client_detail::real_jitter)
        : inner_(std::move(inner)),
          retry_policy_(retry_policy),
          breaker_(breaker_config.fail_threshold,
                   std::chrono::duration_cast<std::chrono::nanoseconds>(breaker_config.open_duration)
                       .count()),
          jitter_(std::move(jitter)) {
        // A retry policy of 0 attempts is nonsensical (chat() must try at least once) -- clamp
        // rather than let a misconfigured caller silently get zero attempts and an unreachable
        // return path.
        if (retry_policy_.max_attempts == 0) retry_policy_.max_attempts = 1;
    }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return inner_.capabilities(); }

    // F1 (retry, 004 §4) + F2 (circuit breaking, 004 §4/022 §3), combined per the file-top
    // rationale. `request` is taken BY VALUE deliberately: this function needs its own mutable
    // copy to stamp the idempotency key onto once and reuse verbatim across every retry, without
    // mutating whatever the caller passed in (which may itself be reused by the caller).
    task<result<ChatResponse>> chat(ChatRequest request, EffectContext& ctx) {
        // Milestone 5 Phase F1 (004 §4: "a retried call carries a stable idempotency key"):
        // computed ONCE, before the first attempt, from exactly the two identity fields
        // `EffectContext` carries (`run_id`, `turn_index`) -- `call_index`/`argument_digest` are 0
        // here (this is a provider-level retry, not `tool_pipeline.hpp`'s per-tool-call effect
        // journal; there is no "call index" or "arguments" at this layer). Reused verbatim, never
        // regenerated, across every attempt below -- that reuse is what "stable" means (004 §4).
        request.idempotency_key = IdempotencyKey{ctx.run_id, ctx.turn_index, 0, 0}.to_string();

        std::uint32_t attempts_used = 0;
        for (;;) {
            // -- F2: admission gate, checked before EVERY attempt (including the first) ----------
            quark::Admit const admit = breaker_.on_send(quark::monotonic_now_ns());
            if (admit == quark::Admit::Shed) {
                // The breaker is Open (or a half-open probe is already in flight): fail fast
                // without ever reaching `Inner` -- no network call, no attempt.
                //
                // Attempt-counting interaction (F1/F2, spec asked to document the exact
                // interaction): a shed never increments `attempts_used` -- that counter is only
                // incremented after an attempt actually reaches `Inner` (below) -- so a shed never
                // consumes a slot of the caller's `RetryPolicy::max_attempts` budget the way a
                // real failed HTTP call does. We do NOT, however, loop/wait out the breaker's
                // cooldown inside this single `chat()` call: while Open, every subsequent
                // `on_send` would shed again until `open_duration` elapses, so spinning here would
                // either busy-loop or require duplicating backoff-style sleep logic against a
                // second, unrelated clock (the breaker's cooldown, not the retry backoff) --
                // muddying "one call path, one budget." Instead we return this shed as this call's
                // outcome immediately; a fresh, later `chat()` call re-checks admission from
                // scratch against the (persistent, instance-lifetime) breaker state, and gets the
                // real half-open probe once the cooldown elapses.
                co_return std::unexpected(
                    error{failure_class::transient, "circuit open, admission shed",
                          "chat_client.circuit_open"});
            }

            result<ChatResponse> res = co_await inner_.chat(request, ctx);
            ++attempts_used;  // a REAL attempt reached Inner -- this is the count max_attempts bounds
            bool const ok = res.has_value();
            breaker_.on_result(ok, quark::monotonic_now_ns());

            if (ok) co_return res;

            error const& failure = res.error();
            if (failure.klass != failure_class::transient) {
                // Non-transient (policy/contract/resource/fatal): 004 §4 scopes retry to
                // `Transient` only (001 §6) -- anything else is the caller's answer, verbatim.
                co_return res;
            }

            if (attempts_used >= retry_policy_.max_attempts) {
                // Retry budget (attempt COUNT) exhausted -- return the last real failure.
                co_return res;
            }

            std::chrono::milliseconds const delay = compute_backoff(attempts_used - 1);

            // -- F1: never let a retry's sleep push past the caller's remaining deadline ---------
            // Zero-value sentinel check: the exact "no deadline set" idiom `core/tool_pipeline.hpp`
            // (around line 296) already uses for `EffectContext::deadline`.
            if (ctx.deadline.time_since_epoch().count() != 0) {
                auto const now = std::chrono::steady_clock::now();
                if (now >= ctx.deadline) {
                    // No budget left at all -- stop retrying, return the last real error.
                    co_return res;
                }
                auto const remaining = ctx.deadline - now;
                if (delay > remaining) {
                    // The computed backoff can't fit before the deadline -- stop retrying rather
                    // than sleep past it (spec: "If the remaining budget can't fit another
                    // attempt's backoff, stop retrying and return the last error").
                    co_return res;
                }
            }

            std::this_thread::sleep_for(delay);
            // loop: the NEXT iteration's breaker admission check gates the next attempt.
        }
    }

    // F2 admission gate only -- 004 §1's literal `chat_stream()` signature has no error channel to
    // carry a "circuit open" result through, and retrying a stream mid-flight is explicitly out of
    // scope for this wrapper (F1 applies to `chat()` only).
    stream<ChatResponseUpdate> chat_stream(ChatRequest request, EffectContext& ctx) {
        // Same idempotency-key stamping as chat(), for request-shape consistency -- costs nothing,
        // and no retry will ever reuse it here since chat_stream() makes exactly one attempt.
        request.idempotency_key = IdempotencyKey{ctx.run_id, ctx.turn_index, 0, 0}.to_string();

        quark::Admit const admit = breaker_.on_send(quark::monotonic_now_ns());
        if (admit == quark::Admit::Shed) {
            // No error slot to carry "circuit open" through 004 §1's bare `stream<T>` return type.
            // A default-constructed `stream<T>` is already invalid/done() immediately
            // (`core/stream.hpp`: st_ == nullptr -> terminal() == Cancelled, done() == true right
            // away) -- the identical "empty, invalid stream" shape
            // `tests/support/recorded_chat_client.hpp`'s own `chat_stream()` already returns for
            // its own no-streaming-support case. A caller sees an immediately-done, Cancelled
            // stream, indistinguishable from "cancelled before it started."
            return stream<ChatResponseUpdate>{};
        }

        // F2 stream-feedback limitation (named explicitly per the task's own instruction, not
        // faked): `breaker_.on_result(...)` is deliberately NOT called for streaming calls.
        // `stream<T>` (core/stream.hpp) exposes no race-free "this stream ultimately succeeded or
        // failed" signal available HERE, synchronously, before returning -- chat_stream() must
        // return the drain handle to the caller immediately (stream.hpp's own file banner:
        // "chat_stream() itself returns synchronously and cannot keep producing after it
        // returns"), so this wrapper has already handed off before any item, success, or failure
        // is observable. Peeking at `done()`/`terminal()` right after `Inner::chat_stream()`
        // returns would almost always see the pre-production Open state, not a real outcome --
        // that would be faking a signal, not reading one. Consequence: the breaker is actively
        // managed (on_send admission gate, on_result feedback) only via `chat()`'s own outcomes;
        // `chat_stream()` contributes admission checks but never feeds results back. A provider
        // that fails ONLY on streaming calls will not trip this breaker until/unless `chat()` is
        // also called against the same instance.
        return inner_.chat_stream(std::move(request), ctx);
    }

private:
    [[nodiscard]] std::chrono::milliseconds compute_backoff(std::uint32_t retry_index) const {
        double const exp_factor = std::pow(2.0, static_cast<double>(retry_index));
        double const base_ms = static_cast<double>(retry_policy_.base_delay.count()) * exp_factor;
        // std::min caps an overflowing/infinite `base_ms` (e.g. a large retry_index) down to a
        // finite `max_delay` BEFORE it is ever cast to an integer type -- safe regardless of how
        // large `exp_factor` grows.
        double const capped_ms =
            std::min(base_ms, static_cast<double>(retry_policy_.max_delay.count()));
        double const r = jitter_ ? jitter_() : 0.0;  // jitter_ always set (constructor default)
        double jittered_ms = capped_ms * (1.0 + retry_policy_.jitter_fraction * r);
        if (jittered_ms < 0.0) jittered_ms = 0.0;  // a negative jitter draw never yields "retry sooner than now"
        return std::chrono::milliseconds(static_cast<std::int64_t>(jittered_ms));
    }

    Inner inner_;
    RetryPolicy retry_policy_;
    quark::CircuitBreaker breaker_;  // this member IS the {provider, model, SecretRef} key, by construction
    JitterSource jitter_;
};

static_assert(true, "ResilientChatClient<Inner>'s ChatClient-concept conformance is asserted per "
                     "instantiation by its own tests (a template can't be static_assert-checked "
                     "against a concept without a concrete Inner).");

}  // namespace agentengine
