# Research record — Provider tool-schema support (for issue #170 / DUIP)

**Checked:** 2026-10-11, against the official OpenAI and Anthropic docs (live pages, cited below).
DeepSeek added 2026-10-11 (see §4 for its lower-confidence caveat).
**Question:** does either provider support what issue #170 assumes — one stable model-facing tool with
schema-on-demand, strict-schema constraints, and (optionally) changing tool definitions mid-conversation?
**Feeds:** issue #170 (Dynamic Tool / DUIP), open design decision #1 ("model provider strict-schema support").

## 1. Both providers: tools are per-request

The `tools` array is part of every API request (OpenAI `tools` / Responses `tools`; Anthropic `tools`).
There is **no server-side lock** of tool definitions between turns — the caller decides what to send each
request. The DUIP design ("one stable `ui` tool; capability catalog and detailed schemas fetched on
demand via `discover`/`describe`") therefore works on both providers today without any special provider
support. The protocol's own payloads (`op`, `context_id`, `request_id`, `payload`) are the only schema
the model ever needs up front.

## 2. OpenAI

- **Strict mode** (`strict: true` on a function tool, and `strict` in `json_schema` response formats):
  grammar-constrained to the schema. Hard requirements, request **rejected** with details if unmet:
  1. `additionalProperties: false` on every object in `parameters`;
  2. every property in `properties` listed in `required` — optional fields are expressed as
     `anyOf [<schema>, {"type": "null"}]`.
- **Arbitrary objects are NOT allowed in strict mode** — a bare `{"type": "object"}` with no
  constrained `properties` is rejected. This is exactly why issue #170's compatibility note says the
  `payload` must become a bounded union of operation-specific payloads or a JSON string parsed and
  validated server-side.
- **Tool search** (gpt-5.4+): the model searches for and loads deferred tools on demand; tools may
  also carry `defer_loading: true`; `namespace` tool type groups related tools. Native progressive
  disclosure exists, but it is provider-specific and version-gated — DUIP correctly does not depend on it.
- `tool_choice: {"type": "allowed_tools", ...}` restricts the callable subset without editing the `tools`
  list (prompt-cache friendly).
- Structured Outputs (`response_format`/`text.format` `json_schema`) shares the same strict subset.

## 3. Anthropic

- **Strict tool use** (`strict: true` on the tool definition): grammar-constrained sampling guarantees
  `tool_use.input` matches `input_schema` exactly (correct types, required fields present). Uses the
  same JSON Schema limitation subset as structured outputs. This is newer than OpenAI's equivalent.
- **Mid-conversation tool changes** (beta, header `inline-tools-2026-09-15`): declare the full tool set
  in `tools` up front, then `tool_addition` / `tool_removal` blocks in `role: "system"` messages
  offer or withdraw tools from a point in the conversation onward — **without editing the `tools`
  array**, so the prompt-cache prefix stays byte-identical. `tool_addition` can also carry a full
  `tool_definition` by value (new tool, or a changed schema under the same name). Tools may declare
  `defer_loading: true` to stay withheld until surfaced. Limits: 10,000 deferred tools, 10,000
  defined-after-first-user-message tools, 4 MB of post-first-message definitions. This is the closest
  native support for "changing tool definitions during an active conversation" — but it is beta and
  provider-specific, so issue #170's decision not to depend on it remains correct.
- **Tool search tool**: server-side discovery/loading of tools on demand (thousands of tools).

## 4. DeepSeek (OpenAI/Anthropic-compatible)

- The API is OpenAI- **and** Anthropic-compatible (`base_url: https://api.deepseek.com`,
  Anthropic surface at `/anthropic`). The `tools` array is per-request like the others, so
  the single-stable-tool contract works unchanged.
- **Strict mode** (`"strict": true` **inside the function definition**): a **Beta** feature —
  the server validates the supplied JSON Schema upfront and constrains tool-call output to
  it; requires `additionalProperties: false`; schemas using unsupported types are rejected at
  request time. Supported on `deepseek-v4-pro` / `deepseek-v4-flash`, in both thinking and
  non-thinking modes.
- **Confidence caveat:** the official docs site (api-docs.deepseek.com) currently serves only
  the quick-start page at fetchable paths — the function-calling / strict-mode pages redirect.
  The strict-mode details above come from a third-party guide citing the official docs
  (deepseekai.guide, last checked 2026-04-30). Verify against the official tool-calling
  doc before relying on it.
- **Integration gotchas:** (1) in thinking mode, a tool-call turn must echo
  `reasoning_content` back on the assistant message of the next request, else the API
  returns **400**; (2) no equivalent of OpenAI tool search / Anthropic mid-conversation
  tool changes was found; (3) without strict mode, guides warn the model may emit invalid
  JSON or hallucinate parameters — validate `tool.function.arguments` defensively.

## 5. Open servers (llama.cpp / Ollama / vLLM)

All three expose the OpenAI wire format, so AgentEngine's OpenAI backend is the
natural path; the `tools` array is per-request like the hosted providers.

- **llama.cpp (`llama-server`)** — tool calling supported (`docs/function-calling.md`,
  repo master): OpenAI-compatible `/v1/chat/completions` with `tools`, requires
  `--jinja` plus a tool-aware chat template; native handlers for Llama 3.x, Qwen 2.5,
  Hermes, Functionary, Mistral Nemo, Command R7B, Firefunction v2, GPT-OSS, with a
  Generic fallback; `parallel_tool_calls` is opt-in. **No `strict: true` on the
  wire.** Constrained decoding is available instead: GBNF grammars (native) or
  **LLGuidance** (build flag `LLAMA_LLGUIDANCE=ON`, requires Rust) — strong JSON
  Schema coverage, `additionalProperties` defaults to true, unsupported schemas error
  loudly rather than being silently ignored. Tool arguments remain best-effort.
- **Ollama** — tool calling via OpenAI-compatible `/v1`; structured outputs via the
  `format` parameter (JSON Schema, grammar-enforced); the OpenAI-compat endpoint also
  accepts `response_format` (Pydantic/Zod helpers). No `strict` parameter for tools.
- **vLLM** — tool calling via the OpenAI-compatible server; structured outputs via
  guided decoding (`guided_json`, being consolidated into `structured_outputs:
  {"json": schema}` with the loose fields deprecated), enforced by xgrammar/outlines;
  `response_format` json_schema supported.

**Net effect for AgentEngine:** `strict: true` is ignored or absent on these servers,
so the strict-mode shaping in `make_strict_schema` is harmless (the schema is still
sent) but not enforced server-side for tool arguments. The engine's own fail-closed
validation (`schema::from_json<Args>` at dispatch, 006 §1/§6) is the actual
guarantee — server-side strictness is a bonus, never a dependency, which strengthens
the #170 design. Note constrained decoding applies to `response_format`, not to
tool-call arguments, so a DUIP `ui` payload still needs client-side validation
everywhere.

## 6. Codebase mapping (AgentEngine today)

- `src/protocol/openai/chat_client.cpp` — `make_strict_schema()` already implements exactly OpenAI's
  strict requirements (adds `additionalProperties: false`, lists all properties in `required`, wraps
  non-required props in `anyOf [..., {"type": "null"}]`); `translate_output_schema()` sets
  `strict: true`. Verified against the official requirements above.
- `src/protocol/anthropic/chat_client.cpp` — `translate_tool()` does **not** currently set
  `strict: true`; Anthropic strict tool use is available if the project wants to adopt it.
- `include/agentengine/core/tool_descriptor.hpp` — one `args_schema_json`/`reply_schema_json` per
  tool, compile-time-derived from the typed `Args`/`Reply` structs (`AE_JSON_SCHEMA`), resolved once
  into an immutable per-run `ToolTable` (006 §6). No per-operation or on-demand schema mechanism
  exists — that is the gap issue #170's `discover`/`describe` operations would fill.

## 7. `defer_loading` / tool search (progressive disclosure)

Both providers shipped tool deferral in 2026 — neither has had it "for a
while": OpenAI's `defer_loading` + `tool_search` is Responses-API-only and
gated to **gpt-5.4+**; Anthropic's `defer_loading` arrived with the
mid-conversation tool changes beta (headers
`mid-conversation-tool-changes-2026-07-01` / `inline-tools-2026-09-15`).

Semantics: declare a tool in `tools` but withhold it from the model's visible
context until it is surfaced later. Uses:

- **Token/cost savings** — tool definitions are input tokens; deferred tools
  enter context only when needed (OpenAI injects loaded tools at the end of
  the context window to preserve the cache).
- **Better tool selection** — fewer visible tools; OpenAI recommends <20
  initially available.
- **Large catalogs** — OpenAI `namespace` groups deferred functions (the
  model sees only the namespace name/description); Anthropic surfaces
  deferred tools via `tool_addition` blocks.
- **App-controlled discovery** — OpenAI client-executed tool search: the
  model emits `tool_search_call`, the application returns `tool_search_output`
  with the tools to load — the provider-level analogue of DUIP's
  `discover`/`describe`, for when the tool set depends on project/tenant
  state.
- **Capability gating** — withhold a tool until a condition (auth, tier,
  mode) is met, then surface it mid-conversation.

Provider differences:

- OpenAI: an individually deferred function still exposes its **name and
  description** (only the parameter schema is deferred); namespaces/MCP
  servers hide everything but name+description. Two execution modes: hosted
  (`execution: "server"`) and client-executed (`execution: "client"`).
- Anthropic: `defer_loading: true` withholds the tool entirely until a
  `tool_addition` block surfaces it; `tool_removal` withdraws it; a
  `tool_addition` can also define a tool by value mid-conversation.

**Relevance to AgentEngine / #170:** this is the provider-native mechanism
for exactly what DUIP does at protocol level (declare the catalog, disclose
details on demand). The engine's per-run `ToolTable` (006 §6) is resolved
once and immutable — a "declared but not model-visible" state would be a new
concept. The provider-neutral `ui` tool remains the correct default;
`defer_loading`/`tool_search` is an optional per-provider optimization, not a
dependency.

## 8. Implications for issue #170

1. The "one stable `ui` tool" contract is provider-safe on both OpenAI and Anthropic as designed.
2. The `payload: {"type": "object"}` shape in §5.1 fails OpenAI strict mode — the documented
   fallback (bounded union or JSON string validated server-side) is required, not optional.
3. Strict-mode shaping is already solved for OpenAI in-tree; adding Anthropic `strict: true` is a
   small, additive change.
4. Neither provider requires mid-conversation tool-definition changes for DUIP; Anthropic's beta
   `tool_addition`/`tool_removal` is a possible future optimization, not a dependency.
5. Both providers now ship native on-demand tool discovery (tool search) — worth watching, but DUIP's
   protocol-level disclosure stays provider-neutral and cacheable.
6. DeepSeek's strict mode is **Beta** — treat it as best-effort relative to OpenAI/Anthropic; the
   `payload`-as-JSON-string fallback from #170 is the safest cross-provider shape, and
   thinking-mode tool calls need `reasoning_content` echo-back (a provider-specific wire detail
   a DeepSeek chat backend must handle).
7. Open servers (llama.cpp / Ollama / vLLM) speak the OpenAI wire format but do not
   enforce `strict: true` on tool arguments — the engine's own `from_json<Args>`
   validation is the real guarantee, which is the correct design anyway (strictness as
   a bonus, never a dependency). Constrained decoding there applies to `response_format`,
   not to tool-call arguments.
8. `defer_loading`/`tool_search` (both providers, 2026) is the provider-native
   analogue of DUIP disclosure — an optional per-backend optimization, not a
   dependency; the engine's immutable `ToolTable` (006 §6) has no "declared but
   model-hidden" state today.

## Sources

- OpenAI Function calling: https://platform.openai.com/docs/guides/function-calling (strict-mode
  requirements, tool search, defer_loading, allowed_tools)
