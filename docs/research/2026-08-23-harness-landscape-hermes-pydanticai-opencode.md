# Research record — Hermes / PydanticAI / OpenCode landscape against AgentEngine and AeroCoWorker

**Compiled:** 2026-08-23 · **Status:** dated snapshot, not a living document

Scoped survey of three named external projects, requested to surface candidate cross-cutting
questions. Follows the comparison-record format of
`2026-08-08-agent-framework-feature-comparison.md`: factual "how it differs," no
better/worse judgment, every AgentEngine-side claim grounded in a direct read of the cited RFC
section, not memory.

**Naming disambiguation, checked first because the terms are overloaded:**
- **"Hermes"** here means the NousResearch **Hermes model family** (Hermes 2/3/4/4.3, open-weight
  LLMs) — not "Hermes Agent," NousResearch's separate agentic framework/app product, which is out
  of scope.
- **"Pi agents"** here means **PydanticAI** (`pydantic-ai`), the Pydantic team's Python agent
  framework — not Physical Intelligence's π0 robotics models, which are unrelated.
- **"OpenCode"** here means `sst/opencode`, the open-source terminal coding-agent harness — the
  only one of the three that is itself a full agent *harness* rather than a model or a
  model-facing framework; compared against AeroCoWorker's shell/Host-Service split, not against
  AgentEngine's model-facing RFCs.

---

## 1. NousResearch Hermes model family — against 004 (Model Provider Plane / ChatClient Plane)

| Finding | AgentEngine mechanism | How it differs / interacts |
|---|---|---|
| Tool calls are ChatML tags (`<tool_call>{"name":...,"arguments":{...}}</tool_call>`, results wrapped `<tool_response>…</tool_response>`) embedded in plain completion text — not a structured `tool_calls` array | `ChatClientCapabilities::tool_calling` bit (004:44-49); "capability set is *per endpoint*, discovered from config, not assumed" (004:96) | **Same model weights produce two different wire shapes depending on serving stack.** vLLM/SGLang ship a normalizing parser (`--tool-call-parser hermes` / `qwen25`) that translates the tags into an OpenAI-shaped `tool_calls` field before the response leaves the server; a raw llama.cpp/GGUF endpoint gives the literal tagged text with no such translation. 004's per-endpoint capability-discovery policy is structurally the right answer (it doesn't assume "the model supports tool calling" implies "the endpoint's wire format is OpenAI-shaped"), but nothing in 004 or 006 names a positive-control test proving an operator can't declare `tool_calling: true` against a bare llama.cpp+Hermes endpoint and have the engine silently treat literal `<tool_call>` text as ordinary `Text` content instead of failing the capability check. |
| Hybrid reasoning is a boolean chat-template flag (`thinking=True` / `enable_thinking`), optional `keep_cots` to retain the trace — not a graded budget | `reasoning_effort {off,low,medium,high}` portable enum (004 §2, ADR-020); "Backends map, and enforce their own native constraints" (004:86-90), already citing Ollama as an existing narrower-level-set example | Consistent with existing design, not a new gap: a Hermes-via-llama.cpp backend is simply a narrower level set than Ollama's, already the documented shape of "backends enforce their own native constraints, fail closed rather than violate them." Worth a concrete backend-mapping test fixture (`{low,medium,high}→think=true`, `off→think=false`) but not a structural question. |
| No single stop-token set across the family (Llama-3.1-based Hermes 3/4 use `<\|eot_id\|>`; Qwen3-based Hermes 4.3 uses Qwen's tokens); tool-result role naming may vary by chat-template version | Backend config is read per-endpoint, not hardcoded per model family (004 §3 backend table) | No gap — 004's per-endpoint-config posture already avoids hardcoding a single family's tokenizer assumptions. Named here only as a concrete reason a shared "Hermes adapter" would need to read `tokenizer_config.json` per checkpoint rather than per family. |
| No structural JSON-mode guarantee from the model itself — Hermes is *trained* to emit and repair valid JSON, but hard enforcement is a serving-layer concern (vLLM guided-decoding/xgrammar, llama.cpp GBNF) | `structured_output_native` / `json_mode` capability bits (004 §2); 003 §4 `OutputSchema<T>` native/tool-shaped/parse-and-repair strategy selection | Matches 004's existing capability-driven model: this is exactly the kind of provider whose `structured_output_native` bit should read `false` even though the model is *trained* toward JSON compliance, because the guarantee lives in the serving layer, not the model. No gap — a useful concrete example for 004's documentation of what "declared, not assumed" capability discovery is protecting against. |
| Fully open-weight (HF, GGUF); NousResearch's own hosted access is a multi-model gateway (Nous Portal), not a Hermes-specific API; no separate proprietary endpoint | "OpenAI-compatible" backend row explicitly lists "vLLM/llama.cpp/Ollama-style local servers" as in-reach (004:96) | No gap — already-named backend reach. |

**Candidate open question surfaced:** see OQ-23 below — whether 004/006 need a named test fixture
proving a misconfigured `tool_calling: true` capability declaration against a completion-only
(no-parser) endpoint fails closed rather than silently misreading literal tag text as ordinary
content.

---

## 2. PydanticAI — against 002 (Agent Model and Authoring), 003 §4 (structured output), 006 (Tool
and Function Plane), 022 (Testing and Evaluation)

