// decisions/ADR-235-batch-inference-coalescing.md §3.2: the OpenRouter `BatchBackend`'s wire behaviour, offline,
// against a loopback HTTP server that replays the documented response shapes
// (https://openrouter.ai/docs/batch-quickstart.md, fetched 2026-10-02). The live counterpart is
// test_openrouter_batch_live_e2e.cpp.
//
// R1  submit: POST {prefix}/batches, bearer key, `endpoint` and `model` serialized BEFORE `requests`, the
//     engine's custom ids passed through verbatim, each body translated by the synchronous client's own code.
// R2  poll: GET {prefix}/batches/{id}; an in-progress batch is not ended and carries no items.
// R3  a completed batch's inline results: a succeeded item parses to the assistant text + usage; an `error`
//     item is errored with its message and a class from its code; results are matched by custom id, not order.
// R4  a failed batch (one bad request fails the whole batch on OpenRouter) is ended with `results: null` and
//     the vendor's error message as detail -- so the engine resolves every item as expired.
// R5  admit() refuses what OpenRouter could reject or silently drop (tools, output schema, reasoning effort,
//     non-text content), and accepts plain text, reporting a positive size.
// R6  cancel() is refused (no cancel endpoint); release() is DELETE {prefix}/batches/{id}.
// R7  a non-2xx answer is an error with the HTTP-status class (429 transient, 401 policy).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/net.hpp"
#include "agentengine/protocol/openai/openrouter_batch_backend.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"

using namespace agentengine;
using agentengine::sandbox::ProviderTransport;

namespace {

int g_failures = 0;
void check(bool cond, std::string const& what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else {
        std::fprintf(stderr, "  ok: %s\n", what.c_str());
    }
}

constexpr std::uint32_t kLoopbackHostOrder = (127u << 24) | 1u;

struct Reply {
    int         status = 200;
    std::string body;
};

// Serves connection N with reply N and records the raw request it received.
class ScriptedHttpServer {
public:
    explicit ScriptedHttpServer(std::vector<Reply> replies) : replies_(std::move(replies)) {
        auto listen_r = pal::tcp_listen(static_cast<std::uint64_t>(kLoopbackHostOrder), 0);
        ok_ = listen_r.has_value();
        if (ok_) {
            listen_fd_ = *listen_r;
            port_      = *pal::local_port(listen_fd_);
            thread_    = std::jthread([this](std::stop_token st) { run(st); });
        }
    }
    ~ScriptedHttpServer() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        if (ok_) pal::close_fd(listen_fd_);
    }
    ScriptedHttpServer(ScriptedHttpServer const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] std::vector<std::string> requests() {
        std::lock_guard<std::mutex> l(mu_);
        return requests_;
    }

private:
    void run(std::stop_token st) {
        while (!st.stop_requested()) {
            auto a = pal::accept_one(listen_fd_);
            if (!a) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            std::size_t const n = served_++;
            serve_one(*a, n < replies_.size() ? replies_[n] : Reply{500, "{}"}, st);
            pal::close_fd(*a);
        }
    }

    void write_all(pal::fd_t fd, std::string const& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            auto w = pal::send_some(fd, reinterpret_cast<std::byte const*>(data.data() + sent), data.size() - sent);
            if (!w) {
                if (w.error() == pal::would_block()) continue;
                return;
            }
            sent += *w;
        }
    }

    void serve_one(pal::fd_t fd, Reply const& reply, std::stop_token const& st) {
        std::string buf;
        std::byte   chunk[4096];
        std::size_t want = std::string::npos;
        for (int i = 0; i < 2000 && !st.stop_requested(); ++i) {
            auto r = pal::recv_some(fd, chunk, sizeof(chunk));
            if (!r) {
                if (r.error() == pal::would_block()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                break;
            }
            if (*r == 0) break;
            buf.append(reinterpret_cast<char const*>(chunk), *r);
            std::size_t const head_end = buf.find("\r\n\r\n");
            if (head_end != std::string::npos && want == std::string::npos) {
                std::size_t len = 0;
                std::size_t const cl = buf.find("Content-Length:");
                if (cl != std::string::npos && cl < head_end) len = std::stoul(buf.substr(cl + 15));
                want = head_end + 4 + len;
            }
            if (want != std::string::npos && buf.size() >= want) break;
        }
        {
            std::lock_guard<std::mutex> l(mu_);
            requests_.push_back(buf);
        }
        write_all(fd, "HTTP/1.1 " + std::to_string(reply.status) + " X\r\nContent-Type: application/json\r\n"
                      "Content-Length: " + std::to_string(reply.body.size()) + "\r\nConnection: close\r\n\r\n" +
                      reply.body);
    }

    std::vector<Reply>        replies_;
    bool                      ok_ = false;
    pal::fd_t                 listen_fd_{};
    std::uint16_t             port_ = 0;
    std::atomic<std::size_t>  served_{0};
    std::mutex                mu_;
    std::vector<std::string>  requests_;
    std::jthread              thread_;
};

