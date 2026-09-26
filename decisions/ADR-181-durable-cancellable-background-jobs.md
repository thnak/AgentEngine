# ADR-181 — Durable, cancellable, retryable background jobs

- **Status**: **Proposed — design pass + red-team pass 1 (2026-09-23), revised (§10). Owner decisions
  Q1–Q3 recorded (§8). Phase 0 (§11) and phase 1 (§12) implemented and proven; phases 2–6 not
  started.** Where §10 and an earlier section disagree, §10 wins.
- **Date**: 2026-09-23
- **Origin**: `docs/planning/first-class-rag-gap.md` §A2. Indexing a large host folder for RAG is a
  long job that needs to run in the background, report progress, be cancelled, survive an app restart,
  and retry transient failures. The background-task support that exists (Milestone 7 Phase B) does none
  of those. Project-owner direction (2026-09-23): finish the background feature properly first — cancel
  (with transaction semantics), persistence, and a retry policy — before RAG builds on it.
- **Implements / completes**: 006 §6b (`Backgroundable`, `background_task`, `StandingEffect`), 019 §3
  (exactly-once effects: idempotency keys, effect journal, effect classes) and 019 §4 (recovery, poison
  quarantine), which are specified but only partly built.
- **Reuses**: ADR-178 / ADR-159 (one `std::stop_source`, checked cooperatively, riding on
  `EffectContext::cancellation`); `rt::EffectJournalEntry` (`rt/effect_journal.hpp`, built, barely
  wired); `IdempotencyKey` (`core/tool_pipeline.hpp`); `effect_class` (`core/tool.hpp`); `RetryPolicy`
  (`core/retry_policy.hpp`); `rt::AppendLogStore`; ADR-060's `report_progress` shape.

## 0. What exists today (read from the code, not the planning notes)

`AgentSession::start_background_task()` (`rt/agent_session.hpp`) runs tool-pipeline steps 1–7
synchronously, then `background_task()` (`core/tool_pipeline.hpp`) runs step 8 onward on a detached
`std::thread` per task. The completion is pushed into a `BackgroundCompletionQueue` held by `weak_ptr`,
so a completion for a destroyed session is dropped without a use-after-free. `StandingEffectRegistry`
(`rt/standing_effect_registry.hpp`, ADR-097) holds the handles.

Measured against what a long job needs:

| Need | Today |
|---|---|
| Cancel stops the work | **No.** `StandingEffectRegistry::cancel()` erases the handle. The thread keeps running; its result is dropped. |
| Cancel is scoped to the task | **No.** The detached thread's `EffectContext` is a copy of the session's, so its `cancellation` token is the **originating run's** token (ADR-178). Cancelling that run signals the background task; cancelling the task signals nothing. |
| `Background<max_concurrent>` is enforced | **No, bypassable (defect, §6 claim 2).** The cap counts registry entries (`count_of(background_task)`). Cancel erases the entry while the thread keeps running, so start→cancel→start… runs unboundedly many threads at once (I8). Not model-reachable today only because nothing exposes `background_task`/cancel to the model yet (see below). |
| Progress | **No.** `background_task()` resets `ctx.report_progress` to a no-op on purpose (ADR-060 §4: the live closure would call into the session from a foreign thread). |
| Survives process restart | **No.** `StandingEffect` is in-memory only; nothing is journaled. |
| Retry | **No.** A failed invoke is delivered as a failed `ToolResult`, once. |
| Outlives / independent of a session | **No.** Owned by one `AgentSession`; completion dropped if the session is gone. A host-level job (RAG indexing) has no home. |
| Bounded threads, clean shutdown | **No.** One detached `std::thread` per task; nothing joins them at shutdown. |
| Model can start/cancel background work (006 §6b) | **No.** Only host code and tests call `start_background_task()`/`cancel_standing_effect()`; no agent-facing tool exists. |

What 019 already specifies and the code has **partly** built:

- `effect_class { pure, idempotent, at_most_once }` is declared per tool (`core/tool.hpp`), default
  `at_most_once` (fail-safe).
- `IdempotencyKey{run_id, turn_index, call_index, argument_digest}` exists.
- `rt::EffectJournalEntry` + `journal_effect_intent/outcome` + `unconfirmed_effect_intents()` exist on
  `AppendLogStore`, but only `worktree_ref_store.hpp` and a test use them. 019 §7 G6's "surface
  *indeterminate*, never guess" decision (the file calls it F3) was explicitly left unbuilt.
- 019 §8 Q1 (resolved): a checkpoint never holds a live capability handle, only a reference; resume
  re-derives the grant from current policy, runs with the narrower set if policy narrowed, and emits a
  `Warning` naming what narrowed.

## 1. The question

