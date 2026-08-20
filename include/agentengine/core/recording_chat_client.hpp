#pragma once
// Implements 004-Model-Provider-Plane.md §6 ("Recording and replay") -- Milestone 5 Phase G1
// (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, Phase G / decision 8): "promote
// the recording mechanism from RecordedChatClient's test-scoped fixture player to the real thing:
// every real backend (D/E) records request, response or full ordered chunk sequence, timing, and
// usage." `RecordingChatClient<Inner>` is that real mechanism -- a `ChatClient`-conforming wrapper
// around ANY conforming `Inner` (Phase D's `OpenAIChatClient`, Phase E's Anthropic backend, or a test
// fake), so recording is available uniformly regardless of which concrete backend produced the call.
//
// Builds directly on `core/chat_recording.hpp`'s already-proven JSON envelope
// (`ChatCallRecording`/`RecordedChunk`/`recording_mode`) -- this file owns none of that codec, only
// the capture/timing logic that fills it in from a live call.
//
// `ChatClient` is a concept, never a base class (chat_client.hpp's own top comment) -- so this is a
// template wrapper composing over `Inner`, never a virtual/type-erased decorator (the same shape
// `ModelCallGateway<Primary, Fallback...>`, core/model_call_gateway.hpp, composes over its own
// backends).
//
// SINK, NOT FILE I/O: the constructor takes an injectable `RecordingSink` (`std::function<void
// (ChatCallRecording)>`) -- the same testability-seam pattern as `ModelCallGateway`'s injectable
// `JitterSource` (that file's own comment: "a testability seam, not a security bypass"). This class
// never opens a file itself; a caller wanting durable recordings supplies a sink that calls
// `write_chat_call_recording` (chat_recording.hpp) on its own chosen path -- keeping "how a recording
// is captured" (this file) and "where it goes" (the caller's sink) two separate concerns, so a test
// can capture recordings into an in-memory vector with a trivial lambda.
//
// TRANSPARENCY: recording must never change what the caller observes. `chat()` returns the inner
// `result<ChatResponse>` completely unchanged (success or failure, both recorded -- a later replay
// must be able to reproduce an error, not just a success). `chat_stream()` re-delivers every chunk
// Inner produced, in Inner's own order, with Inner's own terminal condition mirrored onto the new
// stream -- 004 §7 G3's gate ("a recorded streamed run replays offline with identical chunk
// boundaries") starts here, at the recording side, by never reordering or dropping anything in
// transit.
//
// WHY A DETACHED BACKGROUND THREAD + THE POLL-LOOP IDIOM FOR chat_stream(): `chat_stream()` itself
// must return synchronously (chat_client.hpp's own banner: "the return itself is synchronous... a
// conformer... hands the stream_producer... off to whatever background execution context performs
// the read loop") -- there is nothing to `co_await` here, `Inner::chat_stream()` has ALREADY handed
// back its own consumer handle (`stream<ChatResponseUpdate>`) by the time this wrapper's own
// `chat_stream()` is called, and draining it is a poll-only operation (`stream<T>::next()` never
// blocks, per core/stream.hpp's own file banner) that must run concurrently with -- not before -- the
// caller draining the NEW stream this wrapper hands back. So: spawn one thread that (a) drains
// Inner's stream with the exact `while (!s.done()) { while (auto x = s.next()) {...}; if (!s.done())
// yield(); }` idiom `tests/test_chat_client_stream.cpp` (~lines 148-198) already established as this
// codebase's precedent, recording each chunk's content and elapsed time as it goes and re-pushing it
// into the new stream, then (b) once Inner's stream reaches its terminal, builds and emits the final
// `ChatCallRecording` BEFORE mirroring that terminal onto the new stream's producer -- see the
// in-code comment at that call site for why that ordering (not the reverse) is the one that keeps a
// caller-observable guarantee intact. The thread is DETACHED, not a tracked member, matching
// `protocol/openai/chat_client.hpp`'s own `chat_stream()` precedent verbatim: a single
// `RecordingChatClient` instance can be shared/reused across many concurrent streaming calls (the
// same instance backing many turns), so a tracked member thread that joins-then-replaces would
// wrongly serialize concurrent streams that have nothing to do with each other.
//
// PRODUCER-SIDE TERMINAL MIRRORING IS ONLY A PARTIAL MIRROR, NAMED HONESTLY: `stream_producer<T>`
// (core/stream.hpp) exposes exactly two producer-side terminal setters, `close()` (-> closed) and
// `fail(error)` (-> failed). `cancelled` has NO producer-side setter at all -- `stream<T>::cancel()`
// is a CONSUMER-side operation. So when Inner's stream ends cancelled, this wrapper's own outbound
// producer cannot literally reproduce that exact terminal cause through the public API -- it calls
// `fail()` with a translated `error` as the closest honest approximation, rather than silently
// mislabeling it as a plain Closed success. The RECORDING itself (`ChatCallRecording::stream_terminal`,
// a plain string) is NOT subject to this restriction and always records the real cause exactly
// ("closed"/"cancelled"/"deadline_exceeded"/"failed" -- "deadline_exceeded" a wire-format value kept
// for backward compatibility with recordings made before ADR-037's stream.hpp migration, see
// `replay_chat_client.hpp`'s own `terminal_to_error`; no LIVE stream can reach it anymore, since
// `rt::channel_terminal` -- this codebase's actual backend since that migration -- has no deadline
// concept at all).

