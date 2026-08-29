# ADR-107 — Does `trust::hmac_sha256()` need a real Linux implementation, and if so, does giving it one accidentally widen ADR-005's deliberately Windows-only cross-process-token scope?

- **Status:** Proposed — implemented, verified against RFC 4231, and independently red-teamed
  (2026-08-29), real builds and real test runs on Linux (WSL2 Ubuntu/gcc). The red-team round found and
  fixed one real bug (an unguarded `size_t` overflow in the new code, latent/unreachable in practice but
  a genuine defect). Full Linux `ctest` (207 tests): 206/207 passed both before and after the red-team
  fix; the one failure is the same pre-existing, already-disclosed, unrelated `test_provider_
  egress_address_policy` finding named in `decisions/ADR-105-*.md` §7. Windows side verified by static
  CMake review only — no MSVC toolchain was available in this session's environment to rebuild it live
  (disclosed as a residual, §7).
- **Date:** 2026-08-29.
- **Scope:** New `include/agentengine/detail/sha256_posix.hpp` (a shared, header-only SHA-256 primitive
  factored OUT of `src/core/worktree_digest_posix.cpp`, not newly invented), new `src/trust/hmac_posix.cpp`
  (the actual HMAC-SHA256 Linux implementation), new `tests/test_hmac_sha256.cpp` (a dedicated,
  portable RFC 4231 regression test, wired on both platforms). `src/core/worktree_digest_posix.cpp`
  refactored (not rewritten) to delegate to the shared primitive. `CMakeLists.txt`: a new
  `agentengine_hmac` static library, built on both platforms, that `agentengine_capability_token`
  (still Windows-only, ADR-005's own scope, unchanged) now links instead of compiling `hmac.cpp`
  directly. `tests/CMakeLists.txt`: `test_hmac_sha256` added; `test_secret_quarantine` and
  `test_rt_agent_session_quarantine_tool` un-gated from `WIN32`-only (now build and pass on Linux too);
  `test_session_builder` given a missing `agentengine::hmac` link dependency (a real, independently
  discovered, platform-agnostic bug it had carried all along, fixed as a one-line addition). Two stale
  comments corrected (`include/agentengine/trust/hmac.hpp`, `include/agentengine/trust/secret_
  quarantine.hpp`) that still described `hmac_sha256` as Windows-only after this ADR made it portable.
  **Excludes**: `agentengine_capability_token`'s other three files (`capability_token.cpp`,
  `capability_registry.cpp`, `bearer_token.cpp`) — these call `BCryptGenRandom` directly for
  cross-process bearer-token key generation, a genuinely Windows-only concern, deliberately NOT ported
  (see §2); a live Windows/MSVC rebuild (no toolchain available, §7); `ContainerdExecutionSurface`/
  ADR-101, `native_jail`/`kata` backends, and anything under `docs/planning/proofs/execution_surface/`
  (a parallel session's active scope, untouched, confirmed via `git status`).
- **Related specs:** `decisions/ADR-005-capability-bearer-tokens-cross-process.md` (the deliberately
  Windows-only design `hmac_sha256` was originally built for; this ADR does NOT widen that design's own
  scope — see §2) · `decisions/ADR-021-inbound-protocol-trust-boundary.md` (Milestone 7, the second
  consumer that first made `hmac_sha256` a shared primitive, extracted out of `capability_token.cpp`
  into `trust/hmac.hpp`/`hmac.cpp`) · `decisions/ADR-068-runtime-secret-quarantine-host-delegated-
  detection.md` (the THIRD, unrelated consumer — `QuarantineSecretStore` — whose own design draft
  already named "a portable/cross-platform digest" as a deferred gap) · `decisions/ADR-105-sandbox-
  tool-provider-composed-linux-parity.md` (§4 point 3 / §7 — found and disclosed, but deliberately did
  NOT fix, the exact gap this ADR closes) · `src/core/worktree_digest_posix.cpp` (the existing,
  Judged, from-scratch Linux SHA-256 implementation this ADR's HMAC construction is built on top of,
  and the file this ADR refactors to share that primitive).

## 1. The question

ADR-105 §7 disclosed, but explicitly declined to fix, a real gap: `agentengine::trust::hmac_sha256()`
(declared in `trust/hmac.hpp`) has an implementation only on Windows (`src/trust/hmac.cpp`, via CNG/
BCrypt), built exclusively inside the `WIN32`-only `agentengine_capability_token` CMake library
(ADR-005's own deliberate scope for cross-process capability bearer tokens). But `trust/secret_
quarantine.hpp`'s `QuarantineSecretStore::quarantine()` — a core, unrelated, turn-boundary secret-
quarantine feature (ADR-068) with no connection to ADR-005's cross-process tokens — also calls
`hmac_sha256()` for its own content-addressed naming. Having zero Linux implementation broke every
`AGENTENGINE_WITH_HTTPS`-gated Linux target that touches `QuarantineSecretStore`: concretely,
`test_session_builder` and `agentengine_sandboxed_shell_chat` both failed to link with an undefined
reference to `hmac_sha256`.

Two questions, not one: (a) does `hmac_sha256` need a real Linux implementation at all, and (b) if it
gets one, does that implementation have to live inside (or alongside) `agentengine_capability_token`,
which would mean either widening that library's scope past ADR-005's own deliberate "Windows only, via
BCryptGenRandom for cross-process token randomness" boundary, or duplicating the HMAC construction a
second time somewhere else.

**Disproof, if "no, don't build this":** `hmac_sha256`'s Linux gap turns out to have some other, cheaper
fix (e.g. `QuarantineSecretStore` could use a different digest primitive entirely) — investigated and
rejected below (§2). **Disproof, if "yes, but it has to live inside `agentengine_capability_token`":**
the other three files in that library (`capability_token.cpp`/`capability_registry.cpp`/`bearer_
token.cpp`) turn out to be Windows-only for reasons unrelated to `BCryptGenRandom` too, making a
Windows-only gate the library's genuine, unavoidable shape — investigated and rejected below (§2, they
are NOT unrelated-Windows-only, they're specifically BCryptGenRandom-only, confirmed by direct read).

## 2. Design

**Reused, not reinvented, the SHA-256 primitive.** `src/core/worktree_digest_posix.cpp` already carries
a self-contained, dependency-free, from-scratch FIPS 180-4 SHA-256 implementation for Linux (Milestone
3 Phase C4, Judged) — the exact primitive HMAC-SHA256 needs internally. Rather than a second,
independent SHA-256 implementation (this project's own standing worry, per `hmac.hpp`'s own top comment
about ADR-021's red-team finding: "a fresh HMAC-comparison implementation is exactly the kind of thing
that silently re-introduces a timing oracle the first time"), the compression-function/padding logic was
factored OUT of `worktree_digest_posix.cpp` into a new private header, `include/agentengine/detail/
sha256_posix.hpp` (`agentengine::detail` — "private internals shared across subsystems," per that
directory's own README and CONVENTIONS.md), exposing one function: `sha256_raw(std::span<std::byte
const>) -> std::array<std::uint8_t, 32>`. `worktree_digest_posix.cpp`'s own `compute_digest()` now
calls this and hex-encodes the result — a mechanical, behavior-preserving refactor, not a rewrite (the
same tables, `process_block`, padding computation, byte order — copied verbatim into the shared
header, not re-derived).

This was a genuine, deliberate choice against two alternatives: (a) leave `worktree_digest_posix.cpp`
untouched and write a second, independent SHA-256 inside `hmac_posix.cpp` — rejected, exactly the
"two unaudited copies of the same primitive" risk `hmac.hpp`'s own history already warns against; (b)
have `hmac_posix.cpp` call `compute_digest()` itself and re-parse its hex-string output back into raw
bytes — rejected as needlessly indirect and lossy-looking (round-tripping through a printable
hex string for an internal byte value is exactly the kind of thing that invites an off-by-one). Factoring
out the raw primitive was judged the smallest change that avoids duplication without altering
`compute_digest()`'s own already-Judged behavior; verified not to have altered it (§4, `test_worktree_
object_store` unchanged and passing, plus additional cross-checked digests both by this session and
independently by the red-team round, §5).

**HMAC-SHA256 itself, RFC 2104 §2, from scratch:**

```
HMAC(K, m) = H((K' xor opad) || H((K' xor ipad) || m))
```

where `H` is SHA-256 and `K'` is `K` zero-padded to the 64-byte block size (or `H(K)` zero-padded, if
`K` is longer than 64 bytes). `derive_key_block()` computes `K'` (branching only on the PUBLIC key
length, never key content); `xor_pad()` builds the ipad/opad-XORed blocks; two calls to `sha256_raw()`
compute the inner and outer hashes. `constant_time_equal()` (RFC 2104-adjacent, actually just an
ordinary XOR-accumulate constant-time byte comparison) was left completely untouched — it was already
portable, platform-independent code, duplicated verbatim between `hmac.cpp` and the new `hmac_posix.cpp`
(same as the Windows file already did, no shared-header extraction attempted for a four-line function).

