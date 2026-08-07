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
| Secret seam (`SecretRef`/`SecretStore`/`SecretLease`, 018 §4) | **Does not exist. Greenfield.** Zero implementation hits anywhere in `include/`/`src/` for `SecretRef`, `SecretStore`, `SecretLease`, `api_key`, `credential` — only prose in `.md` files |
| `CapabilityToken`/`SecretKey` (`trust/capability_token.hpp`, ADR-005) | Real, Judged, red-teamed — but **a different concept wearing a confusable name.** This is the HMAC-chain root key for cross-process capability bearer tokens (018 §8 Q2, already resolved and closed). Not 018 §4's outbound-credential seam; do not conflate when scoping this milestone |
| `agentengine::Principal` (`trust/principal.hpp:9-12`) | Real, bare `{id, tenant_id}` struct, wired into `EffectContext`/`AgentSession`/`MemoryOrigin` (unchanged since M4). No registry, no auth mechanism producing a `Principal` from any inbound request — every test-constructed `Principal` today is hand-built C++ |
| `EffectContext` (`core/effect_context.hpp:16-39`) | Real, carries `principal`, `capabilities`, `bound_capabilities`, `deadline`, `trace_id`/`span_id`, `run_id`/`turn_index` (since M4). This is exactly the parameter 018 §4's `SecretStore::resolve(SecretRef, EffectContext&)` should hang off of — nothing does yet |
| Admission check at the `AgentSession` actor boundary (018 §2) | Does not exist. Consistent with M4's own finding that `session_id_` wasn't even actor-keyed until Phase A1 — no principal-ownership check runs before a turn starts |
| ADR-013 (HTTPS egress, mbedTLS) | **Accepted.** `TlsClientSession` (`include/agentengine/sandbox/tls_client.hpp`) wraps mbedTLS 3.6.7, real hostname verification, real HTTPS exchange — but **scoped to one consumer**: ADR-013 §9 states "only the WASM `http-request` import consumes this," gated through a guest-held `cap::NetOut` grant, called only from `wasm_backend.cpp`/`net_egress_proxy.cpp`. **No general-purpose host-side HTTP/SSE client exists** for a native `ChatClient` backend to call `api.openai.com`/`api.anthropic.com` directly — reusing `TlsClientSession`'s TLS primitive is plausible, but the call path (capability-grant/single-verified-endpoint model built for sandboxed guest egress) is new wiring for a host-initiated call with SSE streaming, retries, and vendor JSON bodies |
| Quark `task<T>` (submodule pinned `9ecbf1c`, `third_party/quark/include/quark/core/task.hpp:17-22`) | **Only the `void` specialization is implemented.** Quark's own file comment: "a value-returning `ask` task<T> is 006/ADR-007" — forward-declared, not real. 004 §1's literal signature (`ae::task<result<ChatResponse>>`) cannot be built as written until Quark adds this. `027-Vocabulary-and-Naming.md:112,126` already names `ae::task<T>` as the intended alias; M2's own breakdown doc already deferred wiring it (decision 2 below) |
| Quark streaming (RFC 024, ADR-018) | **Accepted (x86-64), real code**: `include/quark/core/stream_channel.hpp` (inbound credit-ring), `include/quark/core/reply_stream.hpp` (`ReplyStream`/`AskStream`, outbound, Accepted 2026-08-04). This is actor-mailbox streaming, not an HTTP/SSE-facing type — no `ae::stream<T>` wrapper exists on the AgentEngine side; `chat_client.hpp:60-64`'s own comment confirms this |
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

### Phase A — The secret seam (018 §4), no Quark dependency, starts immediately

- **A1.** `SecretRef` (a name, not a value) and `SecretLease` (short-lived, non-copyable, redacted by
  construction — formatter/serializer/debug output all print `***`; the type itself is unprintable,
  not a documentation promise).
- **A2.** `SecretStore` concept: `resolve(SecretRef, EffectContext&) -> result<SecretLease>`. Kept
  synchronous (`result<T>`, not `ae::task<T>`) per decision 2 until Quark's `task<T>` lands; upgraded
  in lockstep with Phase B once it does.
- **A3.** Backends: environment (dev) and file-with-restrictive-permissions first — the two that need
  no new host dependency. OS keychain/DPAPI/Keychain and external managers (Vault, cloud KMS) are
  named as later seam backends (deferred list), not required for this milestone's own gate.
