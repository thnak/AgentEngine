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
  agent can never exceed its parent. Target design; not yet a real call path. `agent.spawn` has no
  implementation to wire a sub-worktree to today — its depth-budget half is proven, in isolation
  from any real spawn (`decisions/ADR-006-agent-spawn-depth-budget-bound.md` §9), and its cost-pool
  half now has a real, tested actor primitive too (`decisions/ADR-031-spawn-cost-budget-actor-
  primitive.md`) — but neither is wired to an actual nested-agent-run invocation mechanism, which
  still does not exist anywhere in this codebase. `AgentSession` DOES now own a real production
  tool-call loop (`decisions/ADR-027-agent-session-tool-call-loop.md`, closing the gap this line
  used to name), but that loop has no `agent.spawn` case — a spawn call would still have nothing to
  invoke. Full trace: `docs/architecture/worktree-sharing-skills-and-subagents.md` §3.

**The trade this makes explicit:** a richer library means the agent can do more per execution
(fewer round trips, less token spend, better results) *and* a wider host attack surface. Each module
is therefore justified individually, capability-gated individually, and testable individually —
rather than shipping one `agent.engine` god-object that grows without review.

**The curation rubric (resolves OQ-14).** "Justified individually" was correct but not falsifiable
— it let any module in as long as *someone* argued for it. A candidate module must instead pass all
four:

1. **Maps to exactly one thing crossing the trust boundary** — either a single named 007 capability
   class (an effect that needs a grant), or a zero-capability reporting channel back into the run's
   own event stream/output schema (`output`, `progress` — nothing to gate because nothing leaves the
   run). A candidate that doesn't cleanly fit either shape is scope creep, not a missing module.