What is the one mechanism that runs background work so that it can be **cancelled for real**,
**survives a restart**, **retries transient failures safely**, **reports progress**, and works **with or
without a session** — without weakening I1 (one executor per session), I2 (no ambient authority), I3
(model output never decides permissions), I5 (nondeterminism recorded), or I8 (budgets enforced)?

## 2. Competing designs, steelmanned

### 2A. Harden the existing per-session path in place

Give each task its own `std::stop_source`, keep the registry entry until the thread really exits, add
the task to `AgentSessionRecord`, add a retry loop inside `background_task()`'s thread.

- **For:** smallest diff; no new type; every existing test keeps its shape.
- **Against:** still session-owned, so the RAG case (a corpus shared by many sessions, indexed before
  any session exists) still has no home — the exact limit the owner flagged. Still one detached thread
  per task, so shutdown still cannot join anything. Persistence inside `AgentSessionRecord` ties a job's
  durability to the session record's write cadence. **Rejected** as the whole answer; its cancel and
  cap fixes are kept (they apply to 2B unchanged).

### 2B. A host-owned `rt::BackgroundJobRunner`; sessions become one kind of job owner (**chosen**)

A runner the host constructs and owns, like `SandboxBackendRegistry` or a store. It has a bounded
worker pool, a durable per-job log on `AppendLogStore`, and one job lifecycle. Two kinds of submitter:

- **Host code** submits a job directly (RAG indexing, maintenance). Completion goes to a host callback
  and is also pollable.
- **`AgentSession::start_background_task()`** becomes a thin client: it still runs pipeline steps 1–7
  under `session_mutex_` exactly as today, then submits step 8+ to the runner with `owner = {session,
  run, principal}`. Completion still reaches the session through the same `weak_ptr` queue shape, so
  a gone session still means "drop, no UAF".

- **For:** one lifecycle for both callers; the runner, not the session, owns threads, so shutdown can
  request stop and join; jobs outlive sessions by construction; durability is the job's own log, not a
  side effect of the session record.
