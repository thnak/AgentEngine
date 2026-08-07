# 2026-08-07 — Provider request/response metadata & sampling-parameter survey

**Status:** Research + a concrete, NOT-YET-BUILT proposed design. Written so this doesn't get lost
before the in-flight QA test work (tool calls / streaming / structured output, Milestone 5 Phases
D/E) lands — pick this back up once that's merged and reviewed. Nothing in this document is
implemented; it is the starting point for a follow-up task, not a changelog.

**Question:** what request/response metadata and sampling-parameter fields do real OpenAI-compatible
and Anthropic-compatible surfaces support beyond what `agentengine::ChatRequest`/
`ChatClientCapabilities` (`include/agentengine/core/chat_client.hpp`) carry today, and which of them
are safe to add without relitigating 004 §1's own deliberate elision?

## Current AgentEngine state (verified against code, 2026-08-07)

`ChatRequest` (`chat_client.hpp:57-69`) carries `messages`, `tools`, `output_schema_json` — nothing
else. Its own file-top comment (`chat_client.hpp:9-13`) states the reason explicitly: *"Sampling
parameters (temperature, top_p, ...) stay elided — no RFC section this project has built against
names a concrete shape for them yet, and no M5 gate needs one; naming this here rather than silently
dropping it."* `ChatClientCapabilities` (`chat_client.hpp:34-55`) has 18 declared capability bits plus
`context_window`/`max_output_tokens` — no bit for reasoning-effort levels, seed support, or app
attribution. Neither `OpenAIChatClient` (`include/agentengine/protocol/openai/chat_client.hpp`) nor
`AnthropicChatClient` (`include/agentengine/protocol/anthropic/chat_client.hpp`, both Milestone 5
Phases D/E) construct any of the fields surveyed below.

## Sources

