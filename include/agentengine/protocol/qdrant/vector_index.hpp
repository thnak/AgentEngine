#pragma once
// Implements decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.5 --
// `QdrantVectorIndex`, the `RemoteVectorIndex` (core/remote_vector_index.hpp) reference conformer.
// Reaches any Qdrant-API-compatible host over plain JSON/HTTPS, reusing the exact same transport
// `protocol/openai/embedder.hpp` already proved live (`sandbox::perform_provider_https_exchange`,
// ADR-063 claim 4) and the identical `SecretStore` credential-resolution discipline (resolved INSIDE
// each call, never at construction, 004 §1/018 §4) -- ZERO new third-party dependency, staying at the
// same "protocol-tier" cost `OpenAIEmbedder` already established, per ADR-180 §2.5's own chosen design.
//
// Every external protocol claim below is dated and cited, per CLAUDE.md's own citation rule --
// see docs/research/2026-09-22-qdrant-rest-api.md for the sourced facts this file implements against
// (fetched directly from Qdrant's own API reference and architecture docs, 2026-09-22):
//   - Upsert: `PUT /collections/{name}/points`, body `{"points": [{"id", "vector", "payload"}, ...]}`.
//   - Search: `POST /collections/{name}/points/search`, body `{"vector", "limit", "with_payload"}`,
//     response `{"status", "time", "result": [{"id", "score", "payload"}, ...]}`.
//   - Retrieve one: `GET /collections/{name}/points/{id}` -- returns HTTP 404 if not found (a real,
//     sourced fact, not assumed from generic REST convention).
//   - Auth: a literal `api-key: <value>` header -- NOT `Authorization: Bearer`, unlike every
//     OpenAI-family conformer in this tree. Copying that header verbatim would be silently wrong.
//   - Error envelope: `{"status": {"error": "<message>"}}` -- structurally different from OpenAI's
//     `{"error": {"message": "..."}}` shape; reading `error.message` here would find nothing.
//
// **The one real protocol mismatch this ADR family's own id scheme collides with, and how this file
// resolves it (research doc §3):** Qdrant point ids must be an unsigned 64-bit integer or a
// UUID-formatted string -- never an arbitrary string. This ADR's chunk ids
// (`corpus_chunk.hpp`/`worktree_types.hpp::Digest`) are 64-lowercase-hex-char SHA-256 digests, which
// are NOT valid Qdrant point ids. `chunk_id_to_qdrant_point_id()` below deterministically reformats a
// digest's own first 32 hex characters into UUID `8-4-4-4-12` dash syntax for every OUTBOUND call
// (upsert/search-by-id/retrieve); the ORIGINAL digest is stored as a `chunk_id` payload field on every
// upserted point and read back from `with_payload: true` search results -- so `ScoredId::id` this
// conformer returns is always the REAL chunk digest every other piece of this ADR family keys on
// (`render_scored_chunk()`, `CorpusChunkRecord` lookup, `WorktreeObjectStore::get_blob()`), never
// Qdrant's own internal point id.
//
// I2/I4 posture, stated explicitly (ADR-180 §2.5's own requirement, not assumed): identical to
// `OpenAIEmbedder`'s -- the capability IS the `SecretRef` grant itself (this codebase has no separate
// `cap::NetEgress`-style token for provider calls), and every call is attributable through the same
// `EffectContext` provenance chain (I4) every other declared backend uses. This conformer introduces
// no new capability primitive, only a second conformer of an existing policy.
//
// `contains()` is a REAL network round-trip (unlike a local `VectorIndex`'s O(1) hash lookup) -- named
// explicitly as a cost, not hidden (ADR-180 §7: no batched "which of these ids exist" call is designed
// here; a large corpus re-mount against this conformer pays one round-trip per chunk).
//
// **A REAL, MUST-READ confidentiality residual, found by ADR-180 §4's red-team pass 2 (2026-09-22) --
// weaker than the in-process guarantee ADR-063's own cross-tenant isolation model relies on, and it is
// the DEPLOYER's responsibility to close it, not something this class enforces:**
// `vector_rag_context_provider.hpp`'s own top comment states this codebase's entire cross-corpus
// confidentiality model: "confidentiality across corpora relies entirely on which ids a caller's OWN
// `IndexT` was ever populated with... `WorktreeObjectStore::get_blob()` carries NO capability check of
// its own." For `BruteForceCosineIndex` that holds STRUCTURALLY -- only an in-process reference can
// call `add_batch()`, so one tenant's process literally cannot reach another tenant's index object.
// For `QdrantVectorIndex`, "the index" is a NETWORK-WRITABLE Qdrant collection gated by nothing more
// than possession of the configured `api-key` -- so if TWO corpora (e.g. two tenants) are ever pointed
// at the SAME `collection_name` with the SAME key, anyone able to write to that collection can upsert a
// point whose `payload.chunk_id` NAMES A DIGEST THEY DO NOT OWN (their own vector, someone else's
// claimed identity) -- and `render_scored_chunk()`'s downstream `object_store_->get_blob(scored.id)`
// call (itself carrying no capability check, per the quote above) will then happily fetch and inject
// THAT digest's real content into whichever corpus's search returned it. `parse_search_response()`
// below cross-checks that a result's own point id agrees with what `payload.chunk_id` claims
// (defense-in-depth against accidental corruption/a foreign point from unrelated collection use) but
// this does NOT stop a determined attacker who understands this file's own (public, inspectable)
// id-derivation scheme and writes both fields consistently.
// **The fix is operational, not a code guarantee this class can provide by itself: give every distinct
// `corpus_scope` (`per_principal`/`per_tenant`/`global_shared`, ADR-063 §2.6a) ITS OWN Qdrant
// `collection_name`, exactly the same "declared, host-configured, never shared across scopes" discipline
// ADR-063 §2.6a already requires of `Mount`/`ref_name` derivation for the in-process case -- a deployer
// who instead shares one collection across tenants has broken that discipline for Qdrant exactly the way
// reusing one `Mount.mount_id` across tenants would break it for `BruteForceCosineIndex`.**

