# Milestone 5 — Real providers, identity and secrets — work breakdown and kick-off

**Status:** Work breakdown (stage 4 of [the review-signoff workflow](v1-review-signoff-workflow.md)),
written just-in-time as this milestone starts, per that doc's §4. Scoped to
[the roadmap's](v1-implementation-roadmap.md) Milestone 5 exit criterion: *"the same agent runs
unchanged across ≥ 3 backends with only capability-table-predicted differences (004 §7 G1), and
secrets never appear in any persisted artifact under a canary scan (018 §7 G2)."*

**RFCs:** 004 (ChatClient Plane), 018 (Identity, Authorization and Secrets). Both Reviewed
(2026-08-05). 004 depends on 018 for its outbound-credential rule (§1); 018 itself depends on 007
(real since M1/M2), 011 (MCP Conformance), and 012 (A2A Conformance) — the latter two are Reviewed
but **not built, scheduled Milestone 7** (`v1-implementation-roadmap.md:148-159`). This milestone
therefore builds the slice of 018 that 004 actually needs and that doesn't require 011/012, and
defers the rest explicitly (decision 1).

Build order: the secret seam (018 §4) first — 004 §1 makes outbound credentials a hard rule for
every `ChatClient` backend, not an optional refinement, so nothing in 004 can be built to spec
without it existing first. Then the `ChatClient` concept itself made real (004 §1-2), then the two
committed backends (004 §3), then reliability/cost (004 §4-5), then recording/replay (004 §6), then
the identity/admission slice of 018 that stands alone from 011/012, then the milestone's own
exit-criterion proof.

## Current state (verified 2026-08-07, after M4)

| Item | State |
|---|---|
| `ChatClient` concept (`include/agentengine/core/chat_client.hpp:60-65`) | **Vocabulary only, stale M0 scaffolding.** Requires `capabilities()`, `chat(ChatRequest, EffectContext&) -> result<ChatResponse>` (synchronous, not `ae::task<T>`), and an unconstrained callable `chat_stream`. `ChatRequest` (`chat_client.hpp:35-39`) carries only `messages` — no tool declarations, no structured-output schema (both real since M2's 006, just never wired back in here). `ChatClientCapabilities` (`chat_client.hpp:16-33`) is missing `stop_sequences`, `seed`, `token_counting`, `batch` from 004 §2's full bitset |
| Mock `ChatClient` instances (M1-M4 precedent) | Real but scattered: `DummyChatClient` (`tests/smoke_vocabulary.cpp:31-39`), `EchoChatClient` (`tests/test_agent_session_isolation.cpp:44`), `RecordedChatClient` (`tests/support/recorded_chat_client.hpp`, a JSON-fixture player explicitly documented as "a test-scoped stand-in for 004 §6, not an implementation of it"). No canonical mock, none reconciled with a real backend shape yet |
| `ChatClientId<"vendor:model">` (`core/agent.hpp:28`) | Compile-time name tag only, no binding to an endpoint, credential, or backend instance |
| `check_chat_client_credentials` (`core/agent_registry.hpp:268-269`) | **Explicit permanent stub, always passes.** Top-of-file comment (lines 7-20) names the exact gap: "needs a real ChatClient registry, 004 — Milestone 2 builds no such registry; there is no `Engine` type yet." Confirmed: no `Engine` type exists anywhere in `include/`/`src/` |
| `check_output_schema_enforceable` (`agent_registry.hpp:321-322`) | Stub, always passes — needs a bound `ChatClient` instance to query `structured_output_native` against |
| `include/agentengine/protocol/openai/` | Empty except a 4-line README pointing at 004 §3. No headers, nothing compiled. No comparable directory exists for Anthropic at all |
| Secret seam (`SecretRef`/`SecretStore`/`SecretLease`, 018 §4) | **Built, Phase A (commit `2ecc85e`).** Correction to this table's own first-pass finding: "greenfield" was true only for `include/agentengine`/`src` — it missed that Quark itself already ships a real, Accepted, tested `SecretSource`/`Secret` mechanism (`third_party/quark/include/quark/core/secret.hpp`, 020-Security §4: zeroizing buffer, non-copyable, no `std::string` conversion, `Env`/`File` adapters; `security_secret_source_test.cpp`/`security_secret_zeroize_test.cpp`). `include/agentengine/trust/secret.hpp` wraps it rather than reimplementing it (the "no second storage engine" discipline 005/025 already apply to persistence, applied here to secrets), adding only the `cap::Secret` capability gate 018 §4 requires and Quark has no opinion about. `tests/test_secret_store.cpp` (15 checks) proves the gate, per-name scoping, rotation-without-restart, and real env/file resolution |
| `CapabilityToken`/`SecretKey` (`trust/capability_token.hpp`, ADR-005) | Real, Judged, red-teamed — but **a different concept wearing a confusable name.** This is the HMAC-chain root key for cross-process capability bearer tokens (018 §8 Q2, already resolved and closed). Not 018 §4's outbound-credential seam; do not conflate when scoping this milestone |
| `agentengine::Principal` (`trust/principal.hpp:9-12`) | Real, bare `{id, tenant_id}` struct, wired into `EffectContext`/`AgentSession`/`MemoryOrigin` (unchanged since M4). No registry, no auth mechanism producing a `Principal` from any inbound request — every test-constructed `Principal` today is hand-built C++ |
| `EffectContext` (`core/effect_context.hpp:16-39`) | Real, carries `principal`, `capabilities`, `bound_capabilities`, `deadline`, `trace_id`/`span_id`, `run_id`/`turn_index` (since M4). This is exactly the parameter 018 §4's `SecretStore::resolve(SecretRef, EffectContext&)` should hang off of — nothing does yet |
| Admission check at the `AgentSession` actor boundary (018 §2) | Does not exist. Consistent with M4's own finding that `session_id_` wasn't even actor-keyed until Phase A1 — no principal-ownership check runs before a turn starts |
| ADR-013 (HTTPS egress, mbedTLS) | **Accepted.** `TlsClientSession` (`include/agentengine/sandbox/tls_client.hpp`) wraps mbedTLS 3.6.7, real hostname verification, real HTTPS exchange — but **scoped to one consumer**: ADR-013 §9 states "only the WASM `http-request` import consumes this," gated through a guest-held `cap::NetOut` grant, called only from `wasm_backend.cpp`/`net_egress_proxy.cpp`. **No general-purpose host-side HTTP/SSE client exists** for a native `ChatClient` backend to call `api.openai.com`/`api.anthropic.com` directly — reusing `TlsClientSession`'s TLS primitive is plausible, but the call path (capability-grant/single-verified-endpoint model built for sandboxed guest egress) is new wiring for a host-initiated call with SSE streaming, retries, and vendor JSON bodies |
| Quark `task<T>` (submodule bumped `9ecbf1c` → `dcb191f`, commit `634a2cc`) | **Real for non-void `T` (ADR-047).** A nested, awaitable coroutine a `task<>` handler (or another `task<T>`) `co_await`s to get a value back — NOT a synchronous "drive to completion" API (no such thing exists; see Phase B4a's own note on `tests/support/run_task_sync.hpp`). `include/agentengine/core/task.hpp` adds the `agentengine::task<T>` alias (`ae::task<T>`, matching `027-Vocabulary-and-Naming.md:112,126`); 004 §1's literal `ChatClient::chat()` signature is real, Phase B4a |
| Quark streaming (RFC 024, ADR-018) | **Accepted (x86-64), real code**: `include/quark/core/stream_channel.hpp` (inbound credit-ring), `include/quark/core/reply_stream.hpp` (`ReplyStream`/`AskStream`, outbound, Accepted 2026-08-04). **Wrapped, Phase B4b**: `include/agentengine/core/stream.hpp` adapts the raw ring (bypassing actor addressing; boxing non-trivially-copyable `T`) into `ae::stream<T>`/`ae::stream_producer<T>`/`ae::make_stream<T>`, `ChatClient::chat_stream()`'s real 004 §1 return type |
| Quark 022 (governance: `TokenBucket`, `CircuitBreaker`, `FairShare`) | **Accepted, real, tested** (`include/quark/core/governance.hpp:47,104,179`; `tests/governance_circuit_breaker_test.cpp`, `governance_rate_limit_test.cpp`). Scoped today to Quark's actor-mailbox admission boundary (`GovernanceKey`-keyed) — wiring it to an HTTP provider call path is greenfield integration, not reuse of an existing call site |
| 011 (MCP Conformance) / 012 (A2A Conformance) | Both Reviewed, not built. `include/agentengine/protocol/{mcp,a2a}/` are README-only. Roadmap schedules both for **Milestone 7** |
| 016 (Observability) / 020 (Configuration and Hosting) | Both scheduled **Milestone 8 / Milestone 9** respectively — not built. 004 §5 needs 020 for per-tenant/session/principal budget ceilings and 016 for the cost-metric surface; 004 §6 needs 016 for recording-privacy content-capture modes; 018 §6 needs 020 for per-tenant Quark-022 resource limits |
| Test infrastructure to build on | `tests/support/recorded_chat_client.hpp` + `tests/fixtures/chat_client/*.json` (3 hand-authored fixtures: `simple_reply.json`, `tool_call.json`, `reasoning_and_text.json`) — the closest thing to reusable scaffolding, though its own README says explicitly it is not 004 §6's real feature |

## Design decisions made while breaking this down

1. **018 is scoped down to what doesn't need 011/012 (both M7).** §1's table has five inbound-identity
   rows; only two are buildable now without a dependency this milestone doesn't own: "Embedded /
   in-process" (host-supplied principal — already the project's own established pattern, every test
   hand-constructs one) and "Local CLI" (local user identity). "HTTP/AG-UI/OpenAI-compatible" needs
   013 (UI and Streaming Surfaces, M7) to have a surface at all; "A2A" needs 012's Agent Card scheme;
   "MCP (as server)" needs 011's authorization spec. Named explicitly, not silently dropped — same
   discipline M4 decision 4 used for 019 §2's wake-condition table.
2. **`ae::task<T>` is a genuine Quark-side prerequisite, not an AgentEngine plumbing task.** 004 §1's
   `chat()` signature is literally `ae::task<result<ChatResponse>>`; Quark's `task<T>` only implements
   `T = void` today (`third_party/quark/include/quark/core/task.hpp:17-22`, its own comment naming
   this "006/ADR-007"). `D:\GitSrc\QuarkCpp` (this session's working directories include it) is the
   actual upstream — same remote as the pinned submodule, HEAD `65c1522` already ahead of the pinned
   `9ecbf1c` — so extending it is a real, reachable option, not a wait-on-a-third-party blocker. Per
   CLAUDE.md's locked decision ("Quark is a submodule, never forked or patched in-tree — runtime
   changes go upstream"), a value-returning `task<T>` is built in `QuarkCpp` proper, reviewed there
   under Quark's own process, then the submodule pin advances. This is out-of-repo work and gates
   Phase B; **Phase A (secret seam) has no such dependency and can start immediately.** Until `task<T>`
   lands, `ChatClient::chat()` stays synchronous `result<ChatResponse>` (today's actual shape) rather
   than blocking the whole milestone on an upstream change — the async signature is adopted the moment
   it's available, not redesigned around its absence.
3. **`ae::stream<T>` is an AgentEngine-side wrapper over Quark's already-Accepted `ReplyStream`/
   `StreamChannel` primitives (RFC 024/ADR-018), not a new Quark-side ask.** Unlike `task<T>`, the
   underlying mechanism is real and tested; only the adapter shaping it as "one HTTP SSE chunk per
   credit-controlled item" is missing. Built in Phase B alongside the rest of the concept.
4. **The general-purpose host-side HTTP/SSE client is new wiring, built fresh against `TlsClientSession`'s
   TLS primitive — not a reuse of `HostEgressProxy`'s call path.** ADR-013 §9's own scope note says
   only the WASM `http-request` import consumes it, through a single-verified-endpoint capability-grant
   model built for sandboxed guest egress. A native `ChatClient` backend is a host-initiated call with
   SSE streaming, retries, and vendor JSON bodies — different shape, same TLS building block. This
   mirrors M3's own "extraction spike code stays as evidence, the real build is fresh against the
   current RFC" pattern (`native_jail`/`python_lockdown`), applied to `tls_client.hpp` instead.
5. **Two backends are committed for the milestone's own exit criterion (004 §7 G1 needs ≥ 3): OpenAI-
   compatible (§3's default) and Anthropic (§3's first-class).** The third is satisfied by
   `RecordedChatClient` promoted to a real conformer of the finished concept (Phase B), not a third
   live vendor integration — G1's own text is "the same agent, unchanged, runs on ≥ 3 backends," and a
   fixture-replay backend exercising the identical `ChatClient` interface satisfies that without a live
   network dependency in CI, consistent with every prior milestone's mock-first precedent and
   CLAUDE.md's machine-safety posture (no reason to make CI depend on a paid vendor call succeeding).
   "Local/embedded via OpenAI-compatible server" (§3's third table row) is real reach for later, not
   required to hit the gate.
6. **004 §4's provider-level breaker key (`{tenant, provider, model, SecretRef}`) and §5's budget-
   ceiling/cost-metric keys are flagged ADR-track by the RFC's own text** ("Invariant-touching...
   doesn't clear on a textual fix alone... owes the full design→red-team→prove→judge cycle," §5).
   Carried forward as a recommendation for the project owner, not yet actioned — same posture M4 gave
   its own Phase F3 exactly-once handling.
7. **004 §5's per-tenant/session/principal budget ceilings (needs 020, M9) and cost-metric emission
   (needs 016, M8) are scoped down to what's buildable now: per-run `TokenBudget<N>` enforcement at
   the turn boundary.** The ceiling/metric plumbing is named explicitly deferred, not silently assumed
   in scope, same discipline as decision 1.
8. **004 §6 recording/replay is built as the real mechanism (promoting `RecordedChatClient` from
   "test-scoped stand-in" to an actual conformer, per decision 5) but 016's content-capture privacy
   controls (metadata-only / hashed-content modes, §6's own text) are scoped down to "off by default"
   only** — the full mode set needs 016 (M8), named deferred rather than half-built against a
   not-yet-real observability surface.
9. **018 §6 multi-tenancy is scoped to what 007/004/005 already carry: `tenant_id` on `Principal`,
   threaded into the breaker/budget keys decision 6 flags, and cross-tenant denial tested against the
   surfaces that exist today** (session, memory — both real since M4; sandbox workspace — real since
   M2/M3). The audit-query surface named in 018 §7 G4 doesn't exist yet (no audit log/query API in the
   tree) — G4 is proven narrower than its full text, named explicitly, not silently assumed complete.
10. **018 §7 G5 (delegation attenuation) is proven at the scope 007 already covers — a derived
    `Principal`/capability set never gains authority its parent lacked, which is 007's existing
    attenuation-only rule (real, Judged, enforced since M2) — not full A2A/MCP delegation chains**,
    which need 012/011 (M7). Same narrowing discipline as decisions 1/9.

## Tasks, in dependency order

### Phase A — The secret seam (018 §4), no Quark dependency, starts immediately — **DONE (commit `2ecc85e`)**

- **A1. Done.** `SecretRef` (a name, not a value) and `SecretLease` (non-copyable, redacted by
  construction — `to_redacted_string()` always `"***"`; the type itself is unprintable, not a
  documentation promise), wrapping Quark's already-zeroizing `quark::Secret` rather than owning a
  second buffer (corrected finding, current-state table above).
- **A2. Done.** `SecretStore` concept: `resolve(SecretRef, EffectContext&) -> result<SecretLease>`.
  Kept synchronous (`result<T>`, not `ae::task<T>`), and **stays that way even now that Phase B4a made
  `task<T>` real** — corrected from this task's own original "upgraded in lockstep with Phase B" plan,
  written before the real call site (`trust/secret.hpp`'s own `resolve_via`) existed:
  `quark::SecretSource::get()` (env/file) is itself a synchronous call with nothing to suspend on, and
  a sync function is freely callable from inside an async `ChatClient::chat()` coroutine body with no
  `co_await` needed — coroutine-ifying it would add frame overhead for zero real benefit.
- **A3. Done.** Backends: `AgentEngineSecretStore` adapts any `quark::SecretSource` — wired against
  `quark::EnvSecretSource` (`QUARK_SECRET_<name>`) and `quark::FileSecretSource`
  (`<root>/<name>`, both real in Quark already). OS keychain/DPAPI/Keychain and external managers
  (Vault, cloud KMS) stay later seam backends (deferred list) — Quark's own §4 text names them
  DEFERRED too, so this isn't a narrower cut than upstream's own scope.
- **A4. Done.** Scoping per name proven (`test_secret_store.cpp`: holding `Secret<"a">` denies
  resolving `"b"`) and rotation-without-restart proven (the same `InMemorySecretStore` instance
  returns a changed value on the very next `resolve()` after its backing value changes, with no
  restart and no cached lease in between). `{provider|server|peer, principal-or-service}`-shaped
  scoping (018 §3's fuller text) is deferred to Phase C/D, where it has a real caller (a `ChatClient`
  backend constructed with a specific provider's `SecretRef`) to prove against.

### Phase B — `ChatClient` made real (004 §1-2) — **DONE (B1/B2/B3/B5/B6, commit `28c88cc`; B4a, commit `61ed421`; B4b below)**

- **B1. Done.** `ChatRequest` gains real tool declarations — `ToolDescriptor` (`core/tool_pipeline.hpp`),
  the exact type `ContextContribution.tools` already reuses, not a second "`ToolDecl`" (that name was
  never actually minted anywhere in the tree, corrected in the doing) — and an optional
  `output_schema_json` (003 §4's `OutputSchema<T>`, compiled via `schema::json_schema_of<T>`).
- **B2. Done.** `ChatClientCapabilities` completed: `stop_sequences`, `seed`, `token_counting`, `batch`
  added, reaching 004 §2's full 18-field bitset.
- **B3. Done.** Outbound-credential wiring, proven behaviorally (`test_chat_client_credential_resolution.cpp`):
  a reference `ChatClient` conformer holds only a `SecretRef` as a member (never a `SecretLease`), and
  two `chat()` calls straddling a rotation of the backing secret return different resolved values —
  only possible if resolution happens fresh inside `chat()`, never cached from construction.
- **B4a. Done.** `chat()`'s literal 004 §1 signature, `ae::task<result<ChatResponse>>` — real the
  moment Quark's `task<T>` for non-void `T` landed (ADR-047, submodule bumped `9ecbf1c` → `dcb191f`,
  commit `634a2cc`), exactly as decision 2 said it would be, no redesign needed. `AgentSession::handle`
  became Quark's async `task<>` handler form (ADR-007) to `co_await` it — the first `Ask<Q,R>` handler
  in either tree to combine async dispatch with `m.respond(...)`; both `m`'s descriptor and
  `effect_context_` (an `AgentSession` data member) stay valid across every suspend point by
  construction, confirmed against `activation.hpp`'s `complete_parked()`/`finish_frame`, not asserted.
  **Blast radius turned out wider than this task's own name suggested**, forced rather than chosen:
  `HistoryProvider<Summarize<N,SummarizerT>>::on_context` and `MemoryProvider::on_turn_end` (both real
  Milestone-4 conformers) call a declared `SummarizerT`/`ChatClient::chat()` internally, so the
  `ContextProvider` concept itself (`core/context_provider.hpp`) had to become task-returning too —
  `on_context -> task<result<ContextContribution>>`, `on_turn_end -> task<std::monostate>` (never bare
  `task<>`: `quark::task<void>` is deliberately NOT awaitable — reserved as the one exact type
  ADR-007's dispatch selects async handler mode by — so a helper meant to be `co_await`ed needs
  `task<T>` for real `T`; `std::monostate` reuses this codebase's existing "no value" convention from
  `trust/secret.hpp`'s `require_secret_capability`). That cascaded into every conformer
  (`HistoryProvider<Window<N>>`, `MemoryProvider::on_context`, `DummyContextProvider`,
  `FixedMessagesProvider`) becoming a (mostly trivial, non-suspending) coroutine, and into
  `core/context_assembly.hpp`'s type-erased `ContextProviderDescriptor`/`assemble_context()` (Phase
  B3's own standalone, not-yet-`AgentSession`-wired seam) doing the same. `SecretStore::resolve()` and
  `sandbox/provider_http_client.hpp` were evaluated and deliberately left synchronous — the underlying
  primitive in both (`quark::SecretSource::get`, the blocking-socket HTTPS exchange) has no I/O to
  suspend on today, and a sync function is freely callable from inside an async coroutine body with no
  `co_await` needed, so coroutine-ifying either would add frame overhead for zero behavioral benefit;
  named here rather than mechanically "upgraded in lockstep" as Phase A2's own forward-looking text
  guessed before the real call sites existed.
- **New seam: `quark::task<T>` (T≠void) is unusable outside a coroutine context** — no synchronous
  "drive to completion, give me the value" API exists (`quark/core/task.hpp`'s own design), so any test
  that called a `ChatClient`/`ContextProvider` conformer directly from a plain, non-actor `main()`
  needed a driver. `tests/support/run_task_sync.hpp` is that driver: a minimal lazy coroutine, resumed
  once, valid ONLY for an awaited task whose body never genuinely parks (every test conformer in this
  tree qualifies — none does real cross-actor I/O). Every pre-existing `AgentSession`/`TestKit`-driven
  test needed no such change: `TestKit::ask<R>()`'s own documented assumption ("the handler runs
  synchronously... resolved by the time drive() returns") continues to hold because nothing in the
  `co_await` chain a `TestKit`-driven turn now runs through ever genuinely suspends either — the same
  "no co_await ⇒ no real parking" property `task<void>` already relied on, extended transitively.
- **B4b. Done.** `ae::stream<T>` (`core/stream.hpp`) — an AgentEngine-side wrapper over Quark's already-
  Accepted `ReplyStream`/`StreamChannel` credit-controlled ring (decision 3), exactly as scoped: no new
  Quark-side ask, only the adapter. `ChatClient::chat_stream()`'s concept constraint is now the literal
  004 §1 signature, `-> std::same_as<stream<ChatResponseUpdate>>` (was: unconstrained beyond "callable").
  Two adaptations over the raw Quark primitive, both named in `stream.hpp`'s own file banner rather than
  silently done: (1) **boxing** — Quark's ring is inline-slot-only (`StreamChannel<F>` requires `F`
  trivially copyable; the by-reference `ZeroCopyRetained` regime is a declared 019/003 seam, "stubbed,
  never wired"), and `ChatResponseUpdate` carries a `std::string`-backed `ContentItem`, so it is NOT
  trivially copyable — `stream<T>`/`stream_producer<T>` box each item on the heap behind a raw owning
  pointer (itself trivially copyable) so arbitrary movable `T` can ride the ring, trading away ADR-018's
  own measured "0 per-item heap" property for non-trivial `T` only; (2) **no actor addressing** —
  `chat_stream()` is a plain synchronous call (unlike `chat()`, NOT `ae::task<...>`-wrapped), so
  `make_stream<T>` reuses Quark's proven `make_ask_stream`/`StreamResponder::accept`/`block_on_open`
  sequence but never crosses an actor boundary, which also means it never triggers the OPEN-resolve
  reentrancy hazard ADR-018's own residual risks name (that hazard is specific to draining a stream
  opened via a real cross-actor `ask_stream<F>`; research directly into `reply_stream.hpp`/
  `ask_stream_coawait_real_scheduler_test.cpp` confirmed this before writing the adapter). Proven end to
  end against a real, non-mock `ChatClient` conformer (`tests/test_chat_client_stream.cpp`,
  `StreamingWordChatClient`) whose `chat_stream()` streams for real across a background producer
  thread — the same shape a live HTTP/SSE backend (Phase D/E) will use: FIFO/lossless delivery under
  real cross-thread backpressure (ring capacity deliberately smaller than the item count, so the
  producer genuinely stalls on credit at least once), an empty stream still closes cleanly, and dropping
  the consumer mid-stream wakes a stalled producer and lets its thread join within bounded time (004 §7
  G2's cancellation text, proven at the `ae::stream<T>` layer — the socket-level proof is Phase D/E's).
  15/15 repeat runs clean (no flakiness from the real cross-thread timing involved). The ~20 pre-existing
  `ChatClient` conformers across `tests/` that previously stubbed `chat_stream` as `int ... { return 0; }`
  (accepted because the constraint was unconstrained) all needed updating to return a real, empty/unused
  `stream<ChatResponseUpdate>{}` — the same "concept tightened, every conformer follows" blast-radius
  shape Phase B4a hit, just narrower here (mechanical stub-signature updates, no coroutine cascade).
  **Incidental fix, needed to even build**: `tests/CMakeLists.txt`'s ADR-012 `try_compile` checks
  (`sandbox_profile_rejects_non_conforming_type`/`sandbox_profile_positive_control`) were missing
  `third_party/quark` (the PAL include root, "for pal/pal.hpp" per `third_party/quark/CMakeLists.txt`)
  from their manual `INCLUDE_DIRECTORIES` list — a pre-existing gap already fixed on three later checks
  in the same file but never on these two; surfaced only once a fresh configure actually re-ran them
  (their `try_compile` result is cached in `CMakeCache.txt` otherwise). Fixed alongside this task since
  it blocked configuring at all; unrelated to the stream adapter itself.
  `ae::stream<T>` is real for both directions 004 §1 and 013 need it for (`ChatClient`-level chunks and
  the higher-level run-event stream) — SSE chunk-boundary parsing to actually feed a live network
  producer is explicitly Phase D's job, not built here (`sandbox/provider_http_client.hpp`'s read loop
  still only knows Content-Length framing, confirmed unchanged).
- **B5. Done.** `select_output_schema_strategy` (`core/chat_client.hpp`): 004 §2's degradation rule /
  003 §4's three enforcement strategies, in preference order (native > tool_shaped > parse_and_repair).
  `register_agent<A>()`'s `check_output_schema_enforceable` calls it when a registry is supplied.
  **Correction to this task's own original wording:** `parse_and_repair` is a universal last resort
  under today's three-tier design (needs no special capability bit at all), so 004 §2's "if no
  fallback exists, `register_agent<A>()` fails at startup" clause is honestly unreachable via this
  function as specified — named in the code rather than faked with an artificial rejection case.
  Recording the chosen strategy in the run trace is deferred (needs 016, M8) — the selection
  *decision* itself is real and tested, the *trace-recording* half is not, named separately rather
  than conflated.
- **B6. Done.** `ChatClientRegistry` (`ChatClientId` → `ChatClientCapabilities`) — the smallest useful
  slice of the Engine-level registry `agent_registry.hpp`'s own comment named missing since M2.
  `register_agent<A>()` gets an ADDITIVE `ChatClientRegistry const*` parameter (default `nullptr`,
  corrected from the original wording's implied in-place rewrite) so every pre-M5 zero-arg call site
  (8 across `tests/`/`tools/`) keeps compiling with its old stubbed-check behavior unchanged; supplying
  a registry wires `check_chat_client_credentials`/`check_output_schema_enforceable` for real.
  off the real concept instead of their permanent-stub bodies — needs a minimal `ChatClient` registry
  (a map from `ChatClientId` to a bound instance), the smallest possible slice of an eventual `Engine`
  type, not a full `Engine` build-out (named out of scope, deferred list).

### Phase C — Host-side HTTP/SSE client (new wiring, decision 4) — **done (commit `85312be`)**

- **C1. Done, narrowed.** `sandbox/provider_http_client.hpp`'s `perform_provider_https_exchange`: a
  general-purpose HTTPS client for host-initiated calls, reusing `net_egress_proxy.hpp`'s
  `resolve_and_validate`/`perform_https_exchange` (which itself already wraps `TlsClientSession`)
  rather than a fresh implementation on `TlsClientSession` directly — smaller diff, same defense-in-
  depth address-blocking, same byte-cap-enforced read loop, no new parsing code to independently get
  wrong. **Still synchronous `result<T>`, by deliberate choice, not because it's blocked** — Phase
  B4a made `ChatClient::chat()` real `ae::task<T>`, but no Phase D/E backend exists yet to call this
  function from inside a coroutine, and the underlying blocking-socket exchange has no I/O to suspend
  on today; a future backend's async `chat()` can call this synchronous function directly and
  `co_return` its result, same as any other sync helper called from a coroutine body. **SSE
  chunk-boundary parsing is not yet built** — `perform_https_exchange`'s read loop currently only
  knows Content-Length-framed responses (a pre-existing cut named in its own header comment, unchanged
  by this phase); real SSE parsing is needed once `chat_stream()` is real (Phase B4b, still open) and
  is deferred there rather than half-built ahead of the type that would consume it. HTTP only
  real-provider-scoped: HTTPS only, no plain-HTTP path (no target this project speaks to needs one).
- **C2. Done.** `stop_token` threaded through `perform_http_exchange`/`perform_https_exchange`'s read
  loops (additive, default-unstoppable, `HostEgressProxy::fetch`'s ADR-011 call site unaffected).
  Proven bound: cancellation is checked once per read-loop iteration, not mid-syscall — against a
  server sending periodic small chunks (a realistic SSE-connection shape), a cancellation requested
  mid-stream aborts in ~440ms in `test_provider_http_client.cpp`; the theoretical worst case (a peer
  that goes fully silent) is bounded by one `kIoTimeoutMs` tick (10s), not instantaneous, named
  honestly in the code rather than oversold as sub-second in every case.
- **New testability seams, not scoped by the original task text:** an injectable resolver and a
  `ca_bundle_pem_override` on `perform_https_exchange`/`perform_provider_https_exchange`, mirroring
  `HostEgressProxy::resolver`'s already-established pattern — needed because a loopback test server's
  own address is itself in `resolve_and_validate`'s blocked-range table and its self-signed cert isn't
  in the real vendored trust store. Production code never passes non-default values for either.

### Phase D — OpenAI-compatible backend (004 §3, default) — **DONE**

`include/agentengine/protocol/openai/chat_client.hpp` — `OpenAIChatClient<Store>`, a real, product-code
`ChatClient` conformer, gated by `AGENTENGINE_WITH_HTTPS` (the same gate `provider_http_client.hpp`
itself is behind). Wire-format field names were sourced directly from the official OpenAI .NET SDK's
generated serialization code (`D:\GitSrc\openai-dotnet`'s `Utf8JsonWriter`/`WritePropertyName` calls
are ground truth for the real wire shape — the project owner pointed at this repo specifically so the
translation logic wouldn't be guessed from memory), not paraphrased from documentation.

- **D1. Done.** Request/response translation for Chat Completions (the Responses API shape is 004
  §3's OTHER named table entry — not built here, narrower than the full text, see the deferred list).
  `build_request_body`/`translate_message` build the real `{model, messages, tools, response_format,
  stream}` wire body; `parse_chat_completion_response` parses `choices[0].message.{content,
  tool_calls[]}` and `usage.{prompt_tokens, completion_tokens, prompt_tokens_details.cached_tokens,
  completion_tokens_details.reasoning_tokens}`. `ChatClientCapabilities` is a CONSTRUCTOR PARAMETER
  (§3's own rule: "discovered from config per endpoint, never assumed uniform") — nothing here probes
  a response to infer a capability. Outbound-credential resolution reuses Phase B3's exact pattern:
  `OpenAIChatClient` holds only a `SecretRef` member, resolved fresh inside `chat()`/`chat_stream()`
  against the caller's `SecretStore` — proven behaviorally in `test_openai_chat_client_live.cpp`
  (a rotated secret is picked up on the very next call, no client reconstruction).
- **D2. Done, honestly narrower than full incremental streaming — named, not silently claimed
  complete.** `perform_provider_https_exchange` (Phase C) has no incremental/chunked-transfer read
  loop yet — it blocks until the whole HTTP response is buffered. `chat_stream()` therefore performs
  one complete blocking HTTPS exchange on a DETACHED background thread (not a tracked member — a real
  `OpenAIChatClient` instance is shared across many concurrent streaming calls via
  `ChatClientRegistry`, so a single joined-then-replaced thread member would wrongly serialize them),
  decodes `Transfer-Encoding: chunked` framing if present (`decode_chunked_body`, a real OpenAI
  streaming response's actual wire shape — proven both as a pure parser against a literal chunked
  body and end-to-end against a real TLS socket in `test_openai_chat_client_live.cpp`), splits the
  decoded body into SSE `data: ...` events (the literal `data: [DONE]` terminal sentinel confirmed
  against the SDK's own `SseUpdateCollection`), and pushes ONE `ChatResponseUpdate` per event/
  assembled-tool-call through `core/stream.hpp`'s credit-controlled ring (Phase B4b) — so the
  vendor's own chunk BOUNDARIES are preserved faithfully in delivery order (004 §7 G3's gate: proven
  in `test_openai_chat_client_translation.cpp`'s D2-R4 case, two text deltas keep their own chunk
  boundaries; a tool call's argument fragments, which arrive incrementally keyed by a per-choice
  `index` per the SDK's `StreamingChatToolCallUpdate`, are accumulated across chunks and emitted as
  ONE complete update once assembled — unavoidable, a partial JSON-string fragment is not a valid
  `ToolCall::arguments_json` on its own). **What is honestly NOT built**: low-latency incremental
  delivery — the network fetch itself is not incremental (Phase C's own named gap), so the first
  `ChatResponseUpdate` is not available to the consumer until the ENTIRE vendor response has already
  arrived; a real incremental read loop on `provider_http_client.hpp` is future work, not scoped here.
  Streaming responses carry no per-chunk `Usage` today either — `ChatResponseUpdate` has no field for
  it (a pre-existing gap this phase didn't introduce or need to close).
- **D3. Done.** Tool-schema shaping: `translate_tool` builds `{"type":"function","function":{"name",
  "description","parameters"}}` (field names/nesting confirmed against the SDK's
  `InternalChatFunctionDefinition` serializer) from `ToolDescriptor.args_schema_json`, passed through
  as raw JSON (no client-side schema validation, matching the SDK's own `WriteRawValue` passthrough
  posture). **MCP-tool-vs-native-tool routing split named as explicitly deferred**, not silently
  dropped: 011 (MCP Conformance) isn't built until Milestone 7 (decision 1's own scoping), so there is
  only one tool shape to route today — every declared `ToolDescriptor` is a native tool; the split
  this checklist item names has nothing to route between yet.
- **D4. Done.** Structured-output shaping: `translate_output_schema` injects `"additionalProperties":
  false` into 003 §4's `OutputSchema<T>` JSON Schema text ONLY when the schema doesn't already declare
  the key (an explicit author choice, e.g. `additionalProperties:true`, is never silently overridden
  — `test_openai_chat_client_translation.cpp`'s D4-R2 case), and wraps the result as OpenAI's
  Structured Outputs `response_format` (`{"type":"json_schema","json_schema":{"name","schema",
  "strict":true}}`, field names/order confirmed against the SDK's
  `InternalResponseFormatJsonSchemaJsonSchema` serializer).

**Incidental finding, not a defect in this phase's own code**: `ChatResponseUpdate`'s `delta` field is
a single `ContentItem`, so a streamed tool call's `arguments_json` is only ever exposed as a COMPLETE
string once assembled — matches 003's existing content model exactly, named here only because it is
the reason D2's tool-call handling could not be "true per-token streaming" for tool calls even in
principle, independent of Phase C's own incremental-fetch gap.

### Phase E — Anthropic backend (004 §3, first-class) — **DONE**

`include/agentengine/protocol/anthropic/chat_client.hpp` — `AnthropicChatClient<Store>`, structurally
mirroring Phase D's `OpenAIChatClient` (same detail-namespace-of-pure-functions shape, same detached
background thread for `chat_stream()`, same injectable resolver/CA-bundle testability seam). Wire
format was sourced directly from the official Anthropic C# SDK's generated model code
(`D:\GitSrc\anthropic-sdk-csharp`, the project owner's own pointer, same rigor as Phase D's
`D:\GitSrc\openai-dotnet` research).

- **E1. Done.** Request/response translation for the Messages API (`POST /v1/messages`). Two real
  reshaping steps Anthropic's own model forces, not optional stylistic choices: (a) Anthropic has NO
  `role:"system"` in `messages[]` at all (the SDK's own doc comment says so explicitly) — any
  `role::system` messages in `ChatRequest.messages` are extracted and concatenated into the top-level
  `system` field, everything else translates normally; (b) `role::tool` has no Anthropic role either —
  a tool reply becomes a `role:"user"` message carrying a `tool_result` content block. Also confirmed
  and handled: `tool_use.input` is a REAL JSON OBJECT on the wire (not OpenAI's stringified
  `arguments`) — outbound translation parses `ToolCall::arguments_json` into a `json::Value` and emits
  it directly; inbound parsing `json::dump`s the received object back into `arguments_json`, this
  project's own uniform string representation, lossless either way. `max_tokens` is REQUIRED by
  Anthropic (unlike OpenAI's optional/deprecated field) but `ChatRequest` carries no sampling-parameter
  field at all — falls back to the bound instance's own `ChatClientCapabilities::max_output_tokens`
  when declared nonzero, else a fixed default (4096), a real named translation-layer decision, not an
  unstated guess. `usage.input_tokens`/`output_tokens`/`cache_read_input_tokens` map directly (distinct
  field names from OpenAI's `prompt_tokens`/`completion_tokens`, confirmed exact); `cache_creation_
  input_tokens` has no AE `Usage` field to land in and is named as an unmapped gap rather than stuffed
  into an unrelated field.
- **E2. Done, same "logic real, nowhere to surface it" honesty as Phase D's D2.** Anthropic's
  `message_delta.usage` fields are documented AND proven (against the SDK's own
  `MessageContentAggregator.GetResult` reduce) to be the RUNNING TOTAL on every event, not a delta —
  `accumulate_message_delta_usage` implements the exact reduce (`output_tokens` unconditionally
  overwritten every event; `input_tokens`/cache-token fields seeded once from `message_start` and only
  overwritten when a later event actually carries a non-null value), proven against a literal
  multi-event sequence in `test_anthropic_chat_client_translation.cpp`'s E2-R1 case. **What this has
  nowhere to surface**: exactly Phase D's own D2 finding — `ChatResponseUpdate{delta, is_final}` has no
  `Usage` field at all, so the conversion logic is real and tested but not wired into any caller-visible
  per-chunk value; that gap is `ChatResponseUpdate`'s own, pre-existing, not reopened or claimed closed
  here.
- **E3. Done, two parts, both scoped explicitly narrower than the full text — named, not silently
  claimed complete.**
  - *Reasoning parts*: response-parsing ONLY, never round-tripped back to the API. A `thinking` block
    requires BOTH `thinking` text AND an opaque `signature` (tamper-evidence) string; `redacted_
    thinking` carries only opaque `data`, no visible text. This project's content model
    (`Reasoning{text, encrypted}`) has no field to carry `signature`/`data` — inbound parsing maps
    `thinking` → `Reasoning{text, encrypted=false}` and `redacted_thinking` → `Reasoning{text="",
    encrypted=true}` for the caller to read, but a `Reasoning` content item is silently dropped from
    OUTBOUND message translation (`test_anthropic_chat_client_translation.cpp`'s E3-R3 case) — re-
    sending a thinking block without its real signature is either rejected by the API or defeats the
    tamper-evidence property it exists for, and there is nowhere in this project's content model that
    signature could have been kept in the first place. Full extended-thinking round-tripping across
    turns is real future work, not built here.
  - *Prompt caching*: `cache_control` breakpoints at the two segment boundaries actually reachable from
    `ChatRequest` as received — the extracted `system` text and the last tool definition — gated on
    `ChatClientCapabilities::prompt_caching` being declared true for the bound instance. 004 §8 Q2's
    resolution names "005 §3's existing context-assembly segment boundaries" as the insertion point,
    but `ChatRequest.messages` is already a FLATTENED list by the time it reaches ANY `ChatClient`
    (`core/context_assembly.hpp`'s `assemble_context` merges every contributor's messages into one
    vector with no per-contributor boundary markers surviving) — finer-grained per-contributor
    breakpoints are not reachable from this seam without threading boundary markers through
    `ContextAssemblyResult`/`ChatRequest` first, a real, separate, not-yet-built architectural change,
    not a gap in this phase's own translation logic. system+tools is Anthropic's own documented
    best-practice breakpoint placement (the stable prefix), so this is a real, meaningful caching win
    at the one boundary genuinely available today, not a token gesture — proven end to end (a request
    with `prompt_caching` declared and a system message actually round-trips over a real TLS
    connection without the server rejecting the shape, `test_anthropic_chat_client_live.cpp`).

Proven in `tests/test_anthropic_chat_client_translation.cpp` (pure translation/parsing logic, offline,
literal wire-format JSON — including the message-reshaping, real-JSON-object tool input, thinking/
redacted_thinking parsing, cumulative-usage reduce, and named-event SSE splitting) and
`tests/test_anthropic_chat_client_live.cpp` (end-to-end against a real local TLS server, both `chat()`
and `chat_stream()`, including secret rotation, capability denial, and the system+caching request
shape).

**Structured output**: uses Anthropic's native `output_config.format` (`{"type":"json_schema",
"schema":...}`), confirmed as the SDK's own primary/newer mechanism — distinct wire shape from OpenAI's
`response_format`, and (unlike Phase D's OpenAI backend) does NOT force `additionalProperties:false`
into the schema, since nothing in the research confirms Anthropic requires or even recognizes that key
— this backend does not assert an unconfirmed requirement onto the schema.

### Phase F — Reliability and cost (004 §4-5, scoped per decisions 6/7) — **DONE (commit `24fdfe3`)**

- **F1. Done.** Retry: `Transient` failures only (001 §6), bounded exponential with jitter, respecting
  the remaining deadline (never the retry's own timeout) — a stable idempotency key on the retried
  call. `ResilientChatClient<Inner>` (`core/resilient_chat_client.hpp`), built together with F2 as one
  wrapper (see the design-decisions block below for why). Idempotency key computed once, reused
  verbatim across retries; not yet wired into either backend's HTTP layer (verified against the
  locally vendored SDKs that neither documents a client idempotency header today).
- **F2. Done.** Rate limiting/circuit breaking: wired Quark 022's real `CircuitBreaker`
  (`governance.hpp:110`) into `ResilientChatClient<Inner>`'s call path. Breaker key stays
  `{provider, model, SecretRef}` for now, satisfied by construction (one wrapper instance already
  wraps exactly one such triple, no runtime map needed) — adding `tenant` is decision 6's flagged
  ADR-track item, not actioned in this phase. `TokenBucket` itself not separately wired (the breaker
  is the mechanism actually exercised; a standalone rate limiter beyond breaker admission was not
  needed to satisfy F2's own text and would have been scope this phase doesn't own).
- **F3. Done.** Failover as explicit policy only — `FailoverChatClient<Primary, Fallback...>`
  (`core/failover_chat_client.hpp`), tries backends in declared order on any error.
  `ChatResponse::fallback_tier` (new field) names which tier answered — the "response metadata" half
  of the gate, real and tested. Full run-trace recording deferred (no trace sink exists yet, needs
  016/M8 — the same scoping Phase B5 already applied to its own trace note). `chat_stream()` failover
  scoped to primary-only (named and reasoned in the header: no synchronous success/failure signal
  exists to gate a fallback decision on before the stream handle must be returned).
- **F4. Done.** Per-run `TokenBudget<N>` enforced at the turn boundary → `Resource` failure (decision
  7's buildable slice of §5). Lives in `AgentSession` itself (not a `ChatClient` wrapper — the
  accumulation is inherently per-run, and a `ChatClient` instance is shared/stateless across runs);
  `input_tokens + output_tokens` accumulate into a per-run counter reset at each `StartRun`, checked
  after the response returns, failing closed (never responds) on the same shape the two pre-existing
  fail branches in that handler already use. No pre-call check: this milestone's turn loop still makes
  exactly one model call per run, so a pre-call check against an always-zero prior total is currently
  dead code, named rather than built early.

**Design decisions (2026-08-07, before implementation):**

- `ChatClient` is a concept, never a base class (004 §1, chat_client.hpp's own top comment: "never
  inherited from on the hot path") — so F1/F2/F3 are template wrappers that themselves conform to
  `ChatClient`, composing over an `Inner` template parameter, exactly the shape every existing
  conformer already uses (`OpenAIChatClient<Store>`/`AnthropicChatClient<Store>`), never a
  virtual/type-erased decorator.
- **F1+F2 combined into one wrapper**, `ResilientChatClient<Inner>` (new file,
  `core/resilient_chat_client.hpp`): retry and circuit-breaking are one call-path concern (the breaker
  must see the outcome of every attempt the retry loop makes; a breaker check gates each attempt before
  it retries) — two separate wrapper layers would either duplicate the attempt loop or need to leak
  breaker state between layers. `RetryPolicy`/`BreakerConfig` are plain runtime constructor fields, not
  compile-time template params: retry timing and breaker thresholds are operational knobs in the same
  spirit as 004 §5's "pricing tables are configuration, not code," and every existing backend already
  takes its own operational config (host/port/model) as runtime constructor args, not template
  parameters. Breaker key is `{provider, model, SecretRef}` per decision 6 — but since one
  `OpenAIChatClient`/`AnthropicChatClient` instance is already constructed bound to exactly one
  provider+model+SecretRef triple, wrapping it needs no runtime key/map at all: the wrapper's own
  single `quark::CircuitBreaker` member IS that key, by construction. Quark's `governance.hpp` is
  clock-free (`now_ns: std::int64_t`, no hidden syscall) — the wrapper calls `quark::monotonic_now_ns()`
  at each check site; no `agentengine`-side clock seam existed to reuse (checked: `EffectContext::deadline`
  is a `std::chrono::steady_clock::time_point` sentinel, converted at the boundary, not threaded through
  as `now_ns`).
- **Idempotency key**: `ChatRequest::idempotency_key` (new optional field, appended last) is set once
  by `ResilientChatClient` from `IdempotencyKey{ctx.run_id, ctx.turn_index, 0, 0}.to_string()`
  (tool_pipeline.hpp's existing type, reused rather than inventing a second shape) before the first
  attempt, and never regenerated across retries — that reuse-not-regeneration is what "stable" means
  here. **Not wired into either backend's HTTP layer**: verified directly against the locally vendored
  SDK source (openai-dotnet, anthropic-sdk-csharp) that neither documents a client-supplied idempotency
  header for Chat Completions/Messages — the field exists so a backend can adopt a verified mechanism
  later without another field-ordering migration, not a claim that one exists today.
- **F3 as a separate wrapper**, `FailoverChatClient<Primary, Fallback...>` (new file,
  `core/failover_chat_client.hpp`): tries `Primary`, then each `Fallback` in order, on any error result
  (matching "explicit policy... never silent" — every attempt is a deliberate configured step, not a
  retry-shaped implicit fallback). `ChatResponse::fallback_tier` (new field, appended last: 0 = primary
  answered, N>0 = the Nth fallback did) is the "response metadata" half of 004 §4's gate. Full run-trace
  recording is explicitly out of scope — no trace sink exists yet (needs 016, M8), the identical scoping
  Phase B5 already applied to its own output-schema-strategy decision.
- **F4 lives in `AgentSession`, not a `ChatClient` wrapper**: "per-run" budget enforcement needs to
  accumulate `Usage` across every turn of ONE run, but a `ChatClient` instance is shared/reused across
  many sessions and runs (Phase B3: credentials resolve fresh per `chat()` call, nothing is cached
  per-run) — there is no `run_id`-keyed state a stateless wrapper could hold without a map. `AgentSession`
  already carries exactly one run at a time (I1: one session, one executor) and already receives
  `ChatResponse::usage` back from its own `chat_client_.chat()` call in `handle(Ask<StartRun,...>)`
  (agent_session.hpp:248), so accumulation is a same-actor member (`run_tokens_consumed_`, reset at
  each new `StartRun`), no new concurrency concern. `AgentMetadata::token_budget`
  (agent_registry.hpp:60, already compiled by `register_agent<A>()` from the agent's declared
  `TokenBudget<N>` policy, but never consumed anywhere until now) is threaded in via an ADDITIVE
  `initialize()` parameter (default `std::nullopt`, matching B6's own "old call sites keep compiling"
  precedent) rather than a constructor change. **Enforcement point: after the response returns**, not
  before the call — a pre-call short-circuit is dead code under this milestone's own still-single-turn-
  per-run scope (turn_index is always 0, no tool-call loop exists yet, named at agent_session.hpp:220-227),
  so a total that could exceed budget before any call is made cannot yet occur; adding that check now
  would be validating a scenario that can't happen (CLAUDE.md). On exceeding, `AgentSession` fails
  closed — never responds (`co_return` without `m.respond(...)`) — the exact same shape the context- and
  chat-failure branches immediately above it already use; `AgentResponse` has no error slot to carry a
  typed `Resource` failure through the `Ask<StartRun, AgentResponse>` protocol, so "closed" here means
  what it already means elsewhere in this handler.
- Neither F1/F2/F3's mechanisms nor F4 touch the tenant-in-key ADR-track item (004 §5's own flagged
  residual, decision 6) — that stays explicitly deferred, unactioned by this phase, per the milestone
  doc's own scoping above.

### Phase G — Recording and replay (004 §6, decision 8)

- **G1.** Promote the recording mechanism from `RecordedChatClient`'s test-scoped fixture player to
  the real thing: every real backend (D/E) records request, response or full ordered chunk sequence,
  timing, and usage.
- **G2.** Replay serves from the recording with identical chunk boundaries — streaming-dependent
  behavior (early tool dispatch, UI cadence) reproduces exactly (this is 004 §7 G3's own gate).
- **G3.** Privacy: off by default (decision 8's scoped-down slice); full metadata-only/hashed-content
  modes deferred to M8 alongside 016.

### Phase H — Identity and admission, scoped past 011/012 (018 §1-2, decisions 1/9/10)

- **H1.** Principal establishment for the two buildable §1 rows: Embedded/in-process (host-supplied)
  and Local CLI (local user identity) — formalizing the project's existing hand-construct pattern into
  a real, if narrow, mechanism rather than leaving it purely ad hoc in every test.
- **H2.** Admission check at the `AgentSession` actor boundary (018 §2): may this principal start a run
  on this session at all, checked before Phase B/D/E's `ChatClient::chat()` is ever reached — session
  ownership, not just effect-level capability checks (007, already real).
- **H3.** Token audience validation and no-token-passthrough (§1's confused-deputy rules) — proven at
  the scope H1 covers; the full inbound-surface version (HTTP bearer/OIDC/mTLS) waits for 013 (M7).
- **H4.** `on_behalf_of` delegation expression (007 §2, already vocabulary-real) wired through outbound
  `ChatClient` calls made on a principal's behalf, closing §1's "no passthrough, delegation only via
  `on_behalf_of`" rule for the one surface this milestone actually builds (outbound provider calls).

### Phase I — Multi-tenancy proof (018 §6-7, decisions 6/9/10)

- **I1.** Cross-tenant denial tested across the surfaces that exist today: session (005/M4), memory
  (029/M4), sandbox workspace (008/M2-M3). Audit-query surface named explicitly out of scope (no audit
  log/query API exists yet) — G4 proven narrower than its full text.
- **I2.** Delegation attenuation (G5) proven at 007's existing scope (decision 10) — a derived
  principal/capability set never gains authority its parent lacked.

### Phase J — The milestone's central falsifiable claims (roadmap exit criterion)

- **J1 (004 §7 G1).** The same agent, unchanged, runs on all three conformers built in Phases D/E/G
  (OpenAI-compatible, Anthropic, recorded-replay) — behavioral differences limited to those the
  capability table predicts, every applied fallback (Phase B5) appears in the trace.
- **J2 (018 §7 G2, secret hygiene).** A canary secret is planted and every persisted artifact this
  milestone's mechanisms actually produce is scanned — checkpoints (real, M4), recordings (Phase G),
  error strings/logs from Phases D/E/F — zero occurrences. Audit payloads/crash dumps/plugin memory
  dumps named narrower than the full gate text (no audit sink or crash-dump pipeline exists yet),
  matching decision 9's I1 scoping.
- **J3 (004 §7 G2, secondary but load-bearing for J1's own backends).** Cancellation mid-stream
  (Phase C2) releases the connection within a bounded time, no orphaned socket or partial state, under
  ASan + the existing leak gate.

## What's explicitly deferred past M5

- **HTTP/AG-UI/OpenAI-compatible and A2A/MCP-as-server inbound identity** (018 §1's other three
  rows) — need 013/012/011 respectively, all M7 (decision 1).
- **The full 018 §7 G4 admission surface** (audit query) — no audit log/query API exists yet.
- **Full A2A/MCP delegation chains** (018 §7 G5's complete text) — need 012/011, M7 (decision 10).
- **`Local/embedded via OpenAI-compatible server` and `Remote agent as ChatClient`** (004 §3's other
  two table rows) — real reach, not required for the exit gate (decision 5); remote-agent-as-
  `ChatClient` specifically needs 012 (M7).
- **Batch API wiring** (004 §8 Q1's resolution: batch-eligible calls classified `Backgroundable`,
  completed via 019 §2's wake table) — needs `Backgroundable`/`StandingEffect` (006 §6b), confirmed
  never built even though 006 has been real since M2 (the same residual M4 decision 4 already named).
- **Per-tenant/session/principal budget ceilings from config** (004 §5, needs 020, M9) and
  **cost-metric emission to the 016 surface** (needs 016, M8) — decision 7.
- **016's full recording-privacy mode set** (metadata-only/hashed-content) — decision 8; only
  off-by-default ships this milestone.
- **OS keychain/DPAPI/Keychain and external secret-manager backends** (Vault, cloud KMS) — decision 3
  in Phase A ships env + file only; the `SecretStore` seam is designed to take more backends later
  without a shape change.
- **The tenant-in-breaker-key / tenant-in-cost-metric-key ADR-track item** (004 §5's own
  invariant-touching flag) — a recommendation for the project owner pending the full
  design→red-team→prove→judge cycle, not yet actioned (decision 6).
- ~~`ae::task<T>` for non-void `T`~~ — landed upstream (ADR-047) and wired in, Phase B4a.
  ~~`ae::stream<T>`~~ — built, Phase B4b (`core/stream.hpp`). Still owed: threading `task<T>`/a live
  producer into `sandbox/provider_http_client.hpp`'s SSE read loop and a real Phase D/E backend's own
  `chat()`/`chat_stream()` bodies (deliberately deferred there, not a gap in B4a/B4b).
- **10⁴-scale gates and full 023 baselining** — stay `TBD-baselined` project-wide until M8, same
  status quo every earlier milestone established.
- **A portable `reasoning_effort` design and `prompt_cache_key`** for Phase D/E's two backends —
  surveyed, still not built (both explicitly need a further design decision, see
  `docs/research/2026-08-07-provider-metadata-and-sampling-params-survey.md`'s "Recommended design"
  section for the options). Everything else that survey recommended (app-attribution headers, `seed`/
  abuse-tracking-id constructor fields, `ChatResponse.model`, `Usage.cache_write_tokens`, Anthropic's
  4-cache_control-blocks hard invariant, and its cache-TTL constructor option) landed 2026-08-07, built
  by two parallel subagents against that same doc. Also still flags an untested llama.cpp
  structured-output wire-shape compatibility risk (Phase D4's `translate_output_schema` may not match
  llama.cpp's simpler `response_format` shape) — not built, no live llama.cpp instance available.

## Handover & kick-off

Written 2026-08-07, immediately following M4's close (`Milestone 4 Phase H`, commit `09933fd`).
Phase A (secret seam), Phase B (`ChatClient` made real, including B4a's `task<T>` upgrade), and Phase
C (host-side HTTP/SSE client) are done. **Update, same day:** the project owner pursued the
`ae::task<T>` upstream Quark change directly (`QuarkCpp`, ADR-047) rather than accepting the
synchronous fallback; Phase B4a landed the moment the submodule bump reached this repo (commit
`634a2cc`), closing that decision point in the "pursue it now" direction. **Update, same day:** B4b
(`ae::stream<T>`, `core/stream.hpp`) is also done — the project owner asked for it directly rather than
leaving it carried forward, even though it gates neither exit-criterion gate. Phase B is now fully
closed. Phase D/E (the two live backends) can start whenever the project owner wants; nothing in Phase
B/C blocks them. Still open, carried from the original kick-off: confirmation of decision 6's ADR-track
recommendation (tenant in the breaker/cost-metric key).
