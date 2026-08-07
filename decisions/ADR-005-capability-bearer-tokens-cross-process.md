# ADR-005 — Should a capability that must cross a process boundary be represented as a self-verifying macaroon-style bearer token, or as an opaque reference checked against a host-side registry?

**Resolves:** OQ-3 (OpenQuestions.md), 007-Capability-and-Trust-Model.md §10 Q1, 008-Sandbox-and-
Isolation.md §9 Q4, 018-Identity-Authorization-and-Secrets.md §Q2.

**Scope, deliberately small** (per the project owner's direction to prove this and the remaining
open questions with small experiments rather than full-scale builds): this ADR proves the *token
shape and verification mechanism* — mint, attenuate, verify, forge-resistance, and a first-order
cost comparison. It does not build distributed revocation infrastructure, a real IPC/network
transport, or a second (Linux) HMAC backend. Those are named as residual risks in §9, not silently
assumed solved.

## 1. The question

007 §10 Q1 asks whether capabilities should be representable as bearer tokens (macaroon-style, with
caveats) so they can cross a process boundary — to the `remote` sandbox profile (008), to a remote
plugin, to a delegated A2A call (018) — without each of those paths inventing its own bespoke
authority protocol. The in-process answer already exists and needs no cryptography:
`trust/capability.hpp`'s `Capability`/`CapabilitySet` are unforgeable by construction (private
handle types that never leave the process). The question only exists because that trick stops
working the moment a capability must be *understood by a different process*, which cannot rely on
the minting process's type system.

Stated so it has a wrong answer: **can a receiving process verify a capability's authenticity and
narrowing (attenuation) using only bytes it was handed, with no round-trip back to the party that
granted it — and if it can, is that actually better than the alternative of always asking?**

## 2. Background this design must respect

- **007 §3 rule 2 (attenuation only):** any derivation must produce a subset, never a superset.
  Whatever crosses the boundary must make "widen" structurally impossible, not merely policy-denied.
- **007 §3 rule 4 (unforgeable across the sandbox boundary):** "the boundary itself grants nothing
  the host did not hand it" — a cross-process capability must be at least as strong a claim as the
  in-process one, just enforced by a different mechanism (cryptography instead of the type system).
- **CONVENTIONS' dependency tiers:** `include/agentengine/trust/capability.hpp` is core (std +
  Quark only, forever). A cross-process token needs cryptography that does not belong in core.
  This ADR places the new mechanism under `src/trust/` — a seam-tier location, not core — using
  Windows CNG/BCrypt directly, the same posture `src/backends/native_jail/job_object_limits.cpp`
  already established for Job Objects: a system API is not a third-party dependency, and 021 §2
  makes Windows the only implementation target right now, so a Windows-specific mechanism behind a
  named seam is exactly the right shape (a Linux backend is a second implementation of the same
  three free functions later, not a redesign).
- **022 §5 (positive controls mandatory for security claims):** every claim below that could be
  vacuously true (a check that always passes) is paired with a control that proves the harness can
  detect a real failure.

## 3. Two competing designs

### Design A — self-verifying bearer token (macaroon-style)

`include/agentengine/trust/capability_token.hpp` / `src/trust/capability_token.cpp`.

A `CapabilityToken` carries `{capability_kind, param, caveats[], signature}`. The signature is an
HMAC-SHA256 **chain**: `sig_0 = HMAC(root_key, encode(kind, param))`, then for each caveat in mint
order, `sig_i = HMAC(sig_{i-1}, encode(caveat_i))` — each step's output becomes the *next* step's
HMAC key. Only the minting host ever holds `root_key`; every other party holds tokens only.

- **`mint_root(key, kind, param)`** — only callable by the key holder.
- **`attenuate(parent, caveat)`** — callable by *anyone holding a valid parent token*, no key
  needed. This is the entire point: a receiving process can narrow a token it was handed and pass
  the narrower one onward, with no callback to the minting host.
- **`verify(token, key, request)`** — recomputes the expected chain from `key` and the token's own
  `(kind, param, caveats)`, compares against `token.signature` in constant time, then checks every
  caveat against the request (`ExpiresAt`, `PathPrefix` — the minimal caveat set this small-prove
  scope covers, §9).

**Steelman:** no round trip is ever required to check a token — the verifying party needs only the
bytes in hand and (if it is the original host) the root key. This is exactly what a `remote` profile
crossing a real network wants: a sandbox on another machine can attenuate and re-delegate without a
synchronous callback to the session that granted it. Forgery requires either the root key or an HMAC
preimage/second-preimage, neither of which this ADR's red-team pass (§6) could produce.

### Design B — opaque reference against a host-side registry (status quo)

