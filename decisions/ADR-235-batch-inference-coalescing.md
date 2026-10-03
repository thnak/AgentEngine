# ADR-235: Sending concurrent single-shot model calls from one workflow round to a vendor batch job (OQ-20)

**Status: Judged (2026-10-02, project-owner sign-off).** Design revised once after an independent red-team (§5: 6 FATAL, 11 MAJOR, all resolved in the design below before any engine code was written). The project owner lifted OQ-20's "document only" direction on
2026-10-02 ("lift it, go through ADR and implement"). This ADR replaces the design in
`docs/planning/batch-inference-coalescing-design-draft.md`. Vendor facts are cited from
`docs/research/2026-10-02-batch-inference-provider-limits.md`.

## 1. The question

Most providers' batch APIs charge about half price. In return, results arrive asynchronously, anywhere from
minutes to many hours later (§2 of the research doc). A workflow round that fans out to N model-backed nodes
pays full price for N synchronous calls. The question is whether those N calls can be sent as one vendor batch
job instead, with the run suspended durably until results arrive, and three conditions holding:

- (a) no item can reach a different tenant's job, and no batch result can be forged by someone who learns an
  id (I2/I3, ADR-169);
- (b) a run that never opts in behaves exactly as it does today;
- (c) the run never claims a cost saving it did not get. If a call cannot be batched, it either runs
  synchronously or fails, depending on policy the host chose, and is never dropped silently.

**Facts every design must respect**, all from the research doc:

1. **No vendor runs a client-side tool loop inside a batch item.** xAI says so in its docs, and Bedrock rejects
   tools outright. A batched item is a single model call.
2. **Results come back in no guaranteed order.** Anthropic's `custom_id` must match `^[a-zA-Z0-9_-]{1,64}$`.
   The draft's `run_id:round:executor_id` breaks that rule, since `run_id_` is `graph_.id + ":run:" + N`
   (`workflow_supervisor.cpp:182`).
3. **Every provider has limits, and some have a minimum.** Bedrock requires at least 100 records.
   OpenAI, Azure and OpenRouter require one model per batch. OpenRouter rejects base64 images and silently
   drops parameters it does not recognize.
4. **Expiry behaves differently per provider.** Anthropic marks each unfinished item `expired` and does not
   bill it. OpenAI cancels the rest of the batch and keeps the partial output. Azure never expires a job.
5. **Batch is not eligible for zero data retention** at OpenAI or Anthropic.

## 2. Competing designs

**A. Workflow-round coalescing, with the engine polling.** A node declares it can be batched, and its body is
a structured model call. When the host has opted in, `execute()`'s gather step builds each eligible
delivery's `ChatRequest`, groups them by backend, and submits each group as a vendor job. The items are stored
durably in the run-state record and the round suspends. When the host calls `poll_batches()` (on a timer, or
after a vendor webhook), the engine polls the vendor through the same backend and checks each result against
its own item table. Resolved items are folded into the next `execute()` the way resolved ports already are.

**B. Workflow-round coalescing, with the host polling.** Same gather and submit, but each item becomes an
ordinary `OpenPort`. The host polls the vendor itself and answers each port with `resume_workflow(id,
assistant_message)`. This is the earlier draft's design, which reuses `resume_workflow()` without changes.

**C. Session-level `BatchingChatClient` decorator.** Wrap any `ChatClient`. Its `chat()` adds the request to
a process-wide queue that is flushed on a timer or a count, and blocks until the result arrives.

**D. Standalone batch tool only.** Keep `tools/batch_infer.cpp`, add no engine integration, and close OQ-20 as
"won't do".

### Steelman and red-team of each design

- **C is the most general,** since any agent gets batching with no graph changes. It is rejected on three
  counts:
  - The waiting call blocks for minutes to 24h inside a turn while holding the session executor. That breaks
    I1's liveness, and a process restart loses the wait, because nothing is durable.
  - A process-wide queue mixes requests from different sessions, and therefore different principals, into one
    vendor job. That is exactly the cross-tenant leak OQ-20 named.
  - An agent turn is a tool loop (fact 1), so in the common case a 5-round turn would take 5 batch windows.
