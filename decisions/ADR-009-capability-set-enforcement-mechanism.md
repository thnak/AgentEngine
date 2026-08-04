# ADR-009 — How should in-process `Capability`/`CapabilitySet` be represented and enforced so that empty-by-default, attenuation-only, and per-invocation revocation are structural properties, not conventions?

**Resolves:** 007-Capability-and-Trust-Model.md §3's in-process enforcement half (the *types* —
`Capability`, `Principal`, `EffectContext`, the taint mechanism — landed in Milestone 1; this ADR is
what makes `Capability`/`CapabilitySet` do something). Scoped per
[docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md](../docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md)'s
Phase A: this milestone's surface of 007 §9 G1 (no ambient authority), G3 (attenuation), and G4
(revocation) — not the full fuzzed/randomized-workload versions those gates eventually need, and not
007 §5's declarative policy DSL (out of scope for M2 per that breakdown's decision 4) or cross-process
capability transport (already resolved, ADR-005/ADR-006).

**Why this needed the full cycle, not an ordinary task:** the pre-M2 stub's own header comment said
so directly — "the enforcement mechanism is security-critical and, per CLAUDE.md, goes through design
-> red-team -> prove -> judge and an ADR before it is real code, not a header comment." This mechanism
is what makes I2 ("no ambient authority") actually true in-process, not merely asserted in prose.

## 1. The question

