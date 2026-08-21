# Ollama's OpenAI-compatible API — exact surface, fetched and cited

**Date:** 2026-08-21. **Source:** `https://docs.ollama.com/api/openai-compatibility` (live docs page;
content verified identical to `docs/api/openai-compatibility.mdx` on `main` in
`github.com/ollama/ollama`). Per `CLAUDE.md`'s research discipline: fetched, not recalled. Written to
ground `tests/test_ollama_live_e2e.cpp` — the same class of "does 004 §3's OpenAI-compatible backend
actually work against this real local server" proof `tests/test_llamacpp_live_e2e.cpp` already gives
for llama.cpp (ADR-016 gate G5), for the other local server 004 §3's own table names by name
("vLLM/llama.cpp/Ollama-style local servers").

## The facts

- **Base URL:** `http://localhost:11434/v1/` — default listen port **11434**, base path **`/v1`**.
  Endpoints include `/v1/chat/completions`, `/v1/models`, `/v1/embeddings`, `/v1/responses` (added
  v0.13.3).
- **Auth header:** required to be PRESENT, but its VALUE is ignored — the doc's own SDK example sets
  `api_key: "ollama"` with the comment `// required but ignored`. Ollama does not validate it against
  any real credential, but an empty/absent `Authorization` header is not confirmed safe — sending a
  placeholder value (the doc's own literal `"ollama"`) is the documented, zero-risk choice.
- **Feature support at `/v1/chat/completions`, confirmed on the stable OpenAI-compat surface (not
  native-API-only):**
  - Tool/function calling — supported (`tools` is a listed request field).
  - Structured output — supported via `response_format`, documented as **"JSON mode"**. NOT confirmed
    whether this means the full OpenAI Structured-Outputs shape (`{"type":"json_schema",
    "json_schema":{"name","schema","strict"}}`, what `openai::detail::translate_output_schema` sends)
    or only the older, schema-less `{"type":"json_object"}` "JSON mode". This project's own
    `translate_output_schema` sends the former; if Ollama only implements the latter, the schema
    portion may be silently ignored rather than rejected -- flagged as a real, live-observable
    uncertainty in the test itself, not resolved by this research pass.
  - Streaming (SSE) — supported (`stream`, `stream_options.include_usage`).
- **Documented UNSUPPORTED fields on `/v1/chat/completions`:** `logprobs`, `tool_choice`,
  `logit_bias`, `user`, `n`. (`user` matters here: AgentEngine's `end_user_id` parameter, when
  non-empty, adds the `user` field to the request body — Ollama silently ignoring it, per the doc's
  own unsupported-field list, is expected, not a bug, so the test below leaves `end_user_id` empty for
  Ollama to avoid asserting anything about a field the vendor's own docs say goes nowhere.)
- **Not found in the docs:** any stated model-pull-first requirement or read-timeout guidance for a
  slow/reasoning model. `test_llamacpp_live_e2e.cpp`'s own `AGENTENGINE_LLAMACPP_PREFIX` workaround
  (a `/no_think`-style prompt prefix, because `net_egress_proxy.cpp`'s 10s idle-read timeout can be
  exceeded by a reasoning model's chain of thought on a non-streaming call) is carried over as the
  same kind of contributor-configurable escape hatch, since nothing in Ollama's own docs rules out the
  identical failure mode against a locally-run reasoning model.

## What this means for the test

Same shape as `test_llamacpp_live_e2e.cpp` (real `OpenAIChatClient`, `ProviderTransport::plaintext_http`,
real resolver/secret/capability path, structural-only assertions, `live-network` ctest label, SKIP not
FAIL when unconfigured) with these differences:
- Default host `127.0.0.1`, default port `11434` (not required, since Ollama's port is standard —
  unlike llama.cpp, which has no fixed default).
- `AGENTENGINE_OLLAMA_MODEL` is the REQUIRED gate (not port): Ollama needs one of the operator's own
  already-pulled model names, and there is no universal default that would exist on an arbitrary
  contributor's machine.
- The structured-output check (LC-4-equivalent) is annotated with the "JSON mode" uncertainty above
  rather than presented as a settled fact.

This machine has no local Ollama instance running (`curl http://127.0.0.1:11434/api/tags` refused the
connection, checked 2026-08-21) — the test is written and compile/skip-path verified, but its live
assertions are NOT run against a real Ollama server this session. Left for a contributor who has one.
