# Runtime & Execution Design Goals

**Status:** Living design goals — a one-page summary of properties the numbered RFCs already
specify in detail, kept here for onboarding and quick reference.

This document defines the long-term architectural goals of the execution environment.

It intentionally does **not** specify APIs, classes, protocols, or implementation details. Those
may evolve as practical experience is gained.

Instead, these goals describe the desired properties of the system. Any implementation should
attempt to satisfy these goals as closely as practical.

**This is a summary, not a second spec.** Every goal below cites the RFC section that actually
specifies it. Per `CLAUDE.md`: when this document and an RFC disagree, the RFC wins — fix this
document, never the other way around. Most of what follows restates decisions already made and
proven out elsewhere; two goals (marked below) were corrected against the RFCs during review
rather than left as this document first stated them, because they repeated a naming collision and
a state-merging mistake this project already found and rejected once.

---

# 1. One Environment

The agent should perceive a single execution environment.

Regardless of whether an action originates from:

* Shell
* Python
* CodeAct
* Future language runtimes

they all execute against the same logical environment.

The agent should never need to reason about runtime boundaries.

*Specified in: 026 §1 ("the agent works in what looks like a normal computer, it is not taught the
architecture that makes that safe").*

---

# 2. One Shared State: Working Directory, Worktree, Environment

All runtimes share:

* current working directory
* worktree
* environment variables

Changing this state from one runtime must be immediately visible to every other runtime.

Example:

```
Shell

cd reports
```

↓

```
Python

Path.cwd()
```

must observe the same directory.

**Corrected during review.** An earlier draft of this goal also bundled *permissions* and
*resource limits* into this same shared state. That repeats a design this project already
considered and rejected (`docs/concepts/runners-worktree-and-memory.md`'s note on
`another-concept.md`'s `ExecutionContext` proposal): `cwd`/`env` are hot, freely-mutable, low-stakes
state, while permissions are the security boundary and must only ever change through a formal
grant/revoke path (007). Putting both in one mutable object means the code path that handles a
`cd` sits next to the code path that could escalate capabilities — precisely what I2 (no ambient
authority) exists to prevent structurally. The real design keeps them apart on purpose and this
goal is scoped to match it.

*Specified in: 010 §3a (`ExecState = {cwd, env}`, shared by reference across `Runner`s). Permissions
and resource limits are a separate object, `SandboxSpec`/`CapabilitySet` (007, 008 §1b) — see Goal 8.*

---

# 3. Kernel Owns State

Persistent execution state belongs to the kernel.

Individual runtimes must not maintain their own copies of mutable state whenever a shared
representation exists.

The kernel is the single source of truth.

*Specified in: 008 §1b ("the sandbox is the whole execution environment... CPython is one component
of it, not the boundary itself").*

---

# 4. Language Independence

Python is not the execution model.

Shell is not the execution model.

Neither should become privileged.

Both are simply frontends capable of expressing actions against the execution kernel.

Future language runtimes should integrate without requiring architectural changes.

*Specified in: 010 §1a (the `Runner` concept — `PythonRunner`/`ShellRunner` are two instances of it)
and §2 ("the interpreter is not Python-only by design, only Python-first by priority").*

---

# 5. Standard Language Semantics

Where practical, standard language behavior should be preserved.

Agent code should look like ordinary Python.

Shell commands should resemble ordinary shell usage.

Framework-specific syntax should be introduced only when representing concepts that do not exist in
the host language.

*Specified in: 026 §1 ("fewer wrong guesses... `open("data.csv")` is knowledge it already has") and
010 §2 (`ShellRunner`'s grammar is explicitly not full POSIX, stated honestly rather than oversold).*

---

# 6. Framework Extensions

Capabilities unique to the framework should be exposed explicitly.

Examples include:

* subagents
* background tasks
* UI rendering
* planner interaction
* agent orchestration

These concepts do not belong to Python or the shell themselves and therefore may be exposed through
dedicated framework APIs.

Framework extensions should complement standard language features rather than replace them.

*Specified in: 026 §5 (the `agent.*` library — `agent.spawn`, `agent.progress`, `agent.ask`), 013
(UI as a projection of one event stream), 014 (workflow orchestration).*

---

# 7. No Runtime Leakage

The execution model should not expose implementation details.

The agent should not need to know:

* whether Python is embedded
* whether shell commands are builtins
* whether filesystem access is virtual
* whether networking is intercepted
* whether tools execute locally or remotely

Implementation choices belong to the kernel.

*Specified in: 026 §1a ("transparency is a prompt-surface decision, never a security mechanism") and
§2/§3 (the environment table, the error-mapping table).*

---

# 8. Kernel-Mediated Effects

Every observable side effect should pass through the execution kernel.

Examples include:

* filesystem access
* network requests
* process execution
* UI rendering
* task scheduling
* tool invocation
* agent orchestration

No runtime should directly access host resources when an equivalent kernel capability exists.

*Specified in: I2, and concretely in 008 §1b's two mediation layers (a closed import allowlist, plus
wrapper mediation of `open`/`socket`/`subprocess` for whatever is allowed).*

---

# 9. Tools Before Implementations

The framework should define **tools**, not libraries, as its stable public surface.

For example:

Tool:

* Spreadsheet
* PDF
* Document
* Search

Implementation:

* openpyxl
* pypdf
* python-docx

Libraries may change over time.

Tools should remain stable.

**Corrected during review.** This goal originally called the stable, named thing a "Capability."
That word is already taken: 007 defines `Capability` as the unforgeable handle authorizing one
class of effect (`FsRead`, `NetOut`, `RunnerCall<name>`) — the term I2 itself is stated in. Reusing
it here for "a stable name over a swappable library" would collide with the project's single most
security-critical piece of vocabulary. The concept this goal wants already has a name: a **Tool**
(006).

*Specified in: 006 §1–§2 (`Tool` declaration; "the model sees one tool list... source appears in
metadata and policy, never in the calling convention").*

---

# 10. Shared Implementations

Equivalent operations should resolve to a single implementation whenever practical.

Example:

```
Shell

grep
```

↓

Search Tool

↓

```
Python

agent.search(...)
```

↓

Search Tool

The implementation should not be duplicated simply because multiple runtimes expose it.

*Specified in: 006 §2 ("the same rule binds across frontends, not only across sources... `grep` in a
shell pipeline and `agent.tools.search(...)` in Python must resolve to the same registered Tool").
This is the same example already written into that section — this goal restates it, not extends it.*

---

# 11. Deterministic Mediation

The kernel should have an opportunity to observe, validate, modify, deny, or record externally
visible effects before they occur.

Examples include:

* adding HTTP headers
* auditing filesystem access
* tracing execution
* permission enforcement
* telemetry
* caching
* policy enforcement

The runtime should remain unaware that such mediation occurred.

*Specified in: 006 §3 (the ten-step invocation pipeline: resolve/validate/taint/authorize/approve/
admit/bind/invoke/normalize/account — "no step is skippable by configuration").*

---

# 12. Replaceable Implementations

Internal implementations should be replaceable without affecting agent behavior.

Examples:

* virtual filesystem
* HTTP client
* Python libraries
* UI renderer

Replacing an implementation should not require changes to prompts or agent code whenever the
exposed capability remains unchanged.

*Specified in: CONVENTIONS.md's dependency tiers (seam backends swap behind one contract) and 010
§2 ("this is a profile default, not an architecture").*

---

# 13. Tool-Oriented Architecture

Tools represent high-level capabilities.

A tool may internally use:

* Python
* Shell
* Native C++
* External software
* Remote services

The agent interacts with the tool abstraction rather than its implementation.

*Specified in: 006 §2 (tool sources: native, WASM plugin, MCP server, remote agent, sandboxed
script, composite workflow — one declaration syntax, one invocation path regardless of source).*

---

# 14. Stable Mental Model

The execution environment should minimize cognitive load.

The agent should think in terms of:

* files
* directories
* tasks
* tools
* capabilities

rather than runtimes, interpreters, processes, or operating-system details.

*Specified in: 026 §7 (the prompt budget: environment description ≤60 tokens, sandbox/capability
architecture 0 tokens, enforced by a measured gate).*

---

# 15. Progressive Enhancement

Advanced features should extend the execution environment without changing its conceptual model.

Examples include:

* tracing
* telemetry
* distributed execution
* sandboxing
* caching
* replay
* execution recording

These should enhance existing behavior rather than introduce separate execution models.

*Specified in: 013 (every external surface — AG-UI, A2A, MCP progress, OpenAI-compatible SSE — is a
projection of one internal run event stream, not a second event model) and 019/022/028, each of
which adds a capability (durability, replay, zero-copy transfer) without a second execution model.*

---

# 16. API Flexibility

Public APIs are implementation details.

They may evolve as practical experience reveals better abstractions.

The architectural goals defined in this document should remain significantly more stable than any
individual API.

RFCs should optimize for achieving these goals rather than preserving specific interfaces.

*Consistent with CONVENTIONS.md's own hierarchy, read in the right direction: the RFCs are the
stable, authoritative layer ("when code and a spec disagree, the spec wins"); this document is a
still-more-informal summary one level above that, useful for orientation, never a tie-breaker
against an RFC's actual text.*

---

# Summary

The framework is fundamentally a native execution kernel.

Languages, shells, libraries, and tools are interchangeable frontends built on top of that kernel.

The kernel owns execution state, capabilities, and mediation of side effects.

Agents should perceive one coherent environment regardless of how actions are expressed.