- **A4.** Scoping per `{provider|server|peer, principal-or-service}` (018 §3) and rotation-without-
  restart: a resolved lease is never cached past its own lifetime: the *next* `resolve()` call picks
  up a rotated backing value, proven by swapping the backend's stored secret between two calls in a
  test and asserting the second lease differs — no in-flight call is broken because nothing holds a
  lease across an await boundary by construction (A1's non-copyable rule).

### Phase B — `ChatClient` made real (004 §1-2)

- **B1.** `ChatRequest` gains tool declarations (reusing 006's real `ToolDecl`) and a structured-output
  schema field (003 §4/§5's existing shape) — un-eliding both, the same "stale placeholder, not new
  design" move M4 decision 6 made for `ContextContribution.tools`.
- **B2.** Complete `ChatClientCapabilities`: add `stop_sequences`, `seed`, `token_counting`, `batch` to
  reach 004 §2's full bitset.
- **B3.** Outbound-credential wiring (decision on the type of a constructor param, not new mechanism):
  a native `ChatClient` backend is constructed with a `SecretRef`, resolves it through `SecretStore`
  inside `chat()`/`chat_stream()` at the point of use, per 004 §1 and Phase A. Proven by a test backend
  that fails the build/a static assertion if it stores a resolved `SecretLease` as a member.
  `SecretLease`'s non-copyable, scope-bound shape (A1) is what makes holding one across calls a
  compile error, not a review-time judgment call.
- **B4.** `ae::stream<T>` adapter over Quark's `ReplyStream`/`StreamChannel` (decision 3) —
  `chat_stream()` becomes a real, constrained type, not an unconstrained callable.
- **B5.** The degradation rule (§2): capability-driven fallback (native structured output → tool-shaped
  per 003 §4) recorded in the run trace; `register_agent<A>()` fails at startup when no fallback exists
  (002 §6), not at first request — extending `agent_registry.hpp`'s existing startup-check pattern.
- **B6.** Wire `check_chat_client_credentials`/`check_output_schema_enforceable` (`agent_registry.hpp`)
  off the real concept instead of their permanent-stub bodies — needs a minimal `ChatClient` registry
  (a map from `ChatClientId` to a bound instance), the smallest possible slice of an eventual `Engine`
  type, not a full `Engine` build-out (named out of scope, deferred list).

### Phase C — Host-side HTTP/SSE client (new wiring, decision 4)

- **C1.** A general-purpose async HTTP/SSE client built on `TlsClientSession`'s TLS primitive
  (`sandbox/tls_client.hpp`), for host-initiated calls — request/response and chunked SSE parsing,
  independent of the WASM guest-egress capability-grant call path.
- **C2.** `stop_token`-driven cancellation propagated into the socket layer (004 §1's rule) — the basis
  for G2's "cancellation mid-stream releases the connection within a bounded time, no orphaned socket."

### Phase D — OpenAI-compatible backend (004 §3, default)

- **D1.** Request/response translation for Chat Completions + Responses shapes; capability set
  discovered from config per endpoint, never assumed uniform (§3's own rule).
- **D2.** Streaming chunk parsing into `ChatResponseUpdate` via Phase B4's stream adapter.
- **D3.** Tool-schema shaping per vendor wire format; MCP-tool-vs-native-tool routing split (§3's
  porting-note checklist item — translation logic, not a design question).
- **D4.** Structured-output shaping: force `additionalProperties: false` into the JSON Schema before
  it reaches the provider (§3's other named checklist item).

### Phase E — Anthropic backend (004 §3, first-class)

- **E1.** Request/response translation for Claude 5 family (Fable 5, Opus 5, Sonnet 5) and Haiku 4.5.
- **E2.** Cumulative→incremental `Usage` conversion on stream events (§3's specific porting note —
  Anthropic reports usage cumulatively per event, the engine needs uniform per-chunk `Usage`, 003 §6).
- **E3.** Reasoning parts and prompt caching as first-class (§3) — caching inserts vendor-specific
  breakpoints at 005 §3's existing context-assembly segment boundaries, per 004 §8 Q2's resolution
  (no new portable cache-hint type; backend-internal translation only).

### Phase F — Reliability and cost (004 §4-5, scoped per decisions 6/7)

- **F1.** Retry: `Transient` failures only (001 §6), bounded exponential with jitter, respecting the
  remaining deadline (never the retry's own timeout) — a stable idempotency key on the retried call.
- **F2.** Rate limiting/circuit breaking: wire Quark 022's real `TokenBucket`/`CircuitBreaker`
  (`governance.hpp:47,104`) into the HTTP provider call path — new integration, proven primitive.
  Breaker key stays `{provider, model, SecretRef}` for now; adding `tenant` is decision 6's flagged
  ADR-track item, not actioned in this phase.
- **F3.** Failover as explicit policy only — appears in trace and response metadata, never silent.
- **F4.** Per-run `TokenBudget<N>` enforced at the turn boundary → `Resource` failure (decision 7's
  buildable slice of §5).

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
- **`ae::task<T>` for non-void `T`** — genuinely gates Phase B's literal async signature. If it hasn't
  landed upstream in `QuarkCpp` by the time Phase B starts, `ChatClient::chat()` ships synchronous
  (today's shape) and is upgraded in a follow-up once the Quark-side primitive is real (decision 2)
  — named as a real schedule risk for this milestone, not a hidden one.
- **10⁴-scale gates and full 023 baselining** — stay `TBD-baselined` project-wide until M8, same
  status quo every earlier milestone established.

## Handover & kick-off

Written 2026-08-07, immediately following M4's close (`Milestone 4 Phase H`, commit `09933fd`).
Phase A (secret seam) has no upstream dependency and is the concrete next step. Two items need a
decision before Phase B can proceed on schedule: whether to pursue the `ae::task<T>` upstream Quark
change now (in the `QuarkCpp` working directory available this session) or accept the synchronous
`chat()` fallback named in decision 2/deferred-list — and confirmation of decision 6's ADR-track
recommendation (tenant in the breaker/cost-metric key), carried forward the same way M4 carried
Phase F3's exactly-once recommendation into its own kick-off.
