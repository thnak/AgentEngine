# Python Runtime Integration Goals

**Status:** Living design goals — a one-page summary of properties `010-Python-Code-Interpreter.md`
and `008-Sandbox-and-Isolation.md` §1b already specify in detail, kept here for onboarding.

This document defines the architectural goals for integrating CPython into the execution kernel.

It intentionally avoids prescribing specific APIs or implementation techniques.

Implementations may evolve over time, but they should strive to preserve the principles described
here.

**This is a summary, not a second spec.** Per `CLAUDE.md`: when this document and an RFC or ADR
disagree, the RFC/ADR wins — fix this document, never the other way around. Every goal below cites
where it's actually specified, and several are now backed by real evidence from
`decisions/ADR-002-pythonrunner-embedding-and-mediation.md`'s prove phase (2026-08-01) — the first
place in this project a Python-integration claim was tested against a real embedded interpreter
rather than argued.

---

# Purpose

Python is supported because it provides an excellent programming language and ecosystem for agent
execution.

Python is **not** the execution kernel.

The execution kernel remains the single owner of execution state, capabilities, permissions, and
observable side effects.

CPython is embedded as a language runtime rather than becoming the foundation of the framework.

*Specified in: 008 §1b ("the sandbox is the whole execution environment... CPython is one component
of it, not the boundary itself") and CLAUDE.md's locked decision ("the Python code interpreter is
embedded native CPython, permanently — never WASM, never a second runtime").*

---

# Goal 1 — Preserve Standard Python

Agent-generated Python should resemble ordinary Python.

Whenever practical, standard language behavior should be preserved.

Examples:

* standard syntax
* standard imports
* standard libraries
* standard programming model

The framework should avoid introducing unnecessary language extensions.

*Specified in: 026 §4 ("the interpreter takes ordinary Python source — no DSL, no magic comments,
no required wrapper function").*

---

# Goal 2 — No Framework-Specific Replacements

The framework should avoid replacing standard Python APIs with framework-specific equivalents.

Preferred:

```python
open(...)
print(...)
os.getcwd()
pathlib.Path(...)
```

Avoid:

```python
agent.fs.open(...)
agent.print(...)
agent.cwd(...)
```

Framework APIs should exist only for concepts that Python itself does not provide.

*Specified in: 026 §4 ("Tools are ordinary Python callables, not a `call_tool("name", {...})`
bridge") and, concretely, in ADR-002 §5's actual mechanism — `open`/`os.getcwd`/`os.chdir` are kept
as their ordinary names and silently redirected through capability-checked wrappers, not renamed to
a framework equivalent. This is the design already proven, not merely intended: `python_lockdown.cpp`
implements exactly this.*

---

# Goal 3 — Embedded Runtime

Python executes inside an embedded CPython interpreter managed by the execution kernel.

The framework should not depend on a system-installed Python whenever practical.

The embedded interpreter should behave consistently across supported platforms.

*Specified in: 010 §2 (embedded native CPython, one runtime, permanently). The
"not-a-system-install" half is **resolved** — 010 §10 Q1 (whether to ship a curated first-party
interpreter image) was decided 2026-08-04: yes, shipping one is not optional, since §5's default
`preinstalled` package policy is only reproducible/offline/fast if a curated image backs it; Q1's own
resolution names ownership (the ADR-judging authority) and a cadence (monthly rescan plus
out-of-band CVSS-High-or-above bumps, matching the Wasmtime-pin discipline of 009 §11 Q4). The
concrete prove-phase embedding still used a real installed CPython (a miniconda distribution) as its
target SDK — that remains true of the SDK used to *build against*, and is not a contradiction: Q1's
resolution is about what the *shipped* image is (a project-owned, pinned artifact), not about the
toolchain used to compile it. Whether that curated image's CPython binary is itself compiled from
source in-tree or vendored from an upstream release build is still an unspecified packaging detail,
not a re-opening of this goal.*

---

# Goal 4 — Upstream Compatibility

The embedded CPython source should remain as close as practical to upstream releases.

The framework should avoid maintaining a permanent fork of CPython.

Whenever possible, integration should rely on public embedding APIs, extension APIs, and runtime
configuration.

*Specified in: CLAUDE.md's locked decision ("the sandbox seam... still lets the isolation backend
evolve; the interpreter itself does not fork") and confirmed by what was actually built: ADR-002 §5.5
and `python_lockdown.cpp` use only `Py_InitializeFromConfig`, the documented meta-path finder
protocol, `PySys_AddAuditHook`, and ordinary attribute replacement — no CPython source patch exists
anywhere in this codebase.*

---

# Goal 5 — Runtime Profile Instead of Source Modification

Framework behavior should be introduced through runtime initialization rather than by modifying
CPython itself.

Examples include:

* registering native modules
* configuring runtime behavior
* replacing selected runtime objects
* installing import hooks
* configuring execution policies

The framework should prefer initialization over modification.

*Specified in: ADR-002 §3.4's three-layer mechanism, and now implemented exactly this way in
`src/backends/native_jail/python_lockdown.{hpp,cpp}` — the Layer 0 sweep is runtime configuration,
the finder is an import hook, `open`/`socket`/`subprocess` mediation is runtime-object replacement.
None of it touches CPython's own source.*

---

# Goal 6 — Kernel Owns Observable Effects

Observable side effects remain the responsibility of the execution kernel.

Examples include:

* filesystem access
* network requests
* process execution
* UI rendering
* task scheduling
* agent orchestration

Python should express operations.

The kernel should perform them.

*Specified in: 008 §1b's two mediation layers and 006 §3's ten-step tool-invocation pipeline.*

---

# Goal 7 — Shared Execution Context

Python shares the same execution context as every other runtime.

Changes performed from Python should immediately become visible to:

* Shell
* CodeAct
* future language runtimes

Likewise, changes performed elsewhere should immediately become visible to Python.

*Specified in: 010 §3a (`ExecState = {cwd, env}`, shared **by reference** across every `Runner` —
not a copy, not a synchronized mirror). Note this goal is correctly scoped to cwd/env/worktree only
— it does not bundle permissions or resource limits into the same shared object, which is the
correction `docs/architecture/DESIGN_GOALS.md`'s Goal 2 needed. Keep it this way.*

---

# Goal 8 — Transparent Mediation

Whenever practical, mediation should be transparent.

Agent-generated Python should not require additional code simply because it executes inside the
framework.

The framework should strive to preserve ordinary Python usage while internally redirecting
execution through the kernel.

*Specified in: 026 §1a ("transparency is a prompt-surface decision, never a security mechanism" —
the agent is not lied to, but the architecture is not spelled out either) and proven in ADR-002 §8.9:
`import numpy; numpy.array(...).sum()` behaves identically, version string and all, under the
lockdown interpreter as under an unmediated one, for the paths tested.*

---

# Goal 9 — Framework APIs Represent New Concepts

Framework modules should expose only concepts that do not naturally exist in Python.

Examples include:

* subagents
* background tasks
* planner interaction
* conversation primitives
* UI rendering
* execution management

Framework APIs should complement Python rather than replace it.

*Specified in: 026 §5 (the `agent.*` library — `agent.spawn`, `agent.progress`, `agent.ask`, none of
which shadow a Python builtin).*

---

# Goal 10 — Standard Libraries Remain Familiar

Whenever practical, standard libraries should continue to behave as developers expect.

Differences introduced by the framework should exist only when required to preserve:

* execution consistency
* security
* capability mediation
* sandbox behavior

Unexpected behavioral differences should be minimized.

**This goal is now backed by the sharpest concrete evidence in this document, and it cuts both
ways.** `decisions/ADR-002-pythonrunner-embedding-and-mediation.md` §8.9/§10.1 (`OpenQuestions.md`
OQ-15) found that "only differ when security requires it" is harder to deliver than it sounds: the
import-allowlist mechanism gates by module name, and granting `numpy`/`pandas` transitively requires
also granting `ctypes`/`winreg`/`subprocess` — names a security review would otherwise deny — because
the mechanism cannot distinguish numpy's own internal use of those names from guest code asking for
them directly. The goal is still correct; achieving it for any package with a real dependency chain
needs a mechanism (caller-aware gating) or an explicit policy-tier decision that doesn't exist yet.
Read this goal together with OQ-15, not in isolation.

*Specified in: 008 §1b; the tension is documented in ADR-002 §10.1–§10.2 and OQ-15.*

---

# Goal 11 — Replaceable Implementations

Internal implementations may change without affecting agent code.

Examples include:

* filesystem backend
* networking implementation
* capability implementation
* package selection
* runtime policies

Agent programs should depend on behavior rather than implementation.

*Specified in: CONVENTIONS.md's dependency tiers (seam backends swap behind one contract) and 010
§2 ("this is a profile default, not an architecture").*

---

# Goal 12 — Patching Is a Last Resort

Direct modifications to CPython or the standard library should be considered an exceptional
measure.

Preferred order:

1. Public embedding APIs
2. Extension modules
3. Runtime initialization
4. Import hooks
5. Runtime object replacement
6. Source patches

Permanent forks should be avoided whenever practical.

*This priority order is not written elsewhere as a single ordered list and is worth keeping exactly
as stated — it matches what `python_lockdown.cpp` actually uses (runtime initialization, import
hooks, runtime object replacement; nothing past step 5) and gives a concrete decision procedure for
future work on this backend, consistent with CLAUDE.md's "the interpreter itself does not fork."*

---

# Goal 13 — Future Compatibility

The integration architecture should remain compatible with future Python releases whenever
practical.

Upgrading CPython should primarily involve updating the embedded runtime rather than redesigning
framework behavior.

*Consistent with Goal 12's API-first priority order; not independently specified elsewhere. Worth
watching in practice: ADR-002 §6 items 1–3 already flag specific CPython-internals questions
(`sys.meta_path`'s exact access pattern, audit-event firing on cache hits) that were version-specific
enough to need a real embedding run rather than documentation — a future CPython upgrade should
re-check those, not assume they still hold.*

---

# Goal 14 — Separation of Responsibilities

Responsibilities should remain clearly separated.

CPython is responsible for:

* parsing
* compilation
* bytecode execution
* language semantics

The execution kernel is responsible for:

* execution state
* permissions
* capabilities
* resource management
* side-effect mediation
* execution policies

Neither layer should assume responsibilities belonging to the other.

*Specified in: 008 §1b, verbatim in spirit ("CPython is one component of it, not the boundary
itself").*

---

# Goal 15 — Stable Mental Model

From the agent's perspective:

Python remains Python.

From the framework's perspective:

Python is one frontend among many.

The execution kernel remains the authoritative execution environment regardless of which frontend
produced the action.

*Specified in: 010 §1a (the `Runner` concept — `PythonRunner` and `ShellRunner` are two instances of
it) and 026 §1.*

---

# Summary

The framework embeds CPython to provide a familiar and powerful programming environment while
preserving a unified execution model.

The kernel owns execution state and externally observable effects.

CPython provides language semantics.

Framework-specific behavior should be introduced through integration rather than modification
whenever practical.

Implementation techniques may evolve, but these architectural goals should remain substantially
stable over time.
