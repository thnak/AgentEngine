# 012 — A2A Conformance

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Protocol revision:** A2A **v1.0** (April 2026, Linux Foundation) · **Depends on:** 001, 003, 006, 018 · **Gate:** §8

## Goal

Expose AgentEngine agents as A2A peers, and consume remote A2A agents as first-class collaborators,
at a named protocol revision with an executable conformance suite (**I7**).

## 0. Normative source and version handling

- **`specification/a2a.proto` is the single authoritative definition.** The published JSON schema is
  explicitly a *non-normative build artifact*. Our wire types are therefore **generated from the
  proto**, never hand-written from JSON examples, and regenerated rather than edited (CONVENTIONS
  §Protocol code rules).
- **`A2A-Version` is sent on every request.** An omitted version means `0.3` to a compliant agent, so
  omitting it is not a neutral default — it silently selects a legacy protocol. As a server we return
  `VersionNotSupportedError` for versions we do not implement. Only `Major.Minor` appears on the
  wire; patch numbers **MUST NOT**.
- JSON is camelCase; enums serialize as full SCREAMING_SNAKE names (`TASK_STATE_INPUT_REQUIRED`,
  `ROLE_USER`).
- **v1.0 removed the `kind` discriminator from `Part`.** The JSON member name (`text` / `raw` / `url`
  / `data`) *is* the discriminator, with `metadata`, `filename`, and `mediaType` common to all kinds.
  A mapping layer written against 0.3.x examples will produce wire-invalid parts, so this is a
  round-trip test case, not a footnote.

## 1. Why A2A maps cleanly

A2A's `Task` lifecycle is `SUBMITTED → WORKING → COMPLETED | FAILED | CANCELED | REJECTED`, with the
interrupted states `INPUT_REQUIRED` and `AUTH_REQUIRED`. **That is exactly the run lifecycle in
001 §2** — chosen deliberately so hosting a run as an A2A task is a projection, not a translation
with a state-machine impedance mismatch in the middle.

Similarly, A2A's `Message → Part[]` and `Artifact → Part[]` are the content model of 003. The
mapping layer is thin by construction.

## 2. Server surface (AgentEngine as an A2A agent)

### 2.1 Agent Card

Generated from agent metadata (002 §7) — never hand-maintained, because a hand-maintained card
drifts from behaviour:

- identity, provider, version, description;
- **skills** derived from the agent's declared tool set and skill plugins (009 §8);
- capabilities: streaming, push notifications, extended card;
- service endpoints per binding;
- authentication schemes and required scopes (018);
- signing, so a consumer can verify the card's integrity; cache headers honoured.

**Rule:** the card advertises only what the conformance suite proves. An unimplemented capability
is absent from the card, not present and broken.

### 2.2 Bindings

A2A v1.0 defines three bindings — **JSON-RPC 2.0**, **gRPC**, **HTTP+JSON/REST** — and **a server
need not implement all of them**: agents declare what they support in the card and clients pick.
That settles the scope question: v1 implements **HTTP+JSON/REST and JSON-RPC**, and gRPC is deferred
behind an option (it is the binding that adds a heavy host dependency, which CONVENTIONS puts in a
seam backend, never the core).

The obligation that *does* bite: where more than one binding is offered, all **MUST** provide
identical functionality, consistent behaviour, the same error handling, and equivalent
authentication. Our two bindings are therefore one implementation with two encoders, not two
implementations — and the TCK checks each binding separately (§8).

### 2.3 Task management

- `Task` ← `Run` (001). `contextId` groups related tasks and maps onto our session.
- **Streaming** projects the internal run event stream (013 §1) — the same source as AG-UI.
- **Ordering is a MUST, and we inherit it for free**: A2A requires that events are delivered in
  generation order and never reordered, on any binding. Our stream is already ordered and
  sequence-numbered per run (013 §1). Where multiple clients subscribe to one task, events **MUST**
  be broadcast identically and in the same order to every stream, and closing one **MUST NOT** affect
  the others — which is why best-effort fan-out (the shape Quark's `Topic<M>` had, evaluated and
  rejected for this purpose — 013 §7 Q2; historical citation, since ADR-037 removed Quark as a
  dependency entirely) is the wrong primitive here.
- **`SubscribeToTask`'s first event MUST be the current `Task` snapshot**, which closes the
  get-then-subscribe race by construction.
