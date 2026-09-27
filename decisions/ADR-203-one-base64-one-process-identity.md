# ADR-203 — One base64 codec and one orphan process-identity check, not four and two copies

- **Status**: **Proposed — design + implementation + proof (2026-09-27); red team round 1 (same day): no BLOCKER or
  MAJOR, 1 MINOR and 2 NOTEs, all addressed (§9).**
- **Date**: 2026-09-27
- **Origin**: GitHub issue #120 (S8, "duplicated helpers"), the structure audit after issue #115 closed. Follows
  ADR-202, which merged two of the five base64 copies into one private copy in `src/core/message_json.cpp`.
- **Touches**: `include/agentengine/core/base64.hpp` (new), `include/agentengine/sandbox/detail/process_identity.hpp`
  (new), `src/core/message_json.cpp`, `include/agentengine/protocol/a2a/types.hpp`,
  `include/agentengine/protocol/mcp/client.hpp`, `src/backends/native_jail/relay_base64.hpp`,
  `include/agentengine/sandbox/docker_execution_surface.hpp`, `include/agentengine/sandbox/containerd_execution_surface.hpp`
  (all reduced to wrappers or using-declarations), `tools/layers.toml` (`core/base64.hpp` is layer V),
  `tests/core/json/test_base64_equivalence.cpp`, `tests/sandbox/execution_surface/test_process_identity_equivalence.cpp`,
  `tests/support/oracle_base64.hpp`, `tests/support/oracle_process_identity.hpp` (new) and the two folders'
  `CMakeLists.txt`.

## 1. The question

Two helpers existed as hand copies:

- **base64**, four copies after ADR-202: the Message JSON codec's (`src/core/message_json.cpp`, private), A2A's
  `a2a::detail::base64_encode`/`base64_decode` (`protocol/a2a/types.hpp`, `raw` parts), MCP's
  `mcp::client_detail::base64_encode` (`protocol/mcp/client.hpp`, the `=?base64?...?=` header-value form; encode only)
  and the native-jail HandleRelay's `native_jail::relay_base64` (`src/backends/native_jail/relay_base64.hpp`, guest
  socket and file bytes across the worker_query wire, used by both the host and the jailed worker).
- **The orphan process-identity check** (ADR-108 §5/§7): `docker_cli_detail::` (`docker_execution_surface.hpp`, POSIX
  and Windows) and `ctr_cli_detail::` (`containerd_execution_surface.hpp`, POSIX only) each defined
  `process_is_alive`, `read_process_start_ticks`, `current_process_start_key`, `process_start_key_for`,
  `ProcessMatch`, `check_process_identity`, `OrphanIdentity` and `parse_orphan_identity`. These decide whether
  `reap_orphans()` destroys a container, and must fail closed: `kUnknown` never destroys.

