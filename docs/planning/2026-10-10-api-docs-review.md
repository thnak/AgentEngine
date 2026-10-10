# API documentation review — does the site teach an *application developer*?

Date: 2026-10-10
Scope: the API documentation family in `web/marketing` — the hub `api.html` plus the 28 detail
pages under `web/marketing/api/*.html`, their 27 reference components in
`web/marketing/src/components/Api*.tsx`, and the ~169 code examples those components render from
`web/marketing/src/data/*.ts`.
Lens: **a developer building their own application on top of AgentEngine** — someone who adds
`#include "agentengine/..."` to their own program, wires the engine into their own build, and
writes app code. Not an engine contributor, not an RFC reviewer.

Method: static reading of every reference component and the snippet constants behind it; an
automated tally of every `CodePanel` and its `filename=` label; and verification of the
consumption path against the root `CMakeLists.txt`, `examples/CMakeLists.txt`, and
`examples/01_hello_agent.cpp`.

A counting note: there are **169 `CodePanel` call sites** across the 27 reference components. One of
those components, `ApiWorkflowPatternDetail.tsx`, is shared by all 8 workflow-pattern pages and
holds 13 of the call sites, of which each page renders only 1–2. So 169 is the size of the code
corpus, not what any single page shows; the shares in §3 F3 are shares of that corpus.

---

## 1. Headline

The site is a **faithful, unusually honest engine-internals reference**. It is not yet an
**application developer's reference**, and the gap is not one of volume — 169 code examples is a
lot. It is a gap of *kind*:

> **Not one code example on the entire site shows a line that includes a header, and not one shows
> how a program starts.**

Every one of the 169 examples is a fragment lifted from the middle of a file. A developer who reads
the whole API family can learn precisely what each type does and can still be unable to write,
build, and run a single program of their own.

That is the review's finding. Everything below is the evidence, then the ranked fixes.

---

## 2. What is genuinely good (do not lose this in a rewrite)

| Property | Evidence |
| --- | --- |
| Every claim is cited to source | 169 panels carry a real `filename=`; ~70 `gh(...)` deep links to headers/tests at the exact line |
| Honesty about what is not built | `ApiDiagnosticNote status="stub"` marks always-pass checks (`ApiAgentReference`, `ApiEventsReference`, `ApiTrustSandboxReference`, `ApiPluginsReference`, `ApiWorktreeReference`); `apiPages.protocols` is the one page badged *Designed, not built* |
| The engine/author split is already seen on one page | `ApiAgentReference.tsx:53-62` redirects app developers to `QuickstartSessionBuilder` on the Builder page instead of letting them read the CRTP authoring surface by mistake |
| Genuinely runnable examples exist and are used | 36 panels are transcribed from `examples/*.cpp` |
| Bilingual parity is real, not decorative | every page's prose, table rows, and ladder steps carry `en`/`vi`; snippets are language-neutral code |
| Newcomer on-ramps already exist | `api/how-it-works.html` (orientation) and `api/pizza-order.html` (worked example) were added for exactly this audience |
| A real hierarchy exists where it matters | the 8 workflow-pattern pages are deliberately second-level: linked from `ApiWorkflowReference.tsx:734` via `patternHref()` and cross-linked to each other (`ApiWorkflowPatternDetail.tsx:611`), instead of flattened into the 20-entry left rail |

The fix is to *add a spine*, not to rewrite the reference.

---

## 3. Findings, ranked

### F1 — Every example is a fragment: zero `#include`, zero `int main()`

Measured across all nine snippet data files (`apiContent.ts`, `builderContent.ts`,
`durabilityContent.ts`, `eventsContent.ts`, `hitlContent.ts`, `memoryContent.ts`,
`providerContent.ts`, `streamingContent.ts`, `content.ts`):

| Probe | Count in all rendered snippets |
| --- | --- |
| `#include` lines | **0** |
| `int main` | **0** in code (3 occurrences, all prose references to other files) |

The sharpest way to see it: the repo's own smallest complete program,
[`examples/01_hello_agent.cpp`](../../examples/01_hello_agent.cpp), is 155 lines, carries 10
`#include` lines, has a real `main()`, and reaches no network or credential — it is the ideal first
program. The site renders **11 lines of it**
(`helloAgentRunSnippet`, `apiContent.ts:572-581`), dropping:

- the `#include` block (`examples/01_hello_agent.cpp:23-33`),
- `main()` (`:133-155`),
- the `Tool<>` declaration and its two `AE_JSON_SCHEMA` types (`:50-66`),
- the deterministic offline `JokerChatClient` (`:93-129`),
- the `ae::result` error path on `make_agent_session<Joker>()` (`:138-141`).

What survives is the interesting middle — the `Joker` policy declaration and two calls. A reader
cannot tell what to include, where any of it goes, or how the fake client they are told about is
written. This is representative, not exceptional: it is the *shape* of all 169 panels.

**This includes the two on-ramp pages added for this exact audience.** `api/how-it-works.html` and
`api/pizza-order.html` follow the house convention too: the pizza page's seven snippets
(`SNIPPET_TOOLS` … `SNIPPET_ESCALATION` in `ApiPizzaOrderReference.tsx`) are presented as the files
of a fictional app (`pizza_palace/tools.hpp`, `pizza_palace/order_flow.cpp`, …) and each is cited to
the real example it was transcribed from — but none shows its `#include` block either, so a reader
still cannot compile the story they have just followed.

### F2 — There is no consumption story, and the engine presently makes one hard

This is the biggest obstacle, and it is larger than the docs.

- **The engine cannot be installed or `find_package`d.** Root `CMakeLists.txt` contains **0
  `install(` rules and 0 `export(` rules** (1444 lines). Every one of the ~30 `agentengine::`
  targets is an in-tree alias.
- **So `add_subdirectory` is the only real path — and nothing says so.** No page in the API family
  mentions `add_subdirectory`, `FetchContent`, or `target_link_libraries`; neither does the README
  (whose headings are spec-index headings: *The shape of it*, *Core invariants*, *Layering*, …).
  No document in `docs/` addresses a consumer project either.
- **Consuming by `add_subdirectory` is currently hostile to a consumer**, by default and
  undocumented:
  - there is **no `PROJECT_IS_TOP_LEVEL` guard** anywhere in the root `CMakeLists.txt`;
  - `AGENTENGINE_BUILD_TESTS` defaults **ON** (`CMakeLists.txt:1440`) and unconditionally does
    `add_subdirectory(tests)` plus `enable_testing()`;
  - `AGENTENGINE_BUILD_EXAMPLES` defaults **ON** (`:1433`) and does `add_subdirectory(examples)`;
  - `add_subdirectory(tools)` at `:1431` is unconditional;
  - `AGENTENGINE_VENDOR_ASIO` defaults **ON** (`:189`), so a consumer's configure step downloads a
    dependency they did not choose;
  - there are **17 `option()`s**, and only three presets (`base`, `dev`, `release`) — all in-repo
    developer presets, none for a consumer.
  A developer who adds the engine to their project therefore inherits `tools/`, `examples/`, the
  entire `tests/` tree, and a vendored Asio fetch, and must discover by trial which of 17 options to
  override.
- **The site's only build guidance is the contributor path.** `GettingStarted` on `index.html`
  (`web/marketing/src/data/content.ts:315-348`) is four steps: `git clone`, `cmake -S . -B build`,
  `cmake --build`, `ctest`. Step 04 is *"Run the correctness gate."* That teaches you to build and
  test AgentEngine; nothing on the site teaches you to build a program that uses it.

