# `wit/`

WIT worlds defining the plugin ABI — the contract of record for the WASM Component Model plugin
system, versioned (`ae:tool@1.0.0`, etc.). Governing RFC: **009-Plugin-and-Extension-System.md §2**,
whose worlds table this directory implements:

| World | File | Implements |
|---|---|---|
| `ae:tool` | [`ae-tool.wit`](ae-tool.wit) | Tools (006): schema, invoke |
| `ae:skill` | not yet authored | A named bundle of instructions + tools + resources |
| `ae:provider` | not yet authored | The model provider seam (004) |
| `ae:memory` | not yet authored | Memory/vector/retrieval store (005) |
| `ae:filter` | not yet authored | Content/safety filter over messages and tool results (017) |
| `ae:codec` | not yet authored | Content transformation: parse, extract, transcode, tokenize (003) |
