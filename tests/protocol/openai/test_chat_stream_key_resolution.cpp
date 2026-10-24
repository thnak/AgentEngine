// decisions/archive/ADR-237 §5.1 (#172, Phase C SecretStore slice): `SecretStore::resolve` is `task`-shaped, and both real
// chat clients resolve their API key as the FIRST step of `chat_stream`'s producer root (an owned root on the
// client's `rt::Runtime`), not inline in `chat_stream()`. Driven offline against a loopback server, for both
// `OpenAIChatClient` and `AnthropicChatClient`.
//
// K1  A denied grant fails the stream with the store's OWN error (`secret.not_granted`, failure_class::policy),
//     and no connection is ever made: resolution precedes any network I/O.
// K2  A granted key reaches the wire (`Authorization: Bearer` / `x-api-key`) after the asynchronous resolve; the
//     server's 401 then fails the stream (policy, http_status_error's mapping), as before the conversion.
// K3  `chat_stream()` called ON a runtime lane (as a session does) with a store whose resolve genuinely suspends
//     on the reactor: the stream still reaches the wire. Resolving inline with `rt::block_on` -- the shape a
//     synchronous `chat_stream()` would need -- is refused on a lane (`rt.block_on_on_lane`), so this check
//     fails if the resolve ever moves back out of the producer root.

#include <chrono>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <thread>

#include "agentengine/core/chat_stream_drain.hpp"
#include "agentengine/pal/net.hpp"
#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/runtime.hpp"
#include "agentengine/rt/sleep.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "../../support/scripted_http_server.hpp"

using namespace agentengine;
using agentengine::sandbox::ProviderTransport;
using agentengine::test_support::ScriptedHttpServer;
using namespace std::chrono_literals;

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

// A store that parks on the reactor before answering -- a vault-backed store's shape.
class SuspendingSecretStore {
public:
    explicit SuspendingSecretStore(pal::Reactor& reactor) : reactor_(&reactor) { inner_.set("key", "sk-loopback"); }

    [[nodiscard]] task<result<SecretLease>> resolve(SecretRef ref, EffectContext& ctx) const {
        (void)co_await rt::sleep_for(*reactor_, 5ms);
        ++parks;
        co_return co_await inner_.resolve(std::move(ref), ctx);
    }

    mutable int parks = 0;

private:
    pal::Reactor*       reactor_;
    InMemorySecretStore inner_;
};
static_assert(SecretStore<SuspendingSecretStore>);

ChatRequest one_message() {
    ChatRequest req;
    Message     m;
    m.role = role::user;
    ContentItem item;
    item.value = Text{"hi"};
    m.content.push_back(std::move(item));
    req.messages.push_back(std::move(m));
    return req;
}

std::string const kUnauthorized = R"({"error":{"message":"bad key","type":"authentication_error"}})";

template <class Store>
auto make_openai(std::uint16_t port, Store const& store, detail::provider_wire::ProviderReactorOptions r = {}) {
    return openai::OpenAIChatClient<Store>("127.0.0.1", port, "m", SecretRef{"key"}, ChatClientCapabilities{}, store,
                                           "/v1", sandbox::resolve_host, {}, {}, {}, {}, std::nullopt,
                                           ProviderTransport::plaintext_http, false, {}, std::move(r));
}
template <class Store>
auto make_anthropic(std::uint16_t port, Store const& store, detail::provider_wire::ProviderReactorOptions r = {}) {
    return anthropic::AnthropicChatClient<Store>("127.0.0.1", port, "m", SecretRef{"key"}, ChatClientCapabilities{},
                                                 store, "/v1", "2023-06-01", sandbox::resolve_host, {}, {}, {}, {},
                                                 {}, ProviderTransport::plaintext_http, {}, std::move(r));
}

