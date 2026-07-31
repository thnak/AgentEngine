# 004 — ChatClient Plane

**Status:** Draft · **Depends on:** 003, 016, 018 · **Gate:** §7

## Goal

One seam between the engine and every inference API, modelled on **capabilities** rather than on
any vendor's request shape — so that adding a backend is implementing a declared capability set,
and an agent's behaviour degrades explicitly (never silently) when a backend lacks one.

> **Terminology (027 §5, §7).** The type is `ChatClient`, matching MAF, not `Provider` — "provider"
> is colloquially the model *vendor*, and overloading it for the seam invited confusion. This RFC
> is retitled and its type names use `ChatClient`; "provider" still appears where it means the
> vendor/backend identity itself (e.g. a `{provider, model, credential}` metric key), which is the
> colloquial sense 027 leaves alone.

## 1. The seam

```cpp
struct ChatClient {                                 // concept, not a base class
    ChatClientCapabilities capabilities() const noexcept;
    ae::task<result<ChatResponse>>  chat(ChatRequest, EffectContext&);
    ae::stream<ChatResponseUpdate>  chat_stream(ChatRequest, EffectContext&);
};
```

- **Requests carry `EffectContext`** — principal, deadline, trace context, budget — because a model
  call is an attributable effect (**I4**), not a library call.
- **Streaming is a Quark credit-controlled stream** (Quark 024/ADR-018), so a slow consumer stalls
  the provider read rather than growing an unbounded buffer.
- **Cancellation is `stop_token`**, propagated into the HTTP/socket layer.

## 2. Capabilities

`ChatClientCapabilities` is a declared bitset, not a runtime probe:

`streaming` · `tool_calling` · `parallel_tool_calls` · `structured_output_native` ·
`json_mode` · `reasoning` · `reasoning_encrypted` · `prompt_caching` · `multimodal_in{image,audio,
video,file}` · `multimodal_out` · `logprobs` · `stop_sequences` · `seed` · `token_counting` ·
`batch` · `context_window` · `max_output_tokens`

**The degradation rule:** when an agent needs a capability the bound `ChatClient` lacks, the engine
applies a **declared** fallback (e.g. native structured output → tool-shaped, 003 §4) and records
it in the run trace and metrics. It never silently ignores the request, and it never invents a
fallback that changes semantics without saying so. If no fallback exists, `register_agent<A>()`
fails at startup (002 §6) rather than at the first user request.

## 3. Backends in v1

| Backend | Reach | Notes |
|---|---|---|
| **OpenAI-compatible** (Chat Completions + Responses) | Widest: OpenAI, gateways, vLLM/llama.cpp/Ollama-style local servers, most vendor compat endpoints | Default. Capability set is *per endpoint*, discovered from config, not assumed |
| **Anthropic** | Claude 5 family (Fable 5, Opus 5, Sonnet 5), Haiku 4.5 | First-class: reasoning parts, prompt caching, tool use |
| **Local / embedded** | On-device via an OpenAI-compatible server, or a plugin `ChatClient` (009) | Keeps heavy inference deps out of the host process |
| **Remote agent as `ChatClient`** | An A2A peer used where a model would be (012) | Makes "delegate the whole turn" uniform |

**Dependency posture:** `ChatClient` backends are seam backends (CONVENTIONS §Dependencies tier 2)
— one HTTP/TLS dependency, behind a CMake option, never in the core. A `ChatClient` may also ship
as a **WASM plugin** (009 — the `ae:provider` world) when its protocol is exotic and its
performance envelope allows.

**Porting note, not copying:** MAF ships no C++ SDK, and its own Anthropic/OpenAI backends are thin
adapters over each vendor's official SDK (`docs/research/2026-maf-provider-concepts.md` §2) — there
is no vendored HTTP/SSE implementation to port. What *is* worth porting as design, because it is
translation logic independent of language or transport: Anthropic's cumulative→incremental `Usage`
conversion on stream events (needed for uniform per-chunk `Usage`, 003 §6, since Anthropic reports
usage cumulatively per event and OpenAI does not), tool-schema shaping per vendor wire format, the
MCP-tool-vs-native-tool routing split, and structured-output shaping that forces
`additionalProperties: false` into the JSON Schema before it reaches the provider. Each is a
concrete pre-implementation checklist item for its `ChatClient` backend, not a research question.

## 4. Reliability

- **Retry** applies to `Transient` only (001 §6), bounded exponential with jitter, and **must
  respect the remaining deadline** rather than its own timeout — a retry that outlives the caller's
  budget is a bug.
- **Idempotency:** a retried call carries a stable idempotency key so a provider that supports it
  does not double-charge or double-execute.
- **Rate limits and overload** use Quark 022 (token buckets, deadline-aware shedding, circuit
  breaking) rather than a bespoke limiter. A provider-level breaker trips per `{provider, model,
  credential}`.
- **Failover** between providers is *explicit policy*, never implicit: a failover that silently
  changes model is a correctness change, and must appear in the trace and in the response metadata.

## 5. Cost and budget

Every response carries `Usage` (003 §6). The plane enforces:

- per-run `TokenBudget<N>` — exceeded → `Resource` failure at the turn boundary;
- per-session and per-principal ceilings from configuration (020);
- an estimated-cost metric per `{provider, model, agent, principal}` for the metrics surface (016).

Pricing tables are **configuration, not code** — they change weekly and must never require a build.

## 6. Recording and replay

The `ChatClient` seam is the primary I5 recording point: request, response or full ordered chunk
sequence, timing, and usage. Replay serves from the recording with identical chunk boundaries so
that streaming-dependent behaviour (early tool dispatch, UI cadence) reproduces exactly.

Recordings inherit 016's content-capture privacy controls: **off by default**, with metadata-only
and hashed-content modes for environments where prompts may not be persisted.

## 7. Promotion gate

- **G1** — the same agent, unchanged, runs on ≥ 3 backends; behavioural differences are limited to
  those the capability table predicts, and every applied fallback appears in the trace.
- **G2** — cancellation mid-stream releases the connection within a bounded time and leaves no
  orphaned socket or partial state (checked under ASan + a leak gate).
- **G3** — a recorded streamed run replays offline with identical chunk boundaries.
- **G4** — engine overhead per model call (excluding network and provider time) is within the 023
  budget at p50/p99.

## 8. Open questions

- **Q1** — Whether the seam should expose batch APIs as first-class or leave batching to callers.
- **Q2** — Prompt-cache management: providers differ enough that a portable cache-hint abstraction
  may be leakier than exposing per-provider hints.
- **Q3** — Token counting without a provider round-trip requires vendored tokenizers (a natural
  WASM plugin, 009); which tokenizers ship first-party is unresolved.
