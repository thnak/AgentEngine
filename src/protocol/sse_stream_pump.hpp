#pragma once
// Implements 004-Model-Provider-Plane.md §3 (chat_stream) -- ADR-206 (#120 S6), ADR-019 (decode as the bytes
// arrive), ADR-017 (the stop token). The streaming exchange shared by the OpenAI and Anthropic ChatClient
// backends' detached stream workers (src/protocol/{openai,anthropic}/chat_client.cpp), which had carried two
// identical copies of it. Each worker builds its own vendor request and then hands it to pump_sse_stream(); the
// vendor's wire format stays in its StreamingUpdateAccumulator, the template parameter here.
//
// Private to src/protocol: not installed, not included by any header.

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/protocol/provider_chat_wire.hpp"
#include "agentengine/sandbox/provider_http_client.hpp"

namespace agentengine::detail::provider_wire {

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

}  // namespace agentengine::detail::provider_wire
