#pragma once
// Implements 004-Model-Provider-Plane.md §4 ("Reliability" / Failover): "Failover between providers
// is explicit policy, never implicit: a failover that silently changes model is a correctness
// change, and must appear in the trace and in the response metadata."
//
// Milestone 5 Phase F3 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, Phase F's
// "Design decisions (2026-08-07, before implementation, IN PROGRESS)" block): F3 is its OWN wrapper,
// separate from F1/F2's `ResilientChatClient` (core/resilient_chat_client.hpp -- a sibling file this
// header does not include and does not depend on existing; see that block for why F1+F2 combine into
// one wrapper while F3 stays separate: retry/circuit-breaking is one call-path concern that must see
// every attempt an inner retry loop makes, while failover is a deliberate, configured step between
// DIFFERENT backends -- never a retry-shaped implicit fallback). The two compose: a caller wanting
// both wraps a `ResilientChatClient<X>` as one of THIS wrapper's `Primary`/`Fallback` template
// arguments (or the other way around), never merged into one type.
//
// `ChatClient` is a concept, never a base class (chat_client.hpp's own top comment: "never inherited
// from on the hot path"; CONVENTIONS.md: "no virtual for policy on the hot path") -- so this wrapper
// is itself a template composing over `Primary`/`Fallback...`, never a virtual/type-erased decorator.
// `Primary`/`Fallback...` are a variadic pack (>= 1 fallback), not a runtime vector of type-erased
// clients: the failover ORDER is a compile-time, declared policy -- a caller writes
// `FailoverChatClient<OpenAIChatClient<S>, AnthropicChatClient<S>>` and the order is right there in
// the type -- matching every other conformer's own compile-time-composition idiom.
//
// `ChatResponse::fallback_tier` (chat_client.hpp, appended last) is the "response metadata" half of
// 004 §4's gate: 0 = Primary answered, N>0 = the Nth fallback (1-based, declaration order) answered.
// This wrapper is the ONE authoritative stamp of that field -- a raw backend conformer
// (OpenAIChatClient/AnthropicChatClient/a test fake) never sets it (it defaults to 0), so whatever
// value an inner `chat()` call happens to return in that field is OVERWRITTEN here, never trusted.
//
// chat_stream() SCOPING DECISION: failover is `chat()`-only this phase; `chat_stream()` always uses
// `Primary` and never falls over. `chat_stream()` returns its `ae::stream<ChatResponseUpdate>`
// SYNCHRONOUSLY and unconditionally (chat_client.hpp's own banner: "the return itself is
// synchronous... hands the stream_producer... off to whatever background execution context performs
// the read loop"). A real backend's failure mode (see protocol/openai/chat_client.hpp's
// chat_stream()) is `producer.fail(...)` called from that background context -- sometimes AFTER
// `chat_stream()` has already returned the consumer handle to the caller, and even after some chunks
// were already delivered (a request-build/validation failure fails before any push; an exchange
// failure can follow real partial delivery). There is no synchronous "did opening this stream
// succeed" signal to decide "should I fall over to the next backend" the way `chat()`'s `result<>`
// gives one -- deciding to fail over only after the stream has already started delivering content to
// the caller would itself BE a silent, mid-stream backend substitution, the exact thing 004 §4
// forbids. So: a caller wanting stream-level failover composes it explicitly at a higher layer (retry
// the whole `chat_stream()` call against a different `ChatClient` after observing
// `terminal() == Failed`), not silently inside this wrapper. Named here rather than silently
// under-implemented (CLAUDE.md's own falsifiable-gate discipline).
//
// capabilities() returns Primary's capabilities only, never a merged/lowest-common-denominator set
// across the whole chain: a caller configuring output-schema-enforcement strategy
// (chat_client.hpp's `select_output_schema_strategy`) or checking `tool_calling`/`streaming`/etc.
// against this wrapper should plan against what actually answers MOST of the time (the primary) --
// merging in the fallbacks' capabilities could claim a capability the primary lacks (wrong whenever
// the primary answers, the common case), and intersecting could hide a capability the primary
// actually has (wrong whenever a narrower-capability fallback exists at all). This is a real
// judgment call, not an obviously-correct default -- named here rather than silently assumed.

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/task.hpp"

