# `agentengine::anthropic`

The Anthropic Messages API surface (the "first-class" `ChatClient` backend — reasoning parts, prompt
caching, tool use) from **004-Model-Provider-Plane.md §3**.

`chat_client.hpp` — `AnthropicChatClient<Store>` (Milestone 5 Phase E, `AGENTENGINE_WITH_HTTPS`-gated):
a real, product-code `ChatClient` conformer over Phase C's `perform_provider_https_exchange`, mirroring
`protocol/openai/chat_client.hpp`'s structure. Wire format was sourced directly from the official
Anthropic C# SDK's generated model code, not paraphrased from documentation. Covers request/response
translation (including the system-prompt/tool-role message reshaping Anthropic's own model requires),
tool-schema shaping, native structured-output shaping (`output_config.format`), prompt-cache
breakpoints at the system+tools boundary, and SSE streaming (Anthropic's named-event shape, distinct
from OpenAI's) into `ae::stream<ChatResponseUpdate>` — see the header's own top comment for exactly
what is and isn't built (notably: extended-thinking blocks are response-parsing only, never
round-tripped back to the API, since this project's content model has nowhere to keep the required
`signature`).

Tested in `tests/test_anthropic_chat_client_translation.cpp` (pure translation/parsing logic, offline,
literal wire-format JSON) and `tests/test_anthropic_chat_client_live.cpp` (end-to-end against a real
local TLS server, both `chat()` and `chat_stream()`).