The good news is that the answer is short. Every offline example links exactly:

```cmake
target_link_libraries(agentengine_example_01_hello_agent PRIVATE agentengine::core agentengine_warnings)
```

(`examples/CMakeLists.txt:12`; the same two-line shape holds for all the offline examples). A
"your first app" page needs to state: vendor or subdirectory the engine, pre-set
`AGENTENGINE_BUILD_TESTS`/`AGENTENGINE_BUILD_EXAMPLES` OFF, and link `agentengine::core`.

### F3 — The examples are mostly about the engine, not about using it

Classification of all 169 `CodePanel` labels:

| What the panel shows | Panels | Share |
| --- | --- | --- |
| Engine header / `src/` internals | 89 | 53% |
| Engine test excerpts (`tests/`, `test_*.cpp`) | 24 | 14% |
| **Application-level (`examples/*.cpp`)** | **36** | **21%** |
| **Application-level (pizza-order worked example)** | **7** | **4%** |
| Data / config samples (YAML, SKILL.md, JSON, directory trees) | 7 | 4% |
| Protocol wire samples (`POST /v1/messages`) | 2 | 1% |
| Other | 4 | 2% |

**113 of 169 (67%) show the engine's own internals; 43 (25%) are application-level.** For a reader
whose question is "how do I use this?", two thirds of the code on screen answers a question they
did not ask. This is defensible as *evidence* — the site's culture is to prove each claim against
real source — but the app-level examples need to be visually separated from the proof, not
interleaved at equal weight.

### F4 — The API is result-based; the examples are 82% happy-path

`result<` appears 99 times across the snippet data — the error model is central to the API and the
site says so repeatedly. But of the 91 snippets in `apiContent.ts`, only **16 (18%) demonstrate
handling a failure** (`has_value()`, `std::unexpected`, `.error()`); **75 show the success path
only.**

`ae::result<T>` is a distinctive, non-obvious part of what a developer must write on every call.
Telling them it exists 99 times while showing it handled 16 times leaves the most common thing in
their program undemonstrated.

### F5 — Offline and credential-requiring examples are not distinguished

The Builder page is the page `ApiAgentReference` explicitly redirects app developers to — so it is
the primary app-developer page. Its one end-to-end example,
`quickstartWorkedExampleSnippet` (`builderContent.ts:301-323`, from
`examples/30_session_builder_quickstart_live.cpp`), requires a live remote model and an API key
from the environment (`AGENTENGINE_OPENROUTER_API_KEY`).

That is a legitimate thing to document. But it is presented without an offline counterpart on the
same page, so the first thing an app developer tries to copy is the one example that cannot run
without a credential and a network. The engine ships deterministic offline clients
(`JokerChatClient` in example 01, `ReplayChatClient`) precisely so this is avoidable.

### F6 — The hub offers 28 equal-weight cards and no order

`ApiApp.tsx:28-69` renders every page as an identical `pillar-card` in one grid. There is no "if you
are building an application, read these in this order" path, and no visual distinction between the
two orientation pages, the 20 reference pages, and the 8 workflow-pattern pages. A newcomer's first
decision — *where do I start* — is left as a choice among 28.

---

## 3b. Confirmed defects, verified against the tree

The findings above are structural. These are specific and were each re-read against the headers
before being recorded. Six were fixed in this pass; the rest need a decision.

**Fixed.**

| # | Defect | Verification | Status |
| --- | --- | --- | --- |
| D1 | **Pizza Act 6 was affirmatively wrong, not merely incomplete** — `ApiPizzaOrderReference.tsx`'s `SNIPPET_ESCALATION` declared all three executors `executor_kind::agent` while binding them to plain lambdas through the two-argument `initialize(wf, bodies)`, and gave both `switch_case` edges an empty case label. | `validate_workflow` rejects the empty label with `workflow.missing_case_label` (`graph.hpp:383-386`, "is switch/case or multi-selection but carries no case label") — two lines above the snippet's own `check(validate_workflow(wf).has_value(), …)`, which would therefore have failed. Independently, `initialize()` folds `agent_kind_bodies_are_structurally_agent_backed()` into `valid_base_` (`src/rt/workflow_supervisor.cpp:30`), and that predicate (`:1687-1696`) returns false for any `agent`-kind node whose body is not tagged `AgentExecutorBodyTag` — i.e. not produced by `agent_session_as_executor_body()`. | **Fixed**: rewritten from `examples/23_handoff_mesh.cpp` (function-kind `node_desc`, labelled `switch_case` edges, `request_port` for the human, `direct` edge taking `{}`). The prose no longer claims the snippet shows a ceiling; it now states what an `agent`-kind node would additionally require, citing `examples/26`. |
| D2 | **Pizza Acts 4 and 5 dereferenced a `result`/`optional` unguarded** — `restore_from_record(**loaded)` and `read_chat_call_recording(…).value()`. | `examples/12_session_checkpoint.cpp:120` and `:122` double-guard (`loaded.has_value() && loaded->has_value()`) before `**loaded` at `:128`. `read_chat_call_recording` returns `result<ChatCallRecording>` (`core/chat_recording.hpp:183`), and `write_chat_call_recording` is `[[nodiscard]] result<void>` (`:180`). Transcribing the page's version crashed where the real example does not. | **Fixed**: both now guard, and the discarded `[[nodiscard]]` write result is checked. |
| D3 | **`api/agent.html` contradicted its own snippet.** The prose said "no example in this repo yet constructs a running `AgentSession` from that validated metadata. Both surfaces are real; they simply aren't wired to each other yet." | `ADR-226-agent-run-bridge.md` is titled *"a declared agent becomes a running session"*, and `make_agent_session` (`core/agent_session_bridge.hpp`) is exactly that wiring — called at `examples/01_hello_agent.cpp:136`, in the panel three lines below the prose. The snippet's own comment in `apiContent.ts:569-571` already said so. | **Fixed** in both languages: the section now describes the two construction roads correctly and cites ADR-226. |
| D4 | **"Six edge kinds" was stale in seven places** — intro, section heading, section body, patterns body, TOC label (both languages), and a prose row in `workflowEntries`. | `edge_kind` has **seven** enumerators; `switch_default` was added by ADR-215 (`workflow/graph.hpp:90-97`). | **Fixed**: all seven now say seven. |
| D5 | **`workflowGraphSnippet` claimed to be "The graph, verbatim" but omitted fields** — no `switch_default`, no `Executor::capability_ceiling`, no `Executor::batch`, no `TerminationBound::max_stalls`/`max_resets`. | `graph.hpp:97` (`switch_default`), `:135` (`capability_ceiling`), `:143` (`batch`, ADR-235), `:253-254` (`max_stalls`/`max_resets`, ADR-149). The omitted ceiling field is the one the pizza page's Act 6 prose leans on. | **Fixed**, with the label semantics (`switch_case` requires a label, others refuse one) stated inline. |
| D6 | **`api/events.html`'s worked example did not compile as printed** — `example29Snippet` called `session.set_capabilities(&held)` with `held` never defined. | `examples/29_agent_session_events.cpp:120` defines `CapabilitySet const held = CapabilitySet::grant_root({});` on the line before; the streaming page's copy of the same lines already had it. | **Fixed**, with the borrowed-pointer lifetime contract noted — the thing that makes the missing declaration more than a typo. |

