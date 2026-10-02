# Batch inference across providers: limits, expiry, and what each one rejects

**Date:** 2026-10-02. **Why:** OQ-20 (coalescing concurrent single-shot model calls onto one vendor
batch job) depends on what each provider actually accepts. The 2026-08-13 doc
(`2026-08-13-vendor-batch-inference-apis.md`) covered only OpenAI and Anthropic, and
`2026-08-21-openrouter-batch-api.md` covered OpenRouter while it was in beta. This doc re-fetches both and
adds eleven more providers. Every number below was read from the official page listed under it, fetched on
2026-10-02. **UNVERIFIED** means the page did not state it. Do not fill those gaps from memory.

## Corrections to the earlier docs

| Earlier claim | As of 2026-10-02 |
|---|---|
| OpenAI batches are polling-only (2026-08-13) | **Wrong.** OpenAI has `batch.completed` / `batch.failed` / `batch.cancelled` / `batch.expired` webhooks. |
| OpenRouter batch is beta at `/api/beta/batches` (2026-08-21) | **GA on 2026-09-22** at `POST /api/v1/batches`. |
| OpenAI 50,000 requests / 200 MB / 2,000 creations per hour / 24h | Unchanged. New: `output_expires_after` can be set from 1h to 30d. |
| Anthropic 100,000 / 256 MB / 24h expiry / 29-day retention / 50% / `custom_id` `^[a-zA-Z0-9_-]{1,64}$` | Unchanged. New: published queue limits per tier, and the server-tool loop runs inside a batch request. |

## Per provider

### OpenAI
Sources: <https://developers.openai.com/api/docs/guides/batch>,
<https://developers.openai.com/api/reference/resources/batches/methods/create>,
<https://developers.openai.com/api/reference/resources/webhooks>,
<https://developers.openai.com/api/docs/guides/your-data>

- **Limits:** "up to 50,000 requests"; the input file can be "up to 200 MB"; "up to 2,000 batches per hour".
  - Each model has a queue limit on prompt tokens. Its value sits behind the Platform Settings login (UNVERIFIED).
- **Shape:** upload a JSONL file, then create the batch. "Each input file can only include requests to a single model."
- **Window:** "only 24h". On expiry, "unfinished requests within that batch are cancelled, and any responses to
  completed requests are made available".
- **Discount:** 50%.
- **Retention:** output is deleted 30 days after completion, or you can set it from 1h to 30d.
- **Ordering:** "the output line order may not match the input line order".
- **Signal:** polling or webhooks.
- **Cancel:** the batch stays in `cancelling` for up to 10 minutes, and partial results are kept.
- **Rejected:** `stream=true`. Tools and images: UNVERIFIED on the page.
- **ZDR:** `/v1/batches` is "Zero Data Retention eligible: No".

### Azure OpenAI (Global / Data Zone Batch)
Sources: <https://learn.microsoft.com/en-us/azure/foundry/openai/how-to/batch>,
<https://learn.microsoft.com/en-us/azure/foundry/openai/how-to/webhooks>

- **Limits:** 100,000 requests per file; 200 MB, or 1 GB when you supply your own Blob storage.
  - There is an enqueued-token quota per model and subscription tier, e.g. gpt-4.1 Global: 5B for Enterprise, 200M default, 50M for credit-card subscriptions.
- **Window:** the target is 24h, but "it doesn't expire jobs that take longer". The same page still lists an `expired` status.
- **Discount:** 50%.
- **Retention:** job records are kept 365 days. Files have no default expiry, but one can be set from 14 to 30 days.
- **Ordering:** not guaranteed.
- **Signal:** polling or webhooks.
- **Batch must target one deployment:** a mismatch is rejected with the `model_mismatch` error.
- **Structured outputs and images:** both are shown in the examples.

### Anthropic (Message Batches)
Sources: <https://platform.claude.com/docs/en/build-with-claude/batch-processing>,
<https://platform.claude.com/docs/en/api/rate-limits>,
<https://platform.claude.com/docs/en/manage-claude/api-and-data-retention>

- **Limits:** "either 100,000 Message requests or 256 MB in size, whichever is reached first".
  - The queue limit is shared across models: 200k (Start), 300k (Build) or 500k (Scale) requests waiting.
- **Shape:** inline JSON `requests[]` of `{custom_id, params}`.
  - `custom_id` must match `^[a-zA-Z0-9_-]{1,64}$`, so **no `:` and no more than 64 characters**.
