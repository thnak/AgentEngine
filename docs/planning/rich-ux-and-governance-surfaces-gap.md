# Rich UX and governance surfaces — what MAF's Harness/Hooks map onto, and what's actually missing

**Status:** Research + gap survey from live conversation (2026-08-21), **not a design draft, not
red-teamed, no code written** — one level less mature than `dynamic-multi-agent-fanout-design-draft.md`.
Written from an explicit user framing: "we are an Engine and consumer application will build
application on top of us so we have to provide things to help build rich UX application, not only just
agent event turns" — asking specifically about MAF's Harness Agent, Agent Hooks, and a general
"UX update system." Companion: `docs/research/2026-08-21-maf-background-execution-concepts.md` (the
prior pass that researched MAF's background/hooks/planning docs directly).

## What already exists — this is not starting from zero

**RFC 013 (UI and Streaming Surfaces), Reviewed status, substantially real.** One internal `RunEvent`
stream (`run_event.hpp`, 24 real event kinds: run/turn/model/tool/sandbox-exec/state/artifact/
input/auth/approval/codeact-ask/warning/policy) projected onto four external surfaces — AG-UI, A2A
streaming, MCP progress, OpenAI-compatible SSE — "adding a surface is writing a projection, not a
second event model" (013 §Goal). AG-UI's own type vocabulary (`protocol/agui/types.hpp`) already
includes `StateSnapshot`/`StateDelta`, `ActivitySnapshot`/`ActivityDelta`, streamed reasoning, streamed
tool calls, `RawEvent`/`CustomEvent` escape hatches — genuinely rich wire shapes, not just text deltas.

**A `ContextProvider` abstraction already exists**, the direct analog of MAF's `AIContextProvider`
(confirmed: `include/agentengine/core/context_provider.hpp`, `composed_context_provider.hpp`), with
real built-ins already shipped: `vector_rag_context_provider.hpp`, `skill_provider.hpp`,
`memory_provider.hpp`, `history_provider.hpp`. This is the extension point any new provider (see below)
would plug into — not new infrastructure, an established pattern.

**A narrower hook already exists.** `TurnMiddlewareHook` (`core/turn_middleware.hpp:271`) —
`std::function<task<result<std::monostate>>(TurnContext&)>`, one lifecycle point (shapes what a turn's
model call sees, ADR-067), opt-in per session via `set_turn_middleware_hook`/`turn_middleware_hook`
(`rt/agent_session.hpp:631,634`).

**RFC 017 (Safety and Content Governance), Reviewed status.** Prompt-injection defense, defense layers,
structural separation, filters, content governance, named attack classes, an operator surface. The
closest existing analog to MAF's Agent Hooks, though narrower-scoped (see below).

## The three MAF concepts, mapped

### 1. "Harness" (MAF's `HarnessAgent`)

MAF bundles `TodoProvider` + `AgentModeProvider` (plan/execute mode) + memory + approval + observability
into one preconfigured default (`docs/research/2026-08-21-maf-background-execution-concepts.md` §4 has
the todo-tool detail: `todos_add/complete/remove/get_remaining/get_all`, `mode_get/mode_set`).
AgentEngine has the individual pieces (`ContextProvider` composition, ADR-029 approval/interaction, RFC
016 observability) but:

- **No bundled "batteries-included" preset exists.** A consumer app builder has to compose the pieces
  themselves; nothing gives them a one-line "give me the default pipeline" the way `chatClient.
  AsHarnessAgent(options)` does. An ergonomics/DX gap, not a missing primitive.
- **No Todo/plan-execute-mode provider exists at all** (confirmed: no `todo*` file anywhere in the
  tree). This is the more interesting gap for UX specifically — a todo list is a concrete, renderable
  thing a consumer app can show a user (Claude Code's own TodoWrite tool is the reference point: users
  visibly track an agent's progress through a checklist). AG-UI already has the WIRE shape for this
  (`StateSnapshot`/`ActivitySnapshot`), nothing populates it with a todo-list-shaped payload today.

### 2. "Hooks" (MAF's Agent Hooks)

MAF's Agent Hooks is a generic, pluggable, 8-point lifecycle interception contract (`agent_startup →
input → pre_model_call → post_model_call → pre_tool_call → post_tool_call → output → agent_shutdown`,
`allow`/`deny`/`transform` verdicts, fail-closed, buffered streaming, Python-only and experimental —
`docs/research/2026-08-21-maf-background-execution-concepts.md` §3) for THIRD-PARTY policy engines to
target.

AgentEngine's `TurnMiddlewareHook` is one point, not eight, and RFC 017 is scoped to prompt-injection/
content-governance specifically, not a general pluggable multi-point contract a consumer's own
compliance/redaction logic could hook into. Worth naming precisely: AgentEngine's I2/I3 enforcement
(capability gating, structural, not opt-in) is a STRONGER default posture than MAF's own docs admit
theirs is — MAF's own page warns "don't describe an `evaluate_only` deployment as enforced governance."
So this gap is narrower than it sounds: it's "no pluggable hook surface for a *consumer's own* policy
logic to observe/transform/veto at each lifecycle point," not "ungoverned by default."

### 3. "UX update system" (no direct MAF analog — a general framing)

Already substantially covered by RFC 013/AG-UI (see above). The one concretely-verified real gap,
found while answering a follow-up question in this same conversation (see
`docs/planning/tool-call-content-streaming-gap.md` for the full writeup): tool-call CONTENT — both the
result after invocation and the arguments while still being generated — doesn't reach the live event
stream today, only `{call_id, ok}` on finish and `{call_id, tool_name}` on start.

## Recommended priority (not decided, a recommendation only)

The todo/plan-state provider is the one with the clearest, concrete UX payoff and the smallest
footprint — it's an ordinary `ContextProvider`, a pattern that already exists, not a new subsystem, and
AG-UI already has the wire shape waiting for it. The generic Agent-Hooks-style pluggable contract is the
biggest and riskiest of the three (a new pluggable third-party policy surface, directly I2/I3-adjacent —
any hook a consumer can register to "transform" or "veto" model/tool traffic needs the same
unforgeable-by-construction scrutiny this project applies everywhere else). The bundled Harness preset
is closer to pure ergonomics once the todo provider exists — likely the easiest of the three, but only
valuable after there's more than one piece worth bundling.

## Open questions (not designed here — scoping only)

1. Does a `TodoProvider` analog need its own `StandingEffect` kind, or does it ride `state_changed` the
   same way `StandingEffect` visibility already does (013's own "not a new event pair" rule, cited in
   `run_event.hpp:21-23`)? Precedent points toward reusing `state_changed`, not inventing a new kind —
   not confirmed against a real design pass.
2. Where does a pluggable Agent-Hooks-style contract's own capability/authority come from? A consumer's
   registered interceptor sits at a position analogous to a `ChatClient`/tool boundary in terms of what
   it can observe or veto — needs the same "who grants this, checked how" design pass
   `agent-as-workflow-executor-design-draft.md` already had to do for capability sourcing, not assumed
   safe by virtue of being "just a hook."
3. Does a bundled Harness-style preset risk baking in an implicit capability ceiling wider than a
   consumer actually wants (I2)? A preset that silently grants "memory + approval + observability +
   todo" as one bundle needs to still compose with explicit, per-piece capability declarations, not
   become an ambient-authority shortcut for convenience.

None of these three topics has had a red-team pass yet, unlike the dynamic-fan-out work — this doc
records what was found and discussed, not a committed design.
