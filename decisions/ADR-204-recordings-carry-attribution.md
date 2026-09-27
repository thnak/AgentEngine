# ADR-204 — Chat-call recordings carry `Message::attribution`

- **Status**: **Proposed — design + implementation + proof (2026-09-27); red team not yet run.**
- **Date**: 2026-09-27
- **Origin**: project-owner decision (2026-09-27) on ADR-202 §9's open question.
- **Touches**: `include/agentengine/core/message_json.hpp` (`Profile::carries_attribution()` removed, comments),
  `src/core/message_json.cpp` (both profiles write and read `attribution`), comment-only:
  `include/agentengine/core/chat_recording.hpp`, `include/agentengine/rt/message_codec.hpp`,
  `include/agentengine/rt/agent_session.hpp`, `include/agentengine/eval/eval_tier1_screen.hpp`. Tests:
  `tests/core/chat/test_message_json_equivalence.cpp`, `tests/core/chat/test_chat_recording_codec.cpp`,
  `tests/core/chat/test_replay_chat_client.cpp`, `tests/eval/test_eval_tier1_screen.cpp` (T43).

## 1. The question

ADR-202 merged the two Message JSON codecs into one with two profiles and kept every difference, including one it
flagged as a gap rather than a design: the recording profile neither wrote nor read `Message::attribution` (ADR-066's
"which context provider contributed this message"). A chat-call recording is the audit trail of what reached the model
(004 §6), so the recorded request of a turn assembled from context providers could not say which provider contributed
which message — an I4 ("every effect is attributable") gap. ADR-202 §9 left it to the owner because closing it changes
the recording format and the eval screen's design hash for attributed prompts.

## 2. Decision

The owner decided (2026-09-27) that recordings carry attribution. `Profile::recording()` now writes and reads
`attribution` exactly as `Profile::state()` does:

- **Encode:** written after `"content"` (the last member) when `Message::attribution` has a value, as
  `{"contributor_index": <number>, "contributor_type": <string>}`; omitted when it is `nullopt`.
- **Decode:** read when `"attribution"` is an object (`contributor_index` via `opt_u64`, `contributor_type` via
  `opt_string`, wrong-typed fields default); ignored when absent or not an object, so the message's attribution stays
  `nullopt`.

**Still exactly two profiles.** The delivery-marks rule is unchanged: the state profile never writes or reads
`approval` / `deliver_as_instructions` (I3, ADR-202 §5), and the recording profile does. What remains different between
the profiles is the marks, the error-code prefix and the non-string-`kind` behaviour (ADR-202 §4 D1-D3, D6); D4/D5
(attribution) are gone.

**`carries_attribution()` is removed.** It would now answer `true` for both profiles. A query that cannot distinguish the
two values selects nothing; keeping it would only suggest attribution is still a per-profile choice and invite someone to
flip it back for one profile. The header says instead, beside `Profile`, that both profiles carry attribution and why
(no consumer decides authority from it, §5), which is the readability the query offered.

## 3. Format change

A recording file gains an `"attribution"` member on each message that has one: in `request.messages[]` and in
`response.message` (the two places the envelope encodes a `Message`). Streaming chunks carry `ContentItem`s, not
messages, and are unchanged.

- **Old files still read.** A recording written before this ADR has no `"attribution"` member; every message reads back
  with `attribution == nullopt`, as before (tested, §6).
- **Unattributed messages encode byte-identically** to before (omit-when-absent), so recordings of sessions that do not
  compose context providers through `assemble_context()` — the default `AgentSession` path (agent_session.hpp's
  ADR-066 §7 note) — do not change at all.

## 4. Eval design hash: effect and migration

`eval_tier1_screen.hpp`'s pre-registration digest (ADR-195 §8, E32) hashes each probe's and regression task's
`task_prompt` as the recording `message_to_json` renders it.

- **A prompt without attribution hashes exactly as before.** Pinned: `tier1_preregistration_digest(spec())` in
  `test_eval_tier1_screen.cpp` equals `c33d2d2610da0a677c7d3a57050b5843935b794097ea88f8709914083f1c49fa`, the value
  computed at b0cbda6 (before this change) by the same test (T43).
- **A prompt with attribution hashes differently.** Before this ADR, attribution was dropped, so an attributed prompt
  hashed the same as the unattributed one; now the design JSON contains each prompt's `attribution` object and the
  digest differs (T43 checks both, and that removing exactly the attribution objects from the attributed design JSON
  gives the unattributed design JSON byte for byte).
