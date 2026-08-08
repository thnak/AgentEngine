# Milestone 7 — Protocol conformance — work breakdown and kick-off

**Status:** Work breakdown (stage 4 of [the review-signoff workflow](v1-review-signoff-workflow.md)),
written just-in-time as this milestone starts, per that doc's §4. Scoped to
[the roadmap's](v1-implementation-roadmap.md) Milestone 7 exit criterion: *"`conformance server`/
`conformance client` pass at `2026-07-28` (011 §10 G1/G2), `a2a-tck` passes with zero MUST-level
failures (012 §8 G1), the AG-UI compatibility suite passes against a pinned schema (013 §6 G5), a
YAML agent produces byte-identical metadata to its C++ equivalent (015 §7 G1, I6's actual
enforcement), and 006 §6b's G6-G9 ... all hold — closing 019 §2's wake-condition table completely."*

**RFCs:** 011 (MCP Conformance), 012 (A2A Conformance), 013 (UI and Streaming Surfaces), 015
(Declarative Agent and Workflow Format), plus 006 §6b (`Backgroundable`/`StandingEffect`) per
`docs/planning/backgroundable-standingeffect-gap.md`'s assignment. All five Reviewed (2026-08-05).

## Current state (verified 2026-08-08, after M6)

| Item | State |
|---|---|
| `include/agentengine/protocol/{mcp,a2a,agui}/` | **README only.** No headers, nothing compiled. `protocol/anthropic/` and `protocol/openai/` (real `ChatClient`s, M5) are the only non-stub siblings — no precedent to reuse for a *wire protocol adapter* shape, only for "a backend behind a policy-tagged interface" |
| `Backgroundable`, `StandingEffect`, `background_task`, `list_standing_effects`, `cancel_standing_effect` | **Do not exist anywhere** (confirmed by grep, matching the residual note's own finding). `Parallelizable` (`core/tool.hpp:29`) is the one real sibling tag — an empty marker struct with no pipeline logic behind it yet, the shape `Backgroundable` will follow |
| `core/tool_pipeline.hpp`'s 10-step pipeline | Real (M2 Phase B, M4 Phase F1). Step 8 ("invoke") is synchronous and blocking end-to-end today — `invoke_tool()` calls `tool->invoke(...)` and returns only after it completes. `Backgroundable`'s whole point is stopping step 8 alone from blocking the turn; every other step (1-7, 9-10) stays as-is per the residual note ("still runs the full 10-step tool pipeline; only step 8 stops blocking") |
| `Interaction` (`core/interaction.hpp`) | Real since M4. `StandingEffect`'s handle is a **different** shape — not a suspend/resume correlation id, an unforgeable *live effect* handle (007 §3.4-style), with its own introspection (`list_standing_effects()`) and kill (`cancel_standing_effect()`) surface. Do not conflate the two; 013 §1 is explicit that `StandingEffect` visibility rides `StateChanged`, never `InputRequired`'s pair |
| `EffectJournal`/`IdempotencyKey` (`core/effect_journal.hpp`) | Real (M4 Phase F1/F2). A backgrounded call still derives and journals a key exactly as a foreground one does — `Backgroundable` changes when step 8 returns to the caller, not the accounting around it |
| `TimerWake` (`agent_session.hpp:137`) | Real (M4 Phase E3), and explicit in its own comment that it is *not* what a real background-task wake needs — "what a real 'resume the paused run this timer was arming' would DO needs 006 §6b's `schedule_wakeup`/`Backgroundable`... a different, un-built vertical". This milestone is what finally builds that vertical |
| `ae::stream<T>` (`core/stream.hpp`) | Real (M5 Phase B4b), credit-controlled, genuinely incremental — the primitive 013 §1's internal run event stream will ride, and what Phase G (013 §7 Q2's resolution) uses for the per-subscriber bounded-eviction fan-out A2A's ordering MUST requires |
| A "run event" type / `RunStarted`/`ModelDelta`/`StateChanged`/... (013 §1's twenty-line enum) | **Does not exist anywhere.** No `RunEvent`, no `StateChanged`, no sequence-numbered per-run stream. `AgentSession::handle(Ask<StartRun,AgentResponse>)` (agent_session.hpp) computes everything it needs and replies once — it emits nothing incrementally today |
| `WorkflowLiveEvent` (`workflow/live_view.hpp`, M6 Phase G) | Real, but scoped narrowly to a workflow supervisor's own superstep boundaries (`executor_live_state`, round number) — **not** 013 §1's general run-event vocabulary (model deltas, tool-call deltas, approvals). A workflow run and an agent-session run are different actors emitting different event shapes; 013 §1 is the agent-session side, and does not yet exist |
| `quark::Engine`, real | Precedent since M4 Phase E2 (`test_agent_session_suspend_resume.cpp`), reused throughout M6. Every M7 test that exercises a genuinely async surface (a backgrounded call outliving its turn, a streamed run) needs this, not `TestKit` — same forcing function M6 decision 3 already named |
| Quark `EventLog<T,S>`/`Store` | Real, used repeatedly in M4/M6 for anything append-only or multi-retained. `list_standing_effects()`'s "what's currently outstanding" is a live in-memory census, not a durable log by itself — but a `StandingEffect`'s *registration*/*resolution* transitions are exactly the kind of unboundedly-growing history `EventLog` already proved itself for (M6 decision 1) if this milestone needs an audit trail of them, not just current state |
| 004's `ChatClientCapabilities::batch` (`core/chat_client.hpp:52`) | Real, declared, **`false` everywhere** — nothing sets it. 004 §8 Q1 resolved that a real batch-API backend rides `Backgroundable`/`StandingEffect` once this milestone builds it; wiring an actual batch-capable `ChatClient` is not itself in scope here (that's a M5-owned RFC's own backend work, only unblocked by this milestone, not completed by it) |
| Machine-safety envelope | Unchanged: `-j4` max, tests pinned ≤ 4 cores. Nothing in this milestone's own gates (a MUST-level TCK run, a schema-pinned compatibility suite, a byte-identical-metadata corpus) implies an actor-count blowup the way M6's N=100 Projects or 10³-seed shuffle did — flagged here only because it wasn't, not because a check was skipped |

## Design decisions made while breaking this down

1. **013 §1 (the internal run event stream) is built before 006 §6b, reversing the roadmap
   sentence's reading order — because 006 §6b's own text requires it, not despite it.** The
   roadmap's prose says "M7 closes 006 §6b first, then builds 011/012 against it" — true for 011/012,
   but 013 §1 itself states `StandingEffect` visibility "rides `StateChanged`, not a new event pair"
   and names this as closing 006 §6b's own observability gap. Building `Backgroundable`/
   `StandingEffect` before a `StateChanged` event exists would mean either inventing a throwaway
   placeholder event (drifts from 013 §1's real shape, the exact "second wire mapping to design and
   maintain" 013 §1 says NOT to do) or shipping G6-G9 with no observability proof at all. 013 §1 has
   no dependency on 006 §6b, 011, or 012 being built first — it is a self-contained emission
   mechanism over `AgentSession`'s own existing turn loop — so there is no real ordering cost to
   moving it first, only a benefit: 006 §6b, 011's progress projection, and 012's streaming all build
   against one real thing instead of three separate stubs.

2. **`Backgroundable`/`StandingEffect` (006 §6b) is Phase B, immediately after the event stream —
   matching the roadmap's own rationale for WHY M7 owns it at all.** 019 §2's six-row wake-condition
   table has two rows shipped (M4) and needs "Local background task completion" (this RFC pair) plus
   "External event"/"Remote task completion" (needs 012, built later this milestone). Building 012
   against a still-stubbed `Suspended` state would make `a2a-tck` MUST-level task-completion cases
   unprovable — the same dishonesty risk the roadmap already names. Phase B closes that dependency
   before 011/012 need it.

3. **011 (MCP) and 012 (A2A) are built as sibling phases, not strictly sequenced, but MCP first —
   012's task/streaming machinery benefits from 011's request/response and progress-projection
   plumbing existing as a second data point before A2A's own (structurally similar but not
   identical) task lifecycle is designed.** Both depend on 013 §1 (progress/streaming projection)
   and 006 §6b (011's tasks extension, 012's task completion) being real, which Phases A-B provide.
   Neither depends on the other (their RFCs list no mutual dependency, and CLAUDE.md's own "the
   dependency graph is not a DAG" caution names 018↔011↔012 as the real cycle — the 018 credential
   seam, not 011↔012 directly). 018 (Identity/Secrets) is real since M5, so that cycle is not live
   for this milestone.

4. **013's remaining surfaces (§2 AG-UI, §3 other projections, §4 transport) are built as their own
   phase AFTER 011/012 exist, not interleaved with them — because 013 §6 G3 (cross-surface content
   equivalence between AG-UI and A2A streaming) needs both surfaces real to compare.** 013 §1 (Phase
   A) already provides what 011's progress notifications and 012's task streaming both project from;
   what's deferred to this later phase is specifically the AG-UI-facing projection and the
   cross-surface proofs that need a second real surface to prove equivalence against.

5. **015 (Declarative format) is the last RFC-scoped phase, not because it is gated on the others —
   its own dependencies (002, 006, 014) are all real since M2/M6 — but because 015 §6's "Publishing
   an agent document generates its A2A Agent Card (012) and MCP tool listing (011) from the same
   metadata" and §7 G4 ("Agent Card / MCP listing regenerated deterministically") need 011/012 to
   exist to generate INTO.** G1-G3 (equivalence, strictness, workflow-trace equivalence) do not
   share this constraint and could in principle be built earlier; kept as one phase rather than
   split across the milestone for the same reason M6 kept single-RFC phases contiguous — a scattered
   G1/G4 split would cost more in context-switching than it saves in parallelism this project's own
   phase-by-phase, one-commit-per-phase discipline doesn't exploit anyway.

6. **AG-UI's own maturity gap (013 §2.0: no spec, no TCK, pre-1.0, MIT/informally-governed) means
   013's own gate is "compatibility against our own authored suite", not "conformance" — carried
   into this breakdown verbatim, not softened.** Where 011/012 gates cite an external, versioned,
   third-party conformance tool run against a named revision, 013's AG-UI gate (G5) cites a suite
   this project authors and maintains itself, pinned to an `@ag-ui/core` schema release. This is
   013's own RFC text, not a scope reduction introduced here.

7. **The `Background<max_concurrent>` capability tag (007 §3) is new vocabulary this phase adds to
   `trust/capability.hpp`, not a reuse of an existing tag.** 007 §3 is real (M2) but has never
   declared a background-task-specific capability shape; `Background<N>` is 006 §6b's own named gate
   (G9: "a session already at its cap has its next `background_task` rejected at pipeline step
   4/authorize"), so it must exist as a real, checkable capability before G9 is provable — this is
   additive to 007's real capability-tag vocabulary, not a redesign of it.

## Phases

- **Phase A** — 013 §1: the internal run event stream (`RunEvent` vocabulary, sequence numbers per
  run, `AgentSession` emission at existing turn-loop boundaries, backpressure over `ae::stream<T>`).

  **Outcome (2026-08-08, commit pending):** `include/agentengine/core/run_event.hpp` (new) defines
  the total 24-kind `run_event_kind` vocabulary from 013 §1's own list, each backed by a real payload
  type (a `std::variant` over flat structs — safe here, unlike `content_record.hpp`'s rejected
  variant-`quark_describe` design, because this stream is never durably serialized). `AgentSession`
  (`agent_session.hpp`) gained `enable_event_stream()` (mirrors `WorkflowSupervisor::enable_live_view()`
  exactly, M6 Phase G) and a private `emit_run_event()` helper, wired at every boundary its turn loop
  actually has today: `RunStarted` → `TurnStarted` → `ModelCallStarted` → `ModelCallFinished` →
  `TurnFinished` → `RunFinished` on the success path, with `RunFailed` as the terminal event (never
  followed by `RunFinished`) on each of the four pre-existing fail-closed branches (context-assembly
  failure, no chat client configured, chat-call failure, token-budget exceeded) — each carrying a real
  `error_code`. The admission-denial branch fires no event at all, matching 001 §1's "no Run is minted
  until admission passes." `run_event_seq_` resets to 0 at the top of every `handle()`, proven by
  running two `StartRun`s on one session and observing the second run's first event at `seq == 1`, not
  a continuation of the first run's count. `tests/test_agent_session_run_event_stream.cpp` (new, 22
  checks, all passing) proves all of the above under `quark::TestKit` (no genuine cross-actor parking
  is introduced — `make_stream<T>` needs no actor addressing per `stream.hpp`'s own header). Full suite:
  129/129 passing (added one test from M6's close-out 128).

  **What is honestly NOT wired**, named per this doc's own decision 1 rather than silently assumed:
  `ToolCallStarted/Delta/Finished`, `ModelDelta`, `SandboxExecStarted/Finished`, `ArtifactProduced`,
  `RunCanceled` have real types but no real producer inside `AgentSession` — its turn loop still never
  reaches the tool pipeline (`invoke_tool`/`invoke_agent_tool` are called only from
  `agent_registry.hpp`, never from `AgentSession::handle`) and never uses `chat_stream()`. `StateChanged`
  is defined but has no caller yet either — Phase B's `StandingEffect` visibility is its first real
  producer, per 013 §1's own "rides `StateChanged`" rule. `EffectContext::report_progress` (013 §1's
  named producer for `ToolCallDelta`, 006 §6a) does not exist yet — deferred to whichever phase first
  needs to call a tool with progress reporting, most likely Phase B or C.
- **Phase B** — 006 §6b: `Backgroundable`, `Background<N>` capability, `background_task()`,
  `StandingEffect` handle, `list_standing_effects()`/`cancel_standing_effect()`; closes 019 §2's
  "Local background task completion" wake row; `StateChanged` visibility from Phase A.

  **Outcome (2026-08-08, commit pending):** `standing_effect.hpp` (new) defines `StandingEffect`
  (`Described`/`QUARK_SERIALIZE`-able from the start, mirroring `Interaction`'s own M4 precedent) and
  `standing_effect_kind`. `tool.hpp` gained the `Backgroundable` tag and `Tool::declared_backgroundable()`
  (undeclared defaults to `false`, the same fail-closed direction every other undeclared tool policy
  already has); `tool_pipeline.hpp`'s `ToolDescriptor` carries it through. `capability.hpp` gained
  `CapabilitySet::find_background()`, the same "pure lookup, not `subsumes()`-based" shape
  `find_fs_write()` already established — `cap::Background`/`cap::decl::Background<N>` themselves
  turned out to already exist (scaffolded generically alongside the rest of 007 §3's capability table
  before this milestone), so only the enforcement was missing, not the vocabulary.
  `tool_pipeline.hpp::background_task()` runs steps 1 (resolve), 4/7 (authorize+bind — the tool's own
  ceiling AND `Background<max_concurrent>` checked against a caller-supplied LIVE count, G9), and 5
  (approve) synchronously, then detaches step 8 (invoke) onto its own `std::thread` + `.detach()` —
  deliberately not a kept-alive `std::jthread`, since 006 §6b names no cancellation mechanism for
  in-flight native `invoke()` work. `AgentSession` gained `start_background_task()` (the real
  producer), `list_standing_effects()`, `cancel_standing_effect()` (G8: cross-principal denial,
  checked against the effect's own recorded `principal_id`), and `handle(BackgroundTaskDone const&)`
  — a tell-only completion message mirroring `TimerWake`'s own "host arms the callback, the actor
  never self-addresses" shape (`self.tell(...)` from the background thread's completion closure).

  A real correctness bug in Phase A's own design was caught and fixed while building this: emitting a
  background completion's `ToolCallFinished` needs the ORIGINATING run's `run_id`, which can differ
  from whatever run is current on the actor by the time the detached thread finishes — a single
  scalar `run_event_seq_` (reset per `StartRun`) would have let a stale completion's sequence number
  collide with a newer run's own numbering. Fixed by keying the counter per run_id
  (`run_event_seq_by_run_`, a map) via a new `emit_run_event_for(run_id, ...)` primitive; Phase A's
  own tests re-ran unchanged and still pass, confirming the fix is behavior-preserving for every
  existing call site.

  `tests/test_agent_session_background_task.cpp` (new, 21 checks, all passing, real `quark::Engine`
  mirroring `test_agent_session_timer_wake.cpp`'s own construction) proves: an undeclared-Backgroundable
  tool is rejected before step 8 ever runs; `start_background_task()` returns in under 50ms even
  though the tool's own `invoke()` sleeps 150ms (G7's "doesn't block the calling turn" half); a second
  call against an already-saturated `Background<1>` is rejected, never queued (G9); cross-principal
  cancellation is denied while the owning principal's succeeds (G8); and once the detached thread
  actually finishes, `ToolCallStarted`/`ToolCallFinished` are both real on the event stream, correctly
  attributed to the run that asked for the work even after a SECOND, later run has already started on
  the same session. Full suite: 130/130 passing (was 129 after Phase A).

  **What is honestly NOT built**, per this doc's own decision 1's precedent of naming rather than
  silently claiming: `schedule_wakeup` (019 §2's "Timer/schedule" row) still ships via `TimerWake`/
  Quark's reminder service exactly as M4 Phase E3 left it, WITHOUT going through the `StandingEffect`
  handle shape — retrofitting it is a real, named follow-up, not attempted here since it would touch
  reminder-arming call sites this phase does not otherwise need to touch. `watch_resource` has no real
  producer anywhere; its wake condition is 019 §2's "External event" row, which needs 012 (A2A) —
  Phase D, not this one. Full G6/G7 durability (surviving a real actor suspend/resume or process
  restart) is not proven: `StandingEffect` is serializable but is not yet threaded into
  `AgentSessionRecord`'s own checkpoint, so "survives a restart" remains a real, separate gap.
- **Phase C** — 011 MCP: client role (§3: tools/resources/prompts, MRTR, caching, pagination) and
  server role (§4: exposing AgentEngine), against `2026-07-28`.

  **Reuse inventory (2026-08-08, Explore agent survey before starting):** HTTP client machinery is
  real and reusable —`agentengine::sandbox::perform_provider_https_exchange`/
  `perform_provider_streaming_exchange` (`sandbox/provider_http_client.hpp`, ADR-011/ADR-013,
  mbedTLS-backed) is the same host-initiated pattern the OpenAI/Anthropic `ChatClient`s already use;
  an MCP client call is architecturally the same shape (host-initiated, no capability to mediate),
  so it reuses this path rather than `HostEgressProxy` (`cap::NetOut`, guest-mediated only). `json::Value`
  (`core/json_value.hpp`) is a real, general-purpose parse/dump type, reusable as-is for MCP bodies.
  **New work, confirmed absent by direct inspection:** a generic JSON-Schema-2020-12 validator (only
  a from-our-own-C++-types schema *generator* exists, `json_schema.hpp`'s `AE_JSON_SCHEMA` macro — no
  `$ref`/composition/depth-budget validator against an arbitrary third-party schema); a JSON-RPC 2.0
  envelope; a persistent-process (writable stdin pipe, incrementally-readable stdout) spawn primitive
  for the stdio transport (`native_jail_backend.cpp::exec()` is real but run-to-completion, wrong
  shape); a `ToolDescriptor` → MCP `tools/list` item mapping.

  Sub-phases (each gets its own build+test+commit, all still under the umbrella "Phase C" task):
  - **C1** — the JSON-RPC 2.0 envelope itself (`protocol/mcp/json_rpc.hpp`), transport- and
    MCP-vocabulary-agnostic on purpose: id-presence decides Request vs Notification (§4), a response
    is result XOR error by construction (§5), both proven by round-trip + negative tests before any
    MCP semantics are layered on.
  - **C2** — server role: `server/discover`, `tools/list` (from `ToolTable`), `tools/call` (via
    `invoke_agent_tool`/`invoke_tool`), `isError` vs JSON-RPC-error split, over the C1 envelope with
    no real transport yet (direct in-process dispatch) — server role needs no generic schema
    validator (we generate our own schemas, we don't validate against someone else's).
  - **C3** — client role: consuming a (mock, in-process) server's `tools/list`/`tools/call`, caching
    (`ttlMs`/`cacheScope`), pagination (opaque cursor, empty-string-valid), digest-pinning (§8
    rug-pull defense), `isError` surfaced to the model. The generic JSON-Schema validator §3.1 asks
    for (`$ref` hardening, composition-keyword bounds) is scoped as its OWN follow-up sub-phase
    (C3 proves basic type-shape argument construction; full hardening is named, not silently skipped).
  - **C4+** — MRTR/tasks extension (needs Phase B's `Backgroundable`/`StandingEffect`, real since
    Phase B), Streamable HTTP transport (client side reuses `perform_provider_https_exchange`; server
    side needs an HTTP SERVER surface this codebase does not yet have — scoped when reached), stdio
    transport (the new persistent-process primitive), authorization (018), trust/supply-chain (§8),
    conformance tooling integration (§10 gate). Scoped in more detail as each is reached, matching
    this milestone's own "vocabulary total, wiring honest" discipline throughout Phases A/B.

  **Outcome, C1 (2026-08-08, commit pending):** `protocol/mcp/json_rpc.hpp` (new) — `JsonRpcRequest`/
  `JsonRpcNotification`/`JsonRpcResponse`/`JsonRpcError`, `parse_message()` (id presence → Request vs
  Notification), `parse_response()` (result XOR error enforced, both-or-neither rejected), `to_json()`
  overloads, and the 011 §5 error-code constants (including the revision's renumbered
  `HeaderMismatch`/`MissingRequiredClientCapability`/`UnsupportedProtocolVersion`). 24 checks in
  `tests/test_mcp_json_rpc.cpp`, all passing, including negative cases (wrong/missing `"jsonrpc"`
  version, malformed error object, an explicit `"id": null` rejected rather than silently treated as
  a notification). 131/131 full suite (was 130 after Phase B).

  **Outcome, C2 (2026-08-08, commit pending):** `protocol/mcp/server.hpp` (new) — `McpServer`, a
  transport-agnostic request dispatcher (`dispatch(JsonRpcRequest) -> JsonRpcResponse`) serving
  `server/discover` (fixed `protocolVersion: "2026-07-28"`, matching §12/§13 Q1's resolved "as a
  server, 2026-07-28-only"), `tools/list` (from a real `ToolTable`, registration order preserved per
  §3.1's own rule), and `tools/call` (via the real `invoke_tool()` 10-step pipeline). Proves §3.1's
  own "isError vs JSON-RPC error is a semantic split we honour on both sides": an unknown tool/method
  is a JSON-RPC error (`MethodNotFound`/`InvalidParams`), while a tool that RAN and failed is a
  JSON-RPC *result* with `isError:true` — `ToolResult::is_error` already carries exactly this
  distinction, so the mapping is direct. `tools/call`'s result always carries `resultType: "complete"`
  (§3.4's revision-required field; MRTR's `input_required` variant needs Phase B's `StandingEffect`
  wired to a real long-running tool, deferred to C4). Content mapping is narrower than MCP's full
  part vocabulary (text/image/audio/resource) — every `ToolResult::content` item this pipeline
  produces today is `Data`/`Error`/`Text`, and all three map onto one `{"type":"text",...}` part; a
  richer content kind is a real, named follow-up, not silently claimed. `tests/test_mcp_server.cpp`
  (new, 20 checks, all passing) proves all of the above against two real tools (one that succeeds,
  one that always fails) through the real pipeline, not a stub. 132/132 full suite (was 131 after C1).

  **What is honestly NOT built for C2**: no real transport (this is direct in-process dispatch);
  `EffectContext`/principal establishment is transport work, not this dispatcher's job (`held`/
  `approve` are supplied as given, mirroring `invoke_tool()`'s own layering); `resources/list`,
  `prompts/list`, and every deprecated-feature surface (§3.5) are simply unrecognized methods
  (`MethodNotFound`) at this stage, not yet implemented; tool `annotations` (readOnlyHint/
  destructiveHint/etc., §3.1) have no declaration surface in `core/tool.hpp` yet.

  **Outcome, C3 (2026-08-08, commit pending):** `protocol/mcp/client.hpp` (new) — `McpClient`, built
  against the same `RequestSender` (`JsonRpcRequest -> JsonRpcResponse`) abstraction `McpServer` uses
  on the other side, so this phase's own test wires it directly into a real `McpServer::dispatch()`
  for the happy path and a hand-rolled mock sender for the caching/pagination/rug-pull properties that
  need multi-call control a truthful real server can't provide. Proves §3.1's client-side half of the
  `isError` split: an execution failure (`isError:true`) is a SUCCESSFUL `result<>`, never thrown or
  treated as a client-side failure -- only a genuine protocol error (unknown tool/method) is. Caching
  follows §3.1's own rule literally: cache key is method+params (an empty-string cursor is its OWN
  cache entry, distinct from any other cursor, never conflated with "no cursor"), and `ttlMs <= 0`
  means "never serve from cache" (not "cache forever" -- the naive reading a `ttl == 0` check would
  produce). Digest-pinning (§8) hashes each `tools/list` response (FNV-1a over
  name+description+schema, the same non-cryptographic deterministic-hash idiom `argument_digest()`
  already uses) and flags a change under the same cache key as a detected rug pull.
  `tests/test_mcp_client.cpp` (new, 16 checks, all passing) proves all of the above. 133/133 full suite
  (was 132 after C2).

  **What is honestly NOT built for C3**: the generic JSON-Schema-2020-12 validator §3.1 asks for
  (validating arbitrary JSON against a THIRD PARTY server's own claimed schema, with `$ref` hardening
  and composition-keyword depth/time budgets) -- this client reads a discovered tool's schema but does
  not validate against it; real multi-page pagination (proven only at the cache-key level against a
  mock, since `McpServer`, C2, has no actual cursor-based paging to page through); `cacheScope`'s
  cross-*principal* isolation (needs a real multi-principal transport context); a rug-pull-detected
  flag surfacing into an actual re-approval gate (§8's "re-approval" half -- `rug_pull_detected()` is
  observable, but nothing yet BLOCKS a subsequent call on it).

  **Outcome, C4 (2026-08-08, commit pending):** the `io.modelcontextprotocol/tasks` extension (011
  §3.6/§12; field names and status vocabulary cited from `docs/research/2026-mcp-protocol-detail.md`
  §12, the exact result-envelope nesting around them is this implementation's own reasonable choice,
  not asserted as the literal upstream wire schema) for backgrounding a `tools/call` on a
  `Backgroundable` tool -- real since Phase B's `background_task()` (tool_pipeline.hpp), called
  DIRECTLY by `McpServer` rather than reinvented. Server side (`protocol/mcp/server.hpp`): a per-request
  opt-in (`params.extensions` containing `"io.modelcontextprotocol/tasks"` -- §12's "MUST NOT return
  CreateTaskResult to a client that did not include the extension capability on that request"),
  `handle_tools_call_as_task()` (mints a `std::random_device`-seeded task id, starts `background_task()`,
  returns a task handle immediately), `handle_tasks_get()` (polls; a completed task carries the same
  `content`/`isError`/`resultType` shape `handle_tools_call`'s own synchronous path already produces),
  `handle_tasks_cancel()` (marks a task `"cancelled"` so a later poll never reports `"completed"` for
  it, but -- inheriting Phase B's own documented limit -- cannot stop the in-flight `std::thread`; its
  eventual completion is simply discarded). Faithfully applies §12's own rule that a tool which RAN and
  FAILED is task status `"completed"` with `isError:true`, never `"failed"` -- proven directly (C4-6).
  `Background<N>`'s own capacity ceiling (G9, Phase B) is enforced through this path unchanged, counted
  fresh off the task registry's own `"working"` entries rather than a second counter that could drift
  (proven, C4-7). Client side (`protocol/mcp/client.hpp`): `call_tool_as_task()`, `get_task()`,
  `cancel_task()`, symmetric with the server's own method names. `tests/test_mcp_tasks_extension.cpp`
  (new, 19 checks, all passing) proves all of the above against a REAL `McpServer` backgrounding REAL
  tools (`SlowBackgroundableTool`, `FailingBackgroundableTool`, `ForegroundOnlyTool`), including that a
  cancelled task's status survives its own uncancellable worker's eventual completion (C4-9) and that
  cancelling an already-completed task is rejected (C4-10). 134/134 full suite (was 133 after C3).

  **What is honestly NOT built for C4**: `tasks/update` (client-to-server input mid-task) and
  `notifications/tasks` (a push channel this request/response-only dispatcher does not have); real task
  status `"failed"` (every failure this pipeline can detect happens synchronously, before a task is
  ever created, and is rejected as a JSON-RPC error instead -- the enum value is named, nothing produces
  it); MRTR's `InputRequiredResult` (011 §3.4) -- `ApprovalDecider` (tool_pipeline.hpp) is still a
  binary `bool(name, args) -> approved?` decider, not a three-state one that could ever return
  `"input_required"`, so tool-call approval and MRTR remain unconnected; task-id authorization (§12:
  "MUST authorize every task request" against the issuing principal) -- transport/principal work this
  in-process dispatcher does not have, the same gap C2/C3 already name for `tools/call` itself.
- **Phase D** — 012 A2A: server role (§2: Agent Card, HTTP+JSON/REST + JSON-RPC bindings, task
  management, push notifications) and client role (§3: consuming remote agents), against v1.0;
  closes 019 §2's remaining two wake rows.

  Sub-phases (same "envelope first, roles after" discipline C1-C4 used for MCP):
  - **D1** — the A2A v1.0 wire object model itself (`protocol/a2a/types.hpp`): `Part`/`Message`/
    `Artifact`/`TaskStatus`/`Task`, `task_state`/`a2a_role` enums, transport- and role-agnostic on
    purpose, proving v1.0's own two breaking changes from 0.3.x before any server/client role is built
    on top: `Part`'s `kind` discriminator is GONE (the JSON member name is the discriminator), and
    enums serialize as full SCREAMING_SNAKE names.
  - **D2+** — server role (Agent Card generation, JSON-RPC/REST bindings, `Task ← Run` projection off
    013 §1's event stream, streaming, push notifications) and client role (remote-agent-as-tool
    binding, digest-pinned card caching) — scoped in more detail as each is reached, per the user's own
    2026-08-08 decision to check in before committing to Phase D's full autonomous scope (it is a
    materially larger surface than Phase C's own MCP conformance work).

  **Outcome, D1 (2026-08-08, commit pending):** `protocol/a2a/types.hpp` (new) — `task_state`/
  `a2a_role` enums with `to_wire_string()`/`from_wire_string()` proving the full 9-value and 3-value
  SCREAMING_SNAKE round trip respectively (cited from `docs/research/2026-a2a-and-agui-detail.md`
  §A.5), `is_terminal()` matching the spec's terminal (`COMPLETED`/`FAILED`/`CANCELED`/`REJECTED`) vs
  interrupted (`INPUT_REQUIRED`/`AUTH_REQUIRED`) split exactly. `Part`'s oneof (`TextPart`/`RawPart`/
  `UrlPart`/`DataPart`) is discriminated purely by JSON member name -- no `"kind"` field is ever
  emitted or expected, proven directly (D1-1). An unrecognized discriminator member is preserved
  verbatim as `UnknownPart{member_name, raw_value}` rather than dropped or rejected -- the same
  "unknown kinds round-trip" precedent `core/content.hpp`'s own `Custom` already establishes for our
  internal content model, applied here to A2A's own wire oneof (proven, D1-5; the fuller "every part
  kind including unknown ones" corpus is G2's own job at milestone close, not claimed here).
  `Message`/`Artifact`/`TaskStatus`/`Task` round-trip every field with camelCase wire names, and unset
  optionals/empty repeated fields are omitted on the wire rather than emitted empty (D1-10).
  `tests/test_a2a_types.cpp` (new, 30 checks, all passing) proves all of the above, including negative
  cases (a `Part` with two oneof members, or zero; a `Message` with an empty `parts[]`; an
  unrecognized `task_state` string) are rejected, never silently coerced. 135/135 full suite (was 134
  after Phase C4).

  **What is honestly NOT built for D1** (all D2+ scope, not silently claimed here): `Task ← Run`
  projection off a real `AgentSession`/013 §1 event stream; Agent Card generation; the JSON-RPC/REST
  bindings themselves (this file has no request/response envelope or method dispatcher, only the
  objects that would ride inside one); streaming (`SubscribeToTask`), push notifications, extensions,
  authentication/authorization, digest-pinned card caching; the round-trip fidelity CORPUS G2 asks for
  (this phase proves the mechanism on hand-picked cases, not an exhaustive corpus run).
- **Phase E** — 013 §2-§4: AG-UI projection, other-surface table, transport (SSE, binary framing,
  WebSocket), cross-surface equivalence proofs.
- **Phase F** — 015: declarative agent/workflow YAML, the shared validator, equivalence corpus.
- **Phase G** — promotion gates: 011 §10 G1-G9, 012 §8 G1-G5, 013 §6 G1-G6, 015 §7 G1-G4, 006 §6b's
  G6-G9 — run for real, published percentages where the gate asks for one, milestone close-out.

Each phase follows the established M6 discipline: implement → build (PowerShell + `vcvarsall`) →
test → full-suite regression → update this doc's own phase "Outcome:" → one commit, narrative body,
no co-author trailer.