2. **Removing it forces a strictly worse channel for a task class this project already commits to
   supporting** — not "would be convenient somewhere," a concrete regression: more model round
   trips, a large result forced through the context window (§5's own "data stays out of the context
   window" argument), or a capability the 006 tool pipeline already exists to gate, reinvented.
3. **Not expressible as an ordinary combination of the other granted modules.** If it would just be
   a two-line wrapper over another module's existing calls, it isn't a module. (`agent.notes` passes
   this one narrowly: it isn't `agent.files.write` under a different mount, because notes must land
   as `AgentAuthored` `MemoryItem`s (029 §4) — a structural tag `agent.files` has no way to apply.)
4. **Every symbol still passes §5's "guessable from its name" bar on its own** — a module that only
   earns its place via 1–3 but whose functions need a paragraph of explanation fails here and must
   be redesigned, not shipped with worse docs to compensate.

**Applied to the current nine** (§5's table): all pass, each for a different reason worth stating
rather than assuming — `tools`/`files`/`data`/`memory`/`notes`/`ask`/`spawn` each map to exactly one
named capability class (rule 1's first branch); `output`/`progress` are the zero-capability
reporting case (rule 1's second branch); `notes` clears rule 3 as shown above; none needed rule 4 to
fail and be cut.

**Applied to a plausible rejected candidate**, so the rubric is shown to discriminate rather than
rubber-stamp: a hypothetical `agent.email` module wrapping SMTP/IMAP directly fails rule 3 — email
is already reachable via stdlib `smtplib`/`imaplib` (010 §10; a granted `NetOut` capability is all
either needs) or, for a richer provider API, via an ordinary registered `Tool` through `agent.tools`
— a dedicated module would just be a two-line wrapper over calls already available through an
existing module, exactly the case rule 3 exists to reject
(`docs/planning/v1-office-user-toolkit.md` §2 already reaches this conclusion independently, for the
same reason).

### 5a. Discovering what's granted (resolves OQ-16)

§4 gives `agent.tools` a real introspection story — docstrings, a `.pyi` stub, `dir()`/`help()`.
Nothing before this section said what happens for the other seven modules, or told the model which
top-level modules are even present before it tries one. Two parts, both sourced from the same
run-start-resolved `CapabilitySet` (007 §6) so pull side and push side cannot drift from each other —
prototyped and proven in `include/agentengine/trust/agent_library_manifest.hpp`:

- **Pull side** — §4's `dir()`/`help()` pattern generalizes from `agent.tools` to the whole `agent`
  namespace: `dir(agent)` shows only modules granted this session; every present module gets the
  same docstring treatment `tools` already has.
- **Push side** — a short capability summary assembled into `instructions` at session start (002
  §1/§2), extending §7's existing "Tool surface (names + one-line descriptions) ≤ 30 tokens/tool"
  line from tools-only to the full action space, so the model doesn't burn a turn probing before it
  can act correctly at all.

**The two sub-questions OQ-16 named, decided:**

- **An ungranted module is omitted, not listed as denied.** §1a's "we omit architecture, we do not
  lie" already sets this precedent elsewhere in this RFC; explicit denial-listing costs tokens per
  §7's budget discipline for a benefit pull-side `dir()`/`help()` already covers cheaply — a model
  that tries `import agent.spawn` anyway gets an ordinary, instant `ImportError`, not a wasted turn.
- **No third, persistent artifact.** The push-side summary is injected once at session start, the
  same way §7 already injects the tool surface; pull-side `dir()`/`help()` is queryable at any point
  during the run at zero additional prompt cost (it is Python introspection, not a prompt insertion),
  which already covers "give me detail on demand" without a separate mechanism to keep in sync with
  the other two.

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
| `agent.*` module surface (names + one-line purpose, §5a) | ≤ 20 tokens per granted module |
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
- **G7 (discoverability, §5a)** — `dir(agent)` and `help(agent)` reflect exactly the run's granted
  module set with no drift from the capability set that produced it; an ungranted module is absent
  from both, never listed as denied.

## 9. Open questions

- **Q1** — Whether `agent.spawn` belongs in the sandbox at all: it lets model-written code create
  runs, which is powerful and is also a recursion/cost hazard. Depth and budget bounds are
  necessary; whether they are sufficient is unproven. **Partially resolved by
  `decisions/ADR-006-agent-spawn-depth-budget-bound.md`**: the depth half is proven sufficient
  against unbounded recursion, conditional on the effect-mediation boundary (006 §9 G4) holding —
  see that ADR for the exact scope. The cost half (token spend per spawned run — wall-clock is a
  further, separately-deferred dimension, see below) started as a **Draft design sketch (2026-08-11,
  designed and red-teamed, deliberately NOT an ADR at the time — no code, no tests yet)**, and is now
  **resolved as real, tested code the same day**: `decisions/ADR-031-spawn-cost-budget-actor-
  primitive.md` (Proposed) turned this sketch into `SpawnCostBudgetActor`, matching the corrected
  shape this sketch calls for below. What ADR-031 does NOT claim, so this isn't overstated: `agent
  .spawn` itself still has zero real call path anywhere in this codebase — no nested-agent-run
  invocation mechanism exists for the cost-pool primitive to gate. The design reasoning below is kept
  as the record of why the shape is what it is:

  - **Correction first**: ADR-006 and `trust/spawn_budget.hpp`'s own comments cite "023's budgets"
    as where a spawn's cost ceiling would come from. That's a miscitation — 023 is entirely engine
    *overhead* regression benchmarks (allocations, atomics, dispatch, cold-start), with no section
    defining per-run or per-spawn model-usage cost policy. The actually-relevant, already-real
    mechanism is 001 §4/`TokenBudget<N>` (`agent_session.hpp`'s `run_tokens_consumed_` accounting,
    real and enforced, "exceeded → Resource failure at the turn boundary") plus 007 §3 rule 2
    (attenuation-only — already general enough to cover a cost dimension, not depth-specific).
  - **A naive extension is actively wrong, not just incomplete.** The obvious move — add a
    `remaining_tokens_` field to `SpawnBudget` alongside `remaining_depth_`, attenuated the same
    way — inherits a bug from copying depth's pattern onto a semantically different quantity.
    `SpawnBudget::attenuate_for_spawn()` is a pure, `const`, immutable-value-type call precisely
    because depth is a **ceiling** (ordinary for two concurrent siblings to each independently
    compute `parent_depth − 1` off the same parent — both should get it). Tokens are a
    **consumed pool**, not a ceiling: two concurrent `agent.spawn` calls (concurrency this engine
    already supports — `Parallelizable`, 006 §6b; `Concurrent`/map-reduce fan-out, 014 §3) each
    independently attenuating against the same `remaining_tokens_` snapshot could each succeed up
    to the full remaining amount — a double-spend that defeats the mechanism's entire purpose.
    This is not hypothetical; it follows directly from capability handles materializing
    independently per invocation (007 §3 property 3).
  - **The corrected shape**: a separate `SpawnCostBudget` type — not fused into `SpawnBudget` — so
    depth's ceiling semantics and tokens' consuming-pool semantics stay orthogonally provable (and
    so a future wall-clock dimension, whose semantics are closer to depth's narrowing than to
    tokens' consuming, doesn't get forced through the wrong primitive). Attenuation against the
    pool must be serialized through the actor that owns the budget (matching Quark's actor model),
    not a bare value-type call — closing the concurrent-double-spend gap requires a concurrency
    red-team test in the style of ADR-006's own S-R1/S-R2 (exhaustive/repeated-attempt proof), not
    just the single-call unit tests depth's proof used. The token amount a child receives must be
    sourced only from that agent's own compiled `AgentMetadata::token_budget` (never from anything
    a model's own output could set, I2/I3) — a wider parameter surface than depth's zero-argument
    design, worth its own explicit mediation-boundary comment when built, not silently inherited
    from ADR-006's depth-specific argument.
  - **Wall-clock stays fully, explicitly open** — no existing per-run deadline-enforcement
    mechanism exists to attenuate against (001's turn-loop prose names "deadline" as a guard;
    `agent_session.hpp` implements none of it, unlike `TokenBudget<N>`). Not folded into "cost
    half resolved" language — naming it separately is the point.
  - **Done:** this sketch became a real ADR the same day (design → red-team → prove, real code +
    tests, per this project's own ADR discipline — `decisions/README.md`; `decisions/ADR-031-
    spawn-cost-budget-actor-primitive.md`, Proposed, awaiting the project owner's explicit "Judged").
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
  central claim of this document is an assertion. **Still open — a methodology skeleton exists
  (2026-08-11, designed and red-teamed; no ADR needed, this is not a security/hot-path/capability
  choice, but the corpus, threshold, and an actual G1 run do not exist yet, so Q5 stays open until
  they do):**

  - **Instantiates 022 §4's existing evaluation framework** for G1's specific claim, rather than
    inventing a parallel one: task corpus (inputs, environment fixtures) + grader + pre-registered
    decision rule, the same three parts 022 §4 already names.
  - **Dual metric, not success-rate alone.** The first-draft protocol graded only first-attempt
    execution success rate — but 022 §4 already lists **token cost** as a named Metric alongside
    success rate, and §1 of this RFC gives token economics as reason #2 (of 3) for the whole
    terse-environment design. A verbose-preamble control that ties the reference agent on success
    rate while costing several times more tokens per attempt is exactly the outcome this RFC's own
    rationale says should count as a loss for the preamble — a success-rate-only decision rule
    would score that as a pass. Both metrics are pre-registered, co-equal comparison axes.
  - **Threshold set pilot-then-confirm, not zero-data.** 022 §4 bars *post-hoc* threshold
    selection ("Post-hoc threshold selection is how a regression ships") — it does not bar a pilot
    phase informing a threshold subsequently locked before a separate confirmatory run. A number
    picked with no pilot data at all is either unfalsifiably easy or unreachable by construction; a
    pilot establishes the achievable range first, then the threshold is frozen before the
    confirmatory corpus run that actually decides G1.
  - **Corpus grows on its own trigger, not a borrowed one.** An earlier draft of this sketch cited
    022 §5's injection-corpus growth cadence ("grows from real findings... every 024 §4.2 cycle
    touching 007/008/017/018") as precedent — that cadence is keyed to security-ADR judging on
    four specific RFCs and doesn't actually describe this corpus's domain. G1's corpus is a
    UX/prompt-economics corpus; its own trigger is a new `agent.*` module clearing 026 §5's
    curation rubric, or an observed real guessability failure in eval or production — named
    explicitly rather than reusing language that doesn't fit the underlying event.
  - **What this sketch does not do**: build the corpus, pick the actual threshold number, or run
    G1. Mechanics-now, numbers-later is consistent with how `023-Performance-Targets-and-Budgets.md`
    §8 resolved its own methodology-only questions (Q2: "a methodology fix, not a number change") —
    but that precedent stops at mechanics; Q5's own text ("without them the central claim... is an
    assertion") still governs the threshold and the corpus itself.
