// ADR-237 §4.5 (rules 3a, 4, 5) on the REAL OpenAI client's reactor stream pump, over a loopback socket.
//
//  P1 back-pressure is lossless and does not park a lane: a slow consumer that lets the 256-slot channel fill
//     still receives every update, in order -- the pump hands the blocking push to the offload pool instead of
//     parking its own lane thread (rule 5).
//  P2 shutdown is bounded: destroying the client while a stream is in flight and UNDRAINED returns promptly, and
//     the stream ends failed `net.cancelled`. Before this the pump only watched the consumer's stop token, so
//     the client's destructor (Runtime shutdown) waited for the whole response -- and deadlocked outright once
//     the channel was full, because the thread that would drain it was the one destroying the client (rule 4).
//  P3 a multi-line SSE `data:` field reaches the accumulator whole (SseDecoder joins lines with '\n'; the pump
//     must re-emit one `data:` prefix per line).
//  P4 the `http_error` -> failure_class mapping keeps a DNS failure transient; P6 proves it end to end: a host
//     that does not resolve fails chat() and chat_stream() `transient` (retried), as the blocking path did.
//  P5 `EffectContext::cancellation` stops an in-flight chat(): a server that sends headers and then nothing
//     holds the exchange open (checked: still pending before the stop), and a stop ends it `canceled` promptly.
//  P7 a fault inside the pump ends the stream FAILED `net.stream_pump_faulted` (fatal), never closed: the pump is
//     driven directly with an accumulator that throws on its first event (control: the real accumulator closes
//     cleanly on the same server). Mutant: drop the pump's catch -> the producer is abandoned and the consumer is
//     never told why.

#ifdef AGENTENGINE_WITH_HTTPS

#include "agentengine/pal/net.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <stop_token>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/protocol/provider_chat_wire.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/runtime.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

#include "protocol/sse_stream_pump.hpp"  // src/: the pump template itself, for P7's fault injection

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
constexpr int           kBurst             = 600;  // > the channel's 256 slots

// One connection: send `events` back to back as chunks, then either finish the body or hold the socket open until
// the client hangs up.
class BurstServer {
public:
    BurstServer(std::vector<std::string> events, bool hold_open)
        : events_(std::move(events)), hold_open_(hold_open) {
        auto listen_r = agentengine::pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        ok_           = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_      = *agentengine::pal::local_port(listen_fd_);
            thread_    = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~BurstServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        if (ok_) agentengine::pal::close_fd(listen_fd_);
    }
    BurstServer(BurstServer const&) = delete;