- **D is the cheapest and safe.** It is rejected because the project owner asked for the feature, and because
  fan-out of single-shot nodes is a real, common shape (map-style classification, summarization, judging).
- **B has the smallest code change.** Its red-team findings:
  1. **Forgery.** Any admitted caller, the same principal that answers human-in-the-loop prompts, could answer
     a batch port with arbitrary text. The engine could not tell a vendor result from a human's answer, so
     content that should have come from the model arrives through a human answer channel, unchecked.
     ADR-169 checks who the caller is, not whether the content is genuine.
  2. **Wrong channel.** Batch items would show up in `open_interactions()` as if they were human prompts. Every
     existing host UI (AG-UI, A2A input-required, the test driver) would show them to a human.
  3. **Lost data.** The host would also have to parse vendor wire formats and map usage itself, which
     reinvents the provider clients outside the engine. Usage and reasoning metadata would be lost.
- **A is chosen.** Its own red-team pass is §5.

## 3. Decision (design A, revised after the red-team in §5)

### 3.0 The reframing that the red-team forced

The first draft added a third way for a run to suspend, but left every existing check written for two. Those
checks are `resume_workflow`'s `still_unresolved`, `continue_workflow`, `cancel()`, `run_is_live()`, the
`run_workflow()` reset, `finish()`, and the nested drivers. Each of them then went wrong in its own way
(§5 findings 1-6).

The revision does not patch each site separately. It adds one predicate and one terminal rule, and every
site uses them:

- **`awaiting_outside()`** is true while the run is waiting on something outside the engine: an unresolved
  port, a pending nested sub-workflow, or a pending batch item.
  - `execute()` is **never entered while it is true**. The four entry points refuse instead and return
    `suspended`: `resume_workflow` (whether a port or a nested resume resolved), `continue_workflow`, and
    `poll_batches`. This also fixes a pre-existing defect: `continue_workflow` on a restored run with
    unanswered ports used to fold each port's request payload as if it were the answer.
  - The prologue additionally skips any port where `!p.resolved`, as defence in depth.
  - `run_is_live()`, `cancel()` and the end-of-`execute()` suspend decision all read the same predicate.
- **Abandon on terminal.** `finish()` with any status other than `suspended` moves every pending batch item
  to the *abandoned* list. A run that has ended never keeps a live, paid job it can no longer use. Because
  the move is a `noexcept` swap, it is also safe inside `cancel()`.

### 3.1 Authoring surface

- **`workflow::Executor::batch` (`bool`, default `false`).**
  - It is part of `operator==`, and the YAML compiler parses it as `batch: true` (I6).
  - `validate_workflow` rejects it on any node kind other than `function` (`validate.batch_on_non_function`):
    - agent nodes run a tool loop (fact 1);
    - request ports and sub-workflows make no model call.
- **`rt::BatchableModelCall`, a fixed functor type** in the same style as `AgentExecutorBodyTag`. It holds:
  - `build`: `(Message const&, EffectContext&) -> result<ChatRequest>`. **Its contract: it is deterministic in
    its input**, because the synchronous fallback in a later round rebuilds the request;
  - `call`: `(ChatRequest const&, EffectContext&) -> result<ChatResponse>`;
  - `complete`: `(Message const& input, ChatResponse) -> result<ExecutorOutcome>`;
  - `backend`: `std::shared_ptr<BatchBackend>`.

  Its `operator()` runs `build`, then `call`, then `complete`. So as an `ExecutorBody` it is an ordinary,
  fully working synchronous function body, and **every fallback reuses the node's own body.**
- **`initialize()` checks the pairing.** The supervisor is `invalid` if any of these hold:
  - `batch == true` but the body is not a `BatchableModelCall`;
  - the body's `backend` is null;
  - the node is the designated stall reporter. A batch result is folded between rounds, where the stall
    valve does not run (§5 finding 16).
