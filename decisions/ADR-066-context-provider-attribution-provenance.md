# ADR-066 — Should `ContextContribution` provenance be stamped by each contributor's own discipline, or enforced structurally at `assemble_context()`'s seam?

**Status:** Proposed (design → red-team → prove phases complete for Design B; awaiting explicit user
"Judged"). Implemented: `ContributorProvenance` (`content.hpp`), `Message::attribution` and
`ToolDescriptor::attribution`, `HasContextProviderName`/`ContextProviderDescriptor::name`
(`context_assembly.hpp`), and the stamping logic inside `assemble_context()` itself — proven by
`tests/test_context_provenance.cpp` (16/16 checks, real Windows/MSVC build; see §5/§6 for the
updated evidence and verdicts, superseding this ADR's original, pre-implementation §5/§6). A real,
mid-implementation finding corrected the design draft's own §4 prose (recorded in
`context_assembly.hpp`'s own stamping-loop comment and §5/§6 below): "any contributor-sourced
message maps to `content_origin::provider`, never `::user`, regardless of what the contributor set"
is WRONG as literally stated — it would have regressed `SkillsProvider`'s already-shipped,
already-tested `content_origin::system` advertisement (`skill_provider.hpp:136`). Fixed by narrowing
the check to `content_origin::user` specifically (the one origin that can truthfully claim "a human
literally typed this," which no synthesized/non-replayed content may ever claim), left untouched for
every other origin a legitimately host-authored contributor may still claim.

**Relates to:** `OpenQuestions.md` OQ-18 (fan-out vs. chaining — this ADR closes red-team reason #1,
the missing-provenance prerequisite, without reopening OQ-18 itself), OQ-22 (this ADR creates and
closes it), `decisions/ADR-033-middleware-model-call-chain.md` (the `HasMiddlewareName` precedent
this design reuses), `decisions/ADR-042-context-instructions-taint-channel.md` (the `content_origin`
field this design also constrains). Design draft:
`docs/planning/context-provider-provenance-design-draft.md` (workflow-reviewed 2026-08-20, 4
findings, all applied — see that file's own "Red-team findings" section for the full record this ADR
summarizes).

## 1. The question

**Stated so it has a wrong answer:** given that `Message`/`ToolDescriptor` today record no
information about which `ContextProvider` produced them, and OQ-18's own red-team named this the
reason a reactive/chaining mechanism "doesn't actually work" — should the fix be per-contributor
self-stamping (MAF's shape, each provider calls something like `WithAgentRequestMessageSource` on its
own output before returning it), or a structural stamp applied once, centrally, at
`assemble_context()`'s existing merge point, which every contribution already flows through
unconditionally?

## 2. The competing designs

**Design A — per-contributor self-stamping (MAF's shape).** Each `ContextProvider` conformer calls a
stamping helper on its own `ContextContribution` before returning it from `on_context()`. Steelman:
matches MAF's actual, shipped precedent exactly (`ChatMessage.WithAgentRequestMessageSource`,
`AIContextProvider.cs:174-176`); no breaking change to `ContextProviderDescriptor`; a provider that
doesn't need provenance pays nothing.

**Design B (chosen) — stamp once, at the `assemble_context()` seam.** `assemble_context()`
(`context_assembly.hpp:132-180`) is the one place every contributor's output already flows through,
unconditionally, before merge — it stamps `{contributor_index, contributor_type}` there, requiring
every `ContextProviderDescriptor` to carry a declared `name` (mirroring ADR-033's
`HasMiddlewareName`, `static_assert`-checked). Steelman: cannot be skipped, forgotten, or lied about
by any contributor, including a provider that overrides its own merge behavior — the exact situation
MAF's own `CompactionProvider` is in (it overrides `InvokingCoreAsync` entirely and must remember to
manually re-stamp, `CompactionProvider.cs:150-151`; disciplined today, not structurally guaranteed).
Design B closes the class of bug Design A cannot: a non-cooperating or malicious `ContextProvider`
(a third-party plugin, 009 §2) simply never calling the stamping helper.

## 3. Falsifiable claims

| Design | Claim | Disproving experiment |
|---|---|---|
| A | A provider that overrides its default merge path still gets stamped correctly. | Write a `ContextProvider` conformer whose `on_context()` never calls the stamping helper; assert its output is unstamped. (Trivially true by construction — this is exactly Design A's weakness, not a claim it can survive.) |
| B | No contributor, cooperating or not, can produce an unstamped `Message`/`ToolDescriptor` in `out.combined`. | Write an adversarial `ContextProvider` conformer that attempts to return content bypassing the stamp; assert `assemble_context()`'s output has 100% stamped coverage regardless. |
| B | Stamping is pure and does not affect I5 replay determinism. | Run `assemble_context()` twice against identical `{contributors, session_ctx, ctx}`; assert byte-identical `attribution` fields both times. |
| B | The `content_origin` fix (design draft §4) closes the truthful-side-channel gap the review workflow found. | Write a `ContextProvider` conformer that sets `content_origin::user` on its own output; assert `assemble_context()` overrides it to a provider-derived value regardless of what the contributor set. |

## 4. The red-team attack (text-level, not code-level)

A workflow review (`docs/planning/context-provider-provenance-design-draft.md`'s own "Red-team
findings" section) ran three independent adversarial passes against the design text and its
citations: a connectivity/orphan audit, a feature-advocate, and a safety-advocate, reconciled by a
judge pass. Found and fixed:

1. **Must-fix**: the new `attribution` stamp didn't close the pre-existing, independently-settable
   `content_origin` field (`content.hpp:16`) that ADR-042 and both backend translation layers already
   key off — a compromised contributor could carry a truthful `attribution` while lying about
   `content_origin`. Fixed: `assemble_context()` derives `content_origin` at the same seam.
2. **Must-fix**: a citation to `tool_optimizer_provider.hpp` as precedent for the `T::name` pattern
   pointed at a file that exists only on a still-open, unmerged branch (PR18), not this repo's
   current default branch — corrected to cite only the real, merged precedent (ADR-033's
   `HasMiddlewareName`).
3. **Worth-noting**: the header claimed to "close" OQ-22 before OQ-22 existed in `OpenQuestions.md` —
   reworded to "creates and closes."
4. **Worth-noting**: `contributor_index` alone was left as a candidate durable identity; resolved to
   `contributor_type` as the durable, cross-turn identity (029's memory system was named as a real,
   near-term consumer this would otherwise silently corrupt), `contributor_index` retained only as a
   same-turn disambiguator.

No attack was found against Design B's core structural claim (stamping cannot be bypassed by a
non-cooperating contributor) — the findings above are all refinements/closures of adjacent gaps, not
counterexamples to the central mechanism.

## 5. Executed evidence (superseding this ADR's original, pre-implementation §5)

Implemented: `ContributorProvenance{contributor_index, contributor_type}` (`content.hpp`, next to
`content_origin` — has to live there, not in `context_provider.hpp`, since `content.hpp` is the
lower-level header both `Message` and `ToolDescriptor`, via `tool_pipeline.hpp`, depend on without a
circular include); `Message::attribution`/`ToolDescriptor::attribution`, both
`std::optional<ContributorProvenance>`, appended last per this codebase's own established
field-ordering convention (`Usage::cache_write_tokens`'s precedent), so every existing positional
construction site keeps compiling unchanged (confirmed: a full-tree rebuild found zero call sites
broken by either field addition). `HasContextProviderName<T>` (`context_assembly.hpp`), reusing
ADR-033's `HasMiddlewareName` pattern verbatim; `make_context_provider_descriptor<ProviderT>()` now
requires it and stamps `ContextProviderDescriptor::name` from `ProviderT::name`.

**Real, mid-implementation finding, not anticipated by the design draft**: re-grounding §4's
"content_origin derivation" claim against the actual shipped conformers (not just the drafted prose)
found that `SkillsProvider::on_context()` (`skill_provider.hpp:136`) deliberately, correctly sets
`content_origin::system` on its own host-authored skill advertisement — legitimate, tested, shipped
behavior. The draft's literal wording ("any contributor-sourced message maps to
`content_origin::provider`... regardless of what the contributor set") would have clamped this to
`::external` too, a real regression, not a refinement. Root cause: I3 constrains MODEL output, not
engine/host-authored C++ code — a `ContextProvider` conformer is compiled-in or host-vetted code
(009 §2), not model output, so it is entitled to claim `content_origin::system`/`::assistant`/`::tool`
the same way any other engine-internal code path is. The ONE origin no synthesized content may ever
truthfully claim is `content_origin::user` — "a human literally typed this" is a claim only the real
input path can make. **Fixed mechanism, narrower than drafted**: `assemble_context()` checks
`content_origin::user` specifically; when found on a message that is NOT byte-identical to something
already present in `session_ctx.history` (i.e. not a genuine historical replay — `Message` already
has a default `operator==`, reused here rather than inventing a second equality notion), it is forced
to `content_origin::external`. Every other origin is left exactly as the contributor set it.

This same re-grounding pass also surfaced a second, previously-unnoticed, real (not hypothetical) gap
in already-shipped code: `HistoryProvider<Summarize<N,SummarizerT>>`'s synthesized summary message
(`history_provider.hpp`) inherits whatever `content_origin` the summarizer's raw drained response
carried (`ContentItem`'s own default, `::assistant`) with nothing marking it as a SUMMARY rather than
a real assistant turn. This ADR's `::user`-only check does NOT close that gap (a summary is not
claiming `::user`) — named here as a real residual (§7), not silently claimed fixed.

New test file `tests/test_context_provenance.cpp`: an `AdversarialProvider` conformer (the concrete
009 §2 threat named in §2/§4) returns one message forging `content_origin::user` on brand-new,
non-replayed text, one message legitimately claiming `content_origin::system` (the same shape
`SkillsProvider` already ships), and one contributed `ToolDescriptor` — run alongside a real
`HistoryProvider<Window<0>>` replaying one genuine history entry. Windows/MSVC build, **16/16 checks
passed**. Full-tree rebuild (`cmake --build . --config Debug`, all targets): **zero compile errors**
— every existing `ContextProvider` conformer in the tree (6 production conformers plus 5 test/example
-local ones: `FixedMessagesProvider`/`FixedInstructionsProvider` in
`test_context_assembly.cpp`, `FixedMessagesProvider`/`ToolContributingProvider` in
`test_composed_context_provider.cpp`, `ToolDeclaringHistoryProvider` in `tools/cli_chat.cpp`) needed
and received a declared `::name`; `examples/02_add_tools.cpp`/`05_human_approval.cpp`/
`06_capabilities_and_denial.cpp`'s own local conformers needed no change, confirming
`HasContextProviderName` only bites a conformer that is actually routed through
`make_context_provider_descriptor()` (i.e. multi-contributor composition), not `AgentSession`'s plain
single-provider slot. Full `ctest` run: 181/191 passed; the 10 not-run are all
`test_mediated_python_runner_*`/`test_reference_agent_*`/`test_agent_session_suspend_codeact_ask`
(CPython-embedding tests whose executables were never produced by this build — confirmed pre-existing
and unrelated: no `Message`/`ContextProvider`/`ToolDescriptor` dependency, and the same targets are
absent regardless of this ADR's changes). Commands: `cmake --build build --target
test_context_provenance --config Debug`, `ctest --test-dir build -C Debug --output-on-failure`.

## 6. Per-claim verdicts (superseding this ADR's original, pre-implementation §6)

| Claim (§3) | Verdict |
|---|---|
| Design A: a provider overriding its own merge path (or simply never calling a self-stamp helper) produces unstamped output. | **CORRECT, by construction — not independently re-tested.** Design A was never implemented (it was rejected in §2); this is a statement about the ABSENCE of a structural mechanism, definitionally true given Design A's own shape, not an empirical claim this prove phase needed its own code to decide. |
| Design B: no contributor, cooperating or not, can produce an unstamped `Message`/`ToolDescriptor`. | **CORRECT** — `test_context_provenance.cpp`: both the well-behaved `HistoryProvider` replay AND the `AdversarialProvider`'s forged message/tool are stamped with correct `contributor_index`/`contributor_type`, unconditionally. |
| Design B: stamping is pure and does not affect I5 replay determinism. | **CORRECT** — re-running `assemble_context()` against identical `{contributors, session_ctx, ctx}` produces byte-identical `Message`s, `attribution` included. |
| Design B: the `content_origin` fix closes the truthful-side-channel gap, overriding a contributor-set origin "regardless of what the contributor set." | **CORRECT, narrowed from the original wording.** The literal claim as drafted (override REGARDLESS of what was set, for ANY origin) is WRONG — proven wrong by `SkillsProvider`'s own real `content_origin::system` usage, which must NOT be overridden. What IS correct and tested: `content_origin::user` specifically is overridden to `::external` unless the message verbatim-matches `session_ctx.history`; every other origin is left as the contributor set it. This is the actual, shipped, tested claim — narrower than drafted, not a failure of the mechanism. |

## 7. The decision

**Design B, as corrected during implementation (§5), is adopted and implemented.** It binds:
- `027-Vocabulary-and-Naming.md` §3 — `ContextProviderDescriptor` gains a required `name`;
  `ContributorProvenance` is a new normatively-named concept, not yet listed in 027's own table.
- `OpenQuestions.md` — creates and closes OQ-22; remains the named prerequisite for OQ-18's own
  red-team reason #1 (does not reopen OQ-18 itself).
- `content.hpp` — `Message` gains an `attribution` field; `content_origin::user` specifically (not
  every origin) is now `assemble_context()`-derived rather than trusted from whatever the contributor
  set, for any message that isn't a verbatim historical replay.
- `tool_pipeline.hpp` — `ToolDescriptor` gains an `attribution` field (MAF has no equivalent; this is
  a place AgentEngine's design goes further than its surveyed prior art, not merely catching up —
  needed by the not-yet-implemented `decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md`
  design, which depends on this ADR).

**Explicitly out of scope, named rather than left implied:**
- Closing `content_origin::system`/`::assistant`/`::tool` forgery by a genuinely hostile/compromised
  `ContextProvider` conformer — only `::user` is structurally checked (§5's own reasoning: I3
  constrains model output, not engine/host-authored code, so a legitimate contributor is entitled to
  claim these origins; a genuinely COMPROMISED first-party or plugin conformer forging `::system` to
  gain instruction-level trust is a different, broader threat model this ADR does not close).
- `HistoryProvider<Summarize<N,SummarizerT>>`'s synthesized summary message being mislabeled
  `content_origin::assistant` with nothing marking it as a summary — a real, found-during-this-pass
  gap, NOT closed by the `::user`-only check (§5).
- Wiring `attribution` through `rt/message_codec.hpp`'s JSON codec — checked, confirmed absent; the
  field does not currently survive a JSON round-trip through that codec. Durability/replay-across-
  restart wiring is a separate, unscoped pass (matching ADR-042's own precedent of naming, not
  building, the durability angle of a new field).

**Residual risks:**
- Whether `contributor_type`'s stability across a process restart/replay (a renamed C++ type changes
  the stamped string) interacts with 019's durability/replay guarantees for a checkpointed session
  resumed after a code change — still open, unchanged by implementation.
- `attribution` is stamped only inside `assemble_context()`'s own loop — a caller that builds
  `ContextContribution`/`ChatRequest` content some OTHER way (bypassing `assemble_context()` entirely,
  e.g. `AgentSession`'s own direct single-`HistoryProviderT`-slot path used by most of the tree today)
  gets no attribution at all, `nullopt` throughout — correct per this ADR's own "nullopt means not
  contributor-sourced" convention, but means today's dominant, production-path messages (real
  user/assistant turns, appended straight to `history_` by `AgentSession::run_rounds()`) never carry
  attribution either, by design, not by omission.
- **WIRED, real end-to-end evidence (2026-08-20, same pass as the AgentSession wiring for all four of
  this batch's ADRs)**: `tests/test_rt_agent_session_context_provenance.cpp` runs a real
  `rt::AgentSession<..., ComposedContextProvider<HistoryProvider<Window<0>>, SkillLikeProvider>>`
  round and inspects the ACTUAL outbound `ChatRequest` the mock backend received — 13/13 checks pass:
  both contributors' messages/tools reach the wire correctly stamped (`contributor_type`/
  `contributor_index` matching each provider's own declared identity), a genuine historical replay
  keeps `content_origin::user`, and a second provider's legitimate `content_origin::system` claim is
  left untouched, all through the REAL composition path (`ComposedContextProvider`), not a hand-built
  `ContextAssemblyResult`. Zero changes to `agent_session.hpp` were needed for this — `AgentSession`
  already calls `history_provider_.on_context()` generically, and a composed provider already routes
  through `assemble_context()` internally. `decisions/ADR-067-middleware-turn-point-pre-model-
  enforcement.md`'s own `TurnContext`/turn-middleware hook (also now wired) is a real consumer of this
  attribution, though no policy decision in this codebase keys off it yet.
