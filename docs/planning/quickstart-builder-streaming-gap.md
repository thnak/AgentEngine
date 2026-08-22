# Quickstart session builder has no streaming path — and neither does a gateway-typed `AgentSession`

**Status:** Gap finding from live conversation (2026-08-22), **not a design draft, not red-teamed, no
code written**. Tracking issue only — deferred deliberately (project owner's own call: streaming has
real work pending at other layers too — see "Related, unresolved gaps" below — and should land as one
coordinated pass, not bolted onto the quickstart builder in isolation). Companion docs: `quickstart-
session-builder-design-draft.md` §6 (first named `.ask_stream()` as an open question, not designed),
`model-call-gateway-routing-design-draft.md` (Finding 2 — the actual fix this gap is waiting on),
`tool-call-argument-streaming-gap.md`/`tool-call-result-streaming-gap.md` (the other, independent
streaming gaps named the same day, before tool invocation and after it respectively).

## The gap, precisely — not "loses retry", structurally cannot stream at all

`include/agentengine/rt/agent_session.hpp` (`start_run()`, around the `run_started` event):

```cpp
if constexpr (agentengine::ModelCallGatewayLike<ChatClientT>) {
    emit_run_event(run_event_kind::warning, run_event_payload::Warning{
        "this run routes model calls through a ModelCallGateway (ADR-036): no live model_delta "
        "events fire for a gateway-routed round, ..."});
} else if (stream_model_calls_) {
    emit_run_event(run_event_kind::warning, run_event_payload::Warning{
        "this run streams each model call (ADR-034): failover/circuit-breaker-feedback do not "
        "apply on the streaming path, ..."});
}
```

Two DIFFERENT, mutually exclusive situations, easy to conflate (a live conversation's own first pass at
explaining this did):

- **`ChatClientT` is a gateway type** (`ModelCallGatewayLike` — `ModelCallGateway<...>`,
  `MiddlewareModelCallGateway<...>`, `ContentReplayGateway<...>`): the `if constexpr` branch is taken
  UNCONDITIONALLY, regardless of `stream_model_calls_`. **No `model_delta` event ever fires for this
  session, full stop** — not "streams without retry," genuinely no streaming path exists for this
  `ChatClientT` shape today.
- **`ChatClientT` is a plain, non-gateway `ChatClient`** with `stream_model_calls_` set: streaming DOES
  work, but any retry/failover/circuit-breaker logic the bound client might otherwise have had (there
  usually isn't any on a raw `OpenAIChatClient`/`AnthropicChatClient` — that's exactly what wrapping in
  `ModelCallGateway` adds) does not apply on that path.

`quickstart-session-builder-design-draft.md` §2a's default (`ModelCallGateway<Primary>`, "one rung
above bare") means `QuickstartSessionBuilder`'s `Bundle` is ALWAYS in the first case — `.ask_stream()`
cannot be added on top of the current default `ChatClientT` at all, not even a degraded version.

## Related, unresolved gaps this should NOT be fixed in isolation from

Named explicitly per the project owner's own reasoning for deferring this: streaming has real,
independent problems at other layers of the pipeline, so a builder-level `.ask_stream()` landing before
those are addressed would be presenting a nicer front door onto a still-broken hallway.

- **`model-call-gateway-routing-design-draft.md` Finding 2** — the actual mechanism that would let a
  gateway-typed session stream WITHOUT losing retry/failover: an additive `call_stream()` entry point on
  `ModelCallGateway`, commit-gated (retry freely before the first chunk reaches the caller; terminal,
  no retry, once it has). Design-only, not implemented — its own closing section states it needs the
  full `design → red-team → prove → judge` cycle before landing, since it touches I5 replay (the
  *attempt sequence*, not just the final response, must become part of the recorded seam) and 004 §5
  cost accounting (a retried, discarded streaming attempt is real, billable, currently-unattributed
  cost). This is the real fix the quickstart builder's `.ask_stream()` is blocked on.
- **`tool-call-argument-streaming-gap.md`** — RFC 013 already names live argument-streaming
  (`ModelDelta` before a tool call resolves) as intended, but nothing implements it yet.
- **`tool-call-result-streaming-gap.md`** — tool-call result content never reaches the live event
  stream at all today (a separate, independent gap from argument-streaming).

## What this doc is not

Not a design. Not scoped work. Exists so `.ask_stream()` isn't silently forgotten or re-discovered from
scratch in a later session — when streaming DOES get picked up, it should be planned as one pass across
all three linked gaps (model-call-gateway routing's Finding 2, tool-call argument streaming, tool-call
result streaming) plus this builder's own `.ask_stream()`, not four separate, uncoordinated patches.
