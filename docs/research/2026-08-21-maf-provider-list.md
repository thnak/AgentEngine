# MAF's first-party model-provider list, and what it means for AgentEngine's own two backends

**Date:** 2026-08-21. **Sources:**
`https://learn.microsoft.com/en-us/agent-framework/integrations/by-component/model-providers/`
(page `ms.date` 2026-07-30, last updated 2026-08-10 — the canonical, per-language C#/Python/Go
provider table), corroborated by
`https://devblogs.microsoft.com/agent-framework/microsoft-agent-framework-version-1-0/` and
`https://github.com/microsoft/agent-framework`. Context providers (vector-store/memory, a separate
MAF component) checked at
`https://learn.microsoft.com/en-us/agent-framework/integrations/by-component/context-providers/`.
Per `CLAUDE.md`'s research discipline: fetched and cited, not asserted from memory. Supersedes the
provider-count portion of `docs/research/2026-maf-provider-concepts.md` (2026-07-31, a local
source-checkout note that only found `agent_framework_openai`/`agent_framework_anthropic` — this doc
is the current, docs-site-sourced, complete list).

## MAF's genuine first-party chat-client/connector classes

Separate provider-specific packages, not "any OpenAI-compatible endpoint via base URL override":

1. **OpenAI** — dedicated connector (Chat Completions + Responses API).
2. **Azure OpenAI** — dedicated connector, Azure identity/managed-identity auth.
3. **Microsoft Foundry** (Azure AI Foundry) — dedicated connector via the Azure AI Projects SDK.
4. **Foundry Local** — separate connector for locally-run Foundry models (C#/Python).
5. **Anthropic** — genuine first-party client (not OpenAI-compat shimmed). The Python variant also
   reaches Claude through Anthropic-on-Foundry, Bedrock, or Vertex AI as alternate backends.
6. **Ollama** — dedicated local/open-source model connector.
7. **Amazon Bedrock** — dedicated AWS-managed connector.
8. **Google Gemini** — dedicated connector (Gemini Developer API or Vertex AI).
9. **ONNX** (C# only) — local ONNX Runtime GenAI inference (on-device).
10. **Dapr** (C# only) — routes inference through Dapr's Conversation building block; a meta/transport
    layer over other providers, not a distinct model vendor.
11. **Go SDK** ships a narrower subset: Foundry, OpenAI, Anthropic, Gemini only.

**Listed but not a real chat provider:** Mistral appears in the model-providers nav/table, but the doc
scopes it to **embedding generation only** (Python); its C# capability row is all `N/A`.

**Not present as first-party anywhere in MAF's docs:** HuggingFace, Cohere, xAI/Grok, DeepSeek,
OpenRouter, NVIDIA NIM, watsonx. MAF's own stated fallback for these: *"Any inference service that
provides a `Microsoft.Extensions.AI.IChatClient` implementation can back a `ChatClientAgent`"* — the
same generic-adapter pattern AgentEngine's own OpenAI-compatible backend already is, not a distinct
connector class.

## What this means for AgentEngine

AgentEngine ships two `ChatClient` backends today (004-Model-Provider-Plane.md §3): an
**OpenAI-compatible** one (real `api.openai.com` and any OpenAI-compatible endpoint via host/path
override — OpenRouter, vLLM/llama.cpp/Ollama-style local servers, most vendor compat endpoints, per
that table's own "Reach" column) and a real **Anthropic** one.

Splitting MAF's list against that architecture:

- **Already reachable today, for free, via the OpenAI-compatible backend's host/path-prefix override**
  (same mechanism `cli_chat.cpp`'s `openrouter` provider and this session's `openai/gpt-4o-mini`-via-
  OpenRouter tests already exercise): any vendor whose REST surface is a genuine OpenAI-Chat-
  Completions-shaped endpoint authenticated with a plain `Authorization: Bearer <key>` header. Ollama
  is the clearest case (its own OpenAI-compat endpoint) — likely already reachable without new code,
  unverified here (would need a live check against a real Ollama instance to confirm, not asserted).
- **Genuinely need new work** — different auth mechanism or wire envelope than a Bearer-token
  OpenAI-shaped POST, so a host-override trick on the existing `OpenAIChatClient` cannot reach them:
  - **Azure OpenAI** — traditionally authenticates via an `api-key` header (not `Authorization:
    Bearer`) or Azure AD/managed-identity tokens; AgentEngine's `OpenAIChatClient::build_http_request`
    hardcodes the `Authorization: Bearer` header today, so this needs either a header-shape option or
    a distinct thin wrapper.
  - **Amazon Bedrock** — AWS SigV4 request signing plus a per-model-family JSON envelope (not a
    uniform OpenAI-shaped body); a genuinely different backend.
  - **Google Vertex AI** / **Microsoft Foundry** — GCP/Azure OAuth2 service-account token flows, not a
    static API-key secret; AgentEngine's `SecretRef`/`SecretStore` seam (018 §4) is built around a
    resolved credential string, not a token-refresh flow, so this is new design surface, not just a
    new backend.
  - **ONNX** — on-device inference, not an HTTP backend at all; a different `ChatClient` conformer
    shape entirely (closer to the embedded-CPython precedent than to a network backend).
  - **Google Gemini (Developer API)** — Google does publish an OpenAI-compatible endpoint
    (`generativelanguage.googleapis.com/.../openai/`) authenticated with a Bearer-style key; whether
    that specific surface is reachable via the existing host-override path is plausible but NOT
    verified live here — flagged as a concrete, cheap thing to check before claiming it "already
    works," per this project's own "a test that cannot fail proves nothing" discipline.

## Net comparison

AgentEngine's "one seam, `ChatClient`, modeled on declared capabilities rather than any vendor's
request shape" design (004's own framing) already reaches a wider set of vendors per line of code
than MAF's one-package-per-vendor model does, for anything that is genuinely OpenAI-Chat-Completions-
shaped over a Bearer token. What AgentEngine does NOT have that MAF does are the providers whose
*auth mechanism* differs from a static bearer token (Azure identity, AWS SigV4, GCP OAuth2) or whose
transport isn't HTTP at all (ONNX on-device) — those are real, separately-scoped gaps, not something
a host-override trick closes.
