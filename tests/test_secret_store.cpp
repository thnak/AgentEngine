// Milestone 5 Phase A (docs/planning/milestone-5-providers-identity-secrets-breakdown.md):
// SecretRef/SecretLease/SecretStore (trust/secret.hpp), a capability-gated wrapper over Quark's
// own, already-Accepted SecretSource seam (020-Security §4). Proves: A1/A2 (SecretRef is a name,
// SecretLease is non-copyable and redacted by construction), the fail-closed capability gate every
// backend shares (018 §4's "a native seam backend is held to the identical discipline" rule, not a
// plugin-only one), A3 (AgentEngineSecretStore over quark::EnvSecretSource/FileSecretSource
// resolves real values), and A4 (scoping per name -- a grant for one secret does not authorize
// another -- and rotation without restart -- a changed backing value is visible on the very next
// resolve(), because nothing holds a lease across calls).

#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>

#include "agentengine/trust/secret.hpp"
#include "quark/core/secret.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

EffectContext make_ctx() {
    EffectContext ctx;
    ctx.principal = Principal{"test-principal", ""};
    return ctx;
}

// Structural claims, checked at compile time so losing them fails the build, not a test run.
static_assert(!std::is_copy_constructible_v<SecretLease>,
              "SecretLease must reject copy construction");
static_assert(!std::is_copy_assignable_v<SecretLease>, "SecretLease must reject copy assignment");
static_assert(std::is_move_constructible_v<SecretLease>, "SecretLease must remain move-constructible");

}  // namespace