- **Against:** a new long-lived host object, and `AgentSession` gains a dependency on it (optional —
  a session with no runner wired keeps today's behavior, or fails closed; §3.8). More surface to prove.

### 2C. Reuse `rt::WorkflowSupervisor` as the job engine

A background job becomes a one-node workflow. The supervisor already has per-run cancel (ADR-159),
checkpoints and time travel, edge retry policies, and quarantine of a failed delivery.

- **For:** maximal reuse of proven cancel/retry/checkpoint code.
- **Against:** the supervisor's checkpoint is *workflow state between supersteps*; a job's resumable
  state is *tool-defined progress inside one long step* (e.g. "indexed 40,000 of 100,000 files"),
  which the supervisor has no seam for. A supervisor run is driven by its caller, not detached, so it
  would still need the runner of 2B underneath. **Rejected** as the engine; its retry predicate
  (`is_retryable()`, transient-only) and cancel shape are reused as-is.

## 3. The design (2B)

### 3.1 The job record — data, never authority

A job is described by a `BackgroundJobSpec`, journaled at submit time:

- `job_id` — minted by the runner (unguessable, like a `StandingEffect` handle; 007 §3.4 "the registry
  is the source of truth").
- `tool_name`, `arguments` (JSON), `idempotency_key` — derived per 019 §3 from the submitting call.
- `effect_class` — **read from the `ToolDescriptor` at submit time**, never from the arguments.
- `owner` — `{kind: host | session, session_id, run_id, principal_id}`.
- `grant_ref` — **a reference** to the capability grant (e.g. the session id / host grant name), never
  a serialized capability or handle. 019 §1 forbids serializing live handles.
- `retry` — a `RetryPolicy` snapshot.

**I2/I3 rule, stated as a claim (§6 claim 8):** nothing in the persisted record can widen what the job
may do. On resume the runner asks the host to re-derive the capability set from `grant_ref` under
**current** policy (019 §8 Q1). A record edited on disk to name extra capabilities, or a different
tool, grants nothing: capabilities come from the host callback, and the tool must still exist in the
host-supplied `ToolTable` with the same `effect_class`.

### 3.2 Lifecycle and the one state machine

```
queued ─► running ─► succeeded
   │         │ ├──► failed            (non-retryable, or retries exhausted)
   │         │ ├──► quarantined       (019 §4 poison: attempts exhausted across restarts)
   │         │ └──► retry_wait ─► running
   │         └──► cancel_requested ─► canceled        (the worker observed the stop and exited)
   │                               └► succeeded/failed (it finished before observing the stop)
   └──► canceled (never started)
running (at_most_once, intent journaled, no outcome, process died) ─► indeterminate (resume only)
```

Every transition is one append to the job's log (`bgjob/<job_id>`) **before** it takes effect, plus an
append of `job_id` to one index log (`bgjobs`) at submit — `AppendLogStore` has no "list logs", so the
index is how recovery finds jobs. Terminal states are never left.

### 3.3 Cancel — real, per-job, and honest about non-cooperative tools

- Each job owns its own `std::stop_source`. The worker runs the tool with
  `ctx.cancellation = job.stop_source.get_token()` — **not** the originating run's token. This fixes the
  "wrong token" row in §0.
- **Run cancel vs. job cancel — superseded by §8 Q1 (owner: cascade).** Cancelling the run that
  started a job cancels the job; the job keeps its own token, so cancelling the job never cancels the
  run. Deleting the owning session also cancels its jobs.
- `cancel(job_id, caller_principal)`: same cross-principal denial as today
  (`standing_effect.cross_principal_denied`). It requests stop and moves the job to `cancel_requested`.
  **The job keeps counting against `Background<max_concurrent>` and the pool until its worker actually
  exits.** This closes the §0 cap bypass.
- A cancel during `retry_wait` interrupts the wait immediately (the wait is on the stop token, not a
  sleep).
- A tool that never checks its token cannot be stopped (no safe thread kill exists). The job then shows
  `cancel_requested` until the tool returns; its result is **recorded** (for audit, I4) but not
  delivered as success. The UI can see "cancel requested, still running". This is a named residual, not
  hidden (§7).

### 3.4 Cancel and transactions — what "undo" means, per effect class

Cancelling (or crashing) mid-job leaves whatever the tool already did. What the engine guarantees
depends on the declared `effect_class` — the same classes 019 §3 already defines:

| effect_class | On cancel | On crash, at resume |
|---|---|---|
| `pure` | Discard. Nothing to undo. | Re-run from the last checkpoint (or from the start). |
| `idempotent` | Stop at the next check. Partial effects stay, and are safe: re-running under the **same** idempotency key converges. | Resume from the last checkpoint under the same key. |
| `at_most_once` | Stop at the next check. If an intent was journaled with no outcome, the job ends `indeterminate` — never "canceled cleanly", because the engine does not know whether the effect happened. | Intent without outcome → **`indeterminate`**, surfaced to the host/human, **never auto-retried** (019 §3, §7 G6). A re-run needs an explicit, recorded operator acknowledgement (019 §6). |

For effects that **can** be made atomic, the runner offers one transaction helper rather than a
general transaction system: **staged worktree publish.** A job writes its outputs as content-addressed
objects (which are invisible until referenced) and publishes them with a single `commit_ref()` of the
final tree at the end. Cancel or crash before that commit leaves the ref exactly as it was; the staged
objects are unreferenced garbage. This is what makes RAG index writes all-or-nothing per publish
step.

**Named dependency, not solved here:** `commit_ref()` has no compare-and-set (ADR-063 §4 finding 5).
Two jobs publishing to the same ref can still lose one writer's update. The helper refuses to run two
jobs against the same ref concurrently **within one runner** (a per-ref lock); across processes it is
the same open residual as everywhere else in the codebase (§7).

### 3.5 Checkpoints and progress

The tool gets two more things on `EffectContext`, both owned by the runner (never by a session, so no
foreign-thread call into a session — the ADR-060 §4 hazard):

- `report_progress(ContentItem)` → stored as the job's latest progress, and fanned out to the owner:
  host callback, or the session's queue via `weak_ptr` (drained under `session_mutex_` like completions,
  I1). Rate-limited by the runner (latest-wins), so a chatty tool cannot flood the event stream.
- `checkpoint(json::Value)` → one durable append to the job's log. On resume the tool receives the
  last checkpoint. **Order rule for tools:** commit the effect, then checkpoint. With `idempotent`
  effects a crash between the two re-does the last step harmlessly.

Only tools that declare **`Resumable`** (new policy tag, sibling of `Backgroundable`) receive a
checkpoint on resume; others restart from the beginning (fine for `pure`/`idempotent`, and never
happens automatically for `at_most_once`, per §3.4).

### 3.6 Retry policy

- Per-job `RetryPolicy` (the existing struct: `max_attempts`, exponential backoff with jitter). Default
  for jobs: `max_attempts = 1` (no retry) unless the submitter sets one — retry is opt-in.
- **Retried only when all hold:** the failure is `failure_class::transient` (the supervisor's
  `is_retryable()` rule); the job is not cancel-requested; and `effect_class != at_most_once`.
  `at_most_once` is **never** auto-retried, whatever the policy says.
- Every attempt reuses the **same** idempotency key and is journaled (I5: the jitter drawn is recorded,
  so replay reproduces the schedule).
- **Attempts survive restarts** (they are in the log). A job that keeps crashing the process counts
  each crash as an attempt; after `max_attempts` it becomes `quarantined` with its last error and state
  kept for inspection (019 §4, §7 G5) — not retried forever, not discarded.

### 3.7 Recovery at startup

`runner.recover(ToolTable const&, GrantResolver const&, ApprovalDecider const&)` — host-called once:

1. Read `bgjobs`, then each non-terminal job's log.
2. Re-resolve: the tool must exist in the table with the same `effect_class`, and `GrantResolver`
   (host code) returns the **current** capability set for `grant_ref`. Missing tool or no grant →
   `failed` with a stable code. Narrowed grant → run with the narrower set and emit a `Warning` (019 §8
   Q1).
3. `at_most_once` with an unconfirmed intent → `indeterminate` (never re-run).
4. **Re-approval (decision, owner may overrule — §8 Q2):** a tool whose `approval != never_require` is
   re-approved through the `ApprovalDecider` on resume. An approval is for "run this now", not "run
   this whenever the process next starts".
5. Everything else re-queues; a `Resumable` tool gets its last checkpoint.

### 3.8 Threads, budgets, shutdown

- A fixed pool of `std::jthread` workers owned by the runner (size set by the host). Jobs beyond the
  pool wait in `queued`. `Background<max_concurrent>` still bounds each session's live jobs; the pool
  bounds the process.
- `shutdown()` requests stop on every running job and joins workers, with a host-set deadline; a
  worker stuck in a non-cooperative tool past the deadline is reported (and left to process exit) —
  it is not killed.
- A session with no runner wired keeps today's detached-thread behavior **only if** the host opts in;
  the default is fail-closed (`standing_effect.no_runner`), so the unbounded path is not silently kept.
  (Decision for the owner — §8 Q3.)

### 3.9 Model-facing tools (last phase)

006 §6b's `background_task(tool, args)`, `list_standing_effects()`, `cancel_standing_effect(handle)` as
agent-callable tools, gated by `Background<max_concurrent>`. Arguments are model output (I3): they
choose *which already-granted* tool to background, never widen anything. Built last, after the runner
is proven, because it is what makes §0's cap bypass model-reachable.

## 4. What this deliberately does not do

- No preemptive kill of a running tool. Cooperative only, like ADR-178.
- No distributed/multi-process job queue. One process, one runner (019 §4: node loss is an accepted gap).
- No general distributed transaction. The one atomic helper is the staged worktree publish (§3.4).
- No compare-and-set on `commit_ref()` (named dependency, §3.4/§7).
- No compensation ("undo") actions for external effects. Idempotency keys + `indeterminate` are the
  019 answer; compensation is a future ADR if a real tool needs it.

## 5. Build phases

1. **Runner core, in memory:** pool, lifecycle, per-job stop source, real cancel, cap counts live
   workers, progress channel, shutdown/join. Fix the §0 cap bypass (positive control first).
2. **Durability:** job log + index log, recovery, `GrantResolver`, re-approval, `indeterminate`.
3. **Retry + poison quarantine**, attempts persisted.
4. **`Resumable` + checkpoints; staged worktree publish helper.**
5. **`AgentSession::start_background_task()` moves onto the runner** (existing tests keep passing).
6. **Model-facing tools** (§3.9).

## 6. Falsifiable claims (each with a disproving test; positive control where noted)

1. **Cancel stops cooperative work.** A tool that loops on `ctx.cancellation` and increments an
   external counter stops within one iteration of cancel; the job ends `canceled`; the counter stops.
   *Positive control:* the same test against today's `cancel_standing_effect()` sees the counter keep
   rising.
2. **The concurrency cap cannot be bypassed by cancel.** With `Background<1>`, a start→cancel→start
   loop against a non-cooperative tool never has more than one live worker. *Positive control:* today's
   code runs N concurrently (demonstrate before fixing).
3. **Run cancel cascades one way (§8 Q1).** Cancelling the originating run stops its jobs; cancelling a
   job does not cancel the run; a run that finishes normally leaves its jobs running; cancelling an
   earlier run never reaches a later run's jobs.
4. **Cancel interrupts a retry backoff** well before the backoff delay elapses.
5. **Restart resumes, not restarts.** Destroy the runner mid-job (simulated crash: no terminal append),
   build a new one on the same store, `recover()`: a `Resumable` `idempotent` job continues from its
   last checkpoint; an external per-step counter reads exactly 1 per step.
6. **`at_most_once` interrupted between intent and outcome is `indeterminate`**, never re-run by
   `recover()` or by the retry policy, across many trials with the crash injected at that point.
7. **Retry is bounded and class-gated.** Transient failures retry up to `max_attempts` with the same
   idempotency key; `contract`/`policy` failures never retry; `at_most_once` never retries; a job that
   crashes the process every attempt is `quarantined` after `max_attempts` restarts, state intact.
8. **The record carries no authority.** A job log edited to name a different tool, a wider capability,
   or a different `effect_class` resumes with exactly what `GrantResolver` + the `ToolTable` say, or
   fails closed. Policy narrowed between runs → narrower set + `Warning`.
9. **Progress without a use-after-free.** A job reporting progress while its owning session is
   destroyed: no crash under ASan; progress for a gone session is dropped.
10. **Staged publish is atomic.** Cancel or crash before publish leaves the ref unchanged; after
    publish the ref points at the complete tree.
11. **Shutdown joins.** After `shutdown()` returns, no worker thread is running a cooperative tool.

## 7. Residuals (named now)

- Non-cooperative tools cannot be stopped; cancel is best-effort for them (§3.3).
- `commit_ref()` has no compare-and-set; concurrent publishers across processes can lose an update.
- No tool versioning: resuming after a deploy runs the **current** tool code under the old job's
  arguments. 019 §4 says the default should be "pin"; there is no tool version to pin to yet.
- Jitter recorded for replay (I5), but a wall-clock backoff across a restart restarts the delay.
- One runner per process; no cross-process fencing (two processes recovering the same store would both
  resume the same job). Hosts must run one recovering process per store.

## 8. Project-owner decisions (2026-09-23)

- **Q1 — Does cancelling a run cancel the jobs it started? → Yes, cascade.** (Overrules the
  recommendation.) Cancelling a run cancels every background job that run started. A run that simply
  *finishes* does not cancel its jobs — they still outlive the turn, which is 006 §6b's point. This also
  matches 006 §6b's "scoped to the run that created it", which the original §3.3 contradicted (red-team
  gap 8). Mechanism: the run's `stop_source` gets a `std::stop_callback` per job that requests the job's
  own stop, so the job still has its **own** token (a job cancel never cancels the run), and a stale
  cancel of an earlier run cannot reach a later run's jobs (ADR-178's per-run source).
