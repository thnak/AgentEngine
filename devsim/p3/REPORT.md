# AgentEngine as a consumer: building a document-processing pipeline from the docs

**Product built:** `devsim/p3/main.cpp` — a document pipeline that fans out to three worker nodes,
fans in to one aggregator, and produces one answer; a second variant of the same graph where the
fan-out edge is replaced by a `switch_case` chosen by a classifier; a bounded cycle; and three
failure paths that print the engine's own error/status and exit non-zero.

**Deliverables:** [`main.cpp`](main.cpp), [`CMakeLists.txt`](CMakeLists.txt), this report.

**Result:** builds and runs. MSVC 19.51.36260.0, CMake, Visual Studio 18 2026 generator, x64,
Release. All 8 scenarios verified; exit codes verified for both the success and failure paths.

```
p3_doc_pipeline all              -> exit 0   (fanout completed, router completed x3 docs, bounded)
p3_doc_pipeline mixed-fanin      -> exit 0   (the trap: status=completed, answer EMPTY)
p3_doc_pipeline invalid-graph    -> exit 1   (workflow.unbounded)
p3_doc_pipeline unroutable       -> exit 1   (workflow_status::routing_failed)
p3_doc_pipeline body-error       -> exit 1   (workflow_status::executor_failed)
p3_doc_pipeline demo-failures    -> exit 1
```

---

## Getting started

The exact sequence of doc pages/files read, per question. Line numbers are against the doc tree as
of this run — see **Blocker 1**, because they moved underneath me mid-session.

### (a) Include the engine — **found in docs** (pattern), **had to reverse-engineer** (the actual headers)

- **Found in docs:** `web/marketing/src/components/ApiFirstAppReference.tsx:11-17` — page
  `api/first-app.html` — is the only page in the site with real `#include` lines:
  `#include "agentengine/core/agent.hpp"`, `.../agent_session_bridge.hpp`, `.../chat_client.hpp`,
  `.../content.hpp`. Nothing else on the site shows a single `#include` (grep for
  `#include <agentengine` across `web/marketing/src` → 0 hits; the workflow pages only carry header
  paths as *comments*, e.g. `apiContent.ts:2918` `// include/agentengine/workflow/graph.hpp`).
- **Reverse-engineered:** the three headers the *workflow* surface actually needs —
  `agentengine/core/content.hpp`, `agentengine/core/tool_call_extraction.hpp` (for `text_of`), and
  `agentengine/rt/workflow_supervisor.hpp` — came from `examples/09_concurrent_workflow.cpp:27-29`.
  No workflow, builder, concurrent, router, or pizza-order page shows an include line for any of
  them.

### (b) Declare the graph as data — **found in docs**

Fully sufficient, no fallback needed.

- `apiContent.ts:2908-2915` — `workflowMinimalSnippet`: the two-executor, one-edge, one-bound minimum.
- `apiContent.ts:2917-2967` — `workflowGraphSnippet`: the real `Executor`, `Edge`, `TerminationBound`,
  `Workflow` shapes and both enums, verbatim from `include/agentengine/workflow/graph.hpp`.
- `apiContent.ts:2774-2793` — `workflowEdgeKinds`: all seven edge kinds and what each means,
  including `fan_out`, `fan_in`, `switch_case`, and `switch_default`.
- `apiContent.ts:2991-3006` — `workflowConcurrentSnippet`: the exact fan-out/fan-in shape I copied.
- `apiContent.ts:3008-3023` — `workflowRouterSnippet`: the exact `switch_case` shape, including a
  body returning `ExecutorOutcome{message, routes}`.
- Rendered by `ApiWorkflowReference.tsx:691-694`.

### (c) Supply executor bodies / how they are matched to nodes — **found in docs**

- **Matching rule, stated twice and unambiguously:** `ApiPizzaOrderReference.tsx:224` — "'bodies' is
  parallel to `wf.executors` BY INDEX"; echoed at `apiContent.ts:3272` — "bodies parallel to
  `wf.executors` by index: src, upper, lower, title, agg (the reducer)". Call form
  `sup.initialize(wf, bodies)` at `apiContent.ts:2987`, `:3004`, `:3132`.
- **Body type, stated:** `apiContent.ts:3080-3082` — "`rt::ExecutorBody` is exactly
  `std::function<result<ExecutorOutcome>(Message const&, EffectContext&)>`, and a Message converts
  implicitly into an `ExecutorOutcome`".
