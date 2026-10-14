// decisions/ADR-237 §5.1 (#172, Phase C slice 2): `OpenAIEmbedder` and `QdrantVectorIndex` run their exchange
// on `rt::http` (`provider_wire::exchange_async`) instead of the blocking `perform_provider_https_exchange` --
// they were "async in name only" (decision A8). Driven offline against a loopback server.
//
// W1  Qdrant over the wire: add_batch is `PUT .../points` with the `api-key` header; search parses the result;
//     contains maps 404 to `false` and 200 to `true`; a 500 is a transient error carrying Qdrant's message.
// W2  The embedder over the wire: `POST /v1/embeddings` with a bearer key, vectors parsed by index.
// C1  `ctx.cancellation` ends an in-flight call -- an embed and a Qdrant search against a server that accepts
//     and never answers both return `canceled` soon after the stop. The blocking exchange took no stop token
//     at all, so this is the property the conversion adds.
// H1  A host-supplied `ProviderReactorOptions` is used, not replaced: its resolver is the only route to a
//     `.invalid` host (RFC 6761), and the calls run on the host's `rt::Runtime`.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/net.hpp"
#include "agentengine/protocol/openai/embedder.hpp"
#include "agentengine/protocol/qdrant/vector_index.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "../../support/scripted_http_server.hpp"

using namespace agentengine;
using agentengine::sandbox::ProviderTransport;
using agentengine::test_support::ScriptedHttpServer;

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

// A content digest is 64 hex characters; Qdrant derives its point id from the first 32.
std::string const kDigest = std::string(32, 'a') + std::string(32, 'b');

using OpenAiEmbedder = openai::OpenAIEmbedder<InMemorySecretStore>;
using Qdrant   = qdrant::QdrantVectorIndex<InMemorySecretStore>;

// Runs `call` on another thread, requests `stop` after 150 ms, and requires it to end within 10 s. A call that
// ignores the stop cannot be unwound, so the process ends here with the failure printed.
template <class Call>
auto run_and_cancel(std::stop_source& stop, Call call, char const* what) {
    auto fut = std::async(std::launch::async, call);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto const stopped_at = std::chrono::steady_clock::now();
    stop.request_stop();
    if (fut.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        std::fprintf(stderr, "FAIL: C1: %s is still in flight 10 s after the stop\n", what);
        std::_Exit(1);
    }
    auto r = fut.get();
    check(std::chrono::steady_clock::now() - stopped_at < std::chrono::seconds(5),
          std::string("C1: ") + what + " returns promptly after the stop");
    return r;
}

}  // namespace

