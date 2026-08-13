#pragma once
// Implements 004-Model-Provider-Plane.md §6 ("Recording and replay") -- Milestone 5 Phase G2:
// `ReplayChatClient`, the real, product-code `ChatClient` conformer that PLAYS BACK a
// `ChatCallRecording` (core/chat_recording.hpp, Phase G1's shared JSON codec) with identical chunk
// boundaries and inter-chunk timing (004 §7 G3's own gate: "streaming-dependent behavior... reproduces
// exactly"). This SUPERSEDES `tests/support/recorded_chat_client.hpp`'s `RecordedChatClient` for
// product code (decision 8, docs/planning/milestone-5-providers-identity-secrets-breakdown.md lines
// 513-521): that type stays test-only scaffolding (hand-authored, 3-content-kind JSON fixtures, no
// streaming support at all, and one other test file still depends on it -- left untouched) -- this is
// the real thing, built against the real recording format, with real streaming chunk-sequence replay.
//
// Same simplicity discipline `RecordedChatClient` established (its own top comment): ONE instance
// always serves ONE `ChatCallRecording` -- no request-based fixture/recording selection. Constructed
// once per scenario, exactly like `RecordedChatClient(fixture_path, caps)`.
//
// `capabilities()` returns the constructor-supplied `caps` verbatim -- declared, not inferred from the
// recording (matches `RecordedChatClient`'s own precedent).
//
// `chat_stream()`'s detached-background-thread-pushes-into-a-`stream_producer` shape mirrors
// `protocol/openai/chat_client.hpp`'s own `chat_stream()` (that file's own top comment: `chat_stream()`
// must return the drain handle to the caller synchronously and cannot keep producing after it
// returns), and now genuinely matches that file's own "every parameter is owned by value... touches no
// state owned by *this" discipline WITHOUT EXCEPTION -- a prior ADR-037 pass's own residual is closed
// here. `quark::error::detail` was a non-owning `std::string_view` (`errors never own heap on the
// failure path`), which forced the old worker to borrow a recorded `stream_error_detail` string
// straight out of `recording_` (a member of `*this`) rather than copy it, since nowhere static existed
// to own dynamic, recorded text -- the file's own named consequence: "a `ReplayChatClient` must
// outlive any `stream<ChatResponseUpdate>` whose `fail_error().detail` a caller intends to read."
// `agentengine::error::message` is an OWNED `std::string` (core/error.hpp), so that constraint is
// simply gone: `chat_stream()` copies `recording_.stream_error_detail` into an ordinary by-value
// argument before spawning the thread (the same way `recording_.chunks` already was), and the worker
// owns its own copy from that point on -- no borrowed state, no outlive requirement, matching the
// OpenAI backend's own discipline exactly rather than a documented, accepted exception to it.
//
// Inter-chunk timing (004 §6/§7 G3: "UI cadence reproduces exactly") is reproduced by sleeping, before
// each push, for the DELTA between this chunk's and the previous chunk's `elapsed_since_start` (0 for
// the first chunk) -- an injectable `SleepFn` (default real `std::this_thread::sleep_for`) is the exact
// testability-seam SHAPE `ModelCallGateway::JitterSource` already established (core/model_call_gateway.hpp:
// a real, callable default, overridable in tests for determinism).

#include <chrono>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>
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

