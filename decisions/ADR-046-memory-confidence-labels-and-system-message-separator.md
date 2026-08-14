# ADR-046 — Memory confidence labels, and a real separator between concatenated system texts

**Status:** Judged (2026-08-14, project owner sign-off). Designed, self-red-teamed, implemented, and
proven (real code + extended tests, full suite green).

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #17 (the finding this
ADR closes: "`MemoryProvider` renders `ModelInferred` and `UserStated` items identically"). `029-
Memory-System.md` §3 ("`MemorySource`... a trust signal, not decoration") and §6 (retrieved memory
is tainted external content). `protocol/anthropic/chat_client.hpp` (the system-message concatenation
site the audit's own recommended approach separately flagged as needing a fix, since a bare
confidence-label prefix would otherwise bleed into whatever text follows it once concatenated).

## 1. The question

**Stated so it has a wrong answer:** once a `MemoryItem` is retrieved and injected into a turn, can
the model (or a human reading the transcript) tell a `UserStated` fact from a `ModelInferred` guess?

**Before this fix: no.** `MemoryProvider::memory_item_to_message()` (`core/memory_provider.hpp`)
rendered every item as bare `item.content`, with `ContentItem::tainted=true` and
`origin=content_origin::external` set identically regardless of `MemoryOrigin::source`. Those two
fields are real, load-bearing enforcement (proven by `test_memory_no_authority_laundering.cpp`) —
but they are structural metadata, not visible text; nothing in the assembled system prompt itself
ever distinguished a confident user assertion from the summarizer's own best guess at what the user
meant. 029 §3 calls `MemorySource` "a trust signal, not decoration," but the rendering layer treated
it as exactly that: decoration, dropped before it reached the model.

## 2. What re-grounding against current code found

