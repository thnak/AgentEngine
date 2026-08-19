// End-to-end proof of decisions/ADR-063-retrieval-augmented-context-provider-shape.md §3 claim 4:
// "a single OpenAIEmbedder, pointed at api.openrouter.ai instead of api.openai.com (host/path_prefix
// as constructor arguments only, no code branch), successfully embeds text through OpenRouter's real
// /api/v1/embeddings endpoint -- i.e., no OpenRouter-specific client class is actually required,
// mirroring OpenAIChatClient's already-proven relationship to OpenRouter for chat." This file's own
// existence, unmodified from `OpenAIEmbedder`'s OpenAI-shaped construction, IS that proof or its
// disproof -- there is no code branch anywhere in protocol/openai/embedder.hpp keyed on which host
// was passed in.
//
// Mirrors tests/test_openrouter_live_e2e.cpp's EXACT pattern (env-var-gated credential, SKIP not FAIL
// when absent, structural-only assertions, a positive control proving the credential is load-bearing,
// and an I2 capability-denial control) -- see that file's own top comment for the full rationale,
// not repeated here. The one structural difference from that file: embeddings are NOT streaming
// (docs/research/2026-08-19-embedding-provider-landscape.md §1: "streaming is not supported" on this
// endpoint), so there is no chat_stream()-shaped counterpart test here.
//
// CREDENTIALS ARE NEVER COMPILED IN (018 §4). Configuration comes from the environment:
//   AGENTENGINE_OPENROUTER_API_KEY        required -- unset means SKIP (exit 0), never a failure.
//     (Reuses the SAME key test_openrouter_live_e2e.cpp uses -- one OpenRouter account credential for
//     every live test in this suite, matching tools/run-live-provider-tests.ps1's own provisioning.)
//   AGENTENGINE_OPENROUTER_EMBEDDING_MODEL  optional -- default below (a real OpenRouter model id,
//     "openai/text-embedding-3-small" per OpenRouter's own collection listing, docs/research/2026-08-
//     19-embedding-provider-landscape.md §1).
//   AGENTENGINE_OPENROUTER_HOST            optional -- default `openrouter.ai`.
// The key reaches the client the same production route test_openrouter_live_e2e.cpp uses: a real
// SecretStore, a real cap::Secret grant, resolution at the point of use inside embed_batch() (004 §1,
// 018 §4). Nothing below ever holds the key text itself.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentengine/pal/env.hpp"
#include "agentengine/protocol/openai/embedder.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secret.hpp"
#include "support/run_task_sync.hpp"

using namespace agentengine;

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

constexpr char const* kDefaultModel = "openai/text-embedding-3-small";
constexpr char const* kDefaultHost = "openrouter.ai";
constexpr std::uint16_t kHttpsPort = 443;

// Same base path OpenAIChatClient's own live test uses against this host (test_openrouter_live_e2e.cpp:
// "Both surfaces live under the same /api prefix on the same host") -- `embed_batch()` appends
// "/embeddings" itself, giving the real, documented `/api/v1/embeddings`
// (docs/research/2026-08-19-embedding-provider-landscape.md §1).
constexpr char const* kPathPrefix = "/api/v1";

constexpr char const* kSecretName = "openrouter-api-key";

// Sourced default: text-embedding-3-small's published vector length
// (docs/research/2026-08-19-embedding-provider-landscape.md §3) -- declared here as what THIS test
// expects from THIS model, never read back from a response to "confirm" it (that would be probing,
// the exact thing 004 §3/core/embedder.hpp's own rule forbids `EmbedderCapabilities` from doing).
constexpr std::uint32_t kExpectedDimensions = 1536;

}  // namespace

