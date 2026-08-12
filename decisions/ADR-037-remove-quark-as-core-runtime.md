# ADR-037 — Remove Quark as AgentEngine's core runtime; become a plain C++ SDK

**Status:** Proposed, design-only (2026-08-12). This is a Phase 0 design + red-team document — no
code has been written or migrated. Per this project's own governance
(`decisions/README.md`; `OpenQuestions.md` OQ-11), the project owner judges an ADR; this one in
particular cannot be "proven" the normal way (real code + tests) until Phases 1-4 (§7) each land and
are separately verified — this document is the design those phases follow, not a claim any of them
are done.

**Relates to:** `AgentEngineSpecification.md` (the "Quark mapping" section this ADR proposes to
retire — see §8); every ADR from 001 through 036 (their BEHAVIORAL claims — what a session/workflow
does — are intended to survive this migration unchanged; their PROOF ARTIFACTS — tests written
against `quark::TestKit`/`quark::Actor` — do not, and are named as a real, large cost in §5);
`third_party/quark/CLAUDE.md` (the locked "Quark is a submodule, never forked or patched in-tree"
rule this ADR does not violate — it proposes removing the dependency, not patching it).

## 1. The question

Should AgentEngine stop building its session/workflow/tool runtime on top of Quark's actor engine
(mailbox dispatch, `Sequential` policy, `quark::task<T>` coroutines, snapshot/persistence,
`ReplyStream`, `CircuitBreaker`, `TestKit`), and instead become a self-contained C++ SDK — in the
shape of Microsoft Agent Framework, the OpenAI Agents SDK, or Anthropic's own agent patterns, none of
which ship a custom actor/distributed runtime — with Quark reduced to, at most, an optional advanced
deployment target rather than the foundation everything is written against?

## 2. Why this is being asked now

The project owner's stated reasoning (this session, verbatim intent): an agent engine should provide
an API to build AI systems, the way MAF/OpenAI SDK/Anthropic SDK do — it should not carry a
distributed-actor-engine's own concerns baked into its foundation, since that clutters the
implementation and confuses the purpose of what an agent engine is for.

Two audits ran before this document was written (this session, both via independent `Explore`
agents reading the real source, not from memory):

- **Distribution/cluster audit**: AgentEngine's build **already has zero footprint** from Quark's
  cluster/transport/membership machinery — no `#include` of any `quark/net/*` or cluster header
  anywhere in `include/`, `src/`, `tests/`, `tools/`; no CMake flag enables it; Quark's own
  `CMakeLists.txt` compiles only `src/version.cpp`, so the cluster/net headers are unused template
  surface with zero compiled object code. AgentEngine's own RFCs (`AgentEngineSpecification.md:236`,
  `020-Configuration-and-Hosting.md` §"Cluster concerns") name this as **planned, deferred** future
  work (Milestone 7+), not accidental baggage.
- **Core coupling audit**: what IS deep is the LOCAL, single-process actor/concurrency substrate.
  `AgentSession` and `WorkflowSupervisor`/`FunctionExecutor` are not callers of Quark — they ARE
  Quark actors, via CRTP (`: public quark::Actor<Self, quark::Sequential>`). The 192-byte
  `MessagePool::kMaxPayload` cap bounds every `Ask<>` message's design; `core/stream.hpp`'s own
  backpressure scheme is shaped around a specific Quark ring-buffer limitation (no heap boxing), not
  just calling its API; persistence goes through `quark::EventLog`/`FenceToken`; ~42 of ~208 test
  files drive `quark::TestKit`/`quark::Engine` directly.

**Conclusion the audits force**: the thing the project owner is actually objecting to (distributed
complexity) is not present in the current build at all — there is nothing to trim there. The real
cost of this ADR is replacing Quark's LOCAL actor/mailbox/coroutine runtime, which currently gives
AgentEngine its core safety invariant (I1: one session, one executor) for free. This is confirmed,
not assumed — the project owner was told this directly and chose to proceed with full removal anyway
or, given the answer to that same question below, that owning this replacement is a deliberate
philosophical choice (an agent SDK provides business logic, not a runtime engine — even a local one).

## 3. What actually needs replacing (per-subsystem, from the coupling audit)

