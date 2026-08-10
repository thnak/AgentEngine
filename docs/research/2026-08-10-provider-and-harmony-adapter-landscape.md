# 2026-08-10 — Provider adapter landscape & Harmony-format survey

**Status:** Research only. Nothing here is implemented; it's the starting point for a scoping
decision on which `ChatClient` backends to build next, and whether a Harmony-aware response
adapter is worth building. No RFC amendment or ADR yet — see "Recommendation" for what this
would require before code.

**Question:** beyond the two existing backends (`OpenAIChatClient`, `AnthropicChatClient`, both
field-driven JSON parsers over `include/agentengine/protocol/{openai,anthropic}/chat_client.hpp`),
which other providers' tool-calling wire formats are worth a dedicated adapter, and what would it
take to correctly handle `gpt-oss`'s Harmony response format — including using Harmony's channel
structure (`analysis`/`commentary`/`final`) to drive richer UI projection, not just tool-call
extraction?

Builds on `docs/research/2026-08-07-provider-metadata-and-sampling-params-survey.md` (sampling
params/metadata, not tool-call wire shape) and the finding recorded earlier this session: AgentEngine
does zero raw-text tool-call parsing today — both backends only read already-structured
`tool_calls`/`tool_use` JSON fields, and free-text `content` is captured verbatim with no scanning
for embedded call syntax.

## Part 1 — Closed-source hosted providers

Compatibility tiers: **(a)** OpenAI-Chat-Completions-compatible as-is · **(b)** OpenAI-shaped with
real quirks · **(c)** genuinely different envelope, needs its own translator.

| Provider | Tier | Key JSON fields | Notable gotcha |
|---|---|---|---|
| Google Gemini (native `generateContent`) | **(c)** | `contents[].parts[].functionCall.{name,args}`; request-side `functionResponse.{name,response}`; config at `toolConfig.functionCallingConfig.mode` | `args`/`response` are native JSON objects, not stringified like OpenAI's `arguments`; Parts-based envelope, not a `tool_calls` array; Gemini 3+ can stream partial function-call args incrementally |
| xAI Grok (`api.x.ai`) | **(a)** | `tool_calls[].function.{name,arguments}` (stringified), `parallel_tool_calls` | Streamed args arrive whole in one chunk, not token-incremental; `parameters` schema root must be an object or HTTP 400 |
| Amazon Bedrock Converse API | **(c)** | `toolConfig.tools[].toolSpec.{name,inputSchema}`; response `content[].toolUse.{toolUseId,name,input}`; request `content[].toolResult.{toolUseId,content,status}` | `input`/results are native JSON, not strings; normalization is **leaky** — Claude's `computer_*`/`bash_*`/native fine-grained tool streaming only exists via Anthropic's own Bedrock path, not through Converse |
| Azure OpenAI | **(a)** | Identical to OpenAI: `tool_calls[].function.{name,arguments}` | API-version gating for parallel calls/`tool_choice` on newer families; 1,024-char tool description cap; `content_filter_results` annotations layered on |
| Mistral La Plateforme | **(a)** | `tool_calls[].function.{name,arguments}`, `role:"tool"` results | `tool_choice:"any"` where OpenAI says `"required"` — same semantic, different token |
| Cohere Chat API v2 | **(b)** | `tool_calls[].function.{name,arguments}` plus extra `tool_plan` string; results as `content:[{type:"document",document:{data}}]` | Tool results are a structured document array, not a bare string; a separate "Compatibility API" exists for true (a)-tier OpenAI-SDK pass-through |

Sources: https://ai.google.dev/gemini-api/docs/function-calling ,
https://ai.google.dev/api/generate-content , https://docs.x.ai/docs/guides/function-calling ,
https://docs.x.ai/developers/model-capabilities/legacy/chat-completions ,
https://docs.aws.amazon.com/bedrock/latest/userguide/tool-use.html ,
https://docs.aws.amazon.com/bedrock/latest/APIReference/API_runtime_Converse.html ,
https://docs.aws.amazon.com/bedrock/latest/APIReference/API_runtime_ToolUseBlock.html ,
https://docs.aws.amazon.com/bedrock/latest/APIReference/API_runtime_ToolResultBlock.html ,
https://learn.microsoft.com/en-us/azure/ai-services/openai/how-to/function-calling ,
https://learn.microsoft.com/en-us/azure/ai-foundry/openai/concepts/content-filter-annotations ,
https://docs.mistral.ai/capabilities/function_calling/ , https://docs.cohere.com/v2/docs/tool-use ,
https://docs.cohere.com/v2/docs/migrating-v1-to-v2 ,
https://docs.cohere.com/v2/changelog/compatibility-api . All fetched 2026-08-10.