#include <chrono>
#include <functional>
#include <memory_resource>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/chat_recording.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/core/task.hpp"

namespace agentengine {

namespace recording_chat_client_detail {

// The default sink: does nothing. "Nothing meaningful in product code" (task brief) -- there is no
// single "real" default for "what to do with a recording" the way ModelCallGateway's jitter has a
// real PRNG default; a caller who wants recordings persisted supplies a sink that does so (see
// file-top comment). Named/addressable (not an inline lambda default arg) to mirror
// `resilient_chat_client_detail::real_jitter`'s own function-pointer-default idiom.
inline void discard_recording(ChatCallRecording) noexcept {}

// `stream_terminal` (open/closed/cancelled/failed) -> the wire string `ChatCallRecording::
// stream_terminal` expects (chat_recording.hpp's own field comment names four strings, including
// "deadline_exceeded" -- see file banner for why a LIVE stream can never produce that one anymore;
// this switch has no case for it since it is not a reachable input here). Only meaningful once a
// stream has actually reached a terminal (i.e. `done()` is true) -- `open` has no wire representation
// and is unreachable from that call site.
[[nodiscard]] inline std::string_view stream_terminal_to_wire_string(stream_terminal terminal) noexcept {
    switch (terminal) {
        case stream_terminal::closed:    return "closed";
        case stream_terminal::cancelled: return "cancelled";
        case stream_terminal::failed:    return "failed";
        case stream_terminal::open:      return "";
    }
    return "";
}

}  // namespace recording_chat_client_detail

// `RecordingChatClient<Inner>` itself conforms to the `ChatClient` concept (chat_client.hpp) --
// `capabilities()` forwards to `Inner`; `chat()`/`chat_stream()` wrap `Inner`'s own calls with
// capture-and-emit recording that never changes what the caller observes (see file-top comment).
template <class Inner>
// ae-naming-lint: allow RecordingChatClient — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class RecordingChatClient {
    // LegacyChatClient (chat_client.hpp), not ChatClient -- this type's own chat()/chat_stream()
    // bodies below call `inner_.chat(...)` directly, which the relaxed `ChatClient` concept
    // (ADR-035 Phase 3) no longer guarantees exists (code review finding, 2026-08-12).
    static_assert(LegacyChatClient<Inner>,
                  "RecordingChatClient's Inner must satisfy the LegacyChatClient concept (004 §1)");

public:
    // Injectable capture sink -- the single seam this class offers (see file-top comment: "SINK, NOT
    // FILE I/O"). Defaults to `discard_recording` (does nothing); production code that wants
    // recordings persisted supplies a sink that calls `write_chat_call_recording` itself.
    using RecordingSink = std::function<void(ChatCallRecording)>;

    explicit RecordingChatClient(Inner inner, RecordingSink sink = &recording_chat_client_detail::discard_recording)
        : inner_(std::move(inner)), sink_(std::move(sink)) {}

    [[nodiscard]] ChatClientCapabilities capabilities() const { return inner_.capabilities(); }

    // Records BOTH outcomes (success and failure) -- 004 §6's "response... and usage" plus this
    // task's own "so a later replay can reproduce an error too" -- and always returns Inner's own
    // `result<ChatResponse>` UNCHANGED: recording is a transparent, behavior-preserving observer of
    // the call, never a participant in what the caller sees.
    task<result<ChatResponse>> chat(ChatRequest request, EffectContext& ctx) {
        // A copy taken BEFORE `request` is moved into Inner -- ChatCallRecording::request needs its
        // own value (chat_recording.hpp), and Inner may consume/mutate its own by-value parameter.
        ChatRequest request_for_recording = request;

        auto const start = std::chrono::steady_clock::now();
        result<ChatResponse> outcome = co_await inner_.chat(std::move(request), ctx);
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        ChatCallRecording rec;
        rec.request = std::move(request_for_recording);
        rec.mode = recording_mode::unary;
        rec.duration = elapsed;
        if (outcome.has_value()) {
            rec.response = *outcome;
        } else {
            rec.chat_error = outcome.error();
        }
        sink_(std::move(rec));

        co_return outcome;
    }

