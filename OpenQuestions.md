# Open Questions

Cross-cutting questions that no single RFC owns, or that block promotion of several. Per-RFC
questions stay in their RFC's §Open questions; this file is for the ones that would change the
shape of the project.

**Legend:** 🔴 blocks a v1 decision · 🟠 needed before implementation of its area · 🟡 can wait

**No open cross-cutting questions remain as of 2026-08-04.** OQ-1 through OQ-17 are all resolved
(most recently OQ-8/OQ-9/OQ-10/OQ-11, closing out the 🟡 "can wait" tier). New questions are added
here as they're identified; per-RFC open questions that don't change the shape of the project stay
in their own RFC's §Open questions and are never promoted here by default.

---

## Resolved

### OQ-11 — Licence and governance

024 Q1/Q3/Q4. Licence (MIT assumed, matching Quark), release cadence versus Quark, ADR judging
authority, and a security disclosure process. All required before any public release.

**Resolved, three parts (2026-08-04):**

1. **Licence** — MIT, confirmed by the project owner, matching Quark exactly. `LICENSE` written at
   the repo root.
2. **Governance/quorum** — not invented speculatively for a contributor population that doesn't
   exist: git state is local-only, no remote, no outside contributors, so the project owner is
   unambiguously the ADR judge (024 §4) today. A quorum rule is owed once the project actually takes
   outside contributions — new design work done at that time, matching Quark's governance if Quark
   has settled one by then, a fresh minimal default otherwise. Nothing about promoting an RFC or
   landing an ADR is blocked by this in the meantime.
3. **Security disclosure** — `SECURITY.md` written at the repo root: private reporting channel
   (placeholder contact pending a public remote), acknowledgement/disclosure timeline, the existing
   024 §3 security-exception carve-out cross-referenced, and OQ-10's corpus-publication split
   (classification public, payloads private) folded in as the disclosure policy's publication rule.

Full text: 024 §1 (C ABI versioning row, incidental to this pass), §7 Q1/Q3/Q4; `LICENSE`;
`SECURITY.md`.

### OQ-10 — Evaluation of safety controls

017 Q4 / 022 Q2. The adversarial corpus is the project's only real evidence that the safety layers
work, and a corpus without an owner and a cadence decays into a fixed set of attacks that the code
has been tuned against.

**Resolved by inheriting an existing mechanism rather than inventing a new one (2026-08-04):**

- **Cadence** — every 024 §4.2 design→red-team→prove→judge cycle touching 007/008/017/018 already
  produces adversarial attempts as part of judging its ADR; those attempts become corpus entries by
  default when the ADR is judged. The corpus grows exactly as often as security-relevant design work
  happens, with a quarterly backstop check so a quiet quarter doesn't let it go stale.
- **Owner** — the same authority that judges an ADR (024 §7 Q3's resolution: the project owner,
  while solo). One authority, not two, so corpus review carries the same enforcement weight as the
  design process feeding it.
- **Publication** — classification (categories, counts, which gate each maps to) is publishable and
  CI-generated, matching 021 §2's honesty-requirement pattern; the payloads themselves stay private
  until the fix proven effective against them ships, mirroring standard CVE disclosure practice.
  Written into `SECURITY.md` as the disclosure policy's publication rule, not tracked separately.

Full text: 017 §9 Q4, 022 §5, 022 §8 Q2, `SECURITY.md`.

### OQ-9 — Emerging protocols not designed against

A2UI (agent-generated declarative UI), AP2 (agentic payments), X42 (cross-boundary trust and
governance) are real and moving, and none is designed against here. 013's projection model means
A2UI is additive. AP2 and X42 would touch 007 and 018 structurally — payments in particular imply
`at-most-once` effects (019 §3) with real money attached.

**Resolved, three different answers for three different protocols (2026-08-04):**

1. **A2UI** — already closed, not merely deferred: 013 §3's projection architecture makes adoption
   additive by construction (AG-UI is already a projection of the internal event stream; A2UI is
   another one). No invariant or mechanism needs to preemptively account for it.
