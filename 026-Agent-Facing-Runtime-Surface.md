# 026 — Agent-Facing Runtime Surface

**Status:** Draft · **Depends on:** 006, 007, 008, 010, 025 · **Gate:** §8

## Goal

Define what the *model* sees: the environment it appears to run in, the Python it writes, and the
library it can call. The design objective is **an ordinary environment, described as briefly as
possible**, so the model spends its tokens and its attention on the task instead of on learning our
architecture.

## 1. The principle

> **The agent works in what looks like a normal computer. It is not taught the architecture that
> makes that safe.**

Three reasons, in order of importance:

1. **Fewer wrong guesses.** A model has seen millions of lines of ordinary Python and almost none of
   any bespoke agent API. `open("data.csv")` is knowledge it already has; `sandbox.fs.read_text(...)`
   is knowledge it must infer, and it will infer it wrong.
2. **Token economics.** A sandbox preamble explaining profiles, capabilities, mounts, and approval
   semantics costs hundreds of tokens on *every* turn, and it is pure overhead — it does not help
   the model do the task.
3. **Attention.** Prompt content about the *environment* competes with content about the *problem*.

### 1a. The security caveat, stated up front

**Transparency is a prompt-surface decision, never a security mechanism.** Nothing in this RFC may
be load-bearing for safety. Specifically:

- Security **must not depend on the model not knowing** it is isolated. Assume it knows, assume it
  is told by an attacker, assume it probes. The controls in 007 and 008 hold regardless (**I3**).
- We do not *lie*. We omit architecture, and we phrase failures in ordinary terms. If the model asks
  whether it can reach the network, the honest answer is what it observes: it cannot.
- The full detail is always available **host-side** — audit, traces, and operator diagnostics lose
  nothing. Only the *prompt* is quiet.

## 2. The environment as it appears

| Surface | What the agent sees | What it never sees |
|---|---|---|
| Filesystem | `/work`, `/input`, `/out` — ordinary directories (025 §7) | Mount ids, worktree digests, object store, profile names |
| Python | A normal interpreter with stdlib | Interpreter build, WASI vs native, snapshot/restore |
| Files | Files persist between executions | That persistence is a content-addressed commit per turn |
| Network | Reachable hosts work; others fail like an unreachable host | Allowlists, egress proxy, capability names |
| Errors | `FileNotFoundError`, `PermissionError`, `TimeoutError` | Policy rule ids, capability names, profile fallbacks |
| Time/limits | Long work is interrupted like a timeout | Fuel, epochs, cgroup/Job Object accounting |

**No boilerplate.** The system prompt contains **no** sandbox description, **no** capability
enumeration, and **no** safety lecture. Where a constraint is *actionable* — "you can write to
`/work` and `/out`" — it is one short line, because that is task information, not architecture.

## 3. Error mapping

Failures reach the agent as the exception an ordinary program would get:

| Cause | Raised as |
|---|---|
| Path outside a mount, or write to a read-only mount | `PermissionError` / `FileNotFoundError` |
| Quota exhausted | `OSError` (`No space left on device`) |
| Host not permitted | ordinary connection failure (`socket.gaierror` / `ConnectionError`) |
| Wall-clock exceeded | `TimeoutError` |
| Memory exceeded | `MemoryError` |
| Tool denied by policy | the tool function raises `PermissionError` with a short, actionable message |
| Approval required | execution suspends (001 §2); the agent is not asked to reason about approval |

**Rules:**

- **Actionable failures say what would work** in one sentence: *"Writable paths are /work and /out."*
  Not a rule id, not a capability name.
- **Non-actionable failures do not invite retries.** If a host will never be reachable, the message
  must not read as transient, or the agent will burn turns retrying.
- **Never a stack trace from our host code**, ever — it leaks architecture and wastes tokens.

## 4. The code interpreter is plain Python

Per 010, the interpreter takes **ordinary Python source**. No DSL, no magic comments, no required
wrapper function, no bespoke result protocol. The last expression's value and anything printed are
captured, exactly as in a notebook — the shape the model already knows.

Tools are **ordinary Python callables**, not a `call_tool("name", {...})` bridge:

```python
from agent import tools

hits = tools.web_search(query="WASI 0.3 async", max_results=5)
rows = [h for h in hits if "bytecodealliance" in h.url]
tools.save_note(title="sources", body="\n".join(r.url for r in rows))
```

Generated from the same tool metadata as everything else (006 §1), so they cannot drift:

- **Real signatures** with real parameter names, defaults, and type hints from the tool's argument
  type.
- **Docstrings** from the tool description; `help()` works.
- **A `.pyi` stub** so the shape is inspectable and, in the `tools` module, discoverable by
  `dir()`.
- **Typed results** — dataclass-shaped objects, not raw dicts, so attribute access is guessable.

Every call still traverses the complete tool pipeline (006 §3) with the **sandbox's** capability set
(007 §6). Idiomatic surface, unchanged enforcement.

## 5. The `agent` library — this *is* CodeAct

**The library is the action space, not an accessory to it.** CodeAct means the model's primary way
of acting on the world is *writing a program*, rather than emitting one tool-call JSON object per
step and waiting for a round trip. That only works if there is something worth calling from inside
the program — so the design of this library **is** the design of what the agent can do.

The consequences are why the pattern is worth adopting:

- **One inference for many actions.** Search, filter, compute, branch on the result, then write a
  file — one execution instead of five model round trips, each of which would re-send the whole
  context.
- **Control flow the tool-call channel cannot express.** Loops, conditionals, retries, joins,
  aggregation over a large result set — ordinary code, not a protocol.
