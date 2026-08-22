# Tool-call argument streaming + validation — where it plugs into AgentEngine

**Status:** Design sketch informed by dated external research, **self-red-teamed once, not the full
`design → red-team → prove → judge` cycle CLAUDE.md requires before code** (this is I3/I5-adjacent —
new-shape model output flowing toward tool dispatch, and an extension of the recorded seam). Companions:
`docs/research/2026-08-22-tool-call-streaming-validation-landscape.md` (the external survey this maps),
`docs/planning/tool-call-argument-streaming-gap.md` (the original gap this extends — display-side scoping),
`docs/planning/tool-call-result-streaming-gap.md` (the sibling gap, results not arguments),
`docs/planning/model-call-gateway-routing-design-draft.md` Finding 2 (the same "extend the I5 recorded seam"
move, for gateway retries instead of tool-call fragments), `docs/planning/quickstart-builder-streaming-gap.md`
(names all of the above as one coordinated pass, not four separate patches).

## 0. The blocker this pass found that the earlier gap doc didn't name

`ChatRequest` (`core/chat_client.hpp:70-81`) deliberately elides **all** sampling parameters — the file's own
top comment, referenced again at line 81 (`// sampling parameters: elided (see file-top comment)`). Grammar
constraints, a per-tool `strict` flag, and Anthropic's `eager_input_streaming` are exactly the class of
per-call backend knob that elision exists to keep out of this struct. This is the real reason none of the
three "what a design would need" items in `tool-call-argument-streaming-gap.md` are simple wiring: they all
need a **named, scoped carve-out** of that elision, not a blanket reopening of it.

There is already a precedent for exactly this kind of carve-out, and it's the right shape to copy: `
output_schema_json` (`chat_client.hpp:75-80`) is schema-shaped, not a raw sampling parameter, and the file's
own comment explains why it gets to exist despite the elision — "003 §4's `OutputSchema<T>` contract,
compiled to its JSON Schema text." A new field for tool-argument schema enforcement should get the same
treatment: named, scoped, justified by citation to the RFC/ADR that requires it — never a general "add a
knobs bag" field.

## 1. The strategy pattern to reuse, not reinvent

003 §4 / 004 §2 already solved the general problem "guarantee schema conformance across heterogeneous backend
capability" — for the **response**, not tool-call arguments, but the same problem shape:

```cpp
// core/chat_client.hpp:234, :247-252 — real code, already shipped
enum class output_schema_strategy { native, tool_shaped, parse_and_repair };

