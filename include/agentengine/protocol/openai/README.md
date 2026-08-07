# `agentengine::openai`

The OpenAI-compatible surface (Chat Completions, the default `ChatClient` backend) from
**004-Model-Provider-Plane.md §3**.

`chat_client.hpp` — `OpenAIChatClient<Store>` (Milestone 5 Phase D, `AGENTENGINE_WITH_HTTPS`-gated):
a real, product-code `ChatClient` conformer over Phase C's `perform_provider_https_exchange`. Wire
format was sourced directly from the official OpenAI .NET SDK's generated serialization code, not
paraphrased from documentation. Covers request/response translation, tool-schema shaping,
structured-output shaping (`additionalProperties:false` injection), and SSE/chunked-transfer
streaming into `ae::stream<ChatResponseUpdate>` — see the header's own top comment for exactly what
is and isn't built (notably: the underlying network fetch is not low-latency incremental yet, and the
Responses API shape is not implemented, only Chat Completions).

Tested in `tests/test_openai_chat_client_translation.cpp` (pure translation/parsing logic, offline,
literal wire-format JSON) and `tests/test_openai_chat_client_live.cpp` (end-to-end against a real
local TLS server, both `chat()` and `chat_stream()`).