    [[nodiscard]] bool          ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = agentengine::pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            serve_one(*a, st);
            agentengine::pal::close_fd(*a);
        }
    }

    bool write_all(agentengine::pal::fd_t fd, std::string const& data, std::stop_token const& st) {
        std::size_t sent = 0;
        while (sent < data.size() && !st.stop_requested()) {
            auto w = agentengine::pal::send_some(fd, reinterpret_cast<std::byte const*>(data.data() + sent),
                                                 data.size() - sent);
            if (!w) {
                if (w.error() == agentengine::pal::would_block()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                return false;
            }
            sent += *w;
        }
        return sent == data.size();
    }

    void serve_one(agentengine::pal::fd_t fd, std::stop_token const& st) {
        std::string buf;
        std::byte   chunk[1024];
        for (int i = 0; i < 400 && !st.stop_requested(); ++i) {
            auto r = agentengine::pal::recv_some(fd, chunk, sizeof(chunk));
            if (!r) {
                if (r.error() == agentengine::pal::would_block()) {
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
        if (!write_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n",
                       st)) {
            return;
        }
        for (auto const& ev : events_) {
            char size_buf[32];
            std::snprintf(size_buf, sizeof(size_buf), "%zx\r\n", ev.size());
            if (!write_all(fd, std::string(size_buf) + ev + "\r\n", st)) return;
        }
        if (!hold_open_) {
            write_all(fd, "0\r\n\r\n", st);
            return;
        }
        // Silent but not deaf: leave as soon as the client hangs up.
        for (int i = 0; i < 2400 && !st.stop_requested(); ++i) {
            auto r = agentengine::pal::recv_some(fd, chunk, sizeof(chunk));
            if (r && *r == 0) return;
            if (!r && r.error() != agentengine::pal::would_block()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    std::vector<std::string> events_;
    bool                     hold_open_;
    bool                     ok_ = false;
    agentengine::pal::fd_t   listen_fd_{};
    std::uint16_t            port_ = 0;
    std::jthread             thread_;
};

[[nodiscard]] std::string delta_event(std::string const& text) {
    return R"(data: {"choices":[{"delta":{"content":")" + text + R"("}}]})" + "\n\n";
}

[[nodiscard]] ChatRequest request_asking(std::string text) {
    ChatRequest req;
    Message     m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(std::move(item));
    req.messages.push_back(std::move(m));
    return req;
}

using Client = openai::OpenAIChatClient<InMemorySecretStore>;

[[nodiscard]] std::unique_ptr<Client> make_client(BurstServer const& server, InMemorySecretStore& store,
                                                  agentengine::detail::provider_wire::Resolver resolver =
                                                      sandbox::resolve_host,
                                                  std::string host = "127.0.0.1") {
    return std::make_unique<Client>(std::move(host), server.port(), "m", SecretRef{"provider-key"},
                                    ChatClientCapabilities{}, store, "/v1", std::move(resolver), std::string{},
                                    std::string{}, std::string{}, std::string{}, std::nullopt,
                                    ProviderTransport::plaintext_http);
}

// Drains to the terminal, returning every text delta concatenated with a separator-free count.
struct Drained {
    int             updates = 0;
    std::string     text;
    stream_terminal terminal = stream_terminal::open;
    std::string     code;
};

[[nodiscard]] Drained drain(stream<ChatResponseUpdate>& s, std::chrono::seconds limit) {
    Drained    out;
    auto const t0 = std::chrono::steady_clock::now();
    while (!s.done() && std::chrono::steady_clock::now() - t0 < limit) {
        while (auto u = s.next()) {
            if (auto const* t = std::get_if<Text>(&u->delta.value)) {
                ++out.updates;
                out.text += t->text;
            }
        }
        if (!s.done()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    out.terminal = s.terminal();
    if (out.terminal == stream_terminal::failed) out.code = s.fail_error().code;
    return out;
}

// P7: an accumulator that faults on the first event it is fed.
class ThrowingAccumulator {
public:
    ThrowingAccumulator(bool /*chunked*/, std::string /*producer_id*/) {}
    [[nodiscard]] bool terminal_seen() const noexcept { return false; }
    [[nodiscard]] result<std::vector<ChatResponseUpdate>> feed(std::string_view /*bytes*/) {
        throw std::runtime_error("P7: injected accumulator fault");
    }
    [[nodiscard]] std::vector<ChatResponseUpdate> finish() { return {}; }
};

// P7: runs the reactor pump with `Accumulator` against `server` on `runtime`, returning the drained stream.
template <class Accumulator>
[[nodiscard]] Drained pump_directly(BurstServer const& server, rt::Runtime& runtime, std::string& fail_klass) {
    namespace pw = agentengine::detail::provider_wire;
    pw::ProviderReactorOptions reactor;
    reactor.runtime = &runtime;
    rt::HttpRequest req;
    req.scheme = rt::http_scheme::http;
    req.host   = "127.0.0.1";
    req.port   = server.port();
    req.method = "POST";
    req.target = "/v1/chat/completions";
    req.body   = "{}";
    auto pair  = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
    (void)runtime.spawn(pw::pump_sse_stream_async<Accumulator>("openai", "m", pw::make_http_context(reactor, {}, false),
                                                               std::move(req), std::move(pair.producer),
                                                               std::stop_token{}, &runtime.offload_pool()));
    Drained const d = drain(pair.consumer, std::chrono::seconds(20));
    if (d.terminal == stream_terminal::failed) {
        fail_klass = pair.consumer.fail_error().klass == failure_class::fatal ? "fatal" : "other";
    }
    return d;
}

}  // namespace

int main() {
#if defined(_WIN32)
    agentengine::pal::ensure_winsock();
#endif
    InMemorySecretStore store;
    store.set("provider-key", "sk-loopback-server-ignores-this");
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"provider-key", std::chrono::seconds{0}}});
    EffectContext ctx{.deadline = agentengine::Deadline::none()};
    ctx.principal    = Principal{"pump-lifecycle", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    // ---- P1: a late consumer still gets every update, in order, with the channel full in between ---------
    {
        std::vector<std::string> events;
        std::string              expected;
        for (int i = 0; i < kBurst; ++i) {
            std::string const piece = std::to_string(i % 10);
            events.push_back(delta_event(piece));
            expected += piece;
        }
        events.push_back("data: [DONE]\n\n");
        BurstServer server(events, /*hold_open=*/false);
        check(server.ok(), "P1: server started");
        if (server.ok()) {
            auto                       client = make_client(server, store);
            stream<ChatResponseUpdate> s      = client->chat_stream(request_asking("hi"), ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(700));  // let the 256-slot channel fill
            Drained const d = drain(s, std::chrono::seconds(30));
            check(d.terminal == stream_terminal::closed, "P1: a consumer that was slow still reaches a clean close");
            check(d.text == expected && d.updates == kBurst,
                  "P1: every one of the 600 deltas arrived exactly once and in order across the full channel");
        }
    }

    // ---- P2: destroying the client with an undrained, in-flight stream is bounded -------------------------
    {
        std::vector<std::string> events;
        for (int i = 0; i < kBurst; ++i) events.push_back(delta_event("z"));  // no [DONE]: the answer is unfinished
        BurstServer server(events, /*hold_open=*/true);
        check(server.ok(), "P2: server started");
        if (server.ok()) {
            auto                       client = make_client(server, store);
            stream<ChatResponseUpdate> s      = client->chat_stream(request_asking("hi"), ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(700));  // channel full, the pump parked in a push
            // The destroying thread is the one that would drain, so an unbounded wait here is a deadlock.
            auto destroyed = std::async(std::launch::async, [&client] { client.reset(); });
            bool const prompt = destroyed.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
            check(prompt, "P2: destroying the client with a live, undrained stream returns promptly (no deadlock)");
            if (!prompt) {
                std::fprintf(stderr, "client destructor is stuck; exiting without joining it\n");
                std::fflush(stderr);
                std::_Exit(1);
            }
            Drained const d = drain(s, std::chrono::seconds(10));
            check(d.terminal == stream_terminal::failed && d.code == "net.cancelled",
                  "P2: and the stream ends failed net.cancelled -- stopped, not a clean (truncated) close");
        }
    }

    // ---- P3: a multi-line `data:` field is delivered whole ------------------------------------------------
    {
        std::vector<std::string> events;
        events.push_back("data: {\"choices\":[{\"delta\":{\"content\":\ndata: \"multi\"}}]}\n\n");
        events.push_back("data: [DONE]\n\n");
        BurstServer server(events, /*hold_open=*/false);
        check(server.ok(), "P3: server started");
        if (server.ok()) {
            auto                       client = make_client(server, store);
            stream<ChatResponseUpdate> s      = client->chat_stream(request_asking("hi"), ctx);
            Drained const              d      = drain(s, std::chrono::seconds(20));
            check(d.terminal == stream_terminal::closed && d.text == "multi",
                  "P3: an event whose JSON spans two `data:` lines is decoded whole, not cut after the first line");
        }
    }

    // ---- P4: a DNS failure stays transient (retried); only an address the policy refuses is policy ---------
    {
        using agentengine::detail::provider_wire::http_error_to_error;
        check(http_error_to_error(rt::http_error::resolve_failed, "net.host_unresolvable", "x").klass ==
                  failure_class::transient,
              "P4: an unresolvable host is transient, as on the blocking path");
        check(http_error_to_error(rt::http_error::address_rejected, "net.address_blocked", "x").klass ==
                  failure_class::policy,
              "P4: a policy-refused address stays policy");
    }

    // ---- P5: EffectContext::cancellation stops an in-flight chat() ------------------------------------------
    {
        BurstServer server({}, /*hold_open=*/true);  // headers, then silence
        check(server.ok(), "P5: server started");
        if (server.ok()) {
            auto             client = make_client(server, store);
            std::stop_source src;
            EffectContext    cctx{.deadline = agentengine::Deadline::none()};
            cctx.principal    = ctx.principal;
            cctx.capabilities = agentengine::borrow_capabilities(held);
            cctx.cancellation = src.get_token();
            auto pending = std::async(std::launch::async, [&] {
                return rt::block_on(client->chat(request_asking("hi"), cctx));
            });
            bool const held_open = pending.wait_for(std::chrono::milliseconds(800)) == std::future_status::timeout;
            check(held_open, "P5: (control) the silent server holds chat() open until something stops it");
            src.request_stop();
            bool const prompt = pending.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
            check(prompt, "P5: a stop on ctx.cancellation ends the in-flight chat() promptly");
            if (!prompt) {
                std::fprintf(stderr, "chat() ignored its cancellation; exiting without joining it\n");
                std::fflush(stderr);
                std::_Exit(1);
            }
            auto const r = pending.get();
            check(!r && r.error().klass == failure_class::canceled, "P5: ... and it ends `canceled`, not a result");
        }
    }

    // ---- P6: a host that does not resolve fails transient on both entry points -----------------------------
    {
        BurstServer server({}, /*hold_open=*/false);  // never reached: only its port is used
        check(server.ok(), "P6: server started");
        if (server.ok()) {
            int  lookups = 0;
            auto dead    = [&lookups](std::string_view, std::uint16_t) -> result<sandbox::VerifiedEndpoint> {
                ++lookups;
                return std::unexpected(error{failure_class::transient, "no such host", "net.host_unresolvable"});
            };
            auto client = make_client(server, store, dead, "no-such-host.invalid");
            auto r      = rt::block_on(client->chat(request_asking("hi"), ctx));
            check(!r && r.error().klass == failure_class::transient,
                  "P6: chat() to an unresolvable host fails transient (retried), not policy");
            stream<ChatResponseUpdate> s = client->chat_stream(request_asking("hi"), ctx);
            Drained const              d = drain(s, std::chrono::seconds(20));
            check(d.terminal == stream_terminal::failed && s.fail_error().klass == failure_class::transient,
                  "P6: chat_stream() to an unresolvable host fails transient (retried), not policy");
            check(lookups >= 2, "P6: (control) both calls went through the resolver that failed");
        }
    }

    // ---- P7: a fault inside the pump fails the stream, it never reads as a clean answer ----------------------
    {
        std::vector<std::string> events{delta_event("fine"), "data: [DONE]\n\n"};
        BurstServer              server(events, /*hold_open=*/false);
        check(server.ok(), "P7: server started");
        if (server.ok()) {
            rt::Runtime runtime{rt::RuntimeConfig{.lanes = 1}};
            std::string klass;
            Drained const ok = pump_directly<agentengine::openai::detail::StreamingUpdateAccumulator>(server, runtime, klass);
            check(ok.terminal == stream_terminal::closed && ok.text == "fine",
                  "P7: (control) the real accumulator closes the same stream cleanly with its text");
            Drained const bad = pump_directly<ThrowingAccumulator>(server, runtime, klass);
            check(bad.terminal == stream_terminal::failed && bad.code == "net.stream_pump_faulted" && klass == "fatal",
                  "P7: an exception inside the pump ends the stream failed net.stream_pump_faulted (fatal), not closed");
        }
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all checks passed\n");
    return 0;
}

#else
int main() { return 0; }
#endif