    // Drains Inner's own stream on a detached background thread, re-delivering every chunk (same
    // content, same order, same boundaries) through a NEW stream this function returns synchronously
    // -- see file-top comment for the full "why a detached thread" rationale.
    stream<ChatResponseUpdate> chat_stream(ChatRequest request, EffectContext& ctx) {
        ChatRequest request_for_recording = request;  // see chat()'s own comment -- same reasoning
        stream<ChatResponseUpdate> inner_stream = inner_.chat_stream(std::move(request), ctx);

        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());

        // Every capture is by value/move -- no reference back to `*this` (mirrors
        // protocol/openai/chat_client.hpp's run_stream_worker precisely: a `RecordingChatClient`
        // instance may be destroyed, reused, or have another concurrent `chat_stream()` call started
        // against it long before this thread finishes).
        std::thread(
            [inner_stream = std::move(inner_stream), producer = std::move(pair.producer),
             request = std::move(request_for_recording), sink = sink_]() mutable {
                auto const start = std::chrono::steady_clock::now();
                std::vector<RecordedChunk> chunks;
                // Once the caller drops/cancels the NEW stream, producer.push() starts reporting
                // Terminated -- stop pushing further items into a torn-down ring, but keep draining
                // and recording Inner's stream to completion regardless, so the recording stays
                // complete even when the caller stopped listening early.
                bool downstream_alive = true;

                // The exact poll-loop idiom this codebase already established
                // (tests/test_chat_client_stream.cpp ~148-198) -- `next()` is poll-only, never
                // blocking (core/stream.hpp's own file banner).
                while (!inner_stream.done()) {
                    while (auto update = inner_stream.next()) {
                        auto const elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start);
                        RecordedChunk chunk;
                        chunk.update = *update;  // copy for the recording...
                        chunk.elapsed_since_start = elapsed_ms;
                        chunks.push_back(std::move(chunk));

                        if (downstream_alive) {
                            // ...then move the original into the new stream -- the caller-facing
                            // delivery, never reordered or altered relative to Inner's own sequence.
                            if (producer.push(std::move(*update)) != stream_push::ok) {
                                downstream_alive = false;
                            }
                        }
                    }
                    if (!inner_stream.done()) std::this_thread::yield();
                }

                stream_terminal const terminal = inner_stream.terminal();
                std::string const terminal_wire =
                    std::string(recording_chat_client_detail::stream_terminal_to_wire_string(terminal));

                ChatCallRecording rec;
                rec.request = std::move(request);
                rec.mode = recording_mode::streaming;
                rec.chunks = std::move(chunks);
                rec.stream_terminal = terminal_wire;
                if (terminal == stream_terminal::failed) {
                    rec.stream_error_detail = inner_stream.fail_error().message;
                }
                rec.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);

                // Emit the recording BEFORE latching the new stream's own terminal below: a caller
                // draining the returned stream observes `done()` become true only once its own
                // producer's terminal is latched (the very next statements) -- calling the sink
                // FIRST guarantees the recording is already committed to the sink by the time any
                // caller polling this wrapper's own stream could possibly observe completion. Doing
                // it in the opposite order would let a caller race ahead of the recording (see
                // `terminal()`/`done()` becoming true the instant `close()`/`fail()` below runs).
                sink(std::move(rec));

                // Mirror Inner's terminal onto the new producer -- exactly (closed/failed) where the
                // public API allows it, translated where it does not (cancelled -- see file-top
                // comment "PRODUCER-SIDE TERMINAL MIRRORING IS ONLY A PARTIAL MIRROR"). No separate
                // "deadline_exceeded" case: `stream_terminal` (rt::channel_terminal) structurally
                // cannot produce one.
                switch (terminal) {
                    case stream_terminal::closed:
                        producer.close();
                        break;
                    case stream_terminal::failed:
                        producer.fail(inner_stream.fail_error());
                        break;
                    case stream_terminal::cancelled:
                        producer.fail(error{failure_class::fatal,
                                             "the inner (recorded) chat_stream() call was cancelled",
                                             "recording_chat_client.inner_stream_cancelled"});
                        break;
                    case stream_terminal::open:
                    default:
                        // Unreachable: inner_stream.done() == true (the outer while's own exit
                        // condition) guarantees terminal() != open here.
                        producer.fail(
                            error{failure_class::fatal,
                                  "recording_chat_client: inner stream ended in an unexpected open terminal",
                                  "recording_chat_client.unexpected_open_terminal"});
                        break;
                }
            })
            .detach();

        return std::move(pair.consumer);
    }

private:
    Inner inner_;
    RecordingSink sink_;
};

static_assert(true, "RecordingChatClient<Inner>'s ChatClient-concept conformance is asserted per "
                     "instantiation by its own tests (a template can't be static_assert-checked "
                     "against a concept without a concrete Inner) -- see tests/"
                     "test_recording_chat_client.cpp's own static_assert against a concrete conformer.");

}  // namespace agentengine
