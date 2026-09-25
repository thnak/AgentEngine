# ADR-182 — An MCP test driver for running AgentEngine sessions

- **Status**: **Proposed — design pass + red-team pass 1 (2026-09-25), revised (§12). Owner
  delegated Q1–Q4 (§11). P1 and P2 (§13) and driver phase 1 (§14) implemented and proven,
  including an end-to-end run by a headless Claude tester. Live mode (§15) built early: a Claude tester drove the engine's agent running on live DeepSeek, 4/4. Scenario export/replay (§16) done: 7 checked-in scenarios (4 recorded from live DeepSeek) replay offline as ctests, 100/100 each. C9 (Inspector CLI) and C2 under TSan (§17) proven with controls; headless recipe written.** Where §12
  and an earlier section disagree, §12 wins.
- **Date**: 2026-09-25
- **Origin**: live-provider tests drive a real `rt::AgentSession` by sending a free-form prompt and
  hoping the model does what the test needs, e.g. "You must call BOTH get_weather … AND send_message"
  (`tests/test_rt_agent_session_hitl_live_e2e.cpp:477`). When the model doesn't comply, the test can only
  downgrade the check to a printed note (`:502`). `tools/farm_ops_interactive_cli.py` exists only to find
  out *why* a live run failed. Project-owner direction (2026-09-25): build a first-class automation-test
  surface, shaped like Playwright MCP, that a Claude Code sub-agent can use to drive a running session.
- **Research**: `docs/research/2026-09-25-agent-test-driver-mcp.md` (Playwright MCP, MCP 2026-07-28,
  Claude Code as MCP client, deterministic agent-testing prior art; all cited).
