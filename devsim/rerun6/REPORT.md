# A document-processing pipeline on AgentEngine — build report

**Exercise:** build a fan-out/fan-in workflow product on AgentEngine from the documentation site
alone, in `devsim/rerun6/`, without modifying any existing repo file.

**Artifacts:** `main.cpp`, `CMakeLists.txt`, plus `build.bat` / `run-all.bat` (Windows toolchain
helpers, not part of the product).

**Environment:** Windows 11 x86-64, MSVC 19.51 (VS 18 Community, toolset 14.51), CMake 3.32, Ninja
(the copy bundled with VS), C++23 (`-std:c++latest`), Release. AgentEngine built from source as a
CMake subproject of this project.

**Result:** built, linked, and run. Every run outcome and both validation failures below are real
captured output, not predictions.

> ### Doc-revision caveat — the documentation moved while this was being written
>
> The marketing/doc tree was **being edited by another process throughout this session**.
> `web/marketing/src/data/apiContent.ts` was rewritten at least three times while I worked
> (mtimes `10:43:53`, `10:45:39`, `10:46:13`; it grew from ~4,764 to 4,819 lines), and
> `ApiWorkflowReference.tsx` (`10:44:17`) and `ApiFirstAppReference.tsx` (`10:45:12`) moved too.
> I re-derived every citation twice and the offsets kept shifting.
>
> What this means for the citations below:
> - **Every line number is from one atomic snapshot taken at `2026-10-10 10:46`**, re-derived in a
>   single pass so the numbers are mutually consistent, then content-verified line by line
>   (22/22 anchors matched, 109/109 citations in range).
> - **Line numbers in this tree are not stable. The named symbol or anchor text printed next to each
>   citation is the authoritative half** — on a later revision, search for the anchor
>   (`workflowConcurrentSnippet`, `fanInTrapKind`, `const SNIPPET_CMAKE`, …) rather than trusting the
>   digit. Every citation in this report carries one.
> - **One of my findings was invalidated by an edit that landed mid-session** — a `WorkflowResult`
>   reference section appeared that closes the gap I had recorded as my lowest-confidence area. That
>   is corrected below and called out explicitly in *Findings that changed under me*.

---

## FALLBACK LOG — **TOTAL: 4**

Four questions the documentation did not answer, and where the answer came from. **No engine header
or source file was read to resolve any of them** — every one was resolved from a citation string
already present in the docs, or from the compiler's own diagnostic. There are no source-reading
fallbacks.

| # | Question | Doc's answer | How resolved |
|---|---|---|---|
| **F1** | `#include` spelling for the two workflow headers | Named in prose only: the eyebrow `graph.hpp — edge_kind` (`ApiWorkflowReference.tsx:56`), a citation `include/agentengine/workflow/graph.hpp:431` (`apiContent.ts:3471`) and `:547-559`/`:581` (`ApiWorkflowReference.tsx:1197`), a citation `include/agentengine/workflow/graph.hpp:590` (`ApiBuilderReference.tsx:151`), a prose mention of `rt/workflow_supervisor.hpp`/`HeldFanIn` (`ApiWorkflowReference.tsx:85`), the CodePanel title `rt/workflow_supervisor.hpp` (`ApiWorkflowReference.tsx:1186`), and `rt/workflow_supervisor.hpp:926-933` (`eventsContent.ts:482`). **No page in the doc set shows an `#include` line for either header** — re-verified after the mid-session edits; the only `#include "agentengine/rt/…"` anywhere in the docs is `rt/agent_session.hpp` on the pizza-order page (`ApiPizzaOrderReference.tsx:60`). | Derived both from the citation strings. `#include "agentengine/workflow/graph.hpp"` and `#include "agentengine/rt/workflow_supervisor.hpp"` compiled first try. **Cost: low** — the path is recoverable, but only by treating a prose citation as a file path. |
| **F2** | `workflow_status_tag()`'s return type | "`agentengine::rt::workflow_status_tag(status)` is the engine's own plain-string form" (`ApiWorkflowReference.tsx:365`, `statusNote`). "Plain-string" is not a type. | Wrote `.c_str()` on it → `error C2228: left of '.c_str' must have class/struct/union`, `note: type is 'const char *'`. It returns `const char*`. **Cost: one compile cycle.** |
| **F3** | `WorkflowSupervisor::initialize()`'s return type, and how its own refusal is reported | The docs describe `initialize()` as calling "the *contexts-aware* sibling" of `check_workflow_executable(wf)`, which refuses an `agent`/`sub_workflow` node, and warn "A refused graph is recoverable; a quietly reinterpreted one is not" (`ApiWorkflowReference.tsx:383–401`; `initialize()…instead calls` at `:392`, the refusal itself cited at `graph.hpp:547-559`). Every snippet in the doc set discards the call (`apiContent.ts:3069,3221,3244,3278…`, `ApiPizzaOrderReference.tsx:284`, `durabilityContent.ts:829`, `eventsContent.ts:495`, `hitlContent.ts:262`). | Probed by binding the result → `error C3313: 'init': variable cannot have the type 'const void'`. **It returns `void`.** Partially mitigated by the docs naming `check_workflow_executable(wf)` as a callable pre-check — but the *contexts-aware* sibling's name and signature are given only as a source line citation, so at the call site the prose "recoverable" still has no documented expression. **Cost: one compile cycle.** |
| **F4** | Which namespace `ExecutorOutcome` lives in | The page's own namespace warning says explicitly *"Do not assume one namespace holds all of these"* and then enumerates `edge_kind`, `executor_kind`, `Executor`, `Edge`, `TerminationBound` (→ `agentengine::workflow::`) and `sharing_mode` (→ `agentengine::sharing_mode`) (`ApiWorkflowReference.tsx:98`). `ExecutorOutcome` is used bare in every snippet (`apiContent.ts:3103,3127,3129…`) but is never placed in a namespace. | Wrote it bare under `using namespace agentengine;` → `error C2065: 'ExecutorOutcome': undeclared identifier`, and MSVC's own expansion of `ExecutorBody` supplied it: `std::function<std::expected<agentengine::rt::ExecutorOutcome, …>>`. It is `agentengine::rt::`. **Cost: one compile cycle.** |

