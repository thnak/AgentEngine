# Real-time voice and computer-use — source-grounded notes

**Date:** 2026-08-05 · **Sources:** cited inline.

## 1. Real-time voice (speech-to-speech)

**Quark's `VoiceChannel` (`third_party/quark/028-Voice-Datagram-Channel.md`, Draft) is not the
right transport for this** — checked directly, not assumed. It's a room-based, best-effort,
unordered UDP relay built for many-peer game/media voice chat between cluster nodes, with an
explicit non-goal of "reliability, ordering, or retransmission of any kind." Every real vendor
speech-to-speech API runs over an ordered, reliable connection instead:

- **OpenAI Realtime API** — WebSocket or WebRTC, continuous chunked PCM both directions, no
  per-turn request/response boundary; turns are delimited by VAD events. Server-side VAD
  (`server_vad`/`semantic_vad`) is the default; `turn_detection: null` disables it for client-driven
  push-to-talk. Interruption sends `conversation.item.truncate` — this edits the retained
  transcript/state, but audio already played to the user cannot be un-played
  ([developers.openai.com/api/docs/guides/realtime-conversations](https://developers.openai.com/api/docs/guides/realtime-conversations),
  [.../realtime-vad](https://developers.openai.com/api/docs/guides/realtime-vad), accessed
  2026-08-05).
- **Gemini Live API** — agrees on server-side VAD as default over a bidirectional WebSocket; on
  interruption, "the ongoing generation is canceled and discarded," and **pending function calls in
  flight are explicitly cancelled with reported cancel IDs**. Hard session caps: 15 min audio-only,
  2 min audio+video ([ai.google.dev/gemini-api/docs/live-guide](https://ai.google.dev/gemini-api/docs/live-guide),
  updated 2026-07-31).
- **Billing is not per-request.** OpenAI meters audio as tokens on a fixed clock (1 input
  token/100ms, 1 output token/50ms); ElevenLabs bills per wall-clock conversation minute regardless
  of compute time.
- **No vendor supports deterministic session replay.** A dropped connection starts a new session;
  reconstructing history is the caller's job via a resent transcript, not a durable replayable
  artifact the API itself offers.
- **Audio prompt injection is measured, not hypothetical**: "Piggybacking on Perception"
  (arXiv:2607.28165, submitted 2026-07-31) reports 69.1–81.6% attack success across 11 multimodal
  voice agents via concurrent/overlapping or ultrasonic audio injection — the model cannot cleanly
  separate system instruction from audio-carried data, the same unsolved problem text-prompt
  injection has, one modality over.

**Why this doesn't get a full spec pass today**: turn interruption-as-the-common-case is a real
tension with 001's turn loop, and the no-deterministic-replay property is a direct, structural
collision with **I5** ("nondeterminism crosses a recorded seam... runs replay offline") — not a case
where determinism can be waived the way 029 §9 G6 waives it for vector retrieval, since a real
fraction of the turn's content (the live audio itself) may be unreplayable by construction, not by
choice. That's an invariant-level question, not a plugin-shaped one.

## 2. Computer use (screen capture + input injection)

- **Action schema is ordinary tool calls**: Anthropic's `computer` tool
  (`computer_20251124`) emits `{"action": "left_click", "coordinate": [x,y]}`-shaped calls —
  `screenshot`, `click`/`drag`/`scroll`, `type`, `zoom` (view a region at full resolution). OpenAI's
  `computer-use-preview` model is structurally identical (`click`, `type`, `scroll`, `keypress`,
  `drag`, `screenshot`) ([platform.claude.com/.../computer-use-tool](https://platform.claude.com/docs/en/agents-and-tools/tool-use/computer-use-tool),
  [developers.openai.com/api/docs/guides/tools-computer-use](https://developers.openai.com/api/docs/guides/tools-computer-use),
  accessed 2026-08-05).
- **Both vendors mandate an isolated virtual display, never the real host desktop.** Anthropic's
  reference implementation runs Claude inside Docker + Xvfb; OpenAI's Operator system card
  (published 2025-01-23, [cdn.openai.com/operator_system_card.pdf](https://cdn.openai.com/operator_system_card.pdf))
  states the model "performs best in browser-sandboxed contexts" and ships a containerized reference
  app for exactly this reason.
- **Pixel targeting is vision-model judgment, not deterministic accessibility-tree lookup** — neither
  vendor defaults to accessibility-tree element targeting. Anthropic's own launch numbers: Claude
  3.5 Sonnet scored 14.9% on the OSWorld benchmark using screenshots alone (22% with extended time
  budget) — targeting accuracy is a tunable, fundamentally probabilistic property, not a bug to fix
  to zero ([anthropic.com/news/developing-computer-use](https://www.anthropic.com/news/developing-computer-use),
  2024-10-22, cross-checked against current docs 2026-08-05).
- **Prompt-injection-via-screen is observed, not hypothetical, and unsolved at the vendor level.**
  Two real attacks within days of Anthropic's October 2024 public beta: **ZombAIs**
  (Johann Rehberger/Simon Willison, 2024-10-25) — a webpage's on-screen text told Claude to
  download and execute malware, which it did
  ([simonwillison.net/2024/Oct/25/zombais](https://simonwillison.net/2024/Oct/25/zombais/));
  **HiddenLayer** (2024-10-24) — a PDF with obfuscated destructive shell commands plus
  social-engineering text convinced Claude it was in a "security testing" environment, and it
  executed the payload, wiping the filesystem
  ([hiddenlayer.com/research/indirect-prompt-injection-of-claude-computer-use](https://www.hiddenlayer.com/research/indirect-prompt-injection-of-claude-computer-use)).
  Anthropic's own docs admit no full defense exists: "instructions on webpages or contained in
  images might override your instructions."

**Why this doesn't get a full spec pass today**: the action schema and sandbox-scoping fit existing
machinery cleanly (007 §3's parameterized-capability shape, 008's `remote` profile, 006 §7's
existing tool-result taint rule already covers a returned screenshot) — but the two observed attacks
both defeat the *specific mechanism* 017 relies on for text (delimiting/provenance-marking external
content in the prompt), because that mechanism assumes text you can wrap in a marker, and neither
attack's payload arrives as text the engine controls the framing of. That's a real gap in 017's
model, not an extension of something already there, and the vendors themselves ship mitigation
(mandatory confirmation before consequential actions) rather than a fix.
