# devsim/rerun3 — a document-processing pipeline on AgentEngine, built from the docs

**Re-run of the exercise, against the improved docs.** Everything below is something I
verified in this session on Windows 11 x86-64, MSVC 19.51 (VS 18 Community), C++23 via
`-std:c++latest`, CMake 3.28+, Ninja. No existing file anywhere in the repo was modified
(only `devsim/rerun3/**` was created).

Files: [`main.cpp`](main.cpp) · [`CMakeLists.txt`](CMakeLists.txt) ·
[`build.bat`](build.bat) · build/run logs: [`build-clean.log`](build-clean.log) ·
[`run-stdout.log`](run-stdout.log) · [`run-stderr.log`](run-stderr.log).
Every intermediate compile log is kept beside them (`build-attempt1..6.log`) as evidence for §9.

Clean consumer build: **18.3 s**, **30 build steps**, 9 engine static libraries.
Run: exit code **7** (non-zero, by design — see §6).

---

## 0. The headline

**Fallback log: 3 numbered entries. Of those, exactly ONE was a hard blocker.**

| # | What I had to go to source for | Blocking? |
|---|---|---|
| 1 | `text_message(...)` — used by ~15 doc snippets, never defined anywhere on the docs site, and **not** engine API | **YES** — compile error `C3861: 'text_message': identifier not found` |
| 2 | The value for `AGENTENGINE_ASIO_ROOT` in an offline build | yes (only if you cannot fetch at configure time) |
| 3 | `workflow_status`'s enumerator list, to confirm the docs' 13-row table is exhaustive | no — verification only; the docs' table was complete and correct |

Everything else — the graph structs, the edge kinds, the fan-out/fan-in semantics, the
executor-body signature and its index-parallel wiring, getting the result out, all thirteen
run statuses and their three kinds, the error model, and the whole consumer CMake — came from
the docs. My first compile attempt produced **exactly one class of error** (`text_message`),
and after reconstructing that one helper from a docs page I had already read, it compiled,
linked and ran.

---

## 1. Getting started — where each piece came from

### (a) Include the engine — **found in docs**

- `web/marketing/src/components/ApiFirstAppReference.tsx:80-108` is a complete, verified
  consumer `CMakeLists.txt`: `cmake_minimum_required(VERSION 3.28)`, `AGENTENGINE_BUILD_TESTS`
  / `AGENTENGINE_BUILD_EXAMPLES` forced OFF at `:86-87` *before* `add_subdirectory()` at `:95`,
  and `target_link_libraries(myapp PRIVATE agentengine::core)` at `:101`. Prose for *why* the
  two cache variables are required (and the measured 2,336 → 159 build-statement difference)
  is at `:174-195`.
- Header names: `agentengine/workflow/graph.hpp` is named in a snippet comment at
  `web/marketing/src/data/apiContent.ts:2918` and as a CodePanel title at
  `ApiWorkflowPatternDetail.tsx:388`; `rt/workflow_supervisor.hpp` at
  `ApiWorkflowReference.tsx:329`; `rt/block_on.hpp` at `ApiSnippetConventions.tsx:16-18`;
  `core/content.hpp` at `ApiFirstAppReference.tsx:17`;
  `core/tool_call_extraction.hpp` at `ApiSnippetConventions.tsx:28`.
- I wrote `CMakeLists.txt` as the docs' panel says, with one change forced by my layout: the
  vendored engine is the repo this program lives inside, so `add_subdirectory()` points at
  `${CMAKE_CURRENT_SOURCE_DIR}/../..` instead of `third_party/AgentEngine`. The docs say
  "point this at wherever you put the repository", so this is within what they describe.
- **Verified:** the docs' claim held exactly. 30 build steps; 9 engine static libraries
  (`pal_cpu`, `message_json`, `rt_file_log`, `reactor_asio`, `tool_pipeline`, `chat_recording`,
  `docker_surface`, `rt_session`, `rt_workflow`); configure 0.4 s; whole clean build 18.3 s.

### (b) Declare the graph as data — **found in docs**