- **Implements / completes**: 022 §3 (golden traces), §6 (`TestKit`: mock providers, "drives approvals
  and input requests programmatically"). The Quark `TestKit` was removed by ADR-037 and nothing replaced
  it.
- **Reuses**: `ReplayChatClient` / `RecordingChatClient` / `ChatCallRecording` (`core/`),
  `AgentSession::{start_run, resolve_interaction, open_interactions, set_run_event_tap, history,
  snapshot_record, fork_from, cancel}`, `WorkflowSupervisor`'s equivalents, `mcp/json_rpc.hpp` envelope
  types, `rt::ThreadPool`, `agent_yaml_compiler.hpp` (RFC 015).
- **Bound by**: ADR-039 / ADR-061 (the engine binds no listener; the host owns transport), ADR-070
  (Delegated Decision Seam), ADR-071 (native unsandboxed providers are explicit opt-in).

## 0. What exists today (read from the code)

| Need | Today |
|---|---|
| Drive a run and answer HITL | Yes, in-process: `start_run(StartRun)`, suspension codes `run.suspended_for_{approval,codeact_ask,hook_decision}`, `resolve_interaction(ResolveInteraction)`, `open_interactions()` (`rt/agent_session.hpp`). |
| Observe a run | Yes: `set_run_event_tap()` (callback, parallel to the stream) and `enable_event_stream()` (single consumer; a second call replaces the first). `run_event_kind` covers lifecycle, model calls, tool calls, sandbox exec, interactions, warnings, policy decisions. |
| Know **what** an approval is for | **No (BUG-3).** `Interaction` (`core/interaction.hpp:57`) holds `interaction_id, run_id, reason, opened_at_ns, expires_at_ns`. `run_event_payload::ApprovalRequested` (`core/run_event.hpp:187`) holds `call_id, interaction_id`. Neither has the tool name or arguments. The only route is `tool_calls_of(history().back())`. |
| Deny one call without denying others | **No (BUG-2).** `resolve_interaction`'s deny branch applies `make_denial_result(…, "denied by operator", …)` to every call in `pending_calls` (`rt/agent_session.hpp:1190-1196`), including calls that never needed approval. |
| Accurate approval events | **No (BUG-1).** `approval_requested` is emitted for every call in the suspending round, not only gated ones. |
| Scripted model | **No shared one.** `ScriptedChatClient` is written separately in about 40 test files. `ReplayChatClient` plays one recording per instance. |
| Host that loads an agent from YAML | **No.** `agent_yaml_compiler.hpp` is used only by tests. |
| Machine-drivable host | **No.** `tools/cli_chat.cpp` reads stdin and asks y/N on stdin. It runs each turn as a job on its own `rt::ThreadPool pool(1)` (`:1190`), which is the threading model to copy. |
| Engine MCP server role | `protocol/mcp/server.hpp` `McpServer` exposes a **ToolTable**, not a session, and has no stdio transport. Not a fit (§2C). |

## 1. The question

How does a test agent (a Claude Code sub-agent) drive a running AgentEngine session — send turns,
decide what the model says, observe events, answer approvals, inspect and fork state, and assert —
so that:

- the LLM is never the source of test flakiness,
- every exploratory run can become a deterministic, checked-in test that runs without Claude or a network,
- and nothing about it weakens I2 (no ambient authority), I3 (model output never decides permissions),
  I4 (effects attributable), I5 (nondeterminism recorded), or ADR-039 (no engine listener)?

## 2. Competing designs, steelmanned

### 2A. A Python MCP server that drives `cli_chat` as a subprocess

Wrap `agentengine_cli_chat` in a small Python MCP server (the `farm_ops_interactive_cli.py` stack) that
writes turns to its stdin and parses its stdout.

- **For:** fastest to build; no C++ changes; Python MCP SDKs are mature.
- **Against:** `cli_chat` has no scripted-model mode and no structured output, so the driver would be
  screen-scraping prose, the very kind of fuzzy assertion this ADR exists to remove. Approvals are a
  blocking y/N on the same stdin as turns. There is no snapshot, fork or event access. Every missing
  feature would have to be added to `cli_chat` as a text protocol and then parsed back out.
  **Rejected.**

### 2B. A C++ host executable, in-process with `AgentSession`, speaking MCP over stdio (**chosen**)

A new program, `tools/agentengine_test_driver`, links the engine the way `cli_chat` does. It owns its
own stdio MCP loop and holds one or more sessions in-process.

- **For:** every seam in §0 is directly callable. The scripted model is a real `ChatClient` in the same
  process. Events come from `set_run_event_tap`. Snapshots come from `snapshot_record()`. The engine
  library itself still binds nothing, so ADR-039/061 are untouched: this is host code, like `cli_chat`.
- **Against:** a new, small JSON-RPC/stdio loop in C++ (the envelope types already exist in
  `mcp/json_rpc.hpp`; framing is newline-delimited JSON). It is also a new place that answers approvals
  programmatically, so §4 has to argue I2/I3 explicitly.

### 2C. Add session tools to the engine's own `McpServer`

Register `session_send`, `interaction_resolve` and so on as tools on the production `McpServer`.

- **For:** reuses `dispatch()`; one MCP implementation.
- **Against:** it puts test-only powers (script what the model says, resolve approvals, fork, dump
  history) on the **production** protocol surface. Its host-fronted server role still has open
  red-team findings (`tools/mcp_conformance_client.cpp:11`). A flag that removes these tools in
  production is exactly the "test surface leaks to prod" failure mode ADR-039 warns about.
  **Rejected.**

### 2D. No MCP; a CLI invoked once per step

Microsoft's own Playwright README notes that CLI calls are more token-efficient than MCP.

- **For:** fewer tokens; trivially scriptable.
- **Against:** a session is live in-process state (a suspended coroutine, open interactions). A CLI
  invoked per step needs a daemon holding that state, which is an MCP server with a worse protocol.
  **Rejected as the driver. Kept for replay:** `agentengine_scenario_runner` (§3.8) *is* the CLI path,
  for CI.

## 3. The design (2B)

### 3.1 Process, transport, build isolation

- stdio only; MCP revision 2026-07-28: `server/discover`, `tools/list` in deterministic order,
  `tools/call`. No `initialize`, no protocol sessions (SEP-2567). State is carried by server-minted
  handles (`session_id`) passed as tool arguments.
- No `--port`/HTTP mode in v1. Adding one reopens ADR-039-class questions and needs its own ADR.
- Built only when `AGENTENGINE_BUILD_TEST_DRIVER=ON` (default OFF). Never installed, never part of the
  `agentengine` library target, never linked by `examples/`. A CMake check fails the configure if
  anything other than the driver and its tests links the driver's sources.

### 3.2 Fixtures and authority

- The driver is started with `--fixtures-root <dir>` (host side, fixed for the process lifetime).
- `session_start {fixture}` names a file under that root. A path that resolves outside the root is
  refused, after symlinks and `..` are resolved. The fixture declares:
  - the agent (RFC 015 YAML),
  - its tool set (from a driver-side `ToolRegistry` of test tools plus opted-in real tools),
  - the capability grant,
  - the sandbox profile,
  - the model mode (§3.3),
  - the suspend-for-approval setting.
- **The capability grant comes only from the fixture file.** No tool argument can name, add or widen a
  capability. The test principal (`SessionCaller{id: "test-driver/<fixture>", tenant_id: "test"}`) is
  host-assigned per session.
- The driver refuses a fixture that requests an ADR-071 native unsandboxed provider or a `none` sandbox
  profile unless it was started with `--allow-unsandboxed`. This follows the CLAUDE.md "machine safety"
  rule: real tools run for real under the default profile and resource caps.

### 3.3 Model modes (per session, from the fixture)

| mode | chat client | purpose | start-up condition |
|---|---|---|---|
| `scripted` (default) | shared strict `ScriptedChatClient` (P2): FIFO of turns, captures every request; **an empty queue fails the model call** with `test.script_exhausted`, never a fallback reply | gating tests | none |
| `replay` | sequencer over `ChatCallRecording`s (P3); a request that doesn't match the next recording's request digest fails with `test.replay_mismatch` | regression from a cassette | recordings exist |
| `live` | real provider wrapped in `RecordingChatClient` writing a cassette | exploration only; never gating | driver started with `--allow-live` and a key file path |

### 3.4 Tool surface (grouped; groups enabled with `--caps`)

Every result leads with a short text rendering (target ≤ 8k tokens, well under Claude Code's 10k
warning) and carries `structuredContent` as a bonus. Claude Code's use of `structuredContent` is
undocumented, so nothing depends on it.

**core** (always on)
- `session_start {fixture, label?}` → `session_id`, snapshot
- `session_send {session_id, text}` → `run_id`; returns at once, and the run proceeds on the session's worker (§3.6)
- `session_wait_for {session_id, until, timeout_ms ≤ 60000}`
  - `until` is one of `run_finished | run_suspended | run_idle | event{kind, field_match?} | tool_called{name}`
  - returns the matching events and a snapshot, or `timed_out` with the events seen so far
  - each call stays under Claude Code's 2-minute auto-background threshold
- `session_snapshot {session_id, include?}` (§3.5)
- `session_events {session_id, since_seq, kinds?, limit?}`
- `interaction_list {session_id}` → pending interactions with reason, tool name, arguments digest and
  arguments (needs P1)
- `interaction_resolve {session_id, interaction_ref, decision: approve|deny, call_refs?, answer?}`
- `session_cancel {session_id}`, `session_close {session_id}`

**script**
- `model_script_push {session_id, turns: [{text?, reasoning?, tool_calls?: [{name, arguments}], finish?, error?, chunks?}]}`
- `model_requests {session_id, since?}` → exactly what the engine sent the model (system prompt, tool
  descriptors, history), with secrets redacted (§4.4)

**state**
- `session_fork {session_id, at_turn?}` → new `session_id` (uses `fork_from`)
- `session_save {session_id, name}` and `session_restore {name}` (snapshot store under a driver-owned directory)

**testing**
- `assert {session_id, checks: [...]}`
  - structural predicates only: tool sequence, event-kind order, "approval resolved before
    `tool_call_started` for call X", state key equals, no `model_output_discarded`, usage ≤ N
  - no prose or LLM-judge predicates; those belong to 022 §4 evaluation, not here
- `scenario_export {session_id, name}` (§3.8)

**workflow**
- `workflow_start`, `workflow_wait_for`, `request_port_list`, `request_port_resolve` over
  `WorkflowSupervisor`, with the same shape. Phase 5.

### 3.5 Snapshot and refs

- A snapshot is a compact projection, not `agent_session_record_to_json`:
  - run state (`idle | running | suspended(reason)`), last run id and turn index
  - per turn: the user text (truncated), the assistant text (truncated), and tool calls with status
  - open interactions
  - usage and budgets
  - the last N event kinds
- Refs are stable, driver-assigned and never reused within a session: `turn#3`, `call#7`, `ix#2`,
  `ev#41`. Tools accept refs, not raw ids, so a model can't mistype a UUID.
- `include: [history]` returns full history. `include: [record]` returns `snapshot_record()` JSON for
  deep inspection.

### 3.6 Threading: I1 is kept by construction

- Each driver session owns one `rt::ThreadPool(1)`, the `cli_chat` model.
- **Every** call that touches the session (`start_run`, `resolve_interaction`, `history`,
  `snapshot_record`, `fork_from`, `open_interactions`) is submitted as a job to that session's one
  worker. The MCP loop thread never calls into `AgentSession` directly.
- Consequence: while a run is in flight, a job that needs session state queues behind it. So
  `session_snapshot` on a running session returns only what the event tap has seen (a thread-safe,
  bounded, `seq`-ordered buffer the tap appends to), marked `partial: true`. A full snapshot is
  available once the run is idle or suspended. This is honest, not a limitation to hide: reading
  `history_` mid-run from another thread would be a data race.
- `session_cancel` calls `cancel()` from the MCP thread. The ADR-178 contract is a `std::stop_source`,
  which is safe to signal from another thread; phase 1 proves this with a test rather than assuming it.
- The event buffer is bounded (default 10,000 events per session). When it overflows, the oldest events
  are dropped and a `warning` event records the gap, so `session_events` reports a missing `seq` range
  instead of silently skipping it.

### 3.7 Interactions

- `interaction_list` reads `open_interactions()` and, after P1, the tool name and arguments from the
  interaction itself. Until P1 lands, the driver does **not** work around it by parsing
  `history().back()`: a test surface that re-derives state through an undocumented convention would
  test the workaround, not the engine. P1 is a prerequisite, not optional.
- `interaction_resolve` builds `ResolveInteraction{interaction_id, approved, caller = the session's
  test principal, answer}`. `call_refs` (a per-call decision within one round) is accepted only once
  P1b (the BUG-2 fix) lands; before that, a call with `call_refs` fails with `test.unsupported` rather
  than silently denying everything.

### 3.8 Scenario export and the replay runner

- `scenario_export` writes `tests/scenarios/<name>/`:
  - `scenario.json`: fixture ref, ordered steps (send / script push / resolve / cancel / fork / wait
    conditions), and the assertions that were run
  - `model/`: for `scripted` sessions the pushed turns; for `live`/`replay` sessions the cassette
  - `expected-events.jsonl`: the event stream normalized per 022 Q3 (`<TIMESTAMP>`, `<RUN_ID>`, and the
    rest of that list)
- `agentengine_scenario_runner <dir>` replays a scenario in-process with **no network and no Claude**:
  it refuses `live` mode and needs no `--allow-live`. It then diffs the normalized event stream and
  re-runs the assertions. One ctest per scenario, label `scenario`, part of the deterministic suite (022
  §1: gating).
- A scenario whose model was `live` replays through its cassette. That is I5 in practice: a confusing
  live run becomes a fixed regression test.

### 3.9 Claude Code side (checked in, not code)

- `.claude/agents/agentengine-tester.md`:
  - `tools:` limited to `mcp__agentengine_test__*` plus Read/Write/Glob scoped by instructions to `tests/scenarios/`
  - an inline `mcpServers` entry launching the driver with `--caps core,script,testing`
  - `maxTurns` set
- Headless use:
  `claude --bare -p --strict-mcp-config --mcp-config .claude/test-driver.mcp.json --output-format stream-json --verbose`.
  The job fails if `system/init` reports any `mcp_server_errors`.
- The sub-agent's job, stated in its prompt: write scenarios, run them through the driver, export the
  passing ones, and report failures with the event-stream diff. It is the *tester*; it never plays the
  model under test except by scripting it explicitly with `model_script_push`.

## 4. Invariants

### 4.1 I3 — "an LLM answers approvals" is not model output deciding permissions

This reads like a violation and is not one. The argument, stated so the red-team can attack it:

- I3 concerns the **agent's own model output** (the session's `ChatClient` stream). Nothing derived
  from that stream reaches the approval decision in this design: `interaction_resolve`'s `decision`
  comes from the MCP caller, and the session's model output is only *observed* by it.