A fix to either (ADR-108 §5's 32-bit pid bound had to be made in both files) had to be made in every copy. Can each be
one implementation with no observable change?

## 2. Designs considered

- **A. One std-only base64 header in `core/` (layer V) and one shared process-identity header inside the sandbox seam,
  every old name kept as a wrapper or using-declaration (chosen).**
- **B. Put the bodies in a `.cpp`.** For base64: the codec is ~50 lines of code, its callers include header-only
  protocol code and two native-jail TUs, and a compiled library would add a link dependency to all of them for no
  compile-time gain worth having. For process identity: the aim would be to keep `<windows.h>` and the POSIX headers
  out of the public headers, but both headers use them anyway for their own process spawning (`run_capture`,
  `posix_spawn`, `poll`), so nothing would leave them. Rejected for both.
- **C. One base64 decoder.** Rejected: the old copies decode two different ways (§4, B2), and the native-jail relay
  is a sandbox wire whose behaviour on malformed input must not move in a refactor.
- **D. Base64 options (`struct { bool skip_line_breaks; bool stop_at_pad; }`).** Rejected for the reason ADR-202
  rejected free-form profiles: only two combinations exist, and each is named by what it does.
- **E. Leave it.**

## 3. Decision

Design A.

- **`core/base64.hpp`**, namespace `agentengine::base64`, std only, header-inline, layer V in `tools/layers.toml` (so
  the L1 relay and the L4 protocols may both include it). One encoder over bytes, `encode(std::span<std::byte const>)`
  (a `std::vector<std::byte>` or `std::span{p, n}` converts), plus `encode(std::string_view)` for the bytes of text.
  Exactly two decoders, both returning `std::optional<std::vector<std::byte>>`: `decode_lenient` and
  `decode_stop_at_padding` (§4). One decode loop underneath (`detail::decode`, a two-value `PaddingRule`), so the two
  share everything but the one branch where they differ. Neither reports a reason; every caller maps `nullopt` to its
  own old error exactly.
- **Wrappers.** `message_json.cpp`'s private `base64_decode(text, profile)` maps to the profile's `bad_base64`;
  `a2a::detail::base64_decode` to `a2a.bad_base64`; both with the old message "invalid base64 character" and
  `failure_class::contract`. `a2a::detail::base64_encode`, `mcp::client_detail::base64_encode` and
  `native_jail::relay_base64::{encode(ptr, n), encode(vector), decode}` keep their signatures and forward.
  `relay_base64.hpp`'s banner argued for a private copy to avoid coupling the native-jail TUs to the content-model
  codec; a std-only V header has no such coupling, and the banner now says so.
- **`sandbox/detail/process_identity.hpp`**, namespace `agentengine::detail::process_identity`, header-inline, inside
  the sandbox seam (`include/agentengine/sandbox/`, L1), where CONVENTIONS lets `#ifdef _WIN32` live. It holds the
  Docker copy's code for both platforms (the containerd copy is its POSIX half, §4), plus `current_pid()`, and
  `parse_orphan_identity(name, name_prefix)`. The comments of both copies are merged into it.
- **Names kept.** `docker_cli_detail::` using-declares `current_pid`, `process_is_alive`, `current_process_start_key`,
  `process_start_key_for`, `ProcessMatch`, `check_process_identity`, `OrphanIdentity` (and
  `read_process_start_ticks` on POSIX, where it existed); `ctr_cli_detail::` the same without `current_pid` (it never
  had one). `parse_orphan_identity(name)` stays in each as a one-line wrapper passing its own prefix constant
  (`kOrphanNamePrefix` = `ae_des_`, `kOrphanIdPrefix` = `ae_ces_`, both unchanged). No caller changed.

## 4. Behaviour — every difference found, and how each is kept

Found by a line diff of the copies with comments stripped (script, §6) and then line by line by hand, and confirmed by
the differential tests.

### base64

| # | Difference | Copies | Kept by |
|---|---|---|---|
| B1 | Encoder input type | Message JSON and A2A: `vector<byte>`; MCP: `string_view` (chars as `unsigned char`); relay: `(byte const*, n)` and `vector<byte>` | `encode(span<byte const>)`, `encode(string_view)`, and the old wrappers |
| B2 | **Decode rule** | **Lenient** (Message JSON, both predecessors; A2A): `=`, `\n`, `\r` skipped anywhere. **Stop-at-padding** (relay): the first `=` ends decoding and everything after it is ignored unread, even characters outside the alphabet; `\n`/`\r` are rejected like any other non-alphabet character | `decode_lenient`, `decode_stop_at_padding` |
| B3 | Failure report | Message JSON: `result`, `recording.bad_base64` / `rt.message_codec.bad_base64`; A2A: `result`, `a2a.bad_base64`; relay: `nullopt`. Message and class identical where present | each wrapper maps `nullopt` as before |
| B4 | Decoder presence | MCP has no decoder | — |

Not observable: the alphabet's spelling (`base64_alphabet`, `kAlphabet`, a `static constexpr char[]`; same 64
characters), MCP's loop bound (`i + 2 < n` vs `i + 3 <= n`), `push_back` vs `+=`, an `int` vs `uint32_t`
intermediate (values < 2^24), `reserve` sizes (identical formulas). Shared by every copy and unchanged: a trailing
partial group is dropped, any length is accepted, non-zero leftover bits are not checked, so non-canonical encodings
decode (`QUJDR` gives `ABC`).

