# ADR-075 — When a `ContextProvider` contributor exceeds its own declared `ContextBudget`, should `assemble_context()` silently trim it, or fail closed?

**Status:** Judged (2026-08-23, project-owner sign-off, same-session decision). Implemented in this
session: `include/agentengine/core/context_assembly.hpp` (`assemble_context()`'s return type widened
from `task<ContextAssemblyResult>` to `task<result<ContextAssemblyResult>>`; the silent oldest-first
trim loop replaced with a fail-closed check), `include/agentengine/core/composed_context_provider.hpp`
(`ComposedContextProvider::on_context()` propagates the new failure verbatim instead of discarding
`ContextAssemblyResult.drops`). Proven by `tests/test_context_assembly.cpp` (rewritten: proves the
failure — class, code, and attributed contributor — instead of the old trim), `tests/test_composed_context_provider.cpp`
(new Part 1b), and signature-only migrations in `tests/test_context_provenance.cpp`/`test_memory_provider.cpp`
(neither exercises a nonzero budget, so their behavior is unchanged).

**Relates to:** `docs/planning/2026-08-22-component-role-audit-tracker.md` Finding E (per-contributor
budget trimming is invisible — an I4 gap) and its own already-recorded "named tension" (would turning
"exceeded" into a hard error be the same fix as making drops observable, or a second, related one).
Compounds Finding F (a large tool-result reaching context assembly had no protection against being the
one thing that gets silently dropped).

## 1. The question

**Stated so it has a wrong answer:** `assemble_context()` (`context_assembly.hpp`) is the one seam
every `ContextProvider` composite (`ComposedContextProvider`, formerly also the now-deleted
`HistoryAndSkillsProvider`/`LazyComposedContextProvider`) funnels its contributors through. When one
contributor's own `ContextBudget.max_tokens` is exceeded, should assembly keep silently trimming that
contributor's oldest messages and returning success (today's behavior, but genuinely invisible — Finding
E's own trace found the diagnostic `assemble_context()` already produces, `ContextAssemblyResult.drops`,
discarded identically by every real composite that wraps it), or should exceeding a declared budget be a
hard failure of the whole `assemble_context()` call?

## 2. The competing designs

**Design A (status quo) — keep silent trimming, make drops observable instead.** Steelman: preserves
today's "a caller who explicitly opted into a low budget gets graceful degradation" behavior; the fix
becomes purely about plumbing `ContextAssemblyResult.drops` somewhere a caller can see it (a `run_event`,
a log line) rather than changing what assembly *does*. This is the shape the tracker's own "Named
tension" paragraph flagged as the harder-to-decide half of Finding E.

**Design B (chosen) — a nonzero, exceeded `ContextBudget` is a hard failure.** `assemble_context()`
itself returns `std::unexpected` the moment a contributor's own declared budget is exceeded, naming
which contributor (index and declared name) and the measured-vs-declared token counts. No second field
on `ContextBudget` to pick "trim" vs. "error" per contributor — one behavior, not a configurable choice
between two.

## 3. Why Design B, and why no dual-mode config knob

Two things resolved what the tracker's own text left as a genuine open tension:

- **Grepped every real (non-test) `ContextBudget{...}` construction in the tree before deciding.**
  Zero. Every production call site that constructs one uses the default (`max_tokens == 0`, unbounded).
  The "a caller who explicitly opted into a low budget expecting graceful degradation" case the tracker
  worried about turning into a hard failure has **no shipped instance today** — there was no real,
  currently-deployed behavior to weigh against the cleaner design, only a hypothetical future one. This
  is the same discipline `ADR-074`'s own §3 used to justify a same-session fix without a full
  design→red-team→prove→judge cycle: verify the blast radius before deciding, don't assume it.
- **A real, already-shipped precedent for the exact shape already exists in this codebase**:
  `rt/agent_session.hpp`'s cross-round `token_budget_` check already fails closed
  (`failure_class::resource`, `"run.token_budget_exceeded"`) rather than silently truncating a run that
  goes over. Matching that shape at the per-contributor layer, instead of inventing a second one, is the
  "prefer a clean redesign over patching around existing shapes" guidance this project has now applied
  twice in one session (`ADR-074`'s own §2). A dual-mode "trim OR error, chosen per-`ContextBudget`" API
  would have been the reuse-compromise version of this fix: it keeps the silently-dangerous default
  alive for anyone who doesn't opt into the new behavior, and it doubles the surface a future reader has
  to reason about for a distinction that (per the point above) nothing today actually needs.

**What this does NOT decide**: `ContextAssemblyResult.drops`/`ContextDrop` are not deleted — they remain
real, load-bearing vocabulary for `TurnMiddleware::TurnContext` (`turn_middleware.hpp`) independent of
`assemble_context()`'s own budget mechanism (confirmed: `AgentSession` hand-builds an empty one even for
turns that never call `assemble_context()` at all). `assemble_context()` itself simply has no producer
of a real `ContextDrop` left after this change — confirmed, via `ADR-049`'s own independent finding, that
nothing else in the tree ever populated one either. Widening or repurposing that broader type is out of
this ADR's scope.

## 4. Decision

Design B, implemented as described above. Finding E's core observability gap (a drop nobody can see) is
closed by eliminating the mechanism that produced silent drops in the first place, not by adding a new
notification path for it. Finding E in the tracker is closed by this ADR.
