// Milestone-parallel work on decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.5:
// proves protocol/openai/embedder.hpp's OFFLINE-provable surface -- request-body construction,
// response parsing (including the out-of-order/partial/malformed shapes a real provider could send),
// and the two synchronous gates `embed_batch()` applies BEFORE any network attempt (a declared
// `max_batch_size` overflow, and the `SecretStore`/`EffectContext` credential-resolution discipline
// itself) -- with no network, no live server, so this runs unconditionally fast and offline, mirroring
// test_openai_chat_client_translation.cpp's own "no server involved" split from its companion live
// test (that file's own top comment: "this file is pure parsing/serialization logic, deliberately
// with no server involved").
//
// tests/test_openai_embedder_openrouter_live_e2e.cpp (a separate file) proves the REST of the path --
// the actual HTTPS exchange against real api.openrouter.ai -- and is this project's ADR-063 §3 claim 4
// disproof test; this file does not attempt that, by design.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/protocol/openai/embedder.hpp"
#include "agentengine/trust/principal.hpp"
#include "support/run_task_sync.hpp"

using namespace agentengine;
using namespace agentengine::openai;
using namespace agentengine::openai::detail;

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

// Hand-built to match the real wire shape confirmed in docs/research/2026-08-19-embedding-provider-
// landscape.md §3 (fetched directly from OpenAI's current API reference):
//   {"object":"list","data":[{"object":"embedding","index":0,"embedding":[...]},...],"model":"...",
//    "usage":{"prompt_tokens":N,"total_tokens":N}}
// `indices_in_order` lets a test scenario deliberately return `data[]` items OUT of request order
// (a real provider is not contractually bound to answer index-ascending, and `index` exists on the
// wire precisely so a caller does not have to assume it does).
[[nodiscard]] std::string canned_response_json(std::vector<std::pair<int, std::vector<float>>> const& items) {
    std::string out = R"({"object":"list","data":[)";
    bool first = true;
    for (auto const& [index, vec] : items) {
        if (!first) out += ',';
        first = false;
        out += R"({"object":"embedding","index":)" + std::to_string(index) + R"(,"embedding":[)";
        for (std::size_t i = 0; i < vec.size(); ++i) {
            if (i) out += ',';
            out += std::to_string(vec[i]);
        }
        out += "]}";
    }
    out += R"(],"model":"text-embedding-3-small","usage":{"prompt_tokens":9,"total_tokens":9}})";
    return out;
}

}  // namespace