namespace replay_chat_client_detail {

// The real sleep function -- default `ReplayChatClient::SleepFn`. Mirrors
// `resilient_chat_client_detail::real_jitter`'s own placement: the real implementation lives right next
// to the injectable seam it defaults, not buried in the class body.
inline void real_sleep(std::chrono::milliseconds d) { std::this_thread::sleep_for(d); }

// Translates a recorded `stream_terminal` ("cancelled" | "deadline_exceeded" | "failed" | anything
// else/unset) into an `agentengine::error` -- "cancelled" -> `failure_class::fatal` (a caller-
// initiated teardown that ends the run, matching `classify_drained_failure`'s own now-retired
// "cancelled is never retried" precedent), "deadline_exceeded" -> `failure_class::transient` (a real,
// retryable timeout condition; kept as an INPUT string this function still recognizes even though no
// LIVE stream can produce that terminal anymore -- see file banner and `recording_chat_client.hpp`'s
// own note -- since old, already-persisted recordings may still carry it on disk), "failed"/anything
// unrecognized -> `failure_class::fatal` (this file's own catch-all, named here rather than silently
// mapped onto something the wire vocabulary doesn't actually say). `detail` is taken and stored BY
// VALUE -- `error::message` owns its text, so there is nothing left to borrow (see file banner).
[[nodiscard]] inline error terminal_to_error(std::string_view stream_terminal, std::string detail) noexcept {
    if (stream_terminal == "cancelled") {
        return error{failure_class::fatal, std::move(detail), "replay_chat_client.stream_cancelled"};
    }
    if (stream_terminal == "deadline_exceeded") {
        return error{failure_class::transient, std::move(detail),
                      "replay_chat_client.stream_deadline_exceeded"};
    }
    return error{failure_class::fatal, std::move(detail), "replay_chat_client.stream_failed"};
}

// The detached background worker (mirrors protocol/openai/chat_client.hpp's own `run_stream_worker` --
// see that file's top comment for why detached, not a tracked member). Every parameter is owned by
// value, INCLUDING `error_detail` now (see file banner) -- `chunks`/`stream_terminal`/`error_detail`
// are all independent copies, safe across the thread boundary and leaving the originating
// `ReplayChatClient`'s own `recording_` untouched for a possible second `chat_stream()` call against
// the same instance.
inline void run_replay_worker(std::vector<RecordedChunk> chunks, std::string stream_terminal,
                               std::string error_detail, stream_producer<ChatResponseUpdate> producer,
                               std::function<void(std::chrono::milliseconds)> sleep_fn) {
    std::chrono::milliseconds previous{0};
    for (auto& chunk : chunks) {
        std::chrono::milliseconds delta = chunk.elapsed_since_start - previous;
        // A malformed/out-of-order recording (a later chunk's elapsed_since_start earlier than a prior
        // one's) must never turn into a negative sleep duration -- clamp to 0 rather than let
        // std::this_thread::sleep_for(negative) do whatever an implementation happens to do with it.
        if (delta < std::chrono::milliseconds{0}) delta = std::chrono::milliseconds{0};
        if (sleep_fn) sleep_fn(delta);
        previous = chunk.elapsed_since_start;
        if (producer.push(std::move(chunk.update)) != stream_push::ok) {
            return;  // consumer cancelled/deadlined -- stop producing, mirrors every real backend here
        }
    }
    if (stream_terminal == "closed") {
        producer.close();
    } else {
        producer.fail(terminal_to_error(stream_terminal, std::move(error_detail)));
    }
}

}  // namespace replay_chat_client_detail

// The real, product-code `ChatClient` conformer that plays back ONE `ChatCallRecording` (see file
// banner). Never a template -- there is exactly one shape of "replay this recording," unlike
// `ModelCallGateway<Primary, Fallback...>` (core/model_call_gateway.hpp), which wraps an arbitrary
// set of backends.
class ReplayChatClient {
public:
    // Injectable inter-chunk sleep source -- default real (`replay_chat_client_detail::real_sleep`
    // above), overridable in tests for determinism. Same seam shape as `ModelCallGateway::
    // JitterSource` (core/model_call_gateway.hpp): production code never passes a non-default sleep
    // function, a test does, so it can assert exact chunk-timing deltas without real wall-clock sleeps.
    using SleepFn = std::function<void(std::chrono::milliseconds)>;