**Did NOT widen `agentengine_capability_token`'s scope.** Read `capability_token.cpp`, `capability_
registry.cpp`, and `bearer_token.cpp` directly (not assumed, per this session's own brief, which had
originally assumed all three were "already portable" — they are not): all three `#include <windows.h>`
and `<bcrypt.h>` directly, and all three call `BCryptGenRandom` for their own key/token-randomness
generation, a genuinely Windows-specific concern with no Linux equivalent attempted here. Porting those
three files was explicitly out of this ADR's scope (a distinct, larger question — ADR-005's own
cross-process-token design, not this ADR's — and not needed to close the actual gap, since `Quarantine
SecretStore` never touches `capability_token.hpp`/`capability_registry.hpp` at all, confirmed by direct
grep: it includes only `trust/hmac.hpp`). Instead, `hmac_sha256`/`constant_time_equal` were split into
their OWN small CMake library, `agentengine_hmac` — built on both platforms (`hmac.cpp` via CNG/BCrypt
on `WIN32`; `hmac_posix.cpp`, this ADR's own from-scratch RFC 2104 construction, on `NOT WIN32`, no
extra system library, matching `agentengine_worktree_store`'s own established Linux SHA-256 posture) —
and `agentengine_capability_token` (still `WIN32`-only, unchanged in every other respect) now links
`agentengine::hmac` PUBLIC instead of compiling `hmac.cpp` inline. This is the cleanest fix: it makes
the truly-shared primitive (HMAC-SHA256) portable without touching the genuinely-Windows-only code next
to it, and without ADR-005's cross-process-token design gaining a Linux surface it never asked for.

**A missing link dependency, found and fixed as a byproduct.** `tests/test_session_builder.cpp`'s own
B7 case builds a real session against `QuarantineSecretStore` — but `test_session_builder`'s CMake
target had never linked anything that provides `hmac_sha256` at all, on EITHER platform (not just
Linux). This was a real, independent, pre-existing bug (an inline, non-template C++ function is only
emitted into an object file's symbols if it is actually ODR-used in that translation unit — this TU
does ODR-use it, via B7 — so this should have failed to link on Windows too, the first time anyone did
a from-scratch relink of this exact target after `QuarantineSecretStore` support was added to the test).
It had simply never been caught: `AGENTENGINE_WITH_HTTPS` had never been turned on for a from-scratch
Linux configure before ADR-105/106 (that ADR's own finding), and evidently no from-scratch relink had
exercised the Windows side either. Fixed directly (one line: added `agentengine::hmac` to its link
list) rather than merely disclosed, since it required no design decision.

## 3. Falsifiable claims

| # | Claim |
|---|-------|
| C27 | `hmac_posix.cpp`'s `hmac_sha256()` produces byte-identical output to the published RFC 4231 HMAC-SHA-256 known-answer test vectors (Test Cases 1-7, including Case 5's deliberately-truncated 128-bit compare), for vectors transcribed directly from the RFC text, not from memory. |
| C28 | A negative control (temporarily corrupting the implementation) causes these same checks to FAIL — the test suite can actually detect a wrong implementation, not just confirm a right one. |
| C29 | `src/core/worktree_digest_posix.cpp`'s refactor to delegate to the shared `sha256_raw()` primitive did not change `compute_digest()`'s observable output for any input, including inputs that cross the SHA-256 padding-block boundary. |
| C30 | `test_session_builder` and `agentengine_sandboxed_shell_chat` — both previously blocked by the undefined `hmac_sha256` reference — now build, link, and (for `test_session_builder`) pass on Linux. |
| C31 | The HMAC construction contains no branch or memory access conditioned on the VALUE of secret key or message bytes (only on their public lengths) — no new timing side-channel. |
| C32 | This ADR's changes cause zero regressions anywhere else in the Linux test suite. |

## 4. Executed evidence (this session, before the red-team round)

- **C27:** A standalone repro (`rfc4231_kat.cpp`, compiled directly against `hmac_posix.cpp` via
  `g++ -std=c++23`) ran all 7 RFC 4231 HMAC-SHA-256 test vectors, transcribed from the authoritative
  RFC text fetched directly (`https://www.rfc-editor.org/rfc/rfc4231.txt`, not from memory — each
  vector's byte length was verified programmatically, 64 hex chars = 32 bytes, before use, after an
  initial AI-summarized extraction of the same RFC produced a garbled 33-byte value that the raw-text
  read caught). Result: **7/7 PASS**, byte-for-byte, including Test Case 6/7's key-longer-than-block-size
  `K'=H(K)` branch and Test Case 5's published 128-bit truncated compare. Plus 5 additional edge-case
  checks (empty key, empty data, key exactly 64 bytes, determinism, different-keys-differ) — **12/12
  total PASS**. This exact vector set was then also written as the permanent `tests/test_hmac_sha256.cpp`
  regression test (wired into `tests/CMakeLists.txt`, both platforms unconditionally) — built and run
  via the project's own `ctest`, passing.
