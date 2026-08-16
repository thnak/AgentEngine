#pragma once
// Implements 004-Model-Provider-Plane.md §4 (Reliability) -- Milestone 5 Phase F1+F2
// (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, Phase F's "Design decisions
// (2026-08-07, before implementation, IN PROGRESS)" block).
//
// `RetryPolicy`/`BreakerConfig`/`real_jitter` -- F1 (retry) and F2 (circuit breaking)'s shared,
// runtime constructor-struct configuration. Originally defined alongside `ResilientChatClient<Inner>`
// (the `chat()`-only wrapper that first implemented F1+F2, Milestone 5), which composed them into ONE
// wrapper rather than two layers -- the breaker had to see every retry attempt's outcome, and a
// breaker admission check had to gate each attempt before the loop retried it.
//
// `ResilientChatClient` itself was REMOVED 2026-08-12 (along with `FailoverChatClient` and
// `MiddlewareChatClient` -- see ADR-036 §7's residual, closed by that removal): this repo had shipped
// nowhere, so there was no deprecation-then-migration cost to justify keeping a `chat()`-only wrapper
// once `ModelCallGateway<Primary, Fallback...>` (ADR-036) gave `AgentSession` a real, streaming-
// capable retry+breaker+failover path with the identical F1+F2 combined-not-layered reasoning (see
// `model_call_gateway.hpp`'s own file-top comment). This file is what survived that removal: the
// three types below are genuinely shared configuration, reused verbatim (not reimplemented) by
// `ModelCallGateway::attempt_with_retry` -- one set of correct, already-tested primitives, not two
// independent copies that could drift.
//
// `RetryPolicy`/`BreakerConfig` are plain RUNTIME constructor structs, not compile-time template
// parameters: retry timing and breaker thresholds are operational knobs in the same spirit as
// 004 §5's "pricing tables are configuration, not code."

#include <chrono>
#include <cstdint>
#include <random>

namespace agentengine {

// F1 (004 §4): "bounded exponential with jitter, respecting the remaining deadline."
// ae-naming-lint: allow RetryPolicy — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct RetryPolicy {
    // Total attempts INCLUDING the first, real (non-shed) attempt. 1 == no retries at all.
    std::uint32_t max_attempts = 3;
    std::chrono::milliseconds base_delay{100};
    std::chrono::milliseconds max_delay{5000};
    // Backoff formula (this file's own design choice -- 004 §4 names "bounded exponential with
    // jitter" but no exact algorithm): for the Nth retry (0-based), the unjittered delay is
    // `min(max_delay, base_delay * 2^N)`; jitter then scales that by `1 + jitter_fraction * r`
    // where `r` is drawn from a jitter source in [-1.0, 1.0] -- i.e. +/- `jitter_fraction` of the
    // computed exponential delay, never negative after clamping. 0.2 == +/-20%.
    double jitter_fraction = 0.2;
};

// F2 (004 §4 / 022 §3): a plain runtime constructor struct, forwarded verbatim into a caller's own
// `quark::CircuitBreaker` member.
// ae-naming-lint: allow BreakerConfig — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct BreakerConfig {
    std::uint32_t fail_threshold = 5;
    std::chrono::milliseconds open_duration{30000};
};

namespace resilient_chat_client_detail {

// The real jitter source: a thread-local PRNG (never shared mutable state across threads/actors),
// seeded from `std::random_device`. Returns a value uniformly in [-1.0, 1.0]. Kept in this
// (historically named) namespace rather than renamed -- ModelCallGateway's own default jitter
// argument already refers to it by this qualified name, and renaming buys nothing beyond mechanical
// churn.
[[nodiscard]] inline double real_jitter() {
    thread_local std::mt19937_64 engine{std::random_device{}()};
    thread_local std::uniform_real_distribution<double> dist(-1.0, 1.0);
    return dist(engine);
}

}  // namespace resilient_chat_client_detail

}  // namespace agentengine
