# ADR-205 — The HandleRelay wire decodes base64 strictly

- **Status**: **Proposed — design + implementation + proof (2026-09-27); red team not yet run.**
- **Date**: 2026-09-27
- **Origin**: project-owner decision (2026-09-27) on the question ADR-203 §8 left open, following ADR-203 §9's red-team
  opinion ("not a security issue … rejecting content after `=` fits the 'never trust an unparseable frame' posture and
  is cheap; do it as its own ADR-noted change").
- **Touches**: `include/agentengine/core/base64.hpp` (`decode_strict` added, `decode_stop_at_padding` removed),
  `src/backends/native_jail/relay_base64.hpp` (`decode` now strict; header comment),
  `tests/core/json/test_base64_equivalence.cpp` (the relay comparison now expects strict),
  `tests/core/json/test_base64_strict.cpp` (new) and its `CMakeLists.txt`, ADR-203 §8 and its README row (pointers),
  `docs/planning/jailed-python-worker-slice-2-handle-relay-design-draft.md` §2 item 2 (one note).

## 1. The question

The native-jail HandleRelay carries guest socket and file bytes as base64 in the `data_base64` field of
`worker_query` frames: the jailed worker sends `connect_send` / `file_write` payloads to the host, and the host sends
`connect_recv` / `file_read` results back. Since ADR-203 the relay decoded with `decode_stop_at_padding`, the rule its
old private copy had: decoding stops at the first `=` and everything after it is ignored unread, a trailing partial
group is dropped, any length is accepted and leftover bits are not checked. So `QQ==!!garbage` decoded to `A`, and
`QQ`, `QUJ` and `QR==` were all accepted. A malformed frame was partly accepted instead of rejected. Should the relay
reject it?

## 2. Decision

**Yes (owner, 2026-09-27).** The relay accepts canonical padded base64 and nothing else; any other `data_base64` is a
malformed frame, rejected whole, in both directions. It is a new third rule in the one codec, `base64::decode_strict`;
`relay_base64::decode` calls it. `decode_lenient` (the Message JSON codec and A2A `raw` parts) is unchanged: those are
protocol-facing and out of scope.

`decode_stop_at_padding` had exactly one caller, `relay_base64::decode` (checked by a whole-tree search:
`include/`, `src/`, `tests/`, `tools/`), so it is **removed**, with its loop branch (`PaddingRule::kStop`). `core/base64.hpp`
still has exactly two decoders; the old rule survives only as the test oracle ADR-203 extracted
(`tests/support/oracle_base64.hpp`), which the equivalence test compares against.

## 3. The rule

RFC 4648 §4, padded, with no line breaks. `decode_strict(text)` returns bytes exactly when `text == encode(b)` for
some bytes `b`, and then returns `b`; otherwise `nullopt`, never a partial result:

1. `text.size()` is a multiple of 4. The empty string is valid and decodes to no bytes.
2. Every character is in the standard alphabet (`A-Z a-z 0-9 + /`), except that the last one or the last two may be
   `=`. So: no line breaks or other whitespace, no URL-safe `-_`, no NUL or non-ASCII.
3. Nothing follows the padding, and there are at most two `=`.
4. **Non-zero leftover bits are rejected.** One `=` leaves 2 unused bits in the last data character, two `=` leave 4;
   they must be zero (`QQ==` is accepted, `QR==` is not; `QUI=` is accepted, `QUK=` is not).

Rule 4 is taken because every legitimate producer is canonical (§4): `encode` builds the last character from a value
whose unused low bits are shifted-in zeros. With rule 4 the accepted set is exactly `encode`'s image, so each byte
string has one and only one wire form. That is what the tests check: of all 4,096 `ab==` strings exactly 256 are
accepted and of all 262,144 `abc=` strings exactly 65,536, one for each 1- and 2-byte string. Without rule 4, 16 and
4 distinct wire strings would decode to each 1- and 2-byte tail, which no producer uses.

Implementation: `decode_strict` checks the length, strips one or two trailing `=`, and runs the shared decode loop
under a new `PaddingRule::kRejectWithZeroTail`, in which `=` (like `\n`, `\r` and every other non-alphabet character) is
rejected and the leftover bits must be zero. `kSkipWithLineBreaks` (lenient) is unchanged: it still skips `=`/`\n`/`\r`
and checks no leftover bits.

## 4. Producer audit — every legitimate producer is canonical

Every writer of a `data_base64` field in the tree (whole-tree search for `data_base64`, and for `base64`, `b64`,
`b64encode`, `binascii` under `src/backends/native_jail/` and the embedded worker bootstrap source):

| Direction | Producer | What it emits |
|---|---|---|
| host → worker | `native_jail_handle_relay.cpp` `dispatch_file_read` | `relay_base64::encode(buf, n)`; `""` on `ERROR_HANDLE_EOF` |
| host → worker | `native_jail_handle_relay.cpp` `dispatch_connect_recv` | `relay_base64::encode(buf, n)`; `""` on `would_block` |
| worker → host | `python_worker_mediation.cpp` `Internal_file_write` | `relay_base64::encode(Py_buffer)` |
| worker → host | `python_worker_mediation.cpp` `Internal_connect_send` | `relay_base64::encode(Py_buffer)` |

`relay_base64::encode` is `base64::encode`, whose output is canonical by construction (padded to a multiple of 4,
`=` only at the end, unused bits zero; the round-trip and last-quantum tests in §6 confirm it) and never contains a
line break. `""` is `encode` of no bytes. **There is no Python-side producer**: the worker bootstrap's `_AeRelayFile`,
`_ae_send` and `_ae_recv` pass `bytes` to the C++ `_ae_internal.*` functions, which encode; no Python code in the tree
uses `base64` or `binascii` (no `.py` file mentions either; the only other hits are the module names in three tests'
import allowlists). The
Linux native-jail backend has no relay. So no legitimate frame changes meaning.

A missing or non-string `data_base64` reads as `""` (`wp::get_string`'s fallback) and still decodes to no bytes, as
before.

## 5. What each caller does on a rejected frame (unchanged mapping)

- **Host** (`native_jail_handle_relay.cpp`, `dispatch_connect_send` and `dispatch_file_write`). The existing length cap
  runs first (`kMaxRelayBase64Chars`, `ceil(1 MiB / 3) * 4`, itself a multiple of 4, so the largest legal frame still
  passes). On `nullopt` the call returns `deny("net.socket_closed", "malformed base64 in connect_send|file_write
  payload")` as its `worker_query_response`: nothing is sent or written (no partial data), the socket or file stays in
  `live_sockets` / `open_files`, and neither the worker nor the session is torn down. The worker maps
  `net.socket_closed` to `OSError`, so the guest's `send()`/`write()` raises. Fail closed, per call.
- **Worker** (`python_worker_mediation.cpp`, `Internal_connect_recv` and `Internal_file_read`). On `nullopt` it raises
  `RuntimeError("internal error: malformed base64 in relayed recv() data" / "... file read data")` to the guest and
  returns no bytes. The host is the only producer here, so this fires only on a host bug or a corrupted frame.

Before this change, a frame the old rule accepted partly (`QQ==!!garbage`) made the host send or write the partial
bytes (`A`) and report success; it is now denied. The only way to produce such a frame is a worker that does not use
its own encoder, i.e. a compromised jailed process, which is exactly the case the host must not trust.

## 6. Evidence

- **`test_base64_equivalence`** (ADR-203's, updated). The relay comparison now checks, on every input (the 127-entry
  corpus, now including `QR==`, `QUK=` and their canonical neighbours; 602 round trips with and without a trailing LF;
  12,000 seeded fuzz inputs): `relay_base64::decode == decode_strict`; anything strict accepts re-encodes to itself;
  where the old relay decoder rejected, strict rejects; where it accepted, strict either gives the same bytes (and the
  input is canonical) or rejects an input that is non-canonical both by an independent classifier written from §3 and
  by `encode(old result) != input`. Each class of newly rejected input must occur. Result (MSVC and g++ identical):
  **886 same bytes; 7,515 rejected by both; newly rejected: content after `=` 2,528, bad length 1,753, misplaced `=` 13,
  non-zero leftover bits 34; canonical inputs newly rejected: 0; nothing the old decoder rejected is accepted.** The
  lenient comparisons and all encoder comparisons are unchanged and pass (128,689 checks in all). Self-controls: the old
  lenient and old relay rules still differ on 32 of 127 corpus inputs; the old relay rule accepts and strict rejects
  each of `QQ==!!garbage`, `QQ==QQ==`, `QQ`, `QUJ`, `QR==`, `QUK=`, `Q===`; both reject `QUJD\n`; both agree on `QUJD`.
- **`test_base64_strict`** (new, std only). Reference written from the rule's definition, not the decoder's code:
  `reference(s)` = `decode_lenient(s)` if re-encoding it gives `s`, else reject. (1) Round trips through `encode →
  decode_strict`: every length 0-64 (zeros, 0xFF, a counting pattern, 3 random seeds), every 1- and 2-byte string
  (65,792), 67 random buffers up to 64 KiB including 65,534-65,536. (2) The last quantum exhaustively: every `ab==` and
  `abc=` over the alphabet, accepted iff the leftover bits are zero, exactly 256 and 65,536 accepted, each re-encoding
  to itself. (3) Every string of length 0-4 over a 14-character pool (`A Q g w / + 8 = LF CR space ! NUL 0x80`) and of
  length 5-8 over `A Q = !`, against the reference. (4) A 64-entry malformed corpus (content after padding including
  `QQ==!!garbage`, bad lengths, misplaced `=`, line breaks, whitespace, URL-safe, NUL, non-ASCII, non-zero leftover
  bits, and a 64 KiB encoding followed by `!`, LF or `=`, truncated by one, with `=` in the middle, or padded and
  followed by junk), each rejected by both strict and the reference. (5) 200,000 seeded fuzz inputs (random text
  weighted to the alphabet, `=` and line breaks; mutated encodings) against the reference. **82,078 accepted, 312,646
  rejected, 726,822 checks, 0 failures.**
- **Positive controls.** On a scratch copy (the worktree never edited), each mutation of `decode_strict` rebuilt with
  g++-14 and both unchanged tests run; every one fails both tests:

  | Control | `test_base64_strict` | `test_base64_equivalence` |
  |---|---|---|
  | C1 accept content after `=` (everything from the first `=` on ignored, the old relay behaviour) | 36,450 failures, e.g. `'=AAA'` accepted | 531 failures, e.g. `'===='` accepted |
  | C2 no leftover-bits check | 401,611 failures, e.g. `'AAB='` accepted | 71 failures, e.g. `'QR=='` accepted |
  | C3 no length check | 10,282 failures, e.g. `'A'` accepted | 1,175 failures, e.g. `'='` accepted |

  All exit 1; the unmutated copy passes.
- **Builds.** MSVC `dev` preset (new `build-dev`, `/W4 /WX`): configure + 721 steps with `-j 2`, no warnings, no errors. `python_worker_mediation.cpp` (the relay's
  worker side, built only with the Python runner) syntax-checked with `cl /W4 /WX /Zs` against the vendored CPython
  headers: clean. g++-14 (WSL Ubuntu 24.04), `-std=c++23 -Wall -Wextra -Werror` at `-O0` and `-O3 -DNDEBUG`:
  `core/base64.hpp` and `relay_base64.hpp` standalone, both tests (with `message_json.cpp`) compile clean and pass.
- **Tests.** `ctest --preset dev -j 2` with CI's `-E` list (plus `test_external_skill_discovery`): **351/351 pass** (2 skipped as always: the two `*_no_process_creation` probes), including `test_base64_equivalence`, `test_base64_strict` and the native-jail tests the default build has (`test_native_jail_backend_windows`, `_concurrency_windows`, `_abuse_corpus_windows`, `_ambient_authority_windows`, `_parity_windows`, `_teardown_cycles_windows`, `_grant_ro_path_once_windows`, `_grant_path_ace_lifecycle_windows`, `test_native_jail_runner_stubs`). **The relay end to end:** a separate build (`-DAGENTENGINE_BUILD_PYTHON_RUNNER=ON` against the vendored CPython 3.13.5, Ninja, Release, `/W4 /WX`, which compiles `python_worker_mediation.cpp` into the real `agentengine_python_worker.exe`) ran `test_native_jail_python_worker_handle_relay` (SOCK-1: real bytes through `connect_send`/`connect_recv` both ways against a loopback TCP echo peer; SOCK-DENY; FILE-RO-1 through `file_write`), `test_native_jail_python_worker_slice1` and `test_native_jail_session_boundary_windows`: **3/3 pass**, so canonical frames still work in both directions under the strict rule.
- **Lints.** `tools/naming_lint.py`, `tools/milestone_status_lint.py`, `tools/layering_lint.py` (no new violations, 8 baselined edges remain) and `--self-test`: all pass.

## 7. Residuals

- **No test feeds a raw malformed frame through the host's dispatch path.** `dispatch_connect_send` /
  `dispatch_file_write` are private statics of `NativeJailBackend`, and the guest cannot put raw base64 on the wire
  (the worker's C++ encodes whatever `bytes` it is given), so a test would need a new test-only seam into production
  code. The host calls exactly `relay_base64::decode`, which the equivalence test proves is `decode_strict` on every
  input, and the caller's `nullopt` branch is unchanged. The real relay test (§6) shows canonical frames still pass both ways.
- **`decode_lenient` still accepts non-canonical input** (data after `=`, line breaks, partial groups, non-zero
  leftover bits) for the Message JSON codec and A2A. Unchanged and out of scope; tightening those is a protocol
  question, not a relay one.
- **The rejection's error code is `net.socket_closed`** (pre-existing mapping, unchanged): the guest sees `OSError`
  with the message "malformed base64 in … payload". A dedicated code would be more precise but changes the worker
  protocol; not done.
- **Red team not yet run.**