2. **AP2** — the architecture already has the right-shaped attachment point and doesn't need to
   change to accommodate it later: a payment mandate is an ordinary `Capability` (007 §3), and
   019 §3's `at-most-once` effect classification already anticipates non-idempotent external effects
   generally, payments included. What's missing is AP2-specific knowledge (mandate semantics, its own
   conformance suite) — genuinely new spec-writing once the protocol is in front of us, not an
   invariant change, and not done speculatively without dated research on its actual wire shape
   (CLAUDE.md's research-is-dated-and-cited rule).
3. **X42** — folded into 018 §8 Q1 rather than tracked separately: cross-boundary agent trust is
   already an open question there (SPIFFE-style workload identity vs. OAuth-only), and X42, if
   adopted, is a candidate *answer* to that question — another inbound-identity row feeding the same
   `Principal` (007 §2) — not a new mechanism sitting outside 018's model.

**Explicitly not done:** designing AP2 or X42's actual mechanics. This resolution closes what the
*architecture* owes them (an attachment point that already exists and is sufficient in shape); it
does not design either protocol, which stays out of scope until one is dated, cited research away
from being real.

Full text: 013 §3, 018 §8 Q1.

### OQ-8 — Second authoring surface

Python and .NET bindings are deferred (Specification §D3), with a C ABI as the intended seam
(020 Q3). Freezing that ABI shape too early constrains the bindings; too late and the bindings are
retrofitted onto whatever the C++ surface happens to be.

**Resolved, design early against stable concepts, freeze late against a real consumer (2026-08-04):**
the dilemma assumed the C ABI is a mechanical export of the C++ authoring surface (002's CRTP
templates), so its timing had to either anticipate or trail that surface. It can't be that: templates
have no ABI to export across a C boundary, so the C ABI was always a small, hand-designed seam over a
handful of already-stable concepts — `StartRun`/`ask_stream` and the `RunEvent` drain loop (020 §3a),
config resolution (020 §2), capability grants (007 §3) resolved to opaque handles — none of which
move the way a template signature would. Design work can start now, scoped to exactly that list.
Freezing is what waits, gated on a **reference out-of-process consumer** (a prototype Python or .NET
binding, revised at least once against what it exposed as wrong) before 024 §2's no-source-break
promise applies — the same argument-alone-does-not-reach-Proven discipline 024 §4 already applies
generally. The C ABI is now its own versioned artifact (024 §1), independent of "Engine" (020 §3a's
source-level, not binary-stable, embedding contract) — conflating the two was part of what made the
timing feel harder than it is.

Full text: 020 §8 Q3, 024 §1, Specification §D3.

### OQ-13 — Worktree merge policy for concurrent agents

025 §4 fails a merge on conflict and surfaces it, retaining both versions. Two unresolved parts:
whether the *model* should be offered conflict resolution as a task (it is often capable, and it is
also a good way to lose work silently), and whether `shared` mode should be permitted at all for
concurrent siblings — single-writer serialization makes it *safe* but still means an agent's files
change under it between reads.

**Resolved, both parts (2026-08-04):**

1. **Model-assisted resolution** — split proposing from confirming rather than treating it as one
   decision. §4's "never resolved by guessing" is about silent resolution, not about who may draft a
   proposal: a model may draft a merge from both versions plus the common ancestor, but writing that
   draft back as the accepted tree stays exactly what §4 already specified — surfaced, confirmed by
   the supervising agent or a human, gated like any other write (007, 006 §4). Escalation-by-default
   is unchanged; a drafting step was added ahead of it, never a shortcut around it.
2. **`shared` mode for concurrent siblings** — permitted as an explicit, non-default override, not
   banned. Banning it would make the worktree stricter than an ordinary computer for something
   ordinary computers do constantly (two processes sharing a directory), cutting against 026 §1's
   whole design commitment, and real collaborative patterns (concurrent producer/consumer, live
   co-editing) genuinely need the live cross-visibility `branch`+merge would tax for no reason. The
   real hazard — silent staleness, with no retained-both-versions safety net unlike a `branch`
   conflict — gets closed by extending §4's existing merge-diff-summary mechanism to run proactively
   for `shared` trees too: a short note when the tree moved since an agent's last read, not a ban.
   `branch` stays the default for concurrent siblings.

Full text: 025 §4, §10 Q1/Q2, new gate G7.

### OQ-6 — Plugin distribution

009 Q2. First-party registry versus OCI artifacts. OCI is content-addressed, signed, mirrorable, and
already deployed in every environment that would run this — which is a strong argument for not
building a registry.

**Resolved, OCI, no first-party registry (2026-08-04):** every property a registry would need —
content addressing, signing, mirroring — is already what OCI provides, and a first-party registry
would recreate the exact false-trust-signal hazard 011 §9 already documents for the MCP registry
(presence read as endorsement even when explicitly disclaimed), except we'd own the disclaiming.
Discovery, which OCI doesn't solve, gets a curated git-hosted index rather than a live registry
service — a pointer, not an endorsement. Full text: 009 §3a.

### OQ-7 — Wasmtime version pinning

009 Q4 / 021 Q3. WASI 0.3 ships enabled by default from Wasmtime 46; earlier versions need the
0.3 release candidate. The async ABI difference between 0.2 and 0.3 is not a small compatibility
surface, and the plugin ABI is supposed to be stable. Pin, or support a range?

**Resolved, pin (2026-08-04):** Wasmtime is a build-time embedded dependency here, not a wire peer
with independently-released versions in the wild we must interoperate with — nothing external
forces multi-version support the way MCP/A2A peers do. Pinned to one version at a time, upgraded
deliberately and gated on the full 008/009 suites passing clean, the same discipline already applied
to Quark's submodule pin and the compiler/CMake floors (021 §5). 021 Q3 (prebuilt vs. build-from-
source in CI) is a separate, still-open question about *how* to obtain the one pinned build. Full
text: 009 §11 Q4.

