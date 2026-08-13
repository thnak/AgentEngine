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
// decides retries from `failure_class`; `AgentSession::run_model_call()` fails closed on missing usage
// AND fires `model_delta` events live, which is different enough from every OTHER caller here that it
// keeps its own inline loop rather than using this shared one -- see that function's own comment).
// `memory_provider.hpp`/`history_provider.hpp` don't care about usage at all; this type reports it as
// `std::optional` precisely so a caller that doesn't need it can simply ignore the field.
//
// ADR-037 (second pass): `DrainedChatStream::failure` is now `agentengine::error` directly -- a
// producer that fails a stream already constructs a real, correctly-classified `error` (see
// `core/stream.hpp`'s own migration), so there is no longer a thinner `quark::errc` vocabulary to
// translate FROM. `classify_drained_failure(quark::errc)` is gone entirely: `failure_class` already
// IS the retry-relevant classification (004 §4), carried on `error` itself now, not derived from it
// after the fact.

#include <chrono>
#include <optional>
#include <thread>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/stream.hpp"

namespace agentengine {

struct DrainedChatStream {
    Message accumulated;
    std::optional<Usage> usage;
    bool ok = false;
    error failure{};  // meaningful only when !ok
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
    if (s.terminal() == stream_terminal::closed) {
        out.ok = true;
    } else {
        out.ok = false;
        out.failure = s.fail_error();
    }
    return out;
}

// Re-labels a drained failure's `code` field for a caller that wants every failure funneled through
// its own stable identifier (e.g. `ModelCallGateway::call()`'s callers matching on
// "gateway.attempt_failed" regardless of which underlying backend/reason produced it) -- `klass`/
// `message`/`native_code` all pass through unchanged, since `error` already carries the real,
// correctly-classified `failure_class` from wherever it was constructed (004 §4's retry policy keys
// off `klass == failure_class::transient` directly; there is no separate coarser vocabulary left to
// translate through, unlike the old `quark::errc`-keyed `classify_drained_failure` this replaces).
[[nodiscard]] inline error drained_failure_to_agent_error(error const& e, char const* code) noexcept {
    return error{e.klass, e.message, code, e.native_code};
}

}  // namespace agentengine
