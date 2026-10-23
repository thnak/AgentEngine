# ADR-202 — One Message/ContentItem JSON codec with two named profiles, not two drifted copies

- **Status**: **Judged (2026-09-27, project-owner sign-off).** Re-checked on `main` before sign-off: I3 controls A and B
  re-run and caught (see "Re-check before sign-off" at the end). Original status: Proposed — design + implementation +
  proof (2026-09-27); red team round 1 (same day): no BLOCKER or MAJOR, 1 MINOR and 3 NOTEs, all addressed in the text
  (§10). Verified live against DeepSeek (§6).
- **Date**: 2026-09-27
- **Origin**: GitHub issue #120 (S3), the structure audit after issue #115 closed.
- **Touches**: `include/agentengine/core/message_json.hpp` (new, declarations and the `Profile` type),
  `src/core/message_json.cpp` (new, bodies), `include/agentengine/core/chat_recording.hpp` and
  `include/agentengine/rt/message_codec.hpp` (both reduced to wrappers), `CMakeLists.txt`
  (`agentengine_message_json`, linked through `agentengine::core`), `tools/layers.toml` (the new files are layer V),
  `tests/core/chat/test_message_json_equivalence.cpp` and `tests/support/oracle_*.hpp` (new). Comment-only:
  `rt/agent_session.hpp`, `rt/workflow_supervisor.hpp`, `protocol/a2a/types.hpp`,
  `src/backends/native_jail/relay_base64.hpp`.

## 1. The question

The engine had two complete Message/ContentItem <-> JSON codecs:

- `core/chat_recording.hpp` (namespace `agentengine`, helpers in `recording_detail`): chat-call recordings (004 §6),
  and through them `RecordingChatClient` and `ReplayChatClient`; and `eval/eval_tier1_screen.hpp`, which hashes each
  prompt as this `message_to_json` renders it (ADR-195 §8).
- `rt/message_codec.hpp` (namespace `agentengine::rt`, helpers in `message_codec_detail`): workflow checkpoints
  (`workflow_run_state_record.hpp`), the AgentSession turn-delta record (`agent_session.hpp`),
  `workflow_as_chat_client.hpp`, the AG-UI projection and the test driver. Its banner said it was copied from the first
  so that it would not depend on `chat_client.hpp`.

The copies had already drifted: ADR-066 added `Message::attribution` to one, ADR-191/192 added the delivery marks to
the other. Each future content-model change had to be made twice, and a change made to one only is exactly how the two
came to differ. Can they be one codec without changing what either caller reads or writes?

The drift is not all accidental. The state codec must not read the delivery marks back (I3, §5), and the recording
codec must not start writing `attribution` without a decision (§9). So the merge cannot be "pick one".

## 2. Designs considered

- **A. One codec, two named profiles, the old functions kept as wrappers (chosen).** The bodies are written once; a
  profile value selects the few behaviours that differ. Callers do not change.
- **B. One codec with free-form options** (`struct { bool marks; bool attribution; ... }`). Rejected: any caller could
  build "state, but with delivery marks", which is the I3 failure. The combinations that exist are the two callers.
- **C. Make one header include the other.** Rejected for the reason the state copy was made: `chat_recording.hpp`
  pulls in `chat_client.hpp`, and the state side has no business depending on the chat plane. It would also still need
  a profile for the differences.
- **D. Leave it.** Every content-model field keeps needing two edits and a decision about which copy gets it.

## 3. Decision

Design A.

- **One codec.** `core/message_json.hpp` declares, in namespace `agentengine::message_json`, `role_to_wire_string`,
  `role_from_wire_string`, `origin_to_wire_string`, `origin_from_wire_string`, `content_item_to_json`,
  `content_item_from_json`, `message_to_json` and `message_from_json`; the four item and message functions take a
  `Profile`, the four wire-string functions do not (they do not differ between the copies).
  The bodies are in `src/core/message_json.cpp`, compiled once into the static library `agentengine_message_json`.
  The header includes only `core/content.hpp`, `core/json_value.hpp`, `core/error.hpp` and std, and is layer V in
  `tools/layers.toml` (as `rt/message_codec.hpp` already was); the `.cpp` is also V.
- **Exactly two profiles.** `message_json::Profile` is a class with a private constructor and two factories,
  `Profile::recording()` and `Profile::state()`. It stores only which of the two it is; the behaviours are read-only
  queries derived from that (`carries_delivery_marks()`, `carries_attribution()` (removed by ADR-204), `rejects_non_string_kind()`,
  `error_code_prefix()`). No caller can build a third combination. The I3 reasoning is written at the definition.
