# 015 — Declarative Agent and Workflow Format

**Status:** Draft · **Depends on:** 002, 006, 014 · **Gate:** §7

## Goal

Define agents and workflows in YAML/JSON that compile to **the same metadata as the C++ form**
(**I6**), so an agent can ship, version, review, and deploy without a compiler — while remaining a
first-class citizen rather than a limited subset.

## 1. The equivalence rule

I6 is the whole point of this RFC, and it is enforced mechanically:

- Both forms produce the **same metadata table**, validated by the **same validator** (002 §6).
- A feature reachable in only one form is a **defect**, tracked as such.
- The gate is a byte-identical-metadata test over a corpus covering every policy (§7 G1).

Where the forms genuinely differ: the C++ form can supply **native handlers and native tools**; the
declarative form references tools by name from the registry (native, plugin, MCP, A2A). That is a
difference in *what can be defined*, not in what can be *expressed about an agent*.

## 2. Agent document

```yaml
apiVersion: agentengine.dev/v1
kind: Agent
metadata:
  id: researcher
  version: 1.4.0
  description: Researches a question and cites sources.
spec:
  provider:
    model: anthropic:claude-opus-5
    options: { temperature: 0.2 }
  instructions: |
    Research the question. Cite sources.
  tools:
    - web_search
    - code_interpreter
    - handoff: writer
  capabilities:
    net_out: ["api.search.example"]
  sandbox:
    profile: strict
    fallback: [microvm, native-jail]
  limits:
    max_turns: 12
    token_budget: 200000
  approval: policy_driven
  memory:
    - kind: semantic
      store: project-docs
  middleware: [redact_pii]
  telemetry: { capture: metadata_only }
  output_schema: { $ref: "./schemas/research-result.json" }
```

## 3. Workflow document

```yaml
apiVersion: agentengine.dev/v1
kind: Workflow
metadata: { id: research-and-write, version: 0.3.0 }
spec:
  start: planner
  executors:
    - { id: planner,  agent: researcher }
    - { id: search,   agent: researcher, concurrency: 4 }
    - { id: review,   kind: request_port, prompt: "Approve the outline?" }
    - { id: writer,   agent: writer }
  edges:
    - { from: planner, fan_out_to: [search] }
    - { from: search,  fan_in_to: review }
    - { from: review,  to: writer }
  limits: { max_rounds: 20, deadline: 15m }
  output_from: writer
```

## 4. Schema and validation

- **JSON Schema 2020-12** for both documents, published in [`schema/`](schema/) and versioned with
  `apiVersion`. Same dialect as MCP tool schemas (011), so one validator serves the whole project.
- Validation is **strict**: unknown fields are errors, not ignored. A typo'd policy silently
  ignored is how a production agent runs without the sandbox its author thought they configured.
- Diagnostics carry document path, line, and column. A format that is unpleasant to debug will be
  abandoned for the C++ form, which defeats its purpose.

## 5. Composition and reuse

- **`$ref`** into local files and registry-resolved documents for schemas, instruction fragments,
  and shared policy blocks.
- **Overlays** for environment differences (dev/stage/prod) that may override *configuration*
  (endpoints, limits, telemetry) but **never capabilities or sandbox profile** — an overlay that
  can widen authority is a privilege-escalation path through a config file.
- **Variables** are resolved from configuration (020) and secrets from the secret seam (018).
  A secret literal in a document is a load-time error, not a warning.

## 6. Lifecycle

Documents are **content-addressed and versioned**: the digest goes into every run's trace, so
behaviour is attributable to an exact document. Hot reload is supported with the same snapshot rule
as everything else — a reload affects subsequent runs, never in-flight ones (006 §6).

Publishing an agent document generates its A2A Agent Card (012) and MCP tool listing (011) from the
same metadata, so the three cannot drift.

## 7. Promotion gate

- **G1 (equivalence)** — for a corpus covering every policy in 002 §3, the YAML agent and the
  hand-written C++ agent produce **byte-identical metadata**.
- **G2 (strictness)** — a negative corpus (unknown field, wrong type, missing required, secret
  literal, capability-widening overlay, cyclic `$ref`) is rejected with precise diagnostics.
- **G3** — a declarative workflow and its C++ equivalent produce identical execution traces for a
  scripted run.
- **G4** — a document's digest appears in every run trace and its Agent Card / MCP listing are
  regenerated deterministically from it.

## 8. Open questions

- **Q1** — Whether to align `apiVersion`/`kind`/`metadata`/`spec` with Kubernetes conventions
  (as written here) or adopt a lighter shape. The K8s shape buys familiarity, tooling, and a natural
  path to a `remote` deployment CRD (008), at the cost of verbosity.
- **Q2** — Inline code in documents: powerful (a small Python transform as a node) and a direct
  route to "config that is actually untrusted code". Currently prohibited; the sandbox makes it
  *possible* to allow, which is not the same as advisable.
- **Q3** — Whether declarative documents should be signable like plugins (009), so a deployed agent
  definition carries provenance.
- **Q4** — Interop with MAF's declarative agent YAML and with `SKILL.md`-style packaging: importer
  or native support? (Pending the RFC 011 skills research.)
