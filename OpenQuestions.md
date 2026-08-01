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

## 🟡 OQ-11 — Licence and governance

024 Q1/Q3/Q4. Licence (MIT assumed, matching Quark), release cadence versus Quark, ADR judging
authority, and a security disclosure process. All required before any public release.

---

## 🔴 OQ-15 — Module-name import gating can't distinguish a trusted package's internals from guest code

Raised by `decisions/ADR-002-pythonrunner-embedding-and-mediation.md`'s prove phase (2026-08-01),
which is the first place in this project an actual security mechanism was measured against a real
dependency instead of a synthetic example. The result: making `import numpy, pandas` work at all
under `PythonRunner`'s import allowlist required granting roughly 130 top-level module names, not
2 — including `ctypes`, `winreg`, `_wmi`, `_winapi`, and `subprocess`, all transitively required by
numpy's own platform-detection code. `ctypes` was 008 §1b's and ADR-002's own worked example of a
name the mechanism exists to deny.

**The mechanism (a `sys.meta_path` finder gating by module name) is not broken — it enforces
exactly the allowlist it's given, correctly (ADR-002 §9's A1/A3/A4 verdicts).** The problem is
granularity: the finder sees *which module* is being imported, never *which code is asking*. Once
an operator grants `numpy`, guest code writing `import ctypes` directly is indistinguishable from
numpy's own internals doing the same thing, and gets the identical access.

This means 010 §9 G1's own flagship success case — "a scripted data task using NumPy + pandas
produces a chart artifact" — is exactly the case where the interpreter-level import allowlist does
**not** provide the "closed by construction" property 008 §1b claims for it. For that policy tier,
008 §1b's kernel jail (layer 3) is the real boundary against guest code directly abusing
`ctypes`/`subprocess`/`winreg`, not a documented residual risk sitting behind a boundary that mostly
holds.

**Candidate resolutions, neither designed yet (ADR-002 §10.2):**

1. **Caller-aware import gating** — the finder inspects the calling frame's `__name__` and permits
   `ctypes`-class names only when the importer is already inside a granted package's own namespace,
   denying the identical import from guest/`__main__` code. Raises the bar substantially; not
   airtight against a sufficiently deliberate guest program manipulating its own namespace, and
   would need its own red-team pass before being trusted as load-bearing.
2. **Accept and document the tiering** — treat `preinstalled: numpy+pandas`-class policies as
   explicitly higher-trust than stdlib-only policies, disclose the specific ancillary access they
   grant, and lean on the kernel jail deliberately for that tier rather than by accident. Costs
   nothing to build; is a policy and documentation decision (010 §5), not a new mechanism.

**Owner:** unassigned. **Blocks:** 010 §9 G1's promotion for any non-trivial package policy, and any
claim that `preinstalled` policies beyond stdlib-only are "closed by construction" rather than
"kernel-jail-bounded."

---

## Resolved

*(none yet — this section records questions closed by an ADR, with the ADR reference)*