- **Bodies moved, not rewritten.** The two copies were line-for-line the same apart from §4's differences; the `.cpp`
  is that common text (the recording copy's spelling, which was shorter) with a `profile` check at each difference.
  `base64_encode`, `base64_decode`, `blob_ref_to_json` and `blob_ref_from_json` are private to the `.cpp`. The field
  readers `opt_string`, `opt_bool` and `opt_u64` are inline in the header under `message_json::detail`, because
  `chat_recording.hpp`'s envelope code (usage, responses, chunks, errors) uses them too.
- **Wrappers.** Every public function keeps its namespace, name and signature:
  `agentengine::{role,origin}_{to,from}_wire_string`, `content_item_{to,from}_json` and `message_{to,from}_json` in
  `chat_recording.hpp` call the recording profile; the same eight in `agentengine::rt` (`rt/message_codec.hpp`) call
  the state profile, including `rt::role_to_wire_string`, which `tools/test_driver/test_driver.hpp` uses. The
  `ChatRequest`/`ChatResponse`/`ChatCallRecording` machinery, the `failure_class`/`error` codec and `recording_detail::
  require` (its code is the envelope's `recording.missing_field`) stay in `chat_recording.hpp`. The duplicated helper
  copies (`recording_detail::base64_*`, `blob_ref_*`, `opt_*`; all of `rt::message_codec_detail`) are gone;
  `recording_detail` re-exports the three field readers by using-declaration. Other base64 copies (`a2a/types.hpp`,
  `mcp/client.hpp`, `relay_base64.hpp`) are #120 S8 and untouched apart from two comments that named the old helpers.
- **Link.** `agentengine_message_json` is linked through `agentengine::core`'s interface, like
  `agentengine_tool_pipeline` (ADR-201). A compiled library that calls a wrapper and does not link `agentengine::core`
  would have to name it itself so a single-pass linker sees it after that library; none does (§6, `dumpbin`), so no `PUBLIC` link was added.
- **Compile-fail gates.** Since #120 S9 these are ordinary targets (`tests/compile_fail/CMakeLists.txt`) linking real
  libraries through `agentengine::core`; there is no hand-maintained `try_compile` source list left to extend. The
  full build (§6) configures and builds them all.

### Seam table

| Profile | Callers | `approval`, `deliver_as_instructions` | `Message::attribution` | Non-string `"kind"` | Error codes |
|---|---|---|---|---|---|
| `recording()` | `core/chat_recording.hpp` wrappers: `RecordingChatClient` (writes recordings), `ReplayChatClient` (reads them), `eval_tier1_screen.hpp` (hashes `message_to_json` output), `tools/cli_chat.cpp`, `tools/test_driver/live_backend.hpp` | written when set (omitted when empty/false); read | neither written nor read (superseded by ADR-204: written when present, read when an object, as `state()`) | throws `std::bad_variant_access` (as before, D3) | `recording.*` |
| `state()` | `rt/message_codec.hpp` wrappers: workflow checkpoints (`workflow_run_state_record.hpp`), the turn-delta record (`agent_session.hpp`), `workflow_as_chat_client.hpp`, the AG-UI projection (`protocol/agui/projection.hpp`), the test driver | **neither written nor read (I3)** | written when present; read when an object | `contract` error `rt.message_codec.missing_field` | `rt.message_codec.*` |

## 4. Behaviour — the differences found, and how each is kept

Found by a line diff of the two old codecs after normalizing the recording copy's unqualified names and the state
copy's `agentengine::`/`agentengine::rt::`/`message_codec_detail::` qualifiers (every remaining line was then
compared by hand), and confirmed by the differential test (§6), whose oracles are verbatim copies of both.

| # | Difference | Recording copy | State copy | Kept by |
|---|---|---|---|---|
| D1 | Delivery marks on encode | writes `"approval"` when non-empty and `"deliver_as_instructions": true` when set, after `"tainted"` | writes neither | `carries_delivery_marks()` |
| D2 | Delivery marks on decode | reads `approval` when a string, `deliver_as_instructions` via `opt_bool` | reads neither; they stay `""`/`false` | `carries_delivery_marks()` |
| D3 | `"kind"` present but not a string | `require()` checks presence only; `as_string()` then throws `std::bad_variant_access` out of the decoder | `contract` error "missing field: kind" | `rejects_non_string_kind()` |
| D4 | `Message::attribution` on encode (removed by ADR-204) | not written | written after `"content"` when present: `{contributor_index, contributor_type}` | `carries_attribution()` |
| D5 | `Message::attribution` on decode (removed by ADR-204) | ignored | read when an object; `opt_u64`/`opt_string` fields | `carries_attribution()` |
| D6 | Error codes | `recording.` + suffix | `rt.message_codec.` + suffix | `error_code_prefix()` |

D6 covers every error the codec returns: `missing_field` (missing `"kind"`, and a `blob_ref` payload without
`"blob_ref"`), `bad_base64`, `bad_role`, `bad_origin`, `bad_media_payload_kind`, `bad_content_kind`. The error
**messages** and `failure_class` (`contract`) were identical in both copies and are unchanged. The recording copy built
"missing field: kind" from `require()`'s `"missing field: " + key`; the text is the same.

Not observable, recorded for completeness: the base64 alphabet constant's name (`base64_alphabet` /
`kBase64Alphabet`, same characters); the state copy's `agentengine::rt::`-qualified internal calls (Gap-15) against the
recording copy's unqualified ones (each resolved to its own copy; the merged code qualifies `message_json::`); the
state copy's `rt::content_item_to_json` forward declaration.

