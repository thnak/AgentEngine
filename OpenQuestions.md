# Open Questions

Cross-cutting questions that no single RFC owns, or that block promotion of several. Per-RFC
questions stay in their RFC's §Open questions; this file is for the ones that would change the
shape of the project.

**Legend:** 🔴 blocks a v1 decision · 🟠 needed before implementation of its area · 🟡 can wait

---

## 🔴 OQ-1 — macOS and Quark's PAL

Quark has `linux_x86_64` and `windows_x86_64` PAL backends; **there is no macOS backend**. RFC 021
claims macOS as a Supported tier. Either the backend is contributed upstream (the clean answer,
and AgentEngine is the natural driver for it), or macOS support is reduced to a lower tier and the
support table says so. This is the largest single portability risk in the project.

**Owner:** unassigned. **Blocks:** 021 promotion, any macOS claim in README.

## 🔴 OQ-2 — Is the single-agent turn loop a workflow?

001 Q1 / 014 Q1. Making the turn loop a special case of the workflow graph buys one execution
model, one checkpointing story, one visualization, and one replay mechanism. It risks paying graph
overhead on the overwhelmingly common single-agent path. This decision shapes 001, 014, and 019 and
gets harder to reverse with every RFC that assumes the current split.

**Candidate resolution:** a design→prove loop measuring the overhead of a graph-of-one against the
direct loop under the 023 budgets.

## 🔴 OQ-3 — Do capabilities cross process boundaries as tokens?

007 Q1 / 008 Q4 / 018 Q2. The `remote` sandbox profile, remote plugins, and delegated A2A calls all
want to carry *attenuated* authority across a process or network boundary. A macaroon-style bearer
capability with caveats is the known answer; it adds a crypto dependency, a revocation problem, and
a new forgery surface. Without it, each remote path invents its own bespoke authority protocol —
which is worse.

## 🔴 OQ-4 — Unifying human-in-the-loop and long-running work

**Escalated from 🟠 after the A2A/AG-UI research**, which showed the three protocols do not merely
differ in encoding — they differ in *control flow*, and each demands a different correlation
identity:

| Protocol | Shape | Identity it requires |
|---|---|---|
| **MCP** `2026-07-28` | Client **retries the original request** with a *new* JSON-RPC id | `requestState` (opaque, client **MUST NOT** parse) |
| **A2A** v1.0 | Task **stays alive** in `INPUT_REQUIRED`; client sends a new message | `taskId` |
| **AG-UI** | Run **ends** with an interrupt outcome; client starts a **new run** | `interruptId` |

A retry, a continuation, and a restart. Our internal `InputRequired` (001 §2) must project to all
three while preserving whichever identity each peer will present on the way back — and AG-UI adds an
ordering obligation (state needed for resume must be emitted *before* the run-ending event) that has
no analogue in the other two.

The same problem recurs for long-running work: our `Suspended` state, MCP's `tasks` extension, A2A's
task lifecycle, and the workflow request port (014). Deferring risks four half-compatible mechanisms
and a correlation table nobody can reason about.

## 🟠 OQ-5 — Span-level taint

003 Q3 / 007 Q2 / 017 Q2. Per-part taint is what 003 specifies and it is coarse: a message that
mixes user text with a quoted tool result taints wholesale. Span-level taint would make
declassification and structural separation far more precise, at a real cost in the content model's
complexity and in every mapping layer.

## 🟠 OQ-6 — Plugin distribution

009 Q2. First-party registry versus OCI artifacts. OCI is content-addressed, signed, mirrorable, and
already deployed in every environment that would run this — which is a strong argument for not
building a registry. It also drags in an OCI client dependency and an authentication story.

## 🟠 OQ-7 — Wasmtime version pinning

009 Q4 / 021 Q3. WASI 0.3 ships enabled by default from Wasmtime 46; earlier versions need the
0.3 release candidate. The async ABI difference between 0.2 and 0.3 is not a small compatibility
surface, and the plugin ABI is supposed to be stable. Pin, or support a range?

## 🟡 OQ-8 — Second authoring surface