- **Q2 — Re-approve approval-gated tools on resume after a restart? → Yes.**
- **Q3 — A session with no runner wired? → Fail closed** (`standing_effect.no_runner`); the old
  detached-thread path is removed, not kept behind a flag.

## 9. Decision

Pending implementation and proof.

## 10. Red-team pass 1 (2026-09-23, `general-purpose` agent, no prior context) and the revisions

The pass checked §0 and §3 against the source. It found 6 Critical findings, 14 Real gaps and several
Minor ones. The Critical findings and gaps 1, 4 and 9 were re-verified by hand against the source
before being accepted. **§0's table held up** for the cap bypass, the inherited run token, the
`report_progress` reset, the detached unjoined threads, and `effect_class` coming from the descriptor.
Two §0 statements were wrong and are corrected here:

- Handle ids are not unguessable: they are `session_id + ":standing:" + ++counter`, and
  `StandingEffectRegistry::reset()` sets the counter back to 0.
- `commit_ref_impl` appends to the journal log directly; only `tests/rt/test_rt_effect_journal.cpp` calls
  `journal_effect_*`.

Each finding and its disposition:

### Critical

- **C1 — the idempotency key never reaches a tool.** `EffectContext` has no key field. The key is
  derived only for audit. → **Fixed in design:** `EffectContext` gains `idempotency_key` and `attempt`,
  set by the runner. `idempotent` becomes an explicit **tool contract**: the tool must dedup its external
  effect on the key. Claim 5 uses a key-deduping tool, and a positive control shows that a tool
  ignoring the key double-applies.