**Found in round 1, fixed in rounds 3–5** (recorded here rather than deleted, because the pattern
matters: four of these six are *provenance* defects — a panel or a page asserting a source that is
not the source — which is the one class of defect a reader cannot detect by trying to compile).

| # | Defect | Evidence | Resolution |
| --- | --- | --- | --- |
| D7 | **Three panel labels name files that do not exist**, presented as filenames in the panel chrome: `web_search_tool.hpp` (tool page), `SearchReply.schema.json` (tool page), `doc_agent.hpp` (builtin-tools page). | No such paths in the tree. They are illustrative labels, but the chrome (`filename=`) asserted otherwise, and every other panel's label is a real path. | **Fixed**: relabelled `your_tool.hpp — the shape, not a file in this repo` and equivalents, so the chrome no longer asserts a path. A gate check asserts the three strings are absent. |
| D8 | **`workflowGroupChatSnippet` was labelled `workflow/graph.hpp` and is not from any real file.** | The label was wrong twice over: not that header, and not any real example or test. | **Fixed**: relabelled `an illustrative graph — not a file in this repo`; the snippet's own comment already said it was a generalisation. |
| D9 | **`workflowReflectionSnippet`'s critic body was rewritten.** The page invented `int revisions = 0;`; the real test uses `critic_calls->fetch_add(1) < 2` on a shared atomic and `appender(...)`. | Graph structure matched the test; the body did not. | **Fixed**: transcribed faithfully from CY-1, including the shared-atomic counter and the graph's real id, with the capture rule explained. |
| D10 | **The workflow hub's "not yet built" box was stale, and its citation landed on the wrong code.** | It claimed `check_workflow_executable` refuses `agent`/`sub_workflow`; the truth is that the *one-argument* overload does and the contexts-aware sibling does not. Its cite `graph.hpp:416-443` pointed at fallback-target validation. | **Fixed**: the box now states which call refuses what, and cites `:547-559` (the refusal) and `:581` (the contexts-aware sibling). |
| D11 | **`api/providers.html` buried its decisive fact** as row 8 of a 10-row audit table at the bottom, after eight sections assuming the symbols exist. | Root `CMakeLists.txt` sets `AGENTENGINE_WITH_HTTPS` default **OFF**. | **Fixed**: lifted to a callout at the top of the seam section, with the misleading `C2653` failure mode named. |
| D12 | **`drive(...)` was called in ~35 places across six data files and defined nowhere.** | Grep across `src/data/*.ts`. | **Addressed**: the shared conventions note (on every detail page) names `drive` as example-local and gives `agentengine::rt::block_on()` as the real equivalent and the one to use. The snippets still call `drive`, deliberately — rewriting 35 verbatim excerpts to use a different name would be its own provenance defect. |

---

## 4. What an application developer actually needs, in order

| # | Need | Status today |
| --- | --- | --- |
| 1 | How do I depend on the engine and link it? | **Missing entirely** |
| 2 | A complete program I can copy, compile, and run offline | **Missing** — no example shows includes or `main()` |
| 3 | The five pages to read, in order | **Missing** — 28 equal cards |
| 4 | How do I handle errors? | **18% of examples** |
| 5 | How do I get from the fake offline client to a real model? | Partial — mentioned in a comment in `examples/01_hello_agent.cpp:16-19`, not on the site |
| 6 | What is real vs designed? | **Good** — badges, stub notes, per-page status |
| 7 | Field-by-field depth per subsystem | **Excellent** — this is what the 169 panels deliver |

The site is strong at 6 and 7 and absent at 1, 2, and 3.

---

## 5. Recommended fixes, by value per unit of work

**Fix A — a "Your first app" page (highest value, moderate effort).**
One new page, `api/your-first-app.html`, added first in the `core` group. It contains, in order:
(1) the four-line CMake block above, with the two options to pre-set and why; (2) the **complete
verbatim** `examples/01_hello_agent.cpp` — all 155 lines, includes and `main()` included — in one
`CodePanel`, because that file is already the correct first program; (3) the `cmake --build` +
run commands; (4) a one-paragraph pointer to how to swap `JokerChatClient` for a real provider; (5)
the error-handling path on `make_agent_session<Joker>()`, called out rather than buried.
Cost: one component, one entry, one `vite.config.ts` input, one `apiPages` entry per language,
`howItWorks`/`pizzaOrder`-style sections constant.

**Fix B — put a `#include` block and an entry point on the app-level examples (low effort, wide
effect).**
For the 36 `examples/*.cpp` panels, prepend the file's real `#include` block and append nothing
else, or split each into "the declaration" + "the program". Even doing this for just the header
examples (`examples/01`, `02`, `05`, `06`, `08`, `12`) removes the "where does this go?" problem
for the pages most likely to be read first. This is a mechanical change to
`src/data/*Content.ts` snippets — but see the conflict note in §6.

**Fix C — an app-developer on-ramp on the hub and sidebar (low effort).**
Split `ApiHubGrid` into two labelled bands — *Start here* (how-it-works, your-first-app,
pizza-order) and *Reference* (the rest) — and mark the app-facing pages in `ApiSidebar`. Answers
F6 without touching 28 pages.

**Fix D — show the failure path in every snippet that returns a `result` (moderate, mechanical).**
Bring the 75 happy-path snippets up to at least: capture, `if (!r) return std::unexpected(r.error());`
or the equivalent, once. Fixes F4.

**Fix E — an offline sibling for every live example (moderate).**
On the Builder page and any page whose worked example needs a credential, add the deterministic
offline variant beside it, labelled. Fixes F5.

**Fix F — make the engine consumable (engineering, not docs — flag, do not do here).**
`PROJECT_IS_TOP_LEVEL` guards around `tools/`, `examples/`, `tests/`, and a `install(`/`export(`
set with an `AgentEngineConfig.cmake` would let Fix A's CMake block be `find_package` instead of
`add_subdirectory` plus two forced options. This is a change to the build system, needs its own
decision, and is out of scope for a docs review — but the docs cannot fully solve F2 while the
build system forces a consumer to know its internals.

---

## 6. Scope, caveats, and risk

- **Measured, not estimated.** Every count in §3 comes from the files listed in *Scope*; the CMake
  claims come from reading the root `CMakeLists.txt` (1444 lines) and `examples/CMakeLists.txt`.
- **This review measures the tree as it stands on branch `adr237-async-design`** (PR 169 head), not
  `main` — `main` is frozen until PR 169 merges, and the marketing site is being edited on both
  sides.
- **Conflict risk for any fix.** PR 169 already touches
  `web/marketing/src/data/apiContent.ts` and `web/marketing/src/components/ApiToolReference.tsx`.
  Fixes A, B, and C all edit `apiContent.ts`, and Fix B edits the snippet data files. Sequencing
  those edits after PR 169 merges would avoid a second round of conflicts; the alternative is to
  accept and resolve them.
- **Not verified visually.** No headless browser is available in this environment, so rendering,
  layout, and the pizza page's animation are reasoned about statically, not observed.
- **Provenance was spot-checked, not exhaustively audited.** This review verified specific snippets
  against the headers, tests, and examples they cite (§3b), which is how D1–D6 were found. It did
  **not** verify all 169 panels line by line. Three known provenance defects (D7–D9) came out of
  that spot-check, which suggests the site's "verbatim, never invented" convention is mostly but not
  universally honoured — an exhaustive pass would be its own piece of work.