- **Gap:** the `initialize` *declaration* is never shown, and the docs disagree on arity — 2 arguments
  in code (`apiContent.ts:2987`), 3 in prose (`ApiPizzaOrderReference.tsx:402`). Real signature has
  four parameters, three defaulted (`rt/workflow_supervisor.hpp:739-741`). Also never stated: a
  `request_port` node's slot must be an empty `ExecutorBody` (`ApiPizzaOrderReference.tsx:227` shows
  it, doesn't say it's required).

### (d) Get the result out of a run — **found in docs**, one helper missing

- `apiContent.ts:3005` — `WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("hi")}));`
- `apiContent.ts:2988` / `:3129` — reading it: `all_text_of(r.output)`.
- **Missing:** `drive(...)` (see the undefined-identifier table); and `WorkflowResult`'s field list
  is never printed — only `.output`, `.status`, `.rounds` ever appear (`apiContent.ts:3159`). I used
  `.failed_executor` and `.open_interactions` only after reading the header.

### (e) Read the run's status — **found in docs** (comparisons only), **had to reverse-engineer** (the enum)

- **Found in docs:** `apiContent.ts:3156` and `:3190` —
  `check(r.status == workflow_status::bound_max_rounds, ...)`; `ApiPizzaOrderReference.tsx:232` —
  `check(r.status == workflow_status::completed, ...)`; `apiContent.ts:3359` names
  `workflow_status::routing_failed` in prose; `ApiWorkflowPatternDetail.tsx:434` repeats the
  `bound_max_rounds`-is-not-an-exception point.
- **Reverse-engineered:** the enum's enumerator list. The docs name only four of the thirteen
  (`completed`, `suspended`, `bound_max_rounds`, `routing_failed`). Without knowing the other nine I
  could not write an honest pass/fail branch or a `switch` that a compiler checks, so I read
  `include/agentengine/rt/workflow_supervisor.hpp:438-486`, and then used the engine's own
  `workflow_status_tag()` (`workflow_supervisor.hpp:492`, also undocumented) so nothing in my output
  paraphrases what the engine said.

### (f) Handle errors — **found in docs**

- **Pattern, documented:** `builderContent.ts:310-319` —
  `if (!built.has_value()) { fprintf(stderr, "... %s (%s)", built.error().message.c_str(), built.error().code.c_str()); }`.
  Same shape on the first-app page: `ApiFirstAppReference.tsx:63-66`, with the prose at
  `:171-172` — "every fallible call in this engine returns a `result<T>` rather than throwing".
- **Validation, documented:** `builderContent.ts:431` /
  `ApiPizzaOrderReference.tsx:222` — `check(validate_workflow(wf).has_value(), ...)`, and
  `builderContent.ts:171` states `validate_workflow` is the *shared* validator the declarative loader
  also uses.
- **Reverse-engineered:** `error`'s own layout (`klass`/`message`/`code`, `core/error.hpp:78-89`).
  The docs show only `.message` and `.code`; `.klass` and `failure_class` appear nowhere, so a
  consumer cannot print the failure class without opening the header — and `failure_class` is exactly
  what a host branches on.

### (g) Build / link — **found in docs**, but only on a page the brief did not list

- **Found in docs:** `ApiFirstAppReference.tsx:80-94` (`SNIPPET_CMAKE`), page `api/first-app.html`:
  ```
  set(AGENTENGINE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(AGENTENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/AgentEngine agentengine)
  add_executable(myapp main.cpp)
  target_link_libraries(myapp PRIVATE agentengine::core)
  ```
  With `:132-140` ("There is no `find_package(AgentEngine)` and no versioned release to install"),
  `:141-152` (why `FORCE` on a cache variable, and the measured 2,336 → 159 build statements), and
  `:153-162` (the `CMAKE_OBJECT_PATH_MAX` warning and keeping the build directory shallow).
- **Note on process:** I did not find this page until late, because the brief's page list does not
  include `first-app.html`. I had already written `CMakeLists.txt` by reverse-engineering the
  engine's own `CMakeLists.txt:92-103` (the `agentengine_core` INTERFACE target and
  `agentengine::core` alias) and `examples/CMakeLists.txt:12`. My file converged on the same four
  lines independently, which is decent evidence the page is correct.
- **What the brief's listed pages give you instead:** `content.ts:329`/`:337` —
  `cmake -S . -B build` and `cmake --build build --config Release -j 4`. Those build **the engine
  itself**, not a consumer, and there is no CMake line anywhere on `workflow.html`, `builder.html`,
  `pizza-order.html`, or any `workflow-*.html`.

