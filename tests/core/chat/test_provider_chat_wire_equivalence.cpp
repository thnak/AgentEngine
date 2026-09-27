// #120 S6 (decisions/ADR-206-provider-chat-clients-out-of-line.md): the OpenAI and Anthropic ChatClient backends
// each had their own copy of the HTTP status -> error mapping. ADR-206 made both one function,
// provider_wire::http_status_error, parameterized by the vendor name; each backend's
// detail::map_http_status_error is now a one-line wrapper over it.
//
// 1. Differential. `oracle_openai::` and `oracle_anthropic::` below are verbatim copies of the two old functions
//    (origin/main at 48ade11, protocol/{openai,anthropic}/chat_client.hpp). For every status 0..999 and every
//    body in a corpus (empty, not JSON, JSON without `error`, `error` of each JSON type, `error.message` of each
//    JSON type, nested and odd documents), each backend's wrapper must return exactly the oracle's failure_class,
//    message and code.
// 2. Self-control. The harness reports differences when handed two mappings that really differ: the OpenAI
//    oracle against the Anthropic wrapper (the vendor name is in every fallback message and every code), and two
//    mappings that differ only in failure_class, or only in message, on one status.
// 3. Through the real clients and a loopback HTTP server (plaintext, production code from the socket up), for
//    both backends: chat() maps a non-2xx through the shared exchange (401 with a JSON error document -> policy,
//    the document's message, "<vendor>.http_401"; 503 with a non-JSON body -> transient, "<vendor> http status
//    503"); chat_stream() fails a 429 as transient "<vendor>.http_429"; and a streamed reasoning trace is stamped
//    "<vendor>:<model>" by the shared SSE pump. ADR-206 §5a: before these, breaking the exchange's status check or
//    the pump's producer id failed no test.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/pal/net.hpp"
#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/trust/secret.hpp"

#include "../../support/run_task_sync.hpp"

namespace {

using namespace agentengine;

int g_failures = 0;
void check(bool ok, char const* what) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

}  // namespace

// ---- verbatim oracles (origin/main 48ade11) --------------------------------------------------------------------
namespace oracle_openai {
using namespace agentengine;
[[nodiscard]] inline error map_http_status_error(std::uint16_t status, std::string const& body) {
    failure_class klass = failure_class::fatal;
    if (status == 429 || status >= 500) {
        klass = failure_class::transient;  // 004 §4: retry applies to Transient only
    } else if (status == 401 || status == 403) {
        klass = failure_class::policy;
    } else if (status >= 400) {
        klass = failure_class::contract;
    }
    std::string message = "openai http status " + std::to_string(status);
    if (auto parsed = json::parse(body); parsed) {
        if (auto const* err = parsed->find("error")) {
            if (auto const* m = err->find("message"); m && m->is_string()) message = m->as_string();
        }
    }
    return error{klass, message, "openai.http_" + std::to_string(status)};
}
}  // namespace oracle_openai

namespace oracle_anthropic {
using namespace agentengine;
[[nodiscard]] inline error map_http_status_error(std::uint16_t status, std::string const& body) {
    failure_class klass = failure_class::fatal;
    if (status == 429 || status >= 500) {
        klass = failure_class::transient;  // 004 §4: retry applies to Transient only
    } else if (status == 401 || status == 403) {
        klass = failure_class::policy;
    } else if (status >= 400) {
        klass = failure_class::contract;
    }
    std::string message = "anthropic http status " + std::to_string(status);
    if (auto parsed = json::parse(body); parsed) {
        if (auto const* err = parsed->find("error")) {
            if (auto const* m = err->find("message"); m && m->is_string()) message = m->as_string();
        }
    }
    return error{klass, message, "anthropic.http_" + std::to_string(status)};
}
}  // namespace oracle_anthropic