| Quark primitive | Where used | Role | Coupling depth |
|---|---|---|---|
| `quark::Actor<Self, Sequential>` (CRTP) | `AgentSession`, `WorkflowSupervisor`, `FunctionExecutor`, `SpawnCostBudgetActor` | Actor identity + mailbox dispatch + `Sequential` (I1's enforcement mechanism) | **Foundational** |
| `quark::Ask<MsgT, ReplyT>` / `Protocol<...>` | Same four types | Declares the message vocabulary Quark's dispatcher routes to `handle()` overloads | **Foundational** |
| `quark::task<T>` | Aliased as `ae::task<T>` almost everywhere | Coroutine return type for every async method | Shallow **API shape**, deep **semantics** (lazy start, no sync drive-to-completion by design) |
| `quark::ReplyStream<T*>` / `ReplyStreamProducer` | `core/stream.hpp` | Backpressured token/event streaming | **Deep** — the wrapper's own boxing scheme exists to work around a Quark ring-buffer limitation |
| `quark::CircuitBreaker` / `quark::Admit` | `model_call_gateway.hpp` | Retry/breaker state machine | **Medium** — self-contained utility, swappable behind the same call shape |
| `quark::EventLog<Record,S>` / `FenceToken` / `snapshot_sequential` / `load_snapshot` | `agent_session.hpp` checkpoint/snapshot, `workflow/checkpoint.hpp`/`time_travel.hpp` | Durable persistence + concurrent-mutation-safe snapshotting | **Deep** — solves a real correctness problem (snapshot-during-mutation race), not just storage |
| `quark::TestKit<A>` / `quark::Engine` | ~42 test files | Actor test harness (synchronous `TestKit` for unit tests, real multi-worker `Engine` for integration/live tests) | **Foundational** for those 42 files |
| `quark::Secret`/`SecretSource` | `trust/secret.hpp` | Secret-material types, adapted into AgentEngine's own `SecretLease` | **Medium** — adapter layer |
| `quark::pal::fd_t` and similar PAL typedefs | scattered (`sandbox/tls_client.hpp` etc.) | Platform-abstraction primitives | **Shallow** |
| Everything else (`tool_pipeline.hpp`, `effect_context.hpp`, `workflow/graph.hpp`, most of `sandbox/`, `trust/capability*.hpp`) | — | — | **Already Quark-agnostic** |

## 4. Target architecture (this ADR's actual proposal)

Each replacement is scoped to be as small as the job requires — explicitly NOT a re-derivation of
Quark's own generality (no distributed anything, no supervision-policy DSL, no cluster placement, no
persistence-adapter zoo). The guiding principle: match what MAF/OpenAI SDK/Anthropic's own agent
tooling actually do — lean on the HOST LANGUAGE's own concurrency primitives wherever one exists, and
own the smallest amount of runtime C++ genuinely needs because, unlike Python/C#/JS, C++ has no
language-level async executor a host environment supplies for free.

1. **`ae::task<T>`** — AgentEngine owns a small, self-contained C++20 coroutine type (no longer an
   alias to `quark::task<T>`). Lazy-start (matches every existing `co_await` call site's assumption,
   so call-site code is largely unchanged), move-only single-owner handle, exception-propagating,
   symmetric transfer for nested `co_await` chains (the same technique `quark::task<T>` already uses
   — a well-understood, widely-implemented C++20 pattern, not novel research). `tests/support/
   run_task_sync.hpp`'s existing "resume once" driver trick becomes AgentEngine's own, no longer
   borrowing Quark's internals.

2. **`AgentSession` becomes a plain class, not an actor.** `handle(Ask<StartRun,...>)` becomes an
   ordinary method, e.g. `task<result<AgentResponse>> start_run(Message user_msg)`; `Ask<
   ResolveInteraction,...>` becomes `task<result<AgentResponse>> resolve_interaction(InteractionId,
   bool approved)`. I1 ("one session, one executor") is re-derived locally: an internal atomic
   "in-flight" guard rejects (or, TBD by whoever implements Phase 2, queues) a second concurrent call
   into the same instance — the SAME guarantee Quark's mailbox gave structurally, now enforced by a
   small, directly-auditable guard instead of an actor scheduler. This is a real, deliberate
   trade — see §5's red-team finding on this specific point.

