# ADR-197 — Tool-call arguments that are not valid JSON are refused, never coerced to `{}`

- **Status**: **Proposed — design + implementation + proof (2026-09-25; not yet red-teamed).**
- **Date**: 2026-09-25
- **Origin**: GitHub issue #105, found by the agent test driver's first end-to-end run (ADR-182 §14,
  "First finding from the harness"). A scripted `echo` call with arguments `{not json` ran with `{}`,
  and the model got back "missing required field 'text'" instead of a parse error.
- **Implements**: 006 §3 step 2 ("validate — reject; do not coerce") and its non-negotiable property
  "Argument validation rejects, never coerces." 006 is accurate and is not amended.
- **Touches**: `include/agentengine/core/tool_call_extraction.hpp` (`tool_call_request_of`),
  `include/agentengine/core/tool_pipeline.hpp` (`ToolCallRequest::arguments_parse_error`,
  `malformed_arguments_error`, step 2 in `admit_call` and `background_task`).

## 1. The defect

```cpp
// before -- tool_call_request_of()
auto parsed = json::parse(call.arguments_json);
return ToolCallRequest{call.call_id, call.tool_name,
                       parsed ? *parsed : json::Value::make_object({}), ...};
```

Argument text that failed to parse became `{}`. For a tool with a required field the call then failed
at `schema::from_json<Args>` with a misleading message ("missing required field"). For a tool whose
arguments are **all optional**, `{}` is a valid call, so the tool **ran** on garbage model output.
That is exactly the coercion 006 §3 forbids, and it sits on model output, so it touches I3: model text
that does not even parse selected a real invocation with default arguments.

**Who could reach it.** Every tool call from a model goes through `tool_call_request_of()`: vendor
structured calls (OpenAI `function.arguments`, Anthropic `tool_use.input`, both streaming
accumulators), `text_derived` calls (response-format leak scan, middleware, hook rewrites), and the
ADR-029 approval-resume path, which rebuilds requests from history with the same helper. CodeAct's
`call_tool()` bridge already refused malformed JSON with `tool.malformed_arguments`
(`native_jail_backend.cpp`, `python_worker_mediation.cpp`); only the model-call path coerced.

## 2. Decision

**A tool call whose argument text is not valid JSON never reaches the tool.** It folds as an
ordinary tool error (a value, 006 §3 last property), with error code `tool.malformed_arguments` — the
code the CodeAct bridge already uses for the same condition, so one condition has one name. The
model sees `tool arguments are not valid JSON (<parser message>); the tool was not run -- resend the
call with a JSON object` and can retry.

Mechanism:

1. `tool_call_request_of()` no longer substitutes silently. On a parse failure it sets the new
   `ToolCallRequest::arguments_parse_error` (the parser's message; empty iff the text parsed).
   `arguments` stays `{}` only as a placeholder, because hooks and approval prompts need a value.
2. `admit_call()` refuses a request with a non-empty `arguments_parse_error` at step 2, right after
   resolve and **before** any capability is bound or any approval or policy is consulted.
   `invoke_tool()` and the ADR-160 parallel-batch path both go through `admit_call()`.
3. `background_task()` has its own step list (it does not call `admit_call()`), so it gets the same
   check at the same point. Both use one shared `malformed_arguments_error()`.

The flag, not a sentinel value in `arguments`, carries the refusal, so no `json::Value` a hook could
write can clear it: a hook that rewrites a malformed call's arguments is still refused (fail closed).
The field is appended last with a default, so every existing aggregate `ToolCallRequest{...}` (MCP
server, CodeAct bridge, tests) is unaffected; those callers already hold a parsed `json::Value`.

`failure_class::contract`, matching `tool.unknown_name`, the other step-1/2 refusal.

### Empty argument text means `{}`, deliberately

Empty or all-whitespace `arguments_json` still means `{}`. This is not coercion of garbage: there
is no text to misread, and an empty string is the natural wire form of a call with no arguments.
The adapters already treat it that way on most paths: both streaming accumulators map empty text to
`"{}"` (`openai/chat_client.hpp`, `anthropic/chat_client.hpp`), as does a missing OpenAI
`arguments` field; only a non-streamed OpenAI `"arguments": ""` reaches `tool_call_request_of()` as
`""`, and it now means the same thing on that path too. Refusing it would break no-argument tools for
no safety gain. Any non-blank text must parse.

Valid JSON that is not an object (`42`, `[1]`) is not this ADR's concern: it parses, and step 2's
shape check (`schema::from_json<Args>`) refuses it as before.

## 3. Proof

`tests/test_tool_call_malformed_arguments.cpp` (ctest `test_tool_call_malformed_arguments`), with a
probe tool whose only argument is `std::optional<std::string>` — so `{}` is a valid call and only a
real step-2 refusal stops it:

| Check | What it proves |
|---|---|
| T1 | `{not json` through `invoke_tool`: the tool does not run; `is_error`; audit code `tool.malformed_arguments`; the model-visible message names invalid JSON |
| T2 | trailing garbage and a truncated object are refused the same way |
| T3 | `""` and whitespace-only arguments run the tool (the deliberate rule above) |
| T4 | control: `{"note":"hello"}` and `{}` run; valid text leaves `arguments_parse_error` empty |
| T5 | `admit_call()` (parallel path) and `background_task()` refuse it before binding or spawning; control: valid arguments background and complete |
| T6 | a real `AgentSession` round: the malformed call is a tool error, the run continues, the model's retry with valid arguments runs the tool exactly once |

Positive controls (run 2026-09-25, each restored by editing back):

1. `tool_call_request_of()` returned to the old coercion: T1, T2, T5 (both) and T6 fail.
2. only `admit_call()`'s check disabled: T1, T2, T5 (`admit_call`) and T6 fail.
3. only `background_task()`'s check disabled: T5 (`background_task`) fails.

**Scenario.** `tests/scenarios/scripted_malformed_arguments.json` (ctest
`scenario_scripted_malformed_arguments`), exported by `agentengine_test_driver`'s `scenario_export`
from a scripted `basic`-fixture session: `echo` with `{not json` → `tool_call_finished` `is_error`
with the not-valid-JSON message → the model retries with `{"text":"retry"}` → `echo` runs once → run
completes ok. Under the old coercion the golden does not match (the first result was "missing
required field 'text'"); ADR-182 §14 deliberately did not export this scenario until now, because a
golden would have locked the bug in.

## 4. Residuals

- ~~**A malformed call to an approval-gated tool still suspends for approval first.**~~ **Closed the same day**
  (same PR): `run_rounds`' suspend check and `resolve_hook_decision`'s cascade skip a call whose
  `arguments_parse_error` is set, since it is refused at step 2 whatever anyone decides
  (`test_approval_resume` M1).
- `tool_call_started` is still emitted for the refused call (the session emits it before
  `invoke_tool`), paired with an error `tool_call_finished`. That is the existing shape for every
  step-1..5 refusal (unknown tool, capability not held, denial), not new behavior.
- Anthropic's outbound history translation (`translate_tool_use_input`) still sends `{}` for a
  historical `tool_use` whose text never parsed, because the wire requires an object there. That is
  replaying history, not running a tool; the call's result in the same history is the refusal.
- A host hook cannot repair malformed arguments (e.g. with a JSON-repair pass): the refusal is keyed
  on the original text. Deliberate; a repair seam would be its own ADR.
