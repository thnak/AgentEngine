# Open Questions

Cross-cutting questions that no single RFC owns, or that block promotion of several. Per-RFC
questions stay in their RFC's §Open questions; this file is for the ones that would change the
shape of the project.

**Legend:** 🔴 blocks a v1 decision · 🟠 needed before implementation of its area · 🟡 can wait

---

## 🟠 OQ-5 — Span-level taint

003 Q3 / 007 Q2 / 017 Q2. Per-part taint is what 003 specifies and it is coarse: a message that
mixes user text with a quoted tool result taints wholesale. Span-level taint would make
declassification and structural separation far more precise, at a real cost in the content model's
complexity and in every mapping layer.

## 🟠 OQ-6 — Plugin distribution

009 Q2. First-party registry versus OCI artifacts. OCI is content-addressed, signed, mirrorable, and
already deployed in every environment that would run this — which is a strong argument for not
building a registry. It also drags in an OCI client dependency and an authentication story.

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

### OQ-7 — Wasmtime version pinning

009 Q4 / 021 Q3. WASI 0.3 ships enabled by default from Wasmtime 46; earlier versions need the
0.3 release candidate. The async ABI difference between 0.2 and 0.3 is not a small compatibility
surface, and the plugin ABI is supposed to be stable. Pin, or support a range?

**Resolved 2026-08-03 by a small, concrete build-and-check** (not a design debate, per the project
owner's direction): **pin to a single version, currently 47.0.3** (the latest release as of this
date — v46 has already been superseded), no 0.2/0.3-RC range. Evidence: downloaded the real,
official `wasmtime-v47.0.3-x86_64-windows-c-api.zip` release directly from GitHub; confirmed the
shipped headers contain full Component Model support (`include/wasmtime/component/*`) and WASI
0.3's async primitives (`stream`/`future` types referenced in `component/func.h`,
`component/linker.h`, `component/types/val.h`) — grounded in the actual downloaded artifact, not
documentation; compiled and linked a minimal C++ smoke test against `wasmtime.dll.lib` with MSVC
19.51.36252 (the same toolset the rest of this project builds with) and ran a real
engine→store→module→instance→function-call round trip, which returned the correct result. A range
was rejected because there is no existing deployment depending on an older pin (design phase, no
external users) and because testing both 0.2 and 0.3 async ABI paths for zero present benefit is
exactly the "not a small compatibility surface" cost the question itself flagged — a straight
`GIT_TAG`/version pin (matching the existing `FetchContent` pattern already used for `nlohmann_json`
in `tests/CMakeLists.txt`) is simpler and sufficient. **Not attempted:** actually instantiating a
real `.wasm` component (as opposed to a core module) through the `wasmtime_component_*` APIs — that
needs `wasm-tools`/`cargo-component` to produce a component binary, out of this small prove's scope;
the Component Model support claim rests on header inspection plus the module-level round trip, not
a component-level execution.

### OQ-3 — Do capabilities cross process boundaries as tokens?

007 Q1 / 008 Q4 / 018 Q2. The `remote` sandbox profile, remote plugins, and delegated A2A calls all
want to carry *attenuated* authority across a process or network boundary. A macaroon-style bearer
capability with caveats is the known answer; it adds a crypto dependency, a revocation problem, and
a new forgery surface. Without it, each remote path invents its own bespoke authority protocol —
which is worse.

**Resolved 2026-08-03 by `decisions/ADR-005-capability-bearer-tokens-cross-process.md`**, a small
prove (per the project owner's direction: real C++23, red-teamed, not a full-scale build) rather
than a design→prove loop measuring overhead first: **yes, narrowly.** A self-verifying HMAC-chained
bearer token (`trust/capability_token.hpp`) is accepted for the `ExpiresAt`/`PathPrefix` caveat
classes proven there — attenuation-only and forge-resistant under red-team (bit-flip, field tamper,
caveat-strip, caveat-reorder, fabricated-parent derivation all rejected; clean under MSVC ASan, zero
findings). The anticipated revocation problem is real and was not solved inside the token: a minted
token is valid until its own caveats lapse, with no way to unmint it early, so any capability needing
immediate revocation should use the ADR's other proven design (a host-side `CapabilityRegistry`)
instead, or a short-lived token re-minted frequently. The anticipated performance win was **not**
established — measured **INCONCLUSIVE**, favoring the registry in this pass's specific (unoptimized,
same-process) measurement — see the ADR §6-§9 before assuming either design is faster. Windows-only
for now (021 §2); a Linux HMAC backend is unbuilt and named as a residual risk, not urgent given the
Windows-now/Linux-next sequencing (OQ-1).

