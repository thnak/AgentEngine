# 003 — Message and Content Model

**Status:** Draft · **Depends on:** Quark 003/016 · **Used by:** 004, 005, 011, 012, 013

## Goal

One content model, shared by the agent core, every provider, and every protocol surface. If MCP,
A2A, AG-UI, and three vendor APIs each get their own message type, every feature costs four
conversions and the conversions are where content gets lost.

## 1. Shape

```
Message  = { role, content[], metadata, message_id, created_at }
role     = system | user | assistant | tool
Content  = Text | Reasoning | Image | Audio | Video | File | Data
         | ToolCall | ToolResult | Citation | Error | Custom
```

**Terminology (027 §7):** the type is `Content`, matching MAF's `AIContent` family — an earlier
draft named it `Part`, which is A2A's word for the same shape. `Part` now appears only in `a2a::`
mapping code and prose describing A2A specifically (012); everywhere else in this project it is
`Content`.

A message is a **sequence of content items**, never a string with attachments. This is the shape
A2A uses (`Message` → `Part[]`, mapped to `Content[]` at our boundary, 012), the shape MCP content
blocks use, and the shape every modern provider API converged on; anything simpler loses
interleaved reasoning, citations, and multi-artifact results.

| Content | Carries |
|---|---|
| `Text` | UTF-8 text, optional annotations (spans, citations) |
| `Reasoning` | Model reasoning content; may be `encrypted` (opaque, pass-through only) |
| `Image`/`Audio`/`Video`/`File` | Bytes **or** URI **or** a `BlobRef` (§3), plus media type |
| `Data` | Structured JSON value + optional schema id |
| `ToolCall` | `{call_id, tool_name, arguments (JSON), origin}` |
| `ToolResult` | `{call_id, content[], is_error, usage}` |
| `Citation` | Source reference bound to a text span |
| `Error` | Structured error surfaced into the conversation |
| `Custom` | Namespaced escape hatch — must carry a `uri`-shaped type id |

**Rules:**

- **Content items are ordered and order is meaningful.** Reasoning interleaved with text stays
  interleaved.
- **Unknown content is preserved, not dropped.** An item a provider does not understand round-trips
  through history untouched, so a session shared between two providers does not degrade.
- **`Reasoning` with `encrypted` content is never inspected, logged, or checkpointed in plaintext**,
  and never sent to a different provider than the one that produced it.

## 2. Provenance and taint

Every content item carries `origin ∈ {user, assistant, tool, system, external}` and a **taint
flag**.

- Content originating outside the trust boundary — tool results, retrieved documents, MCP resource
  reads, remote agent responses, web content — is **tainted**, transitively.
- Taint is a *type-level* marker on the string/bytes accessor, not a bit someone remembers to
  check: `TaintedText` does not implicitly convert to `std::string_view` used by capability APIs.
  This is the mechanism enforcing **I3** and the static half of the prompt-injection defense (017).
- Untainting requires an explicit, logged decision (a sanitizer, a schema validation, or an
  operator policy), never an implicit cast.

## 3. Large content: `BlobRef`

Content above a configured threshold is stored out-of-line as a `BlobRef{digest, media_type, size,
store}` and passed by reference through mailboxes, checkpoints, and protocol frames.

Rationale: Quark's descriptor budget is one cache line, message payloads are stored separately
(Quark 003), and a 40 MB PDF must never be copied per turn. Blob storage is a seam (019) with an
in-memory default and file/object-store backends. **Digests are content-addressed**, which makes
artifact identity, dedup, replay, and audit fall out for free.

## 4. Structured output

`OutputSchema<T>` (002) declares a typed run result. Three enforcement strategies, selected by
provider capability (004) and recorded in the trace:

1. **Native** — provider-enforced JSON schema / constrained decoding. Preferred.
2. **Tool-shaped** — the schema is presented as a single forced tool call.
3. **Parse-and-repair** — bounded re-ask on validation failure. Last resort; counted in metrics
   because a high repair rate is a provider or prompt defect worth surfacing.

Validation is JSON Schema 2020-12, matching MCP's tool schemas, so one validator serves both.

## 5. Serialization and the wire

- **In-process**, messages are C++ types; no serialization on the local path.
- **Cross-node and durable**, they use Quark 016 (one `describe` per type, canonical tagged
  encoding with a negotiated tagless fast path, schema evolution + migrations).
- **Cross-protocol**, each surface owns its own mapping module (`protocol/*/mapping.hpp`) with a
  **round-trip test as the gate**: `internal → wire → internal` must preserve every content item,
  including unknown ones (§1).

**Evolution rule:** adding a `Content` kind is additive and must not break an older peer — unknown
content degrades to a typed placeholder that still round-trips.

## 6. Usage accounting

`Usage{input_tokens, output_tokens, cached_input_tokens, reasoning_tokens, cost_estimate}` attaches
to responses and aggregates per turn, run, session, and agent version. It is a first-class part of
the model, not a telemetry side-effect, because budgets (002 `TokenBudget<N>`) enforce against it.

## 7. Open questions

- **Q1** — Whether `Citation` should be a `Content` item or an annotation on `Text`. Providers
  disagree; A2A and MCP both lean annotation.
- **Q2** — Cross-provider reasoning: some providers require their own reasoning blocks be echoed
  back. The pass-through rule handles it, but multi-provider sessions need a stated policy.
- ~~**Q3** — Whether taint should be tracked at sub-string granularity (span-level) rather than
  per-item.~~ **Resolved 2026-08-03 (see OpenQuestions.md OQ-5,
  `decisions/ADR-007-span-level-taint-vs-per-item.md`): no, keep per-item.** A small prove found
  span-level taint is not merely "materially harder" as this line assumed — a naive, otherwise
  plausible `concat` implementation silently under-taints genuinely tainted bytes on the very first
  attempt, a security-relevant bug class per-item taint cannot have because it carries no offsets to
  mis-shift. The concrete precision gap this question worried about (a message mixing trusted and
  tainted material) is already solvable today via 017 §3's structural-separation idiom (two
  `ContentItem`s instead of one mixed string), at the same precision, with none of that risk.
