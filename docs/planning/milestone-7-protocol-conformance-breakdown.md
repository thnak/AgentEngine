# Milestone 7 — Protocol conformance — work breakdown and kick-off

**Status:** Work breakdown (stage 4 of [the review-signoff workflow](v1-review-signoff-workflow.md)),
written just-in-time as this milestone starts, per that doc's §4. Scoped to
[the roadmap's](v1-implementation-roadmap.md) Milestone 7 exit criterion: *"`conformance server`/
`conformance client` pass at `2026-07-28` (011 §10 G1/G2), `a2a-tck` passes with zero MUST-level
failures (012 §8 G1), the AG-UI compatibility suite passes against a pinned schema (013 §6 G5), a
YAML agent produces byte-identical metadata to its C++ equivalent (015 §7 G1, I6's actual
enforcement), and 006 §6b's G6-G9 ... all hold — closing 019 §2's wake-condition table completely."*

**RFCs:** 011 (MCP Conformance), 012 (A2A Conformance), 013 (UI and Streaming Surfaces), 015
(Declarative Agent and Workflow Format), plus 006 §6b (`Backgroundable`/`StandingEffect`) per
`docs/planning/backgroundable-standingeffect-gap.md`'s assignment. All five Reviewed (2026-08-05).

## Current state (verified 2026-08-08, after M6)

| Item | State |
|---|---|
| `include/agentengine/protocol/{mcp,a2a,agui}/` | **README only.** No headers, nothing compiled. `protocol/anthropic/` and `protocol/openai/` (real `ChatClient`s, M5) are the only non-stub siblings — no precedent to reuse for a *wire protocol adapter* shape, only for "a backend behind a policy-tagged interface" |
| `Backgroundable`, `StandingEffect`, `background_task`, `list_standing_effects`, `cancel_standing_effect` | **Do not exist anywhere** (confirmed by grep, matching the residual note's own finding). `Parallelizable` (`core/tool.hpp:29`) is the one real sibling tag — an empty marker struct with no pipeline logic behind it yet, the shape `Backgroundable` will follow |
| `core/tool_pipeline.hpp`'s 10-step pipeline | Real (M2 Phase B, M4 Phase F1). Step 8 ("invoke") is synchronous and blocking end-to-end today — `invoke_tool()` calls `tool->invoke(...)` and returns only after it completes. `Backgroundable`'s whole point is stopping step 8 alone from blocking the turn; every other step (1-7, 9-10) stays as-is per the residual note ("still runs the full 10-step tool pipeline; only step 8 stops blocking") |
| `Interaction` (`core/interaction.hpp`) | Real since M4. `StandingEffect`'s handle is a **different** shape — not a suspend/resume correlation id, an unforgeable *live effect* handle (007 §3.4-style), with its own introspection (`list_standing_effects()`) and kill (`cancel_standing_effect()`) surface. Do not conflate the two; 013 §1 is explicit that `StandingEffect` visibility rides `StateChanged`, never `InputRequired`'s pair |
| `EffectJournal`/`IdempotencyKey` (`core/effect_journal.hpp`) | Real (M4 Phase F1/F2). A backgrounded call still derives and journals a key exactly as a foreground one does — `Backgroundable` changes when step 8 returns to the caller, not the accounting around it |
| `TimerWake` (`agent_session.hpp:137`) | Real (M4 Phase E3), and explicit in its own comment that it is *not* what a real background-task wake needs — "what a real 'resume the paused run this timer was arming' would DO needs 006 §6b's `schedule_wakeup`/`Backgroundable`... a different, un-built vertical". This milestone is what finally builds that vertical |
| `ae::stream<T>` (`core/stream.hpp`) | Real (M5 Phase B4b), credit-controlled, genuinely incremental — the primitive 013 §1's internal run event stream will ride, and what Phase G (013 §7 Q2's resolution) uses for the per-subscriber bounded-eviction fan-out A2A's ordering MUST requires |
| A "run event" type / `RunStarted`/`ModelDelta`/`StateChanged`/... (013 §1's twenty-line enum) | **Does not exist anywhere.** No `RunEvent`, no `StateChanged`, no sequence-numbered per-run stream. `AgentSession::handle(Ask<StartRun,AgentResponse>)` (agent_session.hpp) computes everything it needs and replies once — it emits nothing incrementally today |
| `WorkflowLiveEvent` (`workflow/live_view.hpp`, M6 Phase G) | Real, but scoped narrowly to a workflow supervisor's own superstep boundaries (`executor_live_state`, round number) — **not** 013 §1's general run-event vocabulary (model deltas, tool-call deltas, approvals). A workflow run and an agent-session run are different actors emitting different event shapes; 013 §1 is the agent-session side, and does not yet exist |
| `quark::Engine`, real | Precedent since M4 Phase E2 (`test_agent_session_suspend_resume.cpp`), reused throughout M6. Every M7 test that exercises a genuinely async surface (a backgrounded call outliving its turn, a streamed run) needs this, not `TestKit` — same forcing function M6 decision 3 already named |
| Quark `EventLog<T,S>`/`Store` | Real, used repeatedly in M4/M6 for anything append-only or multi-retained. `list_standing_effects()`'s "what's currently outstanding" is a live in-memory census, not a durable log by itself — but a `StandingEffect`'s *registration*/*resolution* transitions are exactly the kind of unboundedly-growing history `EventLog` already proved itself for (M6 decision 1) if this milestone needs an audit trail of them, not just current state |
| 004's `ChatClientCapabilities::batch` (`core/chat_client.hpp:52`) | Real, declared, **`false` everywhere** — nothing sets it. 004 §8 Q1 resolved that a real batch-API backend rides `Backgroundable`/`StandingEffect` once this milestone builds it; wiring an actual batch-capable `ChatClient` is not itself in scope here (that's a M5-owned RFC's own backend work, only unblocked by this milestone, not completed by it) |
| Machine-safety envelope | Unchanged: `-j4` max, tests pinned ≤ 4 cores. Nothing in this milestone's own gates (a MUST-level TCK run, a schema-pinned compatibility suite, a byte-identical-metadata corpus) implies an actor-count blowup the way M6's N=100 Projects or 10³-seed shuffle did — flagged here only because it wasn't, not because a check was skipped |

## Design decisions made while breaking this down

1. **013 §1 (the internal run event stream) is built before 006 §6b, reversing the roadmap
   sentence's reading order — because 006 §6b's own text requires it, not despite it.** The
   roadmap's prose says "M7 closes 006 §6b first, then builds 011/012 against it" — true for 011/012,
   but 013 §1 itself states `StandingEffect` visibility "rides `StateChanged`, not a new event pair"
   and names this as closing 006 §6b's own observability gap. Building `Backgroundable`/
   `StandingEffect` before a `StateChanged` event exists would mean either inventing a throwaway
   placeholder event (drifts from 013 §1's real shape, the exact "second wire mapping to design and
   maintain" 013 §1 says NOT to do) or shipping G6-G9 with no observability proof at all. 013 §1 has
   no dependency on 006 §6b, 011, or 012 being built first — it is a self-contained emission
   mechanism over `AgentSession`'s own existing turn loop — so there is no real ordering cost to
   moving it first, only a benefit: 006 §6b, 011's progress projection, and 012's streaming all build
   against one real thing instead of three separate stubs.

2. **`Backgroundable`/`StandingEffect` (006 §6b) is Phase B, immediately after the event stream —
   matching the roadmap's own rationale for WHY M7 owns it at all.** 019 §2's six-row wake-condition
   table has two rows shipped (M4) and needs "Local background task completion" (this RFC pair) plus
   "External event"/"Remote task completion" (needs 012, built later this milestone). Building 012
   against a still-stubbed `Suspended` state would make `a2a-tck` MUST-level task-completion cases
   unprovable — the same dishonesty risk the roadmap already names. Phase B closes that dependency
   before 011/012 need it.

3. **011 (MCP) and 012 (A2A) are built as sibling phases, not strictly sequenced, but MCP first —
   012's task/streaming machinery benefits from 011's request/response and progress-projection
   plumbing existing as a second data point before A2A's own (structurally similar but not
   identical) task lifecycle is designed.** Both depend on 013 §1 (progress/streaming projection)
   and 006 §6b (011's tasks extension, 012's task completion) being real, which Phases A-B provide.
   Neither depends on the other (their RFCs list no mutual dependency, and CLAUDE.md's own "the
   dependency graph is not a DAG" caution names 018↔011↔012 as the real cycle — the 018 credential
   seam, not 011↔012 directly). 018 (Identity/Secrets) is real since M5, so that cycle is not live
   for this milestone.

4. **013's remaining surfaces (§2 AG-UI, §3 other projections, §4 transport) are built as their own
   phase AFTER 011/012 exist, not interleaved with them — because 013 §6 G3 (cross-surface content
   equivalence between AG-UI and A2A streaming) needs both surfaces real to compare.** 013 §1 (Phase
   A) already provides what 011's progress notifications and 012's task streaming both project from;
   what's deferred to this later phase is specifically the AG-UI-facing projection and the
   cross-surface proofs that need a second real surface to prove equivalence against.

5. **015 (Declarative format) is the last RFC-scoped phase, not because it is gated on the others —
   its own dependencies (002, 006, 014) are all real since M2/M6 — but because 015 §6's "Publishing
   an agent document generates its A2A Agent Card (012) and MCP tool listing (011) from the same
   metadata" and §7 G4 ("Agent Card / MCP listing regenerated deterministically") need 011/012 to
   exist to generate INTO.** G1-G3 (equivalence, strictness, workflow-trace equivalence) do not
   share this constraint and could in principle be built earlier; kept as one phase rather than
   split across the milestone for the same reason M6 kept single-RFC phases contiguous — a scattered
   G1/G4 split would cost more in context-switching than it saves in parallelism this project's own
   phase-by-phase, one-commit-per-phase discipline doesn't exploit anyway.

6. **AG-UI's own maturity gap (013 §2.0: no spec, no TCK, pre-1.0, MIT/informally-governed) means
   013's own gate is "compatibility against our own authored suite", not "conformance" — carried
   into this breakdown verbatim, not softened.** Where 011/012 gates cite an external, versioned,
   third-party conformance tool run against a named revision, 013's AG-UI gate (G5) cites a suite
   this project authors and maintains itself, pinned to an `@ag-ui/core` schema release. This is
   013's own RFC text, not a scope reduction introduced here.

7. **The `Background<max_concurrent>` capability tag (007 §3) is new vocabulary this phase adds to
   `trust/capability.hpp`, not a reuse of an existing tag.** 007 §3 is real (M2) but has never
   declared a background-task-specific capability shape; `Background<N>` is 006 §6b's own named gate
   (G9: "a session already at its cap has its next `background_task` rejected at pipeline step
   4/authorize"), so it must exist as a real, checkable capability before G9 is provable — this is
   additive to 007's real capability-tag vocabulary, not a redesign of it.

## Phases

- **Phase A** — 013 §1: the internal run event stream (`RunEvent` vocabulary, sequence numbers per
  run, `AgentSession` emission at existing turn-loop boundaries, backpressure over `ae::stream<T>`).

  **Outcome (2026-08-08, commit pending):** `include/agentengine/core/run_event.hpp` (new) defines
  the total 24-kind `run_event_kind` vocabulary from 013 §1's own list, each backed by a real payload
  type (a `std::variant` over flat structs — safe here, unlike `content_record.hpp`'s rejected
  variant-`quark_describe` design, because this stream is never durably serialized). `AgentSession`
  (`agent_session.hpp`) gained `enable_event_stream()` (mirrors `WorkflowSupervisor::enable_live_view()`
  exactly, M6 Phase G) and a private `emit_run_event()` helper, wired at every boundary its turn loop
  actually has today: `RunStarted` → `TurnStarted` → `ModelCallStarted` → `ModelCallFinished` →
  `TurnFinished` → `RunFinished` on the success path, with `RunFailed` as the terminal event (never
  followed by `RunFinished`) on each of the four pre-existing fail-closed branches (context-assembly
  failure, no chat client configured, chat-call failure, token-budget exceeded) — each carrying a real
  `error_code`. The admission-denial branch fires no event at all, matching 001 §1's "no Run is minted
  until admission passes." `run_event_seq_` resets to 0 at the top of every `handle()`, proven by
  running two `StartRun`s on one session and observing the second run's first event at `seq == 1`, not
  a continuation of the first run's count. `tests/test_agent_session_run_event_stream.cpp` (new, 22
  checks, all passing) proves all of the above under `quark::TestKit` (no genuine cross-actor parking
  is introduced — `make_stream<T>` needs no actor addressing per `stream.hpp`'s own header). Full suite:
  129/129 passing (added one test from M6's close-out 128).

  **What is honestly NOT wired**, named per this doc's own decision 1 rather than silently assumed:
  `ToolCallStarted/Delta/Finished`, `ModelDelta`, `SandboxExecStarted/Finished`, `ArtifactProduced`,
  `RunCanceled` have real types but no real producer inside `AgentSession` — its turn loop still never
  reaches the tool pipeline (`invoke_tool`/`invoke_agent_tool` are called only from
  `agent_registry.hpp`, never from `AgentSession::handle`) and never uses `chat_stream()`. `StateChanged`
  is defined but has no caller yet either — Phase B's `StandingEffect` visibility is its first real
  producer, per 013 §1's own "rides `StateChanged`" rule. `EffectContext::report_progress` (013 §1's
  named producer for `ToolCallDelta`, 006 §6a) does not exist yet — deferred to whichever phase first
  needs to call a tool with progress reporting, most likely Phase B or C.
- **Phase B** — 006 §6b: `Backgroundable`, `Background<N>` capability, `background_task()`,
  `StandingEffect` handle, `list_standing_effects()`/`cancel_standing_effect()`; closes 019 §2's
  "Local background task completion" wake row; `StateChanged` visibility from Phase A.
- **Phase C** — 011 MCP: client role (§3: tools/resources/prompts, MRTR, caching, pagination) and
  server role (§4: exposing AgentEngine), against `2026-07-28`.
- **Phase D** — 012 A2A: server role (§2: Agent Card, HTTP+JSON/REST + JSON-RPC bindings, task
  management, push notifications) and client role (§3: consuming remote agents), against v1.0;
  closes 019 §2's remaining two wake rows.
- **Phase E** — 013 §2-§4: AG-UI projection, other-surface table, transport (SSE, binary framing,
  WebSocket), cross-surface equivalence proofs.
- **Phase F** — 015: declarative agent/workflow YAML, the shared validator, equivalence corpus.
- **Phase G** — promotion gates: 011 §10 G1-G9, 012 §8 G1-G5, 013 §6 G1-G6, 015 §7 G1-G4, 006 §6b's
  G6-G9 — run for real, published percentages where the gate asks for one, milestone close-out.

Each phase follows the established M6 discipline: implement → build (PowerShell + `vcvarsall`) →
test → full-suite regression → update this doc's own phase "Outcome:" → one commit, narrative body,
no co-author trailer.