**Reading for AgentEngine:** Grok, Azure OpenAI, and Mistral are near-zero-cost — `OpenAIChatClient`
already generalizes to them with a base-URL/header change and one or two field-name shims (already
the pattern `docs/research/2026-08-07-...` used for OpenRouter). Gemini and Bedrock Converse are
real (c)-tier work — new backends, not variants. Cohere sits in between: worth doing only via its
own Compatibility API unless the `tool_plan`/document-wrapper fields are specifically wanted.

## Part 2 — Harmony (`gpt-oss`) response format, in detail

Harmony is OpenAI's training/inference format for the open-weight `gpt-oss-120b`/`gpt-oss-20b`
models (Apache-2.0), paired with its own `o200k_harmony` tokenizer. It is mandatory, not cosmetic —
OpenAI's own cookbook: *"gpt-oss should not be used without using the harmony format as it will not
work correctly."* (https://developers.openai.com/cookbook/articles/openai-harmony)

**Message grammar.** Every message is a delimited block:

```
<|start|>{role}<|channel|>{channel}<|message|>{content}<|end|>
```

A turn may contain several assistant messages in sequence; the *last* one closes with `<|return|>`
(a generation-only stop signal, never re-fed on the next turn) instead of `<|end|>` (used when
replaying history). Source: https://imoz.jp/scraps/202604_harmony.en.html, corroborated by the
cookbook's own wording.

**Three channels, three purposes:**

| Channel | Purpose | User-visible? |
|---|---|---|
| `analysis` | Chain-of-thought reasoning | No |
| `commentary` | Tool/function calls, plus short "preamble" text narrating planned tool use | No (call payload); preamble sometimes surfaced |
| `final` | The user-facing answer | Yes |

**Tool-call encoding** — an assistant message on `commentary`, addressed to a namespaced function,
constrained to JSON, closed with `<|call|>` (not `<|end|>`/`<|return|>` — `<|call|>` is the dispatch
signal):

```
<|start|>assistant<|channel|>commentary to=functions.get_current_weather <|constrain|>json<|message|>{"location":"San Francisco"}<|call|>
```

