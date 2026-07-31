# 010 — Code Interpreter and Shell

**Status:** Draft · **Depends on:** 006, 007, 008, 009 · **Gate:** §9 · **Research:** [2026 landscape §6](docs/research/2026-standards-landscape.md)

## Goal

A **built-in, default-on** execution capability — Python and a shell, sharing one sandboxed
execution context — that agents use to compute, transform data, run ordinary commands, and
orchestrate tool calls in code, with the isolation contract of 008, no host trust, and a runtime
choice grounded in what the Python ecosystem actually supports today.

## 1. Three modes, one execution context

| Mode | Shape | Use |
|---|---|---|
| **Interpreter** | `execute_code(code, language="python") → outputs, artifacts` | Data work, computation, file transformation |
| **CodeAct** | The same tool, with the `agent` library present in the sandbox (026 §5) | The agent's primary action space: multi-step logic, conditional tool chains, filtering large results — one execution instead of N model round trips |
| **Shell** | `execute_shell(command) → outputs, artifacts` | Running ordinary commands — `ls`, `grep`, `git`, a build tool, a script — the things a person would reach for a terminal to do rather than write Python for |

CodeAct is a *configuration* of the interpreter, not a separate subsystem: same tool, same sandbox,
same approval, with the library granted or not. This mirrors the design MAF settled on (ADR 0024)
and avoids two code paths for one capability. Shell is a **peer** to the interpreter, not a
capability reached only through it (`subprocess`, `os.system`) — see §1a for the concept that makes
that true, and §3a for the state it shares.

**What makes it CodeAct is the library, not the interpreter.** An interpreter with no host-backed
library is a calculator; the action space is defined by what the program can *reach* — tools,
worktree, memory, artifacts, sub-agents, structured output — which is 026 §5's subject. That RFC
owns the surface; this one owns the execution.

### 1a. The `Runner` concept

Each mode above is backed by a `Runner` — deliberately not named `Executor`, which already means
"a workflow graph node" (014, 027 §5) and does not need a third meaning:

```cpp
struct Runner {                                        // concept, not a base class
    ae::task<result<ExecOutcome>> run(ExecRequest, ExecState&, EffectContext&);
};
```

`PythonRunner` backs Interpreter/CodeAct; `ShellRunner` backs Shell. Both run inside the same
session sandbox (008 §6) and share one `ExecState` by reference (§3a) — not a copy, not a
synchronized mirror, the same object — which is what makes "shared context" an exact guarantee
rather than a best effort.

**`ShellRunner` composes with other `Runner`s instead of exec'ing a real shell.** When a shell
command needs to run Python, `ShellRunner` calls `PythonRunner.run(...)` directly, under a declared
`RunnerCall<python>` capability — the same idiom as `ToolCall<name>` (006) and `AgentCall<agent>`
(026 §5): crossing from one execution unit into another is a capability, never ambient, even when
both units live in the same process. §2 explains why this composition, not a bundled shell binary,
is the design — the short version is that a real shell's entire purpose is resolving and exec'ing
arbitrary named programs, which is the one thing a capability-based system cannot make safe by
sandboxing alone; a `Runner` that never does that removes the hazard instead of containing it.

## 2. Runtime selection — one runtime, permanently, and why

**The runtime is embedded native CPython, under `native-jail` (008 §1b), or cluster-managed under
`remote`. There is no `wasm` Python backend and no `microvm` profile.** Two distinct arguments back
this, and neither alone would be enough:

**The ecosystem argument (empirical, dated, revisitable in itself):**

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

**The architectural argument (permanent, does not expire when the ecosystem argument does):** even
a fully mature WASI Python would still be a *second* Python — its own startup cost, its own subtly
different semantics (threading, C-extension availability, float/GC edge cases), its own place in
every test matrix. An agent's generated code, and the humans verifying it, would have to know which
Python they were dealing with, and a bug that reproduces on one and not the other is exactly the
kind of confusion the "one ordinary machine" principle (026 §1) exists to prevent. **This project
chooses one Python runtime by design, not as a stopgap** — the ecosystem argument is corroborating
evidence for why that costs nothing today, not the reason itself. `microvm` is dropped on the
equivalent logic (008 §1): the workloads it would have served either run fine under `native-jail`
with §1b's interpreter-level mediation, or genuinely need cluster-grade hardware isolation, which is
`remote`'s job, not a second local profile's.