**Total documentation failures: 4.** All four are single-line API-detail gaps. None required reading
engine source; none blocked the build.

### Not counted as fallbacks (checked, and they are genuinely documented)

Listed so the count is auditable:

- **`text_message`, `all_text_of`, `node_desc`, `counted`, `drive`, `check` are not engine API.**
  The shared note says so, names each one, and says where each is defined
  (`ApiSnippetConventions.tsx:16–46`). `drive(task)` → `agentengine::rt::block_on(task)`
  (`rt/block_on.hpp`) is given explicitly at `ApiSnippetConventions.tsx:16`.
- **`ExecutorBody`'s exact type** is given verbatim: "exactly
  `std::function<result<ExecutorOutcome>(Message const&, EffectContext&)>`", plus the fact that a
  `Message` converts implicitly to an `ExecutorOutcome` (`apiContent.ts:3171`).
- **How bodies match nodes** is given twice, unambiguously: "`bodies` is parallel to
  `wf.executors` BY INDEX" (`ApiPizzaOrderReference.tsx:279`), and "bodies parallel to
  `wf.executors` by index: src, upper, lower, title, agg" (`apiContent.ts:3363`).
- **Constructing an `error` to return a typed failure from a body.** Never shown. Not a fallback:
  the docs document the *other* failure channel completely — "a throwing executor can't take the
  process with it: `rt::ThreadPool::submit()` returns a `JobOutcome{faulted, fault_ptr}` rather than
  letting the exception propagate raw" (`ApiWorkflowReference.tsx:327`). Throwing is documented, and
  I used it.
- **How to get more than the status out of a failed run.** This *was* my biggest documented gap
  early in the session; a doc edit landed mid-session that closes it (see below). Not a fallback.
- **The build recipe is complete and correct.** `AGENTENGINE_ROOT` defaulting to
  `third_party/AgentEngine` is the one deviation, and it is this environment (the engine is the repo
  root), not a doc gap.

### Findings that changed under me

