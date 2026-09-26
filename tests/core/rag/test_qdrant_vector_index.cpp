// Implements decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.5/§3 claim 6 --
// `QdrantVectorIndex` (protocol/qdrant/vector_index.hpp): the OFFLINE-provable half of the claim
// (request/response shape, point-id derivation, error parsing) -- mirrors
// `test_openai_embedder.cpp`'s own split exactly: this file exercises the `detail::` functions
// directly, NEVER `sandbox::perform_provider_https_exchange`, so it needs no real network and no live
// Qdrant instance. A separate, live-network-gated test (mirroring
// `test_openai_embedder_openrouter_live_e2e.cpp`'s pattern) is the still-owed other half -- named in
// ADR-180 §8 step 7's own implementation plan, not built in this pass.

#ifdef AGENTENGINE_WITH_HTTPS

#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "agentengine/protocol/qdrant/vector_index.hpp"
#include "../../support/run_task_sync.hpp"

using namespace agentengine::qdrant;
using namespace agentengine::qdrant::detail;

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

}  // namespace

int main() {
    // --- chunk_id_to_qdrant_point_id: docs/research/2026-09-22-qdrant-rest-api.md §3 -------------
    {
        std::string const digest(64, 'a');  // a real Digest is always 64 lowercase hex chars
        auto point_id = chunk_id_to_qdrant_point_id(digest);
        AE_CHECK(point_id.has_value(), "a 64-char digest derives a point id successfully");
        AE_CHECK(point_id.has_value() && *point_id == "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
                 "the derived id is UUID-shaped (8-4-4-4-12 dash syntax) from the digest's own first "
                 "32 hex chars");

        // Determinism: the SAME digest always derives the SAME point id.
        auto point_id_again = chunk_id_to_qdrant_point_id(digest);
        AE_CHECK(point_id_again.has_value() && *point_id_again == *point_id,
                 "the derivation is deterministic -- the same digest always maps to the same point id");

        // Two DIFFERENT digests (differing only past the first 32 hex chars, which is all the
        // derivation actually reads) still derive the SAME point id -- named honestly as a real,
        // narrow collision surface of this scheme, not hidden.
        std::string const digest_b = std::string(32, 'a') + std::string(32, 'b');
        std::string const digest_c = std::string(32, 'a') + std::string(32, 'c');
        auto point_id_b = chunk_id_to_qdrant_point_id(digest_b);
        auto point_id_c = chunk_id_to_qdrant_point_id(digest_c);
        AE_CHECK(point_id_b.has_value() && point_id_c.has_value() && *point_id_b == *point_id_c,
                 "two digests sharing only their first 32 hex chars derive the SAME point id -- a "
                 "real, narrow (128-bit) collision surface of this deterministic scheme, worth "
                 "knowing about explicitly rather than silently");

        auto too_short = chunk_id_to_qdrant_point_id("short");
        AE_CHECK(!too_short.has_value() && too_short.error().code == "qdrant_vector_index.chunk_id_too_short",
                 "reject-not-coerce: a too-short chunk id is rejected with a typed error, not an "
                 "out-of-bounds substr()");
    }

    // --- build_upsert_request_body -------------------------------------------------------------
    {
        std::string const digest(64, 'a');
        auto body = build_upsert_request_body({digest}, {{1.0f, 0.5f, 0.0f}});
        AE_CHECK(body.has_value(), "build_upsert_request_body succeeds for one well-formed chunk");
        if (body.has_value()) {
            auto const* points = body->find("points");
            AE_CHECK(points != nullptr && points->is_array() && points->as_array().size() == 1,
                     "the body has a 'points' array with exactly one entry");
            if (points && points->is_array() && !points->as_array().empty()) {
                auto const& point = points->as_array().front();
                auto const* id = point.find("id");
                AE_CHECK(id != nullptr && id->is_string() &&
                             id->as_string() == "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
                         "the point's own 'id' field is the DERIVED UUID-shaped id, never the raw "
                         "chunk digest");
                auto const* vector = point.find("vector");
                AE_CHECK(vector != nullptr && vector->is_array() && vector->as_array().size() == 3,
                         "the point's 'vector' field carries all 3 components");
                auto const* payload = point.find("payload");
                auto const* chunk_id_field = payload ? payload->find("chunk_id") : nullptr;
                AE_CHECK(payload != nullptr && chunk_id_field != nullptr && chunk_id_field->is_string() &&
                             chunk_id_field->as_string() == digest,
                         "the point's 'payload.chunk_id' field carries the REAL, original chunk "
                         "digest -- the whole mechanism that lets a later search() recover it");
            }
        }

        // Found by this test itself (a real heap-buffer-overflow, caught by AddressSanitizer): this
        // function must reject a length mismatch ITSELF, not merely rely on
        // QdrantVectorIndex::add_batch()'s own (separate) check -- see build_upsert_request_body's
        // own comment.
        auto length_mismatch = build_upsert_request_body({digest, digest}, {{1.0f}});
        AE_CHECK(!length_mismatch.has_value() &&
                     length_mismatch.error().code == "qdrant_vector_index.build_upsert_length_mismatch",
                 "build_upsert_request_body rejects a chunk_ids/vectors length mismatch itself, "
                 "rather than reading vectors[i] out of bounds once i reaches vectors.size()");

        auto short_id_rejected = build_upsert_request_body({"too-short"}, {{1.0f}});
        AE_CHECK(!short_id_rejected.has_value() &&
                     short_id_rejected.error().code == "qdrant_vector_index.chunk_id_too_short",
                 "build_upsert_request_body propagates a too-short chunk id's own point-id-derivation "
                 "rejection");
    }

    // --- build_search_request_body -------------------------------------------------------------
    {
        float const q[3] = {1.0f, 2.0f, 3.0f};
        auto body = build_search_request_body(std::span<float const>(q, 3), /*k=*/5);
        auto const* vector = body.find("vector");
        AE_CHECK(vector != nullptr && vector->is_array() && vector->as_array().size() == 3,
                 "the search body's 'vector' field carries the full query");
        auto const* limit = body.find("limit");
        AE_CHECK(limit != nullptr && limit->is_number() && limit->as_number() == 5.0,
                 "the search body's 'limit' field is the requested k");
        auto const* with_payload = body.find("with_payload");
        AE_CHECK(with_payload != nullptr && with_payload->is_bool() && with_payload->as_bool() == true,
                 "the search body ALWAYS requests with_payload=true -- never optional here, since "
                 "recovering the real chunk digest depends entirely on it");
    }

    // --- QdrantVectorIndex::search(k=0) short-circuits without a network call (ADR-180 §4 red-team
    // pass 2 finding 3) -- exercised via a mock SecretStore that fails resolution if ever called, so
    // this test proves "no call" rather than merely "the call would have failed anyway". ---------------
    {
        class NeverResolveStore {
        public:
            [[nodiscard]] ae::result<ae::SecretLease> resolve(ae::SecretRef const&, ae::EffectContext&) const {
                return std::unexpected(ae::error{ae::failure_class::fatal,
                                                   "resolve() must never be called for k=0",
                                                   "test.unexpected_resolve"});
            }
        };
        static_assert(ae::SecretStore<NeverResolveStore>);
        NeverResolveStore store;
        QdrantVectorIndex<NeverResolveStore> index("127.0.0.1", 1, "test-collection",
                                                     ae::SecretRef{"unused"}, store);
        ae::EffectContext ctx{};
        float const q[2] = {1.0f, 0.0f};
        auto result = ae::test_support::run_task_sync<ae::result<std::vector<ae::ScoredId>>>(
            index.search(std::span<float const>(q, 2), /*k=*/0, ctx));
        AE_CHECK(result.has_value() && result->empty(),
                 "search(k=0) returns an empty, successful result -- matching "
                 "BruteForceCosineIndex::search(query, 0)'s identical behavior, rather than sending "
                 "Qdrant's documented-required 'limit' field as 0 and likely getting a 400");
    }

    // --- parse_search_response -------------------------------------------------------------------
    {
        auto ok = ae::json::parse(R"({
            "status": "ok",
            "time": 0.001,
            "result": [
                {"id": "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa", "score": 0.95,
                 "payload": {"chunk_id": "real-chunk-digest-1"}},
                {"id": "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb", "score": 0.42,
                 "payload": {"chunk_id": "real-chunk-digest-2"}}
            ]
        })");
        AE_CHECK(ok.has_value(), "setup: the well-formed response JSON parses");
        if (ok.has_value()) {
            auto scored = parse_search_response(*ok);
            AE_CHECK(scored.has_value() && scored->size() == 2, "parse_search_response returns 2 entries");
            if (scored.has_value() && scored->size() == 2) {
                AE_CHECK((*scored)[0].id == "real-chunk-digest-1" && (*scored)[0].score == 0.95f,
                         "entry 0's id is the REAL chunk digest from payload.chunk_id, not the "
                         "qdrant-internal UUID id field, and its score is preserved");
                AE_CHECK((*scored)[1].id == "real-chunk-digest-2" && (*scored)[1].score == 0.42f,
                         "entry 1 likewise");
            }
        }

        auto missing_payload = ae::json::parse(R"({"status":"ok","result":[{"id":"x","score":0.1}]})");
        AE_CHECK(missing_payload.has_value(), "setup: parses");
        if (missing_payload.has_value()) {
            auto rejected = parse_search_response(*missing_payload);
            AE_CHECK(!rejected.has_value() &&
                         rejected.error().code == "qdrant_vector_index.missing_chunk_id",
                     "a result item with no payload.chunk_id is REJECTED (a real, actionable error), "
                     "not silently dropped from the result set");
        }

        // ADR-180 §4 red-team pass 2 finding 1: a result item whose own Qdrant point id does NOT
        // match what would be derived from its payload.chunk_id is a foreign/corrupted point,
        // rejected outright -- defense-in-depth, not a full fix for the confidentiality residual
        // this file's own top comment names (see that comment for the real, operational fix).
        auto real_digest = std::string(64, 'a');
        auto real_point_id = chunk_id_to_qdrant_point_id(real_digest);
        AE_CHECK(real_point_id.has_value(), "setup: derive the real point id for digest 'a'*64");
        auto mismatched = ae::json::parse(std::string(R"({"status":"ok","result":[{"id":")") +
                                            "wrong-point-id-not-derived-from-payload" +
                                            R"(","score":0.5,"payload":{"chunk_id":")" + real_digest +
                                            R"("}}]})");
        AE_CHECK(mismatched.has_value(), "setup: the id/payload-mismatched response parses as JSON");
        if (mismatched.has_value()) {
            auto rejected = parse_search_response(*mismatched);
            AE_CHECK(!rejected.has_value() &&
                         rejected.error().code == "qdrant_vector_index.id_payload_mismatch",
                     "a result item whose own 'id' disagrees with the id derived from its "
                     "'payload.chunk_id' is rejected, not silently trusted");
        }
        // The CONSISTENT (self-forged) case is explicitly NOT caught by this check -- named
        // honestly, matching this file's own top-comment disclosure: an id/payload pair that IS
        // internally consistent (because whoever wrote it understood the derivation scheme) passes.
        auto consistent = ae::json::parse(std::string(R"({"status":"ok","result":[{"id":")") +
                                            *real_point_id + R"(","score":0.5,"payload":{"chunk_id":")" +
                                            real_digest + R"("}}]})");
        AE_CHECK(consistent.has_value(), "setup: the self-consistent response parses");
        if (consistent.has_value()) {
            auto accepted = parse_search_response(*consistent);
            AE_CHECK(accepted.has_value() && accepted->size() == 1 && (*accepted)[0].id == real_digest,
                     "a self-consistent id/payload pair is accepted -- the check only catches an "
                     "inconsistent pair, exactly as documented, not a determined self-consistent forgery");
        }

        auto error_envelope = ae::json::parse(R"({"status":{"error":"Format error in JSON body"}})");
        AE_CHECK(error_envelope.has_value(), "setup: parses");
        if (error_envelope.has_value()) {
            auto rejected = parse_search_response(*error_envelope);
            AE_CHECK(!rejected.has_value() &&
                         rejected.error().message.find("Format error in JSON body") != std::string::npos,
                     "a 200-with-error-envelope response is detected and its real message surfaced, "
                     "not silently treated as a valid empty/malformed result");
        }
    }

    // --- map_http_status_error: docs/research/2026-09-22-qdrant-rest-api.md §4 ---------------------
    {
        auto err = map_http_status_error(400, R"({"status":{"error":"Format error in JSON body: Expected"}})");
        AE_CHECK(err.klass == ae::failure_class::contract, "400 maps to failure_class::contract");
        AE_CHECK(err.message == "Format error in JSON body: Expected",
                 "the error message is extracted from 'status.error' (Qdrant's OWN error envelope "
                 "shape), NOT 'error.message' (the OpenAI-family shape a naive copy would have used)");

        auto unauthorized = map_http_status_error(401, R"({"status":{"error":"unauthorized"}})");
        AE_CHECK(unauthorized.klass == ae::failure_class::policy, "401 maps to failure_class::policy");

        auto server_err = map_http_status_error(500, R"({"status":{"error":"internal"}})");
        AE_CHECK(server_err.klass == ae::failure_class::transient, "500 maps to failure_class::transient");

        auto rate_limited = map_http_status_error(429, R"({"status":{"error":"rate limited"}})");
        AE_CHECK(rate_limited.klass == ae::failure_class::transient, "429 maps to failure_class::transient");

        // Malformed/unparseable body -- falls back to a status-only message, never crashes.
        auto bad_body = map_http_status_error(404, "not json at all");
        AE_CHECK(bad_body.message.find("404") != std::string::npos,
                 "a non-JSON error body falls back to a status-only message rather than crashing "
                 "json::parse() or leaving an empty message");
    }

    // --- Concept conformance (compile-time; already checked by the header's own static_assert, but
    // makes the intent visible from this test file's own perspective too) ---------------------------
    static_assert(agentengine::RemoteVectorIndex<QdrantVectorIndex<agentengine::InMemorySecretStore>>);
    static_assert(!agentengine::VectorIndex<QdrantVectorIndex<agentengine::InMemorySecretStore>>,
                  "QdrantVectorIndex is exclusively a RemoteVectorIndex, never accidentally also a "
                  "plain VectorIndex (ADR-180 §4 finding R1's own disjointness guarantee, exercised "
                  "here against a REAL conformer, not just a synthetic mock)");
    AE_CHECK(true, "concept conformance (compile-time, see static_asserts above)");

    std::cout << (g_failures == 0 ? "test_qdrant_vector_index: OK\n" : "test_qdrant_vector_index: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}

#else

int main() {
    return 0;  // AGENTENGINE_WITH_HTTPS is OFF -- this test has nothing to exercise, matching
               // test_openai_embedder.cpp's own posture for the identical build configuration.
}

#endif  // AGENTENGINE_WITH_HTTPS