// K1 + K2 for one vendor.
template <class Make>
void denied_then_granted(char const* vendor, Make make, std::string const& key_header) {
    InMemorySecretStore store;
    store.set("key", "sk-loopback");
    std::string const v = vendor;

    {
        ScriptedHttpServer server({{401, kUnauthorized}});
        auto               client = make(server.port(), store);
        CapabilitySet      none   = CapabilitySet::grant_root({});
        EffectContext      ctx{.deadline = agentengine::Deadline::none()};
        ctx.principal    = Principal{"p", ""};
        ctx.capabilities = borrow_capabilities(none);
        auto drained     = drain_chat_stream(client.chat_stream(one_message(), ctx));
        check(!drained.ok && drained.failure.code == "secret.not_granted" &&
                  drained.failure.klass == failure_class::policy,
              "K1 " + v + ": a denied grant fails the stream with the store's own error (got '" +
                  drained.failure.code + "')");
        std::this_thread::sleep_for(100ms);
        check(server.requests().empty(), "K1 " + v + ": no connection is made before the key resolves");
    }
    {
        ScriptedHttpServer server({{401, kUnauthorized}});
        auto               client = make(server.port(), store);
        CapabilitySet      held   = CapabilitySet::grant_root({cap::Secret{"key", std::chrono::seconds{0}}});
        EffectContext      ctx{.deadline = agentengine::Deadline::none()};
        ctx.principal    = Principal{"p", ""};
        ctx.capabilities = borrow_capabilities(held);
        auto drained     = drain_chat_stream(client.chat_stream(one_message(), ctx));
        auto reqs        = server.requests();
        check(!reqs.empty() && reqs[0].find(key_header) != std::string::npos,
              "K2 " + v + ": the resolved key reaches the wire");
        check(!drained.ok && drained.failure.klass == failure_class::policy &&
                  drained.failure.code != "secret.not_granted",
              "K2 " + v + ": the server's 401 (not the store) fails the stream (got '" + drained.failure.code + "')");
    }
}

// K3 for one vendor: chat_stream() called on a lane, against a genuinely suspending store.
template <class Make>
void on_lane_with_suspending_store(char const* vendor, Make make, std::string const& key_header) {
    std::string const  v = vendor;
    rt::Runtime        host_rt{rt::RuntimeConfig{.lanes = 1}};
    SuspendingSecretStore store(host_rt.reactor());
    ScriptedHttpServer server({{401, kUnauthorized}});
    detail::provider_wire::ProviderReactorOptions opts;
    opts.runtime = &host_rt;
    auto          client = make(server.port(), store, std::move(opts));
    CapabilitySet held   = CapabilitySet::grant_root({cap::Secret{"key", std::chrono::seconds{0}}});
    EffectContext ctx{.deadline = agentengine::Deadline::none()};
    ctx.principal    = Principal{"p", ""};
    ctx.capabilities = borrow_capabilities(held);

    auto start = [&]() -> task<stream<ChatResponseUpdate>> { co_return client.chat_stream(one_message(), ctx); };
    std::optional<stream<ChatResponseUpdate>> s;
    try {
        s.emplace(host_rt.run(start()));
    } catch (std::exception const& e) {
        check(false, "K3 " + v + ": chat_stream() on a lane threw: " + e.what());
        return;
    }
    auto drained = drain_chat_stream(std::move(*s));
    auto reqs    = server.requests();
    check(store.parks == 1, "K3 " + v + ": the suspending store was awaited once");
    check(!reqs.empty() && reqs[0].find(key_header) != std::string::npos,
          "K3 " + v + ": chat_stream() on a lane reaches the wire after a suspending resolve (stream code '" +
              drained.failure.code + "')");
}

}  // namespace

int main() {
#if defined(_WIN32)
    pal::ensure_winsock();
#endif
    denied_then_granted(
        "openai", [](std::uint16_t port, auto const& s) { return make_openai(port, s); },
        "Authorization: Bearer sk-loopback");
    denied_then_granted(
        "anthropic", [](std::uint16_t port, auto const& s) { return make_anthropic(port, s); },
        "x-api-key: sk-loopback");
    on_lane_with_suspending_store(
        "openai", [](std::uint16_t port, auto const& s, auto r) { return make_openai(port, s, std::move(r)); },
        "Authorization: Bearer sk-loopback");
    on_lane_with_suspending_store(
        "anthropic", [](std::uint16_t port, auto const& s, auto r) { return make_anthropic(port, s, std::move(r)); },
        "x-api-key: sk-loopback");

    if (g_failures != 0) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall checks passed\n");
    return 0;
}
