# devsim/p2 — building a refund-approval service on AgentEngine

**Product.** A back-office service that issues refunds. `refund_order` is approval-gated; a refund
above a threshold must be answered by a human; a pending approval has to survive the process dying
and stay attributable to the same principal; and a suspended/refused run has to be branched on in
code with the engine's own error code printed.

**What "the docs" means below.** `web/marketing/api/*.html` are ~1 KB Vite shells — opening one
gives you a `<script src>` and nothing else. All prose and every snippet live in
`web/marketing/src/data/*.ts` and `web/marketing/src/components/Api*Reference.tsx`. I read those.
Citations are `file:line` into that source.

**Citation hygiene.** The docs source is being edited in this shared workspace while I worked: `apiContent.ts` went from 4,687 to 4,728 lines mid-session (+41, everything from `:602` on shifted). Every citation below was re-verified against the current revision at the end of the run; `apiContent.ts` numbers are the post-shift ones.

**Result.** `main.cpp` builds (MSVC 19.51 / VS 18 2026, C++23, CMake 4.3.2) and all three modes run
clean:

```
refund_approval_demo --suspend    C:\Users\thanh\ae-p2-store   # exit 0
refund_approval_demo --reload     C:\Users\thanh\ae-p2-store   # exit 0  (separate process)
refund_approval_demo --lifecycle  C:\Users\thanh\ae-p2-store   # exit 0
```

> Note on page coverage: this task pointed me at `hitl.html`, `durability.html`, `trust-sandbox.html`,
> `runtime.html`, `pizza-order.html`. The page that answers *"how do I depend on the engine and build
> against it"* is **`api/first-app.html`** (`ApiFirstAppReference.tsx`), which was not on that list and
> which I found late. Where a question is answered there, I say so. Its absence from the list is not a
> docs failure; its being the only place that answer exists is a navigation failure worth one line.

---

## Getting started

The exact sequence I had to read, per question, and whether it answered me.

### (a) Include the engine — **found in docs**

`ApiFirstAppReference.tsx:80-94` (`SNIPPET_CMAKE`), prose at `:136-138`:

```
add_subdirectory(third_party/AgentEngine agentengine)
target_link_libraries(myapp PRIVATE agentengine::core)
```

> "There is no `find_package(AgentEngine)` and no versioned release to install: the engine ships as
> a repository you vendor (or add as a submodule). You add it as a subdirectory and link one target,
> `agentengine::core`."

Same page, `:86-87`, gives the two cache variables that must be forced **before**
`add_subdirectory()` (`AGENTENGINE_BUILD_TESTS`, `AGENTENGINE_BUILD_EXAMPLES`), with the reason
(a `plain set()` is shadowed by the engine's own `option()`) and a measured cost (2,336 → 159 build
statements). My `CMakeLists.txt` is this snippet. Confirmed real: `AgentEngine/CMakeLists.txt:92-103`
defines the `INTERFACE` target and the `agentengine::core` alias, and there is no `install(EXPORT)`
anywhere in that file, so `find_package` genuinely is not an option.

*One gap inside (a):* the page says the engine "fetches its vendored dependencies over the network"
(`:145`) but never says how to avoid it. That is a separate fallback below.

### (b) Construct a session — **found in docs**, but there are two surfaces and the pages don't cross-reference

- Declarative: `ApiFirstAppReference.tsx:59-67` — `auto agent = make_agent_session<Greeter>(HelloClient{});`
  plus `:52-55` the `Agent<Derived, ChatClientId<...>, MaxTurns<N>>` declaration.
- Raw: `apiContent.ts:1180` names the type (`AgentSession<ChatClientT, StateT, HistoryProviderT>`),
  `apiContent.ts:624-628` (`multiTurnSnippet`) shows construction:
  `using JokeAgent = agentengine::rt::AgentSession<JokerChatClient>; session.initialize("s-multi-turn",
  Principal{"p-demo", ""}); session.emplace_chat_client();`
- `apiContent.ts:1504-1506` (composed provider) shows the same three-arg form.

`make_agent_session` has no `set_suspend_for_approval`/`set_capabilities`/snapshot surface anywhere in
the docs, so for this product the raw `rt::AgentSession` is the only usable construction path — a fact
the docs never state. Fallback below.

