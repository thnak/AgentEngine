# Research record — Generative UI over AG-UI: CopilotKit Open Intelligent UI, A2UI, and Jev routing

**Checked:** 2026-10-14. Primary sources are the repository and the published npm package, read
from source (pinned below), plus TypeSafe's own public pages for Jev. **No web search was available
for this pass**, so independent third-party evidence (benchmarks, reviews) is absent. Vendor claims
are labelled as vendor claims.
**Question:** how does CopilotKit's "Open Intelligent UI" (the launch app) and its "Open Generative
UI" mechanism actually work? Which parts are protocol (AG-UI) and which are product? What, if
anything, should AgentEngine adopt?
**Feeds:** `docs/planning/sandboxed-web-ui-surface-design-draft.md`, 013 §2.1 (`ACTIVITY_*` mapping),
013 §3 (A2UI deferred as a projection), issue #170 (DUIP),
`docs/planning/tool-call-argument-streaming-gap.md`.

## Sources (pinned)

| Source | Version / commit |
|---|---|
| `github.com/CopilotKit/OpenIntelligentUI` | `8391a94c1c3939c679e40ddd7bfc08a9ef690e70` (2026-10-13) |
| `@copilotkit/runtime` (npm) | `1.55.2-next.1`, file `src/v2/runtime/open-generative-ui-middleware.ts` (373 lines) |
| `@copilotkit/react-core`, `@copilotkit/a2ui-renderer` | `1.55.2-next.1` (as pinned in the app's `package.json`) |
| `@jetbrains/websandbox` | `1.2.1` |
| `@ag-ui/a2ui-middleware` | `^0.0.2` |
| TypeSafe Jev docs | https://docs.typesafe.ai/introduction/quickstart (fetched 2026-10-14) |
| TypeSafe homepage | https://typesafe.ai (fetched 2026-10-14) |

## 1. What the product is

It is a chat app (Next.js + CopilotKit frontend, Python LangGraph "Deep Agent" backend). For each user
turn it answers with one of three things:

- **text**;
- an **A2UI** declarative component, which today means only a `Table`;
- an **Open Generative UI** widget: model-authored HTML/CSS/JS rendered in a sandboxed iframe.

The launch film's scenes (3D airplane, bill splitter, chart, animated coastal map) are all the third
kind. The README states that the film shows "rendered launch scenes" and that live outputs vary.

## 2. Per-turn routing (Jev): a separate model picks the presentation

`apps/agent/src/visualization_router.py` is LangChain `AgentMiddleware`. In `before_agent` it makes
one HTTP call per *new user message*, to `POST https://api.typesafe.ai/v1/systemone`. The request
carries:

- `model: "jev-latest"`;
- `state.conversation` = the last 4 user/assistant text messages, each capped at 6,000 chars;
- a `choice` question with 13 labelled criteria: `text`, `table`, `bar_chart`, `line_chart`,
  `scatter_plot`, `distribution`, `part_to_whole`, `flowchart`, `static_diagram`,
  `interactive_diagram`, `animated_route`, `map`, `calculator`.

The response `{choice, confidence, probabilities}` is validated against the closed label set
(`decision_from_response`). An unknown label is a hard error; there is **no silent fallback**. The
renderer is then derived deterministically:

| Choice | Renderer |
|---|---|
| `text` | `text` |
| `table` | `a2ui` |
| anything else | `open_generative_ui` |

The decision is stored in thread state keyed by user-message ID, so tool continuations reuse it.

In `wrap_model_call` the middleware **removes every rendering tool that does not match** the decided
renderer. Unrelated data and state tools stay available. It then appends a per-choice instruction
block to the system message. For example, `animated_route` names `window.createTripAnimator`, a
6-second pin-drop sequence and USGS tiles.

**What Jev is, per its vendor.** TypeSafe calls it a "System One model" that returns typed decisions
with calibrated probabilities, not generated text. It offers three primitives:

- `choice` — pick one option from a list;
- `score` — rate against a rubric;
- `noul` — the probability that a statement is true.

**Vendor claims, not verified:**

- "a new architecture, a new sampler, and a new training algorithm (RLCD)";
- about 0.114 s vs 8.566 s for an LLM on comparable decision tasks;
- $42 per billion input tokens.

The vendor FAQ asks whether pricing is subsidized and does not answer.

**Assessment.** Routing is not a novel capability. Any model with enum-constrained structured output
can return the same label. Jev's differentiators are claimed latency, cost and calibration. In this
app the `confidence` value is carried but not acted on.

## 3. The Open Generative UI mechanism

### 3.1 A presentation tool whose arguments are the UI

The model calls `generateSandboxedUi`, a CopilotKit-injected tool with six fields. The prompt
(`apps/agent/src/prompt.py`) and the `master-playbook` skill require them **in this exact order**:

```
initialHeight → placeholderMessages → css → html → jsFunctions → jsExpressions
```

The order is enforced only by prompt instruction. JSON Schema cannot constrain key order.

### 3.2 Runtime middleware: incremental JSON parse → AG-UI activity events

`OpenGenerativeUIMiddleware` (`@copilotkit/runtime` v2) wraps the agent's AG-UI event stream. For
each `generateSandboxedUi` call it feeds `TOOL_CALL_ARGS` chunks to **clarinet**, a SAX-style
streaming JSON parser, and emits `ACTIVITY_SNAPSHOT` / `ACTIVITY_DELTA` events with
`activityType: "open-generative-ui"`, `messageId: "<toolCallId>-activity"` and RFC 6902 patches:

| Parser milestone | Event emitted |
|---|---|
| `initialHeight` value | `ACTIVITY_SNAPSHOT {initialHeight, generating: true}` (once) |
| each `placeholderMessages[i]` | `add /placeholderMessages/-` |
| `css` value complete | `add /css`, `add /cssComplete: true` |
| `html` **while the string is still open** | reads the parser's internal `textNode` buffer after every `write()` and emits each new slice as `add /html/-` |
| `html` value complete | final slice, then `add /htmlComplete: true` |
| `jsFunctions` value complete | `add /jsFunctions`, `add /jsFunctionsComplete: true` |
| each `jsExpressions[i]` | `add /jsExpressions/-`; on array close, `add /jsExpressionsComplete: true` |

Other behaviors:

- `TOOL_CALL_START/ARGS/END` for the tool are held until the first activity event is emitted.
- `RUN_FINISHED` is also held until everything ahead of it has been flushed.
- On a parse error the parser resets and resumes rather than failing the run.

**Only `html` streams inside a string.** CSS and JS are emitted only when complete, because a partial
rule or function is useless.

### 3.3 Browser renderer: placeholder → morphing preview → final sandbox

`apps/app/src/components/generative-ui/open-generative-ui/` overrides CopilotKit's built-in renderer
for that `activityType`.

- **Throttle.** Updates are applied at most every 1,000 ms. Milestones flush immediately: CSS
  complete, HTML started or complete, JS arrived, generation ended.
- **CSS-first gate.** Only placeholders are visible until `cssComplete`.
- **Preview iframe.** `processPartialHtml` (forked from `@copilotkit/react-core`):
  - strips a trailing partial tag;
  - strips complete and partial `<style>`, `<script>` and `<head>` blocks;
  - strips a trailing partial entity.

  **Idiomorph** then morphs `document.body`, so existing nodes persist (no flicker). New element
  nodes get a `morph-enter` class that triggers the design system's fade-in.
- **Final sandbox** (on `htmlComplete`). A JetBrains **Websandbox** iframe is built from:
  - a CSP meta: script and connect only to cdnjs, esm.sh, jsdelivr and unpkg; images only from
    USGS tiles and Wikimedia;
  - an **importmap** (three, gsap, d3, chart.js via esm.sh);
  - the trip-animator helper script;
  - design-system CSS (theme tokens, SVG classes, form styles);
  - then the generated CSS and HTML.

  `jsFunctions` runs, then each `jsExpressions` entry, as classic scripts (no top-level `await`).
- **Autosize.** A ResizeObserver inside the iframe posts heights. The host clamps them to 50–4,000 px
  and rejects non-finite values.
- **Bridge.** Only `Websandbox.connection.remote.sendPrompt({text})`, which starts a new agent turn,
  and `openLink({url})` (HTTPS only), both validated by the host. The iframe has no same-origin
  access: no cookies, storage, parent DOM or same-origin fetch.

### 3.4 Lifetime and reuse

- **Each widget is an immutable snapshot**, kept as an activity message in the thread. Its local
  controls keep working.
- **No cross-call patch API.** A follow-up produces a new widget.
- **Widget state is not visible to the agent** unless a user-clicked control sends it in
  `sendPrompt` text.
- **Thread state is in process memory** (`BoundedMemorySaver(max_threads=200)`) and lost on restart.
- **Templates.** A save/apply template mechanism exists in source, but the agent no longer registers
  those tools.

### 3.5 What is pre-built vs generated

- **Generated per answer:** all layout, styling and logic.
- **Pre-built:**
  - the design tokens and CSS;
  - the importmap;
  - `createTripAnimator`, a hand-written controller behind the map scene's polish;
  - `get_trip_stop_images`, a Wikipedia photo lookup with attribution;
  - the A2UI `Table` catalog component (≤12 columns, ≤200 rows, required provenance label).
- **Legacy:** native `barChart`, `pieChart` and `generate_form` remain in source but are filtered
  out by the router.

## 4. What is protocol vs product

**Protocol (plain AG-UI):**

- `TOOL_CALL_ARGS` in;
- `ACTIVITY_SNAPSHOT` / `ACTIVITY_DELTA` with JSON Patch out.

This is the same event family 013 §2.1 already maps.

**Product (CopilotKit-specific):**

- the 373-line middleware;
- the `"open-generative-ui"` content shape (pinned below);
- the React renderer;
- the prompt contract.

Activity content shape, as the app re-declares it in `schema.ts` (it is not exported upstream):

```
{ initialHeight?: number, generating?: boolean,
  css?: string, cssComplete?: boolean,
  html?: string[], htmlComplete?: boolean,
  jsFunctions?: string, jsFunctionsComplete?: boolean,
  jsExpressions?: string[], jsExpressionsComplete?: boolean }
```

## 5. Implications for AgentEngine

1. **Nothing here needs new plumbing.** `ModelDelta` (tool-call deltas), the AG-UI projection and
   JSON-Patch `ACTIVITY_*` (013 §2.1, Q1 resolved) already cover the wire. What is missing is an
   engine-side streaming-argument parser for one declared tool. The parser needs live tool-call
   argument streaming to be surfaced first (`tool-call-argument-streaming-gap.md`).
2. **Matching the content shape above byte-for-byte gives free interop with CopilotKit's renderer.**
   It also needs pinning like 013 §2.0 pins AG-UI, because it is vendor-defined and pre-1.0
   (`-next` builds).
3. **Routing is a narrowing-only seam** and fits ADR-070's Delegated Decision Seam. Jev would be one
   host-supplied implementation, never an engine dependency.
4. **The UI's safety comes entirely from the renderer's sandbox, which the host owns.** The engine's
   own obligation is I3: the payload is opaque data, never executed and never authority. Anything
   returning from the UI re-enters as user input.
5. **OGUI's weak points are worth improving on** rather than copying: no widget identity or patching,
   no state return channel other than free text, in-memory-only persistence, and a full code rewrite
   per follow-up.

## 6. Not established by this pass

- Jev's architecture, accuracy, latency and pricing beyond the vendor's own statements.
- Whether CopilotKit intends `open-generative-ui` as a stable public contract. The `-next` versions
  and the app re-declaring the schema locally suggest it is not yet stable.
- How the launch film was produced beyond the README's own description.