- OpenAI Structured outputs: https://platform.openai.com/docs/guides/structured-outputs
- Anthropic Strict tool use: https://docs.claude.com/en/docs/agents-and-tools/tool-use/strict-tool-use
- Anthropic Mid-conversation system messages and tool changes:
  https://docs.claude.com/en/docs/build-with-claude/mid-conversation-system-messages
- Anthropic Tool use overview (tool search tool, input_schema):
  https://docs.claude.com/en/docs/agents-and-tools/tool-use/overview
- DeepSeek API docs (OpenAI/Anthropic compatibility, base URLs):
  https://api-docs.deepseek.com/
- DeepSeek function calling / strict mode (third-party guide citing official docs,
  checked 2026-04-30 — see §4 confidence caveat):
  https://deepseekai.guide/api/deepseek-api-function-calling/
- llama.cpp Function calling (repo docs, master):
  https://github.com/ggml-org/llama.cpp/blob/master/docs/function-calling.md
- llama.cpp LLGuidance support (JSON Schema constrained decoding):
  https://github.com/ggml-org/llama.cpp/blob/master/docs/llguidance.md
- Ollama Structured outputs (blog, 2024-12-06):
  https://ollama.com/blog/structured-outputs
- vLLM Structured outputs (guided decoding):
  https://docs.vllm.ai/en/latest/features/structured_outputs/