- **Batching is never automatic.** It also needs `enable_batch_coalescing(BatchPolicy)`, following ADR-070's
  delegated-decision seam: explicit host opt-in, off by default, no new authority, set by host code, audited.
  - That call **refuses** (returns an error) when the supervisor is nested (`nesting_depth_ > 0`).
  - `bind_sub_workflow()` refuses an inner supervisor that already has coalescing enabled.
  - The two wrapping drivers, `WorkflowChatClient` and `workflow_as_executor_body`'s `run_once`, check
    `inner.batch_coalescing_enabled()` on every call and fail with class `contract`.

  So **batch runs only at the top level**, in a run whose host drives `poll_batches()` itself (§5 findings 4,
  13). Proxying nested batch is future work, refused rather than mishandled. It is the same stance as
  `reject_request_ports`.

**`BatchPolicy` fields:**

| Field | Default | Meaning |
|---|---|---|
| `on_unbatchable` | `sync` | `sync` or `fail` |
| `on_item_failure` | `sync` | `sync` or `fail` |
| `min_group_size` | 1 | smallest group worth batching |
| `max_wait` | 25h | longest a job may stay pending |
| `max_poll_errors` | 5 | consecutive permanent poll errors before items fail closed |
| `poll_error_grace` | 2 min | a job younger than this is never failed closed by poll errors; they are reported, not counted (added after the live run, §6) |
| `now_ns` | `system_clock` | the clock, injectable (I5) |

The policy is host configuration, like the checkpoint hook. It is not checkpointed. A restored supervisor
needs `enable_batch_coalescing()` again before `poll_batches()` will act; without it, `poll_batches()` refuses
with `invalid` and the run stays suspended.

### 3.2 `BatchBackend`: a declared provider seam

The seam is defined in `core/batch_backend.hpp`:

- `group_key`
- `limits`, returning `{max_items, max_payload_bytes, min_items}`
- `admit(ChatRequest)`, returning the encoded size or a refusal
- `submit(items, ctx)`, returning a job id
- `poll(job, ctx)`, returning `BatchPoll{ended, items, detail}`
- `cancel`
- `release`

The contracts it states:

- **`admit()` fails toward synchronous.** It refuses anything the vendor would reject or silently degrade.
- **An ended poll is complete.** `BatchPoll::ended == true` means the item list is the complete, final result
  set. An item missing from an ended poll will never be returned.
- **Custom ids belong to the engine.** They are never invented or rewritten by the backend.
- **`group_key` names the vendor, endpoint, model and credential reference.** The OpenRouter backend
  includes the `SecretRef` name. The account itself is not fingerprinted (residual, §7).

Two backends ship with this ADR:

- **OpenRouter** (`/api/v1/batches`, GA). Its `admit()` takes text only: no tools, output schema, reasoning
  effort or media, because OpenRouter silently drops unknown parameters. It has no cancel endpoint, so
  `cancel()` returns an error. `release()` deletes the job's stored data once it is terminal.
- **`testing::ScriptedBatchBackend`**, for deterministic tests.

### 3.3 Gather (before dispatch) and submit (after the synchronous wave)

Inside `execute()`'s round, when coalescing is enabled:

1. **Select candidates.** Before the quarantine step, each `exec_deliveries` entry is a candidate when its
   node has `batch == true` and the delivery is not marked `no_batch`.
2. **Build and admit.** For each candidate:
   - Call `build(payload, ctx)` and then `backend->admit(request)`. `ctx` is **a copy** of `contexts_[idx]`
     with `cancellation` set, exactly as for synchronous dispatch.
   - Both calls are wrapped. A thrown exception becomes a `transient` error.
   - **If `build` fails,** the delivery stays synchronous, and its body reproduces the same error under the
     normal failure and retry policy.
   - **If `admit` fails,** the delivery is unbatchable.
3. **Group.** The group key is `(backend pointer, principal id, principal tenant, capability-set pointer)`.
   Items run under different principals or capability grants are **never** placed in one job. Each job is
   submitted under the context of its own items, which are all equal (§5 finding 9).
4. **Chunk.** Split each group evenly into `ceil(n / max_items)` chunks, then split further by bytes.
   - If any chunk is smaller than `max(min_items, policy.min_group_size)`, the whole group is unbatchable.