- The MCP caller is a **host-side operator** occupying exactly the position of the human at `cli_chat`'s
  y/N prompt, or the host `ApprovalDecider` callback. That the operator happens to be an LLM (Claude)
  doesn't change the architecture: the engine already allows any host code to be the decider (ADR-070's
  seam: host opt-in, decides among already-possessed authority, audited).
- **Where the argument is weakest:** in `live` mode, the session's model output is shown to the
  operator LLM, which then approves. A prompt-injected tool result could persuade the tester to approve.
  Mitigations:
  - `live` mode needs `--allow-live`;
  - fixtures in `live` mode are refused unless their tool set is test tools only, with no real effectful
    tools (Q2);
  - the scenario that results is replayed deterministically, so a manipulated approval shows up as a
    diff in review.

  This residual is named in §9, not claimed closed.

### 4.2 I2 — no ambient authority

- Capabilities come only from the host-side fixture (§3.2). No tool argument carries a grant or a path
  outside the fixtures root.
- `model_script_push` can make the model *ask* for any tool call; the engine's normal capability check
  and approval gate still decide. That is the point of the test.

### 4.3 I4 — attributable

- Every session's calls carry the host-assigned test principal.
- The driver writes an append-only action log per session (`driver-actions.jsonl`: MCP tool, arguments
  digest, result code, `seq` range), exported with the scenario.

### 4.4 Secrets

- `live` mode reads keys from a key-file path given on the command line (the
  `run-live-provider-tests.ps1` format), never from tool arguments or fixtures.
- Snapshots, events, `model_requests`, cassettes and scenario exports pass through the engine's
  redaction path. A planted key string must never appear in any driver output (claim C6, with a
  positive control).

## 5. What this deliberately does not do

- It doesn't make live-provider tests gating. 022 §1 stands: live runs are quarantined.
- No LLM-judge or simulated-user assertions. They are 022 §4 evaluation, non-gating; cited research
  finds simulated users unreliable proxies.
- No HTTP transport, no multi-client access, no remote driving.
- It doesn't replace the C++ unit/simulation tests. Scenarios cover the integration seam, with golden
  event streams.
- It doesn't work around BUG-1/2/3 inside the driver.

## 6. Prerequisites (engine-side, each useful on its own)

- **P1 (BUG-3):** carry `tool_name` and an argument payload (or a digest plus accessor) on the approval
  surface, via `Interaction` or `ApprovalRequested`. Which of the two, and how arguments are redacted
  for display, is P1's own design question. Small; likely its own ADR-lite.
- **P1b (BUG-1/BUG-2):** emit `approval_requested` only for gated calls; support per-call decisions in
  one round. Changes `ResolveInteraction`'s semantics, so it needs its own ADR. Not blocking phase 1–2
  (the driver refuses `call_refs` until it lands).
- **P2:** a shared strict `ScriptedChatClient` (streaming and unary, chunk boundaries, request capture,
  exhaustion fails loudly) under `include/agentengine/testing/`. Existing ad-hoc copies migrate
  opportunistically.
- **P3:** a multi-recording replay sequencer over `ReplayChatClient`, with request-digest matching.
- **P4:** a host-side fixture loader: agent YAML + tool registry + grant + sandbox profile → configured
  `AgentSession`. Also useful to `cli_chat`.

## 7. Build phases

1. P1 + P2. Driver skeleton: stdio loop, `server/discover`, `tools/list`, core + script groups,
   `scripted` mode, one hard-coded fixture. Tests for claims C1–C4.
2. P4 fixture loader, snapshot/refs, `assert`, `scenario_export`, `agentengine_scenario_runner`, ctest
   label `scenario`. Claims C5–C7.
3. `.claude/agents/agentengine-tester.md` and the headless recipe; convert 2–3 existing live HITL/
   multitool cases into scripted scenarios as the first corpus.
4. P3 + `replay`/`live` modes + cassette export. Claim C8.
5. Workflow group; P1b and `call_refs`.

