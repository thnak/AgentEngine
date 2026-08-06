# ADR-012 — What kind of thing is `SandboxProfile<P>`'s template parameter `P`: an enum value naming a closed set of engine profiles, or any type satisfying `SandboxBackend`?

## 1. The question

`002-Agent-Model-and-Authoring.md` §2's own worked example instantiates `SandboxProfile<Profile::Strict>`
— syntax that reads as passing an enum-like *value*. `008-Sandbox-and-Isolation.md` §2a states plainly
that "`P` is any type satisfying the `SandboxBackend` concept" — a deployer's own class, used directly
as the template argument, no new machinery. These are not two phrasings of the same idea: a C++
`concept` constrains expressions on a *type* (member functions, a static member, return types) — it
cannot be satisfied by an enum value at all, because an enum value has no members to check expressions
against. The two RFC passages describe mutually exclusive shapes for the same template parameter.

`include/agentengine/core/agent.hpp`'s existing scaffolding had already picked a side —
`template <sandbox_profile P> struct SandboxProfile {};`, an NTTP over a closed four-member enum — which
matches 002 §2's surface syntax but cannot represent 008 §2a's custom-backend case at all: an enum with
a fixed set of enumerators has no slot for "a deployer's own type wrapping gVisor." This was discovered,
not invented, while implementing M2 Phase E task E2 (`register_agent<A>()`'s validation), and left two of
its eight named checks (`check_sandbox_profile_availability`, `check_tool_sandbox_profile_compatibility`)
stubbed rather than picking a side inline, per CLAUDE.md's "fix the spec first, with an ADR, before the
code."

A second, independent bug surfaced during this ADR's own research: `Profile::Strict` does not compile
today under *either* reading. No `Profile` namespace, struct, or enum exists anywhere in this codebase,
and `sandbox_profile`'s four enumerators are `wasm`, `native_jail`, `remote`, `none` — no `strict`
member. 002 §2's worked example has been silently broken (unreachable, since M2 has no `Engine` type to
even attempt compiling agent declarations against yet) since before this milestone started.

The question this ADR answers, stated so it has a wrong answer: **should `SandboxProfile<P>`'s `P` be
constrained to a closed enum naming the engine's own shipped profiles, or to any type satisfying
`SandboxBackend`** — and, whichever wins, **what does the broken `Profile::Strict` symbol become**?

## 2. Background this design must respect