---

## Fallback log

Every time I left the docs. One line each. **17 entries.**

| # | What I needed | What the docs gave me | Where I fell back to |
|---|---|---|---|
| 1 | `#include` lines for the workflow surface | Nothing — zero `#include` lines anywhere in `web/marketing/src` | `examples/09_concurrent_workflow.cpp:27-29` |
| 2 | The consumer CMake wiring | Nothing on any workflow page | engine `CMakeLists.txt:92-103`, `examples/CMakeLists.txt:12` (later found documented on `first-app.html`, which the brief omitted) |
| 3 | Which link target | Nothing | `CMakeLists.txt:103` (`agentengine::core` alias) |
| 4 | `workflow_status`'s full enumerator list | 4 of 13 named | `rt/workflow_supervisor.hpp:438-486` |
| 5 | The engine's own status string (to print the status verbatim) | Nothing | `rt/workflow_supervisor.hpp:492` (`workflow_status_tag`) |
| 6 | `error`'s fields (`klass` vs `kind`) | `.message` and `.code` only | `core/error.hpp:78-89` |
| 7 | `failure_class` enum + `failure_class_to_string` | Nothing | `core/error.hpp:18-42` |
| 8 | `WorkflowResult`'s field list | `.output`, `.status`, `.rounds` only | `rt/workflow_supervisor.hpp:595-619` |
| 9 | `WorkflowSupervisor::initialize`'s real arity/defaults | 2-arg code vs 3-arg prose | `rt/workflow_supervisor.hpp:739-741` |
| 10 | `drive(...)` — used 33× on the site, defined 0× | Nothing | `examples/09_concurrent_workflow.cpp:119-122`; then replaced with the documented `rt::block_on` (`builderContent.ts:328`) |
| 11 | `text_message(...)` — used 33×, defined 0× | Nothing | `examples/09_concurrent_workflow.cpp:52-60` |
| 12 | `check(...)` — used 37×, defined 0× | Nothing | `examples/09_concurrent_workflow.cpp:43-50` |
| 13 | `text_of(...)` | Used 19×; the snippet that uses it includes `core/content.hpp`, implying it lives there | `core/tool_call_extraction.hpp:83` — it does not live in `content.hpp` |
| 14 | `sharing_mode`'s namespace | Used unqualified inside engine-namespace snippets | compile error `C2039`; `core/sharing_mode.hpp:15-18` — it is `agentengine::sharing_mode`, NOT `agentengine::workflow::sharing_mode` |
| 15 | `counted(...)`, `appender(...)`, `handler(...)` — used, never defined | Nothing | `examples/09:96`, `examples/13`, `examples/10` respectively |
| 16 | Why routing into a `fan_in` join produced `completed` with an empty answer | **Nothing anywhere.** No page mentions it | `rt/workflow_supervisor.hpp:1031-1044` — the `HeldFanIn` comment |
| 17 | `std::string all_text_of(Message const&)` source of truth | Defined in docs at `apiContent.ts:3088` — but as a *snippet*, so no header to include | copied from the snippet; it is an example helper, not engine API |

---

## Undefined identifiers

For the doc snippets I relied on — `workflowMinimalSnippet` (`apiContent.ts:2908`),
`workflowGraphSnippet` (`:2917`), `workflowConcurrentSnippet` (`:2991`),
`workflowRouterSnippet` (`:3008`), `workflowReflectionSnippet` (`:3025`),
`workflowSequentialBodiesSnippet` (`:3066`), `workflowConcurrentMergeSnippet` (`:3085`),
`workflowRouterRunSnippet` (`:3116`), `workflowReflectionBoundSnippet` (`:3140`),
`workflowMapReduceRealSnippet` (`:3256`), the pizza-order escalation snippet
(`ApiPizzaOrderReference.tsx:194-232`), and `SNIPPET_CMAKE`/`SNIPPET_PROGRAM`
(`ApiFirstAppReference.tsx:11-17`, `:80-94`).

**Used in those snippets and defined somewhere in the docs:**

