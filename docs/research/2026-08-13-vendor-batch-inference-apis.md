# How OpenAI's and Anthropic's Batch APIs actually work

**Date:** 2026-08-13. **Sources:** `https://developers.openai.com/api/docs/guides/batch` (fetched
2026-08-13), `https://platform.claude.com/docs/en/build-with-claude/batch-processing` (fetched
2026-08-13). Per `CLAUDE.md`'s research discipline: do not assert what a protocol/vendor API does
from memory. **Why this doc exists:** the user proposed letting concurrently-running agents share one
vendor batch inference call to save cost — this is the real shape of what "batch" already means in
AgentEngine's own architecture (`004-Model-Provider-Plane.md` §8 Q1, resolved 2026-08-04, currently
blocked on `StandingEffect`/`background_task`, which shipped for tool calls in M7 Phase B but was
never wired to model calls). This doc grounds the design question in the real vendor mechanics before
any design is drafted — see the companion gap doc,
`docs/planning/batch-inference-coalescing-gap.md`.

## Both vendors: one HTTP call submits N independent requests, async, ~50% cheaper

**OpenAI Batch API**: requests are collected as lines in a single `.jsonl` file (each line: a
`custom_id`, `method`, `url`, `body` matching the normal endpoint's own request shape), uploaded once
via the Files API, then ONE HTTP call creates the batch job referencing that file. Completion is
**polling only, no webhook** — you re-fetch the batch object and check its status
(`validating`/`in_progress`/`finalizing`/`completed`). Turnaround: "within 24 hours (and often more
quickly)." Cost: 50% discount vs. the synchronous API. Limits: up to 50,000 requests or 200 MB per
batch file; up to 2,000 batch creations per hour; per-model queued-token ceilings apply on top of that.
Results come back as a second file; `custom_id` round-trips unchanged so a caller can map each result
back to its request — **the doc explicitly warns output line order may not match input line order.**

**Anthropic Message Batches API**: a single HTTP call's `requests` array carries a list of
`{custom_id, params}` entries (`custom_id`: 1-64 chars, `^[a-zA-Z0-9_-]{1,64}$`), each `params` object
being an ordinary Messages API request. Also polling-based, no webhook. Most batches finish in under 1
hour; results are available once the whole batch completes OR after 24 hours, whichever comes first
(a batch that hasn't finished by 24h **expires** rather than completing late); results stay downloadable
for 29 days. Cost: also a flat 50% discount across every model. Limits: 100,000 requests OR 256 MB per
batch, whichever is hit first; rate limits apply both to the Batches HTTP endpoint itself and to the
count of in-flight batched requests.

## What is explicitly NOT supported in batch mode — the load-bearing finding for AgentEngine

Anthropic's own parameter table names exactly what breaks inside a batch request, and the reasons
matter for AgentEngine's design:

| Parameter | Why it's rejected in batch mode |
|---|---|
| `stream: true` | "Batch results come back as a single file, not a stream." |
| `store` / `previous_thread_event_id` (their server-side Threads/stateful-conversation feature) | "Threads are stateful; batch requests are not." |
| `cache_hint` / `context_hint` (prompt-routing hints) | "These routing hints apply to synchronous request scheduling only." |

Tool use itself IS supported inside a batch request (each batched request can include tools, and the
model can return tool-call content in its batched response) — but **the batch API has no mechanism for
a batched request to see a tool's result and continue the SAME turn.** Each batch item is one
complete, independent request/response pair; there is no live round-trip. An agentic tool-calling loop
(model → tool call → tool result → model again, AgentEngine's own `AgentSession::run_rounds()` shape)
would need to submit EACH ROUND as its own separate batch item, each incurring the full ~1-hour-or-more
turnaround — a 5-round tool-calling turn would take on the order of 5 hours, not 5 API round-trips.

## What this means for AgentEngine, directly (full design question in the gap doc)

Batch inference, as both vendors actually ship it, is a genuinely different shape from
AgentEngine's live agentic loop — it fits **single-shot or bounded-non-interactive model calls**
(the vendors' own listed use cases: bulk evaluation, content moderation, bulk generation), not a
multi-round `AgentSession` conversation that needs to see each round's outcome before deciding the
next tool call. "N agents running in parallel sharing one batch submission to save cost" is real and
buildable for the single-shot case (N independent `chat()` calls, no tool loop, coalesced into one
`.jsonl`/`requests` array instead of N live HTTP calls) — it does not, by itself, make a multi-round
agentic `AgentSession` cheaper, because the loop shape itself is incompatible with what a vendor batch
item can express.
