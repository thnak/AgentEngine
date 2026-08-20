# Design draft: wiring `Middleware<Ms...>`'s `turn` interception point for pre-model compaction and tool-surface enforcement

**Status:** Design draft, workflow-reviewed 2026-08-20 (fixes applied below, see §8; 1 FATAL finding
closed via a structural redesign of §3, not merely a check). No code written. **Serves OQ-18's
motivating need through an orthogonal, already-accepted mechanism — it does NOT reopen
`OpenQuestions.md` OQ-18's fan-out-vs-chaining decision.** `assemble_context()`/`ContextProvider` fan-
out is completely unchanged by this draft; OQ-18 stays resolved exactly as written. Depends on
`context-provider-provenance-design-draft.md` (provenance must exist first; without it this draft
reopens exactly OQ-18 red-team reason #1). Independent of `runtime-secret-quarantine-design-draft.md`
and `content-triggered-response-replay-design-draft.md`. Grounded in
`docs/research/2026-08-20-compaction-provenance-chaining-prior-art.md` throughout.

**Scope, stated precisely up front (was previously overclaimed in this draft's own title and §1):**
this closes 017 §4's `pre_model` filter point and `002 §5`'s `turn` interception point only —
tool-surface (name/schema/approval) enforcement and pre-model-call compaction. It does **not**
deliver `post_model` (see §1), and it does **not** deliver budget/priority arbitration across sources
(see §4a) — both explicitly named as out of scope below rather than left implied by the old, broader
title.

## 1. Reframing: this is not a new "terminal ContextProvider" concept

Earlier framing in this conversation proposed a brand-new "terminal stage" concept bolted onto
`ContextProvider`/`assemble_context()`. Re-reading `002-Agent-Model-and-Authoring.md` §5 found this
is very likely **redundant with something already Judged and partially built**: `Middleware<Ms...>`
declares four interception points — run, turn, model call, tool call — and the `turn` point's own
stated contract already matches almost exactly:

> *"middleware may inspect, annotate, rewrite content, short-circuit with a result, or deny — it may
> not widen capabilities (I2)... Deny-capable middleware is how content policy (017) plugs in
> without the core knowing about content policy."* (002 §5)

The `turn` point is declared-but-unwired (`002-Agent-Model-and-Authoring.md:202-206`, confirmed).
`017-Safety-and-Content-Governance.md` §4's `ae:filter` `pre_model`/`post_model` points are
*also* declared-but-unimplemented (confirmed: zero hits for filter/Filter under `include/agentengine`).
**This draft's `turn` point and 017 §4's `pre_model` point are the same gap**, named twice, and
wiring `Middleware`'s `turn` point (reusing 017 §4's verdict vocabulary — `allow`/`annotate`/
`redact`/`require_approval`/`deny` — as its content contract) closes both in one mechanism.

**`post_model` is a DIFFERENT gap and stays orphaned by this draft** — corrected from an earlier
version of this draft that claimed both were closed together. §2's `TurnContext` runs exactly once,
BEFORE the round's model call, and carries only `assembled` (no `ChatResponse` field) — it cannot do
anything `post_model`-shaped, structurally, regardless of what verdict vocabulary it reuses.
`content-triggered-response-replay-design-draft.md`'s `after_model` hook runs post-model and can act
as a **partial, narrower mitigant** for that gap (its vocabulary is discard-and-retry only, not the
full allow/annotate/redact/require_approval/deny set) — but it does not close it either. 017 §4's
`post_model` point remains real, unimplemented, unclosed work; a future pass owns it explicitly,
it is not silently solved here.

## 2. The mechanism

Insert the `turn`-point middleware chain in `AgentSession::run_rounds()` immediately after
`assemble_context()` returns and before the round's model call is constructed. Reuses ADR-033's
proven composition shape verbatim (`decisions/ADR-033-middleware-model-call-chain.md`), not a new
convention:

```cpp
struct TurnContext {
    ContextAssemblyResult& assembled;   // the merged ContextContribution + drop record, WITH
                                         // provenance now stamped (context-provider-provenance
                                         // draft) on every Message/ToolDescriptor
    // Deliberately NO EffectContext&, NO capability-related type -- same I2-structural argument
    // ADR-033 already made for ModelCallContext (middleware.hpp): a hook cannot widen or even READ
    // a capability because it is never handed one.
};
```

- **Ordering**: identical convention to `ModelCallContext`'s chain — position 0 outermost,
  before-phase forward, after-phase backward, onion-unwind on short-circuit (ADR-033 §4, T6/T8
  already prove this pattern works; reused, not reinvented).
- **A `turn` middleware may**: inspect, annotate, redact, reorder, or deny — mapped directly onto
  017 §4's verdicts. It may **transform `assembled.combined`, never inject a `Message`/
  `ToolDescriptor` with no provenance** — anything it adds must carry ITS OWN attribution
  (`context-provider-provenance-design-draft.md` §3's `name`), same discipline every
  `ContextProvider` already has.
- **A `turn` middleware may NOT**: reach back and change what an earlier fan-out contributor did or
  will do (no behavior override — see this conversation's own earlier rejection of that idea, still
  holds here: crosses I2, and side effects already happened by the time `turn` runs).
- **Chaining is bounded and declared**: `Ms...` at this ONE interception point may be a short,
  explicitly-ordered list (mirroring MAF's own `PipelineCompactionStrategy`, which chains multiple
  compaction strategies over one shared index — real, shipped precedent for needing more than one
  stage here, `docs/research/2026-08-20-...md` §1). This is **narrower** than OQ-18's original
  "should ALL N fan-out contributors chain" question — only contributors that explicitly register as
  `turn` middleware participate in sequencing; `HistoryProvider`/`SkillsProvider`/`MemoryProvider`/
  etc. stay untouched, independent fan-out, exactly as today.

## 3. Closing the ToolCall-laundering parallel risk — structurally, not by detection (FATAL fix)

ADR-033's own FATAL finding: a `before_model`/`after_model` hook rewriting response content could
fabricate/mutate a `ToolCall`, laundering trust through `call_provenance::vendor_structured`
(`decisions/ADR-033-middleware-model-call-chain.md` §3 finding #1). Fixed via
`enforce_backend_tool_call_provenance`, forcing any non-byte-identical `ToolCall` to
`call_provenance::text_derived` before the wrapper returns.

**This draft originally proposed a mirrored field-equality guard** (`enforce_tool_descriptor_
provenance`, checking `name`+`schema`+`capability_ceiling`+`approval_mode` for byte-identity) — the
review workflow found this FATAL: `ToolDescriptor` (`tool_pipeline.hpp:52-90`) actually EXECUTES on
`invoke` (a `std::function` closure, line 64-65), a field the four-field check never touches. A
`turn` middleware could read a legitimate descriptor, copy the four checked fields verbatim into a
brand-new `ToolDescriptor`, and substitute its own `invoke` — passing the guard completely while
replacing what actually runs when the model calls that tool by name. Worse than ADR-033's own
finding, because `invoke` is invisible to field equality no matter how many fields the check covers
— any detection-based fix is one un-checked field away from the same hole recurring.

**Fix: prevent reconstruction, don't detect it.** `TurnContext` never exposes a raw, mutable
`ToolDescriptor` for anything fan-out already produced. Instead:

```cpp
class ToolSurfaceView {
public:
    // Read-only iteration over what fan-out produced this turn -- no accessor returns a mutable
    // ToolDescriptor& or lets a middleware construct a replacement that impersonates an existing
    // entry's identity.
    [[nodiscard]] std::span<ToolDescriptor const> descriptors() const;

    // The ONLY mutations a turn middleware may perform on an EXISTING (fan-out-sourced) tool --
    // neither touches invoke, capability_ceiling, or approval_mode. redact() and reorder() operate
    // on a stable handle (index into the fan-out-produced vector, never a rebuilt object), so
    // "the entry a middleware is looking at" and "the entry that will actually be dispatched to on
    // a tool call" are PROVABLY the same object by construction, not merely by an equality check
    // that could miss a field.
    void redact(std::size_t handle);
    void reorder(std::vector<std::size_t> new_order);
    void annotate_description(std::size_t handle, std::string shortened_text);  // description only

    // Adding a genuinely NEW tool must go through the SAME make_tool_descriptor_with_invoke<T>()
    // path every fan-out ContextProvider already uses (tool_pipeline.hpp) -- it is stamped with
    // the MIDDLEWARE's own provenance (its declared `name`, `context-provider-provenance-design-
    // draft.md` §3), never inherits an existing tool's identity, and is visibly attributable to the
    // middleware that added it, matching §2's existing "no ToolDescriptor with no provenance" rule.
};
```

There is no code path by which an existing tool's `invoke` can differ from what fan-out produced
while still appearing, to `invoke_tool()`, as that same tool — because it IS the same object,
referenced by stable handle, never reconstructed. This is stronger than ADR-033's own fix (which
detects and downgrades trust after the fact); here the exploit has no expression at all, matching
this codebase's own preference for structural guarantees over runtime checks (the same reasoning
`Secret`/`SecretLease` already use: no `std::string` conversion, so it cannot be printed by accident,
rather than a scanner that must remember to catch it — `trust/secret.hpp`).

## 4. Reopening 005 §8 Q3 — compaction's scope boundary

005 §8 Q3 (resolved 2026-08-04) currently states memory-provider/context-provider output is "always
fresh, never compacted" because compaction (005 §4) is scoped to `history[]` specifically, and a
`ContextProvider`'s contribution is "recomputed fresh every turn... there is structurally nothing
there for compaction to act on." **That premise stops being true once a `turn` middleware exists**
— there IS something to act on: the full merged `assembled.combined`, not just `history[]`.

Proposed narrower re-resolution (for the review workflow to attack): a `Compactor<Strategy>` `turn`
middleware may compact the ASSEMBLED VIEW for the current turn's model call, but this is NOT the
same as 005 §4's `history[]` compaction and must not be confused with it:
- 005 §4's `history[]` compaction remains **unchanged** — it still rewrites the durable log itself,
  still requires the Event-sourced-mode retention guarantee, still forbids splitting a pending
  tool-call/result pair.
- A `turn`-level compactor **never rewrites `history[]`** — it only shapes what THIS turn's model
  call sees, the same "transient, recomputed every turn" property `ContextContribution` already has.
  Nothing durable changes; replaying the turn from the same `{contributors, session_ctx, ctx}`
  (I5) reproduces the same compacted view, satisfying determinism without touching the durable-log
  retention guarantee at all.
- Same atomic tool-call/result grouping invariant as 005 §4 (already proven independently in both
  005 and MAF's `CompactionMessageIndex.AppendFromMessages`,
  `docs/research/2026-08-20-...md` §1) — a `turn`-level compactor must not split one either.

**Follow-up refinement, not blocking for the initial mechanism**: a `turn`-level `Compactor` as
specified above can drop content with zero warning — strictly worse than Claude's own context-
editing API, which warns the model before clearing so it can proactively persist important content
first (`docs/research/2026-08-20-...md` §2, Anthropic's "warn-before-evict" pattern, "confirmed
open, not settled" by that survey). No new mechanism is needed to add this later: an extra callback
before a drop commits, inserted into the existing two-phase (before/after) onion shape §2 already
has, is sufficient. Not required for this draft's initial scope.

## 4a. Explicitly out of scope: cross-source dedup and budget/priority arbitration

Two capabilities that motivated this whole design pass are **not delivered by this draft**, named
here so their absence reads as a scope decision, not an oversight:

- **Cross-source dedup** (e.g. MAF's `CompactionProvider` re-stamping specifically to avoid
  re-adding a summary already present in chat history, `docs/research/2026-08-20-...md` §1) —
  appears in none of the four drafts in this batch. Deferred; a `Compactor` strategy COULD implement
  it (it has full visibility into `assembled.combined` with provenance attached), but this draft
  does not specify one that does.
- **Budget/priority arbitration across sources** (e.g. a fresh, highly-relevant `ae:memory` RAG
  result should outrank 3-turn-old history when both compete for remaining budget) — this draft's
  title previously implied this was covered ("...arbitration"); it is not. `context_assembly.hpp`'s
  existing rejection of a shared cross-contributor budget pool (`OpenQuestions.md` OQ-18 red-team
  reason #2) **stands unchanged by this draft**. What §3 delivers is TOOL-SURFACE arbitration
  specifically (which tool wins a name collision, via `ToolDescriptor.attribution`) — a narrower,
  different thing than budget/priority arbitration across message sources, and the two must not be
  conflated. A `Compactor` strategy operating on `assembled.combined` COULD implement priority-based
  trimming across sources as its own internal policy, but that is strategy-author-provided logic,
  not a new cross-contributor budget-pool primitive this draft adds to `context_assembly.hpp` itself
  — the distinction matters because the latter is exactly what OQ-18 already rejected.

## 5. The taint boundary — ADR-042 interaction, resolved with a structural companion

`assemble_context()`'s existing comment (`context_assembly.hpp:148-151`) states merging two
independently-declassified `TaintedText` values is safe specifically BECAUSE "each contributor
already made its own declassification choice at construction" and no contributor ever re-examines
another's. A `turn` middleware reading `assembled.combined.instructions` (already-merged
`TaintedText`) breaks that isolation premise for the first time.

**Resolved: candidate (a) — read-to-decide is safe, read-to-re-emit inherits the original taint —
with a structural companion, not documentation-only discipline.** Candidate (b) (requiring its own
explicit taint-handling capability for every read) was rejected: it makes every compaction/filter use
case need a new grant for a read that only ever DECIDES, not emits, which is a real cost for no
matching benefit when (a) can be made structurally safe instead.

The review workflow found (a) as originally stated was NOT actually safe: `TaintedText`/`Tainted<T>`
has no origin-tracking (ADR-042 §4 names this directly — "not a fix for a careless or bad-faith
wrap"), so a middleware could satisfy rule (a) in letter — call `.unsafe_view()`, then re-wrap in a
fresh `TaintedText{...}` — while actually mixing in new, unattributed content, since nothing checked
the new string was a subset of the old.

**Fix**: `TurnContext` does not expose the general `TaintedText{}` constructor as a way to produce
`assembled.combined.instructions`'s replacement value. The only re-emission API available at this
boundary is subtractive:

```cpp
// The ONLY way a turn middleware may modify assembled.combined.instructions. Removes a byte range
// from the ALREADY-declassified TaintedText and returns the result, still carrying the same
// declassification -- there is no code path here that can introduce content that wasn't already
// present, so "read-to-re-emit inherits the original taint" is enforced by the type signature, not
// by a rule a middleware author has to remember.
[[nodiscard]] TaintedText redact_subspan(TaintedText const& original, std::size_t offset, std::size_t length);
```

A middleware that wants to ADD new instruction-level text (not just redact) must do so the same way
adding a new tool works (§3): as its own attributed contribution, carrying its own provenance, never
as a silent rewrite of `assembled.combined.instructions` in place.

## 6. External grounding this draft leans on

`docs/research/2026-08-20-compaction-provenance-chaining-prior-art.md` §1's confirmed MAF weakness:
**ordering is a documentation convention, never enforced, in either MAF SDK** — Python's own design
ADR admits this as a known wart. This draft's §2 chain is explicitly-ordered and declared at
registration (matching `Ms...` template-argument order, a compile-time list) rather than a runtime
`IEnumerable` — closing the exact gap MAF left open, for free, by reusing this project's own
existing CRTP-policy idiom instead of MAF's `IEnumerable<AIContextProvider>` shape.

## 7. Open questions, resolutions, and known deferred gaps

- **Resolved: do not widen to 017 §4's other filter points (`input`, `tool_args`, `tool_result`,
  `output`) in this pass.** The one guard this draft builds for `turn`/`pre_model` had a FATAL hole
  before §3's structural fix; `tool_args`/`tool_result` sit directly on live in-round tool execution
  — higher-stakes territory than `turn`/`pre_model`. Widening before the one built mechanism is
  proven sound would have multiplied exploitable surface rather than reduced total design passes.
  Wire the remaining five points as a follow-up pass, matching this project's own "prove the
  mechanism against one real consumer, name the rest" precedent (ADR-028, ADR-033 §2).
- §3's `ToolSurfaceView.annotate_description()` is deliberately narrower than the original field-
  equality guard — it structurally cannot touch trust-relevant fields at all, so the "does the guard
  need to distinguish description-only edits from capability-relevant edits" question from an earlier
  version of this draft is now moot: there is no path to a capability-relevant edit through this API.
- §4's re-resolution of 005 §8 Q3 — is the durable-log/transient-view split actually airtight, or
  does a `turn`-level compactor's output leaking INTO a later checkpoint (e.g. via a `StateBag`-like
  mechanism, MAF's `ProviderSessionState<State>` precedent) reopen the retention guarantee anyway?
  Still open.
- **Known, deliberately deferred architectural gap**: `content-triggered-response-replay-design-
  draft.md`'s trigger and `runtime-secret-quarantine-design-draft.md`'s detector are each described
  as "pluggable" in their own drafts, but no draft — including this one, despite owning the verdict
  vocabulary (`allow`/`annotate`/`redact`/`require_approval`/`deny`) the other two lean on — declares
  a shared, versioned detector/verdict interface that would let one `ae:filter` conformer plug into
  all three interception points without per-combination, engine-authored glue. Deferred to a
  follow-up cross-cutting design once at least two of the three mechanisms are real (same ADR-028
  sequencing precedent as above) — flagged here explicitly so it stays discoverable rather than
  silently missing across three separate documents.

## Red-team findings (workflow review)

Reconciled from three independent reviews (connectivity/orphan audit, feature-advocate,
safety-advocate) run against all four drafts, cross-checked against real code
(`tool_pipeline.hpp`, `context_assembly.hpp`, ADR-042) and the other three drafts in this batch.

| # | Finding | Severity | Fix |
|---|---|---|---|
| 1 | §3's `enforce_tool_descriptor_provenance` checks only `name`+`schema`+`capability_ceiling`+`approval_mode` for byte-identity, but `ToolDescriptor` (`tool_pipeline.hpp:52-90`) actually EXECUTES on `invoke` (a `std::function` closure, line 64-65) — the field the guard's own stated check never touches. A `turn` middleware can read a legitimate tool's descriptor off `assembled.combined.tools`, copy the four checked fields verbatim into a brand-new `ToolDescriptor`, and substitute its own `invoke`, passing the guard completely while replacing what actually runs when the model calls that tool by name. Worse than ADR-033's original finding because `invoke` is invisible to field equality. | FATAL | Provenance stamping (`context-provider-provenance-design-draft.md`) must bind an opaque identity to the `invoke` closure itself at `assemble_context()` time — not just index/type — and this guard must check that identity, not just the four surface fields. |
| 2 | §5 picks candidate (a) — read-to-decide is safe, read-to-re-emit inherits the original taint — as the right direction (matches `assemble_context()`'s own precedent that merging two already-vetted `TaintedText` values is not a new trust decision). But `TaintedText`/`Tainted<T>` has no origin-tracking (ADR-042 §4 names this directly: "not a fix for a careless or bad-faith wrap"). A middleware can satisfy rule (a) in letter — call `.unsafe_view()`, then re-wrap in a fresh `TaintedText{...}` — while actually mixing in new content, since nothing checks the new string is a subset of the old. | Must-fix | Give rule (a) a structural companion, not just documented discipline: restrict turn-middleware re-emission of `assembled.combined.instructions` to a subtractive-only API (e.g. `redact_subspan()`), never the general `TaintedText{}` constructor, at this boundary. |
| 3 | §1 claims wiring the `turn` point closes BOTH 017 §4's `pre_model` and `post_model` gaps in one pass. But §2's mechanism inserts the chain exactly once — "immediately after `assemble_context()` returns and before the round's model call is constructed" — a single pre-call point. `TurnContext` carries only `assembled` (no `ChatResponse`), so it structurally cannot do anything `post_model`-shaped. `content-triggered-response-replay-design-draft.md`'s `after_model` hook does run post-model, but its vocabulary (discard-and-retry) is narrower than 017 §4's full allow/annotate/redact/require_approval/deny set, and neither draft cross-references the other on this. 017 §4's `post_model` point is effectively still orphaned. | Must-fix | Correct the header: this draft closes `pre_model` only. State explicitly that 017 §4's `post_model` point remains orphaned, and cross-reference `content-triggered-response-replay-design-draft.md`'s `after_model` hook as a narrower, partial mitigant for that gap — not a closure of it. |
| 4 | This draft's own title promises "…compaction/filter/**arbitration**," but the body delivers only tool-descriptor-approval enforcement and compaction; budget/priority contention across sources (`context_assembly.hpp`'s existing rejection of a shared cross-contributor budget pool, reaffirmed by `OpenQuestions.md` OQ-18 red-team reason #2) is never revisited or re-confirmed anywhere in this draft. | Must-fix | Either narrow the title to drop "arbitration," or add an explicit statement re-confirming OQ-18 reason #2's budget-pool rejection stands unchanged by this draft — tool-surface arbitration via `ToolDescriptor.attribution` is real and covered here; budget/priority arbitration across sources is not, and must be named as such rather than left implied. |
| 5 | §7 asks whether closing this "ALSO obligate[s] closing 017 §4's other filter points" (`input`/`tool_args`/`tool_result`/`output`) in the same pass — a real question, since this draft already builds the general onion-chain and verdict vocabulary that would generalize to all six points at comparatively low marginal design cost. But finding #1 above shows the ONE guard already built for the `turn`/`pre_model` point has a fatal hole, and `tool_args`/`tool_result` sit directly on live in-round tool execution — higher-stakes territory than `turn`/`pre_model`. Widening now, before that hole is closed, multiplies exploitable surface rather than reducing total design passes. | Worth-noting (scope deferred, not blocking) | Do not widen scope in this pass. Fix finding #1 first (closure-identity binding); wire the remaining five `ae:filter` points as a follow-up pass once the `turn` point's own guard is proven sound — matching this project's own "prove the mechanism against one real consumer, name the rest" precedent (ADR-028, ADR-033 §2). |
| 6 | §1 frames this draft as "Reopens `OpenQuestions.md` OQ-18," but the actual mechanism leaves `assemble_context`/`ContextProvider` fan-out completely unchanged — exactly what OQ-18's resolution required to remain true. §1's own text nearly says this already, but the header doesn't reflect it. | Worth-noting | Reword the header: this draft serves OQ-18's motivating need through an orthogonal, already-accepted mechanism (`Middleware`'s `turn` point), not by reopening OQ-18's fan-out-vs-chaining decision. |
| 7 | §4's re-resolution of 005 §8 Q3 adopts only MAF's silent in-run compaction override; it does not consider Anthropic's "warn-before-evict" shape (`docs/research/2026-08-20-...md` §3, "confirmed open, not settled") even though §4 is exactly where it belongs. A `turn`-level `Compactor` as specified can drop content with zero warning — strictly worse than Claude's own context-editing API. | Worth-noting | Add a pre-evict notification phase to the existing two-phase (before/after) onion shape as a follow-up refinement — no new mechanism needed, just an extra callback before a drop commits. Not blocking for the initial mechanism. |
| 8 | Cross-source dedup (`docs/research/2026-08-20-...md` §1: MAF's `CompactionProvider` re-stamps specifically for dedup) appears in none of the four drafts in this batch and isn't marked out of scope anywhere, despite being one of the concrete capabilities that motivated this whole design pass. | Worth-noting | Add an explicit non-goals line in this draft naming cross-source dedup as deferred, so a future reader doesn't mistake its absence for an oversight. |
| 9 | Response-replay's trigger and secret-quarantine's detector are each described as "pluggable" in their own drafts, but no draft — including this one, despite it owning the verdict vocabulary (`allow`/`annotate`/`redact`/`require_approval`/`deny`) the other two lean on — declares a shared, versioned detector/verdict interface that would let one `ae:filter` conformer plug into all three interception points without per-combination, engine-authored glue. | Worth-noting | Defer to a follow-up cross-cutting design once at least two of the three mechanisms are real (matching this draft's own ADR-028 precedent for sequencing); flag here as a known architectural gap rather than leaving it undiscoverable across three separate documents. |

**Applied**: findings 1 (FATAL — §3 rebuilt around `ToolSurfaceView`, structural prevention instead
of field-equality detection), 2 (§5 rebuilt around `redact_subspan()`), 3 (header/§1 corrected,
`post_model` explicitly named as still orphaned), 4 (title narrowed, §4a added), 6 (header reworded),
7 (warn-before-evict follow-up noted in §4), 8 (§4a non-goals), 9 (§7 known-gap note) are all
incorporated into the body above. Finding 5 (do not widen scope) is a scope decision, applied by
NOT widening §2/§3 to the other five `ae:filter` points — recorded in §7.
