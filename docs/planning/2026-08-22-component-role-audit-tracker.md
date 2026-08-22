# Component role/overlap audit — tracker

**Status:** open findings, no ADR yet — nothing here is a decision, just a recorded gap. **Practice:**
this file is the running log for an informal, recurring session ritual (started 2026-08-22): pick one
component at random, check whether it's still pulling its own weight, and whether its role overlaps
with something else in a way that's confusing rather than deliberate. Append new entries below rather
than opening a new file per session.

Each entry: what was picked, what was found, and its **disposition** — closed (fixed), tracked (real
gap, deliberately deferred, with why), or no finding (checked, nothing wrong).

---

## 2026-08-22 — `core/context_provider.hpp` and the `ContextProvider` composition cluster

Random pick landed on `core/context_provider.hpp` (the `ContextProvider` concept itself), which pulled
in three composite types that all do "merge N `ContextProvider`s into one" in slightly different ways:

- `HistoryAndSkillsProvider<H,S>` (`core/history_and_skills_provider.hpp`) — hand-written, fixed
  2-provider composite. The original.
- `ComposedContextProvider<Ms...>` (`core/composed_context_provider.hpp`) — its own top comment names
  itself as the generalization of the above to an arbitrary pack, built on the same
  `context_assembly.hpp::assemble_context()`.
- `detail::LazyComposedContextProvider<Ms...>` (`core/session_builder.hpp`) — a further variant needed
  because `AgentSession::history_provider_` is a plain, always-default-constructed value member, which
  `ComposedContextProvider<Ms...>` can only satisfy when *every* `Ms` is itself default-constructible —
  untrue for real `SkillsProvider`/`MemoryProvider`/`VectorRagContextProvider`. `LazyComposedContextProvider`
  starts empty and is `engage()`d once, after construction, with the real values.

### Finding A — `HistoryAndSkillsProvider` is redundant and its ordering is a latent footgun

Fully subsumed by `ComposedContextProvider<Ms...>` since the latter was added
(`efdac09`). Grep confirms only two real (non-comment) usages left, both regression/e2e tests for a
historical wire-ordering bug (`tests/test_rt_agent_session_real_backend.cpp`,
`tests/test_rt_agent_session_skills_live_e2e.cpp`).

Its template parameter order is `<HistoryProviderT, SkillsProviderT>` but its constructor deliberately
pushes **skills first, history second** onto the wire (a documented, intentional inversion — the type's
own name/parameter order is not a reliable guide to wire order). That inversion is safe today only
because a code comment says so; nothing in the type signature communicates it. `ComposedContextProvider
<Ms...>` doesn't have this problem — declared order *is* wire order, always.

**Disposition: tracked, not closed.** Low-severity (two known call sites, both tests), so not worth an
isolated point-fix. Right move is to fold this into Finding B's redesign rather than patch it alone —
see "Recommended follow-up" below.

### Finding B — `ComposedContextProvider` still has the fork-aliasing bug `LazyComposedContextProvider` was fixed for

`AgentSession::fork_from()` (`rt/agent_session.hpp:1059`) plain copy-assigns `history_provider_`.
`ContextProviderDescriptor`'s closures (`context_assembly.hpp`) capture each wrapped provider through a
`shared_ptr` by value, so copying a composite provider aliases the *same* underlying provider instances
rather than producing independent ones.

This exact bug was found live (round 4 red-team, `session_builder.hpp` finding 9) for
`LazyComposedContextProvider` — reproduced with a stateful fixture: mutate the original via
`on_turn_end`, read the copy's `on_context`, see the mutation. Fixed there by making
`LazyComposedContextProvider` move-only.

The same finding's own text explicitly disclosed the residual: *"The underlying shared_ptr-aliasing
mechanism in `ComposedContextProvider` itself is UNCHANGED — out of this file's scope."* Confirmed via
`git log` that no later commit touched `composed_context_provider.hpp` for this. `ComposedContextProvider`
is still plain-copyable today, and it's used directly in production (`NativeCapabilityAnnouncer`,
`src/backends/native_process/native_capability_announcer.hpp:77`, per ADR-071) as well as being available
to any consumer-dev host that composes providers this way and later calls `fork_from()`.

