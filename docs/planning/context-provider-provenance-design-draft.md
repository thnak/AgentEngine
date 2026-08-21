# Design draft: per-contributor provenance on `ContextContribution`

**Status:** Design draft, workflow-reviewed 2026-08-20 (fixes applied below, see §8). Creates and
closes `OpenQuestions.md` OQ-22 (OQ-22 does not exist prior to this draft — the highest question
number in that file today is OQ-21; this draft both names and resolves it, not merely resolves a
pre-existing entry). No code written.
Foundational to `context-turn-middleware-and-compaction-design-draft.md` (that draft depends on
this one for compaction-eligibility and cross-source arbitration to work at all — see its §2);
independent of `runtime-secret-quarantine-design-draft.md` and `content-triggered-response-replay-
design-draft.md`. Grounded in `docs/research/2026-08-20-compaction-provenance-chaining-prior-art.md`
§1 (MAF's `AgentRequestMessageSourceAttribution` mechanics) and §2's cross-cutting observation that
MAF's inline stamp is the only *shipped* provenance mechanism found anywhere in that survey.

## 1. The gap

Neither `Message` (`content.hpp`) nor `ToolDescriptor` (`tool_pipeline.hpp:52`) records which
`ContextProvider` contributed it. This is OQ-22, and it is the load-bearing prerequisite named by
OQ-18's own red-team reason #1 for rejecting reactive chaining ("no provenance, so the motivating
example doesn't actually work" — `OpenQuestions.md` OQ-18 §"Resolved by design").

## 2. The mechanism — stamp at the seam, not by the contributor

MAF's own shape (`ChatMessage.WithAgentRequestMessageSource`, `AIContextProvider.cs:174-176`) has
each provider's `InvokingCoreAsync` stamp its OWN output before returning it — which means a
provider that overrides the default merge (exactly what `CompactionProvider` does,
`docs/research/2026-08-20-...md` §1) bypasses stamping unless it remembers to do so manually
(confirmed: `CompactionProvider` DOES re-stamp manually, `CompactionProvider.cs:150-151` — it is
disciplined, but the base class doesn't force it).

**AgentEngine can do this structurally instead of by discipline**: `assemble_context()`
(`context_assembly.hpp:132-180`) is the ONE place every contributor's `ContextContribution` already
flows through, unconditionally, before merge. Stamping there means no contributor — first-party or
plugin — can skip it, lie about it, or need to remember it.

```cpp
struct ContributorProvenance {
    std::size_t contributor_index;   // position in the declared contributors[] list
    std::string contributor_type;    // ContextProviderDescriptor's declared name, new field (§3)
};
```

Attached to every `Message` and `ToolDescriptor` that emerges from a contributor's
`ContextContribution`, inside `assemble_context()`'s existing per-contributor loop
(`context_assembly.hpp:137-177`), before insertion into `out.combined`.

## 3. One new required field: `ContextProviderDescriptor` needs a declared name

`ContextProviderDescriptor` (`context_assembly.hpp:76-92`) has no `name` field today — unlike
`Middleware`'s own requirement (ADR-033 finding #2: "every `Middleware` type with at least one hook
must declare `static constexpr std::string_view name`", checked via `HasMiddlewareName`). Proposed:
the same `static_assert`-checked pattern, reused verbatim rather than inventing a second name
convention — `make_context_provider_descriptor<ProviderT>()` (`context_assembly.hpp:103-111`)
requires `ProviderT::name`, grounded in ADR-033's `HasMiddlewareName` precedent alone. (A second,
`Tool<T,...>`/`T::name` precedent was cited here in an earlier version of this draft, pointing at
`tool_optimizer_provider.hpp` — that file is real but lives only on the still-open, unmerged
`tool-optimizer-provider-issue-15` branch (PR18), not on this repo's current default branch; citing
it as already-shipped precedent overstated what's actually on this branch, so the citation was
dropped rather than left implying merged code. `HasMiddlewareName` alone is sufficient grounding —
it IS merged, real, tested code.) This is a real, minor breaking change to the `ContextProvider`
concept — every existing conformer (`HistoryProvider`, `SkillsProvider`, `MemoryProvider`,
`HistoryAndSkillsProvider`, `ComposedContextProvider`, and PR18's `ToolOptimizerProvider` once/if it
merges) needs a `name` added.

## 4. Where provenance surfaces, and closing the `content_origin` gap

`content.hpp:16` already declares a `content_origin` field that ADR-042's system-message
construction and both real backend translation layers key off today — independently settable,
predating this draft. Adding a truthful `attribution` stamp alongside it without touching
`content_origin` would open a real gap: a compromised contributor's `Message` would carry a fully
truthful `attribution` (index/type genuinely can't be spoofed, §2) while its `content_origin` stays
whatever that contributor set, unconstrained — two fields, only one enforced, and existing consumers
trust the unenforced one.

**Fix, applied here rather than left to a future pass**: `assemble_context()` derives
`content_origin` FROM the same stamping step as `attribution`, at the identical seam, instead of
trusting whatever a contributor already set on the `Message` it returned — a contributor no longer
gets to independently declare its own `content_origin`; `assemble_context()` computes it
deterministically from `contributor_index`/`contributor_type` (e.g. any contributor-sourced message
maps to `content_origin::provider`, never `::user`, regardless of what the contributor's own
`ContextContribution` set). This closes the gap structurally, the same way `attribution` itself is
structural rather than disciplinary (§2) — one seam, two fields, both now engine-derived instead of
one derived and one merely trusted.

- **`Message.attribution`** (optional field, `None` for a message that predates this change or came
  from outside a `ContextProvider` — e.g. genuine user/model turn content, matching MAF's own
  `External` default) — same shape as MAF's `AgentRequestMessageSourceType`/`SourceId` split
  (`AgentRequestMessageSourceType.cs`, `AgentRequestMessageSourceAttribution.cs`), but AgentEngine
  gets the source type for free from `contributor_index` (no need for MAF's
  `External`/`AIContextProvider`/`ChatHistory` enum — a `contributor_index` of "not from any
  contributor" already means external).
- **`ToolDescriptor.attribution`** — new; MAF has NO equivalent (confirmed:
  `AIContextProvider.cs:186-192`'s `mergedTools` is a bare `Concat`, no stamp at all,
  `docs/research/2026-08-20-...md` §1). This is a place AgentEngine goes further than its own prior
  art, not merely catching up — needed for the tool-arbitration and `enforce_*` guard use cases in
  the turn-middleware draft.

## 5. Replay/determinism check (I5)

`assemble_context()`'s own comment already states its purity requirement: "deterministic and
replayable given `{contributors, session_ctx, ctx}`... no wall-clock read, no randomness"
(`context_assembly.hpp:125-131`). Stamping `contributor_index`/`contributor_type` is pure — both
values are already fully determined by `contributors[i]`'s position and static declaration, nothing
new to replay. This does not touch I5's guarantee.

## 6. Non-goals, explicit

- **Not a provenance graph** (contrast the academic ARGUS/evidence-tracing work,
  `docs/research/2026-08-20-...md` §2 — "typed provenance graph... heavier machinery than this
  problem currently needs" per that survey's own §3 conclusion). Inline stamping only, matching the
  one shipped precedent found (MAF).
- **Not capability-gating** — provenance is informational/audit, same status as `ae:filter`'s
  layer (017 §2: "Layers are independent. Detection failing must not enable an effect that
  authority and approval would have blocked.") — a message's attribution is never itself a
  capability check.

## 7. Open questions, and one now resolved

- **Backward compat**: 6 existing `ContextProvider` conformers need a `name` added — is this an
  acceptable breaking change mid-implementation (Milestone 7), or does it need a deprecation path?
  Still open.
- **Does `contributor_type` need to be stable across a process restart/replay** (a renamed C++ type
  would change the stamped string) — does this interact with 019's durability/replay guarantees for
  a checkpointed session resumed after a code change? Still open.
- **Resolved**: `contributor_index` alone is NOT sufficient as the durable, cross-turn identity — an
  operator reordering or adding a `ContextProvider` mid-session (a realistic operational event, not
  an edge case: 029's memory system is a real, near-term consumer that would silently corrupt cross-
  turn attribution if it kept only the index) would silently corrupt anything that persists an
  index-only stamp across turns. **`contributor_type` is the durable identity; `contributor_index` is
  retained only as a same-turn disambiguator** (needed when two contributors of the same declared
  type run in one turn — not otherwise meaningful once the turn ends). Anything that persists
  provenance beyond the turn it was stamped in (e.g. a memory item annotated with who originally
  surfaced it) must key off `contributor_type` alone.

## Red-team findings (workflow review)

Reconciled from three independent reviews (connectivity/orphan audit, feature-advocate,
safety-advocate) run against all four drafts, cross-checked against real code
(`content.hpp`, `context_assembly.hpp`, ADR-042) and `OpenQuestions.md`.

| # | Finding | Severity | Fix |
|---|---|---|---|
| 1 | The new `attribution{contributor_index, contributor_type}` stamp is truthful but doesn't close the pre-existing, independently-settable `content_origin` field (`content.hpp:16`) that ADR-042's system-message construction and both real backend translation layers already key off. A compromised contributor can carry a fully truthful `attribution` stamp (index/type genuinely can't be spoofed) while freely setting `content_origin::user` on the same item — this draft adds a truthful side-channel without closing the one consumers already trust. | Must-fix | `assemble_context()` must also constrain `content_origin` at the same seam (derive/validate it, don't leave it caller-chosen), or this draft must explicitly state that every future consumer keys off `attribution`, never `content_origin` — currently unstated anywhere. |
| 2 | §3's precedent for the `T::name` requirement cites `tool_optimizer_provider.hpp:70,97,124` (`Tool<T,...>` already requiring `T::name`) — this file does not exist anywhere in the repo (confirmed by grep across `include/`, `src/`, `tests/`, and every `.md` file; the only hits are inside this draft and the secret-quarantine draft). The argument survives on its other, real citation (ADR-033's `HasMiddlewareName`), but the fabricated citation must go. | Must-fix | Drop the `tool_optimizer_provider.hpp` citation from §3; ADR-033's `HasMiddlewareName` precedent alone is sufficient grounding for the `static constexpr name` pattern. |
| 3 | The header states this draft "Closes `OpenQuestions.md` OQ-22" — `OpenQuestions.md` has no OQ-22 today; its numbered questions stop at OQ-21 (confirmed: 21 `### OQ-` headers, highest number OQ-21). "Closes" overstates a question that doesn't exist yet as already-resolved. | Worth-noting | Reword to "creates and closes OQ-22" (or similar) rather than citing a question number that isn't in the file yet as already closed. |
| 4 | §7's own Q3 ("index, type, or both?") is left fully open, but 029's memory system is a real, named near-term consumer whose cross-turn identity would be silently corrupted by `contributor_index` alone the moment an operator reorders or adds a `ContextProvider` mid-session — a realistic operational event, not an edge case. | Worth-noting | Commit to `contributor_type` as the durable, cross-turn identity now; keep `contributor_index` only as a same-turn disambiguator, rather than carrying this fully open into implementation. |

**Applied**: all 4 findings addressed above (§3 citation corrected, §4 `content_origin` closure
added, header reworded, §7 Q3 resolved). Nothing deferred from this file's own findings.
