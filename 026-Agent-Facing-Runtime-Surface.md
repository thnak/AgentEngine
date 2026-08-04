# 026 — Agent-Facing Runtime Surface

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Depends on:** 006, 007, 008, 010, 025 · **Gate:** §8

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
| Shell | An ordinary-looking shell — `cd`, `ls`, pipes, env vars, exit codes (010 §1) | That there is no real shell process: commands are parsed and dispatched to capability-gated primitives and other `Runner`s/`Tool`s, never resolved against a search path (010 §2) |
| Files | Files persist between executions | That persistence is a content-addressed commit per turn |
| Working directory / env | `cd` and env vars persist across calls, and are shared between Python and Shell (010 §3a) | That this is one `ExecState` object, or that `Runner`/`ExecState` are engine concepts at all |
| Network | Reachable hosts work; others fail like an unreachable host | Allowlists, egress proxy, capability names |
| Errors | `FileNotFoundError`, `PermissionError`, `TimeoutError` | Policy rule ids, capability names, profile fallbacks |
| Time/limits | Long work is interrupted like a timeout | Fuel, epochs, cgroup/Job Object accounting |

**No boilerplate.** The system prompt contains **no** sandbox description, **no** capability
enumeration, and **no** safety lecture. Where a constraint is *actionable* — "you can write to
`/work` and `/out`" — it is one short line, because that is task information, not architecture.

## 3. Error mapping

Failures reach the agent as the exception an ordinary program would get. The shell's equivalent is
an ordinary shell equivalent — a nonzero exit code and a `stderr` line ("no such file or directory",
"permission denied", "command not found") — never a host diagnostic or a policy identifier:

| Cause | Raised as |
|---|---|
| Path outside a mount, or write to a read-only mount | `PermissionError` / `FileNotFoundError` |
| Quota exhausted | `OSError` (`No space left on device`) |
| Host not permitted | ordinary connection failure (`socket.gaierror` / `ConnectionError`) |
| Wall-clock exceeded | `TimeoutError` |
| Memory exceeded | `MemoryError` |
| Tool denied by policy | the tool function raises `PermissionError` with a short, actionable message |
| Approval required | execution suspends (001 §2); the agent is not asked to reason about approval |
| Command not found (name resolves to neither a builtin nor a registered Runner/Tool) | nonzero exit + `stderr` line ("command not found") |

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

Generated from the same tool metadata as everything else (006 §1) — including whatever
`ShellRunner` dispatches to for the identical operation (010 §2, 006 §2's cross-frontend uniformity
rule) — so they cannot drift:

- **Real signatures** with real parameter names, defaults, and type hints from the tool's argument
  type.
- **Docstrings** from the tool description; `help()` works.
- **A `.pyi` stub** so the shape is inspectable and, in the `tools` module, discoverable by
  `dir()`.
- **Typed results** — dataclass-shaped objects, not raw dicts, so attribute access is guessable.

Every call still traverses the complete tool pipeline (006 §3) with the **sandbox's** capability set
(007 §6). Idiomatic surface, unchanged enforcement.

### 4a. The shell reads like a shell; underneath, it is not one

`execute_shell` (010 §1) takes an ordinary-looking shell command line — no bespoke flags, no
engine-specific builtins the agent must learn, and unrecognized commands fail with an ordinary
"command not found" (§3). The same "ordinary knowledge, not our API" argument from §1 applies: a
model has run `grep`, `ls -la`, `git status`, and a pipeline more times than it has seen any tool we
could design, and it is not told — because it does not need to be, and telling it would cost tokens
for no benefit (§1a) — that there is no `PATH`, no binary resolution, and no process being exec'd
underneath: every command is either a builtin over the worktree or a dispatch to a registered
`Runner`/`Tool` (010 §1a, §2). This is the transparency principle (§1) applied to the shell
specifically: the surface is honest about what works, silent about the architecture that makes it
safe.

What makes the shell worth having *alongside* the Python interpreter rather than only through
`subprocess` is the shared-state guarantee in 010 §3a: `cd`, exported environment variables, and the
worktree mounts are the **same `ExecState`** whether the agent reaches them from a shell command or
from Python. An agent that runs `cd /work/data && ls` in the shell and then opens a file with a
relative path in Python is not coordinating two subsystems — it is doing the same thing a person
does at one terminal with one Python REPL open beside it.

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
| `agent.memory` | Ordinary read access to the memory worktree's files under `/memory` (029 §2) — the ranked/on-demand view is `agent.tools.recall`, an ordinary tool (005 §5, 029 §5, resolved 029 §10 Q5) | `FsRead<mount>` on `/memory` |
| `agent.notes` | Durable notes across turns and sessions — ordinary writes into `/memory`, landing as `AgentAuthored` `MemoryItem`s (029 §4) | `FsWrite<mount>` on `/memory` |
| `agent.output` | Emit structured output conforming to the run's schema (003 §4) | — |
| `agent.progress` | Report progress on long work → run event stream (013 §1) | — |
| `agent.ask` | Ask the caller/user a question → `InputRequired` (001 §2) | — |
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

