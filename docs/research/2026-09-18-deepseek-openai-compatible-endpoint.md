# DeepSeek as a second real provider for the `live-network` tests

**Date**: 2026-09-18
**Why**: every test under the `live-network` ctest label talked to exactly one real provider
(OpenRouter). A suite with one provider cannot tell "this project sends a well-formed
OpenAI-compatible request" apart from "this project sends what OpenRouter happens to accept". This
document records what was measured about DeepSeek's endpoint before wiring it in, so the wiring
rests on measurement rather than on recollection of how OpenAI-compatible APIs "usually" look.

## Sources

- **Measured directly**, this date, with a real key against the real endpoint — every HTTP status
  and body quoted below was observed, not recalled. Method: `curl` for the raw wire shape, then this
  repo's own `openai::OpenAIChatClient` for the end-to-end run.
- <https://api-docs.deepseek.com/> (fetched 2026-09-18) documents the base URL as
  `https://api.deepseek.com` and lists `deepseek-flash` and `deepseek-v4-pro` as the current model
  ids, noting `deepseek-v4-flash` and `deepseek-v4-flash-vision-exp` still resolve as legacy names.

## What was measured

| probe | result |
| --- | --- |
| `GET /v1/models` | `200`, ids `deepseek-flash`, `deepseek-v4-pro` — matching the documented list |
| `GET /models` | `200`, identical body |
| `POST /v1/chat/completions` | `200` |
| `POST /chat/completions` | `200` — the documented base URL's own form |
| `POST /api/v1/chat/completions` | `404`, **empty body** |

So the endpoint serves its OpenAI-compatible surface under **both** `/v1` and no prefix at all, and
does **not** serve OpenRouter's `/api/v1`. That single difference was the only thing in this repo
standing between the live tests and a second provider: host, model and key were already
environment-driven, the path prefix was a `constexpr`.

### The request this repo sends is accepted as-is

The exact body `build_request_body()` emits — `model`, `messages`, `user`, and on the tool path
`tools[].function.parameters` — is accepted. A tool call comes back in the standard shape
(`finish_reason: "tool_calls"`, `tool_calls[].function.arguments` as a JSON *string*). Usage
counters are populated, including `completion_tokens_details.reasoning_tokens`.

`deepseek-flash` returns a **`reasoning_content`** field on the assistant message even when
`reasoning_effort` is not set, which is the same field 004 §3's reasoning path already parses;
`test_reasoning_model_delta_live_e2e` passes against it unchanged.

### Surfaces this vendor does NOT expose

- **No Anthropic-compatible surface.** `test_openrouter_live_e2e.cpp` exercises OpenAI-compatible
  *and* Anthropic-compatible surfaces on one host, which is an OpenRouter property, not a general
  one. That file stays OpenRouter-only.
- **No embeddings model** appears in `/v1/models`, so `test_openai_embedder_openrouter_live_e2e.cpp`
  has nothing to point at here.

Against a chat-only vendor, select the OpenAI-compatible subset of the label by name rather than
running the whole `live-network` label.

## A debugging note worth keeping

`api.deepseek.com` sits behind CloudFront. Two failure shapes were observed, and **neither carries a
JSON `error.message`**, so `map_http_status_error()` falls back to its bare
`"openai http status <N>"` string and the real reason never reaches the caller:

- a wrong path → `404` with an **empty** body;
- a malformed request line (in the run that found this, a path containing spaces) → `400` with a
  CloudFront **HTML** error page.

The malformed-path case arrived the way these usually do: `AGENTENGINE_OPENROUTER_PATH_PREFIX=/v1`
exported from Git Bash was rewritten by MSYS2 path conversion into `C:/Program Files/Git/v1` before
the process ever saw it, so the client dutifully sent
`POST C:/Program Files/Git/v1/chat/completions`. **Set the live-test environment from PowerShell, or
with `MSYS2_ARG_CONV_EXCL='*'`** — a value that begins with `/` is not safe to export from that
shell. The status alone said nothing; dumping the raw request bytes found it in one run.

## What this buys

`test_openai_chat_client_openrouter_live_e2e`, `test_reasoning_model_delta_live_e2e`,
`test_session_builder_openrouter_live_e2e`, `test_rt_agent_session_live_multitool_e2e` and
`test_rt_agent_session_skills_live_e2e` — 48 structural checks — now pass against **two**
independent providers, with the same binary and the same defaults. The one assertion that did not
survive the move was a hardcoded `"the reported model is an OpenAI-family model"`, which was a claim
about the vendor rather than about this project; it now derives the expected family token from the
model actually configured. That assertion was exactly what a second provider is supposed to find.

## Two other endpoints measured in the same pass

**A remote llama.cpp server** (`ibm-granite/granite-4.2-8b-GGUF`, reached over plain HTTP at a public
DDNS name) runs `test_llamacpp_live_e2e` green — 24 checks including the two-sided tool loop,
grammar-constrained structured output, and chunked SSE streaming. It also exposed a premise the test
had never had to state: **LC-8's claim is about the ADDRESS, not the guest path.** It asserted that a
guest holding an explicit `cap::NetOut` for "this exact live local server" is refused with
`net.address_blocked` — true of the loopback/RFC-1918 address a llama.cpp server normally has,
and false of a public one, where granted egress to a public host is ordinary and correct. The test
now asks `resolve_and_validate()` — the production classifier — which case the configured endpoint
is, and runs the control that is real for it: the refusal on a private address, and on a public one
the complement, that the same grant *succeeds*. Both branches were exercised (127.0.0.1 and the
public host) and each fails if wrong. The complement is worth having on its own: it is what stops
the private-address branch being vacuous, since a guest path that refused everything would also pass
it.

**OpenRouter's `stealth/ox-alpha` is gone**, and with it the default model of
`test_rt_agent_session_farm_ops_live_e2e`. Every request returns HTTP 404 with the body *"Thank you
for participating in the Stealth Ox Alpha testing period. This model was ZAI's GLM-5.3 Flash."* The
default now points at `z-ai/glm-5.3-flash` directly: a stealth alias is a dated default by
construction, and this is the second time in this repo a routing alias has silently changed what a
live test measures.

With that fixed, `farm_ops` reaches 15/17 on **three** models across **two** providers
(`z-ai/glm-5.3-flash`, `deepseek-flash`, `deepseek-v4-pro`) — and the same two assertions fail on all
three: `check_field_weather` and `check_animal_health` are never called, while their pair-partners in
the same skill are. That is **model choice, not wiring**: the file's own comment beside
`check_pest_pressure` already records a model "already learned the forecast, never calling
`check_field_weather` at all", and `test_rt_agent_session_live_multitool_e2e` — which forces two tool
calls in ONE assistant message — passes against both providers on the same binaries, so nothing is
losing a call. Those two assertions are the suite's remaining semantic (rather than structural)
checks, and three-for-three says they should be restated or the skill instruction strengthened. Left
as a finding, not silently relaxed.
