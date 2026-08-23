# ADR-080 — Does `Strict` sandbox-profile resolution get a real, host-curated backend registry, or stay a permanent no-op?

**Status:** Proposed (design → red-team → prove phases complete; awaiting project-owner "Judged"
sign-off, matching ADR-070/071/072's own flow).

**Relates to:** `decisions/ADR-012-sandbox-profile-template-parameter-kind.md` (built `Strict` as a
real, structurally-distinct type and named exactly this gap as deferred in its own §9, not decided
against — this ADR closes it), `decisions/ADR-054-tool-registry-name-keyed-resolution.md` (the
closest structural precedent — a host-curated, name-keyed registry answering "does this name resolve,
and is it trustworthy" — reused, not reinvented), `008-Sandbox-and-Isolation.md` §3 (amended, see §6),
`docs/planning/2026-08-22-component-role-audit-tracker.md` Finding O (the real, deliberate
architecture this registry's scope boundary respects — see §1), `docs/planning/sandbox-backend-
registry-design-draft.md` (the full design record: Revision 1, a real 3-agent independent red-team
pass, Revision 2's fixes, and the prove-phase account — summarized, not duplicated, below),
`docs/research/2026-08-23-microvm-sandbox-backend-landscape.md` and `docs/research/2026-08-23-
sandbox-feature-parity-survey.md` (the competitor/feature-gap research motivating why this needs to
be pluggable at all, not just resolvable).

## 1. The question

**Stated so it has a wrong answer:** `check_sandbox_profile_availability()` (`core/agent_registry.hpp`)
has been an always-pass stub since M2, its own comment naming why: `resolve_strict()` (`sandbox/
sandbox.hpp`) is real, tested, 008 §3's ranking rule made exact — but nothing in this codebase ever
constructs its `candidates` span from real, registered backends. `Strict` is the default `SandboxProfile`
for every agent that declares none (002 §3) — so today, the common case's own "no fallback → startup
fails" safety rule (008 §3) is unenforceable by construction: a deployment with zero working sandbox
backends registers every default-profile agent successfully, silently.

This gap has a second, separate cost, which is what actually motivated reopening it now: a deployer
wanting to add a pluggable isolation technology — a microVM backend (Kata, gVisor, raw Firecracker)
being the concrete, stated motivation — has no way to make it *available to `Strict`* at all.
`SandboxProfile<P>` resolves at compile time (ADR-012); the only way to use a custom backend today is
to name its type directly on every single agent declaration that should use it, recompiling each one.
There is no config value, env var, or deployment-time choice that changes what `Strict` resolves to.

Does this gap get a real, host-curated backend registry — mirroring `ChatClientRegistry`/
`ToolRegistry`, the two precedents this codebase already has for "resolve a host-curated name/kind to
a real, live thing at runtime" — or does it stay deferred, on the grounds that nothing in this engine
consumes a resolved `SandboxBackend` in production yet (§0's own disclosed scope boundary, unchanged
by this ADR) so there is nothing real to gain by making selection real first?

## 2. The competing designs

### Design A (accepted) — a host-curated `SandboxBackendRegistry`, additive to `register_agent<A>()`

A `SandboxBackendRegistry` class (`sandbox/sandbox_backend_registry.hpp`, new file — `sandbox.hpp`
itself states its own scope as "shape only") that a host populates explicitly via
`register_backend<B>(name, shared_ptr<B> instance, strict_eligibility mode)`, then queries via
`resolve_strict(platform_id)` (008 §3's rule, against the real registered set) and
`resolve_named(HostSandboxSelection)` (the actual "config picks a backend" surface `Strict` alone
cannot express — a deployer wanting "always the microVM backend, even though native-jail scores
higher `strength`" needs a name, not a rank). `register_agent<A>()` grows a second, independent,
defaulted `SandboxBackendRegistry const*` parameter — every existing call site is unaffected;
supplying one opts a caller into `Strict` resolving for real.

**Steelman.** This is the SAME shape this codebase already chose twice for an identical class of
problem — `ChatClientRegistry` (`core/chat_client.hpp`) and `ToolRegistry`
(`core/tool_registry.hpp`, ADR-054) both answer "does this host-supplied name resolve to a real, live
thing, and under what trust tier" via an explicit, host-curated, never-auto-discovered map, wired into
`register_agent<A>()` the same additive-optional-parameter way `ChatClientRegistry` already is. No new
architectural vocabulary, no new resolution timing, no reopening of ADR-012 (which settled `P`'s
*kind* — a type or `Strict` — not how `Strict` gets resolved against live backends, a question ADR-012
§9 explicitly left open). It also directly answers the stated motivation: a microVM backend becomes
selectable via `resolve_named("microvm")` or eligible for `Strict` via one `register_backend()` call
at host startup, with zero recompilation of any individual agent's `SandboxProfile<P>` declaration.

### Design B (rejected) — global static self-registration (macro-based, link-time discovery)

A `AE_REGISTER_SANDBOX_BACKEND(Type, "name")` macro expanding to a static object whose constructor
registers `Type` into a process-wide singleton registrar at static-initialization time — the backend
"just works" once its translation unit is linked in, no explicit host wiring call needed.

**Steelman.** Zero explicit host-side registration code; a backend author ships a header, a consumer
links it, done — a lower-friction authoring experience than Design A's explicit `register_backend()`
call for the common case of "I just want native-jail and wasm available."

**Rejected because:** this project already has a live house rule directly against this shape —
`ToolRegistry`'s own file banner: *"HOST-CURATED ONLY, never auto-discovered — nothing is ever added
to a registry except by an explicit `register_tool()` call the host itself makes... this structurally
answers namespace squatting (nothing self-registers) rather than adding a check for it."* The same
argument applies here with higher stakes: a backend linked into the binary becoming automatically
`Strict`-eligible is authority granted by mere linkage, not by an explicit deployment decision — the
meta-level version of the I2 property this design otherwise preserves structurally (§4). It also
reopens a genuinely gratuitous class of C++ bug this codebase has no reason to accept: static
initialization order across translation units is unspecified unless deliberately sequenced, and this
project's own CRTP-policy-tag idiom (`Agent<Derived, Policies...>`, `Tool<Derived, Policies...>`)
already demonstrates the alternative — compile-time-visible, explicit composition — works fine for
every other "which of N optional things does this deployment want" question in this codebase. Not
prototyped; the ToolRegistry precedent and the I2-adjacent argument are decisive without needing to
build it to find out it fails the same way.

### Design C (rejected) — leave `Strict` resolution permanently deferred; require every agent to name a concrete backend explicitly

Do nothing. `check_sandbox_profile_availability()` stays an honest, permanent "not evaluated" stub;
`Strict`'s only real use becomes "the compile-time default nobody overrides but that also never gets
checked" — any deployer who actually cares which backend they get is expected to write
`SandboxProfile<ConcreteBackend>` directly, which already works today and needs no new machinery.

**Steelman.** Zero new code, zero new surface area, and it does not block anything that works today —
`SandboxProfile<ConcreteBackend>` already resolves entirely at compile time (ADR-012), so a deployer
willing to name a type is already fully served.

**Rejected because:** it contradicts 002 §3's own stated default — `Strict` is not a rarely-used
escape hatch, it is what *every* agent gets when it declares no `SandboxProfile<...>` tag at all, i.e.
the common case — and leaves 008 §3's "no fallback → startup fails" rule permanently unenforceable
for that common case: a deployment with zero working sandbox backends compiled in would keep
registering agents successfully forever, the exact "silently running unisolated... the single worst
failure mode in this design" 008 §3 names as prohibited. It also does not scale to the motivating use
case at all — supporting a pluggable microVM backend selectable by *deployment*, not by *recompiling
every agent* — since a config-driven choice between "native-jail in dev, microvm-kata in prod" for the
identical compiled agent binary is structurally impossible for a compile-time-only `SandboxProfile<P>`
to express, regardless of how many concrete backend types exist. This was the actual reason the
question got reopened (§1) — Design C is a description of the status quo, not an answer to it.

### Design D (considered, not pursued) — extend `SandboxProfile<P>`'s own template-parameter shape to carry runtime registry-lookup semantics

Instead of a separate runtime object, widen ADR-012's `SandboxProfileArg<P>` concept itself so `P` can
also be some new tag type parameterized by a registry-lookup key, unifying compile-time and
runtime-resolved selection into one mechanism at the `SandboxProfile<P>` declaration site.

**Rejected without a build attempt**, the same way ADR-012 §3 Design C was: ADR-012 already settled
what kind of thing `P` is (a real `SandboxBackend`-conforming type, or the `Strict` selector) — the
question this ADR answers is a different one (how `Strict` resolves against live backends *at
deployment time*), and conflating the two would require `SandboxProfile<P>`'s compile-time shape to
somehow also express a genuinely runtime fact: the same compiled agent binary resolving to different
concrete backends across different deployments of that binary (dev vs. prod, say) — the entire point
of `Strict` per 008 §3 ("resolves to the highest-strength profile whose platform list includes the
CURRENT platform," decided per-deployment, not per-compile). A template parameter cannot carry
information that does not exist until the process the compiled binary runs in decides it. Not
steelmanned further — the timing mismatch is decisive without an implementation to prove it.

## 3. Falsifiable claims (Design A)

- **C1 (state persists across the registered lifetime, not per call).** A backend registered once
  and resolved (by `Strict` or by name) is the SAME live instance across a `create()` call followed by
  one or more `exec()`/`destroy()` calls on the handle that call returned — not a fresh,
  default-constructed instance per call. *Disproof: a handle from `create()` fails `exec()`/`destroy()`
  with an "unknown handle"-shaped error, or per-instance state (a call counter, say) resets between
  calls on the same handle.*
- **C2 (blast-radius containment).** A backend registered `strict_eligibility::named_only` never wins
  `resolve_strict()`, regardless of its own declared `strength` — registering it for one agent's
  explicit named selection cannot silently change what every OTHER `Strict`-configured agent in the
  process resolves to. *Disproof: a `named_only` entry with higher `strength` than every
  `strict_eligible` entry is ever returned by `resolve_strict()`.*
- **C3 (fail-closed, no silent fallback).** `resolve_strict()` returns an error, never a default or a
  best-effort guess, when no `strict_eligible` entry supports the current platform — 008 §3's "no
  fallback → startup fails" rule, made real. *Disproof: it returns a value, or a `none`-shaped
  no-boundary result, in that case.*
- **C4 (fully additive; zero behavior change with no registry supplied).** Every existing
  `register_agent<A>()` call site (zero-arg, or one-arg with only a `ChatClientRegistry*`) compiles
  and behaves byte-identically to before this change. *Disproof: any pre-existing call site fails to
  compile, or its runtime result changes with no `SandboxBackendRegistry*` argument added.*
- **C5 (concrete `SandboxProfile<P>` is provably unaffected by registry contents).** An agent naming a
  concrete backend type directly registers successfully regardless of what is or is not registered in
  a supplied `SandboxBackendRegistry` — its availability was already proven at compile time (ADR-012),
  so `check_sandbox_profile_availability()` must never consult the registry for this shape. *Disproof:
  a concrete-backend agent's registration outcome changes depending on registry contents.*
- **C6 (I3 hygiene, honest-limits claim).** `resolve_named()` cannot be called with an implicitly-
  converted bare string — `HostSandboxSelection`'s single-argument constructor is `explicit`, so a
  call site passing a `ToolResult`/`ChatResponse` field or other plausibly-model-derived value must go
  out of its way (an explicit, visible construction) to do so. This is NOT claimed as cryptographic
  proof of provenance — see §5's honest-limits statement — only that the accidental path is
  structurally harder to write than passing a bare string would be. *Disproof: `resolve_named()`
  accepts a `std::string`/`std::string_view` argument without an explicit `HostSandboxSelection{...}`
  construction at the call site.*

## 4. The red-team attack

Three independent agents (not forks of one shared context — deliberately started fresh, to get true
adversarial independence rather than shared-context bias) red-teamed Revision 1 of the design draft in
parallel, each assigned a distinct lens: security/I2-I3, C++ correctness, and architecture-fit against
this project's locked decisions. All three findings below were CONFIRMED (not merely raised) and fixed
before any implementation code was written — Design A as described in §2 above and implemented in §5
is the POST-red-team shape, not the original sketch.

- **Blocking correctness bug (confirmed independently by two of the three agents).** The original
  `register_backend<B>()` sketch closed its `create`/`exec`/`destroy` closures over a fresh,
  default-constructed `B{}` temporary on every call. Both real conformers
  (`NativeJailBackend`/`WasmBackend`) hold per-instance state in an `instances_` map keyed by opaque
  handle id — a temporary's `create()` inserts into a map destroyed with that temporary at the end of
  the call, so the very next `exec()`/`destroy()` call (on a *different* temporary, with an empty map)
  is a guaranteed lookup miss. One reviewer traced a concrete, worse consequence: `Instance`'s
  `JobObjectLimits` destructor unconditionally kills every process assigned to it
  (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), so a real `create()` call under the original sketch would
  spawn a process only to have it killed on the same call's return — fail-closed, not a silent leak,
  but entirely non-functional as sketched, and it would ALSO have silently required `B` to be
  default-constructible, which `SandboxBackend` does not require and which this codebase's own
  `PythonRunner` (constructor-injected config) already demonstrates a real conformer need not be.
  **Closed** by holding one long-lived `shared_ptr<B>` per registration, constructed exactly once
  (C1 above; §5's regression test targets exactly this).
- **I3 exposure (security/I2-I3 lens).** `resolve_named()`'s parameter was a bare `std::string_view` —
  the design draft's prose claimed "host-controlled only," but nothing in the type signature enforced
  or even hinted at that contract, unlike `ToolRegistry::register_tool()`'s explicit
  `tool_provenance`/`outer_grant` structural check for non-native sources. **Closed** by
  `HostSandboxSelection` (C6 above) — explicitly documented, both in the header and in this ADR, as
  raising the bar rather than cryptographically proving provenance; the real open question of whether
  a stronger, `EffectContext`-principal-based check is warranted is named as a residual (§7), not
  quietly resolved by adding a wrapper type.
- **Blast-radius / observability gap (security/I2-I3 lens).** Registering any backend with a
  self-declared `strength` higher than every currently-registered backend would, under the original
  sketch, silently change what EVERY `Strict`-configured agent in the process resolves to,
  process-wide, with no distinction between "meant to compete for `Strict`" and "registered only for
  one agent's explicit named selection," and no audit trail. **Closed** by `strict_eligibility`
  (C2 above) plus an optional resolution-audit hook (`SandboxBackendResolutionAuditHook`, same
  `nullptr`-by-default idiom as `QuarantineAuditHook`, `trust/secret_quarantine.hpp`) that fires with
  the real winning entry's name whenever `resolve_strict()` succeeds.
- **Citation error, self-caught during the red-team pass.** The original wiring sketch used a
  `current_platform()` function that does not exist anywhere in this codebase — every real
  `resolve_strict()` call site before this work was a test passing a literal `platform_id`. Named as
  new, required work rather than silently assumed to already exist (§5).
- **Architecture-fit finding (confirmed, not fatal — a named judgment call, not a defeated design).**
  Shipping "selection without consumption" — this registry makes `Strict`/named resolution real, but
  no production code path anywhere in this engine constructs a `SandboxHandle` from what it resolves
  to; the only non-test `SandboxHandle` construction site in the whole tree today is the M2-era
  "compiled probe program" exec path — risks repeating the shape `docs/planning/2026-08-22-component-
  role-audit-tracker.md` Findings M/N already named ("Judged but not wired"). The reviewer's verdict
  was explicitly "needs a decision, not a rejection": real Python/Shell execution already bypasses
  `SandboxBackend::create/exec/destroy` entirely via interpreter-level mediation (Finding O, same
  tracker), so this registry does not create the selection/consumption split — it documents an
  already-existing one honestly rather than papering over it. **Addressed, not closed**: §1 states this
  scope boundary plainly rather than glossing over it, and §7 carries it forward as a named,
  tracked residual for the real ADR rather than an implicit gap. The same reviewer separately confirmed
  no conflict with CLAUDE.md's "no microvm sandbox profile" locked decision (selection mechanism is
  orthogonal to isolation technology) and confirmed this design is additive to ADR-012, not a
  reopening — ADR-012 §9 itself names this exact gap as deferred, not decided against.

## 5. Executed evidence

**`sandbox/sandbox.hpp`**: added `current_platform()` (closing the red-team's citation-error finding
above) — an `#ifdef`-gated `constexpr`, matching this header's existing "no RTTI, compile-time-
resolvable" convention; a third target platform is a build-time `#error`, not a silent fallback,
matching 021 §2's closed two-member platform set.

**`sandbox/sandbox_backend_registry.hpp`** (new file): `strict_eligibility{eligible, named_only}`;
`RegisteredSandboxBackend` (name, `ProfileTraits`, `strict_eligibility`, and three `std::function`
closures each capturing the SAME `shared_ptr<B>` — the C1 fix); `HostSandboxSelection` (the C6
mitigation, `explicit` single-argument constructor); `SandboxBackendResolutionEvent`/
`SandboxBackendResolutionAuditHook` (the audit-hook fix for the blast-radius finding, `nullptr` by
default); `SandboxBackendRegistry` itself (`register_backend<B>()`, `resolve_strict(platform_id)`,
`resolve_named(HostSandboxSelection const&)`). One implementation-level decision beyond what the
red-teamed design text specified: `entries_` is a `std::map`, not `std::unordered_map` like its
`ChatClientRegistry`/`ToolRegistry` precedents — `resolve_strict()`'s exact-tie case (equal `strength`
AND equal `platform_mask` popcount) needs a deterministic candidate order to hand `sandbox::
resolve_strict()`, and `unordered_map` iteration order is unspecified; sorting by the deployer's own
chosen names makes that outcome reproducible (I5: nondeterminism crosses a recorded seam) instead of
silently order-dependent. Documented in the header itself, not only here.

**`core/agent_registry.hpp`**: `check_sandbox_profile_availability()` now takes a real
`SandboxBackendRegistry const*`; with `nullptr` (the default), stays the honest pre-registry
always-pass stub (C4); with one supplied, resolves `desc.is_strict` cases for real against
`registry->resolve_strict(current_platform())`, and never consults the registry at all when
`!desc.is_strict` (C5 — the concrete-backend case was already proven safe at compile time).
`register_agent<A>()` grows the second, additive, defaulted `sandbox_registry` parameter, threaded
through `agent_detail::compiler<A,...>::run()` exactly the way `ChatClientRegistry*` already is.

**Tests** — `tests/test_sandbox_backend_registry.cpp` (the registry itself) and
`tests/test_agent_registry_sandbox_backend_registry.cpp` (its `register_agent<A>()` wiring), 15 checks
total:
- The load-bearing check is a literal regression test for the confirmed bug above: `StatefulBackend`,
  shaped exactly like `NativeJailBackend`/`WasmBackend` (an `instances_` map keyed by opaque handle
  id), proves `create()` then `exec()` then a second `exec()` on the same handle all reach the SAME
  long-lived instance (an incrementing per-instance counter is observed to actually increment across
  calls) — this test fails outright against the pre-red-team `B{}`-per-call sketch, by construction.
- `register_backend()` rejects a duplicate name (`sandbox_backend_registry.duplicate_name`);
  `resolve_named()` fails closed on an unknown name (`sandbox_backend_registry.name_not_found`).
- A `named_only` entry with `strength = 100` never wins `resolve_strict()` against a `strict_eligible`
  entry with `strength = 42` (C2), while still being directly reachable via `resolve_named()`.
- `resolve_strict()` fails closed (`sandbox_backend_registry.no_strict_candidate`) when the only
  registered entry doesn't support the queried platform (C3).
- The audit hook, when supplied, observes exactly the winning entry's name; `resolve_strict()` still
  works correctly with no hook supplied (the default).
- `register_agent<A>()`: a zero-arg call is unaffected (C4); an empty `SandboxBackendRegistry` now
  fails `Strict` resolution closed for real (`sandbox_backend_registry.no_strict_candidate` surfaces
  through `register_agent<A>()` unmodified); a real, current-platform-supporting backend resolves and
  registration succeeds; a concrete `SandboxProfile<ConcreteBackend>` agent registers cleanly against
  an EMPTY registry, with its compiled metadata still carrying the concrete backend's own real
  `traits` (C5).

**Verification, independent of the implementing pass's own self-report**: full `ALL_BUILD` (Debug,
MSVC 19.x) succeeds with no new warnings; full `ctest` — 223 tests, 213 passing, the only 10
non-passing are pre-existing and confirmed unrelated (`test_mediated_python_runner_*` and siblings,
gated behind an `AGENTENGINE_PYTHON_HOME`-conditioned embedded-CPython toolchain block in
`tests/CMakeLists.txt` this environment has never had configured — confirmed by grepping the build log
for these targets and finding they were never even added to `ALL_BUILD`, not a build failure this
change caused). `tools/naming_lint.py`: every name this work introduces
(`strict_eligibility`/`RegisteredSandboxBackend`/`HostSandboxSelection`/`SandboxBackendRegistry`/
`SandboxBackendResolutionEvent`/`SandboxBackendResolutionAuditHook`) is correctly suppressed with an
inline `ae-naming-lint: allow` comment naming the reason (027 not yet updated); the 9 unsuppressed
violations the tool still reports predate this branch entirely (`ToolCallArgumentChunk`,
`session_builder.hpp`'s `Provider`/`Bundle`/etc., `multi_agent.hpp`'s `SessionFactory`/`Budget`).

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| C1 — state persists across the registered lifetime | **CORRECT** | `test_sandbox_backend_registry.cpp`'s regression case: `create()` → `exec()` (`exec#1`) → `exec()` again (`exec#2`, same instance) → `destroy()` (instance map empties) — all through the registry's own closures, not a direct call to the backend. |
| C2 — `named_only` never wins `Strict` | **CORRECT** | Same file: a `strength = 100` `named_only` entry loses to a `strength = 42` `strict_eligible` one in `resolve_strict()`, while still resolving correctly via `resolve_named()`. |
| C3 — fail-closed with no platform-supporting candidate | **CORRECT** | Same file: a Linux-only-supporting registered entry, queried for Windows, returns `sandbox_backend_registry.no_strict_candidate`, not a value. |
| C4 — fully additive, zero behavior change unsupplied | **CORRECT** | `test_agent_registry_sandbox_backend_registry.cpp`: a zero-arg `register_agent<StrictAgent>()` call still registers cleanly; full `ctest` shows no pre-existing test's outcome changed. |
| C5 — concrete `SandboxProfile<P>` unaffected by registry contents | **CORRECT** | Same file: `ConcreteAgent` (`SandboxProfile<ConcreteBackend>`) registers successfully against a deliberately EMPTY `SandboxBackendRegistry`, with `meta->sandbox_profile.traits` matching `ConcreteBackend::traits` field-for-field. |
| C6 — `HostSandboxSelection` raises the bar (honest-limits) | **CORRECT, with the limit stated, not overclaimed** | `explicit` constructor confirmed to reject implicit construction from `std::string`/`std::string_view` at the type level (compiles only with an explicit `HostSandboxSelection{...}`); NOT claimed, and the header/§5 explicitly disclaim, that this proves *who* called `resolve_named()` — no capability or principal check backs it. |

## 7. The decision

**Design A is accepted, implemented, and proven** on branch `sandbox-backend-registry` (commit
`9d87a67`), scoped exactly as the design draft's §0 stated: this registry makes backend *selection*
real and runtime-configurable; it deliberately does not, by itself, make backend *consumption* real
for any execution path.

**Binds:**
- `008-Sandbox-and-Isolation.md` §3's "Resolving `Strict`" paragraph — amended (this ADR) to replace
  its prior "still needs an Engine-level backend registry M2 does not build (ADR-012 §9)" sentence
  with a description of the real mechanism now available, while preserving the honest disclosure that
  `Strict` still stays unevaluated when no registry is supplied, and that selection is not consumption.
- `decisions/ADR-012-sandbox-profile-template-parameter-kind.md` §9 — its named residual ("`Strict`
  resolution against real availability is still deferred... `resolve_strict()`/`ProfileTraits` are
  both ready and tested; what's missing is the 'enumerate what this deployment actually has available'
  half") is now closed by this ADR, additively — ADR-012's own decision (what kind of thing `P` is) is
  unchanged and unreopened.
- `core/agent_registry.hpp` — `check_sandbox_profile_availability()` and `register_agent<A>()`'s
  signatures, as executed in §5.

**Explicitly out of scope, named rather than left implied** (unchanged from the design draft's own
§0 scope boundary):
- **No production consumption.** Nothing in this engine constructs a real `SandboxHandle` from a
  registry-resolved backend outside tests — real Python/Shell execution continues to bypass
  `SandboxBackend::create/exec/destroy` entirely via interpreter-level mediation (component-role-audit
  tracker Finding O). Wiring `AgentSession` or the mediation layer to actually construct and use a
  resolved backend is separate, larger, unimplemented follow-on work — the architecture-fit red-team's
  own "needs a decision, not a free pass" framing (§4) is carried forward here as a tracked item, not
  silently dropped the way the tracker's Findings M/N observed elsewhere in this codebase.
- **The microVM backend itself is a separate ADR**, per explicit project-owner direction (design
  draft's own header note, restated here): this registry is the seam a microVM (or Kata, or
  gVisor-`runsc`) backend plugs into via one `register_backend()` call — which Linux isolation
  technology to wrap, its own `ProfileTraits`, and whether its own backend type satisfies §7's
  thread-safety requirement are that ADR's decisions to make, informed by `docs/research/2026-08-23-
  microvm-sandbox-backend-landscape.md`'s concrete tradeoffs (including a live 2026 jailer CVE worth
  weighing).

## 8. Residual risks

- **Backend-internal thread-safety is a named requirement on the registered backend, not something the
  registry enforces.** Centralizing one shared `shared_ptr<B>` instance behind closures reachable from
  multiple concurrent sessions makes a pre-existing gap newly reachable in practice: neither
  `NativeJailBackend` nor `WasmBackend` documents a concurrent-call guarantee today (no visible mutex
  in either's `instances_` map). A real deployment built on this registry must either (a) verify each
  registered backend's own internal thread-safety before running it against a multi-session host, or
  (b) have a future revision of this registry serialize calls per registered entry itself (a mutex per
  `RegisteredSandboxBackend`) — deliberately not decided here, matching the design draft's own explicit
  deferral of this exact question.
- **`HostSandboxSelection` raises the bar against an accidental I3 violation; it does not
  cryptographically prove provenance.** Nothing in this design gives the registry a way to verify *who
  actually called* `resolve_named()` — it operates at the same trust tier `ToolRegistry` itself already
  does (host-curated only, enforced by code review and the call site being host code by construction,
  not a runtime credential check). A future revision could require `EffectContext`'s own principal to
  carry a specific host-only marker capability before `resolve_named()` accepts a call; whether that
  additional cost is warranted here is left open, not resolved by this ADR.
- **Selection without consumption, disclosed, not resolved.** As stated in §7's scope boundary — this
  is the single largest residual, named plainly rather than glossed over, because this exact
  "mechanism built, nothing wired to use it" shape has a track record in this codebase
  (component-role-audit tracker Findings M/N) of staying unwired indefinitely once the ADR that built
  it is Judged and attention moves on. Whoever picks up the microVM backend ADR, or any future work
  touching `AgentSession`'s sandbox lifecycle, should treat "does a real execution path now call
  `registry.resolve_*()` and use what comes back" as a question this ADR deliberately left open, not
  one it silently answered "no, forever."
