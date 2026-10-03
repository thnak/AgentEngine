# `failure_class` classification-site inventory for `canceled` (ADR-237 §9 D6)

Date: 2026-10-03. Branch `adr237-canceled` off `adr237-async-design` at `0713daa`.
Evidence for ADR-237 §9 D6 ("'Compiler-checked' is not enough ... the implementation carries a grep
inventory of every classification site as evidence"), and the record of what each site does with the new
class.

## Method

Searches over `include/`, `src/`, `tools/`, `examples/`, `samples/`, `plugins/`, `bench/`, `web/`, `wit/`,
`schema/`, `conformance/` (codegraph index plus ripgrep):

- comparisons / switches: `(==|!=) failure_class::`, `failure_class::x (==|!=)`, `case failure_class::`,
  `klass (==|!=)`, `switch (... klass ...)`;
- predicates taking a class: `\(failure_class \w+\)`, `is_retryable`, `retryable(`;
- string forms: `"transient"|"policy"|"contract"|"resource"|"fatal"` and the readers/writers of `"klass"` /
  `"class"` fields; schema, WIT and web enum lists;
- cancellation and deadline producers: every `failure_class::` construction within three lines of
  `cancel|abort|stop_token|stopped|interrupt|deadline|timeout|timed out`, every string code containing
  `cancel`/`abort`/`deadline`/`timeout`, and every `run_canceled` / `workflow_status::cancelled` /
  `*_error::canceled` consumer;
- protocol mappings: A2A `task_state`, AG-UI `RunError`, MCP task status / `notifications/cancelled`, OTel
  `error.type`.

Totals for scale (after the change): `failure_class::` appears 1,274 times in 206 production files
(`include/`, `src/`, `tools/`) and 420 times in 128 test files. Almost all are *constructions* of an error
with a fixed class unrelated to cancellation or deadlines; they cannot misclassify `canceled` and are not
listed. Nothing under `examples/`, `samples/`, `plugins/`, `bench/`, `wit/`, `schema/` or `conformance/`
compares, switches on or spells a class. `web/marketing/src/components/ApiWorkflowReference.tsx:294/589`
documents "transient or resource is retryable" for workflow edges, which stays true.

## Counts by kind

| Kind (rows below) | Sites | Changed |
|---|---:|---:|
| Definition, the one spelling, serialized string forms, the three `switch` copies (1-7) | 7 | 5 (the copies now call the one mapping) |
| Retry predicates (8-14) | 7 | 6 (all call `is_retryable(class, retry_budget)`) |
| Fallback / fail-over / propagate decisions (15-17) | 3 | 3 |
| Other `==`/`!=` predicates on a class (18-23) | 6 | 2 |
| Producers of a cancellation (24-35, incl. 34a) | 13 | 12 |
| Producers / mappers of a deadline (36-46) | 11 | 4 |
| Protocol mappings: A2A, AG-UI, MCP, OTel (47-51) | 5 | 1 |
| Event-kind (not class) consumers of `run_canceled` (52-55) | 4 | 0 |
| Step 1-4 `rt/` I/O results with their own `canceled`, no class yet (56-61) | 6 | 0 |
| **Total** | **62** | **33** |

There was exactly one `switch` over `failure_class` that decided behaviour-adjacent output
(`failure_marker`); the other two were string-mapping copies (`chat_recording.cpp`, `test_driver.hpp`). Every
decision was an `==` predicate, which is what made D6's "compiler-checked" claim hollow (round 2, G-c).

## The one retry decision

`include/agentengine/core/error.hpp`: `is_retryable(failure_class, retry_budget)` decides **which classes
retry**, once. A site only states which budget its retry runs under:

- `retry_budget::shared` — the same call re-sent inside the caller's same deadline and budget
  (`ModelCallGateway`, the session's ADR-177 stream retry, the eval screen's trial re-draw);
- `retry_budget::fresh` — a whole unit re-invoked with a budget of its own (`multi_agent::spawn_with_retry`,
  a workflow edge's `retry`).

| class | shared | fresh |
|---|---|---|
| transient | retry | retry |
| resource (a deadline included) | no | retry |
| policy, contract, fatal | no | no |
| canceled | no | no |

So "is a `resource` deadline retryable?" has one answer: never under the budget it exhausted; yes by a fresh
attempt that carries its own budget, bounded by the caller's attempt count. This keeps every pre-D6
behaviour (the gateway was transient-only, `multi_agent` and the workflow supervisor were transient+resource
— the divergence `multi_agent.hpp` flagged in its own comment) and explains it instead of repeating it.

## Inventory

Line numbers are after the change. "canceled" = the decision for `failure_class::canceled`; "deadline" = the
decision for deadlines (`resource` + `*deadline_exceeded`).

### Definition and string forms

| # | Site | What it does | canceled | deadline |
|---|---|---|---|---|
| 1 | `include/agentengine/core/error.hpp:18` | `enum class failure_class` | **added** `canceled` (doc comment: requested stop; not retryable, not policy, not fault) | `resource` comment now says a deadline is `resource` |
| 2 | `include/agentengine/core/error.hpp:32,44` | **new** `failure_class_to_string` / `failure_class_from_string`, the one spelling | `"canceled"` (one l); `"cancelled"` is rejected | — |
| 3 | `src/core/chat_recording.cpp:28,30` | recording wire string (`klass` field of `error_to_json`) | **delegates** to #2; old recordings never carry `canceled`; an old build reading a new one fails `recording.bad_failure_class` loudly | — |
| 4 | `src/core/chat_recording.cpp` `error_to_json`/`error_from_json` | recorded `chat_error` / `stream_error` | round-trips (test (10)) | — |
| 5 | `src/core/batch_recording.cpp:121,153` | batch item `klass` field, default `"transient"` | via #3 | — |
| 6 | `tools/test_driver/test_driver.hpp:1413-1416, 1435` | MCP test driver: scripted `error.class`, exported class | **delegates** to #2; the accepted list and error text now include `canceled` | — |
| 7 | `include/agentengine/rt/workflow_supervisor.hpp:420` `failure_marker` | `switch` → `"executor 'x' failed (<class>)"` | **delegates** to #2; never reached for `canceled` (row 15) | — |

### Retry predicates

| # | Site | What it does | canceled | deadline |
|---|---|---|---|---|
| 8 | `include/agentengine/core/error.hpp:64` | **new** `is_retryable(class, retry_budget)` | never | `resource`: `fresh` only |
| 9 | `include/agentengine/core/model_call_gateway.hpp:134` (used at :378, :555) | gateway retry within a tier | calls #8 `shared` → no retry | no retry (was already transient-only) |
| 10 | `include/agentengine/rt/multi_agent.hpp:72` (used at :270) | `spawn_with_retry` | calls #8 `fresh` → no retry (test R3) | retried by a fresh child (unchanged) |
| 11 | `include/agentengine/rt/workflow_supervisor.hpp:1317` (used `src/rt/workflow_supervisor.cpp:911, 1008`) | edge `retry` | calls #8 `fresh` → no retry (test D5c) | retried (unchanged) |
| 12 | `src/rt/agent_session_core.cpp:1152-1156` | ADR-177 session stream-retry predicate | calls #8 `shared`; the `code != "net.cancelled"` string special case is **removed** (row 24 makes it `canceled`) | not retried (unchanged) |
| 13 | `include/agentengine/eval/eval_screen_common.hpp:197` `worth_a_retry` | eval trial re-draw | calls #8 `shared` (= transient only, unchanged by design: a re-draw must not hide a lesson-caused `resource` fault) | not re-drawn |
| 14 | `src/rt/workflow_supervisor.cpp:917` | no retry once the workflow's own cancel is requested | unchanged (the run's own cancel; complements #11) | — |

### Fallback, fail-over, propagate

| # | Site | What it does | canceled | deadline |
|---|---|---|---|---|
| 15 | `src/rt/workflow_supervisor.cpp:1355` `route_from` (the `switch` over `edge_failure_policy`, :1357-1425) | a failed executor's edge policy: fail / propagate (marker) / fallback | **new**: `canceled` returns `workflow_failed` before the switch — never a marker, never the fallback executor, whatever the edge declares (tests D5a, D5b). The run's OWN cancel is decided before routing (:1099, issue #156) and never reaches this | unchanged (`resource` is rerouted like any failure) |
| 16 | `include/agentengine/core/model_call_gateway.hpp:141` `fails_over` (used :304, :316, :433, :445) | gateway tier cascade (primary → fallback providers), `call()` and `call_stream()` | **new**: a `canceled` tier failure is returned, not failed over (test G16) — found by this inventory, not named in D6 | unchanged |
| 17 | `include/agentengine/rt/workflow_supervisor.hpp:515` `inner_run_failure_class` (used `workflow_as_executor.hpp:181`, `workflow_as_chat_client.hpp:546`) | class a wrapped workflow's non-completed run carries outward | **new**: `cancelled` → `canceled` (was `contract`) | **new**: `bound_deadline` → `resource` (was `contract`); others stay `contract` |

### Other class predicates

| # | Site | What it does | canceled | deadline |
|---|---|---|---|---|
| 18 | `include/agentengine/eval/eval_screen_common.hpp:58` `is_measurement_fault` | transient or a canceled run is a measurement fault | `e.code == "run.canceled"` → **`e.klass == canceled`** | — |
| 19 | `src/rt/agent_session_core.cpp:1896` | after a failed model call: run's own cancel → `finish_canceled()` | `code == "run.canceled" \|\| stop_requested()` → **`stop_requested()` alone** (the drain raises `run.canceled` only when that token fired). A `canceled` the run did not ask for → `run_failed`, class carried (test K8) | — |
| 20 | `src/rt/workflow_supervisor_batch.cpp:443` | a non-transient batch poll error counts toward fail-closed | unchanged: no backend's `poll` produces `canceled` today; the run's cancel ends the round first | — |
| 21 | `tools/batch_infer.cpp:195` | same, CLI | unchanged (same reason) | — |
| 22 | `src/backends/native_jail/mediated_filesystem_adapter.cpp:110`, `..._posix.cpp:310` | `exists()`: policy/contract propagate, anything else = "absent" | unchanged: `open_within_mount_root` has no cancellation path. Open question below | — |
| 23 | `include/agentengine/core/model_call_gateway.hpp:372, 537` `breaker.on_result` | every failed attempt counts toward the circuit breaker | unchanged — a canceled attempt still counts as a breaker failure. Open question below | — |

### Producers of a cancellation

| # | Site | Before | After |
|---|---|---|---|
| 24 | `src/sandbox/net_egress_proxy.cpp:98` `net.cancelled` | `transient` | `canceled` |
| 25 | `src/rt/agent_session_core.cpp:1527` `finish_canceled` → `run.canceled` (ADR-178) | `fatal` | `canceled` (ADR-178 amended) |
| 26 | `include/agentengine/rt/agent_session_trust.hpp:136` drain's `run.canceled` | `fatal` | `canceled` |
| 27 | `include/agentengine/core/replay_chat_client.hpp:85` replayed `"cancelled"` terminal | `fatal` | `canceled` |
| 28 | `include/agentengine/core/recording_chat_client.hpp:250` inner stream cancelled | `fatal` | `canceled` |
| 29 | `include/agentengine/eval/eval_trial.hpp:341` summarizer stream cancelled | `fatal` | `canceled` |
| 30 | `include/agentengine/rt/background_job_runner.hpp:312` canceled before it started (`tool.canceled_no_effect`) | `policy` | `canceled` |
| 31 | `include/agentengine/rt/background_job_runner.hpp:390` `background_job.parent_canceled` | `policy` | `canceled` |
| 32 | `include/agentengine/rt/background_job_runner.hpp:544` runner shut down (`tool.canceled_no_effect`) | `transient` | `canceled` |
| 33 | `src/rt/workflow_supervisor_batch.cpp:488` vendor batch item `canceled` | `transient` | `canceled` |
| 34 | `tools/test_driver/test_driver.hpp:380` `test.real_tool_canceled` | `resource` | `canceled` |
| 34a | `include/agentengine/rt/agent_session_trust.hpp:214` drain wraps any unclean stream as `run.stream_incomplete` | `transient` always (the inner class was deliberately not promoted) | `canceled` when the inner stream ended `canceled`, `transient` otherwise. **Found by test K8, not by the grep**: a re-classifying wrapper has no `failure_class` comparison to match |
| 35 | `include/agentengine/rt/background_job_runner.hpp:703` `error_code == kToolCanceledNoEffect` | string-keyed | **kept**: the code says "stopped AND performed no effect", which the class does not (an at_most_once tool stopped part-way is `canceled` too, but `indeterminate`) |

### Producers and mappers of a deadline

| # | Site | Class | Decision |
|---|---|---|---|
| 36 | `include/agentengine/core/replay_chat_client.hpp:88` replayed `"deadline_exceeded"` | `transient` → **`resource`** | the D6-named reconciliation |
| 37 | `src/rt/workflow_supervisor_batch.cpp:431` batch `max_wait exceeded` | `transient` → **`resource`** | a deadline; an edge retry treats both alike (`fresh`) |
| 38 | `src/rt/workflow_supervisor_batch.cpp:486` vendor batch item `expired` | `transient` → **`resource`** | the vendor's completion window is a deadline |
| 39 | `include/agentengine/rt/workflow_supervisor.hpp:515` `bound_deadline` outward | `contract` → **`resource`** | row 17 |
| 40 | `src/core/tool_pipeline.cpp:231` `tool.deadline_exceeded` | `resource` | already right |
| 41 | `include/agentengine/rt/workflow_as_chat_client.hpp:351` `...deadline_exceeded` | `resource` | already right |
| 42 | `src/backends/native_jail/mediated_shell_dispatch.cpp:424` `shell.wall_clock_timeout` | `resource` | already right |
| 43 | `src/sandbox/net_egress_proxy.cpp:107, 419, 448, 496, 665, 679, 709` I/O timeouts (`net.connect_failed`) | `transient` | **kept**: a socket I/O timeout is a network condition, not the caller's budget |
| 44 | `src/rt/workflow_supervisor_batch.cpp:468` job ended without a result for an item | `transient` | **kept**: cause unknown (vendor failure or expiry) |
| 45 | `include/agentengine/core/model_call_gateway.hpp:380-395` deadline leaves no room for a retry | returns the last attempt's own error | **kept**: the deadline only stops the retry; the failure reported is the attempt's |
| 46 | `src/backends/native_jail/python_worker_mediation.cpp:270` `tool.deadline_exceeded` → Python `TimeoutError` | code-keyed | kept; a canceled tool call raises `RuntimeError` in the guest today — open question |

### Protocol mappings (I7)

| # | Site | Wire today | Decision |
|---|---|---|---|
| 47 | `include/agentengine/protocol/a2a/server.hpp:154` `send_message` failed outcome | every failure `TASK_STATE_FAILED` | **`canceled` → `TASK_STATE_CANCELED`**, others `FAILED`. A2A v1.0 has a terminal `CANCELED` state (docs/research/2026-a2a-and-agui-detail.md: "Terminal: COMPLETED, FAILED, CANCELED, REJECTED"). Test D3-11 |
| 48 | `include/agentengine/protocol/a2a/streaming.hpp:111` `run_canceled` → `status(task_state::canceled)` | already `CANCELED` | unchanged (event-kind keyed) |
| 49 | `include/agentengine/protocol/agui/projection.hpp:73` `run_canceled` → `RunError{"run canceled", "run.canceled"}` | `RUN_ERROR` | **kept**: AG-UI has no cancel event — a run ends with exactly one of `RUN_FINISHED`/`RUN_ERROR`, and `RUN_ERROR` is the sole error event (same research doc, AG-UI lifecycle). The class is visible as the `code` |
| 50 | `include/agentengine/protocol/mcp/server.hpp:446, 512` task status | `"completed"` (a tool result with `isError`) / `"cancelled"` (only via `tasks/cancel`) | **kept**: no class reaches MCP's wire; MCP says a tool error is `completed` + `isError`, and `notifications/cancelled` MUST NOT be used for tasks (docs/research/2026-mcp-protocol-detail.md). A tool the engine stopped is `completed` + `isError` |
| 51 | OTel GenAI `error.type` | — | **no site exists**: no exporter maps a class to `error.type` anywhere in the tree; open question |

### Event-kind consumers of `run_canceled` (no class involved)

| # | Site | Decision |
|---|---|---|
| 52 | `include/agentengine/rt/agent_session_core.hpp:1000` → `run_end_reason::canceled` | unchanged |
| 53 | `tools/cli_chat.cpp:1107`, `tools/test_driver/test_driver.hpp:1005` names | unchanged |
| 54 | `tools/test_driver/test_driver.hpp:1383` `RunOutcome{..., "run.canceled", ...}` (a code, not a class) | unchanged |
| 55 | `include/agentengine/rt/workflow_as_executor.hpp:118`, `workflow_supervisor.hpp:509` `workflow_status::cancelled` tags | unchanged spelling (`cancelled` is the status's wire tag) |

### Step 1-4 I/O results (their own `canceled`, no `failure_class` yet)

| # | Site | Decision |
|---|---|---|
| 56 | `include/agentengine/rt/sleep.hpp` `sleep_status` | none yet; "arrives with the seam conversion" (its own comment). Rule when converted: a stop → `canceled`; a deadline → `resource` + `deadline_exceeded` (§4.4) |
| 57 | `include/agentengine/rt/offload.hpp` `offload_status::canceled` | same |
| 58 | `include/agentengine/rt/tcp.hpp` / `pal::tcp_error::canceled` | same |
| 59 | `include/agentengine/rt/dns.hpp` `dns_error::canceled`, `connect_error::canceled` + `timed_out` | same; `timed_out` (the per-attempt deadline) must become `resource`, not `canceled` |
| 60 | `include/agentengine/rt/tls.hpp` `tls_error::canceled` | same |
| 61 | `include/agentengine/rt/process.hpp` canceled operations | same |

`rt/http*` (being written in parallel) and `rt/task.hpp`/`rt/lanes.hpp`/`rt/runtime.hpp`/`rt/scope*` (structured
concurrency, parallel) were not edited here; the HTTP client's error mapping and `when_all`'s sibling
cancellation must produce `canceled` for a requested stop and `resource` for a deadline.

## Open questions

1. **Circuit breaker** (#23): should a canceled attempt count as a breaker failure? It is not the provider's
   fault; skipping it changes the half-open probe bookkeeping, so it is left for a gateway change with its own
   tests.
2. **`when_all` ordering** (#15): when a sibling fails and the others are canceled, `route_from` handles replies
   in executor order, so the run may name a canceled sibling as `failed_executor` instead of the one whose
   failure caused the cancel. No such in-round cancel exists today; structured concurrency should route the
   originating failure first.
3. **OTel `error.type`** (#51): no exporter exists. When one is written, `canceled` should map to a stable
   value (the class name) rather than the code.
4. **Python guest** (#46): a canceled tool call raises `RuntimeError`; `asyncio.CancelledError`/`KeyboardInterrupt`
   would be wrong for a synchronous script. Keyed by code today, not class.
5. **`exists()`** (#22): a future cancellable mount lookup would report "absent" for a canceled lookup.
6. **Re-classifying wrappers** (row 34a's shape): a wrapper that folds an inner error into a fixed class is
   invisible to a `failure_class` comparison grep. A second pass over wrappers that copy an inner `.message`
   into a fixed-class error found `core/ledger_impl.hpp`, `core/ledger.cpp`, `sandbox/sandbox_runtime.hpp`
   and `backends/kata/kata_backend.cpp` (all `fatal`/`policy`); none has a cancellation source today. When the
   async seams land, a wrapper whose inner call can be stopped must keep `canceled`, as row 34a now does.
