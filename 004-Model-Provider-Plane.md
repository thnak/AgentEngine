# 004 — ChatClient Plane

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 003, 016, 018 · **Gate:** §7

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
- **Outbound credentials follow 018 §3's rule, not an exception to it.** A native `ChatClient`
  backend is constructed with a `SecretRef` (018 §4) identifying its provider credential, never a
  resolved value — nothing a backend holds across calls can leak through a crash dump or a config
  dump. Resolution happens inside `chat()`/`chat_stream()`, against the `SecretStore` capability
  reachable through `EffectContext&`, at the point of use — the same `SecretStore::resolve(SecretRef,
  EffectContext&)` shape 018 §4 defines for plugins. A `ChatClient` backend is a native seam backend,
  not a plugin, but it earns no exemption from "never read into a config struct at startup."

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

**Amendment (2026-08-07, ADR-020 — `reasoning_effort` on `ChatRequest`).** §1's sampling-parameter
elision stands for `temperature`/`top_p`/`top_k`; reasoning effort is carved out of it as the one
knob with a defensible portable shape, and is added to `ChatRequest` as
`std::optional<reasoning_effort>` over `enum class reasoning_effort {off, low, medium, high}`.

This is an **abstraction each backend maps down**, never a vendor field passed through. The three
surveyed native shapes genuinely differ — OpenAI's flat string enum, Anthropic's
`thinking:{type, budget_tokens}` token *budget*, Ollama's narrower level set
(`docs/research/2026-08-07-provider-metadata-and-sampling-params-survey.md` Finding 2) — so the
portable vocabulary is the ordinal intersection they agree on, and nothing wider:

- **`minimal` is deliberately excluded.** It exists only on OpenAI; Ollama has no equivalent.
  Admitting it would make this OpenAI's enum with a new name, which is precisely the "one vendor's
  shape leaking onto every other backend" trap §8 Q2's own resolution already avoided for prompt
  caching. Its absence is what makes this a portable abstraction rather than a pass-through.
- **`nullopt` and `off` are different requests**, not two spellings of one. `nullopt` is *no
  opinion* — the backend sends no field at all and the vendor default applies, which is exactly
  today's behaviour and keeps this amendment additive. `off` is *explicitly disable reasoning*, and
  every surveyed backend can express it (OpenAI/llama.cpp/Ollama `none`, Anthropic
  `thinking:{type:"disabled"}`).

**Interaction with the degradation rule.** `low`/`medium`/`high` require the already-declared
`reasoning` capability bit; against a backend that lacks it the call fails with `failure_class::
contract` rather than dropping the field, because no *declared* fallback for reasoning effort
exists and silently omitting it is the "silently ignores the request" this section forbids. `off`
is exempt: a backend that cannot reason satisfies "do not reason" by construction, so requiring a
capability bit to ask for less would fail a caller who sets `off` defensively across a fleet of
mixed backends, for no gain in honesty.

**Backends map, and enforce their own native constraints.** A gateway may not: OpenRouter was
measured accepting `budget_tokens == max_tokens` and `budget_tokens < 1024` with HTTP 200 (ADR-020
§2), both of which `api.anthropic.com` itself rejects. A backend that satisfies a level only by
violating its vendor's documented constraint fails closed rather than sending a request that works
against the lenient hop and breaks against the strict one.

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
  breaking) rather than a bespoke limiter. A provider-level breaker trips per `{tenant, provider,
  model, SecretRef}` — tenant is part of the key, not an afterthought, so one tenant tripping a
  breaker cannot silently degrade another's calls through the same provider (018 §6).
- **Failover** between providers is *explicit policy*, never implicit: a failover that silently
  changes model is a correctness change, and must appear in the trace and in the response metadata.

## 5. Cost and budget

Every response carries `Usage` (003 §6). The plane enforces:

- per-run `TokenBudget<N>` — exceeded → `Resource` failure at the turn boundary;
- per-tenant, per-session, and per-principal ceilings from configuration (020);
- an estimated-cost metric per `{tenant, provider, model, agent, principal}` for the metrics
  surface (016).

Pricing tables are **configuration, not code** — they change weekly and must never require a build.

**Invariant-touching, ADR-track:** making tenant part of the breaker key (§4) and the budget-ceiling
list and cost-metric key above is what makes 018 §6's "resource limits and cost budgets are per
tenant" actually true for model calls, not just asserted — I8 depends on it. Per this project's
review workflow (`docs/planning/v1-review-signoff-workflow.md` §3), a change that touches an
invariant this directly doesn't clear on a textual fix alone: it still owes the full
design→red-team→prove→judge cycle and an ADR under `decisions/` before this section is more than
Draft-consistent, the same posture the roadmap already flags for other security-critical items
(`docs/planning/v1-implementation-roadmap.md`'s ADR-track call-outs).

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

- ~~**Q1** — Whether the seam should expose batch APIs as first-class or leave batching to callers.~~
  **Resolved, first-class, as an instance of existing async machinery, not a new one (2026-08-04):**
  a vendor batch call (submit now, complete hours later, poll or webhook) is structurally the same
  shape already unified for OQ-4 — long-running work with a "come back later" completion model. §2's
  capability bitset already reserves a `batch` bit; wiring it up means a batch-eligible `chat()` call
  is classified `Backgroundable` (006 §6b) and completes via 019 §2's existing wake-condition table
  ("Remote task completion"), not a bespoke batch-tracking structure. Opting in is an explicit policy
  choice (it trades latency for cost), gated the same way any other policy is (002 §3's "changes what
  the agent is" test), not automatic.
- ~~**Q2** — Prompt-cache management: providers differ enough that a portable cache-hint abstraction
  may be leakier than exposing per-provider hints.~~ **Resolved, no new hint abstraction — the
  existing context-assembly segmentation already is the boundary a caching backend needs
  (2026-08-04):** 005 §3's context formula (`instructions ⊕ context_provider outputs ⊕ selected
  history window ⊕ tools ⊕ middleware`) is already ordered roughly stable-to-volatile without having
  been designed for caching. A backend declaring `prompt_caching` inserts its own vendor-specific
  breakpoints at those existing segment boundaries as backend-internal translation logic (explicit
  `cache_control`-style markers where a vendor needs them, nothing at all where a vendor caches
  automatically) — matching §3's "Porting note" pattern of vendor translation living inside each
  backend, not at the seam. A portable byte-offset/breakpoint API would just be one vendor's shape
  leaking onto every other backend, the "per-provider hints in disguise" outcome the question was
  trying to avoid; the segment boundaries already specified elsewhere avoid that without a new type.
- ~~**Q3** — Token counting without a provider round-trip requires vendored tokenizers (a natural
  WASM plugin, 009); which tokenizers ship first-party is unresolved.~~ **Resolved by scope, not by
  naming packages (2026-08-04):** first-party tokenizer plugins (009 WASM components) ship only for
  the backends §3 already commits to as default/first-class — OpenAI-compatible and Anthropic — one
  tokenizer each, pinned with the same deliberate-upgrade discipline already applied to Wasmtime
  (009 §11 Q4). Every other backend's `token_counting` bit is simply absent rather than approximated
  — 002 §2's honest-degradation rule (no fallback exists → fail fast at `register_agent`, never a
  silent approximation) applies here exactly as it does to any other missing capability. This
  deliberately does not commit to shipping a growing, open-ended library of vendored tokenizers with
  their own CVE/update burden (010 §10 Q1 names the same cautionary shape for the interpreter image).
  Which specific tokenizer libraries are appropriate to vendor — licensing, offline availability,
  update cadence — is dated implementation-phase research (CLAUDE.md), not asserted here.
