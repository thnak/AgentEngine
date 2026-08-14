# ADR-044 — `AgentMetadata`/`Workflow` gain real `description`/`version` fields

**Status:** Proposed (2026-08-14). Designed, implemented, and proven (real code + extended tests,
full suite green); awaiting the project owner's explicit "Judged" sign-off per this project's
governance (`decisions/README.md`; `OpenQuestions.md` OQ-11).

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #2 (the finding this ADR
closes). `002-Agent-Model-and-Authoring.md` §1/§7 (the normative source of this requirement — this is
not new surface, an implementation gap against an existing spec). `015-Declarative-Agent-Format.md`
§2/§3 (whose own worked examples already carry `metadata.version`/`metadata.description` — the
declarative surface was ahead of the compiled C++ struct).

## 1. The question

**Stated so it has a wrong answer:** do `AgentMetadata` and `Workflow` — the compiled-metadata types
every authoring surface (native C++, declarative YAML, A2A Agent Cards) is supposed to agree on —
actually carry the identity fields 002 §1 has specified since the RFC was written?

**Before this fix: no.** `AgentMetadata` (`include/agentengine/core/agent_registry.hpp`) had
`agent_name`/`agent_instructions` but no `description`/`version`. `Workflow`
(`include/agentengine/workflow/graph.hpp`) had `id` but no `description`/`version` either. Three
independent compilers hit this identical gap independently (the A2A Agent Card generator, the
Agent-document YAML compiler, the Workflow-document YAML compiler) — each recorded it in its own
top-of-file comment rather than working around it, which is itself the signal that `AgentMetadata`
(002-owned) genuinely needed these fields, not something any one of those three files' own authority
extended to fixing.

## 2. What re-grounding against current code found

- **This is not new surface — 002 §1 has always specified it.** §1: an agent carries *"identity —
  stable `agent_id`, name, description, version."* §7: *"An agent's identity for interop is
  `{agent_id, version}`... A2A Agent Cards (012) and MCP tool listings (011) are generated from this
  same metadata."* `AgentMetadata` simply never grew the fields the RFC always named.
- **Four real call sites, not the audit's cited five.** `register_agent<A>()` (the C++ path),
  `compile_agent_document()` (015 §2's YAML compiler), `compile_workflow_document()` (015 §3's YAML
  compiler), `to_agent_card()` (012's Agent Card generator). No fifth site exists — MCP's
  `handle_tools_list()` operates on per-tool `ToolDescriptor`, not agent-level metadata, so it was
  never a wiring point for this gap.
- **The `Tool<...>` "naming collision" concern is real but narrower than the audit's framing.**
  `Tool<Derived, Policies...>` requires a CRTP `static description` (tool-schema text shown to a
  model); `AgentMetadata`/`Workflow` are plain structs, not CRTP bases parameterized the same way, so
  no actual C++ symbol/ODR collision is possible. The real, narrower risk: `Agent<Derived,...>` now
  also accepts an optional `static description` with a *different* meaning (agent identity text for a
  card/listing) — same member name, different semantics, on two unrelated but easily-confused CRTP
  conventions an author skimming both could reasonably conflate. Named explicitly in code (§3) rather
  than left for someone to discover the hard way.
- **I6's proof machinery needed no new infrastructure.** `tests/test_agent_yaml_compiler.cpp`'s F-2 is
  a field-by-field struct comparison between a YAML-compiled and a C++-compiled equivalent agent —
  extending it to cover two more fields was mechanical, not a new proof shape. The audit's own caveat
  ("proven only for the maintained corpus, not structurally") remains accurate and is not something
  this ADR changes — I6 is still proven by example, not exhaustively.

## 3. The design

- **`AgentMetadata`** gains `std::string agent_description` (defaults empty, matching `agent_name`'s
  own "not declared" convention) and `std::optional<std::string> agent_version` (optional, not a
  defaulted empty string, because "no version declared" and "declared as the empty string" are
  genuinely different states a consumer needs to tell apart).
- **`Workflow`** gains `std::string description`/`std::optional<std::string> version` (bare names,
  matching `id`'s own unprefixed convention).
- **`register_agent<A>()`** populates them via two new, `requires`-detected concepts,
  `has_agent_description<A>`/`has_agent_version<A>` — **optional** statics, unlike the required
  `A::name`/`A::instructions` (used directly, with no detection, so a type missing either already
  fails to compile). Making `description`/`version` required as well would have broken every existing
  `Agent<Derived,...>` conformer that predates this fix; detected-optional statics extend the identity
  002 §1 always specified without an invasive, breaking migration across the whole tree.
- **`WorkflowBuilder`** gains matching `.description(...)`/`.version(...)` chainable setters, for
  native-authoring parity with the declarative form.
- **`compile_agent_document()`/`compile_workflow_document()`** now actually read
  `metadata.description`/`metadata.version` (previously: read commented as future work, never
  implemented) into the new fields.
- **`to_agent_card()`** now falls back to `meta.agent_description`/`meta.agent_version` when the
  caller's `AgentCardIdentity.description`/`.version` is left at its default-empty — `identity`'s own
  fields still win when a caller explicitly supplies them (an Agent Card MAY legitimately want
  different framing than the raw agent metadata), so no existing caller's behavior changes.

## 4. What was checked before landing, not assumed safe

**Field-insertion-order risk, checked directly.** Both new fields were inserted in the *middle* of
their structs (`agent_description`/`agent_version` between `agent_instructions` and `chat_client_id`;
`description`/`version` between `id` and `executors`) — the exact shape `003 §6`'s own `Usage` struct
amendment comment warns about: a positional aggregate-initializer (`AgentMetadata{a, b, c, ...}`)
would have silently shifted every field after the insertion point onto the wrong member. Grepped the
whole tree for `AgentMetadata{` (zero hits — every real construction site default-constructs then
assigns named fields) and for genuine positional `Workflow{...}` construction (zero hits — every real
site uses `RunWorkflow{...}`/`ResumeWorkflow{...}`, unrelated types, or the fluent `WorkflowBuilder`).
Confirmed safe before, not after, something broke.

## 5. What this ADR does not claim

- **Does not close the A2A Agent Card's modality-declaration gap** — `default_input_modes`/
  `default_output_modes` still have no home in `AgentMetadata`; that part of the original A2A gap
  stays open, named in `agent_card.hpp`'s own comment, not silently fixed by this pass.
- **Does not make I6 a structural proof** — still one maintained example pair, extended, not an
  exhaustive equivalence check.
- **Does not resolve the `Agent<Derived,...>`/`Tool<Derived,...>` `description`-member-name overlap**
  beyond naming it — both conventions keep the identical member name with different meanings; a
  future 027 (Vocabulary and Naming) pass could disambiguate this for real, not attempted here.

## 6. Evidence

- `tests/test_agent_yaml_compiler.cpp` (F-1/F-2, extended): the 015 §2 worked example's
  `metadata.description`/`metadata.version` compile into `AgentMetadata` correctly; the equivalent
  hand-written `ResearcherAgent` (now declaring optional `description`/`version` statics) produces an
  identical value via `register_agent<A>()` — I6 holds for the extended field set.
- `tests/test_workflow_yaml_compiler.cpp` (W-2, extended): the same for `Workflow`'s
  `description`/`version`.
- Full suite: green (`ctest`, this pass), zero regressions.