Python and .NET bindings are deferred (Specification §D3), with a C ABI as the intended seam
(020 Q3). Freezing that ABI shape too early constrains the bindings; too late and the bindings are
retrofitted onto whatever the C++ surface happens to be.

## 🟡 OQ-9 — Emerging protocols not designed against

A2UI (agent-generated declarative UI), AP2 (agentic payments), X42 (cross-boundary trust and
governance) are real and moving, and none is designed against here. 013's projection model means
A2UI is additive. AP2 and X42 would touch 007 and 018 structurally — payments in particular imply
`at-most-once` effects (019 §3) with real money attached.

## 🟡 OQ-10 — Evaluation of safety controls

017 Q4 / 022 Q2. The adversarial corpus is the project's only real evidence that the safety layers
work, and a corpus without an owner and a cadence decays into a fixed set of attacks that the code
has been tuned against.

## 🟠 OQ-12 — A second WASM runtime?

008 §1a rejects `wasm3` for the plugin ABI: it has only partial Component Model and WASI P2 support,
which is the one axis the ABI depends on. But its advantages are real — best-in-class cold start,
tiny footprint, portability down to microcontrollers — and they matter for two niches: ultra-short
high-frequency guest calls (a content filter evaluated per message) and constrained deployments where
Wasmtime's footprint is prohibitive.

A second runtime behind the same host interface means **two sandbox-escape surfaces and two
conformance stories**. That is a real cost, and the benefit is currently an assumption: the cited
cold-start comparison is generic, not measured on our workload. Blocked on a 023-budget measurement
before it is even a candidate.

## 🟠 OQ-13 — Worktree merge policy for concurrent agents

025 §4 fails a merge on conflict and surfaces it, retaining both versions. Two unresolved parts:
whether the *model* should be offered conflict resolution as a task (it is often capable, and it is
also a good way to lose work silently), and whether `shared` mode should be permitted at all for
concurrent siblings — single-writer serialization makes it *safe* but still means an agent's files
change under it between reads.

## 🟠 OQ-14 — In-sandbox library surface area

026 §5 makes the `agent` library the CodeAct action space, which means every module added widens both
what the agent can accomplish and the host's attack surface. `agent.spawn` is the sharpest case:
model-written code creating runs is powerful and is a recursion-and-cost hazard; depth and budget
bounds are necessary and not obviously sufficient. There is no principle yet for what earns a place
in the library beyond case-by-case justification.

## 🟠 OQ-16 — CodeAct has no discoverability story for its own granted surface

026 §4 gives `agent.tools` a real introspection story — generated docstrings, a `.pyi` stub,
`dir(tools)`/`help()` sourced from each Tool's declared metadata (006). That treatment stops at
`agent.tools`: the other seven `agent.*` modules (`files`, `data`, `memory`, `notes`, `output`,
`progress`, `ask`, `spawn`) have no equivalent, and nothing tells the model *which* top-level
modules are even present for this session before it tries one. §5's "an ungranted module is simply
absent" (I2's enforcement) has no discovery-side counterpart — today the only way to find out is
`import agent.spawn` and catch the failure. This is not OQ-14 (which is about what should be allowed
to *exist* in the library, i.e. curation) — it is about the model discovering what has already been
*granted*, a distinct question no existing OQ tracks.

**Candidate resolution, two parts, both sourced from the same run-start-resolved `CapabilitySet`/
Tool table (006 §6) so there is one source of truth, not two that can drift:**

1. **Pull side** — generalize 026 §4's existing pattern from `agent.tools` to the whole `agent`
   namespace: `dir(agent)` shows only modules granted this session, `help(agent)` gives a one-shot
   overview, every present module gets the same docstring/`.pyi` treatment `tools` already has. The
   smaller change — applying a pattern the spec already committed to, uniformly.
2. **Push side** — a short capability summary assembled into `instructions` at session start
   (002 §1/§2), extending 026 §7's existing token-budget line for "Tool surface (names + one-line
   descriptions)" from tools-only to the full action space, so the model doesn't burn a turn probing
   before it can act correctly at all.

