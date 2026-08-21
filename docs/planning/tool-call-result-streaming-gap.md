# Tool-call result content never reaches the live event stream

**Status:** Research + gap finding from live conversation (2026-08-21), **not a design draft, not
red-teamed, no code written**. Companion: `tool-call-argument-streaming-gap.md` (the other half of "tool
call content doesn't stream live" — arguments before invocation vs. results after), both surfaced while
answering a user question about rendering a live file-edit diff stat (`editing +9|-10`) in a consumer
UI. Also referenced from `rich-ux-and-governance-surfaces-gap.md`'s "UX update system" section.

## The question that surfaced this

Can a consumer app built on AgentEngine render something like Claude Code's own `editing +9|-10` diff
stat when an agent writes/edits a file? Two real gaps, confirmed against actual code, not assumed.

## Gap 1 — nothing in the engine computes a diff

The only file-write primitive AgentEngine ships is `FileSystemAdapter::write_file(path, data, append)`
(`include/agentengine/sandbox/filesystem_adapter.hpp:44-45`) — a raw byte write/append. No diff or
line-count utility exists anywhere in the tree (checked: no `diff.hpp` or equivalent). A tool wanting to
report `+9/-10` has to read the old content, diff it against the new content itself, and attach the
result as structured data — the engine provides a place to CARRY that (`ToolResult.content` can hold a
`Data{json, schema_id}` or `Custom{type_id, payload_json}` item, `core/content.hpp:81-86,137-142`), not
the diffing logic itself. This part is a non-issue for design purposes — it's ordinary tool-author work,
not an engine gap.

## Gap 2 — the real one: tool result content doesn't reach the live event stream at all

AG-UI's own protocol already defines a `ToolCallResult` event for exactly this
(`protocol/agui/types.hpp` — it's in the `AgUiEvent` variant). But the actual projection code says, in
its own words:

```cpp
// No ToolCallResult: `ToolCallFinished`'s payload carries only `{call_id, ok}`, no
// real result CONTENT to report -- honest scoping, not a dropped event (ToolCallEnd
// still fires).
```

(`include/agentengine/protocol/agui/projection.hpp:112-114`). So today, when a tool call finishes, a
consumer's UI gets `ToolCallEnd{call_id}` and a bare success/fail bool — never the tool's actual result
content, structured or otherwise. Confirmed at the source: `run_event_payload::ToolCallFinished`
(`core/run_event.hpp:86-89`) is literally `{call_id, bool ok}`. A UI can only see result content (a diff
stat, or anything else a tool computed) after the fact, by reading the `ToolResult` content item once it
lands in session history/messages — not as a live streamed update.

## What a real design would need to resolve (not designed here — scoping only)

1. **Where does the real content get threaded from.** `invoke_tool()` (`core/tool_pipeline.hpp:460`)
   already produces a full `ToolResult` with real content (step 9, `:574-596`) — the data exists at the
   point of invocation. The gap is purely that `AgentSession`'s emission of `ToolCallFinished`
   (wherever that call site is — not located in this pass) only forwards `{call_id, ok}`, discarding
   the rest. Widening the payload (or emitting a distinct new event) is additive, not a redesign of the
   pipeline itself.
2. **Payload size/taint.** `ToolResult` content items are explicitly `tainted = true` by convention
   (`tool_pipeline.hpp:594`, "external content and the primary prompt-injection vector," 006 §7) — a
   live-streamed result-content event inherits RFC 013's own existing "sensitive-content aware" rule
   ("events carry the same taint and capture policy as telemetry... a surface configured for
   metadata-only never sees content it should not," 013 §1 Properties) rather than needing a new policy
   invented from scratch. Large content (e.g. a whole rewritten file) likely wants the same
   out-of-line `BlobRef` treatment `core/content.hpp:65-72` already gives oversized bytes elsewhere,
   not inlined whole into an event.
3. **New `run_event_kind` vs. widening `ToolCallFinished`.** Either add a real payload to
   `ToolCallFinished` (breaks nothing structurally, `RunEventPayload`'s variant already has room) or add
   a sibling `tool_call_result` kind fired alongside it — needs a real decision, not assumed; the
   `ToolCallDelta`/`ModelDelta` split (see the companion doc) already shows this project keeps distinct
   *concepts* on distinct event kinds rather than overloading one payload for two purposes.

Not red-teamed. The most likely-contested part of a real design is the taint/size question (#2) — that's
the one to scrutinize hardest before implementing, since it's the one directly touching what an external,
possibly-untrusted surface gets to see.
