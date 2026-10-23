# ADR-239: Three §5.1 seams stay synchronous -- `SessionFactory`, `BatchableModelCall::Build`, `::Complete`

Status: **Proposed** (2026-10-16). Red-teamed once by an independent agent (section 6: 0 FATAL, 4 MAJOR, 4 MINOR,
all folded in). No code changes. Awaiting the owner's Judge sign-off.
Parent: ADR-237 §5.1 / §5.3 / §14.2 (#174). Amends ADR-237 §5.1's named list, and **amends ADR-235 §3.1's contract
for `build`** (section 2).

## 1. Problem

ADR-237 §5.1 lists, among the seams that "must become async", `SessionFactory` and `WorkflowSupervisor`'s batch
callables `Build`/`Call`/`Complete`. The list was written from research §7a's tables before anyone read every
conformer. The #174 triage (ADR-237 §14.2) read each one. For three of the four, a `task` shape would cure nothing:
what they must not do is block, and an `co_await` at their call sites would not change where they run or what they
hold. ADR-237 §5.3's test ("pure computations over in-memory values that no conformer can make remote") says they
belong on the synchronous list, provided their contracts actually say "no I/O". Today one of them only says
"deterministic". This ADR closes that gap rather than relying on it.

A triage table is not the place to amend a locked list. This ADR is.

## 2. Decision

ADR-237 §5.1's named list loses three entries, and §5.3 gains them, each with its reason and its contract:

| Seam | Shape (unchanged) | Why it stays synchronous |
|---|---|---|
| `rt::SessionFactory<C,S,H>` (`rt/multi_agent.hpp:90`) | `unique_ptr<AgentSession<C,S,H>>(Principal const&)` | **Construction only.** The factory builds a fresh, uniquely owned session object. What could need I/O happens later, on the session's own `task`-shaped methods. That covers resume from `SessionStore` (ADR-237 §14 step 47) and credential resolution, which the provider clients do per call (`co_await store_.resolve(api_key_ref_, ctx)`, `protocol/openai/chat_client.hpp:503,546`, `protocol/anthropic/chat_client.hpp:536,574`), never at construction. Its callers, `spawn` and `spawn_with_retry` (`rt/multi_agent.hpp:180,234`), are coroutines. **New contract term:** a factory injects the caller's `rt::Runtime` into the child's client (`ProviderReactorOptions`) instead of letting the client own one (section 5, residual 4). |
| `BatchableModelCall::Build` (`rt/workflow_supervisor.hpp:291`) | `result<ChatRequest>(Message const&, EffectContext&)` | **No I/O, by an amended contract.** ADR-235 §3.1 requires `build` to be "deterministic in its input". This ADR **adds "and performs no I/O"**. Determinism alone does not exclude a keyed retrieval or a template-file read. Retrieval belongs in an upstream executor that passes its result in the `Message`. The reason is where `build` runs (section 3): converting it to a `task` would not help. |
| `BatchableModelCall::Complete` (`rt/workflow_supervisor.hpp:295`) | `result<ExecutorOutcome>(Message const&, ChatResponse const&)` | **No capability.** It receives no `EffectContext`, so it is handed no authority. Any I/O would have to come from captured ambient authority, which is already an I2 violation in the conformer. It maps a response to an outcome, and runs where `build` runs (section 3). |

`BatchableModelCall::Call` is **not** narrowed. It is the synchronous model call and converts with ADR-237 Phase G.

**The rule these rows add to §5.3:** a seam whose call site holds a round-wide lock, or whose conformer is handed
no capability, gets a stated **no-I/O contract** and stays synchronous. A conformer that needs I/O violates the
contract, and the fix belongs in the conformer, not in an async seam.

## 3. Where `build` and `complete` run (the reason a `task` would not help)

`src/rt/workflow_supervisor_batch.cpp:23-24` states it: "host callbacks (`build`/`complete`) and backend calls
... run on the supervisor's thread under the run lock".

- `gather_batch_candidates` (called at `src/rt/workflow_supervisor.cpp:786`, before any `pool_.submit` at `:883`)
  calls `build` for every candidate in a loop (`workflow_supervisor_batch.cpp:236`).
- `resolve_batch_item` calls `finish` and then `complete` for each polled item inside `poll_batches` (`:522`).

Unlike a synchronous `Call`, which goes to the thread pool, these never leave the supervisor. They run serially,
under `run_mutex_`, so **a slow `build` or `complete` stalls the whole round**, not one node. That is a stronger
reason for "no I/O" than §5.3's single-strand argument. Making them `task`-shaped would let them suspend, but still
inside the same round and under the same lock, so the stall would remain. If the supervisor is resumed on a lane, a
`build` that calls `rt::block_on` throws `block_on_refused`. `guarded()` classifies that as transient, and the node
falls back to the synchronous path (`:237-241`): no crash, but a silent loss of batching.

`build` can also run more than once for one node:

- An item that fails after submission is rebuilt and sent synchronously in a later round (the `no_batch` delivery,
  `:564`).
- An unbatchable delivery reuses the request already built (`:222`).
- A `build` error makes the synchronous body call `build` again in the same round (`:238-241`).

So determinism is load-bearing on three paths, and an effectful `build` would repeat its effect.

## 4. Evidence (the gate)

This decision is falsifiable by inventory: it is wrong if any conformer of the three seams does I/O or blocks.
Inventory, by grep on 2026-10-16:

- **`Build` / `Complete`:** three conformers in total, all in tests
  (`tests/workflow/test_rt_workflow_batch_coalescing.cpp:120`, `tests/workflow/test_rt_workflow_batch_replay.cpp:99`,
  `tests/protocol/openai/test_openrouter_batch_live_e2e.cpp:163`). Every `build` builds a `ChatRequest` from
  captured strings, and none touches its `EffectContext`. The only side effect is a test counter
  (`++calls->build`, `test_rt_workflow_batch_coalescing.cpp:122`), which is in memory. Every `complete` wraps the
  response message.
- **`SessionFactory`:** every conformer is a lambda in `tests/rt/test_rt_multi_agent.cpp` (`:262`, `:284`, `:353`,
  `:576`, `:599`, `:622`) that `make_unique`s a session over a scripted client, sets its principal and returns it
  (`:353` returns null, to test the refusal).

No production conformer exists for any of the three. The gate is therefore re-run, not assumed, whenever one is
added: a production conformer that does I/O reopens this ADR.

## 5. Disclosed residuals

1. **Nothing structurally enforces "no I/O".** The terms are contracts. A `std::function` can capture a client or a
   store, for `Complete` as much as for `Build`. Option B (section 7) narrows `Build`'s *passed* authority. It does
   not prove purity.
2. **A blocking `build`/`complete` stalls the round** (section 3). The run lock makes that worse than it would be
   elsewhere, and it is the reason the contract term exists.
3. **The live e2e test and two test harnesses use the loop ADR-175 banned.** `while (!t.done()) t.resume();`
   appears at `test_openrouter_batch_live_e2e.cpp:64,172`, `test_rt_workflow_batch_coalescing.cpp:60` and
   `test_rt_workflow_batch_replay.cpp:59`. ADR-175 replaced it with `rt::block_on` because it can resume a parked
   frame twice. The `:172` site is a `Call`, so Phase G's `Call` conversion should fix it rather than copy it.
   The harness sites should move to `rt::block_on` independently.
4. **A factory that lets each child's client own a Runtime blocks the parent's strand.** `OpenAIChatClient` and
   `AnthropicChatClient` constructed without a Runtime create their own (`protocol/openai/chat_client.hpp:477-479`,
   `protocol/anthropic/chat_client.hpp:507`). That means a lane pool, offload workers, DNS workers and a reactor
   (`rt/runtime.hpp:96-99`), and the destructor joins them all. `multi_agent.hpp:76-77` makes the factory build the
   child's client. So a natural factory creates threads per child on the parent's turn, and `spawn()` joins them
   when the child goes out of scope. That is not network I/O, and a `task` factory would not avoid it. Section 2's
   contract term (inject the caller's Runtime) is the fix. It is documentation only until a test pins it.

## 6. Red-team (2026-10-16, independent agent, no prior context)

0 FATAL, 4 MAJOR, 4 MINOR. All are folded in above:

- **MAJOR 1:** `build`/`complete` run under the run lock, which the draft never said. Now section 3 and residual 2.
- **MAJOR 2:** the draft claimed "pure by ADR-235's contract", but that contract says only "deterministic". Now an
  explicit amendment (section 2).
- **MAJOR 3:** the draft called `Complete` "pure by type", which contradicts the draft's own §4 point that a
  closure can capture a client. Now worded as "no *passed* capability" (section 2, residual 1).
- **MAJOR 4:** Runtime-per-child construction cost was undisclosed. Now residual 4 plus a contract term.
- **MINOR:** the loop's line number was wrong and the harness sites were missing (residual 3); the rebuild path was
  half-described (section 3); the `spawn()` callers are now named; option B loses the cancellation token
  (section 7).

The reviewer confirmed: the cited definition lines, per-call credential resolution, no production conformers,
every test `build` ignoring its context, and that option B breaks nothing ADR-235 relies on.

## 7. Owner decision requested

- **A (recommended):** accept the three narrowings with their contract terms. ADR-237 §5.1/§5.3, ADR-235 §3.1 and
  §10's spec pass carry them.
- **B:** A, plus drop `EffectContext&` from `Build`. That touches three test lambdas, `build()` and `operator()`
  (`workflow_supervisor.hpp:303,308-311`) and the gather call (`workflow_supervisor_batch.cpp:236`). It removes
  `Build`'s passed authority, but also the run's cancellation token (`batch_context`, `:166-173`), so a long `build`
  could no longer notice cancellation. Under A's no-I/O term a `build` should never be long, which is why B is
  optional.
- **C:** reject; all four batch callables and `SessionFactory` convert in Phase G.
