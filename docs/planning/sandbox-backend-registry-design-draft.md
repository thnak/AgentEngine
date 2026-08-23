# Design draft: a runtime-configurable `SandboxBackend` registry

**Status:** Design draft, revised once after a real (3-agent, independent) red-team pass, then proven
with real code and tests (see "Prove phase" below) — **not yet an ADR**. Scoped deliberately narrower
than the companion microVM-backend
question: this document is only about *how a deployment picks which registered `SandboxBackend`
runs*, never about building a microVM backend itself. Per project-owner direction (2026-08-23):
the two are separate ADRs, this one first — a microVM (or any other custom) backend is meant to be
a *consumer* of the mechanism this document specifies, not bundled with it. Companion research
this draft draws on: `docs/research/2026-08-23-microvm-sandbox-backend-landscape.md` (competitor
pluggability survey) and `docs/research/2026-08-23-sandbox-feature-parity-survey.md` (feature-gap
survey); companion tracker finding: `docs/planning/2026-08-22-component-role-audit-tracker.md`
Finding O.

## Prove phase (2026-08-23) — real code and tests on branch `sandbox-backend-registry`

Revision 2's design is now real, unmodified from what's written below (no further design drift found
while implementing it):

- **`include/agentengine/sandbox/sandbox.hpp`**: added `current_platform()` (Revision 2 finding #5),
  exactly as sketched in §2b.
- **`include/agentengine/sandbox/sandbox_backend_registry.hpp`** (new file, not folded into
  `sandbox.hpp` — that header states its own scope as "shape only"): `strict_eligibility`,
  `RegisteredSandboxBackend`, `HostSandboxSelection`, `SandboxBackendRegistry`
  (`register_backend`/`resolve_strict`/`resolve_named`), plus a `SandboxBackendResolutionEvent` +
  `SandboxBackendResolutionAuditHook` pair implementing §2a's "resolution outcome" audit requirement
  — same optional-`nullptr`-by-default idiom as `QuarantineAuditHook`
  (`trust/secret_quarantine.hpp`), not invented fresh. **One implementation decision beyond what
  Revision 2's text specified**: `entries_` is a `std::map`, not `std::unordered_map` like its
  `ChatClientRegistry`/`ToolRegistry` precedents — `resolve_strict()`'s exact-tie case (equal
  `strength` AND equal `platform_mask` popcount) needs a deterministic candidate order to hand
  `sandbox::resolve_strict()`, and `unordered_map` iteration order is unspecified; sorting by the
  deployer's own chosen names makes that outcome reproducible (I5) rather than silently
  order-dependent. Documented in the header itself, not just here.