## 8. Falsifiable claims (each with a disproving test; positive control noted)

- **C1 — scripted exhaustion is loud.** A scenario that sends two turns with one scripted reply fails
  with `test.script_exhausted`. *Positive control:* a client that falls back to an empty reply makes
  this test fail.
- **C2 — I1.** A stress test issues `session_send`, `session_snapshot` and `session_events` concurrently
  from the MCP thread for 1,000 iterations under TSan (Linux) with no race report. *Positive control:*
  a build flag that lets `session_snapshot` read `history()` from the MCP thread produces a TSan report.
- **C3 — no grant through tool arguments.** Fuzzed `tools/call` arguments (including extra fields
  named like capabilities, paths with `..`, and absolute paths) never change the session's
  `CapabilitySet`, and never open a fixture outside the root.
- **C4 — approval ordering is observable.** In a scripted round with one gated tool: `interaction_list`
  shows the tool name and arguments (P1); deny produces no `tool_call_started` for that call; approve
  produces `approval_resolved` strictly before `tool_call_started`.
- **C5 — replay determinism.** Every exported `scripted` scenario replays 100/100 under
  `agentengine_scenario_runner` with a byte-identical normalized event stream (022 G2).
- **C6 — secrets never leave.** With a planted key string in the key file and in an env var, no MCP
  result, event, `model_requests` output, cassette or export contains it. *Positive control:* disabling
  redaction makes the test find it.
- **C7 — regression detection.** Reintroducing BUG-2 after P1b lands (a deny that also denies an
  ungated call) makes a checked-in scenario fail. This is the positive control for the whole approach.
- **C8 — I5.** A `live`-mode scenario's cassette replays with the network disabled, and diverging
  from the recorded request fails with `test.replay_mismatch`.
- **C9 — contract.** `@modelcontextprotocol/inspector --cli` runs `server/discover`, `tools/list`
  (checking ordering is stable across two runs) and one approve round trip, with exit code 0.

## 9. Residuals (named now)

- **R1 — live-mode tester manipulation (§4.1).** A prompt-injected live tool result can steer the
  operator LLM's approval. Mitigated (opt-in, test-tools-only fixtures, deterministic replay review),
  not closed.
- **R2 — partial snapshots mid-run (§3.6).** By design; a test that needs mid-run state must use events.
- **R3 — driver JSON-RPC loop is new parsing code.** It runs locally over stdio against a trusted
  parent process, so the exposure is low, but it is still hand-written parsing: fuzz it in phase 1.
- **R4 — Claude Code client behaviour is not under our control.** Output limits, timeouts and
  `structuredContent` handling are as documented 2026-09-25 and may change. Re-verify when the client
  version moves.
- **R5 — fixture tools vs real tools.** Scenarios using driver test tools prove the engine's loop, not
  the real tools' effects. Real-tool scenarios need `--allow-unsandboxed` or the default sandbox and
  are slower.

## 10. Owner questions

- **Q1 — scope of phase 1:** agent sessions only, with workflows in phase 5 (proposed), or both from
  the start?
- **Q2 — live mode and real tools:** refuse effectful real tools in `live` mode entirely (proposed,
  closes most of R1), or allow them behind a second flag?
- **Q3 — scenario format:** JSON (proposed; matches the existing `ChatCallRecording` codec) or YAML
  (matches RFC 015 fixtures)?
- **Q4 — P1b ordering:** fix BUG-1/BUG-2 before the driver ships (cleaner first corpus), or after
  (proposed: the driver refuses `call_refs` until then, and C7 then becomes the regression test for it)?

## 11. Owner decisions (delegated, 2026-09-25)

The project owner delegated this ADR's decisions ("that feature is our goals so you can run without
me"). Taken with the proposed defaults, tightened by red-team pass 1:

- **Q1:** agent sessions only through phase 4; workflows in phase 5.
- **Q2:** stronger than proposed. Through phase 3 a driver session can only use the driver's own
  built-in test tools (in-process, no real effects). Real tools come later, sandboxed, and never in a
  session whose approvals the tester answers while `--allow-unsandboxed` is set (§12 C-2).
- **Q3:** JSON.
- **Q4:** after. The driver refuses per-call decisions until BUG-2's own ADR lands.

## 12. Red-team pass 1 (2026-09-25, `general-purpose` agent, no prior context) and the revisions

Result: 4 Critical, 11 Real gaps, 8 Minor. The architecture held up: host executable, stdio only, one
`rt::ThreadPool(1)` per session, suspend-mode approvals. The reviewer confirmed from the code that:

- a suspended run frees its worker: `run_rounds` returns `kSuspendedForApproval` through the error
  channel, so the job ends and `resolve_interaction` can run as the next job;
- `cancel()` is safe from any thread;
- BUG-1/2/3 are real.

Every finding is accepted unless stated otherwise.

### Critical

- **C-1. C4 contradicted the code.** On approve, `resolve_interaction` dispatches the tools first and
  emits `approval_resolved` afterwards (`agent_session.hpp`, approved branch). *Revision:* C4 is
  restated against today's order (below). Moving `approval_resolved` ahead of dispatch changes 013's
  event order, so it becomes **P1c**, with its own ADR; C4's stronger form waits for it.
- **C-2. §4.1's I3 argument was unsound, and cited ADR-070 against itself.** ADR-070 §4 property 4
  says only "host code, never model output" may occupy the decision seam. An LLM tester reads tainted
  content (tool results, and tool arguments marked `arguments_tainted=true`) and then approves. That
  is model-derived in every mode, not only `live`. *Revision:* §4.1 is replaced by this statement:
  - **A tester's approval is model-derived and is not claimed to satisfy I3.**
  - It is made harmless by scope, not by argument: an approval in a driver session can only authorize
    effects confined to the test environment.
  - Through phase 3 the only tools a driver session has are the driver's built-in test doubles
    (in-process, no filesystem, network or process effects), so there is nothing real for an approval
    to authorize.
  - When real tools arrive (phase 5), the driver refuses a suspend-mode session whose fixture wants
    `--allow-unsandboxed`.
  - Residual R1 is widened to "any session whose tools return untrusted content".
- **C-3. Fixture authority was enforced by prompt instructions only.** The tester could write its own
  fixture with any grant, edit its own agent definition to add `--allow-*` flags, or read the key file.
  *Revision:*
  - Phase 1 has **no fixture files**. Fixtures are compiled into the driver, and `session_start`
    names one. No tool argument and no file the tester can write changes a session's tools or grant.
  - When P4 (file fixtures) lands, the driver accepts only fixtures that are git-tracked and
    unmodified (`git ls-files --error-unmatch` plus a clean `git diff` for that path), checked at
    `session_start`.
  - The tester agent definition carries Claude Code `permissions.deny` rules for `Write`/`Edit` on
    the fixtures root, on `.claude/**` and on the key-file path. Its launch flags live in a file the
    tester can't edit.
  - New claim C3b: a fixture the tester wrote or modified is refused.