#ifdef AGENTENGINE_WITH_HTTPS

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/remote_vector_index.hpp"
#include "agentengine/sandbox/provider_http_client.hpp"
#include "agentengine/trust/secret.hpp"

namespace agentengine::qdrant {

namespace detail {

using Resolver = std::function<result<sandbox::VerifiedEndpoint>(std::string_view, std::uint16_t)>;

// docs/research/2026-09-22-qdrant-rest-api.md §3: deterministically reformats a chunk digest's own
// first 32 hex characters into UUID `8-4-4-4-12` dash syntax. NOT a real RFC 4122 UUID (no
// version/variant nibble is forced) -- Qdrant's point-id parser accepts any UUID-SHAPED string, and
// this conformer needs a deterministic, content-addressed mapping (the same digest always produces
// the same point id), not a randomly-assigned one. Reject-not-coerce on a too-short digest rather than
// an out-of-bounds substr() -- `Digest` is contractually always 64 hex chars
// (`worktree_types.hpp`), but this function does not silently trust that from a caller it cannot see.
[[nodiscard]] inline result<std::string> chunk_id_to_qdrant_point_id(std::string const& chunk_digest) {
    if (chunk_digest.size() < 32) {
        return std::unexpected(error{failure_class::contract,
                                      "chunk id is too short to derive a Qdrant point id from (need "
                                      "at least 32 hex characters, got " +
                                          std::to_string(chunk_digest.size()) + ")",
                                      "qdrant_vector_index.chunk_id_too_short"});
    }
    std::string const hex = chunk_digest.substr(0, 32);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

[[nodiscard]] inline sandbox::NetEgressRequest build_qdrant_request(std::string method, std::string path,
                                                                       std::string const& api_key,
                                                                       std::string body = {}) {
    sandbox::NetEgressRequest req;
    req.method = std::move(method);
    req.path = std::move(path);
    req.headers.emplace_back("Content-Type", "application/json");
    // docs/research/2026-09-22-qdrant-rest-api.md §2: a literal `api-key` header, NOT
    // `Authorization: Bearer` -- do not copy the OpenAI-family header shape here.
    req.headers.emplace_back("api-key", api_key);
    req.body = std::move(body);
    return req;
}

// docs/research/2026-09-22-qdrant-rest-api.md §4: `{"status": {"error": "<message>"}}`, structurally
// different from OpenAI's `{"error": {"message": ...}}` -- reading `error.message` here would find
// nothing real.
[[nodiscard]] inline error map_http_status_error(std::uint16_t status, std::string const& body) {
    failure_class klass = failure_class::fatal;
    if (status == 429 || status >= 500) {
        klass = failure_class::transient;  // 004 §4: retry applies to Transient only
    } else if (status == 401 || status == 403) {
        klass = failure_class::policy;
    } else if (status >= 400) {
        klass = failure_class::contract;
    }
    std::string message = "qdrant http status " + std::to_string(status);
    if (auto parsed = json::parse(body); parsed) {
        if (auto const* st = parsed->find("status"); st && st->is_object()) {
            if (auto const* e = st->find("error"); e && e->is_string()) message = e->as_string();
        }
    }
    return error{klass, message, "qdrant_vector_index.http_" + std::to_string(status)};
}

// Named residual (ADR-180 §4 red-team pass 2 finding 5, shared infrastructure, not unique to this
// file): neither this function nor build_search_request_body() below checks a vector/query component
// for NaN/Infinity before handing it to json::Value::make_number() -- core/json_value.hpp's own
// dump_into() renders those as the literal (invalid-JSON) tokens `nan`/`inf`/`-inf` via
// std::to_chars. A buggy upstream Embedder or a degenerate similarity computation would surface as a
// visible request failure at the real Qdrant host (not silent corruption), but not the clean,
// locally-raised `contract` error this codebase's reject-not-coerce convention otherwise prefers.
// Root cause is shared json_value.hpp; noted here because these are the first vector-carrying call
// sites in this ADR family to hit it unguarded.
//
// Builds one PUT /collections/{name}/points body: {"points": [{"id", "vector", "payload"}, ...]}.
[[nodiscard]] inline result<json::Value> build_upsert_request_body(
    std::vector<std::string> const& chunk_ids, std::vector<std::vector<float>> const& vectors) {
    // Defense-in-depth, found by this file's own offline test (a real heap-buffer-overflow, caught
    // by AddressSanitizer, not merely reasoned about): `QdrantVectorIndex::add_batch()` already
    // checks `ids.size() == vectors.size()` before calling this function, but this is a plain
    // `detail::` function a test (or any other future caller) can call directly, bypassing that
    // guard. Without this check, a mismatched pair reads `vectors[i]` out of bounds the moment `i`
    // reaches `vectors.size()` -- exactly the failure mode `BruteForceCosineIndex::add_batch()`'s own
    // identical check (vector_index.hpp) and `corpus_source.hpp`'s `embed_batch` result-size check
    // both already guard against for the analogous reason. Never trust a caller-maintained invariant
    // silently; check it here too.
    if (chunk_ids.size() != vectors.size()) {
        return std::unexpected(error{failure_class::contract,
                                      "chunk_ids and vectors must have the same length (got " +
                                          std::to_string(chunk_ids.size()) + " ids, " +
                                          std::to_string(vectors.size()) + " vectors)",
                                      "qdrant_vector_index.build_upsert_length_mismatch"});
    }
    std::vector<json::Value> points;
    points.reserve(chunk_ids.size());
    for (std::size_t i = 0; i < chunk_ids.size(); ++i) {
        auto point_id = chunk_id_to_qdrant_point_id(chunk_ids[i]);
        if (!point_id) return std::unexpected(point_id.error());

        std::vector<json::Value> vec_json;
        vec_json.reserve(vectors[i].size());
        for (float f : vectors[i]) vec_json.push_back(json::Value::make_number(static_cast<double>(f)));

        std::vector<std::pair<std::string, json::Value>> payload_obj;
        payload_obj.emplace_back("chunk_id", json::Value::make_string(chunk_ids[i]));

        std::vector<std::pair<std::string, json::Value>> point_obj;
        point_obj.emplace_back("id", json::Value::make_string(*point_id));
        point_obj.emplace_back("vector", json::Value::make_array(std::move(vec_json)));
        point_obj.emplace_back("payload", json::Value::make_object(std::move(payload_obj)));
        points.push_back(json::Value::make_object(std::move(point_obj)));
    }
    std::vector<std::pair<std::string, json::Value>> body_obj;
    body_obj.emplace_back("points", json::Value::make_array(std::move(points)));
    return json::Value::make_object(std::move(body_obj));
}

// Builds one POST /collections/{name}/points/search body -- always `with_payload: true` (never
// optional here): this conformer's own ability to recover the REAL chunk digest from a search result
// depends entirely on the payload being present (see this file's own top comment).
[[nodiscard]] inline json::Value build_search_request_body(std::span<float const> query, std::size_t k) {
    std::vector<json::Value> vec_json;
    vec_json.reserve(query.size());
    for (float f : query) vec_json.push_back(json::Value::make_number(static_cast<double>(f)));

    std::vector<std::pair<std::string, json::Value>> body_obj;
    body_obj.emplace_back("vector", json::Value::make_array(std::move(vec_json)));
    body_obj.emplace_back("limit", json::Value::make_number(static_cast<double>(k)));
    body_obj.emplace_back("with_payload", json::Value::make_bool(true));
    return json::Value::make_object(std::move(body_obj));
}

// Parses `{"status", "time", "result": [{"id", "score", "payload": {"chunk_id"}}, ...]}` -- reject
// (not skip) a malformed item, matching `openai_embedder.hpp::parse_embeddings_response()`'s own
// stance ("a partial/malformed response is treated as a failure, never a silently short result"): an
// item missing `payload.chunk_id` means this conformer cannot recover that result's real identity at
// all, so silently dropping it would silently shrink the caller's top-k without any signal that
// happened, rather than surfacing the real, actionable problem (a foreign point in this collection
// that this conformer's own upsert did not write).
[[nodiscard]] inline result<std::vector<ScoredId>> parse_search_response(json::Value const& body) {
    // Same defensive stance OpenAIEmbedder's own parser takes: a lenient proxy could plausibly answer
    // 200 with an error envelope instead of a genuine 4xx/5xx.
    if (auto const* st = body.find("status"); st && st->is_object()) {
        if (auto const* e = st->find("error"); e && e->is_string()) {
            return std::unexpected(
                error{failure_class::contract, "qdrant search error: " + e->as_string(),
                      "qdrant_vector_index.error"});
        }
    }

    json::Value const* result = body.find("result");
    if (!result || !result->is_array()) {
        return std::unexpected(error{failure_class::contract, "response has no 'result' array",
                                      "qdrant_vector_index.no_result"});
    }

    std::vector<ScoredId> out;
    out.reserve(result->as_array().size());
    for (json::Value const& item : result->as_array()) {
        json::Value const* score = item.find("score");
        if (!score || !score->is_number()) {
            return std::unexpected(error{failure_class::contract,
                                          "a result[] item is missing a numeric 'score' field",
                                          "qdrant_vector_index.malformed_item"});
        }
        json::Value const* payload = item.find("payload");
        json::Value const* chunk_id = payload ? payload->find("chunk_id") : nullptr;
        if (!payload || !payload->is_object() || !chunk_id || !chunk_id->is_string()) {
            return std::unexpected(error{
                failure_class::contract,
                "a result[] item is missing the 'payload.chunk_id' field this conformer's own "
                "upsert always sets -- this point was not written by this conformer (or its "
                "payload was stripped/overwritten by something else), so its real chunk identity "
                "cannot be recovered",
                "qdrant_vector_index.missing_chunk_id"});
        }

        // ADR-180 §4 red-team pass 2 finding 1 (2026-09-22): cross-check that this point's OWN
        // Qdrant id is actually the DERIVED id of the digest its payload claims, rather than
        // blindly trusting `payload.chunk_id` -- defense-in-depth against a foreign/corrupted point
        // whose id and payload disagree. This is NOT a full fix for the confidentiality residual
        // this file's own top comment names (a determined attacker who understands this file's own
        // public id-derivation scheme can still write BOTH fields self-consistently for a digest
        // they do not own) -- it only catches an inconsistent pair, not a consistent forgery. The
        // real fix is operational (one Qdrant collection per corpus_scope, never shared across
        // tenants), stated explicitly in this file's own top comment, not something this check can
        // provide on its own.
        json::Value const* qdrant_id = item.find("id");
        if (qdrant_id && qdrant_id->is_string()) {
            auto expected_point_id = chunk_id_to_qdrant_point_id(chunk_id->as_string());
            if (expected_point_id && qdrant_id->as_string() != *expected_point_id) {
                return std::unexpected(error{
                    failure_class::contract,
                    "a result[] item's own point id does not match the id this conformer would "
                    "derive from its 'payload.chunk_id' -- this point is either corrupted or was "
                    "not written consistently by this conformer's own upsert; its chunk identity "
                    "cannot be trusted",
                    "qdrant_vector_index.id_payload_mismatch"});
            }
        }
        out.push_back({chunk_id->as_string(), static_cast<float>(score->as_number())});
    }
    return out;
}

}  // namespace detail

// `Store` is any real `SecretStore` -- identical template shape `OpenAIEmbedder<Store>` already uses.
template <SecretStore Store>
// ae-naming-lint: allow QdrantVectorIndex — ADR-180: new vocabulary, not yet in 027 §2-4's tables.
class QdrantVectorIndex {
public:
    // Every step is a plain, synchronous, BLOCKING call (store_.resolve(), perform_provider_
    // https_exchange(), json::parse()) -- the only coroutine keyword used anywhere below is
    // co_return, mirroring OpenAIEmbedder::embed_batch()'s own identical, re-verified-every-change
    // claim (embedder.hpp). Driving any of the three methods below via rt::drive_leaf_task() is sound.
    static constexpr bool synchronous_leaf = true;

