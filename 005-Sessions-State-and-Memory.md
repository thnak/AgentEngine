# 005 — Sessions, State and Memory

**Status:** Draft · **Depends on:** 001, 003, 019, Quark 012/027 · **Gate:** §7

## Goal

Define the durable thing a conversation *is*: history, state, and the memory providers that shape
what a model actually sees, with a persistence story that survives process restart and node loss.

## 1. Session

```
Session = { session_id, principal, history[], state, metadata, created_at, updated_at }
```

- One **Quark actor instance**, keyed by `session_id` (001 §1). Single-executor by I1.
- **`history`** is an append-mostly sequence of `Message` (003). Rewriting history is an explicit,
  audited operation (compaction, redaction, fork), never an incidental mutation.
- **`state`** is a typed bag for workflow and agent scratch state, checkpointed with the session.
- A session belongs to exactly one **principal** (007); cross-principal access is denied at the
  actor boundary, not by a later check.

**Lifecycle:** sessions are activated on demand, passivated when idle (Quark ADR-028/034), and
evicted per policy. An idle session costs storage, not memory — the per-GB idle-session density is
a 023 budget.

## 2. Persistence

The store is Quark 012's `Store` seam. Two modes, selected per deployment:

| Mode | Shape | Use |
|---|---|---|
| **Snapshot** | Whole session serialized at intervals / turn boundaries | Simple, small sessions |
| **Event-sourced** | Append each turn's events; periodic checkpoints | Long sessions, audit, time-travel (014) |

Defaults follow Quark's dependency posture: `InMemoryStore` for tests, `FileStore` (append-only WAL
+ durable flush) as the std-only default, SQLite/RocksDB/Postgres/object-store as opt-in adapters
behind the same seam. **AgentEngine adds no storage engine of its own.**

**Durability contract:** a turn is acknowledged to the caller only after its effects and history
delta are durable, or the run is explicitly marked `at-most-once` (019). Silently acknowledging
before durability is the failure mode that loses a user's conversation on a crash.

## 3. Context assembly

What the model sees is **derived**, never simply "the history". Per turn:

```
context = instructions
        ⊕ memory_provider outputs        (ordered, each budgeted)
        ⊕ selected history window        (compaction strategy)
        ⊕ tool declarations              (006)
        ⊕ middleware additions           (002 §5)
subject to: context_window, TokenBudget, per-source token budgets
```

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
durable log (event-sourced mode) and is addressable for audit and time-travel. Compaction is an
audited, attributed operation with its own span. A compaction that drops a pending tool-call/result
pair is a defect (checked).

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
```

Kinds: `HistoryProvider` (conversation history) · `SkillsProvider` (009 §8) · **working** memory
(in-session scratch) · **episodic** (past sessions of this principal) · **semantic** (retrieval over
a corpus) · **procedural** (learned instructions).

This is also the seam CodeAct attaches to, matching the integration point MAF settled on — which is
why keeping it singular matters (010, 026).

**Rules:**

- Retrieved memory is **tainted external content** (003 §2) — a retrieval store is an injection
  vector and is treated as one.
- Memory reads and writes are **capability-gated** (007) and scoped to the session's principal;
  cross-principal leakage through a shared index is a release-blocking defect class.
- Memory providers may ship as **WASM plugins** (009) — this is the intended path for vector
  stores, embedding pipelines, and third-party memory services.

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
  `Window`).
- **G4** — redaction propagates to every derived artifact; a search over the store finds no residue.

## 8. Open questions

- **Q1** — Whether `state` should be schema-typed and versioned like messages (003 §5). Untyped is
  convenient and ages badly.
- **Q2** — Multi-agent sessions: does a session hold one history shared by several agents, or one
  per agent with a shared transcript view? Handoff (002 §4) needs this answered.
- **Q3** — Where memory-provider results sit relative to compaction: before (compactable) or after
  (always fresh)?