The stub `Capability`/`CapabilitySet` (pre-M2) was `struct Capability { capability_kind kind; }` —
no parameters at all, exactly the "hole" 007 §3 property 5 warns against ("`NetOut` with no allowlist
is not a capability, it is a hole"). `CapabilitySet` had zero `grant`/`check`/`attenuate`/`revoke`
behavior. The question: what representation and API surface makes 007 §3's five properties true by
construction —

1. Empty by default.
2. Attenuation only (a derived set is never wider than its parent).
3. Per-invocation binding, revoked after use.
4. Unforgeable in-process (no convenient "grant everything" shortcut).
5. Parameterized, not boolean.

— given that this codebase already has real, non-hypothetical callers doing **kind-only** checks
(`src/backends/native_jail/shell_dispatch.hpp`: "Capability checks are KIND-ONLY per §2.5.4's
downgrade") that must keep compiling and behaving the same way, since M2 is explicitly not the
milestone that rewrites the native_jail spike.

## 2. Background this design must respect

- **007 §3 rule 2 (attenuation only):** never `widen`.
- **007 §3 rule 4 (unforgeable in-process):** "handle types with private construction" — the
  in-process half ADR-006's `SpawnBudget` already established a pattern for (private constructor,
  one named minting entry point).
- **006 §3 steps 7/10:** a capability handle is minted at "bind" and revoked at "account" — the
  per-invocation lifetime is not incidental, it is the RFC's own pipeline shape.
- **006 §8 G3:** "a capability handle from call n is unusable in call n+1 (proven, not asserted)" —
  the sharpest, most concrete falsifiable claim naming exactly what revocation must survive: a
  *retained* handle, not just an expected one.
- Reuse where it already exists rather than reinventing: `AgentCall`'s "depth budget" parameter is
  literally `trust::SpawnBudget` (ADR-006), not a second counter.

## 3. The competing designs

### Design A (accepted) — parameterized variant + checked attenuation + shared-ticket per-invocation revocation

`Capability = std::variant<cap::FsRead, cap::FsWrite, cap::NetOut, ...>` — 16 alternatives, one per
007 §3 table row plus documented RFC-external extensions (`RunnerCall` 010 §1a, `EnvWrite` ADR-001
§2.5), each carrying that kind's real fields. `subsumes(parent, requested)` is a per-kind predicate
(same variant index, and every parameter narrower-or-equal along every axis that kind has).
`CapabilitySet` has exactly one non-empty-producing entry point (`grant_root`, a named static
factory — no public constructor takes a capability list) and two derivation operations:
`attenuate()` (checked narrowing, returns a *new* set, never mutates) and `bind()` (mints a
`BoundCapability` — a per-invocation handle carrying a `std::shared_ptr<InvocationTicket>` with an
atomic `live` flag). `revoke()` flips the ticket; every copy of the handle — however many the
invoked code made — shares the same ticket, so a stashed copy is exactly as dead as the original
after revocation.

### Design B (rejected by design-level reasoning, not built) — whole-set epoch counter

A single monotonic generation counter on `CapabilitySet`, bumped only by macro-level revocation
events (plugin unload, session end); a handle records the epoch at bind time and compares it to the
set's current epoch on every use.

**Why rejected without implementation** (the same move this project's own ADR-001 §5.1 and ADR-003
made for a design whose flaw doesn't need building to see): 006 §8 G3 requires **per-call** expiry —
"unusable in call n+1" — but an ordinary tool call finishing does not bump a whole-set epoch (only
plugin-unload-class events do, by this design's own definition of when the epoch moves). A handle
retained across an epoch that hasn't moved — the *normal* case between call n and call n+1 within
the same still-valid session — would silently continue to pass its epoch check. This is a structural
gap against the milestone's own named gate, not a tuning question, so Design B is rejected on review.

## 4. Falsifiable claims (Design A)

| # | Claim | Disproven by |
|---|---|---|
| C1 | A default-constructed / `CapabilitySet{}` set is empty and `contains()`/`contains_kind()` return false for everything. | Any non-empty default state, or a false positive on an empty set. |
| C2 | `attenuate()` with a requirement subsumed by the parent succeeds, and the derived set reflects exactly the narrower request (not the parent's wider grant). | A narrowing request rejected, or the derived set still satisfying the parent's original wider shape. |
| C3 | `attenuate()` with a requirement NOT subsumed by the parent — on ANY single axis (mount, path prefix, numeric cap, allowlist entry, or kind itself) — fails closed. | Any one axis silently succeeding while the others correctly reject. |
| C4 | Subsumption is real per-kind logic, not kind-only: two same-kind capabilities with incompatible parameters are correctly distinguished. | A same-kind, different-parameter request wrongly accepted. |
| C5 | A `bind()`'d handle is usable while live; after `revoke()`, the original handle AND any copy made before revocation both fail closed. | A stashed copy remaining usable after the original is revoked. |
| C6 | No public API on `CapabilitySet` produces a non-empty set except `grant_root`, and `CapabilitySet` is not an aggregate (no brace-init shortcut around it). | Any zero-argument or convenience path yielding a non-empty set; `std::is_aggregate_v<CapabilitySet>` being true. |

## 5. The red-team attack

Per-claim, hardest where a naive implementation would look correct but isn't:

- **R-C3** (the main attack): a subsumption check that gets mount/path right but forgets the numeric
  cap comparison, or that treats "uncapped request against a capped parent" as fine by omission,
  would still pass a spot check on one axis. `test_capability_enforcement.cpp` tries **six**
  independent widening attempts against the same parent (different mount, broader prefix, a
  non-prefix sibling path, a higher byte cap, an uncapped request against a capped parent, and a
  kind swap between `FsRead`/`FsWrite` on the same mount) — plus a positive control (an exact-match
  request, proving the mechanism isn't just "always reject").
- **R-C5** (the actual point of the shared-ticket design over a naive per-handle flag): a tool
  implementation could copy its `BoundCapability` into a member variable before the call ends and
  try to use the copy later. The test constructs exactly this — `BoundCapability stashed_copy =
  *bound;` before calling `revoke()` on the original — and confirms the copy is dead too, not just
  the original.
- **R-C5b**: revoking one handle must not affect an unrelated handle from a different `bind()` call
  on the same set — proven by binding twice, revoking one, and confirming the other still works.
- **Path-boundary attack** (folded into R-C3): a naive prefix check using `std::string::starts_with`
  alone would incorrectly treat `"/work"` as covering `"/workshop"`. `path_prefix_covers()` requires
  landing on a path-segment boundary; the "non-prefix sibling path" case in R-C3 (`/reports` parent
  vs. `/repo` request) exercises the adjacent failure mode (a prefix of different structure, not just
  a substring).

## 6. Executed evidence

**Windows, MSVC 19.51.36252 (toolset 14.51.36231), Visual Studio 18 2026 generator, x64, Debug,
`-j4`:** full repo build (`cmake --build build --config Debug -j4`, no target restriction) — clean,
zero errors, only pre-existing third-party warnings (Quark's own `C4324`/`C5030`, unrelated to this
change). Full `ctest` suite: **22/22 run tests pass** (1 `test_shell_runner_no_process_creation`
skipped — `llvm-nm` not found, pre-existing and unrelated), including every test file this change
touched (`smoke_vocabulary`, `test_agent_library_manifest`, `test_shell_runner_proof`,
`test_spawn_budget`) and the new `test_capability_enforcement` (27/27 checks pass).

**Windows, MSVC ASan** (`build-asan-006/`, `-DCMAKE_CXX_FLAGS="/fsanitize=address"`, Debug):
`test_capability_enforcement` and `test_spawn_budget` re-built and re-run (ASan runtime DLL —
`clang_rt.asan_dynamic-x86_64.dll`, MSVC toolset `bin/Hostx64/x64` — put on `PATH` for the run, a
known MSVC-ASan requirement, not a code issue): **identical pass, 27/27 and 17/17, zero ASan
findings.** This is the claim that actually exercises `BoundCapability`'s `shared_ptr<InvocationTicket>`
lifetime under a memory-error detector — R-C5's stashed-copy path is exactly the kind of aliasing
ASan would catch if the ticket were owned/freed incorrectly.

**Linux, Docker `ubuntu:24.04`, g++-14.2.0, Debug, `-j4`** (the established M0/M1 substitute for a
real CI run — no git remote exists yet for GitHub Actions to run against): full source tree copied
via `tar` (excluding `build*`/`.git`, per the M0-established fix for the stray-`build/`-directory
pitfall), configured and built clean, `test_capability_enforcement` (27/27) and `test_spawn_budget`
(17/17) both re-run: **identical pass on the second target platform.** `smoke_vocabulary` and
`test_agent_library_manifest` — the two files with the widest call-site changes from this pass —
also rebuilt and confirmed compiling on gcc-14, catching anything MSVC's more permissive/differently-
ordered overload resolution might have let through unnoticed.

UBSan not attempted — no clang toolchain configured on this machine, the same documented gap
ADR-005/ADR-006 already carry.

## 7. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| C1 | **CORRECT** | `test_capability_enforcement.cpp` "C1" block, Windows + Linux |
| C2 | **CORRECT** | "C2" block |
| C3 | **CORRECT** | "R-C3" block, all six widening axes rejected, positive control accepted |
| C4 | **CORRECT** | "C4" block (FsRead mount mismatch, NetOut allowlist mismatch) |
| C5 | **CORRECT** | "C5"/"R-C5" block, including the stashed-copy case; zero ASan findings on the ticket's shared-ownership path |
| C6 | **CORRECT** | `static_assert(!std::is_aggregate_v<CapabilitySet>)` (build-time) + "C6" runtime block |

## 8. The decision

**Accepted.** Design A — the parameterized `cap::` variant, checked `attenuate()`, and shared-ticket
`BoundCapability`/`revoke()` — is the in-process `Capability`/`CapabilitySet` representation for
007 §3. It closes the exact hole the pre-M2 stub's own comments named: OQ-16's "today's placeholder
Capability{kind} can't yet distinguish a /memory-mount grant from any other mount" is resolved (a
`/memory`-mount `FsRead` is now a real, distinguishable `cap::FsRead{"memory", ...}`, not a bare
kind), and the pre-M2 native_jail spike's "Capability checks are KIND-ONLY per §2.5.4's downgrade"
now sits honestly on top of the real representation via `contains_kind()`/`capability_from_kind()`
rather than on a fake unparameterized one.

**A genuine naming collision was found and fixed during this pass, not anticipated in the design:**
`cap::ToolCall` (the capability "may invoke a named tool") collides with `agentengine::ToolCall`
(`core/content.hpp`'s pre-existing message-content-item type for 003's wire shape). Discovered via a
real MSVC build failure (cascading `std::variant` errors inside `content.hpp` once both same-named
types existed in `namespace agentengine`), not by inspection — resolved by moving all sixteen
per-kind parameter structs into a `agentengine::cap` sub-namespace, keeping `Capability`/
`CapabilitySet`/`BoundCapability`/`capability_kind` unqualified in `agentengine` to match their
existing pervasive unqualified use. A second, same-shaped collision (`agentengine::detail` vs.
`agentengine::trust::detail`, surfaced by `tests/test_agent_library_manifest.cpp`'s `using namespace
agentengine; using namespace agentengine::trust;`) was fixed the same way, renaming this header's
internal namespace to `capability_detail`.

## 9. Residual risks and deferred gates

- **This is a unit-level proof of the mechanism in isolation** (matching ADR-006's own scoping
  language) — no test exercises `bind()`/`revoke()` from inside a real 006 §3 ten-step pipeline,
  because that pipeline doesn't exist yet (Phase B, still pending). Integrating this mechanism into
  the real tool-invocation path needs its own verification that step 7/step 10 actually call
  `bind()`/`revoke()` on every call, not just that the type itself is sound — the same caveat
  ADR-006 recorded for `SpawnBudget` and its future `agent.spawn` integration.
- **007 §5's declarative policy DSL is untouched.** This ADR is the mechanical possession/attenuation
  layer only (milestone-2 breakdown decision 4); a rule-matching language over `Capability` is a
  separate, later design.
- **The policy-reachability tool (007 §9 G6)** still needs building (Phase F, flagged separately) —
  this ADR's `subsumes()` is exactly what that tool would need to enumerate against, but building the
  enumerator itself is out of scope here.
- **UBSan not run** (no clang toolchain on this machine) — carried forward as an open gap, not
  silently dropped, same as ADR-005/ADR-006.
- **`agent_library_manifest.hpp`'s gating stayed kind-only** (`contains_kind`) even though real
  mount-scoped gating (e.g. `agent.memory` requiring `FsRead{mount_id="memory"}` specifically,
  matching 026 §5's actual text) is now possible. Left as a named follow-up, not bundled in, to keep
  this ADR's blast radius to the enforcement mechanism itself.
- **`Exec`'s `profile_name` is a string, not `sandbox::sandbox_profile`**, to avoid a
  `capability.hpp` <-> `sandbox.hpp` include cycle (`sandbox.hpp` already depends on this header for
  `CapabilitySet`). Reconciling the two is Phase C's job once `SandboxBackend` needs to consume this
  directly.