    // Parameter order mirrors OpenAIEmbedder's own constructor as closely as the two classes' actual
    // parameters allow (host/port/.../SecretRef/.../store/path_prefix/resolver/
    // ca_bundle_pem_override/transport) -- `collection_name` takes the slot `model` occupies there
    // (both are "which logical resource on the host this conformer talks to"), and there is no
    // `EmbedderCapabilities`-shaped parameter (RemoteVectorIndex declares no capabilities() method).
    // `max_batch_size` (ADR-180 §4 red-team pass 2 finding 2): unlike `Embedder`, `RemoteVectorIndex`
    // declares no required capabilities()/batch-limit concept at all, so this class self-imposes one
    // as an OPTIONAL constructor parameter (`0` = unlimited, matching `EmbedderCapabilities::
    // max_batch_size`'s own "declared, never probed" convention exactly) -- without it, a caller
    // passing an enormous batch would build the entire request body (potentially gigabytes of JSON
    // text for a large vector count) in memory before a single byte reaches the wire, with no early,
    // clearly-coded rejection the way `OpenAIEmbedder::embed_batch()` already gives
    // (`openai_embedder.batch_too_large`).
    QdrantVectorIndex(std::string host, std::uint16_t port, std::string collection_name,
                       SecretRef api_key_ref, Store const& store, std::string path_prefix = "",
                       std::size_t max_batch_size = 0,
                       detail::Resolver resolver = sandbox::resolve_host,
                       std::string ca_bundle_pem_override = {},
                       sandbox::ProviderTransport transport = sandbox::ProviderTransport::tls)
        : host_(std::move(host)),
          port_(port),
          collection_name_(std::move(collection_name)),
          api_key_ref_(std::move(api_key_ref)),
          store_(store),
          path_prefix_(std::move(path_prefix)),
          max_batch_size_(max_batch_size),
          resolver_(std::move(resolver)),
          ca_bundle_pem_override_(std::move(ca_bundle_pem_override)),
          transport_(transport) {}

