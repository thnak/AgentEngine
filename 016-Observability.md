# 016 — Observability

**Status:** Draft · **Depends on:** 001, 007, 013 · **Gate:** §7 · **Research:** [2026 landscape §4](docs/research/2026-standards-landscape.md)

## Goal

Make every run explicable: what the agent did, why, at what cost, and where it went wrong —
conforming to the **OpenTelemetry GenAI semantic conventions** while insulating the codebase from
their churn, and keeping prompt content private by default.

## 1. Standards posture

As of July 2026 **nothing in the GenAI conventions is marked Stable**; they remain Development,
have no 1.0, and moved into their own repository (`open-telemetry/semantic-conventions-genai`) after
the core repo deprecated and removed all `gen_ai.*` content in v1.42.0 (2026-06-12).

**Therefore:**

- We **conform** to the current conventions — spans `invoke_agent`, `chat`, `execute_tool`;
  attributes `gen_ai.request.model`, `gen_ai.usage.input_tokens`, and the rest of the current set.
- We **isolate** them behind a single mapping layer (`observability/semconv_<version>.hpp`) with the
  convention version pinned as a build-visible constant. When the conventions change — and they
  will — one file changes and a compatibility test tells us what moved.
- We **never invent** an attribute in the `gen_ai.*` namespace. Engine-specific attributes live
  under `agentengine.*`.

## 2. Traces

| Span | Emitted for | Key attributes |
|---|---|---|
| `invoke_agent` | one run | agent id + version + metadata digest, session, principal, outcome, turns, usage |
| `chat` | one model call | provider, model, options, usage, finish reason, applied capability fallbacks (004 §2) |
| `execute_tool` | one tool call | tool name, source, capabilities used, approval decision + rule id, outcome |
| `agentengine.sandbox_exec` | one sandbox execution | profile, backend, cold/warm, limits, resource peaks, outcome |
| `agentengine.plugin_call` | one plugin invocation | plugin id + version + digest, world, fuel/memory |
| `agentengine.workflow_step` | one superstep | workflow id + version, round, executors run |
| `agentengine.context_assembly` | context build | contributor budgets, drops, final token count |

**Structure:** one trace per run; sub-agents and remote calls are child spans linked by standard
context propagation — including into MCP via the `_meta` keys the 2026-07-28 revision documents
(`traceparent`, `tracestate`, `baggage`), and into A2A via its transport headers. A cross-process
agent call must produce **one** connected trace, not two orphans.

## 3. Metrics

Beyond the convention's client metrics:

- **Latency**: turn, run, time-to-first-token, tool duration, sandbox create/exec, context assembly.
- **Cost**: tokens and estimated cost by `{provider, model, agent+version, principal}`.
- **Reliability**: failures by classification (001 §6), retries, provider breaker state, tool error
  rate, structured-output repair rate (a rising repair rate is a real signal, 003 §4).
- **Safety**: approvals requested/granted/denied by rule, policy denials, filter triggers,
  sandbox containment events, capability-check failures.
- **Isolation posture**: executions by profile, and **profile downgrades** — a downgrade must be
  visible on a dashboard, not buried in a startup log (008 §3).
- **Saturation**: sessions active/passivated, queue depths, sandbox pool utilization.

Cardinality is bounded deliberately: principal ids and session ids are **not** metric dimensions
(they are span/audit fields). Quark's cardinality discipline (ADR-022) applies.

## 4. Content capture and privacy

Prompts and completions are the most useful and most sensitive telemetry there is.

| Mode | Emits |
|---|---|
| `Off` | No content anywhere |
| `MetadataOnly` (**default**) | Counts, sizes, hashes, part kinds — no content |
| `Hashed` | Salted digests, enabling equality/dedup analysis without disclosure |
| `WithContent` | Full content, gated by explicit configuration and recorded in the audit log |

Rules: **secrets never appear in any mode** (018); the mode is per agent (`Telemetry<Capture>`,
002) with a deployment-wide ceiling that an agent cannot exceed; **recordings (001 §7) inherit the
same mode**, because a replay corpus is a prompt archive by another name; encrypted reasoning is
never captured in plaintext (003 §1).

## 5. Audit versus telemetry

Two separate streams with different guarantees, deliberately not merged:

| | Telemetry | Audit (007 §8) |
|---|---|---|
| Purpose | Operate and debug | Prove what happened |
| Loss | Sampling and drops acceptable | **No loss** |
| Sampling | Yes | Never |
| Content | Policy-gated | Digests and metadata only |
| Integrity | None | Append-only, hash-chained |

Dropping metrics under load must never drop audit records. If they shared a pipeline, load would
silently erase the security record.

## 6. Debugging surfaces

- **Run inspector**: the event stream (013) with spans, decisions, and context assembly per turn —
  answering "why did it call that tool with those arguments".
- **Replay** (001 §7, 022): re-run offline from a recording.
- **Time-travel** (014 §5) for workflows.
- **Prompt diff**: what changed in the assembled context between two turns or two versions — usually
  the fastest route to "why did behaviour change".

## 7. Promotion gate

- **G1** — emitted telemetry validates against the pinned GenAI convention version, checked by a
  schema test, with the version constant asserted.
- **G2** — a cross-process run (agent → MCP tool → sub-agent → A2A peer) produces **one** connected
  trace with correct parent/child links.
- **G3** — under `MetadataOnly`, a scan of all emitted telemetry, logs, and recordings finds zero
  prompt/completion content and zero secret material, over a corpus seeded with canary strings.
- **G4** — under injected backpressure that drops telemetry, audit record count is exactly the
  effect count.
- **G5** — observability overhead is within the 023 budget with tracing enabled at 100 %.

## 8. Open questions

- **Q1** — Convention churn: how many versions do we support simultaneously, and does the mapping
  layer emit both during a transition?
- **Q2** — Whether cost estimation belongs in telemetry at all, given pricing tables are
  configuration (004 §5) and will be wrong sometimes.
- **Q3** — Evaluation and quality signals (022) as telemetry versus a separate pipeline.