| Identifier | Defined at |
|---|---|
| `Workflow`, `Executor`, `Edge`, `TerminationBound` | `apiContent.ts:2917-2967` (the struct bodies) |
| `edge_kind`, `executor_kind` | `apiContent.ts:2921-2922` |
| `EdgeFailurePolicy` | named at `apiContent.ts:2928`; no body ever shown |
| `all_text_of` | `apiContent.ts:3088` |
| `worker` | `apiContent.ts:3099` |
| `source` | `apiContent.ts:3108` |
| `aggregate` | `apiContent.ts:3112` |
| `uppercase`, `reverse_text` | `apiContent.ts:3068`, `:3075` |
| `node_desc` | `ApiPizzaOrderReference.tsx:198-201` |
| `triage()` (factory form) | `apiContent.ts:3198-3206` |
| `billing()`, `tech()` | `apiContent.ts:3209`, `:3223` |
| `namespace ae = agentengine` | `content.ts:281`, `:386` |

**Used in those snippets and NEVER defined anywhere in the docs, `examples/`, or `include/` as a
public API** (i.e. I had to supply them):

| Identifier | Uses on site | Defs on site | Notes |
|---|---|---|---|
| `text_message(...)` | 33 | **0** | example-local helper; not engine API |
| `drive(...)` | 33 | **0** | the site's most-used helper has no definition anywhere |
| `check(...)` | 37 | **0** | test-local helper |
| `node(...)` | 4 | **0** | the *other* spelling of `node_desc(...)`, used by the concurrent, router, reflection, and group-chat snippets. Two spellings for one helper, only one defined |
| `counted(...)` | 5 | **0** | invocation-counting wrapper, `examples/09:96` |
| `appender(...)` | 3 | **0** | `examples/13` |
| `handler(...)` | 2 | **0** | `examples/10` |
| `kitchen`, `identity_sink` | 2 | **0** | `ExecutorBody` *values* in `ApiPizzaOrderReference.tsx:227` |
| `all_text_of` | 4 | 1 | defined, but only as a doc snippet — there is no header to include it from |

**Engine types used by those snippets with no definition shown:** `WorkflowSupervisor`, `RunWorkflow`,
`WorkflowResult`, `ExecutorBody` (prose definition only, `apiContent.ts:3080`), `ExecutorOutcome`
(shape inferable from `{message, routes}` call sites only), `Message`, `EffectContext`, `result<T>`,
`Text`, `ContentItem`, `workflow_status` (4 of 13 enumerators), `sharing_mode` (enumerators named in a
comment, enum body never shown), `validate_workflow` (called 8×, declaration never shown — no return
type, no signature), `WorkflowSupervisor::initialize` (called, never declared),
`agentengine::rt::task<T>`, `rt::block_on`.

---

## Blockers

Ranked by cost.

1. **The brief's page list omits `api/first-app.html`, which is the only page that answers (a) and
   (g).** The brief says `apiContent.ts`'s `apiPages` array "is the full page list" and enumerates the
   relevant pages; `first-app` is not among them, and no `workflow*.html` page shows an include or a
   CMake line. So I reverse-engineered the include set and the entire CMake wiring from the engine's
   own build files and `examples/`, which is where most of my time went — and then found the page
   already had it, four lines long. **Compounding it:** `apiContent.ts` was edited *during this
   session* (it grew from 4,687 to 4,728 lines; a `first-app` entry was inserted at line 122, shifting
   everything after it by +41). Every line number I had already collected was silently invalidated,
   and every citation in this report had to be re-verified. If you are auditing doc coverage, audit
   against a pinned revision.

2. **`fan_in` + `switch_case` silently yields `completed` with an empty answer.** My first router
   variant kept the base graph's `worker -> aggregate` edges as `fan_in` and only swapped the
   `ingest -> worker` edges to `switch_case`. It validated, ran, and returned
   `workflow_status::completed`, `rounds=2`, `output` empty, aggregator invoked **0** times. Cause:
   a `fan_in` target with 2+ declared sources is *held* until **every** declared source resolves
   (`rt/workflow_supervisor.hpp:1031-1044`), and `switch_case` fires exactly one — so the join can
   never be satisfied, the graph runs dry, and a dry run is `completed`. Nothing in the docs warns
   about this. This is the worst failure class for a consumer: a green status, a validating graph,
   and no answer. I kept it as the `mixed-fanin` scenario so the behaviour is pinned down rather than
   described.

3. **`sharing_mode` is `agentengine::sharing_mode`, not `agentengine::workflow::sharing_mode`.** The
   docs use it unqualified because their snippets sit inside the engine's namespace; a consumer who
   writes the obvious `using namespace agentengine::workflow;` gets `error C2039: 'sharing_mode':
   is not a member of 'agentengine::workflow'`. Cost one full build cycle. (`node_desc` itself is
   given with `sharing_mode::branch` in it at `ApiPizzaOrderReference.tsx:200`, so this is not
   avoidable by not using `node_desc`.)