**Removed names**: `a2a::detail::base64_alphabet` and `native_jail::relay_base64::kAlphabet`; nothing in the tree
used either (`agentengine::base64::kAlphabet` holds the same characters).

### Process identity

| # | Difference | Docker copy | containerd copy | Kept by |
|---|---|---|---|---|
| P1 | Name prefix | `kOrphanNamePrefix` = `ae_des_` | `kOrphanIdPrefix` = `ae_ces_` | `parse_orphan_identity(name, prefix)`; each namespace's wrapper passes its own constant |
| P2 | Windows | `process_is_alive` (`OpenProcess` + `GetExitCodeProcess`), `current_process_start_key` and `process_start_key_for` (`GetProcessTimes` creation time); no `read_process_start_ticks` | none (the header is POSIX-only, gated `NOT WIN32` in CMake) | the shared header has both branches; `ctr_cli_detail` is still only compiled on POSIX, and `read_process_start_ticks` is still only declared there |
| P3 | Own-pid expression in `current_process_start_key` | `current_pid()` | `static_cast<long>(::getpid())` | same value (`current_pid()`'s POSIX branch is that expression) |
| P4 | Parameter name of `parse_orphan_identity` | `name` | `id` | not observable |

All other code — `process_is_alive`'s POSIX branch, `read_process_start_ticks`, `process_start_key_for`,
`ProcessMatch`, `check_process_identity`, `OrphanIdentity`, the body of `parse_orphan_identity` including the
ADR-108 §5 `INT32_MAX` bound — is token-identical between the copies. Comments differ only in wording (containerd's
omit the Windows cases).

**What the merge itself changes** (none is a behaviour change for the tree): `docker_cli_detail::ProcessMatch` and
`ctr_cli_detail::ProcessMatch` (likewise `OrphanIdentity`) used to be two distinct types and are now one type, so
out-of-tree code that overloaded or specialized on both would no longer compile; their associated namespace for ADL is
now `agentengine::detail::process_identity` (every in-tree call is qualified). `docker_execution_surface.hpp` now also
includes `<signal.h>` and `<sys/types.h>` on POSIX through the shared header, and both headers gain one include.

## 5. Security notes

The relay is a sandbox wire: the host side (`native_jail_handle_relay.cpp`) decodes `data_base64` supplied by the
jailed worker, and the worker decodes the host's replies. Its decode rule is now `decode_stop_at_padding`, which is
the old loop with the same branch; the differential test compares it with the old code on every corpus and fuzz input
(§6), and control B1/B2 show that either way of loosening it fails the test. The host still bounds the base64 text
length before decoding (unchanged). The orphan check keeps every fail-closed answer: pid ≤ 0 is "alive" and has no
key (`kUnknown`), an unreadable key is `kUnknown`, and names outside the 32-bit pid range are refused before any
liveness check; controls P1-P4 remove each and the test fails.

## 6. Evidence

- **Oracles, extracted by script** from `aa2573f` (the `codec-merge-s3` head; its tree is identical to `3bb0730`, the
  squash-merge of #127 this branch sits on) with
  `git show`, located by marker lines, copied byte for byte and wrapped in a renamed namespace (the only edit, stated
  in each banner): `tests/support/oracle_base64.hpp` (A2A `types.hpp` lines 116-187, MCP `client.hpp` 143-173,
  `relay_base64.hpp` 22-92) and `tests/support/oracle_process_identity.hpp` (Docker lines 728-734 and 753-929, both
  platforms; containerd lines 298-424, POSIX only). The Message JSON codec's base64 is compared against ADR-202's
  oracles of its two predecessors (`oracle_message_codec.hpp`, `oracle_chat_recording_codec.hpp`), which ADR-202
  proved it equal to.
- **`test_base64_equivalence`** (`tests/core/json/`). Encode: byte strings of every length 0-300 (random under 3
  seeds, all-zero, all-0xFF, counting) and every byte value ×1-3, through the core encoder, the 5 new wrapper paths
  and the 6 old encoders, byte-identical; plus a `media` item through both Message JSON profiles against the ADR-202
  oracles. Decode: 117 hand-built inputs (padding in every position and after data, `\n`/`\r`/tab/space/`\v`/`\f`,
  invalid characters before and after `=`, `-_`, UTF-8, Latin-1, embedded NUL, odd lengths, 4,000-character runs),
  301 round trips with and without a trailing LF, and 12,000 seeded fuzz inputs (random strings weighted toward the
  alphabet, `=` and line breaks, plus mutated valid encodings); each compared for the lenient rule (both old Message
  codecs, A2A's `result` with class, code and message, and a `bytes_base64` item through both Message JSON profiles
  end to end) and the stop-at-padding rule (core and `relay_base64::decode` against the relay oracle).
  **Totals (identical under MSVC and g++): 33,462 encodings and 101,752 decodings compared (15,780 core-decoder
  rejections), 122,502 checks.**
- **`test_process_identity_equivalence`** (`tests/sandbox/execution_surface/`, no daemon). Parse: 19,044 names — 8
  prefixes (right, the other backend's, empty, truncated, upper-case, doubled) × pairs of 33 segments (empty, `0`,
  leading zeros, `INT32_MAX` and +1, `UINT32_MAX` and +1, `LONG64_MAX` and +1, `UINT64_MAX` and +1, 26 digits,
  `+1`, `-1`, `-0`, leading/trailing space, tab, LF, hex, exponent, decimal point, full-width and Arabic-Indic digits,
  embedded NUL), with and without a seq, third and later `_`, empty segments — plus 15,000 seeded fuzz names, through
  each wrapper and the shared function with each prefix, against each oracle. Identity: this process with its own key
  (`kAliveSameProcess`), another key and key 0 (`kGoneOrReplaced`); pid 0, -1, -2, -4096 and `LONG_MIN` (`kUnknown`);
  four pids that do not exist (never `kAliveSameProcess`); pids 1, 2, 4 (someone else's, compared only); a live child
  with its key and another; the child exited but unreaped (a zombie on POSIX, an exited process with an open handle
  on Windows); and reaped. Each compares `check_process_identity`, `process_is_alive`, `process_start_key_for` and on
  POSIX `read_process_start_ticks`, for Docker and (POSIX) containerd. `current_pid` and `current_process_start_key`
  equal their oracles and the key is non-zero. **g++ (POSIX, both backends): 68,088 parses (697 accepted), 38
  identity comparisons, 136,369 checks. MSVC (Windows, Docker): 34,044 parses (358 accepted), 19 identity
  comparisons, 68,184 checks.**
- **Harness self-controls** (in the tests): the old lenient and old stop-at-padding decoders compare as different on
  32 of the 117 corpus inputs, and specifically on data after `=`, junk after `=`, LF and CR (and equal on a plain
  encoding); the Docker oracle and the shared parser given the containerd prefix compare as different on 323 of
  19,044 names.
- **Positive controls.** On a scratch copy (the worktree never edited), each mutation of the NEW code rebuilt with
  g++-14 and the unchanged test run:

  | Control | Result |
  |---|---|
  | B1 stop-at-padding also skips `\n`/`\r` | 2,752 failures, e.g. `'QUJD\x0A' new=ok(3 bytes) old=nullopt` |
  | B2 stop-at-padding skips `=` instead of stopping | 5,027 failures, e.g. `'=QUJD' new=ok(3 bytes) old=ok(0 bytes)` |
  | B3 lenient no longer skips `\r` | 2,800 failures |
  | B4 encoder drops the `=` after a 2-byte tail | 6,848 failures |
  | P1 unreadable key gives `kGoneOrReplaced` (fail open) | 20 failures, e.g. `pid 0 new=kGoneOrReplaced old=kUnknown` |
  | P2 pid bound widened to `UINT32_MAX` (ADR-108 §5 fix reverted) | 240 failures, e.g. `'ae_des_2147483648_0'` accepted |
  | P3 parser accepts pid 0 | 272 failures |
  | P4 `process_is_alive(pid <= 0)` says dead | 30 failures |

  All exit 1; the unmutated and reverted copies pass.
- **Builds.** MSVC `dev` preset from a new build directory (configure + 719 steps, `/W4 /WX`): no warnings, no
  errors. `python_worker_mediation.cpp` (the relay's worker side, built only with the Python runner) syntax-checked
  with `cl /W4 /WX` against the vendored CPython headers: clean. g++-14.2 (WSL Ubuntu 24.04), `-std=c++23 -Wall
  -Wextra -Werror` at `-O0` and `-O3 -DNDEBUG`: the two new headers, the six changed headers, `message_json.cpp` and
  both new tests compile clean and the tests pass; the Linux-only callers `test_containerd_execution_surface.cpp`,
  `test_containerd_isolation.cpp`, `test_docker_orphan_reap.cpp` and `test_execution_surface_image_identity.cpp`
  compile clean.
- **Tests.** `ctest --preset dev` with CI's `-E` list: **350/350 pass** (2 skipped as always: the two
  `*_no_process_creation` probes), including both new tests, `test_message_json_equivalence` and
  `test_chat_recording_codec`.
- **Lints.** `tools/naming_lint.py`, `tools/milestone_status_lint.py`, `tools/layering_lint.py` (no new violations,
  8 baselined edges remain) and `--self-test`: all pass.

## 7. Size

| File | Before | After |
|---|---|---|
| `src/core/message_json.cpp` | 373 | 321 |
| `include/agentengine/protocol/a2a/types.hpp` | 560 | 509 |
| `include/agentengine/protocol/mcp/client.hpp` | 671 | 646 |
| `src/backends/native_jail/relay_base64.hpp` | 92 | 44 |
| `include/agentengine/sandbox/docker_execution_surface.hpp` | 1,870 | 1,712 |
| `include/agentengine/sandbox/containerd_execution_surface.hpp` | 1,047 | 945 |
| `include/agentengine/core/base64.hpp` | — | 121 |
| `include/agentengine/sandbox/detail/process_identity.hpp` | — | 211 |

Production code: 4,613 lines to 4,509; one base64 codec instead of four and one identity check instead of two. The
tests add 707 lines and the oracles 558.

## 8. Residuals and open decisions for the owner

- **The relay decoder ignores everything after the first `=`**, including characters outside the alphabet
  (`QQ==!!` decodes to `A`). Kept, because this change claims no behaviour change. It drops data rather than using
  it, so it is not an escape, but a strict rule (reject non-`=` content after padding) would match the banner's
  "never trust an unparseable frame" posture. Changing it changes what the host accepts from the jailed worker, so it
  is a decision, not a refactor; control B2's harness shows exactly which inputs would move.
  **Resolved by [ADR-205](ADR-205-strict-relay-base64.md) (owner decision, 2026-09-27):** the relay now decodes
  strictly (`base64::decode_strict`, canonical padded base64 only) and `decode_stop_at_padding` is removed.
- **Both decoders accept non-canonical encodings** (a dropped 1-character tail group, non-zero leftover bits). Shared
  by every old copy; unchanged. (For the relay, resolved by ADR-205: `decode_strict` rejects both. `decode_lenient`
  is unchanged.)
- **`parse_orphan_identity` accepts leading zeros** (`ae_des_007_1` is pid 7). Shared by both old copies; harmless
  (the pid is still range-checked and liveness-checked) and unchanged.
- **Thin wrappers kept.** `a2a::detail::base64_*`, `mcp::client_detail::base64_encode` and `relay_base64::*` each
  have in-tree callers; they could be replaced by direct `agentengine::base64::` calls in a follow-up.
- `tools/containerd_shell_chat.cpp` was not compiled by the implementation; the red team compiled it clean with
  g++-14 `-std=c++23 -fsyntax-only -DAGENTENGINE_WITH_HTTPS=1` (§9).

## 9. Red team round 1 (2026-09-27)

An independent pass against old code it extracted itself (`git show 3bb0730:<path>`), each clean probe with a positive
control; the worktree untouched.

- **base64.** 19,571,409 checks per run, 0 failures, under g++-14 (`-O2 -Werror`, and again with `-funsigned-char`)
  and MSVC `/W4 /WX`: every string of length 0-5 over 17 chosen characters (alphabet, `=`, LF, CR, space, tab, NUL,
  0x80, 0xFF, `-_.`), all 65,536 byte pairs alone and around padding, 300,000 random strings; A2A errors compared by
  code, message and class. Encoders: all 1- and 2-byte inputs, 1/17 of the 3-byte space, 100,000 random buffers, empty,
  `relay encode(nullptr, 0)`; all six old/new paths byte-identical. Controls (all fail): stop-at-padding skipping CR/LF
  (103,986), strict after `=` (370,824), lenient skipping space (45,566), the MCP encoder with a signed-char bug
  (884,406), a dropped `=` (98,573), the A2A wrapper hiding errors (1,931,483).
- **Process identity.** Token-identical to both old copies after comments, apart from the disclosed prefix,
  `current_pid()` and the Windows branches (under the same `#ifdef _WIN32`). Parse corpus of 8 prefixes × 33 segment
  kinds (2^31/2^32/2^63/2^64 overflow, signs, whitespace, leading zeros, hex, exponent, full-width and Arabic-Indic
  digits, NUL) × 33 × 7 tails plus 400,000 random names; identity checks on this process, pids 0/−1/`LONG_MIN`/1/2/4,
  a live forked child, the same child as a zombie and reaped: 1,384,062 checks (g++), 922,670 (MSVC), 0 failures.
  Controls: wrong prefix (8,313), widened pid bound (18,932), `kUnknown` treated as gone (20 / 24), pid ≤ 0 dead (20).
  The merged `ProcessMatch`/`OrphanIdentity` types: no `using namespace` of either detail namespace exists, every call
  passes plain `long`/`uint64_t`, both reap sites still destroy only on `== kGoneOrReplaced`.
- **Includes.** The only include removed (`<cstdint>` from `relay_base64.hpp`) returns through `core/base64.hpp`; every
  changed header and a sample of includers compile standalone under g++-14 `-Werror` and MSVC.
- **Layering.** Lint and self-test pass; an injected V→L0 and L1→L4 include are flagged; removing the `base64.hpp = V`
  entry flags two edges.
- **MINOR — wrong base commit named in §6 (fixed).**
- **NOTE — `std::numeric_limits<...>::max()` after `<windows.h>` fails without `NOMINMAX` (fixed).** Pre-existing in
  the Docker header (hidden by the project-wide `NOMINMAX`); now `(std::numeric_limits<std::int32_t>::max)()`, which
  compiles the same call either way.
- **NOTE — `tools/containerd_shell_chat.cpp` compiles (residual closed, §8).**
- **Opinion on the relay decoder (§8).** Not a security issue: the worker already controls every decoded byte, no
  second parser reads the field, and the size cap applies before decoding. Rejecting content after `=` fits the "never
  trust an unparseable frame" posture and is cheap; do it as its own ADR-noted change.