Checked severity for the one shipped direct user (`NativeCapabilityAnnouncer`): `NativeProcessProvider`'s
members (`approval_`, discovery patterns) are set at construction and never mutated per-call, so aliasing
it specifically is currently low-impact. The open exposure is generic: *any* consumer composing a
genuinely stateful provider (a memory provider, a skills provider, a RAG cache) directly through
`ComposedContextProvider` and then forking that session inherits the same aliasing `LazyComposedContextProvider`
was fixed for.

**Disposition: tracked, deliberately deferred — not a bug to rush-fix.** This is exactly the kind of
residual `ADR-070`'s ship-first/harden-later posture and the delegated-decision-seam already cover: the
engine is not trying to make every composition path fork-safe by construction before shipping the
feature surface; a host/consumer-dev composing stateful providers directly and calling `fork_from()`
owns that isolation decision, same as other already-disclosed ADR-070/ADR-071 residuals. Not silently
unaddressed — recorded here, plus already disclosed once in `session_builder.hpp`'s own finding 9 text.

### Recommended follow-up (not started)

Per standing project-owner guidance on design taste (prefer a clean redesign over patching around
existing shapes for reuse's sake): when this cluster is next touched for real, the three-type shape
(`HistoryAndSkillsProvider` / `ComposedContextProvider` / `LazyComposedContextProvider`) is a better
candidate for **one coherent redesign** — e.g. a single composite type that is always safely
constructible in `AgentSession`'s plain value-member slot (closing the reason `LazyComposedContextProvider`
had to exist as a separate type) and move-only by construction (closing Finding B at the source instead
of per-conformer) — than for patching each layer separately. Not scheduled; no ADR opened yet.

### Disposition policy applied to Finding B (per project-owner direction, 2026-08-22)

`ComposedContextProvider`'s fork-aliasing gap is **not** being queued for an immediate point-fix. This
project has already made a deliberate, ADR-backed trade (`ADR-070`): ship a broad feature surface and
let a host/consumer-dev embedding AgentEngine as a library own the hardening of scope they've been
delegated, rather than the engine closing every residual before shipping. Finding B fits that posture —
a disclosed, understood, low-current-blast-radius gap — so it stays *tracked*, not rushed. If/when the
composition cluster gets its promised clean redesign (above), Finding B is closed at the source as a
side effect, not as a separate patch.

---

## 2026-08-22 — `ContextProvider` as an extension point ("AIContextProvider")

Follow-up request used the name `AIContextProvider`. That name does not exist in this codebase — it's
**MAF's** name for the equivalent seam. AgentEngine's own type is `ContextProvider`, and the difference
is deliberate on two axes, both already recorded in 005 §5 / `OpenQuestions.md` OQ-18 (judged,
2026-08-11):

- **Naming**: CLAUDE.md's "no .NET/managed-runtime vocabulary" rule — MAF supplies the *shape*, not the
  spelling.
