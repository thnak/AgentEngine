# REPORT — a refund service built on AgentEngine, from the documentation

**What this is.** A back-office refund service, written as a *consumer* of AgentEngine (not a
contributor), using `web/marketing/src/**` and `web/marketing/api/*.html` as the primary resource.
`main.cpp` declares a `refund_order` tool gated behind human approval, makes a run suspend, obtains
the interaction id, has a human answer, resumes it, snapshots the session to disk, reloads it in a
**second real process**, and branches in code on four distinct engine error codes.

**Everything claimed below was run on this machine**: Windows 11 x86-64, MSVC 19.51.36260
(`14.51.36231`), C++23, CMake 4.3.2, Ninja, Release.

Files in this directory: `main.cpp`, `CMakeLists.txt`, `REPORT.md` (the deliverables), plus
`run-output.txt` (verbatim output of all four modes), `session-store/` (the durable record this
program wrote — the JSON is quoted in §3.1) and `build/` (the build tree). No existing file anywhere
in the repository was modified; `git status` shows no change attributable to this work.

## How to build and run it

```
cmake -S devsim/rerun2 -B devsim/rerun2/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build devsim/rerun2/build --target refund_agent -j 4

devsim/rerun2/build/refund_agent.exe lifecycle
devsim/rerun2/build/refund_agent.exe threshold
devsim/rerun2/build/refund_agent.exe durable-save   devsim/rerun2/session-store
devsim/rerun2/build/refund_agent.exe durable-reload devsim/rerun2/session-store   # a second process
```

Observed: configure exit 0, build exit 0, **30 ninja steps**, zero warnings, and all four modes end
`OK (failures=0)` with exit 0. `run-output.txt` in this directory is the verbatim output of all four.

The build file was also tested **verbatim as the docs print it** (a second consumer project in a
temp directory, sharing only `main.cpp`, with `ApiFirstAppReference.tsx:80-108` copied literally and
only the engine path changed): configure exit 0, build exit 0, 30 steps, and the resulting binary
reproduces the same results. The docs' claim of "30 build steps in total"
(`ApiFirstAppReference.tsx:224`) is exactly what I measured.

---

## 1. Getting started — found in docs, or reverse-engineered

