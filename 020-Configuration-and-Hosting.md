# 020 — Configuration and Hosting

**Status:** Draft · **Depends on:** 002, 008, 015, 018, Quark 013 · **Gate:** §7

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
same input*, it is policy. This is Quark's policy-vs-config boundary (Quark 013) applied one layer
up, and it exists so that "it worked in staging" means something.

**One hard rule:** configuration may **never widen** capabilities, weaken a sandbox profile below
its declared floor, or disable an approval requirement. Those are policy. A deployment that can
turn off the sandbox through an environment variable does not have a sandbox.

## 2. Configuration model

- **Sources, in precedence order:** defaults < file < environment < CLI < programmatic.
- **Scoped overrides** mirroring Quark's resolution: defaults < engine < node < agent-type <
  session. Resolved once at startup into an immutable table; live-reconfigurable knobs are
  explicitly marked (Quark ADR-008's frozen-core/hot-leaf discipline).
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
| **Standalone server** | One process exposing the protocol surfaces (011/012/013) over HTTP | The common deployment |
| **Cluster** | N nodes over Quark's cluster (010/021/026) with sessions placed by HRW/VirtualBins | Scale-out, HA |
| **CLI / one-shot** | Run an agent or workflow to completion and exit | Automation, CI, scripting |
| **Sidecar** | Engine alongside an application, local IPC | Polyglot deployments before bindings exist |

**The same binary serves all five**; the shape is configuration plus which surfaces are enabled.

### 3a. Embedded library: the host contract

§3's "Embedded library" row names the shape; this states what a linked-in C++ host — a desktop app,
a game, a native UI shell — actually calls. No protocol surface (011/012/013 §2) sits between host
and engine here: the host is in the same process as the `Run`, so it consumes 013 §1's event stream
directly, in its native struct form, with none of the wire translation AG-UI/A2A/MCP exist to do.

**Bring-up.** The host constructs an **`EmbeddedHost`** from resolved configuration (§2), which
brings up Quark's actor system in-process. §3 already names the choice: the host's own loop pumps
the scheduler, or the engine owns its worker threads outright. For a host with a UI message pump it
cannot give up — WinUI's `DispatcherQueue`, Win32's `GetMessage`, Qt's `QEventLoop` — the fit is
**the engine owns its threads**: a UI thread must never be the thread the actor scheduler blocks on.

**Starting and draining a run.** No new primitive: `ask_stream<RunEvent>(StartRun{...})` (already
named in 001 §2) has the signature Quark's ADR-018 gives it —
`task<expected<ReplyStream<RunEvent>, error>>` — and the host `co_await`s it once to obtain the
`ReplyStream<RunEvent>`, then drains it in an ordinary coroutine loop. That drain **is** the
backpressure signal 013 §1 already promises ("a slow consumer applies backpressure to the provider
read instead of buffering unboundedly") — it was never a protocol-only property, it is Quark's
credit-controlled reply ring (ADR-018), and an embedded host gets it for free by doing nothing
special. This is what makes "asynchronous by default" true here rather than aspirational: a run was
never going to emit faster than whatever is draining it, embedded host or not.

**One handle, one `Run`.** A tab per subagent (per the earlier discussion: 001 §4 sub-agents are
already separate `Run`s on separate `AgentSession`s) is exactly one independently-drained
`ReplyStream<RunEvent>` per tab. No new concept — N tabs is N ordinary drain loops.

**Threading is the host's problem, explicitly.** The engine delivers `ReplyStream` resumption on
whichever Quark worker thread produced the event — never on a thread the host designated, because
the engine has no way to know a host has a UI thread, let alone which one. A host that mutates a UI
object from inside the drain coroutine without marshaling (`winrt::resume_foreground(dispatcherQueue)`
or equivalent) has a host bug, not an engine defect. Stated plainly so it doesn't get discovered by
a flaky WinUI reference app: this has to be documented, not assumed away.

**Secondary local observers on one run — a partial answer to 013 Q2.** 013 Q2 flags that Quark's
`Topic<M>` (ADR-019) is the wrong primitive for A2A's multi-subscriber requirement, because A2A
**must** deliver identical events in identical order to every concurrent subscriber and `Topic<M>` is
deliberately best-effort, at-most-once, per-subscriber drop-on-full. That disqualification is
specific to the A2A conformance obligation — it is not a defect in `Topic<M>` itself. For a second
in-process observer on one run (a debug pane alongside the tab's primary view, say), best-effort is
exactly the right shape: a coalesced or dropped UI frame is not a correctness bug the way a missed
A2A task update would be. So: a run's **primary** stream is always the credit-controlled
`ReplyStream` from `ask_stream` above; a run **may** additionally `publish` its events onto a
`Topic<RunEvent>` for secondary in-process observers only, never as a substitute for a protocol
surface's delivery guarantee. This closes the embedded-only slice of 013 Q2; A2A's stricter
requirement is untouched and still needs its own primitive.

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

## 4. Server surfaces

Each is independently enable-able, and each is off unless configured:

`/mcp` (011) · `/a2a` + agent card (012) · `/agui` (013) · OpenAI-compatible `/v1/*` (013 §3) ·
`/health`, `/ready` · `/metrics` (Prometheus text; OTLP via the exporter seam) · admin API
(agent/plugin/policy management, separately authenticated and separately bindable)

**The admin API is never on the same listener as the public surfaces by default.** Making that
mistake once is enough.

## 5. Cluster concerns

- **Session placement** and reactivation come from Quark; AgentEngine chooses the key
  (`session_id`) and the placement policy.
- **Sandbox locality**: a `native-jail` sandbox is node-local; a session that migrates loses warm
  sandboxes, which is a cost, not a correctness issue (010 §3 clean-state contract makes it safe).
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
- **G2** — the same agent runs unchanged in all five hosting shapes.
- **G3** — invalid configuration fails at startup with a precise diagnostic, never at first request
  (negative corpus per knob class).
- **G4** — graceful shutdown under load: zero lost acknowledged turns, zero orphaned sandboxes,
  audit flushed, within the declared bound.
- **G5 (§3a)** — a reference embedded C++ host (headless, no UI) drives N concurrent runs, each
  through its own `ReplyStream<RunEvent>` obtained via `ask_stream`: zero cross-run event
  interleaving observed by any drain loop, and a stalled drain measurably applies backpressure to
  its run (the run's provider read slows or blocks) without affecting the other N-1 runs.

## 8. Open questions

- **Q1** — Configuration format: TOML (as assumed) versus reusing the declarative YAML shape (015)
  for uniformity.
- **Q2** — Whether the admin API should exist in-process at all, or only as a separate binary.
- **Q3** — A C ABI is the intended seam for future Python/.NET bindings; its shape is unspecified
  and will constrain those bindings once frozen.
- **Q4** — Deployment descriptor for the `remote` sandbox profile: reuse the Kubernetes Agent
  Sandbox CRD directly, or wrap it?
- **Q5 (§3a)** — Default event-loop ownership for an embedded UI host: this RFC recommends
  "engine owns its worker threads" for hosts with their own message pump, but does not yet require
  it. Whether a host should be able to choose the other mode at all, or whether "host owns the loop"
  should be restricted to headless/single-threaded embeddings (games, CLI-adjacent tools) where no
  UI framework is fighting for the same thread, is undecided.
- **Q6 (§3a)** — Whether the `Topic<RunEvent>` secondary-observer pattern should become a documented,
  supported API (`Run::observe()` or similar) or stay purely a host-side composition over `publish`/
  `subscribe` that this RFC merely licenses. A first-class API is more discoverable; leaving it as
  composition keeps the engine surface smaller, consistent with 026 §5's "small and boring" bias.
  **Update:** once Quark ships the ordered multi-subscriber primitive requested in
  [QuarkCpp#10](https://github.com/thnak/QuarkCpp/issues/10) (013 Q2), that primitive — not
  `Topic<M>` — becomes the natural fit for a first-class `Run::observe()`, since it would give every
  local observer (not just the primary drain) ordered, gap-signaled delivery instead of best-effort
  drop. `Topic<M>` composition would then be reserved for observers that genuinely don't care about
  gaps (e.g. a live metrics tick), rather than being the only local multi-observer option.
