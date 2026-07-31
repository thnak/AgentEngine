# 020 — Configuration and Hosting

**Status:** Draft · **Depends on:** 002, 008, 015, 018, Quark 013 · **Gate:** §7

## Goal

Draw the line between what an agent *is* (policy, 002) and how a deployment *runs* (configuration),
and define the shapes in which the engine is hosted.

## 1. Policy versus configuration

| | Policy (002) | Configuration (here) |
|---|---|---|
| Examples | tool set, capabilities, sandbox profile, max turns, approval mode, output schema | endpoints, credentials, worker counts, pool sizes, timeouts, log levels, retention, pricing tables |
| Changes | agent behaviour and identity | deployment characteristics |
| Form | CRTP template parameters / declarative `spec` | TOML/env/CLI, hot-reloadable where safe |
| Version | part of the agent version (002 §7) | independent |

**The test for where a knob belongs:** if changing it changes what the agent *would do given the
same input*, it is policy. This is Quark's policy-vs-config boundary (Quark 013) applied one layer
up, and it exists so that "it worked in staging" means something.

**One hard rule:** configuration may **never widen** capabilities, weaken a sandbox profile below
its declared floor, or disable an approval requirement. Those are policy. A deployment that can
turn off the sandbox through an environment variable does not have a sandbox.

## 2. Configuration model

- **Sources, in precedence order:** defaults < file < environment < CLI < programmatic.
- **Scoped overrides** mirroring Quark's resolution: defaults < engine < node < agent-type <
  session. Resolved once at startup into an immutable table; live-reconfigurable knobs are
  explicitly marked (Quark ADR-008's frozen-core/hot-leaf discipline).
- **Every knob declares** its type, default, valid range, reconfiguration class (`BuildOnly` /
  `Live`), and whether it is security-relevant. Security-relevant knobs are `BuildOnly` and logged
  at startup with their effective values.
- **Validation is fail-fast** at startup with precise diagnostics — never a runtime surprise on the
  first request.
- **Secrets are `SecretRef`s** (018 §4), never literals. A literal in a config file is a startup
  error.

## 3. Hosting shapes

| Shape | Description | Use |
|---|---|---|
| **Embedded library** | Link the engine into an application; own the event loop or let the engine own one | Desktop apps, games, existing C++ services |
| **Standalone server** | One process exposing the protocol surfaces (011/012/013) over HTTP | The common deployment |
| **Cluster** | N nodes over Quark's cluster (010/021/026) with sessions placed by HRW/VirtualBins | Scale-out, HA |
| **CLI / one-shot** | Run an agent or workflow to completion and exit | Automation, CI, scripting |
| **Sidecar** | Engine alongside an application, local IPC | Polyglot deployments before bindings exist |

**The same binary serves all five**; the shape is configuration plus which surfaces are enabled.

## 4. Server surfaces

Each is independently enable-able, and each is off unless configured:

`/mcp` (011) · `/a2a` + agent card (012) · `/agui` (013) · OpenAI-compatible `/v1/*` (013 §3) ·
`/health`, `/ready` · `/metrics` (Prometheus text; OTLP via the exporter seam) · admin API
(agent/plugin/policy management, separately authenticated and separately bindable)

**The admin API is never on the same listener as the public surfaces by default.** Making that
mistake once is enough.

## 5. Cluster concerns

- **Session placement** and reactivation come from Quark; AgentEngine chooses the key
  (`session_id`) and the placement policy.
- **Sandbox locality**: a `native-jail`/`microvm` sandbox is node-local; a session that migrates
  loses warm sandboxes, which is a cost, not a correctness issue (010 §3 clean-state contract makes
  it safe).
- **Rolling upgrade**: drain, fenced hand-off, version-skew policy for in-flight runs (019 §4).
- **Capacity**: sandbox pools, provider connection pools, and session density are the three limits
  that matter; each has a budget in 023.

## 6. Operations

- **Startup diagnostics** print the resolved isolation posture per agent, including any profile
  downgrade (008 §3), the enabled surfaces, and protocol revisions. An operator should be able to
  read the first screen of logs and know exactly what security posture is in force.
- **Graceful shutdown**: stop admission, drain in-flight runs to checkpoint boundaries, destroy
  sandboxes, flush audit, exit within a bounded time.
- **Health versus readiness** are distinct: readiness accounts for provider reachability and sandbox
  backend availability, so a node with a broken sandbox backend does not silently take traffic.

## 7. Promotion gate

- **G1** — a security-relevant knob cannot widen authority: an exhaustive test over the config
  surface attempts to reach a wider capability set, weaker profile, or disabled approval, from
  every source, and fails in every case.
- **G2** — the same agent runs unchanged in all five hosting shapes.
- **G3** — invalid configuration fails at startup with a precise diagnostic, never at first request
  (negative corpus per knob class).
- **G4** — graceful shutdown under load: zero lost acknowledged turns, zero orphaned sandboxes,
  audit flushed, within the declared bound.

## 8. Open questions

- **Q1** — Configuration format: TOML (as assumed) versus reusing the declarative YAML shape (015)
  for uniformity.
- **Q2** — Whether the admin API should exist in-process at all, or only as a separate binary.
- **Q3** — A C ABI is the intended seam for future Python/.NET bindings; its shape is unspecified
  and will constrain those bindings once frozen.
- **Q4** — Deployment descriptor for the `remote` sandbox profile: reuse the Kubernetes Agent
  Sandbox CRD directly, or wrap it?
