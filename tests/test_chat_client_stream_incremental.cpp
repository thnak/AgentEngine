// Proves decisions/ADR-019-incremental-streaming-response.md's gates G1-G4, closing Milestone 5's
// last self-contained residual: "threading `task<T>`/a live producer into
// `sandbox/provider_http_client.hpp`'s SSE read loop and a real Phase D/E backend's own
// `chat()`/`chat_stream()` bodies."
//
// Before ADR-019, `chat_stream()` performed one COMPLETE blocking fetch and only then replayed the
// already-received events onto the ring. The vendor's chunk boundaries were preserved in delivery
// ORDER (004 §7 G3) but not in TIME: a consumer saw nothing at all until the entire completion had
// arrived. That is not a subtle difference for a real completion -- it is the difference between a
// token appearing as the model produces it and the whole answer appearing at once.
//
// TIME-TO-FIRST-ITEM IS THE ONLY HONEST MEASUREMENT HERE, so it is what this file asserts. A server
// that drips its SSE events over a known interval makes the property falsifiable in the one currency
// that distinguishes the two designs:
//
//   - incremental: the first update arrives after roughly ONE drip interval.
//   - buffered:    the first update arrives only after the LAST one is sent.
//
// The gap between those is the whole point, so the assertions are stated as a comparison between
// first-item latency and total-stream latency rather than as an absolute millisecond bound -- an
// absolute bound would be measuring the test machine, not the design.
//
// Deterministic and offline (a canned drip server on loopback, plaintext via ADR-016 so no
// certificate plumbing distracts from what is being proven), so this belongs in the default suite.

#ifdef AGENTENGINE_WITH_HTTPS

#include "pal/net.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/sandbox/incremental_http_body.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;
using agentengine::sandbox::ProviderTransport;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

constexpr std::uint32_t kLoopbackHostOrder = (127u << 24) | 1u;
constexpr int kChunkCount = 6;
constexpr auto kInterChunkDelay = std::chrono::milliseconds(120);  // ~600ms of pacing after the first

// A plain-HTTP server that sends chunked SSE events one at a time, pausing between them. Whether the
// client is incremental is directly observable as when its first item appears relative to this pacing.
class DripSseServer {
public:
    DripSseServer(std::vector<std::string> events) : events_(std::move(events)) {
        auto listen_r = quark::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_ = *quark::pal::local_port(listen_fd_);
            thread_ = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~DripSseServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        if (ok_) quark::pal::close_fd(listen_fd_);
    }
    DripSseServer(DripSseServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = quark::pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            serve_one(*a, st);
            quark::pal::close_fd(*a);
        }
    }

    bool write_all(quark::pal::fd_t fd, std::string const& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            auto w = quark::pal::send_some(fd, reinterpret_cast<std::byte const*>(data.data() + sent),
                                            data.size() - sent);
            if (!w) {
                if (w.error() == quark::pal::would_block()) continue;
                return false;
            }
            sent += *w;
        }
        return true;
    }

    void serve_one(quark::pal::fd_t fd, std::stop_token const& st) {
        std::string buf;
        std::byte chunk[1024];
        for (int i = 0; i < 400 && !st.stop_requested(); ++i) {
            auto r = quark::pal::recv_some(fd, chunk, sizeof(chunk));
            if (!r) {
                if (r.error() == quark::pal::would_block()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                return;
            }
            if (*r == 0) return;
            buf.append(reinterpret_cast<char const*>(chunk), *r);
            if (buf.find("\r\n\r\n") != std::string::npos) break;
        }
        if (buf.find("\r\n\r\n") == std::string::npos) return;

        if (!write_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                            "Transfer-Encoding: chunked\r\n\r\n")) {
            return;
        }
        for (std::size_t i = 0; i < events_.size() && !st.stop_requested(); ++i) {
            char size_buf[32];
            std::snprintf(size_buf, sizeof(size_buf), "%zx\r\n", events_[i].size());
            if (!write_all(fd, std::string(size_buf) + events_[i] + "\r\n")) return;
            if (i + 1 < events_.size()) std::this_thread::sleep_for(kInterChunkDelay);
        }
        write_all(fd, "0\r\n\r\n");
    }

    std::vector<std::string> events_;
    bool ok_ = false;
    quark::pal::fd_t listen_fd_{};
    std::uint16_t port_ = 0;
    std::jthread thread_;
};

