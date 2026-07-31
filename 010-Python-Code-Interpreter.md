# 010 — Python Code Interpreter

**Status:** Draft · **Depends on:** 006, 007, 008, 009 · **Gate:** §9 · **Research:** [2026 landscape §6](docs/research/2026-standards-landscape.md)

## Goal

A **built-in, default-on** Python execution capability that agents use to compute, transform data,
and orchestrate tool calls in code — with the isolation contract of 008 and no host trust — and a
runtime choice grounded in what the Python ecosystem actually supports today.

## 1. Two modes, one tool

| Mode | Shape | Use |
|---|---|---|
| **Interpreter** | `execute_code(code, language="python") → outputs, artifacts` | Data work, computation, file transformation |
| **CodeAct** | The same tool, plus a `call_tool(name, args)` bridge inside the sandbox | Multi-step logic, conditional tool chains, filtering large results — one sandbox invocation instead of N model round-trips |

CodeAct is a *configuration* of the interpreter, not a separate subsystem: same tool, same sandbox,
same approval, with the tool bridge granted or not. This mirrors the design MAF settled on
(ADR 0024) and avoids two code paths for one capability.

## 2. Runtime selection — the decision and its evidence

**Default backend: `native-jail` CPython (008). Not WASM.** The reasoning is empirical, dated, and
recorded so it can be revisited when the facts change:

- The **rich Python-on-WASM ecosystem is Emscripten-targeted**: Pyodide ships NumPy, pandas, SciPy,
  Matplotlib, scikit-learn, cryptography and more, and since April 2026 PyPI accepts PEP 783
  `pyemscripten_*_wasm32` wheels so maintainers publish directly. **But Emscripten requires a
  JavaScript host** — it cannot be embedded in a C++ process via wasmtime. Adopting it would mean
  embedding a full JS engine in the engine, which is a larger dependency and a larger attack
  surface than the sandbox it would be protecting.
- The **embeddable target is WASI**, and it is not ready for this workload: PEP 816 (accepted) fixes
  CPython's WASI support policy from 3.15, but as of March 2026 the roadmap still lists sockets
  (blocked on WASI 0.3 plumbing), threading, a **wheel platform tag**, and a bundling mechanism as
  outstanding. With no wheel tag there is no binary-wheel ecosystem: a WASI Python interpreter
  today is **stdlib + pure-Python wheels** and cannot `import numpy`.

**Therefore:**

| Profile | Runtime | What you get | When it is the default |
|---|---|---|---|
| `native-jail` | Real CPython + venv + PyPI | Full ecosystem: NumPy, pandas, SciPy, whatever the operator allows | **Default**: local dev, single-tenant, trusted-tenant deployments |
| `microvm` | Real CPython in a micro-VM | Full ecosystem + hardware boundary | Hostile input, multi-tenant, untrusted users |
| `wasm` | CPython on WASI | stdlib + pure-Python wheels; deterministic, instant, snapshot-able, identical on 3 OSes | Deterministic replay, ultra-cheap short computations, platforms where the others are unavailable |
| `remote` | Cluster-managed CPython | Full ecosystem, cluster lifecycle | Production scale-out |

**This is a profile default, not an architecture.** When WASI Python grows a wheel tag and a binary
ecosystem, `wasm` becomes the default by changing one default — the tool, its contract, its
approval model, and its telemetry do not move. That is the entire reason 008 is a seam.

Non-Python languages (JavaScript/TypeScript via a JS component, and native code via `ae:codec`
plugins) reuse the same tool with a different `language`; the interpreter is not Python-only by
design, only Python-first by priority.

## 3. Execution contract

Every `execute_code` invocation:

1. Starts from **clean interpreter state**. In-memory variables never persist across calls.
   Persistence is the workspace (§4) — external state, explicitly granted. (Same contract as MAF's
   CodeAct ADR; it exists to stop a sandbox from becoming an unaudited memory.)
2. Runs with the **capability set granted for this call** (007), empty unless granted.
3. Is **bounded**: wall clock, CPU, memory, processes, file count, disk quota, output bytes.
4. Returns a **structured outcome**: `{stdout, stderr, result_repr, artifacts[], outcome_class,
   usage}`. Timeout, OOM, crash, and policy violation are outcome classes, never exceptions.
5. Emits a span nested in the run and an audit record (I4).

**Output discipline:** stdout/stderr are size-capped with explicit truncation markers; anything
large becomes an artifact `BlobRef` (003 §3). An agent that prints a 200 MB dataframe gets a
truncation notice, not an OOM'd host.

