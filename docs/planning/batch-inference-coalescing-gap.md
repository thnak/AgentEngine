# Coalescing concurrent agents onto one vendor batch call — gap and design questions

**Status:** Pre-milestone scoping note, not a stage-4 work breakdown — same rationale as
`agent-as-workflow-executor-gap.md`: no milestone currently owns this. Written from a real user
proposal (2026-08-13): since providers support batch inference, let concurrently-running agents share
one batch submission to save cost. **Document only, per the same explicit project-owner direction
already standing for the workflow-executor gap: do not implement yet.**

**RFC:** 004 (Model Provider Plane) §8 Q1 — already resolved (2026-08-04) that AgentEngine's `batch`
capability bit rides the SAME `Backgroundable`/`StandingEffect` shape as any other long-running
work, not a bespoke batch-tracking structure. **Research:**
`docs/research/2026-08-13-vendor-batch-inference-apis.md` — OpenAI's and Anthropic's real Batch APIs,
fetched and cited directly (not from memory, per `CLAUDE.md`'s research discipline).

## What's real today

- `ChatClientCapabilities::batch` (`core/chat_client.hpp:52`) — declared, `false` everywhere, nothing
  sets or reads it.
- `StandingEffect`/`start_background_task()` (`core/standing_effect.hpp`,
  `rt/agent_session.hpp:713-`) — real, shipped (M7 Phase B), but **for TOOL calls only**
  (`standing_effect_kind` has exactly three values: `schedule_wakeup, watch_resource,
  background_task` — no model-call variant). `standing_effect.hpp`'s own file-top comment states
  directly: `start_background_task()` "is what 004 §8 Q1's batch-API resolution is blocked on" — i.e.
  this codebase's own comments already name batch as waiting on exactly this prerequisite, which is
  now built, just never extended to model calls.
- The completion-delivery idiom is real and reusable: `start_background_task()` mints a
  `StandingEffect` handle, submits work with a completion callback closing over a `std::weak_ptr<
  BackgroundCompletionQueue>` (safe if the session is gone by completion time — drops silently, no
  UAF, `rt/agent_session.hpp:731-741`), and the session later drains `background_completions_` to
  resolve pending calls. Any batch-call design should reuse this shape, not invent a second one.

## The load-bearing finding: batch mode and AgentEngine's tool-calling loop are structurally at odds

Confirmed from real vendor docs (see research doc): a batch item is one complete, independent
request/response pair. Neither OpenAI's nor Anthropic's batch API lets a batched request see a tool
result and continue the SAME turn — `AgentSession::run_rounds()`'s whole shape (model → tool call →
tool result → model again, N rounds) would need EACH ROUND submitted as its own separate batch job,
each incurring the full turnaround (often <1h for Anthropic, up to 24h for OpenAI) — a 5-round
tool-calling turn could take on the order of hours to a day, not API-round-trip time. Anthropic's own
batch-mode parameter table additionally excludes their stateful "Threads" feature outright ("Threads
are stateful; batch requests are not") — an independent vendor-side confirmation that batch mode is
fundamentally a single-shot mechanism, not a multi-turn conversation primitive.

**This means "coalesce N concurrently-running agents onto one batch call" is a real, valuable,
buildable pattern for a specific shape of workload — not a general accelerator for every
`AgentSession`.** It fits: N independent, single-shot (or tool-free / pre-resolved-tool) model calls
that don't need to see their own result before the workflow can proceed — e.g. N workflow fan-out
nodes each doing one classification/summarization call, or N independent agents each producing one
verdict with no follow-up round. It does NOT fit an ordinary multi-round tool-calling `AgentSession`
turn without multiplying that turn's latency by however many rounds it takes, once per round.

**Resolved, red-teamed once:** `batch-inference-coalescing-design-draft.md` works through concrete
answers to all six questions below, scoped narrowly to N `agent`-kind workflow-fan-out nodes in one
superstep round. Headline results: reuse `request_port`/`Interaction` (ADR-029) instead of
`StandingEffect` (which is explicitly non-durable today) for the suspend/resume shape; no new message
type needed for N-way resolution (`resume_workflow()` already does this, once per port); and a real,
independent, separately-landable prerequisite was found — `WorkflowSupervisor::resume_workflow()` has
NO caller/admission check at all today, unlike `AgentSession::resolve_interaction()`'s own
`principal_admitted_for()` check. Read the design draft before treating the questions below as still
fully open.

## Design questions a future ADR needs to answer (not answered here)

1. **Who decides a call is batch-eligible, and at what granularity?** Per-`ChatClient` configuration
   (matching `ChatClientCapabilities::batch`'s current shape — a backend either supports it or
   doesn't), per-call opt-in (a `StartRun`-level flag, matching `suspend_for_approval_`'s existing
   per-session opt-in pattern from ADR-029), or per-workflow-executor declaration (an
   `Executor`-level field, the same shape OQ-19's capability-ceiling question already proposes adding)?
   Whichever is chosen must not silently make an ordinary multi-round agent invisible-slow — 004 §8
   Q1's own resolution already requires this be "an explicit policy choice... not automatic."
2. **Who owns the coalescing coordinator, and what triggers a flush?** A real batch submission needs
   to accumulate N pending calls before submitting — a count threshold, a time window, or both (the
   classic micro-batching tradeoff: too short a window loses coalescing opportunity, too long adds
   pure latency with no other benefit for calls that arrive early). Is this coordinator process-global,
   per-provider, per-workflow-run, or something else — and does it need its own actor-free concurrency
   discipline (`rt::AsyncMutex`, matching `AgentSession::session_mutex_`'s own I1 enforcement) to avoid
   a torn read/write on the pending-batch accumulator from concurrent `ThreadPool` workers, the exact
   class of hazard `agent-as-workflow-executor-design-draft.md` §2 already found real once this session
   (concurrent workers touching shared state with no merge/dedup step)?
3. **Result fan-out shape.** One vendor batch job produces N results (each tagged by `custom_id`,
   per the research doc); N different `StandingEffect`s/callers are waiting on N different results
   from the SAME vendor job. Does `standing_effect_kind` need a fourth enumerator (e.g.
   `model_batch_call`), and does the completion-queue idiom (`BackgroundCompletionQueue`,
   `rt/agent_session.hpp`) generalize cleanly to "one vendor poll resolves N pending completions
   simultaneously," or does today's shape (built for "one tool call, one completion") need a real
   change to fan one poll result out to N sessions/handles?
4. **`custom_id` and capability/principal attribution.** Every batched vendor request needs a
   `custom_id`; AgentEngine's own `StandingEffect` already carries `session_id`/`principal_id`/`run_id`
   for exactly this class of attribution (I4). Does `custom_id` simply BECOME (an encoding of) the
   `StandingEffect::handle_id`, or does it need its own mapping table — and if a batch coalesces calls
   from DIFFERENT principals/tenants into one vendor submission, does that cross a trust boundary worth
   naming explicitly (a vendor operator can see that N distinct AgentEngine principals' prompts were
   submitted in the same named batch job, which is a real, if minor, cross-tenant metadata leak
   vendor-side — named here so a future ADR decides it deliberately, not by accident)?
5. **Polling ownership and durability.** Both vendors are poll-only (no webhook) — something has to
   own a recurring poll loop per in-flight batch job, and that ownership needs to survive whatever this
   process's own durability story already provides (019, session `Store` seam) — does a pending batch
   job's poll state need to be checkpointed the same way `AgentSession`/`WorkflowSupervisor` records
   already are, or is "in-memory only, lost on restart, caller re-submits" an acceptable, honestly
   documented first cut (matching `StandingEffect` itself, which `standing_effect.hpp`'s own comment
   already states is "in-memory-only, never persisted")?
6. **Cost-vs-latency policy surface.** 004 §8 Q1 frames batch as "trades latency for cost... an
   explicit policy choice." Whatever mechanism answers question 1 should make this tradeoff visible to
   whoever configures it (a real number — expected/typical turnaround, per the research doc's ~1h/24h
   figures — not just a boolean), so a caller can't accidentally opt a latency-sensitive path into a
   multi-hour wait.

## What NOT to design around (named, not solved)

Full OAuth-style dynamic per-request batch admission (accepting a batch-eligible call from ANY
concurrently-running session at ANY time, with no caller-visible boundary on the resulting wait) is
explicitly out of scope for a first design — matching this project's own "narrower than the RFC's own
gate, not silently dropped" discipline. A first real design should scope to ONE clear caller-visible
shape (most plausibly: workflow fan-out nodes, since that's where "N agents running in parallel" is
already a real, structural fact about the graph) rather than a fully general cross-session batching
service.