- **The audit's own recommended approach named a real, adjacent bug**: `protocol/anthropic/
  chat_client.hpp`'s `split_system_messages()` concatenates every `role::system` message's text with
  **zero separator** — confirmed directly in the code (`out.system_text += t->text;`, no boundary of
  any kind) and in that file's own pre-existing test, which only "worked" because its own fixture
  data happened to supply a leading space on the second fragment (`test_anthropic_chat_client_
  translation.cpp`'s E1-R1, unchanged in shape, now updated for the new separator). Adding a label
  prefix to memory items without fixing this first would have made the bug WORSE, not better: a
  `ModelInferred` item's label and a following item's own text (or the agent's own instructions,
  ADR-042) would run together with no boundary between them.
- **The label-forgery surface is real, not hypothetical.** `MemoryProvider::on_turn_end()` derives
  `ModelInferred` content from a declared `ChatClient`'s own response (`029` §4) — a summarizer whose
  output is itself model-generated text, reachable by the same prompt-injection surface every other
  model-derived content in this project already has to defend against (I3). A `ModelInferred` item's
  content embedding literal text shaped like this ADR's own label would, if not neutralized, let a
  low-trust item impersonate a high-trust one once its text sits next to (or, given the previous
  bug, directly touching) a real label in the same concatenated system prompt. This is the SAME
  gap-audit finding's own second half — not a separate discovery, the audit explicitly named this
  risk ("a new label-forgery surface the fix itself introduces") as something the fix had to be
  checked against, not merely hoped to avoid.

## 3. The design

Two independent, composable fixes:

**(a) `split_system_messages()` now joins fragments with a real separator (`"\n\n"`)**, inserted only
*between* two non-empty fragments — never as a leading or trailing pad, so a single system message's
own text is still emitted byte-for-byte unchanged (`protocol/anthropic/chat_client.hpp`). This closes
the concatenation-bleed bug for every system-role source in this codebase, not only memory — it also
benefits ADR-042's injected-instructions message and any future contributor, at zero cost to anyone
already supplying their own boundary whitespace (that whitespace simply becomes redundant, not wrong).

**(b) `MemoryProvider` renders a confidence label ahead of every item's content**, in both places an
item reaches an agent as text: `on_context()`'s default injection AND the contributed `recall(query)`
tool's reply (`memory_detail::memory_item_to_labeled_text()`, shared by both call sites — labeling
only one of the two paths would leave the other exactly as indistinguishable as before, gap 17's own
scope). The label is derived **only** from the trusted, structured `MemoryOrigin::source` field —
never from `item.content` itself:

```
user_stated    -> "⟦memory:user-stated, high confidence⟧"
model_inferred -> "⟦memory:model-inferred, unverified⟧"
tool_derived   -> "⟦memory:tool-derived⟧"
agent_authored -> "⟦memory:agent-authored⟧"
```

The marker uses non-ASCII bracket glyphs (U+27E6/U+27E7), deliberately not plain `[...]` — text an
ordinary model response is unlikely to produce by accident, so an occurrence of the marker inside
retrieved content is a distinguishable, deliberate-looking event rather than something plausible
phrasing could stumble into.

**Forgery neutralization**: before a label is prepended, `item.content` passes through
`neutralize_forged_memory_labels()`, which finds every literal occurrence of the marker's *open*
token and breaks it by inserting a zero-width space (U+200B) inside it. The exact, unbroken marker
sequence can therefore only ever appear in the assembled text where this code itself emitted a real
label — never inside retrieved content, however that content was sourced. Neutralization touches only
the marker bytes; the surrounding (still tainted, still untrusted) content is otherwise unchanged.

## 4. Self-red-team findings

**Fixing only `on_context()` would have been an incomplete fix.** The contributed `recall(query)`
tool renders memory items exactly as much as default injection does — an agent that calls `recall()`
would have received unlabeled, indistinguishable text even after this ADR, defeating its own purpose
on a path the RFC itself calls out (029 §5: "on-demand lookups beyond what was pre-injected"). Both
paths now go through the same shared renderer.

**The separator fix had to land first, and separately proven.** A first-pass instinct might have been
to bundle a separator directly into the label-prepending logic (e.g. always prefix every rendered
item with a leading `"\n"`), but that would only fix the boundary immediately before a labeled memory
item, leaving every OTHER system-message boundary in this codebase (agent instructions, any future
contributor) exactly as broken as before. Fixing `split_system_messages()` itself is the general
fix; the label is a separate, memory-specific concern layered on top.

## 5. What this ADR does not claim

- **Does not change `ContentItem::tainted`/`origin`** — 029 §6's actual enforcement mechanism is
  unchanged, still proven end-to-end (hostile and genuine content alike) by `test_memory_no_
  authority_laundering.cpp`. The label is a rendering aid only; it carries no authority and
  participates in no policy/approval decision.
- **Does not attempt to make the label impossible to notice or reason about** — a sufficiently
  capable adversarial model could still describe or discuss the label's own text; neutralization only
  prevents the exact, structural marker sequence from appearing unbroken outside a real label, which
  is the concrete forgery this ADR's own red-team pass named, not a claim of defending against every
  conceivable social-engineering framing of provenance.
- **Does not address gap 21's broader claim** (raw `ContentItem::text` read unchecked at
  `cli_chat.cpp`/`mcp/server.hpp`/`tool_bridge.hpp`) — out of scope, a separate, already-tracked gap.

## 6. Evidence

`tests/test_memory_provider.cpp` (extended, new "Gap-audit finding 17" block, L1/L2): confirms the
rendered label differs by provenance (`UserStated` vs `ModelInferred`), and that a `ModelInferred`
item's content embedding the literal real marker for a `UserStated` label is neutralized — the exact
marker appears exactly once across both items' rendered text (the genuine item's own real label),
never twice. Also extends G4-R6 with a new G4-R6b proving the `recall()` path renders labels too.

`tests/test_memory_retrieval_determinism.cpp` and `tests/test_memory_no_authority_laundering.cpp`:
updated where they depended on the previous bare-content rendering (substring checks in place of
exact-equality where labels now prefix content); their own actual claims (determinism, no-authority-
laundering) are unaffected and still pass.

`tests/test_anthropic_chat_client_translation.cpp`: E1-R1 updated for the new separator; new E1-R1b
proves the separator prevents a concrete semantic corruption a bare concatenation would otherwise
produce (`"...prefers dark"` + `"mode is not..."` reading as one word, `"...darkmode..."`).

Full suite: green (`ctest`, this pass), zero regressions.