- **C28:** Temporarily corrupted `hmac_posix.cpp`'s ipad constant (`0x36` → `0x37`) and reran the same
  repro: **all 7 RFC 4231 vectors failed** (5 passed — the platform-agnostic edge checks that don't
  depend on the exact byte value — 7 failed). Restored from a byte-for-byte backup and confirmed via
  `diff` that the restored file was IDENTICAL to the pre-corruption original.
- **C29:** `tests/test_worktree_object_store.cpp` (the existing empty-string/"abc" known-answer test)
  built and passed unchanged after the refactor. (The red-team round, §5, independently extended this
  with 9 more cross-checked digests spanning both padding-block boundaries.)
- **C30:** `cmake --build build-linux --target test_hmac_sha256 test_session_builder agentengine_
  sandboxed_shell_chat test_secret_quarantine test_rt_agent_session_quarantine_tool` — all five built
  and linked cleanly on the first try (WSL2 Ubuntu, `AGENTENGINE_WITH_HTTPS=ON`). Ran each: `test_hmac_
  sha256`, `test_session_builder`, `test_secret_quarantine`, `test_rt_agent_session_quarantine_tool` all
  reported `ALL PASS`/exit 0. `agentengine_sandboxed_shell_chat`'s WIN32 gate was removed (confirmed the
  ONLY reason it was previously Windows-only was `hmac_sha256` — every other dependency, `session_
  builder.hpp`/`composed_context_provider.hpp`/`docker_execution_surface.hpp`/`mandatory_sandbox_
  provider.hpp`/`sandbox_tool_provider.hpp`, was already portable per ADR-103/104/105, and the file
  itself has zero `_WIN32`/`windows.h` references, confirmed by grep) — un-gated to `if(AGENTENGINE_
  WITH_HTTPS)` only.