    [[nodiscard]] task<result<void>> add_batch(std::vector<std::string> const& ids,
                                                 std::vector<std::vector<float>> const& vectors,
                                                 EffectContext& ctx) const {
        if (ids.size() != vectors.size()) {
            co_return std::unexpected(error{failure_class::contract,
                                             "ids and vectors must have the same length",
                                             "qdrant_vector_index.add_batch_length_mismatch"});
        }
        // I2: a zero-item batch performs no effect at all -- no secret resolution, no network call.
        if (ids.empty()) co_return result<void>{};

        if (max_batch_size_ != 0 && ids.size() > max_batch_size_) {
            co_return std::unexpected(error{
                failure_class::contract,
                "add_batch() called with " + std::to_string(ids.size()) +
                    " ids, exceeding this instance's declared max_batch_size (" +
                    std::to_string(max_batch_size_) +
                    "); sub-batching a request that exceeds a declared limit is the caller's own "
                    "responsibility, matching OpenAIEmbedder::embed_batch()'s identical stance "
                    "(ADR-063 §4 finding 9)",
                "qdrant_vector_index.batch_too_large"});
        }

        auto body = detail::build_upsert_request_body(ids, vectors);
        if (!body) co_return std::unexpected(body.error());

        // Resolution happens HERE, inside add_batch(), against EffectContext -- never at
        // construction (004 §1/018 §4, the same rule OpenAIEmbedder::embed_batch() follows).
        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) co_return std::unexpected(lease.error());