`include/agentengine/trust/capability_registry.hpp` / `src/trust/capability_registry.cpp`.

No cryptographic token crosses the boundary. `CapabilityRegistry::grant(...)` returns an opaque,
random 128-bit `CapabilityRef` (hex string) that proves nothing by itself. Every use requires
calling back into the registry: `check(ref, kind, path, now)` looks the ref up in a host-side
`unordered_map` under a mutex. Attenuation (`derive_attenuated`) mints a *new* ref with host-enforced
narrower fields — the caller never presents anything but an opaque id, so narrowing is enforced
entirely server-side, not encoded in what crosses the boundary at all.

**Steelman:** this is what "no bespoke protocol" was implicitly settling for before this ADR — no
crypto dependency, no chain-of-custody bugs to get subtly wrong, and it gets one thing Design A
structurally cannot: **immediate revocation** (§6, R-B2). A minted `CapabilityToken` is valid until
its own `ExpiresAt` caveat lapses; nothing about possessing a `SecretKey` lets a host "unmint" a
token already handed out short of tracking every one it ever issued (which is Design B's whole
mechanism, adopted as a patch).

## 4. Falsifiable claims

| # | Design | Claim | Disproven by |
|---|---|---|---|
| A1 | A | A validly minted, unattenuated token verifies against the same key. | `verify` returning an error on a freshly minted token. |
| A2 | A | Attenuating with `PathPrefix`/`ExpiresAt` produces a token that denies requests violating the new caveat while still permitting requests that satisfy it. | A narrowed token still permitting an out-of-scope request, or denying an in-scope one. |
| A3 | A | No bit-flip, field tamper (param, a caveat's payload), caveat-stripping, or caveat-reordering on a valid token produces another token that still verifies. | Any one of these mutations still passing `verify`. |
| A4 | A | A token derived (via `attenuate`) from a fabricated (non-key-derived) parent never verifies, even though `attenuate` itself never checks its input. | A fabricated-parent derivation passing `verify`. |
| A5 | A | `verify` is a pure local computation — no network/IPC call in its implementation. | A round-trip call found in `verify`'s call graph (there is none — inspected directly, §7). |
| B1 | B | An unknown or revoked ref is rejected. | `check` succeeding for a ref never granted, or granted then revoked. |
| B2 | B | `derive_attenuated` cannot produce a ref wider (looser prefix, later expiry) than its parent, even though the caller supplies only the opaque parent ref. | A derived ref passing `check` for a scope the parent didn't cover. |
| B3 | B | Design B supports immediate revocation; Design A does not. | A `CapabilityToken` still verifying after some equivalent of "revoke" is applied to it with no caveat re-check — trivially true by construction, included as a claim so it is falsified honestly rather than assumed. |
| C1 | both | Design A's local verify cost, measured, is lower than Design B's cost **once a real cross-process round-trip is added** to B (not measured here — see §9). | Direct measurement showing otherwise once a real transport is benchmarked. |

## 5. The red-team attack

Both designs were attacked by a party assumed to hold **tokens/refs but never the SecretKey**, and
in Design B's case, to be an unprivileged caller of the registry's public API — i.e., exactly the
trust boundary 007 Q1 is about (`tests/test_capability_token_redteam.cpp`):

- **R-A1** bit-flip one byte of `signature`.
- **R-A2** tamper the signed `param` field post-mint, keep the old signature.
- **R-A3** tamper a caveat's payload (widen `PathPrefix` back toward `"/"`) post-attenuation, keep
  the old signature — the specific "loosen a caveat without detection" attack the HMAC chain exists
  to prevent.
- **R-A4** strip the caveat entirely (truncate `caveats`) while keeping the *attenuated* token's
  signature — the "present the root scope, prove the narrowed one" attack.
- **R-A5** reorder two caveats, keeping the same signature bytes — proves order is part of what's
  signed, not just membership.
- **R-A6** attenuate from a **fabricated** parent (a token with a guessed, not-key-derived
  signature) — proves `attenuate`'s refusal to check its input (by design, it doesn't need to: it
  can't produce anything a downstream `verify` would accept) doesn't create a laundering path.
- **R-B1** present a ref that was never granted.
- **R-B2** present a ref that was granted, verified valid once (setup check), then revoked.
- **R-B3a/b** request a derived ref that widens the parent's prefix or extends its expiry, via the
  registry's own `derive_attenuated` (host-side enforcement, not the caller's declaration).

Each is paired with a positive control in the same file (**R-A0**, **R-B0**: the untampered/
genuinely-granted case still succeeds) and `test_capability_token_proof.cpp`'s **T-C5**
(verification against a deliberately wrong key fails) — proving `AE_CHECK`/`verify`/`check` can
detect a real failure, so a clean run of the attacks above is not vacuous.

