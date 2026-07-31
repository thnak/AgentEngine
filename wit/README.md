# `wit/`

WIT worlds defining the plugin ABI — the contract of record for the WASM Component Model plugin
system, versioned (`ae:tool@1.0.0`, etc.). Governing RFC: **009-Plugin-and-Extension-System.md §2**,
whose worlds table this directory implements:

| World | Implements |
|---|---|
| `ae:tool` | Tools (006): schema, invoke |
| `ae:skill` | A named bundle of instructions + tools + resources |
| `ae:provider` | The model provider seam (004) |
| `ae:memory` | Memory/vector/retrieval store (005) |
| `ae:filter` | Content/safety filter over messages and tool results (017) |
| `ae:codec` | Content transformation: parse, extract, transcode, tokenize (003) |