- **Composition mechanics**: MAF's `AIContextProvider` runs contributors as a *sequential pipeline*
  (provider N sees provider N−1's already-merged `AIContext`). AgentEngine's `assemble_context()` runs
  contributors as *independent fan-out* (each `on_context()` sees only `SessionContext`, never a prior
  contributor's output) — a generic pipeline/reactive variant was designed, red-teamed, and rejected for
  reopening cross-contributor coupling. A concrete reactive need is meant to be solved with a
  purpose-built composite `ContextProvider` (the `HistoryAndSkillsProvider`/`ComposedContextProvider`
  idiom above), not by widening the generic seam.

**No finding on naming/mechanics — this boundary is intentional, judged, and consistently documented.**

### Correction: what reaches the model is actually two tiers, not one

User pointed out (2026-08-22) that "contributing context to the LLM" is a two-tier architecture, confirmed
against real code — the framing above only covered tier 1:

- **Tier 1 — contribute** (`ContextProvider.on_context`, this section's whole subject): every conformer
  produces a `ContextContribution` (instructions/messages/tools); `assemble_context()` fans them out
  independently and merges in declared order. Each contributor may only ADD.
- **Tier 2 — govern/filter** (`TurnMiddleware.on_turn`, `core/turn_middleware.hpp`, `ADR-067`): runs once,
  AFTER tier 1 has already merged everything, immediately before the model call (the `pre_model` point).
  Structurally SUBTRACTIVE/ADVISORY-ONLY against what tier 1 already produced — `redact_subspan()` can
  only remove a byte range from an already-declassified `TaintedText` (never introduce new, undeclassified
  text); `ToolSurfaceView` only allows `redact()`/`reorder()`/`annotate_description()` on tools tier 1
  already contributed, never touching `invoke`/`capability_ceiling`/`approval_mode`. A middleware can also
  `deny` (stop the whole chain, 017 §4's verdict vocabulary). Wanting to ADD new instruction text at this
  point is explicitly not supported — that must be a tier-1 contribution instead.

Not overlap — genuinely complementary halves of "what reaches the LLM," and the boundary between them
(add vs. govern-what-was-added) is structurally enforced by the types themselves, not just by convention.
**Implication for Finding C below**: an onboarding sample/doc for "customize what the LLM sees" needs to
teach both tiers together, not tier 1 alone, or a reader would not learn the ONE sanctioned place to
safely filter/redact already-contributed content.

### Conformer ecosystem check — no overlap found

Checked all five real `ContextProvider` conformers (`HistoryProvider`, `SkillsProvider`, `MemoryProvider`,
`VectorRagContextProvider`, `ToolOptimizerProvider`) plus `NativeProcessProvider`/`NativeCapabilityAnnouncer`.
Each file's top comment states its scope and explicitly disclaims overlap with its nearest neighbor —
e.g. `VectorRagContextProvider`'s own comment: mirrors `MemoryProvider`'s *shape* closely, but is a
deliberately separate class, not a specialization or subclass (`ADR-063` §2.1b), and that "one class per
kind" decision was independently red-teamed and confirmed "consistent with the real
`MemoryProvider`/`SkillsProvider` precedent, not merely asserted." Where two conformers genuinely share
logic, it's factored into a real shared helper instead of duplicated (`provenance_marker.hpp::
neutralize_forged_provenance_markers()`, used by both the memory and RAG providers). **No finding** —
this cluster is well-governed.

### Finding C — the actual "write your own" on-ramp is undocumented and unexampled

The user's own framing of this feature ("tính năng quan trọng cho phép tùy chỉnh và xây dựng tính năng")
is exactly right, but nothing in the tree demonstrates it end-to-end for a third-party/consumer-dev:

- `samples/` — named in `CONVENTIONS.md`'s own layout table as "runnable programs over the public
  surface" — contains only a `README.md` describing intent. Zero actual sample files exist, for any
  extension point, `ContextProvider` included.
- No guide anywhere (checked `docs/architecture/`, `docs/planning/`, RFC 005 itself) walks through
  writing and wiring a custom `ContextProvider` from scratch — RFC 005 §5 documents the concept's shape
  and its built-in kinds, not an authoring walkthrough.
- The only concrete "here's a type that satisfies the concept without being a built-in kind" reference
  in the whole tree is a hand-rolled mock conformer inside `tests/test_composed_context_provider.cpp` —
  not surfaced as a guide or sample anywhere a consumer-dev would find it.

Minor, non-blocking naming note found alongside this: `web/marketing/api/providers.html` already exists
and documents "Model providers" (`ChatClient` backends) — an unrelated seam that happens to share the
English word "provider." Not confusing in code (distinct type names), but worth remembering if a future
"Context providers" web page is added, so the two aren't titled ambiguously against each other.

**Disposition: tracked, not closed.** Recommended next step (not started): add one real sample under
`samples/` showing a minimal custom `ContextProvider` (e.g. a "hello world" provider contributing one
instruction string) wired into an `AgentSession`, and/or a short "Writing a ContextProvider" page in the
web docs, following the same pilot pattern already approved for `runtime.html`/`providers.html`
(see the web-docs-overhaul project memory).

### Finding D — no declared limit on chain length (confirmed intentional); budget enforcement that exists is per-contributor and post-hoc, not aggregate/pre-flight

Checked both chains for an explicit maximum contributor/middleware count, and for what actually stops an
unbounded assembled context from reaching the model. Traced the real call path
(`context_assembly.hpp::assemble_context()` → `AgentSession::run_rounds()` → `run_model_call()`,
`rt/agent_session.hpp:1919-1956`).

- **Tier 1 (`ContextProvider` chain).** `ComposedContextProvider<Ms...>` / `LazyComposedContextProvider
  <Ms...>` take `Ms...` as a compile-time template pack — no project-declared cap (no `MaxProviders<N>`
  policy tag, unlike this codebase's own established idiom for other bounds, e.g. `MaxTurns<N>`,
  `TokenBudget<N>`). The only ceiling is whatever the compiler's own template-instantiation/recursion
  depth allows — an implementation limit, not a declared invariant.
- **Tier 2 (`TurnMiddleware` chain).** Same shape: `run_turn_middleware_chain<Ms...>`
  (`turn_middleware.hpp:207-261`) is a `constexpr`-recursive walk over `std::tuple<Ms...>`, one
  specialization per pack length, again bounded only by the compiler, not a declared count.
- **The budget mechanism that exists (`ContextBudget.max_tokens`) is per-contributor, not aggregate.**
  `assemble_context()` (`context_assembly.hpp:193-205`) trims only the OLDEST MESSAGES WITHIN one
  contributor's own contribution once THAT contributor's own declared budget is exceeded — deliberate,
  documented (drop order must stay predictable from each contributor's own declared budget alone, never
  a shared pool). There is no code path anywhere that caps the SUM of every contributor's output — N
  contributors each individually under budget (or left at the `max_tokens == 0` default, unbounded) can
  still combine into an arbitrarily large `ContextContribution`.
- **The one downstream numeric safety net, `token_budget_` (`AgentSession`), is checked AFTER the model
  call returns, not before.** `run_rounds()` builds `ChatRequest` directly from the fully-assembled,
  unchecked `contribution->messages`/`.tools` and sends it (`rt/agent_session.hpp:1919`, `run_model_call`)
  with no pre-flight size check at all. Only once the response comes back does
  `run_tokens_consumed_ += response->usage...` get compared against `token_budget_` (line 1950) — and
  that's a **cumulative, cross-round RUN budget** (stops future rounds once exceeded), not a per-request
  cap that could have stopped THIS request from being sent oversized in the first place. A first round
  alone, with generously-composed providers, is not gated by anything in-engine before hitting the wire.

**Disposition (confirmed by project owner, 2026-08-22): no chain-length limit is intentional, not a gap.**
Neither `ComposedContextProvider<Ms...>`/`LazyComposedContextProvider<Ms...>` (tier 1) nor
`run_turn_middleware_chain<Ms...>` (tier 2) should have a declared `MaxProviders<N>`/`MaxMiddlewares<N>`
cap — a consumer-dev composing their own tree of providers/middlewares is meant to be free to build
whatever shape they need there, unconstrained by an arbitrary engine-picked number. Matches this
project's already-established `ADR-070` posture: ship a broad, permissive composition surface; a
host/consumer-dev embedding AgentEngine owns the tradeoffs of what they compose. **Not tracked as
something to fix.**

What stays worth recording precisely (distinct from the count question above, not re-opened by this
disposition): the *token-budget* enforcement that does exist is per-contributor and post-hoc, not
aggregate/pre-flight — so "budgets are enforced" (I8) is real but narrower than the name alone suggests.
That's a factual note about what the mechanism currently does, kept here for accuracy, not a request to
add a count-based limit or otherwise constrain how freely a consumer-dev can compose chains.