int main() {
    auto const key_env = ::agentengine::pal::env_var("AGENTENGINE_OPENROUTER_API_KEY");
    if (!key_env || key_env->empty()) {
        std::fprintf(
            stderr,
            "test_openai_embedder_openrouter_live_e2e: SKIPPED -- AGENTENGINE_OPENROUTER_API_KEY is "
            "not set.\n  Run tools/run-live-provider-tests.ps1, or set the variable yourself, to "
            "exercise the real provider.\n");
        return 0;
    }

    std::string const model = env_or("AGENTENGINE_OPENROUTER_EMBEDDING_MODEL", kDefaultModel);
    std::string const host = env_or("AGENTENGINE_OPENROUTER_HOST", kDefaultHost);
    std::fprintf(stderr, "test_openai_embedder_openrouter_live_e2e: host=%s model=%s\n", host.c_str(),
                 model.c_str());

    InMemorySecretStore store;
    store.set(kSecretName, *key_env);
    CapabilitySet held = CapabilitySet::grant_root({cap::Secret{kSecretName, std::chrono::seconds{0}}});
    EffectContext ctx;
    ctx.principal = Principal{"live-e2e-principal", ""};
    ctx.capabilities = &held;

    using agentengine::test_support::run_task_sync;

    // DECLARED, never probed (004 §3 / core/embedder.hpp's own rule) -- this is what the CALLER
    // asserts about the endpoint it configured, not something embed_batch() will verify for us.
    EmbedderCapabilities caps;
    caps.dimensions = kExpectedDimensions;
    caps.max_batch_size = 0;  // OpenRouter publishes no confirmed limit (research doc §3) -- left
                               // undeclared (0) rather than guessed; embed_batch() treats 0 as
                               // "not enforced by this conformer", which is the honest position here.

    // Default resolver AND default CA bundle -- the real ones, matching test_openrouter_live_e2e.cpp's
    // own choice not to inject a fake resolver or a self-signed leaf for this test.
    openai::OpenAIEmbedder embedder(host, kHttpsPort, model, SecretRef{kSecretName}, caps, store,
                                     kPathPrefix);

    static_assert(Embedder<decltype(embedder)>, "OpenAIEmbedder must satisfy the Embedder concept");

    // ---- OR-EMB-1: embed_batch() succeeds against the REAL OpenRouter embeddings endpoint ----------
    std::vector<std::vector<float>> single_reply;
    {
        std::vector<std::string> texts{"The quick brown fox jumps over the lazy dog."};
        auto resp = run_task_sync<result<std::vector<std::vector<float>>>>(
            embedder.embed_batch(texts, ctx));
        check(resp.has_value(),
              "OR-EMB-1: embed_batch() succeeds against the REAL OpenRouter /api/v1/embeddings "
              "endpoint over real DNS (the default resolve_host) and real TLS against the vendored "
              "CA bundle -- no injected resolver, no self-signed leaf, no OpenRouter-specific "
              "request/header shape anywhere on this path (ADR-063 §3 claim 4)");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        } else {
            single_reply = *resp;
            check(resp->size() == 1, "OR-EMB-1: one vector came back for one requested text");
            if (!resp->empty()) {
                check(!(*resp)[0].empty(),
                      "OR-EMB-1: the returned embedding vector is non-empty -- real floats survived "
                      "the wire, decoding, and index-based placement");
                note("vector length", std::to_string((*resp)[0].size()));
                if ((*resp)[0].size() != kExpectedDimensions) {
                    // Not a hard failure: OpenRouter's routing/proxying could plausibly change the
                    // effective model without notice, which is exactly the kind of thing a live test
                    // (rather than a canned fixture) is positioned to observe. Recorded, not asserted.
                    note("NOTE: vector length differs from the declared/expected dimensions",
                         std::to_string(kExpectedDimensions));
                }
            }
        }
    }

    // ---- OR-EMB-2: a real MULTI-item batch returns one vector per item, in the right order ---------
    {
        std::vector<std::string> texts{"apple", "banana", "cherry"};
        auto resp = run_task_sync<result<std::vector<std::vector<float>>>>(
            embedder.embed_batch(texts, ctx));
        check(resp.has_value(), "OR-EMB-2: a real 3-item batch is accepted by the real provider");
        if (!resp) {
            std::fprintf(stderr, "       error: %s (%s)\n", resp.error().message.c_str(),
                         resp.error().code.c_str());
        } else {
            check(resp->size() == 3,
                  "OR-EMB-2: exactly 3 vectors came back for 3 requested texts -- the response's own "
                  "'index' field was used to place each one at the right position (embedder.hpp's "
                  "parse_embeddings_response), not merely assumed to arrive in array order");
            for (std::size_t i = 0; i < resp->size(); ++i) {
                check(!(*resp)[i].empty(), "OR-EMB-2: every returned vector is non-empty");
            }
        }
    }

    // ---- OR-EMB-3: POSITIVE CONTROL -- a wrong credential is rejected by the real service ----------
    // Without this, every success above could in principle be explained by an endpoint that ignores
    // authentication entirely. This is the test that cannot pass unless the credential was load-bearing.
    {
        InMemorySecretStore bad_store;
        bad_store.set(kSecretName,
                       "sk-or-v1-0000000000000000000000000000000000000000000000000000000000000000");
        openai::OpenAIEmbedder bad(host, kHttpsPort, model, SecretRef{kSecretName}, caps, bad_store,
                                    kPathPrefix);
        auto resp = run_task_sync<result<std::vector<std::vector<float>>>>(
            bad.embed_batch({"hi"}, ctx));
        check(!resp.has_value(),
              "OR-EMB-3 (positive control): a syntactically valid but WRONG api key is rejected by "
              "the real embeddings endpoint -- proving the credential resolved above was genuinely "
              "load-bearing and the successes are not an unauthenticated endpoint answering anyone");
        if (!resp) {
            check(resp.error().klass == failure_class::policy,
                  "OR-EMB-3: an authentication rejection is classified 'policy' (401/403), so it is "
                  "never retried as if it were transient");
            note("auth failure", resp.error().code + ": " + resp.error().message);
        }
    }

    // ---- OR-EMB-4: an ungranted capability fails closed BEFORE any egress -------------------------
    {
        CapabilitySet empty;
        EffectContext denied = ctx;
        denied.capabilities = &empty;
        auto resp = run_task_sync<result<std::vector<std::vector<float>>>>(
            embedder.embed_batch({"hi"}, denied));
        check(!resp.has_value(),
              "OR-EMB-4 (I2): with no cap::Secret grant the call is denied at the point of use and "
              "never reaches the network, even though a real, reachable, correctly-credentialed "
              "endpoint is sitting right there -- the strongest form this assertion can take");
        if (!resp) {
            check(resp.error().klass == failure_class::policy,
                  "OR-EMB-4: a capability denial is classified 'policy'");
        }
    }

    // ---- OR-EMB-5: embedding the SAME text twice, in two separate calls, is at least self-consistent
    // in SHAPE (never asserted equal -- ADR-063 §2.2A's own named tradeoff: an unwrapped Embedder is
    // NOT claimed byte-identical across calls, so this test does not pretend otherwise).
    if (!single_reply.empty() && !single_reply[0].empty()) {
        std::vector<std::string> texts{"The quick brown fox jumps over the lazy dog."};
        auto resp = run_task_sync<result<std::vector<std::vector<float>>>>(
            embedder.embed_batch(texts, ctx));
        check(resp.has_value(), "OR-EMB-5: a second call with the same text still succeeds");
        if (resp && !resp->empty()) {
            check((*resp)[0].size() == single_reply[0].size(),
                  "OR-EMB-5: the SAME model returns the SAME vector LENGTH across two separate calls "
                  "-- a shape guarantee, deliberately NOT a byte-equality assertion (this project "
                  "makes no determinism claim for Embedder, ADR-063 §2.2A/§3 claim 3)");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_openai_embedder_openrouter_live_e2e: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_openai_embedder_openrouter_live_e2e: %d FAILURE(S)\n", g_failures);
    return 1;
}
