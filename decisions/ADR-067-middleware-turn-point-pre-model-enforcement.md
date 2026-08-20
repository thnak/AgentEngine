# ADR-067 — Does closing 017 §4's `pre_model` filter gap need a new "terminal `ContextProvider`" concept, or does it reuse 002 §5's already-declared, unwired `Middleware` `turn` point?

**Status:** Design — question, competing designs, and decision recorded from a design→red-team→judge
pass. **NOT Proposed, NOT Judged**: no implementation exists, no executed evidence, no prove phase
has run (§5). Depends on `decisions/ADR-066-context-provider-attribution-provenance.md` (provenance
must exist first — without it, this design reopens exactly `OpenQuestions.md` OQ-18's red-team reason
#1). Independent of `decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md` and
`decisions/ADR-069-content-triggered-model-response-replay.md`.

**Relates to:** `002-Agent-Model-and-Authoring.md` §5 (`Middleware`'s declared, unwired `turn` point),
`017-Safety-and-Content-Governance.md` §4 (`ae:filter`'s declared, unimplemented `pre_model` point —
this ADR closes it), `decisions/ADR-033-middleware-model-call-chain.md` (the composition shape and
the `ToolCall`-provenance-laundering precedent this design's own FATAL finding mirrors and fixes the
same way), `decisions/ADR-042-context-instructions-taint-channel.md` (the taint-merge safety argument
this design's `turn` point is the first thing to read across, addressed in §4 below), `005-Sessions-
State-and-Memory.md` §8 Q3 (re-resolved narrower, §4 below). Design draft: `docs/planning/context-
turn-middleware-and-compaction-design-draft.md` (workflow-reviewed 2026-08-20, 9 findings including 1
FATAL, all applied).

**Explicitly does NOT reopen `OpenQuestions.md` OQ-18.** `assemble_context()`/`ContextProvider`
fan-out is completely unchanged by this design — it serves OQ-18's motivating need (compaction, a
content-policy veto point, tool-surface enforcement) through an orthogonal, already-Judged mechanism.

## 1. The question

**Stated so it has a wrong answer:** this conversation's design work initially proposed inventing a
brand-new "terminal `ContextProvider`" concept to get a single stage that sees the fully-merged
`ContextContribution` before the model call. Is that actually necessary, or does `002-Agent-Model-
and-Authoring.md` §5's `Middleware<Ms...>` — which already declares a `turn` interception point with
almost the exact contract needed ("inspect, annotate, rewrite content, short-circuit... may not widen
capabilities... deny-capable middleware is how content policy plugs in") but has never been wired —
already answer this?

## 2. The competing designs

**Design A — a new "terminal `ContextProvider`" concept.** A second kind of contributor, distinct
from ordinary fan-out `ContextProvider`s, registered separately, that runs after fan-out completes
and sees the merged result. Steelman: purpose-built exactly for `ContextContribution`'s shape; no
need to reconcile with `Middleware`'s separate vocabulary/composition convention.

**Design B (chosen) — wire `Middleware`'s existing `turn` point.** Insert the `turn`-point middleware
chain in `AgentSession::run_rounds()` immediately after `assemble_context()` returns, reusing ADR-
033's already-proven composition shape (position-0-outermost, before-forward/after-backward onion,
`HasMiddlewareName`-style declared-name requirement, structural I2 protection via a context type
that carries no capability-bearing field) verbatim, and reusing 017 §4's verdict vocabulary
(`allow`/`annotate`/`redact`/`require_approval`/`deny`) as the content contract. Steelman: zero new
composition convention; closes the SAME gap named twice across two RFCs (002 §5's `turn` point, 017
§4's `pre_model` point) with one mechanism instead of two; reuses code and reasoning already Judged
rather than re-deriving a parallel one from scratch.

## 3. Falsifiable claims

| Claim | Disproving experiment |
|---|---|
| A `turn` middleware cannot cause a tool's actually-dispatched `invoke` to differ from what fan-out produced (the FATAL finding's fix — `ToolSurfaceView`, §3 of the design draft). | Write a hostile `turn` middleware attempting every exposed `ToolSurfaceView` API to substitute a tool's behavior; assert `invoke_tool()` always dispatches to the ORIGINAL fan-out-produced closure for any tool not newly, attributably added. |
| `redact_subspan()` cannot introduce content that wasn't already present in the original `TaintedText` (the taint-boundary fix). | Property/fuzz test asserting the function's output is always a subsequence of the input's underlying bytes, for arbitrary offset/length arguments including adversarial ones. |
| A `turn`-level `Compactor` never mutates `history[]` (the 005 §8 Q3 re-resolution's core claim). | Run a `Compactor` strategy across many turns against a real `AgentSession`; assert the durable history log is byte-identical to a run with no `Compactor` registered. |
| Chaining multiple `turn` middlewares is deterministic given `{Ms..., assembled}` (I5). | Run the same declared chain twice against identical input; assert byte-identical output both times. |

## 4. The red-team attack (text-level, not code-level), and the FATAL finding

A workflow review ran the same three-lens adversarial process as ADR-066 against this design.
Nine findings, one FATAL:

1. **FATAL, fixed by redesign, not by a stronger check.** The original design proposed
   `enforce_tool_descriptor_provenance`: reject any `ToolDescriptor` whose `name`/`schema`/
   `capability_ceiling`/`approval_mode` weren't byte-identical to what fan-out produced (mirroring
   ADR-033's `enforce_backend_tool_call_provenance`). The review found this checks four fields but
   `ToolDescriptor` actually EXECUTES on a fifth, unchecked one: `invoke` (`tool_pipeline.hpp:52-90`,
   a `std::function` closure). A middleware could copy the four checked fields into a brand-new
   descriptor with its own `invoke`, passing the guard while replacing what actually runs — worse
   than ADR-033's own finding, because `invoke` is invisible to field equality regardless of how many
   fields are added to the check. **Fixed structurally**: `TurnContext` never exposes a mutable
   `ToolDescriptor` for anything fan-out produced; `ToolSurfaceView` offers only `redact()`/
   `reorder()`/`annotate_description()` (none touch `invoke`/`capability_ceiling`/`approval_mode`),
   operating on stable handles into the fan-out-produced vector — there is no code path by which an
   existing tool's `invoke` can differ from what fan-out produced, because it is never reconstructed.
2. **Must-fix**: the taint-boundary candidate originally chosen ("read-to-decide safe, read-to-re-
   emit inherits taint") had no structural enforcement — a middleware could `.unsafe_view()` then
   re-wrap in a fresh `TaintedText{}`, mixing in new content while satisfying the rule in letter only.
   Fixed: the only re-emission API is `redact_subspan()` (subtractive-only, cannot introduce new
   bytes) — the general `TaintedText{}` constructor is not reachable at this boundary at all.
3. **Must-fix**: the design claimed to close both 017 §4's `pre_model` AND `post_model` gaps; the
   mechanism runs exactly once, pre-model only, and structurally cannot do anything post-model-
   shaped. Corrected: this ADR closes `pre_model` only. `post_model` remains real, unimplemented,
   unclosed work — `decisions/ADR-069-content-triggered-model-response-replay.md`'s `after_model`
   hook is a narrower, partial mitigant for that gap, not a closure of it.
4. **Must-fix**: the design's own title/framing implied budget/priority arbitration across sources
   was covered; it is not. `context_assembly.hpp`'s existing rejection of a shared cross-contributor
   budget pool (OQ-18 red-team reason #2) stands unchanged. What this ADR delivers is narrower:
   tool-SURFACE arbitration (name-collision resolution via `ToolDescriptor.attribution`), not
   budget/priority arbitration across message sources.
5. **Worth-noting, resolved as a scope decision**: do not widen to 017 §4's remaining four filter
   points (`input`/`tool_args`/`tool_result`/`output`) in this pass — the one guard built here had a
   FATAL hole before its fix; widening before it's proven sound multiplies exploitable surface.
6-9. Header overclaim on reopening OQ-18 (corrected), warn-before-evict noted as a non-blocking
   follow-up, cross-source dedup named as an explicit non-goal, and a shared detector/verdict
   interface across this design and ADR-068/ADR-069 flagged as a known, deferred architectural gap.

## 5. Executed evidence

**None.** No implementation exists — not `TurnContext`, not `ToolSurfaceView`, not `redact_subspan()`,
not the `Compactor` re-resolution of 005 §8 Q3. The FATAL finding in §4 was found and fixed at the
DESIGN level (the review workflow attacked the proposed API shape, not compiled code) — this is a
real, valuable adversarial pass, but per `decisions/README.md`'s own standard it is not a substitute
for the "implemented in real C++23, attacked adversarially, compiled under multiple compilers, run
under sanitizers, and measured" bar an ADR requires to become Proposed or Judged.

## 6. Per-claim verdicts

Every claim in §3: **INCONCLUSIVE — no executed evidence exists to decide it.**

## 7. The decision

**Design B is adopted as the target for the future prove phase**, scoped narrowly: the `turn`/
`pre_model` point only, tool-surface enforcement via `ToolSurfaceView`, taint-safe compaction via
`redact_subspan()`. It binds:
- `002-Agent-Model-and-Authoring.md` §5 — the `turn` interception point moves from declared-but-
  unwired to specified (pending implementation).
- `017-Safety-and-Content-Governance.md` §4 — closes the `pre_model` filter point only.
- `005-Sessions-State-and-Memory.md` §8 Q3 — re-resolved narrower: a `turn`-level `Compactor` may
  shape what one turn's model call sees, but never rewrites `history[]`; 005 §4's own `history[]`
  compaction is untouched by this ADR.
- Depends on `decisions/ADR-066-context-provider-attribution-provenance.md`.

**Explicitly out of scope, named rather than left implied:**
- `post_model` (017 §4) — remains orphaned; `decisions/ADR-069-content-triggered-model-response-
  replay.md` mitigates but does not close it.
- Budget/priority arbitration across sources — `OpenQuestions.md` OQ-18 red-team reason #2's rejection
  of a shared cross-contributor budget pool stands unchanged.
- Cross-source dedup, warn-before-evict, and a shared detector/verdict interface spanning this ADR
  and ADR-068/ADR-069 — all named, deliberately deferred.

**Residual risks:**
- The entire §5/§6 evidence gap.
- Whether `ToolSurfaceView`'s prevention-based design has a hole a text-level red-team couldn't find
  — only a real, compiled, adversarially-tested implementation can settle this.
- The `Compactor`-output-leaking-into-a-checkpoint question (does a `turn`-level compactor's state,
  if it has any, reopen 005 §4's retention guarantee via a `StateBag`-like mechanism) — still open.