| Finding | AgentEngine mechanism | How it differs |
|---|---|---|
| Three-tier structured-output strategy: **Tool Output** (schema as a provider tool, default) → **Native Output** (provider's dedicated JSON-schema mode) → **Prompted Output** (schema in prompt text, parsed from plain reply) for providers with neither native structured output nor tool-calling | 003 §4 `OutputSchema<T>` — native / tool-shaped / **parse-and-repair** | **Not a gap — AgentEngine already has three tiers, not two.** Initial pass under-read this: 003 §4's own third strategy ("parse-and-repair") is the same shape as PydanticAI's Prompted Output tier. Correcting the earlier framing offered to the user before this doc was written: 004's degradation rule (004:51-55, "native structured output → tool-shaped") describes the two-step *capability-fallback* chain: a bare completion-only backend still reaches 003 §4's parse-and-repair strategy as the terminal tier. No new question needed here. |
| `deps_type`/`RunContext[DepsT]`: a typed, per-run dependency object (DB pool, HTTP client, auth token, or a test double) injected into every tool/system-prompt/output-validator function, distinct from what authority the tool holds | Tools receive `EffectContext&` (006 §2 example, `002:51/54` `RunContext&` on middleware lifecycle hooks only) carrying capability handles; no separate non-capability injection seam found in 002/006 | **Real, narrow design question.** AgentEngine's I2 ("no ambient authority — every effect needs an explicitly passed capability") may make a bare non-capability "deps" channel a deliberate non-feature — any data a tool needs should arguably be either an `Args` field or a capability-mediated effect, not a side channel. But PydanticAI's motivating case is specifically test ergonomics (swap a real dependency for a fake one per run without touching capability grants), which is a real, separate concern from authority. Worth checking whether 022's test/simulation harness already solves this a different way (e.g., swapping the bound `ChatClient`/tool implementation at registration time rather than per-run), or whether it's an open gap. See OQ-24 below. |
| Automatic reflect-and-retry: a Pydantic `ValidationError` (or an explicit `ModelRetry` raise) on tool arguments triggers PydanticAI re-prompting the model with the validation error, up to a **configurable, per-tool or agent-wide retry budget** | 006 §3 pipeline step 2 "validate arguments vs schema (reject; do not coerce)"; step 9 "errors → structured `ToolResult{is_error}`"; §3.1 "a tool error is a value returned to the model (001 §6), not an exception" | **Same mechanism at the value level** (a validation failure becomes a value the model sees and can react to next round), **but no per-tool retry-count bound distinct from `MaxTurns<N>`** was found in 006. PydanticAI's bound is scoped to "this tool, this kind of failure," which stops a narrow failure mode (model stuck re-emitting the same malformed call) without spending the whole run's turn budget on it. See OQ-25 below. |
| Multi-agent tiering: single agent → **agent delegation** (`.run()` called inside a `@agent.tool`, sharing `ctx.usage` for cost accounting, control returns to caller) → **programmatic hand-off** (app code sequences `.run()` calls) → **graph-based** (pydantic-graph, typed FSM, opt-in) | 014 §3's eight-row pattern table (all routed through one shared superstep graph engine, per `2026-08-08` comparison record §1); `Handoff<T>` (002 §4.1, single-hop tool-shaped) vs. structured multi-hop graph (014 §3) | Already covered, not a gap — the `2026-08-08-agent-framework-feature-comparison.md` record already established AgentEngine deliberately splits single-hop `Handoff<T>` from the structured graph, which is a comparable two-tier (not three-tier) split to PydanticAI's delegation/hand-off/graph ladder. No new question. |
| `pydantic-graph`'s own lighter persistence (`BaseStatePersistence`, file-based snapshot, `iter_from_persistence()`) is explicitly positioned as separate from and lighter than a full durable-execution engine (Temporal/DBOS/Prefect/Restate integrations cover that tier) | 019 (turn/session checkpoints) + 014 §5 (workflow superstep checkpoints), one shared `Store` seam (005 §2) | Already covered — AgentEngine already unifies both tiers PydanticAI keeps separate (lightweight graph-state snapshot vs. full durable execution) onto one `Store` seam, per prior comparison record. No new question. |
| `pydantic-evals`: `Dataset`/`Case`/`Evaluator`, including **LLM-as-judge** and **span-based evaluators** that score OpenTelemetry traces of internal behavior (tool calls, retries), not just final output | 022 — golden traces (§3) + "rubric-based model grading (with the grader pinned)" (022:63-64) + re-baselining discipline on grader-version bump (022:123-131) | Already covered — 022 already has LLM-as-judge-equivalent rubric grading with a more careful comparability discipline (never compare scores across grader versions) than PydanticAI's docs describe. No new question. Span-based (trace-content) evaluators specifically weren't separately confirmed present or absent — golden traces (§3) are full-run recordings, which is a superset of what a span-based evaluator would read from, so likely already covered by construction; not independently verified line-by-line in this pass. |
| Explicit, tiered provider-capability degradation is a first-class dispatch decision (per-provider variance in structured-output support), not an edge case | 004 §2's declared capability bitset + degradation rule | Already covered, arguably stricter (004 fails fast at `register_agent<A>()` if no declared fallback exists, rather than silently degrading) — no new question. |

**Candidate open questions surfaced:** OQ-24 (non-capability per-run dependency injection) and
OQ-25 (per-tool validation-retry bound) below. The structured-output and multi-agent-tiering
comparisons initially looked like gaps before re-reading 003 §4 and the existing 2026-08-08
comparison record directly — both are already resolved designs, not gaps, and are recorded here
specifically so a future reader doesn't re-raise them without checking first.

---

## 3. OpenCode (`sst/opencode`) — against AeroCoWorker 001/004 and AgentEngine 011/013

| Finding | AeroCoWorker / AgentEngine mechanism | How it differs |
|---|---|---|
| Client/server split: `opencode serve` runs a headless server (session state, tool execution) independent of any attached client; a TUI/desktop/VS-Code/web client attaches over REST + SSE (`/event`, `/global/event`) and can reattach to a running session (`opencode attach <url>`, `session list`/`--continue`) | AeroCoWorker A1 ("work outlives the shell window") — WinUI shell attaches to a running Host Service over a named pipe (004), reattaching resumes rather than restarts (per Specification A1, exercised per CLAUDE.md's "004 G1–G4 and 001 G1–G2 are exercised" note) | **Comparable goal, opposite default.** OpenCode's *default* invocation co-launches client+server in one process tree — only `serve`/`attach` deliberately splits them; the split is opt-in. AeroCoWorker's split is the invariant, never optional (A1/A2). Worth a one-line addition to AeroCoWorkerSpecification.md or 001 noting this contrast explicitly, since it's a real alternative a reader might otherwise assume is equivalent — not a design gap, a documentation opportunity. |
| Transport is plain HTTP + SSE with a **published OpenAPI 3.1 schema** at `/doc` — self-describing and introspectable by any HTTP tooling | 004's named-pipe transport: versioned handshake exchanges protocol version + capability flags (004:71-72), version mismatch fails cleanly (004 G4) — no self-describing schema endpoint found | **Different by design, not obviously a gap.** AeroCoWorker's IPC is a private one-shell-to-one-Host-Service contract (004's own stated scope, §goal), not a multi-vendor client-facing API the way OpenCode's HTTP surface is — the audience that benefits from OpenAPI-style self-description (third-party tooling, ad hoc debugging clients) doesn't obviously exist for 004's contract today. Worth flagging only as a candidate future nice-to-have (e.g., for a test harness or future third-party integration), not as a resolved question — see OQ note below, held as 🟡. |
| Tool permission model: per-tool, pattern-matched `allow`/`ask`/`deny` (glob rules on shell command strings, e.g. `"bash": {"git *": "allow", "rm *": "deny"}`), `--auto` auto-approves anything not explicitly denied, explicit denies always enforced (incl. via `OPENCODE_PERMISSION` env override for CI) | AgentEngine 006 §4 Approval: `never_require`/`always_require`/`PolicyDriven` (host-supplied `{tool, capability_ceiling, arguments_tainted, principal} → {auto_approve, auto_deny, require_approval}` function), hash-bound approval against the exact validated payload, batch escalation rule | Already covered, and stricter — 006's approval is bound to the exact validated call (hash-mismatch re-triggers approval), which a glob-pattern string match doesn't guarantee (a git command matching `"git *": "allow"` could still carry attacker-controlled arguments a hash-bound approval would catch changing between approval and execution). No new question. |
| MCP client config: per-agent enable/disable, local-process vs. remote-HTTP server distinction in `opencode.json` | 011 §3.1 (multi-server discovery mapped into 006 §2 tool plane) — spec'd, not built (M7, per 2026-08-08 comparison record) | No gap in design scope — 011 already specs multi-server discovery with more obligations (JSON Schema 2020-12 validation, `$ref`-fetch hardening) than OpenCode's config surface implies; simply unbuilt, already tracked. |
| JS/TS plugin hook points: `tool.execute.before`, `shell.env`, session/message/permission/TUI events, loaded from npm packages or local files | AgentEngine 009 (WASM component plugins, WIT worlds) — capability-gated, language-agnostic, sandboxed by construction | Different by design, already covered by the existing README framing ("a heavy library becomes safer as a plugin than as a linked host dependency") — OpenCode's plugin surface is in-process JS with host-process authority by default (no sandbox named in the research), a materially weaker isolation model than 009's WASM component ABI. Not a gap for AgentEngine; worth confirming AeroCoWorker's own extension story (if any is ever added) doesn't quietly adopt the weaker shape for convenience. No action item — informational only. |

**No new open question surfaced from this section that isn't already either covered or held at 🟡
informational weight** (the OpenAPI self-description point). Recorded here for completeness since
it was part of the requested scope, not because it forced a new OQ.

---

## 4. Summary — what actually changes OpenQuestions.md

Two real, narrow, previously-unasked cross-cutting questions survive this pass after checking
each candidate against the actual RFC text (several initial impressions turned out to already be
resolved — 003 §4's three-tier structured-output strategy and 022's rubric grading, specifically —
and are recorded above so they aren't re-raised without checking again):

- **OQ-23** — does a misconfigured `tool_calling`-capable endpoint declaration against a
  completion-only backend (e.g., Hermes served bare via llama.cpp with no tool-call-parsing layer)
  fail closed, or silently misread tagged text as ordinary content? (004/006)
- **OQ-24** — should tools have a typed, non-capability per-run dependency-injection seam distinct
  from `EffectContext`, for test-double substitution without touching capability grants, or does
  I2's capability discipline mean this is deliberately out of scope? (002/022)
- **OQ-25** — does the tool-calling loop need a per-tool validation-retry bound distinct from
  `MaxTurns<N>`, to stop a model looping on the same malformed call without spending the whole
  run's turn budget? (006)

All three are 🟠/🟡 (needed-before-implementation-of-area / can-wait), none blocks a v1 decision,
and none has an implementation direction yet — added to `OpenQuestions.md` as OQ-23/24/25,
document-only, matching the standing pattern for a freshly-identified question with no
project-owner implementation direction (cf. OQ-21's framing).
