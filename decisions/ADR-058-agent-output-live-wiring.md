# ADR-058 — Wiring `OutputSchema<T>` into a live run, and what "validation" actually means here

**Status:** Proposed. Design → red-team → prove phases complete for Design B (a narrow, additive
opt-in setter). Real code implements §8's plan (with one correction found while implementing — §5:
there is exactly one `ChatRequest{...}` construction site in the codebase, not three), the five
committed tests (O1-O5) all pass (17/17 individual checks) against a real `rt::AgentSession`. This
pass's work was written and initially tested in an isolated worktree that briefly lacked ADR-057's
`codeact_ask` machinery (a worktree-isolation artifact, named honestly while it was open — see the
superseded gap-2 note originally here); the worktree branch has since been **rebased onto current
`main`, which includes both of ADR-057's commits, applying clean with no conflicts** (confirmed:
`git diff --stat HEAD~1 HEAD` shows this pass's own changes touch only
`include/agentengine/rt/agent_session.hpp`, `tests/CMakeLists.txt`, and the new test file — disjoint
regions from ADR-057's own edits to the same header). Against that fully-integrated tree: a full
incremental rebuild (485 build steps this invocation, reusing the prior from-scratch 666-target
cache) completed with **0 errors**, and a full `ctest` sweep of the default, non-Python-gated tree
completed **154/154, 0 failed** (a first sweep run immediately after the big rebuild showed 3
unrelated native-jail OOM-classification failures under heavy transient system load; re-run in
isolation and then as a full clean sweep both confirmed 100% pass — see §5 for the flakiness
diagnosis, not a regression). See §5/§6 below for the full executed evidence and per-claim verdicts.
**One named, unresolved gap remains:** the `AGENTENGINE_BUILD_PYTHON_RUNNER`-gated `build-py` tree
(which includes ADR-057's own `test_agent_session_suspend_codeact_ask`) was NOT rebuilt or re-run
this pass — reasoned low-risk (this pass's only production-file change is additive-only and gated
behind `set_output_schema()` being called, which no `build-py` test does) but not directly confirmed.
This ADR stays **Proposed, not Judged** — per `decisions/README.md`/`OpenQuestions.md` OQ-11, only
the project owner marks an ADR Judged.

**Relates to:** `003-Message-and-Content-Model.md` §4 (the RFC text this ADR either makes true or
corrects), `include/agentengine/core/agent.hpp` (`OutputSchema<T>`, the compile-time tag),
`include/agentengine/core/agent_registry.hpp` (Milestone 5 Phase B5/B6 — schema compilation and
strategy selection, real and tested), `include/agentengine/core/chat_client.hpp`
(`output_schema_strategy`, `select_output_schema_strategy`, `ChatRequest::output_schema_json`),
`include/agentengine/rt/agent_session.hpp` (`run_rounds()`, `AgentResponse`, the live turn loop this
machinery has never been connected to), `026-Agent-Facing-Runtime-Surface.md` §5 (`agent.output`, the
CodeAct module this ADR treats as an explicit follow-on, not this pass's own scope).

## 1. The question, and a correction to the record first

The Milestone 3 breakdown doc and my own earlier read of this codebase both described `agent.output`
as blocked on "003 §4's real structured-output-schema enforcement... an empty placeholder today, not
implemented." **That undersold what already exists and mischaracterized what's actually missing.**
Direct inspection found:

- `OutputSchema<T>` (`core/agent.hpp:111-112`) is a real, working compile-time policy tag — deliberately
  empty, the same idiom `Stateless<N>`/`Tools`/`Middleware` already use, not a stub needing to be
  "filled in."
- `agent_registry.hpp`'s `policy_output_schema<OutputSchema<T>>` compiles a declared `OutputSchema<T>`
  to real JSON Schema text (`schema::json_schema_of<T>()`), and `select_output_schema_strategy()`
  (`chat_client.hpp:228-233`) picks native/tool-shaped/parse-and-repair from a backend's declared
  `ChatClientCapabilities` — both real, both exercised in `test_agent_registry_chat_client_registry.cpp`.
- `ChatRequest::output_schema_json` correctly reaches real OpenAI and Anthropic request bodies —
  proven with an actual nested `OutputSchema<T>` type (not a hand-typed literal) in
  `test_openai_chat_client_translation.cpp` (D4-R3), `test_anthropic_chat_client_translation.cpp`
  (E5-R2), and against a live backend in `test_openrouter_live_e2e.cpp`.

**What's actually true, re-verified directly against the running code, not the RFC's own prose:**

1. **None of this reaches a live turn.** `AgentSession::run_rounds()` builds its request as
   `ChatRequest request{contribution->messages, contribution->tools};` (`rt/agent_session.hpp:1082`)
   — two fields. It never reads an agent's declared `OutputSchema<T>` at all, and `AgentSession` has
   no structural link to `AgentMetadata`/`agent_registry.hpp` anywhere in its own type — the
   compile-time registration pipeline and the runtime session are two fully disconnected subsystems
   today (confirmed: zero hits for `AgentMetadata` in `rt/agent_session.hpp` or `tools/cli_chat.cpp`).
2. **`AgentResponse` has no field to carry a structured result at all.** `struct AgentResponse {
   Message message; Usage usage; };` (`rt/agent_session.hpp:216-219`). There is no run-level concept
   of "the structured output" as data anywhere yet — 003 §4's machinery stops at *constructing a
   request*, never *handling a response*.
3. **No JSON Schema validator exists anywhere in this codebase.** 003 §4's own text claims one
   ("Validation is JSON Schema 2020-12, matching MCP's tool schemas, so one validator serves both") —
   a direct search of `include/`/`src/` for anything JSON-Schema-validation-shaped returns nothing.
   What DOES exist, and is this project's own established "does this JSON match the declared shape"
   idiom used everywhere else (`tool_pipeline.hpp`'s own step-2 comment: *"Deferred to `tool->invoke`'s
   call into `schema::from_json_value<Args>` below — a single point of truth... never a second, hand-rolled
   check"*) is `schema::from_json_value<T>(json::Value const&, field_name) -> result<T>`
   (`core/json_schema.hpp:356`) — a type-driven parse that succeeds or fails, not a generic JSON-Schema
   2020-12 rule engine. Tool-call arguments are never actually checked against a real JSON-Schema
   validator either — 003 §4's "one validator serves both" claim describes infrastructure that does
   not exist for EITHER consumer, not a resource `agent.output` alone is missing out on.
4. **`tool_shaped`/`parse_and_repair` are selected but behaviorally inert.** `select_output_schema_strategy()`
   picks the enum value (used today only for registration-time enforceability checking); nothing
   anywhere branches on `output_schema_strategy_chosen` to actually force a single tool call or run a
   bounded re-ask loop.
5. **`agent.output` (026 §5's CodeAct module, `agent.output.set(value)`) has zero wiring** — and per
   finding 2 above, there is nowhere for it to write TO yet even if it were built, since the run itself
   has no structured-output field.

**Stated so it has a wrong answer:** can `OutputSchema<T>`'s already-proven compile-time/request-
assembly machinery be connected to a live `AgentSession` turn — populating the real request, giving
the run a real place to carry a structured result, and validating what comes back — using this
project's own existing "type-driven parse is the validator" idiom rather than inventing a new generic
JSON-Schema engine, without silently degrading 003 §4's own "one validator serves both" claim into
something it isn't (or, if that claim is what's actually wrong, correcting the RFC text rather than
building unwarranted new infrastructure to make a possibly-overreaching sentence literally true)?

## 2. Designs to red-team

### A — thread `AgentMetadata` itself into `AgentSession`

Give `AgentSession` a constructor parameter or member holding the agent's full `AgentMetadata` (or a
reference/shared pointer to it), reading `output_schema_json`/`output_schema_strategy_chosen` off it
each round.

**Where a red-team should aim:** `AgentMetadata` already carries `tools`/`capability_ceiling`/
`max_turns`/etc. — fields that mostly duplicate what `ContextProvider`'s per-round
`ContextContribution` already assembles fresh (`contribution->tools`, used at `run_rounds()`'s own
existing call site). Threading the whole struct in risks two sources of truth for the same
information (which one wins if they disagree — a real hazard class this project has hit before with
"declared ≠ invocable," ADR-024 §3a/§7), and `AgentMetadata` has no compile-time link to the actual
`T` in `OutputSchema<T>` either (it stores `output_schema_json` as already-erased `std::string`) — so
this design doesn't even solve the type-erased-validator problem in Design B/C below, it just adds
struct coupling for no extra capability.

### B — a narrow, additive opt-in setter (matches this project's own established shape)

`AgentSession` gains `set_output_schema(std::string json, output_schema_strategy strategy,
std::function<bool(std::string_view)> validate)` — the same shape `suspend_for_approval_`
(ADR-029) and `stream_model_calls_` (ADR-034) already use: a plain per-session opt-in, default off,
set once by whoever constructs the session (today, that's `main()`/`register_agent<A>()`'s own
caller, which DOES know the real `T` at the call site and can close over `schema::from_json_value<T>`
in the validator lambda without `AgentSession` itself ever needing to know `T`).

**Where a red-team should aim:** does a type-erased `std::function<bool(std::string_view)>` lose
anything 003 §4 actually needs (e.g. a structured validation ERROR the parse-and-repair strategy would
want to feed back to the model as "here's specifically what was wrong" — a bare `bool` can't carry
that)? Should the validator return `result<void>` (an `error` with a real message) instead of `bool`,
matching this codebase's own `result<T>` idiom everywhere else rather than a degraded boolean? Also:
where does `AgentResponse` put the validated value — a new `std::optional<std::string>
structured_output_json` field (raw, still-erased JSON text — the caller who owns the real `T` parses it a
second time if they want the real type) is the minimal addition; anything richer (storing the actual
parsed `T`) would need `AgentSession` to become templated on `T` too, a bigger, session-type-signature-
widening change worth attacking on its own.

### C — build a real, generic JSON Schema 2020-12 validator

Actually build what 003 §4's text describes literally: a standalone validator against arbitrary
compiled schema text, usable by both tool-argument validation (retrofitted) and `OutputSchema<T>`
response validation.

**Where a red-team should aim:** this is a materially larger, standalone piece of infrastructure —
a real JSON Schema 2020-12 engine (`$ref`, `oneOf`/`anyOf`, `pattern`, numeric bounds, etc.) is not a
small addition, and this codebase has functioned without one so far specifically because
`schema::from_json_value<T>`'s type-driven parse already catches the failure mode that matters for a
C++-typed consumer (does this JSON actually deserialize into the type I declared). A red-team should
ask directly: is there a concrete case type-driven parsing MISSES that a real validator would catch
(e.g. a `std::string` field with no length/pattern constraint the C++ type can't express, where JSON
Schema's `maxLength`/`pattern` would reject something `from_json_value<std::string>` happily accepts)?
If yes, is that gap load-bearing enough to justify Design C's cost, or a named residual? If this design
is rejected, 003 §4's own "one validator... JSON Schema 2020-12" sentence should be corrected (per
CLAUDE.md: "if the spec is wrong, fix the spec first, with an ADR, then the code") rather than left to
silently describe infrastructure that will never exist.

## 3. Deliberately out of scope for this pass

- **`tool_shaped`/`parse_and_repair` real behavior.** Only `native` has a real, tested request-
  translation path today (OpenAI/Anthropic). Building real forced-tool-call and bounded-re-ask
  behavior for the other two strategies is a second, independently-scoped task — naming it as a
  residual here (matching Phase G2/G4's own "narrowed scope, presented as an explicit choice"
  discipline) rather than absorbing it silently.
- **`agent.output` itself (026 §5's CodeAct `agent.output.set()`).** Finding 5 above means this has
  nowhere to write to until `AgentResponse` (or whatever design B/C lands on) actually has a
  structured-output field. Once it does, `agent.output.set()` becomes a second writer into the same
  field — a smaller, well-scoped follow-on, not part of this pass.
- **Streaming.** `run_model_call()`'s streaming path (`stream_model_calls_`) is untouched; whether a
  streamed response can be validated incrementally or only after the stream completes is a separate
  question.

## 4. The red-team attack

Read directly against the real source (no probe compiled this pass — every finding below is a
reading-derived, file:line-cited claim, not executed evidence; §5/§6 stay unfilled per this ADR's
own governance). All of §1's factual claims were independently re-derived, not trusted from the
draft's paraphrase, and all check out: `OutputSchema<T>` is exactly the empty CRTP tag claimed
(`core/agent.hpp:111-112`); `AgentMetadata`'s full field list is `agent_name, agent_instructions,
chat_client_id, tools, capability_ceiling, max_turns, token_budget, approval, concurrency,
telemetry, stateless_pool_size, sandbox_profile, output_schema_json, output_schema_strategy_chosen`
(`core/agent_registry.hpp:54-72`); `ChatRequest`'s full field list is `messages, tools,
output_schema_json, idempotency_key, reasoning_effort` (`core/chat_client.hpp:69-98`); `AgentResponse`
is exactly `{message, usage}` (`rt/agent_session.hpp:216-219`); zero grep hits for `AgentMetadata` in
`rt/agent_session.hpp` or `tools/cli_chat.cpp`, confirmed directly, not inferred.

### Design A — thread `AgentMetadata` into `AgentSession`

**A1 (fatal, as literally proposed).** The draft's own red-team target ("`AgentMetadata` already
carries `tools`/... duplicating `ContextContribution`") is correct and understates the blast radius.
Direct trace of the tools path: `AgentSession::run_rounds()` builds its request exclusively from
`contribution->tools` (`rt/agent_session.hpp:1081-1082`, and the identical pattern at lines 583, 991),
where `contribution` comes from `history_provider_.on_context(...)` — a `ContextProvider` conformer,
templated as `HistoryProviderT`. `AgentMetadata.tools` (`core/agent_registry.hpp:57`, a `ToolTable`
compiled by `register_agent<A>()`) is **never read by `AgentSession` today, anywhere** — confirmed by
the zero-hit grep above. These are not "at risk of drifting" — they are two structurally
**independent, currently fully disconnected** tool tables today; only the `ContextProvider` one ever
reaches a live model call. `cli_chat.cpp:451` builds its own `contribution.tools = scoped.descriptors()`
straight from a hand-assembled `ToolTable`, with no call to `register_agent<A>()` anywhere in that
file (confirmed by grep — zero hits). Threading the *whole* `AgentMetadata` struct into `AgentSession`
puts a second, populated-but-currently-inert `tools` field one drive-by edit away from a caller
reading `meta.tools` somewhere in the round loop instead of `contribution->tools` — silently
reinstating exactly the "declared ≠ invocable" hazard class ADR-024 §3a/§7 already named and fixed for
skill-scoped tool tables (`decisions/ADR-024-skill-scoped-tool-and-mount-wiring.md:52,221`, "declared
and invocable [must] stay derived from the same [source]").

**A2 (fatal, new — beyond what the draft itself flagged).** The hazard is not limited to `tools`. Direct
read of the round loop's own enforcement: `AgentSession::max_turns_` (`rt/agent_session.hpp:1303`,
default-member-initialized, i.e. `std::nullopt` = unbounded) is the value **actually enforced** by the
loop bound at `rt/agent_session.hpp:1067` (`!max_turns_.has_value() || effect_context_.turn_index <
*max_turns_`), set via `initialize()`/`set_max_turns()` (lines 361-368, 397-398). `AgentMetadata.max_turns`
(`core/agent_registry.hpp:59`) defaults to **16** ("002 §3 table default", its own comment) and is
compiled from a possible `MaxTurns<N>` policy. These are two independently-defaulting fields for the
same concept (`nullopt`/unbounded vs. `16`) with zero connection today. The same argument applies to
`capability_ceiling` (`AgentMetadata.capability_ceiling`, a compiled `std::vector<Capability>`) vs.
`AgentSession::capabilities_` (a caller-supplied `CapabilitySet const*` via `set_capabilities()`,
`rt/agent_session.hpp:376-379`) — again two independent sources for "what this run may do." Threading
the full struct in doesn't just risk a *tools* drift, it sits three already-diverged fields (tools,
max_turns, capability_ceiling) directly inside `AgentSession`'s own member list, each one a plausible
future "just read it off `meta_` instead" away from silently overriding the session's actual,
enforced state — a strictly larger attack surface than the draft itself described.

**A3 (confirmed-clean, matches draft).** `AgentMetadata.output_schema_json` is a fully type-erased
`std::optional<std::string>` (`core/agent_registry.hpp:71-72`) with no compile-time link back to the
real `T` in `OutputSchema<T>` — confirmed by reading `agent_detail::compiler<A,...>::run()`
(`core/agent_registry.hpp:457`, `meta.output_schema_json = output_schema_of<Policies...>()`): the `T`
is consumed and erased entirely inside `policy_output_schema<OutputSchema<T>>` before it ever reaches
`AgentMetadata`. So Design A does not even solve the type-erased-validator problem Design B/C exist to
answer — it adds real struct coupling (A1/A2) for a benefit (getting `output_schema_json`/
`_strategy_chosen` into scope) that a two-parameter setter gets identically, without the coupling.

**Verdict: DEFEATED as proposed.** No version of "thread the whole `AgentMetadata` struct in" avoids
A1/A2's hazard — the fields that make it dangerous (`tools`, `max_turns`, `capability_ceiling`) are
exactly the fields Design A's own draft says it *wouldn't* read, which is the argument for not storing
them at all rather than storing-but-not-reading them. The narrow kernel that survives — pulling
`output_schema_json: std::string` and `output_schema_strategy_chosen: output_schema_strategy` out of a
compiled `AgentMetadata` *at the call site* and passing those two values (not the struct) into a setter
— is not actually Design A anymore; it collapses into Design B's own shape (see below).

### Design B — narrow, additive opt-in setter

**B1 (must-fix, factual).** The draft's own claimed precedent — "today, that's `main()`/
`register_agent<A>()`'s own caller, which DOES know the real `T`" — does not describe an existing call
site. Grepped every real (non-test) call site of `register_agent<`:
`tools/policy_reachability_fixture.hpp:158` is the **only** production-code call, and it is a static,
offline `ReachabilityAgent` fixture builder for a policy-reachability oracle
(`build_reference_fixture()`) — it never constructs or configures an `AgentSession`. Every other call
site is inside `tests/`. `tools/cli_chat.cpp` — the one real interactive host in this tree — never
calls `register_agent<A>()` at all (confirmed by grep, zero hits) and has no `OutputSchema<T>`-declaring
demo agent today. So there is currently **no real call site that both compiles an `AgentMetadata` and
constructs a live `AgentSession`** — Design B's "the caller... can close over `schema::from_json_value<T>`"
argument is correct in the abstract (a caller that has both `T` and constructs the session in the same
scope trivially can), but it describes a **new** call site this pass would have to build (a demo agent
+ its wiring in `cli_chat.cpp` or a new test), not an existing one that "would need to change." Low
severity on its own (no regression risk — nothing exists to break), but the ADR should say "new,"
not imply a migration of live code.

**B2 (confirmed-clean).** `AgentSession<ChatClientT, StateT, HistoryProviderT>`
(`rt/agent_session.hpp:350-354`) is not templated on any `T` tied to `OutputSchema<T>` — confirmed by
reading the full class template header. A type-erased `std::function<bool(std::string_view)>` (or
`result<void>(std::string_view)`) validator genuinely does not require widening `AgentSession`'s own
template signature; only "store the actual parsed `T`" (the draft's own named bigger alternative)
would. This claim survives as stated.

**B3 (should-fix, not blocking).** `bool` vs. `result<void>`: correct that a bare `bool` cannot carry
"here's specifically what was wrong," which `parse_and_repair`'s eventual re-ask prompt would want.
But §3 of the draft explicitly defers real `tool_shaped`/`parse_and_repair` *behavior* this pass — no
code built in this pass would consume that richer error yet, so this is not a functional blocker for
THIS pass, only a should-fix for forward compatibility. Recommend `result<void>` anyway: the marginal
type-signature cost over `bool` is effectively zero (`std::function<result<void>(std::string_view)>`
vs. `std::function<bool(std::string_view)>`), and matches this codebase's own `result<T>` idiom used
everywhere else in this file (`start_run`, `resolve_interaction`, every fail-closed branch) rather than
introducing the only boolean-only outcome channel in the class.

**B4 (confirmed-clean).** `AgentResponse` gaining an additive `std::optional<std::string>
structured_output_json` field: grepped every `AgentResponse{...}` positional-aggregate construction in
the tree — exactly one exists, `rt/agent_session.hpp:1118`
(`AgentResponse{response->message, response->usage}`), inside `run_rounds()` itself. C++ aggregate
initialization with fewer braced initializers than members value-initializes the rest, so appending a
third field keeps that one call site compiling unchanged (matches this project's own established
field-ordering discipline — `ChatResponseUpdate::usage`, `ChatRequest::reasoning_effort`,
`Usage::cache_write_tokens`, all cited as precedent by ADR-034 for the identical append-only rule).
Grepped for structured bindings on any `AgentResponse`-typed variable (`auto [x, y] = ...`) — none
exist anywhere in the tree, so there is no "wrong binding count" hazard either.
`protocol/a2a/server.hpp:78-81`'s `RunOutcome::response` is a named field holding an `AgentResponse`,
not an aggregate-positional construction of one — unaffected. **No compile/behavior hazard found.**

**B5 (informational, strengthens the design).** The natural insertion point for the validator call is
concrete and singular: `run_rounds()`'s own convergence branch, `calls.empty()`
(`rt/agent_session.hpp:1110-1118`) — the exact point a round already decides "no more tool calls, this
is the final answer" and builds `AgentResponse`. One well-defined seam, not a design needing to search
for its own hook point.

**Verdict: SURVIVES WITH NAMED FIXES.** Fix B1 (say "new call site," not "existing caller changes"),
adopt B3 (`result<void>` over `bool`, cost is negligible). B2 and B4 hold as stated; B5 is a positive,
unsolicited finding for the "prove" phase.

### Design C — build a real, generic JSON Schema 2020-12 validator

**C1 (fatal to the cost/benefit case).** Direct read of `type_fragment<T>()`
(`core/json_schema.hpp:252-280`), the only function that produces schema *fragments* for
`AE_JSON_SCHEMA`/`Described<>`-generated types: the complete constraint vocabulary it can ever emit is
`{"type": "boolean"|"string"|"integer"|"number"|"array"|"object"}`, plus — only for a field wrapped in
`Described<T, "...">` — an injected `"description"` string (`inject_description`, lines 218-229).
Grepped the entire file for `pattern`, `maxLength`, `minLength`, `minimum`, `maximum`, and a literal
`"enum"` key: **zero matches, all six.** So the draft's own posed question — "is there a concrete case
type-driven parsing MISSES that a real validator would catch" — has a definitive answer from reading
alone: **no**, and not because `from_json_value<T>` is unusually strict. `json_schema_of<T>()` is
*structurally incapable*, under the current `AE_JSON_SCHEMA`/`Described<>` machinery, of emitting
anything a real JSON Schema 2020-12 engine could reject that `from_json_value<T>`'s type-driven parse
(`core/json_schema.hpp:394-443`) doesn't already reject via its own type-mismatch checks. There is no
existing or minimally-constructible in-idiom type where the two disagree, because the schema generator
never emits the extra constraint a validator would need to enforce.

**C2 (must-fix — sharper than the draft's own framing, and it compounds C1's verdict rather than
softening it).** The one case where JSON Schema's own vocabulary could express something type-checking
structurally cannot — an enum's *specific allowed values*, via the `enum:[...]` keyword — is not just
unemitted, it's unenforced on the parse side too, independently: an enum field's `type_fragment<T>()`
degrades to bare `{"type":"integer"}` (`core/json_schema.hpp:264-265`, the `std::is_enum_v<U>` branch
folded into the same arm as plain integral types), and `from_json_value<T>`'s own enum branch
(`core/json_schema.hpp:414-416`) does `static_cast<U>(static_cast<std::underlying_type_t<U>>(v.as_number()))`
with **no range check at all** — any in-range-for-the-underlying-type integer is accepted as a valid
enumerator, in range or not. This means a real JSON Schema 2020-12 validator built and run **against
the schemas this codebase's own generator actually produces** would provide **zero additional
protection over what exists today**, because the schema itself never carries the `enum` constraint that
would let a validator reject an out-of-range value — Design C's cost is therefore not "no matching
benefit today" (the draft's own framing) but "negative-until-a-second, unscoped piece of work (enriching
`type_fragment<T>()`/`AE_JSON_SCHEMA` to emit `pattern`/bounds/`enum` fragments) is *also* built," which
this ADR does not budget for and the draft did not name. (Separately, worth a residual regardless of
which design wins: the enum-accepts-out-of-range-integer gap is a real, narrow, pre-existing correctness
issue in `from_json_value<T>`, independent of `OutputSchema<T>` entirely — every `Described<>`/
`AE_JSON_SCHEMA`-declared enum field in this codebase has it today.)

**Verdict: DEFEATED for this pass.** No in-tree type exercises a real type-driven-parse-vs-JSON-Schema
gap, and the one theoretically real gap (enum range) would survive a Design C validator unchanged
because the schema generator itself would need retrofitting too. Per CLAUDE.md's own rule ("if the
spec is wrong, fix the spec first, with an ADR, then the code"), `003-Message-and-Content-Model.md`
§4's sentence — "Validation is JSON Schema 2020-12, matching MCP's tool schemas, so one validator
serves both" (`003-Message-and-Content-Model.md:111`) — is the thing that should be corrected: this
codebase's actual and, per C1/C2, currently *sufficient* validation idiom is "does this JSON
type-parse into the declared C++ shape" (`from_json_value<T>`), not a generic constraint-rule engine,
and no real gap has been found that would justify building one.

### A fourth design, considered and named (not preferred)

Extend `ContextContribution` (`core/context_provider.hpp:33-37`) with `std::optional<std::string>
output_schema_json` and `std::optional<output_schema_strategy> output_schema_strategy_chosen`,
alongside its existing `tools`. Rationale for even considering it: `ContextProvider` is already, by its
own file-top comment, "the one seam for contribute to the context before the model is called" (005
§5), and `contribution->tools` is already the exact, already-wired per-round path `run_rounds()` reads
into `ChatRequest` (`rt/agent_session.hpp:1081-1082`) — this would put output-schema declaration through
the identical mechanism as tool declaration, rather than adding a second, structurally different
parallel setter living beside it (Design B). **Rejected as not preferred, not fatal:** (a) it doesn't
avoid the "who supplies the compiled schema string" question either — some caller still has to build
`ContextContribution.output_schema_json` from somewhere, the identical question Design B answers with a
closure at the setter call site; (b) unlike `tools`, an output schema is a per-*run*, not per-*round*,
property — recomputing/copying it every round via `on_context()` (called fresh each iteration of
`run_rounds()`'s loop) is a wasted `std::string` copy per round, a design smell even if cheap; (c)
`ContextProvider`'s own contract (029 §4) is scoped to history/skills/memory-contribution — stretching
it to also declare the expected *shape of the response* is scope creep on an existing seam, arguably a
worse fit than Design B's dedicated, clearly-named, opt-in setter. Named here so it isn't silently
absent from the record, not adopted.

### Design lean, updated

The original draft leaned toward B "without having attacked any of them." Having attacked all three
(plus a fourth): **Design A is defeated as proposed** (A1/A2 — a real, larger-than-drafted duplication
hazard); **Design C is defeated for this pass** (C1/C2 — no matching benefit exists in this codebase's
actual schema-generation machinery, and 003 §4's text is the artifact that should change, not the
code); **Design B survives**, with two named fixes (B1: describe it honestly as a new call site, not
an existing one changing; B3: prefer `result<void>` over `bool` for idiom-consistency, not functional
necessity). The fourth design (extend `ContextContribution`) survives as a documented alternative but
is not preferred, on (b)/(c) above. **Recommendation for the "prove" phase: build Design B, corrected
per B1/B3, with the insertion point identified at B5** (`run_rounds()`'s `calls.empty()` convergence
branch, `rt/agent_session.hpp:1110-1118`, all three call sites of the pattern), and separately open an
ADR against `003-Message-and-Content-Model.md` §4 to replace "Validation is JSON Schema 2020-12...
one validator serves both" with language matching what C1/C2 found is actually enforced.

## 5. Executed evidence

**Run** (this pass, prove phase, 2026-08-14). Real code implementing §8's plan was written in
`include/agentengine/rt/agent_session.hpp` (the only production file this pass touched) plus a new
`tests/test_agent_session_output_schema.cpp` (O1-O5), registered in `tests/CMakeLists.txt` immediately
after `test_rt_agent_session_suspend_approval`, in the default (non-`AGENTENGINE_BUILD_PYTHON_RUNNER`)
`build` tree — matching that test's own link set exactly (`agentengine::core` + `agentengine_warnings`
only).

**A real plan correction made during this pass, not silently smoothed over — §8's own "three
`ChatRequest{...}` construction sites" claim does not describe the code as it exists in the checkout
this pass actually built and tested against.** §8 cited `rt/agent_session.hpp:583`, `:991`,
`:1081-1082` as the three sites needing the native-only-scoping treatment, carried over from §4 A1's
own file:line citations. Re-grepping the ACTUAL file for `ChatRequest` (not trusting the carried-over
citation) found only **one** `ChatRequest{...}` construction site in the whole file — inside
`run_rounds()` (`ChatRequest request{contribution->messages, contribution->tools};`). The other cited
line is `ToolTable const tool_table = ToolTable::from_descriptors(contribution->tools);` — a
*different* statement one line above the `ChatRequest` construction the original A1 finding was
citing — inside `resolve_interaction()`'s own `reason == approval` branch. That branch never
constructs a `ChatRequest` or calls `run_model_call()` at all: it re-invokes the pending call directly
via `invoke_tool()` and then recurses into `run_rounds()` itself (`co_return co_await run_rounds();`)
— the ONE place a fresh model call, and therefore a fresh `ChatRequest`, is ever built.

**A second finding while chasing this down, reported honestly rather than smoothed over at the time,
now RESOLVED — recorded here for the record rather than deleted.** The isolated worktree this pass's
code was originally written and tested in did NOT contain ADR-057's `codeact_ask` machinery (a direct
grep for `codeact_ask`/`resolve_codeact_ask`/`kSuspendedForCodeActAsk` returned zero hits at that
point) — a worktree-isolation artifact: this pass's branch had not yet been rebased onto the commits
carrying ADR-057's work. **This is now resolved.** The worktree branch was rebased onto current `main`
(which includes both ADR-057 commits, `8be3ca8` "ADR-057: implement and prove agent.ask Design B" and
`a1693fd`, its README-index follow-up) — the rebase applied **clean, with no conflicts**: `git diff
--stat HEAD~1 HEAD` on this pass's own commit shows it touches only `include/agentengine/rt/
agent_session.hpp`, `tests/CMakeLists.txt`, and the new test file, and a direct grep confirms
`codeact_ask`/`resolve_codeact_ask`/`kSuspendedForCodeActAsk` are now all present in the rebased
`agent_session.hpp`, alongside this pass's own `set_output_schema`/`output_schema_validate_`/
`structured_output_json` additions, at the same time. The single real `ChatRequest{...}` construction
site (confirmed by `git show 8be3ca8 --stat`: ADR-057 never touches `native_jail_backend.cpp`/
`job_object_limits.*`, and by direct grep on the rebased file) now sits at
`rt/agent_session.hpp:1113` — exactly where the coordinator's own independent check placed it.
**Correction applied, and now verified against the fully-integrated tree, not merely reasoned about
it:** the native-only request-side scoping (§8) is implemented at the ONE real `ChatRequest{...}`
construction site that exists in the codebase — `resolve_interaction()`'s approval-resume branch never
builds one, exactly as A1's own reasoning predicts, and this holds with ADR-057's `codeact_ask` branch
present too (that branch resumes a pending call and recurses into `run_rounds()`, same shape as the
approval branch — it does not build its own `ChatRequest` either). **The build/test evidence below is
now for the fully-integrated, rebased tree — it supersedes an earlier, narrower pre-rebase evidence
paragraph that stood here before the rebase.**

**Second finding, resolving §8's own named open sub-question — does either real backend behave unsafely
if `output_schema_json` were set while `ChatClientCapabilities.structured_output_native` is false?**
Checked directly, not assumed: `protocol/openai/chat_client.hpp:289-293`'s `build_request_body()` and
`protocol/anthropic/chat_client.hpp:409-413`'s equivalent both do
```
if (request.output_schema_json) { ... translate_output_schema(*request.output_schema_json) ...
    obj.emplace_back("response_format", ...); }
```
— an unconditional check on `request.output_schema_json` alone, with **no gate on
`ChatClientCapabilities.structured_output_native`** anywhere in either function. (Contrast: the SAME
function, `build_request_body()`, DOES gate `reasoning_effort` against `caps` — 004 §2's degradation
rule — proving the capability-gate pattern exists in this file and was deliberately NOT applied to
`output_schema_json`.) **Verdict: the native-only request-side scoping in `AgentSession` is a real,
load-bearing safety necessity, not belt-and-suspenders redundancy.** If `AgentSession` populated
`request.output_schema_json` regardless of the selected strategy, and that request reached either real
backend wired to a `ChatClientT` lacking real native structured-output support, the backend would still
serialize `response_format`/`output_config` onto the wire unconditionally — a field the provider has no
declared support contract for, which the backend itself does nothing to prevent. `AgentSession`'s own
`output_schema_strategy_ == native` gate (`rt/agent_session.hpp`, the single `ChatRequest{...}` site) is
therefore the ONLY place in the whole call path that stops this from happening — removing it would be a
real defect, not a redundant guard on top of a guard that already exists downstream.

**Build.** Configured and built in Release with Ninja + an explicit `vcvarsall.bat x64` environment
(the same generator-front-end substitution ADR-057 §5 already documents for this environment — CMake
4.1.1 doesn't resolve the installed VS 18 toolset by a `-G "Visual Studio ..."` name):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
```

**New test target — direct run:**

```
./build/tests/test_agent_session_output_schema.exe
```

Result: **all 17 checks pass** (O1 through O5, every named `check()` in the file — see the per-claim
table in §6). Exact captured output:

```
  ok: O1: a schema-valid response converges without error
  ok: O1: structured_output_json is populated
  ok: O1: structured_output_json holds the EXACT scripted text, not a re-serialization
  ok: O2: a schema-invalid response fails the run closed
  ok: O2: the failure is specifically run.output_schema_validation_failed
  ok: O3 setup: has_output_schema() is false when unconfigured
  ok: O3: with no set_output_schema(), a non-JSON response STILL converges -- the validator never ran, proving this path is additive, not a regression on the ordinary case
  ok: O3: structured_output_json stays unset when no schema was ever declared
  ok: O3: the converged response is the scripted text, unmodified
  ok: O4 setup: the run converges
  ok: O4: exactly one ChatRequest reached the scripted ChatClientT
  ok: O4: under output_schema_strategy::native, the real ChatRequest carries output_schema_json
  ok: O4: the carried text is the exact compiled schema, not a placeholder
  ok: O5: the run still converges under tool_shaped (real forced-tool-call behavior is a named residual, ADR-058 §3 -- only the request-side scoping and response-side validation are under test here)
  ok: O5: exactly one ChatRequest reached the scripted ChatClientT
  ok: O5: under output_schema_strategy::tool_shaped, the real ChatRequest does NOT carry output_schema_json -- the deliberate native-only request-side scoping
  ok: O5: the RESPONSE is still validated (and structured_output_json still populated) the same way as O1 -- only the request-side carriage differs by strategy
test_agent_session_output_schema: ALL PASS
```

**Rebuild against the rebased, fully-integrated tree.** After the rebase (above), a full incremental
rebuild was run — the existing `build/` tree's object-file cache from before the rebase was reused, so
Ninja recompiled/relinked only what the rebase's new content (ADR-057's own changes plus this pass's
own) actually touched, not from scratch:

```
cmake --build build -j 4
```

**Result: 485 build steps executed this invocation, 0 errors, exit code 0.** Warning count: 81 total
(63 `C4834` "discarding [[nodiscard]] return value", 15 `C4189` unused-local, 3 `C4996` `getenv`) —
`agent_session.hpp` itself now carries the `C4834` pattern at 5 distinct call sites (`:595`, `:637`,
`:1082`, `:1157`, `:1309` in the rebased file), one more than the 4 sites this pass's own pre-rebase
evidence recorded — the extra site is ADR-057's own new `codeact_ask`-resume branch's `on_turn_end`
call, an existing warning class from ADR-057's own work, not a new class this pass introduced. **No
new warning class anywhere in the tree from this pass's changes.**

Then the full registered test suite of the default (non-Python-gated) tree was run to completion:

```
ctest --output-on-failure -j 4        # from build/, immediately after the rebuild above
```

**First result: 151/154 passed, 3 failed** (`test_native_jail_backend_windows`,
`test_native_jail_abuse_corpus_windows`, `test_native_jail_parity_windows`, all in 94.42s total —
roughly 3-8× longer per-test than either this pass's earlier pre-rebase run or the clean re-run below,
strong direct evidence of transient system load right after a large rebuild, not a steady-state
number). All three failures were the identical class — `outcome->klass == exec_outcome_class::oom`
not observed when a script intentionally exceeds `memory_bytes` — a Windows Job Object memory-limit
classification whose accuracy is inherently sensitive to real system memory pressure at the moment
the test runs. Investigated rather than dismissed: `git show --stat 8be3ca8` (ADR-057's own commit)
confirms it touches neither `src/backends/native_jail/native_jail_backend.cpp` nor
`job_object_limits.*` — the files these three tests actually exercise — and this pass's own changes
are confined to `rt/agent_session.hpp`/`tests/CMakeLists.txt`/the new test file, nowhere near the
native-jail backend either. **Re-run in isolation** (`ctest -j 1 -R
"test_native_jail_backend_windows|test_native_jail_abuse_corpus_windows|test_native_jail_parity_windows"`,
away from the other 151 tests' resource contention): **3/3 passed**, at 9.23s/8.70s/13.71s — within
noise of this pass's own original pre-rebase timings for the same three tests (9.00s/8.72s/13.87s).
**Then a full, clean `-j 4` re-run of the entire suite** (same command as above, run again once system
load had settled): **154/154 passed, 0 failed, in 19.23s** — the whole sweep this time faster than the
single flaky run's total, corroborating the transient-load diagnosis rather than a real regression.
**Verdict: the one non-clean sweep was environmental flakiness under post-rebuild system load, not a
regression from ADR-057's rebase or this pass's own changes — demonstrated by isolation and by a
clean full re-run, not merely asserted.**

The clean, final, authoritative sweep (`154/154, 0 failed, 19.23s`) includes, by exact name from the
real ctest log:

- `test_agent_session_output_schema` (this pass's own new O1-O5 target, test #67) — **Passed**
  (0.02s). Re-run directly as a standalone executable against the rebased tree for the exact
  per-check record: all 17 checks pass, byte-for-byte identical output to the pre-rebase run.
- `test_rt_agent_session_suspend_approval` (ADR-029's own SU1-SU6 suite, test #66) — **Passed**
  (0.03s) — confirms this pass's additive changes to `AgentSession` left ADR-029's own suite
  unaffected, now verified with ADR-057's `codeact_ask` machinery genuinely present in the same
  binary (not merely reasoned about, per the resolved gap above).
- Every other test in the default `build` tree's 154-test registry — **all Passed**. No test was
  skipped, disabled, or reported "Not Run".

This is a complete, honestly-scoped sweep of the tree this pass's changes could plausibly affect (the
default, non-Python-gated `build` tree — the tree `test_rt_agent_session_suspend_approval` itself is
registered in). The `AGENTENGINE_BUILD_PYTHON_RUNNER`-gated `build-py` tree (CPython/mediated-runner/
CodeAct tests, including ADR-057's own `test_agent_session_suspend_codeact_ask`) was **not** rebuilt or
re-run this pass — a real, named residual, not silently assumed clean. Reasoning for why this is
low-risk rather than a skipped obligation: every change to `rt/agent_session.hpp` this pass made is
additive-only (new members with safe defaults, one new `if` gate at the single `ChatRequest{...}`
site, and a new branch inside the existing `calls.empty()` arm that is unreachable unless
`set_output_schema()` was called, which no `build-py` test does) — but "low-risk by reasoning" is
explicitly weaker than "run and confirmed," and is reported as such.

## 6. Per-claim verdicts

Decided by observed output (the test run in §5), not argument, matching `decisions/README.md`'s own
standard.

| Claim | Verdict | Evidence |
|---|---|---|
| O1 — a scripted response whose text is valid JSON matching the declared schema: the round converges, `AgentResponse::structured_output_json` is populated with the exact text, no error | **CORRECT** | All 3 O1 checks pass, including the exact-string equality check against the scripted text (not a re-serialization through `DemoOutput`). |
| O2 — a scripted response whose text does NOT match the schema (fails `schema::from_json<T>`/`schema::from_json_value<T>`): the run fails closed with `run.output_schema_validation_failed`, `structured_output_json` stays unset | **CORRECT** | Both O2 checks pass: the run fails (`outcome.has_value()` false) and the error code is exactly `run.output_schema_validation_failed`. There is no `AgentResponse` at all on this path (the run itself failed), so "stays unset" is structurally true, not merely unobserved. |
| O3 (positive control) — a session with no `set_output_schema()` call at all behaves byte-for-byte as before this change: no validator ever runs, `structured_output_json` always unset | **CORRECT** | All 4 O3 checks pass, including the deliberately-adversarial construction: the scripted response text (`"plain prose, not JSON, not schema-shaped"`) would FAIL validation if the validator ran at all, and the run still converges — proving the validator genuinely never executes when unconfigured, not merely that a lucky input happened to pass. |
| O4 — with `output_schema_strategy::native` set, the real `ChatRequest` reaching the scripted `ChatClient` actually carries `output_schema_json` | **CORRECT** | All 4 O4 checks pass, including the exact-string check that the carried text is the real compiled `schema::json_schema_of<DemoOutput>()`, not a placeholder — proven via a new `received_requests()` recorder added to the scripted `ChatClientT` test double specifically for this claim (the approval test's own scripted client did not capture requests; this file extends the pattern rather than reusing it as-is). |
| O5 — with `output_schema_strategy::tool_shaped` set, the real `ChatRequest` does NOT carry `output_schema_json`, while the response is still validated the same way as O1 | **CORRECT** | All 4 O5 checks pass: the recorded request's `output_schema_json` is confirmed `nullopt`, AND (in the same test) the run still converges with `structured_output_json` populated with the exact scripted text — both halves of the claim demonstrated in one scenario, not asserted separately. |

**Overall verdict for this pass: Design B, as concretely specified in §8 (with the one correction
above — one real `ChatRequest{...}` construction site, not three), is CORRECT and BUILDABLE against
the real runtime as it exists today — verified against the fully-integrated tree, ADR-057's
`codeact_ask` machinery genuinely present, not merely reasoned about.** All five committed claims
(O1-O5) are CORRECT, the full 154-test default-tree suite (including the ADR-029 precedent suite this
design's own shape borrows from, and now genuinely co-resident with ADR-057's own work in the same
binary) passes with zero regressions once transient post-rebuild system load settled (§5's flakiness
diagnosis, confirmed by isolation and a clean re-run, not merely asserted), and the ADR's own open
sub-question (does either backend behave unsafely if `output_schema_json` reaches it against a
non-native-capable `ChatClientT`) has a direct, checked answer: **yes, both backends serialize it
unconditionally with no capability gate of their own**, making `AgentSession`'s native-only
request-side scoping a real safety mechanism, not a redundant one.

**What this pass does NOT establish**, named rather than silently assumed: (1) `tool_shaped`/
`parse_and_repair` real enforcement behavior (forced single tool call, bounded re-ask) remains exactly
as inert as §3/§4 (C-verdict) already named — O5 proves the request-side scoping and response-side
validation around `tool_shaped`, not that `tool_shaped` itself does anything differently on the
request side beyond omitting the field; (2) the `AGENTENGINE_BUILD_PYTHON_RUNNER`-gated `build-py`
tree (ADR-057's own scope, `test_agent_session_suspend_codeact_ask` included) was not rebuilt or
re-run this pass — a named, low-risk-by-reasoning but not directly-confirmed residual (§5), now the
SOLE remaining unverified gap since the earlier worktree/ADR-057 divergence was resolved by the
rebase; (3) 003 §4's RFC text was already corrected to match the C1/C2 finding (type-driven parsing,
not a generic JSON-Schema 2020-12 engine) BEFORE this pass began, by a prior editing pass — this
pass's own code and tests match that corrected text, but did not itself author the correction; (4)
`agent.output` (026 §5's CodeAct module) remains entirely unwired, as §3 named up front —
`AgentResponse::structured_output_json` now exists as a place for a future `agent.output.set()` to
eventually write to, but nothing in this pass builds that writer.

## 7. Residuals to name up front, regardless of which design survives

- Whether a validation failure under `native` strategy (the provider's own constrained decoding
  should make this rare, but not impossible) should fail the run closed, or silently fall through
  un-validated — 003 §4 doesn't say, and this ADR shouldn't invent behavior 003 doesn't specify
  without flagging it back to the RFC. **Resolved by this pass's own implementation choice: fails
  closed, unconditionally, regardless of strategy** (§8's own plan, confirmed as-implemented by O2).
- Trace/telemetry recording of which strategy actually fired for a given run (003 §4: "recorded in
  the trace") — not attempted here unless it falls out for free from Design B's chosen shape. **Not
  attempted this pass either** — `output_schema_strategy_` is recorded on the session as a plain
  member, readable by a caller, but nothing in this pass wires it into `RunEvent`/telemetry.
- The `build-py` tree regression sweep (§5's own named residual, above) — reasoned low-risk, not
  directly confirmed this pass.
- **RESOLVED — the worktree/checkout divergence originally named here.** This pass's code was
  initially built and tested against an isolated worktree whose `rt/agent_session.hpp` did not yet
  contain ADR-057's `codeact_ask` machinery (a worktree-isolation artifact — the branch had not been
  rebased onto the commits carrying that work). The worktree branch has since been rebased onto
  current `main` (both ADR-057 commits included); the rebase applied clean, with no conflicts, and
  the full build/test evidence in §5/§6 above is now against that fully-integrated tree — confirmed,
  not merely reasoned about. Kept here, marked resolved, rather than deleted, matching this project's
  own "residuals named, not silently erased once closed" discipline.
- Real `tool_shaped`/`parse_and_repair` enforcement (forced tool call, bounded re-ask) — unchanged,
  exactly as inert as §3 originally scoped it.
- `agent.output` (026 §5) wiring onto the now-real `structured_output_json` field — unchanged, exactly
  as out-of-scope as §3 originally scoped it.

## 8. Concrete mechanism for Design B (prove-phase plan, written before implementation)

Grounded in the red-team's own B1-B5 findings and the corrected `003-Message-and-Content-Model.md`
§4 text (already edited, this pass).

- **`AgentSession` gains an additive opt-in setter**, matching `suspend_for_approval_`/
  `stream_model_calls_`'s existing shape exactly:
  `set_output_schema(std::string json, output_schema_strategy strategy,
  std::function<result<void>(std::string_view)> validate)`, storing
  `output_schema_json_`/`output_schema_strategy_`/`output_schema_validate_` (all empty/unset by
  default — every existing caller is unaffected, matching this codebase's own additive-field
  discipline B4 already confirmed for `AgentResponse`).
- **Request-side population is scoped to `native` only, deliberately.** `ChatRequest::output_schema_json`
  gets populated from `output_schema_json_` **only when `output_schema_strategy_ == native`** — the
  one strategy with a real, tested translation path (OpenAI/Anthropic). For `tool_shaped`/
  `parse_and_repair`, §3 already defers real behavior; leaving `request.output_schema_json` unset for
  those means no backend receives a field it has no support contract for, and `output_schema_strategy_`
  stays recorded on the session purely for observability/future wiring, not silently promising
  enforcement this pass doesn't build. **Open sub-question for the prove pass to settle empirically,
  not guessed here:** does either real backend's translation code (`protocol/openai/chat_client.hpp`,
  the Anthropic equivalent) behave safely if `output_schema_json` were ever set against a backend
  whose `ChatClientCapabilities.structured_output_native` is false — is this scoping a defensive
  necessity or a belt-and-suspenders redundancy? Check directly before assuming either answer.
  **RESOLVED, §5:** neither backend gates on `structured_output_native` at all — both translate and
  serialize `output_schema_json` unconditionally whenever it is set. The scoping is a real necessity.
- **All three `ChatRequest{...}` construction sites** in `run_rounds()` (the red-team's B1 finding
  location plus the two other identical-pattern sites it flagged, `rt/agent_session.hpp:583`, `:991`,
  `:1081-1082`) need the same treatment — not just the one this ADR happened to cite first.
  **CORRECTED, §5:** re-derived directly against the real file rather than trusted from this
  citation — there is exactly ONE `ChatRequest{...}` construction site in the codebase
  (`run_rounds()`'s own, `rt/agent_session.hpp:1113` in the final, rebased tree). The other cited
  line is a `ToolTable const tool_table = ...` statement in `resolve_interaction()`'s approval-resume
  branch, which never constructs a `ChatRequest` — it resumes a pending tool call directly and
  recurses into `run_rounds()`, which is where the single `ChatRequest` gets built. §5 originally
  named, while it was still true, that this pass's worktree did not yet contain ADR-057's
  `codeact_ask` machinery; that gap has since been resolved by rebasing onto current `main` (§5/§7),
  and this "one real site, not three" finding holds unchanged with `codeact_ask` genuinely present —
  its own resume branch has the identical shape (resume-and-recurse, no fresh `ChatRequest`) as the
  approval branch.
- **Validation runs at the existing convergence branch** (`calls.empty()`,
  `rt/agent_session.hpp:1110-1118`, B5's identified seam) — the exact point a round decides "no more
  tool calls, this is the final answer." Applies **regardless of which strategy was chosen** (native,
  tool-shaped, or parse-and-repair all get validated the same way; only whether the REQUEST carried a
  native constraint differs). Calls `output_schema_validate_(response->message`'s text content`)` when
  set; on success, populates the new `AgentResponse::structured_output_json` field (raw JSON text --
  the caller who owns the real `T` parses it a second time via `schema::from_json_value<T>`, matching
  B2's finding that `AgentSession` itself never needs to know `T`).
- **A validation failure fails the run closed** — a new error code
  (`run.output_schema_validation_failed`), never a silent pass-through of unvalidated text as if it
  were the structured result. Named explicitly per §7's own residual: 003 §4 doesn't specify this
  behavior, so this is a deliberate, documented choice (fail closed, matching this codebase's
  established idiom throughout `run_rounds()`), not an invented RFC requirement — flag back to 003 §4
  if a future pass wants `parse_and_repair`'s real bounded-reask instead of a hard failure.
- **`AgentResponse` gains `std::optional<std::string> structured_output_json`** — additive, confirmed
  safe by B4 (exactly one positional-aggregate construction site in the whole tree).

### A new, real call site is required (per B1) — not a migration

Since no production code calls both `register_agent<A>()` and constructs a live `AgentSession` today,
the prove pass needs to build one to exercise this for real: a small demo agent declaring
`OutputSchema<T>` for a simple `T` (e.g. a two-field struct with `AE_JSON_SCHEMA`), wired through
either a new test (matching `tests/test_agent_session_suspend_approval.cpp`'s own deterministic,
offline, scripted-`ChatClient` style) or a small addition to `cli_chat.cpp` — the test route is
preferred, since `cli_chat.cpp` has no existing `OutputSchema<T>`-declaring agent and adding one
there is a bigger, separately-motivated change than this ADR's own scope. **AS IMPLEMENTED, §5:** the
test route was taken, exactly as preferred — `tests/test_agent_session_output_schema.cpp`, a `DemoOutput`
two-field `AE_JSON_SCHEMA` struct standing in for the real `T`, no `register_agent<A>()`/`AgentMetadata`
involved at all (Design B's own point: `AgentSession` never needs `AgentMetadata`, only the two erased
values plus a closure).

### Tests this plan commits to before calling it proven (deterministic, offline, no live model)

- **O1** — a scripted response whose text is valid JSON matching the declared schema: the round
  converges, `AgentResponse::structured_output_json` is populated with the exact text, no error.
- **O2** — a scripted response whose text does NOT match the schema (fails
  `schema::from_json_value<T>`): the run fails closed with `run.output_schema_validation_failed`,
  `structured_output_json` stays unset.
- **O3 (regression/positive control)** — a session with no `set_output_schema()` call at all behaves
  byte-for-byte as before this change (no validator ever runs, `structured_output_json` always
  unset) — proves the new path is additive, matching O3's precedent role in this project's own test
  style (e.g. ADR-057's B5).
- **O4** — with `output_schema_strategy::native` set, the real `ChatRequest` reaching the (test-double)
  `ChatClient` actually carries `output_schema_json` — proves the request-side wiring, not just the
  response-side validator.
- **O5** — with `output_schema_strategy::tool_shaped` (or `parse_and_repair`) set, the real
  `ChatRequest` does NOT carry `output_schema_json` (proves the deliberate native-only request-side
  scoping), while the response is still validated the same way as O1/O2.

**All five: implemented and run, §5/§6 — all CORRECT.**
