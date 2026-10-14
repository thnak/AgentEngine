# devsim/rerun7 — a document-processing pipeline built on AgentEngine, from the docs

**What this is.** A developer-facing build of the brief: a five-node fan-out/fan-in document pipeline
(fan-out to three workers, fan-in to one aggregator), a validated termination bound, the result read
out and the run's status branched on, a second variant of the *same* graph whose routing edge is a
`switch_case` chosen by a classifier, a failure path that prints the engine's own error and exits
non-zero, and a build that works.

**What I worked from.** The marketing docs only, as instructed:
`web/marketing/src/data/apiContent.ts`, `web/marketing/src/components/ApiWorkflowReference.tsx`,
`ApiWorkflowPatternDetail.tsx`, `ApiFirstAppReference.tsx`, `ApiSnippetConventions.tsx`,
`ApiIncludes.tsx`, `ApiDetailLayout.tsx`, `ApiBuilderReference.tsx`, `ApiPizzaOrderReference.tsx`,
plus `web/marketing/api/*.html` (thin per-page Vite entry shells — all of the content is in the
`.tsx`/`.ts` files above; no HTML page carries prose of its own). Engine source (`include/`) was
opened only where noted, and every such place is a numbered fallback below.

**Measured end state.** Configures and builds clean on a cold cache; `docpipeline.exe all` exits 0
and prints, in order:

```
== variant a: docpipeline-a ==
[aggregate] one delivery, 3 ContentItem(s), in graph-declared order:      <- fan-in: ONE delivery
            [1] doc=...;entities=Invoice,Ana,Reyes
            [2] doc=...;dates=2026-03-04
            [3] doc=...;keywords=disputed,charge,contract
[status] completed after 4 round(s)
[check] aggregator invoked 1 time(s) -- fan_in is one delivery, not one per edge
[result] doc=...;entities=Invoice,Ana,Reyes;merged=...;summary=3 branches merged, 7 distinct tokens

== variant b: docpipeline-b ==                                            <- SAME graph + classifier
[status] completed after 5 round(s)
[result] doc=...;route=structured;structured=entities present, table form
[check] classifier chose 'structured'; sink bodies invoked: 1 (structured)

== variant b: docpipeline-b ==                                             <- same graph, other doc
[result] doc=the refund arrived late, so we filed a complaint.;entities=;merged=;route=plain;plain=no entities, prose form
[check] classifier chose 'plain'; sink bodies invoked: 1 (plain)

== failure path: an unbounded graph ==
REFUSED graph 'docpipeline-unbounded'
  validate_workflow: workflow declares no termination bound -- 014 §2 requires MaxRounds, a deadline, or a budget; an unbounded workflow does not run (workflow.unbounded)
== failure path: three malformed graphs, each refused by the engine ==
  (refused 3 of 3)
== failure path: a classifier naming an undeclared route ==
[status] routing_failed after 4 round(s) -- failed_executor='classify'

(variant a exit 0, variant b exit 0, variant b/2 exit 0, broken-graph exit 2, refusals exit 0, bad-route exit 4)
```

Full captured stdout/stderr: `run-all.stdout.txt`, `run-all.stderr.txt`.

**Files.** `main.cpp` (one translation unit), `CMakeLists.txt`, `build.bat` (toolchain convenience
only, not part of the deliverable). Nothing outside `devsim/rerun7/` was created or modified;
`git status` shows the same pre-existing `web/marketing` modifications that were already in the tree
when I started, and no change attributable to this work.

---

## Getting started — what the docs gave me, and what they did not