| Profile | Runtime | What you get | When it is used |
|---|---|---|---|
| `native-jail` | Real CPython + venv + PyPI, mediated per 008 §1b | Full ecosystem: NumPy, pandas, SciPy, whatever the operator allows | **Default**, every deployment tier |
| `remote` | Cluster-managed CPython | Full ecosystem, cluster lifecycle, hardware-isolated where the cluster provides it | Production scale-out, hostile/untrusted-multi-tenant workloads |

This is still a **seam** (008), not a hardcoded call site — a future profile is not architecturally
foreclosed — but it is not a placeholder waiting for WASI to mature, and "when WASI Python is ready"
is not, by itself, a reason to reopen it.

**There is no real shell, on any platform or profile — bundled or otherwise.** An earlier draft of
this RFC proposed shipping one portable shell binary (`dash`/`toybox`-class) so behaviour would be
identical across Windows, Linux, and macOS instead of exposing whichever native shell the host
happens to have. That solves the *cross-platform consistency* problem but not the deeper one: **a
real shell's job is resolving a name against a search path and exec'ing whatever it finds.** That is
exactly the ambient-authority shape **I2** forbids — "reachable" would mean "anything on `PATH`,"
sandboxed or not, and every hardening technique available (seccomp filters, allowlisted binaries,
restricted `PATH`) is a way of *containing* that authority, never a way of not having granted it.

**`ShellRunner` (§1a) is engine-native code, not a wrapped shell,** and it never resolves or exec's
an arbitrary named program:

- It parses a **small, explicitly non-POSIX-complete grammar** — pipes, redirects, variable
  assignment and expansion, `&&`/`||`, and minimal control flow. We do not claim POSIX conformance;
  claiming it and being wrong is worse than stating the subset (the same honesty 013 §2.0 applies to
  AG-UI's maturity).
- A **fixed builtin set** — `cd`, `pwd`, `ls`, `cat`, `echo`, `export`, `mkdir`, `rm`, `mv`, `cp`, and
  similarly ordinary commands — is implemented directly against worktree operations (025) and the
  capability layer (007), not by shelling out to a coreutils binary.
- **Anything else resolves only to a registered `Runner` or a registered `Tool`** (006, 009) — e.g. a
  `git` or `ripgrep` component, if the operator has installed one, is invoked through the ordinary
  tool pipeline, not found on a search path. A name that resolves to neither produces the same
  "command not found" the agent would see from a real shell (026 §3) — the *experience* is ordinary,
  but underneath it is "no capability or registration," not "no file in `PATH`." **It is the same
  registered Tool an `agent.tools.*` call in Python would reach** — 006 §2's uniformity rule now
  binds across frontends, not only across sources, precisely so `grep` in a shell pipeline and a
  Python search call can never quietly diverge into two implementations.
- Because behaviour is engine code rather than a ported binary, **identical behaviour across
  Windows, Linux, and macOS is automatic**, not something that has to be built and verified per
  platform the way §2's Python runtime table does.

This is more implementation than adopting an existing shell would have been — a grammar, a builtin
set, and a dispatch layer, instead of one `dlopen`/component call into something that already works.
That cost buys a structural property a wrapped shell cannot: **I2 holds for shell commands by
construction**, the same way it already holds for the CodeAct tool bridge (§6) — there is no
privilege to contain because there was never a name-to-binary resolution step that could reach
something ungranted.

Non-Python languages (JavaScript/TypeScript via a JS component, and native code via `ae:codec`
plugins) reuse the same tool with a different `language`; the interpreter is not Python-only by
design, only Python-first by priority.

## 3. Execution contract

Every `execute_code` invocation:

1. Runs in the **session's interpreter** (008 §6). Within a session, in-memory state persists
   across executions — a conversation that computed `df` on turn 3 still has it on turn 7, which is
   what makes an interpreter feel like an interpreter rather than a series of amnesiac scripts.
   Across **sessions**, nothing persists: cross-session reuse is prohibited in every profile, and
   that boundary — not the per-execution one — is where isolation lives.
   Callers that want the stricter one-shot semantics select `per_exec` lifetime explicitly.
2. Runs with the **capability set granted for this call** (007), empty unless granted.
3. Is **bounded**: wall clock, CPU, memory, processes, file count, disk quota, output bytes.
4. Returns a **structured outcome**: `{stdout, stderr, result_repr, artifacts[], outcome_class,
   usage}`. Timeout, OOM, crash, and policy violation are outcome classes, never exceptions.
5. Emits a span nested in the run and an audit record (I4).

**Output discipline:** stdout/stderr are size-capped with explicit truncation markers; anything
large becomes an artifact `BlobRef` (003 §3). An agent that prints a 200 MB dataframe gets a
truncation notice, not an OOM'd host.

### 3a. `ExecState` — the same session backs Python and Shell

The point of a session-scoped sandbox (008 §6) is that a conversation feels like **one machine**,
not a bundle of specialized tools that happen to share a filesystem. That guarantee has a concrete
carrier: `ExecState`, `{cwd, env}`, one instance per session, held by the sandbox and passed **by
reference** into whichever `Runner` (§1a) executes.

- **One working directory, one environment, shared by every `Runner`.** A `cd` in a shell command
  mutates the same `ExecState` a subsequent `execute_code` call reads via `os.getcwd()`; `os.chdir()`
  in Python mutates the one a subsequent shell command reads via `pwd`. It is the same object, not a
  synchronized pair — sharing by reference is what makes this exact rather than eventually
  consistent.
- **Python's own `subprocess`/`os` calls and `ShellRunner` observe the same `ExecState`.** An agent
  that shells out from Python gets the same `cwd`/env it would get from the Shell mode; there is
  exactly one notion of "the current directory of this session," not one per `Runner`.
- **The worktree already makes files consistent** (§4, 025) — `ExecState` is the process-level
  counterpart: the state files were never the problem for.
- **Cross-session isolation is unchanged**: `ExecState` lives in the session's sandbox, subject to
  the same per-session lifetime, passivation behaviour (008 §6a), and cross-session prohibition as
  everything else in it. Two sessions never share an `ExecState` any more than they share a heap.
- **Background/long-running shell processes** (a dev server started with `&`, a watcher) are a real
  consequence of a persistent shell and are not fully specified here — see Q6.

## 4. Worktree and artifacts

Files are the **worktree** (025) — durable engine state, not sandbox state:

- `/work` (read-write, quota'd), `/input` (read-only mounts), `/out` (artifacts) — ordinary
  directories inside the guest, granted through `FsRead`/`FsWrite` capabilities scoped to subtrees.
- **Files survive everything the sandbox does not**: destruction, passivation, process restart, node
  migration, and a change of profile. This is the property that makes §2's backend choice a
  performance-and-ecosystem decision rather than a data-loss decision.
- **Artifacts** written under `/out` are collected, digested, content-addressed, and surfaced as
  parts (003) so a chart or CSV flows into the conversation and into A2A artifacts (012) with no
  special case.
- **Inputs** are mounted, never pasted into the prompt: a 40 MB CSV costs a mount, not a context
  window.
- Multiple agents in one session share the worktree or branch from it (025 §3), which is how
  concurrent agents collaborate on one set of files without corrupting each other.

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

## 6. Tools from code (CodeAct)

**The surface is ordinary Python, not a bridge API** — `from agent import tools` and then
`tools.web_search(query=...)`, with real signatures, type hints, docstrings and typed results
generated from tool metadata (026 §4). A `call_tool("name", {...})` indirection would force the
model to guess argument shapes it cannot see; a normal function signature is knowledge it already
has. The enforcement below is unchanged by that choice — only the spelling differs.

- Available only when the `ToolCall` capability is granted for the execution; ungranted tools are
  simply absent from the module rather than present and failing.
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

Execution under `native-jail` and `remote` is **recorded, not deterministic** (I5) — every
`execute_code`/`execute_shell` call is captured for replay (§8, 004 §6's recording discipline
applies equally here), but re-running the same code is not guaranteed to reproduce byte-identical
output the way a `wasm` component under 008 §5's deterministic mode is. This is the accepted cost of
§2's one-runtime decision, stated plainly rather than implied away: `wasm`'s determinism property
remains real and available to **plugins** (009) that use it; it is not a property of the code
interpreter or shell, by choice, not by oversight.

## 8. Observability

Per execution: profile/backend, cold or warm, create/exec/destroy durations, CPU ms, peak memory,
bytes in/out, artifact count and total size, bridged tool calls, package installs, outcome class.
`execute_code` is traced as an ordinary tool invocation nested in the run, and bridged tool calls
emit ordinary tool spans — the cross-SDK telemetry contract MAF standardized, adopted here so
traces are comparable across frameworks.

## 9. Promotion gate

- **G1 (ecosystem)** — under `native-jail`, a scripted data task using NumPy + pandas produces a
  chart artifact identically on Windows, Linux, and macOS from the same pinned `preinstalled` image
  (§5, §10 Q1); a package outside the image produces a *clear, structured* unavailable-package error
  naming the policy (§5), never a mysterious import failure.
- **G2 (containment)** — the hostile corpus (008 §7) plus interpreter-specific attacks (`os.system`,
  `ctypes`, `/proc` and registry probing, symlink escape from the workspace, egress to
  `169.254.169.254`, fork bomb, memory bomb, output flood, `sys.settrace` shenanigans) is contained
  on every backend, with positive controls proving the tests are not vacuous. **For `ShellRunner`,
  the equivalent classes — `PATH` hijacking, command substitution reaching an unregistered binary,
  function/alias shadowing of builtins — must not merely be contained, they must not exist as a
  reachable code path**: a positive control that asks `ShellRunner` to run a name that is neither a
  builtin nor a registered `Runner`/`Tool` must fail closed with "command not found" and must be
  provable to never reach `exec`/`CreateProcess` for that name (§2).
- **G3 (state boundary)** — within a session, a variable defined in execution *n* is present in
  *n+1*, and **an `ExecState` (§3a) mutation made through one `Runner` is visible to every other
  `Runner` and to the next call on the same `Runner`**; across sessions, a variable, open handle,
  background thread, monkeypatch, `ExecState`, or file written by session A is unreachable from
  session B on every backend, including pooled and snapshot-restored ones (008 §9 G7).
- **G4 (bridge)** — a bridged `call_tool` cannot reach a capability the sandbox lacks but the agent
  holds; proven with a deliberately over-privileged control that the check catches. The same proof
  is required for `RunnerCall<python>` (§1a): `ShellRunner` cannot invoke `PythonRunner` without the
  capability, and cannot exceed the capability set it was itself granted when it does.
- **G5 (budget)** — cold and warm `execute_code`/`execute_shell` p50/p99 per profile against 023.
- **G6 (parity)** — the same command's effect (write a file, read `cwd`, read an env var) is
  observable identically whether reached via `ShellRunner` or via `PythonRunner`'s mediated `os`
  surface (§3a, 008 §1b); and `ShellRunner`'s documented grammar and builtins behave identically
  across Windows, Linux, and macOS.
- **G7 (interpreter mediation, 008 §1b)** — a call to `open()`/`socket()`/`subprocess.*`/`os.system`/
  `ctypes` without the corresponding capability raises the exact 026 §3 exception **before any
  syscall is attempted**, proven by a syscall-level trace (strace/ETW) showing zero attempts, not
  merely a caught exception at the Python level. With the capability granted, the call succeeds and
  is indistinguishable in effect from the same operation reached through a `Tool` or `ShellRunner`
  builtin (G6).

## 10. Open questions

- **Q1** — Whether to ship a curated first-party interpreter image, and who owns its CVE cadence.
  A pinned image is the only way `preinstalled` is reproducible, and it is a real maintenance load.
- **Q2** — Nested per-tool approval inside CodeAct: better fidelity, worse UX. Left open.
- **Q3** — Whether long-running executions should be able to stream partial output (they should) and
  how that interacts with the recording seam.
- **Q4** — GPU access for interpreter workloads is unsolved in every profile (008 Q5).
- ~~**Q5** — Whether to track the Pyodide/Emscripten path via an out-of-process JS host for the
  `remote` case.~~ **Effectively closed by §2's architectural argument**: an Emscripten-backed
  Python for `remote` would be exactly the second-runtime confusion §2 rules out, just relocated to
  a different profile. Reopening it would need an ADR revisiting §2, not a narrower carve-out.
- **Q6** — Background shell processes (`command &`, a dev server, a watcher) are a natural
  consequence of a persistent shell (§3a) but are not designed here: per-call wall-clock bounding
  (§3) assumes the process ends with the call. Open: whether backgrounding is permitted at all, how
  its resource use is charged against the session rather than a single call, how the agent lists/
  kills what it started, and whether it survives passivation (008 §6a) on any profile.
- **Q7** — `ShellRunner`'s grammar parser processes text that may be tainted (003 §2) — model output,
  or content quoted from a tool result. §2 argues the *actions* it can cause are capability-gated by
  construction, but the *parser itself* is still first-party code parsing adversarial input, the same
  risk category 009 §7 pushes to a WASM plugin for file formats. Open whether the parser needs the
  same treatment (fuzzed hard and/or run as a component) rather than trusted host code by default.