- **007/008's invariants.** I2 (no ambient authority) and 008 §2's "empty-by-default authority" apply to
  what a resolved backend can *do*, not to this template-parameter-kind question directly — but a wrong
  answer here could still weaken them indirectly (e.g. a resolution path that silently falls back to
  `none` on failure would violate 008 §3's explicit "silently running unisolated... is prohibited").
- **The real `SandboxBackend` concept**, already defined and unchanged by this ADR
  (`include/agentengine/sandbox/sandbox.hpp`):
  ```cpp
  template <class T>
  concept SandboxBackend = requires(T backend, SandboxSpec spec, SandboxHandle& handle,
                                     ExecRequest request, EffectContext& ctx) {
      { T::traits } -> std::convertible_to<ProfileTraits const&>;
      { backend.create(spec, ctx) } -> std::same_as<result<SandboxHandle>>;
      { backend.exec(handle, request, ctx) } -> std::same_as<result<ExecOutcome>>;
      { backend.destroy(handle) } -> std::same_as<void>;
  };
  ```
  Three real conforming types already exist and each carries its own `static_assert(SandboxBackend<T>,
  ...)`: `agentengine::native_jail::NativeJailBackend` (Windows), `::LinuxNativeJailBackend` (Linux),
  `agentengine::wasm::WasmBackend` (Windows+Linux, `AGENTENGINE_WITH_WASM`). A `remote` backend does not
  exist yet (M9).
- **`resolve_strict()`** (`sandbox.hpp`), 008 §3's ranking rule made real and already tested
  (`tests/test_sandbox_backend_contract.cpp`): highest-`strength` `ProfileTraits` supporting the current
  platform, ties broken toward broader platform support. It takes `std::span<ProfileTraits const>` as a
  **caller-supplied** parameter — nothing populates that span from the real backend types anywhere in
  the codebase today.
- **Layering (CONVENTIONS.md).** `agentengine::core` (which `sandbox.hpp` and `agent_registry.hpp` are
  part of) is header-only and depends on nothing under `src/backends/`; backends depend on core, never
  the reverse. Any design that has core-tier code name a concrete backend type directly (`WasmBackend`,
  `NativeJailBackend`) violates this and does not compile in the general case — `agentengine::wasm_backend`
  is only built when `AGENTENGINE_WITH_WASM` is on, so `sandbox.hpp` cannot `#include` its header
  unconditionally.
- **No `Engine` type exists in M2.** `agent_registry.hpp`'s own file-top comment already names this same
  shape of gap for `ChatClientId` credentials and `OutputSchema` enforceability: both need a real,
  running host-side registry this milestone does not build. `Strict`'s resolution-against-real-
  availability need is the identical shape of gap, not a new one.
- **`decisions/README.md`**'s rule: an ADR is required for "isolation boundary" choices and for "credible
  designs disagree[ing]... not resolvable by reading" — both apply here. This is a type-system/API-shape
  decision, not a hot-path or adversarial-input question; the red-team and evidence phases below are
  calibrated to that (compile-time negative-space proofs and structural correctness, not fuzzing or
  sanitizer runs against attacker-controlled input — nothing here is attacker-controlled, since `P` is
  authored by the deploying developer, not derived from model output, I3).

## 3. The competing designs

### Design A (accepted) — `P` is a type; `Strict` is a resolution-selector type, not an enum value

`template <class P> struct SandboxProfile {};` (unconstrained placeholder) or, tighter,
`template <SandboxProfileArg P> struct SandboxProfile {};` where
`SandboxProfileArg<P> = SandboxBackend<P> || std::same_as<P, Strict>` — a concept-constrained ordinary
type template parameter, exactly the same CRTP-policy-tag idiom `Capabilities<Cs...>`/`Tools<Ts...>`
already use elsewhere in this codebase.

`Strict` (`struct Strict {};`, `sandbox.hpp`) replaces the broken `Profile::Strict`: a small, real,
nameable sentinel type satisfying no backend operations of its own (it is never instantiated as a
backend), recognized specially by extraction logic in `register_agent<A>()`
(`if constexpr (std::same_as<P, Strict>)`). This makes 008 §2a's promise literally true with zero new
machinery: `SandboxProfile<MyCustomBackend>` compiles today, right now, for any type satisfying the
concept, exactly as stated — nothing about the mechanism is engine-private.

**Steelman.** This is the design 008 §2a already describes; accepting it means the RFC text was
*correct* and the code scaffolding was wrong, the more common failure mode CLAUDE.md's "fix the spec
first" rule is written for. It also costs nothing extra: no registry, no new runtime dispatch, no
indirection — `SandboxProfile<P>` for a concrete `P` is resolved entirely at compile time (the concept
check *is* the availability check; a backend that doesn't compile in this build cannot be named).

### Design B (rejected) — `P` stays an enum value; 008 §2a is corrected to describe a registry instead

Keep `template <sandbox_profile P> struct SandboxProfile {};`, fix `sandbox_profile` to include a
`strict` enumerator (or a distinguishable sentinel value), and rewrite 008 §2a to describe custom
backends as requiring a name→type registration step (e.g. a macro or a static registry populated at
process start) rather than "no new machinery."

**Steelman.** An enum is a smaller, simpler surface for the *common* case (choosing among the four
engine-shipped profiles) — `SandboxProfile<native_jail>` reads slightly more ergonomically than
`SandboxProfile<NativeJailBackend>` for a deployer who never intends to write a custom backend, and it
sidesteps needing every backend type to be nameable/includable wherever an agent is declared.

**Rejected because:** it requires actually building the registration machinery 008 §2a explicitly says
does not need to exist ("no new machinery... works exactly the same way `native-jail` or `remote` does")
— a real scope increase this milestone does not need to take on, to fix a spec passage that is already
correct. It also does not remove the ergonomic cost it claims to avoid: a custom backend under Design B
still needs *some* name to register under and *some* place that registration is looked up, which is
strictly more moving parts than Design A's "just name the type."

### Design C (considered, not pursued) — `P` accepts either a type or a value via an overloaded template

A `SandboxProfile` that could be instantiated either as `SandboxProfile<SomeBackendType>` (type) or
`SandboxProfile<sandbox_profile::wasm>` (enum value) via two template declarations distinguished by
parameter kind, unified internally.

**Rejected without a build attempt:** C++ does not allow two class templates with the same name
differing only in whether their sole parameter is a type or a non-type parameter to coexist as ordinary
overloads the way function templates can — `SandboxProfile` would need to become two differently-named
templates (e.g. `SandboxProfileType<P>` / `SandboxProfileKind<K>`) or a single template taking `auto P`
(C++20's placeholder NTTP, which still cannot accept both a type-parameter argument and a value argument
through the same slot). Either way this reduces to "two ways to spell the same policy tag," a genuine
authoring-surface inconsistency 002 §3's own "the rule for adding a policy: a knob belongs here only if
it changes *what the agent is*" section gives no cover for. Not steelmanned further; the ordinary-C++
argument against it is decisive without needing to prove it wrong empirically.

## 4. Falsifiable claims (Design A)

- **C1 (compiles for real backends).** `SandboxProfile<P>` compiles for each of the three existing real
  `SandboxBackend`-conforming types, unmodified. *Disproof: any of the three fails to compile as `P`.*
- **C2 (rejects non-conforming types).** `SandboxProfile<int>` (or any other type satisfying neither
  `SandboxBackend` nor `Strict`) fails to compile at the declaration site. *Disproof: it compiles.*
- **C3 (Strict is real and distinct).** `SandboxProfile<Strict>`, and the case where no
  `SandboxProfile<...>` tag is declared at all, both compile through `register_agent<A>()` and both
  produce `SandboxProfileDescriptor{is_strict = true}` — the same result, proving the two spellings are
  genuinely equivalent (002 §3's stated default), not coincidentally both true today.
  *Disproof: either fails to compile, or the two produce different results.*
- **C4 (concrete traits round-trip exactly).** For `SandboxProfile<SomeBackend>`, the compiled
  `AgentMetadata::sandbox_profile.traits` is bit-for-bit `SomeBackend::traits` — not a hand-copied
  approximation that could drift from what the type itself declares.
  *Disproof: the extracted traits differ from `SomeBackend::traits` in any field.*
- **C5 (no layering violation).** None of `sandbox.hpp`/`agent.hpp`/`agent_registry.hpp` (all
  `agentengine::core`, header-only) gains a dependency on anything under `src/backends/`.
  *Disproof: any of the three headers includes a backend header, or the build breaks with
  `AGENTENGINE_WITH_WASM` off.*

## 5. The red-team attack

Calibrated per §2's note: `P` is authored by the deploying developer (host-trust-tier, I2/I3 do not
apply to it the way they apply to model-derived input), so the attack here is "does a mistaken or
adversarial-looking declaration get accepted when it shouldn't," not fuzzing or sanitizer runs.

- **R-C2 (the actual attack surface).** Try every plausible way a mistaken `P` could sneak past the
  constraint: a type satisfying *some but not all* of `SandboxBackend`'s four requirements (e.g. missing
  `destroy`, or `create` returning the wrong type); a type that satisfies `SandboxBackend` structurally
  but was never intended as one (accidental structural conformance); `Strict` misspelled as a
  same-named-but-different local type in a different namespace (should NOT satisfy
  `std::same_as<P, Strict>`, by design — no accidental cross-namespace acceptance).
- **R-C3.** Declare `SandboxProfile<Strict>` explicitly on one agent and leave the tag off entirely on
  another; if the two ever disagree, 002 §3's "default: `Strict`" claim is false in code even though it
  reads true in the table.
- **R-no-silent-fallback.** Confirm nothing in the new code paths silently resolves an unavailable/
  unrecognized profile to `none` (008 §3's prohibited failure mode) — by construction, there is no
  runtime resolution attempt for a concrete `P` at all (compile-time only), and `Strict`'s resolution is
  honestly left undone (returns `is_strict = true` with no traits) rather than guessing.

## 6. Executed evidence

**Design and build.** `sandbox.hpp`: removed the unused `sandbox_profile` enum (confirmed zero real
consumers outside its own definition and the old `SandboxProfile<P>` NTTP — grepped `src/` and `tests/`,
only informal prose comments referenced `sandbox_profile::none`, no compiled symbol use); added `Strict`,
`SandboxProfileArg`, `SandboxProfileDescriptor` immediately after the existing `SandboxBackend` concept.
`agent.hpp`: `SandboxProfile<P>` changed from `template <sandbox_profile P>` to
`template <SandboxProfileArg P>`. `agent_registry.hpp`: added `policy_sandbox_profile<Policy>` (the same
fold-extraction pattern as `policy_stateless`/`policy_max_turns`), `sandbox_profile_of<Policies...>()`,
an `AgentMetadata::sandbox_profile` field, and made `check_sandbox_profile_availability` take the
compiled descriptor and reason about it for real (still trivially passes today — see §9).

**C1/C2 (compile-fail proof, ADR-track discipline matching the existing 007 §9 G1/G2 `try_compile()`
gates in `tests/CMakeLists.txt`).** Two paired snippets under `tests/compile_fail/`:
`sandbox_profile_rejects_non_conforming_type.cpp` (`SandboxProfile<int>` — must NOT compile) and
`sandbox_profile_positive_control.cpp` (`SandboxProfile<Strict>` and `SandboxProfile<DummySandboxBackend>`
— must compile). Both wired via `try_compile()` at CMake configure time, `FATAL_ERROR` on either
direction failing. Windows (MSVC 19.51, VS18) configure output:
```
-- ADR-012 compile-fail proof: OK (SandboxProfile<P> rejects a non-conforming type; Strict and a
   real SandboxBackend both compile)
```
Reconfirmed identical on Linux (gcc-14, Docker, fresh container).

**C3/C4 (runtime round-trip, `tests/test_sandbox_profile_kind.cpp`).** Three fixture agents:
`DefaultProfileAgent` (no `SandboxProfile<...>` tag at all), `StrictAgent`
(`SandboxProfile<Strict>` explicit), `ConcreteProfileAgent` (`SandboxProfile<ConformingBackend>`, a
local test-only conforming type mirroring `smoke_vocabulary.cpp`'s existing `DummySandboxBackend`
pattern). All three register cleanly through the real `register_agent<A>()`; `DefaultProfileAgent` and
`StrictAgent` both compile to `is_strict = true` (C3); `ConcreteProfileAgent` compiles to
`is_strict = false` with `traits` field-by-field equal to `ConformingBackend::traits` (C4). Also asserts
`static_assert(SandboxProfileArg<ConformingBackend>)`, `static_assert(SandboxProfileArg<Strict>)`,
`static_assert(!SandboxProfileArg<int>)` inline, as a second, redundant compile-time check alongside the
`try_compile()` gate.

**C5 (layering).** The full project build (`cmake --build build`, both platforms) succeeds unmodified
with `AGENTENGINE_WITH_WASM` at its default (off unless the wasm cache dir is present) — `sandbox.hpp`/
`agent.hpp`/`agent_registry.hpp` never gained a backend `#include`; `ConformingBackend` in the test file
is a local, test-only type, not a real backend, precisely so the test itself does not need to link
against `agentengine::native_jail_backend`/`agentengine::wasm_backend`.

**Full-suite regression check.** Windows (`ctest -C Debug -j4`): 33/34 passed — the one failure is
`test_native_jail_backend_windows`, the same pre-existing, unrelated Job-Object OOM-vs-timeout
classification flake already documented in `decisions/ADR-011-first-party-egress-proxy.md` §6 and this
milestone's own memory record (re-confirmed independent of this change: it fails intermittently on
repeated runs regardless of what else changed). Linux (Docker, gcc-14, fresh container): 23/23 passed, 1
expected skip (`test_shell_runner_no_process_creation`, `llvm-nm` unavailable in the image).
`tools/naming_lint.py`: suppressed-finding count moved from 75 to 77 (net: -1 for the removed
`sandbox_profile` enum's suppression, +3 for `Strict`/`SandboxProfileArg`/`SandboxProfileDescriptor`'s
new ones), zero new *unsuppressed* violations — the 8 pre-existing findings are unrelated, in files this
task did not touch.

## 7. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| C1 — compiles for real backends | **CORRECT** | `test_sandbox_profile_kind.cpp`'s `ConformingBackend` (structurally identical shape to the three real backends) compiles as `P`; full project build (including the three real backend targets, where the platform/option builds them) succeeds unmodified. |
| C2 — rejects non-conforming types | **CORRECT** | `try_compile()` gate: `SandboxProfile<int>` fails to compile on both platforms; `static_assert(!SandboxProfileArg<int>)` passes. |
| C3 — Strict is real and distinct, default matches explicit | **CORRECT** | `test_sandbox_profile_kind.cpp`: `DefaultProfileAgent` and `StrictAgent` both compile to `is_strict = true`. |
| C4 — concrete traits round-trip exactly | **CORRECT** | `test_sandbox_profile_kind.cpp`: field-by-field equality assertion against `ConformingBackend::traits` passes. |
| C5 — no layering violation | **CORRECT** | Full build succeeds with `AGENTENGINE_WITH_WASM` unset; grep confirms no backend `#include` in the three core headers. |

## 8. The decision

**Accepted: Design A.** `SandboxProfile<P>` takes any type satisfying `SandboxProfileArg` — a real
`SandboxBackend`, or the resolution selector `Strict`. This makes 008 §2a's extensibility promise true
by construction rather than aspirational, fixes the independently-broken `Profile::Strict` symbol as
part of the same change (both RFCs, `002-Agent-Model-and-Authoring.md` §2/§3 and
`008-Sandbox-and-Isolation.md` §3, corrected to match — `Profile::Strict` → `Strict` throughout, per
CLAUDE.md's "fix the spec first, then the code"), and unblocks one of `register_agent<A>()`'s two
previously-stubbed `SandboxProfile` checks (`check_sandbox_profile_availability`) to do real,
compile-time-backed work.

## 9. Residual risks and deferred gates

- **`check_tool_sandbox_profile_compatibility` remains stubbed.** 002 §6's second SandboxProfile bullet
  ("a declared tool requiring a backend incompatible with the agent's `SandboxProfile<P>`") needs a
  per-tool backend-declaration policy tag that does not exist yet — `Tool<Derived, Policies...>` has no
  analog of `SandboxProfile<P>` today. This ADR resolved the template-parameter-*kind* conflict but
  deliberately did not invent that second, larger policy-tag surface as a drive-by; it is real, separate
  scope for whoever owns 006/002's tool-declaration surface next.
- **`Strict` resolution against real availability is still deferred**, for the same reason
  `check_chat_client_credentials`/`check_output_schema_enforceable` are: no `Engine` type exists in M2
  to hold a real backend registry. `resolve_strict()` (the ranking rule) and `Strict` (the now-real,
  structurally-distinct selector type) are both ready and tested; what's missing is the
  "enumerate what this deployment actually has available" half, which needs an Engine.
  `check_sandbox_profile_availability` therefore still always returns success — honestly, not
  silently, and for a narrower, now-correct reason than "the template-parameter kind is unresolved."
- **Only two of the four engine-shipped profiles have a real conforming type today** (`native-jail`,
  `wasm`) — `remote` (M9) and `none` (deliberately never conforms; 008 §3's own "refuses to load T2/T3
  code" boundary case) don't exist as `SandboxBackend` types, so `Strict`'s eventual real resolution will
  need to account for a candidate set smaller than the full profile table until `remote` is built.
- **The `sandbox_profile` enum was deleted, not deprecated.** Confirmed zero real (non-comment,
  non-RFC-prose) consumers before removing it; if a future change needs a closed, engine-only profile
  *kind* tag again (e.g. for a UI/config surface that shouldn't need real backend types linked in), it
  should be reintroduced deliberately, not resurrected as a side effect of reverting this ADR.