## 5a. What earns a place in `agent.*` (resolves OQ-14)

"Justified individually" above was, until now, a case-by-case judgment call with no stated test. A
candidate module earns a place only if it passes **both**:

1. **Capability fidelity** — it maps to one or more capabilities, each already in 007 §3's table
   (`agent.files` is the one module that needs two at once, `FsRead` and `FsWrite` together, still
   each individually already named there), or to none because it is a control primitive over the
   *run's own state* rather than an effect on anything outside it (`output`, `progress`, `ask` —
   ending the run's structured output, reporting progress, pausing for input are transitions in
   001's state machine, not authority over the world). A module is never the place a new,
   library-local authority class gets invented; if an operation needs authority 007 doesn't already
   name, the fix is a new capability in 007 §3, not a bespoke check inside a library function.
2. **In-process necessity** — the operation could not be served equally well as an ordinary Tool
   (006), reached generically through `agent.tools`, without defeating something CodeAct exists to
   provide. Two ways to clear this bar, and only two:
   - **(a) Run-intrinsic** — it touches the run's own control state, which no Tool has the standing
     to reach (minting a new run, ending the current one's structured output, pausing the current
     one for input, streaming a status update into the run's own event stream). `output`, `progress`,
     `ask`, `spawn` clear it this way.
   - **(b) Bulk/streaming necessity** — it operates over already-granted content in a loop, where
     routing each access through the tool-call JSON round-trip would defeat §5's stated point: "data
     stays out of the context window." `files`, `data`, `memory`, `notes` clear it this way; `memory`
     and `notes` are the same justification specialized to the `/memory` mount, not a second one.

A discrete, single-shot external effect that needs neither clears the bar for a **Tool** (arbitrary
capability, vetted through 006's pipeline like any other) but not for a **new top-level module** —
that is what keeps the library from growing by "this would be convenient" alone, which is the
concern Q1 raises about `agent.spawn` specifically and this principle raises about the library in
general. **This resolves what earns a place; it does not re-decide whether an admitted module's
bounds are sufficient** — `agent.spawn` passes this test cleanly (run-intrinsic, maps to
`AgentCall<agent>`) while whether its depth/budget bounds are *enough*, given the module belongs,
stays open as Q1.

**Correction found by applying the test:** §5's table listed `agent.ask` against a capability named
`Elicit`, which 007 §3's table has never defined — an ungoverned exception to rule 1 above. Brought
in line with `output`/`progress`: pausing for input is a run-intrinsic control transition (001 §2),
not an effect requiring its own capability grant, so the table now reads `—` for `agent.ask` like
its two siblings.

## 5b. Discovering the granted surface (resolves OQ-16)

§4 already gives `agent.tools` a real introspection story — generated docstrings, a `.pyi` stub,
`dir(tools)`/`help()` sourced from each tool's declared metadata. That treatment stopped at
`agent.tools`; the other modules in §5's table had no equivalent, and nothing told the model *which*
top-level modules were even present before it tried one — the only way to find out was
`import agent.spawn` and catch the failure. Two changes close the gap, both sourced from the same
run-start-resolved capability set (007) §5's table already keys module presence to, so there is one
source of truth rather than two that can drift:

- **Pull side.** §4's `agent.tools` pattern generalizes to the whole `agent` namespace: `dir(agent)`
  lists only modules granted this session, `help(agent)` gives a one-line-per-module overview, and
  every present module gets the same docstring/`.pyi` treatment `tools` already has. This is not new
  machinery — it is applying a pattern this RFC already committed to, uniformly, instead of stopping
  at one module.
- **Push side.** A short one-line-per-granted-module summary is folded into `instructions` at
  session start, extending §7's existing "Tool surface (names + one-line descriptions)" budget line
  from tools-only to the full action space — so the model does not have to spend a turn probing
  before it can act correctly. No new persistent artifact: `dir()`/`help()` already cover "give me
  detail on demand," and a third mechanism duplicating what pull-side already answers would cost
  tokens for information the model can already get, which §5's "small and boring" constraint rules
  out.

**An ungranted module is omitted, not listed as denied.** This follows §5's existing rule for the
same case ("an ungranted module is simply absent, which reads to the model as 'not available here'
rather than as a policy essay") rather than introducing a new policy — an explicit
`agent.spawn: not granted` line would itself be exactly the kind of capability enumeration §1
already rules out for the sandbox generally, spent on a module the agent cannot use regardless of
whether it knows the name.

**Naming:** this mechanism is engine-generated and per-session, not a mounted, authored, versioned
bundle — it must not be called or mounted as a "skill" (009 §8's vocabulary is reserved for
externally-authored, distributable content); doing so would confuse "the skill I loaded" with "the
engine telling me what I have."

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
| `agent.*` module surface (names + one-line purpose, §5b) | ≤ 20 tokens per granted module |
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
- **G7 (discoverability, §5b)** — `dir(agent)` and `help(agent)` reflect exactly the run's granted
  module set with no drift from the capability set that produced it; an ungranted module is absent
  from both, never listed as denied.

## 9. Open questions

- **Q1** — Whether `agent.spawn` belongs in the sandbox at all: it lets model-written code create
  runs, which is powerful and is also a recursion/cost hazard. Depth and budget bounds are
  necessary; whether they are sufficient is unproven. (§5a settles that `spawn` *earns a place* by
  the library-admission test; this question is about whether its bounds are enough, a narrower and
  still-open claim.)
- ~~**Q2** — Non-actionable failure phrasing (§3) is the hardest part to get right: too vague and the
  agent retries forever, too specific and it becomes an architecture description.~~ **Resolved: don't
  hand-tune wording — source it from real occurrences of the same exception class (2026-08-04):**
  this is §1's transparency principle applied to phrasing specifically, not a fresh judgment call
  per message. A model already knows, from ordinary Python experience, that a bare `ConnectionError`
  for a genuinely unreachable host is usually not worth blind-retrying while a `TimeoutError`
  sometimes is — that calibration is exactly the "ordinary knowledge it already has" property this
  RFC leans on everywhere else, and inventing bespoke non-actionable-sounding text would be
  re-solving a problem the exception *type* already solves. Message text is sourced from (and tested
  against, G6) a corpus of real-world instances of the same mapped exception class, never authored
  fresh per error site — removing the manual "too vague / too specific" balancing act by not asking
  anyone to strike it.
- ~~**Q3** — Whether the `tools` module should expose *all* tools or only those marked
  code-callable — 010 §6's registry is per-execution, and the two lists may reasonably differ.~~
  **Resolved, only what's granted for this execution — forced by existing rules, not a new choice
  (2026-08-04):** `agent.tools` reflects the per-execution `ToolCall<name>` grant (007 §3's existing
  per-invocation capability binding), never the agent's full declared tool set unconditionally — the
  same "ungranted is absent" rule §5 already states for whole modules, confirmed here to apply
  per-tool too. This is what makes §5's own claim true rather than aspirational: "high-consequence
  effects belong on the channel that can be reviewed atomically" only holds if a tool an operator
  wants reviewed one-call-at-a-time can be *excluded* from the bundled-approval CodeAct grant — an
  operator withholds `ToolCall<name>` for that tool from CodeAct executions specifically (while still
  granting it for the ordinary per-turn channel), and it's simply absent from `agent.tools`. No new
  mechanism; 010 §6's per-execution registry and 007 §3's per-invocation binding already jointly
  determine this.
- ~~**Q4** — Whether to offer a JavaScript/TypeScript surface with the same library shape, given the
  same "model has seen a lot of it" argument applies.~~ **Resolved, deferred, inheriting 010 §2's
  already-stated Python-first priority (2026-08-04):** 010 §2 already designs JS/TS as a future
  `execute_code` language option, explicitly lower priority than Python, and nothing about a JS
  runtime embedding is otherwise specced anywhere in this project (unlike Python, which has the whole
  of 010). Building a JS-idiomatic `agent.*` surface before the runtime embedding itself exists would
  be speculative work with no foundation under it. When it is designed, §5a's admission test
  (capability fidelity + in-process necessity) applies unchanged — a JS surface offers the same
  modules for the same reasons, in JS idiom, not a fresh design question.
- **Q5** — G1's threshold and corpus need to exist before this RFC can be promoted; without them the
  central claim of this document is an assertion.