- **Push notifications** deliver a `StreamResponse` over plain HTTP+JSON regardless of the agent's
  binding, authenticated per `AuthenticationInfo`. Requirements we implement: at-least-once delivery,
  a 10–30 s timeout, backoff, idempotent processing on receipt (duplicates are expected), task-id
  verification, and **SSRF validation of webhook URLs** — which routes through the same host-mediated
  egress as everything else (008 §4) rather than a bespoke check. Config deletion **MUST** be
  idempotent. **The event id a delivery is deduplicated by is `task_id` + the event's sequence
  number** — no separate id scheme, since 013 §1's internal stream is already ordered and
  sequence-numbered per run and `task_id` already **is** `run_id` (§1); a receiver dedupes on that
  pair directly. **Dead-lettering is bounded**: a notification that exhausts its retry/backoff budget
  moves to a capped, operator-visible dead-letter queue rather than retrying unboundedly or being
  silently dropped, with an alert on entries arriving and a manual or policy-driven drain — the same
  "bounded, visible, never silent" shape 008 applies to resource denial elsewhere in this project.
- **`INPUT_REQUIRED`** is emitted by the unified human-in-the-loop mechanism (001 §2). Note that A2A's
  shape — the task **stays alive** and the client sends a new message on the same `taskId` — is one
  of *three* incompatible shapes for this one idea (§5a).
- **Terminal is terminal**: further messages to a terminal task are rejected with
  `UnsupportedOperationError`, streams close, the task is immutable, and a continuation is **a new
  task under the same `contextId`**.

### 2.4 Streaming is not a delivery guarantee

The spec is unusually blunt, and it changes how we build the client:

> Clients using streaming to retrieve task updates **MAY** not receive all status update messages if
> the client is disconnected and then reconnects. Messages **MUST NOT** be considered a reliable
> delivery mechanism for critical information.

There is no `Last-Event-ID`-style cursor resume; reconnection is a fresh `SubscribeToTask` that
re-delivers a `Task` snapshot. **Therefore: `Task` and `Artifact` state, fetched via `GetTask`, is
the source of truth, and no engine state may depend on having observed a particular `Message`
event.** This mirrors the MCP re-issue rule (011 §1) — both protocols traded stream resumability for
statelessness, and both push the durability burden onto explicit state.

## 3. Client surface (consuming remote agents)

A remote A2A agent is bound as a tool or a handoff target (002 §4) with the same declaration syntax
as a local one:

- **Discovery** by well-known agent card URL, or explicit configuration; cards are cached, verified,
  and **digest-pinned** — a card whose skills or schemas change is re-approved rather than silently
  trusted (007 §7, the same anti-rug-pull discipline as MCP servers).
- **Deadlines propagate** as remaining duration: a remote agent inherits the caller's budget rather
  than restarting the clock (historical: previously cited to Quark's own RFC 018 as the design
  precedent — ADR-037 removed Quark; deadline/budget propagation now lives in `agentengine::rt::`,
  though the exact current implementation is not re-verified here).
- **Cancellation propagates** — a canceled run cancels the remote task; a remote task that ignores
  cancellation is bounded by the deadline.
- **Long-running remote tasks** map onto our `Suspended` state (019), so waiting on a multi-hour
  remote task does not pin an activation or a connection.
- **Remote responses are tainted external content** (003 §2). A peer agent is exactly as trusted as
  a web page.

## 4. Identity and authorization

- Outbound: the declared scheme from the card (API key, OAuth2, mTLS, OIDC), with credentials from
  the secret seam (018) — never from configuration files in plaintext, never in a message.
- Inbound: the engine authenticates the caller, establishes a **principal** (007 §2), and executes
  with a capability set derived from that principal — **never** the host's.
- **Delegation is explicit and bounded**: an inbound request carrying `on_behalf_of` produces a
  derived principal with attenuated authority and a depth-bounded chain, recorded in the audit log.
- `AUTH_REQUIRED` is the sanctioned way to ask for credentials mid-task rather than failing, and
  chains across agents are permitted. **But the spec's own limit is the one that matters to us:**

  > Agents **MUST NOT** treat the `TASK_STATE_AUTH_REQUIRED` state transition, by itself, as
  > authorization for any particular operation.

  That is **I3** in A2A's own words — a state transition is not a grant. A credential arriving
  through this path enters the capability model (007) as a *request* to be evaluated against policy,
  and never as authority.
