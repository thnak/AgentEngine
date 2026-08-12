# ADR-035 — Every `ChatClient` conformer becomes genuinely streaming-capable, then `chat()` stops being required

**Status:** Proposed (2026-08-12). Designed, red-teamed (independent passes via the `Agent` tool,
general-purpose reviewer, three separate rounds — §2, §3, §5), implemented in four sequential,
individually built/tested/committed phases, and proven (real code + deterministic offline tests +
one live-network run, §6); awaiting the project owner's explicit "Judged" sign-off per this
project's governance (`decisions/README.md`; `OpenQuestions.md` OQ-11's resolution that the project
owner is the ADR judge, never an AI).

**Relates to:** `decisions/ADR-034-agentsession-streaming-turn-loop.md` (Proposed — the opt-in
streaming turn loop this ADR completes: ADR-034 §7 explicitly left "the leak-scan gap... named, not
closed" and "`AnthropicChatClient`'s streaming path is not wired for usage capture" as residuals;
both are closed here). `decisions/ADR-023-response-format-codec-seam.md` (Judged — the confused-
deputy scan this ADR relocates and generalizes). `decisions/ADR-036-model-call-gateway.md`
(Proposed — the coroutine-based retry/failover/middleware mechanism that superseded this ADR's
original Phase 2 plan; see §4). `004-Model-Provider-Plane.md` §1 (the literal `ChatClient` concept
this ADR's final phase amends) and §5 (`TokenBudget<N>`, the guarantee the usage-capture and
fail-closed rules protect).

## 1. The question

The project owner directed, explicitly, twice in succession: streaming becomes the default concept
for model calls everywhere, not an OpenAI-only or `chat()`-fallback-shaped afterthought — and,
eventually, `chat()` itself should stop being something every `ChatClient` conformer is required to
have. ADR-034 made streaming a real, opt-in `AgentSession` path, but named three things it did not
attempt: Anthropic's streaming path reports no usage at all (fails closed, unusable); the ADR-023
confused-deputy leak-scan runs only inside `OpenAIChatClient::chat()`, nowhere else; and the vast
majority of `ChatClient` conformers in this codebase — every test fixture, every example, two real
production call sites — still only genuinely worked through `chat()`, with `chat_stream()` either
absent from the concept's requirement or satisfied by a dead, type-checking-only stub. Can every one
of those gaps be closed for real, in a sequence where each step is independently proven before the
next starts, ending with `chat()` no longer required by the concept at all — without silently
weakening any existing protection, without losing test coverage, and without discovering that "just
enable streaming everywhere" was unsafe only after it shipped?

## 2. Phase 1a — Anthropic streaming usage (closing ADR-034's first named residual)

`AnthropicUsageSnapshot` and its two pure reduce functions, `seed_usage_from_message_start`/
`accumulate_message_delta_usage` (`include/agentengine/protocol/anthropic/chat_client.hpp`),
already existed and were already unit-tested (`tests/test_anthropic_chat_client_translation.cpp`'s
E2-R1) — but neither function was ever actually called from `StreamingUpdateAccumulator`, so no real
streaming call against Anthropic ever populated `ChatResponseUpdate::usage`. Under ADR-034's own
fail-closed rule, this meant every Anthropic-backed streaming call would hit `run.usage_unavailable`
and never resolve — a real, load-bearing gap once `chat()` is no longer available as a fallback.

**Fixed:** `items_from_block()` now handles the previously-unhandled `message_start`/`message_delta`
SSE event types, calling the two existing reduce functions; a new `captured_usage()` attaches the
result to the terminal `ChatResponseUpdate` in `finish()`, including a synthesized-empty-final-update
branch (mirroring the OpenAI backend's own existing handling) for a genuinely content-free completion
that still carries real usage.

**Proven:** the pre-existing `E-STREAM-R1` test (a full named-event SSE stream, already exercising
`message_start`/`message_delta` in its fixture data but never asserting on `.usage`) extended with
five new checks proving the final update carries the correct, correctly-overwritten (not summed)
usage; a new `E-STREAM-R2` proves the synthesized-empty-update path directly. Verified against the
real wire format via `test_anthropic_chat_client_live.cpp`'s existing loopback-TLS harness (not a
mock — a real TLS handshake against a self-signed local server speaking real Anthropic SSE), 206/206
suite pass.

