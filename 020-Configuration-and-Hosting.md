# 020 — Configuration and Hosting

**Status:** Reviewed (2026-08-05, docs/planning/v1-review-signoff-workflow.md) · **Amended 2026-09-04 by ADR-061** (§3, §3a, §4, §7 G2/G5, §8 Q2 — the engine binds no listener; §8 Q2 reopened) · **Depends on:** 002, 006, 008, 015, 018, 019 (historical: originally also Quark 013 — ADR-037 removed Quark as a dependency) · **Gate:** §7

## Goal

Draw the line between what an agent *is* (policy, 002) and how a deployment *runs* (configuration),
and define the shapes in which the engine is hosted.

## 1. Policy versus configuration

| | Policy (002) | Configuration (here) |
|---|---|---|
| Examples | tool set, capabilities, sandbox profile, max turns, approval mode, output schema | endpoints, credentials, worker counts, pool sizes, timeouts, log levels, retention, pricing tables |
| Changes | agent behaviour and identity | deployment characteristics |
| Form | CRTP template parameters / declarative `spec` | TOML/env/CLI, hot-reloadable where safe |
| Version | part of the agent version (002 §7) | independent |

**The test for where a knob belongs:** if changing it changes what the agent *would do given the
same input*, it is policy. This is AgentEngine's own policy-vs-config boundary (historical: applied
one layer up from Quark's identical boundary, Quark 013, before ADR-037 removed that dependency),
and it exists so that "it worked in staging" means something.

**One hard rule:** configuration may **never widen** capabilities, weaken a sandbox profile below
its declared floor, or disable an approval requirement. Those are policy. A deployment that can
turn off the sandbox through an environment variable does not have a sandbox.

## 2. Configuration model

- **Sources, in precedence order:** defaults < file < environment < CLI < programmatic.
- **Scoped overrides**: defaults < engine < node < agent-type < session. Resolved once at startup
  into an immutable table; live-reconfigurable knobs are explicitly marked (historical: this
  frozen-core/hot-leaf discipline mirrored Quark's own resolution and ADR-008, before ADR-037
  removed Quark as a dependency; the discipline itself is unchanged).
- **Every knob declares** its type, default, valid range, reconfiguration class (`BuildOnly` /
  `Live`), and whether it is security-relevant. Security-relevant knobs are `BuildOnly` and logged
  at startup with their effective values.
- **Validation is fail-fast** at startup with precise diagnostics — never a runtime surprise on the
  first request.
- **Secrets are `SecretRef`s** (018 §4), never literals. A literal in a config file is a startup
  error.

## 3. Hosting shapes