- **Two spec clauses adopted verbatim as server obligations**, because they close an information
  leak our own design otherwise had to invent a rule for:

  > Authorization checks **MUST** occur before any data access that could leak resource existence.
  > Servers **MUST NOT** reveal existence of resources outside the caller's authorized scope, and
  > **SHOULD NOT** distinguish "not found" from "not authorized" in error responses.

  This applies to `GetTask`, `ListTasks`, and every push-config operation.

### 4a. Agent Card integrity

Cards **MAY** be JWS-signed over an RFC 8785 canonicalization, with the `signatures` field itself
excluded from the signed payload. As a client we **verify at least one signature before trusting a
card** where signatures are present, and pin by digest regardless (§3). As a server we sign, because
an unsigned card is an unauthenticated description of what we will do on someone's behalf.

Cards are cached per RFC 9111 with `ETag`/conditional requests — which is also what makes
digest-pinned change detection cheap.

### 4b. Extensions

`AgentCapabilities.extensions[]` declares them; clients opt in per request via `A2A-Extensions`.
Three rules we implement: version lives in the extension URI and **a breaking change requires a new
URI**; a `required: true` extension we do not support means the peer **MUST** fail us with
`ExtensionSupportRequiredError` rather than silently degrade; and — matching MCP's posture (011 §3.6)
— **extensions are disabled by default and require explicit opt-in.** Extension support is not
required for base conformance, and we claim none by default.

## 5. Multi-agent topology

A2A is the **inter-organization / inter-process** seam; in-process composition stays in 002 §4 and
014. The engine does not force local agents through A2A to talk to each other — that would trade a
mailbox message for an HTTP round trip. The call-site uniformity rule (002 §4) means an author can
move an agent across that boundary without rewriting callers.

## 5a. Human-in-the-loop has three incompatible shapes

The single most useful cross-protocol finding, and it sharpens [OQ-4](OpenQuestions.md):

| Protocol | Shape | Correlation identity |
|---|---|---|
| **MCP** `2026-07-28` | Client **retries the original request**, with a *new* JSON-RPC id | `requestState` |
| **A2A** v1.0 | Task **stays alive**; client sends a new message on the same task | `taskId` |
| **AG-UI** | Run **ends**; client starts a **new run** carrying `resume[]` | `interruptId` |

One conceptual event — *the agent needs something from a human* — expressed as a retry, a
continuation, and a restart. Our internal `InputRequired` (001 §2) must project to all three without
losing the correlation identity each requires. That is a stronger constraint than "emit an event",
and it is why the unification is a design question rather than a mapping detail.

**Resolved:** the internal identity is `Interaction.interaction_id` (001 §2); each row above maps
it into that protocol's required shape (013 §2.2 has the full mapping). For A2A specifically, no
new mapping was needed — §1's `Task ← Run` identity already means `taskId` disambiguates the run's
one outstanding `INPUT_REQUIRED` without carrying anything extra.

## 6. Interop hygiene

- **Round-trip fidelity**: `internal → A2A → internal` preserves every part including unknown ones
  (003 §5), proven by a round-trip test.
- **Artifacts** map to our content-addressed artifacts (010 §4) with digests preserved.
- **Errors** map to A2A's error model with our classification (001 §6) preserved in structured
  fields, not flattened into a string.
- Protocol version is a build-visible constant and the suite is tagged with it (CONVENTIONS).

## 7. Observability

Every inbound and outbound A2A call is a span with `{peer agent id, card digest, task id, context
id, binding, state transitions}`, linked to the run's trace via standard context propagation, and an
audit record per effect. Task state transitions are events, so a stuck task is visible as a state
histogram rather than as a support ticket.

## 8. Promotion gate

- **G1** — **`a2a-tck` passes** against our server at v1.0 for each implemented binding, on Windows
  and Linux, with **zero MUST-level failures**; SHOULD-level failures are recorded with
  justifications rather than silently `xfail`-ed. The kit derives the transports to test from our
  own `supportedInterfaces`, so the card and the implementation are checked against each other for
  free. Reports (`compatibility.json`, `junitreport.xml`) are CI artifacts.
- **G2** — round-trip fidelity test (§6) passes over a corpus covering every part kind, including
  unknown parts.