int main() {
    // ---- request-body construction --------------------------------------------------------------
    {
        std::vector<std::string> texts{"hello world", "second chunk"};
        json::Value body = build_embeddings_request_body(texts, "text-embedding-3-small");
        check(body.is_object(), "REQ-1: the request body is a JSON object");

        auto const* model = body.find("model");
        check(model && model->is_string() && model->as_string() == "text-embedding-3-small",
              "REQ-1: 'model' round-trips exactly");

        auto const* input = body.find("input");
        check(input && input->is_array() && input->as_array().size() == 2,
              "REQ-1: 'input' is a JSON array with one entry per requested text -- ADR-063 §2.5's own "
              "{input:[...], model} shape, and always an ARRAY even though the wire format also "
              "permits a bare string, matching embed_batch()'s always-a-batch contract");
        if (input && input->as_array().size() == 2) {
            check(input->as_array()[0].is_string() && input->as_array()[0].as_string() == "hello world",
                  "REQ-1: input[0] round-trips");
            check(input->as_array()[1].is_string() && input->as_array()[1].as_string() == "second chunk",
                  "REQ-1: input[1] round-trips");
        }

        check(body.find("dimensions") == nullptr && body.find("encoding_format") == nullptr,
              "REQ-1: no 'dimensions'/'encoding_format' field is sent -- this conformer asks for "
              "neither, and omitting an optional field is the safe default over guessing a value");
    }

    // ---- request-body construction: a single-text batch is STILL an array ------------------------
    {
        std::vector<std::string> texts{"solo"};
        json::Value body = build_embeddings_request_body(texts, "m");
        auto const* input = body.find("input");
        check(input && input->is_array() && input->as_array().size() == 1,
              "REQ-2: a one-element batch is still sent as a one-element ARRAY, never collapsed to "
              "the wire format's alternative bare-string form");
    }

    // ---- HTTP request construction ------------------------------------------------------------------
    {
        sandbox::NetEgressRequest req =
            build_embeddings_http_request("/v1/embeddings", "sk-test-key", R"({"a":1})");
        check(req.method == "POST", "HTTP-1: method is POST");
        check(req.path == "/v1/embeddings", "HTTP-1: path round-trips exactly as constructed");
        check(req.body == R"({"a":1})", "HTTP-1: body round-trips exactly");
        bool saw_auth = false;
        bool saw_content_type = false;
        for (auto const& [k, v] : req.headers) {
            if (k == "Authorization") {
                saw_auth = (v == "Bearer sk-test-key");
            } else if (k == "Content-Type") {
                saw_content_type = (v == "application/json");
            }
        }
        check(saw_auth, "HTTP-1: Authorization: Bearer <key> header is set from the resolved credential");
        check(saw_content_type, "HTTP-1: Content-Type: application/json header is set");
        check(req.headers.size() == 2,
              "HTTP-1: exactly two headers -- no HTTP-Referer/X-Title (this class carries neither, "
              "unlike OpenAIChatClient -- see embedder.hpp's own top-comment rationale)");
    }

    // ---- response parsing: the ordinary in-order case -------------------------------------------
    {
        std::string body_text = canned_response_json({{0, {0.1f, 0.2f}}, {1, {0.3f, 0.4f, 0.5f}}});
        auto parsed = json::parse(body_text);
        check(parsed.has_value(), "RESP-1: the canned fixture itself is valid JSON");
        if (parsed) {
            auto result = parse_embeddings_response(*parsed, /*expected_count=*/2);
            check(result.has_value(), "RESP-1: a well-formed, in-order response parses successfully");
            if (result) {
                check(result->size() == 2, "RESP-1: one vector per requested input");
                check((*result)[0] == std::vector<float>{0.1f, 0.2f}, "RESP-1: vector[0] round-trips");
                check((*result)[1] == std::vector<float>{0.3f, 0.4f, 0.5f}, "RESP-1: vector[1] round-trips");
            }
        }
    }

    // ---- response parsing: OUT-of-order data[] is placed by 'index', not array position ----------
    {
        // A provider is not contractually bound to answer index-ascending -- deliberately reversed.
        std::string body_text = canned_response_json({{1, {9.0f}}, {0, {1.0f}}});
        auto parsed = json::parse(body_text);
        check(parsed.has_value(), "RESP-2: fixture parses as JSON");
        if (parsed) {
            auto result = parse_embeddings_response(*parsed, /*expected_count=*/2);
            check(result.has_value(), "RESP-2: an out-of-order response still parses successfully");
            if (result) {
                check((*result)[0] == std::vector<float>{1.0f},
                      "RESP-2: vector at request position 0 is the item whose OWN 'index' field said "
                      "0, not the item that happened to appear first in the 'data' array -- getting "
                      "this wrong would silently mis-align a chunk's text with a different chunk's "
                      "vector");
                check((*result)[1] == std::vector<float>{9.0f}, "RESP-2: vector at position 1 likewise");
            }
        }
    }

    // ---- response parsing: an incomplete response (fewer items than requested) is a hard failure ---
    {
        std::string body_text = canned_response_json({{0, {1.0f}}});  // only 1 of 2 requested
        auto parsed = json::parse(body_text);
        check(parsed.has_value(), "RESP-3: fixture parses as JSON");
        if (parsed) {
            auto result = parse_embeddings_response(*parsed, /*expected_count=*/2);
            check(!result.has_value(),
                  "RESP-3: a response covering fewer indices than requested is rejected outright -- "
                  "never silently accepted as a short-but-valid result");
            if (!result) {
                check(result.error().klass == failure_class::contract,
                      "RESP-3: classified 'contract' -- the response does not honor the request's own "
                      "shape, not a transient/network condition");
                check(result.error().code == "openai_embedder.incomplete_response",
                      "RESP-3: the specific incomplete-response error code is reported");
            }
        }
    }

    // ---- response parsing: an out-of-range index is rejected ---------------------------------------
    {
        std::string body_text = canned_response_json({{5, {1.0f}}});  // index 5, but only 1 requested
        auto parsed = json::parse(body_text);
        if (parsed) {
            auto result = parse_embeddings_response(*parsed, /*expected_count=*/1);
            check(!result.has_value(), "RESP-4: an index outside the request's own range is rejected");
            if (!result) {
                check(result.error().code == "openai_embedder.index_out_of_range",
                      "RESP-4: the specific out-of-range error code is reported");
            }
        }
    }

    // ---- response parsing: a malformed item (missing 'embedding') is rejected ----------------------
    {
        auto parsed = json::parse(R"({"object":"list","data":[{"object":"embedding","index":0}]})");
        check(parsed.has_value(), "RESP-5: fixture parses as JSON");
        if (parsed) {
            auto result = parse_embeddings_response(*parsed, /*expected_count=*/1);
            check(!result.has_value(), "RESP-5: a data[] item missing 'embedding' is rejected, not "
                                        "silently treated as an empty vector");
            if (!result) {
                check(result.error().code == "openai_embedder.malformed_item",
                      "RESP-5: the specific malformed-item error code is reported");
            }
        }
    }

    // ---- response parsing: a non-numeric embedding element is rejected -----------------------------
    {
        auto parsed = json::parse(
            R"({"object":"list","data":[{"object":"embedding","index":0,"embedding":[0.1,"oops",0.3]}]})");
        check(parsed.has_value(), "RESP-6: fixture parses as JSON");
        if (parsed) {
            auto result = parse_embeddings_response(*parsed, /*expected_count=*/1);
            check(!result.has_value(), "RESP-6: a non-numeric element inside 'embedding' is rejected");
            if (!result) {
                check(result.error().code == "openai_embedder.malformed_vector",
                      "RESP-6: the specific malformed-vector error code is reported");
            }
        }
    }

    // ---- response parsing: a top-level 'error' envelope (a lenient proxy answering 200) ------------
    {
        auto parsed = json::parse(R"({"error":{"message":"model not found","type":"invalid_request"}})");
        check(parsed.has_value(), "RESP-7: fixture parses as JSON");
        if (parsed) {
            auto result = parse_embeddings_response(*parsed, /*expected_count=*/1);
            check(!result.has_value(),
                  "RESP-7: a top-level 'error' object is surfaced as a failure even without a "
                  "corresponding non-2xx status -- defends against a lenient proxy answering 200 with "
                  "an error envelope, mirroring parse_chat_completion_response's own defensive check");
            if (!result) {
                check(result.error().message.find("model not found") != std::string::npos,
                      "RESP-7: the provider's own error message text is preserved");
            }
        }
    }

    // ---- response parsing: no 'data' array at all ---------------------------------------------------
    {
        auto parsed = json::parse(R"({"object":"list"})");
        if (parsed) {
            auto result = parse_embeddings_response(*parsed, /*expected_count=*/1);
            check(!result.has_value(), "RESP-8: a response with no 'data' array at all is rejected");
            if (!result) {
                check(result.error().code == "openai_embedder.no_data",
                      "RESP-8: the specific no-data error code is reported");
            }
        }
    }

    // ================= embed_batch() -- offline-provable gates (no network reached) =================
    using agentengine::test_support::run_task_sync;

    // ---- EB-1: an EMPTY batch performs no effect at all -- no network, no credential resolution ----
    {
        InMemorySecretStore store;  // deliberately NEVER populated with a value for this ref
        EmbedderCapabilities caps;
        caps.dimensions = 1536;
        caps.max_batch_size = 8;
        OpenAIEmbedder embedder("unreachable.invalid", 443, "text-embedding-3-small",
                                 SecretRef{"never-resolved"}, caps, store);

        EffectContext ctx;  // no capabilities granted at all -- ctx.capabilities is null
        ctx.principal = Principal{"test-principal", ""};

        auto outcome = run_task_sync<result<std::vector<std::vector<float>>>>(
            embedder.embed_batch({}, ctx));
        check(outcome.has_value(),
              "EB-1: embed_batch({}) succeeds even with NO granted capability and an UNREACHABLE "
              "host -- proof that a zero-item batch never resolves a credential and never attempts "
              "network egress (I2: a capability genuinely unused is never even checked)");
        if (outcome) {
            check(outcome->empty(), "EB-1: the result is an empty vector, not a fabricated placeholder");
        }
    }

    // ---- EB-2: exceeding a DECLARED max_batch_size is rejected BEFORE any credential resolution ----
    {
        InMemorySecretStore store;  // deliberately never populated -- resolution must never be reached
        EmbedderCapabilities caps;
        caps.max_batch_size = 2;  // declared, deliberately small
        OpenAIEmbedder embedder("unreachable.invalid", 443, "m", SecretRef{"never-resolved"}, caps,
                                 store);

        CapabilitySet held =
            CapabilitySet::grant_root({cap::Secret{"never-resolved", std::chrono::seconds{0}}});
        EffectContext ctx;
        ctx.principal = Principal{"test-principal", ""};
        ctx.capabilities = &held;  // the capability IS granted -- proves the batch-size gate fires
                                    // first, independent of whether resolution would have succeeded

        auto outcome = run_task_sync<result<std::vector<std::vector<float>>>>(
            embedder.embed_batch({"a", "b", "c"}, ctx));  // 3 > declared max_batch_size of 2
        check(!outcome.has_value(),
              "EB-2: a batch larger than the declared max_batch_size is rejected -- and since "
              "`store` was never given a value for 'never-resolved', reaching store_.resolve() would "
              "have thrown/failed differently (InMemorySecretStore::resolve returns 'not found', a "
              "DIFFERENT error code) -- the actual error code below proves the SIZE gate fired, not "
              "a downstream credential failure masquerading as one");
        if (!outcome) {
            check(outcome.error().klass == failure_class::contract,
                  "EB-2: classified 'contract' -- this is a caller-side request-shape problem, never "
                  "policy/transient");
            check(outcome.error().code == "openai_embedder.batch_too_large",
                  "EB-2: the specific batch-too-large error code is reported, proving the SIZE gate "
                  "(not credential resolution, which would report 'secret.not_found') is what fired");
        }
    }

    // ---- EB-3: with NO max_batch_size declared (0 = unbounded), a large batch is NOT rejected by ---
    // ---- size -- it proceeds to credential resolution, which then fails for a DIFFERENT reason -----
    {
        InMemorySecretStore store;
        store.set("granted-but-unreachable", "sk-does-not-matter");
        EmbedderCapabilities caps;  // max_batch_size left at its default (0) -- "not declared"
        OpenAIEmbedder embedder("unreachable.invalid", 443, "m", SecretRef{"granted-but-unreachable"},
                                 caps, store);

        EffectContext ctx;  // NO capability granted
        ctx.principal = Principal{"test-principal", ""};

        auto outcome = run_task_sync<result<std::vector<std::vector<float>>>>(
            embedder.embed_batch({"a", "b", "c", "d", "e"}, ctx));
        check(!outcome.has_value(),
              "EB-3: with max_batch_size undeclared (0), a 5-item batch is NOT rejected for size -- "
              "it reaches store_.resolve(), which fails because the capability was never granted");
        if (!outcome) {
            check(outcome.error().klass == failure_class::policy,
                  "EB-3 (I2): the failure is classified 'policy' (an ungranted cap::Secret), NOT "
                  "'contract' -- proving this is the credential gate, not the size gate, that fired "
                  "this time, and that the gate is checked at the point of use (inside embed_batch()) "
                  "before any network attempt is even constructed");
        }
    }

    // ---- EB-4: capabilities() reports exactly what was declared at construction, never probed ------
    {
        InMemorySecretStore store;
        EmbedderCapabilities caps;
        caps.dimensions = 3072;
        caps.max_batch_size = 2048;
        OpenAIEmbedder embedder("host.invalid", 443, "m", SecretRef{"x"}, caps, store);
        EmbedderCapabilities reported = embedder.capabilities();
        check(reported.dimensions == 3072 && reported.max_batch_size == 2048,
              "EB-4: capabilities() returns EXACTLY the caller-declared struct, unmodified -- this "
              "class never inspects a response to infer either field (004 §3's 'declared, not "
              "probed' rule, restated by core/embedder.hpp for Embedder specifically)");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_openai_embedder: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_openai_embedder: %d FAILURE(S)\n", g_failures);
    return 1;
}
