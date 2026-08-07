# 006 — Tool and Function Plane

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 003, 007, 008, 009, 011, 012, 013, 019 · **Gate:** §8

## Goal

One declaration, one invocation path, one approval model, one telemetry shape — for tools whose
implementations are wildly different: a native C++ function, a WASM plugin, an MCP server tool, a
remote A2A agent, a sandboxed script.

## 1. Tool declaration

```cpp
struct WebSearch : Tool<WebSearch,
        Capabilities<NetOut<"api.search.example">>,
        Approval<Mode::NeverRequire>,
        Parallelizable,
        EffectClass<effect_class::pure>,
        Timeout<30s>> {
    static constexpr std::string_view name = "web_search";
    static constexpr std::string_view description = "Search the web for a query.";

    struct Args  { std::string query; int max_results = 5; };   // → JSON Schema 2020-12
    struct Reply { std::vector<SearchHit> hits; };

    static ae::task<result<Reply>> invoke(Args, EffectContext&);
};
```

- **Schemas are derived from the argument/result types** at compile time (Quark 016's one-`describe`
  discipline), emitted as JSON Schema 2020-12 — the same dialect MCP requires (011), so a tool is
  MCP-publishable without a second schema.
- **A field may carry a per-parameter description**, `Described<T, "text">` (`core/json_schema.hpp`),
  wrapping the field's declared type — `Described<std::string, "The search query text"> query;` —
  rather than a second macro syntax or a change to `AE_JSON_SCHEMA`'s own call sites. Both OpenAI's
  and Anthropic's real `tools` wire formats carry a `description` on every parameter, not only on the
  tool itself; an undescribed (plain-typed) field emits none, byte-for-byte unaffected. Composes with
  `std::optional<T>` in either order the wrapper is applied — `Described<std::optional<T>, "...">` is
  still detected as not required. Proven in `tests/test_json_schema_described.cpp`, including the
  description surviving both backends' real `translate_tool()` end to end.
- **`EffectContext` is mandatory** in the signature. There is no ambient-context accessor, because
  I4 requires attribution at the point of effect and I2 forbids ambient authority.
- **Capabilities are declared on the tool**, and the agent's ceiling must cover them (002 §6), so an
  unauthorized tool fails at startup rather than at 3 a.m.
- **`EffectClass<effect_class::{pure, idempotent, at_most_once}>` declares repeat-safety** — the
  vocabulary 019 §3 defines for exactly-once effects, read by 019 §6 to gate re-execution on rewind:
  `pure` re-runs freely, `idempotent` re-runs under its derived idempotency key
  (`{run_id, turn_index, call_index, argument_digest}`, 019 §3), `at_most_once` requires explicit
  operator acknowledgement before any re-run. **Omitting `EffectClass<...>` defaults to
  `at_most_once`** — the same empty-by-default posture 007 §3.1 already takes for capabilities: a
  tool author must actively claim looser replay semantics, never receive them by omission, so a tool
  its author forgot to classify is never silently treated as safe to repeat. A read-only,
  no-side-effect tool — including a UI-rendering/report/visual tool that only draws an artifact from
  already-known state — is the canonical `pure` case; a payment or any other external mutation that
  must not double-apply is the canonical `at_most_once` case. Implemented and proven:
  `EffectClass<C>`/`effect_class` and `declared_effect_class()`'s conservative default in
  `include/agentengine/core/tool.hpp` and `tool_pipeline.hpp`; `authorize_reexecution()`'s three-way
  gate in `tool_pipeline.hpp`; all three cases covered end-to-end, including the undeclared-default
  case, by `tests/test_effect_reexecution.cpp`.

## 2. Tool sources