| # | Step | Verdict | Where |
|---|------|---------|-------|
| a | Include the engine | **Found in docs** | `ApiFirstAppReference.tsx:11-18` (the complete program's includes), `:80-108` (build file), `:169-172` ("vendored, not installed"). Header paths for the pieces beyond that program: `ApiDurabilityReference.tsx:133` (`rt/agent_session.hpp`), `durabilityContent.ts:645` (`include/agentengine/rt/session_store.hpp:149`), `ApiSnippetConventions.tsx:17` (`rt/block_on.hpp`), `ApiRuntimeReference.tsx:299` (`tool_call_hook.hpp`). The **fallback** is the rest of the include set — see log #6. |
| b | Construct a session | **Found in docs** | `ApiFirstAppReference.tsx:59-62` (`make_agent_session<A>(client, opts)`, the new on-ramp) and `durabilityContent.ts:792-799` + `apiContent.ts:624-628` (`AgentSession<...>` + `initialize` + `emplace_chat_client`, the older route). The join between an *agent declaration* and a raw session is `.session()`, documented at `ApiBuilderReference.tsx:128-131`; that it applies to `make_agent_session`'s bundle is **not** stated anywhere — log #8. |
| c | Declare an approval-gated tool | **Found in docs** | `apiContent.ts:1605-1611` (the exact `Tool<Derived, Capabilities<>, EffectClass<...>, Approval<approval_mode::always_require>>` shape), `apiContent.ts:752-761` (five required members), `:790-816` (`AE_JSON_SCHEMA` + the JSON it emits), `:649-698` (the member table incl. the three override traits and their defaults), `ApiPizzaOrderReference.tsx:64-85` (a real `refund_order` with `policy_driven`/`at_most_once`). |
| d | Make a run suspend, then resume it | **Found in docs**, except the error-code *value* | `apiContent.ts:1228-1236` is the single most complete card (`set_suspend_for_approval(true)`, `ResolveInteraction{interaction_id, approved}`, same `run_id`, second `StartRun` refused, foreign caller denied at admission); also `apiContent.ts:1613-1624`, `ApiHitlReference.tsx:78-87`, `ApiRuntimeReference.tsx:268-298`, `ApiDurabilityReference.tsx:318`. **Fallback #1**: no page gives the string `run.suspended_for_approval`. |
| e | Snapshot and reload a session | **Found in docs** | `durabilityContent.ts:653-662` (the record), `:666-678` (`snapshot_record()` + `CheckpointCadence`), `:683-687` (`InMemorySessionStore` / `FileSessionStore{root}`), `:792-816` (the snapshot → discard instance → `restore_from_record` round trip), `ApiDurabilityReference.tsx:152-166` and `:232`, and the fully worked restart in `ApiPizzaOrderReference.tsx:149-187` including the exact failure code. **Fallback #5** for the record's real field set. |
| f | Handle errors | **Found in docs** (model), **partly reverse-engineered** (the codes) | `ApiFirstAppReference.tsx:258-292`: `result<T>` is `std::expected`, check `has_value()`, `error()` gives `code`/`message`/`klass`, the six `failure_class` values, and "code is the stable, machine-readable string to branch on". Codes: `run.approval_pending` (`ApiDurabilityReference.tsx:318`, `durabilityContent.ts:502`), `run.admission_denied` (`apiContent.ts:1424`), `session.resolve_interaction.round_not_recorded` (`ApiPizzaOrderReference.tsx:177`, `ApiHitlReference.tsx:213`) — all four **found in docs** except `run.suspended_for_approval` (**fallback #1**). |
| g | Build/link | **Found in docs** | `ApiFirstAppReference.tsx:83-108`: turn `AGENTENGINE_BUILD_TESTS`/`AGENTENGINE_BUILD_EXAMPLES` off with `set(... CACHE BOOL "" FORCE)` *before* `add_subdirectory()`, then link exactly one target, `agentengine::core`. `:103-108` also documents the real-model trap (`-DAGENTENGINE_WITH_HTTPS=ON` plus `provider_http_client`, and the misleading compile error without them) — not needed here. The snippet is silent on the C++ standard; it is not required, because `agentengine_core` carries `cxx_std_23` as an `INTERFACE` compile feature (`CMakeLists.txt:95`) — my verbatim-docs build proves this. |

---

## 2. Fallback log

**TOTAL FALLBACKS: 10.** Each is a place where the documentation did not carry me and I read engine
source (`include/`), `examples/`, or ran a probe to recover a fact I needed in order to write code.

1. **The suspension sentinel's string value.** The docs name the C++ identifier only —
   `kSuspendedForApproval` (`durabilityContent.ts:89`, `ApiDurabilityReference.tsx:318`) — and the
   string `run.suspended_for_approval` appears nowhere in `web/`. Since the docs' own error model
   says to branch on `error().code` (`ApiFirstAppReference.tsx:266-267`), the branch they recommend
   is unwritable from the docs. Recovered from `include/agentengine/rt/agent_session_core.hpp:1216`.
   *(The class is `contract`, not `policy` — also undocumented.)*
   Note the naming convention *is* visible once, for a different reason:
   `run.suspended_for_client_input` at `ApiHitlReference.tsx:147`.
2. **`ResolveInteraction`'s shape.** The struct is the one call every non-workflow resume goes
   through, is used positionally with 3, 4 and 6 arguments (`apiContent.ts:1622`, `:1643-1645`,
   `:2759`; `ApiPizzaOrderReference.tsx:144`) and is **never defined**. Field 3 is `caller` — the
   load-bearing field behind the docs' own claim that a non-matching caller is denied at admission
   (`apiContent.ts:1233`). The real struct has 9 fields. Recovered from
   `agent_session_core.hpp:130-160`.
3. **`SessionCaller`**, the type of that field. Used nowhere by name, defined nowhere. Recovered from
   `agent_session_core.hpp:77`.
4. **`ApprovalDecider` / `PolicyDecider`: header and signatures.** `ApiHowItWorksReference.tsx:273`
   lists them as things "you supply"; `ApiPizzaOrderReference.tsx:289-297` describes precisely what
   each seam can and cannot see. Neither page gives an include path or a signature, so the threshold
   question below is unanswerable in code from the docs. Recovered from
   `include/agentengine/core/tool_pipeline.hpp:148-171`.
5. **`AgentSessionRecord`'s real field set.** The docs state "seven fields and nothing else"
   (`durabilityContent.ts:59`) and print a 7-field struct (`:653-662`); the engine has **eight**
   (`require_authority`, ADR-061 §22.1) and that is what lands on disk. Found by dumping the
   persisted JSON, then confirmed in `agent_session_core.hpp:300-312`. See §3.
6. **The include list for the approval flow.** Doc snippets are excerpts, not files: the shared panel
   says so and warns that some names are the source file's own helpers
   (`ApiSnippetConventions.tsx:13-44`), and points at the first-program page for "one file you can
   compile from top to bottom" (`:44`). That page is offline-`ask()` only and covers neither
   approval, tools, schema, nor durability. The working set came from
   `examples/05_human_approval.cpp:18-35`, which the HITL page itself cites as its source
   (`apiContent.ts:1605`).
7. **Printing a `failure_class`.** The six values are listed (`ApiFirstAppReference.tsx:269-271`);
   no doc gives a way to turn one into text. Used the engine's `failure_class_to_string`
   (`core/error.hpp:32`) rather than hand-rolling the switch.
8. **`make_agent_session<A>()`'s return type.** The docs never name it, and `.session()` is
   documented only for the Builder page's `Bundle` (`ApiBuilderReference.tsx:128-131`). That they are
   the same type is stated nowhere. Confirmed via
   `include/agentengine/core/agent_session_bridge.hpp:211-213` (`AgentBundle = quickstart::Bundle<...>`).
9. **Reading one argument out of a `json::Value`.** The docs use `json::dump(...)`
   (`durabilityContent.ts:732`) but never document `json::Value`'s public accessors, which the
   threshold rule needs. Recovered from `include/agentengine/core/json_value.hpp:87` (`find`) and
   `:541` (`as_bounded_integer`, the overflow-safe integer read).
10. **`ToolCallHookContext::arguments`.** The docs show `hctx.tool_name` and
    `hctx.needs_external_dispatch` (`apiContent.ts:1633`) and say a hook may "rewrite its arguments"
    (`ApiHitlReference.tsx:96`), but never name the member that carries them. It is the only
    documented-API way for host code to see the amount of a call that is about to suspend. Recovered
    from `include/agentengine/core/tool_call_hook.hpp:56-72`.

Not counted as fallbacks (docs carried these): `start_run`/`resolve_interaction`/`open_interactions`/
`has_open_interactions`/`session_id`/`principal`/`history`/`restore_from_record`
(`apiContent.ts:1228-1236`, `durabilityContent.ts:653-816`), `rt::block_on`
(`ApiSnippetConventions.tsx:16-18`), `Principal` (`ApiFirstAppReference.tsx:134`), `AE_JSON_SCHEMA`
(`ApiToolReference.tsx:58-70`), `set_tool_call_hook` (`ApiRuntimeReference.tsx:303-305`),
`EffectClass`/`Approval`/`Capabilities` tag spelling (`apiContent.ts:1605-1611`).

---

## 3. Did the docs agree with the engine?

### Agreement, verified by running it

| Claim | Source | Verified |
|---|---|---|
| A `policy_driven` tool with no decider **fails closed and suspends**; nothing runs; a real `Interaction` opens with `reason == approval` | `ApiPizzaOrderReference.tsx:74-76`, `:347-352`; `ApiHitlReference.tsx:78-87` | Yes. `error().code == "run.suspended_for_approval"`, ledger empty, `open_interactions().size() == 1`, `reason=approval`, `run_id=sess-refunds-1:run:1` |
| `resolve_interaction({id, approved=true})` resumes the **same** run and finally executes the tool through the ordinary pipeline | `apiContent.ts:1233`; `ApiRuntimeReference.tsx:290-297` | Yes. `last_run_id()` identical before/after, `turn_index` advanced, `refund_order::invoke()` ran and the ledger recorded `amount_cents=90000` |
| `approved=false` folds a denial in as an ordinary tool error and the run still converges | `apiContent.ts:1233` | Yes. `r2.has_value() == true`, ledger unchanged, interaction closed |
| A second `StartRun` while an interaction is open is refused (`run.approval_pending`) and mints no run id | `ApiDurabilityReference.tsx:318`; `durabilityContent.ts:502` | Yes, exact code printed |
| A `ResolveInteraction` from a caller that does not match the session's owning principal is denied **at admission** | `apiContent.ts:1233`, `:1424` | Yes, `run.admission_denied` / class `policy`; the interaction stayed open and nothing ran |
| Configuring an `ApprovalDecider` makes a `policy_driven` call answerable **in-process instead of suspending** | `ApiPizzaOrderReference.tsx:294-297` | Yes. With a decider installed, `start_run()` returned a value and never suspended |
| The `ApprovalDecider` seam receives the call's **canonical arguments JSON** | `ApiPizzaOrderReference.tsx:292-294` | Yes. It was handed `{"order_id":"ORD-77001","amount_cents":90000,...}` and read the amount |
| The `PolicyDecider` seam does **not** receive argument values | `ApiPizzaOrderReference.tsx:292-294` | Yes, structurally: its parameters are `(Principal const&, ToolDescriptor const&, bool arguments_tainted)` |
| Every open interaction is checkpoint content: it comes back still listed as open and still attributed to the same principal | `ApiHitlReference.tsx:207-211`; `ApiPizzaOrderReference.tsx:168-170` | Yes, across two processes and a real file store: `principal_id=principal-ada`, `tenant_id=tenant-backoffice`, one open interaction with `reason=approval` |
| Answering that restored interaction fails closed with `session.resolve_interaction.round_not_recorded` and the interaction is closed with nothing run | `ApiPizzaOrderReference.tsx:172-187`; `ApiHitlReference.tsx:211-218` | Yes — exact code, class `contract`, and `has_open_interactions()` false afterwards |
| Conversation history is **not** part of the record | `ApiPizzaOrderReference.tsx:186-187`; `durabilityContent.ts:815-816` | Yes: process A had `history().size() == 2`, the reloading process had 0 |
| `session_id`, principal id and tenant round-trip | `durabilityContent.ts:812-813` | Yes |

**So: on the specific question asked — does the documentation accurately describe what can and
cannot be done after reloading a session? — the answer is yes, and unusually well.** The docs say
plainly that a restored session *knows what is owed* but *cannot finish it*, name the exact failure
code, and give the reason (the previous fallback could attribute one call's approval to another).
That is exactly what I observed. Two named gaps are disclosed rather than glossed: history is not in
the record, and `pending_codeact_asks_` is not checkpointed (`ApiHitlReference.tsx:219-222`).

### Where the docs and the engine disagree

1. **`AgentSessionRecord` has eight fields, not seven.** `durabilityContent.ts:59` says "carries
   seven fields and nothing else" and lists them; `:653-662` prints the struct the same way. The
   engine has a further field, `require_authority` (ADR-061 §22.1), and it is in the bytes on disk.
   I read the file this program wrote:
   `{"session_id":"sess-refunds-durable","principal_id":"principal-ada","principal_tenant_id":"tenant-backoffice","deleted":false,"run_counter":1,"turn_index":0,"open_interactions":[...],"require_authority":false}`.
   The docs' own snippet is therefore not the shape a consumer reads back, and the one page whose
   whole subject is this record is stale by one required field. (`durabilityContent.ts:59`'s prose
   and `:653-662`'s code agree with *each other*, so this is a two-places-one-error, not a page
   disagreement.)
2. **Two pages, one threshold rule, opposite implications.** `ApiPizzaOrderReference.tsx:289-297`
   says a "refund above 500 needs a human" rule "cannot live" in `PolicyDecider` — true — and then
   points at `ApprovalDecider` as the seam that "can therefore read an amount". But the same
   paragraph ends by saying that configuring an `ApprovalDecider` is *exactly what makes the call
   answerable in-process instead of suspending*. I verified the consequence: with a decider that
   reads the amount and returns `false` above the threshold, the over-threshold refund is **denied**
   in-process — `start_run()` returns a value, nothing suspends, no human is asked, and the model
   gets an ordinary tool error. So the threshold rule can be *evaluated* in `ApprovalDecider` but
   "above the threshold a **human** must approve" cannot be *expressed* there: its answer is a bare
   `bool` with no third state meaning "park this for a person". The page's own Act 3 note
   (`:347-352`, "the app's own amount threshold, if it has one, is applied by the host on top") is
   the correct statement and matches the engine; Act 0's note reads the other way. **This is the
   sharpest doc-vs-engine/self-inconsistency I found, and it is in the page that a developer of
   exactly this product would trust most.**
3. **A workflow's `request_port` is grouped with session checkpoint content.**
   `ApiHitlReference.tsx:203-218` says "Every open `Interaction` is checkpoint content", names
   `request_port` among the mechanisms, and then attributes the `round_not_recorded` refusal to
   `resolve_interaction()` generally. For a workflow the durable artefact is a *workflow* checkpoint
   resumed through `WorkflowSupervisor::resume_workflow()`, and `durabilityContent.ts:69` says so
   explicitly ("a `Workflow` run has its own, separate durability story"). The paragraph is right
   about approval/`codeact_ask`/`hook_decision`, which are session-scoped, and right that workflow
   resumes work; it reads as one mechanism when it is two. Minor, but it is the paragraph a reader
   uses to decide *what* to persist.
4. **What the docs get right but do not connect:** `durabilityContent.ts:59` says metadata and typed
   session state are deliberately outside the record "each for a named reason" without naming the
   reasons in that sentence; the reasons are in the file banner and in `ApiPizzaOrderReference.tsx`.
   Not a factual error — a citation the reader cannot follow from the page.

---

## 4. Undefined identifiers in doc snippets

Every name a doc snippet used without defining it, and whether I found where it comes from. Items 7-10
and 12-14 are the ones that actually cost me time.

| # | Identifier | Where it appears | Defined anywhere in the docs? | Origin |
|---|---|---|---|---|
| 1 | `drive(task)` | every HITL/durability/pizza snippet | Yes — declared example-local | `ApiSnippetConventions.tsx:16-18`; engine equivalent `rt::block_on` (`rt/block_on.hpp`) |
| 2 | `check(cond, label)` | same | Yes — declared example-local | `ApiSnippetConventions.tsx:19-22` |
| 3 | `user_message(text)` | `apiContent.ts:1616` etc. | Yes — declared example-local | `ApiSnippetConventions.tsx:24-25` |
| 4 | `text_of`, `make_stream<T>` | `ApiSnippetConventions.tsx:27-29` | Yes — real API, header named | `core/tool_call_extraction.hpp`, `core/stream.hpp` |
| 5 | page-local `using` aliases (`Session`, `OrderAgent`) | `ApiPizzaOrderReference.tsx:100` | Yes — declared | `ApiSnippetConventions.tsx:31-34` |
| 6 | `caps, store, held` | `ApiSnippetConventions.tsx:35-38` | Yes — declared as "you built these" | `ApiSnippetConventions.tsx:35-38` |
| 7 | `ResolveInteraction` | `apiContent.ts:1622`, `:1643-1645`, `:2759`; `ApiPizzaOrderReference.tsx:144` | **No** | `agent_session_core.hpp:130-160` (fallback #2) |
| 8 | `SessionCaller` | never named; required for the field above | **No** | `agent_session_core.hpp:77` (fallback #3) |
| 9 | `StartRun`, and its `caller` field's type | `apiContent.ts:1616`; `apiContent.ts:1233` | **No** | `agent_session_core.hpp:106-113` |
| 10 | `ToolCallHookContext::arguments` | implied by `ApiHitlReference.tsx:96`; member never named | **No** | `core/tool_call_hook.hpp:59` (fallback #10) |
| 11 | `HookDispatchAnswer` | `apiContent.ts:1645` | **No** | `core/tool_call_hook.hpp:116` (not needed in the end) |
| 12 | `NoSessionState` | `ApiPizzaOrderReference.tsx:100`; `apiContent.ts:1502` | **No** | `rt/agent_session_core.hpp:71`. Related: `AgentSession`'s 2nd/3rd template parameters are shown with arguments but their **defaults are never stated**, even though `durabilityContent.ts:794` instantiates `AgentSession<CannedChatClient>` with one |
| 13 | `approval_mode`'s members | never enumerated together | Spread only: `never_require` (`apiContent.ts:691`, `:845`), `always_require` (`:1608`), `policy_driven` (`ApiPizzaOrderReference.tsx:79`) | collected from three pages |
| 14 | `effect_class`'s members | never enumerated together | Spread only: `pure` (`apiContent.ts:863`), `at_most_once` (`:697`), `idempotent` (`durabilityContent.ts:740`) | collected from three pages |
| 15 | `ToolTable::from_tools<Ts...>()` | `ApiPizzaOrderReference.tsx:94` | **No** | `core/tool_table.hpp` (not verified — the agent declaration supplied the tool surface instead) |
| 16 | `Capability`, `CapabilitySet::grant_root`, `cap::decl::NetOut<...>` | `ApiPizzaOrderReference.tsx:108-110` | **No** (trust page owns it) | not needed: `Capabilities<>` is empty |
| 17 | `encode_agent_session_record`, `make_tombstone_record` | `durabilityContent.ts:704` | **No** | `rt/agent_session_core.hpp` (not needed — the free functions wrap them) |
| 18 | `Principal`'s field names | `ApiFirstAppReference.tsx:134` shows positional init only | Inferred from `principal_id`/`principal_tenant_id` | `trust/principal.hpp:27-29` (`id`, `tenant_id`) |
| 19 | `turns` in `checkpoint_if_due<CheckpointCadence<3>>(session, store, turns)` | `durabilityContent.ts:681` | **No** — prose says the counter belongs to the caller, no snippet shows where it comes from | not needed |
| 20 | `AgentBundle` (the type `make_agent_session<A>()` returns) | never named | **No** | `core/agent_session_bridge.hpp:213` (fallback #8) |

The shared "How to read the code on this page" panel does real work here: it correctly resolves
items 1-6 and names `set_capabilities(&held)`'s lifetime hazard. It does not cover the engine types
(items 7, 8, 9, 12) — which are the ones that block code rather than confuse a reader.

---

## 5. Blockers, worst first

1. **The suspend sentinel's value is not in the docs** (fallback #1). Requirement 5 — "handle the
   failure path in code and print the engine's error code" — cannot be *written as the docs advise*
   ("code is the stable, machine-readable string to branch on") without opening a header. One line
   beside the two places that name `kSuspendedForApproval` fixes it.
2. **`ResolveInteraction` is never defined** (fallback #2, #3), although it is the single resume call
   for every non-workflow surface, and field 3 (`caller`) is the mechanism behind a security claim
   the same page makes. A reader copying `ResolveInteraction{id, /*approved=*/true, std::nullopt}`
   gets a working call — and cannot ever discover the admission property.
3. **The agent-declaration story and the suspend story never visibly meet** (fallback #8). The
   on-ramp page ends at `agent->ask(...)`; the HITL/runtime pages start from a raw
   `AgentSession session;`. That `make_agent_session<A>()` returns the very `Bundle` whose
   `.session()` escape hatch the *Builder* page documents is stated on neither page. This is the join
   this specific product needs first, and it took three pages plus a header to establish.
4. **`AgentSessionRecord` is documented with the wrong field count** (fallback #5). Anyone writing a
   consumer-side codec, a migration, or a store of their own from `durabilityContent.ts:653-662` is
   wrong against the bytes the engine actually writes.
5. **The threshold rule has a location but no shape** (§3.2). The engine cannot express "suspend above
   T, proceed below T" in a decider, and no page shows the host-side threshold that Act 3 says you
   must apply. I built and verified the host-side form (threshold read from the pending call's
   arguments via the observer hook, then either auto-answer or escalate) — but that design is mine,
   not the docs'.
6. **The only documented-API way to read the amount of a call that is about to suspend is
   undiscoverable** (fallback #10): `ToolCallHookContext::arguments` is never named, and
   `Interaction` deliberately carries no arguments, so `open_interactions()` alone cannot tell a
   host *what* it is being asked to approve.

---

## 6. Confidence per requirement

| Requirement | Confidence | Basis |
|---|---|---|
| 1. Declare an agent with a `refund_order` tool gated behind human approval | **Certain** | Built and ran: `Agent<RefundAgent, ChatClientId<"demo:refunds">, Tools<RefundOrderTool>, MaxTurns<6>>`; `make_agent_session<RefundAgent>` returned a bound bundle; the suspension proves the gate is live |
| 2. Configure it so a call requiring approval **suspends** rather than proceeding | **Certain** | `set_suspend_for_approval(true)` with no decider; `start_run()` returned no value, ledger empty, one open `approval` interaction |
| 3. Full suspend → obtain interaction id → human answers → resume | **Certain** | Ran, including same-`run_id`, `turn_index` advance, tool executed, interaction closed; plus the refusal branch and the busy-session branch |
| 4. Save durably; reload in a fresh process; show what is pending and attributable | **Certain about what I verified**, with a scope limit stated | Two real processes, a real `FileSessionStore` file (354 bytes, contents quoted in §3.1), identity/tenant/interaction restored, `round_not_recorded` reproduced. **Not verified:** any store other than `FileSessionStore`/`InMemorySessionStore`; `CheckpointCadence<N>`'s automatic write at a turn boundary (I called `save_agent_session_snapshot` explicitly); resuming the restored work, which the docs and the engine both say is impossible from the restored session |
| 5. Failure path branched in code, engine error code printed | **Certain** | Four codes printed and branched on: `run.suspended_for_approval`, `run.approval_pending`, `run.admission_denied`, `session.resolve_interaction.round_not_recorded` — three found in the docs, the first reverse-engineered |
| 6. Buildable and runnable | **Certain** | Two independent CMake projects, including one using the docs' build file verbatim; both configure/build clean and produce working binaries |
| Expressing the approval threshold from a call's arguments | **Could not determine from the docs.** Guessed, then verified by experiment | The docs give the rule's location but not its shape, and the engine cannot suspend from a decider. My host-side rule works and is verified; it is not what the docs describe |

---

## 7. Verdict

**Could a competent C++ developer build this from the docs alone?** Substantially yes, but not
entirely, and not without reading a header.

- Requirements 1, 2, 3 (the plain path), 4 and 6 are buildable from the docs. The snippets are
  excerpts rather than compilable files, but `ApiFirstAppReference.tsx` is a genuinely complete,
  compiled program, the build story is accurate down to the step count, and the
  "How to read the code on this page" panel correctly names the example-local helpers — the single
  most common reason a pasted snippet fails to compile.
- Requirement 5 fails as stated: to *branch* on the engine's suspension code by name you must leave
  the docs. You can still print the code you observe at runtime, so the requirement is achievable
  only by probing rather than by reading.
- The threshold rule (the heart of this product) is not answerable from the docs. The page that owns
  refunds gets the entities right and the conclusion half-right; the missing piece — a decider
  cannot ask for a human, so the threshold must be applied by the host around a suspend — is stated
  once, in a different act of the same page.
- The durability answer is the strongest material in the set: precise, honest about two named gaps,
  and borne out exactly by a two-process test.

**Time.** From the docs alone, to reach exactly the state this directory represents — four runnable
modes, four error codes branched on, a real two-process durability test — I estimate **1 to 2 working
days** for a competent C++ developer new to AgentEngine (roughly: 2-4 h to assemble the session/tool
API across the pages, 1-2 h to get a first suspend, 2-3 h on the durability pair and its failure
branch, 1-2 h on the threshold experiment, plus build/iteration time). The expensive part is not the
engine's design, which is coherent and unusually well written down; it is that six facts needed to
write ten lines of code live on four pages and, in three cases, only in the headers. A developer
with the repository open but no permission to read `include/` would be blocked at blockers 1 and 2,
and would produce a working service that silently omits the admission check.
