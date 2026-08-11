# ADR-027 — A real, production tool-call loop inside `AgentSession::handle()`

**Status:** Judged, accepted (2026-08-11). Designed, red-teamed, implemented, and proven (real code
+ deterministic tests, this document's own §5); accepted by the project owner per this project's
governance (`decisions/README.md`; `OpenQuestions.md` OQ-11's resolution that the project owner is
the ADR judge) — the same "asked directly, the user chose" pattern ADR-024 already established for
this project's judged decisions.

**Relates to:** `decisions/ADR-024-skill-scoped-tool-and-mount-wiring.md` (the "declared and
invocable must be the same live table" invariant this ADR now satisfies structurally inside
`AgentSession` itself, not only in `cli_chat.cpp`'s hand-driven loop by convention),
`decisions/ADR-023-response-format-codec-seam.md` (§6 point 4 / 007 §4's `text_derived`
declassification gate — the mechanism this ADR's must-fix #1 protects), `decisions/ADR-006-agent-
spawn-depth-budget-bound.md` (the "prove the counter, name the rest as a residual" scale precedent
this ADR follows), `docs/architecture/worktree-sharing-skills-and-subagents.md` §3 and `026-Agent-
Facing-Runtime-Surface.md` §9 Q1 (both named "`AgentSession` still owns no sandbox/tool-call loop in
production" as the blocking prerequisite this ADR closes).

## 1. The question

`AgentSession::handle(Ask<StartRun, AgentResponse>)` (`include/agentengine/core/agent_session.hpp`)
made exactly one model call per run and never inspected the response for a `ToolCall` — a real
gap named repeatedly this session's own audits (ADR-024 §3b/§7, the worktree as-built trace, 026 §9
Q1) as the root cause blocking several other tracked items (`agent.spawn` wiring, Q1's cost-budget
design) from ever reaching production instead of `cli_chat.cpp`-demo-only. Every proof of real
tool-calling before this ADR hand-rolled the round loop **externally** — repeatedly calling
`kit.ask<AgentResponse>(StartRun{...})` from outside the actor, in three independent, drifting
copies (`tools/cli_chat.cpp`, `tests/test_agent_session_live_multitool_e2e.cpp`,
`tests/test_agent_session_skills_live_e2e.cpp`).

**Stated so it has a wrong answer:** should the loop move inside `handle()` (one `StartRun` → N
internal rounds → one final `AgentResponse`), or should it stay external, formalized into a
reusable orchestration helper instead of three hand-rolled copies?

Decided, by the project owner, before design work started: **inside.** This matches 001 §2's own
vocabulary ("Turn: A segment of a run's coroutine between model calls" — turns nest inside a run's
own coroutine) and `turn_index`'s own pre-existing comment, which already named this as the
intended future shape. Scope was also decided upfront and is not re-litigated here: only tools
whose `invoke()` needs nothing beyond `EffectContext&` are reachable through this loop
(session-scoped stateful tools — a persistent interpreter, mounted-skill state — are a separate,
later design, since `Tool<>::invoke()` has no path to `AgentSession::state_` today); approval is
synchronous-decider only (suspending the run to wait for a real human, via the existing-but-unwired
`open_interaction()`/`resolve_interaction()`, is a separate, later design).

## 2. The design

`handle()`'s body is now a bounded round loop (`for (; effect_context_.turn_index < max_turns_;
++effect_context_.turn_index)`), each round: assemble context via `HistoryProviderT::on_context`,
call `chat_client_->chat(...)`, check the per-run token budget, then either (a) no `ToolCall` in the
response → converged, respond and return, or (b) resolve every `ToolCall` sequentially through the
real ten-step pipeline (`invoke_tool`, `tool_pipeline.hpp`), fold the results into one `role::tool`
message, and continue. `max_turns_` (default 16, matching `AgentMetadata::max_turns`'s own default)
and `approval_decider_` (default empty, fail-closed) are new runtime-configured members —
`max_turns` is an additive `initialize()` parameter, mirroring how `token_budget` already works;
`approval_decider_` gets a `set_approval_decider()` accessor mirroring `set_capabilities()`.

**`ToolTable::from_descriptors(contribution->tools)` is rebuilt fresh every round from the exact
same snapshot used to declare tools to the model that round** — one table backs both the
`ChatRequest` and every `invoke_tool` call in that round, closing (for this loop specifically) the
"declared ≠ invocable" gap ADR-024 §3a/§7 named as enforced only by convention in `cli_chat.cpp`.

A new shared header, `include/agentengine/core/tool_call_extraction.hpp`
(`tool_calls_of`/`text_of`/`tool_results_message`/`tool_call_request_of`), replaces the three
drifting copies these call sites used to maintain independently.

## 3. The red-team pass

Design was reviewed adversarially (an independent pass, not self-review) before any of it shipped.
Two findings were load-bearing enough to change the shipped design, not deferred as residuals:

### Must-fix #1 (fatal as first drafted) — `ToolCall::provenance` must thread into `ToolCallRequest::provenance`

The first draft built each round's `ToolCallRequest` without copying `ToolCall::provenance`
(`content.hpp`) into `ToolCallRequest::provenance` (`tool_pipeline.hpp`), which silently defaults to
`call_provenance::vendor_structured`. `invoke_tool`'s step 5 branches entirely on this field: a
`text_derived` call (a laundered, model-injected tool call — the exact confused-deputy shape
ADR-023 §4b Finding 1 closed) is supposed to bypass the tool's own `approval_mode` entirely and go
through the strict `is_auto_declassifiable_text_derived_call` gate instead. Dropping the field
silently defeats that override — a `text_derived` call against a tool with a non-inert capability
ceiling and `approval_mode::never_require` would be silently approved.

This exact omission **already existed**, independently, in all three external-loop copies this ADR
replaces (`cli_chat.cpp`, both live e2e tests) — none of them had ever been exercised unattended
(a human was always watching the CLI, or the tests used synthetic `vendor_structured` calls only).
Moving the pattern into `AgentSession::handle()` would have promoted it to the first unattended,
no-human-in-the-loop production path — exactly the scenario the declassification gate exists to
protect. **Fixed**: `tool_call_request_of` (tool_call_extraction.hpp) threads `provenance` correctly;
the fix lands in one shared place instead of four independent copies.

### Must-fix #2 (confirmed breaking change) — the three external-loop call sites had to be rewritten in the same change

Tracing exact assertions confirmed the two live e2e tests would fail outright (their external
tool-dispatch code becomes unreachable the instant `handle()` converges internally before the ask
returns) and `cli_chat.cpp`'s own external loop would silently become dead code. All three were
rewritten as part of this change (§6), not deferred.

### Named residuals (not fixed — recorded, matching this project's "named, not silently assumed
covered" discipline)

- **Synchronous tool calls block the Quark worker thread**, not just this actor's mailbox —
  `invoke_tool` has no yield point. Native tools reachable through this loop must stay
  non-blocking/CPU-bound; a tool needing real I/O belongs on the existing `Backgroundable`/
  `start_background_task()` path instead.
- **Approval-gated tools with no `set_approval_decider()` configured are permanently unusable** —
  every call denied every round until `run.max_turns_exceeded`, indistinguishable from any other
  non-convergence reason in that event today.
- **Null `capabilities_` falls back to a per-call-denied empty `CapabilitySet`**, not a run-level
  abort — confirmed reachable and safe, but can burn up to `max_turns_` rounds of denied-call
  theater before failing (R5, §5).
- **`on_turn_end`'s per-round `TurnView` no longer includes the run's originating input message**
  (only the round's own response + tool-results) — a real, silent change from the old single-call
  `{input, response}` contract. No current `HistoryProviderT` conformer reads this argument for
  anything but `co_return`, so nothing observes the difference yet — documented for whoever wires a
  real memory-writing provider (029 §4) next.
- **`AgentMetadata::max_turns` (compiled from `MaxTurns<N>`) is still not wired** to the new runtime
  `max_turns_` — two independent knobs remain.
- **No mid-loop checkpointing** — `AgentSessionRecord`/`to_record()` is unchanged; a process death
  mid-loop loses the whole run, same posture as before this ADR (single-call runs were never
  mid-flight-checkpointed either).

## 4. Falsifiable claims and verdicts

| # | Claim | Evidence | Verdict |
|---|---|---|---|
| C1 | A multi-round tool conversation converges to a final Text answer within one `StartRun` ask, without any external round-driving. | `tests/test_agent_session_tool_call_loop.cpp` R1: a scripted `ChatClientT` returns `ToolCall`s for 2 rounds then plain text; one ask returns the converged response; `client.call_count == 3`. | **CORRECT** |
| C2 | A `text_derived` call against a non-inert-capability, `never_require` tool is denied — proving must-fix #1 actually closes the gap, not merely documents it. | R2: `write_tool_invoked_log()` stays `false`. **Regression-verified both directions**: with the provenance-drop bug deliberately reintroduced, this same assertion FAILS (confirmed live during this session, then reverted) — not a test that would pass regardless. | **CORRECT** |
| C3 | A failing tool call does not abort the run — the error is fed back and the loop continues. | R3: the run converges after `failing_tool` errors; `client.call_count == 2` (a second model call genuinely happened). | **CORRECT** |
| C4 | Exhausting `max_turns_` without convergence fails the run closed, never a hang. | R4: a chat client that always requests a tool call never gets a response (`!r.has_value()`), `client.call_count == max_turns` exactly, process does not hang. | **CORRECT** |
| C5 | Null `capabilities_` denies every tool call per-call rather than crashing, still terminates via the same fail-closed path. | R5: no `set_capabilities()` call; run still fails closed via `max_turns_exceeded`; `write_tool_invoked_log()` stays `false` (denied at step 4, before step 8). | **CORRECT** |
| C6 | `tool_call_started` observably precedes `tool_call_finished` for the same call — a live event-stream consumer sees an in-progress state. | R6: both events fire; `tool_call_started`'s index in the drained `run_event` stream is strictly less than `tool_call_finished`'s. | **CORRECT** |

## 5. Evidence

`tests/test_agent_session_tool_call_loop.cpp` (new, deterministic, offline — no live model, no
network) — all 6 cases above, 14 individual assertions, all pass. Full regression suite: **178/179**
tests pass (`ctest`, this session); the one failure, `test_mediated_python_runner_hostile_corpus`,
is pre-existing and unrelated (native-jail Python sandbox internals, no file this ADR touches) —
**corrected 2026-08-11**: not a flake, two real deterministic test-authoring bugs, both fixed the
same day; see ADR-024 §6's own corrected note. The two rewritten live
e2e tests (`test_agent_session_live_multitool_e2e.cpp`, `test_agent_session_skills_live_e2e.cpp`)
— **re-run live against a real provider** (OpenRouter, `~deepseek/deepseek-v4-flash-latest`)
after this ADR was judged: both pass, including the internal loop genuinely threading
`get_weather`'s real returned value (13.7°C) into `convert_temperature`'s argument across two
internal rounds, resolving a forced-parallel two-tool round, and the model reaching for
`execute_code` purely because it read the mounted skill's advertisement — the same live behavioral
claim ADR-024 originally proved, now reproven through the new internal loop. One of six live runs
of `test_agent_session_live_multitool_e2e` hit a transient `no_reply` (did not converge within
`max_turns` on that single attempt); the other five passed cleanly with identical code — consistent
with ordinary live-model non-determinism this suite's own top comment already anticipates, not a
reproducible defect. Confirmed still compile-and-skip-cleanly when
`AGENTENGINE_OPENROUTER_API_KEY` is unset, per `tools/run-
live-provider-tests.ps1`'s existing contract).

## 6. Files changed

- `include/agentengine/core/agent_session.hpp` — `handle()`'s body (the loop), `max_turns_`/
  `approval_decider_` members and accessors, `initialize()`'s additive `max_turns` parameter.
- `include/agentengine/core/tool_call_extraction.hpp` (new) — the shared, provenance-correct
  helpers.
- `tools/cli_chat.cpp` — external round loop removed; `main()` now issues one ask per user line.
- `tests/test_agent_session_live_multitool_e2e.cpp`, `tests/test_agent_session_skills_live_e2e.cpp`
  — rewritten to assert against single-ask convergence (structural facts recorded from inside each
  test tool's own `invoke()`, since the raw intermediate rounds are no longer externally
  observable — see each file's own updated top comment for what narrowed and why).
- `tests/test_agent_session_tool_call_loop.cpp` (new) — this ADR's §5 evidence.
- `tests/CMakeLists.txt` — registers the new test target.

## 7. What this ADR does not claim

Session-scoped stateful tools (CodeAct/skills needing a persistent interpreter or mounted-skill
state reachable from `Tool<>::invoke()`) and suspend-for-real-human approval are unchanged,
unimplemented, and explicitly out of scope — both were named as separate future design passes
before this ADR's design work started, not discovered as gaps partway through. `agent.spawn` wiring
and 026 Q1's cost-budget design remain their own, separate, not-yet-built work — this ADR removes
their blocking prerequisite (a real `AgentSession`-owned tool-call loop) but does not itself build
either.
