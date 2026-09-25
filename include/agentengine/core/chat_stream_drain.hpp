#pragma once
// Also: ADR-191/192 -- a join requires equal `approval` and `deliver_as_instructions`.
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
#include <utility>
#include <variant>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/stream.hpp"

namespace agentengine {

// Appends one streamed delta to the message being reconstructed. A delta whose producer marked it
// `continues_previous` is joined onto the item before it when that is safe; every other delta is
// appended as an item of its own, exactly as it always was. Both drains in the tree
// (`drain_chat_stream()` below and `rt/agent_session_trust.hpp`'s `drain_streaming_response()`) use
// this; neither pushes a delta directly any more.
//
// WHY THIS IS NOT JUST A MEMORY FIX. A backend pushes one delta per SSE chunk, so a streamed reply
// used to be reconstructed as one `ContentItem` per token -- and every scan that reads a reply reads
// it ONE ITEM AT A TIME. `detect_undeclared_tool_call_leak()` (OQ-23's refuse-don't-accept control)
// and `apply_response_format_scan()` (ADR-035) decode each `Text` item on its own, so a leaked
// `<tool_call>{...}</tool_call>` split across nineteen fragments was invisible to both: measured,
// the non-streamed reply was refused and promoted, the streamed reply carrying the same bytes was
// neither. Streaming was quietly a way around a fail-closed check. Joining the fragments of one piece
// of content gives the scans the run of text they were written for. It also stops a 2000-token reply
// retaining ~30x its own text in 160-byte items that every later turn then deep-copies.
//
// WHY THE PRODUCER DECIDES, NOT THIS FUNCTION. The first version of this joined any two adjacent
// `Text` items. A red-team pass showed that invents boundaries away: Anthropic's accumulator defers
// `tool_use` blocks to `finish()`, so two text blocks either side of a tool call arrive adjacent and
// were glued into one string ("...check.The weather...") that the non-streamed reply keeps as two
// items; a workflow's fan-in hands one agent's text over right after another's and those were glued
// too. Adjacency is not continuity. Only the producer knows whether this fragment belongs to the
// piece of content the last one did (`ChatResponseUpdate::continues_previous`).
//
// WHAT MAY JOIN, EVEN THEN. The producer's say-so is necessary, not sufficient. The two items must
// also be the same kind with identical metadata, so a join can never change what any consumer is told
// about a byte:
//   - `Text` onto `Text`, same `origin`, same `tainted`.
//   - `Reasoning` onto `Reasoning`, same `origin`, same `tainted`, same `producer_chat_client_id`
//     (003 §8 Q2 decides replay eligibility from it), and NEITHER `encrypted`: an encrypted trace is
//     an opaque vendor blob, and concatenating two of them is a corruption, not a coalescing.
// A tool call, media, a `Custom` resume signal, or anything whose metadata differs is never joined.
inline void append_stream_delta(Message& accumulated, ContentItem delta, bool continues_previous) {
    if (continues_previous && !accumulated.content.empty()) {
        ContentItem& back = accumulated.content.back();
        if (back.origin == delta.origin && back.tainted == delta.tainted && back.approval == delta.approval &&
            back.deliver_as_instructions == delta.deliver_as_instructions) {
            if (auto* into = std::get_if<Text>(&back.value)) {
                if (auto* from = std::get_if<Text>(&delta.value)) {
                    into->text += from->text;
                    return;
                }
            } else if (auto* into_r = std::get_if<Reasoning>(&back.value)) {
                if (auto* from_r = std::get_if<Reasoning>(&delta.value);
                    from_r != nullptr && !into_r->encrypted && !from_r->encrypted &&
                    into_r->producer_chat_client_id == from_r->producer_chat_client_id) {
                    into_r->text += from_r->text;
                    return;
                }
            }
        }
    }
    accumulated.content.push_back(std::move(delta));
}

namespace chat_stream_drain_detail {
// A compile-time tripwire, never called. `append_stream_delta()` compares metadata field by field,
// so a field added later to `ContentItem`, `Text` or `Reasoning` would be silently discarded by a
// join. Each structured binding below names every member of its type and stops compiling the moment
// the member count changes -- whoever adds the field is sent here to decide whether it must match
// for a join, instead of finding out from a lost value.
inline void join_compares_every_field(ContentItem const& c, Text const& t, Reasoning const& r) {
    // approval (ADR-191) and deliver_as_instructions (ADR-192) must match for a join.
    [[maybe_unused]] auto const& [c_value, c_origin, c_tainted, c_approval, c_deliver_as_instructions] = c;
    [[maybe_unused]] auto const& [t_text] = t;
    [[maybe_unused]] auto const& [r_text, r_encrypted, r_producer_chat_client_id] = r;
}
}  // namespace chat_stream_drain_detail

// ae-naming-lint: allow DrainedChatStream — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct DrainedChatStream {
    Message accumulated;
    std::optional<Usage> usage;
    bool ok = false;
    error failure{};  // meaningful only when !ok
};

// Drains `s` to completion (poll loop: pull everything currently buffered, sleep briefly if the
// producer is still live but momentarily has nothing ready, repeat until `done()`) and reports
// whether it reached a clean `Closed` terminal or failed. Accumulates every delta's `ContentItem`,
// IN ORDER, into one `Message`, joining the fragments a producer marked as continuing
// (`append_stream_delta()` above)
// -- the SAME reconstruction `AgentSession::run_model_call()`'s own
// streaming branch performs, just without that function's additional live-event-emission and
// fail-closed-on-missing-usage policy layered on top.
[[nodiscard]] inline DrainedChatStream drain_chat_stream(stream<ChatResponseUpdate> s) {
    DrainedChatStream out;
    out.accumulated.role = role::assistant;
    while (!s.done()) {
        while (std::optional<ChatResponseUpdate> upd = s.next()) {
            // A pure argument-chunk update carries no content in `delta` (left at its default), the
            // same rule the session's own drain has applied since unified-streaming-design-draft.md §1
            // Finding 14; appending it here left an empty placeholder item per chunk in every reply
            // reconstructed through this function (the gateway's `call()`, both summarizers).
            if (!upd->tool_call_argument_chunk.has_value()) {
                append_stream_delta(out.accumulated, std::move(upd->delta), upd->continues_previous);
            }
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
