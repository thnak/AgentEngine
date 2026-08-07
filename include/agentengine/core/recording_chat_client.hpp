#pragma once
// Implements 004-Model-Provider-Plane.md §6 ("Recording and replay") -- Milestone 5 Phase G1
// (docs/planning/milestone-5-providers-identity-secrets-breakdown.md, Phase G / decision 8): "promote
// the recording mechanism from RecordedChatClient's test-scoped fixture player to the real thing:
// every real backend (D/E) records request, response or full ordered chunk sequence, timing, and
// usage." `RecordingChatClient<Inner>` is that real mechanism -- a `ChatClient`-conforming wrapper
// around ANY conforming `Inner` (Phase D's `OpenAIChatClient`, Phase E's Anthropic backend, a
// `ResilientChatClient`/`FailoverChatClient` composition, or a test fake), so recording is available
// uniformly regardless of which concrete backend produced the call.
//
// Builds directly on `core/chat_recording.hpp`'s already-proven JSON envelope
// (`ChatCallRecording`/`RecordedChunk`/`recording_mode`) -- this file owns none of that codec, only
// the capture/timing logic that fills it in from a live call.
//
// `ChatClient` is a concept, never a base class (chat_client.hpp's own top comment) -- so, exactly
// like `ResilientChatClient`/`FailoverChatClient`, this is a template wrapper composing over `Inner`,
// never a virtual/type-erased decorator.
//
// SINK, NOT FILE I/O: the constructor takes an injectable `RecordingSink` (`std::function<void
// (ChatCallRecording)>`) -- the same testability-seam pattern as `ResilientChatClient`'s injectable
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
// PRODUCER-SIDE TERMINAL MIRRORING IS ONLY A PARTIAL MIRROR, NAMED HONESTLY: Quark's
// `ReplyStreamProducer<F>` (third_party/quark/include/quark/core/reply_stream.hpp) exposes exactly
// two producer-side terminal setters, `close()` (-> Closed) and `fail(error)` (-> Failed).
// `Cancelled`/`DeadlineExceeded` have NO producer-side setter at all -- `ReplyStream<F>::cancel()`/
// `expire_deadline()` are CONSUMER-side operations on quark::ReplyStream's own file banner. So when
// Inner's stream ends Cancelled or DeadlineExceeded, this wrapper's own outbound producer cannot
// literally reproduce that exact terminal cause through the public API -- it calls `fail()` with a
// translated `quark::error` (`errc::cancelled` / `errc::timeout`) as the closest honest
// approximation, rather than silently mislabeling either as a plain Closed success. The RECORDING
// itself (`ChatCallRecording::stream_terminal`, a plain string) is NOT subject to this restriction
// and always records the real cause exactly ("closed"/"cancelled"/"deadline_exceeded"/"failed").

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

#include "quark/core/error.hpp"
#include "quark/core/reply_stream.hpp"

namespace agentengine {

namespace recording_chat_client_detail {

// The default sink: does nothing. "Nothing meaningful in product code" (task brief) -- there is no
// single "real" default for "what to do with a recording" the way ResilientChatClient's jitter has a
// real PRNG default; a caller who wants recordings persisted supplies a sink that does so (see
// file-top comment). Named/addressable (not an inline lambda default arg) to mirror
// `resilient_chat_client_detail::real_jitter`'s own function-pointer-default idiom.
inline void discard_recording(ChatCallRecording) noexcept {}

// `quark::ReplyStreamTerminal` (Closed/Cancelled/DeadlineExceeded/Failed) -> the wire string
// `ChatCallRecording::stream_terminal` expects (chat_recording.hpp's own field comment names exactly
// these four strings). Only meaningful once a stream has actually reached a terminal (i.e. `done()`
// is true) -- `Open` has no wire representation and is unreachable from that call site.
[[nodiscard]] inline std::string_view stream_terminal_to_wire_string(
    quark::ReplyStreamTerminal terminal) noexcept {
    switch (terminal) {
        case quark::ReplyStreamTerminal::Closed: return "closed";
        case quark::ReplyStreamTerminal::Cancelled: return "cancelled";
        case quark::ReplyStreamTerminal::DeadlineExceeded: return "deadline_exceeded";
        case quark::ReplyStreamTerminal::Failed: return "failed";
        case quark::ReplyStreamTerminal::Open: return "";
    }
    return "";
}

}  // namespace recording_chat_client_detail

// `RecordingChatClient<Inner>` itself conforms to the `ChatClient` concept (chat_client.hpp) --
// `capabilities()` forwards to `Inner`; `chat()`/`chat_stream()` wrap `Inner`'s own calls with
// capture-and-emit recording that never changes what the caller observes (see file-top comment).
template <class Inner>
class RecordingChatClient {
    static_assert(ChatClient<Inner>,
                  "RecordingChatClient's Inner must satisfy the ChatClient concept (004 §1)");

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
                            if (producer.push(std::move(*update)) != quark::ReplyPush::Ok) {
                                downstream_alive = false;
                            }
                        }
                    }
                    if (!inner_stream.done()) std::this_thread::yield();
                }

                quark::ReplyStreamTerminal const terminal = inner_stream.terminal();
                std::string const terminal_wire =
                    std::string(recording_chat_client_detail::stream_terminal_to_wire_string(terminal));

                ChatCallRecording rec;
                rec.request = std::move(request);
                rec.mode = recording_mode::streaming;
                rec.chunks = std::move(chunks);
                rec.stream_terminal = terminal_wire;
                if (terminal == quark::ReplyStreamTerminal::Failed) {
                    rec.stream_error_detail = std::string(inner_stream.fail_error().detail);
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

                // Mirror Inner's terminal onto the new producer -- exactly (Closed/Failed) where the
                // public API allows it, translated where it does not (Cancelled/DeadlineExceeded --
                // see file-top comment "PRODUCER-SIDE TERMINAL MIRRORING IS ONLY A PARTIAL MIRROR").
                switch (terminal) {
                    case quark::ReplyStreamTerminal::Closed:
                        producer.close();
                        break;
                    case quark::ReplyStreamTerminal::Failed:
                        producer.fail(inner_stream.fail_error());
                        break;
                    case quark::ReplyStreamTerminal::Cancelled:
                        producer.fail(quark::error{quark::errc::cancelled,
                                                    "recording_chat_client.inner_stream_cancelled"});
                        break;
                    case quark::ReplyStreamTerminal::DeadlineExceeded:
                        producer.fail(
                            quark::error{quark::errc::timeout,
                                         "recording_chat_client.inner_stream_deadline_exceeded"});
                        break;
                    case quark::ReplyStreamTerminal::Open:
                    default:
                        // Unreachable: inner_stream.done() == true (the outer while's own exit
                        // condition) guarantees terminal() != Open here.
                        producer.fail(
                            quark::error{quark::errc::internal,
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