        auto req = detail::build_qdrant_request("PUT", path_prefix_ + "/collections/" + collection_name_ +
                                                            "/points",
                                                  lease->reveal_text(), json::dump(*body));
        auto resp = sandbox::perform_provider_https_exchange(host_, port_, req, {}, std::nullopt,
                                                                resolver_, ca_bundle_pem_override_,
                                                                transport_);
        if (!resp) co_return std::unexpected(resp.error());
        if (resp->status < 200 || resp->status >= 300) {
            co_return std::unexpected(detail::map_http_status_error(resp->status, resp->body));
        }
        co_return result<void>{};
    }

    [[nodiscard]] task<result<std::vector<ScoredId>>> search(std::span<float const> query, std::size_t k,
                                                                 EffectContext& ctx) const {
        // ADR-180 §4 red-team pass 2 finding 3: `BruteForceCosineIndex::search(query, 0)` succeeds
        // with an empty result (core/vector_index.hpp); Qdrant's own `limit` field is documented
        // REQUIRED, `>= 1` (docs/research/2026-09-22-qdrant-rest-api.md §1) -- sending `k == 0`
        // would very likely 400 against a real host, meaning the SAME provider configuration
        // (`max_injected_ = 0`) silently behaves differently depending only on which VectorIndex
        // conformer backs it. Short-circuited here for parity with the local case, matching
        // add_batch()'s own existing "no effect for an empty request" I2 posture (no secret
        // resolution, no network call) for the identical reason.
        if (k == 0) co_return std::vector<ScoredId>{};

        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) co_return std::unexpected(lease.error());

