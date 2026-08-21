# Maintenance-ops AI assistant and live dashboard — case study

A user-supplied application case, worked through against the existing spec to check whether the
locked design already covers it. Not an RFC, not a decision — a trace of which sections answer
which part of the case, and which parts are declared but not yet wired to code. Two sub-cases,
worked through in order.

## Case 1 — AI assistant over an existing maintenance app's MCP server

A maintenance team has its own equipment-maintenance application, which already exposes an MCP
server for query and CRUD (equipment lookup, maintenance history, work orders, status updates).

This maps directly onto the tool/capability/MCP stack as specified — no new mechanism needed:

- Standard MCP client/tool flow (`006`, `011-MCP-Conformance.md`).
- The real design decision is **write-path safety, not connectivity**: reads are free to leave
  agent-autonomous; every CRUD write/delete needs its own declared capability, an `Approval` mode
  (`007-Capability-and-Trust-Model.md §3`) appropriate to blast radius, and an audit record
  attributable to the actual caller (I4) — never inferred from model intent (I2/I3).
- If the app's technical docs (manuals, incident history) aren't exposed over MCP, that's a
  separate RAG/context-provider concern, not something to fold into the same MCP server.

No gap identified here — this is the stack's home turf.

### External validation — IBM's AssetOpsBench

