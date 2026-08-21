# OpenRouter `session_id` / `x-session-id`: the real prompt-cache routing key

**Date:** 2026-08-21
**Scope:** Corrects/extends `docs/research/2026-08-07-provider-metadata-and-sampling-params-survey.md`
Finding 5's OpenRouter bullet and item 8's `prompt_cache_key` deferral. Does NOT reopen item 8 --
`session_id` is a different, OpenRouter-native field, not the OpenAI-Responses-API-only
`prompt_cache_key` that survey left deferred for lack of a distinguishing test.

## What triggered this

Session work threaded `end_user_id` (-> OpenAI's `user` field / Anthropic's `metadata.user_id`)
through `tools/cli_chat.cpp` and several `tests/test_rt_agent_session_*_live_e2e.cpp` files under the
claim "OpenRouter keys prompt-caching... on this value." That claim is **wrong** -- caught by a live
user report ("i only see `AgentEngine Live E2E` in the dashboard") that led to this research. The
`end_user_id`/`user` field only affects OpenAI/Anthropic's own abuse-tracking and (pre-July-2025) a
now-legacy OpenAI cache-bucketing heuristic -- it has no effect on OpenRouter's own gateway-level
caching at all.

## The actual mechanism

OpenRouter's own docs are explicit and directly contrast this against the `user` field:

- **`session_id`** -- either a top-level field in the JSON request body, or the **`x-session-id`**
  HTTP header (max 256 characters). When set, OpenRouter uses it directly as the sticky-routing key.
  Stickiness activates after the *first* successful request, not only after an observed cache hit.
- **Fallback 1**: if `session_id` is absent, OpenRouter falls back to OpenAI's `prompt_cache_key`
  body field as the routing key (interoperability with existing OpenAI-style clients) -- this is the
  ONE place `prompt_cache_key` has a confirmed, documented effect through OpenRouter specifically,
  distinct from the 2026-08-07 survey's still-open question about whether `api.openai.com`'s own raw
  Chat Completions endpoint honors it.
- **Fallback 2**: with neither field set, OpenRouter derives a routing key by hashing the first
  system/developer message plus the first non-system message.
- **Mechanics**: after a cache-eligible request succeeds, OpenRouter remembers which upstream
  provider served it and routes subsequent requests for the same model to that same provider,
  keeping that provider's own cache warm. The session expires after **10 minutes of inactivity**;
  each successful request resets the timer.
- **`x-session-id` also groups OpenRouter's non-chat endpoints** (embeddings, TTS, image generation)
  under the same conversation/run -- something no chat-request body field could do, since those
  endpoints don't share the chat body shape.
- **Explicitly NOT `user`**: "This differs from OpenAI/Anthropic's `user` field -- those providers use
  `user` for usage tracking and organization, not cache management. OpenRouter's `session_id`
  specifically orchestrates provider affinity for caching optimization." (OpenRouter blog, cited below.)

## OpenAI's own `user` field: also moved on

Independently confirms the 2026-08-07 survey's own framing was already dating: as of ~July 2025 OpenAI
split the old overloaded `user` field into two dedicated ones on its own API --
**`prompt_cache_key`** (cache routing) and **`safety_identifier`** (abuse tracking only). `user` itself
is legacy for caching purposes on OpenAI's own endpoints now, independent of anything OpenRouter does.

## Why this is actionable where item 8 (`prompt_cache_key`) was not

The 2026-08-07 survey deferred `prompt_cache_key` specifically because the only available test
(does OpenRouter/llama-server return HTTP 200) could not distinguish real support from silent
tolerance -- both also returned HTTP 200 for a deliberately invented field name. `session_id` is
different in kind: it's documented as OpenRouter's own native mechanism (not borrowed from another
vendor's differently-scoped API surface), sourced from OpenRouter's own docs/blog directly, and its
claimed effect (provider affinity / cache warmth) is independently observable through
`ChatResponse::model` staying constant across calls and `Usage::cached_input_tokens` rising on
follow-ups sharing the same key -- both already surfaced by this project's own response parsing. That
makes it a testable claim, not merely an accepted-but-unverified one; a future live-network test that
sends two same-`session_id` requests sharing a long prefix and asserts a `cached_input_tokens` > 0 on
the second would be a real gate, not attempted in this pass.

## Decision

Add `session_id` as a new optional, APPENDED-last constructor parameter on both `OpenAIChatClient` and
`AnthropicChatClient` (matching every other item-1/2/4/5/6/7 field's own "append only, never insert"
precedent from the 2026-08-07 survey), sent as the `x-session-id` HTTP header only when non-empty --
header, not body field, so it composes uniformly across both backends' differently-shaped bodies and
is harmlessly ignored by any endpoint that doesn't recognize it (an unknown header is far less likely
to trip strict schema validation than an unknown top-level JSON field would be against a real,
non-OpenRouter provider).

## Sources

- [Prompt Caching -- OpenRouter docs](https://openrouter.ai/docs/guides/best-practices/prompt-caching)
- [OpenRouter Prompt Caching: What Cached Tokens Cost -- OpenRouter Blog (sticky routing)](https://openrouter.ai/blog/tutorials/prompt-caching-sticky-routing/)
- [Prompt Cache Routing + the `user` Parameter -- OpenAI Developer Community](https://community.openai.com/t/prompt-cache-routing-the-user-parameter/1267103)