### OQ-12 — A second WASM runtime?

008 §1a rejects `wasm3` for the plugin ABI: it has only partial Component Model and WASI P2 support,
which is the one axis the ABI depends on. But its advantages are real — best-in-class cold start,
tiny footprint, portability down to microcontrollers — and they matter for two niches: ultra-short
high-frequency guest calls (a content filter evaluated per message) and constrained deployments where
Wasmtime's footprint is prohibitive.

A second runtime behind the same host interface means **two sandbox-escape surfaces and two
conformance stories**. That is a real cost, and the benefit is currently an assumption: the cited
cold-start comparison is generic, not measured on our workload. Blocked on a 023-budget measurement
before it is even a candidate.

**Resolved, the engine ships none, and doesn't need the measurement (2026-08-04):** 008 §2a's open
`SandboxBackend` seam already lets a deployer with a genuine ultra-short-call or microcontroller-
footprint need supply a wasm3-backed custom backend themselves, to the same gate bar any custom
backend clears — without the core project taking on a second conformance story for a niche it
doesn't itself have. Same move as `native-jail`-first (§1) applied to the opposite end of the
cold-start axis: back a small number of profiles well, let the open seam absorb the rest. This
dissolves the blocking measurement rather than answering it — the engine isn't adopting a second
runtime, so there's nothing of its own to benchmark. Full text: 008 §1a.

### OQ-14 — In-sandbox library surface area

026 §5 makes the `agent` library the CodeAct action space, which means every module added widens both
what the agent can accomplish and the host's attack surface. `agent.spawn` is the sharpest case:
model-written code creating runs is powerful and is a recursion-and-cost hazard; depth and budget
bounds are necessary and not obviously sufficient. There is no principle yet for what earns a place
in the library beyond case-by-case justification.

**Resolved (2026-08-04):** a two-part admission test (026 §5a) — **capability fidelity** (maps to an
existing 007 §3 capability, or to none if it's a control primitive over the run's own state rather
than an effect on the world) **and in-process necessity** (either run-intrinsic — touches control
state no Tool has standing to reach — or a genuine bulk/streaming need that a tool-call round trip
would defeat). An operation that clears neither belongs as an ordinary Tool, reachable via
`agent.tools`, not as a new top-level module — which is what actually bounds the library's growth,
replacing "justified individually" with a test rather than a judgment call each time. Validated
against every existing module (§5a walks all eight); one inconsistency the test surfaced was fixed
in the process — `agent.ask` cited a capability (`Elicit`) 007 never defined, corrected to `—`
alongside its `output`/`progress` siblings, all three being run-intrinsic control transitions rather
than effects needing a capability grant.

**Explicitly not re-decided:** whether `agent.spawn`'s depth/budget bounds are *sufficient* once it
has earned its place — that stays open as 026 §9 Q1, a narrower question this resolution doesn't
touch.

### OQ-16 — CodeAct has no discoverability story for its own granted surface

026 §4 gives `agent.tools` a real introspection story — generated docstrings, a `.pyi` stub,
`dir(tools)`/`help()` sourced from each Tool's declared metadata (006). That treatment stops at
`agent.tools`: the other seven `agent.*` modules (`files`, `data`, `memory`, `notes`, `output`,
`progress`, `ask`, `spawn`) had no equivalent, and nothing told the model *which* top-level modules
were even present for this session before it tried one. Not the same question as OQ-14 (which is
about what should be allowed to *exist* in the library, i.e. curation) — this one is about the model
discovering what has already been *granted*.

**Resolved by candidate (2026-08-04)**, both parts adopted, both open sub-questions decided:

