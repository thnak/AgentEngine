# Embedding-provider landscape — OpenRouter and Anthropic, source-grounded

**Date:** 2026-08-19 · **Sources:** cited inline. Fetched via web search, not asserted from memory
(CLAUDE.md's own rule this file exists to satisfy — an earlier draft of `decisions/ADR-063-
retrieval-augmented-context-provider-shape.md` §2.5 stated these claims inline without this file;
corrected here, ADR updated to cite this instead).

## 1. OpenRouter runs a real, OpenAI-compatible embeddings endpoint

OpenRouter provides `POST https://openrouter.ai/api/v1/embeddings`, an OpenAI-request-shape-
compatible endpoint (`{input, model}` in, an array of vectors out), proxying multiple underlying
embedding providers — OpenAI, Cohere, Google, Mistral — behind one endpoint and one API key. Unlike
chat completions, embeddings are returned as complete responses; **streaming is not supported** on
this endpoint (relevant: `Embedder::embed_batch()`'s design in ADR-063 §2.2 is already non-streaming,
so this is a non-issue for that design, not a constraint it has to work around).

Sources:
- [Embeddings API — OpenRouter docs](https://openrouter.ai/docs/api_reference/embeddings)
- [Text Embedding Models — OpenRouter](https://openrouter.ai/collections/embedding-models)

## 2. Anthropic has no first-party embeddings endpoint

Anthropic does not run its own embedding model or endpoint — Claude is text-generation-only. Anthropic's
own published guidance points to a third-party provider, Voyage AI, as its recommended embeddings
partner (`voyage-3-large` cited as MTEB-leading as of the search date). This is a permanent product
asymmetry, not a temporary gap: there is no Anthropic wire format for an `AnthropicEmbedder` conformer
to implement.

Sources:
- [Embeddings — Claude Platform Docs](https://platform.claude.com/docs/en/build-with-claude/embeddings)

## Relevance to ADR-063

§2.5's `OpenAIEmbedder`-covers-OpenRouter argument and its "no `AnthropicEmbedder`" decision both
rest on these two facts. Both are now cited here instead of inline in the ADR, per CLAUDE.md's
citation rule; the ADR's own claim 4 (§3) is the falsifiable version of point 1 above — a live test
against real `api.openrouter.ai`.

## 3. Request/response wire shape and published batch/dimension limits (added during `OpenAIEmbedder`
   implementation, 2026-08-19)

Fetched directly from OpenAI's current API reference (`developers.openai.com`, the `platform.openai.com`
mirror 403's to an unauthenticated fetch) rather than assumed, since these numbers are exactly the
"real, confirmed provider limit" `EmbedderCapabilities.max_batch_size`/`dimensions` must be populated
from (ADR-063 §2.5, `core/embedder.hpp`'s own top comment) rather than guessed:

- **Request body**: `{"input": <string | string[]>, "model": <string>, "encoding_format"?: "float",
  "dimensions"?: <number>}` — `input` accepts a single string or an array of strings (also arrays of
  token-id arrays, not used here); `dimensions` is documented as "Only supported in `text-embedding-3`
  and later models."
- **Response body** (verbatim shape, `developers.openai.com/api/reference/resources/embeddings`):
  `{"object": "list", "data": [{"object": "embedding", "index": 0, "embedding": [<float>, ...]}, ...],
  "model": <string>, "usage": {"prompt_tokens": <int>, "total_tokens": <int>}}` — one `data[i].embedding`
  per input, in request order (`index` names the position explicitly, so an implementation does not
  have to assume order is preserved, though OpenAI's own docs describe it as such).
- **Published OpenAI limits** (`developers.openai.com/api/reference/resources/embeddings`, quoted
  verbatim): input "must not exceed the max input tokens for the model (8192 tokens for all embedding
  models)"; "any array must be 2048 dimensions or less" (i.e. **max 2048 items in the `input` array
  per request**); "maximum of 300,000 tokens summed across all inputs in a single request." Per-model
  embedding vector length (`developers.openai.com/api/docs/guides/embeddings`, plus corroborating
  secondary sources — OpenAI's own `openai.com/index/new-embedding-models-and-api-updates` announcement
  and Pinecone's writeup, both consistent): `text-embedding-3-small` → **1536 dimensions**,
  `text-embedding-3-large` → **3072 dimensions** by default (both support a `dimensions` request
  parameter to shorten the vector, e.g. `text-embedding-3-large` truncated to 256 still reportedly
  outperforms the older `text-embedding-ada-002` at its native 1536 per the same sources — a quality
  claim, not a wire-shape fact, noted only for context).
- **OpenRouter's own documented limits, gap named honestly**: `openrouter.ai/docs/api_reference/
  embeddings` documents the request/response shape (`{input, model}` in, `data[].embedding` out,
  confirmed compatible with the OpenAI shape above) but **does not publish an explicit max batch size
  or max-tokens-per-request number anywhere in the fetched page** — only the general note "Each model
  has a maximum input length (context window). Longer texts may need to be chunked or truncated."
  Community/secondary sources (not OpenRouter's own docs) suggest some proxied embedding models cap
  around 96 inputs per request, but this is unconfirmed against an authoritative source and NOT cited
  as fact here. **Conclusion for `OpenAIEmbedder`'s implementation**: `EmbedderCapabilities` is a
  constructor argument the CALLER supplies (mirroring `OpenAIChatClient`'s own `ChatClientCapabilities
  caps` constructor parameter) — the header itself hardcodes no batch-size/dimension number for either
  host, since OpenRouter's own limit is unconfirmed and even OpenAI's confirmed 2048/8192/300000
  figures are model- and deployment-specific (a caller pointing at a proxy or self-hosted OpenAI-
  compatible server needs to declare whatever THAT endpoint actually enforces, not a value baked into
  this project's source).

Sources (this section):
- [Embeddings API — OpenRouter docs](https://openrouter.ai/docs/api_reference/embeddings)
- [Embeddings guide — OpenAI API docs](https://developers.openai.com/api/docs/guides/embeddings)
- [Embeddings resource reference — OpenAI API docs](https://developers.openai.com/api/reference/resources/embeddings)
- [New embedding models and API updates — OpenAI](https://openai.com/index/new-embedding-models-and-api-updates/)
