# ADR-049 — Cross-provider Reasoning exclusion (gap 20, 003 §8 Q2)

**Status:** Proposed (2026-08-14). Designed, self-red-teamed, implemented, and proven (real code +
new/extended tests across three files, full suite green); awaiting the project owner's explicit
"Judged" sign-off per this project's governance (`decisions/README.md`; `OpenQuestions.md` OQ-11).

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #20 (the finding this
ADR closes). `003-Message-and-Content-Model.md` §8 Q2 (already Resolved 2026-08-04: "exclude from
context assembly, never translate" — this ADR is the first real implementation of that resolution,
not a new design). `decisions/ADR-046-*.md` (this session's other memory/content-model fix,
unrelated in mechanism but touching the same content-model surface).

## 1. The question

**Stated so it has a wrong answer:** does a `Reasoning` content item ever get excluded from a turn's
assembled context when it was produced by a DIFFERENT backend than the one currently bound?

**Before this fix: no such check existed anywhere — the question was structurally unanswerable.**
`Reasoning` (`core/content.hpp`) carried no field recording which backend produced it at all, so
nothing could compare "this item's producer" against "the currently bound backend," regardless of
whether such a comparison was attempted.

## 2. What re-grounding against current code found

- **003 §8 Q2 already resolved the POLICY** (2026-08-04) — "a `Reasoning` item is included in a
  turn's assembled context only when it originated from the `ChatClientId` currently bound... the
  exclusion is recorded in the trace via 005 §3's existing drop-recording rule." This ADR implements
  that already-settled resolution; it does not re-litigate whether cross-provider reasoning should be
  excluded.