1. **Pull side** — 026 §4's `agent.tools` pattern generalized to the whole `agent` namespace (026
   §5b): `dir(agent)`/`help(agent)` now reflect exactly the granted module set, with the same
   docstring/`.pyi` treatment every present module gets.
2. **Push side** — a one-line-per-granted-module summary folded into `instructions` at session
   start, extending 026 §7's tool-surface budget line (now ≤ 20 tokens/module) rather than adding a
   third mechanism — pull-side `dir()`/`help()` already covers "detail on demand."
3. **Ungranted modules are omitted, not listed as denied** — resolved by following §5's existing
   rule for the identical case rather than carving out an exception; an explicit "not granted" line
   would itself be the kind of capability enumeration §1 already rules out.

Naming caution carried through unchanged: this is engine-generated and per-session, never mounted
or called a "skill" (009 §8's vocabulary is reserved for authored, distributable content). New gate:
026 G7. Full text: 026 §5b, §7, §8 G7.

### OQ-5 — Span-level taint

003 Q3 / 007 Q2 / 017 Q2. Per-part taint is what 003 specifies and it is coarse: a message that
mixes user text with a quoted tool result taints wholesale. Span-level taint would make
declassification and structural separation far more precise, at a real cost in the content model's
complexity and in every mapping layer.

**Resolved, No (2026-08-04):** the framing undersold the real cost — it isn't just implementation
effort, it's a downgrade of I3's enforcement from a compile-time to a runtime guarantee. Per-item
taint's `TaintedText`/`Tainted<T>` is proven by a **compile-fail test** today (003 G3, 007 G2)
because it's a whole-value type tag; span-level taint needs a runtime interval structure, which can
only be checked at runtime, weakening the static half of I3 across every text-touching path for a
UX-precision gain, not a security fix — coarse taint never under-taints, only over-taints. Kept
per-item. The motivating "which part of this came from where" use case is better served by
`Citation` (003 §1 Q1), a display/attribution annotation independent of the security-enforcement
taint bit. Full reasoning: 003 §8 Q3, 007 §10 Q2, 017 §9 Q2.

### OQ-3 — Do capabilities cross process boundaries as tokens?

007 Q1 / 008 Q4 / 018 Q2. The `remote` sandbox profile, remote plugins, and delegated A2A calls all
want to carry *attenuated* authority across a process or network boundary. A macaroon-style bearer
capability with caveats is the known answer; it adds a crypto dependency, a revocation problem, and
a new forgery surface. Without it, each remote path invents its own bespoke authority protocol —
which is worse.

**Resolved, No — not as bearer capabilities, and the question turned out to bundle two different
problems (2026-08-04):**

1. **`remote` sandbox callbacks / any future remote-plugin-hosting mode** — the actual gap was
   authenticating which live `Exec` a callback belongs to, not delegating authority across the
   wire. 008 §4a's `RemoteExecToken` is an HMAC'd lookup key into a host-side exec registry; the
   host enforces from its own server-side record of the capability set, exactly as it already does
   locally. Macaroon-style caveats were considered and rejected: their value is offline
   verifiability without contacting the issuer, and 008 §4's "egress is always host-mediated" rule
   means every callback reaches the issuing host anyway, so that property buys nothing here.
2. **Delegated A2A calls** — needed no new mechanism at all. 018 §1's existing "no token
   passthrough" + `on_behalf_of` delegation already carries a **principal** across the boundary, not
   a capability set; each side derives its own attenuated capabilities locally from policy (007 §5)
   keyed to that principal. Capabilities were never the right thing to put on that wire.

`Capability` (007 §3) is unchanged: host-process-only, unforgeable by private construction, never
serialized, in every case. **Not yet proven** — no implementation exists to test `RemoteExecToken`
against a forged/replayed/expired-token corpus or measure registry-lookup latency; that is explicit
implementation-phase ADR work, not claimed as done here (008 §10 Q4).

### OQ-2 — Is the single-agent turn loop a workflow?

001 Q1 / 014 Q1. Making the turn loop a special case of the workflow graph buys one execution
model, one checkpointing story, one visualization, and one replay mechanism. It risks paying graph
overhead on the overwhelmingly common single-agent path.

**Resolved, No (2026-08-04):** kept as two distinct execution models. The decisive argument turned
out to be structural rather than about overhead — 014 §1 already defines
`Executor = an agent | a function | a sub-workflow | a request port`, i.e. workflows are built
*from* agents. Making the turn loop itself a workflow graph would invert or merge that dependency
(001 needing 014's graph machinery to define a run, while 014 still needs 001 to know what an agent
is), forcing a "graph-of-one is special" carve-out that defeats the uniformity this question wanted
in the first place. The overhead concern therefore becomes moot rather than needing to be measured:
no implementation exists yet to benchmark a graph-of-one against the direct loop, and the layering
argument doesn't depend on that measurement anyway. What *is* unified, at the layer that actually
should own it: turn boundaries and workflow superstep boundaries are peer checkpoint-boundary kinds
sharing one `Store` and replay mechanism (019 §1), and both project onto one internal event stream
(013 §1) — the shared checkpointing/observability story the candidate resolution wanted, without
collapsing the two execution models that produce it. Full reasoning: 001 §10 Q1, 014 §9 Q1.

### OQ-4 — Unifying human-in-the-loop and long-running work

**Escalated from 🟠 after the A2A/AG-UI research**, which showed the three protocols do not merely
differ in encoding — they differ in *control flow*, and each demands a different correlation
identity:

| Protocol | Shape | Identity it requires |
|---|---|---|
| **MCP** `2026-07-28` | Client **retries the original request** with a *new* JSON-RPC id | `requestState` (opaque, client **MUST NOT** parse) |
| **A2A** v1.0 | Task **stays alive** in `INPUT_REQUIRED`; client sends a new message | `taskId` |
| **AG-UI** | Run **ends** with an interrupt outcome; client starts a **new run** | `interruptId` |

A retry, a continuation, and a restart, plus the same three-way split recurring for long-running
tool/task work (`Suspended`, MCP's `tasks` extension, A2A's task lifecycle).

**Resolved (2026-08-04), in two parts:**

1. **Human-in-the-loop correlation** — a new internal identity, `Interaction.interaction_id`
   (001 §2), is the one thing the run itself knows; each protocol adapter maps it into its own
   required shape and nothing more:
   - **AG-UI**'s `interruptId` *is* `interaction_id` (naming equivalence, no encoding); its
     structural "every open interrupt needs one covering resume" rule already enforces the
     multi-interaction case a run with several concurrent workflow request ports (014 §4) can hit.
   - **MCP** carries `interaction_id` inside `requestState`, integrity-protected per 011 §8a — the
     new JSON-RPC id MCP mints on every retry carries no correlation meaning for us.
   - **A2A** needed no new mapping: `Task ← Run` (012 §1) is already a direct identity, so `taskId`
     already disambiguates a task's one outstanding `INPUT_REQUIRED`.
   - The pre-pause "emit pending state first" rule AG-UI's spec states explicitly is generalized to
     every adapter (001 §2), not treated as an AG-UI-only obligation.
2. **Long-running work** — recognizing the three shapes were never peers at the same granularity
   dissolved the question rather than requiring a fourth mechanism: A2A tasks already **are** our
   `Suspended` run (no gap); MCP's tasks extension, which is scoped to a single long tool call, maps
   onto `Backgroundable`/`StandingEffect` (006 §6b); a whole-run pause surfaced over MCP reuses the
   same `requestState`-carried `interaction_id` as MRTR. See 019 §8 Q2 for the full breakdown.

Full mapping: 013 §2.2. Written into 001 §2, 011 §3.4/§13 Q2, 012 §5a, 013 §2.2, 014 §4, 019 §8 Q2.

### OQ-1 — macOS and Quark's PAL

Quark has `linux_x86_64` and `windows_x86_64` PAL backends; there was no macOS backend, and RFC 021
claimed macOS as a Supported tier anyway — the largest single portability risk in the project,
carried with no owner and no CI evidence.

**Resolved by candidate (b), sharpened to a full drop (2026-08-04):** macOS is dropped as a target
platform entirely, project-wide — not contributed upstream, not merely downgraded to a lower tier.
021 §2's target matrix now lists macOS as **Unsupported — no claim**; every "Windows, Linux, and
macOS" claim across CONVENTIONS.md, the Specification, README, and RFCs 008/009/010/025 was edited
to "Windows and Linux." 008 §1's `no microvm profile` decision, whose original rationale cited the
macOS PAL gap, was re-grounded instead in a Windows-hosting finding
(`docs/research/2026-microvm-windows-portability.md`, 2026-08-04: Firecracker is architecturally
Linux+KVM-only, no production-grade Windows-hosted path exists either) so the decision doesn't lose
its footing now that macOS is moot rather than missing. Re-adding macOS later, if a PAL backend is
ever contributed to Quark, is a fresh decision starting at Best-effort — not a reinstatement.

**Owner:** n/a — scope removed rather than resourced. **Unblocks:** 021 promotion (§6 G1/G3/G4 now
read against two platforms, not three); no macOS claim remains anywhere in the project.

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
