# ADR-018 — Hosting a real `ChatClient` backend in `AgentSession`

**Status:** Accepted — 2026-08-07
**Closes:** the Milestone 5 Phase J1 residual, "`AgentSession<ChatClientT>` cannot host a
non-default-constructible `ChatClient` conformer"
(`docs/planning/milestone-5-providers-identity-secrets-breakdown.md`).
**Relates to:** 001-Execution-Model.md §3 (the turn loop), 004-Model-Provider-Plane.md §1/§3,
018-Identity-Authorization-and-Secrets.md §4, ADR-016 (the plaintext provider transport this ADR's
proof uses).

## 1. Context

Phase J1 found, while proving 004 §7 G1, that no real backend had ever been driven through
`AgentSession`'s turn loop:

> `chat_client_` has no setter and `quark::TestKit<A>` only default-constructs its actor — a real
> backend built for Phase D/E has never actually been driven through a live `AgentSession` turn loop.

Investigating it turned out to be sharper than "no setter", and in two ways.

**(a) It did not fail to configure; it failed to compile.** `quark::TestKit<A>` declares `A actor_;`
and default-constructs it. `AgentSession` held `ChatClientT chat_client_;` as a plain value, so
`AgentSession<ChatClientT>` is default-constructible only if `ChatClientT` is. A real backend is not:
`OpenAIChatClient<Store>` and `AnthropicChatClient<Store>` both hold a `Store const&`, because 004 §1
requires the credential to be resolved **at the point of use** rather than captured at construction.
So `AgentSession<OpenAIChatClient<...>>` could not be named under `TestKit` at all. Every session test
necessarily used a hand-written mock, and the real backends were only ever exercised standalone.

**(b) A second, unnamed gap sat behind it.** `effect_context_.capabilities` was never assigned
anywhere — every session run carried a null capability set. Harmless while every conformer was a mock
that performs no effects; a hard blocker the moment one is real, because a backend's own
outbound-credential resolution (004 §1 / 018 §4) is a capability-gated effect and would be denied by
construction. Fixing (a) alone would have produced a session that compiles, runs, and fails every
call. This ADR fixes both, because either alone leaves the residual open.

## 2. Options considered

**(a) An upstream Quark `TestKit` change to forward actor constructor arguments.** The shape J1
itself suggested. Rejected as the *first* move: CLAUDE.md fixes Quark as a submodule, never forked or
patched in-tree, so this is an upstream round-trip — and it turns out not to be needed, because the
real constraint is `AgentSession`'s own default-constructibility, not `TestKit`'s API. `TestKit`
already exposes a mutable `actor()`, which is all post-construction configuration requires. Fixing
this downstream also keeps the property true for a real `Engine`, which has the same constraint and
which no `TestKit` change would have helped.

**(b) A `set_chat_client(ChatClientT)` setter.** Does not compile for the types it exists to serve: a
backend holding a reference member is not copy-assignable. It would have worked for exactly the mocks
that never needed it.

**(c) Hold `ChatClientT*` — a non-owning pointer to an externally-owned client.** Workable, but it
changes ownership semantics for every existing conformer, all of which are values today, and pushes a
second lifetime onto every call site to solve a problem only some types have.

**(d) `std::optional<ChatClientT>`, default-engaged where possible, plus in-place emplacement.
— CHOSEN.**

## 3. Decision

`chat_client_` becomes `std::optional<ChatClientT>`, initialised by a helper that engages it when
`ChatClientT` is default-constructible and leaves it empty otherwise. Two consequences, both wanted:

- `AgentSession` is default-constructible whatever `ChatClientT` is, so it works under `TestKit<A>`
  and under a real `Engine`.
- **Every pre-existing conformer is bit-for-bit unaffected.** All of them are default-constructible
  mocks, so their optional is engaged at construction exactly as the value member was. The new
  "disengaged" branch is reachable *only* for a type that could not have been a value member at all.

`emplace_chat_client(Args&&...)` constructs the client in place, after the actor exists. In-place
rather than by assignment for the reason option (b) fails: emplacement requires only that the type be
constructible from `args`, not assignable.

`set_capabilities(CapabilitySet const*)` supplies the session's capability set, which `handle()` now
threads into `EffectContext` alongside `principal_`. Non-owning, matching
`EffectContext::capabilities`' own contract: the granting host owns it and must outlive the session.

Both are configuration-time, like `initialize()`: called before the first `StartRun`, by the host that
also owns the `SecretStore` the client references. Neither is derived from model output or from
anything a turn produced — **a run cannot widen its own authority** (I3).

### Invariants preserved, deliberately

- **I2 (no ambient authority).** `capabilities_` defaults to null, which is exactly the pre-ADR
  behaviour: an unconfigured session reaches no effect. Authority appears only when a host passes one
  in explicitly.
- **Fail-closed.** A run on a session whose non-default-constructible `ChatClientT` was never emplaced
  returns without responding, rather than fabricating an `AgentResponse` for a turn that never reached
  a model — the same shape the budget, context, and chat-failure branches in `handle()` already use.
- **`reset()` leaves both alone.** They are configuration, like `chat_client_`/`history_provider_`
  already were, not per-run state.

### Cost

One `std::optional` engaged-check per turn, on a path that then performs a network round trip. Not
measurable, and not on any hot loop.

## 4. Falsifiable gates

| # | Claim | What would falsify it |
|---|---|---|
| G1 | `AgentSession` is default-constructible even when `ChatClientT` is not. | The `static_assert` pair in `test_agent_session_real_backend.cpp` failing to compile. |
| G2 | A real backend completes a real turn through the real loop. | J1-R3 failing: a `StartRun` ask not resolving with the server's own reply text and parsed `Usage`. |
| G3 | The session's own assembled context reaches the wire. | J1-R4 failing — the server's received body not containing the user turn the session assembled, or not the emplaced client's configured model. |
| G4 | An emplaced client survives across runs. | J1-R6 failing: a second `StartRun` not reaching the socket, or history not holding two full turns. |
| G5 | A never-emplaced client fails closed. | J1-R7 failing: an ask resolving with a fabricated `AgentResponse`. |
| G6 | Existing conformers are unaffected. | Any pre-existing `test_agent_session_*` / `test_m1_walking_skeleton` behaviour changing. |

All six hold on acceptance (18 assertions in `test_agent_session_real_backend.cpp`; full suite green).

The proof drives a canned loopback server over `ProviderTransport::plaintext_http` (ADR-016) rather
than TLS — deliberately, so the file stays about session/backend wiring instead of re-proving the TLS
path `test_openai_chat_client_live.cpp` already covers exhaustively. It is deterministic, so it lives
in the default suite; `test_llamacpp_live_e2e.cpp` covers a real model.

## 5. Consequences

- 001 §3's turn loop and a backend that actually talks to a server are now demonstrated **together**,
  which was the substance of the J1 residual.
- Sessions can now hold real authority. That is a new surface: `set_capabilities` is the one place a
  host grants a session the right to reach effects, so it is the right place to look first when
  auditing what a session may do.
- Option (a)'s upstream `TestKit` change is no longer needed for this. If a future actor genuinely
  requires constructor-argument forwarding (rather than post-construction configuration), that is a
  separate case and should be re-argued on its own merits.