[[nodiscard]] inline output_schema_strategy select_output_schema_strategy(
        ChatClientCapabilities const& caps) noexcept {
    if (caps.structured_output_native) return output_schema_strategy::native;
    if (caps.tool_calling)             return output_schema_strategy::tool_shaped;
    return output_schema_strategy::parse_and_repair;
}
```

`agent_registry.hpp:373-380`'s own comment already names why this degrades to `parse_and_repair` as a
universal last resort rather than failing at build time (004 §2's "fail at startup" clause is "currently
unreachable from here"). **Recommendation: tool-call argument validation gets an analogous strategy enum**,
keyed off new bits on `ChatClientCapabilities` (`chat_client.hpp:35-50`, the same flat-bool-capability struct
`structured_output_native`/`tool_calling`/`json_mode` already live on), rather than a parallel mechanism:

- **`native`** — the backend itself guarantees the final arguments conform (OpenAI per-tool `strict: true`,
  confirmed NOT wired today — `strict` only appears for `response_format` at `openai/chat_client.hpp:209-231`,
  never for individual `tools[]` entries; or, for a self-hosted backend, grammar-constrained decoding, see §3).
- **`tool_shaped`/today's implicit behavior** — accumulate the full argument string, `json::parse` once
  complete (unchanged: `invoke_tool()`, `tool_pipeline.hpp:460`, never sees anything else), and on parse
  failure, round-trip an `is_error: true` `ToolResult` back to the model. **No new mechanism needed for this
  part** — `ToolResult.is_error` already exists and is already set at multiple call sites
  (`tool_pipeline.hpp:374,585,754`); this is a new call site in the invalid-JSON failure path, not new
  plumbing, and it mirrors Anthropic's own documented `{"INVALID_JSON": "..."}` recovery pattern exactly
  (research doc, class D).

The schema itself needs no new plumbing either way: `ToolDescriptor::args_schema_json`
(`core/tool_pipeline.hpp:53-63`) already carries it, reused verbatim by `ContextContribution.tools`
per `context_provider.hpp`'s own top comment (cited at `chat_client.hpp:73`).

## 2. Local backends: grammar-constrained decoding is not currently reachable, confirmed

AgentEngine's llama.cpp/Ollama paths (`tests/test_llamacpp_live_e2e.cpp`, `tests/test_ollama_live_e2e.cpp`)
reuse `OpenAIChatClient` against each server's **OpenAI-compatible** `/v1/chat/completions` endpoint with
`ProviderTransport::plaintext_http` (ADR-016) — there is no native llama.cpp binding anywhere in the tree.
`docs/research/2026-08-21-ollama-openai-compat-api.md` already flags, unresolved, whether Ollama's
`response_format` on that surface implements the full OpenAI Structured-Outputs shape or only schema-less
"JSON mode" — and that finding was about the **response** schema, not tool-call argument schemas, which is
an even less-verified question. **Grammar-constrained decoding (research doc class A) is real and
directly relevant to this project's own backends in principle, but wiring it would mean going through
whatever each server's OpenAI-compat surface exposes for tool definitions specifically — unverified, not
designed here.** This is new scope this pass surfaced; none of the four existing gap docs named it.

## 3. Display tier — separate system, separate risk class from §1/§2

The actual "typing effect" gap (`tool-call-argument-streaming-gap.md`'s core finding) lives entirely in
`StreamingUpdateAccumulator` (`openai/chat_client.hpp:560-701`): `items_from_block()` already accumulates
`PendingToolCall::arguments` fragment-by-fragment but only `finish()` (once, at stream end) converts it to a
real `ToolCall` content item pushed to `out`. Fixing this needs:

1. A best-effort partial-JSON renderer applied **only** to what gets pushed into the live `ModelDelta` event
   — research doc class B's pattern (parse-as-far-as-valid, close open strings/objects), analogous to what
   `jiter`'s `partial_mode` does. This is new C++ code (nothing in-tree does this today), scoped narrowly to
   the outgoing event payload.
2. The accumulator's own internal buffer (`pending_by_index_`) stays exactly as authoritative as it is
   today — `finish()` still does the one real `json::parse`. **Zero change to the dispatch-tier invariant.**
3. `ChatRequest` needs **no new field** for this part — it's purely about how already-arriving bytes get
   exposed early, not about asking the backend for anything different. This is the cheapest, most contained
   piece of the whole design, and does not depend on §0's carve-out question at all.

Event-shape decision (widen `ModelDelta`'s existing `ToolCall` variant vs. a new payload) is intentionally
left open — both `tool-call-argument-streaming-gap.md` and `tool-call-result-streaming-gap.md` already flag
this as the one real decision needing its own pass, and it should be made once for both gaps together
(013's existing `ModelDelta`/`ToolCallDelta` split already keeps "model still generating" and "tool reporting
progress" apart on purpose — whatever shape is chosen must not blur that line back together).

## 4. I5 recording

004 §6: "The `ChatClient` seam is the primary I5 recording point ... Replay serves from the recording with
identical chunk boundaries." Progressive tool-call fragments are chunks like any other — recording them is
an extension of the existing chunk-sequence contract, not a new recording mechanism, the identical framing
`model-call-gateway-routing-design-draft.md` Finding 2 already used for the gateway's attempt sequence.

## 5. Self-red-team (one pass)

- **I3** — does an early, unvalidated fragment ever tempt something downstream to act on it? No: `
  invoke_tool()` (`tool_pipeline.hpp:460`) is untouched by §3; the display tier and dispatch tier are
  different code paths reading the same accumulator at different times, never sharing a result value.
- **Cost/attribution (004 §5)** — §3 and §1's `tool_shaped` tier add no new backend calls (same request,
  earlier exposure of bytes already being sent). Only §2 (grammar, if ever pursued) or a `native` `strict`
  tool flag could change request shape — and per §0, either needs its own named `ChatRequest` carve-out
  before it's real, at which point it gets its own cost/I5 red-team, same as `output_schema_json` presumably
  already had.
- **I2/capability** — the schema driving any of this (`args_schema_json`) is host-authored via the tool's
  declared `Capabilities<...>`, never model-derived. No new capability class needed for §1/§3; §2 (grammar)
  would be an ordinary backend-capability declaration, same shape as any other `ChatClientCapabilities` bit.
- **Scope-creep risk on §0's carve-out** — flagged explicitly so it isn't lost: §3 (the actual UI-facing gap)
  needs **zero** `ChatRequest` changes. Only a `native` strategy tier (OpenAI per-tool `strict`, or grammar)
  touches the elision, and that's exactly the piece this draft recommends deferring (§6).

## 6. Sequencing recommendation

1. **§3 first** — progressive `ToolCall` fragments in `StreamingUpdateAccumulator`, plus the event-shape
   decision (made jointly with `tool-call-result-streaming-gap.md`). No `ChatRequest` surface change, most
   contained, closes the concrete gap the original doc scoped.
2. **The `is_error: true` recovery round-trip alongside it** — `ToolResult.is_error` already exists; this is
   a new call site in `invoke_tool()`'s parse-failure path, not new plumbing.
3. **Defer the `native` strategy tier** (OpenAI per-tool `strict`, grammar-constrained local backends) — it's
   the one part that needs §0's `ChatRequest` carve-out decided first, plus the unresolved Ollama/llama.cpp
   OpenAI-compat surface question from §2. Real, worth doing, but a separate scoped pass.
4. Coordinate timing with the gateway `call_stream()` work (Finding 2) and the quickstart builder's
   `.ask_stream()` per the existing gap docs' own stated preference for one pass — but §0's `ChatRequest`
   carve-out question is new ground this pass surfaced, not something those three already covered.

## Open questions (not resolved here)

1. Widen `ModelDelta`'s `ToolCall` variant vs. a new `RunEventPayload` kind for progressive arguments —
   same undecided question `tool-call-result-streaming-gap.md` names for result content; should be decided
   once, for both.
2. Does llama.cpp's or Ollama's OpenAI-compat `tools`/`response_format` surface do anything with a JSON
   Schema for tool arguments specifically (as opposed to response schema, the only thing verified so far)?
   Unverified — needs a live-server research pass, not assumed.
3. Is `additionalProperties: false` + all-fields-`required` (OpenAI strict mode's actual requirement) already
   satisfied by how `ObjectBuilder`/`json_schema_of<T>()` (`core/json_schema.hpp`) emit tool argument schemas
   today, or would enabling per-tool `strict` require changing schema generation too? Not checked in this
   pass.

## What this draft is not

Not an implementation, not an ADR, not the full red-team pass CLAUDE.md requires before anything here touches
code — this is one level short of `model-call-gateway-routing-design-draft.md`'s own three findings (which
were self-red-teamed but still explicitly marked "not judged or proven"). §3 is the smallest, safest slice to
take through that full cycle first; §1/§2's `native` tier needs its own, separate pass once §0's carve-out
question has an actual answer.