- **Migration.** Hashes of attributed prompts change at this commit. A stored pre-registration digest (a
  `Tier1AttemptLog` entry, a `Tier1ScreenRecord`'s `preregistration_digest`) over a design whose prompts carried
  attribution no longer matches a recomputation: re-run such saved evals (a new attempt under the new digest). The
  attempt log is keyed by lineage, not by design digest, so the old attempts stay counted in their lineage; the re-run
  shows a different digest and adds one to `distinct_preregistrations` — the conservative direction (more visible
  retries, never fewer). Designs whose prompts carry no attribution — every prompt built
  by hand as a plain `Message`, which is how the screens are driven today — are unaffected. `eval_tier1_screen.hpp`'s
  hash note says the same.
- **Fixtures.** Searched `tests/`, `conformance/`, `schema/`, `docs/` and `examples/` for recording fixtures or stored
  digests containing attributed messages: none. The `"attribution"` literals are in the codec tests only
  (`test_message_json_equivalence.cpp`, `test_message_codec_attribution.cpp`, `oracle_message_codec.hpp`); the chat
  fixtures under `tests/fixtures/chat_client/` carry no attribution; the only 64-hex digest literals in eval tests are
  a zero digest in `test_promotion_ack.cpp` (unrelated) and the new T43 pin. No fixture needed updating.

## 5. I3 / I2: reading attribution back from a file restores data, and nothing decides authority from it

Reading `attribution` from a recording lets a file assert provenance for a message. That is acceptable only if no
consumer turns attribution into authority. Every use in `include/`, `src/` and `tools/` (grep for `attribution`, then
for `.attribution` / `->attribution`):

| Site | What it does | Decision? |
|---|---|---|
| `core/context_assembly.hpp` (`assemble_context`) | **Writes** `{index, contributor.name}` on every contributed message and tool, unconditionally — overwriting whatever the message carried. | Producer; reads nothing. |
| `src/core/message_json.cpp` | Encodes and decodes it (both profiles). | Codec. |
| `eval/eval_trial.hpp` `message_is_memory_attributed` | Measures whether each recorded request carried the lesson in a message attributed to the `memory` contributor (`delivered`). | An eval measurement, not an authority or permission decision. Its input is the trial's own in-memory sink (`trial_result.recordings`), never a decoded file, and the trial composes its providers through `ComposedContextProvider`, so every request message was stamped by `assemble_context()` this turn. Unchanged by this ADR. |

Nothing else reads it: no capability check, policy, approval, fence or serializer (`protocol/*` do not mention it;
`system_channel_fence.hpp` and the approved-lesson path consult `origin`, `tainted` and the marks, not attribution),
and `tools/` only mentions ADR-066 in comments. `content.hpp` defines it as provenance.

The two readers of recording files:

- **`read_chat_call_recording` / `chat_call_recording_from_json`** rehydrate `request.messages` "for human/debug
  legibility" (chat_recording.hpp); nothing re-issues a recorded request (there is deliberately no
  `chat_request_from_json` consumer path), so a restored request attribution goes nowhere but the reader.
- **`ReplayChatClient`** serves the recorded **response** only. Its message now keeps its recorded attribution and, in an
  `AgentSession`, is appended to history like any response. From there it reaches (a) a later request only through
  the history provider — through `assemble_context()` it is overwritten with this turn's stamp; through the plain
  single-provider path it is carried unchanged, and no serializer reads it — and (b) the state profile's turn-delta
  record, which already carried attribution (ADR-066). A live provider never sets attribution on a response (the
  protocol translators construct messages without it), so a genuine recording only ever replays `nullopt` there; a
  crafted file can assert a value, which is data in the same sense as the text beside it.

So attribution stays what ADR-066 made it: provenance recorded for audit (I4), never an input to a permission (I2) and
never something model output sets or that can grant trust (I3). A mark-style guard like the state profile's is not
needed. **Condition for keeping this true:** any future consumer that decides anything from `Message::attribution` must
not trust a value restored from a stored file (recording or state) — only one `assemble_context()` stamped in the
current turn. Recorded as a residual (§7).

## 6. Evidence

