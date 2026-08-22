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