namespace agentengine {

namespace failover_detail {

template <class T, class... Ts>
inline constexpr std::size_t count_type_v = (std::size_t{0} + ... + (std::is_same_v<T, Ts> ? 1 : 0));

// Every type in <Primary, Fallback...> appears exactly once -- the failover order is expressed
// entirely by the pack's declaration order, which is meaningless if the same backend type appears
// twice (which of the two same-type slots "is" tier 2 if tier 1 already failed identically?).
template <class... Ts>
inline constexpr bool all_distinct_v = (... && (count_type_v<Ts, Ts...> == 1));

} // namespace failover_detail

// Wraps one primary ChatClient-conforming backend and an ORDERED, non-empty list of fallback
// backends, all distinct types. Conforms to `ChatClient` itself (see the `static_assert`s below --
// the requirement is checked here, not left implicit).
template <class Primary, class... Fallback>
class FailoverChatClient {
    static_assert(ChatClient<Primary>,
                  "FailoverChatClient's Primary must satisfy the ChatClient concept (004 §1)");
    static_assert((ChatClient<Fallback> && ...),
                  "FailoverChatClient's every Fallback must satisfy the ChatClient concept (004 §1)");
    static_assert(sizeof...(Fallback) >= 1,
                  "FailoverChatClient needs at least one fallback (Milestone 5 Phase F3) -- with zero "
                  "fallbacks there is nothing to fail over to; use Primary directly instead");
    static_assert(failover_detail::all_distinct_v<Primary, Fallback...>,
                  "FailoverChatClient's Primary and every Fallback must be distinct types -- the "
                  "failover order is expressed by the pack's declaration order alone");

public:
    explicit FailoverChatClient(Primary primary, Fallback... fallbacks)
        : primary_(std::move(primary)), fallbacks_(std::move(fallbacks)...) {}

    // Primary's capabilities only -- see file-top comment for why merging/intersecting across the
    // whole chain is wrong in either direction.
    [[nodiscard]] ChatClientCapabilities capabilities() const { return primary_.capabilities(); }

    // Tries Primary, then each Fallback in declaration order -- ONE attempt per backend, never
    // retried here (that is F1/`ResilientChatClient`'s job, a separate wrapper this one composes
    // with, not duplicates: "never silent" means every fallback step is a deliberate, distinct,
    // configured backend, not the same backend tried again). Returns the first success, with
    // `ChatResponse::fallback_tier` stamped to the tier that actually answered (0 = Primary, 1 = the
    // first Fallback, 2 = the second, ...), OVERWRITING whatever the inner backend itself may have
    // set -- this wrapper is the one authoritative source of that field. If every backend in the
    // chain errors, returns the LAST backend's error (the most "final" failure), not the first.
    [[nodiscard]] task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext& ctx) {
        result<ChatResponse> primary_result = co_await primary_.chat(request, ctx);
        if (primary_result.has_value()) {
            primary_result->fallback_tier = 0;
            co_return std::move(primary_result);
        }
        co_return co_await try_fallback<0>(request, ctx, std::move(primary_result).error());
    }

    // SCOPING DECISION (see file-top comment): Primary only, no fallback traversal for streaming this
    // phase -- there is no synchronous "did this fail" signal from `chat_stream()`'s return alone to
    // decide "fail over" on without risking a silent mid-stream backend substitution.
    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest const& request, EffectContext& ctx) {
        return primary_.chat_stream(request, ctx);
    }

private:
    // Compile-time recursion over the Fallback pack, one tier at a time -- `I` is the 0-based index
    // into `fallbacks_`; the tier stamped into the response is `I + 1` (0 is reserved for Primary).
    // `last_error` threads the most recent failure forward so that once every backend has been
    // tried, the LAST one's error (not the first) is what gets returned to the caller.
    template <std::size_t I>
    task<result<ChatResponse>> try_fallback(ChatRequest const& request, EffectContext& ctx,
                                             error last_error) {
        if constexpr (I >= sizeof...(Fallback)) {
            co_return std::unexpected(std::move(last_error));
        } else {
            result<ChatResponse> attempt = co_await std::get<I>(fallbacks_).chat(request, ctx);
            if (attempt.has_value()) {
                attempt->fallback_tier = static_cast<std::uint32_t>(I + 1);
                co_return std::move(attempt);
            }
            co_return co_await try_fallback<I + 1>(request, ctx, std::move(attempt).error());
        }
    }

    Primary primary_;
    std::tuple<Fallback...> fallbacks_;
};

} // namespace agentengine
