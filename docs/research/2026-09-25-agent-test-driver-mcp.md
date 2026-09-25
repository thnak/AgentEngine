# Agent test-driver MCP server — a "Playwright MCP" for AgentEngine sessions

**Date:** 2026-09-25 · **Researched for:** RFC 022 §6 (`TestKit`, unbuilt since ADR-037) and the
live-provider test flakiness problem · **Status:** research + proposed shape; no ADR yet

## Why this was researched

Live-provider tests (`ctest -L live-network`) drive a real `rt::AgentSession` by sending a free-form
prompt and hoping the model does the thing the test needs ("You must call BOTH get_weather … AND
send_message", `tests/test_rt_agent_session_hitl_live_e2e.cpp:477`). When the model doesn't comply
the test can only downgrade the check to a printed note (`:502`), and `tools/farm_ops_interactive_cli.py`
exists purely to work out *why* a live run failed, because pass/fail from the C++ test hides it.
The model under test is both the thing being exercised **and** the source of nondeterminism.

The goal: a first-class MCP server that lets a Claude Code sub-agent drive a **running** AgentEngine
session step by step — start it, send a turn, script what the model says, observe events, answer
approvals, snapshot state, assert — the way Playwright MCP lets an agent drive a browser.

## What already exists in this repo (inventory, 2026-09-25)

Nothing is wired up as a driver, but almost every seam a driver needs is already public API.

| Need | Existing surface | Gap |
|---|---|---|
| Drive a run | `AgentSession::start_run(StartRun)` `rt/agent_session.hpp:929`; suspending runs return `run.suspended_for_{approval,codeact_ask,hook_decision}` (:3057-3073) | none |
| Answer HITL | `resolve_interaction(ResolveInteraction)` :1057; `open_interactions()` :862; workflows: `WorkflowSupervisor::resume_workflow` / `open_interaction_asks` (`rt/workflow_supervisor.hpp:674,703,812`) | **BUG-1/2/3** below |
| Observe | `enable_event_stream()` (single-consumer, :816), `set_run_event_tap()` :840; `run_event_kind` `core/run_event.hpp:41`; `workflow_event_kind` `workflow/workflow_event.hpp:54` | no condition-wait helper |
| Inspect state | `history()`, `state()`, `metadata()`, `run_usage()`, `snapshot_record()`, `agent_session_record_to_json` (:480, :1485-1512) | no compact "snapshot" projection |
| Branch / cancel | `fork_from()` :1355, `cancel()` :745 (ADR-178) | none |
| Script the model | `ScriptedChatClient` — **re-implemented ad hoc in ~40 test files** (e.g. `tests/test_rt_agent_session.cpp:88`, `examples/05_human_approval.cpp:89`) | no shared, strict, multi-turn scripted client |
| Record / replay | `RecordingChatClient<Inner>` `core/recording_chat_client.hpp:118`; `ReplayChatClient` `core/replay_chat_client.hpp:135` (one recording per instance); `cli_chat` already dumps cassettes (`AGENTENGINE_CLI_CHAT_DUMP_DIR`) | no multi-call sequencer |
| Interactive host | `tools/cli_chat.cpp` (stdin REPL, blocking y/N approval) | not machine-drivable |
| MCP server role | `protocol/mcp/server.hpp:178` `McpServer` | exposes a **ToolTable**, not a session; no stdio; not reusable here |
| Declarative agents | `core/agent_yaml_compiler.hpp` (RFC 015) | no host loads agent YAML |

Known engine bugs a driver hits immediately (documented at `tests/test_rt_agent_session_hitl_live_e2e.cpp:10-40`):
- **BUG-1:** `approval_requested` fires for *every* call in a round, not only gated ones.
- **BUG-2:** one deny applies to all pending calls in the round.
- **BUG-3:** neither `Interaction` nor the approval payload carries tool name/args — a driver must
  re-derive them from `history().back()`. A test agent can't make a meaningful approve/deny decision
  without this; it is a hard prerequisite.

RFC 022 already specifies the destination: §3 golden traces (`{scripted inputs, recorded
nondeterminism} → expected event stream`, compared on the 013 event stream, never free text), §6 a
`TestKit` that "drives approvals and input requests programmatically", §1 live-provider runs are
quarantined and **never** gate a merge. The driver below is the interactive front end to that.

## External prior art (sources fetched 2026-09-25)

### Playwright MCP — the model to copy
Source: <https://github.com/microsoft/playwright-mcp> (README on `main`), <https://playwright.dev/docs/test-agents>.
- **Structured snapshot, not pixels.** `browser_snapshot` returns the accessibility tree; design goals
  are "LLM-friendly … operates purely on structured data" and "deterministic tool application".
  Screenshots are opt-in (`--caps=vision`).
- **Refs.** Every element in a snapshot has a ref; action tools take `target` (exact ref) plus
  `element` (human description, used for permission prompts). Actions return the post-action snapshot.
- **Condition waits.** `browser_wait_for {text | textGone | time}` — wait on a condition, not a sleep.
- **Capability groups.** `--caps=vision|pdf|devtools|storage|network|testing`; `testing` adds
  `browser_verify_*` assertion tools; `browser_run_code_unsafe` is the named escape hatch.
- **Lifecycle.** stdio by default, `--port` for HTTP; `--isolated` in-memory profile.
- **Test agents (Playwright 1.56, Oct 2025).** `npx playwright init-agents --loop=claude` writes
  `.claude/agents/*.md` for **planner** (explore → `specs/*.md`), **generator** (plan →
  `tests/*.spec.ts`), **healer** (run + fix). Key point: *the LLM explores and authors a deterministic
  test file; CI replays the file without any LLM.*
- Microsoft's own caveat: CLI flows are more token-efficient than MCP; MCP is for "persistent state,
  rich introspection, and iterative reasoning" — which is exactly the session-driving case.

### MCP spec 2026-07-28 (stateless core)
Sources: <https://modelcontextprotocol.io/specification/2026-07-28/changelog>,
<https://blog.modelcontextprotocol.io/posts/2026-07-28/>,
<https://modelcontextprotocol.io/specification/2026-07-28/basic/patterns/mrtr>,
<https://modelcontextprotocol.io/extensions/tasks/overview>.
- No `initialize`; `server/discover` is mandatory. Protocol sessions / `Mcp-Session-Id` removed —
  state across calls uses **explicit server-minted handles passed as ordinary tool arguments**
  (SEP-2567). A `session_id` returned by `session_start` is the prescribed pattern.
- Server→client requests replaced by MRTR (`resultType: "input_required"` + opaque `requestState`,
  which the spec says MUST be treated as attacker-controlled and integrity-protected if it influences
  authorization).
- Tasks moved to extension `io.modelcontextprotocol/tasks` (`tasks/get|update|cancel`).
- `structuredContent` any JSON; `inputSchema`/`outputSchema` full JSON Schema 2020-12; `tools/list`
  SHOULD be deterministically ordered.

### Claude Code as the client
Sources: <https://code.claude.com/docs/en/mcp>, <https://code.claude.com/docs/en/headless>,
<https://code.claude.com/docs/en/sub-agents>, <https://code.claude.com/docs/en/hooks>.
- stdio / streamable HTTP; v2 runtime speaks 2026-07-28.
- `MAX_MCP_OUTPUT_TOKENS` default 25k (warn at 10k); per-tool `_meta["anthropic/maxResultSizeChars"]`
  up to 500k; oversize output spills to a file.
- Calls longer than `CLAUDE_CODE_MCP_AUTO_BACKGROUND_MS` (2 min) auto-background → **keep each call
  short; long waits return a handle to poll.**
- Elicitation is supported but under `-p --permission-prompts none` unanswered elicitations are
  cancelled → **approvals must be plain tool calls, not elicitation.**
- `structuredContent` / tasks-extension consumption is **not documented** → always return a text
  rendering; do not depend on tasks.
- Subagents: `.claude/agents/*.md` with `tools` allowlist (`mcp__server__*`), inline `mcpServers`
  (connected only while that subagent runs), `maxTurns`, `isolation: worktree`.
- Headless CI: `claude --bare -p --strict-mcp-config --mcp-config … --output-format stream-json
  --verbose`; `system/init` lists `mcp_server_errors` (fail the job if non-empty).

### Deterministic agent-testing prior art
- Scripted models: LangChain `GenericFakeChatModel`
  (<https://docs.langchain.com/oss/python/langchain/test/unit-testing>); pydantic-ai `TestModel`,
  `FunctionModel`, `ALLOW_MODEL_REQUESTS=False`, `capture_run_messages`
  (<https://pydantic.dev/docs/ai/guides/testing/>); MAF guidance: scripted `IChatClient`, assert on
  workflow events incl. "approval pause happens before side effect"
  (<https://www.lukaswalter.dev/posts/agentframework_1_18/>, 2026-07-15).
- Wire mocks with request capture + runtime-reconfigurable scripts: fake-openai
  (<https://github.com/khimaros/fake-openai>), mock-llm (<https://github.com/dwmkerr/mock-llm>).
- Replay that records **decisions** but re-executes tools: langchain-replay
  (<https://github.com/sixty-north/langchain-replay>).
- MCP Inspector `--cli` (scriptable, exit codes) — a CI smoke gate for the server's own contract
  (<https://modelcontextprotocol.io/docs/2026-07-28/tools/inspector>).
- Simulated users (τ²-bench, promptfoo Simulated User, LangWatch Scenario) belong in a **non-gating
  eval tier**: "LLM-Simulated Users are Unreliable Proxies for Human Users"
  (<https://arxiv.org/pdf/2601.17087>).
- No existing project found that combines *drive session + script model + answer approvals +
  snapshot* behind MCP. LangGraph's `/mcp` is stateless invocation
  (<https://docs.langchain.com/langsmith/server-mcp>); MAF DevUI is a sample UI
  (<https://learn.microsoft.com/en-us/agent-framework/devui/>, 2026-08-25).

## The reframe that fixes the confusion problem

The confusion comes from putting an LLM in the **system-under-test** seat. Move it:

- **Claude (the sub-agent) is the tester** — it decides what to send, what the model "says", what to
  approve, and what to assert. It is good at that; it never has to be coerced by a prompt.
- **The model inside AgentEngine is scripted by default.** Claude injects the exact assistant turn
  (text / tool calls / malformed output / errors) the scenario needs. The engine's real tool loop,
  approval gate, capability checks, sandbox, compaction etc. run for real.
- **Live models are an explicit, recorded mode** for exploratory runs; every live call goes through
  `RecordingChatClient`, so a confusing run becomes a cassette that replays deterministically.
- **Output is a checked-in scenario file**, not a transcript: Playwright's planner → generator →
  spec-file split. A plain ctest runner replays it with no Claude and no network (I5, RFC 022 §3).

## Proposed shape

### Process and transport
- New host executable `tools/agentengine_test_driver` (C++; in-process with `rt::AgentSession`),
  speaking MCP 2026-07-28 over **stdio**. It is a *host*, like `cli_chat` — the engine library still
  binds nothing (ADR-039/061 untouched). Built only under a `AGENTENGINE_BUILD_TEST_DRIVER` option,
  never installed, never linked into `agentengine` — mitigates ADR-039's named "example silently
  becomes de facto production" residual.
- Sessions are configured from a **fixture file** (agent YAML per RFC 015 + tool set + capability
  grant + sandbox profile) named at `session_start`, resolved against a fixtures root fixed on the
  command line. Capability grants come from that host-side file, never from free-form tool args.

### Model modes (per session)
| mode | chat client | use |
|---|---|---|
| `scripted` (default) | shared strict `ScriptedChatClient`: FIFO of turns; `model_script_push`; request capture; **fails the call** if the queue is empty (no silent fallback) | gating tests |
| `replay` | multi-call sequencer over `ReplayChatClient` recordings | regression from a cassette |
| `live` | provider wrapped in `RecordingChatClient`; refuses to start unless the driver was launched with `--allow-live` | exploration only; never gating |

### Tool surface (grouped like Playwright `--caps`)
Core (always):
- `session_start {fixture, model_mode, session_label?}` → `session_id` + snapshot
- `session_send {session_id, text}` → starts a run, returns immediately with `run_id`
- `session_wait_for {session_id, until: run_finished|run_suspended|event{kind,match?}|tool_called{name}, timeout_ms ≤ 60000}` → matched events + snapshot (short calls, well under the 2-min auto-background)
- `session_snapshot {session_id, include?: [history, state, usage, events_since]}` → compact structured view with stable refs `turn#3`, `call#7`, `ix#2`
- `session_events {session_id, since_seq, kinds?}` → paged event log (from `set_run_event_tap`)
- `interaction_list {session_id}` → pending approvals/asks **with tool name + args** (needs BUG-3 fixed)
- `interaction_resolve {session_id, interaction_ref, approve|deny|answer}`
- `session_cancel`, `session_close`

`script` cap:
- `model_script_push {session_id, turns: [{text?, tool_calls?, reasoning?, finish?, error?, chunks?}]}`
- `model_requests {session_id, since?}` → exactly what the engine sent the model (prompt, tools, history) — assert on context assembly

`state` cap: `session_fork {session_id, at_turn?}`, `session_save/restore` (snapshot store).

`testing` cap:
- `assert {session_id, checks: [...]}` — structural predicates only (tool sequence, event kinds,
  approval-before-effect ordering, state keys, no `model_output_discarded`, budget ≤ N)
- `scenario_export {session_id, path}` — writes the replayable scenario (inputs + model script /
  cassette refs + normalized expected event stream per RFC 022 Q3's `<TIMESTAMP>`/`<RUN_ID>` rules)

Workflow parity (`workflow` cap): `workflow_start`, `workflow_wait_for`, `request_port_list/resolve`
over `WorkflowSupervisor` — same shape.

Every result: short text rendering first (≤ ~8k tokens), `structuredContent` as a bonus; tools
listed in deterministic order with terse descriptions (Claude Code Tool Search).

### Replay without Claude
`agentengine_scenario_runner <scenario.json>` (a ctest target per checked-in scenario under
`tests/scenarios/`) replays inputs + model script, resolves interactions as recorded, and diffs the
normalized event stream against the golden. This is where the value lands in CI; the MCP driver is
how scenarios get written and debugged.

### Claude Code side
- `.claude/agents/agentengine-tester.md`: `tools: mcp__agentengine_test__*` plus Read/Write for
  `tests/scenarios/`, inline `mcpServers` launching the driver, `maxTurns` cap.
- Optional planner/healer split later (planner writes a scenario plan from an RFC section; healer
  re-runs failing scenarios and explains the event-stream diff).
- Headless: `claude --bare -p --strict-mcp-config --mcp-config .claude/test-driver.mcp.json
  --output-format stream-json --verbose`; fail on `mcp_server_errors`.

## Invariant / safety analysis (to be red-teamed)

- **I2 (no ambient authority).** The driver acts as one explicit test `Principal`; every
  `interaction_resolve` passes it as `caller`. Capabilities come from the host fixture file. No tool
  argument can widen a grant. Resolving an approval is *deciding among already-possessed authority*
  — within ADR-070's Delegated Decision Seam shape, and audited.
- **I3 (model output is data).** Scripted model output enters exactly where real model output
  does; it never reaches the approval decision. The *tester* (Claude) answering approvals is host-
  side, not model-output-derived — the same position as `cli_chat`'s human y/N. This distinction
  must be stated explicitly in the ADR, because "an LLM answers approvals" reads like an I3 violation.
- **I4 / audit.** Every driver action is attributable to the driver principal and logged.
- **Machine safety.** Real tools run for real, so the default sandbox profile and resource caps
  apply; the driver refuses fixtures that request an unsandboxed / native-process provider
  (ADR-071) unless launched with an explicit flag.
- **Secrets.** `live` mode reads keys the same way `run-live-provider-tests.ps1` does; keys never
  appear in snapshots, events, scenario exports, or `model_requests` output (redaction test with a
  positive control).
- **stdio only**; no `--port` in v1. An HTTP mode would reopen ADR-039-class listener questions.

## Prerequisites (engine-side, independent value)
1. Fix **BUG-3** (tool name/args on approval `Interaction` / payload); ideally BUG-1/BUG-2 too.
2. Extract a shared strict `ScriptedChatClient` into `include/agentengine/testing/` (or `core/`),
   replacing the ~40 ad-hoc copies over time.
3. Multi-recording `ReplayChatClient` sequencer.
4. A host-side loader: agent YAML fixture → configured `AgentSession` (nothing does this today).

## Suggested gates (falsifiable)
- G1: a scenario authored through the driver in `scripted` mode replays under `agentengine_scenario_runner` 100/100 with an identical normalized event stream.
- G2 (positive control): a deliberately introduced BUG-2-style regression (deny-all) makes a checked-in scenario fail.
- G3: `model_script_push` queue exhaustion fails loudly (a test that would silently pass on fallback is the negative control).
- G4: `inspector --cli` contract test of `server/discover` + `tools/list` ordering + one full approve/deny round trip.
- G5: secret redaction — a planted key string never appears in any driver output.
- G6: a live-mode run's cassette replays with no network (I5).

## Recommendation
Proceed via `design → red-team → prove → judge` as a new ADR (next free number): it adds a new
host surface that resolves approvals programmatically, which touches I2/I3 and ADR-039/070. Land
prerequisites 1–2 first — they help the existing test suite regardless. Build the driver in phases:
(a) core + `script` caps + session snapshot over one fixture; (b) `scenario_export` + runner +
ctest integration; (c) workflow parity, `replay`/`live` modes; (d) tester sub-agent definitions.
