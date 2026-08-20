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

**Historical note (added post-ADR-037, 2026-08-13):** every `quark::`/`Quark` reference below (e.g.
`quark::Engine`, `quark::TestKit`, Quark's `EventLog<T,S>`/`Store`) is a dated, point-in-time record
of what this milestone's phases were actually built and proven against as of 2026-08-08, matching
this doc's own append-only "Outcome" log convention — it is not a claim about what exists today.
`decisions/ADR-037-remove-quark-as-core-runtime.md` (executed 2026-08-13, after this doc's most
recent dated entry) later removed Quark as a dependency entirely; every test file this doc names as
proven "under `quark::TestKit`" or "against a REAL `quark::Engine`" has since been ported onto
`agentengine::rt::` equivalents (see `tests/CMakeLists.txt`'s own per-test ADR-037 migration notes).
Anyone resuming Milestone 7 work from this doc should read its Quark mentions as history, and check
current test files directly for what they actually run against today.

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
  - **D2** — Agent Card generation (`protocol/a2a/agent_card.hpp`) from a real, `register_agent<A>()`-
    compiled `AgentMetadata`: skills derived one-per-tool from the real `ToolTable`, §2.1's own
    "advertises only what the conformance suite proves" rule enforced by construction (no binding
    exists yet, so `supportedInterfaces`/`capabilities.streaming`/`pushNotifications` default empty/
    false rather than fabricated).
  - **D3+** — JSON-RPC binding (server role: `SendMessage`/`GetTask`/`CancelTask` wired to a real
    `AgentSession`, `Task ← Run` projection off 013 §1's event stream and `Interaction`/
    `open_interactions`), client role (remote-agent-as-tool binding, digest-pinned card caching),
    streaming (`SubscribeToTask`), push notifications, extensions, authentication — scoped in more
    detail as each is reached, per the user's own 2026-08-08 "full autonomous push through D2+, same as
    Phase C" decision superseding the earlier per-sub-phase check-in.

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

  **Outcome, D2 (2026-08-08, commit pending):** `protocol/a2a/agent_card.hpp` (new) — `AgentCard`/
  `AgentInterface`/`AgentSkill`/`AgentCapabilities` (§A.3's own field shapes) plus `to_agent_card()`,
  which reads exactly what `AgentMetadata` (002, agent_registry.hpp) actually carries today —
  `agent_name` and a real `ToolTable` (one `AgentSkill` per registered tool, name/description straight
  from `ToolDescriptor`, the same "one schema source" discipline `protocol/mcp/server.hpp`'s own
  `to_mcp_tool_list_entry()` already established for `tools/list`) — and nothing it does not.
  `description`/`version`/`defaultInputModes`/`defaultOutputModes` are explicit caller-supplied
  `AgentCardIdentity` fields rather than fabricated from `agent_instructions`: `AgentMetadata` (002-
  owned, M2/M5 vintage) has no description/version/modality-declaration fields yet, a real gap named
  here rather than papered over. **Update (2026-08-14):** the description/version half of this gap is
  closed — `decisions/ADR-044-agent-workflow-description-version.md` gave `AgentMetadata` real
  `agent_description`/`agent_version` fields and wired `to_agent_card()` to fall back to them when
  `AgentCardIdentity`'s own fields are left default-empty; the modality-declaration
  (`defaultInputModes`/`defaultOutputModes`) half named here stays open, per that ADR's own §5 scope
  note. §2.1's own "the card advertises only what the conformance suite
  proves" rule is enforced BY CONSTRUCTION, not just by convention: `supported_interfaces` defaults
  empty and `capabilities.streaming`/`push_notifications` default `false`, since no JSON-RPC/REST
  binding or streaming/push machinery exists yet (D3+'s own job) — proven directly (D2-3). `AgentSkill.
  tags` is always a present-but-empty array (§A.3 requires the field; no tag vocabulary exists
  anywhere in `core/tool.hpp` to populate it from, so it is never invented from a tool's name/
  description text, proven D2-7). `tests/test_a2a_agent_card.cpp` (new, 15 checks, all passing)
  proves all of the above against two REAL `register_agent<A>()`-compiled agents (one with two tools,
  one with none), including that a caller-supplied `AgentInterface` (the shape a real D3+ binding will
  actually populate) round-trips untouched. 136/136 full suite (was 135 after D1).

  **What is honestly NOT built for D2**: the optional §A.3 fields (`provider`, `documentationUrl`,
  `securitySchemes`, `securityRequirements`, `signatures` — §4a's JWS signing, `iconUrl`); the
  well-known-URI HTTP surface (`GET /.well-known/agent-card.json`) that would actually SERVE this card
  (transport work, D3+); `AgentSkill.examples`/`inputModes`/`outputModes` population (present in the
  type, never populated — no source for them exists yet, same honesty as `tags`); skill plugins (009
  §8) contributing additional skills beyond the declared tool set.

  **Outcome, D3 (2026-08-08, commit pending):** `protocol/a2a/mapping.hpp` (new) — the `ContentItem`/
  `Message` (003, internal) ↔ `Part`/`Message` (012, wire) projection §1 calls "thin by construction."
  Deliberately narrow on content: `Text`/`Data`/`Error` are the three `ContentItem` kinds this
  codebase's own pipelines actually produce (the identical set `protocol/mcp/server.hpp` already
  names for the same reason); every other kind (`Reasoning`/`Media`/`ToolCall`/`ToolResult`/
  `Citation`/`Custom`) falls back to `UnknownPart` — REUSING D1's own forward-compat shape rather than
  inventing a second one. Role mapping is lossy in a named direction only: `role::system`/`role::tool`
  (no A2A equivalent) map to `a2a_role::unspecified`, never silently promoted to user/agent; the
  reverse direction only ever produces `role::user`/`role::assistant` (an inbound A2A message is
  always a peer speaking to us, 012 §3). `protocol/a2a/server.hpp` (new) — `A2aServer`
  (`send_message`/`get_task`/`cancel_task`), transport-agnostic like `McpServer`: handed a
  `RunStarter` seam rather than actor-messaging plumbing itself, this phase's own test wiring one
  against a REAL `quark::Engine`-hosted `AgentSession`. `Task.id` really IS the session's own
  `run_id` (`AgentSession::last_run_id()`, read right after the `Ask<StartRun,AgentResponse>`
  settles) — §1's "Task ← Run" identity, proven directly (D3-1), not asserted. Every OTHER task-
  lifecycle claim is scoped to exactly what `AgentSession`'s current (still M1-era, fully synchronous,
  no tool-call loop) turn loop can really produce: `TASK_STATE_COMPLETED` for a real successful run,
  `TASK_STATE_FAILED` (with a dispatcher-minted, non-run-id task id, since no run_id exists to borrow
  when the ask never settles) for every failure shape `AgentSession` collapses to one signal today —
  proven against a second real session with a genuinely failing `ChatClient` (D3-8), not a mock
  return value. `cancel_task()` always rejects, faithfully: every task this dispatcher can produce is
  ALREADY terminal by the time it's observable (fully synchronous dispatch), so §2.3's "terminal is
  terminal" is proven honestly rather than fabricating a `CANCELED` transition this implementation
  cannot really perform (D3-6). `tests/test_a2a_mapping.cpp` (new, 14 checks) and
  `tests/test_a2a_server.cpp` (new, 17 checks), all passing. 138/138 full suite (was 136 after D2).

  **Bug found and fixed while building this phase**: the test harness's `LocalRouter` was initially a
  constructor-local variable, but Quark's `ActorRef` holds a raw `LocalRouter*` internally — the
  router was destroyed the instant the constructor returned, leaving `ref` dangling and segfaulting on
  first use. Fixed by making `router` a class member with the same lifetime as `ref` itself, matching
  what every prior real-`Engine` test in this codebase does by keeping router/ref/engine as sibling
  locals in one function scope — this was the first time that pattern got wrapped in a reusable test
  harness type, which is what surfaced the ordering assumption.

  **What is honestly NOT built for D3**: `TASK_STATE_SUBMITTED`/`WORKING` (never independently
  observable — `send_message()` itself blocks until the run settles, no `returnImmediately: true`
  async dispatch exists); `TASK_STATE_INPUT_REQUIRED`/`AUTH_REQUIRED` (the turn loop never opens an
  `Interaction` on its own — `open_interaction()`/`resolve_interaction()` are host-callable but
  unwired, agent_session.hpp's own Phase E1 comment); a genuinely cancellable in-flight task (needs
  async dispatch, D4+); the JSON-RPC/REST envelope itself (this is a pure dispatcher, no wire
  encoding, matching MCP's own "envelope first" C1-before-C2 split — this is the "roles" layer built
  before A2A's own envelope exists); `ListTasks`, push-notification config CRUD, streaming
  (`SubscribeToTask`/`SendStreamingMessage`), extensions, authentication/authorization, digest-pinned
  Agent Card caching (client role, not built at all yet).

  **Outcome, D4 (2026-08-08, commit pending):** `protocol/a2a/client.hpp` (new) — `A2aClient`
  (`send_message`/`get_task`/`cancel_task`, plain passthrough over a `RemoteAgentTransport` seam —
  three callables mirroring §A.2's own `SendMessage`/`GetTask`/`CancelTask` table, the same "no real
  transport yet, take what a transport would supply as given" layering `A2aServer`'s own `RunStarter`
  already established), proven against a REAL `A2aServer` (D3) over a real `quark::Engine`-hosted
  `AgentSession`, not a stub. `fetch_agent_card()` adds §4a's own digest-pinned caching: an FNV-1a
  digest over the card's own JSON (the same non-cryptographic change-DETECTION idiom `McpClient`'s
  own `digest_of()` already establishes for MCP tool listings, §8 there / §4a here) flags a card whose
  content changed under repeated fetches as a rug pull — "re-approved rather than silently trusted" —
  never silently overwriting the prior trust decision. `tests/test_a2a_client.cpp` (new, 12 checks,
  all passing) proves the real send/get/cancel passthrough plus caching/no-rug-pull/rug-pull-detected/
  fetcher-failure across four independent `A2aClient` instances sharing one real transport. 139/139
  full suite (was 138 after D3).

  **Bug found and fixed while building this phase**: `agent_card.hpp` (D2) and `types.hpp` (D1) had
  each independently defined `agentengine::a2a::detail::strings_to_json()` — harmless while no file
  included both, but `client.hpp` needed `AgentCard` (agent_card.hpp) alongside `Task`/`Message`
  (types.hpp) and pulled in both, producing a same-translation-unit redefinition error. Fixed by
  having `agent_card.hpp` include `types.hpp` and reuse its copy instead of duplicating — the two
  files are the same feature/namespace, unlike the deliberate per-feature base64/FNV-1a duplication
  this codebase uses elsewhere to avoid coupling UNRELATED features.

  **What is honestly NOT built for D4** (all D5+ scope): binding a remote agent as a local `Tool<T>`
  (§3's own "same declaration syntax as a local one," 012 §8's G5 gate — touches `core/tool.hpp`'s
  zero-cost CRTP machinery, a Phase G promotion-gate proof, not built speculatively here); deadline/
  cancellation PROPAGATION to a remote task (this client relays a caller's `cancel_task()` call, but
  derives or forwards no deadline); `Suspended`-state mapping for a long-running remote task; JWS
  card-signature verification (§4a); the JSON-RPC/REST transport itself (still a pure dispatcher on
  both client and server sides, matching D3's own scope note).

  Phase D's four sub-phases now cover the same conceptual ground Phase C covered for MCP (wire
  envelope/types, server role, client role) plus the Agent Card piece MCP has no equivalent of. The
  remaining A2A scope (D5+: the JSON-RPC/REST binding envelope itself, streaming, push notifications,
  extensions, OAuth 2.1 authorization, remote-agent-as-tool binding for G5, TCK conformance tooling)
  is transport/security-critical surface, the same category of work the user already chose to defer
  for MCP (Phase C4+) in favor of moving on to the next phase — see the 2026-08-08 checkpoint after
  this phase for how that was resolved for Phase D.
- **Phase E** — 013 §2-§4: AG-UI projection, other-surface table, transport (SSE, binary framing,
  WebSocket), cross-surface equivalence proofs.

  Sub-phases (same "vocabulary first, wiring after" discipline C1/D1 used for MCP/A2A's own wire
  types before any role/projection logic was built on top):
  - **E1** — the AG-UI event vocabulary itself (`protocol/agui/types.hpp`): every event kind §2.1/§2.2
    names, with exact wire identifiers/field shapes cited from `docs/research/2026-a2a-and-agui-detail.md`
    Part B (013 §2.0's own maturity note: AG-UI has no formal spec, so this is pinned to a dated,
    sourced schema snapshot, never asserted from memory) — no projection from the internal `RunEvent`
    stream yet.
  - **E2+** — the projection itself (internal `RunEvent`, real since Phase A, onto `AgUiEvent`),
    §2.2's interrupt-ends-the-run mapping, other-surface projections (A2A streaming, MCP progress),
    transport framing (SSE/binary), cross-surface equivalence proofs (§6 G3) — scoped in more detail
    as each is reached.

  **Outcome, E1 (2026-08-08, commit pending):** `protocol/agui/types.hpp` (new) — `AgUiEvent`, a
  25-alternative variant covering every §2.1/§2.2 event category (lifecycle: `RunStarted`/
  `RunFinishedSuccess`/`RunFinishedInterrupt`/`RunError`/`StepStarted`/`StepFinished`; text; tool
  call; state; activity; reasoning; the `Raw`/`Custom` escape hatches), `event_type_name()` and
  `to_json()` proving every alternative serializes with its exact cited wire identifier
  (`RUN_STARTED`, `TEXT_MESSAGE_CONTENT`, ... — 013 §2.1: "Exact identifiers, since they are the
  contract") and camelCase field names. `RunFinishedSuccess`/`RunFinishedInterrupt` are deliberately
  TWO structs sharing one wire `"type":"RUN_FINISHED"` (rather than one struct with an optional
  outcome) so a caller cannot construct the nonsensical "finished, no outcome at all" state — proven
  directly (both produce `RUN_FINISHED`, with different `outcome` shapes). `tests/test_agui_types.cpp`
  (new, 22 checks, all passing) proves every alternative. 141/141 full suite (was 140 after ADR-022,
  which added no product code of its own).

  **What is honestly NOT built for E1**: the `*_CHUNK` "auto-expanding convenience form" events
  (`TEXT_MESSAGE_CHUNK`/`TOOL_CALL_CHUNK`/`REASONING_MESSAGE_CHUNK`) — client-side ergonomic sugar
  over the canonical START/CONTENT/END triad this file emits, not a distinct wire concept a projector
  needs to produce; the deprecated `THINKING_*` family (013 §2.1: "we do not emit it"); the Draft
  `MetaEvent`; and — the larger gap — no projection from the internal `RunEvent` stream exists yet at
  all (E2+'s own job). `StateDelta`/`ActivityDelta`'s `patch` fields carry whatever RFC 6902 JSON
  Patch array a caller supplies; this file has no diff GENERATOR (no state-diffing engine exists
  anywhere in this codebase yet), only the wire shape for one.

  **Outcome, E2 (2026-08-08, commit pending):** `protocol/agui/projection.hpp` (new) —
  `RunEventProjector`, the first STATEFUL projection in this milestone (every prior MCP/A2A mapping,
  Phases C/D, was a pure function of one item): AG-UI's `TEXT_MESSAGE_START/CONTENT/END` triad needs a
  `messageId` to bracket a model's incremental text output, and `RunEvent`'s own `ModelDelta` payload
  carries none, so the projector tracks, per `run_id`, which `messageId` is currently open — minted on
  `model_call_started`, closed on `model_call_finished`, with a defensive lazy-open fallback if
  `model_delta` ever arrives out of order (proven, E2-4). §2.2's hard rule is implemented directly: a
  run entering `input_required`/`auth_required`/`approval_requested` does NOT pause (AG-UI has no
  pause event) — it ENDS the AG-UI-visible run via `RunFinishedInterrupt`, with the internal
  `interaction_id`/`call_id` becoming the `Interrupt.id` verbatim (013 §2.2: "AG-UI — interruptId IS
  the interaction_id"), `auth_required` using the `ae:auth_required` extension namespace (no native
  auth member in AG-UI's own reason enum) and `approval_requested` using the native `confirmation`
  reason — proven for all three (E2-11/12/13). Two deliberate, named divergences from a naive reading
  of §2.1's own summary table: `tool_call_delta` (the TOOL's own progress report, distinct from the
  MODEL's argument-construction streaming this codebase has no separate event kind for yet) projects
  to `CustomEvent`, not `TOOL_CALL_CHUNK` — the cited research record gives no field shape for any
  `*_CHUNK` variant, and inventing one would violate "research is dated and cited" worse than using
  the `CUSTOM` escape hatch 013 §2.1 itself sanctions; `state_changed` projects to `CustomEvent`, never
  a fabricated RFC 6902 patch, since no state-diffing engine exists anywhere in this codebase. Proven
  BOTH ways, honestly split (matching Phase A's own test precedent for the identical real/unwired
  split): end to end against a REAL `AgentSession` turn loop via `enable_event_stream()` (`TestKit`-
  driven) for the 6 event kinds that loop actually emits today — a full success-path run (E2-1) and a
  full failure-path run (E2-2), both projected from genuinely-fired internal events, not synthetic
  ones — and via hand-fed `RunEvent`s for every other vocabulary kind (`model_delta`, `tool_call_*`,
  `sandbox_exec_*`, `state_changed`, `artifact_produced`, all four interrupt/resolution pairs,
  `run_canceled`, `warning`, `policy_decision`), honestly labeled as unwired rather than claimed
  end-to-end. `tests/test_agui_projection.cpp` (new, 32 checks, all passing). 142/142 full suite (was
  141 after E1).

  **What is honestly NOT built for E2**: `input_resolved`/`auth_resolved`/`approval_resolved` project
  to nothing (proven, E2-14) — by the time any of these fires the AG-UI-visible run already ended;
  turning a resolution into a NEW run's `RunAgentInput.resume[]` is a caller-side concern (§2.2's own
  resumption model) this single-event projector does not own; `ToolCallResult` is never emitted
  (`ToolCallFinished`'s payload carries only `{call_id, ok}`, no real result content to report yet);
  `STATE_SNAPSHOT`/`MESSAGES_SNAPSHOT` (no session-history/state-serialization caller wired to this
  projector yet); the pre-`RUN_FINISHED`-snapshot ordering obligation §2.2 states for a resumable
  interrupt (needs a caller that actually emits a snapshot before calling this projector, not this
  file's own job to enforce).

  **Outcome, E3 (2026-08-08, commit pending):** `protocol/agui/sse.hpp` (new) — `to_sse_frame()`/
  `to_sse_stream()`, §4's exact framing (cited from `docs/research/2026-a2a-and-agui-detail.md` §B.3:
  "SSE (default): `text/event-stream`, one `data: <JSON BaseEvent>` frame per event") — a single
  `"data: " + json::dump(...) + "\n\n"` line per event. Proven directly why a single `data:` line is
  always safe rather than merely assumed: `json::dump()`'s own string escaping means an embedded
  newline inside event content (e.g. a multi-line `TextMessageContent.delta`) never appears as a
  literal byte in the framed output — the whole frame carries exactly two newline bytes (the trailing
  terminator), proven against a delta deliberately containing `\n` (E3-2), not just against
  newline-free fixtures that would pass vacuously. Proven end to end (E3-3): a real `RunEventProjector`
  output, framed and concatenated, preserves event order. `tests/test_agui_sse.cpp` (new, 10 checks,
  all passing). 143/143 full suite (was 142 after E2).

  **What is honestly NOT built for E3**: binary protobuf framing (§4's other named encoding, a 4-byte
  big-endian length prefix + protobuf-encoded event) — no protobuf library is vendored anywhere in
  this codebase; adopting one is a real, separate dependency decision under CONVENTIONS' tier
  discipline (a seam backend may take one heavy dependency behind a CMake option, the mbedTLS/
  wasmtime/CPython precedent), not a drive-by inside this phase. WebSocket transport (§4's third named
  option, for bidirectional mid-run input/approvals). No actual network listener anywhere — this is
  framing logic only, matching ADR-021/ADR-022's own explicit deferral of the listener itself.

  Phase E has now built a complete, real, tested slice of AG-UI's own core wire path end to end: the
  event vocabulary (E1), the projection from a real running session (E2), and the wire framing (E3).
  What remains in Phase E's own original scope (§3's other-surface projections — A2A streaming, MCP
  progress notifications — touching the already-built A2A/MCP code from Phases C/D rather than new
  AG-UI ground, plus §6 G3's cross-surface equivalence proof) is scoped but not started.

  **Outcome, E4 (2026-08-08, commit pending):** `protocol/a2a/streaming.hpp` (new) —
  `TaskStatusUpdateEvent`/`TaskArtifactUpdateEvent`/`StreamResponse` (§A.4's wire envelope types D1
  scoped out — D1 built the `Task`/`Message`/`Part`/`Artifact` object model, not the streaming
  envelope around it) and `A2aStreamProjector`, mapping `RunEvent` onto A2A's real task-lifecycle
  states. A2A's task model is coarser-grained than AG-UI's own event stream (012 §1: one
  task-lifecycle state machine per run, no wire slot for turn/model/tool-call granularity) — most
  `RunEvent` kinds have no A2A streaming projection at all, honestly returning an empty vector rather
  than fabricating one (proven directly). The notable, RFC-confirming finding: `input_required`/
  `auth_required` map onto REAL, NON-terminal A2A task states (`TASK_STATE_INPUT_REQUIRED`/
  `TASK_STATE_AUTH_REQUIRED`, proven non-terminal via `is_terminal()`) — unlike AG-UI, which has no
  pause event and must END the run (E2's own `RunFinishedInterrupt`). This is 012 §5a's "three
  incompatible shapes for one idea" made concrete in code: MCP retries, AG-UI restarts, A2A
  *continues* — the same internal `input_required` event produces a genuinely different SHAPE of
  reaction on each surface, not just a different wire encoding of the same reaction.
  `approval_requested` honestly collapses onto `TASK_STATE_INPUT_REQUIRED` (A2A has no distinct
  "confirmation" state); `artifact_produced` produces a real `TaskArtifactUpdateEvent` with an
  HONESTLY EMPTY `parts[]` (`ArtifactProduced`'s own payload carries no content to populate them
  from). `protocol/mcp/progress.hpp` (new) — `McpProgressProjector`, narrowly scoped to
  `tool_call_delta` only (013 §3's own table names exactly this one source, not a vocabulary to map
  exhaustively), with a per-`progressToken` monotonically-increasing counter closing the cited spec
  MUST ("the progress value MUST increase with each notification",
  `docs/research/2026-mcp-protocol-detail.md` §10) — proven directly, including that a different token
  gets its own independent counter. `tests/test_a2a_streaming.cpp` (new, 13 checks),
  `tests/test_mcp_progress.cpp` (new, 8 checks), and `tests/test_cross_surface_equivalence.cpp` (new,
  11 checks) — the last one is 013 §6 G3's own real evidence: ONE real `AgentSession` run (both a
  success path and a failure path), the SAME captured internal event sequence fed through both
  `RunEventProjector` and `A2aStreamProjector`, proving both surfaces agree on outcome (neither ever
  shows success while the other shows failure) and relative ordering (both observe "started" strictly
  before "finished"), judged at the outcome level rather than byte-identical wire content — which is
  impossible across two structurally different protocols and not what G3 asks for. All passing.
  146/146 full suite (was 143 after E3).

  **What is honestly NOT built for E4**: `TaskStatusUpdateEvent.status.message` is never populated (no
  `Message` content is threaded through from the internal event, only the bare state transition);
  `TaskArtifactUpdateEvent.append`/`last_chunk` (no chunked-artifact-streaming source exists yet);
  MCP's `total` field for progress notifications (nothing in `ToolCallDelta`'s payload carries one);
  full G1-G6 promotion-gate EXECUTION (§6) — this phase produces real evidence toward G3 specifically,
  not a claim that the gate itself has been run; that is Phase G's own job at milestone close.
- **Phase F** — 015: declarative agent/workflow YAML, the shared validator, equivalence corpus.

  Two hard infrastructure gaps were confirmed absent before any sub-phase was scoped (2026-08-08
  research pass): no YAML parser anywhere in this codebase, and no GENERIC JSON Schema 2020-12
  validator (`core/json_schema.hpp`'s `AE_JSON_SCHEMA` macro only ever GENERATES a schema from a known
  C++ type, one direction only — it cannot validate an arbitrary YAML-sourced document against an
  arbitrary schema). The workflow side is in materially better shape: `workflow/graph.hpp`'s own
  `Workflow` struct + `validate_workflow()` (Milestone 6) were deliberately designed so "the
  declarative loader (015, Milestone 7) calls the SAME validator rather than reimplementing it"
  (`graph.hpp`'s own comment) — a real, already-built compile target for a future Workflow-document
  compiler, unlike the Agent-document side which has no equivalent shortcut. Given the combined size
  of "YAML parser + generic JSON-Schema validator + Agent-document compiler + Workflow-document
  compiler + I6 byte-identical-metadata equivalence corpus" — one of the largest single asks in this
  milestone — the user explicitly chose a narrow first slice (2026-08-08) over committing to the full
  stack up front.

  Sub-phases:
  - **F1** — a real, tested YAML-SUBSET parser (`core/yaml_value.hpp`), proven against 015's own §2/§3
    example documents, deferring the JSON-Schema validator and both document compilers.
  - **F2+** — scoped in more detail as each is reached: the JSON-Schema 2020-12 validator, the
    Agent-document compiler (YAML → `AgentMetadata`-equivalent), the Workflow-document compiler
    (YAML → `Workflow` → `validate_workflow()`), the I6 equivalence corpus (§7 G1/G2).

  **Outcome, F1 (2026-08-08, commit pending):** `core/yaml_value.hpp` (new) — a hand-rolled YAML-subset
  parser matching CONVENTIONS' core-tier discipline ("std + Quark only, no third-party dependency,
  ever") the same way `json_value.hpp` already does for JSON — no YAML library exists anywhere in this
  codebase or is vendored here. Parses DIRECTLY into the existing `agentengine::json::Value` tree
  rather than a second value type, since YAML's data model (mappings/sequences/scalars) maps cleanly
  onto JSON's for the subset this parser targets — every future consumer (a document compiler) works
  with the exact same `json::Value` API MCP/A2A/AG-UI already use. Scope, proven directly rather than
  merely claimed: block-style mappings/sequences, including a sequence item that opens a mapping
  inline (`- key: value` continued by further same-indent keys — 015 §3's own `executors`/`edges`
  shape in ONE valid form); flow-style `{ }`/`[ ]` collections, YAML-FLAVORED (unquoted keys/values
  allowed, unlike strict JSON — 015 §2's own `options: { temperature: 0.2 }` requires this); block
  literal `|` scalars with default ("clip") chomping (015 §2's own multi-line `instructions:` block);
  quoted (single/double, with real escape handling) and plain scalars, `#` comments, `null`/`~`. A
  literal tab in indentation is rejected, not silently accepted (real YAML's own rule).

  Proven primarily against 015's OWN example documents, parsed VERBATIM, not paraphrased or simplified
  fixtures: §2's full Agent document (Y-9) and §3's full Workflow document (Y-10) — both mix block and
  flow style throughout (§3's `executors`/`edges` entries are actually `- { ... }` FLOW mappings inside
  a block sequence, a different valid shape from the block-continuation form Y-4 separately proves),
  which is why this parser had to support both styles from the start rather than deferring flow style
  as a later increment. `tests/test_yaml_value.cpp` (new, 45 checks, all passing, all correct on the
  first real test run against this complexity of parser — worth noting plainly, not oversold: real
  YAML edge cases and adversarial-input hardening beyond the negative suite built here remain
  unexercised, a named residual, not a claim that no further scrutiny is warranted). 147/147 full suite
  (was 146 after E4).

  **What is honestly NOT built for F1**: anchors/aliases (`&name`/`*name`), tags (`!!type`),
  multi-document streams (`---`/`...`), explicit block-scalar indentation/chomping indicators
  (`|2`/`|-`/`|+`), merge keys (`<<:`) — all named directly in the header's own file-top comment, a
  document using any of them is rejected with a real parse error, never silently misparsed. The
  generic JSON-Schema 2020-12 validator, the Agent-document compiler, the Workflow-document compiler,
  and the I6 equivalence corpus are ALL still unbuilt — this phase is the parser only, the narrow first
  slice the user explicitly chose.

  **Outcome, F2 (2026-08-08, commit pending):** `workflow/yaml_compiler.hpp` (new) —
  `compile_workflow_document()`, a parsed 015 §3 Workflow document → `workflow/graph.hpp`'s own
  `Workflow` struct, checked by the SAME `validate_workflow()` the C++ `WorkflowBuilder` authoring
  form already uses — the actual I6 property this compiler exists to serve, not merely asserted.

  A real, honest finding surfaced while building this, not papered over: **015 §3's own illustrative
  example document has NO `input_type`/`output_type` anywhere**, but 014 §1's port-typing rule
  (`validate_workflow()`'s own `workflow.untyped_port` check) requires both on every executor. The
  RFC's own example COMPILES (`compile_workflow_document()` itself has no opinion on port types) but
  `validate_workflow()` correctly REJECTS it with `workflow.untyped_port` — proven directly (W-1), the
  same diagnostic a hand-written C++ workflow with an untyped port would also produce, never an
  invented placeholder type manufactured to make an underspecified example silently "pass." An
  EXTENDED version of the identical document (adding `input_type`/`output_type` per executor, nothing
  else changed) compiles AND validates successfully (W-2) — I6 in action: the declarative and C++
  forms are checked by the literal same function, so an agreement between them is structural, not
  coincidental.

  Scope, matching what 015 §3's own example edges actually use: `to` (direct), `fan_out_to` (a list —
  compiling to N SEPARATE `Edge` records sharing one `from`, since `graph.hpp`'s own `Edge` struct is
  single-target by design, proven with a real multi-target case, W-3), `fan_in_to` (single target); an
  explicit `kind:` always overrides `agent:`-implied `executor_kind::agent` inference (015 §4's own
  "strict" posture — an author's explicit statement is never silently overridden); a small real
  duration parser for `limits.deadline` covering `ms`/`s`/`m`/`h` (proven for all four, plus a rejected
  unrecognized unit, W-6). `tests/test_workflow_yaml_compiler.cpp` (new, 24 checks, all passing, all
  correct on the first real run). 148/148 full suite (was 147 after F1).

  **What is honestly NOT built for F2**: `switch_case`/`multi_selection`/`chain` edge kinds and
  per-edge `on_failure` failure policy (014 §6) — 015 §3's own example never uses any of them, and
  extending the YAML edge shape to cover them is real, separate follow-up work, not a drive-by here;
  `metadata.version` has no home in `Workflow`'s own current shape (pure graph data, no
  document-identity/versioning field) and is read then honestly dropped, the same "the struct doesn't
  have a slot for this yet" finding D2 already recorded for `AgentMetadata`'s own missing description/
  version fields (**closed 2026-08-14, ADR-044** — `Workflow` gained real `description`/`version`
  fields too, see F3's own update below); the generic JSON-Schema 2020-12 validator, the Agent-document
  compiler, and the I6 equivalence CORPUS (a systematic sweep over every 002 §3 policy, §7 G1's own
  bar) are all still unbuilt.

  **Outcome, F3 (2026-08-08, commit pending):** `core/agent_yaml_compiler.hpp` (new) —
  `compile_agent_document()`, a parsed 015 §2 Agent document → a REAL `AgentMetadata`, the same
  compiled type `register_agent<A>()` (the C++ authoring form) produces.

  Two real, blocking gaps were confirmed absent before this file was written, both named rather than
  worked around: **no tool/capability NAME-KEYED REGISTRY exists anywhere in this codebase** (015 §1
  itself assumes one — "the declarative form references tools by name from the registry"), and, even
  given one, **`core/tool_pipeline.hpp`'s own `ToolTable` has no runtime construction API at all** —
  `from_tools<ToolTs...>()` is a compile-time template over real C++ `Tool` types, and `descriptors_`
  is private with no public append/builder surface. `tools`/`capability_ceiling` therefore stay
  HONESTLY EMPTY (proven directly, F-1) rather than fabricated — a tool/capability registry is
  006/009's own scope, not 015's, and a runtime `ToolTable` builder is a `tool_pipeline.hpp` API
  change, neither a drive-by inside a document compiler. `output_schema_json` stays unset for a
  separate, third reason: 015 §2's own example supplies only a `{$ref: ...}` pointing at an EXTERNAL
  FILE, and resolving/loading/compiling one is real file-IO work out of this compiler's own scope.

  `metadata.version`/`metadata.description` have no home in `AgentMetadata`'s current shape — read,
  then honestly dropped, confirmed now a THIRD independent time (after Phase D2's Agent Card generator
  and Phase F2's Workflow-document compiler each hit the identical gap on their own compiled targets).
  Three independent compilers landing on the same missing fields is a real signal `AgentMetadata`
  (002-owned) genuinely needs them at some point — outside this file's own authority to add.
  **Update (2026-08-14):** exactly this signal is what `decisions/ADR-044-agent-workflow-description-
  version.md` acted on — `AgentMetadata`/`Workflow` both gained real `description`/`version` fields,
  and `compile_agent_document()`/`compile_workflow_document()` now read them for real instead of
  honestly dropping them. The A2A modality-declaration fields (`defaultInputModes`/
  `defaultOutputModes`, D2's own separate note) remain out of scope of that fix.

  The actual I6 property, proven directly rather than merely asserted (F-2): a hand-written C++
  `Agent` equivalent to 015 §2's own example (`ChatClientId<"anthropic:claude-opus-5">`,
  `MaxTurns<12>`, `TokenBudget<200000>`, `Approval<policy_driven>`), compiled via the REAL
  `register_agent<A>()`, agrees FIELD-FOR-FIELD with the YAML-compiled `AgentMetadata` on
  `agent_name`/`agent_instructions`/`chat_client_id`/`max_turns`/`token_budget`/`approval` — every
  field this narrow slice covers. `tests/test_agent_yaml_compiler.cpp` (new, 27 checks, all passing,
  all correct on the first real run). 149/149 full suite (was 148 after F2).

  **What is honestly NOT built for F3**: `tools`/`capability_ceiling` (blocked on the two gaps above),
  `output_schema_json`/`output_schema_strategy_chosen` (blocked on external-`$ref` file resolution),
  `provider.options` (no compile-time metadata slot in EITHER form — not a declarative-only gap), the
  generic JSON-Schema 2020-12 validator, and the I6 equivalence CORPUS (§7 G1's own bar: a systematic
  sweep over every 002 §3 policy, not the single hand-picked cross-check this phase proves).

  Phase F now has a real YAML parser (F1), a Workflow-document compiler (F2), and an Agent-document
  compiler (F3) — both compilers cross-checked against their real C++ authoring-form counterparts for
  I6. What remains (the JSON-Schema validator, the tool/capability registry + runtime `ToolTable`
  builder that would unblock full tools/capabilities compilation, the systematic equivalence corpus,
  §7 G2's negative-corpus strictness gate) is now a list of individually-scoped, well-understood gaps
  rather than an undifferentiated unknown.
- **Phase G** — promotion gates: 011 §10 G1-G9, 012 §8 G1-G5, 013 §6 G1-G6, 015 §7 G1-G4, 006 §6b's
  G6-G9 — run for real, published percentages where the gate asks for one, milestone close-out.

  **Gate audit (2026-08-08)** — before attempting to close any individual gate, every gate across all
  five RFCs was checked against what Phases A-F actually built, with a citation for every verdict.
  Most gates presuppose infrastructure this milestone deliberately did not build (a real network
  listener/transport — ADR-021/ADR-022 both explicitly deferred it; official conformance-tool
  integration; a reference AG-UI client; recording/execution-tracing integration; a document
  digest/content-addressing scheme) — this audit's job is to say so precisely, per gate, rather than
  leave the milestone's own exit criterion an open question.

  **MET (real, executed evidence):**
  - **006 §6b G8** — cross-principal `cancel_standing_effect` denial (Phase B, `test_agent_session_background_task.cpp`).
  - **006 §6b G9** — `Background<max_concurrent>` cap rejection at authorize (Phase B; reconfirmed through the MCP tasks-extension path, Phase C4-7).
  - **013 §6 G3** — cross-surface equivalence: one real `AgentSession` run projects consistently to AG-UI and A2A (Phase E4, `test_cross_surface_equivalence.cpp`, 11 checks).

  **PARTIALLY MET (real evidence, narrower than the gate's own full bar):**
  - **006 §6b G7** — the "does not block the turn" half is proven (Phase B); full survival across a
    real suspend/resume or process restart is NOT proven — `StandingEffect` is serializable but not
    yet threaded into `AgentSessionRecord`'s own checkpoint (named in Phase B's own outcome).
  - **011 §10 G5 (cache correctness)** — `ttlMs`/cache-key correctness proven (Phase C3); cross-
    principal `cacheScope` isolation is explicitly NOT built (Phase C3's own scope note).
  - **011 §10 G6 (rug-pull)** — detection is proven (`McpClient::rug_pull_detected()`, Phase C3-8);
    the flag surfacing into an actual re-approval gate that BLOCKS a subsequent call is explicitly NOT
    built (Phase C3's own scope note).
  - **011 §10 G7 (authorization negative suite)** — the underlying bearer-token MECHANISM is fully
    proven (ADR-021's prove phase: wrong audience, expired, replayed, tampered signature, wrong
    issuer, 32 checks) but is not integrated into an actual MCP request flow (no listener exists).
  - **012 §8 G2 (round-trip fidelity)** — every `Part` kind including unknown ones round-trips (Phase
    D1, 30 checks), but not the full systematic corpus across nested `Message`/`Artifact`/`Task`
    depths the gate's own "over a corpus" language implies.
  - **012 §8 G3 (full lifecycle coverage)** — `INPUT_REQUIRED`/`AUTH_REQUIRED` as real, non-terminal
    task-state transitions are proven (Phase D3, Phase E4); "terminal is terminal" is proven (Phase
    D3-6); genuine cancel-IN-FLIGHT is NOT provable — every task this codebase's dispatcher can
    produce is already terminal by the time it is observable (Phase D3's own scope note, an honest
    limitation of the fully-synchronous dispatch this milestone built, not an oversight).
  - **015 §7 G1 (equivalence)** — a real, field-for-field cross-check between the YAML and C++ forms
    is proven for both document types (Phase F2, Phase F3) — but each is ONE hand-picked comparison,
    not the systematic corpus "covering every policy in 002 §3" the gate's own text requires.

  **BLOCKED (no infrastructure to run against yet, cited to what's missing):**
  - **011 §10 G1/G2/G3/G4/G8/G9** — no real HTTP transport/listener (ADR-021/ADR-022 both explicitly
    deferred building it), no official `@modelcontextprotocol/conformance` tool integration, no
    replica/deployment infrastructure, no fault-injection harness, no `traceparent`/016 telemetry
    integration, no `server.json` registry publishing.
  - **012 §8 G1/G4/G5** — no `a2a-tck` integration, no push-notification delivery mechanism at all
    (named absent since Phase D's own scoping), no remote-agent-as-tool binding (Phase D4's own named
    residual — touches `core/tool.hpp`'s CRTP machinery, scoped as Phase G's own future work there).
  - **013 §6 G1/G2/G4/G5/G6** — no AG-UI reference-client integration, no backpressure/memory
    measurement harness, no recording integration for the `RunEvent` stream, no AG-UI compatibility
    suite or access to AG-UI's own client-side verifier, binary protobuf framing not built (Phase E3's
    own named residual — no protobuf library vendored anywhere).
  - **015 §7 G2/G3/G4** — the specific negative-corpus cases the gate names (secret literal, capability-
    widening overlay, cyclic `$ref`) are NOT covered by F1-F3's own ad-hoc rejection tests, which check
    a different, narrower set (malformed YAML, unknown enum values, ambiguous edge forms); no
    execution/trace-comparison exists (F2 only compiles to `Workflow`, never runs one); no document
    digest/content-addressing scheme exists for declarative documents.
  - **006 §6b G6** — not merely unproven but currently UNPROVABLE as stated: `schedule_wakeup` still
    ships via the pre-existing `TimerWake`/reminder-service path, never through `StandingEffect`
    (Phase B's own note), and `watch_resource` has no real producer anywhere. The gate's own "a run
    that calls `schedule_wakeup` or `watch_resource`" premise does not connect to a `StandingEffect`
    census today regardless of what test is written.

  **Net**: of 28 named gates across the five RFCs, 3 are fully MET, 7 are PARTIALLY met with real,
  cited evidence, and 18 are BLOCKED on infrastructure this milestone's own earlier phases deliberately
  scoped out (a real listener chief among them — the single largest unblock, since it gates the
  majority of 011/012/013's own remaining gates simultaneously). The milestone's own stated exit
  criterion (this doc's own header quote) is NOT achievable without that infrastructure; this audit is
  the honest accounting the roadmap needs before deciding what real work closes the gap, rather than a
  claim that it already has.

- **Phase H** — the inbound-transport question, and the conformance gates it was thought to block.

  Opened after Phase G's audit named a real listener as "the single largest unblock." Project-owner
  direction (2026-08-15) removed the premise: **AgentEngine will not implement HTTP networking**;
  consumer code owns the socket and hands the engine parsed requests, MAF-style. That is a trust-
  boundary change, so it went to an ADR rather than straight to code —
  `decisions/ADR-061-host-provided-inbound-transport.md`.

  **Outcome (2026-08-15): four live security defects fixed, 33 red-team findings against the design,
  three design iterations, and 011 §10 G2 producing a real conformance number.** Ordered by what a
  reader needs first.

  **H1 — four shipped security defects, fixed and proven** (`0ec26b6`). ADR-061's first red-team round
  found these in M7 Phase C/D code, not in the design. All four were re-verified by hand before being
  acted on:
  - **A2A `GetTask`/`CancelTask` had no principal check**, and `Task.id` is `run_id`, which
    `AgentSession` mints as `session_id + ":run:" + counter` — structured and enumerable. `Task::history`
    carries both sides of the conversation. Knowing or guessing a session id was an unauthenticated
    cross-principal read of entire conversations (011 §8a's MUST; 018 §7 G4's release-blocking class).
  - **Every inbound A2A message bypassed 018 §2 admission**: `StartRun::caller` defaults to `nullopt`,
    `nullopt` *skips* the check, and `A2aServer` never set it.
  - **MCP's task store had the same missing check**, with ids from `mt19937_64` (not a CSPRNG).
  - **`EffectContext` was default-constructed** on both MCP dispatch paths, so every inbound call ran
    as an empty `Principal{}` and produced audit records with no identity (007 §8 requires it).

  Fixed with per-principal binding, ownership checked *before* task state (so a stranger cannot learn a
  task exists from a state-specific error), byte-identical not-found responses (012 §4), a new
  header-only `trust/secure_random.hpp` (BCrypt/`getrandom`, fails closed), and identity on
  `ToolInvocationAudit`. `tests/test_task_principal_binding.cpp` — 19 checks, every negative paired
  with a positive control. **Harness teeth verified** per ADR-015's precedent: neutering the ownership
  predicate turns 5 checks red while every positive control stays green.

  Two halves were deliberately NOT fixed because each needs a spec change: `Task.id`'s enumerability
  (012 §1/§5 assert `task_id` IS `run_id`, and `run_id` must stay deterministic per 001 §7/I5) and
  `IdempotencyKey`'s shape (019 §3 specifies the tuple). Both recorded in ADR-061 §7a.

  **H2 — the design did not survive contact** (`3ab0808`, `fe5047c`, `5b262c9`, `e329a7c`, `d7c174e`).
  Three iterations, two independent red-team rounds, **33 findings against iteration 2 alone**. Two
  are worth carrying forward:
  - Iteration 2's recommended design (F+G) was defeated: the exchange seam launders a host-chosen
    identity into `verified_by_engine`, and Design F's premise rested on a **fabricated citation** —
    020 §3a specifies no identity contract, and `make_embedded_principal` appears in no spec file.
  - `AuthorityRef` reinvented ADR-005 Design B with a guessable handle, and `live()` was checked at
    event boundaries while effects run unbounded on detached threads.

  One measured result survives and is reusable: a 16-byte authority handle puts
  `Ask<StartRun, AgentResponse>` at **104 bytes against `kMaxPayload`'s 192**, with the 208-byte
  control reproducing `agent_session.hpp:92-97`'s own cited figure.

  **H3 — Tier 1: 011 §10 G2 is not listener-blocked, and now produces a number** (`e71ec15`,
  `96583b1`, `f80a268`). The harness's two roles are asymmetric: `conformance server` needs a `--url`,
  but **`conformance client --command` spawns our binary**. So G2 needs no listener, no host adapter,
  no fixture, and no attribution apparatus. **The Phase G audit was wrong to list G2 as
  listener-blocked; only G1 is.**

  `tools/mcp_conformance_client.cpp` wires `McpClient`'s existing `RequestSender` seam to ADR-011's
  `perform_http_exchange` via ADR-016's host-initiated resolver. Running the real suite found four
  more genuine defects, each fixed: **`_meta` was missing entirely** (011 §2 specifies it; Phase C1
  deferred it and C3 never added it, so *no* outbound request was schema-valid for this revision),
  `clientCapabilities.extensions` shape, and **SEP-2243 `x-mcp-header` entirely unimplemented** —
  which 011 §8b itself calls "a mandatory client-side surface... easy to miss".

  **Conformance, client role, spec `2026-07-28`: 42/48 (88%), six scenarios clean** at the time this
  was first written. Full detail, including the tool-version and RFC-mismatch findings, in
  `docs/research/2026-08-15-mcp-conformance-harness.md`. **Superseded (2026-08-19) by ADR-061 §11's
  formal Tier 1 prove phase**, run against the full official suite rather than a partial pass:
  **non-auth 75/75 (100%)**; **auth 3/49 (~6%)** — this driver has no OAuth machinery at all, so **011
  §10 G2 is NOT met** despite non-auth being complete (G2 names `auth` as one of its four required
  suites). See the ADR's own §11 for the six claims' individual evidence, including two real teeth
  experiments (`derive_param_headers` broken -> passing count drops by 30, reverted) and the previously
  forward-referenced-but-nonexistent `tests/test_mcp_conformance_transport.cpp` written for real.

  **Open, and load-bearing for whoever picks this up:**
  - **All 33 findings remain open for Tier 3** (host-fronted HTTP). Research confirmed Tier 3 is the
    *only* path to G1 — the suite is URL-only for both roles, so stdio yields no gate.
  - ~~**`perform_http_exchange` cannot talk to a chunking MCP server.**~~ **Closed (2026-08-19).**
    `perform_http_exchange`/`perform_https_exchange` now dechunk a `Transfer-Encoding: chunked`
    response for real (`net_egress_proxy.cpp`'s `dechunk_response_body_if_needed`, ADR-011's own
    addendum). Byte cap enforcement during the read loop (claim C8) is unchanged; dechunking runs only
    after the full buffer is already in hand. `tools/mcp_conformance_client.cpp`'s own local decode
    workaround, retired the same day during ADR-061 §11's prove phase (running the real suite exposed
    it as now redundant AND actively harmful — double-decoding an already-plain body).
  - **011 §10's own gate is unrunnable as written**: `latest` (0.1.16) does not know `2026-07-28`;
    only `0.2.0-alpha.11` does. The RFC requires a percentage "pinned to a conformance release" and
    the only qualifying version is a prerelease.
  - **011 needs corrections**: §7 omits the required `MCP-Protocol-Version` header; §10's scenario
    list names `mrtr-client` (does not exist) and `json-schema-ref-deref` (really
    `json-schema-ref-**no**-deref` — a semantic inversion that would lead an implementer to build the
    opposite behaviour).
  - `sep-2322-client-request-state` (2/5) needs MRTR — the client retrying with `inputResponses`.

Each phase follows the established M6 discipline: implement → build (PowerShell + `vcvarsall`) →
test → full-suite regression → update this doc's own phase "Outcome:" → one commit, narrative body,
no co-author trailer.