3. **A minimal executor for driving I/O continuations.** Real backend calls (HTTP/SSE) already run
   their blocking read loop on a detached worker thread today (`OpenAIChatClient`/
   `AnthropicChatClient`'s own `chat_stream()` implementations) — that pattern does not depend on
   Quark's scheduler at all and needs no change. What DOES need an owner is: who resumes a suspended
   `ae::task<T>` once its awaited operation completes? AgentEngine owns a small thread-pool executor
   (no mailbox, no descriptor-size limits, no supervision-policy DSL, no distributed anything) sized
   for this one job. **Open question for whoever implements Phase 1** (flagged, not decided here):
   should the default posture be synchronous (block the calling thread for a round-trip, matching how
   several community C++ SDKs for OpenAI/Anthropic already work, simplest, but loses live streaming
   concurrency) or thread-pool-async (matches AgentEngine's current live-streaming behavior, more
   moving parts)? This ADR recommends thread-pool-async, since ADR-034's real, Judged, live-verified
   streaming behavior is a genuine capability worth preserving, not something to regress.

4. **`ae::stream<T>` keeps its existing public shape** (callers do not need to change) — only its
   INTERNAL implementation swaps from `quark::ReplyStream<T*>` to a small, self-owned bounded channel
   (a ring buffer + condition variable, or an intrusive MPSC queue if profiling later shows it's
   warranted — not decided here, deliberately left to Phase 1's own prove step).

5. **Persistence becomes a host-injected interface**, not a runtime-owned mechanism: `SessionStore`
   (`save(SessionId, bytes) -> result<void>`, `load(SessionId) -> result<bytes>`), which the embedding
   application implements however it wants (file, SQLite, a real database) — matching how MAF/OpenAI
   SDK's own checkpoint story works (the framework defines the interface; the host owns the storage).
   AgentEngine keeps the byte-level serialization logic it already owns (`quark::serialize`-driven
   snapshot encode/decode is currently a thin layer over AgentEngine's own struct definitions per the
   coupling audit) — only the durable-write mechanism and the "safe to snapshot while a handler is
   mutating state" concurrency guarantee move from Quark's `FenceToken`/`EventLog` machinery to
   AgentEngine's own, built around the SAME in-flight guard item 2 already needs (a snapshot request
   simply waits for "not in-flight" the same way a second concurrent call would).

6. **`WorkflowSupervisor`/`FunctionExecutor` become plain classes** with a hand-rolled supervised-
   retry loop replacing Quark's `OnFailure<Restart, MaxRestarts<3, Within<1000>>>` policy DSL — the
   BEHAVIOR (restart up to 3 times within a window) is simple enough to write directly, without a
   general policy-composition system backing it.

7. **`quark::CircuitBreaker`/`Admit`** gets reimplemented as `ae::CircuitBreaker` — same algorithm
   (already small and self-contained per the coupling audit), owned rather than borrowed.

8. **Test harness**: a small AgentEngine-owned synchronous driver replaces `quark::TestKit` for the
   ~34 unit-test files that only need synchronous resolution; the ~8 files currently using a real
   `quark::Engine` for genuine multi-thread/timing proofs (e.g. `test_spawn_budget.cpp`'s real-race
   proof) need the new executor (item 3) to be real enough to reproduce the same class of proof —
   named as a real, non-trivial migration cost per file, not hand-waved.

## 5. Red-team findings (self-authored; a second, independent pass is recommended before Phase 1 starts)

- **I1 becomes a runtime-checked contract instead of a structurally-enforced one.** Today, a second
  concurrent call into the same `AgentSession` is IMPOSSIBLE — Quark's mailbox only ever admits one
  activation. Under item 2's design, it becomes a runtime guard that can be gotten wrong (an
  implementer who forgets to check the flag on a new entry point reintroduces the exact race Quark
  currently makes unreachable by construction). **Mitigation, not yet built**: the guard needs to live
  in ONE place (a base class or a mandatory wrapper every public entry point funnels through), with a
  test that specifically tries to violate it and confirms rejection — matching this project's own
  "a load-bearing invariant without a test is not done" rule.
- **This invalidates the proof artifacts behind 15+ Judged ADRs (027 through 036), not their claims.**
  Their BEHAVIORAL contracts (what a session/workflow does under retry, suspension, streaming,
  approval) are not in question and should be re-derived unchanged. But every test file that drives
  `quark::TestKit`/`quark::Ask<>` needs re-authoring against the new substrate — this is the single
  largest cost item in this ADR, likely exceeding the design/build work itself.