- **Window:** "most batches completing within 1 hour… Batches expire if processing does not complete within 24 hours."
  - Each request finishes as `succeeded`, `errored`, `canceled` or `expired`. Expired and errored requests are not billed.
- **Discount:** 50%. It stacks with prompt caching, but cache hits are best-effort, between 30% and 98%.
- **Retention:** 29 days.
- **Ordering:** "any order".
- **Signal:** polling only.
- **Supported:** vision, tools including server tools, multi-turn input, extended thinking.
- **Rejected:** `stream`, `speed`, and `max_tokens: 0`.
- **Tool loop:** client tools do not loop; the item ends on `tool_use`. Server tools do loop inside the item, and can return `pause_turn`.
- **ZDR:** "No… 29-day retention". Using batch "is a choice to step outside your ZDR arrangement".

### AWS Bedrock (batch inference, Claude models)
Sources: <https://docs.aws.amazon.com/bedrock/latest/userguide/batch-inference.html>,
<https://docs.aws.amazon.com/bedrock/latest/userguide/batch-inference-data.html>,
<https://docs.aws.amazon.com/general/latest/gr/bedrock.html>,
<https://docs.aws.amazon.com/bedrock/latest/APIReference/API_CreateModelInvocationJob.html>

- **Limits:** a **minimum of 100 records per job** (not adjustable), and a maximum of 100,000 (adjustable).
  - 1 GB per input file and 5 GB per job.
  - 100 jobs in progress or submitted.
- **Shape:** JSONL in S3, with `{recordId, modelInput}` per line.
- **Window:** `timeoutDurationInHours` can be set from 24 to 168.
  - A job finishes as `PartiallyCompleted` (completed records kept) or `Expired` (never started).
- **Discount:** 50%.
- **Ordering:** "not guaranteed".
- **Signal:** polling or EventBridge.
- **Rejected:** "Batch inference does not support tool calling (function calling) or structured output".
- **Model coverage:** Claude Opus 5.5 and Sonnet 5/5.5 are not in the batch model table (UNVERIFIED).

### Google Vertex AI ("Gemini Enterprise Agent Platform")
Sources: <https://docs.cloud.google.com/gemini-enterprise-agent-platform/models/partner-models/claude/batch>,
<https://docs.cloud.google.com/gemini-enterprise-agent-platform/models/capabilities/batch-inference>

- **Claude:** 4 concurrent batch jobs per project by default. Input is BigQuery or GCS JSONL with `custom_id`.
  - Results are available once every row completes or after 24h.
  - The global endpoint is not supported.
  - Size limit and discount: UNVERIFIED.
- **Gemini:** up to 200,000 requests and 1 GB per GCS file.
  - A job waits up to 72h in the queue; once running, it is cancelled after 24h and you pay only for completed requests.
  - 50% off, and the batch discount does not stack with the cache discount.
  - Rejected: explicit caching, RAG and Provisioned Throughput.

### Google Gemini API (ai.google.dev)
Sources: <https://ai.google.dev/gemini-api/docs/batch-mode>, <https://ai.google.dev/gemini-api/docs/rate-limits>

- **Shape:** inline requests under 20 MB, or a JSONL file up to 2 GB.
- **Limits:** 100 concurrent batch jobs; 20 GB of file storage.
  - The enqueued-token limit depends on model and tier, e.g. Gemini 3.8 Flash: 3M (Tier 1), 400M (Tier 2), 1B (Tier 3).
  - Maximum requests per batch: UNVERIFIED.
- **Window:** 24h target; the job is `JOB_STATE_EXPIRED` after 48h, with no results.
- **Discount:** 50%.
- **Retention:** 6 weeks.
- **Signal:** polling **or webhook** (`batch.succeeded`).
- **Not idempotent:** "If you send the same creation request twice, two separate batch jobs will be created."
- **Supported:** tools, structured output and context caching.

### OpenRouter
Sources: <https://openrouter.ai/docs/batch-quickstart>,
<https://openrouter.ai/docs/api/api-reference/batch/create-a-batch>,
<https://openrouter.ai/blog/announcements/batch-api/>

