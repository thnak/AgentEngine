# OQ-23 — Detecting a `tool_calling`-declared endpoint that silently leaks tool calls as text

**Status:** Design draft, red-teamed once (2026-08-23, independent fresh-context pass — see §8).
Recommendation was **(b) proceed with named required revisions**, all incorporated below (§3's
Design D spec, §5's DoS analysis, §6's residuals). Not yet judged, not yet implemented. Relates to
`OpenQuestions.md` OQ-23, `decisions/ADR-023-response-format-codec-seam.md` (the codec/declassifier
this design sits beside without modifying), `decisions/ADR-035-chatclient-streaming-completeness.md`
(the backend-agnostic `AgentSession::run_model_call()` centralization this design reuses as its
insertion point). Confirming fixture for the underlying gap:
`tests/test_openai_chat_client_translation.cpp`'s `OQ-23-R1` block (2026-08-23, 9/9 passing).

## 1. The question

OQ-23, confirmed real by a passing fixture (`OQ-23-R1`): an operator declares
`ChatClientCapabilities::tool_calling = true` for an endpoint that is, in fact, serving raw text —
e.g. llama.cpp/GGUF Hermes/Qwen without `--tool-call-parser`. When the model attempts a tool call, it
arrives as literal `<tool_call>{"name":...}</tool_call>` text inside `content`, not a structured
`tool_calls[]` entry. Today this is captured as an ordinary, untainted `Text` item — no error, no
warning — **unless** the operator has *also*, separately, armed `AgentSession::scan_response_format_leaks`
(default `false`, ADR-023 Finding 6: "operator-armed, never content-triggered"). The two flags
(`tool_calling`, `scan_response_format_leaks`) have no relationship to each other anywhere in the code.

**Stated so it has a wrong answer:** should declaring `tool_calling: true` for a `ChatClient`, by
itself, cause the engine to notice — and refuse, rather than silently pass through — a response whose
content structurally looks like a leaked tool call to a real, currently-offered tool, when the scan
that would otherwise recover it is not armed?

## 2. Non-negotiable constraints (not themselves contested)

- **I2/I3, and ADR-023 §2 exactly as already locked:** a text-derived reconstruction may never acquire
  the same unconditional authority as a vendor-structured tool call, and no capability-granting or
  policy-deciding API may accept a tainted value. **This design must never promote, invoke, or grant
  anything** — it may only ever produce a refusal (or nothing). It does not touch, weaken, or
  duplicate ADR-023's own declassifier (`invoke_tool` step 5, `is_auto_declassifiable_text_derived_call`).
- **ADR-023 Finding 6, unchanged:** scanning/detection must be operator-armed, never switched on by
  content alone. This design's trigger is `ChatClientCapabilities::tool_calling == true` — an explicit,
  pre-existing operator declaration, not a response-content heuristic — so it inherits, rather than
  reopens, that already-settled scoping argument.
- **004 §2:** capabilities are declared, not probed. This design does not turn `tool_calling` into a
  runtime probe of the backend; it only checks that a response consistent with the backend NOT having
  the capability it declared doesn't get silently treated as if everything were fine.

## 3. Competing designs