- **Fixed in this pass:** D1–D6. **Not fixed:** D7–D12 and Fixes A–F, all of which are decisions
  about scope rather than unclear defects.

---

## 6b. Per-page verdict, from the three batch reviews

Each page was read in full, with the snippet constants behind its panels, by a reviewer working only
from the app-developer lens. 28 pages, 169 panel call sites.

| Page | Audience it actually serves | Panels | App-level | Best→worst rank within its batch |
| --- | --- | --- | --- | --- |
| `hitl` | mixed, app-dev **strongest** | 12 | 6 | 1 of 8 (safety/platform) |
| `pizza-order` | **app developer** | 7 | 7 | the only genuinely app-targeted page reviewed |
| `builder` | **app developer** | 9 | 2 | 1 of 8 (app-core) |
| `router` (pattern) | app developer | 2 | 2 | 1 of 8 patterns |
| `streaming` | mixed | 10 | 4 | 2 of 8 (app-core) |
| `sequential` (pattern) | app developer | 2 | 2 | 2 of 8 patterns |
| `skill` | mixed, app-dev-leaning | 12 | 5 | 2 of 8 |
| `agent` | mixed, leaning engine-author | 3 | 3 | 3 of 8 |
| `durability` | mixed, app-dev-leaning | 9 | 4 | 3 of 8 |
| `reflection` (pattern) | mixed | 2 | 1 | 3 of 8 patterns |
| `providers` | mixed, engine-author-leaning | 8 | 4 | 4 of 8 |
| `runtime` | mixed | 12 | 8 | 4 of 8 |
| `concurrent` (pattern) | app developer | 2 | 2 | 4 of 8 patterns |
| `builtin-tools` | app-dev-leaning, delivered as internals | 4 | 1 | 5 of 8 |
| `trust-sandbox` | engine-author | 14 | 3 | 5 of 8 |
| `group-chat` (pattern) | mixed | 2 | 1 | 5 of 8 patterns |
| `tool` | mixed → engine-author | 7 | 2 | 6 of 8 |
| `handoff` (pattern) | app developer | 1 | 1 | 6 of 8 patterns |
| `codeact` | engine-author | 7 | 1–2 | 6 of 8 |
| `memory` | engine-author | 13 | 2 | 7 of 8 |
| `worktree` | engine-author | 8 | 0 | 7 of 8 |
| `events` | engine-author | 11 | 2 | 8 of 8 |
| `map-reduce` (pattern) | app developer | 1 | 1 | 7 of 8 patterns |
| `plugins` | **neither — plugin author** | 4 | 0 | 8 of 8 |
| `planner` (pattern) | fragment only | 1 | 1 | 8 of 8 patterns |
| `workflow` (hub) | engine-author | 3 | 0 | below the workflow family average |
| `protocols` | engine-author | 5 | 0 | below the workflow family average |
| `how-it-works` | mixed, leaning engine-author | **0** | 0 | no panels at all |

A note on the **App-level** column: it is the reviewer's per-page judgement of "panels a developer
building on the engine could actually use", which is deliberately wider than §3 F3's strict count.
F3 counted only panels whose `filename=` begins `examples/` or `pizza_palace/` (43 of 169); the
reviewers also credited panels that show engine API *surface* a consumer must call — a real
`CapabilitySet::grant_root(...)`, a `minimalCapabilitiesSnippet`, a conformer declaration — even when
the panel's label is a header. Read F3 as the hard floor and this column as the practical ceiling.

Two things this table makes visible that the aggregate numbers hide:
1. **The pages aimed at this audience are not the pages with the most app-level code.** `builder` and
   `pizza-order` are the two pages that explicitly say they are for app developers, and they hold 9
   of the 43 app-level panels between them. The rest is spread across pages whose framing is an
   audit.
2. **`worktree` and `plugins` have zero app-level panels each**, and `how-it-works` — the page the
   site tells a first-time reader to read *first* — has no code at all. `how-it-works`'s
   `#ownership` section, which lists exactly what a host must supply (`ChatClient`,
   `SandboxBackend`, `SessionStore`, `ApprovalDecider`, `Tool<>` conformers, `MemoryProvider`, the
   socket), is the best conceptual on-ramp on the site and the only one whose entries are not
   linked to the pages that construct them.

---

## 7. Round 1 remediation — what changed, and how it was verified

The review above is diagnosis. This section is the first round of treatment, and the evidence for
it. The working rule adopted here: **a documentation claim about behaviour is verified by running
the engine, not by reading a header** — the same falsifiable-gate discipline the RFCs use.

### 7.1 The instrument

Three things were built to make "are the docs good?" measurable rather than a matter of opinion.

| Instrument | What it does | Where |
| --- | --- | --- |
| **A consumer compile harness** | Compiles and links an arbitrary `.cpp` against the already-built engine using the exact flags and library set CMake gives the repo's own examples (extracted from `build-dev/build.ninja`). Attempts the repo's own `/W4 /WX` gate first, then retries without `/WX` so "does not compile" is distinguishable from "compiles but warns". | `devsim/verify-consumer.ps1` |
| **An authentic consumer CMake project** | A real parent project that `add_subdirectory()`s the vendored engine and links `agentengine::core` — the path a developer actually takes, tested both naively and with the documented options. | `devsim/consumer-cmake/`, `devsim/consumer-cmake-setform/` |
| **A snippet-fidelity assertion** | Asserts that the program printed on the page is byte-identical to the program that was compiled and run, so the page cannot drift from it silently. | `devsim/compare-snippet.js` |

Plus a claim check (`devsim/verify-claims.js`) asserting that specific false strings are absent from
the built bundles and their corrections are present, an encoding guard
(`devsim/verify-encoding.js`, 211 files: no BOM, valid UTF-8, no mojibake — written because this
session corrupted a file that way once), and an anchor guard (`devsim/verify-anchors.js`: every
right-rail TOC entry must resolve to an id its page actually renders).

All of it runs as one command:

```
& .\devsim\verify-all.ps1      # 7 gates: build, lint baseline, snippet fidelity, claim check,
                               # encoding integrity, TOC anchors, and the program compiles+runs
```

Last run: **ALL GATES PASS**.

### 7.2 The consumption measurement (F2, now quantified)

Two consumer projects were configured against the same vendored engine, one naive and one with the
two options the new page documents:

| Consumer configure | `agentengine_example_*` refs | `compile_fail` refs | build statements | configure time |
| --- | --- | --- | --- | --- |
| naive `add_subdirectory()` | 810 | 1,135 | **2,336** | 48.9 s |
| with `AGENTENGINE_BUILD_TESTS`/`EXAMPLES` OFF | 0 | 0 | **159** | 4.4 s |

The naive configure also pulled the engine's vendored dependencies over the network and tripped
`CMAKE_OBJECT_PATH_MAX` (183-character object paths). Both documented forms were verified —
`-D` on the command line, and `set(... CACHE BOOL "" FORCE)` before `add_subdirectory()`; the second
is what the page prints, and it was checked to produce the same 159-statement build. The consumer
binary built and ran: `hello from the consumer build`, exit 0.

### 7.3 What the three developer simulations found

Three sub-agents were given real product briefs (a CLI order desk with a custom tool and a live
provider; a refund service needing human approval across a restart; a document pipeline combining
fan-out/fan-in with routing) and told to build them, preferring the docs and **logging every
fallback to engine source as a documentation failure**. All three built and ran their programs.