        auto body = detail::build_search_request_body(query, k);
        auto req = detail::build_qdrant_request(
            "POST", path_prefix_ + "/collections/" + collection_name_ + "/points/search",
            lease->reveal_text(), json::dump(body));
        auto resp = sandbox::perform_provider_https_exchange(host_, port_, req, {}, std::nullopt,
                                                                resolver_, ca_bundle_pem_override_,
                                                                transport_);
        if (!resp) co_return std::unexpected(resp.error());
        if (resp->status < 200 || resp->status >= 300) {
            co_return std::unexpected(detail::map_http_status_error(resp->status, resp->body));
        }

        auto parsed = json::parse(resp->body);
        if (!parsed) co_return std::unexpected(parsed.error());
        co_return detail::parse_search_response(*parsed);
    }

    [[nodiscard]] task<result<bool>> contains(std::string const& id, EffectContext& ctx) const {
        auto point_id = detail::chunk_id_to_qdrant_point_id(id);
        if (!point_id) co_return std::unexpected(point_id.error());

        auto lease = store_.resolve(api_key_ref_, ctx);
        if (!lease) co_return std::unexpected(lease.error());

        auto req = detail::build_qdrant_request(
            "GET", path_prefix_ + "/collections/" + collection_name_ + "/points/" + *point_id,
            lease->reveal_text());
        auto resp = sandbox::perform_provider_https_exchange(host_, port_, req, {}, std::nullopt,
                                                                resolver_, ca_bundle_pem_override_,
                                                                transport_);
        if (!resp) co_return std::unexpected(resp.error());
        // docs/research/2026-09-22-qdrant-rest-api.md §1: 404 is Qdrant's own documented "point not
        // found" signal for this endpoint -- a normal, successful "no" answer, not a transport/policy
        // error. Every OTHER non-2xx status is still a real error, unchanged.
        // Named residual (ADR-180 §4 red-team pass 2 finding 4): the research doc only confirms
        // 404-on-missing-POINT for this endpoint, not whether a missing/deleted COLLECTION folds
        // into the identical status. If it does, a misconfigured `collection_name` would make every
        // `contains()` call silently report `false` (treating the corpus as never-ingested) rather
        // than surfacing the real, actionable problem -- which would only then appear later, at
        // `add_batch()`. Not confirmed either way; left as a named gap, not silently assumed safe.
        if (resp->status == 404) co_return false;
        if (resp->status < 200 || resp->status >= 300) {
            co_return std::unexpected(detail::map_http_status_error(resp->status, resp->body));
        }
        co_return true;
    }

private:
    std::string host_;
    std::uint16_t port_;
    std::string collection_name_;
    SecretRef api_key_ref_;
    Store const& store_;
    std::string path_prefix_;
    std::size_t max_batch_size_;
    detail::Resolver resolver_;
    std::string ca_bundle_pem_override_;
    sandbox::ProviderTransport transport_;
};

// `QdrantVectorIndex` is templated on `Store`, so a bare `RemoteVectorIndex<QdrantVectorIndex>` cannot
// be written -- concept-checking a template requires a concrete instantiation, the same reason
// `OpenAIEmbedder<InMemorySecretStore>` is what `embedder.hpp`'s own bottom static_assert checks.
static_assert(RemoteVectorIndex<QdrantVectorIndex<InMemorySecretStore>>,
              "QdrantVectorIndex must satisfy the real RemoteVectorIndex concept (ADR-180 §2.1/§2.5)");

}  // namespace agentengine::qdrant

#endif  // AGENTENGINE_WITH_HTTPS