4. **`drive()` is used 33 times across the docs and defined zero times.** Every workflow snippet's
   result-handling line is un-compilable as printed. I had to go to `examples/09` for a definition,
   and then discovered the engine ships a *correct* one — `rt::block_on` — which the Builder page
   documents (`builderContent.ts:328`) while the Workflow pages do not.

5. **Two names for one helper, one of them defined.** `node_desc(...)` (defined on the pizza page)
   and `node(...)` (used by the concurrent/router/reflection/group-chat snippets, defined nowhere).
   A reader following the Concurrent page cannot compile that page.

6. **`workflow_status` is the one thing a consumer must branch on and is 4/13 documented.** An
   incomplete enum is not a cosmetic gap: it is impossible to write an exhaustive `switch` (and get
   compiler checking), or to know whether a given termination is "expected" or "failed", without
   opening the header. `workflow_status_tag()`, the engine's own status string, is not mentioned at
   all.

7. **`error` and `failure_class` are shown but not defined.** The docs teach
   `built.error().message` / `.code` — enough to print, not enough to branch. `.klass` and
   `failure_class` are absent from the whole site.

8. **Minor: `initialize`'s arity is inconsistent between code and prose** (2 vs 3 args; really 4 with
   defaults), and `text_of` is not where the includes imply it is (`core/tool_call_extraction.hpp`,
   not `core/content.hpp`).

---

## Confidence

| # | Requirement | Confidence | Evidence |
|---|---|---|---|
| 1 | Fan-out to 3 workers, fan-in to 1 aggregator | **certain** | cloned verbatim from `apiContent.ts:2991-3006`; measured: aggregator invoked exactly **1** time, 3 ContentItems merged in graph-declared order |
| 2 | Real termination bound, validated before running | **certain** | `wf.bound.max_rounds`; `validate_workflow(wf)` runs before `initialize`; measured refusal `workflow.unbounded` when removed |
| 3 | Run it, get the result out, branch on status | **certain** | `WorkflowResult r = drive(sup.run_workflow(RunWorkflow{...}))`; `.output` printed; all four relevant statuses observed: `completed`, `bound_max_rounds`, `routing_failed`, `executor_failed` |
| 4 | Second variant of the SAME graph with a `switch_case` chosen by a classifier | **certain** | same 5 executors (asserted at runtime), same start, same aggregator, same bound; 3 documents → 3 different routes, exactly one branch's `invoke()` called |
| 5 | Failure path prints the engine's own error/status, exits non-zero | **certain** | validation: `workflow.unbounded` / `class=contract`; routing: `routing_failed`; body fault: `executor_failed`, `failed at: summarize`; all three exit **1** |
| 6 | Buildable and runnable by me | **certain** | configured + built + ran from a clean build dir; MSVC 19.51, VS 18 2026, x64, Release; 9 engine static libraries linked via `agentengine::core` |

Nothing was `could not determine`.

---

## Verdict

**Yes, a developer can build this product from the docs — but not from the pages this exercise
pointed me at, and not without getting burned once.** The graph DSL is genuinely well documented:
`workflowGraphSnippet` is the real header verbatim, the seven edge kinds are tabulated with honest
semantics, the fan-out/fan-in shape is a copy-pasteable snippet, and the "bodies are parallel to
`wf.executors` BY INDEX" rule is stated twice. That is more than most C++ engines give you, and it is
why requirements 1–3 went in almost without friction. What is *not* documented is everything that
makes the snippets into a program: `text_message`, `drive`, and `check` are used 103 times across the
site and defined zero times, `node` and `node_desc` are two spellings for one undefined helper, and
the one page that answers "how do I depend on this and what do I link" — `first-app.html` — is not
reachable from the workflow pages the docs themselves recommend and was not in the brief's page list,
which is why I spent most of this task reverse-engineering four lines of CMake that were already
written down. The single most serious finding is not a documentation gap but a semantic trap: routing
a `switch_case` into a `fan_in` join that has other declared sources produces a graph that validates,
reports `workflow_status::completed`, and returns **nothing**. No page mentions it, the validator
cannot catch it, and a developer following the Concurrent page's fan-in edges while adding the Router
page's routing would ship exactly that. The docs are honest about their own limits — they name gaps
rather than rounding them up — but the gaps here are at the seams between pages, and those are the
ones a developer hits first.