[[nodiscard]] ChatRequest request_asking(std::string text) {
    ChatRequest req;
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(std::move(item));
    req.messages.push_back(std::move(m));
    return req;
}

struct DrainTiming {
    std::chrono::steady_clock::duration to_first{};
    std::chrono::steady_clock::duration total{};
    int text_updates = 0;
    std::string text;
    stream_terminal terminal = stream_terminal::open;
};

// Drains a live stream, recording when the FIRST item landed and when the stream ended.
[[nodiscard]] DrainTiming drain_timed(stream<ChatResponseUpdate>& s) {
    auto const t0 = std::chrono::steady_clock::now();
    DrainTiming out;
    bool first_seen = false;
    while (!s.done()) {
        while (auto update = s.next()) {
            if (!first_seen) {
                out.to_first = std::chrono::steady_clock::now() - t0;
                first_seen = true;
            }
            if (auto const* t = std::get_if<Text>(&update->delta.value)) {
                ++out.text_updates;
                out.text += t->text;
            }
        }
        if (!s.done()) std::this_thread::yield();
    }
    out.total = std::chrono::steady_clock::now() - t0;
    out.terminal = s.terminal();
    return out;
}

[[nodiscard]] long long ms(std::chrono::steady_clock::duration d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
}

}  // namespace

