# 006 — Tool and Function Plane

**Status:** Draft · **Depends on:** 003, 007, 008, 009, 011, 012 · **Gate:** §8

## Goal

One declaration, one invocation path, one approval model, one telemetry shape — for tools whose
implementations are wildly different: a native C++ function, a WASM plugin, an MCP server tool, a
remote A2A agent, a sandboxed script.

## 1. Tool declaration

```cpp
struct WebSearch : Tool<WebSearch,
        Capabilities<NetOut<"api.search.example">>,
        Approval<Mode::NeverRequire>,
        Parallelizable,
        Timeout<30s>> {
    static constexpr std::string_view name = "web_search";
    static constexpr std::string_view description = "Search the web for a query.";

    struct Args  { std::string query; int max_results = 5; };   // → JSON Schema 2020-12
    struct Reply { std::vector<SearchHit> hits; };

    static ae::task<result<Reply>> invoke(Args, EffectContext&);
};
```

- **Schemas are derived from the argument/result types** at compile time (Quark 016's one-`describe`
  discipline), emitted as JSON Schema 2020-12 — the same dialect MCP requires (011), so a tool is
  MCP-publishable without a second schema.
- **`EffectContext` is mandatory** in the signature. There is no ambient-context accessor, because
  I4 requires attribution at the point of effect and I2 forbids ambient authority.
- **Capabilities are declared on the tool**, and the agent's ceiling must cover them (002 §6), so an
  unauthorized tool fails at startup rather than at 3 a.m.

## 2. Tool sources

| Source | Binding | Isolation |
|---|---|---|
| **Native** | Compiled in-process function | Host trust — reserved for first-party code |
| **WASM plugin** | WIT world `tool` (009) | Component sandbox, capability-scoped |
| **MCP server** | Discovered from `tools/list` (011) | Process/network boundary + remote trust |
| **Remote agent** | A2A skill (012) | Network boundary + remote trust |
| **Sandboxed script** | Code interpreter (010) | Sandbox profile (008) |
| **Composite** | Workflow exposed as a tool (014) | Inherits its nodes' |

**Uniformity rule:** the model sees one tool list; the author writes one declaration syntax; the
engine emits one span shape. Source appears in metadata and policy, never in the calling convention.

## 3. Invocation pipeline

Every tool call, regardless of source, traverses exactly this pipeline:

```
1. resolve       name → declaration      (unknown name → Contract error, never a guess)
2. validate      arguments vs schema     (reject; do not coerce)
3. taint         arguments carrying model-originated content are tainted (003 §2)
4. authorize     capability check against the run's set          (007)
5. approve       policy → auto | require approval → InputRequired (001 §2)
6. admit         rate limit, concurrency, quota                  (Quark 022)
7. bind          materialize capability handles for this call only
8. invoke        with deadline + stop_token, in the declared isolation
9. normalize     result → parts (003); errors → structured ToolResult{is_error}
10. account      usage, duration, bytes; span closed; audit record written
```

**Non-negotiable properties:**

- **No step is skippable by configuration.** A "fast path" that bypasses 4, 5, or 10 does not exist.
- **Argument validation rejects, never coerces.** Coercion is how a `path` becomes `../../etc`.
- **Capability handles are per-call and revoked at step 10.** A tool cannot retain authority beyond
  its invocation; a handle outliving its call is a defect class with a dedicated test.
- **A tool error is a value** returned to the model (001 §6), not an exception and not a run abort.

## 4. Approval

The vocabulary is deliberately MAF's — `never_require` / `always_require`, with a `PolicyDriven`
mode added — because it is well understood and maps onto the human-in-the-loop mechanisms of both
protocols we speak (MCP MRTR, A2A `INPUT_REQUIRED`).

- **Escalation is conservative and pre-execution.** For a batch, if any tool requires approval, the
  batch requires approval. Approval is granted before execution, over the *concrete, validated
  arguments* the user is shown.
- **Approval is bound to the exact call.** The approved payload is hashed; a mismatch at execution
  is a `Policy` failure. Approve-then-substitute is a real attack, not a hypothetical one.
- **PolicyDriven** consults declared policy over `{tool, capability set, argument predicates,
  principal, taint}` — e.g. auto-approve reads under a mounted workspace, require approval for any
  write outside it, always require approval for network egress to a host not in the allowlist.
- **Approvals never come from a model** (I3). A model may *request*; only a host policy or a human
  may *grant*.

## 5. Concurrency

Sequential by default (001 §4). A batch is parallel only when **every** call in it is
`Parallelizable`. `Parallelizable` is a claim about external effects — that concurrent execution
with other tools is observably equivalent to some sequential order — and declaring it on a
state-mutating tool is a defect, not a tuning choice.

Parallel batches still serialize their **result append** in the model's emitted order, so the
history is deterministic regardless of completion order — a precondition for I5 replay.

## 6. Tool discovery and dynamic tool sets

- **Static** tools come from `Tools<...>` (002).
- **Dynamic** tools come from MCP servers, A2A peers, and loaded skills. They are resolved at run
  start into an immutable per-run tool table (snapshotted, like MAF's per-run tool surface), so a
  mid-run change to a remote server cannot alter what a run is allowed to do.
- **Caching:** MCP list results carry `ttlMs` and `cacheScope` (2026-07-28); we honour both and
  prefer deterministic ordering for prompt-cache stability (011).
- **Tool-name collisions across sources are an error**, resolved by declared namespacing, never by
  silent last-wins.

## 7. Tool result hygiene

A tool result is external content and is the primary prompt-injection vector (017):

- Tainted, provenance-marked, and delimited when rendered into the prompt.
- **Size-bounded** with declared truncation; large payloads become `BlobRef`s (003 §3).
- **Structured where possible** — a `Data` part with a schema beats prose the model must parse.
- Results never carry executable directives that the engine acts on. There is no in-band control
  channel from a tool to the engine; control flows through typed fields only.

## 8. Promotion gate

- **G1** — one agent invokes the same logical tool through all five sources (§2) and produces
  identical span shape, audit record shape, and model-visible result shape.
- **G2** — negative suite: every one of {unknown name, schema violation, capability not held,
  approval mismatch, deadline exceeded, sandbox crash, oversized result} produces a structured
  `ToolResult{is_error}` with the correct classification and no leaked capability.
- **G3** — a capability handle from call *n* is unusable in call *n+1* (proven, not asserted).
- **G4** — parallel batches produce deterministic history order across 10⁴ randomized completions.

## 9. Open questions

- **Q1** — Should tool *outputs* be schema-validated as strictly as inputs? MCP's `outputSchema`
  and `structuredContent` allow it; strictness may break real servers.
- **Q2** — How to expose long-running tools: MCP's tasks extension, A2A tasks, and our own
  `Suspended` state are three shapes for one idea (see 019 Q2).
- **Q3** — Whether `Parallelizable` can be *inferred* for pure WASM plugins with no capabilities
  (arguably yes, and it would be free).