**D3 is a latent defect, kept on purpose.** A recording file (read by `read_chat_call_recording`, hence by
`ReplayChatClient`) whose content item has `"kind": 5` makes the recording decoder throw instead of returning a
`result` error. This change keeps that behaviour, because "no observable change" is the claim being proven and a
throwing decoder is observable. Fixing it (make the recording profile return `recording.missing_field`, like the state
profile) is a one-line follow-up that changes one outcome from "throws" to "error"; §8 lists it.

**Shared behaviours, unchanged** (both copies did the same, so the merged codec does too): the base64 decoder is
lenient (skips `=`, `\n` and `\r` anywhere, drops a trailing partial group, accepts any length) and only rejects a
character outside the alphabet; wrong-typed optional fields fall back to their defaults silently; a non-string
`"origin"` is ignored while an unknown string is an error; numeric fields are cast from `double` to `uint64_t` with no
range check (undefined for negatives and values of 2^64 or more); `Reasoning::producer_chat_client_id` and
`ToolCall::origin`/`provenance` are not encoded; nested `ToolResult` recursion has no depth limit of its own (parsed
input is bounded by `json::parse`'s `max_depth`).

**What the move itself changes.** The codec functions are no longer `inline`, so a caller in another file cannot
inline them; each call is one out-of-line call per message or item, not per token. Error codes are now built at run
time from prefix and suffix instead of being literals; the strings are equal. Name lookup for existing callers is
unchanged: the new functions live in `agentengine::message_json`, which is an associated namespace only of `Profile`,
so no existing one-argument call can find them through ADL. The old `detail` namespaces are removed; nothing in the
tree used them. Two things out-of-tree code could have relied on are gone: the helpers in those public `detail`
namespaces (`recording_detail::base64_*`, `blob_ref_*`; all of `rt::message_codec_detail`), which such code would now
fail to compile against, and header-only use: a program that includes `rt/message_codec.hpp`, `agent_session.hpp` or
`chat_recording.hpp` and calls the codec must link `agentengine::core` (as with ADR-199/200/201), or it gets an
undefined `message_json::` symbol.

## 5. The I3 argument

content.hpp: `approval` is set "ONLY by `AgentSession` when it builds a request, after re-verifying the text against
its host-owned `ApprovedLessonRegistry`; the session clears it on every other item, so no provider, plugin or stored
history can grant it", and `deliver_as_instructions` likewise. These marks tell the serializers to name a fenced block
an approved lesson (ADR-191) or to send it unfenced as plain instructions (ADR-192): they decide how the model is told
to trust text.

The state profile serves durable state that is read back into a live session or workflow — checkpoints, the turn-delta
record, a workflow's pending payloads. That state is stored history, can be written by anyone who can write its store,
and carries model output. If the state decoder read the marks, a crafted or model-influenced store entry would come
back as a message the next request renders as an approved lesson or as plain instructions, which is stored data (and,
through history, model output) deciding a trust presentation — what I3 forbids. So the state profile never reads them.
It also never writes them, so the state format carries no field a reader must know to ignore, and a snapshot of an
in-flight request cannot leak an approval id into a store that outlives the request.

The recording profile keeps them because a recording is evidence, not state: a replayed request must render the same
fences (I5) and the audit must show which approval reached the model (I4). `ReplayChatClient` replays the response side
only and does not feed recorded requests back into a live session (chat_recording.hpp's note on the missing
`chat_request_from_json`), and `AgentSession` clears both marks on every top-level item when it builds a request
(`src/rt/agent_session_core.cpp`), so a mark read from a recording cannot reach a live request as a top-level item.
It does not clear marks on a `ToolResult`'s nested children, and `ReplayChatClient` decodes responses through the
recording profile, so a replayed response can carry nested marks; this is unchanged by this ADR and harmless, because
the serializers and `needs_system_channel_fence` consult the marks only on top-level `role::system` text items.

The `Profile` type is what keeps this from eroding: there is no way to ask for "state with marks", and the
differential and I3 tests fail if the state profile's behaviour moves (§6, positive control).

## 6. Evidence

- **Differential test** (`tests/core/chat/test_message_json_equivalence.cpp`). The oracles
  `tests/support/oracle_chat_recording_codec.hpp` and `tests/support/oracle_message_codec.hpp` are the old codecs
  extracted from `origin/main` (8a73cc9) by script: the recording copy's lines 42-181 and 226-438, and the state
  copy from `namespace agentengine::rt {` to the end. Two mechanical edits only, stated in each banner: the namespace
  (`agentengine::oracle_recording` / `agentengine::oracle_state`), and in the recording oracle 8 internal calls
  qualified `oracle_recording::` (unqualified, ADL also finds the new `agentengine::` wrapper and the call is ambiguous;
  qualification picks what the original unqualified call resolved to). For every input the new public wrappers
  (`agentengine::` for recording, `agentengine::rt::` for state) are compared with the matching oracle: `json::dump`
  byte-identical for encode; for decode, equal values on success, or equal `failure_class`, code **and** message on
  error, or the same exception type. Every encoding is also decoded by every decoder (old-writes/new-reads and back),
  and the direct `message_json::` API is checked against the wrappers.
  - Item corpus: every variant (text, reasoning, media as bytes of lengths 0-7, 64 and 1,000, as URI and as blob_ref
    with sizes up to 2^53, data with and without `schema_id`, tool call, tool result, citation with spans up to 2^53,
    error, custom), each with every origin, `tainted` both ways, `approval` empty / non-empty / unicode, and
    `deliver_as_instructions` both ways; empty strings, a 20,000-character string, unicode and control characters;
    nested `ToolResult` to depth 4 with marks on inner and outer items.
  - Message corpus: every role, attribution absent / present / zeroed / with index 2^40, message ids empty and
    unicode, content mixing the above.
  - Malformed items (87 cases): non-objects; missing, null, numeric, boolean, array, object, unknown, empty and
    wrong-case `"kind"`; missing, unknown and numeric `payload_kind`; `blob_ref` missing, null, a string, wrong-typed
    fields, fractional size; 22 base64 strings (padding in every position, wrong lengths, `\n`/`\r`/tab/space,
    `-_`, non-ASCII, 4,001 characters); bad, empty, wrong-case and numeric origins; wrong-typed fields of every
    variant; `approval`/`deliver_as_instructions` present, empty and wrong-typed; bad children in a `ToolResult` at
    depth 1 and 2; duplicate keys; unknown keys.
  - Malformed messages (24 cases): non-objects, bad/empty/numeric roles, bad content items (including a non-string
    kind and bad base64), attribution as object, empty object, wrong-typed fields, string, null, array, fractional.
  - Wire strings: every role and origin both ways, out-of-range enum values (the `"user"`/`"assistant"` fallbacks),
    and unknown, empty and unicode strings.
  - **Seeded fuzz:** 3 seeds × 1,500 = 4,500 cases. Each builds a random item (depth ≤ 2) or message with random
    marks and attribution, compares both encoders, encodes with one of the old codecs, applies 1-3 random mutations
    (drop a key, retype a value, set a known key to a random value, replace an array element; descending into nested
    objects and arrays), and compares both decoders. Numbers stay in [0, 2^53) because the shared cast is undefined
    outside it (§4).
  - Totals (one run, identical under MSVC and g++): 13,410 encodings and 18,042 decodings compared (15,206 decoded, 2,651 errors, 185 throws), 40,438 checks, 0.66 s.
  - Harness self-control: the test asserts that the two OLD codecs compare as different (marks on encode, codes on
    error, values on decode, throw vs error on a non-string kind), so the comparison demonstrably can fail.
- **I3 test** (same file, `run_i3`). The state profile's encoding of an approved lesson, of a `ToolResult` whose
  child is one, and of a message holding both contains neither `approval` nor `deliver_as_instructions`. Decoding the
  recording encoding (marks present, outer and nested) through the state profile yields `""`/`false` everywhere.
  Controls: the recording profile writes both marks and restores them exactly. Attribution: the state profile
  round-trips it; the recording profile neither writes nor reads it. `static_assert`s pin what each profile carries
  and its code prefix.
- **Positive control.** Run on a scratch copy under g++-14 (the worktree was never edited), rebuilding the codec
  with one profile field changed and re-running the unchanged test:
  - A: the state decoder reads `approval`/`deliver_as_instructions` (the read guard forced true): **2,652 failures**,
    e.g. `decode item state: corpus item #2 [from recording encoding]` for `{"kind":"text",...,"approval":"ack:..."}`;
    the I3 assertions fail too. Exit 1.
  - B: the state encoder writes them: **4,994 failures** (`encode item state: ...`). Exit 1.
  - C: the recording profile rejects a non-string `kind` (the D3 fix): **185 failures**, every one "new=err
    recording.missing_field, old=threw std::bad_variant_access". Exit 1. This is also the exact blast radius of the D3
    follow-up (§8).
  - Reverted: all pass, same totals as above.
- **Builds.** Full MSVC `dev` preset build from a clean build directory (configure, 715 steps, `/W4 /WX`): no warnings, no errors; every compile-fail gate target configured and built.
- **Tests.** `ctest --preset dev` with CI's `-E` list plus `test_external_skill_discovery`: **348/348 pass** (2 skipped as always: the two `*_no_process_creation` probes), including `test_chat_recording_codec`, `test_message_codec_attribution` and the new test.
- **g++.** In WSL Ubuntu 24.04, g++-14.2 `-std=c++23 -Wall -Wextra -Werror`: `src/core/message_json.cpp` (`-O3 -DNDEBUG` and `-O0`) and the new test compile clean, and the test passes at both levels; `chat_recording.hpp`, `message_codec.hpp` and `message_json.hpp` each compile on their own.
- **Lints.** `tools/naming_lint.py` (the one new type, `message_json::Profile`, carries an `ae-naming-lint: allow`),
  `tools/milestone_status_lint.py`, `tools/layering_lint.py` (no new violations, 8 baselined edges remain) and
  `tools/layering_lint.py --self-test`: all pass.
- **Live.** On this branch, with `AGENTENGINE_WITH_HTTPS=ON`, against DeepSeek (`deepseek-flash`, `api.deepseek.com/v1`):
  `test_rt_agent_session_live_multitool_e2e` and `test_rt_agent_session_hitl_live_e2e` (session state, approval
  suspend and resume), `test_eval_trial_driver_live_e2e` and `test_eval_summarizer_live_e2e` (recordings): 4/4 pass,
  every check run, none skipped. Observation only; the equivalence claim rests on the differential test.
- **Link.** `dumpbin /symbols` over all 15 static libraries of the build: none has an undefined reference to a `message_json` symbol (control: the same query finds `agentengine_rt_session`'s reference to `admit_call`), so no library needs a `PUBLIC` link; `agentengine_message_json.lib` defines the 15 external `message_json` symbols. Every consumer reaches it through `agentengine::core`.

## 7. Size

| File | Before | After |
|---|---|---|
| `include/agentengine/core/chat_recording.hpp` | 718 | 428 |
| `include/agentengine/rt/message_codec.hpp` | 445 | 65 |
| `include/agentengine/core/message_json.hpp` | — | 109 |
| `src/core/message_json.cpp` | — | 373 |

Production code: 1,163 lines to 975, and one copy of the codec instead of two. The test adds 826 lines and the two
oracles 809.

## 8. Residuals

- **D3, the recording decoder throws on a non-string `"kind"`** (§4). Kept to make this change behaviour-neutral. A
  follow-up can make `rejects_non_string_kind()` true for both profiles (then remove it); the differential test's
  malformed cases will show exactly which outcomes move from "threw" to "error".
- **The state codec's AG-UI use.** `protocol/agui/projection.hpp` encodes content with the state profile, so AG-UI
  output carries neither delivery mark; unchanged by this ADR, noted because the seam table makes it visible.
- **A host can replace a codec function at link time**, as ADR-201 §7 records for the tool pipeline: host code is
  trusted, and this needs a deliberate exact-signature definition.
- **Remaining base64 copies** (`a2a/types.hpp`, `mcp/client.hpp`, `relay_base64.hpp`) are #120 S8.

## 9. Open decision for the owner: recordings do not carry `Message::attribution` (I4)

**Superseded by ADR-204** (owner decision, 2026-09-27): recordings carry attribution; `carries_attribution()` is removed.
The text below is kept as the question was posed.

The #120 audit noted that a chat-call recording drops `Message::attribution`, so the recorded request of a turn whose
context came from a context provider does not say which provider contributed which message — an I4 ("every effect is
attributable") gap in the audit trail that recordings are meant to be. This ADR does **not** change it; it is a
decision, not a refactor:

- **Adding it** would make the recording profile write (and read) `attribution`, the same omit-when-absent encoding as
  the state profile. That changes the recording file format for every message that carries attribution, and it changes
  `eval_tier1_screen`'s design hash for any prompt that carries attribution, since that hash is taken over
  `message_to_json`'s output (ADR-195 §8; eval_tier1_screen.hpp says so). Prompts without attribution would hash the
  same, because absent attribution is omitted, but any stored design hash over an attributed prompt would stop
  matching. Reading it back in replay also has to be thought through: a recording file would then assert provenance
  for replayed messages.
- **Leaving it** keeps recordings and every design hash as they are, and keeps the gap.
- If added, it would be a change to what `Profile::recording()` carries, not a third profile: the two-profile rule
  stands.

Owner: decide whether recordings should carry attribution, and if so how stored design hashes migrate.

## 10. Red team round 1 (2026-09-27)

An independent pass with its own harnesses (g++-14; the worktree untouched), each clean probe with a positive control:

- **I3.** Every encoding mutated at every object and array position, at every depth, with 34 probe keys (including
  `approval`, `deliver_as_instructions`, `attribution`, `kind`) and 29 wrong values, one a `content` array holding a
  marked child: 1,156,948 decodes, none through the state profile with a mark set, and no state encoding containing
  either key; the recording profile restored marks in about 110,000. Control: the state read guard forced true moved
  112,929 lines. All 10 attempts to build a third `Profile` (`{}`, default, private id, designated init,
  `optional{in_place}`, `construct_at`, an inheriting derived class, ...) fail to compile; copying between the two
  factory values (the control) compiles.
- **Equivalence, independently of this ADR's oracles.** One harness compiled against `origin/main`'s include tree and
  against this branch, over unicode and control characters, embedded NUL, `SIZE_MAX`, 2^53+1, ±1e300, inf, NaN,
  negatives, duplicate keys, empty arrays, a wrong type for every field, 300 base64 corruptions and out-of-range enums:
  1,158,537 output lines byte-identical, including 419,582 errors (code, message, `failure_class`, `native_code`) and
  4,005 D3 throws. Controls: the D3 fix moved exactly 4,005 lines; recording writing attribution moved 76,718. Both
  oracles match `origin/main` exactly once their disclosed edits are reverted, and the test's totals reproduce.
- **Name lookup and link.** Signatures and `noexcept` unchanged; the new overloads are reachable only qualified or by
  ADL on `Profile`; no compiled library references a `message_json` symbol (`nm -u`, control finds 1 in a consumer).
- **Layering.** Lint and self-test pass; injecting an L2 include into the header or the `.cpp` is flagged.
- **Consumers.** `eval_tier1_screen.hpp`'s hash input is byte-identical for the whole corpus, attributed messages
  included; the state-codec call sites are textually unchanged.
- **MINOR — out-of-tree breakage not disclosed (fixed).** §4 now says the public `detail` helpers are gone and that
  header-only use needs `agentengine::core`.
- **NOTE — §5 overstated how marks are cleared (fixed).** Only top-level items are cleared; §5 now says so and why
  nested marks are harmless.
- **NOTE — §3 wording (fixed).** The wire-string functions take no `Profile`.
- **NOTE — link order.** A future compiled library that calls a wrapper needs its own link to the codec (§3 already
  says this).

## Re-check before sign-off (2026-09-27)

On `main` at f37912a: full `dev` build with `/W4 /WX`, no warnings; `ctest -LE live-network` with Docker 29.7.2 running,
363/363 pass (the two `*_no_process_creation` probes skipped as always, `test_external_skill_discovery` excluded).

Controls re-run on `src/core/message_json.cpp`:

- A, the state profile reads `approval`/`deliver_as_instructions`: `test_message_json_equivalence` fails, e.g. `decode
  item state: corpus item #2 [from recording encoding]`.
- B, the state profile writes them: it fails on `encode item state: ...`.

Reverted; the test passes.