| Source | Binding | Isolation |
|---|---|---|
| **Native** | Compiled in-process function | Host trust — reserved for first-party code |
| **WASM plugin** | WIT world `tool` (009) | Component sandbox, capability-scoped |
| **MCP server** | Discovered from `tools/list` (011) | Process/network boundary + remote trust |
| **Remote agent** | A2A skill (012) | Network boundary + remote trust |
| **Sandboxed script** | Code interpreter (010) | Sandbox profile (008) |
| **Composite** | Workflow exposed as a tool (014) | Inherits its nodes' |

**Uniformity rule:** the model sees one tool list; the author writes one declaration syntax; the
engine emits one span shape. Source appears in metadata and policy, never in the calling convention.

**The same rule binds across *frontends*, not only across sources.** A domain-level operation
reachable from more than one place an agent can act from — the model's tool-call channel,
`agent.tools` inside CodeAct (026 §4), `ShellRunner`'s dispatch (010 §2) — is **exactly one Tool
implementation**, never two that merely agree by convention. `grep`-in-a-shell-pipeline and
`agent.tools.search(...)` in Python must resolve to the same registered Tool if both exist, not to
independently written code that happens to behave similarly today and silently diverges later. This
does **not** pull `ShellRunner`'s cheap, worktree-native builtins (`cd`, `ls`, `cat`, and similarly
ordinary commands, 010 §2) into the tool pipeline — those stay builtins deliberately, because paying
the full ten-step pipeline (§3) for listing a directory is overhead with no matching benefit. The
rule applies once an operation is rich enough to be Tool-shaped in the first place (search, diff,
document/spreadsheet/PDF/image manipulation, and anything in 009 §7's library track): at that point
there is one implementation, and every frontend that reaches it reaches the same one.

## 3. Invocation pipeline

Every tool call, regardless of source, traverses exactly this pipeline:

```
1. resolve       name → declaration      (unknown name → Contract error, never a guess)
2. validate      arguments vs schema     (reject; do not coerce)
3. taint         arguments carrying model-originated content are tainted (003 §2)
4. authorize     capability check against the run's set          (007)
5. approve       policy → auto | require approval → InputRequired (001 §2)
6. admit         rate limit, concurrency, quota                  (Quark 022)
7. bind          materialize capability handles for this call only
8. invoke        with deadline + stop_token, in the declared isolation — may emit interim
                 progress via `EffectContext.report_progress` (§6a); a `Backgroundable` tool
                 may return without waiting on completion (§6b)
9. normalize     result → parts (003); errors → structured ToolResult{is_error}
10. account      usage, duration, bytes; span closed; audit record written
```

**Non-negotiable properties:**

- **No step is skippable by configuration.** A "fast path" that bypasses 4, 5, or 10 does not exist.
- **Argument validation rejects, never coerces.** Coercion is how a `path` becomes `../../etc`.
- **Capability handles are per-call and revoked at step 10.** A tool cannot retain authority beyond
  its invocation; a handle outliving its call is a defect class with a dedicated test.
- **A tool error is a value** returned to the model (001 §6), not an exception and not a run abort.

## 4. Approval

The vocabulary is deliberately MAF's — `never_require` / `always_require`, with a `PolicyDriven`
mode added — because it is well understood and maps onto the human-in-the-loop mechanisms of both
protocols we speak (MCP MRTR, A2A `INPUT_REQUIRED`).

- **Escalation is conservative and pre-execution.** For a batch, if any tool requires approval, the
  batch requires approval. Approval is granted before execution, over the *concrete, validated
  arguments* the user is shown.
- **Approval is bound to the exact call.** The approved payload is hashed; a mismatch at execution
  is a `Policy` failure. Approve-then-substitute is a real attack, not a hypothetical one.
- **PolicyDriven** consults declared policy over `{tool, capability set, argument predicates,
  principal, taint}` — e.g. auto-approve reads under a mounted workspace, require approval for any
  write outside it, always require approval for network egress to a host not in the allowlist.
- **Approvals never come from a model** (I3). A model may *request*; only a host policy or a human
  may *grant*.

## 5. Concurrency

Sequential by default (001 §4). A batch is parallel only when **every** call in it is
`Parallelizable`. `Parallelizable` is a claim about external effects — that concurrent execution
with other tools is observably equivalent to some sequential order — and declaring it on a
state-mutating tool is a defect, not a tuning choice.

Parallel batches still serialize their **result append** in the model's emitted order, so the
history is deterministic regardless of completion order — a precondition for I5 replay.

## 6. Tool discovery and dynamic tool sets

- **Static** tools come from `Tools<...>` (002).
- **Dynamic** tools come from MCP servers, A2A peers, loaded skills, and **context providers**
  (005 §5 — a provider's `ContextContribution.tools`, the precedent being MAF's `TextSearchProvider`
  exposing an on-demand search tool). All are resolved at run start into an immutable per-run tool
  table (snapshotted, like MAF's per-run tool surface), so a mid-run change to a remote server or a
  provider's own state cannot alter what a run is allowed to do.
- **Caching:** MCP list results carry `ttlMs` and `cacheScope` (2026-07-28); we honour both and
  prefer deterministic ordering for prompt-cache stability (011).
- **Tool-name collisions across sources are an error**, resolved by declared namespacing, never by
  silent last-wins.

## 6a. Progress reporting during invoke

A tool whose work is incremental or long-running — writing a large file, walking a big tree,
running a multi-step search — can report interim status without the model waiting on a silent call
until it returns.

- **`EffectContext` carries `report_progress(ProgressUpdate)`.** It is the only way to emit
  progress — there is no ambient stream a tool could reach for instead, consistent with §1's "no
  ambient-context accessor" rule.
- **`invoke()` (§3 step 8) may call it zero or more times before returning.** Each call emits a
  `ToolCallDelta` onto the run's internal event stream (013 §1) — the same stream every other run
  event rides, not a second channel — which 013 already projects onto AG-UI's `TOOL_CALL_CHUNK`
  (013 §2.1), MCP's `notifications/progress` (013 §3, 011 §3.4), and A2A's task/artifact updates
  (013 §3, 012). No new transport, no new protocol mapping — the wiring already exists; this is the
  one producer-side hook that was missing.
- **Progress never enters the model's context.** It is a UI/observability signal only; the model
  sees exactly one thing from a tool call — the final `ToolResult` normalized at step 9. This is
  what keeps `report_progress` free of §7's token-budget hazard: however many deltas a call emits,
  none of them are appended to the transcript, so they cannot be the mechanism that exhausts a
  turn's budget.
- **Still bounded, for a different reason.** Unbounded chunk size or rate is a transport and
  audit-log cost even though it never reaches the prompt. `ProgressUpdate` is a small, fixed-shape
  struct (e.g. `{done, total, message}`, not arbitrary content) and is rate-limited per call — a
  cap independent of, and much tighter than, §7's model-context budget.
- **Attribution, not a new capability.** `report_progress` is telemetry about an effect already
  authorized at pipeline step 4; it grants and checks nothing of its own. It carries the call's
  `EffectContext` so the emitted event is attributed (I4) to the same span as the call it
  describes.
- **Purely optional for tool authors.** A tool that never calls it behaves exactly as today:
  `ToolCallStarted` then `ToolCallFinished`, no deltas between them.

## 6b. Scheduling, watch, and background tools

Three more effects a call can trigger beyond returning a result — a durable future wake, a wake on
external change, and a call that outlives the turn that started it. None of these invent a second
mechanism: all three are agent-callable entry points onto 019's existing `Suspended`-run machinery
(019 §2), which already had the wake-condition table but no way for the model itself to arm one.
None of MAF's tool vocabulary has an equivalent, because MAF has no capability model to gate them and
no `Suspended` state to resume into.

- **`schedule_wakeup(WakeCondition)` and `watch_resource(ResourceRef, WakeCondition)`** are declared
  tools gated by a new capability, **`Schedule<max_horizon, max_active>`** (007 §3) — bounding how far
  into the future and how many concurrent registrations an agent may hold, for the same reason
  `NetOut` is bounded by an allowlist: an unparameterized "may schedule" is a hole, not a capability.
  Both resolve to the same underlying registration — a durable wake condition on the run (019 §2's
  "Timer/schedule" and "External event" rows), backed by Quark's durable reminders (Quark 027) — so
  no new scheduling primitive is invented here, only the missing producer-side hook, the same shape
  §6a took for progress.
- **The run actually suspends.** Calling either tool and then ending the turn (001 §3) transitions the
  run to `Suspended` (019 §2) — no activation, no sandbox, no connection, no thread held while
  waiting, not merely a note the host happens to honor later (measured by 019 §7 G3's census check,
  not asserted). This is the property MAF cannot express at all: "check back on this in three days"
  costs storage, not a parked process.
- **`watch_resource` over a source with no native push** (a polled HTTP endpoint, most non-filesystem
  targets) **is a reminder-driven poll under the hood**, not a live subscription — stated explicitly
  so a tool author never assumes push semantics a given resource kind cannot deliver. `Schedule` only
  grants the right to register the wake; it grants no new read access — watching a path or host still
  requires that target's own `FsRead`/`NetOut` capability, held independently.
- **Background tools.** A tool may declare **`Backgroundable`** — §5's sibling to `Parallelizable`:
  where `Parallelizable` claims "safe to run concurrently with other calls in a batch," `Backgroundable`
  claims "safe to detach from the turn that called it." Invoking a `Backgroundable` tool through the
  wrapper `background_task(tool, args)` — gated by **`Background<max_concurrent>`** (007 §3) — still
  runs the full ten-step pipeline (§3) for the wrapped call, but step 8 does not block the turn on its
  completion. The session actor still processes exactly one thing at a time (I1); what is detached is
  the *model's wait*, not a second executor inside the session. Completion is delivered as an ordinary
  `ToolCallFinished` on the run's event stream (013 §1), plus a new 019 §2 wake condition — **"Local
  background task completion"** — so a run may also suspend and be specifically woken by it, the local
  counterpart to 019 §2's existing "Remote task completion" row for A2A/MCP tasks. **This resolves 010
  Q6**: backgrounding is permitted, gated by `Background`; its resource use is charged to the session
  under the same accounting as any call (§3 step 10); its survival across passivation follows the
  sandbox profile's own properties (008 §6a) rather than a bespoke rule.
- **One handle shape, three producers.** `schedule_wakeup`, `watch_resource`, and `background_task`
  each return a **`StandingEffect`** handle — unforgeable, like a capability handle (007 §3.4), scoped
  to the run that created it. `list_standing_effects()` and `cancel_standing_effect(handle)` are the
  one introspection/kill surface for all three kinds, rather than three bespoke list/cancel pairs.
  Cancelling a handle from a different run or principal fails: a `StandingEffect` is attributable (I4)
  to the run that registered it, the same rule that governs every other capability-adjacent handle in
  this spec. Registering, resolving, or cancelling one is visible on the run's event stream via
  `StateChanged` (013 §1) — a UI is never blind to what a run currently has outstanding.
- **Notification is not a fourth mechanism.** A host or human being told something already flows
  through 013's existing projections — `InputRequired`, `ApprovalRequested`, and now `ToolCallDelta`
  (§6a) — onto AG-UI/MCP/A2A. An agent that wants to *proactively* message a human outside those flows
  needs an ordinary external-effect tool (Slack, email, a push service), already covered by §1's
  declaration pattern. Nothing new is needed here.

## 7. Tool result hygiene

A tool result is external content and is the primary prompt-injection vector (017):

- Tainted, provenance-marked, and delimited when rendered into the prompt.
- **Size-bounded against the run's actual budget, not a fixed constant.** A truncation threshold set
  in bytes independent of the model in play is not a safety mechanism — a "small" fixed cap can still
  be a context-window-consuming disaster against a small-context model, and a generous one does
  nothing against a large-context model with little headroom left. The threshold for a given call is
  **derived from the run's effective per-turn token budget** (005 §3's `TokenBudget` and per-source
  budgets), scaled to a declared fraction, never a global byte constant applied uniformly regardless
  of model or how much of the turn's budget is already spent. This is what stops an agent's very
  first tool call — reading one extremely large file, say — from consuming the run's entire budget in
  one shot: truncation happens *before* the result is appended, scaled to what the next model call can
  actually afford, not discovered after that call has already failed against an oversized context.
- Above that threshold, the result becomes a `BlobRef` (003 §3); the agent opens it explicitly and
  pages through what it needs — the same content-goes-to-a-handle pattern 028 §2 codifies for bulk
  structured data, generalized here to every tool result, differing only in what "threshold" is
  measured in (rows/bytes there, tokens here).
- **Structured where possible** — a `Data` content item with a schema beats prose the model must
  parse.
- Results never carry executable directives that the engine acts on. There is no in-band control
  channel from a tool to the engine; control flows through typed fields only.

## 8. Promotion gate

- **G1** — one agent invokes the same logical tool through all six sources (§2) and produces
  identical span shape, audit record shape, and model-visible result shape.
- **G2** — negative suite: every one of {unknown name, schema violation, capability not held,
  approval mismatch, deadline exceeded, sandbox crash, oversized result} produces a structured
  `ToolResult{is_error}` with the correct classification and no leaked capability.
- **G3** — a capability handle from call *n* is unusable in call *n+1* (proven, not asserted).
- **G4** — parallel batches produce deterministic history order across 10⁴ randomized completions.
- **G5** — a tool that calls `report_progress` 10³ times with maximal-size payloads produces zero
  bytes in the model-visible transcript and zero change to the run's token budget; only the final
  `ToolResult` appears there. Positive control: a deliberately mis-wired projection that leaks one
  progress chunk into the prompt is caught by the test, not just described as prevented.
- **G6** — a run that calls `schedule_wakeup` or `watch_resource` and ends its turn is fully
  `Suspended` (019 §7 G3's census check: no activation, sandbox, connection, or thread) until its wake
  condition fires; measured, not asserted.
- **G7** — a `Backgroundable` call invoked via `background_task` does not block the turn that started
  it, and its `ToolCallFinished` lands on the run's event stream even across a suspend/resume of the
  run in between.
- **G8** — `cancel_standing_effect` on a handle from a different run or principal fails with a
  capability/ownership error, proven over both same-principal and cross-principal cases (I2, I4).
- **G9** — a session already at its `Background<max_concurrent>` cap has its next `background_task`
  call rejected at pipeline step 4 (authorize), never silently queued or throttled elsewhere.

## 9. Open questions

- ~~**Q1** — Should tool *outputs* be schema-validated as strictly as inputs? MCP's `outputSchema`
  and `structuredContent` allow it; strictness may break real servers.~~ **Resolved, split by whose
  schema it is (2026-08-04):** see 011 §13 Q3 for the full resolution — our own tools publish
  `outputSchema` and are enforced strictly against it (our own contract, our own bug to catch); a
  third-party MCP server's output validated against *its own* claimed schema degrades to an annotated
  marker surfaced to the model on mismatch, never a hard rejection, since real-world servers are
  inconsistent and schema conformance was never a trust signal (017's taint applies either way).
- ~~**Q2** — How to expose long-running tools: MCP's tasks extension, A2A tasks, and our own
  `Suspended` state are three shapes for one idea (see 019 Q2).~~ **Resolved (OQ-4, 019 §8 Q2,
  2026-08-04):** the three shapes were never peers at the same granularity — A2A tasks already are
  our `Suspended` run; MCP's tasks extension is scoped to one long tool call and maps onto
  `Backgroundable`/`StandingEffect` (§6b); a whole-run MCP pause reuses MRTR's `interaction_id`-in-
  `requestState` mechanism (001 §2, 011 §3.4). No new unification primitive needed.
- ~~**Q3** — Whether `Parallelizable` can be *inferred* for pure WASM plugins with no capabilities
  (arguably yes, and it would be free).~~ **Resolved, Yes — and it's provable, not inferred by
  heuristic (2026-08-04):** a plugin with an empty `CapabilitySet` (007 §3.1's empty-by-default rule)
  has, by construction, no way to touch anything outside its own call arguments — I2 guarantees it.
  `Parallelizable`'s definition (§5: "concurrent execution... observably equivalent to some
  sequential order") is exactly what that guarantees for a zero-capability plugin: there is nothing
  for two concurrent calls to even race over, since neither can reach shared state. This is free, as
  the question suspected, but stronger than an inference — a direct consequence of the capability
  model already specified, checkable mechanically at load time from the manifest's declared (empty)
  capability request (009 §3): `Parallelizable` is set `true` automatically when that set is empty, and
  stays an explicit author declaration — subject to the same "defect on a state-mutating tool"
  scrutiny §5 already states — for any plugin that does hold capabilities.
- ~~**Q4** — The exact fraction of a turn's token budget a single tool result may claim (§7) is
  unspecified... The sharper case is a **parallel batch** (§5): N results each individually under
  the per-result cap can still collectively exceed the turn's budget, and this section does not yet
  say whether the cap applies per-result or per-batch.~~ **Resolved, both parts (2026-08-04):**
  **(1) Scaling** — already decided elsewhere in this RFC, just not marked here: 010 §3 already
  quotes it directly ("the cap itself follows 006 §7's rule — scaled to the run's effective token
  budget, not a fixed byte constant"), so a fixed fraction was already rejected. Risk-weighting is a
  refinement left to operator policy (007 §5's argument-predicate mechanism can already differentiate
  by tool if an operator wants a riskier tool capped tighter), not an engine-hardcoded function.
  **(2) The sharper parallel-batch case** — the cap applies **per-batch**, not per-result
  independently: a per-result-only cap cannot bound total consumption, exactly as the question
  identifies, so a parallel batch's results are, collectively, one budgeted contributor. 005 §3's
  existing "every contributor declares a token budget... drop order is declared, recorded in the
  trace" mechanism applies directly — the batch is the contributor, individual results are its
  sub-allocation — rather than N independent, uncoordinated allowances that can collectively exceed
  the turn's budget.
- ~~**Q5** — The poll interval for `watch_resource` over a no-native-push source (§6b) is unspecified
  — a fixed default, a per-call declared hint, or one inferred from the resource kind are all
  plausible and untested.~~ **Resolved, a per-call declared hint, bounded by an operator-declared
  floor (2026-08-04):** matches this project's consistent pattern (extraction cadence, 029 §4;
  consolidation cadence, 029 §10 Q3) — *when* or how often something runs is a genuine 020 §1
  configuration knob, not a hardcoded constant. Not inferred from resource kind: that's an open-ended
  taxonomy needing a lookup table the engine would have to build and maintain, the same avoidable
  burden as 004 §8 Q3's tokenizer-list question, for a benefit a simple hint already delivers more
  directly. The caller (model via tool-call argument, or agent author via policy) declares its
  desired interval; an operator-declared floor (extending `Schedule<max_horizon, max_active>`'s
  existing parameterized bounds, 007 §3) prevents a model from requesting an abusively tight poll
  that hammers a resource or exhausts the `max_active` budget. A single fixed default would be wrong
  for most cases — a rarely-changing file and a high-frequency feed genuinely need different
  cadences — which is why a hint, not a constant, is the right shape.