namespace {

std::vector<std::string> const kBodies = {
    "",
    "not json at all",
    "{",
    "{}",
    "[]",
    "null",
    "42",
    R"("error")",
    R"({"error":null})",
    R"({"error":true})",
    R"({"error":7})",
    R"({"error":"a plain string"})",
    R"({"error":["message"]})",
    R"({"error":{}})",
    R"({"error":{"type":"rate_limit_error"}})",
    R"({"error":{"message":"Rate limit reached"}})",
    R"({"error":{"message":""}})",
    R"({"error":{"message":null}})",
    R"({"error":{"message":12}})",
    R"({"error":{"message":{"nested":"x"}}})",
    R"({"error":{"message":["x"]}})",
    R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})",
    R"({"message":"top-level message only"})",
    R"({"error":{"message":"café \"quoted\" \\ back"}})",
    R"({"error":{"message":"first"},"error":{"message":"second"}})",
    R"(  {"error":{"message":"padded"}}  )",
    R"({"error":{"message":"trailing"}} junk)",
};

// Answers connection N with `responses[N]`, written verbatim (head and body), then closes.
class RawHttpServer {
public:
    explicit RawHttpServer(std::vector<std::string> responses) : responses_(std::move(responses)) {
        auto listen_r = pal::tcp_listen(static_cast<std::uint64_t>(kLoopback), 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_ = *pal::local_port(listen_fd_);
            thread_ = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~RawHttpServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        if (ok_) pal::close_fd(listen_fd_);
    }
    RawHttpServer(RawHttpServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

    static constexpr std::uint32_t kLoopback = (127u << 24) | 1u;

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            std::size_t const n = served_.fetch_add(1);
            if (n < responses_.size()) serve_one(*a, responses_[n], st);
            pal::close_fd(*a);
        }
    }