Tools are declared to the model as TypeScript-like namespace signatures in the system/developer
prompt. The model routinely emits a short `analysis` message first ("Need to use function
get_current_weather.") before the `commentary` call.

**Reference implementation.** OpenAI publishes `openai/harmony` on GitHub — a Rust core with a
PyO3 Python binding (`openai-harmony` on PyPI) and a directly consumable Rust crate. Relevant API
surface: `load_harmony_encoding()`, `render_conversation_for_completion()` (prompt side),
`parse_messages_from_completion_tokens()` (response side — turns raw tokens back into structured
`{role, channel, content}` messages). A community TypeScript reimplementation
(`fredrikalindh/openai-harmony-js`) confirms the grammar is precise enough to reimplement outside
Rust/Python. Known rough edge: `openai/harmony` issue #80 — the reference parser itself sometimes
mis-parses `gpt-oss-120b` refusal-message shapes, i.e. even the canonical implementation has open
corner cases.

**Is Harmony ever hidden from the client?** No universally-hosted escape hatch exists: gpt-oss is
explicitly **not** served on `api.openai.com` — OpenAI's own positioning is "run it anywhere:
locally, on-device, or through third-party inference providers"
(https://help.openai.com/en/articles/11870455-openai-open-weight-models-gpt-oss). Every path to
gpt-oss goes through a self-hosted or third-party serving layer, which makes Part 3 load-bearing:
there is no first-party normalized endpoint to lean on instead.

## Part 3 — Does the serving layer normalize Harmony, or does it leak?

| Server | Harmony tool parser | Enable flag | Leaks raw tokens into `content`? |
|---|---|---|---|
| **vLLM** | Yes, dedicated | `--tool-call-parser openai --enable-auto-tool-choice` | Not when the flag is set; a separate `/v1/responses` endpoint uses `openai-harmony` directly for both render and parse. Behavior with the flag *omitted* is undocumented in the recipe — a real gap, not confirmed either way. |
| **llama.cpp server** | Yes, native C++ PEG parser (independent of OpenAI's Rust one) | `--jinja` (+ `--chat-template-kwargs`) | Historically yes — multiple 2025 reports show raw `<|channel|>analysis<|message|>` tokens leaking into `content` under `--jinja`. As of build **b9656** (2026) this is hardened: PEG failures now surface a clean error instead of emitting fragments. Older/pinned builds still leak. |
| **Ollama** | Yes, own Harmony-mimicking template+parser | Automatic for the `gpt-oss` library model | Generally normalized, but a real, reproducible gap exists: `"harmony parser: no reverse mapping found for function name"` when a tool name doesn't round-trip through Ollama's own name-mangling (`ollama/ollama#11991`). |
| **SGLang** | Yes, dedicated | `--tool-call-parser harmony` (+ `"no_stop_trim": true`, or the call-terminator token gets silently trimmed) | Normalized when configured correctly; own issue tracker shows this is still an active area (`#10738`, `#9139`). |

Sources: https://github.com/vllm-project/recipes/blob/main/OpenAI/GPT-OSS.md ,
https://docs.vllm.ai/en/latest/features/tool_calling/ ,
https://pseedr.com/stack/hardening-local-agentic-workflows-llamacpps-peg-native-tool-call-parsing-update ,
https://github.com/ggml-org/llama.cpp/discussions/15396 ,
https://github.com/ollama/ollama/issues/11991 . Fetched 2026-08-10.

**Conclusion:** no serving layer guarantees leak-proof `tool_calls[]` unconditionally — each needs a
non-default flag, and each has open or only-recently-fixed bugs that let raw Harmony syntax reach
`content`. A defensive Harmony-aware adapter inside AgentEngine (detect `<|channel|>`/`<|call|>`
tokens leaking into `content`, re-parse them) is a real hedge against serving-layer version/config
drift — not a redundant safety net on top of something already reliable.

## Part 4 — Other native (non-JSON-field) tool-call text formats

- **Llama 3.1/3.2/3.3.** Built-in tools trigger via `Environment: ipython` in the system prompt; a
  call is prefixed with a literal `<|python_tag|>` and closes with `<|eom_id|>` (continue-turn) vs.
  `<|eot_id|>` for user-defined JSON-style calls; a new `ipython` role carries tool results.
  vLLM/llama.cpp both ship a dedicated parser (`--tool-call-parser llama3_json` in vLLM), since
  `<|python_tag|>` isn't valid JSON on its own.
- **Hermes/NousResearch-style `<tool_call>` (Qwen 2/2.5/3, Hermes-2/3).** A JSON object wrapped in
  literal tags: `<tool_call>{"name":"...","arguments":{...}}</tool_call>`, repeated for parallel
  calls. vLLM's `--tool-call-parser hermes`. **Qwen2.5-Coder deviates** and uses `<tools>` wrapper
  tags instead, needing a separate community parser — "Hermes-style" is a family of near-identical
  conventions, not one fixed grammar.
- **DeepSeek (V3/R1).** Its own multi-token delimiter set using the `▁` (U+2581) glyph inside
  special-token names: `<｜tool▁calls▁begin｜><｜tool▁call▁begin｜>function<｜tool▁sep｜>NAME...`.
  vLLM ships version-specific parsers (`deepseek_v3`, `deepseek_r1`, `deepseek_v31`) each paired
  with its own Jinja template. DeepSeek's own spec is stricter than Harmony's: "except for function
  calls, no other text must be included in the response" — no equivalent to Harmony's
  `analysis`-channel preamble alongside a call.

Sources: https://github.com/meta-llama/llama-models/blob/main/models/llama3_1/prompt_format.md ,
vLLM tool-calling docs (as above). Fetched 2026-08-10.

## Part 5 — Reasoning tags: the same leak risk, a plainer grammar

DeepSeek-R1 and Qwen3 both wrap chain-of-thought in a bare `<think>...</think>` block before the
final answer — DeepSeek always-on, Qwen3 toggleable via `enable_thinking` in
`apply_chat_template()` plus an in-conversation `/think`/`/no_think` soft switch. Sources:
https://api-docs.deepseek.com/guides/thinking_mode/ , https://huggingface.co/Qwen/Qwen3-32B .
Fetched 2026-08-10.

This is **not** Harmony-equivalent — it's two unrelated, independently-bolted-on conventions per
model (a bare reasoning tag pair, plus a completely separate tool-call delimiter syntax from Part 4),
not one unified channel/addressing grammar. But the leak risk is the same shape as Part 3's Harmony
finding: if the serving layer doesn't split `<think>` out, it reaches `content` verbatim. vLLM
mirrors its tool-call-parser pattern here too — a `--reasoning-parser` flag
(`deepseek_r1`/`qwen3`/etc.) that extracts `<think>` content into a separate `reasoning_content`
field (https://docs.vllm.ai/en/latest/features/reasoning_outputs/). Practical implication: a
defensive raw-text scanner shouldn't be scoped as "the gpt-oss/Harmony special case" — it's more
accurately "a raw-text reasoning/tool-call leak detector" that Harmony, DeepSeek, and Qwen all need,
at different grammar complexity (Harmony's full channel/recipient/constraint tokens vs. a single
open/close tag pair for the `<think>`-tag families).

## Recommendation

**Adapters, priority order:**

1. **Grok, Azure OpenAI, Mistral** — cheapest: `OpenAIChatClient` generalizes with base-URL/header
   changes plus 1-2 field-name shims (`tool_choice:"any"` vs `"required"` for Mistral). Same shape
   of work as OpenRouter, already done.
2. **Gemini, Bedrock Converse** — real new backends. Different envelope each (Parts-based /
   ToolUse-block-based), object-typed args instead of stringified JSON, own `ChatClient`
   implementation and translation tests, same rigor as the OpenAI/Anthropic ones.
3. **Cohere** — lower priority; its Compatibility API gets tier-(a) coverage for free if the native
   `tool_plan`/document-result fields aren't specifically wanted.

**Harmony adapter — two distinct asks, worth separating:**

- **(A) Tool-call correctness hedge.** A response-side scanner that detects Harmony special tokens
  leaking into `content` (Part 3's finding: every serving layer has a real leak path under some
  config/version) and re-parses them into `ToolCallRequest`/`ToolCall`, rather than silently losing
  the call the way today's parser would. This is scoped, bounded work: the grammar is a small
  delimited-token format, not a general parser problem.
- **(B) Channel-aware UI projection — the "great for rendering UI" idea.** This is more interesting
  than (A) and already has a landing spot: RFC `013-UI-and-Streaming-Surfaces.md` already models
  `ModelDelta` with distinct **text**/**reasoning**/**tool-call** delta kinds, and `003 §1` already
  has a `Reasoning`/`ReasoningEncryptedValue` content type. Harmony's `analysis`/`commentary`/`final`
  channels map almost directly onto that existing three-way split — `analysis` → reasoning delta,
  `commentary` (non-call preamble) → a form of reasoning/tool-intent text, `final` → text delta. A
  Harmony-aware backend could feed AgentEngine's *existing* streaming/UI projection machinery with
  **better-founded** channel separation than today's OpenAI/Anthropic backends get (those infer
  "reasoning" from vendor-specific out-of-band fields; Harmony states it explicitly, token by
  token). This turns "handle Harmony" from a defensive parser into a genuine UI-quality upgrade for
  any Harmony-served model — worth scoping as its own follow-up, distinct from (A).

**Open design question, not resolved here:** whether to reimplement the Harmony grammar natively in
C++ (small, well-specified, but the reference parser itself has open corner cases per
`openai/harmony#80` — reimplementing risks silently diverging from those), or FFI into the Rust
`openai/harmony` crate (correctness parity with the canonical implementation, but pulls in a new
build dependency and a second parser toolchain into a C++23-only codebase). This is exactly the kind
of contested, security-relevant design (raw model-output parsing feeds `ToolCall`, which is
authority-adjacent under invariant **I3**) CLAUDE.md's `design → red-team → prove → judge` process
exists for — it should get an ADR, not an ad-hoc implementation, before code is written.

## Explicitly deferred / unverified

- vLLM's behavior when `--tool-call-parser openai` is *omitted* for a gpt-oss model — not confirmed
  either way, worth a live check before assuming it's safe to skip.
- Gemini's and Bedrock's *streaming* tool-call delta shape in detail (only the non-streaming
  response shape was surveyed here) — needed before either backend's streaming path could be built.
- Whether `openai/harmony`'s Rust crate exposes a stable C ABI suitable for direct linking from a
  C++23 codebase, or would need a hand-written shim — not checked in this pass.
