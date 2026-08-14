# ADR-048 — A fail-closed outbound Media capability gate (gap 19, Phase 1)

**Status:** Proposed (2026-08-14). Designed, self-red-teamed, implemented, and proven (real code +
new tests across two files, full suite green); awaiting the project owner's explicit "Judged"
sign-off per this project's governance (`decisions/README.md`; `OpenQuestions.md` OQ-11).

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #19 (the finding this
ADR closes, Phase 1 only). `004-Model-Provider-Plane.md` §2 (`ChatClientCapabilities`'s
`multimodal_in_image`/`_audio`/`_video`/`_file` bits — declared, not probed). `003-Message-and-
Content-Model.md` (the `Media` content kind this gate checks).

## 1. The question

**Stated so it has a wrong answer:** when a `ChatRequest`'s history carries `Media` content (an
image, audio clip, video, or file) that the bound backend has no wire encoding for, does the caller
find out?

**Before this fix: no.** Both real backends' own outbound `translate_message()` (`protocol/openai/
chat_client.hpp`, `protocol/anthropic/chat_client.hpp`) loop over each message's content items with
branches for `Text`/`ToolCall`/`ToolResult` only — `Media` (along with `Reasoning`/`Citation`/
`Custom`) simply has no branch, so it is dropped with zero signal, unconditionally, regardless of
what `ChatClientCapabilities` declares. The drop was never actually a capability mismatch worth
silently tolerating — every backend that exists in this codebase today drops it every time, whether
`multimodal_in_image` is declared true or false, because neither backend's translation layer checks
the capability bit at all before (not) encoding the content.

## 2. What re-grounding against current code found

- **This is Phase 1 of the audit's own two-phase split**, confirmed as the right scoping: Phase 2
  (real wire encoding — actually sending image/audio/video bytes to a backend) needs RFC 019's
  blob-store seam, confirmed absent from the tree (no `019` implementation exists anywhere under
  `include/` or `src/`). Phase 1 does not attempt encoding at all — it only makes the EXISTING silent
  drop loud, which is fully implementable now with zero new infrastructure.
- **The gap's own "Media nested inside `ToolResult::content` remains unaddressed by either phase"
  residual** is closed here as a direct, low-cost consequence of writing the check correctly: a tool
  reply carrying an image is exactly as unencodable as one arriving directly in a `Message`, and
  `translate_message()`'s own tool-result loop already recurses into `ToolResult::content` for the
  kinds it DOES handle — the gate below mirrors that same recursive shape.
- **`Citation`/`Custom` are out of scope, correctly** — 003 §1 treats `Citation` as an annotation on
  `Text` (not a standalone content kind since 2026-08-04's Q1 resolution) and `Custom` carries no
  capability bit in `ChatClientCapabilities` at all; neither is what gap 19's title (Image/Audio/
  Video/File) or 004 §2's `multimodal_in_*` bitset actually names.

## 3. The design

`validate_outbound_media_capabilities(request, caps) -> result<void>` (`core/chat_client.hpp`):
walks every `Message`'s content items (recursing into `ToolResult::content`), categorizes each
`Media` item by its `media_type`'s MIME prefix (`image/`→image, `audio/`→audio, `video/`→video,
anything else→file — the same catch-all `ChatClientCapabilities`'s own bitset uses), and checks the
matching `multimodal_in_*` bit. The FIRST violation found fails the whole call closed with a real,
attributable `result<void>` error (`chat_client.multimodal_capability_missing`) naming the offending
message id and media type — never a partial send, never a silent continuation.

Wired into `AgentSession::run_model_call()` (`rt/agent_session.hpp`), called first, before any of
the gateway/chat/chat_stream branches — the ONE real production call site every model call passes
through regardless of `ChatClientT`'s shape (`ModelCallGatewayLike` or a raw `ChatClient`), using
`chat_client_->capabilities()` (already required by every conformer). A violation emits a real
`run_event_kind::run_failed` and returns the same error to the caller — the run stops before the
backend's own translation ever gets a chance to silently drop anything.

## 4. Self-red-team findings

**Scoping the recursion correctly mattered.** An early sketch checked only top-level `Message::
content`, missing the `ToolResult`-nested case the audit itself named as a known residual — since
recursing costs nothing extra (the same walk `translate_message()`'s own tool-result loop already
performs) and directly closes a named gap, there was no reason to leave it out.

**Fail on the first violation, not collect-and-report-all.** Unlike ADR-045's JSON Schema validator
(which legitimately needs a bounded LIST of violations for a caller doing schema authoring), this is
a pre-flight reject for a single outbound request — one clear, attributable reason to stop is
sufficient, and collecting every violation would add complexity with no real consumer for the extra
detail.

## 5. What this ADR does not claim

- **Phase 2 (real wire encoding) is not attempted** — named, not silently implied. A caller whose
  backend DOES declare the matching capability but whose `Media::payload` this codebase has no
  encoder for yet would still need Phase 2's own, separate work once RFC 019's blob-store seam
  exists.
- **Does not gate `Citation`/`Custom`/`Reasoning`** — proven directly (a `Reasoning`-only request
  passes the gate regardless of capabilities) that this check is scoped strictly to `Media`, matching
  004 §2's bitset and nothing broader.
- **A ModelCallGateway composition's OWN capabilities() is what's checked**, not any individual
  backend it might route to internally (retry/failover tiers) — `ModelCallGatewayLike::capabilities()`
  is already a single, already-required accessor every gateway conformer provides; this ADR adds no
  new per-tier visibility beyond what that accessor already reports.

## 6. Evidence

`tests/test_outbound_media_capability_gate.cpp` (new, pure-function level): each of the four
categories independently fails closed with no declared capability and a real, attributable error
code; a positive control proves a declared capability lets the identical content through; a
text-only request is unaffected regardless of capabilities; `Media` nested inside `ToolResult::
content` is caught (with its own positive control); a `Reasoning` item never trips the gate.

`tests/test_rt_agent_session_media_capability_gate.cpp` (new, end-to-end): a real run whose history
carries an ungrantable image fails closed and `chat()` is NEVER invoked — the whole point, verified
directly rather than merely inferred from the pure-function result; a positive control with the
capability declared converges normally with exactly one `chat()` call.

Full suite: green (`ctest`, this pass), zero regressions.