- **C2 — the journal pairs intent and outcome by key only.** Once retries reuse the key, an attempt-2
  intent after an attempt-1 outcome reads as "confirmed". One corrupt record also fails the whole read.
  → **Fixed in design:** the runner keeps its own per-job log with `{attempt, phase}` records.
  "Unconfirmed" means the **last** attempt's intent has no outcome. A corrupt record is flagged
  (`corrupt_record`) instead of failing the read. The session-scoped `effect_journal_log_id` is not
  reused for jobs.
- **C3 — `FileAppendLogStore` corrupts every record after a torn write (a pre-existing bug, verified
  at `rt/append_log_store.hpp:151-217`).** `read_from()` stops at a torn tail, but `append()` opens
  with `ios::app` and writes **after** the torn bytes. On the next read the torn header's length
  swallows the new record, so framing is lost from then on. The file comment says "every PRIOR record
  stays intact", which is true, but it silently omits that every LATER record is lost. Also: `flush()`
  is not `fsync`, there is no checksum, and every `append()` re-reads the whole log (O(n²)). This
  affects today's users of the store (workflow checkpoints, project archive), not only this ADR.
  → **Its own fix, done first, before phase 2:** reproduce with a test, then truncate a torn tail
  before appending, add a per-record CRC, add an opt-in real `fsync`/`FlushFileBuffers`, and cache the
  sequence number.
- **C4 — `bgjob/<id>` is rejected by `path_for()` (`/`), and `:` is an NTFS alternate-data-stream
  separator.** → Job ids are 32 hex characters from a CSPRNG. Log ids are `bgjob_<hex>` and
  `bgjobs_index`. The same `:` exposure in `effect_journal_log_id` is named as a separate residual.
