# ADR-067 — Does closing 017 §4's `pre_model` filter gap need a new "terminal `ContextProvider`" concept, or does it reuse 002 §5's already-declared, unwired `Middleware` `turn` point?

**Status:** Proposed (design → red-team → prove phases complete for Design B; awaiting explicit user
"Judged"). Implemented: `TurnContext`/`ToolSurfaceView`/`redact_subspan()`/`Compactor<N>`/
`run_turn_middleware_chain()` (`include/agentengine/core/turn_middleware.hpp`), proven by
`tests/test_turn_middleware.cpp` (22/22 checks, real Windows/MSVC build — see §5/§6 for the updated
evidence and verdicts, superseding this ADR's original, pre-implementation §5/§6). Depends on
`decisions/ADR-066-context-provider-attribution-provenance.md` (Proposed as of this pass — provenance
exists first, as required). Independent of
`decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md` and
`decisions/ADR-069-content-triggered-model-response-replay.md`.

Two real, mid-implementation findings, neither anticipated by the design draft (recorded in
`turn_middleware.hpp`'s own top comment and §5/§6 below): (1) the draft's §2 said this "reuses
ADR-033's proven composition shape verbatim... before-phase forward, after-phase backward" — that
shape exists to sandwich a real inner action (the backend model call) between hooks; the turn/
pre_model point has no analogous inner action to sandwich, so what actually got built is a single
forward pass, first-deny-short-circuits, not the full onion. (2) `ToolSurfaceView` as drafted implied
each middleware constructs its own view over `assembled.combined.tools` — building that literally
produces a silent no-op: a fresh `ToolSurfaceView`'s `redact()`/`reorder()` bookkeeping is private to
that instance, disconnected from any other instance's `finalize()` call. Found by the first version of
this ADR's own test file failing in exactly that way. Fixed: `TurnContext` now owns ONE
`ToolSurfaceView` (`tool_surface`), shared by every middleware in the chain and finalized exactly once
by `run_turn_middleware_chain()` itself.

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

## 5. Executed evidence (superseding this ADR's original, pre-implementation §5)

Implemented `include/agentengine/core/turn_middleware.hpp`: `TurnContext{assembled, tool_surface}`;
`ToolSurfaceView` (`descriptors()`/`redact()`/`reorder()`/`annotate_description()`/`finalize()`, never
exposing a mutable `ToolDescriptor&`); `redact_subspan()`; `run_turn_middleware_chain<Ms...>()`
(single forward pass over a declared `std::tuple<Ms...>`, first `on_turn` returning
`std::unexpected` stops the chain, `finalize()`s the shared `tool_surface` exactly once at the end);
`Compactor<N>` (a real `turn`-middleware conformer proving §4's 005 §8 Q3 re-resolution).

**Two real, mid-implementation findings** (§1 header, and `turn_middleware.hpp`'s own top comment,
have the full account):
1. The draft's "reuses ADR-033's before/after onion verbatim" claim does not survive contact with
   what's actually being wrapped. `middleware.hpp`'s onion exists to sandwich a REAL inner action (the
   backend call) between a middleware's `before_model`/`after_model`. The turn/pre_model point has no
   inner action to sandwich — `assemble_context()` already fully ran before the chain starts, and
   there is no model call between two turn middlewares. What's implemented, and what actually matches
   017 §4's verdict vocabulary, is simpler: one forward pass, `on_turn` succeeds (allow, having
   applied whatever mutations in place) or denies (stops the chain). This was never load-bearing for
   any §3 claim as originally worded (none of the four claims require a two-phase onion specifically),
   so this is a genuine simplification, not a broken promise.
2. `ToolSurfaceView` as drafted (a middleware "constructs" one over `assembled.combined.tools`)
   produces a silent no-op if taken literally: two independently-constructed `ToolSurfaceView`
   instances over the same vector do NOT share `redacted_`/`pending_reorder_` state — a middleware's
   `redact()` call on ITS OWN local instance is invisible to whichever instance's `finalize()`
   eventually runs. Found by `test_turn_middleware.cpp`'s own first version, which built exactly this
   mistake (a local view inside the test middleware, a second local view after the chain to check the
   result) and got a passing "allow" but an unredacted tool list. **Fixed**: `TurnContext` owns ONE
   `ToolSurfaceView` (`tool_surface`), constructed once per chain run and shared by every middleware
   via `ctx.tool_surface`; `run_turn_middleware_chain()` itself calls `finalize()` exactly once, after
   the whole chain settles.

`tests/test_turn_middleware.cpp`, **22/22 checks passed**, Windows/MSVC:
- A `HostileToolMiddleware` using ONLY `ctx.tool_surface`'s public API (never reaching around it)
  attempts `annotate_description`+`redact` on a 3-tool surface; every SURVIVING tool's `invoke()` is
  called for real and its return value checked against a per-tool marker set at construction — proving
  no substitution occurred through the sanctioned API (§3 claim 1).
- A 2-middleware chain (`DenyingMiddleware` then `CountingMiddleware`) proves first-deny stops the
  chain: the second middleware's own call counter stays at 0.
- Re-running an identical `Compactor<2>` chain against identical input twice produces byte-identical
  output (§3 claim 4, I5).
- A 5-message scenario with a deliberately-split-inducing `Compactor<3>` (naive last-3 windowing would
  land exactly on the `ToolResult` message, separating it from its `ToolCall` one message earlier) is
  extended backward to keep both — proving the atomic-pair invariant for real, not just asserting it
  holds by construction. `history[]` non-interference (§3 claim 3) is proven BOTH ways: structurally
  (`TurnContext` carries no reference to any `history_` at all — provable by reading the type, the
  same "proof by absence" ADR-066's I2 argument already uses) and behaviorally (a real, untouched
  `history` vector is asserted byte-identical before/after the compactor runs).
- `redact_subspan()` is checked against 8 `{offset, length}` cases including adversarial ones
  (`offset == size`, `offset > size`, `length` far exceeding the remainder, whole-string removal) —
  every result is exactly `original`'s bytes with one contiguous range removed (§3 claim 2).

Full-tree rebuild (`cmake --build . --config Debug`, all targets): **zero compile errors**. Full
`ctest`: same 181/191 baseline as ADR-066's own pass (10 not-run are the pre-existing, unrelated
CPython-embedding targets — confirmed unaffected by this ADR too). Commands: `cmake --build build
--target test_turn_middleware --config Debug`, `ctest --test-dir build -C Debug --output-on-failure`.

**Not run, named rather than left implied**: `require_approval` (017 §4's fifth verdict) is not
modeled by `on_turn`'s binary success/`std::unexpected` outcome — it would mean suspending the run for
human approval before the model sees the context, needing `rt::AgentSession`'s own real suspend/
approval machinery, out of scope here (§7).

## 6. Per-claim verdicts (superseding this ADR's original, pre-implementation §6)

| Claim (§3) | Verdict |
|---|---|
| A `turn` middleware cannot cause a tool's actually-dispatched `invoke` to differ from what fan-out produced. | **CORRECT, scoped to the sanctioned API.** `test_turn_middleware.cpp`'s hostile-middleware test: using only `ToolSurfaceView`'s public methods, every surviving tool's `invoke()` still returns its original marker. Narrower than an unscoped reading of the claim: a middleware that bypasses `ToolSurfaceView` and reaches `TurnContext::assembled.combined.tools` directly (still a plain mutable reference) is NOT prevented — C++ offers no way to give one reference read/write access to `.messages` while denying it for `.tools` within the same struct without a larger refactor, out of scope here (named in `turn_middleware.hpp`'s own top comment, matching `Tainted<T>`'s own "not a fix for a careless or bad-faith wrap" scoping). |
| `redact_subspan()` cannot introduce content that wasn't already present in the original `TaintedText`. | **CORRECT** — 8-case property check including adversarial offset/length combinations; every result is a prefix+suffix split of the original bytes with the requested range removed, clamped safely on out-of-range input rather than throwing. |
| A `turn`-level `Compactor` never mutates `history[]`. | **CORRECT, and provable two ways** — structurally (by `TurnContext`'s own field list: no reference to any `history_` vector exists anywhere reachable from `Compactor<N>::on_turn`, so there is no expression that could touch it) and behaviorally (a real `history` vector is asserted untouched after a real compaction run). |
| Chaining multiple `turn` middlewares is deterministic given `{Ms..., assembled}`. | **CORRECT** — re-running an identical chain against identical input twice produces byte-identical `assembled.combined.messages`. |

## 7. The decision

**Design B, as corrected during implementation (§5), is adopted and implemented**, scoped narrowly:
the `turn`/`pre_model` point only, tool-surface enforcement via `ToolSurfaceView`, taint-safe
compaction via `redact_subspan()`, a single forward pass (not the full before/after onion). It binds:
- `002-Agent-Model-and-Authoring.md` §5 — the `turn` interception point moves from declared-but-
  unwired to specified, implemented, AND wired into `rt::AgentSession` for real (2026-08-20, see §7's
  residual-risk update below): `AgentSession::set_turn_middleware_hook()` runs the chain once per
  round, right before that round's `ChatRequest` is built.
- `017-Safety-and-Content-Governance.md` §4 — closes the `pre_model` filter point's `allow`/
  `annotate`/`redact`/`deny` verdicts; `require_approval` is explicitly not modeled (see residuals).
- `005-Sessions-State-and-Memory.md` §8 Q3 — re-resolved narrower: a `turn`-level `Compactor` may
  shape what one turn's model call sees, but never rewrites `history[]` — proven both structurally
  (the type signature) and behaviorally (§5/§6); 005 §4's own `history[]` compaction is untouched.
- Depends on `decisions/ADR-066-context-provider-attribution-provenance.md` (Proposed).

**Explicitly out of scope, named rather than left implied:**
- `post_model` (017 §4) — remains orphaned; `decisions/ADR-069-content-triggered-model-response-
  replay.md` (not yet implemented) mitigates but does not close it.
- `require_approval` (017 §4's fifth verdict) — `on_turn`'s binary allow/deny outcome does not model
  it; wiring it needs `rt::AgentSession`'s own real suspend/approval machinery.
- Budget/priority arbitration across sources — `OpenQuestions.md` OQ-18 red-team reason #2's rejection
  of a shared cross-contributor budget pool stands unchanged.
- Cross-source dedup, warn-before-evict, and a shared detector/verdict interface spanning this ADR
  and ADR-068/ADR-069 — all named, deliberately deferred.

**Residual risks:**
- **WIRED, real end-to-end evidence (2026-08-20)**: `AgentSession::set_turn_middleware_hook()`
  (`agent_session.hpp`) is a new, optional (`nullptr`-by-default) member — a
  `TurnMiddlewareHook = std::function<task<result<std::monostate>>(TurnContext&)>` (`turn_middleware.
  hpp`) a host sets once, and `run_rounds()` calls it exactly once per round, after context assembly
  (including the dynamically-injected `schedule_wakeup` tool) and before `ChatRequest` is built —
  confirmed via re-grounding that this is the ONE genuine `pre_model` seam in the file: the two OTHER
  `on_context()` call sites (`resolve_interaction()`'s approval branch, `resolve_codeact_ask()`) only
  ever build a `ToolTable` to dispatch an ALREADY-DECIDED tool call and never reach the model, so
  wiring only `run_rounds()`'s own call site is complete, not a partial cut. `AgentSession` adapts its
  raw `ContextContribution` into a `ContextAssemblyResult` with an empty `drops` list at the call site
  (the ADR-066 seam's own `ContextAssemblyResult` doesn't exist yet at this point in `AgentSession`,
  since it never calls `assemble_context()` itself) — a small, local adapter, not a change to
  `turn_middleware.hpp`. `tests/test_rt_agent_session_turn_middleware.cpp`, 8/8 checks: a redacting
  middleware's decision reaches the REAL outbound `ChatRequest` (the redacted tool is verifiably
  absent from what the mock backend received); a denying middleware fails the round with the model
  NEVER CALLED AT ALL (a real pre-model denial, not a post-hoc check); with no hook set, behavior is
  byte-identical to every other existing `AgentSession` test in the tree. Full-tree rebuild: zero
  compile errors; full `ctest`: 187/197 (same 10 pre-existing, unrelated not-run targets as every
  other ADR in this batch).
- `ToolSurfaceView`'s guarantee holds against a middleware using ONLY its sanctioned public API — NOT
  against one that bypasses it and mutates `TurnContext::assembled.combined.tools` directly (§6's own
  narrowed verdict). Closing this fully would need restructuring `ContextContribution`/
  `ContextAssemblyResult` so `.tools` isn't reachable as a plain mutable vector once a `TurnContext`
  exists — a larger refactor, not attempted here.
- The `Compactor`-output-leaking-into-a-checkpoint question (does a `turn`-level compactor's state, if
  it has any, reopen 005 §4's retention guarantee via a `StateBag`-like mechanism) — still open,
  unchanged by implementation (the proven `Compactor<N>` itself holds no state across turns, but a
  hypothetical stateful strategy is not ruled out by anything built here).