| | fallbacks | headline blocker |
| --- | --- | --- |
| Sim 1 (CLI + live provider) | 18 | the authority wiring (`chat_client_grants`) is undocumented and its absence looks like a broken API key |
| Sim 2 (approval + restart) | 20 | `set_capabilities()` stores a non-owning pointer; a local `CapabilitySet` segfaults |
| Sim 3 (workflow) | 17 | `drive`/`check`/`text_message` used 103 times and defined zero times |

Across all three, the same two structural complaints recurred: **no snippet showed an `#include`**
and **no page explained how to depend on or link the engine**.

### 7.4 Changes made in response

**New page: `api/first-app.html`** (`ApiFirstAppReference.tsx`) — the on-ramp that did not exist.
It carries the consumer CMake with the measured build cost, a complete 68-line program whose
includes and `main()` are intact and which is asserted byte-identical to the compiled one, the
build/run commands, the real-provider step with the credential grant, and the error model.

**New shared note: "How to read the code on this page"** (`ApiSnippetConventions.tsx`, rendered by
`ApiDetailLayout` on every detail page) — says why a copied snippet may not compile, glosses the
example-local helpers, names `agentengine::rt::block_on()` as `drive()`'s real equivalent, and
flags the `set_capabilities()` lifetime footgun. One change, addressing the complaint every
simulation and every page reviewer raised.

**Corrected false claims — three, two of them on pages written earlier in this session:**

| Claim | Reality | Verified against |
| --- | --- | --- |
| Pizza Act 4: a restored interaction is "still answerable after the restart" | It comes back *listed* but not answerable: `restore_from_record` clears `suspended_rounds_`, so `resolve_interaction` always fails with `session.resolve_interaction.round_not_recorded` and closes it, nothing run | `src/rt/agent_session_core.cpp:805`, `:423-432` |
| HITL page: a suspended run "can be resumed after a genuine process restart" | Same as above; only the Durability page told the truth | same |
| Pizza Act 6: a graph with empty `switch_case` labels that "validates" | `validate_workflow` rejects an empty label (`workflow.missing_case_label`), and `agent`-kind nodes bound to plain lambdas are refused | `workflow/graph.hpp:383-386`; `src/rt/workflow_supervisor.cpp:30`, `:1687-1696` |

**Documented a silent wrong-answer trap discovered by simulation and then reproduced here.** A
`fan_in` target with two or more declared sources is held until *every* one resolves
(`HeldFanIn`, `rt/workflow_supervisor.hpp:1031-1038`). Mix routing with it — a `switch_case` fires
exactly one branch — and the join starves: the graph validates, reports
`workflow_status::completed`, invokes the aggregator **zero** times, and returns an empty answer.
Reproduced end to end (`devsim/p3`, scenario `mixed-fanin`: `aggregator invoked 0 time(s)`,
`answer: ''`, exit 0). Now a caveat callout on the workflow page.

**Other corrections:** the full `workflow_status` table with its three-way bound/fault/refusal
split (was 4 of 13 documented); seven edge kinds, not six, in seven places including a TOC label;
five missing fields in the "graph, verbatim" snippet; an undefined identifier in the Events page's
worked example; a wrong return-type comment on the Durability page; the `agentengine::sharing_mode`
namespace trap; and the approval-seam framing, which implied an amount threshold could live in
`PolicyDecider` when that seam receives no argument values (`core/tool_pipeline.hpp:170-171`).

**Hub on-ramp:** the flat grid of 29 equal cards is now two bands — *Start here* (first-app →
how-it-works → pizza-order) and *Reference*.

### 7.5 Verification state at the end of round 1

- `npm run build` clean; 29 API pages emitted; `npm run lint` 11 warnings, 0 errors, none in any
  file touched (the 11 are pre-existing, in `ApiToc`, `ApiDurabilityReference`,
  `ApiWorkflowPatternDetail`, `LanguageContext`).
- `node devsim/compare-snippet.js` → **IDENTICAL**: the documented program is exactly the compiled one.
- `node devsim/verify-claims.js` → **ALL CHECKS PASS**: 5 false strings gone, 16 corrections present.
- The documented program compiles and links under `/W4 /WX` and runs (`Hello from AgentEngine.`,
  exit 0). Both consumer CMake forms configure and build (159 statements, exit 0).
- **The corrected Act 6 is verified by running it** (`devsim/verify-act6/main.cpp`, 5 assertions,
  exit 0): `validate_workflow` accepts the graph, routing to Kitchen completes and the Kitchen
  branch actually runs, and routing to the Manager `request_port` suspends with a real open
  interaction. The version this replaced would have failed the first assertion and the second
  scenario's `initialize()`.
- **Not verified:** the real-provider step as printed. Both of its defects were found by simulation
  and fixed from the source and from that simulation's reproduced compiler/engine errors; the HTTPS
  path was not rebuilt here.
- **Not verified visually:** no headless browser, so rendering and layout are reasoned about
  statically.

### 7.6 Still open after round 1

The unfixed defects from §3b (D7–D12), and these, all reported by simulation and not yet addressed:

- **`drive`, `check`, `user_message` remain example-local.** The conventions note now *explains*
  them and points at `rt::block_on`, but the snippets still do not compile as printed. Fix B — the
  mechanical one — is still the fix.
- **The offline configure hatch** (`AGENTENGINE_VENDOR_ASIO=OFF` + `AGENTENGINE_ASIO_ROOT`) is now
  in the CMake snippet as a comment, but is not explained.
- **`/W4 /WX` arrives via `agentengine_warnings`**, which all 35 examples link and the consumer CMake
  does not. A consumer who copies an example's link line inherits a warnings-as-errors gate nothing
  mentions.
- **`all_text_of`** exists only as a docs snippet, not as an includable function.
- **`AgentEngineSecretStore`** is named three times across the docs and constructed zero times
  before this round; the provider step now constructs it, but no page explains `SecretSource`
  implementations generally.

---

## 8. Round 2 — the re-run, and what the number now is

The three developer simulations were re-run against the improved docs, same three product briefs,
same instruction that every fallback to engine source is a documentation failure. This is the
objective's scorecard rather than a review of it.

### 8.1 The measurement

| Simulation | Round 1 fallbacks | Round 2 fallbacks | What it built |
| --- | --- | --- | --- |
| CLI order desk + custom tool + live provider | 18 | **7** | configure 7.4 s, build 37.8 s (161 edges), `AGENTENGINE_WITH_HTTPS=ON`; reproduced `secret.not_found` and `agent.tool_name_collision` |
| Refund service: approval across a real restart | 20 | **10** | 30 ninja steps, zero warnings, first try; two real processes and a real store; reproduced `round_not_recorded` |
| Document pipeline: fan-out/fan-in + routing | 17 | **3** | clean consumer build 18.3 s / 30 steps / 9 engine libs — independently reproducing §7.2's numbers |

**55 fallbacks → 20.** Two of the three finished with no more than ten, and the workflow one had a
single hard blocker.

The qualitative results matter more than the counts:

- *"I never had to open `workflow/graph.hpp` or `rt/workflow_supervisor.hpp` to write the program."*
  — the strongest possible statement of the goal, from the workflow simulation.
- The offline first-app program is *"buildable from the docs alone, verbatim."*
- The consumer CMake recipe was copied **verbatim** into a scratch project and worked; the docs'
  own "30 build steps" figure was reproduced exactly by two independent simulations.
