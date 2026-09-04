# 003 — Message and Content Model

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Amended 2026-09-04** (§2, `decisions/ADR-173-system-channel-taint-fence.md` / GitHub issue #61: taint had no obligation at the serializer, so every conformer legitimately dropped it on the way to the wire) · **Depends on:** (historical: originally also depended on Quark 003/016 — ADR-037 removed that dependency) · **Used by:** 004, 005, 011, 012, 013 · **Gate:** §7

## Goal

One content model, shared by the agent core, every provider, and every protocol surface. If MCP,
A2A, AG-UI, and three vendor APIs each get their own message type, every feature costs four
conversions and the conversions are where content gets lost.

## 1. Shape

```
Message  = { role, content[], metadata, message_id, created_at }
role     = system | user | assistant | tool
Content  = Text | Reasoning | Image | Audio | Video | File | Data
         | ToolCall | ToolResult | Error | Custom
```

**`Citation` is not a `Content` kind (§8 Q1, resolved 2026-08-04)** — it is an annotation carried on
`Text` (below), matching how providers actually represent it (A2A and MCP both lean annotation) and
closing a redundancy the original table had: `Text`'s "optional annotations (spans, citations)" and
a separate top-level `Citation` item were two shapes for the same thing, and the separate item made
"bound to a text span" an indirect reference instead of the direct attachment this is.

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
| `ToolCall` | `{call_id, tool_name, arguments (JSON)}` |
| `ToolResult` | `{call_id, content[], is_error, usage}` |
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
  reads, remote agent responses, web content — is **tainted**, transitively. **So is all
  model-generated content** — `origin=assistant` text, `ToolCall.arguments`, and any code the model
  generates — per 007 §1's threat model, which lists model output itself among what's assumed
  hostile ("Model output — text, tool arguments, and generated code. Models are steerable by
  anything in their context."). Assistant-origin is not a trust boundary; the taint trigger is about
  whether content can carry attacker-influenced instructions, and a model's own output qualifies
  exactly like retrieved content does.
- Taint is a *type-level* marker on the accessor, not a bit someone remembers to check. For text and
  bytes it is `TaintedText`, which does not implicitly convert to the `std::string_view`/byte-span
  capability APIs take. For structured JSON content — `Data`'s value, `ToolCall.arguments` — the same
  mechanism applies as `Tainted<T>`: a single opaque wrapper around the whole JSON value, not
  field-level propagation, so no individual field can be read out untainted without declassifying the
  item as a whole. This matches §8 Q3's per-item (not sub-span) granularity decision, extended to
  structured content, and reconciles the two names used across this project: `TaintedText` is
  `Tainted<T>` specialized for the text/bytes case, not a separate mechanism. This is the mechanism
  enforcing **I3** and the static half of the prompt-injection defense (017).
- Untainting requires an explicit, logged decision (a sanitizer, a schema validation, or an
  operator policy), never an implicit cast.
- **Taint carries a wire-level obligation, not only a type-level one** (amended 2026-09-04,
  `decisions/ADR-173-system-channel-taint-fence.md`, GitHub issue #61). Everything above describes
  taint as a marker on an *accessor* — which left a real gap this section did not name: a
  `ContextProvider` may legitimately place tainted content in the `role::system` channel (memory,
  RAG, todo, reflection feedback, and summarizer output all do, correctly), and every serializer was
  then free to emit its text with nothing marking it, because no rule here said otherwise. It is not
  an untainting, so the bullet above never applied; the content simply reached the model as bytes
  indistinguishable from host-authored instructions. **A conformer that emits tainted content on the
  `role::system` channel must delimit it on the wire, with markers the content itself cannot
  produce, and must state the reading rule once per request.** This is a marking obligation, not a
  claim that a model cannot be persuaded by fenced text — see the ADR's §5 for what is and is not
  claimed. Conformers: `protocol/anthropic/chat_client.hpp`, `protocol/openai/chat_client.hpp`;
  shared mechanism: `core/system_channel_fence.hpp`.
- Related, and settled by the same ADR: **only host-authored text may claim `content_origin::system`
  with `tainted = false`.** ADR-066 §5 established that a `ContextProvider` is compiled-in code and
  so is entitled to claim `::system` for text *it* composes; that entitlement does not extend to
  text a model produced, however trusted the provider relaying it. `HistoryProvider`'s summary was
  the one place in the tree that crossed this line.

**This section's extension of the taint trigger to assistant-origin content is security-critical
and invariant-touching (I3).** Closing the textual contradiction here is not the same as this being
Reviewed: per the review-signoff workflow's sign-off rule for invariant-touching changes
(docs/planning/v1-review-signoff-workflow.md §3), it still owes the full
design→red-team→prove→judge cycle and an ADR under `decisions/` before this section can move past
Draft on the strength of more than this edit alone.

## 3. Large content: `BlobRef`

Content above a configured threshold is stored out-of-line as a `BlobRef{digest, media_type, size,
store}` and passed by reference through session state, checkpoints, and protocol frames (historical:
"mailboxes" before ADR-037 removed Quark's actor mailboxes; the reference-not-copy discipline is
unchanged).

Rationale: keep the in-line message descriptor small (historical: one cache line, matching Quark's
own descriptor budget — Quark 003 — before ADR-037 removed that dependency), store message payloads
separately, and never copy a 40 MB PDF per turn. Blob storage is a seam (019) with an in-memory
default and file/object-store backends. **Digests are content-addressed**, which makes artifact
identity, dedup, replay, and audit fall out for free.

## 4. Structured output

`OutputSchema<T>` (002) declares a typed run result. Three enforcement strategies, selected by
provider capability (004) and recorded in the trace:

1. **Native** — provider-enforced JSON schema / constrained decoding. Preferred.
2. **Tool-shaped** — the schema is presented as a single forced tool call.
3. **Parse-and-repair** — bounded re-ask on validation failure. Last resort; counted in metrics
   because a high repair rate is a provider or prompt defect worth surfacing.

**Validation is type-driven parsing against the declared C++ shape, matching MCP's tool-argument
validation, so one mechanism serves both** (corrected 2026-08-14, `decisions/ADR-058-agent-output-
live-wiring.md` §4 C1/C2 — this line previously described validation as a generic JSON Schema
2020-12 rule engine; direct inspection found neither `OutputSchema<T>` nor tool arguments were ever
checked against one, and the schema this codebase's own generator (`schema::json_schema_of<T>`)
compiles is structurally incapable of expressing anything beyond type/required-ness — no `pattern`,
length, numeric bound, or `enum` constraint is ever emitted — so a real rule engine run against these
schemas would reject nothing a type-driven parse doesn't already reject. If the schema generator is
later enriched to emit those constraints, revisit whether a real validator earns its cost then; until
it is, one exists to enforce nothing beyond what type-driven parsing already does).

## 5. Serialization and the wire

- **In-process**, messages are C++ types; no serialization on the local path.
- **Durable**, they serialize through `rt::SessionStore`/`rt::AppendLogStore` (historical: this was
  Quark 016's canonical tagged encoding — one `describe` per type, negotiated tagless fast path,
  schema evolution + migrations — before ADR-037 removed Quark; AgentEngine has no multi-node
  cluster story to serialize *for* any more, so "cross-node" is out of scope, not carried over).
- **Cross-protocol**, each surface owns its own mapping module (`protocol/*/mapping.hpp`) with a
  **round-trip test as the gate**: `internal → wire → internal` must preserve every content item,
  including unknown ones (§1).

**Evolution rule:** adding a `Content` kind is additive and must not break an older peer — unknown
content degrades to a typed placeholder that still round-trips.

## 6. Usage accounting

`Usage{input_tokens, output_tokens, cached_input_tokens, cache_write_tokens, reasoning_tokens,
cost_estimate}` attaches to responses and aggregates per turn, run, session, and agent version. It is
a first-class part of the model, not a telemetry side-effect, because budgets (002 `TokenBudget<N>`)
enforce against it.

**Amendment (2026-08-07, Milestone 5 Phase D/E follow-up):** `cache_write_tokens` added —
`cached_input_tokens` already covers a cache READ (tokens served from a previously cached prefix,
cheaper); real backends also report the symmetric cache WRITE cost (tokens spent establishing a new
cache entry, e.g. Anthropic's `cache_creation_input_tokens`, OpenRouter's `usage.prompt_tokens_details.
cache_write_tokens`) which had no field to land in — see
`docs/research/2026-08-07-provider-metadata-and-sampling-params-survey.md` Finding 5. Purely additive
(defaults to 0), not a breaking change to any existing conformer.

## 7. Promotion gate

- **G1 (round-trip fidelity)** — for each protocol mapping module (A2A, MCP, AG-UI, OpenAI-compatible
  — 011/012/013), `internal → wire → internal` preserves every `Content` item bit-for-bit, including
  order, across a corpus covering every kind in §1's table. This is §5's stated gate, made explicit
  and mandatory here rather than left as a design note.
- **G2 (forward-compatible unknown content)** — a peer sending a `Content` kind this build does not
  recognize degrades to a typed placeholder that still round-trips untouched on the next hop; a build
  that silently drops it instead is caught by a negative control, not merely described as prevented.
- **G3 (taint is statically enforced, not conventionally observed)** — a compile-fail test proves
  `TaintedText`/tainted byte accessors have no implicit conversion to the untainted `std::string_view`
  capability-API surface (007). This is I3's static half: the proof is a build failure, not a runtime
  assertion.
- **G4 (encrypted reasoning never leaks in plaintext)** — positive control: a deliberately mis-wired
  logger, checkpoint writer, or cross-provider forward path that would surface `encrypted` `Reasoning`
  content in plaintext is caught by the test, across all three paths named in §1.
- **G5 (`BlobRef` addressing)** — content at or above the configured threshold is always stored
  out-of-line; two byte-identical large payloads produce the same digest; content fetched by digest
  matches the original bytes exactly, across every configured blob backend (019).
- **G6 (structured output enforcement)** — for a schema-bearing run, all three §4 strategies (native,
  tool-shaped, parse-and-repair) produce output that either validates against the declared schema or
  surfaces a classified failure — never an unvalidated value reaching the caller — and a forced repair
  increments the metric §4 requires.
- **G7 (usage accounting rolls up correctly)** — `Usage` recorded per response aggregates correctly at
  every declared level (turn, run, session, agent version), and a run that crosses its declared
  `TokenBudget<N>` (002) is actually halted by the aggregated total, not merely computed for display.
- **G8 (MAF content-model equivalence, since §1 makes the claim)** — a message built from real MAF
  `AIContent` instances (`TextContent`, `DataContent`, `FunctionCallContent`, `FunctionResultContent`,
  and reasoning/error/citation equivalents where MAF has them) converts into this model and back
  without loss — verifying §1's "matching MAF's `AIContent` family" claim against MAF's actual shapes,
  not leaving it asserted by name only.

## 8. Open questions

- ~~**Q1** — Whether `Citation` should be a `Content` item or an annotation on `Text`. Providers
  disagree; A2A and MCP both lean annotation.~~ **Resolved, annotation on `Text` (2026-08-04):** see
  §1 — matches provider consensus (A2A, MCP), matches what the `Text` row already specified, and
  removes a redundancy the table had been carrying (two shapes for one concept). `Citation` is no
  longer a top-level `Content` kind.
- ~~**Q2** — Cross-provider reasoning: some providers require their own reasoning blocks be echoed
  back. The pass-through rule handles it, but multi-provider sessions need a stated policy.~~
  **Resolved, exclude from context assembly, never translate (2026-08-04):** §1's existing rule for
  `encrypted` `Reasoning` ("never sent to a different provider than the one that produced it")
  generalizes to *all* `Reasoning` content, encrypted or not — even unencrypted reasoning is
  vendor-specific chain-of-thought formatting, not a portable representation, and some vendor APIs
  actively reject a reasoning block they don't recognize as their own rather than merely ignoring it.
  Mechanically this needs no new machinery: a `Reasoning` item is included in a turn's assembled
  context (005 §3) only when it originated from the `ChatClientId` currently bound; a different-
  provider item is **excluded from that turn's context**, not deleted — it stays intact in the
  durable `history[]` (available for audit, display, and round-trip, G2), and the exclusion is
  recorded in the trace via 005 §3's existing drop-recording rule, the same treatment any other
  budget-driven drop already gets. No summarization or translation is attempted; omission is the
  honest behavior for content that is opaque by design.
- ~~**Q3** — Whether taint should be tracked at sub-string granularity (span-level) rather than
  per-item. Span-level is strictly better and materially harder.~~ **Resolved, No (OQ-5,
  2026-08-04):** "materially harder" undersold the cost — span-level taint would regress the
  guarantee's *kind*, not just its implementation effort. §2's `TaintedText`/`Tainted<T>` is a
  compile-time property today (007 §4/G2: no implicit conversion, proven by a compile-**fail**
  test) precisely because it is a whole-value type tag. Span-level taint needs a runtime interval
  structure over a string's bytes, which cannot be checked at compile time — a "does this span-
  tainted value expose any tainted subrange to an untainted accessor" question is inherently a
  runtime one. That downgrades I3's static half (003 G3, 007 G2) to a runtime assertion for
  everything that touches text, in every protocol mapping module doing a round-trip, for a benefit
  (less over-tainting friction) that is about UX precision, not security — coarse per-item taint
  never *under*-taints, so it is already sound, just conservative. Kept per-item.

- ~~**Q3** — Whether taint should be tracked at sub-string granularity (span-level) rather than
  per-item.~~ **Resolved 2026-08-03 (see OpenQuestions.md OQ-5,
  `decisions/ADR-007-span-level-taint-vs-per-item.md`): no, keep per-item.** A small prove found
  span-level taint is not merely "materially harder" as this line assumed — a naive, otherwise
  plausible `concat` implementation silently under-taints genuinely tainted bytes on the very first
  attempt, a security-relevant bug class per-item taint cannot have because it carries no offsets to
  mis-shift. The concrete precision gap this question worried about (a message mixing trusted and
  tainted material) is already solvable today via 017 §3's structural-separation idiom (two
  `ContentItem`s instead of one mixed string), at the same precision, with none of that risk.

  What was actually motivating this is served better elsewhere: `Citation` (§1) already anchors a
  text *span* to a source for **display/attribution**, independent of the security-enforcement
  taint bit. If §1's open Q1 (Citation placement) resolves toward a structured span-provenance
  annotation, that gives assistant output the "which part came from where" precision this question
  wanted, without making the taint type itself span-aware or trading away its static guarantee.
