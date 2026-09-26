// End-to-end proof of decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §3 claim 6's
// live half: "QdrantVectorIndex reaches a real Qdrant instance and round-trips add_batch -> search ->
// contains correctly." Mirrors tests/protocol/openai/test_openai_embedder_openrouter_live_e2e.cpp's EXACT pattern
// (env-var-gated credential/host, SKIP not FAIL when unset, an auth positive control, an I2
// capability-denial control) -- see that file's own top comment for the full rationale, not repeated
// here. The offline-provable half (request/response shape, point-id derivation, error parsing) is
// tests/core/rag/test_qdrant_vector_index.cpp -- this file exists ONLY to prove the real wire round-trip a
// mocked/canned test cannot.
//
// CREDENTIALS ARE NEVER COMPILED IN (018 §4). Configuration comes from the environment:
//   AGENTENGINE_QDRANT_HOST       required -- unset means SKIP (exit 0), never a failure.
//   AGENTENGINE_QDRANT_API_KEY    required -- unset means SKIP (exit 0), never a failure.
//   AGENTENGINE_QDRANT_PORT       optional -- default 6333 (Qdrant's own documented default REST port).
//   AGENTENGINE_QDRANT_COLLECTION optional -- default "ae_qdrant_live_e2e_test". MUST already exist
//     with a matching vector size (this test does not create collections -- that's a real, separate
//     Qdrant admin operation, `PUT /collections/{name}`, out of THIS conformer's own designed surface,
//     ADR-180 §2.5 never names collection management as part of VectorIndex's job).
//   AGENTENGINE_QDRANT_TRANSPORT  optional -- "tls" (default) or "plaintext_http" for a local/loopback
//     instance with no certificate (matches ADR-016's own opt-in-plaintext posture, provider_http_
//     client.hpp's ProviderTransport::plaintext_http -- e.g. `docker run -p 6333:6333 qdrant/qdrant`
//     serves plain HTTP by default with no TLS at all).
//
// A local `docker run -e QDRANT__SERVICE__API_KEY=<key> -p 6333:6333 qdrant/qdrant` is the natural
// target (matching how OpenAIEmbedder's own live test uses a real hosted provider) -- set
// AGENTENGINE_QDRANT_HOST=127.0.0.1, AGENTENGINE_QDRANT_TRANSPORT=plaintext_http, and
// AGENTENGINE_QDRANT_API_KEY to whatever the container was started with. The collection must be
// created first: `curl -X PUT http://127.0.0.1:6333/collections/ae_qdrant_live_e2e_test -H "api-key:
// <key>" -H "Content-Type: application/json" -d '{"vectors":{"size":3,"distance":"Cosine"}}'`.

#ifdef AGENTENGINE_WITH_HTTPS

#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/qdrant/vector_index.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "../../support/run_task_sync.hpp"

using namespace agentengine;
using namespace agentengine::qdrant;

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

void note(char const* label, std::string const& value) {
    std::fprintf(stderr, "  .. %s = %s\n", label, value.c_str());
}

[[nodiscard]] std::string env_or(char const* name, std::string fallback) {
    auto const v = ::agentengine::pal::env_var(name);
    return (v && !v->empty()) ? *v : std::move(fallback);
}

constexpr char const* kSecretName = "qdrant-live-e2e-api-key";
constexpr std::uint16_t kDefaultPort = 6333;
constexpr char const* kDefaultCollection = "ae_qdrant_live_e2e_test";

// A real, 64-lowercase-hex-char digest shape (Digest's own documented contract,
// worktree_types.hpp) -- NOT computed via compute_digest() (this file avoids the worktree_store
// link dependency entirely; the exact bytes chosen don't matter, only that the shape is valid).
[[nodiscard]] std::string fake_digest(char fill) {
    return std::string(64, fill);
}

}  // namespace

