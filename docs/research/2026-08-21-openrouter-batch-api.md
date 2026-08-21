# OpenRouter has a Batch API (beta) — how it differs from OpenAI's and Anthropic's

**Date:** 2026-08-21. **Source:** `https://openrouter.ai/docs/batch-quickstart` (fetched 2026-08-21).
Per `CLAUDE.md`'s research discipline: do not assert what a vendor API does from memory. Companion to
`docs/research/2026-08-13-vendor-batch-inference-apis.md` (OpenAI's and Anthropic's own batch APIs),
which did not cover OpenRouter — this doc closes that gap, prompted directly by the question "does
OpenRouter have a batch API at all" while scoping a standalone (non-`AgentSession`-integrated) batch
inference tool for `tools/`.

## The shape

- **Endpoint:** `POST https://openrouter.ai/api/beta/batches`. The `/api/beta/` path segment is
  OpenRouter's own beta marker.
- **Submission is inline JSON, not a file upload.** Unlike OpenAI (uploads a `.jsonl` via the Files
  API first, then references the `file_id`) and Anthropic (single call, but still a distinct
  Message-Batches-only endpoint), OpenRouter's batch create call takes a `requests` array directly in
  the POST body — each entry a `{custom_id, body}` pair, `body` being an ordinary request body for
  whichever endpoint the batch targets. OpenRouter persists this as JSONL internally; the caller never
  builds or uploads one.
- **Completion window:** fixed at 24h — "the only supported completion window."
- **Retrieval:** poll the batch by ID via GET; once complete, `results` comes back **inline in the
  same response** (no separate results-file download step, unlike OpenAI's output-file-id indirection
  or Anthropic's `results_url`).
- **Supported target endpoints:** `/v1/chat/completions`, `/v1/responses`, `/v1/messages`
  (Anthropic-shaped), and `/v1/embeddings` — i.e. a single OpenRouter batch job can be pointed at
  either of the two wire shapes AgentEngine already has conformers for (OpenAI-style
  `chat/completions`, Anthropic-style `messages`).
- **Pricing:** 50% of standard per-token pricing, matching both other vendors' batch discount —
  except non-token components (e.g. web search) stay at standard rates.
- **Limitation:** text-only for now. Requests carrying image/audio/video/file content are rejected by
  validation and must go through the synchronous API instead. The embeddings batch path additionally
  doesn't support `input_type` preference or multimodal inputs.

## Why this matters for AgentEngine specifically

`docs/planning/batch-inference-coalescing-gap.md` already established (from the OpenAI/Anthropic
research) that vendor batch mode is fundamentally single-shot — no mechanism for a batched request to
see a tool result and continue the same turn — and is explicitly **not** to be wired into
`AgentSession`'s run loop without a future ADR answering that doc's six open design questions. Nothing
here changes that: OpenRouter's batch API has the identical single-shot shape (no stated multi-round
mechanism), so it falls under the same gate.

What this DOES enable, outside that gate: `api-test.txt` (this workstation's live-test credential
file) holds a real OpenRouter key, and only OpenRouter — of the three vendors this codebase's live
tests target — has a batch endpoint reachable with a credential actually present in this environment
(no real `api.openai.com`/`api.anthropic.com` key is configured here). A standalone, non-interactive
CLI tool (`tools/batch_infer.cpp`) that submits N independent prompts straight to a vendor's batch
endpoint and polls for completion — with no `AgentSession`/`StandingEffect`/workflow integration at
all — sits outside the coalescing gate (it is direct vendor batch usage, not agentic-loop coalescing)
and can be built and live-verified against OpenRouter today. The same tool's OpenAI/Anthropic direct
paths are written against their real documented APIs (per the 2026-08-13 research doc) but can only be
live-verified once a real key for either vendor is available in this environment.

## Recommended design implication

Reuse `sandbox::perform_provider_https_exchange` (the same seam `OpenAIChatClient`/
`AnthropicChatClient` already call) for the batch tool's HTTP round trips — batch create, poll, and (for
OpenAI only) the Files-API upload/download — rather than a new HTTP path. OpenRouter's and Anthropic's
inline-JSON submission shape needs no multipart support; OpenAI's Files-API upload is the one path
needing a `multipart/form-data` body, since its batch API is the only one of the three requiring an
uploaded file rather than an inline array.