- The fan-in trap warning was reproduced on two separate classifier inputs, and the prescribed fix
  (route before the join) was verified working: aggregator invoked exactly once with 2 merged items.
- The round-1 corrections held: the reload story — that a restored interaction is listed but cannot
  be answered, failing with `session.resolve_interaction.round_not_recorded` — was reproduced
  across two processes and called *"the strongest material in the set."*
- All 13 `workflow_status` values were confirmed exhaustive against the header; a correct handler
  for each is writable from the docs alone.

### 8.2 What the re-run found wrong — including in round 1's own new work

Every one of these was verified against the tree before being fixed.

| Defect | Reality | Where |
| --- | --- | --- |
| `api/first-app.html`'s real-model snippet did not compile: `AnthropicChatClient<Store>` written bare | The class is `agentengine::anthropic::AnthropicChatClient`; MSVC reports a misleading `C7568` | `protocol/anthropic/chat_client.hpp:118,444` |
| That same snippet omitted `caps.tool_calling` | `ChatClientCapabilities::tool_calling` defaults **false**, so an agent with tools silently never gets a tool call | `core/chat_client.hpp:37` |
| Its CMake hardcoded `third_party/AgentEngine` | Now a `CACHE PATH` variable; the documented form was re-configured and rebuilt to confirm | — |
| The Durability page said `AgentSessionRecord` "carries seven fields" and printed seven | It has **eight** — `require_authority` (ADR-061 §22.1) — and that field is in the bytes on disk | `rt/agent_session_core.hpp:300-312,332-346` |
| The pizza page's Act 0 said configuring an `ApprovalDecider` makes a `policy_driven` call "answerable in-process instead of suspending", while Act 3 said the opposite | The decider's answer is a single `bool`: allow or deny in-process. It **cannot escalate**, so "refund above 500 needs a human" is expressible in neither seam | `core/tool_pipeline.hpp:149-150,170-171` |
| The suspend sentinel was named only by identifier (`kSuspendedForApproval`) while the docs advise branching on `error().code` | The strings are `run.suspended_for_approval` and three siblings; the failure class is `contract`, not an error class | `rt/agent_session_core.hpp:1216-1239` |
| `ResolveInteraction` was used positionally and never defined | Nine fields; the third is `caller`, load-bearing for the page's own admission-denial claim | `rt/agent_session_core.hpp:129-160` |
| `Described<T, "...">`'s read side was undocumented | `args.order_id == "x"` does not compile; read `.value` | `core/json_schema.hpp:100-117` |
| `json_schema_of<T>()` used unqualified in two places | It is `agentengine::schema::json_schema_of` | `core/json_schema.hpp:126,286` |
| The workflow page's validator sentence read as exhaustive | It is not: ~29 refusal codes exist, including `workflow.unreachable_executor`, which is exactly what a reader following the trap warning hits first | `workflow/graph.hpp:302-605` |
| The `completed` row in the status table said the output selection "produced a result" | A starved fan-in reports `completed` with **no** result. A handler written from the table alone had the very bug the trap box warns about | reproduced by simulation |

### 8.3 Still open after round 2

- **`drive` / `check` / `user_message` remain example-local.** The conventions note now glosses
  `text_message`, `all_text_of`, `node_desc`, `counted`, `appender` and `handler` as well, and the
  pizza page now shows the head of its file. Snippets still do not compile standalone. This is a
  deliberate position — a fragment cannot be made compilable by adding includes when it also
  references file-local types — but Fix B (make each snippet complete) is unstarted.
- **The Builder API has no documented way to attach `Tools<...>`** — a dead end for any application
  that has a tool, and the Agent page recommends the Builder API for app code.
- **`ToolCallHookContext::arguments`** is never named, so the only documented-API way for host code
  to learn the argument values of a call about to suspend is undiscoverable.
- **The join between "declare an agent" and "suspend a run"** is documented only on the Builder page;
  six facts needed for ten lines of code live on four pages.
- **Offline configure** (`AGENTENGINE_VENDOR_ASIO=OFF` + `AGENTENGINE_ASIO_ROOT`) is a commented
  hatch with no explanation of what value to give it.
- D7–D12 from §3b remain.

Addressed since the re-run reported it: **a tool together with a real provider** is now shown —
`api/first-app.html` §4 item 5 declares a tool, names it in the agent, and states that
`caps.tool_calling` must be set. The declaration is verified by compilation and binding
(`devsim/verify-tool-agent`, exit 0), with the Anthropic client itself substituted for an offline
stub because the HTTPS build is not available here.

### 8.4 Verification state at the end of round 2

`& .\devsim\verify-all.ps1` → **ALL GATES PASS** (8 gates: build, lint baseline, snippet fidelity,
40 claim checks — 6 strings that must be gone and 34 corrections that must be present — encoding
integrity across 214 files, 21 pages' TOC anchors, the documented program compiling/linking/running,
and the documented tool+agent declaration compiling and binding). The pizza page's new file-preamble
is itself compiled and run (`devsim/verify-preamble`, exit 0), and the documented CMake recipe was
re-configured and rebuilt to confirm the path-variable form works as printed.

---

## 9. Round 3 — verification, and a correction to round 2

The simulations were run a third time against the round-2 revision, same three briefs.

### 9.1 The measurement

| Simulation | R1 | R2 | R3 | Hard blockers (R3) |
| --- | --- | --- | --- | --- |
| A — CLI order desk, custom tool, live provider | 18 | 7 | **5** | **none** — compiled first attempt; 3 real live turns ran |
| B — refund service, approval across a restart | 20 | 10 | **6** | **none** — 30 steps, first try, zero diagnostics |
| C — document pipeline, fan-out/fan-in + routing | 17 | 3 | **4** | **none** — 30 build steps, **zero engine headers read** |

**55 → 20 → 15 fallbacks, and zero hard blockers in all three.** Round 2 still had blockers; round 3
has none. Two of the three compiled on the first attempt with no error iterations.

What the round-3 runs establish beyond the count:

- Simulation A ran **three real live turns** against an Anthropic-compatible endpoint and printed
  answers whose numbers exist only in its own tool's reply — so the tool genuinely executed — plus
  two real failure paths (`secret.not_found`, `anthropic.http_401`).
- Simulation B verified **11 documented claims by running them**, across two real processes and a
  real on-disk store, including all four suspend sentinel strings verbatim.
- Simulation C reproduced the fan-in trap exactly (`completed`, aggregator invoked 0, empty output)
  and then verified the documented fix works — without opening a single engine header or source file.
- Two independent simulations reproduced the docs' own build figures: 30 build steps, ~25–45 s cold,
  ~4–9 s incremental.

### 9.2 A round-2 change that verification caught — in the wrong direction

Round 2 taught the **new** `invoke` form (non-static `const` coroutine member) on the tool page,
because `core/tool.hpp:219-226` documents it as ADR-237 §5.2's target shape. Before publishing it I
compiled it, and it **failed**:

```
core/tool_descriptor.hpp(211): error C2352: 'OrderLookup::invoke':
  a call of a non-static member function requires an object
```

The registration path still calls `invoke` statically, so the documented *target* shape is not yet
usable — the header describes where ADR-237 is going, not where it is. The tool page now teaches the
static form, states the new shape, states the failure, and says when to convert. The verification
program exercises both spellings and asserts the static one binds.