- **C5 — resuming from `grant_ref = session id` can widen authority (I2).**
  `start_background_task()` runs under a per-request `RequestAuthority` (its own principal,
  capabilities and expiry), which can be narrower than the session's own grant. Expiry is checked only
  at admission. → **Fixed in design:** the record stores the effective capability set as a
  **ceiling** (data that can only narrow), plus the principal id, the tenant and the authority's expiry.
  On resume: effective = `GrantResolver(current policy)` ∩ recorded ceiling. Editing the ceiling to
  widen it gains nothing, because the resolver bounds it; editing it to narrow it is harmless. A job past
  its authority's expiry is not resumed (`failed: authority_expired`). A running job whose authority
  expires gets a stop request at the expiry time.
- **C6 — borrowed pointers become use-after-free once jobs outlive sessions.** `set_capabilities()`
  wraps a **non-owning** `shared_ptr` (`agent_session.hpp:620-623`). The comment at
  `tool_pipeline.hpp:661` calls that pointer "owned", which is wrong for session-level capabilities.
  The worker also holds a raw `ToolDescriptor const*`. → **Fixed in design:** the runner copies the
  `CapabilitySet` by value and holds the descriptor by value (`ToolDescriptor` is copyable). The job's
  `EffectContext` is built **fresh from an allowlist** (principal, capabilities, run/job ids,
  idempotency key, the job's own cancellation and progress, and no deadline unless the job sets one)
  instead of copying the session's context and blanking fields. Today's blocklist misses `blob_sink`,
  `agent_turn_sink`, `moderator_delta_sink` and `deadline`. The misleading `tool_pipeline.hpp:661`
  comment gets corrected.

### Real gaps

1. **Guessable, reusable handle ids.** After `reset()` a stale completion can match a reused id: it
   delivers the wrong result and erases a live entry, which is a second cap bypass. → CSPRNG ids
   (C4). Completions are matched by job id plus a generation number.
2. **The background path skips the approval policy (I3).** It does its own step 5 instead of
   `admit_call()`/`resolve_approval_outcome()`, so `text_derived` provenance and the ADR-070
   `PolicyDecider` are ignored. → Admission goes through `admit_call()`. Provenance and
   `arguments_tainted` are persisted. On resume they are read as the most restrictive value if the
   record is missing or unreadable.
3. **Crashes counted as retry attempts, with a default of 1, quarantine every crashed job.** A poison
   job crashing the process also charges every innocent job. → Two separate counters:
   `max_attempts` (in-process transient retries, default 1) and `max_resumes` (restarts, default 3).
   `attempt_started` is journaled. A crash counts only against jobs whose last record is
   `attempt_started`. When several jobs were running at the crash, each is charged. That can still
   charge an innocent job, so it is named as a residual. The alternative is not knowing which job
   crashed.
4. **`is_retryable()` is `transient || resource`, not transient only, and jitter cannot be
   recorded.** → Jobs retry `transient` only by default; retrying `resource` is opt-in per job. The
   runner takes an injected jitter source and journals the chosen delay (I5).
5. **"Finished after cancel" was contradictory, and a cooperative `at_most_once` cancel can leave
   partial effects.** → A job that finishes before it observes the stop ends `succeeded` with a
   `cancel_too_late` flag. It is delivered as a success, because hiding a real `at_most_once` success
   invites a redo. An `at_most_once` tool that stops on cancel must report `effect_performed:
   none | partial | unknown`. Anything but `none` ends the job `indeterminate`.
6. **`std::jthread` has no timed join, so shutdown with a deadline hangs or leaves a
   use-after-free.** → The runner's core (queue, mutex, store reference, state) lives in a
   `shared_ptr` that every worker holds. `shutdown(deadline)` requests stop, waits on a
   per-worker "done" condition until the deadline, joins the finished workers, and detaches
   stragglers. Detached stragglers still hold the core, so there is no use-after-free. Their results
   are journaled if the store is still alive. Stragglers are reported to the host.
7. **Non-cooperative tools can hold the pool and the session's `Background<N>` forever.** → Named
   as a denial-of-service residual (§7). Backoff waits sit in a runner timer queue, not on a worker
   thread.
8. **Spec requirements silently dropped.** → `StateChanged` is emitted on register, resolve and
   cancel (006 §6b). A "Local background task completion" wake condition (019 §2) is added for session
   jobs. Run scoping is settled by §8 Q1 (cascade). Job usage is charged to the owning session's
   accounting (006 §3 step 10). How exactly is a phase-5 design item; if it can't be done in phase 5,
   it is named as a residual.
9. **Completions reach no model context.** `drain_background_completions_locked()` only emits an
   event. → In phase 5, a completion is appended to session history as a tool-result message for its
   `call_id`, keeping the tool output's taint, so the next run's model sees it. This must be done
   before the model-facing tools (phase 6), or those tools would be pointless.
10. **A recovered session job can never deliver.** After a restart there is no `weak_ptr` queue. →
    The job log doubles as a **durable completion outbox**. When a session loads, it asks the runner
    for undelivered completions for its id, appends them, and the runner journals `delivered`.
11. **I1 is already broken: `cancel_standing_effect()`/`list_standing_effects()` are unlocked,
    and `list` returns a `const&`.** → Both become locked and return by value. `delete_session()`,
    `clear_in_process_state()` and `fork_from()` are hooked to the runner: delete and clear cancel the
    session's jobs, and a fork copies no jobs.
12. **Cross-tenant cancel on a host-wide runner.** `cancel()` compares the principal id only. →
    Compare `{principal id, tenant id}`, and filter listings by owner.
13. **Progress through the session queue is not live.** The queue is drained only at run start and
    at interaction resolve. → Progress goes to a runner-owned, thread-safe event sink that the host
    wires (the RAG UI reads that). It never goes through the session's `emit_run_event`, which avoids
    the ADR-060 §4 hazard.
14. **`ApprovalDecider` is a synchronous `bool`, so no human is in the loop at startup.** → On
    recovery the runner asks the host's decider. If it is denied or absent, the job waits in a new,
    visible `awaiting_approval` state. The host then calls `approve(job_id)` or `cancel(job_id)`.
    In-process retries do not re-approve, because they are still inside the same approval.

### Minor (all accepted)

- Submit order: the job log is written first, then the index. A log with no index entry is invisible,
  harmless garbage. An index entry with no log is reported as `failed: corrupt_record`. Index
  compaction is a residual.
- On load, the argument digest is recomputed and compared with the stored idempotency key. A
  mismatch is `failed: corrupt_record`.
- Jobs do not inherit the run's `deadline` (covered by C6's allowlist).
- `Background<N>` is per session registry, so one grant set shared by several sessions allows N per
  session. The runner counts per `{session}`, as today, and adds a pool-wide bound. Named, not changed.

