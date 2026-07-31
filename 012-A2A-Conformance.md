# 012 — A2A Conformance

**Status:** Draft · **Protocol revision:** A2A **v1.0** (April 2026, Linux Foundation) · **Depends on:** 001, 003, 006, 018 · **Gate:** §8

## Goal

Expose AgentEngine agents as A2A peers, and consume remote A2A agents as first-class collaborators,
at a named protocol revision with an executable conformance suite (**I7**).

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

A2A v1.0 defines three functionally equivalent bindings: **JSON-RPC 2.0**, **gRPC**, and
**HTTP+JSON/REST**. v1 implements **HTTP+JSON/REST and JSON-RPC**; gRPC is deferred behind an
option (it is the one that adds a heavy host dependency, and per CONVENTIONS that is a seam
backend, never core).

### 2.3 Task management

- `Task` ← `Run` (001). `context_id` groups related tasks and maps onto our session.
- **Streaming** projects the internal run event stream (013 §1) — the same source as AG-UI.
- **Push notifications** are webhook deliveries with the standard reliability requirements: signed,
  retried with backoff, idempotent by task+event id, dead-lettered after a bounded number of
  attempts (Quark 017's delivery discipline, not a bespoke retry loop).
- **`INPUT_REQUIRED`** is emitted by the unified human-in-the-loop mechanism (001 §2), so an
  approval gate (006 §4), a workflow request port (014), and an MCP MRTR round-trip all surface
  identically to an A2A caller.
- **Terminal is terminal**: further messages to a terminal task are rejected, and a continuation is
  a new task.

## 3. Client surface (consuming remote agents)

A remote A2A agent is bound as a tool or a handoff target (002 §4) with the same declaration syntax
as a local one:

- **Discovery** by well-known agent card URL, or explicit configuration; cards are cached, verified,
  and **digest-pinned** — a card whose skills or schemas change is re-approved rather than silently
  trusted (007 §7, the same anti-rug-pull discipline as MCP servers).
- **Deadlines propagate** as remaining duration (Quark 018): a remote agent inherits the caller's
  budget rather than restarting the clock.
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
- `AUTH_REQUIRED` is the sanctioned way to ask for credentials mid-task rather than failing.

## 5. Multi-agent topology

A2A is the **inter-organization / inter-process** seam; in-process composition stays in 002 §4 and
014. The engine does not force local agents through A2A to talk to each other — that would trade a
mailbox message for an HTTP round trip. The call-site uniformity rule (002 §4) means an author can
move an agent across that boundary without rewriting callers.

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

- **G1** — the conformance suite passes against the reference A2A test tooling for the implemented
  bindings, at v1.0, on Windows and Linux.
- **G2** — round-trip fidelity test (§6) passes over a corpus covering every part kind, including
  unknown parts.
- **G3** — full lifecycle coverage: each state transition, including `INPUT_REQUIRED`,
  `AUTH_REQUIRED`, cancel-in-flight, and terminal-state rejection, exercised end to end.
- **G4** — push notification delivery is reliable under injected failures: no loss, no duplicate
  effects (idempotent by task+event id), bounded dead-lettering.
- **G5** — a remote agent bound as a tool is indistinguishable at the call site from a local one
  (compile-time + behavioural test).

## 9. Open questions

- **Q1** — gRPC binding: worth the dependency, or leave to a deployment-side proxy?
- **Q2** — Agent card signing: which trust root, and how does key rotation work for a self-hosted
  engine?
- **Q3** — Whether `context_id` should map to our session id directly or to a group of sessions;
  multi-agent sessions (005 Q2) forces the answer.
- **Q4** — A2A + MCP overlap: an MCP server exposing an agent-shaped tool and an A2A peer are two
  spellings of the same thing. Guidance on which to publish is missing.