- The minimal graph and the full struct skeleton are at `apiContent.ts:2908-2915` and
  `:2917-2967` (`Executor`, `Edge`, `TerminationBound`, `Workflow`, the seven `edge_kind`
  enumerators, `executor_kind`'s four). The Concurrent (fan-out/fan-in) graph is
  `apiContent.ts:2991-3006`; the Router (`switch_case`) graph is `:3008-3023`.
- The namespace trap is documented at `ApiWorkflowReference.tsx:97-107`: everything is
  `agentengine::workflow::` **except** `sharing_mode`, which is `agentengine::sharing_mode`.
  I avoided it by using the 4-field positional form from `apiContent.ts:2911`
  (`Executor{"src", executor_kind::function, "Text", "Text"}`), which leaves `worktree_mode` at
  its `branch` default. The defaults are spelled out at `ApiWorkflowReference.tsx:117-126`.
- Fan-in semantics are documented precisely at `apiContent.ts:2779` and
  `ApiWorkflowReference.tsx:75-77`: the target runs **once per round**, receiving one
  `ContentItem` per contributing branch **in graph-declared order**.
- The bound is documented as mandatory at `apiContent.ts:2915`, `:2957` and
  `builderContent.ts:170-172` ("a type-correct graph with no bound is still rejected").

### (c) Supply executor bodies and know how they match nodes — **found in docs**

- The exact type is given at `apiContent.ts:3080-3083`: *"`rt::ExecutorBody` is exactly
  `std::function<result<ExecutorOutcome>(Message const&, EffectContext&)>`, and a `Message`
  converts implicitly into an `ExecutorOutcome`"*. That sentence is the single most useful line
  in the whole workflow documentation — it is what let me write plain `result<Message>` bodies
  and drop them into a `std::vector<ExecutorBody>`.
- The **matching rule** — bodies are positional, by index, parallel to `wf.executors` — is at
  `ApiPizzaOrderReference.tsx:239-242` (*"'bodies' is parallel to `wf.executors` BY INDEX"*,
  including `{}` as the deliberate empty slot for a `request_port`) and restated at
  `apiContent.ts:3272`.
- Body examples: `apiContent.ts:3066-3079` (Sequential) and `:3085-3114` (Concurrent merge,
  including how to read a merged fan-in delivery). `ExecutorOutcome{message, routes}` is shown
  at `apiContent.ts:3018-3022`.

### (d) Get the result out of a run — **found in docs**

- `WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("hi")}));` at
  `apiContent.ts:2988`, `:3005`, `:3155`, `:3189`; the output is read as `r.output` at
  `:2989` and `:3134`; `r.rounds` at `:3159`. `run_workflow`'s exact type
  (`agentengine::rt::task<WorkflowResult>`) is at `eventsContent.ts:498`.
- `drive(...)` is **explicitly** called out as example-local, with the engine equivalent named:
  `ApiSnippetConventions.tsx:16-18` — `agentengine::rt::block_on(task)`, `rt/block_on.hpp`.
  I used `block_on` and it bound directly to `WorkflowResult` on the first attempt.
- **Verified:** fan-out/fan-in run → 3 rounds, aggregator invoked once, output
  `"upper:QUARTERLY REPORT + lower:QUARTERLY REPORT + title:QUARTERLY REPORT"` — exactly the
  graph-declared order and once-per-round behaviour the docs describe.

### (e) Read and act on the run's status — **found in docs**

- `ApiWorkflowReference.tsx:329-363` is a complete, correct reference: all **13**
  `workflow_status` values, each labelled with one of three kinds (bound / fault / refusal,
  plus success and waiting), with the guidance that a handler *"that treats 'not completed' as
  one thing will treat a deliberate round cap as a crash, or retry an admission denial that
  will never succeed."* The plain-string printer is named with its full namespace at `:360`:
  `agentengine::rt::workflow_status_tag(status)`.
- I wrote my `kind_of()` switch from that table alone. **Verified against the header:** the
  enum at `include/agentengine/rt/workflow_supervisor.hpp:438-486` has exactly the 13
  enumerators the docs list — the docs' table is exhaustive and its kind labels match the
  header's own comments (`cancelled` and the four `bound_*` are documented as "honest, expected,
  non-error termination").

### (f) Handle errors — **found in docs**

- `ApiFirstAppReference.tsx:259-292` is the error model: `result<T>` **is**
  `std::expected<T, agentengine::error>`; `code` is the stable machine-readable string **to
  branch on**, `message` is host-facing prose, `klass` is a `failure_class`; only `transient`
  and `resource` are retryable. Five real error codes are tabulated at `:278-284`.
- Two workflow validation codes are named at `ApiPizzaOrderReference.tsx:224-227`:
  `workflow.missing_case_label` (empty label on a `switch_case` edge) and
  `workflow.unexpected_case_label` (a label on a non-switch edge). `workflow.unbounded` is
  named at `ApiBuilderReference.tsx:181-189`.
- **Verified:** the refused-graph path prints the engine's own `code` + `message` and exits
  non-zero:
  ```
  [validate-unbounded] engine error: code=workflow.unbounded message=workflow declares no
  termination bound -- 014 §2 requires MaxRounds, a deadline, or a budget; an unbounded
  workflow does not run
  ```

### (g) Build / link — **found in docs**

Complete at `ApiFirstAppReference.tsx:80-108` (see (a)). The `#include` list came from snippet
comments and the conventions note (see (a)). Nothing about the build had to be
reverse-engineered — except the offline-vendored-dependency path (fallback #2).

---

## 2. Fallback log

### #1 — `text_message(...)` is undefined on the docs site and is not engine API — **BLOCKER**

`text_message(std::string)` is used verbatim in at least 15 documented snippets:
`apiContent.ts:2988, 3005, 3132, 3155, 3189, 3202, 3213, 3216, 3227, 3230, 3233, 3313`, and
`ApiPizzaOrderReference.tsx:246`. It is **never defined** anywhere in
`web/marketing/src/data/*.ts` or `web/marketing/src/components/*.tsx`, and it is **not** engine
API — `grep` across `include/agentengine/**` finds no `text_message` at all. Every engine
example defines its own private copy (`examples/09_concurrent_workflow.cpp:52-60`).

It is also **absent from the one place that exists to say so.** The new shared
"How to read the code on this page" note (`ApiSnippetConventions.tsx:13-44`) lists `drive`,
`check`, `user_message`, page-local aliases, `caps/store/held`, and `set_capabilities` — but
not `text_message`, even though it appears far more often than any of them.

**How I resolved it:** I reconstructed it from documented types I had already read — the
first-app program shows `Message`, `role::assistant`, `ContentItem{origin, value}`,
`content_origin::assistant`, `Text{"..."}` (`ApiFirstAppReference.tsx:29-47`), and the
Concurrent merge snippet shows `.value` / `std::get_if<Text>` (`apiContent.ts:3088-3097`).
My reconstruction is byte-for-byte equivalent to `examples/09_concurrent_workflow.cpp:52-60`.
I then confirmed it by reading that example.

**Why it matters:** this is the *only* thing that stopped the docs' workflow material from
being paste-and-compile, and it is a one-line addition to a note that already exists.

### #2 — `AGENTENGINE_ASIO_ROOT` has no documented value

`ApiFirstAppReference.tsx:89-92` documents the escape hatch for an offline build:

```cmake
set(AGENTENGINE_VENDOR_ASIO OFF CACHE BOOL "" FORCE)
set(AGENTENGINE_ASIO_ROOT "<dir containing include/asio.hpp>" CACHE PATH "" FORCE)
```

and explains that the engine otherwise fetches Asio over the network. It does not say where a
previous fetch put it. To build offline I had to discover the FetchContent source directory
myself (`build-dev/_deps/ae_vendored_asio-src/include/asio.hpp`, i.e. the value is that
directory's parent), which meant listing `_deps` in an existing engine build tree. This is
avoidable — allowing the network fetch works and needs no fallback — but the docs present the
offline path as the supported one and then leave its argument unspecified.

*Not counted as an engine-source read, but recorded:* I also read `CMakePresets.json`,
`build-dev/CMakeCache.txt` and the repo `CMakeLists.txt` option list purely to find the
toolchain (ninja path, `cl.exe`, generator). That is environment discovery, not a doc gap.

### #3 — verifying that the 13-row status table is exhaustive (verification only, not a blocker)

`ApiWorkflowReference.tsx:343-357` lists 13 outcomes. Nothing on the page says the list is
complete, and a `switch` over an enum at default warning level does not prove exhaustiveness.
I read `include/agentengine/rt/workflow_supervisor.hpp:438-486` to confirm. **The docs' table
is exactly right — all 13, no more, no fewer, and the three kinds are labelled as the header
comments label them.** Logged only because I did read source.

---

## 3. Combining patterns — the `switch_case` variant

### Does the docs warn you about routing feeding a fan-in join? — **Yes, explicitly and correctly.**

`ApiWorkflowReference.tsx:78-95` (English copy) is a prominent `trap`-styled diagnostic box
titled *"A fan-in join waits for every declared source"*. It states the mechanism (a `fan_in`
target with 2+ declared sources is held until **every** one resolves, citing the `HeldFanIn`
comment in `rt/workflow_supervisor.hpp`), names the exact failure (*"the graph
`validate_workflow` accepts can finish with `workflow_status::completed` and an **empty**
output, having invoked the aggregator zero times"*), gives the rule (*"Do not route into a
fan-in join … route **before** the fan-in rather than inside it"*), and gives the defensive
practice (*"count the aggregator's invocations before you trust the status"*).
`apiContent.ts:2779` independently gives the fan-in delivery contract.

### Did I hit surprising behaviour while reporting a successful status? — **Yes, exactly as warned.**

Both variants were built and run. Reproduced output:

**Variant A — `switch_case` feeding the fan-in (the trap).**
`classifier --switch_case{invoice|crash}--> {upper, lower}`, then `upper --fan_in--> agg`,
`lower --fan_in--> agg`. Run twice, once per classifier outcome:

```
[validate] graph doc-pipeline-router-into-join is well-formed (the graph the docs' trap describes)
[router-into-fanin] classifier needle=invoice aggregator_invocations=0
[router-into-fanin] status=completed        kind=success  rounds=2 output=""
[router-into-fanin] had a successful status, empty answer, aggregator invoked 0 times
[router-into-fanin] classifier needle=crash aggregator_invocations=0
[router-into-fanin] status=completed        kind=success  rounds=2 output=""
[router-into-fanin] had a successful status, empty answer, aggregator invoked 0 times
```

Every element of the warning reproduced, on both classifier outcomes:
`validate_workflow` accepted the graph; the run reported `workflow_status::completed`; the
engine's own tag printed `"completed"`; `r.output` was empty; the aggregator's invocation
counter read **0**.

**Variant B — the same `switch_case` decision, routed before the join.**
`classifier --switch_case--> {upper, lower}` → `join` → `join --fan_out--> {w1, w2}` →
`{w1, w2} --fan_in--> agg`:

```
[validate] graph doc-pipeline-router-before-join is well-formed
[router-before-fanin] aggregator_invocations=1
[router-before-fanin] status=completed        kind=success  rounds=5
[router-before-fanin] output="w1:UPPER:PLEASE PROCESS MY INVOICE -> UPPER + w2:UPPER:PLEASE PROCESS MY INVOICE -> UPPER"
```

The aggregator ran exactly once with two merged items. The docs' advice works as stated.

### The thing that genuinely surprised me

**Reaching the documented trap is harder than the docs imply, and my first attempt was refused
for a reason the docs never mention.** My initial Variant A left a third node (`title`) that
the switch no longer reached. `validate_workflow` refused the graph outright:

```
[validate-router-into-join] engine error: code=workflow.unreachable_executor
message=executor 'title' is unreachable from start 'classifier'
```

`workflow.unreachable_executor` appears **nowhere** in `web/marketing/**` (I checked, including
all 8 pattern pages, `workflow.html`, `builder.html`, `first-app.html` and `apiContent.ts`).
So: the docs describe a validator that accepts trap graphs, and the docs' tabulated error-code
list (`ApiFirstAppReference.tsx:278-284`) plus the label codes
(`ApiPizzaOrderReference.tsx:224-227`) plus `workflow.unbounded`
(`ApiBuilderReference.tsx:186`) omit a check that a plausible first attempt trips over. The
reachability rule is *good* — it is the reason my first attempt failed instead of silently
producing a starved graph — but a reader cannot anticipate it. See reverse-engineered item #1
in §6.

### Secondary surprise, worth a line in the docs

`routing_failed` and the fan-in trap are **indistinguishable from the outside** apart from the
status string. Both give `rounds` ≥ 1, `output == ""`, and no exception:

```
[routing-failure] status=routing_failed   kind=fault    rounds=1 output=""
[router-into-fanin] status=completed      kind=success  rounds=2 output=""
```

The docs' advice ("count the aggregator's invocations before you trust the status") is the only
thing that separates them, and it is stated in the trap box only — not in the status table entry
for `completed`, which is where someone would look while writing a handler.

---

## 4. Statuses — could I write a correct handler from the docs alone?

**Yes, for all thirteen.** The table at `ApiWorkflowReference.tsx:343-357` gives every
enumerator, and the three-kind framing plus the prose at `:331-341` is enough to write the
right *policy* per outcome (bound = expected, don't fail; fault = fail; refusal = nothing
changed, safe to retry the same call; suspended = waiting, dump your state and come back).
`workflow_status_tag` at `:360` gives printing. My handler is written entirely from that page.

**Outcomes I had to go to the header to confirm:** none for *existence*. **One** for
*exhaustiveness* — I read the enum at `workflow_supervisor.hpp:438-486` to confirm the list was
complete rather than merely a sample (fallback #3). The header confirmed the docs table exactly.

**What the docs do NOT give that a handler author needs:**

| Missing | Consequence |
|---|---|
| The **full field list** of `WorkflowResult` | I used `status`, `output`, `rounds` (all documented by usage). `open_interactions` / `unopened_ports` are named only in prose at `ApiWorkflowReference.tsx:209-210`. A `suspended` handler needs the first of these, and its type is never stated. |
| Whether the engine surfaces a **`failed` edge's own error** alongside a fault status | A fault run prints `output=""` and a status tag, nothing else. `workflow_status::executor_failed`'s entry says *"a node's body failed"* — but the failed node's `result.error().code` is never shown reaching the caller. My handler can say *that* it failed and not *why*. |
| `merge_conflict` / `invalid` / `admission_denied` / `invalid_routes` triggers | Fully described in prose; a handler can be written, but not *tested*, from the docs — none of the pages show a run reaching them. |

I verified `completed`, `bound_max_rounds` and `routing_failed` for real; the other ten are
handler-only (no trigger path was reachable without a durable store, a request port and a
second principal).

---

## 5. Undefined identifiers in doc snippets

Every name a documented snippet uses without defining it, in the snippets I worked from.

| Identifier | Defined on the docs site? | Where it really comes from |
|---|---|---|
| `text_message(std::string)` | **No.** Used ~15 times. Not in the conventions note. | **Example-local.** `examples/09_concurrent_workflow.cpp:52-60`. Fallback #1. |
| `node(id)` | **No.** Used at `apiContent.ts:2994, 3011, 3028, 3049, 3171, 3240, 3261`. | Example-local, but **under a different name**: the real helper is `node_desc(...)`, defined on the docs site only at `ApiPizzaOrderReference.tsx:213-216` and used at `apiContent.ts:3144, 3163-3171`. Two names, one thing, no note. |
| `counted(body, counter)` | **No.** Used at `apiContent.ts:3126-3127, 3183-3184, 3278`. | Example-local. `examples/09_concurrent_workflow.cpp:96-106`. |
| `handler(name)` | **No.** `apiContent.ts:3126-3127`. | Example-local (`examples/10_conditional_routing.cpp`). |
| `appender(name)` | **No.** `apiContent.ts:3151, 3183-3184`. | Example-local (`examples/13`, `examples/15`). |
| `moderator(lambda)` | **No.** `apiContent.ts:3182`. | Example-local (`examples/15_group_chat_and_planner.cpp`). |
| `triage` | **Contradictory.** Zero-arg `triage()` *is* defined at `apiContent.ts:3198-3204`; the run snippet at `:3123` calls `triage(classifier_lambda)` — a different signature, same name, no note. | Example-local, two different files. |
| `identity_sink` | **No.** `ApiPizzaOrderReference.tsx:242`. | Example-local. |
| `kitchen` | **No.** `ApiPizzaOrderReference.tsx:242`. | Example-local. |
| `drive(task)` | Yes — conventions note, `ApiSnippetConventions.tsx:16-18`. | Example-local; engine equivalent `agentengine::rt::block_on`. **Resolved.** |
| `check(cond, label)` | Yes — `ApiSnippetConventions.tsx:19-21`. | Example-local. **Resolved.** |
| `user_message(text)` | Yes — `ApiSnippetConventions.tsx:22-24`. | Example-local. **Resolved.** |
| `text_of(message)` | Yes — `ApiSnippetConventions.tsx:28`. | **Engine API**, `core/tool_call_extraction.hpp`. **Resolved.** |
| `all_text_of(message)` | Yes — defined at `apiContent.ts:3088-3097`, though *used* earlier at `:3040`. | Example-local, defined on-page. **Resolved.** |
| `make_stream<T>(...)` | Yes — `ApiSnippetConventions.tsx:28`. | **Engine API**, `core/stream.hpp`. **Resolved.** |
| `validate_workflow(wf)` | **Named but never declared** — `apiContent.ts:2915, 2957`; `ApiPizzaOrderReference.tsx:237`; `builderContent.ts:171`; `ApiWorkflowReference.tsx:88`. No header, no namespace, no return type anywhere. | I guessed `agentengine::workflow::validate_workflow` returning a `result`-like with `.has_value()`/`.error()`. **It compiled first try.** Lucky inference, not a documented fact. |
| `block_on(task)` | Yes — `ApiSnippetConventions.tsx:17`, fully qualified with header. Return type not stated. | **Engine API**, `rt/block_on.hpp`. Resolved; return type inferred from `drive`'s `T`. |
| `workflow_status` (the type) | **Named but never declared**; only `agentengine::rt::workflow_status_tag` at `ApiWorkflowReference.tsx:360` pins the namespace. | Inferred `agentengine::rt`. Compiled. |
| `ExecutorBody` | Yes — `apiContent.ts:3080-3083` gives namespace and full signature. | **Engine API**, `rt`. **Resolved.** |
| `ExecutorOutcome` | **Shape only.** Used as `ExecutorOutcome{message, routes}` at `apiContent.ts:3021`. Field names never stated. | Aggregate init worked. |
| `RunWorkflow` | **Shape only.** Only ever `RunWorkflow{text_message(x)}` (`apiContent.ts:2988`). Fields never documented. | A one-field aggregate init worked. `workflow_supervisor.hpp:523-526` later confirmed the comment that `RunWorkflow{input}` is a positional aggregate-init call site. |
| `WorkflowResult` | **Partially.** `status` (`:3156`), `output` (`:2989`), `rounds` (`:3159`) by usage; `open_interactions`/`unopened_ports` by prose only (`ApiWorkflowReference.tsx:209-210`). Full type never shown. | Compiled against documented usage. |
| `WorkflowSupervisor`, `initialize(wf, bodies)`, `run_workflow(...)` | Yes, by usage throughout; namespace inferable from `rt/workflow_supervisor.hpp`. | Compiled. |
| `Message`, `ContentItem`, `Text`, `content_origin`, `role` | Yes — the first-app program, `ApiFirstAppReference.tsx:29-47`, with `core/content.hpp` at `:17`. | **Engine API. Resolved.** |
| `result<T>`, `agentengine::error{code, message, klass}`, `failure_class` | Yes — `ApiFirstAppReference.tsx:259-292`. | **Resolved.** |
| `sharing_mode` | Yes — namespace trap note, `ApiWorkflowReference.tsx:97-107`. | `agentengine::sharing_mode`. **Resolved** (I sidestepped it). |
| `custom_backend`, `cap::Secret`, `Principal`, `Capability` … | Other pages, not needed here. | — |

---

## 6. Reverse-engineered items (anything not written down in the docs)

1. **`workflow.unreachable_executor`.** A `validate_workflow` check and error code with no
   mention anywhere in `web/marketing/**`. Discovered by running a plausible first-draft
   graph. Message form: `executor '<id>' is unreachable from start '<start>'`.
2. **`ExecutorOutcome`'s field names** (`message`, `routes`) — only ever inferable from a
   brace-init at `apiContent.ts:3021`, never stated.
3. **`validate_workflow`'s namespace and return type** — inferred, never stated. It worked
   under `using namespace agentengine; using namespace agentengine::workflow;`, so the
   namespace is one of those two.
4. **The aggregate-init arity of `Executor`** — the docs show both a 4-field positional form
   (`apiContent.ts:2911`) and a designated-initializer form (`ApiPizzaOrderReference.tsx:214`).
   Both work; neither is described as the recommended one.
5. **A fault run's partial results / failing node identity.** `ApiWorkflowReference.tsx:325`
   says *"a failed workflow's partial results are preserved"*, and the status table says a
   fault status identifies *that* a node failed — but no documented field carries which node,
   or that node's own `error().code`, back to the caller. `r.output` was empty on my
   `routing_failed` run.

---

## 7. Blockers, worst first

1. **`text_message` is an undocumented, non-engine helper used by every workflow snippet.**
   It is the *only* thing that stopped the code compiling, it affects the majority of snippets
   on all 8 pattern pages plus the workflow and pizza pages, and the conventions note that was
   added to solve exactly this class of problem omits it. One row in
   `ApiSnippetConventions.tsx`'s table fixes it permanently.
2. **The fan-in/router trap has no worked graph.** The warning is excellent and the mechanism
   is correct, but the docs never show a graph that reaches it. My first attempt was refused
   for an undocumented reason (blocker #3), so a reader following the warning literally will
   most likely *also* fail to reproduce the trap — and may conclude the warning is theoretical.
   A three-node code block next to the trap box (`classifier --switch_case--> {a,b}`,
   `{a,b} --fan_in--> agg`) would close this.
3. **Undocumented validator checks and codes.** `workflow.unreachable_executor` is absent from
   every error list in the docs, including the page that tabulates five real codes and the page
   that says `validate_workflow` "rejects a missing id, start, or bound, but nothing else here
   needs an explicit value" (`ApiWorkflowReference.tsx:123-124`) — which reads as an
   exhaustive statement of what the validator rejects. It is not: it also rejects unreachable
   executors. `workflow.unbounded` is documented, but on the *builder* page only, not on the
   workflow page where the bound requirement lives.
4. **`validate_workflow` has no declared type.** Not blocking (inference worked), but a reader
   who wants to check the error code or bind the result has nothing to go on.
5. **Identifier drift between pages.** `node` vs `node_desc`; `triage()` vs `triage(lambda)`;
   `all_text_of` used before it is defined on the same page. Individually trivial; together
   they are the reason the conventions note exists, and they are not all covered by it.
6. **`completed` is documented as if it means an answer exists.** The trap box says otherwise,
   but the status table's own `completed` row — *"Every reachable node resolved and the output
   selection produced a result"* — is misleading for a starved fan-in, where the output
   selection produced nothing. Cross-referencing the two would prevent the exact bug the trap
   box warns about.

---

## 8. Confidence, per requirement

| Product requirement | Confidence | Basis |
|---|---|---|
| 1. Declare a fan-out/fan-in graph as data | **certain** | docs give the struct, the seven edge kinds, the exact fan-out/fan-in graph; compiled and ran |
| 2. Real termination bound + validate before running | **certain** | `TerminationBound` documented; `validate_workflow` documented as mandatory-bound-rejecting; verified a valid graph passes and an unbounded one is refused with the engine's own message |
| 3. Run it, print final output, branch on status | **certain** | compiled and ran; output printed; status branched via the documented 13-value table; 3 statuses exercised for real |
| 4. Second variant of the SAME graph with a `switch_case` edge | **certain** | built and ran two `switch_case` variants (trap + corrected); both behaved exactly as the docs describe |
| 5. Failure path prints the engine's own error/status and exits non-zero | **certain** | verified for a validation refusal (`code=workflow.unbounded`) and a run fault (`routing_failed`); process exit 7 |
| 6. Buildable and runnable by me | **certain** | clean consumer build in 18.3 s / 30 steps; program runs |
| Consumer CMake wiring | **certain** | docs' panel used near-verbatim; the docs' own quantitative claims (30 steps, 9 libs) independently reproduced |
| Executor-body ↔ node matching rule | **certain** | stated explicitly at `ApiPizzaOrderReference.tsx:239`; verified by running a 5-body and a 7-body graph |
| Fan-in delivery semantics | **certain** | documented at three places; verified by invocation counter and by ordered merge output |
| `switch_case` + fan-in join behaviour | **certain** | reproduced the docs' exact warning on two classifier outcomes |
| Every `workflow_status` handler | **certain** for the 13 values and their kinds; **guessed** for the trigger conditions of the 10 I could not execute; **could not determine** the failing node's own error code from a fault status |
| `validate_workflow`'s complete rejection set | **could not determine** — one undocumented check found by accident; there may be more |
| `WorkflowResult`'s complete field set | **guessed** — only `status`/`output`/`rounds` confirmed; `open_interactions`/`unopened_ports` unverified |
| `text_message`'s status as engine API | **certain** it is not (grep across `include/` finds nothing) |

---

## 9. Verdict

**Could a competent C++ developer build this from the docs alone? Yes — and unlike a
docs-thin exercise, almost all of it is genuinely *in* the docs.**

The on-ramp works. `first-app.html` is a complete, verified program *and* its build file, and
its claims survived my independent check: the two cache variables are load-bearing, the result
is 30 build steps and 9 engine libraries, and the error model (`result<T>` =
`std::expected<T, agentengine::error>`, branch on `code`, don't string-match `klass`) is
stated plainly enough to write a handler from. The workflow reference page is unusually good:
the `ExecutorBody` sentence at `apiContent.ts:3080-3083`, the index-parallel body rule at
`ApiPizzaOrderReference.tsx:239`, the "13 outcomes in three kinds" framing, and the fan-in trap
box are each the kind of thing consumer docs usually leave out. The fan-in trap warning in
particular is not just correct — I reproduced its exact failure mode, including the successful
status and the empty answer, on two separate classifier inputs.

The single real defect is that **`text_message` is presented as engine API when it is not**:
used in every workflow snippet, defined nowhere on the docs site, and missing from the
conventions note that was written to prevent precisely this. It cost me one compile iteration
and about ten minutes, because I could reconstruct it from the first-app page's `Message` /
`ContentItem` / `Text` usage — I did not have to guess at the shape. A developer who had not
read `first-app.html` would be stuck, because the workflow pages never show a `Message` being
built.

Second-order friction is all about the validator's edges: `workflow.unreachable_executor` is
undocumented and will bite anyone who edits a graph into a switched shape, and the docs' own
sentence about what `validate_workflow` rejects reads as exhaustive when it is not.

**Time, honestly:** the clean consumer build is **18.3 s** and 30 build steps — the engine
builds itself as part of your project, and the two cache variables really do collapse it to
that. My own program took **five** configure/build invocations. **Attempt 1** failed on the
`text_message` gap and nothing else — one unknown name, ten errors, all the same name.
**Attempt 2 was already green** and the program ran. Attempts 3 and 4 failed on my own typos
(a duplicated function after an edit, a range-`for` over a braced list, an unbalanced paren);
attempt 5 was green and is the committed program.

Realistically, a competent C++ developer with the docs open, no access to `examples/`, and no
prior AgentEngine knowledge would spend **2–3 hours** to reach my `main.cpp`: roughly 30–40
minutes on the build and the on-ramp, 30–45 minutes on the fan-out/fan-in graph and its status
handling (the docs carry this well — it is the part that came out right first time), and the
remaining hour on the `switch_case` variant — most of which is a losing fight against the
fan-in trap until they find the warning, and then a second fight against the undocumented
reachability check when they try to build the trap on purpose. Add 30–60 minutes if they are
unlucky enough to try the offline build path before the online one, since
`AGENTENGINE_ASIO_ROOT`'s value is not documented. With `examples/` available — which the
snippet convention note itself points readers toward — the first blocker disappears and the
estimate is closer to **1.5–2 hours**.
