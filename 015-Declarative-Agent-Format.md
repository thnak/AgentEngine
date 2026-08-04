# 015 — Declarative Agent and Workflow Format

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 002, 006, 014 · **Gate:** §7

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
    fallback: [native-jail, remote]
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

- ~~**Q1** — Whether to align `apiVersion`/`kind`/`metadata`/`spec` with Kubernetes conventions
  (as written here) or adopt a lighter shape. The K8s shape buys familiarity, tooling, and a natural
  path to a `remote` deployment CRD (008), at the cost of verbosity.~~ **Resolved, K8s shape,
  confirming what §2/§3's own examples already committed to (2026-08-04):** the concrete reason it's
  right for this project, not just convention-following — 008 §3's `remote` profile targets the
  Kubernetes Agent Sandbox CRD directly, and 020 §8 Q4 is already weighing whether a future `remote`
  deployment descriptor reuses that CRD's own shape. Anyone deploying to `remote` is near-certainly
  K8s-fluent already; the accepted cost is verbosity, not unfamiliarity.
- ~~**Q2** — Inline code in documents: powerful (a small Python transform as a node) and a direct
  route to "config that is actually untrusted code". Currently prohibited; the sandbox makes it
  *possible* to allow, which is not the same as advisable.~~ **Resolved, No, stays prohibited
  (2026-08-04):** the motivating case is already served without it — "a small Python transform as a
  node" is exactly 014 §1's `function` executor kind, referenced by name from the registry the same
  way §1 already references tools by name, going through 009's ordinary review/signing/capability-
  declaration pipeline before it runs. Allowing inline code would let a document — reviewed under
  configuration-shaped scrutiny, which the "sandbox makes it possible" framing already flags as the
  wrong bar — carry unsigned, unpinned T3-tier code with none of that pipeline's checks. If a genuine
  need for truly ad hoc, author-here-run-immediately transform code ever appears, that's new,
  narrowly-scoped design surface for an explicitly-code document type, not a reason to weaken this
  format's config/code boundary.
- ~~**Q3** — Whether declarative documents should be signable like plugins (009), so a deployed agent
  definition carries provenance.~~ **Resolved, Yes, the same trust model as plugins (2026-08-04):**
  hot-reloadable documents (§6) share plugins' deployment shape — loaded and updated independently of
  a full engine rebuild — not compiled C++'s, where the build pipeline itself already is the
  provenance. A document carries an optional `SIGNATURE` (mirroring 009 §3's package format),
  verified before parsing (009 §4's rule, applied here too), with operator approval keyed to the
  digest §6 already tracks — extending an existing mechanism, not adding a new one, and matching
  012 §4a's card signing and 018 §8 Q4's caller-pinned-trust posture. Unsigned documents stay
  loadable for the common case of a document built and reviewed entirely through the operator's own
  pipeline (their own git repo, their own CI); signing is required only when §5's `$ref`/registry
  resolution pulls a document from outside that pipeline — the same boundary 009 already draws for
  plugins.
- ~~**Q4** — Interop with MAF's declarative agent YAML and with `SKILL.md`-style packaging: importer
  or native support? (Pending the RFC 011 skills research.)~~ **Resolved, split — one half was
  already answered, the other stays deferred (2026-08-04):** the `SKILL.md` half isn't actually an
  open question anymore: 009 §8a already adopted it as *the* skill format of record, natively, not
  as something translated into via an importer. What remains is MAF's own *agent-definition* YAML —
  a genuinely different, harder problem, because it very likely references language-specific
  callables (a Python function, a .NET type) with no general equivalent in a C++/WASM-plugin engine,
  unlike `SKILL.md`'s genuinely portable declarative content. **No importer for v1** — resolving
  arbitrary cross-language callable references is real, speculative design work with no concrete
  demonstrated need (no real MAF deployment has asked to migrate here yet); if one does, that's a
  scoped, evidence-driven pass at that time. 002/015 already carry most of the familiarity benefit by
  borrowing MAF's own vocabulary (CLAUDE.md: "the developer model is MAF-shaped") without needing to
  solve callable portability.