## 3. Phase 1b — the leak-scan becomes backend-agnostic

`apply_response_format_scan` (ADR-023's confused-deputy detector) ran only from
`OpenAIChatClient::chat()`, gated by that class's own `scan_response_format_leaks` constructor flag.
`AnthropicChatClient` had no equivalent mechanism at all, in either path — not a narrower version of
the protection, a total absence of it.

**First design red-teamed, one must-fix found and closed before implementation:** relocating the
scan into `AgentSession::run_model_call()` (so it applies once, uniformly, regardless of backend or
streaming) creates a real double-scan hazard if a caller arms both `OpenAIChatClient`'s own flag and
the new `AgentSession`-level flag together on the `chat()` path. `apply_response_format_scan`'s
commentary-channel handler builds its inert diagnostic `Text` by literally concatenating the
extracted recipient/arguments back into a new string; if those extracted `arguments` happen to
contain a *different* format's own literal markers, a **second** independent scan of that diagnostic
string can sniff the embedded markers and promote a **different, attacker-chosen** candidate the
first pass deliberately declined to promote — laundering an unrecognized recipient into a recognized
one across two passes. **Fixed:** `apply_response_format_scan` now skips any `Text` item already
marked `tainted` (exactly the predicate for "a prior pass's own diagnostic, not fresh model text"),
making a second pass a true no-op by construction, not by convention. A second, narrower hardening
closed the same red-team pass's finding #4: `AnthropicChatClient`'s SSE accumulator now ignores a
`text_delta` arriving on an index whose block was started as `tool_use`/`thinking`, so a malformed
Anthropic-wire-compatible gateway (Bedrock/Vertex/self-hosted proxies are a real deployment shape,
not hypothetical) can't inject a stray `Text` item into the scan's surface by misrouting a delta.

**Accepted design:** `apply_response_format_scan` relocated verbatim (one implementation, not two
that could drift) from `protocol/openai/chat_client.hpp` into a new shared
`core/response_format_leak_scan.hpp`; `OpenAIChatClient`'s own flag and call site are unchanged in
behavior. `AgentSession` gains its own `scan_response_format_leaks_` flag (default false, matching
ADR-023's own "operator-armed, never content-triggered" rule) and applies the scan once, in
`run_model_call()`'s shared tail, after either the streaming or non-streaming branch produces a
`ChatResponse` — reaching Anthropic for the first time. `tools/cli_chat.cpp` migrated from arming
the now-dead-under-streaming `OpenAIChatClient` flag to the new `AgentSession`-level one.

**Proven:** a new `tests/test_agent_session_response_format_leak_scan.cpp` (L1-L4): a Harmony-leaked
tool call is promoted and invoked when the flag is armed (streaming path, L1); passes through
completely unchanged when it isn't (L2, proving the flag genuinely gates behavior); the identical
promotion happens through `chat()` too, with no `OpenAIChatClient`-specific flag armed at all,
proving the mechanism is truly backend/path-agnostic (L3); and the double-scan idempotence
regression itself — a second scan of an already-scanned message is byte-for-byte unchanged (L4),
the direct proof the red-team's must-fix is actually closed, not merely no-longer-observed-to-fail.
207/207 suite pass (206 + the new file).

## 4. Phase 2 was superseded, not skipped

The original plan's Phase 2 — give `FailoverChatClient`/`ResilientChatClient`/`MiddlewareChatClient`
real `chat_stream()` parity as three wrapper templates — was attempted, independently red-teamed
twice, and abandoned in favor of a different mechanism entirely once the red-team found a genuine,
unresolved use-after-free hazard in the middleware piece (a coroutine hook driven from a non-
coroutine `chat_stream()` via a "resume once and hope it never suspends" trick can be resumed later
by whatever it awaited, touching stack-lifetime state long since gone). See
`decisions/ADR-036-model-call-gateway.md` for that design's own full record — its
`ModelCallGateway`/`MiddlewareModelCallGateway` are what actually deliver retry/failover/middleware
under streaming; the three original wrapper templates are unmodified, kept as documented, `chat()`-
only legacy types (§7).

## 5. Phase 3 — every conformer proven streaming-capable, then `chat()` relaxed

A full-codebase survey (independent `Explore`-agent pass) found 54 `ChatClient`-conforming types
across 51 files whose `chat_stream()` was a dead, immediately-`Cancelled` stub — never exercised by
any test, present only to satisfy the concept's compile-time shape. Verified, exact list: 46 files
(45 matched by an exact stub-body pattern, one — `test_chat_client_credential_resolution.cpp` — by
manual inspection after the pattern missed its multi-line formatting). Two real production call
sites (`MemoryProvider::on_turn_end`, `HistoryProvider<Summarize<N,SummarizerT>>::on_context`) called
`SummarizerT::chat()` directly and unconditionally, with no streaming alternative at all.

**Phase 3, part 1 (mechanical, behavior-preserving):** all 46 files converted, dispatched as five
parallel batches (independent `Agent` tool calls, each given the exact same prescriptive pattern —
`tests/test_agent_session_streaming_model_calls.cpp`'s already-tested `ScriptedStreamingChatClient`
as the canonical shape to mirror) rather than one large serial edit. Each fixture's `chat_stream()`
now streams the identical content its own `chat()` already built, as one final `ChatResponseUpdate`;
a handful of fixtures whose whole test purpose is simulating an always-failing backend
(`AlwaysFailingChatClient`, `NeverEngagedChatClient`, `FailingChatClient`, and similar) correctly get
`producer.fail(...)` instead of a fabricated success. No `chat()` body, test assertion, or expected
value was touched anywhere — this pass changes only what a previously-dead method now does, never
what any currently-passing test observes. Full rebuild + suite: 208/208 pass (0 regressions across
46 independently-edited translation units).

**Phase 3, part 2 (real design work, not mechanical):** both remaining production call sites
converted to drain `chat_stream()` instead of calling `chat()`. A new shared
`core/chat_stream_drain.hpp` (`DrainedChatStream`/`drain_chat_stream()`) is factored out of
`ModelCallGateway`'s own internal copy of the identical poll loop, rather than writing a third
independent implementation — `ModelCallGateway` itself updated to use the shared version (a pure,
behavior-preserving refactor, reverified against its own unmodified 20-check test suite before
proceeding). `MemoryProvider::on_turn_end` keeps its existing best-effort semantics (an extraction
failure never fails the turn) and doesn't need `DrainedChatStream::usage` at all.
`HistoryProvider<Summarize<...>>::on_context` keeps propagating a real failure, now converted from
the drained `quark::error` via a shared `drained_failure_to_agent_error`. 208/208 pass.

**Phase 3, part 3 (the concept change itself):** `chat()` removed from `ChatClient`'s `requires`
clause — `chat_stream()` is now the only method every conformer must have. This does not delete
`chat()` from any existing type (every real backend and every fixture in this codebase still has
one); it only widens what *can* satisfy the concept going forward. Safe with zero behavior change
for anything that exists today, precisely because parts 1 and 2 had already independently verified
every current conformer genuinely streaming-capable before this line was touched — this ordering
(prove universal capability, *then* relax the requirement) is the whole reason parts 1-3 are three
separate, independently-tested commits rather than one. 208/208 pass, unchanged.

## 6. Falsifiable claims and verdicts

| # | Claim | Verdict |
|---|---|---|
| C1 | `AnthropicChatClient`'s streaming path now reports real, correctly-accumulated (not summed) usage on the terminal update, including for a content-free completion. | **CORRECT** — E-STREAM-R1 (extended) + new E-STREAM-R2, both against real Anthropic SSE event shapes; reverified live via the existing loopback-TLS harness. |
| C2 | The leak-scan is genuinely backend-agnostic and path-agnostic, not merely relocated. | **CORRECT** — L1 (streaming, armed) and L3 (non-streaming `chat()`, armed, no `OpenAIChatClient`-specific flag involved) both promote the identical leaked tool call through the same `AgentSession`-level flag. |
| C3 | The flag genuinely gates the behavior — it is not silently always-on. | **CORRECT** — L2: with the flag off (default), the identical leaked text reaches the final `Message` byte-for-byte unchanged. |
| C4 | A second scan of an already-scanned message never promotes a different candidate the first pass declined. | **CORRECT** — L4, the direct regression proof for the red-team's must-fix, not an absence-of-failure inference. |
| C5 | The 46-file fixture conversion changed no test's observable behavior. | **CORRECT** — full suite 208/208, zero `chat()` bodies or assertions touched, confirmed by diff review of all 46 files. |
| C6 | `MemoryProvider`/`HistoryProvider`'s move off `chat()` preserves each call site's own existing failure semantics (best-effort vs. propagating). | **CORRECT** — same 208/208 suite, including `test_memory_provider.cpp`/`test_history_provider_summarize.cpp` unmodified. |
| C7 | Dropping `chat()` from the concept's `requires` clause changes no existing conformer's behavior. | **CORRECT** — full rebuild (every translation unit in the project) + 208/208, identical to the pre-change baseline, confirming the ordering claim in §5 empirically, not just by argument. |

## 7. What this ADR does not claim

- `chat()` is **not deleted** from any type anywhere — real backends, the three legacy wrapper
  templates, and every fixture still declare and use it exactly as before. This ADR only stops the
  *concept* from requiring it; a future, separate decision would be needed to actually remove the
  method itself from any conformer, and nothing here argues that should happen.
- `FailoverChatClient`/`ResilientChatClient`/`MiddlewareChatClient`'s own `chat_stream()` passthrough
  limitations (no failover, no breaker feedback, no hook interception on that path — each file's own
  long-standing, unchanged top comment) are **not addressed** by this ADR. `ADR-036`'s
  `ModelCallGateway`/`MiddlewareModelCallGateway` supersede their purpose for anyone who needs
  retry/failover/middleware genuinely working under streaming; whether to formally deprecate the
  three older types, or leave them as a documented, lower-capability legacy path, is an explicit,
  separate, not-yet-made decision.
- The Anthropic SSE hardening in §3 (rejecting a misrouted `text_delta`) assumes a malformed gateway
  fails in that *specific* way; it is not a general proof that every conceivable malformed-gateway
  behavior is safe against the scan — named as the same class of residual the original red-team
  flagged, softened but not eliminated.
- No new live-network verification was run for Phase 3's fixture conversions or the concept
  relaxation — both rest on the deterministic offline suite (208/208) and, for Phase 1, the existing
  live/loopback-TLS coverage that was already in place.

## 8. Files changed

**Phase 1a:** `include/agentengine/protocol/anthropic/chat_client.hpp`,
`tests/test_anthropic_chat_client_translation.cpp`.

**Phase 1b:** `include/agentengine/core/response_format_leak_scan.hpp` (new),
`include/agentengine/core/agent_session.hpp`, `include/agentengine/core/content.hpp` (comment
accuracy), `include/agentengine/protocol/openai/chat_client.hpp`,
`include/agentengine/protocol/anthropic/chat_client.hpp`, `tools/cli_chat.cpp`,
`tests/test_agent_session_response_format_leak_scan.cpp` (new), `tests/CMakeLists.txt`.

**Phase 3, part 1:** 46 files across `examples/` (7) and `tests/` (39) — see the commit
(`ADR-035 Phase 3 (part 1)`) for the exhaustive list; every one already conformed to `ChatClient`
before this change, only `chat_stream()`'s body changed.

**Phase 3, part 2:** `include/agentengine/core/chat_stream_drain.hpp` (new),
`include/agentengine/core/model_call_gateway.hpp` (refactored to use the shared helper),
`include/agentengine/core/memory_provider.hpp`, `include/agentengine/core/history_provider.hpp`.

**Phase 3, part 3:** `include/agentengine/core/chat_client.hpp` (the `ChatClient` concept itself).

Full regression suite at every phase boundary: **208/208** (Phase 1b onward; 206/206 at Phase 1a,
before the new leak-scan test file existed). Two unrelated, pre-existing, load-dependent flakes
(`test_native_jail_*`'s OOM-classification timing, `test_provider_egress_address_policy`) were
observed once under parallel `-j4` execution and confirmed non-reproducing both in isolation and
against a clean re-run of the full suite — neither has any header dependency on anything this ADR
touches.
