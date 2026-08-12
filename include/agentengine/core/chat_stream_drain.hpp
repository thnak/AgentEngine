#pragma once
// ADR-035 Phase 3: the shared "drain a chat_stream() call to completion and reconstruct a Message"
// primitive. Three independent call sites need exactly this, none of which needed a fourth copy of
// the same ~15-line poll loop: `core/model_call_gateway.hpp` (each retry/failover attempt),
// `core/memory_provider.hpp` (turn-extraction summarization), `core/history_provider.hpp`
// (`Summarize<N, SummarizerT>`'s window-compaction summarization) -- all three previously called
// `SummarizerT::chat()` directly; this header is what lets them call `chat_stream()` instead
// (Phase 3's actual goal: no `ChatClient` conformer needs to be reached via `chat()` anymore).
//
// Deliberately policy-free: this function does NOT decide retry-worthiness, does NOT fail closed on
// missing usage, does NOT emit any run_event -- those are each caller's OWN concern (`ModelCallGateway`
// decides retries from `quark::errc`; `AgentSession::run_model_call()` fails closed on missing usage
// AND fires `model_delta` events live, which is different enough from every OTHER caller here that it
// keeps its own inline loop rather than using this shared one -- see that function's own comment).
// `memory_provider.hpp`/`history_provider.hpp` don't care about usage at all; this type reports it as
// `std::optional` precisely so a caller that doesn't need it can simply ignore the field.

#include <chrono>
#include <optional>
#include <thread>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/stream.hpp"

#include "quark/core/error.hpp"
#include "quark/core/reply_stream.hpp"

namespace agentengine {

struct DrainedChatStream {
    Message accumulated;
    std::optional<Usage> usage;
    bool ok = false;
    quark::error failure{};  // meaningful only when !ok
};

// Drains `s` to completion (poll loop: pull everything currently buffered, sleep briefly if the
// producer is still live but momentarily has nothing ready, repeat until `done()`) and reports
// whether it reached a clean `Closed` terminal or failed. Accumulates every delta's `ContentItem`,
// IN ORDER, into one `Message` -- the SAME reconstruction `AgentSession::run_model_call()`'s own
// streaming branch performs, just without that function's additional live-event-emission and
// fail-closed-on-missing-usage policy layered on top.
[[nodiscard]] inline DrainedChatStream drain_chat_stream(stream<ChatResponseUpdate> s) {
    DrainedChatStream out;
    out.accumulated.role = role::assistant;
    while (!s.done()) {
        while (std::optional<ChatResponseUpdate> upd = s.next()) {
            out.accumulated.content.push_back(upd->delta);
            if (upd->is_final && upd->usage.has_value()) out.usage = upd->usage;
        }
        // The ring is momentarily empty but the producer thread is still live (a real backend runs
        // its blocking HTTP/SSE read loop on a detached worker thread) -- a bounded sleep, not a
        // bare spin.
        if (!s.done()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (s.terminal() == quark::ReplyStreamTerminal::Closed) {
        out.ok = true;
    } else {
        out.ok = false;
        out.failure = s.fail_error();
    }
    return out;
}

// Maps `quark::errc`'s coarser vocabulary onto this codebase's own `failure_class` -- NOT a blanket
// `transient`, because `failure_class` is what 004 §4's retry policy and `workflow/supervisor.hpp`'s
// `is_retryable()` actually key off. Getting this wrong means a permanent, non-retryable failure
// (e.g. `quark::errc::validation`, which `classify_http_status_stream_error` uses for a policy-denied
// or malformed request -- see that function's own comment) would be relabeled retryable and retried
// forever, an I2 hazard (retrying a denial is asking the same question until a different answer
// comes back). Mirrors `ModelCallGateway`'s own `is_retryable()` split (only `unavailable`/`timeout`/
// `overloaded` are retryable) plus `map_http_status_error`'s status->class mapping, re-expressed over
// `quark::errc`'s thinner shape.
[[nodiscard]] inline failure_class classify_drained_failure(quark::errc code) noexcept {
    switch (code) {
        case quark::errc::unavailable:
        case quark::errc::timeout:
        case quark::errc::overloaded:
            return failure_class::transient;
        case quark::errc::circuit_open:
            return failure_class::resource;
        case quark::errc::validation:
        case quark::errc::not_found:
        case quark::errc::serialization:
            return failure_class::contract;
        case quark::errc::ok:
        case quark::errc::cancelled:
        case quark::errc::supervised_stop:
        case quark::errc::internal:
        default:
            return failure_class::fatal;
    }
}

// Converts a drained failure into this codebase's own `ae::error` shape -- `quark::error::detail` is
// a non-owning `string_view` pointing at a static literal (every real producer of one is required to
// use a static literal, never a dynamic message -- see `protocol/openai/chat_client.hpp`'s own
// `classify_http_status_stream_error` comment for why), so copying it into an owned `std::string`
// here, at the point it's finally read, is a safe, ordinary copy.
[[nodiscard]] inline error drained_failure_to_agent_error(quark::error const& e, char const* code) noexcept {
    return error{classify_drained_failure(e.code), std::string(e.detail), code};
}

}  // namespace agentengine
