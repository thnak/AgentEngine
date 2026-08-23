# 005 — Sessions, State and Memory

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 001, 003, 019, 027 (historical: originally also depended on Quark 012 — ADR-037 removed that dependency) · **Gate:** §7

## Goal

Define the durable thing a conversation *is*: history, state, and the memory providers that shape
what a model actually sees, with a persistence story that survives process restart and node loss.

## 1. Session

**Terminology (027 §7):** the type is `AgentSession`. The bare word "session" is unambiguous in
prose — MCP's own transport-level session concept is gone as of `2026-07-28` (027 §5), so nothing
external claims it — but the public *type name* is still qualified for the same reason `Agent`,
`AgentSkillsProvider`, and this project's other public types are: a bare generic English noun is a
weak name for a type meant to be imported into arbitrary host code. Only the declaration changes;
this RFC still says "session" throughout in prose, as before.

```
AgentSession = { session_id, principal, history[], state, metadata, created_at, updated_at }
```

- A plain templated class instance, one per session, keyed by `session_id` (001 §1; `rt/agent_session.hpp`)
  — no actor lifecycle (historical: was one Quark actor instance before ADR-037 removed Quark).
  Single-executor by I1, now enforced by `rt::AsyncMutex` held for the whole duration of every
  public async entry point — a runtime-checked guard, not the structural mailbox exclusivity Quark
  gave for free; ADR-037's own red-team pass names this a real, honest narrowing.
- **`history`** is an append-mostly sequence of `Message` (003). Rewriting history is an explicit,
  audited operation (compaction, redaction, fork), never an incidental mutation.
- **`state`** is a typed bag for workflow and agent scratch state, checkpointed with the session.
- A session belongs to exactly one **principal** (007); cross-principal access is denied by the
  `rt::AsyncMutex`-guarded entry point (historical: "at the actor boundary" before ADR-037), not by
  a later check.