- OpenAI Tool search (`defer_loading`, `tool_search`, namespaces, client-executed
  discovery):
  https://developers.openai.com/api/docs/guides/tools-tool-search

## Glossary

**Protocol / wire**

- `tools` array — the list of tool/function definitions sent with every
  request; per-request, so it can change between turns (no server-side lock).
- `tool_choice` — whether the model may/must call a tool (`auto`, `required`,
  `none`, forced by name; OpenAI `allowed_tools` restricts to a subset).
- `tool_calls` / `function_call` (OpenAI) vs `tool_use` (Anthropic) — the
  model's request to invoke a tool; the result returns as a `tool` message /
  `function_call_output` (OpenAI) or a `tool_result` block (Anthropic).
- `parameters` (OpenAI) / `input_schema` (Anthropic) — the JSON Schema of a
  tool's arguments.
- MCP — Model Context Protocol; external tool servers.
- prompt cache / cached prefix — provider-side caching of the hashed request
  prefix (`tools` → `system` → `messages`); editing earlier parts invalidates
  it, which is why mid-conversation changes append instead of edit.

**Schema enforcement**

- strict mode / strict tool use — grammar-constrained sampling so tool
  arguments provably match the schema (OpenAI `strict: true`; Anthropic
  `strict: true` on the tool definition).