- **C-4. Output paths were unconfined tool arguments.** `scenario_export {name}` and
  `session_save/restore {name}` allowed `../` writes. *Revision:* fixed `--scenarios-root` and
  `--snapshot-root` on the command line; every `name` must match `[a-z0-9_-]{1,64}`; C3's fuzz test
  covers them.

### Real gaps

- **R1. Threading, stated precisely.**
  - I1 is enforced by the session's own `session_mutex_`. The per-session pool protects the *unlocked*
    accessors (`history()`, `open_interactions()` which returns a reference, `state()`), and the
    driver copies those values inside the job.
  - The run-event tap is installed once at configuration time, before the first job. It can be called
    from ADR-160 parallel-batch threads, not only the worker. So the driver's buffer is
    mutex-protected, never blocks on I/O, and never calls back into the session.
  - `RunEvent::seq` counts per run, so the driver assigns its own session-wide sequence number inside
    the tap.
  - The threading model comes from `cli_chat`, but its approval model (a blocking y/N decider on the
    worker) does not.
- **R2. `session_cancel` on a suspended session.** Cancel alone leaves the interaction open and
  `start_run` blocked, and ADR-178 §2 notes an approved tool would still run once after
  cancel-then-resume. *Revision:* on a suspended session, `session_cancel` = `cancel()` plus a deny of
  every open interaction, as one job. On a running session it is `cancel()` only.
- **R3. `interaction_list` must not depend on evictable events.** *Revision:* the driver keeps a
  per-interaction index (call id, tool, arguments, needs_approval), filled from `approval_requested`
  in the tap and kept separate from the event ring. It is bounded by the open interactions and cleared
  on resolve. One interaction can cover several calls, so the list is per call.
- **R4. `fork_from` does not produce a usable session.** It doesn't copy capabilities, the chat
  client, the suspend flag, the tap or the deciders. It also clears open interactions and takes a
  message count, not a turn. *Revision:* `session_fork` moves to phase 5, specified as:
  - rebuild the target from the fixture and give it a fresh `ScriptedChatClient`;
  - refuse to fork a suspended session or at a mid-round point;
  - map turns to message indices;
  - run `fork_from` as a job on the source's worker.
