# ADR-023 — A response-format codec seam for raw-text tool-call/reasoning extraction

**Status:** Judged — 2026-08-10. Both halves named in §6 are now real, tested, landed code: Phase 1
(§6 point 3, the Reasoning/Text half — see §8) and Phase 2 (§6 point 4, the text-derived
declassifier, including the 007 §4 amendment it required — see §9). Every falsifiable claim in §5 now
has BOTH a spike/red-team-level verdict and a real-code regression test behind it, including the
confused-deputy scenario (§4b Finding 1) as a permanent test (`tests/test_tool_pipeline.cpp`'s
`ADR-023 P2-T2`). "Judged" here does not mean §7's residuals are closed — format-coverage gaps
(Llama 3.1 native syntax, the fail-closed delimiter-collision cost) and the inherited WASM-codec perf
question (G6) remain real, named, and open, matching this project's own precedent (e.g. ADR-006,
ADR-021) for judging the ADR's own contested question while carrying forward named follow-on work.
**Relates to:** `docs/research/2026-08-10-provider-and-harmony-adapter-landscape.md` (the survey that
motivated this), 007 §4 (`Tainted<T>`/declassifier rules, load-bearing here), 006 §4/§5 (the 10-step
tool invocation pipeline this design must feed without bypassing), 009 §2 (`ae:codec` world),
009 §11 Q1/G4a (the still-open WASM streaming-latency question this ADR inherits rather than
resolves).

## 1. The question

`OpenAIChatClient`/`AnthropicChatClient` today parse tool calls **only** from vendor-structured JSON
fields; free-text assistant `content` is captured verbatim into a `Text` content item with zero
scanning. The research survey established that every serving layer for gpt-oss (Harmony format) —
vLLM, llama.cpp, Ollama, SGLang — has a documented, sometimes-still-open leak path where raw format
tokens reach `content` instead of being normalized into `tool_calls[]`. DeepSeek's and Qwen's own
`<think>...</think>` reasoning tags carry the identical leak risk, with a simpler grammar.

**Stated so it has a wrong answer:** How does AgentEngine extract reasoning/tool-call content out of
a raw, non-JSON-field model response format — Harmony, DeepSeek/Hermes/Qwen delimiters, `<think>`
tags, and formats not yet designed — extensibly enough that a new format is added without a core
rebuild, while never letting a text-derived reconstruction acquire the same unconditional authority
as a vendor-structured tool call?

## 2. The non-negotiable constraint (not itself contested)

007 §4, already locked: *"Model-originated and externally-retrieved content is `Tainted<T>`,
transitively. No capability-granting or policy-deciding API accepts a tainted value... Concretely
forbidden: ... deriving an approval decision from model-supplied text."* A tool call reconstructed by
pattern-matching raw assistant text is exactly this — a policy-relevant conclusion derived from
model-supplied text. Every design below produces `Tainted<ToolCallCandidate>`, a type structurally
distinct from today's `ToolCall`/`ToolCallRequest` (unchanged by this ADR — the vendor-structured-JSON
path stays exactly as it is). It becomes invokable only through an explicit, logged declassifier.

## 3. Competing designs (mechanism axis) — steelmanned, then attacked

**Design A — native, hardcoded per-format C++ parsers, sniff-and-dispatch table.** Fast (zero
indirection); extensibility claim **false by the question's own bar** — a new format needs a core
rebuild, named honestly rather than argued around.

**Design B — declarative grammar table, data-driven dispatch.** A generic interpreter walks a table
of `{block_open, body_open, terminators[], recipient field-spec, arguments field-spec}` entries; a
new format is a new table row, no rebuild. Extensibility claim: contested, spiked below.

**Design C — `ae:codec` WASM plugin per format** (009 §2's already-locked world for exactly this
class of job — content transformation, distinct from `ae:provider`, which is the whole-HTTP-backend
seam and the wrong world for a content-transform sub-step inside an existing `ChatClient`). New
format = new signed plugin, no rebuild, plus genuine defense-in-depth: a parser bug is contained to a
zero-ambient-authority sandbox instead of running in-process.

