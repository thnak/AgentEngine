# ADR-074 — Should the three overlapping `ContextProvider` composites (`HistoryAndSkillsProvider`, `ComposedContextProvider`, `LazyComposedContextProvider`) be consolidated into one type?

**Status:** Judged (2026-08-23, project-owner sign-off, same-session decision). This is explicitly
**not** a full multi-agent design→red-team→prove→judge cycle — the two hard mechanisms this
consolidation carries forward (move-only with a correct moved-from reset; a strong-exception-guarantee
`engage()`) were already separately designed, red-teamed (`session_builder.hpp`'s own rounds 4, 5, and
8), and proven on `LazyComposedContextProvider` before this ADR. What this ADR actually decides is
narrower: whether to *keep three types carrying those fixes inconsistently* or *merge them into one
type that carries both fixes everywhere*. Implemented in this session:
`include/agentengine/core/composed_context_provider.hpp` (rewritten), `history_and_skills_provider.hpp`
(deleted), `include/agentengine/core/session_builder.hpp` (`detail::LazyComposedContextProvider`
removed; `ComposedQuickstartSessionBuilder::HistoryProviderT` now names `ComposedContextProvider<Ms...>`
directly). Proven by `tests/test_composed_context_provider.cpp` (new Part 3: `engage()`, move-only,
no-aliasing-after-move), the full pre-existing `tests/test_session_builder.cpp` suite (B14–B22,
unchanged in what they assert, now exercising the same type instead of a separate one), and every other
real/test call site in the tree re-built and re-run clean (`test_tool_optimizer_provider`,
`test_rt_agent_session_context_provenance`, `test_rt_agent_session_real_backend`,
`test_native_capability_announcer`, `test_rt_agent_session_skills_live_e2e` — compiles, live-network
so not run without an API key).

**Relates to:** `docs/planning/2026-08-22-component-role-audit-tracker.md` Finding A
(`HistoryAndSkillsProvider` fully subsumed, redundant) and Finding B (`ComposedContextProvider`'s
plain-copyable fork-aliasing bug, I1/I4-adjacent), `decisions/ADR-066-context-provider-attribution-provenance.md`
(the composition seam this touches).

## 1. The question

**Stated so it has a wrong answer:** three types in this codebase all do "merge N `ContextProvider`s
into one `ContextProvider`" — `HistoryAndSkillsProvider<H,S>` (a hand-written, fixed two-provider
composite), `ComposedContextProvider<Ms...>` (the general N-provider version, eager-construction-only,
plain-copyable), and `session_builder.hpp`'s `detail::LazyComposedContextProvider<Ms...>` (exists only
because `ComposedContextProvider`'s old default constructor required every `Ms` to be
default-constructible, untrue for a real `SkillsProvider`/`MemoryProvider`/`VectorRagContextProvider`).
Should this project keep three separate types, each carrying its own subset of fixes, or consolidate
into one?

## 2. The competing designs

**Design A (status quo) — keep three types.** Steelman: zero migration risk, zero risk of a new bug in
a rewrite. This is the disposition the audit tracker originally recorded (Findings A/B: "tracked, not
closed," matching `ADR-070`'s ship-first/harden-later posture).

**Design B (chosen) — one type, `ComposedContextProvider<Ms...>`.** Absorb
`LazyComposedContextProvider`'s already-proven fixes (move-only with a correct moved-from reset;
strong-exception-guarantee `engage()`) into the general type; delete the redundant
`HistoryAndSkillsProvider`; delete the now-redundant `LazyComposedContextProvider`. Steelman: closes
Finding B's fork-aliasing bug (I1/I4-adjacent session-isolation gap) **at the source**, for every
caller, not just the one type that happened to get the fix first; closes Finding A's redundancy;
matches this project's own standing design-taste guidance (prefer a clean redesign over patching around
existing shapes for reuse's sake).

## 3. Why this was judged safe to do in-session, not deferred behind a full gate

Two things distinguish this from an ordinary "just go fix it": (a) the fix only *completes* enforcement
of already-designed, already-red-teamed mechanisms (move-only semantics, exception-safe `engage()`) —
it does not invent new security policy; (b) the blast radius was verified, not assumed, before writing
any code: grepped the whole tree for `.fork_from()` callers (3 test files, none touch these types) —
making `ComposedContextProvider` move-only is a **non-breaking** change today, the same situation
`LazyComposedContextProvider`'s own move-only fix was already in when it shipped.

## 4. A real design conflict found DURING implementation, not anticipated in the plan

The first implementation attempt gave `ComposedContextProvider`'s default constructor an
`if constexpr ((std::default_initializable<Ms> && ...))` branch: auto-engage with `Ms{}...` when every
`Ms` happens to be default-constructible (matching the OLD eager-only `ComposedContextProvider`'s own
ergonomics), otherwise start empty (matching `LazyComposedContextProvider`). This looked like a clean
"best of both" unification. It was wrong, and the full test suite caught it:

- `tests/test_session_builder.cpp`'s B22 (round 8's exception-safety regression test) uses
  `ComposedQuickstartSessionBuilder<..., CountingProvider, ThrowingProvider>` — both fixtures happen to
  be default-constructible. With auto-engage, `ProviderT lcp;` (bare default construction) immediately
  populated itself with throwaway default instances and set `engaged_ = true` — so the test's own
  explicit `lcp.engage(...)` call failed with `already_engaged` before ever reaching the code under
  test, and the whole B22 block cascaded into 5 failures.
- This is not a test-only artifact: `ComposedQuickstartSessionBuilder::build()` (session_builder.hpp)
  always calls `.engage()` on `AgentSession::history_provider_` after it default-constructs, and
  `AgentSession::history_provider_` is a plain, always-default-constructed value member with no way for
  the builder to intercept or control *how* it default-constructs. **Any real caller of
  `ComposedQuickstartSessionBuilder` whose chosen `Ms...` all happened to be default-constructible would
  have had `build()` fail every time**, with auto-engage in place — a genuine functional regression the
  audit-tracker-session workflow caught via its own test suite, not something a human reviewer
  necessarily would have spotted from reading the diff alone.

**Resolution:** default construction now **always** starts empty/unengaged, unconditionally — matching
`LazyComposedContextProvider`'s original, simpler, already-correct behavior — never auto-engaging
regardless of `Ms`'s own default-constructibility. Four real call sites that relied on the old eager
type's auto-engage-on-default-construction behavior (`test_composed_context_provider.cpp` Part 2,
`test_rt_agent_session_context_provenance.cpp`, `test_rt_agent_session_real_backend.cpp`'s
`SkillsSession`, `test_rt_agent_session_skills_live_e2e.cpp`'s `SkillsLiveSession`) needed one explicit
`.engage(...)` call added each — a small, mechanical fix once the real requirement was understood
correctly. The lesson generalizes: "auto-behave differently based on a template parameter's own
properties" is exactly the kind of implicit, non-local coupling this project's own naming/design
discipline (CONVENTIONS.md) warns against — the explicit-always-empty default is not just safer here,
it is simpler to reason about, matching the "prefer a clean redesign over a clever conditional" guidance
this ADR's own Design B section already invokes.

## 5. Decision

Design B, implemented as described above. Findings A and B in the component-role-audit tracker are
closed. `session_builder.hpp`'s own extensive round-4/5/8 red-team narrative (its file-top comment) is
kept verbatim as a point-in-time historical record — not rewritten — with a short 2026-08-23 note
redirecting a reader to where those same, still-accurate findings now live.