5. **Handle unbatchable deliveries.** They stay in `exec_deliveries`:
   - `sync`: each runs **through a one-off body that sends the request already built and admitted**:
     `call(request)`, then `complete`. It is not rebuilt, so the request sent is exactly the one checked.
     A `batch_fallback` event names the node and the reason.
   - `fail`: each gets a pre-set failed reply of class `contract`. Pre-set replies **never enter `todo`**,
     so they are never retried. The edge's failure policy routes them like quarantined deliveries.
6. **Run the round.** The synchronous wave and sub-workflow dispatch run exactly as before. Chunks still
   waiting for submission are not in `exec_deliveries`.
7. **Submit just before `++rounds_`.** Each chunk is submitted at this point, so the gap between a paid
   submit and that round's checkpoint is only the fold and routing code, not the whole synchronous wave
   (§5 finding 14).
   - **Custom ids** are `"i" + n`, where `n` is unique across the whole round.
   - **A failed submit** makes the chunk's deliveries unbatchable. Since the synchronous wave has already
     run, `sync` defers them to the next round: they are re-enqueued with `no_batch = true` and an event.
   - **A job id that is already present** in the item table is treated as a failed submit.
8. **Store the items and suspend.** Each submitted item becomes a pending `BatchItem` in `batch_items_`:

   | Field | Meaning |
   |---|---|
   | `executor_index` | the node |
   | `input` | the delivery's payload |
   | `group_key` | the backend's durable key |
   | `job_id` | the vendor job |
   | `custom_id` | the item's id within that job |
   | `submitted_at_ns` | when it was submitted |

   The end-of-round suspend decision then uses `awaiting_outside()`.

### 3.4 Durability, and resolution without trusting a record

- **Only pending items are persisted.** `RunStateRecord` gains `batch_items` (pending items only) and
  `abandoned_batches`. Both are optional when reading, so older records still load.
  - **Nothing resolved is ever written to a record.** Results are held in memory until `poll_batches()`
    folds the whole set in one step. A record therefore cannot carry forged outputs, and the record-editing
    attack from §5 finding 19 has nothing to edit.
  - **After a restore, items are pending again and are re-polled.** Vendors keep results for 29 days
    (Anthropic) or 30 days (OpenRouter).
- **`DeliveryRecord` gains an optional `no_batch` field.**
- **Batch items are not interactions.** They never appear in `open_interactions()` or
  `open_interaction_asks()`, and `resume_workflow()` cannot name them. They have no `Interaction` id at all.
  This avoids changing `interaction_reason` and its codec (§5 finding 18).
- **What the host sees instead:**
  - `WorkflowResult::pending_batches`, a list of `{job_id, group_key, item_count, submitted_at_ns}`;
  - an accessor of the same name.

### 3.5 `poll_batches(PollBatches{caller})`

`poll_batches` holds the run lock (I1) and runs `admit_caller()` first (ADR-169). If coalescing is not
enabled or `valid_` is false, it refuses with `invalid`. Then it proceeds in four steps:

1. **Abandoned jobs first, even on an ended or cancelled run.**
   - For each abandoned entry `{job_id, executor_index, group_key}`, if the node's current body backend has the
     same `group_key`, call `cancel(job)` and then `release(job)`, both best effort.
     - On a mismatch, skip that cancel and report it. A job id is never sent to a different backend.
   - Emit `batch_abandoned` for each and remove it from the list.
2. **Then the cancel check:** `refuse_if_cancelled()`. A run that has already ended answers with its
   terminal status and does nothing more, because terminal runs keep no pending items (§3.0).
