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

## 🟠 OQ-4 — Unifying long-running work

001 §2 / 006 Q2 / 019 Q2. Four shapes for one idea: our `Suspended` run state, MCP's `tasks`
extension, A2A's task lifecycle, and the workflow request port. `InputRequired` is already unified
across three surfaces; long-running work is not. The risk of deferring is four half-compatible
mechanisms.

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

## 🟡 OQ-11 — Licence and governance

024 Q1/Q3/Q4. Licence (MIT assumed, matching Quark), release cadence versus Quark, ADR judging
authority, and a security disclosure process. All required before any public release.

---

## Resolved

*(none yet — this section records questions closed by an ADR, with the ADR reference)*
