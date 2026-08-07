# 2026-08-07 — External LLM-call JSON fixture/mock corpus survey

**Question:** what public resources exist for testing an LLM-call flow deterministically, offline,
without a live API call — and which of them are actually reusable as source material for
`tests/fixtures/chat_client/` (see its `README.md` for the schema `RecordedChatClient`
(`tests/support/recorded_chat_client.hpp`) consumes)? Researched to seed follow-up sessions that add
more `ChatClient` fixtures and their matching cases in `test_recorded_chat_client.cpp`.

Research performed 2026-08-07 via web search plus hands-on cloning/inspection of the repos below.
Nothing here is vendored into this repo — see "Why nothing is vendored" below.

## The constraint that matters most

`RecordedChatClient` is a small, hand-authored, test-scoped stand-in (its header's own top comment
is explicit about this), not an implementation of the general record/replay feature described in
`004-Model-Provider-Plane.md` §6. Its fixture schema is narrower than any real provider payload or
any of the corpora below:

- One fixture file = one `ChatResponse` (a single `Message` + `Usage`). No conversation history, no
  multi-turn.
- `ContentItem.kind` only dispatches `text`, `reasoning`, `tool_call` — no `Media`, `Data`,
  `ToolResult`, `Citation`, `Error`, `Custom`.
- No streaming fixture schema yet (blocked on real `ae::stream<T>` vocabulary).

So nothing below is "drop-in." Every source is useful only as *inspiration for hand-authoring* a new
fixture in the existing schema — pick a realistic scenario, extract the shape of the tool call /
response, write it by hand in the AgentEngine schema. `tests/fixtures/chat_client/parallel_tool_calls.json`
(added this session) was authored this way from the Gorilla source below.

## Sources surveyed

1. **Berkeley Function-Calling Leaderboard (BFCL) v3** — Hugging Face dataset
   `gorilla-llm/Berkeley-Function-Calling-Leaderboard` (huggingface.co/datasets/gorilla-llm/Berkeley-Function-Calling-Leaderboard,
   fetched 2026-08-07) and source repo `github.com/ShishirPatil/gorilla`
   (`berkeley-function-call-leaderboard/bfcl_eval/data/`, same date). ~50 JSON files: question +
   function-schema pairs plus `possible_answer/` ground truth, split by category (`simple`,
   `multiple`, `parallel`, `parallel_multiple`, `multi_turn_*`, `java`, `javascript`, `sql`, `live_*`,
   `irrelevance`). Best source for realistic tool-call *shapes* (function name, argument types,
   parallel-call patterns) — this is what `parallel_tool_calls.json` borrowed its Spotify
   `play(artist, duration)` scenario from (`BFCL_v3_parallel.json`, id `parallel_0`). License:
   Apache-2.0 per the HF repo card (not independently re-verified beyond the card at fetch time).
2. **llmock** / **aimock** (`github.com/CopilotKit/llmock`, `github.com/CopilotKit/aimock`, fetched
   2026-08-07) — deterministic mock LLM servers with their own fixture format
   (`fixtures/examples/llm/*.json`: `blocks-tool-first.json`, `sequential-responses.json`,
   `streaming-physics.json`, `error-injection.json`, `embeddings.json`). Their schema is
   `{"fixtures":[{"match":{...},"response":{"blocks":[...]}}]}` — a request-matching mock config, not
   a recorded response — structurally different from `RecordedChatClient` (which has no request
   matching, one fixture = one client instance). Useful for scenario ideas (tool-call-then-text
   ordering, sequential multi-response turns, error injection), not for direct schema borrowing.
3. **langchain-openai test suite cassettes** — `github.com/langchain-ai/langchain`
   (`libs/partners/openai/tests/cassettes/`, sparse-checked-out 2026-08-07) — 101 real VCR cassettes
   (gzipped YAML) recorded from actual OpenAI API calls made by LangChain's own test suite. Genuine
   provider wire format, but multi-turn and streaming-chunk heavy — would need real streaming fixture
   support (see the constraint above) to be worth mining further. Revisit once `ae::stream<T>` lands.
4. **vcrpy** (`github.com/kevin1024/vcrpy`, fetched 2026-08-07) — the record/replay library itself
   (`tests/fixtures/`), not an LLM-specific corpus. Relevant only as the *pattern* to point to if
   `004-Model-Provider-Plane.md` §6's real record/replay feature is ever implemented — cassette
   recording of a real vendor call, not hand-authoring.
5. **mock-llm**, **MockLLM (StacklokLabs)**, **ai-mocks** — OpenAI/Anthropic-compatible mock HTTP
   servers configured via YAML, not JSON fixture corpora; useful if this project ever wants an
   integration-level mock server rather than an in-process test stand-in, but that's a different
   layer than `RecordedChatClient`.

## Why nothing is vendored

- **Schema mismatch** (above) means none of it is copy-pasteable; every fixture worth having still
  has to be hand-authored against `tests/fixtures/chat_client/README.md`'s schema.
- **License hygiene** — BFCL is Apache-2.0, llmock/aimock fixtures are MIT-ish per their repos, but
  none of that was verified rigorously enough to commit third-party files wholesale into this repo;
  hand-authoring a fixture "inspired by" a public example sidesteps the question entirely.
- Matches the existing convention in this directory: `README.md` already says fixtures are
  "hand-authored," not captured.

## Ideas for the next fixture (and its `test_recorded_chat_client.cpp` case)

Ranked by how directly they fit the *current* schema (no streaming, no multi-turn, no new
`ContentItem` kinds):

1. **Done this session**: `parallel_tool_calls.json` — two `ToolCall` items in one message. No test
   case added yet (left for a follow-up session per the task owner's instruction).
2. **Complex/nested `arguments_json`** — BFCL's `sql`, `java`, and `multi_turn_*` categories have
   argument schemas with nested objects/arrays; a fixture using one would stress-test that
   `arguments_json` round-trips as an opaque string regardless of internal structure (it's stored and
   compared as a string in `RecordedChatClient`, never parsed as JSON itself).
3. **Irrelevance / no-tool-call text-only reply despite tools being offered** — BFCL's
   `BFCL_v3_irrelevance.json` / `live_irrelevance.json` categories are full of "the user asked
   something no available function can satisfy" turns; a fixture pairing a `ChatClientCapabilities`
   with `tool_calling = true` against a plain `text` response would exercise that a client can
   legitimately decline to call a tool.
4. **Blocked on loader support**: an `error`-kind `ContentItem` fixture (llmock's
   `error-injection.json` is the model for this scenario) needs `parse_content_item` in
   `recorded_chat_client.hpp` extended first — the README already documents this as "add a branch,
   not a redesign."
5. **Blocked on `ae::stream<T>`**: any of the langchain cassette or llmock `streaming-physics.json`
   material, once a streaming fixture schema exists (the README sketches `{"delta": <ContentItem>,
   "is_final": bool}` as the likely shape).

## Re-fetching the raw corpus

Not committed anywhere in this repo. To pull it again for a future session: shallow-clone
`CopilotKit/llmock`, `CopilotKit/aimock`, `kevin1024/vcrpy`; sparse-checkout
`ShishirPatil/gorilla:berkeley-function-call-leaderboard` and
`langchain-ai/langchain:libs/partners/openai/tests` (needs `git config core.longpaths true` on
Windows — several cassette filenames exceed 260 chars); and pull the BFCL dataset files directly via
the Hugging Face dataset API (`https://huggingface.co/api/datasets/<repo_id>` for the file list, then
`https://huggingface.co/datasets/<repo_id>/resolve/main/<file>` per file — no auth needed, it's a
public dataset).