3. **Poll each distinct pending job.**
   - **Backend check.** Check the `group_key`. On a mismatch, the job's items fail closed with class
     `contract`, and the new backend is never called.
   - **Poll.** Call `poll` with that group's context copy. The call is wrapped, and a throw becomes
     `transient`.
   - **Errors.**
     - A transient error leaves the items pending.
     - A non-transient error increments a consecutive-failure count, kept in memory only, unless the
       job is younger than `poll_error_grace`. At `max_poll_errors` the job's items fail closed (class
       `contract`, never re-run synchronously: the job may still be live and billed).
     - Each error emits `batch_poll_failed`.
   - **Results.** Each result is matched only by `(job_id, custom_id)` against the engine's own table.
     - Unknown, duplicate or already-matched results are ignored and counted
       (`unmatched_batch_results`).
     - `succeeded` calls `complete(input, response)`, wrapped. `errored`, `expired` and `canceled` follow
       `on_item_failure`.
   - **End of job.**
     - An `ended` poll resolves every remaining item of that job as `expired`.
     - A job pending longer than `max_wait` gets a best-effort `cancel`, and its items resolve as `expired`.
       That bound exists because Azure jobs never expire (fact 4).
4. **Fold or stay suspended.**
   - **Stay suspended** if any item is still pending, or a port or nested sub-workflow is unresolved
     (`awaiting_outside()` with the resolved items counted as done). Resolutions are cached in memory, and the
     next poll re-polls only jobs that have not resolved.
   - **Otherwise fold and continue.** Call `execute()`. Its prologue folds the resolved items **with the same
     treatment a round's own results get**:
     - an `executor_completed` event;
     - usage;
     - `record_partial` and `record_visit`;
     - output selection;
     - the merge-on-join hook for branch-mode nodes;
     - routing through `route_from` with `is_same_round_quarantine_echo` over the folded set.
   - **What each resolution becomes:**
     - a failed item becomes a failed reply routed by its edge policy, with `retry` treated as exhausted;
     - a `fallback` item becomes `Delivery{idx, input, no_batch = true}` in `next`.
   - **After folding,** `release(job)` is called best effort for each job whose items have all been folded.

### 3.6 Visibility (OQ-20's Q6)

There are five new `workflow_event_kind`s, appended at the end of the enum:

- `batch_submitted`: `{group_key, job_id, executor_ids}`
- `batch_fallback`: `{executor_id, reason}`
- `batch_item_resolved`: `{executor_id, job_id, status}`
- `batch_poll_failed`: `{job_id, detail}`
- `batch_abandoned`: `{job_id, cancelled}`

The new live state `executor_live_state::batch_pending` is shown for submitted nodes.

`WorkflowResult` gains:

- `pending_batches`
- `abandoned_batches`
- `unmatched_batch_results`
- `batch_poll_errors`

The test driver's "finished" check and its kind-name switch are updated to match.

### 3.7 Cancel and re-run

- **`cancel()` settles batch-only suspensions.** It settles the run when `awaiting_outside()` is true, so a
  suspension on batch items alone is settled too. `finish(cancelled)` moves the items to `abandoned_batches_`
  through a `noexcept` swap.
- **The abandoned list survives a new run.** `abandoned_batches_` is a supervisor member, not part of
  `state_`, so `run_workflow()` does not clear it. It is persisted.
- **`run_workflow()` while items are pending** first abandons them, with no silent reset.
- **Vendor cancellation** happens on the next `poll_batches()` (§3.5 step 1). `cancel()` is `noexcept` and
  makes no blocking HTTP calls.

### 3.8 Specs bound

| Spec | Change |
|---|---|
| 014 §1 | `Executor::batch` added |
| 014 §4 | Batch suspension is a third suspend source, behind the single `awaiting_outside()` predicate |
| 014 §5 | The checkpoint includes pending batch items and abandoned jobs, never resolved results |
| 015 §3 | `batch:` key added |
| 004 §8 Q1 | Narrowed: workflow batch rides a durable item table plus the checkpoint, not `StandingEffect`, which is in-memory only |
| OQ-20 | Moves to Resolved |

## 4. Falsifiable claims

Each negative claim has a positive control.