**Design D — detect-and-quarantine.** Same mechanism as B/C for the Reasoning/Text split only
(safe by construction — inert display content); the tool-call/commentary channel is **never**
promoted past an observable diagnostic. Safe claim true by construction; correctness cost named
honestly — real tool calls are silently lost whenever a serving layer misconfigures, same shape as
today's status quo, now understood rather than accidental.

## 4. Executed evidence

### 4a. Spike — does one declarative table shape (Design B) actually work?

A real, independently-recompiled-and-rerun C++23 program (`static constexpr Format TABLE[]`, POD,
no regex, no lambdas, no per-format branch in the interpreter) plus a Python twin, driven against the
real Harmony/DeepSeek/Hermes example strings from the research survey. **Verified command, rerun by
the architect independently of the spike agent's own report** (not taken on faith):

```
clang++ -std=c++23 -Wall -o spike_verify.exe spike.cpp && ./spike_verify.exe
--- Harmony tool call            fmt=harmony  blocks=1 dispatching=1  recipient=get_current_weather  PASS
--- DeepSeek (U+FF5C/U+2581)     fmt=deepseek blocks=1 dispatching=1  recipient=get_current_weather  PASS
--- Hermes/Qwen                  fmt=hermes   blocks=1 dispatching=1  recipient=get_weather           PASS
--- NEG Harmony analysis (must not dispatch)                          dispatching=0                   PASS
--- NEG Harmony final (must not dispatch)                             dispatching=0                   PASS
--- ADV literal <|call|> inside args (must fail closed)                dispatching=0                   PASS
OVERALL: ALL CASES PASS   sizeof(Format)=528   TABLE is static constexpr POD, 3 rows
```

**Findings, including where the claim had to give ground (this is the actual result, not the
success banner alone):**

1. **The v1 field shape was falsified and had to change.** A single-literal `stop_at` broke on a real
   Harmony variant with no space before `<|constrain|>` — it produced a garbage recipient
   (`get_weather<|constrain|>json`) **and still dispatched**. Fixed by making `stop_at` a *list* of
   stop literals, earliest wins — still pure data, but the shape is already one revision old.