## 4. Workspace and artifacts

- A **workspace** is a mounted directory, per-session or per-run, granted via `FsRead`/`FsWrite`
  with a quota. It is the only sanctioned persistence across executions.
- **Artifacts** — files the code writes to a designated output area — are collected, digested,
  content-addressed, and surfaced as parts (003) so a chart or CSV flows into the conversation and
  into A2A artifacts (012) without a special case.
- **Inputs** are mounted read-only by `BlobRef`, never copied through the prompt.

## 5. Packages

Package availability is **operator policy, never model choice** (I3):

| Policy | Behaviour |
|---|---|
| `preinstalled` (default) | A curated, pinned image/venv. No install at runtime. Reproducible, offline, fast. |
| `allowlist` | Install only from a pinned allowlist, from a local mirror, with digest verification. |
| `open` | Install from an index at runtime. Requires `NetOut` to the index and is off by default. |

`pip install` from model-generated code is **not** a capability that exists unless the operator
grants both the policy and the egress. A model that asks for a missing package receives a
structured error naming the policy, which is a better failure than a silent network call.

## 6. The `call_tool` bridge (CodeAct)

- Available only when the `ToolCall` capability is granted for the execution.
- Calls from inside the sandbox execute at the **sandbox's trust tier (T3)**, with the sandbox's
  capability set — never the agent's (007 §6). Otherwise the sandbox would be a privilege-escalation
  gadget rather than a boundary.
- Each bridged call traverses the **full tool pipeline** (006 §3): validation, authorization,
  admission, accounting. There is no bypass because the caller is "already inside".
- **Approval is bundled at `execute_code` time** over the pre-registered bridged tool set: if any
  bridged tool requires approval, `execute_code` requires approval. Nested per-call approval
  interrupts long CodeAct runs badly and is deliberately not the default (an option is left open,
  §10 Q2).
- Bridged tool results re-enter the sandbox as **tainted data** (003 §2) — code that then passes
  them to another tool is doing exactly what taint tracking exists to constrain.

## 7. Determinism

Under the `wasm` profile with determinism enabled (008 §5), an execution is a pure function of
`(code, inputs, capabilities)` — cacheable by digest and replayable offline. Under `native-jail` and
`microvm`, execution is recorded, not deterministic, and the spec says so rather than pretending.

## 8. Observability

Per execution: profile/backend, cold or warm, create/exec/destroy durations, CPU ms, peak memory,
bytes in/out, artifact count and total size, bridged tool calls, package installs, outcome class.
`execute_code` is traced as an ordinary tool invocation nested in the run, and bridged tool calls
emit ordinary tool spans — the cross-SDK telemetry contract MAF standardized, adopted here so
traces are comparable across frameworks.

## 9. Promotion gate

- **G1 (ecosystem)** — under `native-jail`, a scripted data task using NumPy + pandas produces a
  chart artifact on Windows and Linux; under `wasm`, the same task fails with a *clear, structured*
  unavailable-package error rather than a mysterious import failure.
- **G2 (containment)** — the hostile corpus (008 §7) plus interpreter-specific attacks (`os.system`,
  `ctypes`, `/proc` and registry probing, symlink escape from the workspace, egress to
  `169.254.169.254`, fork bomb, memory bomb, output flood, `sys.settrace` shenanigans) is contained
  on every backend, with positive controls proving the tests are not vacuous.
- **G3 (state isolation)** — a variable, an open file handle, a background thread, and a monkeypatch
  from execution *n* are all absent in execution *n+1*, including on pooled/snapshot backends.
- **G4 (bridge)** — a bridged `call_tool` cannot reach a capability the sandbox lacks but the agent
  holds; proven with a deliberately over-privileged control that the check catches.
- **G5 (budget)** — cold and warm `execute_code` p50/p99 per profile against 023.

## 10. Open questions

- **Q1** — Whether to ship a curated first-party interpreter image, and who owns its CVE cadence.
  A pinned image is the only way `preinstalled` is reproducible, and it is a real maintenance load.
- **Q2** — Nested per-tool approval inside CodeAct: better fidelity, worse UX. Left open.
- **Q3** — Whether long-running executions should be able to stream partial output (they should) and
  how that interacts with the recording seam.
- **Q4** — GPU access for interpreter workloads is unsolved in every profile (008 Q5).
- **Q5** — Whether to track the Pyodide/Emscripten path at all via an out-of-process JS host, purely
  to unlock the WASM package ecosystem for the `remote`/out-of-process case.