| # | Claim | Disproved if |
|---|---|---|
| C1 | No opt-in means no change: `batch: true` nodes without `enable_batch_coalescing()` produce the same result, partial outputs and event kinds as the same graph without the flag | the two runs differ |
| C2 | N eligible deliveries (N ≤ `max_items`) in one round to one backend under one context produce exactly one `submit` with N items, and the run suspends with `pending_batches` naming that job | the submit count, the item count or the status differs |
| C3 | Every submitted custom id matches `^[a-zA-Z0-9_-]{1,64}$` and is assigned by index alone (`"i"+n`) | any id fails the regex or is not of that form |
| C4 | A result with an unknown custom id or a repeated custom id changes nothing and is counted | an output changes, or the counter stays 0 |
| C5 | A batch item is not listed by `open_interactions()` and not resolvable through `resume_workflow()` | it is listed, or it is resolvable |
| C6 | Restore-then-poll gives the same output as an uninterrupted run, and `continue_workflow()` on a restored run with pending items returns `suspended` without moving | the outputs differ, or `continue_workflow` advances |
| C7 | A `group_key` mismatch after restore fails the item closed without calling the new backend's `poll` | `poll` is called on the new backend |
| C8 | `on_unbatchable=sync` below `min_items` sends the admitted request synchronously this round, with a `batch_fallback` event; `=fail` fails those nodes with class `contract` and never retries them | a call is dropped, batched, rebuilt or retried |
| C9 | `on_item_failure=sync`: an expired item runs synchronously exactly once and is not resubmitted for that delivery | it is resubmitted, or it runs twice |
| C10 | An `ended` poll with a missing item, or `max_wait` exceeded, resolves the item, so the run cannot hang | the run stays suspended |
| C11 | `cancel()` on a batch-only suspension settles the run `cancelled`, and the next `poll_batches()` calls `backend.cancel` for each abandoned job | the run is not settled, or `cancel` is never called |
| C12 | An unadmitted `poll_batches()` caller is refused before any backend call | the backend is called |
| C13 | Each of these makes the supervisor `invalid`: a `batch` node with a non-batchable body or a null backend, a `batch` node that is the stall reporter, `batch` on a non-function node | it runs |
| C14 | YAML `batch: true` compiles to the same `Executor` as the C++ form (I6) | the compiled executor differs |
| C15 | Live: OpenRouter submits a real 2-item job, polls it to completion, and folds both outputs into a completed run | it fails other than by exceeding the test's wait budget (that case is INCONCLUSIVE) |
| C16 | Answering the last port while batch items are pending keeps the run suspended, and `poll_batches()` never folds an unanswered port | the run moves, or a port's request payload is routed as its answer |
| C17 | A run ending in a terminal state while items are pending moves them to `abandoned_batches`, and a later `poll_batches()` does not move the run | an item survives a terminal status, or the run moves |
| C18 | Batching is refused when nested: `enable_batch_coalescing` on a nested supervisor errors, `bind_sub_workflow` refuses an enabled inner, and the two drivers refuse an enabled inner | any of them proceeds |
| C19 | Items under different principals are never submitted in one job | one job carries two principals |
| C20 | A throwing `build`, `admit`, `submit`, `poll` or `complete` is classified, not propagated | an exception escapes `execute()` or `poll_batches()` |
| C21 | A batch node feeding a multi-source `fan_in` target dispatches that target once, with every source's contribution, as on the synchronous path | the target runs twice, or runs early |
| C22 | Non-transient poll errors on a job younger than `poll_error_grace` are reported but never fail it closed; past the grace, `max_poll_errors` consecutive ones do | a young job fails closed, or an aged one never does |
| C23 | (added by ADR-236 §3.1) A cancel requested during a round's batch gather dispatches nothing and submits no job; the run ends `cancelled` with nothing pending or abandoned | a synchronous call runs, or a job is submitted |

## 5. Red-team (independent adversarial pass on the first draft, 2026-10-02)

An independent agent, prompted to break the design against the real code, reported 22 findings.

### FATAL findings

| # | Finding | Resolution |
|---|---|---|
| 1 | `poll_batches` → `execute()` with a human port still open folded the port's own request payload as its answer. The prologue never checks `p.resolved`, `workflow_supervisor.cpp:651-676`. This bypasses approval. | §3.0 |
| 2 | Answering the last port while batch items were pending ran `execute()` early. The run could complete with the jobs lost. | §3.0 |
| 3 | `continue_workflow` on a restored run did the same. This was pre-existing for ports. | §3.0 |
| 4 | A nested batch suspension appeared to humans as an interaction with an empty inner id, and any answer destroyed it. | §3.1: refused when nested |
| 5 | Terminal runs kept live, paid jobs, and a later poll revived a run that had already ended. | §3.0, §3.5 step 2 |
| 6 | `cancel()` did not settle a batch-only suspension. | §3.0, §3.7 |