2. **Delimiter-vs-content escaping is not expressible in any fixed-field table.** A literal
   `<|call|>`/`</tool_call|>`/`` ``` `` inside an argument string truncates the block early — no
   fixed-field table can say "this token counts as a delimiter only outside a string, never inside
   one" without a real grammar (escaping/nesting — exactly why llama.cpp's own parser is a PEG, not a
   table). The spike's interpreter fails closed (requires the extracted span to parse as valid JSON
   before dispatching) rather than mis-terminating — safe, but a real, named correctness cost: a
   legitimate call is silently dropped whenever its own arguments happen to contain a delimiter
   substring.
3. **A hard expressiveness limit found:** Llama 3.1's *built-in*-tool form
   (`<|python_tag|>brave_search.call(query="x")`) is Python-expression syntax, not a JSON payload —
   not expressible in this shape at any field setting. Only its JSON-tool variant fits.
4. Two formats (Qwen2.5-Coder's `<tools>` wrapper, Llama's JSON-tool variant) were added to the
   Python twin **with zero interpreter code changes** — real evidence the shape generalizes within
   its proven class.

**Verdict: Design B is CORRECT, narrowed.** It works for delimiter-shaped, JSON-payload formats —
Harmony, DeepSeek, Hermes/Qwen, Qwen2.5-Coder — which covers every format the research survey found
in real current use except Llama's raw-Python-expression tool syntax. It is not a general answer;
Design A or C remains required as the fallback for whatever doesn't fit that class, now or in the
future.

### 4b. Red-team — does the safety framing in §2 actually hold?

An independent adversarial pass (not the architect grading their own design) attacked the central
claim directly.

**Finding 1 — the confused-deputy attack succeeds against the draft as originally framed, and the
proposed mitigation does not save it.** A page fetched by a `fetch`/`WebSearch` tool contains, inside
what looks like ordinary content, valid Hermes/Harmony tool-call syntax addressed at a tool the agent
genuinely holds, with attacker-chosen arguments. When the model summarizes/quotes that page (an
ordinary, expected behavior), the payload is **laundered through the model's own output** — it never
arrives as a tool result being scanned, it arrives as a fresh assistant turn at a genuine stream
boundary, indistinguishable in shape from a legitimate leaked call. The draft's proposed bound ("only
scan content at a real vendor stream boundary, never inside a tool result") does not close this,
because that is exactly where the laundered payload appears.

**Finding 2 — declassifier (a) (schema-validation-only) is unsafe, and worse than merely
insufficient.** 006/007's existing policy match vocabulary already tags *every* model-originated
argument `taint: high` — so a schema-declassified `Tainted<ToolCallCandidate>` would arrive at 006
step 5 **structurally indistinguishable from an ordinary vendor-structured call**. There is no field
an operator's policy rule could match against to treat it differently. (a) is not "feeds the existing
gate like a normal call" — it is *invisible* to it. Rejected as unconditionally unsafe.

**Finding 3 — declassifier (b) (mandatory approval, always) is mechanically sound but has a real
cost.** Approval-habituation risk scales with leak frequency — exactly the deployments where this
feature pays off are the ones that would train users to click through. The approval UI also renders
attacker-chosen tool name/arguments verbatim, giving the injection a rendering channel into the
human-review step itself.

**Finding 4 — a narrower declassifier (a′), proposed and adjudicated by red-team, survives.**
Auto-declassify only when **both** hold: `arguments_json` validates against the tool's own declared
schema, **and** the target tool's declared capability set contains no egress/mutation capability
(`NetOut`, `FsWrite`, `Secret`, `AgentCall`, `Exec`) with `EffectClass == pure`. This is a static,
per-tool-registration check (007 §4's own "strict allowlist match" declassifier, applied to the
*tool*, not the untrusted arguments) — deterministic, auditable, and closes the exfiltration path
Finding 1 relies on: a read-only, no-egress tool has nothing for the attack to exfiltrate through or
mutate. Everything else — any tool with real capability — falls to mandatory approval,
unconditionally, no exceptions.

**Finding 5 — this requires a real spec change, not a workaround inside this feature.** (a′) needs
call **provenance** (`origin: vendor_structured | text_derived`) threaded as a first-class field into
007 §5's policy match vocabulary and 006 step 5's approval gate. This does not exist today. Building
(a′) without it means building an invisible, unauditable side channel — the same failure Finding 2
found in (a).

**Finding 6 — scanning must be operator-armed, never content-triggered.** Authority to even attempt
raw-text scanning must come from trusted per-endpoint configuration (007 §1's trusted-operator tier),
never switched on by the presence of suspicious tokens in a response. This converts the feature from
"a new attack class exposed to everyone by default" into "an explicitly accepted, scoped risk on one
named, known-imperfect serving-layer configuration" — matching I2's shape (no ambient authority; the
capability to scan is itself granted, not inferred).

**Finding 7 (Design C) — the "sniff cheap, decode rare" perf claim does not survive contact with
benign use.** A developer asking their own agent *about* Harmony or `<think>` tags produces content
that trips every registered codec's `sniff()` with zero attacker involved — false-positive sniffing,
not an attack. Worse, the streaming/UI-rendering use case (the part of this ADR's motivation this
project actually wants) forces re-sniffing a growing prefix per delta, which is O(N²) boundary
traffic if done naively. **Verdict: real, unresolved — this ADR restricts to whole-message (buffered,
non-streaming) decoding for v1**, naming streaming decode as explicit future work needing a stateful
primitive this ADR does not design.

**Finding 8 (Design A) — right conclusion, wrong citation.** The draft cited 009 §1's
`dlopen`/`LoadLibrary` reasoning against a hot-reload escape hatch for Design A; red-team found that
passage governs third-party plugin distribution, not first-party in-tree code, so it was cited out of
context. The real reason A can't cheaply gain "no rebuild" is that a native codec parsing hostile
text would run at host trust tier (007 §6) — the wrong tier for this specific job — and 007 §7's
revocation requirement ("in-flight uses are canceled") has no answer for a loaded shared library.
Conclusion unchanged; reasoning corrected.

## 5. Per-claim verdicts

| # | Claim | Verdict | Basis |
|---|---|---|---|
| G1 | Design B expresses Harmony/DeepSeek/Hermes/Qwen as pure declarative data, no per-format code | **CORRECT, narrowed** | §4a: 3/3 real formats pass, re-verified independently; 2 more added with zero interpreter changes |
| G2 | Design B expresses every possible future format | **WRONG** | §4a: delimiter-escaping and non-JSON-payload (Llama raw-Python) formats are structurally inexpressible |
| G3 | "Only scan at stream boundaries" prevents raw-text injection | **WRONG** | §4b Finding 1: laundering through the model's own quoting defeats it |
| G4 | Schema-validation-only auto-declassification (a) is safe | **WRONG** | §4b Finding 2: structurally invisible to existing policy, not merely insufficient |
| G5 | Mandatory-approval-only (b) is the only safe declassifier | **WRONG (too strong)** | §4b Finding 4: a capability-scoped declassifier (a′) closes the concrete exfiltration path while keeping benign, no-egress calls working without a human |
| G6 | `ae:codec`'s "sniff cheap, decode rare" holds under both adversarial and ordinary use | **WRONG for streaming; INCONCLUSIVE for buffered** | §4b Finding 7: false-positive sniff tax and O(N²) streaming re-scan are real; buffered per-message cost not measured here |
| G7 | Design A cannot be made extensible without violating an already-locked decision | **CORRECT, different reason than drafted** | §4b Finding 8 |

## 6. Decision

1. **Mechanism**: Design B (the spiked, revised table shape — list-valued `stop_at`, fail-closed on
   unparseable JSON spans) for delimiter-shaped, JSON-payload formats. Design C (`ae:codec` plugin)
   remains the stated extensibility path for anything B cannot express (non-JSON payloads, formats
   requiring real escaping/nesting) — matching 009's existing precedent of native/table-driven
   coverage for common cases and a plugin seam for exotic ones. Design A is not used for new formats.
2. **Scope for v1: whole-message, non-streaming decoding only.** Streaming decode is named future
   work, blocked on a stateful `decode-chunk` primitive and a per-message step/byte budget this ADR
   does not design (§4b Finding 7).
3. **The Reasoning/Text half ships independently of the tool-call half.** Extracting `analysis`/
   `<think>`/`final` content for display is safe by construction (never produces anything
   policy-relevant) and has no dependency on the declassifier work below — it can be implemented and
   land on its own, feeding the existing `ModelDelta` reasoning/text kinds (013), which is the actual
   "great for rendering UI" outcome this investigation started from.
4. **The tool-call half is gated behind spec work not yet done.** Before any `Tainted<ToolCallCandidate>`
   is declassified into an invokable `ToolCallRequest`:
   - 007 §5's policy match vocabulary needs a real `origin: vendor_structured | text_derived` field
     (Finding 5) — a 007 amendment, not an implementation detail of this feature.
   - The declassifier is **(a′)**: auto-declassify only when schema-valid **and** the target tool's
     declared capability set has zero egress/mutation capabilities and `EffectClass == pure`;
     otherwise mandatory operator approval, unconditionally (Finding 4).
   - Raw-text scanning is **off by default**, armed only by trusted per-endpoint operator
     configuration — never triggered by content (Finding 6).

## 7. Residual risks and open gates (not closed by this ADR)

- No production code exists yet. This ADR fixes the shape and the safety framing; implementation,
  the 007 §5 amendment, and real tests (positive control: a genuine leaked call from a real local
  model correctly recovered; negative control: the Finding-1 injection scenario correctly refused or
  correctly forced to approval) are separate follow-on work.
- G6 (WASM codec perf) inherits 009 §11 G4a's already-open measurement gate and is not resolved here.
- The fail-closed behavior on delimiter/content collision (§4a finding 2) means some legitimate tool
  calls will still be silently lost — a named, bounded version of today's accidental silent-loss
  behavior, not a full fix. Worth deciding later whether that residual is acceptable permanently or
  needs a real escaping-aware grammar (Design C, format-by-format) for high-value formats.
- Llama 3.1's native (non-JSON) tool syntax has no covered path in this ADR at all.

## 8. Addendum (2026-08-10) — Phase 1 implemented

§6 point 3 (the Reasoning/Text half) is now real, tested code:

- `include/agentengine/core/response_format_codec.hpp` — the table+interpreter from §4a's spike,
  ported with the revised (list-valued `stop_at`) shape and extended beyond the spike in two ways the
  spike itself didn't need: (a) a `channel_field`/`fixed_channel` mechanism so a matched block routes
  to `Reasoning` (analysis), clean `Text` (final), or an inert tainted `Text` diagnostic (commentary)
  rather than the spike's flat `Block` list; (b) leftover-text handling, so content between/around/
  after recognized blocks (e.g. DeepSeek-R1's trailing answer text after `</think>`) is preserved as
  ordinary `Text` in original order, not silently dropped the way the spike's own `decode()` would
  have (a real gap found while porting: the spike's block-scan loop `break`s on a dangling/unterminated
  block without ever appending the unconsumed remainder — fixed here by making that case route to the
  whole-content `partial = true` fallback instead). Built-in rows: `harmony`, `deepseek_tool_call`,
  `hermes_qwen_tool_call`, `think_tag`.
- `OpenAIChatClient` gained `scan_response_format_leaks` (bool, default `false`, appended last after
  `transport`) and `detail::apply_response_format_scan(Message)`, called from `chat()` only when
  armed — Finding 6's "operator-armed, never content-triggered" honored exactly. `chat_stream()` is
  untouched (Finding 7, streaming stays out of scope).
- Tests: `tests/test_response_format_codec.cpp` (30 checks: all 4 real formats including a full
  multi-channel Harmony turn proving ORDER is preserved, 2 negative controls, 2 adversarial controls —
  delimiter-in-args and a dangling/truncated block, both proven fail-closed) and 3 new checks appended
  to `tests/test_openai_chat_client_translation.cpp` (the armed splice, a clean-content no-op, and
  proof that structured `ToolCall` items are never touched). 150/150 project tests pass, zero
  regressions, including both live-network backend tests.
- What's still NOT built: everything in §6 point 4 (the tool-call declassification half) and
  everything in §7 above — this addendum only closes the safe, ungated half. (Superseded by §9 below —
  point 4 is now built too.)

## 9. Addendum (2026-08-10) — Phase 2 implemented

§6 point 4 (the text-derived declassifier) is now real, tested code, closing the last item this ADR's
own §6 left open.

- **007 §4 amendment** (`007-Capability-and-Trust-Model.md`, dated 2026-08-10): documents
  `call_provenance` as an extension of the existing `Tainted<T>`/declassifier framework and states the
  admitted declassifier — a strict allowlist match against the tool's own declared shape (capability
  ceiling + effect class), never the untrusted arguments — plus operator approval as the only other
  admitted path. Cites the ADR directly rather than re-deriving the reasoning in two places.
- `core/content.hpp` gained `enum class call_provenance {vendor_structured, text_derived}` (shared by
  both `ToolCall`, appended last, and `ToolCallRequest`, `core/tool_pipeline.hpp`, also appended last —
  defined here rather than in `tool_pipeline.hpp` specifically to avoid a dependency cycle, since
  `content.hpp` has no include of the pipeline header and shouldn't gain one).
- `trust/capability.hpp` gained `is_inert_for_text_derived_declassification(capability_kind)` — an
  **allowlist** (not the ADR's own illustrative 5-kind denylist) covering all 15 real capability
  kinds explicitly, each with a one-line rationale: only `fs_read`/`clock`/`entropy` qualify as
  provably inert; the other 12 (including `tool_call`/`runner_call`/`agent_call`/`schedule`/
  `background`/`elicit` — recursive-authority and deferred-effect kinds the ADR's own illustrative
  list didn't name) are classified dangerous. Deliberately stricter than the ADR's literal wording,
  in the direction 007's own fail-closed posture already argues for (a denylist silently misclassifies
  any future capability kind as safe by omission; this allowlist does not).
- `ToolDescriptor` gained `effect_class` (`core/tool_pipeline.hpp`, appended last, defaulting to the
  conservative `at_most_once` — the same default `declared_effect_class()` itself already used, so a
  hand-built descriptor that predates this field fails closed); `make_tool_descriptor<T>()` populates
  it from `ToolT::declared_effect_class()`, which existed at compile time (019 §3) but was never
  copied onto the runtime descriptor before now.
- `invoke_tool` step 5 rewritten: a `vendor_structured` call takes the byte-for-byte original branch
  (`tool->approval != never_require`); a `text_derived` call NEVER consults `tool->approval` at all,
  using `is_auto_declassifiable_text_derived_call` (empty-or-inert-only capability ceiling AND
  `effect_class::pure`) instead — overriding even an explicit `Approval<never_require>` on a
  capability-bearing tool, the exact override the confused-deputy scenario (§4b Finding 1) forced.
- `response_format_codec.hpp`'s `decode_result` gained `candidates` (`DetectedToolCallCandidate{
  recipient, arguments_json, diagnostic_item_index}`) — plain data, populated ADDITIONALLY alongside
  the Phase-1 inert diagnostic `Text` item, never instead of it. Still zero dependency on
  `tool_pipeline.hpp`.
- `OpenAIChatClient::apply_response_format_scan` now takes `ChatRequest::tools` (already available at
  the call site — reused, not duplicated) and promotes a candidate to a real `ToolCall` content item
  tagged `provenance = text_derived` ONLY when its recipient matches a known tool name; an unrecognized
  name keeps the Phase-1 diagnostic (hygiene, not safety — `invoke_tool` step 1 would reject an
  unknown name regardless). The actual trust decision stays entirely in `invoke_tool` step 5, never in
  the `ChatClient`.
- **Tests, including the load-bearing one**: `tests/test_tool_pipeline.cpp` gained the full
  declassifier suite — a positive control (a capability-free, pure tool auto-declassifies, with a
  tripwire decider proving `approve()` is never even consulted) and, critically, `ADR-023 P2-T2`: a
  `text_derived` call to a real, `NetOut`-capable tool that ALSO declares `Approval<never_require>`
  for its own vendor-structured calls is refused without an approving decider — the actual injection
  scenario the red-team pass found, now a permanent regression test. (This test caught a real bug in
  its own first draft: the granted `CapabilitySet` didn't actually cover the tool's declared host,
  so all three sub-cases failed at step 4 instead of exercising step 5 — fixed by granting a `NetOut`
  matching exactly what the tool's declaration converts to, confirmed by rerunning and seeing the
  right checks pass for the right reason.) `vendor_structured` behavior is proven byte-for-byte
  unaffected for both a `never_require` and a capability-bearing tool. `tests/
  test_response_format_codec.cpp` and `tests/test_openai_chat_client_translation.cpp` extended with
  candidate-population and promotion/non-promotion checks.
- Full project build + `ctest`: **150/150 tests pass**, zero regressions, including both live-network
  backend tests.
- What's still not built: format coverage beyond the four built-in table rows, the WASM `ae:codec`
  plugin path (Design C, never implemented — Design B's native table was), and streaming decode — all
  named in §7, all unaffected by this addendum, all real follow-on work if picked up later.