- **R5. Byte-identical replay needs more than §3 said.** *Revision:*
  - gating scenarios use only deterministic test tools (no `Parallelizable`, no `Backgroundable`);
  - session ids are minted `s1, s2, …`, so run and interaction ids are counter-derived and stable;
  - every step is anchored to an event predicate, never to wall-clock timing;
  - `model_script_push` is accepted only when the session is idle or suspended;
  - recorded tool doubles for real tools become **P5** (022 §3's "recorded nondeterminism").
- **R6. Normalization list.** `RunEvent` carries no timestamps, so the noise is ids. The normalizer
  (phase 3) rewrites session, run, interaction and call ids to their order of first appearance
  (`<SESSION_1>`, `<RUN_1>`, …) and drops `exec_id`s. With deterministic session ids most of this
  changes nothing. It still runs, so a future non-deterministic id shows up as a diff instead of
  passing silently.
- **R7. P3 is mostly built.** `ReplayChatClient(std::vector<ChatCallRecording>)` already sequences
  recordings and fails with `sequence_exhausted` (ADR-177), so §0's "one recording per instance" was
  wrong. *Revision:* P3 is now only request matching. The digest is the canonical JSON of each
  message's role and content, plus tool names and argument schemas, plus `output_schema_json`. The
  runner injects a no-op `SleepFn`.
- **R8. There is no engine-wide secret scrubber.** *Revision:* new **P6**, a driver-side output canary
  filter. Every MCP response is checked for the key-file bytes and for the values of the env vars the
  key file names, and a hit replaces the response with `test.secret_leak_blocked`. Only needed from
  the phase that adds `live` mode.
- **R9. Driver resource caps.** *Revision:*
  - `--max-sessions`, default 8;
  - a bounded request-capture ring in the driver (the shared client's unbounded capture stays, for
    unit tests);
  - a 4 MiB cap per input line;
  - an event ring of 10,000 per session.
- **R10. Close, disconnect and shutdown could hang**, because `~ThreadPool` joins in-flight jobs and
  cancel is cooperative. *Revision:*
  - `session_close` = cancel, deny open interactions, join with a timeout, and report
    `test.close_timed_out` if the timeout expires;
  - on stdin EOF: a bounded drain, then exit;
  - each session's pool is declared after the session, so it is destroyed first, and the event buffer
    outlives both;
  - phase 1 has no real tools, so the join always completes.
- **R11. The build-isolation check tested the wrong thing.** *Revision:*
  - a lint (phase 2) fails if anything under `src/`, or any public header outside `testing/`,
    includes `agentengine/testing/`;
  - the install rules exclude that directory;
  - the driver and scenario runner build with the tests (no separate default-OFF option), so scenario
    tests run in the default suite.

### Minor (accepted)

- Also answer `initialize` (pre-2026-07-28 clients), not only `server/discover`, until C9 proves which
  one Claude Code sends.
- §2C's rebuttal, sharpened: reusing the `McpServer` *class* privately still doesn't fit. Its
  `tools/call` runs engine `Tool`s through `invoke_tool` with a `CapabilitySet` and an approval
  decider, which is the wrong shape for driver control tools.
- Windows stdio: `_setmode` binary on stdin and stdout. The MCP stream uses a duplicated stdout
  handle, and fd 1 is redirected to stderr at startup, so no stray write corrupts the protocol stream.
- Phase 6 note: `agent_session_as_executor_body` resets the tap around each run, which would erase
  the driver's tap on a session inside a workflow.
- I4: `ApprovalResolved` carries no approver identity. The driver's action log records it meanwhile.
  Named below as residual R6.
- C2's positive control uses a test tool that blocks on a latch, so the racing read is guaranteed to
  overlap a live run.
- Large results (`include:[history]`) are paginated.
- C5 cites 022 §3, not G2.

### Revised claims (replace §8's C4; add C3b and C10)

- **C4 (restated).** In a scripted round with one gated tool:
  - `interaction_list` shows the call's name, arguments and `needs_approval`;
  - a deny produces no `tool_call_started` for that call;
  - an approve produces exactly one `tool_call_started` for it, after its `approval_requested`.

  The "resolved before started" form waits for P1c.
- **C3b.** A fixture that is not one of the compiled-in set is refused (phase 1). A file fixture that
  is untracked or modified is refused (from P4).
- **C10.** `session_cancel` on a suspended session leaves no open interaction and invokes no gated
  tool, and a following `session_send` is accepted.

### Additional residuals

- **R6 — approver identity** is only in the driver's action log, not in the engine's audit (I4,
  ADR-070 property 5).
- **R7 — P1 put the data on the event, not on `Interaction`.** A consumer that joins after the event
  can't learn from the engine alone what an interaction is for. The driver's own index covers this;
  other consumers have no such index.

### Revised build phases (replaces §7)

1. **Done:** P1 (tool name, arguments and needs_approval on `approval_requested`) and P2 (shared
   strict `ScriptedChatClient`). See §13.
2. Driver phase 1:
   - the stdio loop (`initialize` + `server/discover`, `tools/list`, `tools/call`);
   - compiled-in fixtures and test tools;
   - the core and script tool groups;
   - the tap buffer and interaction index;
   - cancel and close semantics;
   - the `testing/` include lint.

   Claims C1, C2, C3, C3b, C4 and C10.
3. `assert`, the normalizer, `scenario_export`, `agentengine_scenario_runner`, and ctest label
   `scenario`. Claims C5, C9, and C7 once BUG-2's ADR lands.
4. The tester agent definition and headless recipe; convert 2–3 live HITL/multitool cases.
5. P4 file fixtures (git-tracked check), P5 tool doubles, sandboxed real tools, `session_fork`, P3
   matching, and `live` mode with P6. Claims C6 and C8.
6. Workflows; P1b and P1c.

## 13. P1 and P2 — done (2026-09-25)

- **P1.** `run_event_payload::ApprovalRequested` gains `tool_name`, `arguments_json` and
  `needs_approval`, appended last (`core/run_event.hpp`). Both emit sites in `rt/agent_session.hpp`
  fill them.
  - The arguments are the ones the approval check judged: the post-hook request's when a tool-call
    hook ran, otherwise the model's text.
  - `run_rounds`'s pre-check loop no longer stops at the first gated call, so it can record
    `needs_approval` for every call.
  - BUG-1's semantics are deliberately unchanged: the event still fires for every call in the round.
- **P2.** `agentengine::testing::ScriptedChatClient`
  (`include/agentengine/testing/scripted_chat_client.hpp`):
  - a FIFO of turns with a thread-safe `push`, and request capture;
  - a call made with the script empty fails with `scripted_chat_client.script_exhausted`;
  - `chat_stream()` pushes synchronously and puts usage on the last content update, because
    `drain_chat_stream` appends every update's delta;
  - builders: `text_turn`, `tool_calls_turn`, `failure_turn`.
- **Proof:** `tests/test_scripted_chat_client_and_approval_payload.cpp`, 22/22 checks.
  - SC2 has a positive control: the repeat-last policy that every ad-hoc copy uses answers the extra
    call, so the check can fail.
  - AP2 proves a mixed round reports `needs_approval` true for the gated call and false for the free
    one.
  - The first run caught a real bug in the P2 draft: a separate trailing final update added an empty
    content item to every streamed reply.

## 14. Driver phase 1 — done (2026-09-25)

- **Code:**
  - `tools/test_driver/test_driver.hpp` holds the `Driver` class: JSON-RPC dispatch, tools, sessions,
    `SessionMonitor`.
  - `tools/agentengine_test_driver.cpp` is only the stdio pump. It duplicates the real stdout for the
    protocol stream, points fd 1 at stderr, and sets binary mode on Windows.
  - The target builds by default and links only `agentengine::core`.
  - `.mcp.json` registers it as `agentengine-test`.
  - `.claude/agents/agentengine-tester.md` is the tester sub-agent. Its tools are the driver's MCP
    tools plus Read/Grep/Glob. It has no Write or Edit, which closes §12 C-3's "tester edits its own
    fixtures or launch flags" for this phase.
- **Tools shipped:** `fixtures_list`, `session_start`, `model_script_push`, `session_send`,
  `session_wait_for`, `session_snapshot`, `session_events`, `interaction_list`,
  `interaction_resolve`, `session_cancel`, `session_close`, `model_requests`.
- **Deferred:** refs (`turn#3` and similar). Ids are already short and deterministic (`s1`,
  `s1:run:1`, `s1:interaction:1`, `call_1`), so refs would add nothing yet.
- **Lint (§12 R11):** `cmake/check_testing_includes.cmake`, registered as ctest
  `lint_testing_includes`. Passes on the tree; a planted `#include "agentengine/testing/..."` under
  `src/` makes it fail (positive control, run by hand 2026-09-25). The project has no install rules,
  so there is nothing to exclude yet.
- **Proof:** `tests/test_agentengine_test_driver.cpp`, 42/42 checks, driving `handle_line()` with the
  exact JSON-RPC lines the binary receives:
  - protocol P1–P5, and C1, C3, C3b, C4 (restated), C10;
  - a mixed round, and the refusals (send while suspended, per-call decisions, too many sessions);
  - C2 in its Windows form: 200 runs observed from the MCP thread while the worker runs, each
    settling with its own scripted text.
  - The first run caught a driver bug: tool replies are `Data` items, and the driver's text
    rendering dropped them.
  - C10 observed: cancel-then-deny ends with `run.canceled`, so ADR-178 §2's "an approved tool would
    still run once" case does not arise on the deny path.
- **End to end (2026-09-25):** `claude -p --mcp-config .mcp.json --allowedTools
  "mcp__agentengine-test__*"` ran three cases against the real binary over stdio: deny, tool error,
  malformed arguments. 3/3 settled as expected, 19 turns, about 35 s.
- **Stdio smoke test:** initialize, notification, start, push, send, wait, list. Output verified by
  hand; nothing but JSON-RPC on stdout, and the banner on stderr.
- **Not yet proven:** C2 under TSan (Linux), C9 (Inspector `--cli`), and whether Claude Code sends
  `initialize` or `server/discover` (both are answered).
- **Full offline suite** after P1: 302/304 passed under `ctest -j 8`. `test_rt_parked_task_home` and
  `test_rt_workflow_supervisor` failed under load and both pass when run alone.

### First finding from the harness

In case 3 of the end-to-end run, the tester scripted an `echo` call with arguments `{not json`. The
engine ran the tool with `{}`, and the model got back "missing required field 'text'". Cause:
`tool_call_request_of()` (`core/tool_call_extraction.hpp`) substitutes an empty object when
`json::parse` fails. That contradicts 006 §3 step 2 ("reject, do not coerce"). A tool whose arguments
are all optional would *run* on malformed model output. Recorded here and not fixed in this ADR: the
fix changes the tool pipeline, so it needs its own change and a scenario that proves it.

## 15. Live mode — done (2026-09-25), ahead of the original phase plan

Project-owner direction (2026-09-25): AgentEngine's own agent should run on a real model (DeepSeek,
key in the git-ignored `deep-seek.txt`) while a Claude sub-agent drives it. This moves §12's phase-5
`live` mode and P6 forward. The rest of phase 5 (file fixtures, real tools, fork, P3 matching) stays
where it was.