- **Status:** GA on 2026-09-22.
- **Shape:** `POST /api/v1/batches` with an inline JSON body carrying **one `endpoint` and one `model` per batch**.
- **Limits:** a 200 MB payload. A 402 error rejects the batch if its estimated cost exceeds your balance. Maximum requests per batch: UNVERIFIED.
- **Window:** 24h is the only accepted value. In the beta, the median batch finished in 7 minutes and 99% finished within 10.3h.
- **Discount:** about 50%.
- **Routing:** every request in a batch runs on one provider, chosen once at submit time.
- **Retention:** 30 days.
- **Signal:** polling.
- **Rejected:** streaming, audio/video, **inline base64 images and files** (they must be public URLs), and the web plugin.
  - On Google models, the whole batch must share one `response_format`.
- **Per-model support:** batch support is a capability of each model, separate from synchronous support (confirmed live on 2026-08-21: a DeepSeek alias returned 400 because it "does not have a :batch endpoint").
- **GA wire facts** (fetched 2026-10-02 from <https://openrouter.ai/docs/batch-quickstart.md>):
  - `POST /api/v1/batches` with `endpoint`, `model`, `requests`, and `endpoint`/`model` must be serialized before `requests` (the body is stream-parsed; out of order is a 400). One model per batch; a request body's own `model` must match it.
  - `GET /api/v1/batches/{id}`: `validating` → `in_progress` → `finalizing` → `completed`; also `failed`, `expired`, `cancelling`, `cancelled`. Terminal: `completed`, `failed`, `expired`, `cancelled`. `results` is inline and non-null only when `completed`.
  - A request that fails OpenRouter's per-request checks fails the whole batch after the 202.
  - No cancel endpoint. `DELETE /api/v1/batches/{id}` purges a terminal batch (409 while in progress).
  - Unknown request parameters are silently dropped.
  - Provider choice applies the account's provider allowlist, data policy and BYOK settings; `provider.only` is the only routing preference accepted.
- **Measured live 2026-10-02** (AgentEngine's `test_openrouter_batch_live_e2e`):
  - `openai/gpt-4o-mini` is listed in `/api/v1/models` with a `:batch` variant, yet its submit returned 400 "Model 'openai/gpt-4o-mini' does not have a :batch endpoint." for this account. `google/gemini-2.5-flash-lite` was accepted. Being listed is not proof that a given account can batch a model. The cause was not determined; the account's data policy is a candidate, since batch is not ZDR-eligible at the underlying vendors.
  - A `GET` 5s after a successful submit returned 404 "Batch job ... not found."; the next, 10s later, found the job. Read-after-write lag is real and must not be treated as a dead job (ADR-235's `poll_error_grace`).
  - `GET /api/v1/models/{id}/endpoints` does not list `:batch` endpoints, so it cannot tell you whether a model can be batched.

### DeepSeek
Sources: <https://api-docs.deepseek.com/quick_start/pricing>, <https://api-docs.deepseek.com/>

- **No batch API was found** (absence of evidence only).
- **Substitute:** synchronous calls get 50% off during off-peak hours, which are everything outside 01:00–04:00 and 06:00–10:00 UTC on weekdays, excluding Chinese public holidays.
- This is a scheduling discount on ordinary synchronous calls, so streaming and tools are unaffected.

### Mistral
Sources: <https://docs.mistral.ai/studio/batch-processing>, <https://docs.mistral.ai/resources/known-limitations>

- **Shape:** a JSONL file, or inline below 10k requests.
- **Limits:** the docs contradict each other on the maximum: "up to 1 million" on one page, "100,000" on the other. 512 MB per file.
- **Discount:** 50%.
- **Retention:** results are available for only **24h after completion**.
- **Signal:** polling.

### Groq
Source: <https://console.groq.com/docs/batch>

- **Shape:** OpenAI-compatible `/v1/batches` with a file upload.
- **Limits:** 50,000 lines and 200 MB.
- **Window:** settable from 24h to 7d. You are charged only for completed requests.
- **Discount:** 50%.
- **Retention:** 30 days.
- **Ordering:** explicitly not ordered.
- **Signal:** polling.

### Together AI
Source: <https://docs.together.ai/docs/batch-inference>

- **Shape:** a native API, explicitly not OpenAI-compatible.
- **Limits:** 50,000 requests, 100 MB per file, 10 MB per line, and 30B enqueued tokens per model.
- **Window:** 24h, best-effort.
- **Discount:** up to 50%, on selected models.
- **Retention:** 7 days.
- **Ordering:** arbitrary.

### Fireworks
Source: <https://docs.fireworks.ai/guides/batch-inference>

- **Shape:** a native API with a JSONL dataset.
- **Limits:** up to 80 GiB in and 8 GB out.
- **Window:** 12, 24, 48 or 72h; on expiry, completed rows are billed and kept.
- **Discount:** 50%.
- **Resume:** `--continue-from` reprocesses only the unfinished rows.

### xAI
Sources: <https://docs.x.ai/docs/guides/batch-api>, <https://docs.x.ai/developers/pricing>

- **Shape:** inline incremental add, or a file; files are sealed once the batch is created.
- **Limits:** a file holds 50k requests or 200 MB, and each request is capped at 25 MB.
  - Batches over 100k requests may be throttled.
  - 2 batch creations per second.
- **Discount:** **20%**, on selected models only.
- **Tools:** "Multi-turn tool calling requires submitting a new batch request with the tool result messages included."

### Self-hosted: llama.cpp and vLLM
Sources: <https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md>,
<https://docs.vllm.ai/en/latest/cli/run-batch/>

- **llama.cpp:** no `/v1/batches` and no `/v1/files`.
  - `--parallel` slots plus continuous batching give **concurrency for synchronous requests**, not an async job API.
- **vLLM:** no async batch job API on the server.
  - `run-batch` is an offline JSONL runner.
  - `/v1/chat/completions/batch` is synchronous and accepts N conversations at once, with no streaming and no tools.

## Comparison

| Provider | Max requests | Max size | **Min** | Window | Expiry outcome | Discount | Webhook | One model per batch | `custom_id` rule | ZDR |
|---|---|---|---|---|---|---|---|---|---|---|
| OpenAI | 50,000 | 200 MB | — | 24h | partial kept | 50% | yes | yes | none stated | **no** |
| Azure OpenAI | 100,000 | 200 MB / 1 GB | 1 | 24h target, no expiry | runs on | 50% | yes | yes (deployment) | unique | UNVERIFIED |
| Anthropic | 100,000 | 256 MB | — | 24h | per-item `expired`, unbilled | 50% | no | no | **`[A-Za-z0-9_-]{1,64}`** | **no** |
| Bedrock (Claude) | 100,000 | 1 GB file / 5 GB job | **100** | 24–168h | `PartiallyCompleted` | 50% | EventBridge | yes | `recordId` | UNVERIFIED |
| Vertex Gemini | 200,000 | 1 GB | — | 72h queue + 24h | completed billed | 50% | no | yes | — | UNVERIFIED |
| Vertex Claude | UNVERIFIED | UNVERIFIED | — | 24h | results at 24h | UNVERIFIED | no | UNVERIFIED | `custom_id` | UNVERIFIED |
| Gemini API | UNVERIFIED | 20 MB inline / 2 GB file | — | 24h, expires 48h | no results | 50% | **yes** | yes | `key` | UNVERIFIED |
| OpenRouter | UNVERIFIED | 200 MB | 1 | 24h | UNVERIFIED | ~50% | no | **yes** | unique | UNVERIFIED |
| Mistral | 100k or 1M (docs conflict) | 512 MB | — | UNVERIFIED | UNVERIFIED | 50% | no | UNVERIFIED | `custom_id` | UNVERIFIED |
| Groq | 50,000 | 200 MB | — | 24h–7d | charged for completed only | 50% | no | UNVERIFIED | `custom_id` | UNVERIFIED |
| Together | 50,000 | 100 MB | — | 24h | UNVERIFIED | ≤50% | no | UNVERIFIED | `custom_id` | UNVERIFIED |
| Fireworks | UNVERIFIED | 80 GiB | — | 12–72h | completed kept | 50% | no | UNVERIFIED | `custom_id` | UNVERIFIED |
| xAI | 50k per file | 200 MB | — | ~24h | UNVERIFIED | **20%** | no | UNVERIFIED | `batch_request_id` | UNVERIFIED |
| DeepSeek / llama.cpp / vLLM | — no async batch API — | | | | | | | | | |

## What every provider has in common

1. **No client-side tool loop inside a batch item.**
   - xAI states it outright. For Anthropic, a client `tool_use` ends the item. Bedrock rejects tools entirely.
   - Only server-side tools (Anthropic, Gemini, xAI) loop inside the item. This confirms the 2026-08-13 conclusion with more sources.
2. **Result order is never guaranteed.** Every provider that states it says results can come back in any order, so a correlation id is mandatory.
3. **Batch jobs are not data-zero-retention.** OpenAI and Anthropic both explicitly exclude batch from ZDR.

## Tool calling and reasoning inside a batch request

Fetched 2026-10-02.

**How to read this:**
- **EXPLICIT** means a batch page states it.
- **IMPLIED** means it follows from the page's own "a batched `body` is the same as the synchronous request body" rule. It is weaker evidence.
- **UNVERIFIED** means nothing was found.

**Sources:** the same batch pages listed above, plus:
- <https://developers.openai.com/api/docs/models/gpt-5.4>
- <https://platform.claude.com/docs/en/build-with-claude/extended-thinking>
- <https://docs.aws.amazon.com/bedrock/latest/userguide/claude-messages-extended-thinking.html>
- <https://ai.google.dev/gemini-api/docs/gemini-3>
- <https://console.groq.com/docs/reasoning>
- <https://docs.x.ai/docs/guides/reasoning>
- <https://docs.together.ai/docs/inference/batch/tutorial>

| Provider | Tools accepted | Tool calls returned | Tool result → next round as a new item | Server tools (search, code exec) | Reasoning params | Reasoning content returned |
|---|---|---|---|---|---|---|
| OpenAI | IMPLIED | IMPLIED | IMPLIED | IMPLIED | IMPLIED; reasoning models batch-capable EXPLICIT (gpt-5.4 "Batch: Supported") | UNVERIFIED |
| Azure OpenAI | IMPLIED (fields shown in sample output) | IMPLIED | IMPLIED | UNVERIFIED | IMPLIED (o3/o4-mini/gpt-5.x in batch table) | UNVERIFIED |
| Anthropic | **EXPLICIT** | EXPLICIT (item ends on `tool_use`) | IMPLIED ("Multi-turn conversations") | **EXPLICIT**, loops inside the item, may return `pause_turn` | **EXPLICIT** "Extended thinking"; "For thinking budgets above 32k, use batch processing" | IMPLIED (full Message) |
| AWS Bedrock | **NO** (EXPLICIT) | NO | NO | NO | IMPLIED (`modelInput` = InvokeModel body) | UNVERIFIED |
| Gemini API | **EXPLICIT** | IMPLIED | IMPLIED; thought signatures must be echoed back | **EXPLICIT** (`google_search`) | IMPLIED | IMPLIED / UNVERIFIED |
| Vertex (Gemini, Claude) | UNVERIFIED | UNVERIFIED | UNVERIFIED | UNVERIFIED | UNVERIFIED | UNVERIFIED |
| OpenRouter | IMPLIED (`tool_calls` in response schema) | IMPLIED | IMPLIED | provider-native only; "OpenRouter-orchestrated search is not available in batch" | UNVERIFIED | UNVERIFIED |
| Mistral | IMPLIED | IMPLIED | IMPLIED | UNVERIFIED | IMPLIED | UNVERIFIED |
| Groq | IMPLIED | IMPLIED | IMPLIED | UNVERIFIED | IMPLIED | IMPLIED |
| Together | IMPLIED | IMPLIED | IMPLIED | n/a | IMPLIED | UNVERIFIED |
| Fireworks | UNVERIFIED | UNVERIFIED | UNVERIFIED | n/a | UNVERIFIED | UNVERIFIED |
| xAI | **EXPLICIT** | **EXPLICIT** | **EXPLICIT** ("requires submitting a new batch request with the tool result messages") | **EXPLICIT** | IMPLIED / UNVERIFIED | UNVERIFIED |

### Hazards

1. **OpenRouter drops parameters silently.** Its docs say "Unknown parameters are dropped by the provider serializer, matching the sync API." A batched request carrying tools or reasoning settings may therefore come back as plain text with no error. A coalescer has to check that the response is the shape it expected, not just trust that the submit was accepted.
2. **Possible OpenAI clash between tools and reasoning effort.** A search snippet says "Starting with GPT-5.4, Chat Completions does not support tool calling with reasoning_effort values other than none." This is **UNVERIFIED**: the sentence was not found on the model page itself. If it is true, a batch combining tools and reasoning effort would have to target `/v1/responses` instead.
3. **Anthropic `budget_tokens` returns 400 on Claude 4.7 and later.** Batch requests for those models must use adaptive thinking, which is the same rule as synchronous calls.
4. **Anthropic has a batch-only 300k-token output beta** (`output-300k-2026-03-24`). It is not available on Bedrock, Vertex or Foundry. Anthropic warns "A single 300k-token generation can take over an hour."
5. **Gemini thought signatures and Anthropic thinking signatures have to be carried forward.** If a tool loop is continued as new batch items, each next-round item must include the signed reasoning from the previous round.
