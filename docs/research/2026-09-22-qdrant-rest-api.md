# Qdrant REST API — points upsert/search/retrieve, point ID constraints, authentication

**Fetched:** 2026-09-22, via web search + direct scrape of Qdrant's official documentation and API
reference. Backing `decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md` §2.5's
`QdrantVectorIndex` — a `RemoteVectorIndex` conformer over plain JSON/HTTPS, reusing the same
`sandbox::perform_provider_https_exchange` transport `protocol/openai/embedder.hpp` already uses.

## 1. Endpoints (source: `https://api.qdrant.tech/v-1-14-x/api-reference/points/upsert-points`,
`.../search/points`, `.../points/get-point`, all fetched 2026-09-22)

- **Upsert**: `PUT /collections/{collection_name}/points` — "Performs the insert + update action on
  specified points. Any point with an existing `{id}` will be overwritten." Optional query params
  `wait`/`ordering`, not used by this conformer (defaults are fine — no ordering guarantee is needed
  beyond what a single-writer-per-mount corpus already assumes, per ADR-063's own inherited posture).
- **Search**: `POST /collections/{collection_name}/points/search` — "Retrieves the closest points based
  on vector similarity and given filtering conditions." Request fields: `vector` (required, `list of
  doubles`), `limit` (required, `>=1`), `with_payload` (optional, **default `false`**), `with_vector`
  (optional, default `false`). Response: `{"status": "...", "time": ..., "result": [ {...} ]}`, each
  result item carrying (per the "Show 7 properties" collapsed schema and cross-checked against every
  client-library example scraped) at minimum `id`, `score`, and (when `with_payload` was requested)
  `payload`.
- **Retrieve one point**: `GET /collections/{collection_name}/points/{id}` — "Retrieves all details from
  a single point." Response: `{"status": "...", "time": ..., "result": {...}}` on success.
  **Returns HTTP 404 if the point is not found** (source: Qdrant's own architecture doc, scraped
  2026-09-22, "Retrieve Point ... Limitations: Returns 404 if point not found," cross-confirmed by a
  real reported-behavior GitHub issue, `qdrant/qdrant#4048`, describing a batch-recommend 404 for a
  missing point in the same product) — a real, sourced fact, not assumed from REST convention alone.

## 2. Authentication (source: same three API-reference pages, each listing identical auth metadata)

Every endpoint above lists: `api-key: string — API Key authentication via header`. This is a custom
header named literally `api-key` (lowercase, hyphenated), **not** `Authorization: Bearer <token>` the
way OpenAI/Anthropic/OpenRouter are — a real, load-bearing difference from `OpenAIEmbedder`'s own header
construction (`embedder.hpp`'s `build_embeddings_http_request()`, `"Authorization: Bearer " + api_key`)
that must NOT be copied verbatim.

## 3. Point ID format — the one real design constraint this ADR's chunk-id scheme collides with

Source: `https://qdrant.tech/documentation/manage-data/points/` §"Point IDs"/"Upload Points" (scraped
2026-09-22), cross-checked against every Python/Go/TypeScript/Java client example on that same page.
**Every example uses either a small unsigned integer (`1`, `2`, `3`) or a UUID-formatted string**
(`"5c56c793-69f3-4fbf-87e6-c4bf54c28c26"`) as a point id — never an arbitrary string. This matches
Qdrant's documented ID type across the three reference pages above: `id uint64 or string Required`
(the `get-point` page's own parameter listing) — the `string` variant is constrained to UUID syntax by
Qdrant's own point-id parser (confirmed by every example using UUID-shaped strings, never an arbitrary
byte string like a hex digest).

**This ADR's corpus-chunk ids are 64-lowercase-hex-character SHA-256 digests** (`worktree_types.hpp`:
`using Digest = std::string; // hex-encoded SHA-256, 64 lowercase hex chars`) — not valid Qdrant point
ids on their own. `QdrantVectorIndex` must therefore:
1. Derive a UUID-SHAPED point id deterministically from each chunk digest (the same digest always maps
   to the same point id — no randomness, no separate id-allocation state to keep in sync) for every
   outbound call (`upsert`, `search`-by-id, `retrieve`).
2. Store the ORIGINAL chunk digest as a payload field on the point (`{"chunk_id": "<64-hex-digest>"}`),
   requesting `with_payload: true` on every `search()` call, so the digest — the real id every OTHER
   piece of this ADR family keys on (`render_scored_chunk()`, `CorpusChunkRecord` lookup, `WorktreeObjectStore::
   get_blob()`) — can be read back from the payload rather than from Qdrant's own internal point id.

This is a real, sourced protocol constraint, not a stylistic choice this file invents — see
`protocol/qdrant/vector_index.hpp`'s own top comment for how `QdrantVectorIndex` implements it.

## 4. Error response shape

Source: a real reported error message quoted verbatim in a Qdrant-client GitHub issue (scraped
2026-09-22): `400 - {"status":{"error":"Format error in JSON body: Expected ..."}}` — Qdrant's error
envelope is `{"status": {"error": "<message>"}}`, structurally DIFFERENT from OpenAI's
`{"error": {"message": "..."}}` shape `openai_embedder.hpp`'s own `parse_embeddings_response()` parses.
`QdrantVectorIndex`'s own status-mapping error parser must read `status.error`, not `error.message` —
copying the OpenAI shape here would silently fail to extract any real message from a Qdrant error body.