- **The audit's own cited blocker (`FailoverChatClient`'s Primary-only identity would falsely flag
  ordinary failover as cross-provider) is now moot** — `FailoverChatClient` was removed outright
  (ADR-036's 2026-08-12 amendment), superseded by `ModelCallGateway`. No wrapper composition with an
  ambiguous "which backend actually answered" identity exists in the current tree to misfire against.
- **005 §3's "existing drop-recording rule" (`ContextDrop`/`assemble_context()`) is NOT the real
  production call site** — `AgentSession::run_rounds()` calls `history_provider_.on_context()`
  directly (`HistoryProvider<Window<N>>` or a composite like `HistoryAndSkillsProvider`), never
  `assemble_context()` (context_assembly.hpp's own top comment: "`AgentSession` itself still wires
  exactly one contributor... widening is deferred to Phase G"). The REAL call site the audit's own
  note named ("a real production call site, `HistoryAndSkillsProvider`, was missed") is `run_rounds()`
  itself, since `HistoryProvider<Window<N>>::on_context()` just copies session history verbatim —
  filtering has to happen after that copy, before the `ChatRequest` is built.
- **Only Anthropic genuinely produces `Reasoning` content in this codebase today** — confirmed
  directly: OpenAI's Chat Completions backend has no reasoning-trace field on a response message at
  all (`protocol/openai/chat_client.hpp`'s own comment, only a `reasoning_tokens` USAGE COUNT exists);
  the only other `Reasoning{}` construction site (`core/response_format_codec.hpp`'s ADR-023 leak-
  scan) synthesizes it from free text as a `text_derived` extraction, not a genuine structured trace.
  This narrows real-world scope considerably without narrowing the mechanism itself, which is backend-
  agnostic.

## 3. The design

`Reasoning` (`core/content.hpp`) gains `producer_chat_client_id` (`std::string`, appended last, 003
§6's field-ordering discipline — every existing positional `Reasoning{text, encrypted}` call site
keeps compiling unchanged), the "vendor:model" runtime string `ChatClientId<"...">`/
`ChatClientRegistry` already use.

**Producing side**: `AnthropicChatClient::producer_chat_client_id() const` returns `"anthropic:" +
model_` — derived from THIS instance's own real construction parameters, deliberately not threaded
in from an agent's compile-time `ChatClientId<...>` policy tag (what determines wire compatibility is
which backend/model actually produced a trace, a real runtime property of the object, not a separate
configuration string that could in principle diverge from it). Stamped at the SINGLE choke point
every emitted `ContentItem` passes through regardless of construction site: `StreamingUpdateAccumulator
::release()` for the streaming path, and threaded as an optional parameter through
`translate_response_block()`/`parse_message_response()` for the one-shot path — both default empty,
so every pre-existing call site (production and test) is unaffected.

**Consuming side**: `AnthropicChatClient`/`OpenAIChatClient` both expose `producer_chat_client_id()`
as their own identity (OpenAI's, though it never PRODUCES `Reasoning`, is needed so the comparison
correctly recognizes and excludes an Anthropic-origin item once a session switches TO OpenAI — the
more common real-world direction). Detected via a new optional, duck-typed concept
`HasProducerChatClientId<T>` (`core/chat_client.hpp`, matching ADR-044's `has_agent_description`/
`has_agent_version` precedent) rather than widening the `ChatClient`/`ModelCallGatewayLike` required
shape — a mock/test `ChatClientT` without this method simply gets no filtering, `if constexpr`, never
a compile error and never a silently-wrong filter.

`AgentSession::run_rounds()` calls a new private `filter_cross_provider_reasoning(contribution,
current_id)` right after `history_provider_.on_context()` returns, gated on `if constexpr
(HasProducerChatClientId<ChatClientT>)`: strips any `Reasoning` item whose stamp doesn't exactly
match the currently-bound backend's own id (Q2's rule is an ALLOWLIST — "included only when it
matches" — so an EMPTY stamp, including every `text_derived` leak-scan item and every pre-existing
record from before this field existed, is excluded too, never assumed safe by default). A message
that becomes empty solely because of this filter is dropped entirely; a message already empty for an
unrelated reason is untouched. Each exclusion fires a real `run_event_kind::policy_decision` — this
project's own 013 §1 vocabulary already had this event kind and payload defined with zero real
producer anywhere (`run_event.hpp`'s own top comment); this is its first real one.

## 4. Self-red-team findings

**A literal 002-`ChatClientId`-policy-tag threading design was considered and rejected.** Wiring the
agent's compile-time `ChatClientId<"vendor:model">` tag through to runtime, rather than deriving
identity from the bound backend instance's own construction parameters, would require either a new
required `ChatClient` concept member (breaking every mock/test conformer) or plumbing an extra
runtime parameter through `AgentSession`'s construction. Deriving from the backend's own real state
instead needed zero concept changes and is arguably MORE correct: what determines whether a
`Reasoning` trace is safe to echo back is which backend/model actually produced it, not a separate
compile-time configuration string that could, in principle, diverge from it.

**Filtering had to run at message granularity, not whole-turn granularity.** `Reasoning` items
typically sit alongside `Text`/`ToolCall` items in the SAME assistant `Message` — dropping the whole
message (matching `ContextDrop`'s own existing message-level shape) would have discarded
legitimately-portable content too. Proven directly (T3): a message carrying both a cross-provider
`Reasoning` item and a `Text` item survives with only the `Reasoning` item stripped.

**Empty/unstamped provenance defaults to EXCLUDED, not included.** A tempting first instinct is
"if we don't know where it came from, assume it's fine" (fail-open) — but Q2's own wording is an
allowlist ("included only when it matches"), and this codebase's existing `text_derived` leak-scan
mechanism (ADR-023) already treats unverified, heuristically-extracted reasoning-shaped text as
inherently less trustworthy than a genuine structured trace. Defaulting empty-stamp to excluded is
both the RFC-literal reading and the conservative one; named explicitly rather than left as an
implicit consequence a future reader might assume was accidental.

## 5. What this ADR does not claim

- **OpenAI never produces genuine `Reasoning` content today** — its `producer_chat_client_id()`
  accessor exists purely for the CONSUMING side of this mechanism; there is no Phase-2-style "OpenAI
  reasoning encoding" this ADR adds.
- **`text_derived` leak-scan Reasoning items are always excluded once any filtering is active**,
  even when replayed back to the SAME OpenAI backend that produced the underlying text — a
  deliberate, conservative consequence of never stamping a producer id on them (§4), not an
  oversight.
- **`ModelCallGateway`-composed sessions get no filtering** — `HasProducerChatClientId` detection is
  against `ChatClientT` as a whole; a gateway composition has no single real backend identity of its
  own to report, so filtering silently (and correctly, per the concept's own design) does not apply
  there. Extending this to gateway-routed sessions is real, separate follow-up work.
- **`write_seq`-style prediction races are not a concern here** (unlike ADR-047's memory-ranking
  fix) — this mechanism has no equivalent write-ordering hazard; stamping happens synchronously
  within each backend's own response-parsing call.

## 6. Evidence

`tests/test_rt_agent_session_reasoning_provenance.cpp` (new): T1 proves a cross-provider Reasoning-
only message is excluded entirely and a real `policy_decision` event names the excluded message and
its producer; T2 (positive control) proves a same-provider item survives untouched; T3 proves mixed-
content filtering strips only the offending item; T4 proves the "no identity, no filtering" default
degrade for a `ChatClientT` without the optional accessor.

`tests/test_anthropic_chat_client_translation.cpp` (extended, E6 block): `parse_message_response()`
stamps a given producer id onto both `thinking` and `redacted_thinking` blocks, and stamps nothing
(empty, matching every pre-existing call site) when none is given; the STREAMING path
(`parse_streaming_response_into_updates`/`StreamingUpdateAccumulator::release()`) stamps identically,
proven against a real SSE thinking-block sequence, not merely asserted symmetric with the one-shot
path.

Full suite: green (`ctest`, this pass), zero regressions.