| Shape | Description | Use |
|---|---|---|
| **Embedded library** | Link the engine into an application; own the event loop or let the engine own one | Desktop apps, games, existing C++ services |
| **Engine + host adapter** (renamed from "Standalone server", ADR-061 — see the amendment note below) | One process exposing the protocol surfaces (011/012/013) over HTTP, where **the host binary owns the listener and the engine does not**: consumer code binds the socket, terminates TLS/auth, and calls engine-provided request handlers with already-parsed requests | The common deployment |
| **Cluster** | N nodes with sessions placed by HRW/VirtualBins (historical design intent only — no cluster placement mechanism exists in `agentengine::rt::`; this used to ride Quark's cluster machinery, ADR-037 removed Quark and there is no multi-node story at all today, see §5) | Scale-out, HA (not currently buildable) |
| **CLI / one-shot** | Run an agent or workflow to completion and exit | Automation, CI, scripting |
| **Sidecar** | Engine alongside an application, local IPC | Polyglot deployments before bindings exist |

~~**The same binary serves all five**; the shape is configuration plus which surfaces are enabled.~~

**Amendment (ADR-061, applied 2026-09-04).** Project-owner direction of 2026-08-15, Judged
2026-08-20, removed the assumption every row above was written under —
`decisions/ADR-061-host-provided-inbound-transport.md` §0: *"**AgentEngine will not implement HTTP
networking at all.** The engine exposes a protocol-handler API; consumer code owns the socket, TLS,
HTTP framing, and routing"*, and, in the same section, *"the reactor question is moot **if no
first-party listener is ever built**."* That ADR's §2a conflict table names this section's two
casualties directly, and §8.6 committed to the edits now applied here:

- **The Standalone row is renamed, not deleted.** §2a: *"AgentEngine alone no longer satisfies this
  row."* §8.6 left an either/or — rename to "engine + host adapter", or drop the table to four
  shapes; **renaming is the choice made here (2026-09-04)**, because the deployment shape is still
  real and still the common one. Only *who binds the socket* changed. The adapter is an ordinary
  §3a embedded-library host that routes its own HTTP paths to engine-provided handlers —
  `McpServer::dispatch()` (`include/agentengine/protocol/mcp/server.hpp`) and its A2A sibling take
  the caller's `Principal` and per-request `CapabilityGrant` as arguments precisely because they
  never see a connection of their own.
- **"The same binary serves all five" does not survive** — ADR-061 §7c R21, in those words. The
  engine *library* is the same across shapes; the binary is not, because the shapes that speak a
  protocol surface now require host-supplied transport the engine does not ship. (The Cluster row was
  already void for the unrelated reason recorded in §5.) What remains true, and is the property §7 G2
  was reaching for, is that **the agent** is unchanged across shapes — see G2's own amendment.

### 3a. Embedded library: the host contract

§3's "Embedded library" row names the shape; this states what a linked-in C++ host — a desktop app,
a game, a native UI shell — actually calls. No protocol surface (011/012/013 §2) sits between host
and engine here: the host is in the same process as the `Run`, so it consumes 013 §1's event stream
directly, in its native struct form, with none of the wire translation AG-UI/A2A/MCP exist to do.

**As-built status (2026-09-04): not yet implemented, targeted for Milestone 9.** No `EmbeddedHost`
type exists anywhere in the tree; this whole subsection is a contract, written in the present tense
like the rest of this RFC, not a description of shipped code. 020 is an M9 RFC
(`docs/planning/v1-implementation-roadmap.md:207-227`), and the facade's absence is already tracked
where it bites: `docs/planning/milestone-6-multi-agent-orchestration-breakdown.md` decision 2 builds
030 §6's four project verbs as engine-level operations *beneath* the facade rather than waiting for
it, and `decisions/ADR-053-schedule-wakeup-standing-effect.md` §"does not design" names the missing
facade as why the host-side wake poller is out of its own scope. The individual primitives this
subsection composes — `start_run`, `agentengine::stream<RunEvent>`, `std::stop_token` cancellation —
are real and usable today; only the bring-up object that packages them is not.

**Bring-up.** The host constructs an **`EmbeddedHost`** from resolved configuration (§2), which
brings up AgentEngine's own `agentengine::rt::` runtime in-process (historical: this used to bring
up Quark's actor system before ADR-037 removed that dependency). §3 already names the choice: the
host's own loop pumps `rt::ThreadPool`, or the engine owns its worker threads outright. For a host
with a UI message pump it cannot give up — WinUI's `DispatcherQueue`, Win32's `GetMessage`, Qt's
`QEventLoop` — the fit is **the engine owns its threads**: a UI thread must never be a thread
`rt::ThreadPool` blocks on.

**Starting and draining a run.** No new primitive: `start_run(StartRun{...})` (already named in 001
§2) returns `agentengine::rt::task<result<AgentResponse>>`, and the host `co_await`s it once; a
streaming host instead consumes `agentengine::stream<RunEvent>` (`core/stream.hpp`, over
`rt::channel<T,E>`) in an ordinary coroutine drain loop. That drain **is** the backpressure signal
013 §1 already promises ("a slow consumer applies backpressure to the provider read instead of
buffering unboundedly") — it was never a protocol-only property, it is `stream<T>`'s own
credit-controlled contract (historical: originally Quark's credit-controlled reply ring, ADR-018;
ADR-037 replaced the backend, the contract is unchanged), and an embedded host gets it for free by
doing nothing special. This is what makes "asynchronous by default" true here rather than
aspirational: a run was never going to emit faster than whatever is draining it, embedded host or
not.

**One handle, one `Run`.** A tab per subagent (per the earlier discussion: 001 §4 sub-agents are
already separate `Run`s on separate `AgentSession`s) is exactly one independently-drained
`stream<RunEvent>` per tab. No new concept — N tabs is N ordinary drain loops.

**Threading is the host's problem, explicitly.** The engine delivers stream resumption on whichever
`rt::ThreadPool` worker thread produced the event (historical: "whichever Quark worker thread"
before ADR-037) — never on a thread the host designated, because the engine has no way to know a
host has a UI thread, let alone which one. A host that mutates a UI object from inside the drain
coroutine without marshaling (`winrt::resume_foreground(dispatcherQueue)`
or equivalent) has a host bug, not an engine defect. Stated plainly so it doesn't get discovered by
a flaky WinUI reference app: this has to be documented, not assumed away.

**Secondary local observers on one run — a partial answer to 013 Q2.** 013 Q2 flags that a
best-effort, at-most-once, per-subscriber drop-on-full pub-sub primitive (historical: Quark's
`Topic<M>`, ADR-019, before ADR-037 removed Quark as a dependency) is the wrong shape for A2A's
multi-subscriber requirement, because A2A **must** deliver identical events in identical order to
every concurrent subscriber. That disqualification is specific to the A2A conformance obligation —
it is not a defect in a best-effort primitive itself. For a second in-process observer on one run (a
debug pane alongside the tab's primary view, say), best-effort is exactly the right shape: a
coalesced or dropped UI frame is not a correctness bug the way a missed A2A task update would be.
So: a run's **primary** stream is always the credit-controlled `stream<RunEvent>` from `start_run`
above; a run **may** additionally fan its events out to secondary in-process observers only, never
as a substitute for a protocol surface's delivery guarantee — see §8 Q6's resolution below to build
this as ordinary AgentEngine-owned code rather than lean on any Quark primitive. This closes the
embedded-only slice of 013 Q2; A2A's stricter requirement is untouched and still needs its own
primitive.

**Feeding input back and cancelling.** Both are already-named mechanisms, not embedding-specific
ones: resolving `InputRequired`/`ApprovalRequested` (001 §2) is an ordinary `ask` carrying the resume
payload, the same one a web caller uses; cancelling a tab's run triggers the `std::stop_token` (001
§5) the host was given at `start_run`.

**What this contract does not promise.** Source-level embedding, not binary-stable embedding. The C
ABI 020 Q3 anticipates is for out-of-process bindings (future Python/.NET); a linked-in C++ host
builds against the same compiler and ABI as the engine, and `RunEvent` and friends can change shape
between engine versions like any other internal type. A host that needs to embed across independent
builds without recompiling wants the C ABI seam, not this one.

**Concretely, for a WinUI host: only the C++/WinRT face of WinUI 3 is in scope today.** WinUI 3
(Windows App SDK) is consumable two ways — the common C#/.NET one, and a native **C++/WinRT**
projection that compiles to ordinary native code with no CLR involved. This contract is source-level
C++ embedding, so a C++/WinRT host links the engine exactly like any other embedded-library host
(§3a above) and is not a ".NET binding" in the sense CLAUDE.md's locked decisions defer — C++/WinRT
never touches managed code. A **C#/.NET WinUI** host is a different question entirely: it needs the
still-unspecified C ABI (Q3 below), not this contract, and is out of scope until that seam is
designed.

### 3b. Declarative run triggers

Every path to `StartRun` covered so far assumes something already decided to call it — a host's UI
action, an inbound MCP/A2A/AG-UI request, a CLI invocation. Missing: a *declarative* way to say "start
this agent on this schedule" or "start this agent when this webhook fires," without a bespoke cron job
or webhook handler living outside the engine and outside its audit trail. This is distinct from 019
§2's wake conditions, which only resume an already-`Suspended` run belonging to an existing session —
a trigger here mints a **new** `Run`.

- **Reuses 006 §6b's scheduling machinery, not a second one.** A trigger config is `{agent,
  WakeCondition, principal}` — the same `WakeCondition` type §6b introduced for
  `schedule_wakeup`/`watch_resource`, registered by configuration instead of by a tool call from
  inside a run. Firing calls the already-named `StartRun` (001 §2, §3a above) rather than resuming
  anything.
- **Configuration, not a model-callable capability.** Per the §1 policy-versus-configuration line, a
  trigger changes *what runs*, so it is versioned with the agent like a policy knob, declared by an
  operator, and never something a model registers for itself — deciding when a new, independent run
  gets created is a materially different authority than a model asking to be woken inside its own run
  (007 I2/I3), and §6b's `Schedule` capability deliberately does not reach this far.
- **Principal is explicit, not inherited.** A triggered run has no calling human or caller to derive a
  principal from the way a tool call or sub-agent would (007 §2); the trigger config names the
  principal it runs as, resolved at admission (018) like any other run.
- **A webhook trigger authenticates before `StartRun` is ever called.** An unauthenticated `StartRun`
  reachable from arbitrary inbound traffic is the obvious failure mode here, so verification (018)
  gates the call rather than being left for each deployment to reinvent — or skip.

## 4. Server surfaces

Each is independently enable-able, and each is off unless configured:

`/mcp` (011) · `/a2a` + agent card (012) · `/agui` (013) · OpenAI-compatible `/v1/*` (013 §3) ·
`/health`, `/ready` · `/metrics` (Prometheus text; OTLP via the exporter seam) · admin API
(agent/plugin/policy management, separately authenticated and separately bindable)

**The admin API is never on the same listener as the public surfaces by default.** Making that
mistake once is enough.

**Amendment (ADR-061, applied 2026-09-04): this is now a host obligation, and the engine cannot
enforce it.** Per §3's amendment the engine binds no listener, so it cannot see how many listeners
exist or which surface a request arrived on unless told. ADR-061 §2a states the consequence in one
line — the rule *"becomes a host obligation the engine cannot enforce"* — and the paths listed above
are read as **the paths a host adapter routes to engine handlers**, not paths the engine serves.

**No engine-side backstop exists, and one was wrongly claimed.** ADR-061 §13.7 originally asserted
that an admin method is engine-refused on a public-API-surfaced `EndpointId`; §33.3 explicitly
retracts that — *"this claim was never true and stood uncorrected in this document for six days...
No such refusal mechanism was ever built"*, confirmed against both the §30.1 prove-phase file list
and current `include/agentengine/`. `EndpointId::surface` is unbuilt design text; §31.1 declined to
build it for want of a consumer. So the separation above is stated here as what a host **must** do,
with nothing in the engine that will catch a host that doesn't:

- Bind the admin API on its own listener, with its own credentials, per the original rule.
- Do not multiplex admin and public paths onto one listener and rely on path routing alone.
- Treat this as security-relevant configuration the host owns end-to-end, exactly like TLS
  termination and inbound authentication (018, ADR-061 Tier 3).

**020 §8 Q2 is reopened by this amendment** — its resolution rested on §3's "same binary serves all
five" and on this section's separate-listener rule being engine-guaranteed, and both premises are
now gone. See §8.

## 5. Cluster concerns

**Historical note (ADR-037):** this section describes multi-node cluster placement as it was
originally designed, riding Quark's own cluster/placement machinery. ADR-037 (2026-08-13) removed
Quark as a dependency entirely and audited zero real cluster footprint in AgentEngine's build even
before removal (`decisions/ADR-037-remove-quark-as-core-runtime.md` §2) — `agentengine::rt::` has
no multi-node cluster placement mechanism at all. The concerns below are retained as an honest
record of unbuilt, currently out-of-scope future work, not a description of anything that exists
today.

- **Session placement** and reactivation used to come from Quark; AgentEngine chose the key
  (`session_id`) and the placement policy. No placement/reactivation mechanism exists post-ADR-037.
- **Sandbox locality**: a `native-jail` sandbox is node-local; a session that migrates loses warm
  sandboxes, which is a cost, not a correctness issue (010 §3 clean-state contract makes it safe) —
  moot without a cluster to migrate across.
- **Rolling upgrade**: drain, fenced hand-off, version-skew policy for in-flight runs (019 §4).
- **Capacity**: sandbox pools, provider connection pools, and session density are the three limits
  that matter; each has a budget in 023.

## 6. Operations

- **Startup diagnostics** print the resolved isolation posture per agent, including any profile
  downgrade (008 §3), the enabled surfaces, and protocol revisions. An operator should be able to
  read the first screen of logs and know exactly what security posture is in force.
- **Graceful shutdown**: stop admission, drain in-flight runs to checkpoint boundaries, destroy
  sandboxes, flush audit, exit within a bounded time.
- **Health versus readiness** are distinct: readiness accounts for provider reachability and sandbox
  backend availability, so a node with a broken sandbox backend does not silently take traffic.

## 7. Promotion gate

- **G1** — a security-relevant knob cannot widen authority: an exhaustive test over the config
  surface attempts to reach a wider capability set, weaker profile, or disabled approval, from
  every source, and fails in every case.
- **G2** — the same agent runs unchanged in every hosting shape §3 still names as buildable.
  *(Amended 2026-09-04. Original: "the same agent runs unchanged in all five hosting shapes." Two
  rows no longer support the literal claim: Cluster is not buildable at all (§5, ADR-037), and the
  renamed engine-plus-host-adapter row needs a host binary the engine does not ship (§3 amendment,
  ADR-061 §7c R21). The property this gate is actually for — an **agent** is portable across shapes,
  its policy and behaviour unchanged by where it is hosted — is untouched by ADR-061 and is what the
  gate now states. A run of this gate must say which shapes it covered and which it could not, rather
  than claiming five.)*
- **G3** — invalid configuration fails at startup with a precise diagnostic, never at first request
  (negative corpus per knob class).
- **G4** — graceful shutdown under load: zero lost acknowledged turns, zero orphaned sandboxes,
  audit flushed, within the declared bound.
- **G5 (§3a)** — a reference embedded C++ host (headless, no UI) drives N concurrent runs, each
  through its own `agentengine::stream<RunEvent>` (`core/stream.hpp`, over `rt::channel<T,E>`):
  zero cross-run event interleaving observed by any drain loop, and a stalled drain measurably
  applies backpressure to its run (the run's provider read slows or blocks) without affecting the
  other N-1 runs.
  *(Type names corrected 2026-09-04. Original: "its own `ReplyStream<RunEvent>` obtained via
  `ask_stream`." `ReplyStream` was Quark's type and survives only as a historical comment
  (`include/agentengine/core/chat_client.hpp`) after ADR-037. `ask_stream()` does exist
  (`include/agentengine/core/session_builder.hpp`, ADR-073) but returns
  `result<stream<std::string>>` — a text stream, not a `RunEvent` stream — so it was never the
  right call for this gate. §3a's own body already named the correct shapes; this gate now agrees
  with the section it gates.)*
- **G6 (§3b)** — a webhook trigger fired without passing verification (018) never reaches
  `StartRun`, proven over a negative corpus, not merely asserted; a trigger that passes verification
  produces a run whose principal matches the trigger config exactly, never one derived from request
  contents (I2/I3).

## 8. Open questions

- ~~**Q1** — Configuration format: TOML (as assumed) versus reusing the declarative YAML shape (015)
  for uniformity.~~ **Resolved, TOML, deliberately not 015's shape (2026-08-04):** §1 makes the
  policy/config boundary a hard, security-relevant line ("configuration may never widen
  capabilities... a deployment that can turn off the sandbox through an environment variable does
  not have a sandbox"), and 015 §5's overlay rule shows how easily "just configuration" can slide
  toward capability-widening if nothing about the document's shape warns an author they're in
  different territory. A visually and structurally distinct format — TOML, flat/scalar, no
  `apiVersion`/`kind`/`spec` ceremony — versus 015's Kubernetes-shaped, versioned, signable YAML is a
  deliberate signal an author can't miss, not an inconsistency to fix for uniformity's sake. 009's
  `manifest.toml` (a request, never a grant) is a consistent precedent for TOML as this project's
  lightweight-declaration format, distinct from 015's heavier, capability-bearing shape.
- **Q2 — REOPENED 2026-08-20 by ADR-061 §13.7, un-struck here 2026-09-04.** Whether the admin API
  should exist in-process at all, or only as a separate binary.

  *Prior resolution, withdrawn — kept for the record:* ~~**Resolved, in-process, same binary
  (2026-08-04):** §3's "same binary serves all five [hosting] shapes" is a strong existing commitment
  this question would otherwise cut against for no clear gain. §4's separate-listener, separately-
  authenticated rule already delivers the actual security property at stake — an admin-surface
  compromise doesn't automatically compromise a public surface, different bind, different
  credentials. A genuinely process-isolated admin surface (real isolation beyond listener separation)
  is already reachable today as an operator's own deployment choice — run the admin surface as its
  own instance pointed at the same session/policy store via ordinary configuration — not a v1
  architectural requirement forcing every deployment to run two binaries.~~

  **Why it is open again:** that resolution cited exactly two grounds, and ADR-061 voided both.
  §3's "same binary serves all five" does not survive (§7c R21, and §3's amendment above), and §4's
  separate-listener rule is no longer a property the engine delivers — it is a host obligation with
  no engine-side backstop, the supposed backstop having been explicitly retracted at ADR-061 §33.3.
  ADR-061 §13.7, in its own words: *"**020 §8 Q2 is reopened**, tracked explicitly (not silently,
  T16's finding) as a real open question this ADR's acceptance carries forward, because both
  premises of its prior 'in-process, same binary' resolution are gone under a host-owned listener."*

  **What answering it now requires, which the 2026-08-04 pass did not face:** the question is no
  longer "one binary or two" — under ADR-061 the admin API, like every other surface, is served by a
  host adapter the engine does not ship, so the real question is what this RFC *obliges an adapter to
  do*, and whether any of it is engine-checkable at all (e.g. whether the engine should refuse
  admin-class operations arriving without an admin-surfaced endpoint identity, which is the unbuilt
  `EndpointId::surface` idea ADR-061 §31.1 declined to build for want of a consumer). Milestone 9,
  which builds the first real adapter, is the natural forcing function. Cross-referenced from
  `OpenQuestions.md` OQ-26 residual item 4; per that file's own convention this stays a per-RFC
  question and is tracked here, not promoted there.
- ~~**Q3** — A C ABI is the intended seam for future Python/.NET bindings; its shape is unspecified
  and will constrain those bindings once frozen.~~ **Resolved, design early against stable concepts,
  freeze late against a real consumer (OQ-8, 2026-08-04):** the dilemma assumed the C ABI is a
  mechanical export of the C++ authoring surface (002's CRTP templates), so its timing had to either
  anticipate or trail that surface. It isn't and can't be — templates have no ABI to export across a
  C boundary — so the C ABI was always going to be a small, hand-designed seam over a handful of
  already-stable *concepts*, not a reflection of whatever the template surface currently looks like:
  `StartRun`/`ask_stream` and the `RunEvent` drain loop (§3a), config resolution (§2), and capability
  grants (007 §3) resolved to opaque handles. None of those move the way a template signature would,
  so **design work can start now**, scoped to exactly that list.

  What must wait is calling it **frozen**. 024 §2's "no source break within a major version" promise
  is not owed to a seam nobody has built against — freezing is gated on a **reference out-of-process
  consumer**: a prototype Python or .NET binding (not a shipped one) built against the draft ABI and
  revised at least once in response to what the prototype exposed as wrong, the same
  argument-alone-does-not-reach-Proven discipline 024 §4 already applies to RFCs generally. The C ABI
  is versioned as its own artifact (024 §1), independent of "Engine" — conflating the two (a binary-
  stable seam for out-of-process bindings versus §3a's source-level embedding contract, which was
  never meant to be binary-stable) was part of what made the freeze timing feel harder than it is.
- ~~**Q4** — Deployment descriptor for the `remote` sandbox profile: reuse the Kubernetes Agent
  Sandbox CRD directly, or wrap it?~~ **Resolved, wrap (2026-08-04):** forced by a decision already
  made — 008 §2's `SandboxSpec` is explicitly the one portable contract every `SandboxBackend`
  accepts ("profiles differ in backend, never in semantics," §1). Reusing the CRD verbatim as
  AgentEngine's own descriptor would make `SandboxSpec` K8s-shaped for `remote` specifically,
  breaking that uniformity and coupling the core to K8s's own schema evolution in a way no other
  backend is coupled to its backend's internals. The `remote` backend owns the translation
  (`SandboxSpec` → CRD fields) as ordinary backend-internal logic, the same shape 004 §3's "Porting
  note" already establishes for vendor-specific translation living inside a backend, not at the seam.
- ~~**Q5 (§3a)** — Default event-loop ownership for an embedded UI host: this RFC recommends
  "engine owns its worker threads" for hosts with their own message pump, but does not yet require
  it. Whether a host should be able to choose the other mode at all, or whether "host owns the loop"
  should be restricted to headless/single-threaded embeddings (games, CLI-adjacent tools) where no
  UI framework is fighting for the same thread, is undecided.~~ **Resolved, conditional on whether a
  UI message pump is declared, not a blanket rule either way (2026-08-04):** for a host declaring a
  UI message pump, §3a's own stated hazard ("a UI thread must never be the thread the actor scheduler
  blocks on") leaves only one safe answer, so it isn't really optional there — `EmbeddedHost`
  construction **fails fast** (matching 002 §6 / §2's "validation is fail-fast at startup" pattern) if
  a host declares a UI message pump and also requests host-owns-the-loop, rather than silently
  allowing a configuration known to starve the UI. For headless/single-threaded embeddings (games,
  CLI-adjacent tools, nothing competing for the thread) no hazard exists, so both options stay
  available as an ordinary configuration choice (§2), matching G5's own reference host being
  headless — the tested, proven path — while UI-host safety is enforced rather than merely
  recommended.
- ~~**Q6 (§3a)** — Whether the `Topic<RunEvent>` secondary-observer pattern should become a
  documented, supported API (`Run::observe()` or similar) or stay purely a host-side composition.~~
  **Resolved, Yes, first-class `Run::observe()` (2026-08-04):** the blocking condition in this
  question's own prior update no longer holds — 013 §7 Q2 resolved to building the needed ordered,
  bounded-eviction fan-out as ordinary AgentEngine-owned code (a per-subscriber cursor plus a bounded
  ring buffer) rather than waiting on Quark's still-unshipped ADR-039 primitive (historical: moot
  regardless since ADR-037 later removed Quark as a dependency entirely). That same mechanism,
  already required for AgentEngine's own A2A conformance, is the natural backing for a general
  `Run::observe()` serving any secondary local observer (a debug pane, a metrics tick, a second UI) —
  giving every observer ordered, gap-signaled delivery instead of best-effort drop-on-full, with
  nothing to wait on. A lighter-weight, best-effort composition (historical: `Topic<M>`, Quark's own
  primitive) is no longer available post-ADR-037; an observer that genuinely doesn't care about gaps
  (a live metrics tick, §3a's own example) uses the same `Run::observe()` mechanism today, not a
  separate primitive.
