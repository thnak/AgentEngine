// Milestone 5 Phase B3 (docs/planning/milestone-5-providers-identity-secrets-breakdown.md): proves
// 004 §1's outbound-credential rule -- "a native ChatClient backend is constructed with a
// SecretRef... never a resolved value... resolution happens inside chat()/chat_stream()... at the
// point of use" -- against a reference backend conforming to the real ChatClient concept
// (core/chat_client.hpp) and the real SecretStore seam (trust/secret.hpp, Milestone 5 Phase A).
//
// The proof is behavioral, not merely structural: CredentialedTestChatClient holds only a SecretRef
// (a name) as a member -- never a SecretLease -- and its chat() calls store_.resolve(...) fresh on
// every invocation. Two chat() calls against a rotated backing secret value return DIFFERENT
// resolved values, which is only possible if nothing cached a lease across calls (018 §3's
// rotation-without-restart requirement, extended here from Phase A's direct SecretStore proof to a
// real ChatClient conformer sitting on top of it).

#include <cstdio>
#include <string>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/trust/secret.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

// A minimal, real ChatClient conformer. `api_key_ref_` is the ONLY credential-shaped member --
// there is no SecretLease field anywhere on this type, so a resolved value cannot outlive the
// call that resolved it by construction, not merely by convention.
template <agentengine::SecretStore Store>
class CredentialedTestChatClient {
public:
    CredentialedTestChatClient(Store const& store, agentengine::SecretRef api_key_ref)
        : store_(store), api_key_ref_(std::move(api_key_ref)) {}

    [[nodiscard]] agentengine::ChatClientCapabilities capabilities() const { return {}; }

    [[nodiscard]] agentengine::result<agentengine::ChatResponse> chat(
        agentengine::ChatRequest const&, agentengine::EffectContext& ctx) {
        // Resolution happens HERE, inside chat(), against EffectContext -- never at construction.
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) return std::unexpected(lease.error());

        agentengine::ChatResponse resp;
        agentengine::ContentItem item;
        // Test-only: echoes the resolved credential back so the test can observe which value was
        // actually used per call. A real backend would put it in an Authorization header, never in
        // response content -- this is the test's own observation point, not a pattern to copy.
        item.value = agentengine::Text{lease->reveal_text()};
        resp.message.content.push_back(std::move(item));
        return resp;
    }

    int chat_stream(agentengine::ChatRequest const&, agentengine::EffectContext&) { return 0; }

private:
    Store const&           store_;
    agentengine::SecretRef api_key_ref_;
};

}  // namespace

int main() {
    using namespace agentengine;

    InMemorySecretStore store;
    store.set("provider-api-key", "key-v1");
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"provider-api-key", std::chrono::seconds{0}}});

    CredentialedTestChatClient client(store, SecretRef{"provider-api-key"});
    static_assert(ChatClient<CredentialedTestChatClient<InMemorySecretStore>>,
                  "CredentialedTestChatClient must satisfy the real ChatClient concept (004 §1)");

    EffectContext ctx;
    ctx.principal = Principal{"test-principal", ""};
    ctx.capabilities = &held;

    // ---- first call resolves the current (v1) value -----------------------------------------------
    auto first = client.chat(ChatRequest{}, ctx);
    check(first.has_value(), "chat() succeeds when the Secret capability is held");
    if (first.has_value()) {
        auto const* text = std::get_if<Text>(&first->message.content.at(0).value);
        check(text != nullptr && text->text == "key-v1", "first call resolves the current backing value (v1)");
    }

    // ---- rotate the backing value; NO restart, NO reconstruction of the client --------------------
    store.set("provider-api-key", "key-v2");

    auto second = client.chat(ChatRequest{}, ctx);
    check(second.has_value(), "chat() still succeeds after rotation");
    if (second.has_value()) {
        auto const* text = std::get_if<Text>(&second->message.content.at(0).value);
        check(text != nullptr && text->text == "key-v2",
              "the SECOND call reflects the rotated value -- proves resolution happens fresh inside "
              "chat(), not once at construction, since the client instance itself was never rebuilt "
              "between calls (018 §3 rotation-without-restart, at the ChatClient layer)");
    }

    // ---- revoking the capability mid-lifetime denies the very next call ---------------------------
    CapabilitySet empty;
    ctx.capabilities = &empty;
    auto denied = client.chat(ChatRequest{}, ctx);
    check(!denied.has_value(),
          "with the Secret capability no longer held, the next chat() call is denied -- the gate is "
          "checked per call, not cached from an earlier successful resolution");

    if (g_failures == 0) {
        std::fprintf(stderr, "test_chat_client_credential_resolution: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_chat_client_credential_resolution: %d FAILURE(S)\n", g_failures);
    return 1;
}