int main() {
    auto const host_env = ::agentengine::pal::env_var("AGENTENGINE_QDRANT_HOST");
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_QDRANT_API_KEY");
    if (!host_env || host_env->empty() || !key_env || key_env->empty()) {
        std::fprintf(stderr,
                      "test_qdrant_vector_index_live_e2e: SKIPPED -- "
                      "AGENTENGINE_QDRANT_HOST/AGENTENGINE_QDRANT_API_KEY not set.\n  Start a local "
                      "Qdrant (see this file's own top comment) to exercise the real provider.\n");
        return 0;
    }

    std::string const host = *host_env;
    std::uint16_t const port =
        static_cast<std::uint16_t>(std::stoi(env_or("AGENTENGINE_QDRANT_PORT", std::to_string(kDefaultPort))));
    std::string const collection = env_or("AGENTENGINE_QDRANT_COLLECTION", kDefaultCollection);
    std::string const transport_str = env_or("AGENTENGINE_QDRANT_TRANSPORT", "tls");
    sandbox::ProviderTransport const transport = transport_str == "plaintext_http"
                                                      ? sandbox::ProviderTransport::plaintext_http
                                                      : sandbox::ProviderTransport::tls;
    std::fprintf(stderr, "test_qdrant_vector_index_live_e2e: host=%s port=%u collection=%s transport=%s\n",
                 host.c_str(), static_cast<unsigned>(port), collection.c_str(), transport_str.c_str());

    InMemorySecretStore store;
    store.set(kSecretName, *key_env);
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal = Principal{"live-e2e-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    using agentengine::test_support::run_task_sync;

    QdrantVectorIndex index(host, port, collection, SecretRef{kSecretName}, store, /*path_prefix=*/"",
                             /*max_batch_size=*/0, sandbox::resolve_host, /*ca_bundle_pem_override=*/{},
                             transport);
    static_assert(RemoteVectorIndex<decltype(index)>, "QdrantVectorIndex must satisfy RemoteVectorIndex");

    std::string const digest_a = fake_digest('a');
    std::string const digest_b = fake_digest('b');

    // ---- QD-1: add_batch() succeeds against the REAL Qdrant instance -----------------------------
    {
        auto resp = run_task_sync<result<void>>(
            index.add_batch({digest_a, digest_b}, {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, ctx));
        check(resp.has_value(),
              "QD-1: add_batch() succeeds against the REAL Qdrant /collections/{name}/points endpoint "
              "(ADR-180 §3 claim 6) -- 2 points, upserted for real over the wire");
        if (!resp) std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                                 resp.error().code.c_str());
    }

    // ---- QD-2: search() finds the just-added point closest to the query, by real cosine similarity -
    {
        float const query[3] = {1.0f, 0.0f, 0.0f};  // matches digest_a's own vector exactly
        auto resp = run_task_sync<result<std::vector<ScoredId>>>(
            index.search(std::span<float const>(query, 3), /*k=*/2, ctx));
        check(resp.has_value(), "QD-2: search() succeeds against the real instance");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        } else {
            check(resp->size() == 2, "QD-2: both previously-added points come back for k=2");
            check(!resp->empty() && resp->front().id == digest_a,
                  "QD-2: the point whose vector matches the query exactly ranks first, AND its "
                  "returned id is the REAL digest_a (from payload.chunk_id) -- not Qdrant's own "
                  "internal UUID point id -- proving the id-derivation/payload round-trip works "
                  "for real, not just against canned JSON");
            if (!resp->empty()) note("top score", std::to_string(resp->front().score));
        }
    }

    // ---- QD-3: contains() reports true for an added id, false for one never added -----------------
    {
        auto found = run_task_sync<result<bool>>(index.contains(digest_a, ctx));
        check(found.has_value() && *found,
              "QD-3: contains() reports true for a real, previously-added digest");

        std::string const never_added = fake_digest('c');
        auto missing = run_task_sync<result<bool>>(index.contains(never_added, ctx));
        check(missing.has_value() && !*missing,
              "QD-3: contains() reports false (not an error) for a digest that was never added -- "
              "the real HTTP 404-from-Qdrant path, not just the offline test's canned status code "
              "(docs/research/2026-09-22-qdrant-rest-api.md §1)");
    }

    // ---- QD-4: k=0 short-circuits without a network call even against the real instance -----------
    // (ADR-180 §4 red-team pass 2 finding 3 -- proven offline via a stub transport already; this is
    // the live counterpart, proving it holds against a real host too, not just a mock.)
    {
        float const query[3] = {1.0f, 0.0f, 0.0f};
        auto resp = run_task_sync<result<std::vector<ScoredId>>>(
            index.search(std::span<float const>(query, 3), /*k=*/0, ctx));
        check(resp.has_value() && resp->empty(), "QD-4: search(k=0) returns empty, even live");
    }

    // ---- QD-5: POSITIVE CONTROL -- a wrong api-key is rejected by the real service -----------------
    // Without this, every success above could in principle be explained by a Qdrant instance that
    // ignores authentication entirely (a real possibility -- Qdrant runs with NO auth by default
    // unless QDRANT__SERVICE__API_KEY is set).
    {
        InMemorySecretStore bad_store;
        bad_store.set(kSecretName, "wrong-key-0000000000000000000000000000");
        QdrantVectorIndex bad(host, port, collection, SecretRef{kSecretName}, bad_store, "", 0,
                               sandbox::resolve_host, {}, transport);
        auto resp = run_task_sync<result<bool>>(bad.contains(digest_a, ctx));
        check(!resp.has_value(),
              "QD-5 (positive control): a wrong api-key is rejected by the real service -- proving "
              "the credential resolved above was genuinely load-bearing, not an unauthenticated "
              "endpoint answering anyone. NOTE: if this FAILS, the most likely cause is the target "
              "Qdrant instance has no API key configured at all (started without "
              "QDRANT__SERVICE__API_KEY) -- that is a real, meaningful finding about the test "
              "environment, not a bug in QdrantVectorIndex, and should be reported as such.");
        if (!resp) {
            check(resp.error().klass == failure_class::policy,
                  "QD-5: an authentication rejection is classified 'policy' (401/403)");
            note("auth failure", resp.error().code + ": " + resp.error().message);
        }
    }

    // ---- QD-6: an ungranted capability fails closed BEFORE any egress ------------------------------
    {
        CapabilitySet empty;
        EffectContext denied = ctx;
        denied.capabilities = agentengine::borrow_capabilities(empty);
        auto resp = run_task_sync<result<bool>>(index.contains(digest_a, denied));
        check(!resp.has_value(),
              "QD-6 (I2): with no cap::Secret grant, contains() is denied at the point of use and "
              "never reaches the network, even though a real, reachable, correctly-credentialed "
              "instance is sitting right there");
        if (!resp) check(resp.error().klass == failure_class::policy, "QD-6: classified 'policy'");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_qdrant_vector_index_live_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_qdrant_vector_index_live_e2e: %d FAILURE(S)\n", g_failures);
    return 1;
}

#else

int main() {
    return 0;  // AGENTENGINE_WITH_HTTPS is OFF -- nothing to exercise.
}

#endif  // AGENTENGINE_WITH_HTTPS