### What the pass confirmed

- The cap bypass is real. The exact path: start (count 0 < 1) → cancel (erase) → start (count 0).
- The run token is inherited, and a `session.cancel()` **between** runs still stops a task, because
  it holds the last run's source.
- Rejecting design 2C was fair. The reuse claim was corrected (gap 4).

### Revised build phases (replaces §5)

0. **`FileAppendLogStore` torn-write fix (C3)**, with a reproducing test first. A standalone commit,
   because it fixes existing users too.
1. **Runner core, in memory.** CSPRNG ids, allowlist `EffectContext` with key and attempt,
   per-job stop source with the run cascade (§8 Q1), cap counts live workers (positive control first),
   runner-owned progress sink, `shared_ptr` core with deadline shutdown, cancel checking `{principal,
   tenant}`, `StateChanged`.
2. **Durability.** Per-job log (attempt-aware), index log, ceiling ∩ resolver re-grant, expiry,
   `awaiting_approval`, `indeterminate`, completion outbox, digest check.
3. **Retry** (transient only by default, injected jitter journaled) and **resume/poison**
   (`max_resumes`).
4. **`Resumable` + checkpoints**, and the staged worktree publish helper.
5. **Move `AgentSession` onto the runner.** Route admission through `admit_call()`, lock
   `cancel`/`list`, hook delete/clear/fork, inject completions into history, add the wake condition and
   the accounting, fail closed with no runner (§8 Q3).
6. **Model-facing tools.**

## 11. Phase 0 — done (2026-09-23): `FileAppendLogStore` torn-write fix

- **Reproduced first.** New checks L8a–c in `tests/rt/test_rt_append_log_store.cpp` failed 4 times against
  the unfixed store. `append()` returned seq 3 for a record that was then unreadable (L8a), or was read
  back misframed (L8b, L8c).
- **Fix.** One framing parser, `append_log_store_detail::scan_log()`, is shared by `read_from()`,
  `append()` and `last_seq()`, so they can no longer disagree about where the valid log ends.
  - `append()` truncates a torn tail before writing.
  - New files carry an 8-byte magic (`AELOGv2\n`) and a CRC-32 per record. A full-length final record
    with a bad CRC is treated as torn. A bad CRC before the end is `rt.append_log_store.corrupt_record`
    (`fatal`), which `read_from()` returns instead of hiding what follows, and which `append()` refuses
    to write past.
  - Logs in the original (v1) format are still read, and appended to in v1 framing.
  - New option `append_log_sync::disk` (`_commit`/`fsync` on every append); the default stays
    OS-buffer flush.
  - Appends are serialized by a process-wide mutex. Truncation is unsafe with another append to the
    same file in flight, so the store is now documented as **single writer process per root**.
    Cross-process appends could already hand out duplicate seq numbers before this change.