### MAJOR findings

| # | Finding | Resolution |
|---|---|---|
| 7 | Abandoned jobs could never be cancelled, the list was lost on re-run, and there was no backend identity for the cancel. | §3.5 step 1, §3.7 |
| 8 | An allocating step ran inside a `noexcept` path. | a `noexcept` swap |
| 9 | A chunk was submitted under the first item's context, so other items borrowed its authority (I2/I4). | §3.3 step 3 |
| 10 | Callbacks had no exception guard, and the stored context was mutable. | §3.3, §3.5 |
| 11 | There was no bound on a batch wait, and poll errors were permanent. | `max_wait`, `max_poll_errors` |
| 12 | `group_key` had no credential identity. | `SecretRef` name added; account residual in §7 |
| 13 | `WorkflowChatClient` and `run_once` break on a batch suspension. | §3.1: refused |
| 14 | The crash window was wider than it needed to be. | §3.3 step 7 |
| 15 | `fail` used class `resource`, which is retryable. | `contract`, never in `todo` |
| 16 | The stall valve and the merge hook were skipped for folded items. | stall reporter rejected; merge hook run in the fold |
| 17 | Usage was lost. | counted at fold; abandoned spend is residual in §7 |

### MINOR findings

| # | Finding | Resolution |
|---|---|---|
| 18 | Interaction ids collided. | no interaction minted; custom ids are round-unique |
| 19 | A record carried resolved outputs that could be edited. | only pending items are persisted |
| 20 | The fold skipped the quarantine echo; ended polls were incomplete; chunks were unbalanced; the envelope bytes were not counted. | fold reuses the echo; ended means complete; chunks are even; the envelope is counted |
| 21 | The test driver checked "finished" wrongly, and folded items emitted no `executor_completed`. | §3.6, §3.5 |
| 22 | The policy was not checkpointed, the backend could be null, and `build` ran twice. | §3.1, admitted request reused in §3.3 step 5 |

The red-team's remarks on the claims were applied: C2 is qualified by `max_items`, C3 is restated, C5 now
covers the nested case by refusal (C18), C10 covers `max_wait`, C11 is made reachable, and C16-C21 are added.

## 6. Evidence

All runs on Windows, MSVC, 2026-10-02.

**Deterministic: `tests/workflow/test_rt_workflow_batch_coalescing.cpp`, 97/97 checks.** It covers
C1-C14 and C16-C22 against `ScriptedBatchBackend` (`include/agentengine/testing/scripted_batch_backend.hpp`),
with an injected clock for C10 and C22.

**Wire: `tests/protocol/openai/test_openrouter_batch_backend.cpp`, R1-R7 pass.** It runs against a
loopback scripted HTTP server and covers `admit()` refusals, body field order, id parsing, status
parsing (`cancelling` stays non-terminal), per-item error classes, the `DELETE` release, and cancel
refused as `contract`.

**Mutation positive controls: 10 of 10 killed.** Each mutant was planted from a scratchpad backup and
restored with `cp`, verified byte-identical with `cmp`.

| # | Mutant | Killed by |
|---|---|---|
| M1 | result matching ignores `custom_id` | C4 |
| M2 | no `group_key` check before polling | C7 |
| M3 | `poll_batches()` skips admission | C12 |
| M4 | no batchable-body check at `initialize()` | C13 |
| M5 | `awaiting_outside()` ignores batch items | C16, C6 |
| M6 | grouping ignores the principal | C19 |
| M7 | `finish()` keeps pending items on a terminal status | C17 |
| M8 | `continue_workflow()` drives a run awaiting the outside | C6 |
| M9 | `awaiting_outside()` ignores unresolved ports | C16 |
| M10 | `poll_error_grace` disabled | C22 (4 checks) |

