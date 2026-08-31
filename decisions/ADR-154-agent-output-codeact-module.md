# ADR-154 — `agent.output` for CodeAct: declaring a script's structured result

**Status:** Judged (2026-08-31, project-owner sign-off). Designed, implemented, and proven end to end
against the REAL `agentengine_python_worker.exe` process — 12 checks (R1-R4) all pass, and the full
related native-jail/python-worker test suite (7 targets, including ADR-057's own `agent.ask` end-to-end
test, which shares the same wire frame this ADR extends) shows no regression. See §5/§6.

**Relates to:** `026-Agent-Facing-Runtime-Surface.md` §5 (`agent.output`'s spec, "Emit structured
output conforming to the run's schema", zero capability), GitHub issue #39 (filed from the same audit
that produced ADR-153), `decisions/ADR-058-agent-output-live-wiring.md` (the OTHER, unrelated half of
"agent.output" — `OutputSchema<T>`/`set_output_schema()`, the run's own final structured output driven
by the model's own chat message; that ADR explicitly named the CodeAct module "an explicit follow-on,
not this pass's own scope" — this ADR is that follow-on), `decisions/ADR-057-agent-ask-suspend-
without-deadlock.md` (the sibling `agent.ask` mechanism this ADR's own worker-side binding pattern
follows), `src/backends/native_jail/python_worker_mediation.cpp` (`Internal_set_output`,
`WorkerExecResult::structured_output_json`), `include/agentengine/sandbox/sandbox.hpp`
(`ExecOutcome::structured_output_json`).

## 1. The question

Per issue #39's audit: `agent.output` is spec'd (026 §5) and manifest-registered
(`trust/agent_library_manifest.hpp:53`) but has no implementation — no codegen module, no `agent`
attribute, nothing. `import agent; agent.output` raises `AttributeError` today.

**Stated so it has a wrong answer:** can a CodeAct script declare a structured result mid-script
(without aborting or suspending, unlike `agent.ask`) using only the ALREADY-EXISTING final
`exec_response` frame — no new frame type, no touching `exec_session()`'s mid-loop dispatch at all —
and land that value somewhere a host can actually read it, without silently conflating it with
ADR-058's genuinely separate run-level `structured_output_json` mechanism?

## 2. The design

`agent.output.set(value)` (matching ADR-058 §2 Design B's own naming, "`agent.output.set(value)`",
for consistency across the two ADRs) — a Python module function taking any JSON-serializable value.
Calling it stores the value; calling it again REPLACES it (last-call-wins, matching "declares the
final value," not a streaming/accumulating channel — the SAME scoping precedent ADR-060 §3 already
set for `agent.progress`: "structured/typed progress payloads... unchanged," this ADR's own
equivalent restraint is "no accumulation, no history, just the current value").

**No new frame type, no new mid-execution IPC round trip.** Unlike `agent.progress` (ADR-155, still to
be written — needs to interrupt a RUNNING script to push an event before it finishes) or `agent.ask`
(needs to suspend/abort), `agent.output.set()` only needs to be readable ONCE THE SCRIPT ENDS — so it
piggybacks on the exec_response frame that already, unconditionally, ends every `execute_code` call:

1. **`_ae_internal.set_output(json_text)`** (`python_worker_mediation.cpp`) — a plain, synchronous C
   function storing `json_text` into a file-scope `std::string g_structured_output_json`, reset to
   empty at the top of every `run()` call (the SAME per-run reset discipline `g_preseeded_answer_index`
   already has, for the identical reason: one execution's own call must never leak into the next).
2. **`agent_output_codegen.hpp`** — the Python bootstrap, `agent.output.set(value)` = `_ae_internal.
   set_output(json.dumps(value))`, matching `agent_ask_codegen.hpp`'s exact shape (one fixed,
   hand-authored function, reuse-not-recreate the `agent` module object).
3. **Three new fields, one per layer of the existing pipe**, each carrying the same string
   unmodified: `WorkerExecResult::structured_output_json` (worker-internal) →
   `build_exec_response()`'s new `"structured_output_json"` wire field (`python_worker_main.cpp`) →
   `ExecOutcome::structured_output_json` (`sandbox.hpp`, read by `native_jail_backend.cpp`'s
   `exec_session()` at the SAME point it already reads `result_repr`/`ask_prompt` off the SAME frame
   — no new branch in the frame-type dispatch loop, just two more fields read off the ALREADY-matched
   `kExecResponse` frame).
4. A new opt-in `expose_agent_output` boolean, threaded through
   `MediatedPythonConfig`/`WorkerInitConfig`/the init-request wire frame, matching `expose_agent_ask`'s
   exact precedent (zero-capability module, still opt-in per session — a session may have no reason to
   want it even though it costs nothing).

## 3. Deliberately out of scope

- **Does NOT populate `rt::AgentSession::structured_output_json`** (ADR-058's own field, keyed off the
  model's final chat message). `agent.output.set()`'s value lands in the `execute_code` TOOL's own
  result (`ExecOutcome`), visible to the model as ordinary tool output — nothing here makes it
  automatically become the RUN's own final answer. Wiring a tool result into an early-exit-the-round-
  loop decision is a materially bigger, separate design question (does the run end immediately? does
  the model get a chance to add commentary? what if the model calls `execute_code` again afterward?)
  this ADR does not attempt, matching ADR-058 §2's own "narrow, additive opt-in" restraint and ADR-060
  §3's precedent of building a channel without wiring further consumption in the same pass.
- Structured VALIDATION against a declared schema (003 §4's own schema, if any) — the value is passed
  through as whatever JSON `json.dumps()` produces; no validation is attempted here, matching ADR-058
  §1's own finding that no generic JSON-Schema validator exists anywhere in this codebase yet.
- `MediatedShellRunner` — Shell has no REPL-style semantics and no `agent.*` module surface at all
  today; this ADR touches only the Python worker.

## 4. Red-team notes

Built and run for real before being declared done, not merely designed. Two things worth naming
explicitly, neither a defect:

- **Cross-execution leak was a real risk, closed by construction, and proven by a dedicated
  regression check (R4).** Without the `g_structured_output_json.clear()` reset at the top of `run()`,
  a script that never calls `agent.output.set()` would silently inherit whatever the PREVIOUS
  `execute_code` call on the same worker last set — R4 is a genuine regression test for this, not a
  formality: it runs a THIRD script on the same worker, after R2/R3 already set real values, and
  confirms the field comes back empty.
- **`klass == ask_pending` never carries a structured-output value**, deliberately: `outcome.
  structured_output_json` is only assigned on the `"ok"` path in `python_worker_mediation.cpp::run()`,
  matching `result_repr`'s own existing precedent (also `"ok"`-only). Consistent with ADR-057 §9's
  abort-and-replay semantics — a suspended run's `agent.output.set()` call (if any ran before the
  suspend point) belongs to a script execution that has not actually finished; the whole script
  re-runs on replay, so the "final" value should come only from a run that ran to completion.

No must-fix findings this pass — the design's own scope boundary (§3, no round-loop interaction) is
what keeps this materially simpler and lower-risk than ADR-153's `capability_ceiling` finding or
ADR-060's `Backgroundable`-thread finding: there is no capability check to bypass (026 §5's own `{}`
for this module) and no cross-thread call path (the worker process is single-script-at-a-time,
synchronous, no detached-thread equivalent of `background_task()` exists in this subsystem).

## 5. Executed evidence

**Windows, MSVC (Visual Studio 18 Community), MSBuild, Debug, existing `build/` tree**
(`AGENTENGINE_BUILD_PYTHON_RUNNER=ON` already configured; real `agentengine_python_worker.exe`
rebuilt as part of this pass). New target, run via `ctest` (needed for its `ENVIRONMENT` properties):

```
  ok: R1 setup: runner initializes a real jailed worker process
  ok: R1: exec_session() returns a real outcome, not a hang/crash
  ok: R1: an AttributeError from a script is an ordinary 'ok' outcome with a traceback on stderr, not a policy_violation/crash
  ok: R1: the script's own traceback shows agent.output was never attached -- not reachable without the opt-in
  ok: R1: no structured output was recorded (the call never ran)
  ok: R2 setup: runner initializes with agent.output exposed
  ok: R2: exec_session() returns a real outcome
  ok: R2: the run converges normally
  ok: R2: the script kept running after the call -- this is not a suspend/abort the way agent.ask() is
  ok: R2: the real JSON-encoded value round-tripped into ExecOutcome, not a stub
  ok: R3 setup: exec_session() returns a real outcome
  ok: R3: two calls in one script keep only the LAST value, not the first or both
  ok: R4 setup: exec_session() returns a real outcome
  ok: R4: a script that never calls agent.output.set() carries no structured output -- and does NOT leak the PREVIOUS call's own value across executions
test_agent_output_codegen: ALL PASS
```

**No regression** — the full related native-jail/python-worker suite, rebuilt and rerun via `ctest`:

```
test_agent_tools_codegen ...................... Passed
test_agent_files_data_codegen .................. Passed
test_mediated_python_runner_smoke .............. Passed
test_native_jail_python_worker_slice1 .......... Passed
test_agent_output_codegen ...................... Passed
test_native_jail_python_worker_handle_relay .... Passed
test_agent_session_suspend_codeact_ask ......... Passed  (ADR-057's own agent.ask test -- shares the
                                                            SAME exec_response wire frame this ADR
                                                            added a field to)
100% tests passed, 0 tests failed out of 7
```

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| `agent.output` is absent (loud `AttributeError`, not a silent gap) without the opt-in | **CORRECT** | R1's checks |
| With the opt-in, `agent.output.set(value)` round-trips a real JSON value into `ExecOutcome` | **CORRECT** | R2's checks |
| The call does not abort or suspend the script | **CORRECT** | R2: "script ran" printed AFTER the call |
| Two calls in one script: last-call-wins | **CORRECT** | R3's check |
| No cross-execution leak on the same worker | **CORRECT** | R4's dedicated regression check |
| No regression to `agent.ask`/`agent.tools`/`agent.files`/`agent.data` or the worker lifecycle | **CORRECT** | Full 7-target suite, 100% pass |

## 7. Residuals to name up front

- Does NOT populate `rt::AgentSession::structured_output_json` (ADR-058) — a separate, larger,
  not-yet-designed follow-on (§3).
- No schema validation of the emitted value — passed through as whatever `json.dumps()` produces.
- `MediatedShellRunner` untouched — Python worker only.
- No `.pyi` stub generated for `agent.output` (matching `agent_tools_codegen.hpp`'s own named residual
  for the same reason: no consumer for one exists yet anywhere in this codebase).
