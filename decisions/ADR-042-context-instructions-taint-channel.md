# ADR-042 — Wiring `ContextContribution.instructions` through a real, taint-checked `role::system` channel

**Status:** Judged (2026-08-14, project owner sign-off). Designed, self-red-teamed, implemented, and
proven (real code + two new test files, full suite 176/176 green).

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gaps #16 and #21 (both
closed by this ADR, in the shape the audit itself said they had to be closed together: "these are
foundational enough that fixing one without the other leaves an inconsistent taint story").
`003-Message-and-Content-Model.md` §2 and `007-Capability-and-Trust-Model.md` §4 (the taint mechanism
this ADR gives its first real content-model field, `Tainted<T>`/`TaintedText`, already Judged as a
type per 007 §9 G2's compile-fail proof — never previously instantiated as an actual field anywhere).

## 1. The question

**Stated so it has a wrong answer:** does `ContextContribution.instructions` — the field every
`ContextProvider` conformer has to contribute instruction-level text to the model — actually reach
the model, and if it does, is the channel it rides taint-checked the way I3 ("model output is never
authority," extended here to "untrusted-sourced text is never silently promoted to instruction-level
authority") requires?

**Before this fix: no, on both counts.** The field was read once (by `assemble_context()`, correctly)
and then never referenced again anywhere in the engine — a real, current, non-stale bug, re-verified
against post-ADR-037 code, not carried over from the 4-day-old audit snapshot. Separately, the ONE
mechanism this project already built specifically to make exactly this kind of channel safe
(`Tainted<T>`, `include/agentengine/core/tainted.hpp`, Judged as a type at 007 §9 G2) was never used
as an actual field type anywhere in the real content model — `ContextContribution.instructions` was a
bare `std::string`.

## 2. What re-grounding against current code found (corrections to the audit, not just confirmations)

- **The drop site is one level later than the audit's framing implied.** `assemble_context()`
  (`include/agentengine/core/context_assembly.hpp`) does correctly read and merge every contributor's
  `.instructions`. The actual drop happens in `rt::AgentSession::run_rounds()`
  (`include/agentengine/rt/agent_session.hpp`), which builds `ChatRequest{contribution->messages,
  contribution->tools}` without ever consulting `contribution->instructions` — and `ChatRequest` has
  no `instructions`/`system` field to put it in even if it tried. Fixing `assemble_context()` alone,
  as a naive reading of the audit row might suggest, would have left the actual bug untouched.
- **The gap is broader than "dynamic `ContextProvider` instructions get dropped while static
  host-authored instructions work fine."** Grepping every real consumer of `AgentMetadata::
  agent_instructions` (the static, host-authored instructions an `Agent<Derived,...>` declares) turns
  up zero production code that ever reads it to build a message — that path is a separately-scoped,
  already-named gap (the audit's own deferred note: "needs an `AgentSession`-from-`AgentMetadata`
  construction point that doesn't exist yet"), explicitly NOT touched by this ADR. Before this fix,
  no instructions of *either* kind reached the model via any general, reusable code path — only one
  hand-rolled example (`tools/cli_chat.cpp`'s own `ContextProvider`, which builds a `role::system`
  `Message` directly, bypassing `.instructions` entirely) worked, and only because it never used the
  field this ADR fixes.
- **The "taint-blind role::system channel" finding is confirmed on both real backends, precisely.**
  OpenAI's `translate_message()` and Anthropic's `split_system_messages()`
  (`include/agentengine/protocol/{openai,anthropic}/chat_client.hpp`) both concatenate every
  `role::system` message's text into the outbound wire payload with no origin/taint check anywhere.
  This ADR does not add a check at either backend (§3 explains why that would be redundant, not why
  it was skipped for convenience).
- **`Tainted<T>`/`TaintedText` is sound but was completely unused as a field type.** Its compile-fail
  proof (007 §9 G2, `tests/compile_fail/tainted_*.cpp`) exercises the wrapper in total isolation — a
  local variable in `main()`, never a real struct field. `ContentItem::text`/`Reasoning::text`/etc.
  all stayed raw `std::string`, gated only by a same-struct `bool tainted` flag that is written
  meaningfully in several places but read as an actual gate in exactly one (a leak-scanner's dedup
  logic, not a security decision). This is the real, broader gap #21 describes — and this ADR does
  **not** close that broader claim (§5).

## 3. The design

**`ContextContribution::instructions` changes type**: `std::optional<std::string>` →
`std::optional<TaintedText>` (`include/agentengine/core/context_provider.hpp`). This is the minimal,
honest slice of gap #21 that gap #16 actually needs — not an attempt at the much larger "migrate
every text/structured content-model field to `Tainted<T>`" rewrite gap #21's broader claim implies
(that is real, RFC-003-§2-level work needing its own design→red-team→prove→judge cycle, out of scope
here and named as still-open in §5).

Consequences of the type change, all deliberate:

1. **A `ContextProvider` that wants to contribute instructions must construct `TaintedText{...}`
   explicitly.** `Tainted<T>`'s own design (explicit-only constructor, no implicit conversions) makes
   this a real, compile-time-enforced trust decision at the one point that actually matters:
   construction. A provider whose text is itself derived from already-tainted material (e.g. a memory
   item sourced from an external tool result) must call *that* source's own `.unsafe_view()` first —
   a visible, individually-reviewable declassification inside the provider's own code, exactly the
   shape `tainted.hpp`'s header comment describes and RFC 003 §2 specifies.
2. **`assemble_context()`'s combine step** (`context_assembly.hpp`) now declassifies each
   already-vetted `TaintedText` via `.unsafe_view()`, concatenates, and rewraps as a new `TaintedText`.
   This is not a new trust decision — `tainted.hpp`'s own carve-out ("must never be reached from a
   capability-granting or policy-deciding code path directly") does not apply: merging two
   independently-already-trusted strings decides nothing new.
3. **`rt::AgentSession::run_rounds()`** — the actual drop site — now checks
   `contribution->instructions` before building `ChatRequest`; if present, calls `.unsafe_view()`
   (the **one** explicit declassification site for the whole engine on this path), builds a
   `Message{role::system, [ContentItem{origin=content_origin::system, tainted=false, Text{...}}]}`,
   and prepends it to `contribution->messages`. Prepended, not appended, matching
   `HistoryAndSkillsProvider`'s own established system-message-first convention
   (`tools/cli_chat.cpp`) — and coexists fine with any OTHER role::system message a provider already
   places directly in `.messages` (both real backends already concatenate every `role::system`
   message they see, not just the first, confirmed in §2's re-grounding).
4. **No new taint-checking code in either backend's translation layer.** By the time a synthesized
   `role::system` `Message` reaches `OpenAIChatClient`/`AnthropicChatClient`, it is ordinary, already-
   vetted content — the safety comes from gating at the source (the `TaintedText` field), not from a
   redundant check at the sink. Adding one anyway would be scope creep with no marginal safety benefit
   for THIS specific channel, though it remains a real gap for content that never goes through
   `.instructions` at all (§5).

## 4. Self-red-team findings

**Not a fix for a careless or bad-faith wrap — named, not overclaimed.** `Tainted<T>`'s own doc
comment is explicit that its accessor "does no checking by design" and an explicit declassifier is
"the caller's responsibility." A `ContextProvider` author who carelessly wraps genuinely untrusted
text in `TaintedText{...}` is not caught by this fix — nothing in this codebase's use of `Tainted<T>`
anywhere else claims to catch that either. What this fix closes is the *accidental* case: before it,
there was no decision point at all — a plain `std::string` flowed from "provider computed some text"
to "text with elevated authority reaches the model" with zero compile-time or run-time gate. After it,
a provider author must make an explicit, greppable choice.

**No budget enforcement for the new channel — named, not silently accepted.** `assemble_context()`'s
existing per-contributor token-budget trimming (`ContextBudget::max_tokens`) only ever operated on
`contribution->messages`; `.instructions` was never in scope for it before (it did nothing), and this
fix doesn't add any either. A provider contributing unbounded instructions text now has a real,
functioning path to inflate every model call's token cost with no cap. Deciding whether instructions
should share the existing message budget or get a new field of their own is a genuine, separate design
question this ADR does not answer — flagged here so it isn't mistaken for already-solved.

**Ordering/coexistence with `HistoryAndSkillsProvider`'s own hand-rolled pattern was checked, not
assumed.** Both mechanisms can now produce a `role::system` message in the same request. Confirmed
(§2) that Anthropic's `split_system_messages()` and OpenAI's uniform-role translation both already
handle multiple `role::system` entries by concatenation, not by taking only the first — so this is a
real, checked compatibility, not an unverified assumption.

**"First-match" or field-ordering ambiguity does not apply here** — unlike `find_fs_write`/
`find_fs_read` (ADR-040), there is exactly one `.instructions` field per `ContextContribution` and
one merge step; no analogous "which of several candidates wins" question exists for this fix.

## 5. What this ADR does not claim

- **Does not close gap #21's broader scope.** `ContentItem::text`/`Reasoning::text`/`Data::json`/etc.
  remain raw, ungated types. The three consumption boundaries the audit named
  (`tools/cli_chat.cpp`, `include/agentengine/protocol/mcp/server.hpp`,
  `src/backends/native_jail/tool_bridge.hpp`) are unchanged by this ADR and still read raw fields
  directly — a genuine residual, real RFC-003-§2-level work for a future, separately-scoped pass.
- **Does not wire `AgentMetadata::agent_instructions`** into any `ContextContribution` — that needs
  an `AgentSession`-from-`AgentMetadata` construction point that doesn't exist yet, already named as
  out of scope by the original audit and left that way here.
- **Does not add budget enforcement for the new instructions channel** (§4).
- **Does not add taint checks inside either backend's translation code** — the design's own argument
  for why that would be redundant for THIS channel is in §3 point 4, not a claim that the backends are
  taint-aware generally (they are not, for any `Message` a provider constructs directly rather than
  through `.instructions`).

## 6. Evidence

- `tests/test_context_assembly.cpp` (B3-I1/I2/I3): two `TaintedText`-contributing providers combine
  in declared order via `.unsafe_view()` concatenation; a provider that never sets `.instructions`
  leaves the combined field unset (no stray empty `TaintedText`).
- `tests/test_rt_agent_session_instructions.cpp` (T1-T3, new file): instructions reach the model as a
  real, correctly-shaped, correctly-positioned `role::system` `Message` (`origin=system`,
  `tainted=false`, verbatim text, prepended ahead of the user's own message); the no-instructions path
  is byte-identical to before this fix (nothing regressed for the dominant, current-production case);
  instructions are recomputed every round, not cached from round 1.
- `tests/test_memory_retrieval_determinism.cpp`: a pre-existing equality helper
  (`contribution_identical()`) broke against the type change (`Tainted<T>` deliberately provides no
  `operator==`) and was fixed to declassify explicitly on both sides before comparing — the one real
  ripple effect of the type change anywhere in the tree, found by the build, not missed.
- Full suite: 176/176 tests pass (`ctest`, this pass, up from 175 — the new test file), zero
  regressions.