    void serve_one(pal::fd_t fd, std::string const& response, std::stop_token const& st) {
        std::string buf;
        std::byte chunk[1024];
        for (int i = 0; i < 400 && !st.stop_requested(); ++i) {
            auto r = pal::recv_some(fd, chunk, sizeof(chunk));
            if (!r) {
                if (r.error() == pal::would_block()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                return;
            }
            if (*r == 0) return;
            buf.append(reinterpret_cast<char const*>(chunk), *r);
            if (buf.find("\r\n\r\n") != std::string::npos) break;  // the head is enough to know a request came
        }
        std::size_t sent = 0;
        while (sent < response.size()) {
            auto w = pal::send_some(fd, reinterpret_cast<std::byte const*>(response.data() + sent),
                                    response.size() - sent);
            if (!w) {
                if (w.error() == pal::would_block()) continue;
                return;
            }
            sent += *w;
        }
    }

    std::vector<std::string> responses_;
    bool ok_ = false;
    pal::fd_t listen_fd_{};
    std::uint16_t port_ = 0;
    std::atomic<std::size_t> served_{0};
    std::jthread thread_;
};

[[nodiscard]] std::string http_response(std::string const& status_line, std::string const& content_type,
                                        std::string const& body) {
    return "HTTP/1.1 " + status_line + "\r\nContent-Type: " + content_type +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

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

struct Drained {
    std::vector<ChatResponseUpdate> updates;
    stream_terminal terminal = stream_terminal::open;
    error failure{};
};

[[nodiscard]] Drained drain(stream<ChatResponseUpdate> s) {
    Drained out;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!s.done() && std::chrono::steady_clock::now() < deadline) {
        while (auto u = s.next()) out.updates.push_back(std::move(*u));
        if (!s.done()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    while (auto u = s.next()) out.updates.push_back(std::move(*u));
    out.terminal = s.terminal();
    if (out.terminal == stream_terminal::failed) out.failure = s.fail_error();
    return out;
}

[[nodiscard]] std::string reasoning_producer(Drained const& d) {
    for (auto const& u : d.updates) {
        if (auto const* r = std::get_if<Reasoning>(&u.delta.value)) return r->producer_chat_client_id;
    }
    return "<no reasoning item>";
}

// One backend's transport cases. `make_client(port)` builds a plaintext client bound to the server's port.
template <class MakeClient>
void transport_cases(std::string const& v, MakeClient make_client, std::string const& reasoning_sse,
                     EffectContext& ctx) {
    using test_support::run_task_sync;
    auto label = [&](char const* what) { return v + ": " + what; };

    {
        RawHttpServer server({http_response("401 Unauthorized", "application/json",
                                            R"({"error":{"type":"authentication_error","message":"bad key"}})"),
                              http_response("503 Service Unavailable", "text/plain", "upstream down")});
        check(server.ok(), label("status server started").c_str());
        auto client = make_client(server.port());
        auto r401 = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), ctx));
        check(!r401 && r401.error().klass == failure_class::policy && r401.error().message == "bad key" &&
                  r401.error().code == v + ".http_401",
              label("chat() maps a 401 with a JSON error document to policy, its message, <vendor>.http_401").c_str());
        auto r503 = run_task_sync<result<ChatResponse>>(client.chat(request_asking("hi"), ctx));
        check(!r503 && r503.error().klass == failure_class::transient &&
                  r503.error().message == v + " http status 503" && r503.error().code == v + ".http_503",
              label("chat() maps a 503 with a non-JSON body to transient, \"<vendor> http status 503\"").c_str());
    }
    {
        RawHttpServer server({http_response("429 Too Many Requests", "application/json",
                                            R"({"error":{"message":"slow down"}})")});
        check(server.ok(), label("stream status server started").c_str());
        auto client = make_client(server.port());
        Drained d = drain(client.chat_stream(request_asking("hi"), ctx));
        check(d.terminal == stream_terminal::failed && d.failure.klass == failure_class::transient &&
                  d.failure.code == v + ".http_429" && d.failure.message == v + " http status 429" &&
                  d.updates.empty(),
              label("chat_stream() fails a 429 as transient <vendor>.http_429, nothing pushed").c_str());
    }
    {
        RawHttpServer server({"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: " +
                              std::to_string(reasoning_sse.size()) + "\r\nConnection: close\r\n\r\n" +
                              reasoning_sse});
        check(server.ok(), label("reasoning server started").c_str());
        auto client = make_client(server.port());
        Drained d = drain(client.chat_stream(request_asking("hi"), ctx));
        check(d.terminal == stream_terminal::closed, label("the reasoning stream completes").c_str());
        check(reasoning_producer(d) == v + ":m-test",
              label("a streamed reasoning trace is stamped \"<vendor>:<model>\" by the shared pump").c_str());
    }
}

// Returns the number of (status, body) pairs on which `got` and `want` differ in class, message or code.
template <class Got, class Want>
int count_differences(Got got, Want want) {
    int diffs = 0;
    for (std::uint32_t status = 0; status < 1000; ++status) {
        for (std::string const& body : kBodies) {
            auto const s = static_cast<std::uint16_t>(status);
            error const a = got(s, body);
            error const b = want(s, body);
            if (a.klass != b.klass || a.message != b.message || a.code != b.code) ++diffs;
        }
    }
    return diffs;
}

}  // namespace