- **G3** — full lifecycle coverage: each state transition, including `INPUT_REQUIRED`,
  `AUTH_REQUIRED` (projected from the internal `AuthRequired`/`AuthResolved` events, 013 §1/§2.2),
  cancel-in-flight, and terminal-state rejection, exercised end to end.
- **G4** — push notification delivery is reliable under injected failures: no loss, no duplicate
  effects (idempotent by task+event id — `task_id` + 013 §1 sequence number, per §2.3), bounded
  dead-lettering (§2.3's capped queue with operator-visible alert and drain).
- **G5** — a remote agent bound as a tool is indistinguishable at the call site from a local one
  (compile-time + behavioural test).

## 9. Open questions

- ~~**Q1** — gRPC binding: worth the dependency, or leave to a deployment-side proxy?~~ **Resolved,
  deployment-side proxy, no first-party gRPC binding for v1 (2026-08-04):** §2.2 already flags gRPC
  as the CONVENTIONS heavy-dependency binding; adding it as a third in-engine encoder would extend
  §2.2's "one implementation, N encoders" equivalence obligation — and therefore the TCK gate (§8 G1)
  — to a protobuf/gRPC stack for a binding that's mechanically redundant with our already-implemented
  JSON-RPC (same `a2a.proto`-generated wire types, §0, different encoding only). A standard gRPC↔JSON
  transcoding proxy (grpc-gateway, Envoy's grpc-json transcoder) in front of our HTTP+JSON/REST
  binding is exactly the well-understood mechanical translation a deployment-side proxy does well.
  We do not claim gRPC conformance ourselves — an operator fronting us with a transcoding proxy owns
  that interoperability claim, honestly outside what our own TCK run can prove (021 §1's "no claim"
  discipline, the same move as OQ-1's macOS drop). A first-party adapter is addable later, as an
  optional seam backend, if real deployment demand shows the proxy approach insufficient — not
  designed speculatively now.
- ~~**Q2** — Agent card signing: which trust root, and how does key rotation work for a self-hosted
  engine?~~ **Resolved, self-signed, no built-in CA/PKI; rotation reuses the existing change-triggers-
  re-approval rule (2026-08-04):** there is no assumption of a shared root across arbitrary A2A
  peers the way TLS has a web PKI, and there doesn't need to be — 018 §8 Q4's resolution already
  establishes that the actual trust decision is the **caller's own configured pin** per known peer,
  never a chain-of-trust to an authority. That means the JWS signature's job (§4a) is narrower than a
  CA chain would imply: it proves in-transit/storage integrity between publish and fetch, not identity
  vouching — vouching was already answered elsewhere. So: the signing key is self-generated by the
  operator, resolved through the existing secret seam (018 §4) like any other credential, with the
  public key discoverable alongside the card (a `.well-known`/`jwks`-style reference) — no new
  key-management subsystem. **Rotation needs no new mechanism either**: a signing-key change changes
  the card, which already triggers §3's "a card whose skills or schemas change is re-approved rather
  than silently trusted" — the identical rug-pull defense already covers a key change, since the
  card's digest changes along with its signature. An operator wanting zero-downtime rotation
  dual-signs or dual-publishes during a transition window — an operational practice, not new engine
  machinery.
- ~~**Q3** — Whether `context_id` should map to our session id directly or to a group of sessions;
  multi-agent sessions (005 Q2) forces the answer.~~ **Resolved (005 §8 Q2, 2026-08-04):**
  `context_id` maps to `session_id` one-to-one, never to a group. 005 §1's data model has exactly one
  `history[]` per session by construction, and multi-agent-in-one-session cases (Handoff, 014 §3
  patterns) get per-agent *views* over that one history, not separate storage — there is never a
  "group of sessions" for one context to span in the first place.
- ~~**Q4** — A2A + MCP overlap: an MCP server exposing an agent-shaped tool and an A2A peer are two
  spellings of the same thing. Guidance on which to publish is missing.~~ **Resolved (011 §13 Q4,
  2026-08-04):** publish both by default — 002 §7 generates them from one metadata table with no
  drift risk, so there's no real cost to picking one over the other. They serve different consumer
  populations: MCP tool listings for LLM-tool-calling clients wanting a bounded function call, A2A
  Agent Cards for peer orchestrators wanting to delegate an open-ended goal with task-lifecycle
  machinery. Which style an agent's skills description emphasizes is an authoring-quality question,
  not a protocol-exposure restriction; enabling either surface is an ordinary per-agent 020 §4
  configuration choice.