- Structured Outputs — OpenAI's schema-enforced `response_format`
  (`json_schema`); JSON mode (`json_object`) only guarantees valid JSON, not
  schema adherence.
- `additionalProperties: false` — no keys outside the declared `properties`;
  required by OpenAI strict mode.
- all-required rule — OpenAI strict mode lists every property in `required`;
  optional fields are expressed as `anyOf [<schema>, {"type": "null"}]`.
- grammar-constrained decoding — decoding constrained to a grammar so only
  schema-valid tokens can be emitted.
- GBNF — llama.cpp's native grammar format. LLGuidance — Rust-based JSON
  Schema / CFG (Lark syntax) constrained-decoding backend for llama.cpp
  (build flag `LLAMA_LLGUIDANCE=ON`). xgrammar / outlines — vLLM's
  guided-decoding backends.

**Progressive disclosure**

- `defer_loading: true` — declare a tool but withhold it from the model's
  visible context until it is surfaced later.
- `tool_search` — OpenAI's on-demand tool discovery (Responses API,
  gpt-5.4+); hosted (`execution: "server"`) vs client-executed
  (`execution: "client"`: the model emits `tool_search_call`, the application
  returns `tool_search_output` with the tools to load).
- `namespace` — OpenAI tool grouping; deferred functions inside are
  discovered via tool search (model sees only the namespace name/description).
- `tool_addition` / `tool_removal` — Anthropic beta blocks in `role: "system"`
  messages that surface / withdraw tools mid-conversation without editing
  `tools`; `tool_reference` names a declared tool, `tool_definition` carries a
  full definition by value.

**AgentEngine**

- `ToolDescriptor` — one tool's static description: name, description, one
  `args_schema_json`, one `reply_schema_json` (spec 006 §1).
- `ToolTable` — the per-run, immutable resolved tool set (spec 006 §6).
- `AE_JSON_SCHEMA(Type, members...)` — macro deriving both the JSON Schema
  string and `ae_to_json`/`ae_from_json` from a single field list.
- `make_strict_schema` / `translate_tool` — the OpenAI / Anthropic backend
  schema transforms in `src/protocol/`.
- `schema::from_json<Args>` — engine-side fail-closed deserialization of
  model-supplied tool arguments into the typed `Args` struct.
- DUIP — Dynamic UI Protocol (issue #170): one stable `ui` tool with a
  generic payload; the capability catalog and detailed schemas are fetched on
  demand via `discover`/`describe`.
- `ui` tool — the single model-facing tool in DUIP, carrying `op`,
  `context_id`, `request_id`, `payload`.
