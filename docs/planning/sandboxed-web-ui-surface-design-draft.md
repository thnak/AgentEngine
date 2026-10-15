# Sandboxed web UI surface: generative UI that the host renders in its own WebView

**Status:** Design sketch (2026-10-14), written at the project owner's direction. **Not red-teamed, not
proven, no code.** The I3-adjacent parts (§5 inbound UI actions, §4 router seam) need the full
`design → red-team → prove → judge` cycle and an ADR before code. The rest is ordinary
implementation. Research basis: `docs/research/2026-10-14-generative-ui-landscape.md`.

**Owner decision recorded (2026-10-14):**

- The generated-UI surface is **HTML/CSS/JS rendered in a host-supplied WebView**.
- It is **not** a native declarative widget tree, and **not** a second UI language the model must learn.
- Applications built on AgentEngine ship their own WebView and their own renderer.
- AgentEngine "does not have to carry everything in the world." This mirrors the existing decision
  that AgentEngine does not own the socket (CLAUDE.md, ADR-061): it does not own the screen either.

**Relates to:** 013 §1 (`ModelDelta`, `ToolCallDelta`, `ArtifactProduced`), 013 §2.1 (`ACTIVITY_*`,
JSON Patch), 013 §3 (A2UI deferred as a projection), 006 §6 (`ToolTable`), 008 (sandbox profiles),
025 (worktree), 026 §5 (`agent.spawn`, target design), 030 (Project lifetime), ADR-070 (Delegated
Decision Seam), ADR-224 (host-premounted built-in skills), issue #170 (DUIP),
`tool-call-argument-streaming-gap.md`, `tool-call-argument-streaming-validation-design-draft.md`.

## 1. Who owns what

| AgentEngine owns | The host application owns |
|---|---|
| Declaring the presentation tool(s) (§2) | The WebView (WebView2, WKWebView, Qt WebEngine, a browser) |
| The streaming-argument parser (§3) | Isolation of generated content (origin, CSP, no host APIs) |
| The AG-UI projection of UI activity (§3.2) | The renderer: placeholder, preview, morph, final mount |
| The router seam: narrowing only (§4) | Design tokens, library importmap, fonts |
| The inbound UI-action path: data only (§5) | Bridge validation (which messages a page may send) |
| Budgets (§6), recording and replay (I5) | Persistence of rendered surfaces, export, theming |
| A default prompt skill describing the contract (§2.3) | Replacing or extending that skill with its own tokens and SDK |

**The engine never executes, parses as markup, or interprets a UI payload.** To the engine it is
opaque, size-bounded data with a declared shape (I3). Containment of what the payload does when
rendered is the host's responsibility. This is a disclosed, ADR-070-style delegated residual, not an
engine guarantee, and it must be documented as such.

## 2. The presentation tools

Two tools. Both are effect-free and capability-free, and both are enabled by the host. Neither is on
by default.

### 2.1 `render_inline_ui`: one-off widgets (CopilotKit-compatible)

```
render_inline_ui {
  surface_id?: string,          // §2.4; omitted = new surface
  initialHeight: number,
  placeholderMessages: string[],
  css: string,
  html: string,
  jsFunctions?: string,
  jsExpressions?: string[]
}
```

- **Fields and their order match CopilotKit's `generateSandboxedUi`.** A host can therefore reuse
  CopilotKit's renderer unchanged (research §3–4). Order is a prompt contract, not a schema guarantee.
- **"Execution" is an acknowledgement.** It returns `{surface_id, status: "rendered" | "rejected", reason?}`.
  `rejected` covers budget overflow (§6) or a host renderer that reported a mount failure.

### 2.2 `render_ui_bundle`: render a pre-built, validated UI project (see §8 UC-1)

```
render_ui_bundle {
  surface_id?: string,
  bundle: string,               // content digest of a built bundle in this session's bundle registry
  entry?: string,               // view/route inside the bundle; default "main"
  props: object                 // data for this render, validated against the bundle's props schema
}
```

- **The engine resolves `bundle` against a session-scoped registry** of bundles that passed their
  build-and-check pipeline (§8 UC-1). An unknown digest, or one without a passing build record, is
  rejected *before* anything reaches the host.
- **`props` is validated against the JSON Schema the bundle declares.** This keeps the code
  (a validated, reusable bundle) separate from the data (cheap, per-turn).
- Re-rendering an existing bundle with new data costs one small tool call, not a code rewrite.

### 2.3 Prompt contract as a built-in skill

- **The default skill** covers the field order, the CSP and importmap the host promises, the bridge
  API, and the "no storage, no same-origin" rules. It is a host-premounted built-in skill (ADR-224).
- **Hosts replace or extend it** with their own design tokens, component kit and bridge SDK typings.

### 2.4 Surface identity: an improvement over OGUI

- **Every render belongs to a `surface_id`**, minted by the engine and returned in the result.
- **A later call with the same `surface_id` replaces that surface's content** (projected as an
  `ACTIVITY_SNAPSHOT` on the same `messageId`). OGUI cannot patch earlier widgets; this lets an agent
  update a chart in place.