Two open sub-questions a real design pass should resolve, not decided here: whether an ungranted
module should be listed explicitly ("`agent.spawn`: not granted") or simply omitted — explicit
listing costs more tokens but prevents more wasted attempts, and does not itself weaken I2 since
naming a capability is not granting it; and whether the push-side summary needs its own persistent,
re-readable artifact or whether pull-side `dir()`/`help()` already covers the "give me detail on
demand" case well enough to skip a third mechanism. **Naming caution:** whatever this becomes, it
should not be called or mounted as a "skill" (009 §8's vocabulary is for externally-authored,
versioned, distributable bundles) — an engine-generated, per-session capability manifest is a
different kind of artifact that would only confuse "the skill I loaded" with "the engine telling me
what I have" if it reused that name or mount point.

## 🟡 OQ-11 — Licence and governance

024 Q1/Q3/Q4. Licence (MIT assumed, matching Quark), release cadence versus Quark, ADR judging
authority, and a security disclosure process. All required before any public release.

---

## Resolved

### OQ-17 — No generic/first-party skills or tools catalog

006, 009 §7/§8, 026 §5. MAF ships no first-party generic skills or tools — every `SKILL.md` in its
repo is sample/demo content, not a shipped standard library — so a generic catalog for AgentEngine
was new design surface, not something to port.

**Resolved by candidate (a):** 009 §7 is now explicitly the generic tool catalog (source-agnostic,
capability-crossing operations only — math/JSON/unit-conversion stays un-Tooled per 026 §1's code
interpreter), headed by a first-party `read_content`-class tool whose preview-plus-`BlobRef` shape
(006 §7, 028 §2) exists specifically to close the token-budget hazard a naive "read this file" tool
would open. 009 §8f adds the generic-skills half: `using-the-code-interpreter`, `using-codeact`,
`reading-large-content`, `producing-structured-output`, `shell-pipelines`, shipped in-repo and
mounted by default under the same grant model as any skill. Related, still open: **OQ-14** (agent.*
library curation) is the same "what earns a place" question one level down, at the Python-surface
level rather than the tool/skill-catalog level.

### OQ-15 — Module-name import gating can't distinguish a trusted package's internals from guest code

Originally raised by `decisions/ADR-002-pythonrunner-embedding-and-mediation.md`'s prove phase
(2026-08-01): making `import numpy, pandas` work under `PythonRunner`'s import allowlist required
granting roughly 130 top-level module names, not 2 — including `ctypes`, `winreg`, `_wmi`, `_winapi`,
and `subprocess`, all transitively required by numpy's own platform-detection code, and all worked
examples (008 §1b, ADR-002) of names the mechanism exists to deny. The finder itself was not broken
(it enforces exactly the allowlist it's given, correctly) — the problem was granularity: it sees
*which module* is being imported, never *which code is asking*, so granting `numpy` also grants
guest code identical, indistinguishable access to `ctypes` directly.

**Resolved by `decisions/ADR-003-caller-aware-import-gating.md` (2026-08-01)**, candidate resolution
1 (caller-aware import gating): designed, red-teamed, revised twice (once after red-team, once after
an additional gap surfaced during independent prove-phase re-verification), implemented, and proven
against the real target (CPython 3.13.5, numpy 2.3.3, pandas 2.3.3, Windows/MSVC+clang) for the
gated-name set `ctypes`/`_ctypes`/`winreg`/`_wmi`/`_winapi`/`subprocess`. 010 §9 G1's flagship
NumPy+pandas success case may now be described as closed-by-construction for these six names
specifically, not merely kernel-jail-bounded, within the scope below.

**Not airtight — read ADR-003 §9 in full before relying on this for a new package policy.** §9.2/§9.3
name the residual risks explicitly: a gadget-chaining variant (§6.1) and a fail-closed C-reentrancy
question (§6.5) remain open; registry-pointer address-reuse safety (claim B12) is INCONCLUSIVE rather
than proven; and the mechanism has a demonstrated history — three independent misses across design,
red-team, and the initial prove pass, each finding a different unhandled entry point into CPython's
import machinery — of missing entry points, not a hypothetical risk. A future caller-gated name or
CPython version should be specifically re-verified, never assumed covered by the existing skip-anchor
set. Candidate resolution 2 (accept-and-document tiering, ADR-002 §10.2) remains the fallback for any
gated name or package this mechanism has not been checked against.