This is the case for the compile harness earning its keep: the page would otherwise have shipped a
snippet no developer could compile, in the one place every tool author reads.

### 9.3 What the round-3 runs found, and what changed

| Defect | Reality | Source of truth |
| --- | --- | --- |
| The tool page documented the **retiring** form and called the new one "still accepted transitionally" — backwards | The static form is the one that binds; the non-static member fails `C2352` | compiled, above |
| The Durability page explained the post-restart failure as the **history-tail** guard, naming `session.resolve_interaction.stale` | The first guard is the missing suspended-round record: `round_not_recorded`, which *closes* the interaction with nothing run. `stale` is a second guard a live session hits. Fixed in both languages, in both places it appeared | `src/rt/agent_session_core.cpp:423-439` |
| `AgentSessionRecord` "seven fields" in the Vietnamese text — a **second** occurrence the round-2 fix missed | Eight, as the English text already said | `rt/agent_session_core.hpp:300-312` |
| `-DAGENTENGINE_WITH_HTTPS=ON` triggers two **undocumented network fetches** (mbedTLS, a pinned CA bundle) with no offline override, while the page gave one for Asio | Both have local overrides (`AGENTENGINE_VENDOR_MBEDTLS`/`_MBEDTLS_ROOT`, `AGENTENGINE_VENDOR_CA_BUNDLE`/`_CA_BUNDLE_PATH`); the CA bundle is a `file(DOWNLOAD)`, so a git-only proxy fails there specifically. A locked-down CI was hard-blocked | `CMakeLists.txt:679-772` |
| The two build options were called "the whole fix" for offline use | They remove the test/example tree; they do not stop the dependency fetches | measured in §7.2 |
| `using namespace agentengine;` shown as sufficient | Every runtime type — `StartRun`, `ResolveInteraction`, `SessionCaller`, `AgentSession`, every suspend sentinel — is `agentengine::rt::` | `rt/agent_session_core.hpp:59` |
| The prose promised a graph could be "refused", but `initialize()` returns `void` with no return channel | The refusal surfaces at the first run entry point as `workflow_status::invalid`; `validate_workflow(wf)` is the call that returns a `result` | `src/rt/workflow_supervisor.cpp:169,241,425` |
| `workflow_status_tag()` described as a "plain-string form" | It returns `char const*`, so `.c_str()` fails to compile | `rt/workflow_supervisor.hpp:492` |
| `ExecutorOutcome`'s namespace absent — on the page that warns about namespaces and lists every graph type except that one | `agentengine::rt::` | `rt/workflow_supervisor.hpp` |
| `failed_executor` scoped to `executor_failed`/`merge_conflict` in the new result section | Also set on `routing_failed`, so the docs understated a useful field | `src/rt/workflow_supervisor.cpp:1072` |
| A fresh shell has neither `cl.exe` nor Ninja on `PATH`; the failure reads `CMAKE_CXX_COMPILER not set` | Stated as a prerequisite | reported by two simulations |

Also fixed this round, from the original review's defect list: three panel labels that asserted
filenames no file has (`web_search_tool.hpp`, `SearchReply.schema.json`, `doc_agent.hpp`); the
group-chat panel labelled `workflow/graph.hpp` for a graph that is in no file; `node(...)` where
every cited example spells `node_desc(...)`; and a reflection snippet whose critic body had been
rewritten rather than transcribed, now faithful to CY-1.

### 9.4 Still open after round 3

- **No `#include` block on the reference pages.** Simulation C recovered the workflow headers from
  citation strings alone; it remains the most-cited single gap across all nine simulations. Fix B is
  still unstarted.
- **`drive` / `check` / `user_message` remain example-local**, now glossed in the conventions note
  rather than removed.
- **Producing a tool call** — the `ToolCall` literal and `call_provenance::vendor_structured` — is
  still only in `examples/05`, which two simulations had to open for exact literals.
- **`AE_JSON_SCHEMA`'s preconditions** (namespace scope, default-constructible type) are unstated.
- D10–D12 from §3b, and the unlinked `#ownership` entries on how-it-works.

### 9.5 Verification state at the end of round 3

`& .\devsim\verify-all.ps1` → **ALL GATES PASS** (8 gates; 73 claim checks — 11 strings that must be
gone and 62 corrections that must be present — plus encoding integrity across 217 files, 21 pages'
TOC anchors, and two documented programs that compile, link, bind and run).

Two caveats carried forward, both material:

- **The doc revision moved under every simulation.** All three round-3 runs reported edits landing
  mid-run (`apiContent.ts` was rewritten three times during simulation C). Their fallback counts are
  the measurement; their line numbers are not stable, and each report says so. Measuring a moving
  target is a weakness of this instrument, not of the docs.
- **The real-provider step is verified by simulation, not here.** No API key exists in this
  environment and the HTTPS build was not rebuilt; simulation A's live turns used the repo's own
  git-ignored dev key against an Anthropic-compatible endpoint. No turn against `api.anthropic.com`
  is claimed by anyone.

---

## 10. Round 4 — the include blocks, and closing the loop on them

The single most-cited gap across all nine simulations was that **no reference page showed an
`#include`**. Simulation C in round 3 recovered the two workflow headers from citation strings and
compiled first try — it worked, but only because every affected page cites its header by path. A
developer should not have to reverse-engineer that.

### 10.1 What was added

A shared `ApiIncludes` component (`web/marketing/src/components/ApiIncludes.tsx`) renders one
verified `#include` block per page, above the first code panel, with a note saying what each header
is *for*. It is on **14 pages, covering 30 header paths**:

| Page | Headers | Why those |
| --- | --- | --- |
| `first-app` | (already had it, inside the program itself) | the on-ramp |
| `agent` | `core/agent.hpp`, `core/agent_session_bridge.hpp`, `core/chat_client.hpp`, `core/tool.hpp`, `core/content.hpp` | declaration → `make_agent_session` → concept → tool → message |
| `builder` | `core/session_builder.hpp`, `core/chat_client.hpp`, `trust/secret.hpp`, `workflow/graph.hpp`, `workflow/magentic.hpp` | the builder family and what it takes |
| `tool` | `core/tool.hpp`, `core/json_schema.hpp`, `core/tool_call_extraction.hpp`, `core/error.hpp` | the whole tool plane |
| `runtime` | `rt/agent_session.hpp`, `rt/agent_session_core.hpp`, `core/agent_session_bridge.hpp`, `core/chat_client.hpp`, `rt/block_on.hpp` | the session and its vocabulary |
| `workflow` | `workflow/graph.hpp`, `rt/workflow_supervisor.hpp`, `workflow/workflow_event.hpp`, `rt/block_on.hpp`, `core/content.hpp` | data vs. running types, spelled out |
| `hitl` | `core/interaction.hpp`, `core/tool_call_hook.hpp`, `rt/agent_session.hpp`, `rt/agent_session_core.hpp`, `rt/block_on.hpp` | the three decision seams |
| `durability` | `rt/agent_session.hpp`, `rt/agent_session_core.hpp`, `rt/session_store.hpp`, `core/interaction.hpp`, `rt/block_on.hpp` | snapshot, record, store, interaction |
| `providers` | `core/chat_client.hpp`, `trust/secret.hpp`, `core/error.hpp`, `protocol/anthropic/chat_client.hpp`, `protocol/openai/chat_client.hpp` | concept, credential, the two gated backends |
| `skill` | `core/skill_provider.hpp`, `core/context_provider.hpp`, `core/tool.hpp` | the provider, the concept it satisfies, what a skill's tools become |
| `memory` | `core/memory_provider.hpp`, `core/context_provider.hpp` | the provider and the slot-sharing concept |
| `events` | `core/run_event.hpp`, `workflow/workflow_event.hpp`, `rt/agent_session.hpp` | two different streams, and which namespace each is in |
| `streaming` | `core/chat_client.hpp`, `core/stream.hpp`, `core/content.hpp` | the concept, the primitive, the payload |
| `trust-sandbox` | `trust/capability.hpp`, `sandbox/sandbox.hpp`, `rt/agent_session.hpp` | authority, isolation, where a set is installed |
| `plugins` | `plugin/plugin.hpp`, `sandbox/sandbox.hpp` | the manifest and the backend it loads into |