- **Seam.** A session's model is a type-erased `ModelBackend` behind one `DriverChatClient`, so one
  `Session` type serves both modes. `ScriptedBackend` wraps the P2 client. `LiveBackend`
  (`tools/test_driver/live_backend.hpp`, compiled only with `AGENTENGINE_WITH_HTTPS`) wraps
  `RecordingChatClient<openai::OpenAIChatClient<InMemorySecretStore>>`. `DriverChatClient` captures
  every request in both modes, in a ring bounded at 64 (§12 R9).
- **Host decisions only.** These command-line flags are fixed for the process:
  - `--allow-live`
  - `--live-key-file` (first line, trimmed)
  - `--live-host` (default `api.deepseek.com`), `--live-path-prefix` (`/v1`), `--live-model`
    (`deepseek-flash`)
  - `--live-max-calls` (default 40 per session; past it every call fails
    `test.live_call_budget_exhausted`, I8)
  - `--record-dir`

  No tool argument can enable live mode, pick the key or change the budget. The live fixtures
  (`basic_live`, `no_tools_live`) are compiled in, and are refused (`test.live_disabled`) unless the
  host enabled live mode.
- **I2.** The key sits in an `InMemorySecretStore` and is resolved at the point of use against the
  session's grant. A live session's grant is exactly `cap::Secret{"test_driver.live_model_key"}`; a
  scripted session's grant is empty.
- **P6 secret canary.** The key is registered as a canary. `Driver::handle_line()` withholds any reply
  containing a canary and returns `test.secret_leak_blocked`, which names no content.
- **I5.** Every live call is written to `--record-dir` as `<session>-call-<n>.json`
  (`ChatCallRecording`), so a confusing live run can be replayed later.
- **Q2 held.** Live fixtures have only the in-process test tools, so a tester's approval of a live
  model's call still authorizes nothing real (§12 C-2).
- **Config.** `.claude/test-driver-live.mcp.json` launches `build-https-driver/agentengine_test_driver`
  with the flags above (12 calls per session).
- **Build note (Windows).** The existing `build-https` cache could not configure: its C compiler is
  `clang-cl` (which the vendored mbedTLS needs, because it passes MSVC flags) and CMake found no `mt`.
  A fresh tree works with `-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang++
  -DCMAKE_MT=<LLVM>/bin/llvm-mt.exe -DAGENTENGINE_WITH_HTTPS=ON`. `build-https-driver/` was
  configured that way (git-ignored).

### Proof

- **Offline** (`tests/test_agentengine_test_driver.cpp`, now 50/50):
  - a live fixture is refused without live mode;
  - with a stand-in live backend (no network), the session starts, refuses `model_script_push`
    (`test.live_session`), is answered by the backend, reports `model: live`, and `model_requests`
    captures the call;
  - C6: with no canary configured, a scripted reply carrying the canary text reaches the client
    (positive control); with the canary configured, the reply is withheld and the block is counted.
- **Live stdio smoke test** (DeepSeek `deepseek-flash`): the real model called `gated_echo` with
  `{"text": "hello from deepseek"}`, the run suspended, and `interaction_list` showed the call. One
  recording was written, containing zero occurrences of the key (checked with `grep -F -f`, key
  never printed).
- **Claude tester driving the live agent.** `claude -p --mcp-config .claude/test-driver-live.mcp.json`
  ran 4 cases, all PASS, in 60 s, 24 turns and 8 DeepSeek calls, all recorded, none containing the
  key:
  - approve: exactly one `tool_call_started`, after `approval_requested`;
  - deny: no `tool_call_started`;
  - tool error: `is_error` with `kaboom`, and a model answer reflecting it;
  - a mixed echo + gated_echo round that the live model really did issue in one round: both events
    fired, with `needs_approval` false and true, and the ungated call was held until the decision.
- **Rediscovered on its own.** Without being told, the tester reported that `approval_resolved` comes
  after the tool ran (§12 C-1 / P1c): "anything that reads the events and expects 'resolved, then
  executed' will see them in the wrong order". That is two independent sources for P1c.

## 16. Scenario export and replay — done (2026-09-25)

The design choice that keeps replay simple: **every scenario replays with the scripted model.** That
holds whether the session was scripted or live.

- `DriverChatClient` logs every model exchange (the response, or the failure in its place) in call
  order (`ExchangeLog`, up to 256; a streamed call marks the session unrecordable).
- On export those exchanges become the scenario's `model_turns`, in the *exact* turn shape: an
  ordered `content` list of text, reasoning and tool calls with raw argument text and call ids, plus
  usage, or an `error` with its class and code.
- On replay the whole list is pushed up front. The script is a FIFO consumed in call order, so no
  step needs to know when the model is called.
- A live DeepSeek run and a hand-scripted run produce the same kind of file, and both replay with no
  network.

### What was built

- **Turn codec.** One format for `model_script_push` input and scenario `model_turns`
  (`parse_turn`, `exchange_to_turn`). The short shape (`text` / `tool_calls`) still works for hand
  scripting; the exact shape adds `content`, `usage` and all five failure classes.
- **Step journal.** `session_send` → `{op: send, text}`. `interaction_resolve` →
  `{op: resolve, interaction_id: "<S>:interaction:N", decision}`. `session_cancel` on a suspended
  session → `{op: cancel}`. A cancel while a run is in flight marks the session non-deterministic,
  because where it lands depends on timing (§12 R5), and export refuses it.
- **Normalization (§12 R6).** Every string that starts with `<session_id>:` becomes `<S>:`. That
  covers run and interaction ids. Call ids need no rewriting, because they come from the recorded
  model turns and are reproduced exactly. `RunEvent` has no timestamps.
- **`scenario_export {session_id, name, description?, overwrite?}`** writes
  `<scenarios-root>/<name>.json`. It holds the fixture (a live fixture maps to its scripted twin,
  `basic_live` → `basic`), `recorded_with` (fixture, model, live model), `model_turns`, `steps`, and
  `expected` {final state, final outcome, every normalized event}.
  - It refuses a session that is running, non-deterministic, unrecordable, has dropped events or has
    no steps.
  - It refuses a name outside `[a-z0-9_-]{1,64}` (§12 C-4), and an existing file unless
    `overwrite: true`.
  - It refuses content that contains a secret canary: scenario files are output too, so the P6 guard
    applies before anything is written.
  - `--scenarios-root` is a host flag (default `tests/scenarios`).
- **`replay_scenario()`** runs a fresh `Driver`:
  - start the fixture, push `model_turns`, then for each step issue it and wait (≤ 60 s) until the
    session settles;
  - compare the full normalized event stream element by element and report the first difference with
    expected and actual, then the final state, the final outcome, and any recorded turns never
    consumed (the replay made fewer model calls).
- **Surfaces.** The MCP tool `scenario_replay {name}`, so the tester confirms its own export. The
  binary `agentengine_scenario_runner <file>...`. One ctest per `tests/scenarios/*.json` (label
  `scenario`, `CONFIGURE_DEPENDS` glob), all in the default build with no HTTPS and no network.

### Proof

