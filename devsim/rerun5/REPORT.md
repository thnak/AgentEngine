# rerun5 — a back-office refund service on AgentEngine

**Product.** A refund service: one agent, one `refund_order` tool gated behind human approval, a
refund above a threshold that must be answered by a human, and a durable session snapshot that a
**fresh process** reloads.

**Deliverables in this directory**

| file | what |
| --- | --- |
| `main.cpp` | the whole program (four modes, one translation unit) |
| `CMakeLists.txt` | the consumer build |
| `run-output.txt` | the exact captured output of all four modes, from the final binary |
| `REPORT.md` | this document |

**Build & run (verified).** Windows 11 x64, MSVC 19.51.36260.0 (`cl.exe` from VS 18 Community),
CMake 4.3.2, Ninja from the VS install, C++23, Release. Build directory deliberately **out of tree**
at `C:\ae-r5-build` (the docs' own `CMAKE_OBJECT_PATH_MAX` warning: a nested in-repo build directory
trips Windows' 250-character object-path cap).

```
cmake -S devsim/rerun5 -B C:\ae-r5-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build C:\ae-r5-build --target rerun5_refunds -j 8
C:\ae-r5-build\rerun5_refunds.exe --lifecycle      # suspend -> id -> human -> resume (+ thresholds, denials)
C:\ae-r5-build\rerun5_refunds.exe --phase1 <dir>   # suspend, snapshot, exit
C:\ae-r5-build\rerun5_refunds.exe --phase2 <dir>   # FRESH process: reload, show what is pending, branch on the failure
C:\ae-r5-build\rerun5_refunds.exe --decider        # what the ApprovalDecider seam can and cannot express
```

Verified results: configure **2.8 s**; build **30 steps** (exactly the number
`ApiFirstAppReference.tsx:253-261` predicts), **first attempt, zero compiler diagnostics**, no engine
file touched. Nothing under the engine's own `include/`, `src/`, `examples/` or `web/` was modified;
`git status` shows the same pre-existing modifications it showed before this work started, plus
`devsim/` as untracked.

---

## Getting started — found in docs, or reverse-engineered

Legend: **[docs]** = the docs contained what was needed. Anything they did not is a numbered entry in
the Fallback log below, cross-referenced by number.

### (a) Include the engine — **[docs]**

`web/marketing/src/components/ApiFirstAppReference.tsx` is the on-ramp and is complete for this:

- `:80-114` — the whole `CMakeLists.txt`: `cmake_minimum_required(VERSION 3.28)`, the two cache
  variables (`AGENTENGINE_BUILD_TESTS` / `AGENTENGINE_BUILD_EXAMPLES` = `OFF … FORCE`) set **before**
  `add_subdirectory()`, `AGENTENGINE_ROOT` held in a variable, and `target_link_libraries(myapp PRIVATE
  agentengine::core)`.
- `:100-107` — `add_executable` + the one link target, and the explicit warning **not** to copy the
  engine's own `agentengine_warnings` gate onto consumer code. I did not.
- `:86-92` — the `AGENTENGINE_VENDOR_ASIO OFF` + `AGENTENGINE_ASIO_ROOT` escape hatch "only if you
  cannot fetch at configure time"; `:219-227` — the shallow-build-directory warning I followed.
- Program includes: `ApiFirstAppReference.tsx:11-18` and the pizza preamble
  `ApiPizzaOrderReference.tsx:51-63` between them name `agentengine/core/agent.hpp`,
  `…/agent_session_bridge.hpp`, `…/chat_client.hpp`, `…/content.hpp`, `…/json_schema.hpp`,
  `…/tool.hpp`, `…/tool_call_extraction.hpp`, `agentengine/rt/agent_session.hpp`,
  `agentengine/trust/principal.hpp`. `rt/block_on.hpp` is named by `ApiSnippetConventions.tsx:17`.
  `rt/session_store.hpp`, `core/interaction.hpp` and `rt/agent_session.hpp` are named as file:line
  citations in `durabilityContent.ts:120,292,632`. The one header the docs never name is
  `core/json_value.hpp` — needed to read a suspended call's arguments (Fallback 4).

`agentengine::core` alone was sufficient for everything below — no `provider_http_client`, no
`AGENTENGINE_WITH_HTTPS`, because the client here is offline. That is exactly the split
`ApiFirstAppReference.tsx:109-114` and `:265-295` describe.

### (b) Construct a session — **[docs]**

- `ApiPizzaOrderReference.tsx:140-151` — `using OrderAgent = AgentSession<PizzaPalaceChatClient,
  NoSessionState, OrderHistoryProvider>;` / `session.initialize("sess-4711",
  Principal{"customer-4711", "tenant-pizzapalace"});` / `session.emplace_chat_client();` /
  `CapabilitySet const allowed = …; session.set_capabilities(&allowed);`
- `ApiFirstAppReference.tsx:59-78` + `:144-149` — the other road: `AgentSessionOptions` with
  `session_id` / `principal` / `chat_client_grants`, then `make_agent_session<A>(client, opts)`.
- `ApiAgentReference.tsx:129-131`, `apiContent.ts:611-619` — `make_agent_session<A>()` runs
  `register_agent<A>()`'s validation and binds the declaration to a real `rt::AgentSession`.
- `ApiFirstAppReference.tsx:177-193` — `Bundle::session()` is the documented way to reach that raw
  session, and the page closes the loop itself: `auto built = make_agent_session<OrderAssistant>(client,
  opts);` … `agentengine::rt::AgentSession<RealClient>& session = built->session();` …
  `session.set_suspend_for_approval(true);   // now the HITL page's API applies to this session`.
  **I reverse-engineered this before finding that paragraph; it is not a documentation failure** (see
  the note at the end of the Fallback log).
- Session construction uses `make_agent_session<RefundsAgent>(ScriptedRefundClient{…},
  base_options("sess-refunds-9001"))`, then `bound->session()`.

### (c) Declare an approval-gated tool — **[docs]**

- `ApiPizzaOrderReference.tsx:104-125` — the exact shape, policy tags in the documented order:
  `Tool<RefundTool, Capabilities<cap::decl::NetOut<…>>, EffectClass<effect_class::at_most_once>,
  Approval<approval_mode::always_require>>` with `static name`, `static description`,
  `using Args`, `using Reply`, `static result<Reply> invoke(Args, EffectContext&)`.
  `apiContent.ts:1605-1611` (`approvalExampleSnippet`) is the same declaration.
- `ApiPizzaOrderReference.tsx:127-138` — where tools reach a session: a `ContextProvider`'s
  `on_context()` returning `ToolTable::from_tools<…>().descriptors()`. I did **not** need this,
  because the agent-declaration road binds `Tools<...>` for me (`apiContent.ts:604-619`,
  `ApiFirstAppReference.tsx:163-170`).
- `apiContent.ts:752` — `minimalToolSnippet`: the five required members.
- `AE_JSON_SCHEMA(T, field, …)` — named and used on `ApiFirstAppReference.tsx:158-159`; the exact
  spelling of a declaration is in `examples/05_human_approval.cpp:51-54`, which the docs cite as the
  ground truth for this mechanism.

My declaration is `RefundOrderTool` (`Capabilities<>`, `effect_class::at_most_once`,
`Approval<approval_mode::always_require>`) inside `RefundsAgent : Agent<RefundsAgent,
ChatClientId<"demo:refunds">, Tools<RefundOrderTool>, MaxTurns<6>>`.

### (d) Make a run suspend, then resume it — **[docs]**

- `apiContent.ts:1605-1624` (`approvalExampleSnippet`) — `session.set_suspend_for_approval(true);`
  with no decider → `start_run()` "has NO value — fails closed", `session.has_open_interactions()`,
  `session.open_interactions().front().interaction_id`, and
  `session.resolve_interaction(ResolveInteraction{id, /*approved=*/true, std::nullopt})`.
- `apiContent.ts:1629-1660` (`suspendCodesSnippet`) — the four sentinel codes as `static_assert`s and
  `ResolveInteraction`'s nine fields in order. All four code strings match
  `rt/agent_session_core.hpp:1216,1224,1232,1239` — the exact line range the docs cite.
- `ApiHitlReference.tsx:78-104` — the prose form, including "a suspended run is deliberately *not* a
  failure (its class is `contract`)".
- `ApiDurabilityReference.tsx:318` — "A second `start_run()` is refused with `run.approval_pending` —
  no new run id is minted."
- `apiContent.ts:1233` + `:1426` — a resolver that is not the owning principal is denied at admission
  with `run.admission_denied`.
- `ApiPizzaOrderReference.tsx:173-187` (Act 3) — the same lifecycle in the worked example.

### (e) Snapshot and reload a session — **[docs]**

- `durabilityContent.ts:796-820` (`checkpointRoundTripSnippet`, from `examples/12_session_checkpoint.cpp:101-143`)
  — `InMemorySessionStore store;` … `save_agent_session_snapshot(session, store);` …
  `load_agent_session_snapshot(store, "s-checkpoint")` … a **fresh** instance …
  `fresh_session.restore_from_record(*loaded);` … `session_id()` / `principal()` round-trip, and
  `fresh_session.history().empty()` asserted as a *named gap*.
- `ApiPizzaOrderReference.tsx:189-227` (Act 4) — the same, plus "`load_*` returns
  `result<optional<record>>` — there are TWO ways to have nothing" and the whole reload honesty
  statement.
- `durabilityContent.ts:288-298` (`checkpointFields`) — exactly what a record carries and does not.
- `durabilityContent.ts:684,691,827,840` — `FileSessionStore{root}` / `FileSessionStore store(root);`
  / `FileSessionStore reopened(root);`, i.e. the durable conformer's construction **is** shown.
  `ApiDurabilityReference.tsx:180-182` and `durabilityContent.ts:632` also disclose that `save()`
  truncates in place (no atomic write).
- `durabilityContent.ts:119` — "an open approval is durable and restorable … A restored session
  therefore knows an approval was pending and cannot resume it."
- `ApiPizzaOrderReference.tsx:216-224` and `:438-448` — the reload failure code
  `session.resolve_interaction.round_not_recorded`, and that it is deliberate.

### (f) Handle errors — **[docs]**

- `ApiFirstAppReference.tsx:297-331` — `result<T>` is `std::expected<T, agentengine::error>`;
  `has_value()`; `error()` gives `code` ("the stable, machine-readable string to *branch on*"),
  `message`, `klass`; the six `failure_class` values (`transient`, `policy`, `contract`, `resource`,
  `fatal`, `canceled`) and which are retryable. `core/error.hpp:18-29` matches, in the same order.
- `apiContent.ts:1637-1645` — the documented branch: `r.error().code == Session::kSuspendedForApproval`.
- `apiContent.ts:1644` — "`r.error().klass` names its class; `r.error().message` is for the operator log."

Four engine error codes are branched on in code and printed, and all four are documented:
`run.suspended_for_approval` (contract), `run.approval_pending` (contract), `run.admission_denied`
(policy), `session.resolve_interaction.round_not_recorded` (contract). No undocumented code is relied
on anywhere in the program.

### (g) Build and link — **[docs]**

`ApiFirstAppReference.tsx:80-114` (the CMake file), `:252-261` (the two commands and the "30 build
steps" expectation), `:265-295` (the real-model build requirements I did not need). Measured: 30
build steps, 2.8 s configure, zero diagnostics, first try.

---

## Fallback log

# TOTAL FALLBACKS: 6

Two are **blocking** (the program cannot be written without them); four are name/shape-level and cost
minutes. Every one was resolved from the engine's own headers or from an example the docs *cite as
ground truth*, never from speculation.

1. **[blocking] How to express a tool call at all in a scripted `ChatClient`.**
   The docs explain the *consequence* (`apiContent.ts:1439`, `runtime.html` step 5: "a `text_derived`
   call … is denied by the declassification gate regardless of the target tool's own `approval_mode`")
   but never show the `ToolCall` literal: five positional fields in order, and that
   `call_provenance::vendor_structured` is what keeps the call out of that gate. A snippet-copier
   cannot produce an approval-gated call. Source used: `examples/05_human_approval.cpp:102-104`
   (an example the docs cite as ground truth for this exact mechanism — `hitlContent.ts:193`,
   `ApiPizzaOrderReference.tsx:529`) and `include/agentengine/core/content.hpp:109-116`.

2. **[blocking] `ToolCall`'s field names, needed to read a *suspended* call's arguments.**
   `tool_calls_of(response.message)` is documented (`apiContent.ts:1439`); the struct it returns is
   not. Fields are `call_id`, `tool_name`, `arguments_json`, `origin`, `provenance` — not `name` /
   `arguments`. Source: `include/agentengine/core/content.hpp:109-119`,
   `include/agentengine/core/tool_call_extraction.hpp:38-48`.

3. **[minor] `ApprovalDecider`'s actual C++ type and `set_approval_decider()`.**
   Three pages describe the semantics (`memoryContent.ts:395`: "`(tool_name, canonical_args_json) ->
   bool`"; `hitlContent.ts:309`: the method name; `ApiPizzaOrderReference.tsx:352-357`: "does receive
   the call's canonical arguments JSON … its answer is a single `bool`"), but no page gives the type,
   its leading `Principal const&` parameter, or an include. Source:
   `include/agentengine/core/tool_pipeline.hpp:149-150`, `rt/agent_session_core.hpp:502`.

4. **[minor] `json::Value`'s accessor API.** The docs name `json::Value` and `json::dump`
   (`durabilityContent.ts:734-736`, `apiContent.ts:1055`) but show no way to read a member out of a
   parsed value. Needed for the amount threshold: `json::parse`, `Value::find`, `is_number`,
   `as_number`. Source: `include/agentengine/core/json_value.hpp:31-90,509`.

5. **[minor] `SessionCaller`'s namespace.** Its field names are documented inside a struct dump
   (`apiContent.ts:1653`, `SessionCaller{id, tenant_id}`), but the type is
   `agentengine::rt::SessionCaller`, so the pages' implied `using namespace agentengine;` is not
   enough to compile the denial branch. Source: `include/agentengine/rt/agent_session_core.hpp:77-80`.

6. **[cosmetic] `last_run_id()`'s accessor spelling.** The docs name the *field* `last_run_id`
   (`durabilityContent.ts:289`, `ApiDurabilityReference.tsx:233`) and require a resumed run to keep
   the same run id, but no page names a reader. Source:
   `include/agentengine/rt/agent_session_core.hpp:759`.

**Not counted, and why (honesty about my own process).** Three things I first took from headers turned
out to be documented after all, so they are **not** in the six:

- `Bundle::session()` — documented at `ApiFirstAppReference.tsx:177-193`. I read
  `core/session_builder.hpp:442-467` first and only found that paragraph afterwards. My process
  failure, not a docs failure.
- `FileSessionStore{root}` — documented at `durabilityContent.ts:684,691,827,840`. Same sequence.
- The `ToolCall` **`content_origin`/`provenance` defaults** — `content.hpp:114-116` defaults
  `provenance` to `vendor_structured`, so a three-field literal would have compiled; I found this
  while confirming fallback #1.

Also deliberately excluded from the count, because `ApiSnippetConventions.tsx` (the "How to read the
code on this page" note that renders on every detail page) explicitly accounts for them: `drive`
(→ `rt::block_on`, with its header), `check`, `user_message`, `text_of` (→ real API in
`core/tool_call_extraction.hpp`), page-local aliases such as `Session`/`OrderAgent`, and the
`caps`/`store`/`held` values. That note is the single most useful thing on the site for a developer
trying to compile a snippet, and it is accurate.

---

## Hard blockers

**None.** Nothing stopped the build. Configure and link succeeded on the first attempt, the
translation unit compiled with zero diagnostics on the first attempt, and **no engine source file had
to be edited**. All six fallbacks were resolved by reading headers/examples, not by changing the
engine. The only friction outside the docs was environmental and pre-existing: this shell has no
`vcvars`, `cl.exe` is not on `PATH`, and the engine's VS-bundled Ninja is not on `PATH` either, so
the configure command has to run inside `vcvars64.bat` with `-DCMAKE_MAKE_PROGRAM=<VS ninja>`. That is
a shell-setup detail, not an engine or documentation problem.

The engine **did** fetch Asio over the network at configure time even with
`AGENTENGINE_BUILD_TESTS=OFF` and `AGENTENGINE_BUILD_EXAMPLES=OFF` — a fresh build directory contained
`_deps/ae_vendored_asio-subbuild/…/src/asio-1-38-2.tar.gz` and `_deps/ae_vendored_asio-src/include/asio.hpp`.
`ApiFirstAppReference.tsx:207-218` calls those two variables "the whole fix" for the developer build;
they remove ~2,177 build statements, but they do **not** remove the configure-time network fetch. The
page does disclose the escape hatch in a code comment (`:89-92`), so this is an imprecision in the
prose, not a missing fact — and it only bites an offline machine.

---

## Does the documentation tell the truth?

### The reload story: **yes, and it is unusually honest**

Every claim below was verified by running the two processes; the transcript is `run-output.txt`.

| documented claim | page | verified |
| --- | --- | --- |
| With `set_suspend_for_approval(true)` and no decider, the run genuinely suspends; the gated tool's `invoke()` is never reached | `apiContent.ts:1605-1624`, `ApiHitlReference.tsx:78-88` | Yes — `start_run` returned `run.suspended_for_approval`, invocation counter stayed `0` |
| A suspended run is *not* a failure; its failure class is `contract` | `ApiHitlReference.tsx:96-99` | Yes — `class=contract` |
| The suspend code strings are exactly `run.suspended_for_approval`, `…_codeact_ask`, `…_hook_decision`, `…_client_input` | `apiContent.ts:1632-1635` (cites `rt/agent_session_core.hpp:1209-1239`) | Yes — all four match verbatim at `:1216/1224/1232/1239`; the first is exercised |
| A second `start_run()` while an approval is open is refused with `run.approval_pending` and mints no new run id | `ApiDurabilityReference.tsx:318` | Yes — code matched, `last_run_id()` unchanged |
| A `ResolveInteraction` from a non-owning caller is denied at admission, before the interaction lookup, and leaves the interaction open | `apiContent.ts:1233`, `:1426` | Yes — `run.admission_denied`, class `policy`, interaction still open |
| `approved=true` resumes the **same** run/turn, never a new run; the tool then runs through the ordinary pipeline | `apiContent.ts:1622-1624`, `ApiPizzaOrderReference.tsx:416-424` | Yes — same `run_id`, `invoke()` ran exactly once, interaction closed |
| A snapshot survives the process; a fresh instance loads the record back | `durabilityContent.ts:796-820`, `ApiPizzaOrderReference.tsx:189-207` | Yes — a **separate process** read back `session_id`, principal id+tenant, `run_counter=1`, `turn_index=0` |
| A record has exactly the listed fields, encoded as hand-written JSON | `durabilityContent.ts:57-61,288-298` | Yes — the file on disk is exactly that JSON, 8 fields |
| The pending approval survives and is still attributable to the same principal | `durabilityContent.ts:119`, `ApiPizzaOrderReference.tsx:208-210` | Yes — listed with its id, run id and `reason=approval`, under the same principal |
| Answering it on the restored session fails closed with `session.resolve_interaction.round_not_recorded`; nothing runs | `ApiPizzaOrderReference.tsx:216-224`, `:438-448` | Yes — code matched, class `contract`, invocation counter `0 → 0`, interaction closed |
| Conversation history does **not** come back | `durabilityContent.ts:819-820`, `examples/12_session_checkpoint.cpp:140` | Yes — `history().size() == 0` where the original had 2 |

One extra fact I verified that no page states, and that a resume designer needs: **once the failed
`resolve_interaction()` has closed the unanswerable interaction, a brand-new `start_run()` on the
restored session is accepted** (it converges normally). So a reload gives you a session that is
usable for new work while the old work is provably unanswerable — which is exactly the "reconcile it
as the host's job" division `ApiPizzaOrderReference.tsx:222-224` describes, but the page never says
whether the session is still *usable*.

### Can an approval threshold on a call's argument values be expressed through the seams the docs point at?

**No — not through the deciders, and the docs say so themselves; yes — by composing three documented
seams, which no page shows.** Both halves were measured.

- `PolicyDecider` cannot see argument values at all: its type is
  `policy_decision(Principal const&, ToolDescriptor const&, bool arguments_tainted)`
  (`tool_pipeline.hpp:170-171`). `ApiPizzaOrderReference.tsx:350-355` states this correctly.
- `ApprovalDecider` **does** see them: `bool(Principal const&, std::string_view,
  std::string const& canonical_args_json)` (`tool_pipeline.hpp:149-150`). I installed one that parses
  `amount_cents` out of `canonical_args_json` and prints it. Measured: `{"order_id":"88232",
  "amount_cents":12000,…}` for the small refund (decider returned `true` → allowed in-process, **no
  suspend at all**, tool ran), and `{"order_id":"88231","amount_cents":120000,…}` for the large one
  (decider returned `false` → **denied in-process**, tool did not run, **no interaction ever opened**).
  So the doc's "it cannot escalate, because there is no third state" is exactly right.
- The structural reason, which the docs do *not* name: suspend-vs-decider is a **per-session**
  decision, not per-call — `suspend_for_approval_ && !approval_decider_`
  (`rt/agent_session_core.hpp:1273`). You therefore cannot have one session where the small refund
  auto-resolves and the large one escalates; with a decider installed, *nothing* suspends, and without
  one, *everything* gated does.
- The working pattern, which I implemented and ran: leave the decider unset so the gated call always
  suspends, then read the suspended call's own canonical arguments from the **documented**
  `session.history().back()` + `tool_calls_of(...)`, compare with the threshold, and answer with the
  **documented** `ResolveInteraction`. Measured on the same session: `amount_cents=120000` →
  "ESCALATE TO A HUMAN", human answers, tool runs; `amount_cents=12000` → "auto-approve (host policy,
  no human)", resolved without one. That is `ApiPizzaOrderReference.tsx:353-359`'s "host policy
  applied *around* the suspend" made concrete — the docs assert the conclusion but never show the
  composition that makes it possible, and without it a reader is left thinking the threshold is
  impossible.

### Where two pages disagree, or a page disagrees with the engine

1. **`AgentSessionRecord`'s field count — the same page disagrees with itself, in English and
   Vietnamese.** `durabilityContent.ts:59` (EN) says "carries **eight** fields and nothing else" and
   lists eight, ending with `require_authority`. `durabilityContent.ts:171` (VI) says "mang đúng **bảy**
   trường" and lists seven, omitting `require_authority`. The page's own stat says
   `"7" / "fields in a session checkpoint"` (`ApiDurabilityReference.tsx:126-127`). The engine has
   **eight** (`rt/agent_session_core.hpp:300-312`), and the JSON my program wrote to disk contains all
   eight including `"require_authority":false`. EN is right; VI and the stat are wrong.
2. **Why the restored session cannot answer a pending approval.** `durabilityContent.ts:119` explains
   it as "`resolve_interaction()` requires `history_`'s tail to still be the exact assistant tool-call
   message that suspended, and `history_` is not in the record".
   `ApiPizzaOrderReference.tsx:441-442` explains it as "the record does not carry the round it
   suspended on". The engine fails on the **round** record first — the missing `suspended_rounds_`
   entry returns `round_not_recorded` at `src/rt/agent_session_core.cpp:423-432` — and only checks the
   history tail afterwards, at `:434`. Same conclusion, but durability.html names a guard that is not
   the one that fires; the pizza page's wording is the accurate one.
3. **Snippets that do not compile as printed.** `approvalExampleSnippet` (`apiContent.ts:1605-1624`),
   `suspendCodesSnippet` (`:1651-1660`) and `checkpointRoundTripSnippet`
   (`durabilityContent.ts:796-820`) all write `StartRun{…}`, `ResolveInteraction{…}` and
   `session.resolve_interaction(…)` under an implied `using namespace agentengine;`, but
   `StartRun`/`ResolveInteraction`/`SessionCaller` live in `agentengine::rt`. `ApiSnippetConventions.tsx`
   excuses `drive`/`check`/`user_message`/page-local aliases and names `rt::block_on`, but says nothing
   about the missing `using namespace agentengine::rt;`. (Overload resolution rescues the free
   functions — `save_agent_session_snapshot(session, store)` is found via ADL on the `rt::` store — but
   not the aggregate types; `examples/05_human_approval.cpp:32-35` spells the three `using`
   declarations out, and the docs' excerpts dropped them.)
4. **"the whole fix"** for the configure-time network fetch — see Hard blockers. Minor prose
   imprecision, disclosed one paragraph earlier in a code comment.

No other disagreement was found. Notable claims that *are* accurate and were checked rather than
assumed: `result<T>` is `std::expected` with `code`/`message`/`klass` and the six classes in the
documented order; `kSuspendedForApproval` is a static member of the session type; `load_*` really does
have two distinct ways to yield "nothing" (a store error, versus an empty record for a
never-snapshotted or tombstoned id); `history` is outside the record; `Capabilities` are not in the
record; and the `ApiDiagnosticNote`-style "named residual" framing on the durability page matches what
the code actually does.

---

## Undefined identifiers, and where each comes from

"Undefined" = used by a docs snippet and defined nowhere on the page that uses it. Most are
*excused* by `ApiSnippetConventions.tsx`, which renders on every detail page; I have marked those.

| identifier | status | where it actually comes from |
| --- | --- | --- |
| `drive(task)` | excused by the conventions note | example-local; engine equivalent `agentengine::rt::block_on(task)`, `rt/block_on.hpp` — the note gives both |
| `check(cond, label)`, `g_failures` | excused | example-local test harness |
| `user_message(text)` | excused, and its full body is on `ApiPizzaOrderReference.tsx:65-73` | example-local |
| `text_of(message)` | excused as real API | `include/agentengine/core/tool_call_extraction.hpp:83` |
| `Session`, `OrderAgent` | excused as page-local aliases | a named `AgentSession<ChatClientT, StateT, HistoryProviderT>` |
| `SessionCaller` | **not** excused; used at `apiContent.ts:1653` | `agentengine::rt::SessionCaller` — `rt/agent_session_core.hpp:77-80` (fallback 5) |
| `RequestAuthority`, `HookDispatchAnswer`, `ApprovalCallDecision`, `ClientInputAnswer` | **not** excused; named only as `ResolveInteraction` field types (`apiContent.ts:1655-1659`) | `rt/agent_session_core.hpp:91+`, `core/tool_call_hook.hpp`, `rt/agent_session_core.hpp:116-119`, `:124-127`. Not needed by this product |
| `ApprovalDecider`, `PolicyDecider` | described in prose, never defined | `core/tool_pipeline.hpp:149-150`, `:170-171` (fallback 3) |
| `EffectContext`, `TurnView`, `SessionContext`, `ContextContribution`, `ToolTable`, `ToolDescriptor` | **not** excused | `core/effect_context.hpp`; `core/context_provider.hpp:77,60,45`; `core/tool_descriptor.hpp:316` |
| `CapabilitySet`, `cap::decl::NetOut<…>` | excused as "the page that owns that subject shows the construction" | `trust/capability.hpp:826`. Avoided entirely here by using `make_agent_session`, which builds the set |
| `InMemorySessionStore`, `FileSessionStore` | used in snippets; `rt::` not written | `rt/session_store.hpp:101,195` |
| `save_agent_session_snapshot`, `load_agent_session_snapshot`, `restore_from_record`, `start_run`, `resolve_interaction`, `open_interactions`, `has_open_interactions`, `history`, `session_id`, `principal` | names given, namespace/header not | `rt/agent_session.hpp:544-566`, `:556`; `rt/agent_session_core.hpp:466,839,768,771,751,752,753` |
| `kSuspendedFor*` | **defined in the docs** (`apiContent.ts:1632-1635`), as static members of "YOUR session type" | `rt/agent_session_core.hpp:1216-1239`; `Session::` needs your own alias |
| `PizzaPalaceChatClient`, `OrderHistoryProvider`, `SendArgs`, `SendReply`, `LookupArgs`, `LookupReply`, `ScriptedChatClient` | invented by the snippet; the pizza page says the app is illustrative | host code; I wrote my own `ScriptedRefundClient` |

One real API gap, not a documentation problem: **`interaction_reason` has no `to_string`**. Every
other enum in the engine has one (`failure_class_to_string`, `core/error.hpp:32`); `interaction_reason`
does not, so printing which kind of interaction is pending means hand-writing the switch
(`main.cpp`, `reason_name`). Trivial, but it is the one thing a host must duplicate.

---

## Blockers ranked

1. **No compilable example of producing a tool call** (fallback 1). Highest impact by a wide margin:
   the whole product is "a tool call gated behind approval", and the docs cannot produce that call.
   Mitigation is good — the docs cite `examples/05_human_approval.cpp` as ground truth for exactly
   this mechanism — but a developer who trusts only the prose is stuck.
2. **`ApprovalDecider` has no C++ shape anywhere on the site** (fallback 3), while three pages rely on
   its semantics to answer the most important design question on the HITL page.
3. **Argument values are readable only by composing two seams the docs never compose** (the threshold
   section). The conclusion is documented; the technique is not, and it is the difference between "the
   threshold is impossible" and "here is how you build it".
4. **The `rt::` qualification and headers for the durability/session API** (fallbacks 2, 5, 6; docs
   defect 3). Costs a few minutes of guessing per identifier; ADL hides most of it.
5. **The `AgentSessionRecord` field-count disagreement** (docs defect 1). Harmless in practice — you
   read the struct — but it is a wrong statement on a page whose entire selling point is precision,
   and it is wrong in one language only.
6. **`durability.html`'s stale explanation of the reload failure** (docs defect 2). Directionally
   right, mechanically off by one guard.
7. **Configure-time network fetch survives the two documented options** (Hard blockers). Only matters
   offline, and the escape hatch is disclosed.

## Confidence per requirement

| # | requirement | confidence | basis |
| --- | --- | --- | --- |
| 1 | an agent with a `refund_order` tool gated behind human approval | **Certain** | declared and run; tool never executed before approval (counter `0`) |
| 2 | a call requiring approval **suspends** rather than proceeding | **Certain** | `run.suspended_for_approval`, class `contract`, invocation counter `0`, real `Interaction{reason=approval}` |
| 3 | full suspend → interaction id → human answers → resume | **Certain** | same `run_id` before and after, `invoke()` ran exactly once on resume, interaction closed |
| 4 | durable state; a **fresh process** reloads it and shows what is still pending, same principal | **Certain** | two processes, one `FileSessionStore`; record on disk inspected; principal/run/turn/interaction all round-tripped; reload limit (`round_not_recorded`) reproduced |
| 5 | the failure path branched on in code, engine error code printed | **Certain** | four documented codes branched on and printed: `run.suspended_for_approval`, `run.approval_pending`, `run.admission_denied`, `session.resolve_interaction.round_not_recorded` |
| 6 | buildable and runnable by me | **Certain** | 30-step build, first attempt, zero diagnostics, all four modes exit 0 |
| — | threshold on a call's argument values | **Certain about the answer** (deciders cannot; host-around-the-suspend can) | `ApprovalDecider` measured receiving `amount_cents` and only ever allowing/denying; the host composition measured working on both sides of the threshold |

## Verdict

**Yes, with one qualification.** A competent C++ developer can build this product from the docs, but
not from the docs *alone*: they must also read the two examples the docs themselves name as ground
truth (`examples/05_human_approval.cpp` for the `ToolCall` literal, `examples/12_session_checkpoint.cpp`
for the snapshot/restore shape), plus a handful of headers for exact signatures. Everything
conceptual — what a suspension is, the sentinel codes, the `ResolveInteraction` shape, the admission
rule, exactly what a checkpoint carries and what it will not carry back, and the fact that a restored
session can *list* but not *answer* a pending approval — is stated accurately and in unusual detail,
and I confirmed the reload story line by line rather than taking it on faith. The six fallbacks are
all at the "exact identifier or literal" level; none required a design decision, none contradicted a
page, and none needed an engine edit.

**Time estimate for a competent C++ developer with no prior AgentEngine exposure:** roughly **2.5
hours** — about 60–75 minutes reading the HITL/durability/runtime/first-app/pizza pages and the
conventions note, ~30 minutes writing this program once the tool-call literal and the `Bundle::session()`
/ `FileSessionStore` plumbing are known, ~5 minutes configure+build, and ~15 minutes of small API-shape
corrections (the `rt::` qualifications, `set_approval_decider`'s three-argument type, `json::Value`'s
accessors). Add an hour if the developer refuses to open `examples/`, because then fallbacks 1 and 2
are not recoverable from the site at all.