## 6. Executed evidence

Built and run this session, MSVC 19.51.36252.0 (toolset 14.51.36231), Visual Studio 18 2026
generator, x64, Debug config, `-j4`:

```
cmake --build build --config Debug -j4 --target
  test_capability_token_proof test_capability_token_redteam test_capability_token_benchmark
```

All three link cleanly against the new `agentengine::capability_token` static library
(`src/trust/capability_token.cpp`, `src/trust/capability_registry.cpp`, linking `bcrypt.lib` — a
Windows system API, gated `if(WIN32)` in `CMakeLists.txt`, unconditionally available like
`job_object_limits`). Full existing suite re-run afterward to confirm no regression:
**19/19 CTest pass** (`test_shell_runner_no_process_creation` skipped — pre-existing, `llvm-nm` not
found on this machine, unrelated to this change).

**Proof pass** (`test_capability_token_proof`): all of T-C0 through T-C5 pass, including the T-C5
positive control (wrong-key verification correctly rejected with `capability_token.bad_signature`).

**Red-team pass** (`test_capability_token_redteam`): all of R-A0/R-A1–R-A6 and R-B0/R-B1–R-B3 pass —
every attack rejected, both positive controls (R-A0, R-B0) succeed. No claim in §4's A3/A4/B1/B2 was
falsified.