Each note also names the namespace split the page's subject straddles — `agentengine::workflow::`
for graph *data* versus `agentengine::rt::` for the *running* types on the workflow page; the same
boundary on the runtime page; `workflow/` rather than `rt/` for the workflow event header on the
events page — because that is the second-most-cited source of failed builds.

### 10.2 The loop is closed by construction, not by care

A new gate check (`devsim/verify-includes.js` + `devsim/verify-includes/main.cpp`) makes the
advertised paths self-policing:

- Every `*_HEADERS` array in any component is extracted from source.
- Each path in it must appear as an `#include` in `devsim/verify-includes/main.cpp`, a translation
  unit the harness **compiles** under `/W4 /WX`.
- A published path that is not compiled there fails the gate.

So a header cannot be advertised on the site without something proving it still exists, and a
rename in the engine turns into a gate failure rather than a broken instruction. Current state:
**30 published paths, all 30 compiled** — the published set and the compiled set are now identical,
so the check has no slack in either direction.

### 10.3 Verification state at the end of round 4

`& .\devsim\verify-all.ps1` → **ALL GATES PASS** (9 gates: build, lint baseline, snippet fidelity,
88 claim checks — 11 strings that must be gone, 77 corrections that must be present — encoding
integrity across 219 files, 21 pages' TOC anchors, the include-path check above, and two documented
programs that compile, link, bind and run).

Remaining after round 4: `drive`/`check`/`user_message` are still example-local (glossed, not
removed); the `ToolCall` literal for *producing* a call is still only in `examples/05`; the
`AE_JSON_SCHEMA` preconditions are unstated; D10–D12 from §3b; and five pages still carry no include
block (codeact, protocols, builtin-tools, worktree — whose subjects are generated code, wire
payloads, a catalog, and an object model respectively, so their "headers" are less obvious and none
was cited by a simulation).

---

## 11. Round 5 — producing a tool call, and an instrument fix

### 11.1 The last cited gap

Two simulations had to open `examples/05_human_approval.cpp` for exact literals, because **no page
showed how a tool call is produced**. That is the missing half of the tool contract: declaring a
tool is one side, and the other side is what actually arrives to invoke it. It matters
disproportionately because a synthesised `ToolCall` is *how you test a tool-using agent with no
network and no API key* — which is what every example does and what a developer writing their first
offline test needs.

The tool page now carries that recipe (`fakeToolCallSnippet`, from `core/content.hpp:107-119`),
stating the two fields that fail differently (`tool_name` → `tool.unknown_name`; `arguments_json`
validated against the tool's own `Args` schema before `invoke()`), the `provenance` default, and what
`text_derived` costs.

**It is verified by running it.** `devsim/verify-toolcall` builds a client that synthesises a
`ToolCall`, points a real agent at it, and asserts that the declared tool's `invoke()` ran exactly
once and that the value it returned reached the model's second turn as a `ToolResult` — the whole
ten-step pipeline, offline, no key. That check is now a gate.

### 11.2 The instrument was wrong, and the gate caught it

The encoding guard failed on `devsim/rerun7/REPORT.md` — a **sub-agent's scratch report**, not a
file this project maintains. The guard was walking `devsim/` wholesale, so any sub-agent that wrote
a BOM could fail the docs' gate. It now skips simulation scratch and build output and guards only
the doc sources and this directory's own tooling (210 files, down from 220).

Worth recording as a lesson about the instrument rather than the docs: a check that fails on inputs
you do not own is a check that will be ignored, and I would rather find that from a false positive
now than from a red gate everyone learns to skip.

### 11.3 The confirmation run, and the two defects it found in round 4's work

A tenth simulation ran the workflow brief with one added question: *did you have to work out any
`#include` path yourself?* Its answer is the direct test of round 4:

> *"I did NOT have to guess any of the five advertised paths — all correct, all compiled first try,
> from the 'what to include' panel."*

**7 fallbacks, no hard blockers, one compile error in the whole exercise**, cold configure 3.8 s /
30 build statements, and the aggregator instrumented to prove it was invoked exactly once per
fan-in round. Its verdict: buildable from the docs alone, ~45 min to a clean build.

It also found two real defects, both in round 4's own additions, and both fixed here:

| Defect | Reality | Fix |
| --- | --- | --- |
| The include panel omitted `text_of` — used by *every* body snippet on the workflow page — so its header had to be retrieved from a different page's collapsed note | `text_of` lives in `core/tool_call_extraction.hpp`, which was not among the five advertised headers | Added to `WORKFLOW_HEADERS` (now six), and named in the note |
| The `ExecutorBody` sentence advertised the `Message` → `ExecutorOutcome` convenience without saying where it stops applying | A routing body annotated `result<Message>` **does not compile** (`C2440`): the convenience sits inside `std::expected`'s converting constructor, and two user-defined conversions do not chain | The sentence now separates the pass-through shape from the routing shape and names the failure |

The second is the sharper one: it was the **only build failure** in that simulation, and the docs'
own routing example silently used the correct form, so the prose leaned a reader the wrong way. Both
shapes are now compiled by `devsim/verify-bodyforms`, which asserts they coexist in one graph and
that the router's own route is honoured.

A third finding — that `text_of()` concatenates a fan-in delivery's items with no separator, gluing
branches together silently — is now stated in the shared conventions note, since `text_of` appears
on pages that have no fan-in context to hang it off.

### 11.4 The last named "unstated" gap

`AE_JSON_SCHEMA`'s preconditions were cited across rounds as unstated. They are now on the tool page
as four rules, each of which previously failed with a message that did not name the rule: it must sit
where ADL finds it for the type (its own namespace, not inside a function); it takes up to 16
members; every field type must be one the table lists, an `optional`/`vector` of one, or a nested
type with its own annotation; and **"required" is the field's own type** — a missing key is an error
unless the field is `std::optional<U>`, which is therefore the one way to make a parameter optional.

### 11.5 Verification state at the end of round 5

`& .\devsim\verify-all.ps1` → **ALL GATES PASS** (11 gates: build, lint baseline, snippet fidelity,
97 claim checks — 11 strings that must be gone, 86 corrections that must be present — encoding
integrity across 210 source files, 21 pages' TOC anchors, 30 compile-verified include paths, and
**four** documented programs that compile, link, bind and run: the first program, the tool+agent
declaration, the tool-call recipe, and both `ExecutorBody` shapes).

Remaining, all polish rather than obstacles: `drive`/`check`/`user_message` are example-local by
design (glossed in the conventions note, and every simulation that hit them wrote three-line
replacements); D10–D12 from §3b; and five pages carry no include block.