- **C31:** Reviewed `hmac_posix.cpp`/`sha256_posix.hpp` line by line: `derive_key_block`'s only branch
  (`key_len > kBlockSize`) depends on the PUBLIC key length, never key bytes; `sha256_process_block`'s
  `ch`/`maj`/`s0`/`s1` are fixed bitwise/arithmetic operations with no secret-indexed branch or memory
  access, matching FIPS 180-4's own reference algorithm; `sha256_raw`'s padding/block-count logic
  depends only on `bytes.size()` (public), never byte values. No early-exit comparison anywhere in the
  hash computation itself. (Independently re-verified by the red-team round, §5.)
- **C32:** Full Linux rebuild (`cmake --build build-linux -j12 -- -k`, `AGENTENGINE_WITH_HTTPS=ON`):
  100% built, zero errors (only two pre-existing, unrelated compiler warnings: a `-Wself-move` in
  `test_session_builder.cpp` and a `-Wmissing-field-initializers` in an example file, neither touched by
  this ADR). Full `ctest -j8`: **206/207 passed**. The one failure, `test_provider_egress_address_
  policy`, is the SAME pre-existing, already-disclosed, unrelated finding named in `decisions/ADR-105-*
  .md` §7 (`G3: an octal-encoded address is not a dotted quad...` — an ADR-016 egress-resolver
  discrepancy, structurally unrelated to `hmac_sha256`/`QuarantineSecretStore`) — confirmed via direct
  `ctest -R` re-run showing the identical failure text. (Note: this run's total, 207, includes THREE
  tests newly present on Linux for the first time this ADR added or un-gated — `test_hmac_sha256`,
  `test_secret_quarantine`, `test_rt_agent_session_quarantine_tool` — plus the now-passing `test_
  session_builder`; the total nonetheless matches ADR-105's own 207-test count because that count did
  not include the 4 `test_kata_backend_*_linux` tests this session's default configure did not build
  (`AGENTENGINE_BUILD_KATA_BACKEND=OFF` by default, an environment/config difference from ADR-105's own
  build, not a regression this ADR caused).)

