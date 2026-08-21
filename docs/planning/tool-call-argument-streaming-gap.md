# Live tool-call argument streaming — RFC 013 already named it, nothing implements it

**Status:** Research + gap finding from live conversation (2026-08-21), **not a design draft, not
red-teamed, no code written**. Companion: `tool-call-result-streaming-gap.md` (the other half — results
after invocation vs. arguments before invocation). Written from a user question: Google/OpenAI-style
apps render a live "typing" effect while a model writes a tool call's arguments (e.g. a file's content
streaming word-by-word before the write happens) — does AgentEngine support this, and could it be
designed natively rather than bolted on?

## RFC 013 §1 already names this concept

Direct quote, `013-UI-and-Streaming-Surfaces.md` (around `ToolCallDelta`'s definition):

> [`ModelDelta` is] the model incrementally producing a call's arguments before invocation starts;
> `ToolCallDelta` is the tool reporting on work already in flight.

So the spec's own intent is that argument-streaming folds into `ModelDelta` — the SAME event kind used
for streamed assistant text — and `ToolCallDelta` is deliberately reserved for a different thing:
progress reported from *inside* an already-invoked tool body (confirmed at the code:
`run_event.hpp:79-84`, `ToolCallDelta{call_id, progress_text}`, sourced from
`EffectContext.report_progress` during `invoke()`). These are temporally distinct — before invocation
vs. during — and the RFC already keeps them apart on purpose.

## The real implementation doesn't do this yet

Confirmed directly in the OpenAI backend's streaming parser
(`include/agentengine/protocol/openai/chat_client.hpp:555-600`). Its own comment: *"Text deltas map 1:1
to the vendor's own chunks and stream immediately; tool calls are assembled and appended by `finish()`."*
Concretely: each SSE chunk's `delta.tool_calls[].function.arguments` fragment is appended into an
internal `PendingToolCall::arguments` accumulator (`:687-689`) as it arrives, but `items_from_block()`
never pushes anything into the caller-visible `out` vector for it — only `finish()`, called once at
stream end, converts the fully-accumulated fragments into ONE complete `ToolCall` content item
(`:592-601`). So today: a consumer gets nothing for a tool call in progress, then the whole thing at
once when the model finishes generating it. The RFC's stated intent (fold into `ModelDelta`, stream
progressively) is a real, confirmed gap against the real code, not a hypothetical one.

## Real vendor precedent — Anthropic's `eager_input_streaming` (verified against current docs, not memory)

Fetched live from `platform.claude.com/docs/en/agents-and-tools/tool-use/fine-grained-tool-streaming`,
2026-08-21. Key facts:

- **Declared per tool**, not per parameter, not per request: `eager_input_streaming: true` on a specific
  tool's definition. Default (unset): the server buffers AND validates each parameter value before
  streaming it back. With it set: fragments arrive raw and unvalidated, for whatever parameter the model
  is currently writing, in *generation* order — not schema order, and NOT specially scoped to "the last
  declared parameter." The "only the big content field visibly streams" effect apps show is emergent
  (small fields resolve almost instantly; a large blob takes visibly long to generate), not a structural
  rule.
- **The accumulated string is not guaranteed to be valid JSON.** Anthropic's own docs state this
  plainly, and name a real failure mode: a response can hit `max_tokens` mid-parameter, truncating it.
- **The tool is never invoked on anything but complete, successfully-parsed input.** If the accumulated
  fragments don't parse, Anthropic's documented recovery is to report a `tool_result` back to the model
  with `is_error: true` and content `{"INVALID_JSON": "<the unparseable input>"}` — never to run the
  tool speculatively on partial data.
- Not verified: whether OpenAI's or Google's APIs expose an equivalent mechanism at the raw
  function-calling-argument level, or whether the "word-by-word file edit" behavior the user has seen in
  those apps is product-layer UX (e.g. a bespoke canvas/apply-patch tool with its own presentation) built
  on something other than generic per-parameter argument streaming. Named as unverified rather than
  assumed symmetric with Anthropic's mechanism — AgentEngine ships a real Anthropic backend today, so
  that's the one with confirmed, checkable ground truth; OpenAI's equivalent (if any) needs its own
  research pass before design assumes it exists.

## What a design would need to include (not designed here — scoping only)

1. **A per-tool declared opt-in field**, reusing Anthropic's own term directly (matching this session's
   precedent of reusing OpenRouter's real `session_id` header name verbatim rather than inventing a
   parallel vocabulary for the same wire concept) — threaded through to
   `anthropic::detail::build_request_body` at minimum; OpenAI's path needs its own research before
   assuming an equivalent field exists there.
2. **A genuinely new streaming shape, not a repurposing of today's `ToolCallDelta`.** The RFC's own text
   already keeps "model still generating arguments" and "tool reporting progress" as distinct concepts;
   conflating them under one payload would undo that distinction on purpose. Likely shape: `ModelDelta`
   already carries a `ContentItem`, and `ContentItem`'s variant already includes `ToolCall` — the
   accumulator would need to emit PROGRESSIVE `ToolCall`-shaped deltas (growing `arguments_json`, or a
   fragment-only shape) instead of withholding until `finish()`, which is real, non-trivial work inside
   `StreamingUpdateAccumulator`, not just a wiring change.
3. **One hard invariant, carried over unchanged from both the RFC and Anthropic's own contract:**
   `invoke_tool()` (`core/tool_pipeline.hpp:460`) must never see anything but a complete,
   `json::parse`-validated `json::Value` — exactly as true today. The live-render stream and the actual
   dispatch path stay fully decoupled; a UI can render malformed-mid-stream text freely, but nothing
   downstream of that rendering may act on it until the real, validated `ToolResult`/`ToolCallRequest`
   boundary is crossed, same as it already works for ordinary (non-streamed) tool calls.
4. **A real "report invalid JSON back to the model" path**, mirroring Anthropic's own documented
   recovery pattern, rather than the accumulated-but-unparseable case silently failing or crashing
   `invoke_tool()`'s existing `json::Value`-typed boundary.

## Why this needs a red-team pass before implementation, not just a wire-up

This sits directly in the streaming hot path (013's own performance-sensitive territory) and is I3-
adjacent: it's a new, observable surface for PARTIALLY-GENERATED, UNVALIDATED model output, flowing
toward something that eventually becomes tool-invocation arguments. The critical question a red-team
pass needs to stress-test: can any code path downstream of the new streaming delta be tempted to act on
partial/unvalidated data (the same class of mistake this project's own I3 discipline exists to prevent),
the way a naive "speculative apply" implementation might be tempted to write partial file content to
disk before the full call is validated and approved. This project's own established pattern
(`dynamic-multi-agent-fanout-design-draft.md`) is design draft → adversarial red-team → corrected design
before any code — the same treatment this topic should get before implementation, not a plain wire-up.