IBM Research publishes **AssetOpsBench** (KDD 2026 Datasets & Benchmarks Track;
[arXiv:2506.03828](https://arxiv.org/abs/2506.03828),
[github.com/IBM/AssetOpsBench](https://github.com/IBM/AssetOpsBench)) — "a unified framework for
developing, orchestrating, and evaluating domain-specific AI agents in industrial asset operations
and maintenance," 141+ reproducible scenarios across 9 asset classes. This is a real, published
precedent for Case 1's shape, worth citing directly rather than treating Case 1 as a green-field
design:

- **Five domain-specific MCP servers, not one generic CRUD server**: IoT (read-only:
  `sites`/`asset_detail`/`installed_sensors`/`latest_reading`/`history`/`sensor_stats`), FMSR
  (failure-mode catalog: `get_failure_modes`/`generate_failure_modes`/`add_failure_modes`), TSFM
  (41 tools — time-series model catalog, feature catalog, and a run/eval ledger:
  `run_recipe`/`evaluate`/`list_runs`), **WO** (work orders, 15 tools: 9 read + 6 write — `generate_
  work_order`/`update_workorder`/`approve_workorder`/`assign_technician`/`close_workorder`/
  `cancel_workorder` — backed by CouchDB with IBM Maximo `mxwo` field names), and **Vibration**
  (`compute_fft_spectrum`/`compute_envelope_spectrum`/`calculate_bearing_frequencies`/
  `diagnose_vibration`).
- **The WO server ships an explicit `AOB_READONLY=1` flag** that exposes only its 9 read tools —
  real-world confirmation that the read/write capability split argued for above isn't
  over-engineering; a production system in this exact domain drew the same line independently.
- **The Vibration server is evidence for a shape beyond plain CRUD**: exposing a compute-heavy
  domain analysis (FFT, envelope spectrum, bearing-frequency diagnosis) as its own MCP tool, rather
  than shipping raw sensor samples for the agent/model to analyze inline. Worth considering if the
  maintenance app's own MCP surface currently only exposes raw readings.
- **Four independently-built agent-framework variants are benchmarked against the same scenarios**:
  Plan-Execute (sequential), Deep Agent (planning + sub-agents + a virtual filesystem for
  long-horizon tasks — notably close to this project's own worktree/workflow shape, `014`/`025`),
  and ReAct-based Claude/OpenAI orchestrators using agent-as-tool delegation. External cross-check
  that a typed workflow graph is one of several viable orchestration shapes for this domain, not
  the only one an outside team reached for.
- **A university extension project independently converged on the Skill-reuse design discussed
  earlier in this project's own research** (`docs/research/2026-08-20-emerging-agent-application-
  cases.md` §9): "Skills and Knowledge Plugin MCP Servers for Optimized Industrial O&M Agents"
  exposes reusable multi-step operational workflows via an MCP Skills Server plus a Knowledge
  Plugin Server for context-specific docs, specifically to reduce planning overhead and redundant
  tool calls.

**Caveat:** AssetOpsBench is a research benchmark for evaluating agent frameworks, not a production
system to depend on — cited here as domain evidence for tool-surface shape and design choices, not
as infrastructure to integrate.

## Case 2/3 — live dashboard fed by background aggregation over MCP tools, plus periodic export

The harder case: dashboard/report data continuously refreshed by pulling several MCP tools,
aggregating, and rendering — without forcing the agent to sit in a poll loop, and with the
aggregation *logic itself* often defined ad hoc by a technician's request (so it can't be
hand-written in advance).

### Why plain MCP doesn't fit the shape

MCP is a pull/request-response protocol, driven by an agent turn. The 2026-07-28 spec revision
removed `resources/subscribe`/`unsubscribe` entirely, replacing it with `subscriptions/listen` —
but that only covers `*ListChanged` notification types (`011-MCP-Conformance.md §3.3`), not
per-value change push. So a numeric reading or equipment status from a maintenance app is, in
practice, always poll-based — there is no MCP shortcut being missed here.

### The mechanism that fits: StandingEffect, not a bespoke poll loop

`006-Tool-and-Function-Plane.md §6b` gives three agent-callable entry points onto the run's
existing `Suspended`-run machinery:

- **`watch_resource(ResourceRef, WakeCondition)`** — over a source with no native push (the normal
  case above), this is explicitly a **reminder-driven poll under the hood**, not a live
  subscription.
- **`schedule_wakeup(WakeCondition)`** — a durable future wake, for the aggregation cadence.
- **`background_task(tool, args)`** on a `Backgroundable`-declared tool — detaches the *model's
  wait* from the turn, not a second executor inside the session (I1 still holds).

All three return a **`StandingEffect`** handle. The property that matters for cost: a run that
calls one of these and ends its turn is fully `Suspended` — no activation, sandbox, connection, or
thread held while waiting (gate G6, measured by 019 §7 G3's census check). Between polls, the
agent costs nothing. This directly answers the original worry — *"MCP không đáp ứng đủ nhanh theo
mức mà AI và user dùng"* — by never putting the model in that loop to begin with.

A dedicated aggregation run's own event stream projects onto AG-UI (`013-UI-and-Streaming-
Surfaces.md §1`) exactly like any other run's does. A UI subscribing to that run's stream *is* the
live-updating dashboard — no separate push-notification layer needs to be built on top.

### Fetch + aggregate: Map-reduce workflow, `function` executor kind

`014-Workflow-and-Orchestration.md §3`'s Map-reduce pattern (fan-out over a collection + fan-in
reducer) is exactly "call N MCP tools, then aggregate." The reducer can be an
`executor_kind::function` node — **implemented, not just specified**: `graph.hpp:91,437` — Milestone
6 Phase B built `function` and `request_port`; only `agent`/`sub_workflow` kinds are currently
rejected by `check_workflow_executable` (`graph.hpp:441-453`).

A `function` executor's body is `ExecutorBody` — a plain
`std::function<result<ExecutorOutcome>(Message const&, EffectContext&)>`
(`include/agentengine/rt/workflow_supervisor.hpp:196`). It receives the fan-in `Message` (the
already-fetched JSON from the upstream fetch executors) directly — no re-fetching needed inside the
reducer.

### The part a hand-written `ExecutorBody` can't cover: ad hoc aggregation logic

The aggregation query a technician wants ("group failures by zone this month", etc.) isn't knowable
at workflow-authoring time — it's decided by the LLM, per request. A fixed C++ `ExecutorBody`
can't contain that.

Resolution: the `ExecutorBody` doesn't need to *contain* the logic — it needs to be a thin, fixed
wrapper that calls the built-in code interpreter tool, `execute_code(code, data)`
(`010-Python-Code-Interpreter.md §1`, Interpreter mode), where `data` is the fan-in message and
`code` is a Python snippet the LLM writes **once**, when the technician defines the query. Every
subsequent `StandingEffect` wake reruns the *same* code against freshly fetched data — no further
LLM call per cycle.

This is consistent with I3 ("model output is data, never authority"): the generated code never
becomes trusted — every invocation, the first or the five-hundredth, goes through the same sandbox
(`008-Sandbox-and-Isolation.md`) and the same capability gate as any other tool call. Reuse doesn't
elevate its trust level.

**Caveat, not automatic:** because this then runs unattended on a schedule, a human should approve
the query once before it's registered as a recurring `StandingEffect` — the spec doesn't force this
gate itself; it's a deployment choice (`Approval` mode, `007 §3`, applied to the
`schedule_wakeup`/`background_task` capability).

### Reuse across sessions: Skills — with a subtlety worth being precise about

The natural next step — let the LLM package the aggregation as a **Skill** (`SKILL.md` +
`scripts/`/`references/`, `009-Plugin-and-Extension-System.md §8`) so a later session doesn't
re-derive the query from scratch.

Checked against the actual loader, not just the RFC prose: `DiskSkillSource::load_skills()`
(`include/agentengine/core/skill_source.hpp:143-192`) walks `scripts/`, `references/`, and
`assets/` **uniformly** (`collect_files`, same call for all three, lines 183-187) — there is no
differential trust check at the loading layer between a bundled script and a bundled reference
file. `026-Agent-Facing-Runtime-Surface.md §6` confirms why that's fine: a bundled script "runs as
an ordinary program" through the same sandboxed interpreter (010) as any other agent-run code,
using whatever capabilities the session already holds — no elevation.

So `009 §8c`'s "code ships → packaged as an `ae:skill` plugin → signed, capability-declaring,
operator-approved, digest-pinned" pipeline applies to a skill whose script becomes its **own
declared, capability-bearing tool** — not to a script an agent simply reads and runs through
`execute_code` like any other file. For this case, the latter is what's wanted, and it needs no
operator sign-off ceremony to become reusable.

**Where the real gate lives:** skills are mounted **read-only** at `/skills/<name>` (`009 §8b`,
`025-Worktree-and-Virtual-Filesystem.md §3`) — an agent cannot write into its own mounted view to
plant a new skill. Creating a new skill on disk requires a distinct, explicitly granted write
capability (e.g. `FsWrite` scoped to the skill source root, or a dedicated `create_skill` tool) —
and *that* capability is where an operator attaches `Approval` mode if human review before reuse is
wanted. I2 holds without the loader itself needing an opinion about script vs. reference content.

## Summary table

| Need | Mechanism | Status |
|---|---|---|
| Agent-driven CRUD over existing MCP server | `006`/`011`/`007` capability+approval | Implemented, no gap |
| Background polling without blocking/costing the agent | `StandingEffect` (`watch_resource`/`schedule_wakeup`/`background_task`, `006 §6b`) | Implemented (gate G6) |
| Live dashboard feed | Run event stream → AG-UI projection (`013 §1`) | Implemented |
| Fetch N tools + aggregate | Map-reduce workflow, `function` executor (`014 §3`) | `function` kind implemented (M6 Phase B); `agent`/`sub_workflow` kinds not yet built |
| Ad hoc, technician-defined aggregation logic | LLM writes code once → `execute_code` reruns it per cycle (`010 §1`) | Implemented; needs an explicit human-approval step to be added at deployment, not provided automatically |
| Reuse aggregation logic across sessions | Skill with `scripts/`/`references/`, disk-loaded (`009 §8`, `skill_source.hpp`) | Implemented; no signing pipeline required for this usage; review gate belongs on the *skill-creation write capability*, not the loader |
| Periodic PDF/Excel export | Ordinary external-effect tool (`006 §6b`'s own framing) | No dedicated mechanism named in spec — author as a plain tool |

## Open item

`FunctionExecutor` fleet wiring for `agent`/`sub_workflow` executor kinds is not built yet
(`check_workflow_executable` rejects them) — irrelevant to this case study since it only needs
`function` and `request_port`, both implemented, but worth knowing if a later case study needs an
`agent`-kind workflow node.