**A `WorkflowResult` reference section appeared during the session** —
`ApiWorkflowReference.tsx:371–380` (`resultHeading` / `resultBody`, "What the run hands back — the
status is not the whole answer") plus the struct printed at `apiContent.ts:2819–2854`
(`workflowResultSnippet`), rendered in a `CodePanel` titled `rt/workflow_supervisor.hpp`.

At the start of the session `WorkflowResult`'s field set was documented **nowhere**; my working notes
recorded the absence of a run-level error channel as the lowest-confidence finding of the exercise.
The new section documents `status`, `rounds`, `output`, `partial`, `transcript`, `failed_executor`,
`open_interactions`, `unopened_ports`, `usage`, and explicitly says *"A handler written against the
status tag alone will get a fault wrong in a specific way: it cannot say **which** node failed, and it
has no reason to look at what the other nodes produced."*

**I re-verified these fields against the compiled engine and used them.** `rounds`,
`failed_executor` and `partial` all exist with exactly the documented names and types, and the fault
output below is a real capture. Requirement 5's confidence is raised accordingly.
**This is a documentation improvement, not a change to the fallback count** — F1 was re-verified as
still open (no `#include` for the workflow headers), and F2/F3/F4 are unaffected.

### Disclosures

- I read the repo-root `CMakeLists.txt` for two non-API purposes: locating `cl.exe`/`ninja.exe`
  (via `build-dev/CMakeCache.txt`, an environment concern), and confirming the `agentengine::core`
  alias and the `AGENTENGINE_BUILD_TESTS`/`AGENTENGINE_BUILD_EXAMPLES` option names exist. The docs
  already state all three (`ApiFirstAppReference.tsx:86–87, 107, 113`), so this added no fallback.
  **No engine header under `include/agentengine/` and no file under `src/` was opened.**
- I did not read any other agent's report or any `devsim/rerun*/` artifact.

---

## GETTING STARTED — each step, and where it came from

### (a) Include the engine — **found in docs** (with F1)

- **Depend on it:** "There is no `find_package(AgentEngine)` and no versioned release to install:
  the engine ships as a repository you vendor… You add it as a subdirectory and link one target,
  `agentengine::core`." — `ApiFirstAppReference.tsx:233` (`s1Body`).
- **Turn its developer build off first.** Both `set(... CACHE BOOL "" FORCE)` lines, and why a plain
  `set()` is shadowed by the engine's own `option()` — `ApiFirstAppReference.tsx:86–87`. The docs
  quantify it: 2,336 build statements without, 159 with; configure 49 s vs 4 s
  (`ApiFirstAppReference.tsx:241–253`, "2,336" at `:248`). **My measurement corroborates this**: the
  target needed exactly the docs' own "30 build steps in total for the program above"
  (`ApiFirstAppReference.tsx:287–292`, "build steps" at `:292`).
- **Keep the build directory shallow** — Windows' 250-character object-path cap
  (`ApiFirstAppReference.tsx:255`). I kept the build directory at `devsim/rerun6/build` and saw no
  `CMAKE_OBJECT_PATH_MAX` warning.
- **Per-header includes for core types** — `ApiFirstAppReference.tsx:11–17` (`SNIPPET_PROGRAM`).
- **Per-header includes for the workflow headers — F1.**

### (b) Declare the graph as data — **found in docs**

The docs' central claim is that a `Workflow` is plain data: "no execution, no actors, no
scheduling" (`apiContent.ts:2999`). The complete struct is printed verbatim at
`apiContent.ts:2999–3049` (`workflowGraphSnippet`: `Executor`, `Edge`, `TerminationBound`,
`Workflow`; `struct TerminationBound` at `:3034`), and the minimal authoring case at
`apiContent.ts:2990–2997` (`workflowMinimalSnippet`). Everything I wrote is a direct transcription
of those two blocks:

```cpp
Workflow wf;
wf.id        = "doc-pipeline";
wf.executors = {node_desc("src"), node_desc("upper"), node_desc("lower"), node_desc("title"),
                node_desc("agg")};
for (char const* w : {"upper", "lower", "title"}) {
    wf.edges.push_back(Edge{"src", w, edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
}
wf.start = "src";
wf.output_selection.push_back("agg");
wf.bound.max_rounds = 8;
```

Useful specifics the docs supply and I relied on:

- **Defaults carry the weight**: `Executor::kind` → `function`, `worktree_mode` → `branch`,
  `Edge::kind` → `direct`, `on_failure` → `fail` (`ApiWorkflowReference.tsx:119–134`,
  `graphShapeRecoBody`).
- **The namespace trap is called out.** `sharing_mode` is `agentengine::sharing_mode`
  (`core/sharing_mode.hpp`), *not* `agentengine::workflow::`, so a bare
  `using namespace agentengine::workflow;` "fails to compile with an unhelpful 'is not a member'"
  (`ApiWorkflowReference.tsx:98–109`). I pulled in both namespaces and hit nothing.
- **The five-member `Executor` aggregate** (designated initializers with `.input_type`,
  `.output_type`, `.worktree_mode`, `.capability_ceiling`) is printed verbatim as `node_desc()` at
  `ApiPizzaOrderReference.tsx:253–256`.
- **Seven edge kinds**, each defined, in a table: `apiContent.ts:2856–2865` (`workflowEdgeKinds`;
  `fan_in` at `:2863`, `switch_default` at `:2866`), and `ApiWorkflowReference.tsx:56–65`.

### (c) Supply executor bodies and know how they match nodes — **found in docs** (with F4)

- **The type:** `rt::ExecutorBody` *is* `std::function<result<ExecutorOutcome>(Message const&,
  EffectContext&)>`, and "a `Message` converts implicitly into an `ExecutorOutcome` for the common
  'just pass a message downstream' case" (`apiContent.ts:3171`). My three worker bodies return
  `result<Message>` and needed no adaptation.
- **The matching rule:** positional, by index into `wf.executors`
  (`ApiPizzaOrderReference.tsx:279`, `apiContent.ts:3363`). A `request_port` node's slot is
  deliberately left empty (`{}`) — `ApiPizzaOrderReference.tsx:282`.
- **A routing body** returns `ExecutorOutcome{message, routes}`, where `routes` names the one case
  label it selects (`apiContent.ts:3090–3104`, `workflowRouterSnippet`; `return ExecutorOutcome{` at
  `:3105`). This is the only body shape that needs the explicit `ExecutorOutcome` — hence F4.
- **Four helpers are local, not engine API**, and I reimplemented them: `text_message`,
  `all_text_of`, `node_desc`, `counted` (`ApiSnippetConventions.tsx:32–33`).

### (d) Get the result out of a run — **found in docs**

```cpp
std::vector<ExecutorBody> bodies = { /* parallel to wf.executors */ };
WorkflowSupervisor sup;
sup.initialize(wf, bodies);
WorkflowResult const r = rt::block_on(sup.run_workflow(RunWorkflow{text_message("…")}));
```

- `sup.initialize(wf, bodies)` / `sup.run_workflow(RunWorkflow{…})` → `WorkflowResult`
  (`apiContent.ts:3057–3070` and `:3075–3090`; `ApiPizzaOrderReference.tsx:283–287`).
- `run_workflow` returns a `task<WorkflowResult>`, and the docs' `drive(...)` is
  `rt::block_on(...)` — `ApiSnippetConventions.tsx:16`, signature confirmed at
  `eventsContent.ts:498`.
- **Reading the answer:** `all_text_of(r.output)` (`apiContent.ts:3225`), where `r.output` is a
  `Message`. A fan-in delivery arrives as "one `ContentItem` per contributing branch, in
  graph-declared order — never once per inbound edge" (`apiContent.ts:2861`), which is what my
  aggregator concatenates.
- **The rest of the result** — `ApiWorkflowReference.tsx:371–380` and the struct at
  `apiContent.ts:2819–2854`: `status`, `rounds`, `output`, `partial` ("the LATEST message each
  executor produced… preserved"), `transcript`, `failed_executor`, `open_interactions`,
  `unopened_ports`, `usage`. I use `status`, `output`, `rounds`, `failed_executor` and `partial`.

### (e) Read and act on the run's status — **found in docs** (with F2)

- **Thirteen values, three kinds** (`ApiWorkflowReference.tsx:338` `statusBody`, and the table at
  `:350–364`). The docs are emphatic that the kind matters more than the value: a *bound* is expected
  termination, a *fault* is real failure, and a *refusal* means nothing changed and the same call can
  be made again (`ApiWorkflowReference.tsx:337–347`). Every one of the thirteen is named in the
  table, so an exhaustive `switch` is writable from the docs alone.
- **Printing it:** `agentengine::rt::workflow_status_tag(status)`
  (`ApiWorkflowReference.tsx:365`, `statusNote`) — return type via F2. Confirmed output strings:
  `completed`, `bound_max_rounds`, `executor_failed`, `routing_failed`.
- **The caveat that matters most**, stated three separate times: `completed` "guarantees" only that
  no node is left to dispatch — "An empty answer is still completed… If your handler treats completed
  as 'there is a result', it has exactly the bug that caveat warns about"
  (`ApiWorkflowReference.tsx:351`; the diagram caveat at `:79–95`; and the new struct comment
  "empty on a starved join that still reports completed" at `apiContent.ts:2824–2825`).

My handler therefore does not test `status == completed` alone:

```cpp
if (text.empty()) { /* FAIL: completed but empty output, agg invoked N times */ return 4; }
```

### (f) Handle errors — **found in docs** (with F3)

- **`result<T>` is `std::expected<T, agentengine::error>`**; check `has_value()`; on failure
  `error()` gives `code` (the stable string to branch on), `message` (host-facing prose), and `klass`
  (a `failure_class`: `transient`/`policy`/`contract`/`resource`/`fatal`/`canceled`; only `transient`
  and `resource` are retryable) — `ApiFirstAppReference.tsx:368–390` (`s5Body` at `:368`,
  `workflow.missing_case_label` row at `:390`).
- **Validate before running:** `check(validate_workflow(wf).has_value(), "the graph validates before
  anything runs")` — `ApiPizzaOrderReference.tsx:277`. `validate_workflow` "refuses a missing id,
  start or bound, and around two dozen other things besides, each with its own stable code"
  (`ApiWorkflowReference.tsx:119–134`), and the docs say to branch on the code, not the message.
- **Named codes I used:** `workflow.missing_case_label` and `workflow.unexpected_case_label`
  (`ApiPizzaOrderReference.tsx:264–267`), `workflow.unbounded` (`ApiBuilderReference.tsx:186`),
  `workflow.unreachable_executor` (`ApiWorkflowReference.tsx:128`) and
  `workflow.switch_default_without_cases` (`:130`).
- **Per-edge failure policy**: `fail | propagate | retry | fallback`, declared on each `Edge`, with a
  faulting executor contained by `rt::ThreadPool::submit()`'s `JobOutcome`, and a failed workflow's
  **partial results preserved** (`ApiWorkflowReference.tsx:308–334`, `failureBody` at `:310`,
  `:327`, `:332`).
- **How `initialize()` reports its own refusal — F3.** Verified: it returns `void`. A graph that the
  contexts-aware check would refuse therefore has no return channel at `initialize()`; the docs name
  `check_workflow_executable(wf)` as a separate callable pre-check (`:388`) and cite
  `graph.hpp:547-559` / `:581` for the contexts-aware sibling, but never give its call-site signature.
- **What a fault handler actually needs** — `ApiWorkflowReference.tsx:371–380`: `failed_executor`
  names the node, `partial` keeps what the other nodes produced "preserved, not discarded, which is
  what makes a failed run diagnosable rather than just red". I print both.

### (g) Build and link — **found in docs**

- `ApiFirstAppReference.tsx:80–122` gives the entire `CMakeLists.txt` (`const SNIPPET_CMAKE` at `:80`); `:287–293` gives the three
  commands. I ran exactly those two commands (plus putting MSVC on `PATH`, a Windows environment
  concern and not an AgentEngine concern).
- **Do not link `agentengine_warnings`** — that target is `/W4 /WX` and is "meant for the engine's
  own targets. Copying it onto your application makes every warning a build failure"
  (`ApiFirstAppReference.tsx:114–116`).
- **`agentengine::core` brings the workflow engine with it** — confirmed:
  `target_link_libraries(agentengine_core INTERFACE agentengine_rt_workflow)`.
- **Air-gapped configure is documented too** — an *Offline / air-gapped configure* block appeared
  mid-session (`ApiFirstAppReference.tsx:89–101`) naming a local-path override per vendored
  dependency (`AGENTENGINE_VENDOR_ASIO`/`AGENTENGINE_ASIO_ROOT`, `…_MBEDTLS_ROOT`,
  `…_CA_BUNDLE_PATH`, and the option-gated `WASMTIME`/`PYTHON`/`PDFIUM`), with the useful warning
  that the CA bundle is a `file(DOWNLOAD)` and therefore fails specifically behind a
  git-only proxy. I did not need it — this machine fetches Asio successfully at configure time and
  configure still took 5.1 s — but it is the right answer for a locked-down build host.

**Measured, this machine:**

| Step | Measured | Docs claim |
|---|---|---|
| Configure (clean build dir) | **5.1 s** | "configure takes 49 s instead of 4 s" — `ApiFirstAppReference.tsx:248` |
| Full build from clean | **25.3 s**, exactly **30** build steps | "30 build steps in total for the program above" — `ApiFirstAppReference.tsx:292` |
| Incremental rebuild after editing `main.cpp` | **8.6 s** | "changing `main.cpp` rebuilds only `main.cpp`" — `ApiFirstAppReference.tsx:293` |

---

## HARD BLOCKERS

**There were none.** Nothing stopped the build outright. The project configured, compiled, linked and
ran on the first attempt at each step; the only three compile errors in the entire exercise were the
one-line API-detail gaps F2, F3 and F4, each fixed within one build cycle — plus one error that was
my own omission (a missing `using agentengine::rt::ExecutorOutcome;`), not a doc failure. No missing
header, no missing symbol, no link error, no unrecoverable "the docs don't say how to do this at all".

The build was additionally reproduced from scratch in a clean directory from the delivered
`CMakeLists.txt` and `main.cpp` alone: `30/30` targets, no errors.

---

## COMBINING PATTERNS — the `switch_case` variant

### Did the docs warn about routing into a fan-in join? **Yes — loudly, and they were right.**

`ApiWorkflowReference.tsx:79–95` is a dedicated callout (rendered as a `trap` diagnostic note):

> A `fan_in` target with two or more declared sources is held until **every one of those sources has
> resolved** — not until the first contribution arrives. … It becomes a trap when you *also* route:
> if routing means only some of an aggregator's declared sources ever fire, the join can starve. The
> failure is silent — the graph `validate_workflow` accepts can finish with
> `workflow_status::completed` and an **empty** output, having invoked the aggregator zero times. **Do
> not route into a fan-in join**: keep an aggregator's declared sources to ones that always fire, and
> route *before* the fan-in rather than inside it. When you get an empty answer with a successful
> status, count the aggregator's invocations before you trust the status. (`:91` is the "do not
> route" sentence; `:93` the "count the invocations" one.)

It is restated in the status table's own `completed` row (`ApiWorkflowReference.tsx:351`) **and** in
the new `WorkflowResult` struct comment (`apiContent.ts:2824–2825`, "empty on a starved join that
still reports completed") — so a reader who skips the callout meets it twice more. The underlying
rule is also in the edge-kind table: a `fan_in` target "runs exactly ONCE per round, receiving one
ContentItem per contributing branch" (`apiContent.ts:2861`).

### Did I hit anything surprising that still reported success? **Yes — and I reproduced it exactly as documented.**

`--variant=trap` takes the working graph and swaps the source's three `fan_out` edges for three
`switch_case` edges driven by a classifier that returns exactly one valid label. That is the same
graph, with routing into the join. Real output:

```
== docpipeline variant=trap ==
validated   : ok (bound max_rounds=8)
status      : completed          <-- the engine's own tag
status kind : success
agg invoked : 0
rounds      : 2
partial     : 2 node(s) produced a message preserved after the run
output      : <empty>
```

**A silent, successful-looking, result-free run.** The bound did not trip, the validator did not
complain, and `workflow_status_tag` printed `completed`. The only signals are the empty output and the
invocation count — which is precisely why the docs tell you to count. This is the single most valuable
thing on the whole doc set, and it is written down before you can hit it.

Note the shape of the surprise: the run did **not** report `routing_failed`. The classifier returned a
*valid, declared* label; it is the *join*, not the route, that failed. A handler that assumes
"no error status ⇒ a result exists" ships this bug.

### Can a correct combination be written from the docs alone? **Yes.**

`ApiWorkflowReference.tsx:91–92` prescribes it: keep the aggregator's declared sources to ones that
always fire, and route **before** the fan-in. `--variant=routed` does exactly that — a classifier
`switch_case`-routes to one of two sources, and *whichever* fires fans out to all three workers, so
the aggregator's three declared sources always resolve:

```
== docpipeline variant=routed --route=detailed ==
status      : completed     agg invoked : 1     rounds : 4
output      : upper:DETAILED:QUARTERLY REPORT -> DETAILED + lower:… + title:…

== docpipeline variant=routed --route=brief ==
status      : completed     agg invoked : 1     rounds : 4
output      : upper:BRIEF:QUARTERLY REPORT -> BRIEF + lower:… + title:…
```

Two different classifier decisions through the same graph, both merging correctly. This is the
documented-correct answer to requirement 4, and it needed no fallback beyond F1/F2.

### Could I write a correct handler for **every** run outcome from the docs alone? **Yes.**

The thirteen-value table (`ApiWorkflowReference.tsx:349–364`) is complete enough to write an
exhaustive `switch`, and the three-kind taxonomy (`:337–347`) tells you what each *means* for control
flow: a bound means "expected, stop", a fault means "failed, report", a refusal means "nothing
changed, the same call may be attempted again". With the `WorkflowResult` section
(`:371–380`) the fault branch is also *informative* rather than just red. I wrote that handler and
exercised every branch this pipeline can reach:

| Variant | Engine's own tag | Kind | rounds | failed node | agg invocations | Exit | Verified |
|---|---|---|---|---|---|---|---|
| `concurrent` | `completed` | success | 3 | — | 1 | 0 | ✅ |
| `routed` (detailed / brief) | `completed` | success | 4 | — | 1 | 0 | ✅ |
| `switch-default` | `completed` | success | 4 | — | 1 | 0 | ✅ |
| `trap` | `completed` (empty!) | success | 2 | — | **0** | 4 | ✅ silent-starvation guard fires |
| `bounded` | `bound_max_rounds` | bound | 6 | — | 0 | 2 | ✅ |
| `throw` | `executor_failed` | fault | 1 | **`src`** | 0 | 3 | ✅ |
| `bad-route` | `routing_failed` | fault | 1 | **`classifier`** | 0 | 3 | ✅ |
| `missing-label` | *(no run)* `workflow.missing_case_label` | refusal at validation | — | — | — | 1 | ✅ |
| `unbounded` | *(no run)* `workflow.unbounded` | refusal at validation | — | — | — | 1 | ✅ |

`suspended` is reachable only with a `request_port` node, which this product does not have; its
meaning, its `open_interactions`/`unopened_ports` fields and its resume call
(`resume_workflow(ResumeWorkflow{interaction_id, response, routes})`) are documented at
`ApiWorkflowReference.tsx:201–219` and `durabilityContent.ts:833–849`. `cancelled`,
`bound_deadline`, `bound_max_stalls`, `bound_max_resets`, `merge_conflict`, `invalid`,
`admission_denied` and `invalid_routes` are all named and explained in the table; I did not construct
a reproducer for each, and I am not claiming I did.

### Two things I noticed while combining

1. **`failed_executor` is populated for `routing_failed` too.** The new docs scope it: "`WHICH` node
   failed (`executor_failed`, `merge_conflict`)" (`apiContent.ts:2831`), and the example switch only
   reads it in those two cases (`:2845–2848`). Observed: `routing_failed` also sets it
   (`failed node : classifier`). **The docs understate a useful field rather than overstate one** —
   harmless, and worth knowing if you want a single "who failed" path.
2. **A zero-match route with a `switch_default` edge is absorbed, not failed.** The docs assert two
   things that read as contradictory: `switch_default` "fires iff **ZERO** of the same source's
   `switch_case` edges matched the reply's routes" (`apiContent.ts:2864`), while "A route naming
   **zero** or **more than one** declared label fails the run with `routing_failed`"
   (`ApiWorkflowPatternDetail.tsx:305`). `--variant=switch-default` sends a reply matching no declared
   label to a classifier holding one `switch_case` edge plus a `switch_default` edge. **Observed: the
   default fired, the run reported `completed`, and the output was produced** (`agg invoked: 1`,
   output prefixed `BRIEF:` from the default's target). The two statements are consistent once you
   read the default as the absorber of the zero-match case, and `routing_failed` as covering the
   zero-match case *when no default exists*. **Not counted as a fallback**, because
   `apiContent.ts:2864` states the exact rule that produced the behaviour; it is a wording hazard, not
   a missing fact — and it is testable from the docs.

---

## UNDEFINED IDENTIFIERS, and where each comes from

Every identifier that appears in a docs snippet and is **not** engine API, with its source. The shared
note on every API page owns most of this (`ApiSnippetConventions.tsx:13–48`).

| Identifier | Status | Source / what I did |
|---|---|---|
| `drive(task)` | example-local | `ApiSnippetConventions.tsx:16` maps it to the real `agentengine::rt::block_on(task)` (`rt/block_on.hpp`). Used the real one. |
| `check(cond, label)` | example-local | `ApiSnippetConventions.tsx:20` — the examples' own failure counter. Replaced with `std::printf` + exit codes. |
| `user_message(text)` | example-local | `ApiSnippetConventions.tsx:24` — shorthand for the `Message` in a `StartRun{}`. Not used (a workflow takes a `Message` directly). |
| `text_message(text)` | example-local | `ApiSnippetConventions.tsx:32`. Wrote my own. |
| `all_text_of(message)` | example-local | `ApiSnippetConventions.tsx:33`, body at `apiContent.ts:3179–3188`. Wrote my own. |
| `node_desc(id, kind)` | example-local | `ApiSnippetConventions.tsx:33` cites `examples/23_handoff_mesh.cpp:103`; body printed at `ApiPizzaOrderReference.tsx:253–256`. Wrote my own. |
| `counted(body, counter)` | example-local | `ApiSnippetConventions.tsx:33` ("wraps a body with an invocation counter"). Wrote my own. |
| `appender`, `handler` | example-local | `ApiSnippetConventions.tsx:33` — not needed. |
| `text_of(message)` | **real engine API** | `ApiSnippetConventions.tsx:28` → `core/tool_call_extraction.hpp`. Used directly. |
| `make_stream<T>(…)` | **real engine API** | `ApiSnippetConventions.tsx:28` (same row) → `core/stream.hpp`. Not needed. |
| `Session`, `OrderAgent`-style aliases | page-local `using` | `ApiSnippetConventions.tsx:36–37` — a named `AgentSession<…>` instantiation. Not needed. |
| `caps`, `store`, `held` | caller-supplied values | `ApiSnippetConventions.tsx:40–41`. Not needed (no model, no secrets in this product). |
| `set_capabilities(&held)` | real API, dangerous | `ApiSnippetConventions.tsx:44–45` — takes a pointer with a no-op deleter, so the `CapabilitySet` must outlive every run. Not used; noted as the one crash-rather-than-fail call. |
| `agg_invocations` | mine | The invocation counter the docs' trap warning tells you to keep (`ApiWorkflowReference.tsx:93`, `apiContent.ts:3363`). |
| `ExecutorOutcome` namespace | **engine API, namespace undocumented** | **F4.** `agentengine::rt::ExecutorOutcome`, recovered from the compiler's expansion of `ExecutorBody`. |
| `ExecutorOutput` | engine API, shape **not documented** | Used only as `r.partial.size()` / `r.transcript` — the new `WorkflowResult` section names the type but does not define it. Not needed for this product; noted as a residual. |
| `Usage` | engine API, shape **not documented** | `r.usage` — constructor `Usage{1, 1, 0, 0, 0.0}` appears in the on-ramp program (`ApiFirstAppReference.tsx:38`), but no page defines the fields. Not used. |

---

## BLOCKERS, RANKED

There were no build blockers. Ranked by how much they cost a developer who trusts the docs:

1. **F3 — `initialize()` returns `void`, so the refusal the prose promises has no expression at the
   call site.** The highest-value gap, because it is the one place the docs set an *expectation* the
   API does not meet. The docs say `initialize()` "instead calls the *contexts-aware* sibling, which
   accepts an `agent` node once `contexts[i]`'s granted capabilities satisfy that node's declared
   `capability_ceiling`", and warn "A refused graph is recoverable"
   (`ApiWorkflowReference.tsx:392–398`). There is nothing to check. A developer following that prose
   writes `if (!sup.initialize(wf, bodies)) …` and gets `C3313: variable cannot have the type
   'const void'`. Partially mitigated after the mid-session edit: the same paragraph now names
   `check_workflow_executable(wf)` (`:388`) and cites `graph.hpp:547-559 (the refusal) · :581 (the
   contexts-aware sibling)` (`:1197`), so the *one-argument* pre-check is reachable — but the
   contexts-aware sibling's name and signature are still only a source line, and the docs never say
   `initialize()` returns `void`.
2. **F4 — `ExecutorOutcome`'s namespace is not given, on the one page that warns about namespaces.**
   `ApiWorkflowReference.tsx:98–109` is a whole paragraph telling you not to assume one namespace
   holds everything — and it enumerates every type in the graph struct *except* `ExecutorOutcome`,
   which is the one type a routing body must name explicitly. The warning primes you to look for the
   answer and then omits it. Cost is one compile cycle; the risk is that MSVC's message is
   `'ExecutorOutcome': undeclared identifier` with no hint, and the namespace is only visible by
   reading the expansion of an unrelated type.
3. **F1 — the two workflow headers have no `#include` line anywhere in the doc set.** Recoverable
   from the citation strings, so cheap in practice, but "the on-ramp page shows you a complete
   translation unit, and the workflow pages show you eight graphs, and none of them shows you the line
   that makes either compile" is a real seam between the on-ramp and the reference.
4. **F2 — `workflow_status_tag()`'s return type.** Trivial; one compile cycle. Listed for
   completeness.

---

## CONFIDENCE PER REQUIREMENT

| # | Requirement | Confidence | Basis |
|---|---|---|---|
| 1 | Fan-out to three workers, fan-in to one aggregator | **High** | Built and run; aggregator invoked exactly once for three fan-in edges (`agg invoked : 1`), matching the docs' own proven claim (`ApiWorkflowPatternDetail.tsx:224`). |
| 2 | Real termination bound, validated before running | **High** | `bound.max_rounds` set; `validate_workflow` refuses an unbounded graph with `workflow.unbounded` (observed); the bound genuinely terminates a non-converging cycle (`bound_max_rounds`, `rounds: 6` of a bound of 6, observed). The docs list `TerminationBound`'s five fields (`apiContent.ts:3032–3040`) but never give `deadline_ms`/`token_budget` semantics beyond the field names — not needed here. |
| 3 | Run it, print the output, branch on status | **High** | `r.status` + `all_text_of(r.output)` + `workflow_status_tag`; all three status kinds (success/bound/fault) exercised for real. |
| 4 | Second variant with an edge `switch_case`-chosen by a classifier | **High** | Two correct shapes built and run (routed, switch-default); the trap shape reproduced. |
| 5 | On failure, print the engine's own error/status and exit non-zero | **High** *(raised — see below)* | Validation: `validated.error().code`/`.message` are the engine's own strings (`workflow.missing_case_label`, `workflow.unbounded`), exit 1. Run faults: `workflow_status_tag(r.status)` plus the newly documented `r.failed_executor` (`src`, `classifier`) and `r.rounds`, exit 3. **Residual:** there is still no documented way to get an `agentengine::error` (`code`/`message`/`klass`) out of a `WorkflowResult`, so the *reason* behind `executor_failed` is not readable from the docs — only *which node* and *how many rounds*. For this product that is enough; a product that branches on `failure_class` to decide retry might still need `rt/workflow_supervisor.hpp`. **Second residual:** `ExecutorOutput` (`r.partial`, `r.transcript`) is named but never defined, so a handler can count partial results but not read them. |
| 6 | Buildable and runnable | **High** | Clean configure + build + run, reproduced in a fresh directory: 30/30 targets, 25.3 s from cold, exactly as the docs describe. |

**Change from the version of this report written before the mid-session doc edit:** requirement 5 was
**Medium** and the "lowest-confidence area" was the absence of any run-level result documentation. The
`WorkflowResult` section that landed (`ApiWorkflowReference.tsx:371–380`, `apiContent.ts:2819–2854`)
closes that, the fields were verified to exist against the compiled engine, and requirement 5 is now
**High** with the two residuals above. The exercise's overall lowest-confidence area is now **F3**
(how a refused graph is reported at the call site), which is a documentation-versus-API mismatch
rather than a missing fact.

---

## VERDICT

**Can a competent C++ developer build this product from the documentation alone?**

**Yes.** Not "yes with caveats that undermine it" — yes, cleanly. The doc set is unusually sufficient
for this task:

- The graph is printed as a struct, verbatim, with every default called out.
- `ExecutorBody`'s exact type is given, and the bodies-to-nodes matching rule is stated twice.
- The full result-out path (`validate_workflow` → `initialize` → `run_workflow` → `block_on` →
  `WorkflowResult::status` / `::output` / `::failed_executor` / `::partial`) is shown as compiling
  code, and the result struct is now printed in full.
- All thirteen termination outcomes are tabulated, grouped into the three kinds that actually drive
  control flow.
- The single most dangerous interaction in the whole exercise — routing into a fan-in join, which
  fails *silently and successfully* — is called out in a dedicated trap note and restated twice more,
  with the concrete defensive action ("count the aggregator's invocations before you trust the
  status"). I reproduced the failure exactly as described.
- The build recipe is correct as written, including the two counter-intuitive parts (turn the engine's
  developer build off first; do not link `agentengine_warnings`), and its own performance numbers
  matched my measurement.

Four one-line API details had to be recovered (F1–F4), and none of them required opening an engine
header: one from a citation string, three from compiler diagnostics. That is a *good* ratio — the
failure mode here is not "the docs don't tell you how to build it", it is "the docs don't print the
last mile of a couple of signatures".

**How long?**

For a C++ developer competent in the language but new to AgentEngine, with the docs open:

- **~20–30 minutes to a first fan-out/fan-in run that prints its output** — the reference page's graph
  block and the concurrent-pattern page's snippet are close to copy-paste, and the local helpers are
  named and explained.
- **~1–2 hours to the finished product** including the status taxonomy, the failure paths, the
  `switch_case` variant, and reading enough of the fan-in trap note to defend against it.
- The earlier caveat ("add 30–60 minutes if you need the reason for a run fault") is **withdrawn**:
  `failed_executor` + `partial` + `rounds` are documented and sufficient for a diagnosable fault. It
  applies only to a product that must branch on `failure_class`.

Build cost on top of that is negligible: **5 s configure, 25 s from cold, 9 s incremental**. The
first-build cost the on-ramp page warns about (2,336 build statements, 49 s configure) is real but was
fully avoided by the two `CACHE BOOL "" FORCE` lines it tells you to write.

**Bottom line:** the docs are sufficient for this product, and they got better during the session.
The two things I would still fix are (1) say how `WorkflowSupervisor::initialize()` reports a refused
graph now that it returns `void`, and (2) put `#include` lines on the workflow reference page the way
the on-ramp page already does.