int main() {
    // ---- A1: SecretRef is a name; SecretLease redacts by construction --------------------------
    {
        InMemorySecretStore store;
        store.set("openai-api-key", "sk-real-value-do-not-print");
        CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"openai-api-key", std::chrono::seconds{0}}});
        auto ctx = make_ctx();
        ctx.capabilities = &held;

        auto lease = store.resolve(SecretRef{"openai-api-key"}, ctx);
        AE_CHECK(lease.has_value(), "A1: resolve() succeeds when the capability is held and the value exists");
        if (lease.has_value()) {
            AE_CHECK(lease->reveal_text() == "sk-real-value-do-not-print",
                      "A1: reveal_text() returns the real underlying value");
            AE_CHECK(lease->ref().name == "openai-api-key", "A1: the lease carries back its own SecretRef");
            AE_CHECK(to_redacted_string(*lease) == "***",
                      "A1: to_redacted_string() never reflects the real value, regardless of what it is");
        }
    }

    // ---- Fail-closed capability gate: no capabilities pointer at all ---------------------------
    {
        InMemorySecretStore store;
        store.set("db-password", "hunter2");
        auto ctx = make_ctx();  // ctx.capabilities left null
        auto lease = store.resolve(SecretRef{"db-password"}, ctx);
        AE_CHECK(!lease.has_value(), "gate: resolve() denies when EffectContext carries no capability set at all");
        if (!lease.has_value()) {
            AE_CHECK(lease.error().code == "secret.not_granted",
                      "gate: denial is classified secret.not_granted, not a generic failure");
        }
    }

    // ---- Fail-closed capability gate: capability set present but empty -------------------------
    {
        InMemorySecretStore store;
        store.set("db-password", "hunter2");
        CapabilitySet empty;
        auto ctx = make_ctx();
        ctx.capabilities = &empty;
        auto lease = store.resolve(SecretRef{"db-password"}, ctx);
        AE_CHECK(!lease.has_value(), "gate: resolve() denies against an empty (but non-null) capability set");
    }

    // ---- A4: scoping per name -- holding Secret<"a"> does not authorize resolving "b" ----------
    {
        InMemorySecretStore store;
        store.set("service-a-key", "value-a");
        store.set("service-b-key", "value-b");
        CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"service-a-key", std::chrono::seconds{0}}});
        auto ctx = make_ctx();
        ctx.capabilities = &held;

        auto allowed = store.resolve(SecretRef{"service-a-key"}, ctx);
        AE_CHECK(allowed.has_value(), "A4: the granted name resolves");

        auto denied = store.resolve(SecretRef{"service-b-key"}, ctx);
        AE_CHECK(!denied.has_value(),
                  "A4: a DIFFERENT secret name is denied even though some Secret capability is held -- "
                  "least privilege per name, not per kind");
    }

    // ---- A4: rotation without restart -- the next resolve() sees a changed backing value -------
    {
        InMemorySecretStore store;
        store.set("rotating-key", "value-v1");
        CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"rotating-key", std::chrono::seconds{0}}});
        auto ctx = make_ctx();
        ctx.capabilities = &held;

        auto first = store.resolve(SecretRef{"rotating-key"}, ctx);
        AE_CHECK(first.has_value() && first->reveal_text() == "value-v1", "A4: first resolve sees v1");

        store.set("rotating-key", "value-v2");  // simulates an external rotation event
        auto second = store.resolve(SecretRef{"rotating-key"}, ctx);
        AE_CHECK(second.has_value() && second->reveal_text() == "value-v2",
                  "A4: the very next resolve() reflects the rotated value -- nothing cached a lease "
                  "across calls to go stale");
    }

    // ---- A3: AgentEngineSecretStore over quark::EnvSecretSource resolves a real env var ---------
    {
#if defined(_WIN32)
        _putenv_s("QUARK_SECRET_test-env-secret", "env-value-123");
#else
        setenv("QUARK_SECRET_test-env-secret", "env-value-123", 1);
#endif
        AgentEngineSecretStore store(std::make_unique<quark::EnvSecretSource>());
        CapabilitySet held = CapabilitySet::grant_root({cap::Secret{"test-env-secret", std::chrono::seconds{0}}});
        auto ctx = make_ctx();
        ctx.capabilities = &held;

        auto lease = store.resolve(SecretRef{"test-env-secret"}, ctx);
        AE_CHECK(lease.has_value() && lease->reveal_text() == "env-value-123",
                  "A3: AgentEngineSecretStore/EnvSecretSource reads QUARK_SECRET_<name>");

        CapabilitySet missing_held =
            CapabilitySet::grant_root({cap::Secret{"never-set-anywhere", std::chrono::seconds{0}}});
        ctx.capabilities = &missing_held;
        auto missing = store.resolve(SecretRef{"never-set-anywhere"}, ctx);
        AE_CHECK(!missing.has_value(), "A3: EnvSecretSource fails closed (not empty-string) when unset");
    }

    // ---- A3: AgentEngineSecretStore over quark::FileSecretSource resolves a real file -----------
    {
        std::string dir = ".";  // CTest's own working directory -- writable, cleaned up by us below
        std::string path = dir + "/ae_test_secret_file_store_secret";
        {
            std::ofstream f(path, std::ios::binary);
            f << "file-value-456\n";
        }
        AgentEngineSecretStore store(std::make_unique<quark::FileSecretSource>(dir));
        CapabilitySet held = CapabilitySet::grant_root(
            {cap::Secret{"ae_test_secret_file_store_secret", std::chrono::seconds{0}}});
        auto ctx = make_ctx();
        ctx.capabilities = &held;

        auto lease = store.resolve(SecretRef{"ae_test_secret_file_store_secret"}, ctx);
        AE_CHECK(lease.has_value() && lease->reveal_text() == "file-value-456",
                  "A3: AgentEngineSecretStore/FileSecretSource reads <root>/<name>, trailing newline stripped");
        std::remove(path.c_str());

        auto missing = store.resolve(SecretRef{"ae_test_secret_file_store_secret"}, ctx);
        AE_CHECK(!missing.has_value(), "A3: FileSecretSource fails closed once the file is gone");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All secret store (018 §4, Milestone 5 Phase A) checks passed.\n";
    return 0;
}
