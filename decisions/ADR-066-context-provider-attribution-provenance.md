# ADR-066 — Should `ContextContribution` provenance be stamped by each contributor's own discipline, or enforced structurally at `assemble_context()`'s seam?

**Status:** Design — question, competing designs, and decision recorded from a design→red-team→judge
pass. **NOT Proposed, NOT Judged**: no implementation exists, no executed evidence, no prove phase
has run (§5). Per `decisions/README.md`'s own requirement ("competing designs implemented in real
C++23, attacked adversarially, compiled under multiple compilers, run under sanitizers, and
measured"), this ADR is not eligible for either status until that work happens. Recorded now so the
decision and its reasoning are not lost, and so a future prove-phase pass has a fixed target rather
than a moving one.

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

## 5. Executed evidence

**None.** No implementation exists. This is the explicit, acknowledged gap this ADR does not paper
over: the falsifiable claims in §3 are stated so they CAN be run against real code, but none have
been. A future prove phase must implement `ContextProviderDescriptor::name`, the `assemble_context()`
stamping change, and the `content_origin` derivation, then run the §3 experiments (plus whatever a
fresh red-team pass against the REAL code finds — a text-level red-team, as run here, is not a
substitute for one run against compiled, tested code, per `decisions/README.md`'s own standard).

## 6. Per-claim verdicts

Every claim in §3: **INCONCLUSIVE — no executed evidence exists to decide it.** This is an honest
verdict, not a placeholder for an assumed CORRECT — per `decisions/README.md` §6, INCONCLUSIVE "must
not be laundered into either" CORRECT or WRONG.

## 7. The decision

**Design B is adopted as the target for the future prove phase.** It binds:
- `027-Vocabulary-and-Naming.md` §3 — `ContextProviderDescriptor` gains a required `name`.
- `OpenQuestions.md` — creates and closes OQ-22; remains the named prerequisite for OQ-18's own
  red-team reason #1 (does not reopen OQ-18 itself).
- `content.hpp` — `Message` gains an `attribution` field; `content_origin`'s derivation moves from
  contributor-set to `assemble_context()`-derived.
- `tool_pipeline.hpp` — `ToolDescriptor` gains an `attribution` field (MAF has no equivalent; this is
  a place AgentEngine's design goes further than its surveyed prior art, not merely catching up —
  needed by the not-yet-implemented `decisions/ADR-067-middleware-turn-point-pre-model-enforcement.md`
  design, which depends on this ADR).

**Residual risks, named not solved:**
- Backward compatibility for 6 existing `ContextProvider` conformers needing a `name` added —
  breaking-change-vs-deprecation-path question, still open.
- Whether `contributor_type`'s stability across a process restart/replay (a renamed C++ type changes
  the stamped string) interacts with 019's durability/replay guarantees for a checkpointed session
  resumed after a code change — still open, not addressed by this ADR.
- The entire §5/§6 evidence gap — this ADR records a decision, not a proof. Implementation and a real
  prove phase are required before this can become Proposed, let alone Judged.