- **Persistence's hardest problem (safe concurrent snapshot) is currently solved by Quark's
  `FenceToken`, a mechanism this ADR is choosing to reimplement, not just relocate.** The in-flight
  guard (item 2) gives a correct but coarse answer (never snapshot mid-handler); Quark's own
  `FenceToken` design may have finer-grained reasons for its shape not yet understood from the outside
  — Phase 1 needs its own design → red-team pass specifically on this piece before it's trusted.
- **The minimal executor (item 3) is new, unproven infrastructure exactly where AgentEngine currently
  gets a mature, independently-red-teamed scheduler for free.** Quark's own engine has dozens of ADRs
  behind its mailbox/scheduler hot path (per Quark's own README). AgentEngine's replacement starts
  from zero red-team history. This is the single largest NEW risk this ADR introduces, not merely a
  cost of migration — it deserves its own dedicated design → red-team → prove cycle before Phase 2
  (AgentSession migration) is allowed to depend on it.
- **Scope discipline**: this ADR explicitly does NOT propose re-deriving Quark's mailbox, supervision
  DSL, distributed placement, or persistence-adapter ecosystem — only the narrow slice AgentEngine's
  own currently-implemented behavior actually needs. Scope creep back toward "build a smaller Quark"
  would defeat the entire point (§2's stated purpose: an SDK, not a runtime engine) and should be
  treated as a red-team finding against any future phase that drifts there.

## 6. What this ADR does not claim

- No code exists. Nothing in §4 has been built, tested, or proven.
- The threading-model choice in item 3 (synchronous vs. thread-pool-async default) is flagged, not
  decided — Phase 1's own design step must settle it, ideally with the project owner's input given how
  much it shapes the resulting API's feel.
- Quark stays a real, useful engine for OTHER projects (including, potentially, a future AgentEngine
  deployment target for real multi-node/HA needs) — this ADR removes it as AgentEngine's REQUIRED
  foundation, it does not argue Quark itself is flawed.
- No timeline commitment — §7's phases are a sequencing proposal, not a schedule.
- This does not relitigate `CLAUDE.md`'s other locked decisions (WASM plugin ABI, embedded CPython, no
  microvm profile, C++ CRTP + declarative authoring surfaces) — those are orthogonal to the runtime
  substrate question and are assumed to survive this migration unchanged.

## 7. Proposed phasing (sequencing only, not a committed plan)

- **Phase 0 (this document).** Design + red-team. Awaiting project owner review/Judged sign-off before
  Phase 1 starts — given the scale, likely with amendments, not a rubber stamp.
- **Phase 1.** Build the new substrate (`ae::task<T>`, the minimal executor, `ae::CircuitBreaker`, the
  new `stream<T>` backend, `SessionStore` interface) as new, additive AgentEngine types, each proven
  standalone with new tests — NOT yet wired into `AgentSession`/`WorkflowSupervisor`. Quark keeps
  running everything real during this phase; nothing regresses.
- **Phase 2.** Migrate `AgentSession` off `quark::Actor` onto the new substrate. Every existing ADR's
  BEHAVIORAL claim (027 tool-call loop, 029 suspend-for-approval, 034 streaming, 036 gateway, etc.)
  gets re-proven against the new substrate, one at a time — not a single big-bang cutover.
- **Phase 3.** Migrate `WorkflowSupervisor`/`FunctionExecutor` the same way.
- **Phase 4.** Remove the Quark submodule, all remaining `quark::` includes, and the CMake dependency.
- **Phase 5.** Full-suite re-verification against the Quark-free build; update
  `AgentEngineSpecification.md`'s "Quark mapping" section (§8) to reflect the new architecture, since
  this ADR, once executed, supersedes that section's current framing.

## 8. Files this ADR proposes eventually changing

Not exhaustive — a real accounting is Phase 1's own job. The largest-known items, from §3's table:
`include/agentengine/core/agent_session.hpp`, `include/agentengine/core/task.hpp`,
`include/agentengine/core/stream.hpp`, `include/agentengine/core/model_call_gateway.hpp`,
`include/agentengine/workflow/supervisor.hpp`, `include/agentengine/workflow/executor.hpp`,
`include/agentengine/workflow/checkpoint.hpp`, `include/agentengine/workflow/time_travel.hpp`,
`include/agentengine/trust/spawn_cost_budget.hpp`, `include/agentengine/trust/secret.hpp`, ~42 test
files currently on `quark::TestKit`/`quark::Engine`, `AgentEngineSpecification.md`, `CMakeLists.txt`,
and eventual removal of the `third_party/quark` submodule itself.