- **`tests/test_agentengine_test_driver.cpp`, 67/67.** The new scenario checks:
  - export → `scenario_replay` passes, and `replay_scenario()` (the runner's path) passes twice;
  - three positive controls, each caught:
    - a changed expected event;
    - a changed model-turn argument;
    - a changed step (approve → deny), reported as "event 7 differs";
  - refusals: a path-shaped name, overwriting without the flag, a cancel-while-running session, and
    export with no root;
  - a stand-in live session exports against the scripted twin and replays offline;
  - a scenario containing a canary is not written.
- **First corpus, built by the tester.** A `claude -p` tester (57 turns, $0.82 of Claude plus a few
  cents of DeepSeek) produced 7 scenarios, each confirmed with `scenario_replay`:
  - 4 from live DeepSeek runs: `live_gated_approve`, `live_gated_deny`, `live_tool_error`,
    `live_mixed_round`. In the last one the live model really did issue both calls in one round.
  - 3 scripted: `scripted_parallel_free_calls`, `scripted_model_failure`,
    `scripted_cancel_suspended`.
  - None of the files contains the key (`grep -F -f`).
- **C5 met:** `ctest -L scenario -j 8 --repeat until-fail:100` passes every scenario 100/100 (700
  replays, 34 s).

### Findings from this round

- **New: two error codes for one model failure.** In `scripted_model_failure` the `run_failed` event
  carries `error_code: run.chat_failed`, while the run's result carries `provider.overloaded`. A
  consumer of the event stream (AG-UI, A2A) and a caller of `start_run()` see different codes for
  one failure. The tester also noted that a `transient` model failure was not retried in-session.
  Whether that is expected depends on the session's retry configuration (ADR-177). Recorded here, not
  judged.
- **Locked in on purpose, so it will show up as a diff.** `live_gated_approve` and `live_mixed_round`
  expect today's order: `approval_resolved` after the tool ran (§12 C-1). When P1c fixes the order,
  those two scenarios will fail with an "event N differs" pointing at the move, which is the intended
  signal. They are then re-exported, or edited to the new order, as part of P1c's own change.
- **Still open (§14):** `tool_call_request_of()` coerces malformed arguments to `{}`. Not exported
  as a scenario, because a golden would lock in the bug.

### Revised build phases (replaces §12's list)

1. **Done:** P1, P2 (§13); driver phase 1 (§14); live mode and P6 (§15); scenario export, replay,
   runner and the first corpus (§16).
2. Next: C9 (MCP Inspector `--cli` contract run); C2 under TSan on Linux; the headless recipe in docs
   (tester + live config + `ctest -L scenario`).
3. P4 file fixtures (git-tracked check), P5 tool doubles, sandboxed real tools, `session_fork`.
4. Workflows; P1b (BUG-1/BUG-2) and P1c (event order), each its own ADR. The two-error-code finding
   and the malformed-arguments finding are candidates for the same kind of small ADR.

## 17. C9, C2 under TSan, and the headless recipe — done (2026-09-25)

### C9 — the MCP contract, via the Inspector CLI

- `tools/test_driver/c9_inspector_contract.py` runs `@modelcontextprotocol/inspector --cli` against
  the driver:
  - `tools/list` twice, identical (14 tools);
  - `tools/call fixtures_list`;
  - an approve round trip: `scenario_replay live_gated_approve` (suspend, approve, tool runs, final
    text; 16 events compared).
  Result: PASS, exit 0. *Negative control:* pointing the driver at an empty scenarios root makes the
  round trip fail and the script exit 1.
- **Deviation from §8's wording.** Each Inspector CLI command starts a fresh server process, so an
  approve round trip cannot span several `tools/call`s. It runs inside one call through
  `scenario_replay`. The multi-call round trip over MCP is already covered by the headless Claude
  tester runs (§14, §15).
- **The script found two things.**
  - The Inspector CLI parses any `--flag` after the server command as its own option, so
    `driver --scenarios-root X` silently lost the flag and the driver fell back to its default root.
    The script's first negative control passed when it should have failed, which exposed this. Server
    args now go through a generated `--config` file.
  - **Driver bug, fixed.** `initialize` echoed any `protocolVersion` the client asked for while
    `supportedVersions` listed others. It now echoes only a version it serves (2026-07-28,
    2025-11-25, 2025-06-18) and otherwise answers with its own. Test: P1 in
    `tests/test_agentengine_test_driver.cpp` (the Inspector's version is served, `1999-01-01` is not
    echoed).
- **Which handshake clients send (closes §14's open question).** Captured with a stdio tee: both the
  MCP Inspector CLI and Claude Code (`claude -p`, 2026-09-25) send `initialize` with
  `protocolVersion: 2025-11-25`, then `notifications/initialized` and `tools/list`. Neither sends
  `server/discover`. Keep answering both.

### C2 — I1 under TSan (Linux)

- `tools/test_driver/c2_tsan.sh` builds `test_agentengine_test_driver` twice with
  `-fsanitize=thread` (GCC 15, WSL2 Ubuntu, RelWithDebInfo) and runs the C2 stress at 1,000
  iterations (`AE_C2_ITERATIONS`; the default suite keeps 200):
  - normal tree: 1,000/1,000 runs settle with their own scripted text, **0 TSan reports**;
  - positive control (`-DAGENTENGINE_TEST_DRIVER_C2_RACE`, which makes `session_snapshot` read
    `history()` from the MCP thread while a run is in flight): **3 TSan data-race reports**, on
    `std::vector<Message>::push_back` in `run_rounds` against the MCP thread's read.
  Result: `C2: PASS`. The control needed no latch tool (§12): 1,000 overlapping iterations were
  enough for TSan to see the unsynchronized pair.
- The control flag appears only in `test_driver.hpp`'s snapshot path and is never defined by the
  build.

### Headless recipe

`docs/guides/testing-with-the-mcp-test-driver.md`: replay in CI (`ctest -L scenario`), scripted
exploration, live exploration on DeepSeek (the Windows HTTPS build recipe and the bounds), turning a
run into a scenario, and the C9 check.

### Revised build phases (replaces §16's list)

1. **Done:** P1, P2 (§13); driver phase 1 (§14); live mode and P6 (§15); scenario export and replay
   (§16); C9, C2 under TSan, and the recipe (§17). Every §8/§12 claim is now proven, except C7
   (waits for P1b) and C8. C8 is half met by §16's route: a live-derived scenario replays with no
   network. Its second half, where a request that diverges from the recording fails with
   `test.replay_mismatch`, is not built. A diverging replay shows up as an event-stream diff, not a
   request mismatch. Recordings under `--record-dir` are kept but nothing replays them as cassettes.
2. Next: P4 file fixtures (git-tracked check), P5 tool doubles, sandboxed real tools, `session_fork`.
3. Workflows; P1b (BUG-1/BUG-2) and P1c (event order), each its own ADR. The two-error-code and
   malformed-arguments findings are candidates for the same kind of small ADR.