int main() {
#if defined(_WIN32)
    quark::pal::ensure_winsock();
#endif

    InMemorySecretStore store;
    store.set("provider-key", "sk-drip-server-ignores-this");
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"provider-key", std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal = Principal{"incremental-test", ""};
    ctx.capabilities = &held;

    // ---- G1/G2: the OpenAI-compatible backend streams incrementally --------------------------------
    {
        std::vector<std::string> events;
        for (int i = 0; i < kChunkCount; ++i) {
            events.push_back(R"(data: {"choices":[{"delta":{"content":"x"}}]})"
                             "\n\n");
        }
        events.push_back("data: [DONE]\n\n");

        DripSseServer server(events);
        check(server.ok(), "OpenAI: drip server started");
        if (server.ok()) {
            openai::OpenAIChatClient client("127.0.0.1", server.port(), "m", SecretRef{"provider-key"},
                                             ChatClientCapabilities{}, store, "/v1", sandbox::resolve_host,
                                             /*ca=*/std::string{}, /*referer=*/std::string{},
                                             /*x_title=*/std::string{}, /*end_user=*/std::string{},
                                             /*seed=*/std::nullopt, ProviderTransport::plaintext_http);
            stream<ChatResponseUpdate> s = client.chat_stream(request_asking("hi"), ctx);
            DrainTiming const t = drain_timed(s);

            check(t.terminal == stream_terminal::closed,
                  "G1 (OpenAI): the incremental stream reaches the success terminal");
            check(t.text_updates == kChunkCount && t.text == std::string(kChunkCount, 'x'),
                  "G1 (OpenAI): every text delta is delivered exactly once, with no loss and no "
                  "duplication -- incremental decoding did not drop an event straddling a read boundary");

            // The load-bearing comparison. A buffered implementation cannot produce a first item
            // before the server's last write; an incremental one produces it after ~one interval.
            auto const pacing = kInterChunkDelay * (kChunkCount - 1);
            check(t.to_first < t.total / 2,
                  "G2 (OpenAI): the FIRST update arrived in well under half the stream's total "
                  "duration. Before ADR-019 the first item could not appear until the whole body had "
                  "been read, so this ratio was ~1.0 by construction -- it is the measurement that "
                  "distinguishes incremental delivery from replay-after-buffering");
            check(t.to_first < pacing,
                  "G2 (OpenAI): the first update also beat the server's own total pacing outright -- "
                  "the consumer was reading while the server was still writing");
            std::fprintf(stderr, "  .. OpenAI: first item %lld ms | total %lld ms | server pacing %lld ms\n",
                         ms(t.to_first), ms(t.total), static_cast<long long>(pacing.count()));
        }
    }

    // ---- G3: the Anthropic backend streams incrementally too ----------------------------------------
    // A structurally different wire shape (named events, a content_block state machine), so it is a
    // genuinely separate decoder and gets its own measurement rather than an assumption.
    {
        std::vector<std::string> events;
        events.push_back("event: content_block_start\ndata: {\"index\":0,\"content_block\":{\"type\":\"text\"}}\n\n");
        for (int i = 0; i < kChunkCount; ++i) {
            events.push_back("event: content_block_delta\ndata: {\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"y\"}}\n\n");
        }
        events.push_back("event: message_stop\ndata: {}\n\n");

        DripSseServer server(events);
        check(server.ok(), "Anthropic: drip server started");
        if (server.ok()) {
            anthropic::AnthropicChatClient client("127.0.0.1", server.port(), "m", SecretRef{"provider-key"},
                                                   ChatClientCapabilities{}, store, "/v1", "2023-06-01",
                                                   sandbox::resolve_host, /*ca=*/std::string{},
                                                   /*referer=*/std::string{}, /*x_title=*/std::string{},
                                                   /*end_user=*/std::string{}, /*cache_ttl=*/std::string{},
                                                   ProviderTransport::plaintext_http);
            stream<ChatResponseUpdate> s = client.chat_stream(request_asking("hi"), ctx);
            DrainTiming const t = drain_timed(s);

            check(t.terminal == stream_terminal::closed,
                  "G3 (Anthropic): the incremental stream reaches the success terminal");
            check(t.text_updates == kChunkCount && t.text == std::string(kChunkCount, 'y'),
                  "G3 (Anthropic): every text_delta is delivered exactly once through the named-event "
                  "decoder");
            check(t.to_first < t.total / 2,
                  "G3 (Anthropic): the first update arrived in well under half the total duration -- "
                  "the named-event decoder is incremental too, proven separately rather than assumed "
                  "from the OpenAI result");
            std::fprintf(stderr, "  .. Anthropic: first item %lld ms | total %lld ms\n", ms(t.to_first),
                         ms(t.total));
        }
    }

    // ---- G4: the framing decoders survive an adversarial byte-by-byte feed ---------------------------
    // The incremental decoders' whole risk is state across call boundaries: a chunk-size line split
    // mid-digit, an SSE terminator split between two reads. Feeding one byte at a time is the
    // strongest available form of that test, and it needs no server at all.
    {
        std::string const payload = "data: {\"a\":1}\n\ndata: {\"b\":2}\n\ndata: [DONE]\n\n";
        std::string framed;
        {
            char size_buf[32];
            std::snprintf(size_buf, sizeof(size_buf), "%zx\r\n", payload.size());
            framed = std::string(size_buf) + payload + "\r\n0\r\n\r\n";
        }

        sandbox::ChunkedBodyDecoder decoder;
        sandbox::SseEventFramer framer;
        std::vector<std::string> blocks;
        bool decode_ok = true;
        for (char c : framed) {
            auto d = decoder.feed(std::string_view(&c, 1));
            if (!d) {
                decode_ok = false;
                break;
            }
            for (auto& b : framer.feed(*d)) blocks.push_back(std::move(b));
        }
        check(decode_ok, "G4: byte-by-byte chunked decoding never reports a malformed body");
        check(decoder.complete(), "G4: the terminal 0-size chunk is recognised even split byte-by-byte");
        check(blocks.size() == 3,
              "G4: all three SSE event blocks are recovered from a one-byte-at-a-time feed -- neither "
              "the chunk-size line nor an event terminator was lost across a call boundary");
        if (blocks.size() == 3) {
            check(blocks[0] == "data: {\"a\":1}" && blocks[1] == "data: {\"b\":2}" &&
                      blocks[2] == "data: [DONE]",
                  "G4: each block's content is exact");
        }

        // A malformed chunk size must be a real error, never a silently-empty success: a decoder that
        // returned "" for garbage would present a corrupt stream as an empty but valid one.
        sandbox::ChunkedBodyDecoder bad;
        auto r = bad.feed("zz\r\nnope\r\n");
        check(!r.has_value() && r.error().code == "net.chunked_malformed",
              "G4 (negative control): a malformed chunk size is reported as an error, not swallowed "
              "into an empty-but-successful decode");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_chat_client_stream_incremental: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_chat_client_stream_incremental: %d FAILURE(S)\n", g_failures);
    return 1;
}

#else   // AGENTENGINE_WITH_HTTPS
int main() { return 0; }
#endif  // AGENTENGINE_WITH_HTTPS