- **Proof.** All checks pass (L1–L12). Positive controls: a mutant that skips the truncation fails 6
  checks (L8a–c, L9, L11); a mutant that treats mid-file corruption as a torn tail fails the 3 L10
  checks. Neither the fix nor its tests produce a warning under the project's warnings-as-errors build.
- **Residuals.** Every append still re-reads the whole file, so the total write cost of a log grows
  quadratically (unchanged from before). The directory entry of a newly created log is not synced.
  Cross-process writers are unsupported.

**Follow-up found in phase 1:** the phase 0 commit wrote `std::numeric_limits<std::uint32_t>::max()`,
which breaks any file that includes `<windows.h>` without `NOMINMAX`, because `max` is a macro there.
Configure-time check ADR-096 C2 compiles exactly such a file, so the next CMake reconfigure failed. The
phase 0 full build had passed only because CMake did not reconfigure. Fixed with
`(std::numeric_limits<std::uint32_t>::max)()`.

## 12. Phase 1 — done (2026-09-23): the in-memory runner

`include/agentengine/rt/background_job_runner.hpp`, `tests/rt/test_rt_background_job_runner.cpp` (R1–R17).

- **What it is.** A bounded pool of worker threads the runner owns. Every job has its own
  `std::stop_source`. The run cascade (§8 Q1) is a `std::stop_callback` on the submitting run's token.
  `Background<max_concurrent>` counts every non-terminal job of an owner, including
  `cancel_requested`. Capabilities and the tool descriptor are copied into the job. The tool's
  `EffectContext` is built from an allowlist, and `EffectContext` gained `idempotency_key` and
  `attempt` (C1). Job ids come from the system CSPRNG. `cancel()`/`list()` check principal and tenant.
  Progress and state changes go to a host sink, never with the runner's lock held, and each event
  carries a per-job sequence number. `shutdown(deadline)` joins exited workers and detaches the rest,
  disables the sink, then waits for in-flight sink calls, so the sink is never called after it returns.
- **Terminal states (§10 gap 5).** A success after a cancel is `succeeded` with `cancel_too_late`. When
  a stop was requested and the tool failed:
  - it returned `tool.canceled_no_effect` → `canceled`;
  - it is `at_most_once` → `indeterminate`;
  - otherwise → `canceled`.
- **Approval.** The runner does not run step 5; it belongs to the submitter's admission path (phase 5
  routes it through `admit_call()`). To fail closed, a tool that is not `never_require` is refused
  unless the submitter sets `approval_attested`.
- **Lifetime details handled.**
  - `std::stop_callback`'s destructor blocks while its callback runs, and the callback takes the
    runner's lock, so a finished job's callback is always moved out and destroyed after unlocking.
  - The cascade callback may destroy its own `stop_callback` from inside itself; the standard allows
    that on the same thread, so the callback copies its captures to locals first.
  - Code holds its own `shared_ptr` to a job before finishing it, because finishing can evict it
    from the table.
- **Proof.**
  - 67/67 checks pass, and 40 of 40 consecutive runs pass.
  - Under AddressSanitizer, 10 of 10 runs are clean, with `/W4 /WX`. The phase 0 log store test is also
    clean under AddressSanitizer.
  - Seven planted bugs, each caught by the check written for it:

    | Planted bug | Checks that fail |
    |---|---|
    | cap ignores `cancel_requested` jobs | 1 (R4) |
    | tool gets a default token instead of the job's | 9 (R2 …) |
    | `cancel()` does not request a stop | 10 (R2 …) |
    | cascade not registered | 2 (R5) |
    | tenant not compared | 1 (R6) |
    | bound handles not revoked | 1 (R13) |
    | sink not disabled at shutdown | 1 (R17) |
- **Positive control for the cap bypass, against today's `AgentSession`.** A scratch program with
  `Background<1>` and a tool that ignores cancellation ran `start_background_task()` →
  `cancel_standing_effect()` five times. Output: `accepted=5 peak_concurrent=5`. The runner's R4
  refuses the second job while the first is still running. The session itself keeps the bypass until
  phase 5 moves it onto the runner.
- **Residuals added by phase 1.**
  - The foreground `invoke_tool()` path does not set `EffectContext::idempotency_key`/`attempt` yet.
  - Jobs never promote an oversized result to a blob: there is no `blob_sink` in the allowlist. A large
    result is inlined; to be revisited with the phase 5 session wiring.
  - `approval_attested` is a submitter statement, not a check the runner can verify.
  - `shutdown()` waits for in-flight sink calls without a deadline, so a sink that blocks forever
    blocks shutdown.

A second red-team pass is owed once phases 0–2 are implemented. The design has changed a lot, and the
failure paths are where the Vulkan backend's Critical findings were (ADR-180 §4e).