- Local SDK repos (same rigor Phase D/E's own wire-format research used — ground truth from generated
  serialization code, not documentation paraphrase):
  - `D:\GitSrc\openai-dotnet\OpenAI\src\Generated\Models\Chat\ChatCompletionOptions.Serialization.cs`
    (metadata/user/safety_identifier/reasoning_effort/seed wire property names)
  - `D:\GitSrc\openai-dotnet\OpenAI\src\Custom\Chat\ChatCompletionOptions.cs`
  - `D:\GitSrc\openai-dotnet\OpenAI\src\Generated\Models\Chat\ChatReasoningEffortLevel.cs`
    (enum wire values: `none`/`minimal`/`low`/`medium`/`high`)
  - `D:\GitSrc\anthropic-sdk-csharp\src\Anthropic\Models\Messages\Metadata.cs` (the ONLY field is
    `user_id`, an opaque abuse-detection identifier — explicitly documented "do not include any
    identifying information")
- Live docs, fetched/searched 2026-08-07 (dated per CLAUDE.md's research discipline — these move):
  - <https://openrouter.ai/docs/api-reference/overview>
  - <https://openrouter.ai/docs/app-attribution>
  - <https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md>
  - vLLM docs (`docs.vllm.ai/en/v0.21.0/serving/openai_compatible_server/`,
    `docs.vllm.ai/en/v0.8.2/features/structured_outputs.html`), found via web search
  - Ollama OpenAI-compatibility docs/GitHub issues (`ollama.com/blog/openai-compatibility`,
    `github.com/ollama/ollama/issues/14820`), found via web search

## Finding 1 — App/agent attribution is a GATEWAY convention, not a Chat-Completions field

No field in the OpenAI or Anthropic wire spec identifies the calling application. This is entirely
an **OpenRouter-specific HTTP header** convention (confirmed by two independent fetches of
OpenRouter's own docs, agreeing on the same names):

| Header | Required? | Purpose |
|---|---|---|
| `HTTP-Referer` | yes, for leaderboard ranking | app's URL, primary identifier |
| `X-OpenRouter-Title` (legacy alias `X-Title`) | no | display name in rankings/analytics |
| `X-OpenRouter-Categories` | no | up to 2 marketplace category tags per request |

No other surveyed backend (OpenAI, Anthropic, llama.cpp, vLLM, Ollama) has an equivalent. This is
**headers, not JSON body** — mechanically the simplest possible addition, since `NetEgressRequest.
headers` (`sandbox/net_egress_proxy.hpp`) already carries arbitrary key/value pairs and
`detail::build_http_request` in both backends already constructs them per-call.

## Finding 2 — sampling/config knobs are NOT portable across vendors

| Field | OpenAI | Anthropic | llama.cpp server | vLLM | Ollama |
|---|---|---|---|---|---|
| `temperature`/`top_p` | yes (`temperature` now deprecated on newest reasoning models) | yes | yes | yes | yes |
| `top_k` | **not in spec at all** | yes | yes | yes | yes |
| `seed` | yes, best-effort determinism | **no native field** | yes | yes | unconfirmed/undocumented |
| reasoning effort | `reasoning_effort`: flat string enum `none`/`minimal`/`low`/`medium`/`high` | **different mechanism**: `thinking: {type, budget_tokens}` — a token BUDGET, not a level | `reasoning_effort` (+ its own `reasoning_format`/`reasoning_control`, `none` disables) | inherited via OpenAI-compat | maps `reasoning_effort` into Ollama's own "think" levels — only `high`/`medium`/`low`/`none`, **no `minimal`** |
| abuse-tracking id | `user` (legacy string) / `safety_identifier` (newer string) | `metadata.user_id` (opaque, PII forbidden) | — | — | — |
| generic metadata | `metadata: {k:v,...}` free-form string map | **narrower**: only `user_id` exists, no general map | — | — | — |

**The load-bearing finding**: `reasoning_effort` is not one concept with three spellings — OpenAI's
flat enum, Anthropic's token-budget object, and Ollama's narrower 4-value subset are three genuinely
different shapes. A portable `ChatRequest.reasoning_effort` field would be exactly "one vendor's shape
leaking onto every other backend," the trap 004 §8 Q2's own resolution already named and avoided for
prompt caching (`docs/planning/milestone-5-providers-identity-secrets-breakdown.md` decision, referenced
in `AnthropicChatClient`'s own file-top comment point (4)). The same reasoning applies here.

## Finding 3 — a real, untested structured-output compatibility risk

`OpenAIChatClient::translate_output_schema` (Phase D4) always emits OpenAI's full wrapper:
`{"type":"json_schema","json_schema":{"name","schema","strict"}}`. llama.cpp's own server docs show a
**simpler** shape: `{"type":"json_schema","schema":{...}}` — no `name`/`strict`, no nested
`json_schema` wrapper, `schema` directly at the top level. This has never been verified against a real
llama.cpp instance (Phase D/E's own live verification used OpenRouter, which proxies to hosted models,
not a local llama.cpp server) — it is a genuine, open risk that the current translation could silently
misbehave (extra ignored keys, or a rejected request) against a real llama.cpp-backed endpoint. vLLM,
by contrast, documents `response_format={"type":"json_schema",...}` matching OpenAI's shape more
directly (it explicitly targets OpenAI API compatibility), so this risk is llama.cpp-specific as far as
this survey went.

## Finding 4 — response-side fields `ChatResponse` doesn't capture today

`ChatResponse` (`chat_client.hpp:71-74`) is just `{message, usage}`. `Usage` (`core/content.hpp`) is
`{input_tokens, output_tokens, cached_input_tokens, reasoning_tokens, cost_estimate}`. Both wire
formats carry more that never reaches the caller:

- **`model`** — both OpenAI and Anthropic responses report the model that ACTUALLY answered (confirmed
  in Phase D/E's own wire-format research and directly observed in the live-probe diagnostic dump —
  `"model":"google/gemini-3.5-flash-lite"` came back from an `openrouter/auto` request). Currently
  invisible to the caller. This matters most exactly when routing isn't pinned to one model — OpenRouter
  `auto`, fallback lists (`models[]`, Finding-1's own OpenRouter extension table), or any gateway that
  can silently substitute a different backend — and it also matters for correlating cache-hit behavior
  (Finding 5) with which concrete backend served a given call.
- **`system_fingerprint`** (OpenAI only, optional) — identifies the exact backend/model configuration
  snapshot that served the request. Not a cache-hit LEVER by itself, but useful observability: a
  fingerprint change between two calls that were expected to hit the same cache explains an otherwise
  mysterious cache miss.
- **`id`** (both) / **`created`** (both, Unix timestamp) — request/response correlation id and server
  timestamp. Useful for support/tracing, not currently threaded anywhere (004 §1's own attributability
  invariant, I4, is presently satisfied by `EffectContext`'s `trace_id`/`span_id` on the AgentEngine
  side, not by threading the vendor's own response id back out — worth deciding whether that's
  sufficient or whether the vendor id should also be preserved).

## Finding 5 — concrete, provider-specific cache-hit-rate levers

Beyond "keep the stable content first, volatile content last" (already implicit in 005 §3's own
context-assembly ordering, `docs/planning/milestone-5-providers-identity-secrets-breakdown.md`'s
decision on 004 §8 Q2), each vendor has REAL, documented levers:

- **OpenAI**: caching is fully automatic (no `cache_control`-equivalent needed) for any request ≥1024
  prompt tokens, in 128-token increments beyond that floor. Default eviction is 5–10 minutes idle (up
  to ~1 hour off-peak) for pre-GPT-5.6 models; GPT-5.6+ models keep a cached prefix eligible for reuse
  for **at least 30 minutes**. `prompt_cache_key` (string) is documented at the general API-guide level
  as *"a routing hint... requests with the same cache key are routed to the same backend, increasing
  the likelihood of a cache hit"* — **but** in the locally-vendored `openai-dotnet` SDK snapshot, this
  field is wired ONLY into the newer Responses API (`OpenAI.Responses\src\Generated\Models\
  CreateResponseOptions.cs`), not into `ChatCompletionOptions` (Chat Completions, the endpoint
  `OpenAIChatClient` actually targets). **Unconfirmed**: whether the raw Chat Completions wire endpoint
  accepts `prompt_cache_key` even though this SDK's typed Chat Completions model doesn't expose it (SDKs
  routinely lag the raw API) — needs a live check before relying on it, named explicitly rather than
  assumed either way. The Responses API also has `prompt_cache_retention`: `"in_memory"` (default) or
  `"24h"` (extended retention for sparser traffic) — a genuine cache-hit-rate lever, but Responses-API
  only, unreachable from `OpenAIChatClient`'s current Chat-Completions-only scope.
- **Anthropic**: caching is fully explicit via `cache_control` breakpoints (already built, Phase E3) —
  no automatic floor, but a **hard cap of 4 `cache_control` blocks per request, counted across system +
  tools + messages COMBINED** (confirmed via a real reported bug: exceeding it returns `HTTP 400: A
  maximum of 4 blocks with cache_control may be provided`). `AnthropicChatClient::build_request_body`
  today places at most 2 (system + last tool) — safely under the cap — but this is a REAL, testable
  invariant to enforce if per-message or additional breakpoints are ever added later, not just a soft
  guideline. TTL choice (`ttl: "5m"` default vs `"1h"`, `detail::translate_tool`/system-block cache_control
  construction) is itself a lever this project doesn't expose yet — always emits the bare
  `{"type":"ephemeral"}` (5-minute default) today.
- **OpenRouter (gateway layer, relevant since it's what Phase D/E's own live verification used)**:
  translates cache hints between vendor shapes for you (*"a text block marked with Anthropic-style
  `cache_control` gets a `prompt_cache_breakpoint` when routed to a supporting OpenAI model"*) and does
  its own **provider sticky routing** — automatically routing a session's subsequent calls to the same
  concrete provider endpoint after a cache hit, specifically to keep the cache warm — UNLESS a caller
  pins `provider.order` explicitly (Finding 1's own OpenRouter request-extension table), which disables
  sticky routing in favor of the caller's explicit choice. Response-side, OpenRouter's own
  `usage.prompt_tokens_details` carries `cached_tokens` (a cache hit — already directly observed in the
  live-probe diagnostic dump this session) PLUS `cache_write_tokens` and `cache_discount`, neither of
  which `agentengine::Usage` has a field for today (only `cached_input_tokens` exists, mapped from
  `cached_tokens`/`cache_read_input_tokens` already in Phase D/E's own response parsing).

## Recommended design — **items 1/2/4/5/6/7 DONE (2026-08-07), items 3/8 still deferred**

Items 1, 2, 4, 5, 6, 7 below are now implemented, on both `OpenAIChatClient` and `AnthropicChatClient`
(Anthropic additionally got item 6/7 since they're Anthropic-specific), proven in both backends'
translation and live test files. Two parallel subagents built the two backends independently against
this same doc; both, unprompted, hit and correctly avoided the exact "insert a new field/parameter in
the middle and silently break every existing positional call site" bug named in the `Usage.
cache_write_tokens` amendment above — every new constructor parameter on both backends is APPENDED
after the previous last parameter (`ca_bundle_pem_override`), never inserted earlier.

1. ~~**App attribution headers**~~ — **Done.** `http_referer`/`x_title` optional constructor params on
   both backends, stamped into `HTTP-Referer`/`X-Title` headers only when non-empty.
2. ~~**`seed`/`metadata`/`user`/`safety_identifier`**~~ — **Done, per-backend as scoped.** OpenAI:
   `end_user_id` (→ `"user"`) and `seed` (→ `"seed"`) constructor params. Anthropic: `end_user_id` only
   (→ `metadata.user_id`) — **no `seed` param exists on `AnthropicChatClient`**, deliberately: Finding 2
   confirmed Anthropic has no native seed field at all, and a fake no-op parameter would be worse than
   its honest absence.
3. **`reasoning_effort`** — still deferred, unchanged from the original recommendation below. Not
   implemented.
4. ~~**Expose `model` on `ChatResponse`**~~ — **Done.** `ChatResponse` gained a `model` field (appended
   last, after `usage`); both backends' response parsers populate it from the wire's own top-level
   `"model"` field, empty when absent. Landed as a prerequisite core-type change (`003-Message-and-
   Content-Model.md` needed no amendment — `ChatResponse`'s field list isn't literally declared there,
   unlike `Usage`) ahead of both backend implementations.
5. ~~**Widen `Usage` with a `cache_write_tokens`-equivalent field**~~ — **Done.** `Usage` gained
   `cache_write_tokens` (appended LAST, after `cost_estimate` — see the field's own code comment and
   `003-Message-and-Content-Model.md` §6's dated amendment for why: inserting it earlier broke existing
   positional `Usage{a,b,c,d,e}` call sites with a real compile error, caught and fixed before either
   backend agent started). OpenAI maps OpenRouter's `usage.prompt_tokens_details.cache_write_tokens`;
   Anthropic maps `usage.cache_creation_input_tokens`.
6. ~~**A hard invariant, not a feature**~~ — **Done.** `AnthropicChatClient::detail::
   count_cache_control_blocks` does a generic recursive walk of the assembled request body counting
   every `cache_control` key at any depth (not hardcoded to the two known placement sites, so it stays
   correct if more are added later) — `build_request_body` fails closed
   (`anthropic.cache_control_limit_exceeded`) if the count exceeds 4. Directly unit-tested against a
   hand-built 5-block body (unreachable through the public API as designed, so this is the only way to
   exercise the failure path) plus the real 0/1/2-block cases proving normal use still succeeds.
7. ~~**Anthropic cache TTL as a constructor option**~~ — **Done.** `cache_ttl` constructor param
   (`""`/`"5m"`/`"1h"`), validated at CONSTRUCTION time (`detail::is_valid_cache_ttl`; an invalid value
   throws `std::invalid_argument` — a cold-setup-path exception per CONVENTIONS.md, since no
   runtime-assert or validated-setter precedent existed elsewhere in this codebase to follow instead),
   applied via a new shared `make_cache_control(cache_ttl)` helper to every `cache_control` object this
   backend builds.
8. **`prompt_cache_key`** — still deferred, unchanged from the original recommendation below. Not
   implemented.

Original recommendation text for the two still-deferred items, preserved for when they're picked back
up:

- **`reasoning_effort`** needs a real design decision FIRST, not a quick add. Options to weigh: (a)
  leave elided permanently, matching temperature/top_p's own status quo; (b) add it as a
  backend-constructor-local field per backend (no portability claim, same shape as the now-done items
  above — simplest, but an agent author can't express "I want low reasoning effort" once, has to know
  which backend is bound); (c) a real RFC 004 amendment defining a portable, coarser tri-state (e.g.
  `low`/`medium`/`high` only, the subset every surveyed vendor actually agrees on) with each backend
  mapping down to its own native shape — the RFC-touching option, needs the project's own
  design→red-team→prove→judge discipline before landing since it's a genuine 004 amendment, not
  backend-internal translation.
- **`prompt_cache_key`** — explicitly NOT recommended yet. Confirmed wired only into OpenAI's Responses
  API in the locally-vendored SDK, not Chat Completions (the endpoint this project's `OpenAIChatClient`
  targets) — whether the raw Chat Completions wire endpoint accepts it anyway is unconfirmed. Needs a
  live check before treating this as available at all.

## Explicitly deferred / open verification items

- **llama.cpp structured-output shape mismatch** (Finding 3) — untested against a real llama.cpp
  server. Needs either a live llama.cpp instance to verify against, or authoritative confirmation that
  llama.cpp's `/v1/chat/completions` endpoint tolerates the extra `name`/`strict` keys and the
  `json_schema` nesting OpenAI's own shape uses (plausible, since many llama.cpp deployments front an
  OpenAI-shaped client library that always sends the fuller shape — but not verified here).
- **vLLM/Ollama live verification** — this survey is documentation-only for these two; Phase D/E's own
  live verification only exercised OpenRouter (a hosted gateway), never a self-hosted llama.cpp/vLLM/
  Ollama instance directly. `004-Model-Provider-Plane.md §3`'s own table lists "Local / embedded" (an
  OpenAI-compatible local server) as real future reach, not required for the M5 exit gate — this survey
  is preparatory research for that row, not a claim it's been proven.
- **Ollama `seed` support** — genuinely undocumented as of this survey; needs either a live check or a
  more authoritative source before assuming either way.
- **`prompt_cache_key` on the raw Chat Completions wire endpoint** — the locally-vendored `openai-dotnet`
  SDK only types it for the Responses API; whether the underlying Chat Completions HTTP endpoint itself
  accepts and honors it is unconfirmed (SDK typed-model coverage lags the raw API routinely). Needs a
  live check before either building against it or ruling it out.
- **Response-side `model` field for llama.cpp/vLLM/Ollama** — Finding 4's `model` recommendation was
  confirmed against OpenAI/Anthropic/OpenRouter responses directly; not independently verified for the
  three self-hosted servers (though all three explicitly target OpenAI Chat Completions response-shape
  compatibility, so this is very likely fine — just not independently checked here).