int main() {
    int const openai = count_differences(
        [](std::uint16_t s, std::string const& b) { return openai::detail::map_http_status_error(s, b); },
        [](std::uint16_t s, std::string const& b) { return oracle_openai::map_http_status_error(s, b); });
    check(openai == 0, "OpenAI: the shared mapping equals the old OpenAI copy on every status and body");

    int const anthropic = count_differences(
        [](std::uint16_t s, std::string const& b) { return anthropic::detail::map_http_status_error(s, b); },
        [](std::uint16_t s, std::string const& b) { return oracle_anthropic::map_http_status_error(s, b); });
    check(anthropic == 0, "Anthropic: the shared mapping equals the old Anthropic copy on every status and body");

    // Self-control: every pair differs (the vendor name is in every code), so all 1000 * |corpus| must be counted.
    int const control = count_differences(
        [](std::uint16_t s, std::string const& b) { return anthropic::detail::map_http_status_error(s, b); },
        [](std::uint16_t s, std::string const& b) { return oracle_openai::map_http_status_error(s, b); });
    check(control == static_cast<int>(1000 * kBodies.size()),
          "self-control: the harness sees the difference between the Anthropic and OpenAI mappings");

    // Self-controls that differ in ONE field each, so a comparator ignoring `klass` or `message` cannot pass:
    // the oracle with 401 reclassified (every body at 401, and only there), and with 500's message changed.
    int const class_only = count_differences(
        [](std::uint16_t s, std::string const& b) {
            error e = oracle_openai::map_http_status_error(s, b);
            if (s == 401) e.klass = failure_class::contract;
            return e;
        },
        [](std::uint16_t s, std::string const& b) { return oracle_openai::map_http_status_error(s, b); });
    check(class_only == static_cast<int>(kBodies.size()),
          "self-control: a difference in failure_class alone is counted, on exactly the changed status");
    int const message_only = count_differences(
        [](std::uint16_t s, std::string const& b) {
            error e = oracle_anthropic::map_http_status_error(s, b);
            if (s == 500) e.message += "!";
            return e;
        },
        [](std::uint16_t s, std::string const& b) { return oracle_anthropic::map_http_status_error(s, b); });
    check(message_only == static_cast<int>(kBodies.size()),
          "self-control: a difference in message alone is counted, on exactly the changed status");

    // ---- 3. through the real clients -------------------------------------------------------------------------
#if defined(_WIN32)
    pal::ensure_winsock();
    _putenv_s("AGENTENGINE_NET_IO_TIMEOUT_MS", "5000");
#else
    setenv("AGENTENGINE_NET_IO_TIMEOUT_MS", "5000", 1);
#endif
    InMemorySecretStore store;
    store.set("provider-key", "sk-loopback-server-ignores-this");
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"provider-key", std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal = Principal{"test-principal", ""};
    ctx.capabilities = borrow_capabilities(held);
    auto const loopback = [](std::string_view, std::uint16_t port) -> result<sandbox::VerifiedEndpoint> {
        return sandbox::VerifiedEndpoint{RawHttpServer::kLoopback, port};
    };

    transport_cases(
        "openai",
        [&](std::uint16_t port) {
            return openai::OpenAIChatClient<InMemorySecretStore>(
                "127.0.0.1", port, "m-test", SecretRef{"provider-key"}, ChatClientCapabilities{}, store, "/v1",
                loopback, std::string{}, std::string{}, std::string{}, std::string{}, std::nullopt,
                sandbox::ProviderTransport::plaintext_http);
        },
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"thinking it over\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":7,\"total_tokens\":12}}\n\n"
        "data: [DONE]\n\n",
        ctx);

    transport_cases(
        "anthropic",
        [&](std::uint16_t port) {
            return anthropic::AnthropicChatClient<InMemorySecretStore>(
                "127.0.0.1", port, "m-test", SecretRef{"provider-key"}, ChatClientCapabilities{}, store, "/v1",
                "2023-06-01", loopback, std::string{}, std::string{}, std::string{}, std::string{},
                std::string{}, sandbox::ProviderTransport::plaintext_http);
        },
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
        "\"model\":\"m-test\",\"content\":[],\"usage\":{\"input_tokens\":5,\"output_tokens\":1}}}\n\n"
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\","
        "\"thinking\":\"\"}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\","
        "\"thinking\":\"thinking it over\"}}\n\n"
        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":7}}\n\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n",
        ctx);

    std::printf("pairs per backend: %zu; differences: openai %d, anthropic %d; control %d\n",
                1000 * kBodies.size(), openai, anthropic, control);
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_provider_chat_wire_equivalence: OK\n");
    return 0;
}