- **`include/agentengine/core/agent_registry.hpp`**: `check_sandbox_profile_availability()` now takes
  a real `SandboxBackendRegistry const*` and resolves `Strict` for real when one is supplied (still
  the honest pre-registry stub when it isn't); `register_agent<A>()` grows the second, additive,
  defaulted `sandbox_registry` parameter exactly as §2b specified, threaded through
  `agent_detail::compiler<A,...>::run()`.
- **Tests** (`tests/test_sandbox_backend_registry.cpp`, `tests/test_agent_registry_sandbox_backend_
  registry.cpp`): 15 checks total. The load-bearing one is a literal regression test for the
  confirmed Revision 1 bug — a `StatefulBackend` shaped exactly like `NativeJailBackend`/
  `WasmBackend` (an `instances_` map keyed by opaque handle id) proves `create()` then `exec()` then
  a second `exec()` on the same handle all reach the SAME long-lived instance; this test would fail
  outright against Revision 1's `B{}`-per-call sketch. Also covers: duplicate-name rejection,
  unknown-name fail-closed, `named_only` never winning `Strict` regardless of declared strength,
  `resolve_strict()` failing closed with no platform-supporting candidate, the audit hook firing with
  the real winning name (and working correctly when omitted), and `register_agent<A>()`'s wiring
  (no-registry unaffected, empty-registry now fails closed for real, a real registered backend
  resolves, a concrete `SandboxProfile<P>` is provably unaffected by registry contents either way).
- **Verification**: full `ALL_BUILD` (Debug) succeeds with no new warnings; the full `ctest` suite
  passes (223 tests; the only non-passing ones are 10 pre-existing, unrelated tests gated behind an
  embedded-CPython toolchain check this environment doesn't have configured — confirmed via the
  build log and `AGENTENGINE_PYTHON_HOME`-gated `tests/CMakeLists.txt` block, not something this
  change touched). `tools/naming_lint.py` passes for every name this work introduces (all correctly
  suppressed with `ae-naming-lint: allow` — the 9 unsuppressed violations it still reports predate
  this branch entirely).
- **Left exactly as open as Revision 2 left it, not resolved by writing code**: backend-internal
  thread-safety is still a named requirement on the registered backend, not something the registry
  itself enforces (§2a); `HostSandboxSelection`'s trust tier is still "raises the bar," not
  cryptographic proof (§2c/§3); real production *consumption* of a resolved backend still does not
  exist anywhere (§0's disclosed scope boundary — unchanged, on purpose). These remain the real ADR's
  decisions to make, not something a passing test suite can settle.

## Revision 2 (2026-08-23) — what the red-team found and how this draft changed

Three independent agents red-teamed Revision 1 in parallel (security/I2-I3, C++ correctness,
architecture-fit against locked decisions). Kept here as a point-in-time record rather than
silently editing the original text away, matching this project's ADR-history convention.

**Confirmed, fixed in this revision:**

1. **Blocking bug, confirmed independently by two agents**: Revision 1's `register_backend<B>()`
   closed over `B{}` — a fresh, default-constructed, *temporary* `B` on every `create`/`exec`/
   `destroy` call. Both real conformers (`NativeJailBackend`, `WasmBackend`) hold per-instance state
   in an `instances_` map; a temporary's `create()` inserts into a map that is destroyed when the
   temporary is destroyed at the end of that call, so the next `exec()`/`destroy()` call — on a
   *different* temporary with an empty map — is a guaranteed lookup miss
   (`native_jail.unknown_handle`). One reviewer additionally traced a concrete consequence: an
   `Instance`'s `JobObjectLimits` destructor unconditionally kills every process assigned to it
   (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), so a real `create()` call under Revision 1's sketch would
   spawn a process only to have it killed on the same call's return. Fail-closed, not a silent
   leak — but non-functional as sketched. **Fixed in §2a below**: the registry now owns one
   long-lived instance per registration, constructed once, captured by `shared_ptr` in the three
   closures — matching how `tests/test_wasm_tool_bridge.cpp` already uses `WasmBackend` in practice
   (one `std::make_shared<WasmBackend>()`, reused across calls), and how the real
   `mediated_command_registry.hpp` precedent this draft cited actually works (it stores closures
   around an *already-constructed* instance the caller owns — it never default-constructs anything
   itself, contrary to how Revision 1 characterized the analogy).
2. **`register_backend<B>()`'s reliance on `B{}` also silently required default-constructibility,
   which `SandboxBackend` does not require and which a real, expected conformer shape
   (constructor-injected config, the same "not default-constructible" pattern this codebase's own
   `PythonRunner` already documents) would fail to satisfy.** Fixed by the same change as #1 — the
   registry now takes an already-constructed instance (or a factory the host controls), never
   default-constructs anything.
3. **`resolve_named()`'s parameter was a bare `std::string_view`** — the draft's prose claimed
   "host-controlled only," but nothing in the type signature enforced or even hinted at that
   contract, unlike `ToolRegistry::register_tool()`, which structurally checks a `tool_provenance`
   and an `outer_grant` for every non-native source. Fixed in §2c below with a distinct
   host-constructed selection type, closing the gap partially (raises the bar; does not
   cryptographically prove provenance — see §3's honest limits statement).
4. **Global blast radius when `Strict` silently re-picks**: registering any backend with a
   self-declared `strength` higher than today's real backends changes what *every* `Strict`-agent
   in the process resolves to, process-wide, with no per-agent scoping, no audit trail, and no
   distinction between "meant to compete for `Strict`" and "registered only for one agent's
   explicit named selection." Fixed in §2a/§2c below with a `strict_eligible` flag per registration
   plus a required resolution-audit hook.
5. **`current_platform()`, used in Revision 1's §2b wiring sketch, does not exist anywhere in this
   codebase** — every real caller of `resolve_strict()` today is a test passing a literal
   `platform_id`. Revision 1's own "verified directly against current code" claim did not hold for
   this one line. Fixed by naming this as a new primitive this design must also add (§2b).
6. **Thread-safety of a now-shared backend instance across concurrent sessions was unaddressed.**
   Real backends (`NativeJailBackend`/`WasmBackend`) mutate an unsynchronized `instances_` map; a
   registry that centralizes one shared instance behind closures reachable from multiple sessions
   makes a pre-existing gap newly reachable in practice. Named as an explicit residual/requirement
   in §2a rather than left invisible.

**Reviewed and cleared, unchanged:** the architecture-fit reviewer found no conflict with
CLAUDE.md's "no microvm profile" decision (selection mechanism is orthogonal to isolation
technology), confirmed §2b's registry parameter is additive to ADR-012 (not a reopening — ADR-012
§9 itself names this exact gap as deferred, not decided against), and confirmed the draft's
characterization of `resolve_strict()`/008 §3 as accurate. The one open, non-bug item that reviewer
raised — whether shipping selection-without-consumption (§0's disclosed scope boundary) repeats the
tracker's "Judged but not wired" pattern (Findings M/N) — is a judgment call for the eventual ADR's
sign-off, not something this design draft can resolve unilaterally; §0 and §6 (below) keep it
visible rather than deciding it here.

## 0. Re-grounding the question, not just restating a hunch

Verified directly against current code (2026-08-23), not assumed from the RFC text:

- **`check_sandbox_profile_availability()` — the one function whose job is "is this profile
  actually usable in this deployment" — is a stub.** `include/agentengine/core/agent_registry.hpp:363-365`:
  ```cpp
  [[nodiscard]] inline result<void> check_sandbox_profile_availability(SandboxProfileDescriptor const&) {
      return {};
  }
  ```
  Its own adjacent comment names the reason: *"needs an Engine-level backend registry M2 does not
  build... `resolve_strict()`/`ProfileTraits` are the tested, real ranking logic already waiting
  for that registry."* This document is that registry.

- **The ranking algorithm this registry needs to feed already exists, is real, and is tested.**
  `resolve_strict()` (`include/agentengine/sandbox/sandbox.hpp:73-90`) is a pure function:
  `std::span<ProfileTraits const> candidates, platform_id current) -> std::optional<std::size_t>`
  — highest `strength` wins, ties broken toward broader `platform_mask`, `nullopt` when nothing
  supports `current` (008 §3's "no fallback → startup fails" case, which the function deliberately
  leaves for its caller to enforce). **Nothing in this design needs to touch this function** — the
  gap is entirely upstream of it: nothing constructs the `candidates` span from real, registered
  backends.

- **Backend selection is 100% compile-time today.** `SandboxProfile<P>` (`core/agent.hpp:39-40`)
  takes `P` = a concrete `SandboxBackend`-conforming type, or the `Strict` selector — resolved at
  `register_agent<A>()`. There is no config file, env var, or runtime value that changes which
  backend an agent gets without editing and recompiling that agent's C++ declaration.

- **A real, working precedent for exactly this shape already exists twice in this codebase**:
  `ChatClientRegistry` (`core/chat_client.hpp:380-394`, `register_client`/`find` over an
  `unordered_map`) and `ToolRegistry` (`core/tool_registry.hpp:48-108`, same shape plus a
  provenance-trust rule and an `exclusion_reason()` diagnostic). Both are host-curated,
  constructor-injected into `register_agent<A>()`/equivalent, never auto-discovered. This design
  reuses that shape rather than inventing a new one.

- **A larger, adjacent gap this draft does NOT close, named honestly rather than assumed fixed**:
  grepping the whole tree for real (non-test) `SandboxHandle` construction finds exactly **one**
  call site, and it is `tests/test_wasm_tool_bridge.cpp` — a test fixture. No production
  `AgentSession` code path calls any `SandboxBackend::create()` today. `AgentMetadata.sandbox_profile`
  (`agent_registry.hpp:73`) is compiled and structurally validated, but nothing downstream currently
  *reads* it to construct a real sandbox — it is write-only metadata in production as of this
  writing. This matches Finding O's own conclusion from a different angle: real Python/Shell
  execution already bypasses the `SandboxBackend` lifecycle entirely (`invoke_tool()` directly), so
  the M2-era "compiled probe program" exec path (§1 of `native_jail_backend.hpp`) is, today, the
  *only* thing `SandboxBackend::create/exec/destroy` is for. **This design makes backend
  *selection* real and runtime-configurable; it does not, by itself, make backend *consumption*
  real for Python/Shell** — that is either a separate, larger design (wiring `AgentSession` or the
  mediation layer to actually construct and use a resolved backend) or an explicit, disclosed scope
  boundary this ADR states plainly rather than glosses over.

## 1. What a `SandboxBackendRegistry` actually has to answer

Given a deployment that has compiled in N conforming `SandboxBackend` types (first-party
`NativeJailBackend`/`LinuxNativeJailBackend`/`WasmBackend`, plus zero or more consumer-supplied
custom backends per 008 §2a — a future microVM backend among them), four questions, in order:

1. **How does a backend get into the registry at all?** (Host-curated, explicit, like
   `ToolRegistry` — never auto-discovered; see §5's I2 point.)
2. **Given `Strict`, which registered backend wins on the current platform?** (`resolve_strict()`
   already answers "which wins" — the registry's job is only to hand it real `candidates`.)
3. **Given an explicit name (a config file, an env var, a deployment script — some real,
   host-controlled value, never model output), which backend does that name resolve to?** This is
   the actual "config to choose backend" the user asked for — `Strict` alone can never express
   "always use the microVM backend even though native-jail scores higher strength," which a
   deployer may legitimately want (e.g., trading cold-start for a stronger boundary on a specific
   high-risk agent).
4. **What happens when nothing is registered, or nothing registered supports this platform?**
   `resolve_strict()` already returns `nullopt` for the "nothing supports current platform" case;
   the registry needs to turn that into 008 §3's "no fallback → startup fails" `result<void>` at
   `check_sandbox_profile_availability()`'s call site, replacing today's always-`{}` stub with a
   real fail-closed check.

## 2. The design

### 2a. `SandboxBackendRegistry` — host-curated, mirrors `ChatClientRegistry`/`ToolRegistry`

```cpp
namespace agentengine {

enum class strict_eligibility { eligible, named_only };  // Revision 2, finding #4

// One entry per registered backend. `create`/`exec`/`destroy` close over a `shared_ptr<B>` — ONE
// long-lived instance per registration, constructed exactly once at register_backend() time, never
// per-call (Revision 2, findings #1/#2 — a real, confirmed bug in Revision 1's B{}-per-call sketch,
// and a real mismatch with SandboxBackend conformers that are not default-constructible).
struct RegisteredSandboxBackend {
    std::string          name;        // e.g. "native-jail", "wasm", "microvm-kata" — deployer-chosen
    ProfileTraits         traits;      // copied once at registration, exactly B::traits
    strict_eligibility    strict_mode; // eligible: competes in resolve_strict() ranking.
                                       // named_only: reachable ONLY via resolve_named(), never
                                       // silently wins Strict for an unrelated agent — closes
                                       // Revision 2 finding #4's blast-radius gap. A deployer
                                       // registering a custom/experimental backend for one specific
                                       // high-risk agent's named selection sets this explicitly.
    std::function<result<SandboxHandle>(SandboxSpec const&, EffectContext&)>        create;
    std::function<result<ExecOutcome>(SandboxHandle&, ExecRequest const&, EffectContext&)> exec;
    std::function<void(SandboxHandle&)>                                             destroy;
};

class SandboxBackendRegistry {
public:
    // `instance` is a caller-owned, already-constructed backend — the host builds it however B's
    // own constructor needs (default, config-injected, whatever), exactly once, and hands ownership
    // here. This is the fix for Revision 1's B{}-per-call bug: every closure below captures the SAME
    // shared_ptr, so state B stores between create()/exec()/destroy() (NativeJailBackend's
    // instances_ map, e.g.) survives across the whole registered lifetime, not just one call.
    //
    // THREAD-SAFETY, named explicitly rather than left invisible (Revision 2 finding #6): a backend
    // registered here MUST tolerate concurrent create/exec/destroy calls from unrelated sessions —
    // this registry does not add its own synchronization around the shared instance. Neither
    // NativeJailBackend nor WasmBackend documents this guarantee today (their instances_ maps have
    // no visible mutex) — a real ADR built on this draft must either (a) require and verify each
    // registered backend's own internal thread-safety before this ships against a multi-session
    // host, or (b) have the registry serialize calls per registered entry itself (a mutex per
    // RegisteredSandboxBackend), trading concurrency for correctness. Left as an explicit open
    // decision for the ADR, not resolved here.
    template <SandboxBackend B>
    [[nodiscard]] result<void> register_backend(std::string name, std::shared_ptr<B> instance,
                                                 strict_eligibility mode = strict_eligibility::eligible) {
        if (entries_.contains(name)) {
            return std::unexpected(error{failure_class::contract,
                                          "a sandbox backend named '" + name + "' is already registered",
                                          "sandbox_backend_registry.duplicate_name"});
        }
        entries_.emplace(name, RegisteredSandboxBackend{
            name, B::traits, mode,
            [instance](SandboxSpec const& spec, EffectContext& ctx) { return instance->create(spec, ctx); },
            [instance](SandboxHandle& h, ExecRequest const& r, EffectContext& ctx) { return instance->exec(h, r, ctx); },
            [instance](SandboxHandle& h) { instance->destroy(h); },
        });
        return {};
    }

    // §1 Q2: Strict resolution — filters to strict_eligible entries only, hands resolve_strict()
    // the real candidate set, translates its nullopt into 008 §3's fail-closed contract instead of
    // leaving that to the caller (closing check_sandbox_profile_availability's own stub). Every
    // resolution logs the winning entry's name (Revision 2 finding #4's audit requirement) — a
    // deployer can see, after the fact, which backend every Strict-configured agent actually got,
    // rather than that choice being silent and undiscoverable.
    [[nodiscard]] result<RegisteredSandboxBackend const*> resolve_strict(platform_id current) const;

    // §1 Q3 / §2c: named resolution — the actual "pick a specific backend via config" entry point.
    // Both strict_eligible and named_only entries are reachable here (named_only exists to be
    // reachable ONLY here). Fails closed on an unknown name (Revision 1 §4's own self-check, still
    // correct): sandbox_backend_registry.name_not_found, never a silent fallback.
    [[nodiscard]] result<RegisteredSandboxBackend const*> resolve_named(HostSandboxSelection const& selection) const;

private:
    std::unordered_map<std::string, RegisteredSandboxBackend> entries_;
};

}  // namespace agentengine
```

**Why type-erasure here, unlike `ToolRegistry`'s already-type-erased `ToolDescriptor`/`ToolRegistry`
pairing** — `SandboxBackend` is a compile-time *concept*, not an existing type-erased vocabulary
type the way `ToolDescriptor` already is for tools. `RegisteredSandboxBackend` is the analog this
registry needs to introduce. **Revision 2 correction**: the original text here claimed this mirrors
`mediated_command_registry.hpp`'s `RegisteredRunner`/`RegisteredTool` pattern "for an identical
problem" — the correctness red-team checked this claim directly and found the real precedent
*already* takes a pre-constructed instance and never default-constructs anything itself; Revision 1
had the shape right but its own `B{}` sketch didn't actually follow the precedent it cited. This
revision now genuinely matches it.

### 2b. Wiring into `register_agent<A>()`

**New primitive this design must also add (Revision 2 finding #5)**: `current_platform()` does not
exist anywhere in this codebase today — grepped the whole tree; every real `resolve_strict()` call
site is a test passing a literal `platform_id`. This design adds it, in `sandbox.hpp` alongside
`platform_id` itself, as an ordinary compile-time-constant function (matching this file's existing
"no RTTI, compile-time-resolvable" convention — a `#ifdef`-gated `constexpr`, not a runtime probe):

```cpp
[[nodiscard]] constexpr platform_id current_platform() noexcept {
#if defined(_WIN32)
    return platform_id::windows_x86_64;
#elif defined(__linux__)
    return platform_id::linux_x86_64;
#else
#error "current_platform(): unsupported target platform"
#endif
}
```

`check_sandbox_profile_availability()`'s signature grows an optional registry pointer, the same
shape `check_chat_client_credentials(chat_client_id, registry)` already uses for `ChatClientRegistry`
— and, per §0's own precedent, this needs threading through `register_agent<A>()`'s real call chain,
not left implicit as Revision 1 left it (a real correctness-red-team finding: Revision 1 never
showed how the pointer reaches `compiler<A,...>::run`, only `check_sandbox_profile_availability`'s
own signature). `agent_detail::compiler<A, Agent<...>>::run()` (`agent_registry.hpp:449`) already
takes `ChatClientRegistry const* registry`; this design adds a second, independent optional
pointer alongside it — `register_agent<A>()` grows a second defaulted parameter, mirroring the
first's own "opts a caller into the real check" contract:

```cpp
template <class A>
[[nodiscard]] result<AgentMetadata> register_agent(ChatClientRegistry const* chat_registry = nullptr,
                                                     SandboxBackendRegistry const* sandbox_registry = nullptr) {
    return agent_detail::compiler<A, agent_detail::agent_base_t<A>>::run(chat_registry, sandbox_registry);
}
```

```cpp
[[nodiscard]] inline result<void> check_sandbox_profile_availability(
        SandboxProfileDescriptor const& desc, SandboxBackendRegistry const* registry) {
    if (registry == nullptr) return {};  // unchanged behavior when no registry is supplied —
                                          // same "not evaluated" honesty check_output_schema_
                                          // enforceable already uses for its own optional registry
    if (desc.is_strict) {
        return registry->resolve_strict(current_platform()).transform([](auto*) { return; });
    }
    // is_strict == false means P already named a concrete, compile-time-proven backend type
    // directly (SandboxProfileDescriptor's own existing comment) — nothing to resolve by name;
    // this path is unaffected by the registry either way.
    return {};
}
```

`SandboxProfileDescriptor` itself is unchanged — this design adds no new field to it. The registry
answers "is `Strict` resolvable," not "what did it resolve to as data the descriptor should now
carry" — carrying the resolved backend forward into `AgentMetadata` in a form something can later
construct from is exactly §0's named "not attempted here" scope boundary (consumption, not
selection).

### 2c. Named resolution as the actual "config picks a backend" surface

**Revision 2 finding #3 fix**: Revision 1 had `resolve_named(std::string_view name)` — a bare
string, structurally indistinguishable from any other string in the program, unlike
`ToolRegistry::register_tool()`'s explicit `tool_provenance` + `outer_grant` check for non-native
sources. This revision introduces a distinct, deliberately inconvenient-to-misuse type:

```cpp
// Constructible only from something a host actually controls at deployment/startup time — a
// config value, an env var read at process start, a value baked into the host's own deployment
// code. NOT implicitly constructible from a bare std::string/string_view, so a call site that
// tries to pass a ToolResult, a ChatResponse field, or anything else that could plausibly trace
// back to model output has to go out of its way to do so — raising the bar, not (this is stated
// honestly, not oversold) cryptographically proving provenance the way a signed capability token
// would. A C++ function is host code by construction; this type exists to make an accidental
// model-output→backend-selection path visually and structurally awkward to write, not to make it
// impossible for a determined, careless integrator. See §3 for the honest limits of this mitigation.
struct HostSandboxSelection {
    explicit HostSandboxSelection(std::string name) : name_(std::move(name)) {}
    [[nodiscard]] std::string const& name() const noexcept { return name_; }
private:
    std::string name_;
};
```

`resolve_named()` (§1 Q3) is what a deployment's config file/env var/host code calls directly,
independent of the `Strict`/compile-time-`P` split entirely — e.g. a host reads
`AGENTENGINE_SANDBOX_BACKEND=microvm-kata` from its own deployment config and calls
`registry.resolve_named(HostSandboxSelection{that_string})` before ever touching
`register_agent<A>()`'s machinery. This is deliberately a **parallel, independent path**, not a new
`SandboxProfileArg` shape threaded through `SandboxProfile<P>` — `P` stays a compile-time
concept-conforming type or `Strict`, exactly as ADR-012 fixed it; this design does not reopen that
template-parameter-kind question.

## 3. I2/I3 check — backend selection stays routing, never authorization

Directly grounded in the Cloudflare Computer comparison research (`docs/research/2026-08-06-
cloudflare-computer-vfs-sandbox-comparison.md` §4): *"Backend selection is routing, not
authorization... The backend argument is never itself authorization."* This design preserves that
property structurally, not by convention:

- **`register_backend<B>()` is host-only, call-site-explicit, never auto-discovered** — same
  "nothing is ever added except by an explicit call the host itself makes" rule `ToolRegistry`'s
  own file banner already states for tools. No scan of a directory, no dynamic load, no
  self-registration from a plugin's own manifest.
- **`resolve_named()`'s input must be a host-controlled string** (a config value, an env var, a
  value the host's own deployment code chose) — this design does **not** propose any path from
  model output, tool-call arguments, or agent-authored content to `resolve_named()`'s parameter.
  That would be a straightforward I3 violation (model output selecting sandbox strength) and is
  explicitly out of scope/rejected, not merely unaddressed. **Revision 2, stated honestly**: §2c's
  `HostSandboxSelection` type raises the bar against an *accidental* violation (a careless future
  call site wiring a config value that itself traces back to conversation content) by making the
  construction site visually explicit — it does **not** cryptographically prove provenance the way
  a signed capability token does, because nothing in this design gives the registry a way to verify
  *who actually called* `resolve_named()`. This matches the trust tier `ToolRegistry` itself
  operates at ("HOST-CURATED ONLY... nothing is ever added except by an explicit call the host
  itself makes" — a documented contract enforced by code review and the call site being host code
  by construction, not a runtime credential check). The real ADR should decide whether that trust
  tier is sufficient here or whether sandbox-backend selection is security-sensitive enough to
  warrant a stronger mechanism (e.g., requiring `EffectContext`'s own principal to carry a specific
  host-only marker capability before `resolve_named()` accepts a call) — left as an open question,
  not silently resolved by adding the wrapper type alone.
- **`Strict` resolution changing process-wide when a new backend registers is a real, disclosed
  routing-drift risk, not an authorization bypass** (Revision 2 finding #4): §2a's `strict_eligible`/
  `named_only` split means a backend registered for one agent's explicit named selection can no
  longer silently win `Strict` for every other agent in the process. For backends that ARE
  registered `strict_eligible`, `resolve_strict()` now logs its resolution outcome (§2a) — an
  auditable record of what every `Strict`-configured agent actually got, closing the observability
  gap the security red-team flagged (routing choices changing silently is still a policy-relevant
  effect even when no single call is individually unauthorized).
- **A registered backend still passes through `check` at §9's gate bar regardless of how it was
  selected** — this design changes *which* backend a deployment picks, never *what standard* a
  registered backend must clear to be trustworthy at all (008 §2a's existing "third-party backend
  is host-trust-tier code" rule, unchanged).

## 4. Self-red-team pass (Revision 1 — superseded/confirmed by the real red-team, kept as record)

- **Does `resolve_named()` need its own fail-closed contract for an unknown name, the same as
  `resolve_strict()`'s `nullopt`-on-no-match case?** Yes — an unrecognized name must be a
  `result<...>` error (`sandbox_backend_registry.name_not_found`), never silently fall back to
  `Strict`'s resolution or to any default backend. A deployer who typoes a config value should get
  a startup failure, not a silently-different sandbox strength than they configured. *(Unchanged by
  Revision 2 — still correct, still the contract §2a states.)*
- **Does registering two backends under the same *type* but different names create a real hazard?**
  No — `RegisteredSandboxBackend::create/exec/destroy` are independent closures per registration
  entry; nothing shares mutable state between two registrations of the same `B` type (each gets its
  own `shared_ptr<B>` instance under Revision 2's fix). *(The `B{}`-per-call concern this bullet
  originally raised as "worth flagging" was independently confirmed as a real, blocking bug by two
  red-team agents, not merely a question — see Revision 2 findings #1/#2 above and §2a's fix.)*
- **Does this design let a deployer register the SAME backend type under Strict AND force it by
  name redundantly?** Yes, harmlessly — `resolve_strict()` and `resolve_named()` both read the same
  `entries_` map; nothing prevents (or needs to prevent) a deployer from also naming a backend that
  `Strict` would have picked anyway. *(Unchanged by Revision 2.)*
- **What breaks if this ships without also closing §0's named "consumption" gap?** Nothing new
  breaks — today's zero production `SandboxHandle` construction sites stay zero. This design is a
  precondition for a future consumption-wiring ADR (or for the microVM ADR itself, if it chooses to
  be the first real consumer), not a claim that shipping it alone makes Python/Shell execution
  route through a resolved backend. *(The architecture-fit red-team confirmed this reasoning is
  sound but flagged it as "needs a decision" given this codebase's own recurring
  selection-without-consumption pattern — Findings M/N in the component-role-audit tracker — not a
  free pass; the eventual ADR should carry an explicit, tracked follow-up item for consumption
  wiring rather than leave it implicit the way ADR-005's designs were left.)*

## 5. Relationship to the future microVM ADR

This registry is the seam a microVM (or Kata, or gVisor-`runsc`) backend plugs into: a
consumer-dev writes a type satisfying `SandboxBackend` (008 §2a, already real today), constructs
one instance however its own config needs (`std::make_shared<MyMicroVmBackend>(kata_socket_path,
...)`), calls `registry.register_backend("microvm", std::move(instance), strict_eligibility::eligible)`
once at host startup — deliberately choosing `named_only` instead if this backend is meant for one
specific high-risk agent's explicit selection rather than to compete for every `Strict`-configured
agent's resolution (§2a) — and every agent using `Strict` (if eligible) or naming `"microvm"` via
`HostSandboxSelection` picks it up, without editing or recompiling any individual agent's
`SandboxProfile<P>` declaration. The microVM ADR itself owns: which Linux isolation technology to
wrap (Kata/gVisor/raw Firecracker+jailer — `docs/research/2026-08-23-microvm-sandbox-backend-
landscape.md` names concrete tradeoffs for each, including a live 2026 jailer CVE worth weighing),
its own `ProfileTraits` values, whether its own backend type needs to guarantee the thread-safety
§2a now names as an explicit requirement for a shared-instance registration, and — per §0's
disclosed gap — whether it also takes on wiring real consumption (making some real execution path
actually call `registry.resolve_*()` and use what comes back), since nothing in this engine does
that yet for any backend.
