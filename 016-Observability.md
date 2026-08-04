# 016 — Observability

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 001, 007, 013 · **Gate:** §7 · **Research:** [2026 landscape §4](docs/research/2026-standards-landscape.md)

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
| `chat` | one model call | ChatClient, model, options, usage, finish reason, applied capability fallbacks (004 §2) |
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
- **Cost**: tokens and estimated cost by `{ChatClientId, model, agent+version, principal}`.
- **Reliability**: failures by classification (001 §6), retries, ChatClient breaker state, tool
  error rate, structured-output repair rate (a rising repair rate is a real signal, 003 §4).
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
same mode** by default, because a replay corpus is a prompt archive by another name — **except**
recordings taken specifically to serve as replay or golden-trace fixtures (022 §3), which require
`Capture<WithContent>` at capture time regardless of the deployment's production default: 022's
replay/golden-trace guarantee is to reproduce exact behavior from recorded nondeterminism, and
content a `MetadataOnly` capture never recorded cannot be replayed back; encrypted reasoning is
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
  effect count. Deny/approve decisions are attributed via telemetry, not audit (017 §8 G5): a
  denial produces no effect, so it is out of scope for this count by design, not a gap in it.
- **G5** — observability overhead is within the 023 budget with tracing enabled at 100 %.

## 8. Open questions

- ~~**Q1** — Convention churn: how many versions do we support simultaneously, and does the mapping
  layer emit both during a transition?~~ **Resolved, one at a time, no dual-emit (2026-08-04):**
  unlike MCP/A2A this isn't a wire protocol with an independent peer forcing multi-version tolerance
  — it's our own output format into infrastructure the operator controls, so there's no external
  un-upgradeable party creating pressure to emit two shapes simultaneously. §1's mapping layer
  already is the mechanism: a version bump is a deliberate, single-file, single-commit change gated
  on the compatibility test it already describes, the same pin-and-bump-deliberately discipline
  applied to Wasmtime (009 §11 Q4) and the interpreter image (010 §10 Q1). An operator on an older
  collector pins their engine version until they're ready, same as any other breaking change behind
  a version bump (024 §2).
- ~~**Q2** — Whether cost estimation belongs in telemetry at all, given pricing tables are
  configuration (004 §5) and will be wrong sometimes.~~ **Resolved, Yes — already what §3 does;
  scoped as an explicit estimate (2026-08-04):** §3 already lists cost as a metric, so the open
  question was really "how do we keep a possibly-stale number honest," not "whether." The naming
  already carries the answer: it's `estimated_cost`/`cost_estimate` (003 §6, 004 §5), never bare
  "cost" — nobody downstream should read it as billing-grade, and the name says so without needing a
  disclaimer mechanism. §7's gate for this metric is arithmetic correctness against whatever pricing
  config is actually loaded, never real-world accuracy, which the engine has no way to guarantee
  since the table is operator-owned configuration. Keeping that table current is the operator's
  maintenance responsibility, the same class of ongoing-freshness obligation as an interpreter
  image's CVE cadence (010 §10 Q1) — not a new mechanism to design here.
- ~~**Q3** — Evaluation and quality signals (022) as telemetry versus a separate pipeline.~~
  **Resolved, split (2026-08-04):** the underlying run data is ordinary telemetry — 022 §4 already
  states evaluation runs are "recorded, traced, auditable" exactly like production runs, so no new
  mechanism is needed for that half. The **scoring** layer (grader verdicts, cross-run aggregate
  statistics, confidence intervals, pre-registered thresholds, 022 §4) is a separate pipeline, because
  it's a genuinely different kind of artifact — not a per-run operational signal but a statistical
  aggregate over a whole suite, produced by a (possibly model-based, versioned) grader, shaped nothing
  like a metric dimension §3's cardinality discipline bounds for. This is §5's own audit-versus-
  telemetry principle applied a third time, not a new one: different guarantees justify separate
  streams, and "was it good, statistically, against a pre-registered bar" is a third, distinct
  purpose from either operating/debugging or proving what happened — fed *from* telemetry/audit-
  recorded runs as raw material, living in neither stream itself.