    explicit ReplayChatClient(ChatCallRecording recording, ChatClientCapabilities caps = {},
                               SleepFn sleep_fn = &replay_chat_client_detail::real_sleep)
        : recording_(std::move(recording)), capabilities_(caps), sleep_fn_(std::move(sleep_fn)) {}

    // Declared, not inferred from the recording (matches `RecordedChatClient`'s own precedent).
    [[nodiscard]] ChatClientCapabilities capabilities() const { return capabilities_; }

    // Only meaningful when the stored recording's `mode == recording_mode::unary`; a streaming-mode
    // recording fails closed with `failure_class::contract` rather than silently coercing/fabricating a
    // response -- a caller asking for a unary call against a stream-shaped recording is a contract
    // mismatch (task instruction), not something this player quietly papers over.
    [[nodiscard]] task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) const {
        if (recording_.mode == recording_mode::streaming) {
            co_return std::unexpected(error{
                failure_class::contract,
                "ReplayChatClient::chat() called against a STREAMING-mode recording -- use "
                "chat_stream() instead; a unary call against a stream-shaped recording is a contract "
                "mismatch, never silently coerced",
                "replay_chat_client.mode_mismatch_streaming"});
        }
        if (recording_.response) co_return *recording_.response;
        if (recording_.chat_error) co_return std::unexpected(*recording_.chat_error);
        // Neither response nor chat_error set on a unary recording: malformed. failure_class::fatal,
        // never a fabricated empty success -- mirrors RecordedChatClient::chat()'s own "a missing or
        // malformed fixture is failure_class::fatal" precedent verbatim.
        co_return std::unexpected(error{failure_class::fatal,
                                         "malformed unary ChatCallRecording: neither response nor "
                                         "chat_error is set",
                                         "replay_chat_client.malformed_recording"});
    }

    // Only meaningful when the stored recording's `mode == recording_mode::streaming`; a unary-mode
    // recording returns an already-failed stream that never pushes an item -- contract mismatch, never
    // silently coerced (task instruction: "it IS a contract mismatch, not merely an unimplemented
    // feature"). `request`/`ctx` are unused: this player always serves the ONE constructor-supplied
    // recording, never the live caller's own request -- matches `RecordedChatClient`'s own "ignores the
    // live caller's own request" precedent (chat_recording.hpp's own `chat_request_to_json` neighbour
    // comment makes the same call for the recorder side).
    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) const {
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());

        if (recording_.mode != recording_mode::streaming) {
            // Contract mismatch: push nothing, fail immediately (task instruction: "it should carry a
            // real failure terminal since it IS a contract mismatch, not merely an unimplemented
            // feature" -- unlike RecordedChatClient::chat_stream()'s own plain empty-stream stub).
            pair.producer.fail(error{failure_class::contract,
                                      "ReplayChatClient::chat_stream() called against a UNARY-mode "
                                      "recording -- a streaming call against a unary-shaped recording "
                                      "is a contract mismatch, never silently coerced",
                                      "replay_chat_client.mode_mismatch_unary"});
            return std::move(pair.consumer);
        }

        // See file banner: `recording_.stream_error_detail` is copied BY VALUE here, before the thread
        // is spawned -- the worker thread owns its own independent copy, so `*this` no longer needs to
        // outlive it.
        std::thread(&replay_chat_client_detail::run_replay_worker, recording_.chunks,
                    recording_.stream_terminal, recording_.stream_error_detail, std::move(pair.producer),
                    sleep_fn_)
            .detach();
        return std::move(pair.consumer);
    }

private:
    ChatCallRecording recording_;
    ChatClientCapabilities capabilities_;
    SleepFn sleep_fn_;
};

static_assert(ChatClient<ReplayChatClient>,
              "ReplayChatClient must satisfy the real ChatClient concept (004 §1) -- checked directly "
              "here, not deferred to per-instantiation tests, since this is a concrete (non-template) "
              "type, unlike ModelCallGateway<Primary, Fallback...>.");

}  // namespace agentengine