- **Differential test** (`tests/core/chat/test_message_json_equivalence.cpp`), still against ADR-202's verbatim
  oracles of both pre-merge codecs. The state comparison is unchanged. The recording comparison now expects:
  the oracle unmodified for every item (attribution is message-level), every message without attribution, every
  error (class, code, message) and every throw (the D3 `std::bad_variant_access`, 185 of them, unchanged); and for an
  attributed message, the old recording encoding plus the `attribution` member exactly where and as the old state codec
  writes it (last, after `content`; the test asserts the state oracle puts it last), and on decode the old recording
  value with `attribution` set exactly as the old state decoder reads it. Every attributed encoding is also checked to
  differ from the old recording encoding. Totals: 13,410 encodings and 18,042 decodings compared (15,206 ok, 2,651
  errors, 185 throws), 43,091 checks; recording messages encoded 1,130 without / 1,121 with attribution, decoded 1,799
  without / 513 with an `"attribution"` member. The I3 tests are kept (state never writes or reads the marks, outer and
  nested; the recording profile does, as control), and the attribution section now checks: both profiles round-trip
  it; the recording decoder reads the state encoding's attribution; the state decoder reads a recording's attribution
  and still drops its marks; both write the same bytes in the same position; absent attribution is omitted and an
  unattributed message encodes byte-identically to the old recording codec; an old recording (no member) reads back
  with none. Harness self-controls kept, plus two new ones: the ADR-204 expected encoding and decode differ from the old
  recording oracle for an attributed message, so the attributed comparisons are not vacuous.
- **File round trip** (`test_chat_recording_codec.cpp`, `test_attribution_file_round_trip`): a recording with an
  attributed request message, an unattributed one and an attributed response is written with
  `write_chat_call_recording` and read with `read_chat_call_recording` — both attributions survive, the unattributed
  message reads back with none; a hand-written pre-ADR-204 recording file reads back with no attribution on request or
  response.
- **Replay** (`test_replay_chat_client.cpp`, case 9): the same through a file, then served by `ReplayChatClient`: the
  replayed response carries the recorded attribution and is otherwise the recorded response; the read-back request keeps
  its attribution.
- **Hash** (`test_eval_tier1_screen.cpp`, T43): §4.
- **Positive control.** On scratch copies under g++-14 (the worktree was never edited), rebuilding
  `message_json.cpp` with one guard changed and re-running the unchanged tests:
  - A: the recording profile stops writing attribution (`profile == Profile::state() &&` on the encode guard):
    `test_message_json_equivalence` **2,246 failures**, `test_chat_recording_codec` **3**, `test_replay_chat_client`
    **2**; all exit 1.
  - B: the recording profile stops reading it (same guard on decode): **309**, **2**, **2**; all exit 1.
  - C: the whole pre-change codec (b0cbda6's `message_json.hpp`/`.cpp`) under the new tests: T43 **2 failures** (the
    attributed digest equals the pinned pre-change digest; the attributed design JSON has no attribution), while its
    unattributed-digest check passes (the pin is the pre-change value); `test_chat_recording_codec` **3**,
    `test_replay_chat_client` **2**; exit 1.
  - Reverted: all pass.
- **Builds / tests / g++ / lints:** §6a.

### 6a. Build and test results

- **MSVC:** `dev` preset (`cmake --preset dev`, fresh `build-dev`), `cmake --build build-dev -j 2`: 712 steps, `/W4 /WX`,
  no warnings, no errors; every compile-fail gate target built.
- **ctest** (`-j 2`, CI's `-E` list plus `test_external_skill_discovery`): **350/350 pass** (the two
  `*_no_process_creation` probes skipped as always), including `test_message_json_equivalence`,
  `test_chat_recording_codec`, `test_replay_chat_client`, `test_message_codec_attribution` and
  `test_eval_tier1_screen`.
- **g++-14** (WSL Ubuntu 24.04, `-std=c++23 -Wall -Wextra -Werror`, `-O0` and `-O3 -DNDEBUG`):
  `src/core/message_json.cpp`, the three chat tests and `test_eval_tier1_screen.cpp` compile clean and pass at both
  levels (the equivalence totals match MSVC's; T43's pinned digest is the same on Linux and Windows).
- **Lints:** `tools/naming_lint.py`, `tools/milestone_status_lint.py`, `tools/layering_lint.py` (8 baselined edges, no
  new) and `tools/layering_lint.py --self-test`: all pass.

## 7. Residuals

- **A future consumer that decides from attribution** would have to treat a value restored from a stored recording or
  state file as untrusted (§5). None exists today.
- **Stored design hashes over attributed prompts** stop matching (§4); re-run those evals. None exists in the repo.
- **`ReplayChatClient` replays a recorded response's attribution into a session's history.** Data only (§5); a genuine
  recording always replays `nullopt` there.
- ADR-202's other residuals (D3 throw, AG-UI projection uses the state profile, link-time replacement) are unchanged.
