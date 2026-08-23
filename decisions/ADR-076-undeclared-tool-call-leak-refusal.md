# ADR-076 — Refusing a response that leaks a tool call as text when `tool_calling` is declared but the recovery scan isn't armed

**Status:** Judged (2026-08-23, approved by project owner). Design → red-team → prove complete, all
in one session: design draft written and self-attacked, an independent fresh-context red-team pass
found one required revision (incorporated before any code was written) and three residuals (named,
not fixed), then implemented and proven against the real codebase with 22 new passing tests and a
full-suite regression check. **Resolves `OpenQuestions.md` OQ-23.** **Numbered 076, not 074**: judged
and drafted locally as ADR-074, but `origin/main` had already independently claimed both 074 and 075
(`ADR-074-composed-context-provider-consolidation.md`, `ADR-075-context-budget-fail-closed.md`, an
unrelated concurrent session) by the time this branch was pushed — caught and renumbered at merge
time, before 074 appeared anywhere public under this ADR's content.

**Relates to:** `decisions/ADR-023-response-format-codec-seam.md` (the codec/declassifier this ADR
sits beside and does not modify — reuses its `response_format_codec::decode_response_format()` as a
pure detector), `decisions/ADR-035-chatclient-streaming-completeness.md` (the backend-agnostic
`AgentSession::run_model_call()` centralization this ADR's insertion point reuses),
`004-Model-Provider-Plane.md` §2 (`ChatClientCapabilities` — "declared, not probed" — this ADR adds
the one enforced check that a declared `tool_calling` bit isn't quietly contradicted by what a
response actually contains). Design draft, kept as the full uncompressed record:
`docs/planning/oq23-undeclared-tool-call-leak-design-draft.md`. Confirming fixture for the
underlying gap: `tests/test_openai_chat_client_translation.cpp`'s `OQ-23-R1` block.

## 1. The question

**Stated so it has a wrong answer:** an operator declares `ChatClientCapabilities::tool_calling =
true` for a `ChatClient` — a real, plausible configuration, because the bound model genuinely
supports tool calling. That endpoint is, in fact, served raw (e.g. llama.cpp/GGUF Hermes/Qwen with
no `--tool-call-parser`), so a tool-call attempt arrives as literal `<tool_call>{"name":...}
</tool_call>` text inside `content`, not a structured `tool_calls[]` entry. `OQ-23-R1` (a real
fixture, not a hypothetical) confirms today's actual behavior: this is captured as an ordinary,
untainted `Text` item — no error, no warning — unless the operator has *also*, separately, armed
`AgentSession::scan_response_format_leaks` (default `false`). The two flags have no relationship to
each other anywhere in the code. Should declaring `tool_calling: true`, by itself, cause the engine
to notice and refuse such a response, rather than silently passing it through as if the model simply
chose not to call a tool?

## 2. Non-negotiable constraints (not themselves contested)

- **I2/I3, and ADR-023 §2 exactly as already locked:** a text-derived reconstruction may never
  acquire the same unconditional authority as a vendor-structured tool call, and no
  capability-granting or policy-deciding API may accept a tainted value. This design may only ever
  produce a refusal (or nothing) — never promote, invoke, or grant anything. It does not touch,
  weaken, or duplicate ADR-023's own declassifier (`invoke_tool` step 5).
- **ADR-023 Finding 6, unchanged:** scanning/detection must be operator-armed, never switched on by
  content alone. This design's trigger is `ChatClientCapabilities::tool_calling == true` — an
  explicit, pre-existing operator declaration, not a response-content heuristic.
- **004 §2:** capabilities are declared, not probed. This design does not turn `tool_calling` into a
  runtime probe of the backend; it only checks that a response consistent with the backend NOT
  actually having the capability it declared doesn't get silently treated as if everything were fine.

## 3. Competing designs

**Design A — Auto-arm `scan_response_format_leaks` whenever `tool_calling` is declared true.**
Steelman: zero new mechanism, smallest possible diff. **Rejected:** conflates two orthogonal
properties ("this backend supports tool calling," true for nearly every configured endpoint,
including every well-behaved vendor-structured OpenAI/Anthropic API that never leaks; and "this
backend's serving layer might leak raw format tokens," true only for a minority of raw/self-hosted
configurations). Auto-arming would run the full decode-and-**promote** pipeline — including
ADR-023's auto-declassify-to-a-real-`ToolCall` path — on every response from every correctly-behaving
backend, for zero benefit on the common path, and (red-team, steelmanning a "softened A" that
auto-arms scanning but forces mandatory approval instead of auto-declassify) still loses to Design D:
it surfaces attacker-chosen recipient/arguments into a human approval UI (ADR-023 Finding 3's own
named cost) for every `tool_calling`-declaring backend, which D avoids entirely.

**Design B — Construction-time validation: reject/warn if `tool_calling == true` and
`scan_response_format_leaks == false`.** **Rejected:** content-blind — cannot distinguish "this
backend genuinely never leaks" (the correct, common case) from "this backend might leak," so has a
near-100% false-positive rate against legitimate deployments. Red-team's steelmanned "softened B" — a
non-blocking diagnostic-only startup warning instead of a hard reject — survives (it blocks nothing)
and is judged a reasonable **complement** to D, not a replacement; not required for D, left as
low-cost future work, not built here.

**Design C — Always-on content sniff, unconditional on `tool_calling`.** **Rejected:** directly
violates ADR-023 Finding 6's already-adjudicated scoping. Exposes every deployment, including ones
that never intend to use tool-calling at all, to the detection cost and to the confused-deputy shape
named in §5, with no operator opt-in at all.

**Design D (chosen) — A new, detect-only, refuse-not-promote check, gated on `tool_calling == true`
AND `scan_response_format_leaks == false`, centralized at `AgentSession::run_model_call()`'s existing
choke point.** Reuses `response_format_codec::decode_response_format()` (ADR-023's existing sniff +
candidate extraction — no new parser) purely as a *detector*: for each plain, **untainted** `Text`
item in the response, decode it; if any resulting candidate's `recipient` matches a tool name in the
same live `request.tools` list `apply_response_format_scan` itself already consults, refuse the
entire response with a `failure_class::contract` error — mirroring the exact shape
`validate_outbound_media_capabilities`'s existing gate already uses one line above this design's
insertion point (`agent_session.hpp:1581-1586`) — instead of returning it as a successful
`ChatResponse`. Never constructs a `ToolCall`, never reads or validates `arguments_json` against a
schema, never touches `invoke_tool`. Runs only in the exact window that had zero coverage before this
ADR; when scan IS armed, this check does not run at all — ADR-023's own already-red-teamed
promote-or-diagnostic mechanism is already the answer there.

## 4. Falsifiable claims

| # | Claim | Verdict | Basis |
|---|---|---|---|
| D1 | The new check fires on the exact `OQ-23-R1` scenario and returns a `Contract`-class error instead of a successful response. | **CORRECT** | `tests/test_openai_chat_client_translation.cpp` `OQ-23-D1`: a literal llama.cpp-raw-Hermes leak matching a live `get_weather` tool is refused with `chat_client.undeclared_tool_call_leak`, `failure_class::contract`. |
| D2 | The check does not fire when scanning is armed, AND does not misfire on an unmatched-candidate diagnostic left behind by a prior scan pass. | **CORRECT, two scenarios both proven, not one** | `OQ-23-D2a`: run against an already-`apply_response_format_scan`-promoted message — silent (nothing left to detect). `OQ-23-D2b`: run DIRECTLY against a tainted, unmatched-candidate diagnostic `Text` (`ADR-023 P2-R2`'s own shape) — silent, because the detector's own `item.tainted` skip holds independent of which call site invokes it (the red-team's required revision, built into the implementation from the start, not bolted on after a failing test). |
| D3 | The check does not fire on ordinary clean content, or on a leak whose recipient is not a currently-offered tool name. | **CORRECT** | `OQ-23-D3` (two sub-cases) at the function level; `OQ-M3`/`OQ-M4` at the real `AgentSession::run_model_call()` level (unrecognized recipient converges normally; ordinary clean content is byte-identical pass-through). |
| D4 | The check never produces or exposes a `ToolCall`/`ToolCallRequest` under any input. | **CORRECT, structural** | `detect_undeclared_tool_call_leak`'s return type is `result<void>`; no `ToolCall`/`ContentItem` construction appears anywhere in its body (`core/response_format_leak_scan.hpp`) — a property of the code's shape, not a runtime test outcome. |
| D5 | A malicious tool-result forging a tool-call-shaped block addressed at a real, live tool forces a refusal with no side effect beyond the one refused response, and does not retry/loop. | **CORRECT, traced against real code, not assumed** | `OQ-M1`: exactly one streamed round happens (`client.stream_call_count == 1`), a `RunFailed` event carries the specific code. Independently confirmed at the mechanism level: `failure_class::contract` is retried by neither `ModelCallGateway::attempt_with_retry`/`stream_attempt_with_retry` (`model_call_gateway.hpp:116-117`, retries only `transient`) nor `WorkflowSupervisor`'s edge-level retry (`workflow_supervisor.hpp:1030-1033`, retries only `transient`/`resource`); `run_rounds()` returns immediately on `!response` (`agent_session.hpp`), so there is no loop within one run that could re-encounter the same content. |

## 5. The red-team attack

An independent, fresh-context pass (not the design's own author) attacked every claim above and the
design's self-identified DoS/confused-deputy-adjacent shape before any code existed. Full record:
`docs/planning/oq23-undeclared-tool-call-leak-design-draft.md` §5 and §8. Summary:

**Must-fix, found and closed before implementation:** the original justification for "the check
doesn't fire when scanning is armed" (D2) was only proven for the *matched* candidate case
(promoted to a real `ToolCall`, so nothing is left to inspect); it was never checked against the
*unmatched* case (`ADR-023 P2-R2`'s shape — a tainted diagnostic `Text` left behind), which
`response_format_leak_scan.hpp`'s own existing convention already documents as unsafe to blindly
re-decode. **Fix:** the detector explicitly skips `item.tainted` content, as a structural property
independent of whichever call site invokes it — not merely because today's two insertion points
happen to be mutually exclusive. Proven directly by `OQ-23-D2b`, which feeds the tainted diagnostic
to the detector with no intervening call-site logic at all.

**The DoS-shaped attack, analyzed in full, not merely conceded as a residual:** an attacker who
controls fetched content (a web page, a search result) can embed a forged tool-call-shaped block
addressed at a real, live tool name; when the model later quotes it, this design refuses that
response. Red-team traced the real retry and turn-counting code (not assumed) and found:
`failure_class::contract` is retried nowhere in the engine (§4's D5 basis above), so the refusal is
bounded to exactly one wasted run — categorically safer than ADR-023 Finding 1's original danger
(attacker-controlled arguments reaching a real capability-bearing tool), since this design only ever
refuses, never promotes. **Two real, out-of-engine-scope surfaces named, not silently assumed away**:
(1) a *host* that naively retries a failed `start_run()` could re-trigger the same refusal
indefinitely — the identical shape every other `failure_class::contract` refusal already has
(e.g. `validate_outbound_media_capabilities`), not new to this design, and consistent with
CLAUDE.md's locked decision that the host, not AgentEngine, owns inbound retry/orchestration;
(2) cross-session amplification — one poisoned page fetched by many independent sessions forces one
wasted, refused run *per session*, a genuinely new availability surface this design introduces (no
signal existed before it), bounded per-run (one cheap linear sniff) but not rate-limited across
sessions by this design, which is correct given cross-session admission is host-owned by the same
locked architecture decision, not an oversight.

**Two additional findings not originally named, added as residuals (§6):** a tool-name-enumeration
oracle (a forged block addressed at a real tool name now produces an observably different outcome
than one addressed at a fabricated name — a new signal, requiring an attacker vantage point on run
success/failure that's narrower than the base injection scenario); and a domain-specific
false-positive risk (this project's own likely deployment population — debugging bots for LLM-serving
misconfiguration — is unusually likely to have legitimate conversations quoting real tool-call syntax
with a real, live tool name, so the "asking about tool-call syntax" false positive may be less rare
here than in a generic deployment).

**Verdict: no finding was fatal to the design's central safety claim.** A decision to refuse,
derived from tainted content, is not "deriving an approval decision" (007 §4's actual prohibition) —
this design never grants, promotes, or invokes anything, so it does not reopen 007 §4 regardless of
what triggers the refusal.

## 6. The decision

**Design D is accepted, implemented, and proven.**

- `include/agentengine/core/response_format_leak_scan.hpp` — new
  `detect_undeclared_tool_call_leak(Message const&, std::vector<ToolDescriptor> const&) ->
  result<void>`, with the required `item.tainted` skip built in.
- `include/agentengine/rt/agent_session.hpp` — wired into `run_model_call()` at both real insertion
  points (the non-streaming early-return branch and the shared gateway/streaming tail) as an
  `else if` sibling to the existing `scan_response_format_leaks_` branch, so mutual exclusivity with
  that mechanism is structural, not incidental. Emits `run_event_kind::run_failed` with the specific
  error code on refusal, mirroring `validate_outbound_media_capabilities`'s existing gate exactly.
- `include/agentengine/protocol/openai/chat_client.hpp` — the `scan_response_format_leaks`
  constructor parameter's comment now names the scope gap this design leaves at the direct-call
  level (§7). `AnthropicChatClient` needed no equivalent comment — confirmed it never had its own
  per-backend scan flag to annotate (ADR-035's "closing Anthropic's total gap" already meant scanning
  was `AgentSession`-only for that backend from the start).

**Binds:** `004-Model-Provider-Plane.md` §2 — the degradation rule already states capabilities are
"declared, not probed" for the outbound direction (what the engine sends); this ADR adds the
symmetric inbound enforcement (what a response actually contains is checked against what
`tool_calling` declared, when the one existing recovery path for a mismatch isn't armed). See the
amendment added to 004 §2 in this same change.

**Evidence (`docs/planning/oq23-undeclared-tool-call-leak-design-draft.md` §10 has the full
uncompressed record):** 22 new tests, all passing — 9 function-level (`OQ-23-D1/D2a/D2b/D3` in
`tests/test_openai_chat_client_translation.cpp`) and 13 real `AgentSession::run_model_call()` round
trips (`OQ-M1..OQ-M4` in `tests/test_rt_agent_session_streaming_and_events.cpp`, via a
`ScriptedChatClient` extended with a `caps_tool_calling` flag, default `false`, so no existing test
was disturbed). Full project build: clean. Full `ctest`: 227/229 real passes. The only 2 failures
(`test_openai_chat_client_live`, `test_anthropic_chat_client_live`, both in the same live-TLS
tool-call-assembly sub-check, unrelated to this change) were **confirmed pre-existing**, not a
regression — reproduced identically against the untouched pre-change baseline via `git stash`,
rebuild, and re-run, then restored.

## 7. Residual risks and deferred work (not closed by this ADR)

- **Named, not closed:** the `AgentSession`-only scope. A direct `OpenAIChatClient::chat()` call
  bypassing `AgentSession` entirely gets none of this protection — matching today's status quo for
  the *existing* `scan_response_format_leaks` mechanism (`OpenAIChatClient` keeps its own independent
  copy of that flag for exactly this direct-call case). Centralizing at `AgentSession` first (the
  same order ADR-035 already took for `apply_response_format_scan` itself) rather than doubling the
  first pass's surface was a deliberate choice, not an oversight; porting down to the `ChatClient`
  layer is real, named follow-on work if judged valuable later.
- **Named, not mitigated:** the tool-name-enumeration oracle (§5) and the domain-specific
  false-positive risk (§5) — both real, both bounded (a refusal, never a promotion or grant), neither
  closed by this design.
- **Named, out of engine scope by design:** cross-session/cross-tenant rate-limiting against repeated
  refusals from the same poisoned content, and protection against a host that naively retries a
  failed run — both are the host's responsibility under this project's locked
  networking/orchestration architecture (CLAUDE.md), not gaps this ADR should have closed.
- **Format coverage** is bounded by the same four built-in `response_format_codec` table rows
  ADR-023 §7 already names as a limitation — inherited, not reopened.
- **Streaming-arrival leaks are not covered** — this design, like ADR-023 itself, is scoped to
  whole-message, non-streaming decoding. A leak arriving only via a live stream (as opposed to being
  present in the final reconstructed `Message` `run_model_call()` inspects) is out of scope, matching
  ADR-023's own v1 restriction.
- **Softened Design B** (a non-blocking startup diagnostic nudging an operator toward arming
  `scan_response_format_leaks` for a raw/self-hosted endpoint) was judged a reasonable complement,
  not built here — real, low-cost, optional follow-on work.
