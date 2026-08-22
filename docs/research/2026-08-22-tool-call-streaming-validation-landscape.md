# Tool-call argument streaming: how the ecosystem does validation/repair — four distinct classes

**Date:** 2026-08-22. **Sources:** fetched live via web search this session (not recalled) —
`https://github.com/pydantic/jiter`, `https://simonwillison.net/2024/Sep/22/jiter/`,
`https://pydantic.dev/docs/validation/dev/concepts/json/`, `https://python.useinstructor.com/concepts/partial/`,
Vercel AI SDK `streamObject`/Zod coverage (`devactivity.com`, `baeseokjae.github.io`), OpenAI function-calling
and structured-outputs docs (`developers.openai.com/api/docs/guides/function-calling`, `ergini.com`),
XGrammar paper (`arxiv.org/pdf/2411.15100`), llama.cpp grammar docs (`github.com/ggml-org/llama.cpp/blob/
master/grammars/README.md`, DeepWiki `8.1-grammar-and-structured-output`), and general-purpose search results
for `partial-json-parser`/`json-repair`-class libraries (PyPI, GitHub topic listing). Written to answer a
direct question raised while scoping `docs/planning/tool-call-argument-streaming-gap.md`: streaming a tool
call's arguments live isn't only a UI-rendering problem, it's also a validate/repair problem — what does the
rest of the industry actually do?

**Companion:** `docs/planning/tool-call-argument-streaming-validation-design-draft.md` maps these findings
onto AgentEngine's real code (files, types, capability bits). This doc stays external-only, per CLAUDE.md's
research discipline.

## Four classes, not variants of one idea

### A. Grammar-constrained decoding — prevention, not repair

**GBNF (llama.cpp)** converts a JSON Schema into an extended-BNF grammar; the model's own decoding loop is
restricted to only the tokens the grammar allows next. **XGrammar** (default structured-output backend for
vLLM/SGLang) compiles a JSON Schema into a finite-state machine ahead of time and reports sub-40µs/token
overhead. **Outlines** does the same as a logit processor wrapping a Transformers pipeline. All three make
invalid-JSON-mid-generation structurally impossible — there is nothing to "repair" because the model can never
emit a token that would violate the grammar. **This only works where something controls the actual sampling
loop** — a self-hosted/local backend, not a hosted vendor API whose sampling AgentEngine does not touch.
OpenAI's own server-side `strict: true` on both `response_format:{type:"json_schema",...}` and individual
function/tool definitions is the vendor doing exactly this technique on their own infrastructure: "setting
`strict: true` ensures function calls reliably adhere to the function schema, instead of being best effort."
Requirements for strict mode: `additionalProperties: false` on every object, every property listed in
`required` (optionality expressed via a `null` type option instead).

Sources: [XGrammar paper](https://arxiv.org/pdf/2411.15100), [llama.cpp grammars README](https://github.com/ggml-org/llama.cpp/blob/master/grammars/README.md), [llama.cpp grammar overview (DeepWiki)](https://deepwiki.com/ggml-org/llama.cpp/8.1-grammar-and-structured-output), [OpenAI function calling guide](https://developers.openai.com/api/docs/guides/function-calling).

### B. Partial/incremental JSON parsing — display only, never dispatch

Converged, near-identical pattern across every language ecosystem checked: Python `partial-json-parser`,
`jiter`'s `partial_mode` (`True`/`"on"`/`"trailing-strings"`), JS `partial-json-parser-js`, Kotlin
`partial-json-parser-kmp`, Swift `PartialJSON`. All do the same thing — parse as far as the input is valid,
best-effort-close any still-open string/object/array, return the largest valid prefix. **`jiter` is the
strongest-provenance option in this class**: it's the Rust parser inside `pydantic-core` (Pydantic's own JSON
engine, not a third party), and is also the parser the **OpenAI Python SDK itself depends on**
(`openai/openai-python#1616` discusses replacing it, confirming the dependency is real and current).
None of these libraries claim their partial-parse output is safe to act on — they exist purely to feed a
"typing" UI effect while the real, complete document is still being generated.

Sources: [jiter (GitHub)](https://github.com/pydantic/jiter), [jiter (Simon Willison)](https://simonwillison.net/2024/Sep/22/jiter/), [Pydantic JSON docs — partial mode](https://pydantic.dev/docs/validation/dev/concepts/json/), [openai-python issue #1616](https://github.com/openai/openai-python/issues/1616).

### C. Schema-aware progressive display — a *widened* schema, not the real one, mid-stream

Vercel AI SDK's `streamObject()` (Zod) and Instructor's `Partial[Model]` (Pydantic, "treats all of the
original model's fields as Optional") both do the identical trick: construct a relaxed variant of the real
schema (every field optional) to validate against *while streaming*, and only apply the real, strict schema
once the document is complete. **No library found anywhere does true incremental validation of a genuinely
partial document against the unmodified target schema** — every mainstream approach routes around that
problem rather than solving it, either via class A (make invalid output impossible) or class C (defer real
validation to stream-end). This is a meaningful negative result, not an omission in the search.

Sources: [Vercel AI SDK streaming coverage](https://devactivity.com/posts/development-integrations/boosting-ai-app-performance-streaming-structured-content-with-vercel-ai-sdk/), [Instructor — streaming partial responses](https://python.useinstructor.com/concepts/partial/).

### D. Repair-after-the-fact — for genuinely truncated/corrupted streams, not ordinary partial state

A newer, smaller class of tools sits as a proxy or post-processing pass to patch a stream that was cut off
mid-argument (hit `max_tokens`, dropped connection) rather than merely "not yet finished" — e.g.
`stream-json-repair` (PyPI). Several other names surfaced in search (`suture-stream-repair`, `StreamFix`)
read as newer or marketing-forward projects; **I could not verify real-world adoption or maturity for these
specific ones**, and flag that explicitly rather than presenting them as established practice. The
credible, vendor-documented version of this pattern is Anthropic's own recovery contract for its
`eager_input_streaming` feature (researched in the companion gap doc, fetched 2026-08-21 from
`platform.claude.com`): if accumulated argument fragments don't parse as valid JSON, report a `tool_result`
back to the model with `is_error: true` and `{"INVALID_JSON": "<the unparseable input>"}` — **never** run the
tool speculatively on partial data. That is the one recovery pattern in this class with a real, citable,
first-party specification behind it.

Source: `stream-json-repair` (PyPI, surfaced via general search, not independently vetted further).

## Net conclusion

AgentEngine's own existing invariant — `invoke_tool()` (`core/tool_pipeline.hpp:460`) never sees anything but
a complete, `json::parse`-validated `json::Value` — already matches where the whole industry converged
(classes B/C): stream partial content for display only, keep real validation gated on stream-completion.
Nothing found here weakens that invariant or argues it should change. What this search adds beyond what the
gap doc already scoped: class A (grammar-constrained decoding) is a materially different, stronger technique
that AgentEngine hasn't discussed anywhere yet, and it's not purely theoretical for this project — the repo
already has live-tested llama.cpp/Ollama backend paths (`tests/test_llamacpp_live_e2e.cpp`,
`tests/test_ollama_live_e2e.cpp`) where it could someday apply, alongside OpenAI's own already-partially-wired
`strict` mechanism (`openai/chat_client.hpp:209-231`, currently only for response-schema, not yet tool-call
schemas). See the companion design draft for how these map onto real AgentEngine types.