### OQ-2 — Is the single-agent turn loop a workflow?

001 Q1 / 014 Q1. Making the turn loop a special case of the workflow graph would buy one execution
model, one checkpointing story, one visualization, and one replay mechanism, at the risk of paying
graph overhead on the overwhelmingly common single-agent path.

**Resolved 2026-08-03, by grounding in MAF's own source rather than a design→prove overhead
measurement** (candidate resolution originally proposed): **no, keep them separate.**
`docs/research/2026-08-03-maf-workflow-and-hitl-model.md` §1 shows MAF's `agent.run()` never touches
`Workflow`/`Executor` machinery, and the dependency direction is the opposite of "agent is a
special-cased workflow" — `AgentExecutor` wraps `agent.run()` to let an agent opt into being one node
of a graph, not the reverse. Since AgentEngine's developer model is deliberately MAF-shaped
(CLAUDE.md), this settles the question by precedent rather than by re-deriving it from an overhead
benchmark: 001 §3's turn loop stays its own lightweight coroutine; 014 §1's `Executor = an agent | a
function | a sub-workflow | a request port` already has the right shape for the opt-in case.

### OQ-4 — Unifying human-in-the-loop and long-running work

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
no analogue in the other two. The same problem recurs for long-running work: our `Suspended` state,
MCP's `tasks` extension, A2A's task lifecycle, and the workflow request port (014).

**Resolved 2026-08-03**: `InputRequired`/`Suspended` carry one internal `request_id`-shaped
correlation token — 001 §2 — matching MAF's own `request_info`/checkpoint mechanism exactly
(`docs/research/2026-08-03-maf-workflow-and-hitl-model.md` §2). Each protocol's identity
(`requestState`/`taskId`/`interruptId`) is a **projection** of that one token at its boundary, not a
parallel identity to keep synchronized — following MAF's own AG-UI bridge, which reuses one
`request_id` as both an interrupt `id` and a `tool_call_id` simultaneously without conflict. For A2A
specifically, `taskId` maps to the run/session identity (it outlives `INPUT_REQUIRED`), with the
per-request token carried as task metadata for the case of multiple outstanding requests against one
task (012 §5a). Updated: `001-Execution-Model.md` §2, `014-Workflow-and-Orchestration.md` §4,
`012-A2A-Conformance.md` §5a, `013-UI-and-Streaming-Surfaces.md` §2.

### OQ-1 — macOS and Quark's PAL

Quark has `linux_x86_64` and `windows_x86_64` PAL backends; there was no macOS backend, and RFC 021
claimed macOS as a Supported tier anyway — the largest inconsistency between claim and reality in
the project.

**Resolved 2026-08-03 (project-owner decision): macOS is not a target, full stop** — no PAL backend
will be contributed upstream for it, and no RFC may claim macOS support. Delivery is explicitly
sequenced rather than simultaneous: **Windows x86-64 is the v1 target and the only platform under
active implementation now; Linux x86-64 is the next target, taken up once the Windows
implementation reaches a stable state.** This replaces the "contribute upstream vs. downgrade the
tier" framing the question was originally posed with — neither candidate resolution was taken;
macOS is dropped outright rather than downgraded to a lower tier.

Updated to match: `021-Platform-Support-and-Portability.md` §2 (target matrix), §3 (per-subsystem
table), §5 (CI matrix), §6 (G1), §7 (this question); `CONVENTIONS.md` Target & scope;
`AgentEngineSpecification.md` (portability property, WASM-artifact claim); `008-Sandbox-and-
Isolation.md` (goal statement, locked-decision bullet, `wasm`/`native-jail` profile table, G1 gate);
`009-Plugin-and-Extension-System.md` (goal statement, G1 gate); `010-Python-Code-Interpreter.md`
(shell-portability discussion, G1 and G6 gates); `024-Versioning-Compatibility-and-Governance.md`
Q2; `025-Worktree-and-Virtual-Filesystem.md` G2; `README.md` (plugin-portability line);
`src/backends/native_jail/README.md` and `src/backends/wasm/README.md`. `decisions/ADR-001/002/004`
and the dated `docs/research/*.md` notes are left as-is — they are historical records of what was
actually run/researched, not live claims.

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