**Lifecycle:** sessions are activated on demand and evicted per policy. **Passivation of an idle
session (historical: Quark ADR-028/034's host-managed passivate/reactivate) is not carried over**:
AgentEngineSpecification.md §7 names the absence of host-managed passivation/reactivation across
process restarts or node loss as a permanent, accepted gap from ADR-037, not a re-hosted mechanism —
an idle session's storage cost (the per-GB idle-session density budget, 023) still applies, but the
activate-on-demand/passivate-when-idle lifecycle itself is not currently reproduced in `rt::`.

## 2. Persistence

The store is `rt::SessionStore` (single-slot, overwrite-latest) and `rt::AppendLogStore`
(append-only, multi-version) — two distinct contracts (historical: both were unified under Quark
012's single `Store` seam before ADR-037 removed that dependency). Two modes, selected per
deployment:

| Mode | Shape | Use |
|---|---|---|
| **Snapshot** | Whole session serialized at intervals / turn boundaries, via `rt::SessionStore` | Simple, small sessions |
| **Event-sourced** | Append each turn's events via `rt::AppendLogStore`; periodic checkpoints | Long sessions, audit, time-travel (014) |

Defaults follow AgentEngine's own minimal-dependency posture (historical: originally Quark's):
`InMemoryStore` for tests, `FileStore` (append-only WAL + durable flush) as the std-only default,
SQLite/RocksDB/Postgres/object-store as opt-in adapters behind the same seam. **AgentEngine adds no
storage engine of its own.**

**Durability contract:** a turn is acknowledged to the caller only after its effects and history
delta are durable, or the session declares an `at_most_once_ack` durability policy — a
session-level acknowledgment policy defined here in 005, distinct from 019 §3's per-effect
`pure`/`idempotent`/`at-most-once` classification, which governs individual tool effects, not
turn acknowledgment. Silently acknowledging before durability is the failure mode that loses a
user's conversation on a crash.

## 3. Context assembly

What the model sees is **derived**, never simply "the history". Per turn:

```
context = instructions
        ⊕ context_provider outputs       (ordered, each budgeted — instructions ⊕ messages ⊕ tools)
        ⊕ selected history window        (compaction strategy)
        ⊕ statically declared tools      (002, 006)
        ⊕ middleware additions           (002 §5)
subject to: context_window, TokenBudget, per-source token budgets
```

**Tools are not solely a static declaration.** A context provider's contribution can include tools,
not just text (§5) — the statically declared set (002) and every provider's tool contribution are
unioned into one per-run table (006 §6), which is where the snapshot-at-run-start rule applies.

**Rules:**

- **Every contributor declares a token budget.** Assembly is deterministic under budget pressure:
  drop order is declared, not incidental, and drops are recorded in the trace.
- **Assembly is pure and replayable** given `{history, memory outputs, policies}` — it is part of
  the I5 replay surface.
- **Tainted content stays tainted through assembly** (003 §2) and is delimited in the prompt with
  provenance markers (017).

## 4. Compaction

Long sessions exceed context windows. Strategies, selected by policy:

| Strategy | Mechanism |
|---|---|
| `Window<N>` | Keep the last N turns verbatim |
| `Summarize<N>` | Summarize older turns into a `system` summary message via a declared model |
| `Salience` | Retain by scored importance (tool results, decisions, user constraints) |
| `Hierarchical` | Recursive summaries with retained anchors |

**Invariants:** compaction never silently deletes — the pre-compaction history remains in the
durable log and is addressable for audit and time-travel. This guarantee is **Event-sourced-mode
only** (§2): it is backed by the append-only log that mode retains, and there is no equivalent
retention mechanism for Snapshot mode. A Snapshot-mode session accepts **lossy compaction** — the
prior snapshot is overwritten, not archived — as the accepted cost of that mode's simplicity,
rather than this RFC inventing a second retention mechanism to give Snapshot mode the same
guarantee. Compaction is an audited, attributed operation with its own span in either mode. A
compaction that drops a pending tool-call/result pair is a defect (checked).

## 5. Context providers

**Terminology (027 §3):** there is one seam for "contribute to the context before the model is
called", named `ContextProvider` after MAF. Memory, history, skills, and retrieval are *kinds* of
context provider, not parallel concepts. An earlier draft of this RFC specified a separate
`MemoryProvider`; that was two seams doing one job and is superseded.

```cpp
struct ContextProvider {
    ae::task<result<ContextContribution>> on_context(SessionContext&, EffectContext&);
    ae::task<> on_turn_end(TurnView, EffectContext&);
};

struct ContextContribution {
    std::optional<std::string> instructions;   // appended, budgeted (§3)
    std::vector<Message>       messages;        // injected before the history window (003)
    std::vector<ToolDecl>      tools;            // unioned into the run's tool table (006 §6)
};
```

`ContextContribution` deliberately mirrors MAF's `AIContext` (`Instructions` / `Messages` / `Tools`
— `docs/research/2026-maf-provider-concepts.md` §1): a provider is not limited to injecting text. A
retrieval provider that exposes an on-demand search tool rather than always dumping results into
context is the concrete precedent (MAF's `TextSearchProvider`), and the same shape covers any
provider that needs to hand the model a capability rather than a paragraph.

**Composition mechanics — a verified, judged divergence from MAF (OQ-18, resolved 2026-08-11).**
`assemble_context()` (`include/agentengine/core/context_assembly.hpp`) runs multiple contributors as
**independent fan-out**: each `on_context()` call sees only `SessionContext`/`EffectContext`, never
a prior contributor's `ContextContribution`, and results are merged afterward in declared order
(`instructions` concatenated, `messages`/`tools` appended). MAF's own `AIContextProvider` does not
work this way — it is a **sequential pipeline**: provider N's input is provider N−1's
already-merged `AIContext`, letting a later provider see and react to an earlier one's contribution
within the same turn (`docs/research/2026-08-11-maf-middleware-codeact-skills-deep-dive.md` §2). A
generic pipeline/reactive mechanism was designed and red-teamed against this seam and **rejected**
— it would reopen exactly the cross-contributor coupling this file's own budget rule already refuses
(§3), and MAF's own version only works because it source-stamps every contributed message with its
origin provider, a provenance mechanism this seam doesn't have and a bare accumulated-context
parameter can't substitute for. The judged answer: fan-out stays generic; a concrete cross-provider
reactive need (e.g. a future memory provider deduping against `SkillsProvider`) is solved with a
purpose-built composite `ContextProvider` that owns its sub-providers directly and decides their
composition itself, not by extending this generic seam. That pattern was originally proven by a
hand-written, fixed two-provider type, `HistoryAndSkillsProvider<H,S>`; `decisions/ADR-074-composed-context-provider-consolidation.md`
(2026-08-23) deleted that type and absorbed its fixes into the general
`ComposedContextProvider<Ms...>` (`include/agentengine/core/composed_context_provider.hpp`) —
declaring `Ms...` in the desired wire order now gets the same ordering guarantee the hand-written
composite used to provide bespoke. A *reactive* composite (one whose sub-providers see each other's
output, the case this paragraph is actually about) still has no built-in type and remains
consumer-authored, same as before. Full reasoning: `OpenQuestions.md` OQ-18.

Kinds: `HistoryProvider` (conversation history) · `SkillsProvider` (009 §8) · **working** memory
(in-session scratch, this RFC's `state`) · **episodic** / **semantic** / **procedural** memory,
whose storage model, writing, retrieval ranking, and consolidation are their own RFC (029) — this
section owns the seam they attach through, not the memory architecture itself.

This is also the seam CodeAct attaches to, matching the integration point MAF settled on — which is
why keeping it singular matters (010, 026).

**On "tool search" for large tool/skill catalogs — not adopted, on evidence.** MAF's source has no
vector- or embedding-based search over tools or skills; its actual answer is progressive disclosure
— a small fixed set of loader tools, or in our case (009 §8b) no loader tools at all, since skills
are mounted read-only on the worktree and read with ordinary file operations. A `ContextProvider`
contributing a *specific* tool per turn (the retrieval-provider case above) is a different, narrower
thing than "search across the whole tool catalog," and we do not build the latter as a core
mechanism — it is exactly the kind of heavy, stateful dependency (embeddings, a vector index) that
CONVENTIONS' dependency tiers push to a plugin (009 §7) or a `SemanticSkillsProvider`-shaped WASM
plugin, never the core.

**Rules:**

- Retrieved memory is **tainted external content** (003 §2) — a retrieval store is an injection
  vector and is treated as one.
- Memory reads and writes are **capability-gated** (007) and scoped to the session's principal;
  cross-principal leakage through a shared index is a release-blocking defect class.
- Memory providers may ship as **WASM plugins** (009) — this is the intended path for vector
  stores, embedding pipelines, and third-party memory services (029 §5).
- **A tool contributed via `ContextContribution.tools` still traverses the full invocation pipeline**
  (006 §3) — a provider can make a tool *available*, never bypass authorize/approve/admit for it.
  Provider-contributed tools carry the provider's identity in the audit record, same as a plugin's.

## 6. Fork, redact, delete

- **Fork** — copy-on-write new session id from a history prefix; the sanctioned answer to
  concurrent runs (001 §4) and to "what if" exploration.
- **Redact** — replace content in place with a tombstone carrying reason and actor. Required for
  data-subject requests; must propagate to checkpoints, recordings, and derived summaries.
- **Delete** — hard removal, including derived artifacts and recordings, with a completion receipt.

These are the operations that make the difference between a demo and a system someone can operate
under a privacy regime, and they are specified here rather than deferred.

## 7. Promotion gate

- **G1** — 10⁶ sessions with realistic history: idle footprint per session and activation p50/p99
  within 023 budgets.
- **G2** — kill -9 mid-turn; on restart, no acknowledged turn is lost and no unacknowledged turn is
  partially applied.
- **G3** — a compaction pass followed by a full replay produces the same final response as the
  uncompacted control for a scripted session (bounded divergence for `Summarize`, exact for
  `Window`). **Event-sourced mode only** — Snapshot mode's compaction is lossy by §4 and has no
  pre-compaction record to replay against.
- **G4** — redaction propagates to every derived artifact; a search over the store finds no residue.

## 8. Open questions

- ~~**Q1** — Whether `state` should be schema-typed and versioned like messages (003 §5). Untyped is
  convenient and ages badly.~~ **Resolved, typed, not separately versioned (2026-08-04):** typed —
  `state` becomes a declared C++ type per agent/workflow (the same "declared, schema-typed"
  discipline 002 already applies to tools and output schemas), which catches an author's own
  shape-drift at compile time instead of at a runtime read. Not separately versioned — 024 §2's
  persistence-migration contract ("checkpoints and sessions are readable across engine upgrades
  within a major version... a format change ships a forward migration") already covers *every*
  persisted format generically; giving `state` its own bespoke version field would duplicate a
  mechanism that already applies to it rather than close a gap.
- ~~**Q2** — Multi-agent sessions: does a session hold one history shared by several agents, or one
  per agent with a shared transcript view? Handoff (002 §4) needs this answered.~~ **Resolved, one
  shared `history[]`, per-agent *views* (2026-08-04):** the answer was already implied by combining
  parts of this spec that hadn't been read together. §1's data model has a single `history[]` per
  `AgentSession`, not one per agent; §3's context assembly already derives a per-turn view via each
  agent's own declared compaction/window/salience policy. Put together: storage is singular, and
  "sees the history per its own policy" (002 §4's Handoff description) was already describing a
  *view*, not separate storage — no new mechanism needed, just naming what was already specified.
  This also explains why no third option was missing: 002 §4's two composition modes already map
  onto the two real cases — **Handoff** (same session, shared `history[]`, per-agent views) for
  agents that should see the same conversation, and **Sub-agent** (a separate run on a separate
  session, 001 §4) for agents that need real isolation, e.g. a sub-agent's internal deliberation the
  parent shouldn't see verbatim. Feeds 012 §9 Q3 directly: `context_id` maps to `session_id`
  one-to-one, never to a group, because there is exactly one history per session by construction.
- ~~**Q3** — Where memory-provider results sit relative to compaction: before (compactable) or after
  (always fresh)?~~ **Resolved, the framing was a category error — always fresh, never compacted
  (2026-08-04):** §4's compaction is specifically an operation on the *accumulating* `history[]`
  (it exists because history grows unboundedly and needs pruning); a memory provider's
  `ContextContribution` (§5) is recomputed fresh every turn and never appended to `history[]` at
  all — there is structurally nothing there for §4's compaction mechanism to act on. What providers
  *do* share with the history window is §3's ordinary per-turn token-budget enforcement ("every
  contributor declares a token budget... drops are recorded in the trace") — but that's budget
  enforcement, not compaction; the two were being conflated. One more piece worth stating explicitly:
  retrieval should query the **durable, uncompacted record** (the full event-sourced log, or 029's
  own consolidated memory store), never the display-trimmed history window — compaction fits what
  the *model* sees this turn to the context window, it does not change what is true about the
  conversation, and a memory provider's job is squarely the latter.