## 5. Red-team round

An independent, fresh `general-purpose` subagent (zero context from the session that made these
changes) was briefed to attack the HMAC-SHA256 implementation specifically — correctness, timing,
edge cases, and whether the `worktree_digest_posix.cpp` refactor silently changed behavior — and given
explicit git-safety instructions after ADR-105's own red-team round had once accidentally discarded an
uncommitted diff via `git checkout --`.

- **Real bug found and fixed**: `hmac_sha256()`'s `std::vector<std::byte> inner_buf(kBlockSize +
  data_len)` computed its size with no overflow guard. GCC's `-Wstringop-overflow=` at `-O2` (not
  visible at the project's default `-O0` dev build) flagged a bogus wraparound-sized `memcpy` bound,
  tracing directly to this unchecked addition — a latent heap-buffer-overflow-class defect. Not
  practically exploitable (`data_len` near `SIZE_MAX` cannot correspond to a real, addressable buffer
  on any actual machine), but a genuine defect worth closing rather than leaving as reasoning-only UB.
  **Fixed**: added an explicit overflow check at the top of `hmac_sha256()` that returns a `contract`-
  class error (`hmac.data_len_overflow`) instead of proceeding, mirroring the Windows sibling's
  error-return style rather than asserting or crashing. Re-verified: `-O2 -Wall -Wextra` now compiles
  clean, `test_hmac_sha256` still passes, and the fix was confirmed byte-identical-output on every real
  input tested (RFC vectors, edge cases, extra Python-cross-checked vectors) before vs. after.
- **RFC 4231 re-derivation, independent of this session's own transcription**: fetched the RFC text
  independently, manually reconciled all 7 numbered vectors against `tests/test_hmac_sha256.cpp` — all
  7 match exactly — then cross-checked all 7 again via Python's own independent `hmac`/`hashlib` stdlib
  implementation (a completely separate codebase from both this project's and the RFC's own published
  values) — all 7 match.
- **10 additional, self-generated vectors**, cross-checked against Python's independent `hmac.new(...)`,
  covering cases RFC 4231 doesn't: a 65-byte key (one byte past the `K'=H(K)` boundary), empty key with
  both a genuine `nullptr` and a non-null-but-zero-length pointer, 200-byte and 1,000,000-byte data, the
  63/64-byte key boundary, and 55/56/64-byte data (the SHA-256 padding-branch boundaries) — **10/10
  match**, both before and after the overflow fix.
- **`compute_digest()` refactor**: `test_worktree_object_store` passed 100% (19/19). Independently
  hashed 9 more inputs (0, 3, 55, 56, 63, 64, 65, 70, 130 bytes — spanning both SHA-256 padding
  boundaries) through the real, built `compute_digest()` (linked against `libagentengine_worktree_
  store.a` directly) and cross-checked every one against `hashlib.sha256` — all matched. The refactor
  did not change behavior.
