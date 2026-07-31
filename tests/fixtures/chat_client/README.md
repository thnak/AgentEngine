# ChatClient fixtures

Hand-authored JSON fixtures for `agentengine::test_support::RecordedChatClient`
(`tests/support/recorded_chat_client.hpp`). Each fixture maps onto one `ae::ChatResponse` (a
`Message` + `Usage`) — see `004-Model-Provider-Plane.md` §6 for the recording/replay concept this
is a test-scoped, hand-authored stand-in for (not an implementation of that feature).

## Schema

```json
{
  "message": {
    "role": "assistant",           // system | user | assistant | tool
    "message_id": "m-1",           // optional, defaults to ""
    "content": [ /* ContentItem entries, see below */ ]
  },
  "usage": {                        // optional, all fields default to 0 / 0.0
    "input_tokens": 10,
    "output_tokens": 5,
    "cached_input_tokens": 0,
    "reasoning_tokens": 0,
    "cost_estimate": 0.0
  }
}
```

Each entry in `content` is one `ContentItem`. Common fields on every entry:

- `kind` (required) — selects which `ContentItem` variant alternative to build.
- `origin` (optional) — `user | assistant | tool | system | external`; defaults to `assistant`.
- `tainted` (optional) — bool; defaults to `false`.

### Supported `kind` values today

- `text` — `{ "kind": "text", "text": "..." }` -> `ae::Text`
- `reasoning` — `{ "kind": "reasoning", "text": "...", "encrypted": false }` -> `ae::Reasoning`
- `tool_call` — `{ "kind": "tool_call", "call_id": "...", "tool_name": "...", "arguments_json": "..." }`
  -> `ae::ToolCall`

`ContentItem`'s other variant alternatives (`Media`, `Data`, `ToolResult`, `Citation`, `Error`,
`Custom`) are **not implemented** in the loader yet. Adding one is extending the `if (kind == ...)`
dispatch in `parse_content_item` inside `tests/support/recorded_chat_client.hpp` — it is not a
redesign of this schema or of `RecordedChatClient`.

## Streaming

There is no fixture schema for a `ChatResponseUpdate` sequence yet. `RecordedChatClient::chat_stream`
is a stub (see the class's top comment) because `chat_client.hpp`'s `chat_stream` is intentionally
unconstrained until real streaming vocabulary (`ae::stream<T>`) exists. A future streaming fixture
would most naturally be a JSON array of objects shaped like `{ "delta": <ContentItem>, "is_final":
bool }`, reusing the same `parse_content_item` dispatch for `delta` — left undone here rather than
speculatively designed against a type that doesn't exist yet.

## Fixtures in this directory

- `simple_reply.json` — plain text response, exercises the basic path and `Usage`.
- `tool_call.json` — a response whose only content item is a `ToolCall`.
- `reasoning_and_text.json` — multi-part response mixing `Reasoning` and `Text`, with a
  reasoning-token-focused `Usage`.