**Sanitizer coverage.** No clang toolchain is installed on this machine (`clang-cl`/`clang++` not
found; ADR-001's own recorded path for it no longer exists here) — **UBSan: NOT ATTEMPTED**, and per
CONVENTIONS/the task brief ADR-001 already established, UBSan has no MSVC-native equivalent on
Windows regardless. **ASan was attempted and is clean**: a separate build tree
(`build-asan-005/`, `-DCMAKE_CXX_FLAGS="/fsanitize=address"`, same MSVC toolset) built and ran all
three executables with the MSVC ASan runtime (`clang_rt.asan_dynamic-x86_64.dll`, matching toolset
14.51.36231) — **zero ASan findings**, identical pass/fail results to the unsanitized build.

**Cost measurement** (`test_capability_token_benchmark`, 20,000 iterations each, single-threaded):

| | Unsanitized (Debug) | Under ASan |
|---|---|---|
| `capability_token::verify()` | 5.22 µs/call | 35.54 µs/call |
| `capability_registry::check()` | 0.57 µs/call | 3.24 µs/call |

**This is a real, surprising finding, reported honestly rather than assumed away**: in absolute
local terms, this implementation's `verify()` is **~9× slower** than an in-process registry lookup,
not faster. Root cause, confirmed by reading `hmac_sha256()`
(`src/trust/capability_token.cpp`): it calls `BCryptOpenAlgorithmProvider`/`BCryptCreateHash` fresh
on **every** HMAC invocation rather than caching the algorithm handle, and `verify()` makes one such
call per caveat plus one for the root — for a two-caveat token that's three full BCrypt
provider-open round trips per `verify()`, against Design B's single mutex-guarded hash-map lookup.
This is an implementation-quality artifact of this small-prove pass, not a property of the design —
a cached-handle version was not attempted (out of this ADR's small-prove scope, §9) — but it means
**claim C1 is not demonstrated by this measurement** and must not be asserted from it. What the
measurement *does* establish is narrower and still true: Design B's 0.57 µs/3.24 µs numbers are the
**floor** for that design — real cross-process use adds at least one IPC or network round-trip on
top, which was not measured here (no transport was built, per this ADR's declared scope) — while
Design A's number, however currently inflated by uncached BCrypt handles, requires no such addition
by construction (A5, confirmed by inspection: `verify`'s implementation contains no socket, pipe, or
IPC call of any kind).

## 7. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| A1 | **CORRECT** | T-C1 |
| A2 | **CORRECT** | T-C2, T-C3, T-C4 |
| A3 | **CORRECT** | R-A1 (signature), R-A2 (param), R-A3 (caveat payload), R-A5 (order) |
| A4 | **CORRECT** | R-A6 |
| A5 | **CORRECT** | Direct inspection of `verify()` — no IPC/network call in its implementation |
| B1 | **CORRECT** | R-B1, R-B2 |
| B2 | **CORRECT** | R-B3a, R-B3b |
| B3 | **CORRECT** | R-B2 demonstrates the asymmetry directly: the registry ref is unusable the instant it's revoked; nothing analogous exists for a minted `CapabilityToken` short of waiting out its own `ExpiresAt` |
| C1 | **INCONCLUSIVE** | Measured local cost favors B, not A, in this implementation — but the measurement omits B's real-world round-trip and is confounded by A's unoptimized (uncached) BCrypt usage. Neither "A is cheaper" nor "B is cheaper" is established; §9 names what would resolve it |

`INCONCLUSIVE` is recorded honestly rather than rounded to whichever answer was expected, per
`decisions/README.md`'s own rule.

## 8. The decision

**Design A (self-verifying bearer token) is accepted as the mechanism for capabilities crossing a
process boundary**, for the caveat classes proven here (`ExpiresAt`, `PathPrefix`) — narrowly, not as
a blanket replacement for host-side state everywhere:

- It structurally satisfies 007 §3 rule 2 (attenuation-only, proven adversarially in §6) and rule 4
  (unforgeable across the boundary, via cryptography rather than the type system) with no bespoke
  per-path protocol — directly answering 007 Q1/008 Q4/018 Q2 in the affirmative for the `remote`
  profile and delegated A2A calls those questions named.
- **Design B is not rejected** — R-B2/claim B3 surfaced a real, structural gap in Design A:
  **capabilities that must support immediate revocation should use Design B (or a short-lived
  Design A token re-minted frequently), not a long-lived bearer token.** This is the same shape of
  qualified acceptance as ADR-001 ("Design A accepted... Design B never implemented, not defeated on
  evidence") and ADR-002 ("accepted; narrowed after prove phase") — a real finding narrowing scope,
  not a clean sweep.
- **Claim C1 (performance) is left open**, honestly, rather than asserted in either direction. The
  cost comparison this ADR actually needed — Design A's local floor vs. Design B's floor **plus a
  real round-trip** — was not produced this pass.

## 9. Residual risks and deferred gates

- **C1 is unresolved** (§7/§8): a follow-up measurement needs (a) a cached-BCrypt-handle version of
  `hmac_sha256` and (b) an actual IPC or network transport in front of `CapabilityRegistry::check`
  (even a loopback socket) before Design A's and Design B's costs can be honestly compared.
- **Linux has no HMAC backend.** The token *shape* (§3) is platform-independent; only
  `hmac_sha256`'s BCrypt implementation is Windows-specific. A Linux backend (OpenSSL EVP or
  libsodium, behind the same three free functions) is required before 021 §6 G1's cross-platform
  parity gate can include this mechanism — consistent with 021 §2's Windows-now/Linux-next
  sequencing; this is explicitly not urgent yet.
- **Caveat vocabulary is minimal by design** (§2 scope note): only `ExpiresAt` and `PathPrefix` are
  proven. A real deployment needs more (byte quotas, host allowlists for `NetOut`, ...) — the
  mechanism generalizes (any caveat is `encode() -> bytes` folded into the chain), but each new
  caveat kind needs its own red-team pass before being trusted, per this project's own demonstrated
  history (007 §3, and ADR-003 §9's explicit warning that this class of mechanism has repeatedly had
  entry points missed on first pass) — do not assume a new caveat is safe by analogy to these two.
- **`PathPrefix` does its own prefix check only; it does not canonicalize.** 021 §4's "paths are a
  type, not a string" is not yet satisfied by this caveat — using it against real filesystem paths
  before 025's `Path` type exists risks the exact class of `..`/symlink/case-folding escape 021 §6
  G3 is a gate for. Flagged in the header, not solved here.
- **No side-channel analysis of BCrypt itself, no gadget-chaining analysis of the HMAC chain
  construction beyond what §6's red-team covered**, and no analysis of key provisioning/rotation
  lifecycle (where does `SecretKey` live for the process's whole lifetime, how is it rotated) — all
  out of this small-prove's scope and not to be assumed solved by this ADR.
- **No distributed revocation infrastructure** — explicitly out of scope per this ADR's declared
  scope (§0) and the project owner's direction; B3/§8 name the interim answer (use Design B, or
  short-lived Design A tokens) rather than deferring the question silently.
- **No standing leak-lifecycle instrumentation for cross-process capability handles.** Design B's
  host-side registry (§3) can accumulate entries for handles a remote peer never releases, and
  nothing in this ADR specifies a way to see that happening short of an ad hoc investigation.
  Cloudflare Computer's capnweb RPC boundary — a comparable cross-process capability-stub problem —
  answers this with a concrete, cheap pattern: an opt-in live counter per capability class
  (`CAPNWEB_TRACK_STUBS=1`), a debug endpoint exposing it, and a dedicated soak test asserting no
  unbounded growth under sustained load (`docs/research/2026-08-06-cloudflare-computer-vfs-sandbox-comparison.md`
  §3). Worth adopting the same shape for `CapabilityRegistry` before the `remote` profile (008 §3)
  ships — a live entry-count counter plus a soak harness — rather than inventing a different
  mechanism; not solved here.
