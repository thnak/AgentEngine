# Testing the engine with a Claude tester (MCP test driver)

`agentengine_test_driver` is an MCP server over stdio that lets a Claude Code agent drive real
AgentEngine sessions: start a session, script what the model says, send user turns, answer approvals,
cancel, and read the event stream. A finished run can be exported as a scenario that ctest replays
offline. Design, claims and proof: [`ADR-182`](../../decisions/ADR-182-agent-test-driver-mcp.md).

There are three ways to use it, from cheapest to most expensive:

| Mode | Engine's model | Who drives | Cost per run |
|---|---|---|---|
| Replay (CI) | scripted, from the scenario file | `ctest -L scenario` | nothing |
| Scripted exploration | scripted by the tester | Claude tester | Claude tokens only |
| Live exploration | real model (DeepSeek) | Claude tester | Claude tokens + a few model calls |

## 1. Replay the scenarios (CI, no network, no Claude)

The default build already has everything:

```
cmake --build build
ctest --test-dir build -L scenario
```

Every `tests/scenarios/*.json` becomes one ctest (`scenario_<name>`). A replay starts the scenario's
fixture, feeds the recorded model answers as the script, repeats the recorded steps (send, approve or
deny, cancel), and compares the whole normalized event stream. Before that, every model call is
checked against the recorded turn's `request_digest`. If the engine asks the model something different
(another prompt, tool result text or tool description), the replay stops with
`test.replay_mismatch at model call N` and prints the actual request. Otherwise a failure names the
first event that differs, with expected and actual side by side. To run one file directly:

```
build/agentengine_scenario_runner tests/scenarios/live_gated_approve.json
```

A scenario records today's behaviour, including known bugs it happens to cross. When an engine
change moves an event on purpose, update the affected scenarios as part of that change: ADR-183 did
this when it moved `approval_resolved` ahead of dispatch, and exactly the two scenarios that cross an
approve failed, at exactly the moved event.

A forked session (`session_fork`) exports as format 2. Its `segments` list the ancestors, each with
the model turns it had consumed, the steps it had taken and the turn it was forked at. Replay rebuilds
each ancestor, forks it, and then compares the final session's events.
`tests/scenarios/scripted_fork_branch.json` is an example.

A scenario exported before request digests existed can be given them with
`build/agentengine_scenario_runner --stamp-requests <file>...`. This replays the scenario and writes the
observed digests only if the replay passes with one model call per recorded turn.

## 2. Scripted exploration (Claude tester, engine model scripted)

`.mcp.json` at the repo root registers the driver as `agentengine-test`, and
`.claude/agents/agentengine-tester.md` defines the tester subagent. Inside an interactive Claude Code
session, ask for the `agentengine-tester` agent with the cases you want tested.

Headless, from the repo root:

```
claude -p --mcp-config .mcp.json --allowedTools "mcp__agentengine-test__*" \
  --output-format json --max-turns 40 \
  "You are testing AgentEngine through the agentengine-test MCP server only. Run these cases, each
   in a fresh session from fixture 'basic', and report a PASS/FAIL table: ..."
```

- The tester scripts every model answer with `model_script_push`, so a case never depends on an LLM
  choosing to behave. A model call with nothing scripted fails the run loudly
  (`scripted_chat_client.script_exhausted`).
- `--allowedTools "mcp__agentengine-test__*"` is what lets the tester use the driver without a
  permission prompt; headless runs cannot answer prompts.
- `--output-format json` returns the tester's final report plus its turn count and cost.

**Your own agent under test (file fixtures).** Put a 015 Agent document at
`tests/fixtures/test_driver/<name>.yaml` (see `one_sentence.yaml`). It sets the instructions, which
driver test tools the agent has (`echo`, `gated_echo`, `fail`), and `limits`. Commit it: the driver
uses only a file that git tracks with no uncommitted change, and `fixtures_list` shows any other file as
refused, with the reason. A fixture can't grant capabilities or name any other tool. The driver takes
`--fixtures-root <dir>` to look elsewhere, and the scenario runner takes the same flag.

## 3. Live exploration (engine on DeepSeek, Claude tester drives)

Live mode needs an HTTPS build of the driver and a key file. The key file is never committed
(`deep-seek.txt` is git-ignored) and the key is never passed on a command line or through MCP.

Windows HTTPS build (the configuration that works on this repo, 2026-09-25):

```
cmake -S . -B build-https-driver -G Ninja -DAGENTENGINE_WITH_HTTPS=ON -DAGENTENGINE_WITH_PDF=OFF \
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_MT=llvm-mt.exe
cmake --build build-https-driver --target agentengine_test_driver
```

Then use `.claude/test-driver-live.mcp.json`, which adds the live flags:

```
claude -p --mcp-config .claude/test-driver-live.mcp.json --allowedTools "mcp__agentengine-test__*" \
  --output-format json --max-turns 60 \
  "... fixture 'basic_live' ... Use timeout_ms 60000 on waits. ..."
```

What bounds a live run:

- Live mode is off unless the host passes `--allow-live`; the tester cannot turn it on.
- `--live-max-calls` caps model calls per session (12 in the checked-in config). Past it, calls fail
  with `test.live_call_budget_exhausted`.
- Every model call is recorded under `--record-dir`.
- Any driver output that would contain the key is withheld, and `scenario_export` refuses to write a
  file containing it.
- Live fixtures (`basic_live`, `no_tools_live`) have only in-process test tools (`echo`,
  `gated_echo`, `fail`), so approving a call never reaches anything real.

In live mode the tester cannot script the model; it sends user messages and checks what the engine
does with whatever the model answers. A live run still exports to an ordinary scenario: the model's
observed answers become the script, so the scenario replays offline (section 1).

## 4. Turning a run into a regression test

When a case passes and is worth keeping, the tester calls
`scenario_export {session_id, name, description}` and then `scenario_replay {name}` to confirm it.
The file lands in `tests/scenarios/<name>.json`; re-run CMake (the glob uses `CONFIGURE_DEPENDS`) and
it is a ctest.

Export refuses a run it cannot reproduce: one cancelled while a run was in flight (the result depends
on timing), one whose model answers were not all captured, or one whose event log overflowed. Names
must match `[a-z0-9_-]{1,64}`, and an existing file is only replaced with `overwrite: true`.

Review an exported file before committing it, as you would any golden file: it records exactly what
the engine did, right or wrong.

## 5. Checking the MCP contract

`tools/test_driver/c9_inspector_contract.py` runs the MCP Inspector CLI against the driver
(`tools/list` twice, a `tools/call`, and an approve round trip via `scenario_replay`). It needs `npx`
and network access for the first download, so it is a manual check rather than a ctest:

```
python tools/test_driver/c9_inspector_contract.py build/agentengine_test_driver
```

The Inspector and Claude Code both connect with the older `initialize` handshake (protocol
`2025-11-25`, observed 2026-09-25). The driver answers it as well as MCP 2026-07-28's
`server/discover`.

On Linux or WSL2, `tools/test_driver/c2_tsan.sh` checks the driver's threading under ThreadSanitizer.
It runs 1,000 concurrent send/observe cycles, plus a deliberately racy build that TSan must catch.