- **Data stays out of the context window.** A 50 000-row result is filtered inside the sandbox; only
  the answer is returned. With the JSON channel every intermediate result must transit the prompt.
- **The model already knows the idiom.** It has written Python against libraries its whole life
  (§1).

**The two channels coexist, with a clear division:**

| Channel | Used for |
|---|---|
| **Code** (this library) | Multi-step work, data manipulation, anything with control flow — the default |
| **Tool-call JSON** (006) | Single high-consequence actions, especially approval-gated ones, where the host must show the user exactly one call with exactly its arguments before it happens |

That split is deliberate: a bundled approval over a *program* is inherently coarser than an approval
over *one call with concrete arguments* (010 §6). High-consequence effects belong on the channel that
can be reviewed atomically.

Everything in the library maps to a capability; **nothing in it is ambient**. Modules are present
only when the corresponding capability is granted — an ungranted module is simply absent, which
reads to the model as "not available here" rather than as a policy essay.

| Module | Provides | Capability |
|---|---|---|
| `agent.tools` | The agent's tools as callables (§4) | `ToolCall<name>` per tool |
| `agent.files` | Convenience over the worktree — `artifact()`, `input()`, listing | `FsRead`/`FsWrite` |
| `agent.data` | Tabular/JSON helpers over inputs without loading them wholly into memory | `FsRead` |
| `agent.memory` | Session memory read/write (005 §5) | `Memory` |
| `agent.notes` | Durable scratch notes across turns | `FsWrite` |
| `agent.output` | Emit structured output conforming to the run's schema (003 §4) | — |
| `agent.progress` | Report progress on long work → run event stream (013 §1) | — |
| `agent.ask` | Ask the caller/user a question → `InputRequired` (001 §2) | `Elicit` |
| `agent.spawn` | Run a sub-agent, returning its result | `AgentCall<agent>` |

**Design constraints:**

- **Small and boring.** Every symbol must be guessable from its name by a model that has never seen
  the docs. If a function needs explanation, it is the wrong function.
- **Ordinary Python idiom** — iterables, context managers, dataclasses, exceptions. No callbacks, no
  handles to close, no session objects to thread through.
- **Versioned like a public API** (024), because prompts and agent code depend on it.
- **Every call is an effect**: attributed, audited, budgeted, cancellable (I4).
- **`agent.spawn` inherits an attenuated capability set** and a sub-worktree (025 §3) — a spawned
  agent can never exceed its parent.

**The trade this makes explicit:** a richer library means the agent can do more per execution
(fewer round trips, less token spend, better results) *and* a wider host attack surface. Each module
is therefore justified individually, capability-gated individually, and testable individually —
rather than shipping one `agent.engine` god-object that grows without review.

## 6. Skills as ordinary files

Loaded skills are mounted read-only at `/skills/<name>` (025 §3) and the agent reads them with
ordinary file operations. This is why the transparent-environment principle and the skill format fit
together well: the `SKILL.md` progressive-disclosure model (009 §8) is *already* "a short
description up front, read the rest of the files if you need them", which is exactly what an
ordinary filesystem affords.

Script files bundled in a skill run as ordinary programs whose **stdout enters the context, while
their source does not** — the token-efficiency property that makes skills worth having.

## 7. Prompt budget

The environment description is a **measured budget, not a style preference**:

| Element | Budget |
|---|---|
| Environment description (paths, what persists) | ≤ 60 tokens |
| Tool surface (names + one-line descriptions) | ≤ 30 tokens per tool |
| Per-skill advertisement (name + description) | ≤ 100 tokens |
| Sandbox/capability/safety architecture | **0 tokens** |

Enforced by a test that measures the assembled system prompt for a reference agent (022) and fails
when it grows. Prompt bloat is a regression like any other; without a gate it only ever increases.

## 8. Promotion gate

- **G1 (guessability)** — over a task corpus, an agent given only the §7 budget produces code whose
  first-attempt execution success rate meets a declared threshold; a control agent given a verbose
  architecture preamble does **not** do better. If the preamble wins, this RFC is wrong and gets
  rewritten.
- **G2 (token cost)** — the assembled prompt for the reference agent stays within §7, measured.
- **G3 (no leakage)** — across the hostile corpus, no host path, profile name, capability name,
  rule id, or host stack trace appears in any agent-visible string.
- **G4 (transparency is not security)** — the full 008/017 hostile suites are re-run against an
  agent that has been **explicitly told** it is sandboxed and given accurate architecture detail in
  its prompt. Containment results must be identical. This is the gate that proves §1a.
- **G5 (parity)** — every `agent.*` function enforces its capability; a call without the grant fails
  closed, proven per module with a positive control.
- **G6 (errors)** — each §3 cause produces the mapped exception with an actionable message and no
  architecture terms.

## 9. Open questions

- **Q1** — Whether `agent.spawn` belongs in the sandbox at all: it lets model-written code create
  runs, which is powerful and is also a recursion/cost hazard. Depth and budget bounds are
  necessary; whether they are sufficient is unproven.
- **Q2** — Non-actionable failure phrasing (§3) is the hardest part to get right: too vague and the
  agent retries forever, too specific and it becomes an architecture description.
- **Q3** — Whether the `tools` module should expose *all* tools or only those marked
  code-callable — 010 §6's registry is per-execution, and the two lists may reasonably differ.
- **Q4** — Whether to offer a JavaScript/TypeScript surface with the same library shape, given the
  same "model has seen a lot of it" argument applies.
- **Q5** — G1's threshold and corpus need to exist before this RFC can be promoted; without them the
  central claim of this document is an assertion.