- Whether the host keeps the old version visible, as history, is the host's choice.

## 3. Streaming

### 3.1 `UiArgsParser` (engine, C++)

- **What it is:** an incremental JSON parser over one tool call's argument deltas. It emits ordered
  field events:

  ```
  Height(n) · Placeholder(s) · Css(complete) · HtmlChunk(s)* · HtmlComplete
  · JsFunctions(complete) · JsExpression(s)* · Done
  ```

  For `render_ui_bundle` it emits `Bundle(id) · Props(complete) · Done`.
- **HTML is the only field streamed mid-string**, matching OGUI.
- **Robustness:**
  - it never throws on malformed input;
  - a parse error ends the stream with `Done{error}`;
  - only the final, complete arguments go through `from_json<Args>` validation (the existing
    fail-closed path).
- **Who uses it:**
  - (a) the AG-UI projection;
  - (b) native hosts that consume the run event stream directly (013 §3 "Terminal / CLI: direct
    consumption"), so a WebView2 host gets progressive fields without running AG-UI at all.
- **Dependency:** it needs live tool-call argument deltas surfaced on the run stream, which is the
  open `tool-call-argument-streaming-gap.md`. **That gap is a prerequisite**, not part of this design.

### 3.2 AG-UI projection

- **Mapping:** parser events become `ACTIVITY_SNAPSHOT` / `ACTIVITY_DELTA` with
  `activityType: "open-generative-ui"`, `messageId: "<surface_id>"`, and JSON Patch ops identical to
  research §3.2's table.
- **Bundle renders** use `activityType: "ae:ui-bundle"` with content `{bundle, entry, props}`.
- **Pinning:** the CopilotKit content shape is vendor-defined and pre-1.0. Pin it as a build constant
  next to the AG-UI schema pin (013 §2.0), and claim *compatibility*, not conformance.

## 4. Router seam: narrowing only

- **The host may install a `PresentationRouter`**, called once per new user message. It returns one
  of a host-defined closed label set plus a renderer: `text`, `inline_ui`, `bundle`, or a host catalog
  component.
- **The engine applies the decision only by removing presentation tools** from that turn's tool set
  and appending the matching skill section. It can never add a tool or a capability (ADR-070:
  "narrows or decides among already-possessed authority only").
- **Jev, a local classifier, or a small LLM with an enum schema are all just implementations.** None
  is an engine dependency.
- **Open question:** 006 §6's `ToolTable` is per-run and immutable. Per-turn narrowing is either a
  per-model-call filtered *view* of that table or needs an amendment. Resolve this in the ADR.
- **The router sees conversation text**, which is model- and user-derived. Its output can therefore
  only choose among presentation tools. It must never feed a permission decision (I3).

## 5. Inbound UI actions: data, never authority

- **Path:** a page sends a message over the host bridge. The host validates it against its own
  allowlist and calls `session.submit_ui_action({surface_id, action, values})`.
- **What the engine does with it:**
  - it starts a **new turn** whose user content is a structured `ui_action` part;
  - the part is attributed to the human principal of the session that rendered the surface (I4);
  - the part is marked as UI-originated in the audit trail.
- **What it may not do:**
  - it is never a tool call;
  - it is never an approval;
  - it never resolves an `InputRequired` / `ApprovalRequested` interaction by itself;
  - nothing in `values` reaches a `PolicyDecider` input.
- **Only explicit user gestures** (clicks) may produce actions. Load, timers and passive input do not.
  The engine cannot see the difference, so this is a host obligation in the contract, and it is a
  disclosed residual.
- **Positive control (required for the ADR):** a `ui_action` whose `values` contain text crafted to
  look like an approval or grant must leave every permission decision unchanged. The test must be
  shown to **fail** when the action is deliberately misrouted into the approval path.

## 6. Budgets (I8)

| Budget | Default (to be set against 023) |
|---|---|
| Inline UI payload bytes per call | 256 KiB |
| Presentation calls per turn | 4 |
| `props` bytes per bundle render | 64 KiB |
| Bundles per session registry | host-set |
| Build wall-clock per bundle (UC-1) | host-set; enforced by the sandbox profile |

Exceeding a budget yields `status: "rejected"` with a reason the model can act on. Rejected calls are
never silently truncated.

## 7. Proposed promotion gates

- **G1 Prefix totality.** For every prefix of every recorded `render_inline_ui` argument stream
  (fuzzed, plus a corpus of real provider streams), the parser emits a valid, ordered event sequence
  and never throws.
- **G2 Interop.** The projection's output, replayed into CopilotKit's renderer
  (`@copilotkit/react-core` at the pinned version), mounts the widget. This is an automated
  browser test.
- **G3 I3 positive control.** As in §5, including a control that is shown to fail.
- **G4 Bundle gate.** `render_ui_bundle` with an unregistered digest, a digest whose build record is
  failing, or `props` that fail schema validation is rejected before any host-facing event is emitted.
  Each case has a test.
- **G5 Replay (I5).** Record and replay reproduces the identical `ACTIVITY_*` sequence.

## 8. Use cases for application developers (non-normative)

These are patterns a host can build on the surface above. They are illustrations, not engine features.

### UC-1: Native Windows app with WebView2 and a reusable UI project, built by a sub-agent

**For:** large, complex, long-lived UI such as dashboards, editors, multi-view tools, and anything a
single inline HTML blob would do poorly.

**Shape:**

1. **The host is a Windows desktop app** (WinUI 3, WPF or Win32) embedding **WebView2**. It serves
   bundles from a local virtual origin (folder-to-hostname mapping), blocks all other network requests,
   and exposes a small typed bridge.
2. **The session worktree holds a TypeScript UI project** (025), for example `/ui/app/`, scaffolded
   from a host-supplied template:
   - Vite + TS;
   - the host's design tokens and component kit;
   - the bridge SDK typings;
   - a `props.schema.json` per entry view;
   - a **vendored, lockfile-pinned `node_modules`**, so builds need no network.

   Because files belong to the worktree, not the sandbox (025 §1), the project survives sandbox
   teardown and app restart for as long as the session or Project lives (030).
3. **A UI-builder sub-agent does the heavy work.** The main agent delegates "build/modify the view
   that shows X". The sub-agent:
   - runs in a sandbox profile with the Node toolchain and **no network**;
   - edits the project, then runs the pipeline: `tsc --noEmit`, the bundler, then a **headless smoke
     render** (load each entry with sample props; assert no console errors, a bridge handshake, a
     non-empty DOM).

   **Only a fully passing pipeline registers a bundle digest.** That is when the bundle is emitted as
   `ArtifactProduced` (content-addressed, 003).

   *Today:* `agent.spawn` is target design, not a real call path (026 §5). Until it is, use a workflow
   step (014) or a separately hosted session.
4. **The main agent only ever calls `render_ui_bundle{bundle, entry, props}`.** Because the engine
   rejects any digest without a passing build record (§2.2, G4), the render call cannot reference
   broken code. Follow-ups that only change data re-render the same bundle with new `props`, with no
   rebuild and no code tokens.
5. **Reuse:** while the session exists, any turn can re-render any registered bundle. After an app
   restart, the host re-mounts surfaces from its own persisted `(surface_id, bundle, props)` records.

**Be precise about "never fails":**

- **What the gate guarantees:** the render call can never reference an unbuilt, uncompiled or
  smoke-test-failing bundle, and never passes props that violate the bundle's schema.
- **What it cannot guarantee:** runtime correctness for every possible `props` value, or behavior on
  real data.

**Mitigations:**

- the smoke render;
- schema-validated props;
- the page reports runtime errors over the bridge as a `ui_action` of kind `error`, which the agent
  can see and fix in the project.

**Other trade-offs:**

- **Build latency** is seconds to minutes. Show progress via `SandboxExecStarted/Finished` and a
  placeholder surface. For quick answers, use UC-2.
- **Supply chain:** no package installs at build time by default. Dependency updates are a separate,
  explicitly approved, network-enabled step.

### UC-2: Inline widgets (OGUI-style)

**For:** one-off calculators, small charts and explanatory diagrams.

- The host enables `render_inline_ui` and implements CopilotKit's renderer pattern: placeholder until
  CSS, morphing preview, final sandboxed mount.
- **Combines with UC-1:** route quick or ephemeral requests to inline UI and persistent or complex
  ones to bundles (§4).

### UC-3: Web frontend using CopilotKit directly

- A web app points CopilotKit's client at a host-fronted AG-UI endpoint that serves AgentEngine's
  projection.
- Because §3.2 matches the `open-generative-ui` content shape, CopilotKit's built-in renderer works
  with **no custom frontend code**. This is the cheapest adoption path.

### UC-4: Host catalog components (A2UI-like)

- A host that wants exact, branded components (tables, forms, approval cards) registers its own
  catalog and exposes it as tools.
- The router (§4) picks catalog components for structured data and inline or bundle UI for the long
  tail.
- When A2UI stabilizes, this becomes the 013 §3 A2UI projection.

## 9. Open questions

- **Q1.** Per-turn tool narrowing vs. the immutable per-run `ToolTable` (006 §6), from §4.
- **Q2.** Relationship to DUIP (#170): should `render_inline_ui` / `render_ui_bundle` be **operations
  of DUIP's single `ui` tool** rather than separate tools? Both fit DUIP's `op` + `payload` shape and
  its `discover`/`describe` disclosure. Reconcile with #170 before the ADR.
- **Q3.** Where the bundle registry lives: a session record (019 durability) or a worktree-side
  manifest (025) that the engine re-verifies on load. Either way, digests are re-checked, never
  trusted from the manifest alone.
- **Q4.** Should the engine offer a reference smoke-render harness (headless Chromium via a sandbox
  profile), or leave the whole build pipeline to host templates?
- **Q5.** Whether WebView2 specifics (virtual host mapping, request filtering, `postMessage` bridge)
  belong in a host guide (`docs/guides/`). They need a dated research note against Microsoft's
  documentation first; **none of the WebView2 API claims in UC-1 have been checked against
  documentation in this pass.**