int main() {
#if defined(_WIN32)
    pal::ensure_winsock();
#endif
    InMemorySecretStore store;
    store.set("key", "sk-loopback");
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"key", std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal    = Principal{"p", ""};
    ctx.capabilities = borrow_capabilities(held);

    // ---- W1: Qdrant ----
    {
        std::string const hit = R"({"status":"ok","time":0.001,"result":[{"score":0.9,"payload":{"chunk_id":")" +
                                kDigest + R"("}}]})";
        ScriptedHttpServer server({
            {200, R"({"status":"ok","time":0.001,"result":{"operation_id":1,"status":"completed"}})"},
            {200, hit},
            {404, R"({"status":{"error":"Not found: point"}})"},
            {200, R"({"status":"ok","result":{"id":"x"}})"},
            {500, R"({"status":{"error":"storage is on fire"}})"},
        });
        check(server.ok(), "W1 setup: loopback server listening");
        Qdrant index("127.0.0.1", server.port(), "docs", SecretRef{"key"}, store, "", 0, sandbox::resolve_host, {},
                     ProviderTransport::plaintext_http);

        auto added = rt::block_on(index.add_batch({kDigest}, {{1.0f, 0.0f}}, ctx));
        check(added.has_value(), "W1: add_batch succeeds" + (added ? std::string{} : " -- " + added.error().code));
        auto reqs = server.requests();
        check(!reqs.empty() && reqs[0].rfind("PUT /collections/docs/points ", 0) == 0, "W1: add_batch is PUT .../points");
        check(!reqs.empty() && reqs[0].find("api-key: sk-loopback") != std::string::npos,
              "W1: the key travels in the api-key header");

        float const q[2] = {1.0f, 0.0f};
        auto found = rt::block_on(index.search(std::span<float const>(q, 2), 3, ctx));
        check(found.has_value() && found->size() == 1 && found->front().id == kDigest,
              "W1: search parses the scored chunk id");
        auto missing = rt::block_on(index.contains(kDigest, ctx));
        check(missing.has_value() && !*missing, "W1: contains maps 404 to false");
        auto present = rt::block_on(index.contains(kDigest, ctx));
        check(present.has_value() && *present, "W1: contains maps 200 to true");
        auto failed = rt::block_on(index.search(std::span<float const>(q, 2), 3, ctx));
        check(!failed && failed.error().klass == failure_class::transient &&
                  failed.error().message == "storage is on fire",
              "W1: a 500 is a transient error carrying Qdrant's own message");
    }

    // ---- W2: the embedder ----
    {
        ScriptedHttpServer server({{200, R"({"object":"list","data":[{"object":"embedding","index":1,"embedding":[0,1]},)"
                                         R"({"object":"embedding","index":0,"embedding":[1,0]}],"model":"m"})"}});
        check(server.ok(), "W2 setup: loopback server listening");
        OpenAiEmbedder embedder("127.0.0.1", server.port(), "m", SecretRef{"key"}, EmbedderCapabilities{2, 16}, store, "/v1",
                          sandbox::resolve_host, {}, ProviderTransport::plaintext_http);
        auto vecs = rt::block_on(embedder.embed_batch({"first", "second"}, ctx));
        check(vecs.has_value() && vecs->size() == 2 && (*vecs)[0] == std::vector<float>{1.0f, 0.0f} &&
                  (*vecs)[1] == std::vector<float>{0.0f, 1.0f},
              "W2: embed_batch parses both vectors by index" + (vecs ? std::string{} : " -- " + vecs.error().code));
        auto reqs = server.requests();
        check(!reqs.empty() && reqs[0].rfind("POST /v1/embeddings ", 0) == 0, "W2: POST /v1/embeddings");
        check(!reqs.empty() && reqs[0].find("Authorization: Bearer sk-loopback") != std::string::npos,
              "W2: bearer key from the SecretRef");
    }

    // ---- C1: cancellation reaches the in-flight exchange ----
    {
        auto silent = test_support::silent_listener();
        check(silent.has_value(), "C1 setup: silent listener");
        if (silent) {
            {
                OpenAiEmbedder embedder("127.0.0.1", silent->port, "m", SecretRef{"key"}, EmbedderCapabilities{2, 16}, store,
                                  "/v1", sandbox::resolve_host, {}, ProviderTransport::plaintext_http);
                std::stop_source stop;
                EffectContext    cctx = ctx;
                cctx.cancellation     = stop.get_token();
                auto r = run_and_cancel(stop, [&] { return rt::block_on(embedder.embed_batch({"x"}, cctx)); },
                                        "an embed");
                check(!r && r.error().klass == failure_class::canceled,
                      "C1: the cancelled embed fails as canceled" + (r ? std::string{} : " (" + r.error().code + ")"));
            }
            {
                Qdrant index("127.0.0.1", silent->port, "docs", SecretRef{"key"}, store, "", 0, sandbox::resolve_host,
                             {}, ProviderTransport::plaintext_http);
                std::stop_source stop;
                EffectContext    cctx = ctx;
                cctx.cancellation     = stop.get_token();
                float const q[2]      = {1.0f, 0.0f};
                auto r = run_and_cancel(
                    stop, [&] { return rt::block_on(index.search(std::span<float const>(q, 2), 3, cctx)); },
                    "a Qdrant search");
                check(!r && r.error().klass == failure_class::canceled,
                      "C1: the cancelled search fails as canceled" + (r ? std::string{} : " (" + r.error().code + ")"));
            }
            pal::close_fd(silent->fd);
        }
    }

    // ---- H1: the host's reactor options are the ones used ----
    {
        ScriptedHttpServer server({
            {200, R"({"object":"list","data":[{"object":"embedding","index":0,"embedding":[1,0]}],"model":"m"})"},
            {404, R"({"status":{"error":"Not found: point"}})"},
        });
        check(server.ok(), "H1 setup: loopback server listening");
        rt::Runtime      host_rt{rt::RuntimeConfig{.lanes = 1}};
        std::atomic<int> resolved{0};
        auto make_opts = [&] {
            agentengine::detail::provider_wire::ProviderReactorOptions opts;
            opts.runtime  = &host_rt;
            opts.resolver = [&resolved](std::string const& host) {
                pal::ResolveOutcome out;
                if (host == "rag.invalid") {
                    ++resolved;
                    out.addresses.push_back(pal::IpAddress::v4(127, 0, 0, 1));
                } else {
                    out.error = pal::resolve_error::not_found;
                }
                return out;
            };
            return opts;
        };
        OpenAiEmbedder embedder("rag.invalid", server.port(), "m", SecretRef{"key"}, EmbedderCapabilities{2, 16}, store,
                          "/v1", sandbox::resolve_host, {}, ProviderTransport::plaintext_http, make_opts());
        auto vecs = rt::block_on(embedder.embed_batch({"x"}, ctx));
        check(vecs.has_value(), "H1: the embedder reaches the server through the host's resolver");
        Qdrant index("rag.invalid", server.port(), "docs", SecretRef{"key"}, store, "", 0, sandbox::resolve_host, {},
                     ProviderTransport::plaintext_http, make_opts());
        auto has = rt::block_on(index.contains(kDigest, ctx));
        check(has.has_value() && !*has, "H1: the Qdrant index reaches the server through the host's resolver");
        check(resolved.load() >= 2, "H1: the host-supplied resolver was called for both");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall checks passed\n");
    return 0;
}