[[nodiscard]] Message user_text(std::string text) {
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m;
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) out += t->text;
    }
    return out;
}

[[nodiscard]] ChatRequest ask(std::string q) {
    ChatRequest r;
    r.messages.push_back(user_text(std::move(q)));
    return r;
}

}  // namespace

int main() {
#if defined(_WIN32)
    pal::ensure_winsock();
#endif
    InMemorySecretStore store;
    store.set("or-key", "sk-or-loopback");
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"or-key", std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal    = Principal{"p", ""};
    ctx.capabilities = borrow_capabilities(held);

    std::string const completed = R"({"id":"batch_123","object":"batch","status":"completed","request_counts":{"total":2,"completed":1,"failed":1},
      "results":[
        {"id":"r2","custom_id":"i1","response":null,"error":{"code":429,"message":"rate limited upstream"}},
        {"id":"r1","custom_id":"i0","response":{"status_code":200,"body":{"id":"gen-1","object":"chat.completion","model":"openai/gpt-4o-mini",
          "choices":[{"index":0,"message":{"role":"assistant","content":"forty-two"},"finish_reason":"stop"}],
          "usage":{"prompt_tokens":11,"completion_tokens":3,"total_tokens":14}}},"error":null}
      ],"error":null})";

    ScriptedHttpServer server({
        {202, R"({"id":"batch_123","object":"batch","status":"validating","results":null,"error":null})"},
        {200, R"({"id":"batch_123","object":"batch","status":"in_progress","results":null,"error":null})"},
        {200, completed},
        {200, R"({"id":"batch_9","object":"batch","status":"failed","results":null,"error":{"message":"request i3: base64 images are rejected"}})"},
        {200, R"({"deletion":{"openrouter":"deleted"}})"},
        {429, R"({"error":{"message":"slow down"}})"},
        {401, R"({"error":{"message":"bad key"}})"},
    });
    check(server.ok(), "setup: loopback server listening");

    openai::OpenRouterBatchBackend<InMemorySecretStore> backend("127.0.0.1", server.port(), "openai/gpt-4o-mini",
                                                                SecretRef{"or-key"}, store, "/api/v1", sandbox::resolve_host,
                                                                {}, ProviderTransport::plaintext_http);

    // ---- R1 ----
    auto job = backend.submit({{"i0", ask("what is six times seven?")}, {"i1", ask("say hi")}}, ctx);
    check(job.has_value() && *job == "batch_123", "R1: submit returns the vendor's job id");
    {
        auto reqs = server.requests();
        std::string const r = reqs.empty() ? std::string{} : reqs[0];
        check(r.rfind("POST /api/v1/batches ", 0) == 0, "R1: POST {prefix}/batches");
        check(r.find("Authorization: Bearer sk-or-loopback") != std::string::npos, "R1: bearer key from the SecretRef");
        std::size_t const e = r.find("\"endpoint\""), m = r.find("\"model\""), q = r.find("\"requests\"");
        check(e != std::string::npos && m != std::string::npos && q != std::string::npos && e < q && m < q,
              "R1: endpoint and model precede requests (OpenRouter stream-parses the body)");
        check(r.find("\"/v1/chat/completions\"") != std::string::npos, "R1: chat-completions endpoint");
        check(r.find("\"custom_id\":\"i0\"") != std::string::npos && r.find("\"custom_id\":\"i1\"") != std::string::npos,
              "R1: the engine's custom ids pass through verbatim");
        check(r.find("what is six times seven?") != std::string::npos, "R1: the request body carries the prompt");
    }

    // ---- R2 ----
    auto p1 = backend.poll("batch_123", ctx);
    check(p1.has_value() && !p1->ended && p1->items.empty(), "R2: in_progress -> not ended, no items");
    {
        auto reqs = server.requests();
        check(reqs.size() >= 2 && reqs[1].rfind("GET /api/v1/batches/batch_123 ", 0) == 0, "R2: GET {prefix}/batches/{id}");
    }

    // ---- R3 ----
    auto p2 = backend.poll("batch_123", ctx);
    check(p2.has_value() && p2->ended && p2->items.size() == 2, "R3: completed -> ended with both results");
    if (p2 && p2->items.size() == 2) {
        BatchItemResult const* ok = nullptr;
        BatchItemResult const* bad = nullptr;
        for (auto const& it : p2->items) (it.custom_id == "i0" ? ok : bad) = &it;
        check(ok && ok->status == batch_item_status::succeeded && text_of(ok->response.message) == "forty-two",
              "R3: i0 succeeded with the assistant text (matched by id, though it came second)");
        check(ok && ok->response.usage.input_tokens == 11 && ok->response.usage.output_tokens == 3,
              "R3: i0 carries its usage");
        check(bad && bad->status == batch_item_status::errored && bad->detail == "rate limited upstream" &&
                  bad->klass == failure_class::transient,
              "R3: i1 errored, with its message and a transient class from code 429");
    }

    // ---- R4 ----
    auto p3 = backend.poll("batch_9", ctx);
    check(p3.has_value() && p3->ended && p3->items.empty() &&
              p3->detail == "request i3: base64 images are rejected",
          "R4: a failed batch is ended, has no results, and reports the vendor's reason");

    // ---- R5 ----
    check(backend.admit(ask("plain")).has_value() && *backend.admit(ask("plain")) > 0, "R5: plain text is admitted");
    {
        ChatRequest t = ask("x");
        t.tools.push_back(ToolDescriptor{});
        check(!backend.admit(t).has_value(), "R5: tools are refused");
        ChatRequest s = ask("x");
        s.output_schema_json = std::string("{}");
        check(!backend.admit(s).has_value(), "R5: an output schema is refused");
        ChatRequest e = ask("x");
        e.reasoning_effort = reasoning_effort::high;
        check(!backend.admit(e).has_value(), "R5: a reasoning effort is refused");
        ChatRequest img = ask("x");
        ContentItem media;
        media.value = Media{};
        img.messages[0].content.push_back(media);
        check(!backend.admit(img).has_value(), "R5: non-text content is refused");
        check(!backend.admit(ChatRequest{}).has_value(), "R5: an empty request is refused");
    }

    // ---- R6 ----
    check(!backend.cancel("batch_123", ctx).has_value(), "R6: cancel is refused (OpenRouter has no cancel endpoint)");
    check(backend.release("batch_123", ctx).has_value(), "R6: release succeeds");
    {
        auto reqs = server.requests();
        check(reqs.size() >= 5 && reqs[4].rfind("DELETE /api/v1/batches/batch_123 ", 0) == 0, "R6: release is DELETE");
    }

    // ---- R7 ----
    auto t429 = backend.poll("batch_123", ctx);
    check(!t429.has_value() && t429.error().klass == failure_class::transient, "R7: 429 is a transient error");
    auto t401 = backend.poll("batch_123", ctx);
    check(!t401.has_value() && t401.error().klass == failure_class::policy, "R7: 401 is a policy error");

    check(backend.group_key().find("openai/gpt-4o-mini") != std::string::npos, "group key names the model");

    // ---- R8: every documented non-terminal status keeps the job pending -- `cancelling` included, which is
    // not terminal (only completed/failed/expired/cancelled are) and must never expire the remainder.
    for (char const* status : {"validating", "in_progress", "finalizing", "cancelling"}) {
        auto body = json::parse(std::string(R"({"id":"b","status":")") + status + R"(","results":null})");
        auto p    = body ? openai::detail::parse_openrouter_batch_object(*body, "openai:m")
                         : result<BatchPoll>(std::unexpected(body.error()));
        check(p.has_value() && !p->ended, std::string("R8: '") + status + "' is not ended");
    }
    for (char const* status : {"completed", "failed", "expired", "cancelled"}) {
        auto body = json::parse(std::string(R"({"id":"b","status":")") + status + R"(","results":null})");
        auto p    = body ? openai::detail::parse_openrouter_batch_object(*body, "openai:m")
                         : result<BatchPoll>(std::unexpected(body.error()));
        check(p.has_value() && p->ended, std::string("R8: '") + status + "' is ended");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall checks passed\n");
    return 0;
}
