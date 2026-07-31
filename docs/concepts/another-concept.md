# Unified Runtime Architecture

## Status

**Superseded.** Reviewed against 008 §1b, 010 §1a/§2/§3a, and 006 §2 (2026-07-31). Most of this
proposal was already the locked design under different names; two ideas were genuinely new and were
resolved on their merits rather than adopted verbatim:

- **Adopted, renamed:** "one implementation exposed through multiple frontends" (§Capabilities
  below) is real and now spec'd — 006 §2's uniformity rule extends across *frontends*
  (`ShellRunner`, `agent.tools`, the model's tool-call channel), not only across sources. It is
  **not** named `Capability` — that word is already `007`'s unforgeable authorization handle, the
  term I2 itself is stated in terms of, and reusing it for "one shared implementation" would be a
  worse collision than the `Executor`/`Runner` one already avoided elsewhere. The concept this
  document calls a `Capability` (Search, Diff, Spreadsheet, Document, PDF, Image, Git) is a **Tool**
  (006), and the rule is scoped to genuinely Tool-shaped operations — it does not pull cheap,
  worktree-native builtins (`cd`, `ls`, `cat`) into the tool pipeline, which would be real overhead
  for no benefit.
- **Rejected:** merging `cwd`/`env`/worktree/resource-limits/capability-registry into one
  `ExecutionContext` object. `cwd`/`env` are hot, freely-mutable, low-stakes state (`ExecState`,
  010 §3a); the capability set is the security boundary and must only change through 007's formal
  grant/attenuation/revocation path. Putting both in one mutable object means the code path that
  changes a working directory sits next to the code path that could escalate capabilities — exactly
  what I2 exists to prevent structurally, not by convention. The existing split (`ExecState` +
  `Sandbox`, 008 §1b) stays.

Everything else here — one shared filesystem/cwd/env, a virtual shell that is not a real OS shell,
external tools (Pandoc, LibreOffice, Tesseract, MCP servers) staying outside the kernel, no claim of
full POSIX conformance — was independent confirmation of what 008/010/009 already specify, not a
change. Kept below verbatim as the record of the proposal that was reviewed.

---

## Motivation

The framework is designed around a **native C++ kernel**, not around Python.

Python, Shell, and future runtimes are merely different frontends that operate on the same execution environment. An agent should never have to reason about runtime selection, environment migration, or capability differences.

The guiding principle is:

> **The agent sees one environment. The kernel owns the implementation.**

---

# Design Goals

The runtime model should satisfy the following requirements:

* One filesystem.
* One current working directory.
* One environment.
* One permission model.
* One execution context.
* One capability registry.

Regardless of whether the agent issues shell commands or Python code, every operation executes against the same shared state.

---

# Architecture

```text
                Agent
                   │
         Planner / Tool Calls
                   │
      ┌────────────┴────────────┐
      │                         │
   Shell Runtime          Python Runtime
      │                         │
      └────────────┬────────────┘
                   │
           Execution Context
                   │
             Capability Kernel
                   │
     Filesystem / Memory / Git / ...
```

The runtimes themselves are intentionally lightweight.

The C++ kernel owns all state.

---

# Execution Context

Every runtime receives the same execution context.

```text
ExecutionContext

- worktree
- cwd
- environment variables
- resource limits
- permissions
- capability registry
- process/session metadata
```

The execution context is the single source of truth.

No runtime maintains its own copy of these values.

---

# Runtime Behavior

## Shell

The virtual shell is not an operating-system shell.

Instead, it parses commands into kernel operations.

Example:

```text
grep invoice *.txt
```

↓

```text
SearchCapability(...)
```

No external process is required.

---

## Python

Python executes inside an embedded CPython interpreter hosted by the kernel.

Python is treated as another frontend over the execution context.

Example:

```python
from pathlib import Path

print(Path.cwd())
```

returns the same working directory that the shell currently uses.

Likewise,

```python
import os

os.chdir("reports")
```

changes the execution context so that subsequent shell commands observe the new directory.

---

# Shared State

The following state is shared between every runtime.

## Working directory

```shell
cd reports
```

↓

```python
Path.cwd()
```

must return

```text
/workspace/reports
```

---

Environment variables

```shell
export REPORT=summary.xlsx
```

↓

```python
os.environ["REPORT"]
```

must return

```text
summary.xlsx
```

---

Filesystem

A file created from Python immediately appears to the shell.

Likewise, files created from the shell are immediately visible to Python.

---

# Capabilities

Capabilities belong to the kernel, not to individual runtimes.

Examples include:

* Filesystem
* Search
* Diff
* Spreadsheet
* Document
* Presentation
* PDF
* Image
* Git

Each capability has exactly one implementation exposed through multiple frontends.

For example:

```text
Shell
    grep
```

↓

```text
Search Capability
```

↓

```python
agent.search(...)
```

↓

```text
Search Capability
```

Only one implementation exists.

---

# Tools

Tools are separate from runtimes.

The planner invokes tools explicitly.

Examples:

* browser
* memory
* database
* llm
* github
* slack

Tools may internally use shell commands, Python, or native code, but this is invisible to the agent.

---

# External Integrations

Heavy or externally managed software remains outside the kernel.

Examples:

* Pandoc
* LibreOffice
* Tesseract
* MCP servers

These are treated as external tools rather than embedded runtime components.

This keeps the execution environment deterministic while still allowing access to richer functionality.

---

# Design Principles

1. The kernel owns state.

2. Runtimes are interchangeable frontends.

3. Capabilities are implementation details.

4. The agent should never reason about runtime selection.

5. Shell and Python always observe the same execution context.

6. External software is accessed through tools rather than becoming part of the runtime.

---

# Non-Goals

The framework does not attempt to emulate a complete POSIX operating system.

The virtual shell exists to expose capabilities, not to reproduce every feature of Bash.

Similarly, Python is not treated as the core of the framework; it is simply one execution frontend provided by the kernel.

---

# Summary

The runtime model presents a single, coherent execution environment to the agent.

Internally, the C++ kernel owns all state and capabilities.

Shell commands, Python execution, and future language runtimes are merely different ways of expressing actions against the same execution context.

This minimizes cognitive load for the agent, avoids runtime-specific behavior, and allows implementations to evolve without changing the programming model.
