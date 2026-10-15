#pragma once
// Implements rfcs/004-Model-Provider-Plane.md §3 (chat_stream) -- ADR-206 (#120 S6), ADR-019 (decode as the bytes
// arrive), ADR-017 (the stop token). The streaming exchange shared by the OpenAI and Anthropic ChatClient
// backends' detached stream workers (src/protocol/{openai,anthropic}/chat_client.cpp), which had carried two
// identical copies of it. Each worker builds its own vendor request and then hands it to pump_sse_stream(); the
// vendor's wire format stays in its StreamingUpdateAccumulator, the template parameter here.
//
// Private to src/protocol: not installed, not included by any header.

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/protocol/provider_chat_wire.hpp"
#include "agentengine/rt/await_chain.hpp"
#include "agentengine/rt/http.hpp"
#include "agentengine/rt/http_tls.hpp"
#include "agentengine/rt/offload.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/sandbox/io_timeout.hpp"
#include "agentengine/sandbox/provider_http_client.hpp"

namespace agentengine::detail::provider_wire {

namespace sse_pump_detail {
[[nodiscard]] inline rt::AddressPolicy accept_all() {
    return [](pal::IpAddress const&) { return true; };
}
}  // namespace sse_pump_detail

// ADR-237 §5.1 (step 34): builds the `rt::HttpContext` for one provider exchange from the client's
// `ProviderReactorOptions` -- the reactor + DNS pool from the Runtime, the transport's CA seam, ADR-016's
// accept-all default policy, and the blocking path's own idle bound (`sandbox::io_timeout_ms()`, which rt/ is L0
// and may not read itself). Lives here (src/, HTTPS-only) rather than in the shared header so the TLS connector
// does not leak into every consumer of the provider client headers.
[[nodiscard]] inline rt::HttpContext make_http_context(ProviderReactorOptions const& o,
                                                       std::string const& ca_bundle_pem_override, bool https) {
    rt::HttpContext ctx;
    ctx.reactor  = o.runtime != nullptr ? &o.runtime->reactor() : nullptr;
    ctx.dns_pool = o.runtime != nullptr ? &o.runtime->dns_pool() : nullptr;
    ctx.policy   = o.policy ? o.policy : sse_pump_detail::accept_all();
    ctx.options  = o.options;
    ctx.options.idle_timeout     = std::chrono::milliseconds{sandbox::io_timeout_ms()};
    ctx.options.connect.resolver = o.resolver;
    if (https) ctx.tls = rt::tls_connector(rt::TlsConnectorOptions{.ca_bundle_pem_override = ca_bundle_pem_override});
    return ctx;
}

// `Accumulator` is constructed as `Accumulator(bool chunked, std::string producer_chat_client_id)` and provides
// `feed(std::string_view) -> result<std::vector<ChatResponseUpdate>>`, `truncated() -> bool` and
// `finish() -> std::vector<ChatResponseUpdate>`. `vendor` is the provider name: it prefixes the producer id
// ("<vendor>:<model>", the same string the client's producer_chat_client_id() returns, gap-audit finding 20 /
// 003 §8 Q2) and the http_status_error message. Every outcome ends the producer: close() after a complete
// stream, fail() otherwise, or nothing when the consumer has gone.
template <class Accumulator>
void pump_sse_stream(std::string_view vendor, std::string const& model, std::string const& host,
                     std::uint16_t port, sandbox::NetEgressRequest const& req,
                     stream_producer<ChatResponseUpdate>& producer, Resolver const& resolver,
                     std::string const& ca_bundle_pem_override, sandbox::ProviderTransport transport,
                     std::stop_token stop) {
    // ADR-019: decode and push AS THE BYTES ARRIVE. `chunked` is not known until the response head is
    // parsed, which happens before the first `on_body` call -- but the sink cannot see the head, so it
    // is inferred from the first fragment instead: a chunked body always begins with a hex chunk-size
    // line, an unchunked SSE body always begins with `data:`/`event:`/a colon comment. Cheap, and only
    // consulted once.
    std::optional<Accumulator> acc;
    bool push_failed = false;
    std::optional<error> decode_error;

    auto on_body = [&](std::string_view fragment) -> bool {
        if (!acc) {
            bool const looks_like_sse = fragment.starts_with("data:") || fragment.starts_with("event:") ||
                                        fragment.starts_with(":");
            acc.emplace(!looks_like_sse, std::string(vendor) + ":" + model);
        }
        auto updates = acc->feed(fragment);
        if (!updates) {
            decode_error = updates.error();
            return false;
        }
        for (auto& update : *updates) {
            if (producer.push(std::move(update)) != stream_push::ok) {
                push_failed = true;  // consumer cancelled/deadlined -- the ring already latched why
                return false;
            }
        }
        return true;
    };

    auto resp = sandbox::perform_provider_streaming_exchange(host, port, req, on_body, stop, std::nullopt,
                                                              resolver, ca_bundle_pem_override, transport);
    if (push_failed) return;  // nothing left to say; the consumer is gone
    if (!resp) {
        // Forward the network layer's own real, already-correctly-classified error (transient for a
        // connect/timeout failure, resource for a byte-cap trip, policy for a denied grant -- see
        // sandbox/net_egress_proxy.cpp) instead of a blanket synthetic "unavailable", which would have
        // retried a byte-cap/policy failure that should never be retried.
        producer.fail(resp.error());
        return;
    }
    if (resp->status < 200 || resp->status >= 300) {
        // A non-2xx body is an error document, not SSE, so it produced no events and nothing bogus was
        // pushed above -- the stream simply fails here instead, with the SAME transient/policy/contract
        // split chat()'s non-streaming path uses. An empty body: the streaming path never captured the
        // raw error document as a separate buffer, so the message falls back to "<vendor> http status N".
        producer.fail(http_status_error(vendor, resp->status, {}));
        return;
    }
    if (decode_error) {
        producer.fail(*decode_error);
        return;
    }
    if (!acc) {
        // A 2xx response whose body never delivered a single byte is not a finished chat stream: the
        // accumulator is created by the first fragment, so without one `truncated()` cannot even be
        // asked. Head-then-cut used to fall through to a clean close and read as "no usage" (contract).
        producer.fail(error{failure_class::transient,
                            "the response ended before its body began: the connection was cut right "
                            "after the response head",
                            "net.stream_truncated"});
        return;
    }
    if (acc->truncated()) {
        producer.fail(error{failure_class::transient,
                            "the response stream ended before its final chunk: the connection was cut "
                            "while the model was still answering",
                            "net.stream_truncated"});
        return;
    }
    for (auto& update : acc->finish()) {
        if (producer.push(std::move(update)) != stream_push::ok) return;
    }
    producer.close();
}

namespace sse_pump_detail {

using SharedProducer = std::shared_ptr<stream_producer<ChatResponseUpdate>>;

// ADR-237 §4.5 rule 5: a coroutine on a lane never parks its thread. Enqueue without blocking; when the consumer
// is slow and the channel is full, the blocking `push` runs on the OFFLOAD pool (the one place a parked thread is
// allowed) while the pump suspends. The job holds its own `shared_ptr` to the producer, so it stays valid even if
// the pump resumes `canceled` and finishes first; the stop callback in `pump_sse_stream_async` fails the channel,
// which is what wakes a push parked on a full queue.
[[nodiscard]] inline task<bool> push_update(SharedProducer prod, ChatResponseUpdate update, rt::OffloadPool* pool,
                                            std::stop_token stop) {
    if (auto const r = prod->try_push(update)) co_return *r == stream_push::ok;
    if (pool == nullptr) co_return prod->push(std::move(update)) == stream_push::ok;  // no pool: caller's thread
    auto res = co_await rt::offload(*pool, std::move(stop), [prod, u = std::move(update)]() mutable {
        return prod->push(std::move(u)) == stream_push::ok;
    });
    co_return res.completed() && res.value.value_or(false);
}

template <class Accumulator>
task<void> pump_body(std::string const& vendor, std::string const& model, rt::HttpContext const& ctx,
                     rt::HttpRequest const& req, SharedProducer const& producer, rt::OffloadPool* pool,
                     std::stop_token stop) {
    rt::HttpStreamResponse head = co_await rt::http_stream(ctx, req, stop);
    if (!head.ok()) {
        producer->fail(http_error_to_error(head.error, head.code, head.message));
        co_return;
    }
    if (head.status < 200 || head.status >= 300) {
        // Same transient/policy/contract split as the non-streaming path; the streaming path never buffered the
        // error document, so the message falls back to "<vendor> http status N".
        producer->fail(http_status_error(vendor, head.status, {}));
        co_return;
    }

    Accumulator acc(/*chunked=*/false, std::string(vendor) + ":" + model);
    rt::SseStream sse = rt::sse_events(std::move(head.body), ctx.options.max_event_bytes);
    bool any_event    = false;
    for (;;) {
        rt::SseNext n = co_await sse.next(stop);
        // A body cut before its final chunk is a framing fault, not a short answer -- unless the stream already
        // delivered its own terminal event, in which case it is COMPLETE (the blocking pump's `truncated()`
        // deferral): failing it would discard a finished, billed response and retry it, paying twice.
        bool const cut_after_terminal =
            acc.terminal_seen() && ((n.status == rt::sse_status::error && n.error == rt::http_error::truncated) ||
                                    (n.status == rt::sse_status::end && n.incomplete_dropped));
        if (cut_after_terminal) {
            for (auto& update : acc.finish()) {
                if (!co_await push_update(producer, std::move(update), pool, stop)) co_return;
            }
            producer->close();
            co_return;
        }
        if (n.status == rt::sse_status::error) {
            if (n.error == rt::http_error::canceled && stop.stop_requested()) {
                producer->fail(error{failure_class::canceled, "canceled by the caller", "net.cancelled"});
            } else if (n.error == rt::http_error::idle_timeout || n.error == rt::http_error::transport) {
                // ADR-177: a read that failed AFTER the response head is a response that stopped mid-way (a silent
                // provider at the idle timeout, a reset), not a failure to reach the server. The session's one
                // retry decision keys on this code (`should_retry_stream`), as the blocking transport re-coded it.
                auto e = http_error_to_error(n.error, n.code, n.message);
                producer->fail(error{e.klass, "the response stopped while it was being read: " + e.message,
                                     "net.stream_read_failed"});
            } else {
                producer->fail(http_error_to_error(n.error, n.code, n.message));
            }
            co_return;
        }
        if (n.status == rt::sse_status::end) {
            if (n.incomplete_dropped) {
                producer->fail(error{failure_class::transient,
                                     "the response stream ended inside an event: the connection was cut while the "
                                     "model was still answering",
                                     "net.stream_truncated"});
                co_return;
            }
            if (!any_event) {
                // A 2xx whose body delivered not one event: head-then-cut (the blocking pump's `!acc` case).
                producer->fail(error{failure_class::transient,
                                     "the response ended before its body began: the connection was cut right after "
                                     "the response head",
                                     "net.stream_truncated"});
                co_return;
            }
            for (auto& update : acc.finish()) {
                if (!co_await push_update(producer, std::move(update), pool, stop)) co_return;
            }
            producer->close();
            co_return;
        }
        any_event = true;
        // Re-serialize the decoded event into the vendor accumulator's own entry format. The `event:` NAME is
        // preserved when the wire carried one (Anthropic's named-event framing, `split_sse_named_events`); an
        // OpenAI-compatible stream sends none, so it is omitted there. SseDecoder joins a multi-line `data:` with
        // '\n', but the vendor accumulators take each `data:` line as a separate event: so the payload goes out as
        // ONE line, its joining newlines as spaces (JSON whitespace -- the payloads are JSON, where a raw newline
        // cannot occur inside a string anyway), which keeps a pretty-printing server's event whole.
        std::string block;
        if (!n.event.event.empty()) {
            block += "event: ";
            block += n.event.event;
            block += "\n";
        }
        block += "data: ";
        for (char const c : n.event.data) block += (c == '\n' ? ' ' : c);
        block += "\n\n";
        auto updates = acc.feed(block);
        if (!updates) {
            producer->fail(updates.error());
            co_return;
        }
        for (auto& update : *updates) {
            if (!co_await push_update(producer, std::move(update), pool, stop)) co_return;
        }
    }
}

}  // namespace sse_pump_detail

// ADR-237 §5.1/§6.3 (step 34): `pump_sse_stream`'s reactor twin -- a COROUTINE that suspends on `rt::http_stream`
// / `rt::sse_events` instead of holding a thread. Started by the client through `rt::Runtime::spawn` (the client
// owns the root, §4.5 rule 2), so no per-call `std::thread` is detached. The six terminal rules of the blocking
// pump are preserved: a transport failure is forwarded (mapped through `http_error_to_error`, keeping the
// transient/policy/contract/resource split), a non-2xx goes through `http_status_error`, a body cut mid-stream is
// `net.stream_truncated`, a 2xx that delivered no event at all is `net.stream_truncated` (the "body never began"
// case), and a complete stream closes the producer.
//
// The accumulator is constructed `chunked=false`: rt's `BodyDecoder` already stripped any chunk framing, so feed()
// receives decoded payload. rt's `SseDecoder` parses events itself, so each decoded event is re-serialized as
// `data:` lines before `feed()` -- the vendor accumulators' own entry point.
//
// Stop: the pump ends on EITHER the consumer's stop (`stop`, ADR-017) or the spawned root's (`rt::scope_stop_token()`,
// which Runtime shutdown requests, §4.5 rule 4) -- merged into one token that every await below observes. On a stop
// the producer is failed `canceled`, which also wakes a `push` parked on a full queue. Any exception ends the stream
// failed rather than closed, so a fault is never read as a clean (truncated) answer.
template <class Accumulator>
task<void> pump_sse_stream_async(std::string vendor, std::string model, rt::HttpContext ctx, rt::HttpRequest req,
                                 stream_producer<ChatResponseUpdate> producer, std::stop_token stop,
                                 rt::OffloadPool* pool = nullptr) {
    auto prod = std::make_shared<stream_producer<ChatResponseUpdate>>(std::move(producer));
    std::stop_source merged;
    std::stop_callback on_consumer(stop, [&merged] { merged.request_stop(); });
    std::stop_callback on_runtime(rt::scope_stop_token(), [&merged] { merged.request_stop(); });
    std::stop_callback on_stop(merged.get_token(), [prod] {
        prod->fail(error{failure_class::canceled, "the stream was stopped", "net.cancelled"});
    });
    try {
        co_await sse_pump_detail::pump_body<Accumulator>(vendor, model, ctx, req, prod, pool, merged.get_token());
    } catch (...) {
        prod->fail(error{failure_class::fatal, "the stream pump failed with an unexpected exception",
                         "net.stream_pump_faulted"});
    }
}

}  // namespace agentengine::detail::provider_wire