**Live: `tests/protocol/openai/test_openrouter_batch_live_e2e.cpp` (C15)**, labelled `live-network-heavy`,
needs `AGENTENGINE_OPENROUTER_BATCH_API_KEY`.
- First run, `openai/gpt-4o-mini`: OpenRouter refused the submit, 400 "does not have a :batch endpoint",
  although `/api/v1/models` lists `openai/gpt-4o-mini:batch`. The engine did what §3.3 says. Both
  deliveries got `batch_fallback`, ran synchronously in the next round (`sync_calls=2`), and the run
  completed. That is a live proof of the `on_unbatchable = sync` path, not of C15.
- Second run, `google/gemini-2.5-flash-lite`: **PASS.** One job (`batch-1790923707-u3CyOSBwNRY3ECCC4OCF`) carried both nodes; the run suspended on it. Over 22 `poll_batches()` calls (about 10 minutes at 5/10/20s, then 30s, intervals) it stayed pending, then both items came back `succeeded`, matched by custom id (in the order `i1`, `i0`). The run completed with `sync_calls=0`, the `fan_in` join received both answers ("Paris is the capital of France. | Bright blue."), and the vendor's usage was counted.
- **Finding from the live run, fixed:** the first `GET` 5s after a successful submit returned 404 "Batch
  job ... not found"; the next found the job. Under the first build, `max_poll_errors` counted that 404.
  A host polling quickly would have failed a live, paid job closed over the vendor's own lag.
  `BatchPolicy::poll_error_grace` (default 2 min) now reports such errors without counting them (C22,
  M10).

**Per-claim verdicts:** C1-C14 and C16-C22 hold by the deterministic suite, each with its mutant or
control. C15 holds live, against OpenRouter.

## 7. Residuals

- **Direct backends are not built:** OpenAI (file upload), Anthropic (inline), Bedrock (S3), Gemini. No
  credential for them exists here, and code no live run can check is not shipped. Each is a new
  `BatchBackend` conformer with no engine change.
- **Crash between submit and the round's checkpoint.** The window is now only fold and routing, but it is
  not zero. Restoring the previous checkpoint resubmits, and the first job is paid for and orphaned. No
  vendor-side idempotency is assumed; Gemini documents that creation is not idempotent.
- **Account identity.** `group_key` carries the `SecretRef` *name*, not a fingerprint of the account. Two
  stores mapping one ref name to different accounts are not told apart; the vendor answers 404, which fails
  closed after `max_poll_errors`.
- **OpenRouter silently drops unknown parameters.** `admit()` is therefore text-only. Widening it needs live
  proof per parameter. **Left undone by project-owner decision (2026-10-03):** the owner found a blocker
  that this work cannot fix or get past here, so widening was not attempted and PR #165 merges without it.
  This is an open follow-up, not a closed one.
- **OpenRouter has no cancel endpoint.** An abandoned OpenRouter job runs to its terminal state. It is
  released (deleted) on a later poll only once it is terminal.
- **The spend of abandoned jobs is not known to the engine.** Usage is counted when results are folded.
- **The batch wait does not count toward `deadline_ms`,** which measures execution. `max_wait` is the bound
  for the wait.
- **Scope is one round of one top-level run.** There is no cross-run coalescing (rejected in §2 C) and no
  nested batch.
- **A model listed as batch-capable may still be refused.** OpenRouter lists `openai/gpt-4o-mini:batch` but
  refused it for this account (§6). The refusal costs one round of latency (`on_unbatchable = sync`)
  and nothing else; the engine does not try to predict it.
- ~~**`tools/batch_infer.cpp` still targets the beta path `/api/beta/batches`.**~~ **Closed by ADR-236 §4**
  (2026-10-03): the tool now drives this ADR's `OpenRouterBatchBackend` on the GA path.
- ~~**No recorded seam for replaying batch results (I5).**~~ **Closed by ADR-236** (2026-10-03):
  `BatchRecorder` / `BatchReplayer` record and replay every backend call and clock reading as one checked
  stream.