- **Timing analysis, independently performed**: line-by-line read of `hmac_posix.cpp`/`sha256_posix.hpp`
  confirmed every branch depends only on public lengths, never secret byte values; `sha256_process_
  block`'s `ch`/`maj`/`s0`/`s1` are pure fixed bitwise/arithmetic ops with no secret-indexed memory
  access or branch. Verdict: no timing side-channel.
- **I2/I3 check**: grepped both new files for capability/policy/grant/principal references — none in
  code, only a comment citing ADR-005's unrelated scope. Confirmed no capability/authority decision is
  embedded in this crypto-primitive code.
- **Excluded-directory / git-safety compliance**: confirmed via `git status` that `native_jail`/`kata`/
  `execution_surface` files were untouched, and that no uncommitted work was lost (only read-only `git
  status`/`git diff` commands were run; the one file the red-team round edited, `hmac_posix.cpp`, was
  untracked, so it does not appear in `git diff --stat`, but its content was verified intact and
  rebuilt/retested after the fix).

No other bug, timing issue, or regression found.

## 6. Decision

Give `trust::hmac_sha256()` a real Linux implementation (`src/trust/hmac_posix.cpp`, RFC 2104,
built on the existing `worktree_digest_posix.cpp` SHA-256 primitive, factored into a shared `agentengine
::detail::sha256_raw()` header). Split HMAC-SHA256/`constant_time_equal` into their own small, portable
`agentengine_hmac` CMake library rather than porting `agentengine_capability_token` as a whole — that
library's other three files are genuinely Windows-only (`BCryptGenRandom`), a distinct, larger,
deliberately out-of-scope question belonging to ADR-005's own design, not this one. Add a real, dedicated
RFC 4231 regression test (`tests/test_hmac_sha256.cpp`), wired for both platforms since the vectors
exercise the portable interface itself. Fix the `test_session_builder` missing-link bug found as a
byproduct, since it needed no design decision. Fix the red-team's found integer-overflow defect directly,
since it was unambiguously a real bug with an obvious, low-risk fix.

## 7. Residual risks

- **No live Windows/MSVC rebuild performed.** This session's environment had no MSVC toolchain
  (`vcvars64.bat` not found under the installed Visual Studio 2022 instance) reachable to rebuild
  `agentengine_hmac`/`agentengine_capability_token` on Windows after this ADR's CMake restructuring.
  The change is a mechanical, low-risk static-library split (`hmac.cpp`'s own contents are completely
  unchanged; `agentengine_capability_token` now links `agentengine::hmac` PUBLIC instead of compiling
  the same file inline) — verified correct by hand-tracing the `if(WIN32)`/`if(NOT WIN32)`/`endif()`
  structure in both `CMakeLists.txt` and `tests/CMakeLists.txt` (balanced, no orphaned targets, no lost
  dependencies) — but this is static review, not an executed build, and is named here rather than
  silently assumed. Whoever next has Windows/MSVC access should do a from-scratch Windows configure +
  full rebuild + `ctest` as a follow-up confirmation.
- **`test_provider_egress_address_policy`'s pre-existing `G3` octal-address failure** (ADR-016 residual,
  first surfaced by ADR-105) remains open and unrelated to this ADR — not investigated further here,
  named again for whoever picks up ADR-105's own residual list.
- **The 4 `test_kata_backend_*_linux` tests** were not built in this session's default configure
  (`AGENTENGINE_BUILD_KATA_BACKEND=OFF`, this environment's own default) — not evaluated one way or the
  other by this ADR; ADR-105 already recorded these as environment-caused failures when built.
- **`agentengine_capability_token`'s own three Windows-only files** (`capability_token.cpp`/
  `capability_registry.cpp`/`bearer_token.cpp`, i.e. ADR-005's cross-process bearer tokens and Design B's
  host-side registry) remain entirely un-ported to Linux — deliberately, per this ADR's own scope
  decision (§2), not a gap this ADR leaves silently unaddressed: it is ADR-005's own distinct design
  question, not incidentally blocked by anything this ADR touched.