### (c) Declare an approval-gated tool — **found in docs**

`apiContent.ts:1605-1611` (`approvalExampleSnippet`, "examples/05_human_approval.cpp (trimmed) -- ADR-029"):

```cpp
struct SendMessageTool
    : Tool<SendMessageTool, Capabilities<>, EffectClass<effect_class::pure>,
           Approval<approval_mode::always_require>> { ... };
```

`apiContent.ts:688-692` (`toolStaticMembers`) documents `declared_approval()` defaulting to
`never_require` ("Opt-in only"), and `:653-657` documents `declared_effect_class()` defaulting to
`at_most_once`. `ApiPizzaOrderReference.tsx:76-84` shows a *refund* tool with
`Approval<approval_mode::policy_driven>`. My tool is the verbatim `always_require` shape.

### (d) Make a run suspend, then resume it — **found in docs**

`apiContent.ts:1613-1624`:

```cpp
session.set_suspend_for_approval(true);   // no decider configured -> genuinely suspend, don't hang
auto r1 = drive(session.start_run(StartRun{user_message(...)}));
// r1 has NO value -- start_run() fails closed. ... session.has_open_interactions() == true.
std::string const id = session.open_interactions().front().interaction_id;
auto r2 = drive(session.resolve_interaction(ResolveInteraction{id, /*approved=*/true, std::nullopt}));
```

`apiContent.ts:1233` (runtime page, `suspend-for-approval` entry) adds the semantics: the same
`run_id`, `approved=false` folds a denial in as an ordinary tool error, a second `StartRun` is
refused while one is open, and a resolve from a foreign principal is denied at admission.
`hitlContent.ts:90-134` gives the resume call per mechanism (`resolve_interaction({id, approved})`).
`ApiPizzaOrderReference.tsx:132-146` is the same flow for a refund. This is the best-covered part of
the docs.

### (e) Snapshot and reload the session — **found in docs**, with one wrong snippet

- `durabilityContent.ts:653-687` (`checkpointSnippet`): the whole `AgentSessionRecord` field list,
  `snapshot_record()`, `CheckpointCadence<N>`, and `InMemorySessionStore store; // or FileSessionStore{root}`.
- `durabilityContent.ts:790-814` (`checkpointRoundTripSnippet`): `save_agent_session_snapshot`,
  `load_agent_session_snapshot(store, "s-checkpoint")`, `fresh_session.restore_from_record(*loaded)`,
  and the named gap `fresh_session.history().empty()`.
- `ApiPizzaOrderReference.tsx:148-172`: the same, plus the correction that
  `load_*` returns `result<optional<record>>` ("TWO ways to have nothing").
- `durabilityContent.ts:286-313` (`checkpointFields`) is the authoritative carried/not-carried table —
  this is what told me a pending approval survives and history does not.

*The one wrong snippet:* `durabilityContent.ts:714` writes
`auto found = load_agent_session_snapshot(store, "s-1");   // -> std::nullopt`, which implies a bare
optional. The real signature is `result<std::optional<AgentSessionRecord>>`
(`include/agentengine/rt/agent_session.hpp:556-566`). The pizza page has it right; the durability
page — the page that owns the topic — does not.

### (f) Handle errors — **partly found in docs**