| # | Requirement | Where it came from |
|---|---|---|
| a | **Include the engine** | **In docs.** `ApiWorkflowReference.tsx:23-29` is a five-header `ApiIncludes` panel rendered at `:885` (`agentengine/workflow/graph.hpp`, `agentengine/rt/workflow_supervisor.hpp`, `agentengine/workflow/workflow_event.hpp`, `agentengine/rt/block_on.hpp`, `agentengine/core/content.hpp`), with the split explained at `:66-78`. **But it is incomplete — see fallback 4.** |
| b | **Declare the graph as data** | **In docs.** The verbatim struct definitions are in `apiContent.ts:3000-3050` (`edge_kind`, `executor_kind`, `Executor`, `Edge`, `TerminationBound`, `Workflow`) plus the minimal example at `:2991-2998`. Also in docs, and load-bearing: the namespace split (`ApiWorkflowReference.tsx:122-137` — data is `agentengine::workflow`, running types are `agentengine::rt`, and `sharing_mode` is a *third*, `agentengine`) and the defaults (`:147-162`). My `make_variant_a()`/`make_variant_b()` are that data. |
| c | **Supply executor bodies; know how they match nodes** | **In docs**, and unambiguous: `sup.initialize(wf, bodies)` takes the body vector positionally, `bodies[i]` ↔ `wf.executors[i]` — stated at `apiContent.ts:3086-3087` and again at `:3364-3365` ("bodies parallel to wf.executors by index: src, upper, lower, title, agg"). `ExecutorBody`'s signature is `:3172-3175`. The two body shapes are `result<Message>` for a plain pass-through and `result<ExecutorOutcome>` for a routing node (`apiContent.ts:3102`, `:3160`). **One wrinkle the docs do not state — fallback 3.** |
| d | **Get the result out** | **In docs.** `WorkflowResult` field-by-field at `apiContent.ts:2819-2836`, re-explained in prose at `ApiWorkflowReference.tsx:402-413` (`output` is the output selection's result; `failed_executor` names the node; `partial` preserves what the other nodes produced). |
| e | **Read and act on the run's status** | **In docs, and better than I expected.** All thirteen `workflow_status` values in a three-kind table (success / bound / fault / refusal) at `ApiWorkflowReference.tsx:365-393`; "the status alone is not enough" with a ready-made `switch` at `apiContent.ts:2838-2855`; and the one trap spelled out at `:114-118` and `:380` — `completed` with an *empty* output is what a starved fan-in looks like. Printing it: `workflow_status_tag(status)` returns `char const*`, "pass it straight to `printf`" (`:394-401`). My `report()` is a direct descendant of that snippet and that table. |
| f | **Handle errors** | **In docs.** Validation-then-run is prescribed at `:414-426`: `validate_workflow(wf)` is the call that gives you a `result`, check it *before* `initialize()` because `initialize()` returns `void` and reports nothing; `workflow_status::invalid` from a run is the backstop. `check_workflow_executable` and the two-check split are at `:428-447`. The error model itself (`result<T>` = `std::expected<T, error>`, `.error().code` to branch on, `.message` for the operator) is `ApiFirstAppReference.tsx:365-383`. Error codes are named in docs at `ApiFirstAppReference.tsx:390`, `ApiBuilderReference.tsx:208`, `ApiPizzaOrderReference.tsx:265-266`, `apiContent.ts:3029`, `ApiWorkflowReference.tsx:157-159`, `:435`. |
| g | **Build / link** | **In docs, exactly.** `ApiFirstAppReference.tsx:80-123` is a whole `CMakeLists.txt`: `cmake_minimum_required(VERSION 3.28)`, `AGENTENGINE_BUILD_TESTS`/`AGENTENGINE_BUILD_EXAMPLES` set `OFF CACHE BOOL "" FORCE` *before* `add_subdirectory()`, an `AGENTENGINE_ROOT` cache variable, `add_subdirectory("${AGENTENGINE_ROOT}" agentengine)`, `target_link_libraries(myapp PRIVATE agentengine::core)`, and an explicit note (`:114-116`) *not* to link `agentengine_warnings`. `:297-307` covers the MSVC/Ninja developer-prompt prerequisite; `:308-309` the expected output. Mine is that file with one changed line (the root points at this checkout instead of `third_party/AgentEngine`). |

Net: **of the seven steps, all seven are in the docs**, and five of them (b, c, d, e, g) I could
have written straight off the page. The two that cost real time were (a) — an incomplete include
list — and one sub-case of (c). Everything that cost time is itemised below.

---

## Fallback log

**Total: 7 fallbacks.** Of those, **1 was a real build failure** (a hard compile error), **1 is a
genuine omission in a panel whose stated job is to prevent exactly that problem**, and **5 are
smaller gaps** — a helper named but not shipped, an include that is real but is not among the page's
own headers, a silent concatenation, and two places where the docs contradict or omit a detail I
confirmed from source. Numbered in the order I hit them.

The `text_of` include — the item the brief asks about by name — is fallback 4; it is also expanded
on its own in the next section, and it is counted **once**.

### 1. `text_message(...)` does not exist, and the docs say so — but do not say what to write instead

* **Where the docs stop.** Every workflow snippet on every pattern page ends with
  `RunWorkflow{text_message("hi")}` (`apiContent.ts:3071`, `:3088`, `:3104`, `:3128`, `:3147`,
  `:3247`, `:3281`, `:3395`, `:3405`). `ApiSnippetConventions.tsx:24` does tell the truth about it —
  "Example-local shorthand for the Message inside a `StartRun{...}`" — but nothing in the docs shows
  the construction it is shorthand *for*.
* **What I fell back to.** `include/agentengine/core/content.hpp:153-209` for the real `ContentItem`
  / `Message` shape (`value` is a `std::variant<Text, ...>`; `origin` defaults to `assistant`), then
  `grep` across `include/agentengine` to confirm there is no engine `text_message` at all. My
  `user_text()` is the result.
* **Disposition.** Docs failure, mild: the truth is disclosed, the replacement is not. A reader can
  recover in one grep, or from the first-app page's `HelloClient`, which builds a `Message` by hand
  (`ApiFirstAppReference.tsx:30-37`).

### 2. `node_desc(...)` is named but not shipped

* **Where the docs stop.** `ApiSnippetConventions.tsx:32-33` lists `node_desc` among the
  "example-local helpers on the workflow pages", and points at `examples/23_handoff_mesh.cpp:103`.
  But `node_desc` is how *every* concurrent/router/reflection/handoff graph in the docs builds its
  executors (`apiContent.ts:3077`, `:3094`, `:3111`, `:3141`, `:3263`, `:3332`, `:3353`), so a reader
  replacing it needs `Executor`'s exact aggregate-init form — which `apiContent.ts:3007-3022` *does*
  give, but only in a comment-laden struct dump, not as a call site.
* **What I fell back to.** Nothing, strictly: `{id, executor_kind::function, "Text", "Text"}` follows
  from `:3007-3010` plus the documented `workflowMinimalSnippet` at `:2994-2995`, which writes
  `Executor{"A", executor_kind::function, "Text", "Text"}` longhand. I wrote `node_desc` as a
  one-liner over that.
* **Disposition.** Docs failure, mild, and self-inflicted more than engine-inflicted: the longhand
  form is on the page twice.

### 3. **A routing body cannot be written as `result<Message>`** — the only hard build failure

* **Where the docs stop.** `apiContent.ts:3172-3175` says, of the fan-out workers, "a `Message`
  converts implicitly into an `ExecutorOutcome` for the common 'just pass a message downstream' case
  — these bodies need no changes to plug into the graph". True, and it is why my `ingest`,
  `entities`, `dates`, `keywords`, `aggregate` and `summarize` are all declared
  `result<Message>` and compile. The routing example at `:3102` declares a different return type,
  `result<ExecutorOutcome>`, with no note that the difference is *required*. I wrote my classifier in
  the documented `result<Message>` form and got:

  ```
  main.cpp (the classifier's return): error C2440: 'return': cannot convert from
      'agentengine::rt::ExecutorOutcome' to 'std::expected<agentengine::Message,agentengine::error>'
  ```

  The reason is that the implicit `Message` → `ExecutorOutcome` conversion (`workflow_supervisor.hpp:242`)
  is a *user-defined* conversion, and it sits inside `std::expected`'s own converting constructor —
  two user-defined conversions in one implicit sequence, which C++ does not allow. So the
  convenience the docs advertise works for `std::function<result<ExecutorOutcome>(...)>`'s return-type
  covariance but **not** through an explicitly written `result<Message>` return type.
* **What I fell back to.** `include/agentengine/rt/workflow_supervisor.hpp:220-249` to see the two
  constructors and the `ExecutorBody` alias.
* **Disposition.** **Docs failure, and the sharpest one.** The page shows both spellings and
  explains only one of them; the explanation it gives is the one that misleads. One sentence —
  "a body that sets `routes` must declare `result<ExecutorOutcome>`" — closes it. Fixed in one edit,
  cost ~1 build cycle (~15 s after the first configure).

### 4. The `#include` for the engine's own text reader is not in any of the page's five headers

The item the brief asks about. Expanded in full in the next section; in short:
`text_of(...)` is used by **every** body snippet on the page, is real engine API at
`include/agentengine/core/tool_call_extraction.hpp:83`, exists in none of the five headers the
workflow page's "what to include" panel lists (`ApiWorkflowReference.tsx:23-29`), and is documented
only on a different page — `ApiSnippetConventions.tsx:29`. One `grep` to confirm the spelling; no
build cycle lost, because I looked before writing the include rather than after the compiler
complained.

* **Disposition.** Docs failure — a cross-page retrieval rather than a true absence. Cheap to fix:
  add `agentengine/core/tool_call_extraction.hpp` to `WORKFLOW_HEADERS`.

### 5. The engine's own `text_of` concatenates — fan-in delivery must be read per `ContentItem`

* **Where the docs stop.** `apiContent.ts:2862` says a `fan_in` target "receives one ContentItem per
  contributing branch in graph-declared order", and `:3177-3206` shows an example-local
  `all_text_of` that joins them with `" + "`. Nothing states what the *engine's* `text_of` does with
  a multi-item message.
* **What I hit.** `text_of()` is `tool_call_extraction.hpp:83-89`: it appends every `Text` part with
  **no separator**. My first working build therefore printed
  `...;entities=...doc=...;dates=...doc=...;keywords=...` — three branch records run together, which
  then broke the aggregator's own field parsing downstream. Verified against source, fixed by
  iterating `in.content` and using `std::get_if<Text>` (which is exactly the loop the docs' own
  `all_text_of` shows at `:3180-3189`).
* **Disposition.** Docs failure, mild: the per-item reading loop *is* on the page, one paragraph
  below the fan-in description, as an example-local helper. I had to learn it was necessary by
  running into it.

### 6. `text_of`'s header is documented on a different page from the one that lists includes

* `ApiSnippetConventions.tsx:29` says `text_of(message), make_stream<T>(...)` are "Real engine API —
  `core/tool_call_extraction.hpp` and `core/stream.hpp` respectively." Correct.
* But the same conventions list at `:32` attributes `all_text_of` to `examples/09_concurrent_workflow.cpp:64`
  as example-local — also correct — and the workflow page's own include panel
  (`ApiWorkflowReference.tsx:66-78`) explains `core/content.hpp` as the header that "gives you
  `Message` and `Text` to build one", without mentioning `text_of` anywhere.
* So the one header a reader needs for the most-used text accessor in every snippet on the page is
  documented on a *different* page, in a collapsed `<details>` panel (`ApiSnippetConventions.tsx:106`)
  that every API page renders (`ApiDetailLayout.tsx:40`).
* **Disposition.** Docs failure — a retrieval problem, not an absence. Same one-line fix as fallback
  4; listed separately because the *cause* is different (the path is in the docs, the panel does not
  carry it).

### 7. Details the docs never state, which I confirmed from source rather than assumed

Three, grouped because each is a single unstated fact, none of which blocked anything:

1. **`WorkflowSupervisor::initialize()` has two further defaulted parameters.**
   `workflow_supervisor.hpp:739-741` is
   `initialize(Workflow, std::vector<ExecutorBody>, std::vector<EffectContext> = {}, std::string designated_stall_reporter = {})`.
   The docs mention `contexts[i]` and a stall reporter only in prose about `agent`-kind nodes
   (`ApiWorkflowReference.tsx:438-442`, `apiContent.ts:3037-3039`), never as a signature. I call the
   two-argument form; it works because the defaults are real.
2. **A per-node invocation counter must be captured by value, not by reference.** The docs *do*
   say the mechanism — "initialize() takes the body vector by value, so a body is a copy that can
   outlive the scope that built it" (`apiContent.ts:3119-3121`) — and show
   `std::make_shared<std::atomic<int>>` at `:3122`. What they do not show is that this is why the
   `counted(...)` helper exists at all, and my first draft did capture by reference with a raw
   `std::shared_ptr` counter. Fixing it to match the documented idiom removed the dangling-capture
   risk.
3. **`RunWorkflow{...}`'s third field is a `std::stop_token`.** `workflow_supervisor.hpp:544-554`.
   Irrelevant to a synchronous run and never needed, but the aggregate-init form is the documented
   one (`apiContent.ts:3071`) and it is worth knowing the struct is not empty.

**Nothing else was reverse-engineered.** In particular I did **not** need to: find the
`Workflow`/`Executor`/`Edge` field order (on the page), find the seven `edge_kind` values and their
meanings (on the page, twice — as a table and as an enum), find `validate_workflow` or
`check_workflow_executable` (on the page), find `run_workflow`/`RunWorkflow` (on the page), find the
status enum (on the page, with a three-kind taxonomy), find `workflow_status_tag` (on the page,
including the `char const*` gotcha), find that a bound is mandatory (on the page, three times), find
that `initialize()` returns `void` (on the page), find which build options to turn off and which
target to link (on the page), or find the `set_capabilities(&held)` lifetime hazard (on the page —
not used here, but it is the kind of thing this docs set is unusually good about).

---

## Specifically: did I have to work out any `#include` path myself?

**One, yes — and it is the most-used accessor in every snippet on the workflow page.**

* **What the docs give me.** `web/marketing/api/workflow.html` (rendering
  `ApiWorkflowReference.tsx`) shows a panel titled **"what to include"** (`ApiIncludes.tsx:13`),
  emitted from a five-entry list at `ApiWorkflowReference.tsx:23-29` and rendered at `:885`. Its
  prose (`:66-78`) walks the five: `workflow/graph.hpp` for the data, `rt/workflow_supervisor.hpp`
  for the running types, `workflow/workflow_event.hpp` for the live view, `rt/block_on.hpp` to drive
  from a synchronous `main`, `core/content.hpp` for `Message` and `Text`. All five paths are correct
  and all five compiled first try. That panel is why (a) in the table above is "in docs".
* **What it does not give me.** `text_of(...)`, which appears in **every** body snippet on the page
  (`apiContent.ts:3161`, `:3168`, `:3193`, `:3201`, `:3205`, `:3292`, `:3303`, `:3317`, `:3379`,
  and the result handler at `:2846`). Grepping the five listed headers for it returns nothing. Its
  real home is `include/agentengine/core/tool_call_extraction.hpp:83`.
* **Where I actually found it.** Not in `include/` — in a *different docs page*: the collapsed
  "How to read the code on this page" note that `ApiDetailLayout.tsx:40` renders on every API detail
  page, at `ApiSnippetConventions.tsx:29`, which says verbatim that `text_of` is "Real engine API —
  `core/tool_call_extraction.hpp`". So the path *is* in the docs — on the snippet-conventions note
  rather than in the workflow page's include panel, which is exactly the panel whose own header
  comment (`ApiIncludes.tsx:6-8`) says it exists because "a reader who cannot guess a header is stuck
  before they can try anything."
* **Cost.** One `grep` across `include/agentengine` to confirm the header and its exact spelling
  before adding the line. No build cycle was lost to it, because I looked before writing the include
  rather than after the compiler complained.
* **Verdict on this question.** I did **not** have to guess any of the five advertised paths, and I
  did not have to work out `text_of`'s path by trial and error. But I did have to retrieve it from a
  second page, and if `ApiSnippetConventions.tsx:29` were ever trimmed, nothing else in the docs
  would tell a reader where `text_of` lives.

One further include note, offered as a finding rather than a fallback: my file adds
`agentengine/core/tool_call_extraction.hpp` to the five. It also does **not** need
`agentengine/workflow/workflow_event.hpp` — that header is listed for the live-view vocabulary I do
not use. Including it costs nothing and I left it in, because the panel's job is to be the *safe*
list, and it is.

---

## Hard blockers

**There were none.** Stated plainly, because it is the headline:

* Configure succeeded first attempt: CMake with the VS 18 MSVC 19.51 toolchain and Ninja,
  `CONFIGURE_EXIT=0`, 3.8 s, 30 build statements, nine engine static libs. That is the same
  "30 build steps" `ApiFirstAppReference.tsx:289-293` predicts for a consumer that sets the two
  options — the docs' own number, reproduced.
* Linking succeeded first attempt. `agentengine::core` alone was sufficient for a workflow program
  with no model provider, no secret store and no network. No unresolved externals, no WinSock or
  runtime-library surprises — the `ws2_32` link (`CMakeLists.txt:100-102`) is carried by
  `agentengine::core` as advertised.
* Exactly **one** compile error in the whole exercise, fallback 3, fixed by changing one function's
  return type.
* The one thing that *could* have been a blocker was a documentation hazard rather than a build
  failure, and it never fired: the docs recommend a shallow build directory because Windows caps a
  full object path at 250 characters (`ApiFirstAppReference.tsx:254-263`). My build directory is
  `devsim/rerun7/build` under a checkout at `C:\Users\thanh\Git\AgentEngine`, and no
  `CMAKE_OBJECT_PATH_MAX` warning appeared; the engine's compile-fail tests are off.

---

## Undefined identifiers, and where each came from

Every identifier in `main.cpp` that is not defined in `main.cpp`, with its real home. "Docs" means
the marketing docs named it and I took it from there; "source" means I had to open `include/`.

| Identifier | Real home | Where I learned it |
|---|---|---|
| `Workflow`, `Executor`, `Edge`, `TerminationBound` | `agentengine::workflow` (`workflow/graph.hpp`) | Docs (`apiContent.ts:3000-3050`) |
| `edge_kind`, `executor_kind`, `edge_failure_policy` | `agentengine::workflow` (`workflow/graph.hpp`) | Docs (`apiContent.ts:2857-2876`, `:3004-3005`) |
| `MessageTypeId` | `agentengine::workflow` = `std::string` (`graph.hpp:53`) | Docs (`apiContent.ts:3010`), type itself source |
| `workflow_status` | `agentengine::rt` (`rt/workflow_supervisor.hpp:438`) | Docs (`ApiWorkflowReference.tsx:379-393`) |
| `workflow_status_tag()` | `agentengine::rt` (`workflow_supervisor.hpp:492`) | Docs (`ApiWorkflowReference.tsx:394-401`) |
| `WorkflowResult`, `ExecutorOutput` | `agentengine::rt` (`workflow_supervisor.hpp:588-619`) | Docs (`apiContent.ts:2819-2836`) |
| `ExecutorBody` | `agentengine::rt` (`workflow_supervisor.hpp:248`) | Docs (`apiContent.ts:3172`) |
| `ExecutorOutcome` | `agentengine::rt` (`workflow_supervisor.hpp:220`) | Docs (`apiContent.ts:3101-3104`) |
| `RunWorkflow`, `ResumeWorkflow` | `agentengine::rt` (`workflow_supervisor.hpp:544`, `:562`) | Docs (`apiContent.ts:3071`, `:2839`) |
| `WorkflowSupervisor` | `agentengine::rt` (`workflow_supervisor.hpp:644`) | Docs (`apiContent.ts:3069`) |
| `validate_workflow()` | `agentengine::workflow` (`graph.hpp:297`) | Docs (`ApiWorkflowReference.tsx:420`) |
| `check_workflow_executable()` | `agentengine::workflow` (`graph.hpp:547`) | Docs (`ApiWorkflowReference.tsx:434`) |
| `block_on()` | `agentengine::rt` (`rt/block_on.hpp:326`) | Docs (`ApiSnippetConventions.tsx:17`) |
| `sharing_mode` | `agentengine` (`core/sharing_mode.hpp`), **not** `workflow` | Docs (`ApiWorkflowReference.tsx:127-131`) |
| `Message`, `ContentItem`, `Text`, `role`, `content_origin` | `agentengine` (`core/content.hpp`) | Docs, shape source (`content.hpp:42,153,175,197`) |
| `Usage` | `agentengine` (`core/content.hpp:177`) | Docs (`apiContent.ts:2835`) |
| `EffectContext` | `agentengine` (`core/effect_context.hpp`) | Docs (`apiContent.ts:3160`) |
| `result<T>`, `error`, `failure_class` | `agentengine` (`core/error.hpp`) | Docs (`ApiFirstAppReference.tsx:365-383`) |
| **`text_of()`** | `agentengine` (`core/tool_call_extraction.hpp:83`) | Docs, **on another page** — fallback 4 |
| `user_text()` (mine) | `main.cpp` | Replaces the docs' example-local `text_message` — fallback 1 |
| `node_desc()`, `counted()`, `counter()` (mine) | `main.cpp` | Replace the docs' example-local `node_desc`/`counted` — fallbacks 2 and 7.2 |
| `tagger()`, `field()`, `tokens_of()`, `print_error()`, `report()`, `validate_or_report()` (mine) | `main.cpp` | My own; nothing documented is hidden behind them |

No identifier was left undefined, and no name had to be guessed from a compiler error.

---

## Blockers, ranked

Ranked by how much of a competent developer's time each would cost if they hit it unaided. None of
these stopped the build; the ranking is about friction, not severity.

1. **The routing-body return type (fallback 3).** The only error that actually stops a build, it
   arrives as a C2440 that names a `std::expected` conversion rather than the routing rule, and the
   page states the *opposite-leaning* half of the rule ("a `Message` converts implicitly into an
   `ExecutorOutcome` ... these bodies need no changes"). Highest cost, lowest fix cost: one sentence
   on `ApiWorkflowReference.tsx` near `:305`.
2. **The include panel missing `text_of` (fallback 4).** Not a build failure — a stopped reader.
   Every snippet on the page uses it, the panel exists specifically to prevent header-guessing, and
   the answer is on a different page in a collapsed panel. Add one string to `WORKFLOW_HEADERS`.
3. **`text_message`/`node_desc` named but not shipped (fallbacks 1, 2).** Disclosed honestly by
   `ApiSnippetConventions.tsx`, so a careful reader knows to look; a hasty one copies a snippet and
   gets "undeclared identifier". The fix is a three-line `using`-free helper block on the page, or a
   "substitute this" line in the conventions row.
4. **Engine `text_of` on a multi-item fan-in delivery (fallback 5).** Silent wrong output rather
   than any error — the worst *kind* of problem, but the correct loop is on the page as
   `all_text_of`. Worth one sentence in the fan-in description: "read the delivery per item; the
   engine's `text_of` concatenates with no separator".
5. **Unstated defaulted parameters (fallback 7.1–7.3).** Cosmetic. The two-argument `initialize()`
   is what every snippet shows, and it works.

---

## Confidence, per requirement

| Requirement | Confidence | Basis |
|---|---|---|
| 1. Graph with fan-out to three workers and fan-in to one aggregator | **High** | Built and run; the aggregate printed exactly 3 `ContentItem`s in graph-declared order on every run, and its body was invoked exactly once — checked by counter, not inferred from output (`apiContent.ts:223-232` is the claim; my run reproduces it). |
| 2. Real termination bound, validated before running | **High** | `wf.bound.max_rounds = 8/10` on both variants; `validate_workflow(wf)` called and checked before `initialize()`; the counter-demo (no bound) was refused with the engine's own `workflow.unbounded`. |
| 3. Run it, get the result out, branch on status | **High** | `WorkflowResult r = block_on(sup.run_workflow(...))`; `switch (r.status)` over the documented three kinds; `[result]` printed from `text_of(r.output)`; the empty-output case is guarded separately. Two successful runs and two failing runs exercised it. |
| 4. Second variant of the SAME graph, one edge a `switch_case` chosen by a classifier | **High** | Variant B is variant A plus `classify` and two `switch_case` edges; run twice with different documents, and the classifier's own label selected a different sink each time (`structured`, then `plain`) with exactly one sink body invoked — instrumented, so a hardcoded branch would have failed the check. |
| 5. Failure path: engine's own error/status printed, exit non-zero | **High** | Four failure shapes exercised: no bound (`workflow.unbounded`, exit 2), missing case label (`workflow.missing_case_label`), case label on a non-switch edge (`workflow.unexpected_case_label`), and a validating graph whose classifier names an undeclared route (`routing_failed`, `failed_executor='classify'`, `partial` preserved, exit 4). Every printed string is the engine's, not mine. |
| 6. Buildable and runnable | **High** | Cold configure 3.8 s / 30 steps; incremental rebuild of `main.cpp` links in ~5 s; `docpipeline.exe all` exits 0. One compile error in total (fallback 3). |
| Docs completeness for this task | **Medium-high** | Every step is documented and the two hardest things (status taxonomy, `initialize()` returning `void`) are documented *well*. The web is not seamless: one wrong-leaning sentence, one incomplete include panel, two unshipped helpers, one silent concatenation. |

I did **not** verify, and so claim nothing about: request ports / suspend-resume, checkpointing,
time travel, the event stream, per-edge failure policy (`retry`/`fallback`/`propagate` — documented
at `ApiWorkflowReference.tsx:337-363`, not exercised), `agent`- or `sub_workflow`-kind executors
(documented as refused on this build, `:428-447`), `WorkflowBuilder`/`MagenticWorkflowBuilder`, the
live-model paths, or anything on Windows other than this machine's VS 18 / MSVC 19.51 / Ninja.

---

## Verdict

**Can a competent C++ developer build this from the docs alone? Yes — with one wrong turn, and in
about two hours.**

The favourable half is more than I expected from a docs set for a large C++ engine. The workflow
page carries the graph as data verbatim, all seven edge kinds with meanings, the mandatory bound,
the validator split, the full thirteen-value status enum *organised by what a handler should do with
it*, a ready-made fault handler that already knows `failed_executor` and `partial` exist, the
namespace trap (`sharing_mode` lives in a third namespace), and the pointer-lifetime trap on
`set_capabilities`. The first-app page carries a complete, compiled `CMakeLists.txt` including the
"turn the developer build off first" cache-variable idiom and the "do not link `agentengine_warnings`"
warning. Nothing about the build had to be discovered.

The cost is concentrated in exactly two places. First, the routing-body return type: the page shows
both `result<Message>` and `result<ExecutorOutcome>` bodies, explains only the first as a
convenience, and that explanation is what a reader will follow — so the one compile error in this
exercise is a doc error rather than a reader error, and it is the kind that costs a developer twenty
minutes because C2440 about `std::expected` reads as a template problem, not an authoring rule.
Second, the include panel omits `text_of`, the accessor every snippet on the page uses; the answer
exists, on a different page, in a collapsed panel. Both are one-line fixes.

Time estimate for a competent C++ developer who reads the workflow page before typing, as
instructed: **~45 minutes to a first clean build** (configure is the slow part at ~4 s cold, but
reading the page properly is the real cost), **~1.5–2 hours** to this complete program — the extra
time spent entirely on the two items above plus wiring the classifier's two documents. A developer
who copies snippets first and reads second should budget **3 hours**, because the failure mode of
fallback 3 is a misleading compiler error and the failure mode of fallback 5 is silent wrong output.
Neither is fatal; both are avoidable with a sentence each in the docs.