**Design A — Auto-arm `scan_response_format_leaks` whenever `tool_calling` is declared true.** Tie the
two existing flags together; remove the independent misconfiguration window by construction.
Steelman: zero new mechanism, smallest possible diff, directly closes the exact gap named.
**Attack:** conflates two orthogonal properties — "this backend supports tool calling" (true for the
overwhelming majority of configured endpoints, including every well-behaved vendor-structured OpenAI/
Anthropic API that never leaks) and "this backend's serving layer might leak raw format tokens into
`content`" (true only for a minority of raw/self-hosted configurations). Auto-arming would run the
full decode-and-promote pipeline — including ADR-023's auto-declassify-to-a-real-`ToolCall` path
(Finding 4's `a′`) — on every response from every correctly-behaving backend, unconditionally widening
the blast radius of an already-carefully-scoped mechanism (Finding 6 scoped it to *explicit* per-
endpoint arming precisely so it wouldn't become "on for everyone by default") for zero benefit on the
common path. **Rejected**: wrong shape — it silently makes promotion more aggressive project-wide to
fix a narrower "the operator should have known" problem.

**Design B — Construction-time / config-validation check: reject or warn if `tool_calling == true` and
`scan_response_format_leaks == false`.** Steelman: catches the misconfiguration before any traffic
flows at all, cheapest possible enforcement point. **Attack:** content-blind — it cannot distinguish
"this backend genuinely never leaks" (the correct, common configuration, which would now be flagged as
wrong) from "this backend might leak" (the actual target). A blanket check here has a near-100%
false-positive rate against every legitimate OpenAI/Anthropic deployment. Rejected without a new,
explicit "this endpoint is raw/self-hosted" declaration field — and even with one, it only helps an
operator who has already correctly self-classified their endpoint, which is exactly the judgment the
misconfiguration scenario shows cannot be relied on.

**Design C — Always-on content sniff, unconditional on `tool_calling`.** Scan every response from every
backend, regardless of declared capabilities, and refuse on any tool-call-shaped match. **Attack:**
directly violates ADR-023 Finding 6's already-adjudicated scoping ("an explicitly accepted, scoped risk
on one named, known-imperfect serving-layer configuration" vs. "a new attack class exposed to everyone
by default"). Exposes every deployment — including ones that never intend to use tool-calling at all —
to the detection cost and, more importantly, to Finding 1's confused-deputy shape (see §5) with no
operator opt-in at all. **Rejected**, same reasoning ADR-023 already used once.

**Design D (chosen) — A new, detect-only, refuse-not-promote check, gated on `tool_calling == true`
AND `scan_response_format_leaks == false`, centralized at `AgentSession::run_model_call()`'s existing
choke point.** Reuses `response_format_codec::decode_response_format()` (ADR-023's existing sniff +
candidate extraction — no new parser) purely as a *detector*: for each plain **untainted** `Text` item
in the response (`item.tainted == true` items are explicitly skipped, see the required revision
below), decode it; if any resulting candidate's `recipient` matches a tool name in the SAME live
`request.tools` list `apply_response_format_scan` itself already consults, refuse the entire response
with a `failure_class::contract` error — mirroring the exact shape already used one line above this
design's insertion point (`validate_outbound_media_capabilities`'s gate at
`agent_session.hpp:1581-1586`) — instead of returning it as a successful `ChatResponse`. Never
constructs a `ToolCall`, never reads or validates `arguments_json` against a schema, never touches
`invoke_tool`. Runs *only* in the exact window today has zero coverage in (`tool_calling` declared,
scan not armed); when scan IS armed, this check does not run at all — ADR-023's own already-red-teamed
promote-or-diagnostic mechanism is already the answer there, and this design does not second-guess it.

**Required revision from red-team (2026-08-23), incorporated here, not left as a follow-on:** the
detector must skip `item.tainted` content items **explicitly, in its own code**, mirroring
`response_format_leak_scan.hpp:43-52`'s own established convention for exactly the same hazard — not
rely on the two insertion sites (armed vs. unarmed) staying mutually exclusive as the only thing
preventing a double-decode. Red-team traced the real control flow and confirmed today's wiring makes
the two branches mutually exclusive in practice, so this was not exploitable as specified — but the
draft's original justification for D2 ("nothing left to detect after promotion") is only true for the
*matched* candidate case (recipient found, promoted to a real `ToolCall`, item type changes so there's
nothing left to inspect); it does NOT hold for the *unmatched* candidate case (`ADR-023 P2-R2`'s own
shape — a candidate whose recipient does *not* match any live tool stays behind as a `tainted == true`
diagnostic `Text` item, `response_format_leak_scan.hpp:70-76`). Without an explicit tainted-skip, a
future change that made the two insertion sites NOT mutually exclusive (e.g. a caller arming both
flags, or a refactor that runs the detector unconditionally after the scan step regardless of
`scan_response_format_leaks_`) would silently reintroduce `response_format_leak_scan.hpp`'s own
documented re-scan hazard into this new code path. The tainted-skip is cheap, structural,
defense-in-depth, and required regardless of whether today's wiring happens to make it unreachable.

## 4. Falsifiable claims

| # | Claim | How to check |
|---|---|---|
| D1 | The new check fires on the exact `OQ-23-R1` fixture scenario (`tool_calling` declared, scan unarmed, literal Hermes leak matching a live tool name) and returns a `Contract`-class error instead of a successful response. | Extend `OQ-23-R1` (or a sibling block) to call the new function directly against the same fixture content + a `ToolDescriptor{"get_weather"}` and assert `!result.has_value()`. |
| D2 | The check does NOT fire when scanning is armed (ADR-023's existing path handles it) — no double error, no interference with `ADR-023 P2-R1`'s existing promote behavior; AND does not misfire on an unmatched-candidate diagnostic left behind by a prior scan pass. | Two scenarios, not one: (a) run the new check immediately after `apply_response_format_scan` on the SAME already-promoted message from `ADR-023 P2-R1`; assert no error (the `Text` item is gone, replaced by a `ToolCall`). (b) run the new check directly against the `ADR-023 P2-R2` fixture's OUTPUT (a `tainted == true` diagnostic `Text` left behind by an unmatched candidate); assert no error — proving the detector's own `item.tainted` skip holds even when fed a diagnostic directly, independent of which branch called it. |
| D3 | The check does NOT fire on ordinary clean content, or on a leak whose recipient is NOT a currently-offered tool name. | Reuse `ADR-023 P1-R2` (clean content) and `ADR-023 P2-R2` (unrecognized name) fixtures against the new function; assert both return success (no refusal). |
| D4 | The check never produces or exposes a `ToolCall`/`ToolCallRequest` under any input — the refusal path constructs nothing invokable. | Code-level: the function's return type is `result<void>`; no `ToolCall` construction appears anywhere in it (structural, not merely tested). |
| D5 | A malicious tool-result (fetched page) containing forged tool-call-shaped text addressed at a real, live tool can force a refusal on an otherwise-benign turn that quotes/summarizes it (the DoS-shaped concern named in §5). | Construct the confused-deputy fixture ADR-023 §4b Finding 1 already describes; run it through the new check; confirm it refuses (expected — see §5 for why this is treated as an acceptable, bounded cost, not a blocker) and confirm the refusal carries no side effect beyond the one refused response (no state corruption, no privilege change). |

## 5. Attack surface — DoS/confused-deputy-adjacent shape (red-teamed, 2026-08-23)

**The shape, structurally similar to ADR-023 §4b Finding 1 but with refusal instead of promotion as
the payload.** An attacker who controls content the agent fetches (a web page, a search result, any
tool result) can embed `<tool_call>{"name":"<a real live tool name>","arguments":{...syntactically
valid JSON...}}</tool_call>` inside otherwise-ordinary text. If the model later quotes or summarizes
that content verbatim (ordinary, expected behavior), the forged block reaches a fresh assistant turn
exactly the way Finding 1 describes — and this design's check, running over that response, would
refuse it: `Contract` error, `run_failed`, no `ToolCall` ever created.

**Red-team traced the real retry/turn-counting code (not assumed) to check whether refusal is actually
bounded per-run, and confirmed it is, for a precise, code-grounded reason:**
- `failure_class::contract` (this design's refusal class, matching `validate_outbound_media_capabilities`'s
  own gate) is **not retried** by either engine-level retry mechanism: `ModelCallGateway::attempt_with_retry`/
  `stream_attempt_with_retry` (`model_call_gateway.hpp:116-117`) retries only `failure_class::transient`;
  `WorkflowSupervisor`'s edge-level retry (`workflow_supervisor.hpp:1030-1033`) retries only `transient`
  and `resource`. Neither retries `contract`. The new check also runs *outside* the gateway's own
  retry-scoped `call()`/`call_stream()`, so it isn't reachable from inside that loop at all.
- `run_rounds()` returns `std::unexpected` immediately on `!response` (`agent_session.hpp:1943-1947`);
  `turn_index` only advances in the loop's increment clause, never reached on this path — so the refusal
  technically doesn't count against `MaxTurns<N>`. **This is moot, not a gap**: a `run_model_call()`
  failure terminates the entire run, not just the current turn — there is no loop within one
  `run_rounds()` invocation that could re-encounter the same poisoned content a second time. The
  `MaxTurns<N>`-doesn't-bound-it concern is technically true of the counter and irrelevant in practice.

**Two real residual surfaces this design does NOT close, stated precisely rather than folded into a
vague "acceptable residual" claim:**
1. **Host-level naive retry-on-failure.** AgentEngine does not own inbound orchestration (CLAUDE.md's
   locked networking decision — the host owns the socket and calls `start_run()`). Nothing prevents a
   host from wrapping a failed `start_run()` in its own retry logic; if the same poisoned tool result
   re-enters history and the model quotes it again, a fresh run produces a fresh refusal, repeatable as
   many times as the host's own retry policy allows. This is the identical shape every other
   `failure_class::contract` refusal already has (e.g. `validate_outbound_media_capabilities` has the
   same property) — not a defect specific to this design — but the bound depends on host behavior this
   design has no visibility into, and that dependency must be stated, not implied.
2. **Cross-session amplification is real and structurally new.** Before this design, a forged block
   addressed at any name — real or fabricated — produced byte-identical silent pass-through. After this
   design, one poisoned page fetched by many independent sessions/tenants forces one wasted, refused run
   *per session* — bounded per-run (the added cost is one cheap linear sniff, per ADR-023 §4b Finding 7's
   already-accepted buffered-cost conclusion) but a genuine new availability surface that did not exist
   before. `TokenBudget<N>` provides no protection (usage is never accounted on a refused response,
   `agent_session.hpp:1949` is unreached; TokenBudget is per-run/session and cannot bound an attacker
   spawning many *fresh* sessions against the same poisoned content anyway). **Consistent with, not a
   violation of, this project's own architecture**: cross-session admission/rate-limiting is
   host-owned by the same locked decision named above — this design does not provide it and should not
   attempt to, but must say explicitly that it doesn't, rather than let the "bounded residual" framing
   read as "bounded, full stop."

**Verdict (red-team): the core safety argument holds.** A decision to *refuse*, derived from tainted
content, is not "deriving approval" — 007 §4's prohibition is specifically about approval decisions,
and this design never grants, promotes, or invokes anything. Refusal is categorically safer than
Finding 1's original danger (attacker-controlled arguments reaching a real capability-bearing tool).
The specificity bar (a real, live tool name plus syntactically-valid JSON in an exact wire-format
shape) is the same bar ADR-023's own red-team already accepted as sufficiently narrow for the
*promotion* mechanism (§4b Finding 4's `a′`) — reusing it here, for a strictly less dangerous outcome
(refuse vs. promote), does not need a stricter bar.

**Two additional findings from red-team not named in the original draft, added as residuals (§6):**
- **A tool-name-enumeration oracle.** Pre-design, a forged block's outcome was identical regardless of
  which name it addressed (silent pass-through either way). Post-design, a block addressed at a real,
  live tool name produces a hard, observable refusal, while one addressed at a fabricated name still
  passes through silently — a new, previously-absent observable signal. An attacker with some indirect
  visibility into run success/failure (error logs, timing, a support surface's own behavior) — but
  *without* already knowing the tool surface — could spray candidate names across served content to
  enumerate which tool names are live. Requires a narrower vantage point than the base injection
  scenario, not fatal, but real and not previously named.
- **A domain-specific false-positive risk likely underestimated in the original draft.** AgentEngine's
  own plausible deployment population (support/debugging bots for LLM-serving-layer configuration
  issues — the exact population OQ-23 is about) is unusually likely to have legitimate conversations
  that quote real tool-call syntax addressed at a real, live tool name (e.g., explaining this very bug
  to a user). The "asking about tool-call syntax" false-positive case, conceded in the original draft as
  rare, may not be rare for this project's own users.
- **A perverse operator incentive**, not a code flaw: if refusals become operationally annoying, the
  natural operator response is to arm `scan_response_format_leaks` blanket-wide, trading this design's
  safe-refuse residual for ADR-023's promotion-capable one (with all of that mechanism's own
  carefully-scoped residuals attached). Worth naming so an operator-facing error message can point at
  the right fix (arm scanning for THIS endpoint specifically) rather than encourage a blanket toggle.

## 6. What this design does NOT do (named, not silently assumed)

- Does not change ADR-023's declassifier, its capability-scoped auto-declassify rule, or anything in
  `invoke_tool`.
- Does not change default behavior for any endpoint that has `tool_calling == false`, or that has
  `scan_response_format_leaks == true` already armed.
- Does not attempt format-coverage beyond the four built-in `response_format_codec` table rows (same
  named limitation ADR-023 §7 already carries).
- Does not address the streaming path (`chat_stream()`/`drain_streaming_response()`) — matching
  ADR-023's own v1 scope restriction to whole-message, non-streaming decoding (§6 point 2). A leak
  arriving only via a live stream is not covered by this design either, named as an inherited gap, not
  newly introduced.
- Does not decide whether the *existing* misconfiguration (declared `tool_calling`, unarmed scan,
  `vendor_structured`-only content — i.e. a backend that is behaving exactly as declared) should ever
  be flagged; only fires on content that structurally looks like a specific, addressed leak.
- Does not cover a direct `OpenAIChatClient::chat()`/`AnthropicChatClient::chat()` call that bypasses
  `AgentSession` entirely — matching today's status quo for the *existing* `scan_response_format_leaks`
  mechanism (`OpenAIChatClient` keeps its own independent copy of that flag,
  `protocol/openai/chat_client.hpp:872,892,929-931`, for exactly this direct-call case). Red-team's
  opinion, adopted here: centralize at `AgentSession` first (the same order ADR-035 already took for
  `apply_response_format_scan` itself, from a `ChatClient`-local mechanism to a session-level one), and
  port down to the `ChatClient` layer only if judged valuable later — do not double the first pass's
  surface. **Required, not deferred**: name this gap in the `ChatClient` header comment near
  `scan_response_format_leaks_`, so a direct caller isn't misled into thinking `tool_calling=true` alone
  gets them this protection.
- Does not provide cross-session or cross-tenant rate-limiting against repeated refusals from the same
  poisoned content (§5) — host-owned by this project's locked networking/admission architecture, not an
  oversight.
- Does not close the tool-name-enumeration oracle named in §5 — named as a residual, not mitigated.

## 7. Considered-and-rejected variants (added after red-team steelmanning)

Red-team attempted to steelman harder versions of Designs A and B before accepting D; neither survived,
but both are worth recording as considered rather than silently absent:
- **"Softened A" — auto-arm scanning but force mandatory approval, never auto-declassify, whenever
  arming was implicit (not operator-explicit).** Still loses to D: it surfaces attacker-chosen
  recipient/arguments into a human approval UI (ADR-023 §4b Finding 3's own already-named cost) for
  every `tool_calling`-declaring backend, which D avoids entirely by never promoting anything.
- **"Softened B" — a non-blocking, diagnostic-only startup warning** (not a hard construction-time
  reject) when `tool_calling == true` and `scan_response_format_leaks == false`. This does not have B's
  fatal false-positive-*rejection* problem, since it blocks nothing. Judged a reasonable **complement**
  to D (nudging an operator toward arming the scan for a raw/self-hosted endpoint specifically), not a
  replacement — low-cost additive work, not required for D to proceed.

## 8. Red-team findings (2026-08-23, independent fresh-context pass)

**Recommendation: (b) proceed with named required revisions — all incorporated into this draft.**
Per-claim verdicts: D1 confirms-clean (contingent on implementation matching the spec); D2 confirmed
correct for the real insertion point but its original justification was incomplete (fixed, see §3's
required revision and §4's revised D2 test plan); D3 confirms-clean; D4 not yet verifiable (no code
exists — kept as a hard structural gate before landing, §9); D5 confirms-clean, with the fuller §5
analysis above superseding the original draft's narrower framing. Designs A/B/C were confirmed
correctly rejected, including against steelmanned harder variants (§7). No finding was fatal to Design
D's central safety claim (refuse-not-promote holds under I2/I3 given the real, traced control flow) —
one real gap between a claim and what its own test plan proved (§3/§4's revision), plus three residuals
not originally named (§5/§6), now folded in above rather than left as a separate report.

## 9. Open items before this can go to judging

1. A real positive/negative-control test implementing D1-D5 (including the revised D2, two-scenario
   version) against the actual codebase — not yet written, this draft is still text-only.
2. The `ChatClient` header documentation update named in §6 (near `scan_response_format_leaks_` on both
   `OpenAIChatClient` and `AnthropicChatClient`) — not yet written.
3. Project-owner judgment on whether to proceed to implementation, matching this project's own
   design → red-team → prove → judge → ADR sequence (024 §4.2) — not yet sought.