Found: `ApiFirstAppReference.tsx:222-229` ("Fallible calls return `result<T>`. You check
`has_value()`, and on failure read `error().code` — a stable string you can branch on — and
`error().message`"), and its runnable snippet `:63-74` does exactly that. `apiContent.ts:1424`
(`run.admission_denied`), `durabilityContent.ts:502` (`run.approval_pending`), `:527`
(`run.durable_ack_failed`), `:402/440` (`effect.reexecution_requires_ack`) name real codes in prose.
`durabilityContent.ts:89` names the suspension sentinel: *"`run_rounds()` returns when it suspends
(with the `kSuspendedForApproval` sentinel)"*.

Not found (fallen back): that `kSuspendedForApproval` is a `static constexpr` **member of the session
class template** and how to compare against it; the `error` object's third field (`klass`) and the
`failure_class_to_string()` helper; and the error code for answering a restored interaction
(`session.resolve_interaction.round_not_recorded`), which appears **zero** times in the docs.

### (g) Build/link — **found in docs**

`ApiFirstAppReference.tsx:183-197`: `cmake -S . -B build -G Ninja`, `cmake --build build --target myapp
-j 4`, "what you should see", the statement that the first configure compiles nine engine static
libraries, and the `CMAKE_OBJECT_PATH_MAX` / keep-the-build-directory-shallow note (`:153-162`) — which
is real: I put the build directory at `C:\Users\thanh\ae-p2-build` and never saw the warning.
(`content.ts:325-348` is the *contributor* build, `ctest` included, which is a different thing.)

---

## Fallback log

Every time I left the docs. This is the metric.

1. **Offline Asio.** The docs say dependencies are fetched over the network; they never say how to
   avoid it. → `AgentEngine/CMakeLists.txt:187-209` (`AGENTENGINE_VENDOR_ASIO` /
   `AGENTENGINE_ASIO_ROOT`), then reused an already-fetched copy from the engine's own `build-dev`.
2. **`AGENTENGINE_FAST_TESTS`.** Not a documented option. → `AgentEngine/CMakeLists.txt:66`.
3. **Toolchain/generator identity.** `content.ts:331` says "MSVC 19.4x"; the machine has MSVC 19.51
   under VS 18 2026, and the generator name isn't in the docs. → inspected `AgentEngine/build/CMakeCache.txt`.
4. **`FileSessionStore`'s home and constructor.** Docs show `FileSessionStore{root}` and
   `FileSessionStore store(root)` (inconsistent) but never the header or the parameter type.
   → `include/agentengine/rt/session_store.hpp:195-203` (`explicit …(std::filesystem::path)`, creates the dir).
5. **Session-id constraints on a file store.** Ids containing `/`, `\` or `..` are refused outright and
   Windows-reserved characters are percent-escaped. Undocumented; would silently break an id like
   `tenant/4711`. → `session_store.hpp:304-343`.
6. **`load_agent_session_snapshot`'s real return type** (see (e)). → `agent_session.hpp:556-566`.
7. **`save_agent_session_snapshot` is a coroutine** (`task<result<void>>`) and therefore needs a task
   driver. Docs call `drive(...)` around it and never define `drive`. → `agent_session.hpp:544-549`
   plus every example's own copy.
8. **`set_capabilities()` does not own the set.** It stores a bare non-owning pointer
   (`agent_session_core.hpp:484-487`); a `CapabilitySet` declared local to a wiring function dangles,
   compiles clean, and crashes with `0xC0000005` on the first `start_run()`. Measured. Documented only
   in a header comment; the durability page even repeats "capabilities_ is a bare host-owned pointer"
   (`durabilityContent.ts:297`) without it being any kind of warning.
9. **`kSuspendedForApproval`'s qualification.** Docs: "the `kSuspendedForApproval` sentinel"
   (`durabilityContent.ts:89`). Reality: `AgentSession<...>::kSuspendedForApproval`, a static member of
   the class template. → `agent_session_core.hpp:1208-1216`.
10. **`error`'s field names and `failure_class_to_string`.** Docs show `.code` and `.message`; I print
    `.klass` too. → `include/agentengine/core/error.hpp:78-82` and `:32`.
11. **`result<T>` is `std::expected<T, error>`** (`.has_value()/.error()/.value()`), and
    `agentengine::rt::AgentResponse` is not reachable through `using namespace agentengine;` — a
    nested-namespace name needs its own `using`. This cost me one build with a misleading MSVC
    `error C2664 … 'classify(const <error type> &)'`. → `error.hpp:91-92`, `agent_session_core.hpp:168`.
12. **The `ChatClient` concept's exact surface** (`capabilities()`, `chat()`, `chat_stream()`,
    `static_assert(ChatClient<T>)`) — *now* documented in full by `ApiFirstAppReference.tsx:25-49`;
    I read `examples/05_human_approval.cpp:89-136` first, before I found that page.
13. **`ContextProvider` as the only way a session gets tools.** The pizza page shows the shape
    (`ApiPizzaOrderReference.tsx:88-97`); the concept's name, its two required method signatures, and
    `ToolTable::from_tools<Ts...>().descriptors()` are not in the docs. →
    `examples/05_human_approval.cpp:77-87`, `include/agentengine/core/tool_descriptor.hpp:319`.
14. **`user_message(...)`** — used in 20 doc snippets, defined in none. → `examples/05:138-146`.
15. **`check(...)`** — used in the pizza page's snippet; `durabilityContent.ts:149` acknowledges a
    *file-local* `check(cond, label)` for tests and never defines it for snippets. → `examples/05:40-47`.
16. **`drive(...)`** — used in 33 doc snippets, defined in none of them and in no header. Every one of
    the 35 examples defines its own copy. → `examples/05:152-156`.
17. **`text_of(...)` / `text_message(...)`** — used in doc snippets, undefined there. →
    `include/agentengine/core/tool_call_extraction.hpp:83`.
18. **The threshold itself.** The pizza page declares `refund_order` `policy_driven` "because whether a
    refund needs a human depends on how much it is" (`ApiPizzaOrderReference.tsx:74-78`) and then shows
    no decider, no threshold, and no amount. The ADR-070 seam it points at (`PolicyDecider`) receives
    `(Principal, ToolDescriptor, bool arguments_tainted)` — **no arguments** — so an amount threshold is
    inexpressible through it (`core/tool_pipeline.hpp:168-171`); and setting an `ApprovalDecider`
    (the one decider that *does* see the arguments) switches suspension off entirely
    (`agent_session_core.hpp:1272-1274`, `approval_waits_for_human()`). I implemented the threshold
    host-side and say so in the code.
19. **Answering a restored interaction.** Not in the docs at all. The code refuses with
    `session.resolve_interaction.round_not_recorded`. → `src/rt/agent_session_core.cpp:423-432`
    (ADR-196 §7). See Blockers #2 — two doc pages claim the opposite.
20. **`interaction_reason` → printable name.** The six enumerators are documented
    (`hitlContent.ts:64`); no mapping helper exists anywhere in the engine. → wrote my own 12-line switch.
21. **`Interaction`'s fields** are documented (`hitlContent.ts:66-72`, `durabilityContent.ts:116`) —
    listed for completeness: this one is *not* a fallback.

---

## Undefined identifiers

For the doc snippets I actually relied on, every identifier they use without defining it. "Nowhere"
means: not in any doc snippet, not in the page's prose, and not in any header or example — the reader
must invent it.

| Identifier | Where it is used in the docs | Where it actually comes from |
|---|---|---|
| `drive(...)` | `apiContent.ts:1616`, `:1581`, `:1602`; `ApiPizzaOrderReference.tsx:112,134,142,149`; `durabilityContent.ts:798`; **33 uses total** | **Nowhere in the docs, nowhere in the engine.** Every one of the 35 examples defines a file-local `template <class T> T drive(agentengine::rt::task<T>)` (`examples/05_human_approval.cpp:152-156`, `examples/12_session_checkpoint.cpp:93-97`). Not exported, not in a support header. |
| `check(cond, label)` | `ApiPizzaOrderReference.tsx:149,158,162,165`; **37 uses total** | **Nowhere in the docs** for snippets; `durabilityContent.ts:149` calls it "a file-local `check(cond, label)`" when describing *tests*, and never defines it. Each example rolls its own (`examples/05:40-47`). |
| `user_message(text)` | `apiContent.ts:1616`, `:1596`; `ApiPizzaOrderReference.tsx:112,126,134`; **20 uses total** | **Nowhere in the docs.** File-local in every example (`examples/05:138-146`, `examples/12:80-88`). |
| `Session` (as a type alias) | `apiContent.ts:1613`; `durabilityContent.ts:792` (`using Session = AgentSession<CannedChatClient>;`), `:805` | Defined *inside the snippet* in two places, undefined in `apiContent.ts:1613` where it is used bare. Each example declares its own local alias (`examples/12:102`). There is no engine-level `Session` type. |
| `CannedChatClient`, `JokerChatClient`, `ScriptedSendChatClient`, `PizzaPalaceChatClient`, `HelloClient` | `apiContent.ts:619,625`; `durabilityContent.ts:792`; `ApiPizzaOrderReference.tsx:99` | Test/example-local fakes. `HelloClient` is the only one defined *in a doc* (`ApiFirstAppReference.tsx:25-49`). The rest are named and never shown. |
| `held` | — | **`held` never appears as an identifier in any doc snippet.** The only occurrence is a diagram *node label*, `"CapabilitySet held"` (`ApiHowItWorksReference.tsx:124`). The real identifier is `CapabilitySet const held = CapabilitySet::grant_root({});` in `examples/05:164` and `examples/06:48`. |
| `text_of(...)` | `hitlContent.ts:248,273` | `include/agentengine/core/tool_call_extraction.hpp:83`. Never defined in the docs. |
| `text_message(...)` | `hitlContent.ts:264,270`; `durabilityContent.ts:827` | A *workflow* message builder, distinct from `user_message`; also file-local per example (`examples/04_first_workflow.cpp`). Never defined in the docs. |
| `make_bodies()` | `hitlContent.ts:262` | Example-local. Never defined. |
| `EffectContext`, `ContextContribution`, `SessionContext`, `TurnView`, `ToolTable`, `ToolDescriptor`, `CapabilitySet`, `Principal`, `Usage`, `ChatRequest`, `ChatResponse` | throughout the snippets | Real engine types, but no doc snippet shows the `#include` that brings any of them in, and the first-app page only includes four headers. I derived the include list from `examples/05`/`examples/12`. |
| `failure_class_to_string`, `error::klass`, `error::native_code` | nowhere | `include/agentengine/core/error.hpp:32,78-88`. The docs show `.code`/`.message` only. |
| `AgentResponse` | implied by `r1->message` (`apiContent.ts:1624`) | `agentengine::rt::AgentResponse` (`agent_session_core.hpp:168`). Not named in the docs, and not reachable via `using namespace agentengine;`. |
| `kSuspendedForApproval` | `durabilityContent.ts:89,201` | `AgentSession<...>::kSuspendedForApproval` (`agent_session_core.hpp:1216`). Docs never say where it lives or that it is a class-static. |
| `session.resolve_interaction.round_not_recorded` | **nowhere** | `src/rt/agent_session_core.cpp:431`. |
| `FileSessionStore` | `durabilityContent.ts:680,821` | `rt/session_store.hpp:195`. Header never named; constructor parameter type never shown. |

The three the task called out are the three worst, and they are the same failure: **the doc snippets
are transcribed from files whose helper prelude is not part of the transcription.** `drive`, `check`
and `user_message` are the entire scaffolding a reader needs to run any snippet, and none of the three
is defined in the docs, in a shared header, or in a `tests/support/` header a consumer could include.
A reader who copies the approval snippet verbatim gets a program that does not compile until they
invent those three helpers themselves.

---

## Blockers, ranked

1. **`set_capabilities()` takes a non-owning pointer, and nothing warns you.** My first build
   compiled clean and died with `0xC0000005` before printing a single line, because the
   `CapabilitySet` was a local of the wiring function. There is no compiler diagnostic, no runtime
   error — just an access violation on the first `start_run()`. Cost: the single largest slice of
   time, because "no output at all" points at static initialisation, not at a dangling capability set.
   Fixing it correctly is one word (`static`), and the fact is stated in a header comment
   (`agent_session_core.hpp:484-487`) that no doc page surfaces. **This is the one thing I would fix
   first in the docs.**
2. **The docs contradict themselves about resuming after a restart, and the code agrees with the
   smaller claim.** `ApiHitlReference.tsx:207-213` (the HITL page's "Surviving a restart" section):
   *"A suspended run — approval, `codeact_ask`, `hook_decision`, or a workflow's own `request_port` —
   can be resumed after a genuine process restart, not just later in the same process."*
   `ApiPizzaOrderReference.tsx:167-169`: the pending refund is *"still answerable after the restart"*.
   `durabilityContent.ts:119`: *"A restored session therefore knows an approval was pending and cannot
   resume it — ADR-029 named this residual itself."* The code: `SuspendedRoundRecord` is cleared by
   `restore_from_record` (`src/rt/agent_session_core.cpp:805`) and never repopulated, so
   `resolve_interaction` on a restored interaction always fails with
   `session.resolve_interaction.round_not_recorded` (`:423-432`) and closes it with nothing run.
   The engine is right and the HITL and pizza pages are wrong. I only settled it by building the
   thing and watching it refuse — which means requirement 4, read as *"reload and resume"*, is
   **impossible**, while requirement 4 read as written (*"show it is still pending and attributable"*)
   is fine. I did not discover this from the docs; I discovered it *against* them.
3. **The refund threshold has no home in the engine, and the docs' own refund example pretends it
   does.** `policy_driven` is documented as "whether a refund needs a human depends on how much it
   is, which is the host's policy to state" (`ApiPizzaOrderReference.tsx:74-78`) — but the
   `PolicyDecider` seam receives no arguments, and installing the one decider that *does* see the
   arguments (`ApprovalDecider`) turns suspension off. So an amount-based approval rule cannot be
   expressed through the mechanism the page names. I enforce the threshold in the host's approval
   console and labelled it as host policy in the code. A truly engine-native threshold needs the
   documented `ToolCallHook` (a *hook_decision* interaction, a different reason tag) or a tool
   declared `always_require` with the threshold applied outside — neither of which the pizza page
   mentions.
4. **`agentengine::rt::` names are not reachable through `using namespace agentengine;`.** This
   produced MSVC `error C2664: … 'classify(const <error type> &)'` — an unresolvable *parameter type*
   reported at three call sites, with no "undeclared identifier" to point at the real cause. Two build
   cycles.
5. **Consumer build wiring beyond `add_subdirectory`.** The two forced options are documented
   (first-app page); the network fetch they do *not* remove, the `AGENTENGINE_ASIO_ROOT` escape hatch,
   and the fact that this is the only offline path are not. Without the local Asio copy, a consumer
   configure needs network access.
6. **`/W4 /WX` leaks into the consumer via `agentengine_warnings`.** All 35 examples link it; the
   first-app page's CMake does not. A consumer who follows the examples inherits warnings-as-errors
   for their own code. I linked it (and am clean), but a reader cannot tell which of the two
   documented wirings is intended.

Not a blocker, but worth recording: **the crate of "small" mismatches.** `FileSessionStore{root}` vs
`FileSessionStore store(root)` on adjacent lines (`durabilityContent.ts:680` vs `:821`);
`load_agent_session_snapshot` documented as returning an optional on the page that owns it and
correctly on the page that doesn't; `content.ts:331` claiming MSVC 19.4x against an installed 19.51.
Individually trivial; collectively they mean every snippet has to be re-verified against the tree
before it compiles, which is exactly what a "verbatim from the repo, never invented" convention is
supposed to save you from.

---

## Confidence

| # | Requirement | Confidence | Basis |
|---|---|---|---|
| 1 | Declare an agent with a `refund_order` tool gated behind human approval | **certain** | `Tool<…, Approval<always_require>>` documented verbatim (`apiContent.ts:1605-1611`); builds and runs. |
| 2 | Configure the agent so an approval-requiring call suspends rather than proceeding | **certain** | `set_suspend_for_approval(true)` documented (`apiContent.ts:1614`); `--lifecycle`/`--suspend` show `invoke()` never reached before the answer (`ok: refund_order::invoke() did NOT run`). |
| 3 | Full suspend → interaction id → human answers → resume | **certain** | `--lifecycle`; run id preserved, interaction closed, tool ran only after approval, denial folded as a tool error. |
| 4 | Durable save, fresh-process reload, still pending + same principal | **certain** for what is written; **could not determine** if "reload and then resume" was intended | Two real processes (`--suspend` then `--reload`) plus the raw 342-byte record on disk show `open_interactions[0].reason == "approval"`, `principal_id == "refund-ops-agent"`, `run_id` re-derived identically. Answering it after reload fails with `session.resolve_interaction.round_not_recorded` — documented nowhere, contradicted by two doc pages (Blocker 2). |
| 5 | Branch on refusal/suspension in code, print the engine's error code | **certain** | Three real branches (`classify()`, on `error().code`), two codes printed for real: `run.suspended_for_approval` and `session.resolve_interaction.round_not_recorded`. No comment-only handling anywhere. |
| 6 | Buildable and runnable by me | **certain** | Configured and built with CMake 4.3.2 + VS 18 2026 / MSVC 19.51 (C++23), engine built from source as part of the consumer project, all three modes exit 0 with all internal checks passing. |

---

## Verdict

**Could a developer build *this* product from the docs alone? Not this one — and the reason is narrow
and fixable.** The mechanical half is genuinely well documented now: how to depend on the engine, how
to write a `ChatClient`, how to declare an approval-gated tool, how a run suspends and how an
interaction is answered are all on the page (the first-app page's CMake and whole-program snippet are
better than most commercial SDK docs, and the approval flow in `apiContent.ts` is exact). What is
missing is everything *around* those snippets: `drive`, `check` and `user_message` are used in 90
places and defined in none, so a copied snippet does not compile; the one ownership footgun that
turns a correct-looking wiring into a segfault is in a header comment; the durable-reload story is
told three different ways by three pages and the code agrees with the one page nobody would read first;
and the docs' own refund example asserts a per-amount policy for a tool whose declared policy seam
structurally cannot see an amount. A strong developer with the source open can build this in a day —
I did — but the docs did not carry them; for the HITL/durability intersection they actively misled, and
the single most valuable output of this exercise is that **answering a restored interaction is the
product's hardest requirement, it does not work, and two published pages say it does.**

---

## Evidence

`--suspend` (process A), then `--reload` (process B), abbreviated:

```
=== --suspend: process A, a 48000-cent refund, no human on shift ===
  starting run: refund_order(88231, 48000c)
  [start_run] engine verdict: SUSPENDED
         failure_class = contract
         code          = run.suspended_for_approval
         message       = round suspended awaiting human approval
  [suspended] open interactions: 1
         interaction_id = refund-svc-4711:interaction:1
         run_id         = refund-svc-4711:run:1
         reason         = approval
  approval surface: no human has answered yet -- leaving the run SUSPENDED and the interaction open
  store now holds a record for 'refund-svc-4711' with a still-open approval

=== --reload: a fresh process, nothing in memory, only the store on disk ===
    principal_id        = refund-ops-agent
    principal_tenant_id = tenant-backoffice
    open_interactions   = 1
    -> interaction_id   = refund-svc-4711:interaction:1
    -> reason           = approval
  a human answers interaction refund-svc-4711:interaction:1: approved
  [resolve_interaction (after restart)] engine verdict: REFUSED
         failure_class = contract
         code          = session.resolve_interaction.round_not_recorded
         message       = this interaction has no recorded round in this session (restored or
                         replaced) -- closed, nothing ran
```

The durable record as it sits on disk (`<store>/refund-svc-4711`, 342 bytes, verbatim):

```json
{"session_id":"refund-svc-4711","principal_id":"refund-ops-agent",
 "principal_tenant_id":"tenant-backoffice","deleted":false,"run_counter":1,"turn_index":0,
 "open_interactions":[{"interaction_id":"refund-svc-4711:interaction:1","run_id":"refund-svc-4711:run:1",
 "reason":"approval","opened_at_ns":0,"expires_at_ns":0}],"require_authority":false}
```

`--lifecycle` (per-act evidence that the gate held, and that the denial path is distinct):

```
act 1: 1200c  -> SUSPENDED -> host threshold rule answers -> converged -> refund journal: refund 1200c
act 2: 48000c -> SUSPENDED -> a HUMAN approves           -> converged -> refund journal: refund 48000c
act 3: 91000c -> SUSPENDED -> a HUMAN denies             -> converged -> refund journal: (empty)
```

Note act 1: the engine **still suspends** for a refund under the threshold. The engine has no
threshold; the service applies its own policy to the question the engine asks. That is stated in the
code and in Blocker 3.

## Build, verbatim

```
cmake -S C:\Users\thanh\Git\AgentEngine\devsim\p2 -B C:\Users\thanh\ae-p2-build -G "Visual Studio 18 2026" -A x64
cmake --build C:\Users\thanh\ae-p2-build --config Release -j 4
C:\Users\thanh\ae-p2-build\Release\refund_approval_demo.exe --suspend   C:\Users\thanh\ae-p2-store
C:\Users\thanh\ae-p2-build\Release\refund_approval_demo.exe --reload    C:\Users\thanh\ae-p2-store
C:\Users\thanh\ae-p2-build\Release\refund_approval_demo.exe --lifecycle C:\Users\thanh\ae-p2-store
```

(Generator name per `AgentEngine/build/CMakeCache.txt`; the docs' `-G Ninja` assumes Ninja on `PATH`,
which this machine does not have.)
