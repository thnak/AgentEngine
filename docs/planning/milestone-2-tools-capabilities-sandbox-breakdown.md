# Milestone 2 — Tools, capabilities enforced, sandboxed — work breakdown and kick-off

**Status:** Work breakdown (stage 4 of [the review-signoff workflow](v1-review-signoff-workflow.md)),
written just-in-time as this milestone starts, per that doc's §4. Scoped to
[the roadmap's](v1-implementation-roadmap.md) Milestone 2 exit criterion: *"an agent declares
`Tools<...>`, a capability-gated native tool call is enforced end to end, `native-jail` sandbox
parity (008 §9 G1) holds on Windows and Linux, and one WASM `ae:tool` component loads and executes
(009 §10 G1)."*

**RFCs:** 006 (Tool and Function Plane, gate §8), 007 (Capability and Trust Model — §3 enforcement;
core types already landed in M1, gate §9), 008 (Sandbox and Isolation — `native-jail` first, gate
§9), 009 (Plugin and Extension System — WASM Component Model ABI, gate §10), 002 (Agent Model and
Authoring — the CRTP surface, gate §8). All five are Reviewed (2026-08-05). This is the roadmap's
first milestone with a real RFC dependency cycle (006↔007↔008↔009); the roadmap's own build order —
capability enforcement → tool pipeline → `native-jail` → WASM plugin host, then 002's authoring
surface once there's something to author — is followed here.

Every phase below is explicitly narrower than its RFC's full promotion gate, the same discipline M1
used: gate items needing machinery this milestone doesn't build (019 durability, 023 baselined
budgets, 014 workflow composition, 015 declarative parity, real credentials/004) are named and
deferred, not silently dropped. See "What's explicitly deferred past M2" at the end.

## Current state (verified 2026-08-05, after M1)

| Item | State |
|---|---|
| `Capability`/`CapabilitySet` (`trust/capability.hpp`) | Vocabulary only. `Capability{kind}` has **no parameters** — exactly the "hole" 007 §3 property 5 warns against by the stub's own design comment. `capability_kind` is missing `net_listen`, `exec`, `schedule`, `background` (all in 007 §3's current Reviewed table, added after this stub was written). It has an `env_write` entry not in 007 §3's table at all (justified separately by ADR-001 §2.5's `export` mutation case) while 007 §3's own `Env<key>` (read one env var) has no entry yet. `CapabilitySet` has zero `grant`/`check`/`attenuate`/`revoke` methods — the header comment explicitly defers all of it to this milestone |
| `CapabilityRegistry`/`CapabilityRef`/`capability_token.hpp` | Cross-process bearer-token mechanism (ADR-005) — **already implemented, out of scope**. This milestone's gap is the in-process side only |
| `Tool<Derived>` (`core/tool.hpp`) | Empty CRTP base — no schema derivation, no ten-step pipeline (that's host machinery, correctly not modeled on the base itself) |
| `Agent<Derived, Policies...>` (`core/agent.hpp`) | CRTP base with 8 of 002 §3's 13 policy tags stubbed as empty types (`Concurrency`, `Retry`, `Memory`, `Middleware`, `Stateless`, `OutputSchema` missing). `Capabilities<capability_kind... Ks>` takes bare enum values — does **not** match 002/006's own examples (`Capabilities<NetOut<"api.search.example">>`, a host-parameterized template). `Agent` itself does nothing — no metadata compiler, no `register_agent` |
| `SandboxBackend` concept (`sandbox/sandbox.hpp`) | Matches 008 §2's contract shape closely (`SandboxSpec`'s six fields map 1:1). Missing `ProfileTraits` (needed for `Profile::Strict` resolution), `MountSpec`'s host-path/blob-store source field, and — same pattern as `ChatClient`/`Runner` — async `ae::task<T>` wiring, currently a synchronous stand-in |
| `Runner` concept / `ExecState` (`sandbox/runner.hpp`) | Exists, unused by any concrete `Runner` yet |
| `PluginManifest` (`plugin/plugin.hpp`) | Vocabulary only — missing signature/digest fields, entry-point declaration, and the `fs_read`/`net_out`/`secrets` capability-request breakdown 009 §3's manifest example shows (reuses the same under-parameterized `Capability`) |
| `wit/` | **README only — no `.wit` files exist.** 009's own header names this directory "the contract of record"; it's currently empty. Authoring the `ae:tool` world is a real, previously-invisible task this survey surfaced |
| `src/backends/native_jail/*` | ADR-001/002/004 prove-phase spike code (`ShellRunner`, `PythonRunner`, `job_object_limits`, `real_filesystem_adapter`, ADR-002's single-tier import allowlist). Per project-owner direction (2026-08-05): stays in place as historical evidence; **M2 is written fresh against 008 as currently Reviewed**, not layered on this. ADR-003's caller-aware dual-registry import-gating mechanism is Judged but has never been built as real C++ (only a standalone Python reproduction used for red-team/prove) — that gap is M3's (PythonRunner), not M2's, since M2's sandbox proof doesn't touch CPython at all (see decision 3) |
| `src/backends/wasm/`, `src/backends/remote/` | README only. No Wasmtime dependency wired into CMake yet. `remote/` is out of scope for M2 (deferred to M9) |
| Async (`ae::task<T>`) wiring | Still not wired anywhere in the codebase — `chat_client.hpp`, `runner.hpp`, `sandbox.hpp` all use synchronous stand-ins by explicit, tracked design, same as M1 |

## Design decisions made while breaking this down

1. **Capability *declaration* (compile-time) and capability *grant* (runtime) are two distinct
   things sharing one vocabulary, and the current stub only has half of each.** Resolution: each
   capability kind in 007 §3's table gets (a) a compile-time declaration tag — `FsRead<mount>`,
   `NetOut<host>`, `Secret<name>`, etc., following the `fixed_string`-template pattern `ChatClientId`
   already established, used in `Capabilities<Cs...>` at both `Tool` and `Agent` declaration sites —
   and (b) a runtime-parameterized struct carrying the actual granted instance (mount id + path
   prefix + size cap for `FsRead`; host/port/scheme allowlist + byte cap for `NetOut`; ...), held in
   `CapabilitySet` as `std::vector<std::variant<FsRead, FsWrite, NetOut, ...>>` (kind is the variant
   index, not a separate enum field once this lands — `capability_kind` stays only where a bare tag
   with no payload is genuinely all that's needed, e.g. logging/audit). The declaration tag and the
   runtime struct are related but not the same type: a tool's `NetOut<"api.search.example">`
   declaration is a fixed compile-time ceiling; the runtime grant a session actually holds can be
   narrower (attenuated) but never wider.
2. **`ae::task<T>` stays deferred for M2 too.** None of this milestone's scoped-in gate items need
   real coroutine concurrency to be proven — a native tool call, a `native-jail` exec, and a WASM
   component invocation are all provable synchronously, the same way M1 proved 001 §3's turn loop
   synchronously. 006 §5's parallel-batch determinism gate (G4) needs concurrent overlap to mean
   anything and is explicitly deferred (see below) — that is the actual trigger for wiring
   `ae::task<T>` for real, not this milestone.
3. **`native-jail`'s M2 proof target is the raw `SandboxBackend` contract via a minimal built-in
   probe program, not the Python interpreter.** 010 (Python Code Interpreter and Shell) isn't
   scheduled until M3, and ADR-002/003's import-gating mechanism is specifically about mediating
   CPython's `import` — mixing that into M2 would pull a whole RFC's scope forward. `008 §2`'s
   `ExecRequest{language, source}` doesn't require `"python"`/`"shell"` specifically; M2 exercises
   `create`/`exec`/`destroy` and the abuse-case list (§7) with small compiled probe binaries (fork
   bomb, OOM, infinite loop, fs-escape attempt, unbounded output) that need no scripting language
   runtime at all. `PythonRunner`/`ShellRunner` (010 §1a) become real `Runner`s that plug into this
   same `SandboxBackend` in M3, unchanged.
4. **007 §5's full declarative policy DSL (`match {...} decide ...`) is out of scope for M2.** The
   roadmap's own build-order line names only "`Capability` enforcement plumbing (007 §3's
   empty-by-default, attenuation-only rules)" for this milestone, and separately flags the
   policy-reachability tool (§9 G6) as new, not-yet-built CI tooling — meaning the policy language
   itself isn't assumed to exist by M2. This milestone implements **mechanical possession/attenuation
   enforcement only**: does the held `CapabilitySet` cover what a tool call requires, checked
   directly, no rule-matching language, no policy file format. The declarative layer becomes its own
   future task once a real rule-authoring need exists.
5. **Wasmtime 47.0.3 (OQ-7) enters the build as a new CMake-optional seam-backend dependency**
   (`AGENTENGINE_WITH_WASM`, off by default like any CONVENTIONS tier-2 heavy dependency), Windows +
   Linux. The `wasm` profile's core contract (`SandboxBackend`) stays std+Quark-only; only the
   concrete backend under `src/backends/wasm/` links Wasmtime.
6. **The two ADR-track items the roadmap explicitly flags for M2** — the first-party egress proxy
   (008 §10 Q3) and the policy-reachability tool (007 §9 G6) — go through
   design→red-team→prove→judge per CLAUDE.md, each producing its own ADR, rather than being folded
   into an ordinary task. Sized and listed separately below (Phase F).
7. **The duplicate `approval_mode`/`approval_policy_mode` enum** (`tool.hpp` vs. `agent.hpp`,
   identical enumerators) collapses to one. 006 §4 owns `Approval`'s semantics (`never_require` /
   `always_require` / `PolicyDriven`); `tool.hpp`'s `approval_mode` is kept as the one definition,
   `agent.hpp`'s policy tag reuses it — one concept, one name, same precedent M1 set for
   `StartRun`/`AgentResponse`.
8. **`capability_kind` gains `net_listen`, `exec`, `schedule`, `background`** to match 007 §3's
   current table, and keeps `env_write` as a documented, locally-justified addition beyond that table
   (ADR-001 §2.5's `export`-mutation case — reading one var and mutating `ExecState.env` are
   different authorities and 007 §3's `Env<key>` only covers the read case). `env_read` is added
   separately to actually cover `Env<key>` as the RFC names it, closing that gap rather than
   conflating it with `env_write`.

## Tasks, in the roadmap's own build order

### Phase A — Capability enforcement plumbing (007 §3)

- **A1 + A3 (done, merged — see [ADR-009](../../decisions/ADR-009-capability-set-enforcement-mechanism.md)).**
  Mid-breakdown correction: A3 turned out to be exactly the invariant-touching case CLAUDE.md and
  the [sign-off workflow](v1-review-signoff-workflow.md) §3 require the full
  design→red-team→prove→judge cycle for — the pre-M2 stub's own header comment said as much
  ("goes through design -> red-team -> prove -> judge and an ADR before it is real code"), and A3
  is literally what makes I2 true in-process, not just documented. Run as a full ADR (not deferred
  to Phase F) because the parameterized representation (A1) and its enforcement (A3) turned out to
  be one inseparable design, not two: you cannot design attenuation-checking without first deciding
  what a capability's parameters look like. **Result:** `cap::` variant (16 per-kind structs) +
  checked `attenuate()` + shared-ticket `BoundCapability`/`revoke()`, proven via
  `tests/test_capability_enforcement.cpp` (27/27 checks, positive-control-bearing) on Windows
  (MSVC + ASan, zero findings) and Linux (Docker, gcc-14). A real `agentengine::ToolCall` naming
  collision against `content.hpp` was found via a live build failure and fixed (per-kind structs
  moved to `agentengine::cap`). Miniature G1/G3/G4 scoped as originally planned — not the full
  fuzzed/randomized-workload gates.
- **A2 (done).** Compile-time capability declaration tags — `trust::cap::decl::{FsRead, FsWrite,
  NetOut, NetListen, Secret, ToolCall, RunnerCall, Exec, Clock, Entropy, EnvRead, EnvWrite,
  AgentCall, Schedule, Background, Elicit}`, one per ADR-009's runtime `cap::*` kind, parallel to
  `ChatClientId<fixed_string>`. `fixed_string` extracted from `core/agent.hpp` into its own
  `core/fixed_string.hpp` so `trust/capability.hpp` (a lower tier) can use it without an include
  cycle. `agent.hpp`'s `Capabilities<Cs...>` now takes these tag types (was bare `capability_kind`
  values, which couldn't express a host/mount at all) — matches 002 §2 / 006 §1's own
  `Capabilities<NetOut<"api.search.example">>` examples literally. `to_capability()` converts a tag
  into the real runtime `Capability` a `CapabilitySet` checks against (needed because `cap::NetOut`
  etc. carry a `std::vector`, so aren't structural types and can't themselves be NTTPs — the decl
  tags are the necessarily-simpler structural stand-in). Proven in
  `tests/test_capability_declaration_tags.cpp` (9/9 checks — declaration-site compile, NetOut/
  FsRead/ToolCall/AgentCall round-tripping to the correct runtime grant, including the AgentCall
  case reusing ADR-006's `SpawnBudget` for its depth parameter) on Windows and Linux (Docker,
  gcc-14). Ordinary task, not ADR-track: this is declaration-surface plumbing, not itself an
  enforcement decision — the enforcement it feeds into is ADR-009's, already judged.
- **A4 (done).** Configure-time `try_compile()` compile-fail proof, mirroring M1's `Tainted<T>`
  gate exactly: `tests/compile_fail/capability_set_no_direct_construction.cpp` (MUST NOT compile —
  `CapabilitySet set{some_capability};`, since there is no such constructor and `CapabilitySet` is
  not an aggregate) paired with `capability_set_grant_root_positive_control.cpp` (MUST compile —
  the same construction via the real `grant_root()` entry point). Verified load-bearing, not
  vacuous, the same way M1's gate was: temporarily adding a `CapabilitySet(Capability)` constructor
  made the negative file compile and the gate correctly fired `FATAL_ERROR` on reconfigure; reverted
  immediately after confirming. Confirmed on both Windows (MSVC) and Linux (Docker, gcc-14).

### Phase B — Tool pipeline (006 §3) with a trivial native tool

- **B1 (done).** `Tool<Derived, Policies...>` real CRTP base plus compile-time JSON-Schema-shape
  derivation from nested `Args`/`Reply` (006 §1), emitted as JSON Schema 2020-12. C++23 has no
  compile-time reflection (P2996 is C++26), so field *names* can't be pulled from a struct
  definition alone — the RFC's own macro-free `Args`/`Reply` example is aspirational until C++26;
  every schema-bearing type instead pairs its definition with `AE_JSON_SCHEMA(Type, member...)`
  (`core/json_schema.hpp`), reusing Quark's `QUARK_FOR_EACH` variadic-expansion macro directly
  (006 §1's own "Quark 016's one-describe discipline" citation) rather than re-deriving the same
  preprocessor machinery. Field types are read via `decltype(declval<Type&>().member)` — no
  instance is constructed, so Args/Reply need no default constructor. `std::optional<T>` fields are
  excluded from `"required"`; `std::vector<T>` maps to `{"type":"array","items":...}`; a nested
  `AE_JSON_SCHEMA`-described struct recurses (found via ADL, the same lookup shape
  `quark_describe` uses) rather than flattening or stringifying. `Tool<Derived,
  Policies...>::args_schema()/reply_schema()` route to exactly this. Proven in
  `tests/test_tool_json_schema.cpp` (parses the emitted string with nlohmann::json and asserts on
  real structure — primitive types, required/optional split, vector-of-nested-object, and that
  `Tool`'s methods match `schema::json_schema_of<T>()` exactly) on Windows and Linux (Docker,
  gcc-14). Cross-platform Linux Docker verification for this task also surfaced a pre-existing,
  unrelated gap: `tests/test_real_filesystem_adapter.cpp`'s case-fold-consistency check
  (ADR-001-era, M1) assumes a case-insensitive filesystem and a Windows junction (`cmd /c mklink
  /J`) and fails/wouldn't run on Linux — not a regression from this task, but apparently never
  previously run against a real Linux filesystem; tracked for Phase C's cross-platform parity work
  (C4) rather than fixed here (out of Phase B's scope, and `native-jail`'s real Linux backend
  doesn't exist yet — C2). Ordinary task, not ADR-track: schema derivation is neither
  security-critical nor hot-path.
- **B2 (done).** The ten-step pipeline — `resolve → validate → taint → authorize → approve → admit →
  bind → invoke → normalize → account` — as real host machinery (`core/tool_pipeline.hpp`), NOT yet
  wired into `AgentSession`'s turn loop (that wiring is real `ChatClient`/004 tool-calling
  integration, deferred past M2 — `chat_client.hpp`'s own comment already flags 004's real seam as
  not due until Milestone 5; `invoke_tool()` is directly callable and exhaustively tested instead).
  Building this surfaced that `Tool`/`Agent` needed real policy-tag plumbing that didn't exist yet:
  `Capabilities<Cs...>` moved out of `agent.hpp` into a new shared `core/policy_tags.hpp` (used at
  both Tool and Agent declaration sites, per 006 §1); `agent.hpp`'s duplicate `approval_policy_mode`/
  `Approval<M>` collapsed into `tool.hpp`'s canonical `approval_mode`/`Approval<M>` (breakdown
  decision 7, executed here); `tool.hpp` gained `Parallelizable` and `Timeout<Ms>` tags (`Ms` a
  plain integer, not a `std::chrono` NTTP — duration types typically keep `rep` private, which
  disqualifies them as C++20 structural types, the same constraint ADR-009 hit for `cap::decl::*`);
  and `Tool<Derived, Policies...>` gained `declared_capabilities()`/`declared_approval()`, reading
  its own policy pack via partial-specialization extraction. A dependency-free `core/json_value.hpp`
  (`Value` + recursive-descent `parse()`/`dump()`) was added because `agentengine::core` cannot link
  nlohmann::json (CONVENTIONS.md's dependency-tier discipline keeps it test-only) but the pipeline's
  steps 2/9 need real JSON (de)serialization in product code, not just schema-shape derivation,
  proven in `tests/test_json_value.cpp`. `core/json_schema.hpp`'s `AE_JSON_SCHEMA(Type, member...)`
  macro (B1) was extended — same one field list, no duplication — to also generate `ae_to_json`/
  `ae_from_json` round-tripping a real instance through `json::Value`: `std::optional<T>` fields are
  omitted from the JSON object when absent (never emitted as `null`) and accept either an absent key
  or an explicit `null` on the way in; a present-but-wrong-typed field is rejected, never coerced
  (006 §3 step 2); proven in `tests/test_json_schema_codec.cpp`. Scoping decisions made explicit in
  the pipeline header itself: step 3 (taint) is tracked as one bool stamped onto the result's
  `ContentItem`, not deep per-field `Tainted<T>` propagation into every `Args` member (003/006 don't
  specify that granularity); step 5 (approve) auto-approves `never_require`, calls the injected
  `ApprovalDecider` over the call's canonical JSON for `always_require`, and fail-closed degrades
  `policy_driven` to the same decider until 007 §5's rule language exists (decision 4); step 6
  (admit — Quark 022) is a documented no-op, not in the M2 build order; step 7/4 (authorize+bind) are
  one call into ADR-009's `CapabilitySet::bind()`, which already performs both atomically; step 8's
  deadline is checked only at the call boundary (not preemptible mid-call without real coroutines,
  decision 2). `EffectContext` gained a `bound_capabilities` field (this call's freshly minted,
  step-10-revoked handles, distinct from the long-lived `capabilities` pointer) so a tool can
  actually reach the handles step 7 describes. **XL**, the mechanism the whole milestone's exit
  criterion hangs off — proven end to end in `tests/test_tool_pipeline.cpp` (below) on Windows and
  Linux (Docker, gcc-14).
- **B3 (done).** One trivial native tool (`EchoTool`, gated by a bare `Entropy` capability) plus a
  second tool (`GatedTool`, `Approval<always_require>`) proving both authorize and approve paths —
  miniature 006 §8 G2's covered subset: capability held → succeeds; unknown tool name → contract
  error naming the tool; missing required argument → rejected, not coerced; capability not held →
  policy error that leaks neither which capability was checked nor what's held; approval denied →
  blocked, decider actually consulted; approval granted → decider sees the exact call's
  canonicalized arguments (006 §4's binding). "`TestKit`-driven" in this task's original wording
  assumed tool-calling was already wired into `AgentSession`'s turn loop — B2's note above explains
  why that wiring is out of scope for M2; the pipeline function itself is proven directly instead,
  which is what's actually testable at this milestone's real scope. `tests/test_tool_pipeline.cpp`,
  Windows + Linux (Docker, gcc-14).
- **B4 (done).** Capability-handle-reuse-denial, at the PIPELINE level (ADR-009's
  `test_capability_enforcement.cpp` already proves the primitive in isolation; this proves
  `tool_pipeline.hpp` actually wires it end to end). A `StashingTool` copies its own per-call handle
  out of `EffectContext::bound_capabilities` during `invoke()` — a positive control confirms the
  handle is genuinely live and usable *during* its own call (`.use()` succeeds) before the pipeline
  revokes it at step 10; the stashed copy is unusable immediately after `invoke_tool()` returns, and
  is *still* unusable after a second, later call has run (no resurrection, no cross-call reuse) —
  the literal "a capability handle from call n is unusable in call n+1" (006 §8 G3). Verified
  load-bearing the same way M1's `Tainted<T>` gate and A4's compile-fail gate were: temporarily
  commented out the step-10 `revoke()` call, confirmed the test's three post-call assertions
  correctly failed, reverted, confirmed pass again. `tests/test_tool_pipeline_capability_reuse.cpp`,
  Windows + Linux (Docker, gcc-14). **S**, as sized.

### Phase C — `native-jail` sandbox (008), Windows + Linux

**Process note (2026-08-05, before C2 started).** `decisions/README.md`'s rule requires a full
`design → red-team → prove → judge` ADR cycle for any "isolation boundary" choice, and ADR-004 (the
Windows AppContainer + Job Object design C2 builds on) is explicitly self-described as "Spiked, not
Judged" — its own §11 lists "a real, adversarial red-team pass on this design... before any claim
here is treated as more than a spike" as still outstanding. Raised explicitly with the project owner
before C2's implementation started; **decision: proceed as this doc already scoped it** — C2 is an
ordinary implementation task carrying forward ADR-004's findings, verified by this phase's own
build+test+gate work (C3–C6), not a separate ADR cycle. ADR-004 itself is not thereby Judged; it
remains a spike, cited as such everywhere C2's writeups reference it.

- **C1 (done).** `SandboxBackend` contract completed (`sandbox/sandbox.hpp`) — still synchronous per
  decision 2. `ProfileTraits{strength, platform_mask, cold_start}` added as 008 §2's "static
  constexpr ProfileTraits traits" line, plus the concept requirement that every conforming backend
  actually declare one (`{ T::traits } -> std::convertible_to<ProfileTraits const&>`), not just the
  three methods. `platform_id` (a two-flag bitmask, `windows_x86_64`/`linux_x86_64` — 021 §2, macOS
  excluded by construction) and `cold_start_class` (008 §3's Cold-start column as a closed set, not
  a number — 023 stays `TBD-baselined`) back it. `resolve_strict()` implements 008 §3's
  `Profile::Strict` rule literally over a `std::span<ProfileTraits const>`: highest `strength`
  among candidates supporting the current platform, ties broken toward the wider `platform_mask`
  (more platforms = "broader, more-proven"); returns `std::nullopt` when nothing supports the
  current platform, leaving "no fallback -> startup fails" to its caller rather than enforcing it
  itself — no concrete backend exists yet to resolve for real (that's C2/D1's job), so this is
  proven against synthetic traits. `MountSpec::source` is now `std::variant<std::string /*host
  path*/, BlobRef>` (008 §2's "host path or blob store"), reusing `core/content.hpp`'s `BlobRef`
  (003 §3) rather than inventing a second digest+store vocabulary — no existing caller constructed
  `MountSpec` yet, so this was a pure addition, not a breaking change. `smoke_vocabulary.cpp`'s
  `DummySandboxBackend` updated to declare `traits` (the concept now requires it).
  `tests/test_sandbox_backend_contract.cpp` (8 checks: `resolve_strict`'s strength/platform/tie-break
  cases including the "nothing supports this platform" case, `MountSpec::source` holding each
  alternative) on Windows (MSVC) and Linux (Docker, gcc-14) — both green. Full suite also run both
  platforms: 22/22 on Windows; 17/18 on Linux, the one failure being `test_real_filesystem_adapter`'s
  already-tracked case-fold-consistency gap (Phase B's B1 writeup; explicitly assigned to C4, not a
  regression from this task). Ordinary task, not ADR-track: contract-shape plumbing, no isolation
  logic. **S**
- **C2 (done, both platforms).**
  `NativeJailBackend` (`src/backends/native_jail/native_jail_backend.{hpp,cpp}`,
  `app_container_profile.{hpp,cpp}`) — written fresh (not extending the ADR-004 spike code, which
  does not survive in the repo), carrying forward that ADR's *findings*: AppContainer (zero granted
  capabilities, `PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`) for process identity/authority, one
  profile reused across sessions (`ensure()` is idempotent — `ERROR_ALREADY_EXISTS` treated as
  success); `JobObjectLimits` (already built) for `memory_bytes`/`pids` (reliable) and `cpu_ms`
  (best-effort, documented as such in this file's own header, not silently trusted); the wall-clock
  watch as the actual trusted timeout mechanism (ADR-004 §10.5). Process launch: `CREATE_SUSPENDED`
  → `AssignProcessToJobObject` → `ResumeThread`, so every `ResourceLimits` axis applies before the
  guest's first instruction runs; separate stdout/stderr pipes (not merged, unlike the test-only
  `hostile_child` pattern) drained with a bounded read (`output_bytes`, or a 16 MiB safety floor if
  unset) so unbounded guest output cannot itself DoS the host (008 §2 item 2). `ExecOutcome`
  classification: `wall_clock_timeout` → `timeout`; exit 0 → `ok`; nonzero exit is split `oom` vs.
  `crash` by a peak-job-memory-vs-cap heuristic (Job Objects give no completion-port-free way to
  distinguish the two — documented as an honest approximation, not a precise signal, citing
  ADR-004 §9.3/§10.4's still-open "why" gap). `policy_violation`/`escape_attempt` are not produced
  by this layer yet — no mechanism here generates them; that is 010's interpreter-level mediation,
  M3.
  Decision made explicit in the file's own header: `ExecRequest::source` is, for M2 only, a
  fully-resolved Win32 command line (per decision 3 — the exec target is a compiled probe, not
  Python/shell) that the *caller* is trusted to have already resolved from a name; this backend
  does not itself mediate `language`/`source`, matching 008 §1b layer 2's framing that name
  resolution is a Runner/Tool registry's job, not the sandbox backend's. `MountSpec::source` as a
  `BlobRef` fails closed with a named policy error (unscoped — materializing a blob-store mount
  into a live filesystem grant is new work this task does not attempt), not silently ignored.
  `tests/helpers/hostile_child.cpp` gained a `fail <code>` mode (a clean nonzero exit unrelated to
  any resource limit) to give the crash/oom split a positive control.
  `tests/test_native_jail_backend_windows.cpp`: create() against a real mount + limits; exec()
  reporting `ok`/`timeout`/`crash`/`oom` against real child processes under real AppContainer +
  Job Object isolation; exec() on a destroyed handle fails closed. All pass on Windows (MSVC).
  Full suite: 23/23 on Windows; Linux build/test confirmed the new code is cleanly `WIN32`-gated
  (no native_jail_backend/app_container symbols built at all) with the same pre-existing C4-tracked
  `test_real_filesystem_adapter` gap as C1, no new failures. Per-owner direction (see this doc's
  process note above): proceeds as an ordinary implementation task carrying forward ADR-004's
  findings, not a fresh ADR cycle — ADR-004 itself remains a self-described spike, not Judged.

  **Linux half.** `LinuxNativeJailBackend` (`linux_native_jail_backend.{hpp,cpp}`) — namespaces
  (`clone()` with `CLONE_NEWPID|CLONE_NEWNET|CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWIPC`, atomically, not
  `unshare()` from the host thread, which would leak namespace changes across this backend's other
  calls) + cgroups v2 (`cgroup_limits.{hpp,cpp}`) + a real hand-built seccomp-BPF denylist
  (`seccomp_filter.{hpp,cpp}`, no libseccomp dependency — ptrace/mount/unshare/setns/kernel-module/
  reboot-class syscalls denied, `execve` deliberately NOT denied since the filter must survive its
  own installing thread's later `execve` into the guest, with an `AUDIT_ARCH_X86_64` check first to
  close the classic 32-bit-compat-syscall-table bypass) + `PR_SET_NO_NEW_PRIVS`. Same scope parity
  with the Windows half: `ExecRequest::source` is `/bin/sh -c <source>` (the Linux analogue of "a
  fully-resolved command line the caller already resolved from a name"); `MountSpec::source` as a
  `BlobRef` fails closed; full filesystem isolation (pivot_root/bind-mount jail) is explicitly NOT
  attempted (`CLONE_NEWNS` gives a private mount-table copy, not a restricted view) — deferred to
  010's interpreter-level mediation on both platforms alike, M3.

  Process launch: `clone()` into the new namespaces with the child immediately blocked on a
  sync-pipe read; parent adds the child to its cgroup (or kills it, fail-closed, if that fails)
  *then* closes the pipe to release it — same "before the guest's first instruction runs" ordering
  as the Windows side's `CREATE_SUSPENDED`. `ExecOutcome` classification needed real,
  environment-measured iteration, not assumption: cgroups v2's `memory.max` is NOT uniformly a hard
  kill — measured directly running the identical 512 MB-request-against-32 MB-cap workload
  repeatedly, three real, distinct outcomes surfaced across runs before the mechanism was fully
  understood: (a) a genuine OOM-kill (SIGKILL, kernel-forced because a page fault cannot fail back
  to userspace the way a syscall can); (b) a clean `ENOMEM` from the allocator's own `mmap()` call,
  surfacing as an unhandled `std::bad_alloc` → `SIGABRT`; (c) **the run silently succeeding via swap
  overflow** — `memory.max` alone caps resident usage only, and this backend was not also capping
  `memory.swap.max`, so the kernel would spill into swap rather than deny the guest, a real
  correctness gap now fixed (`memory.swap.max=0` written alongside `memory.max`, best-effort since
  some kernels don't expose the file). A fourth wrinkle, also measured: the guest is PID 1 of its
  own `CLONE_NEWPID` namespace, and the kernel reports a namespace-init process's signal death to
  its parent as an *ordinary exit* with code 128+signal (`WEXITSTATUS==137`), not `WIFSIGNALED`/
  `WTERMSIG==SIGKILL` — a documented Linux behavior, not a bug in the wait loop. Final
  classification checks four corroborating signals (the `memory.events` `oom_kill` counter — can
  read stale immediately after `waitpid()`, so never trusted alone; `WIFSIGNALED`+`SIGKILL`; the
  namespace-init 128+signal exit-code form; a peak-usage-vs-cap fallback for the clean-`ENOMEM` case,
  same shape as the Windows heuristic) rather than any single one. This produced a genuine, useful
  measurement 021 Q2 and ADR-004 §10.5 both named as missing (“no cgroups v2 counterpart to compare
  against yet”): once `memory.swap.max=0` closes the swap escape hatch, cgroups v2's OOM-kill fires
  reliably (6/6 clean repeated runs after the fix) but with real, variable latency in this
  environment (~2-8s observed, kernel reclaim-before-OOM-kill dance) — categorically slower than
  Windows' Job Object memory limit (14-22ms, ADR-004 §10.2), a real cross-platform latency
  asymmetry, not a flaw in either backend.

  `tests/helpers/hostile_child_posix.cpp` (POSIX port of `hostile_child.cpp`, same modes including
  `fail`) and `tests/helpers/cgroup_v2_test_setup.sh` (test-only bootstrap of the cgroup v2
  delegation `linux_native_jail_backend.hpp` states as a deployment precondition — must be
  `source`d, not executed as a subprocess, or the invoking shell's own new child process becomes an
  additional un-movable root-cgroup resident; a plain `gcc:14` container with no init also
  accumulates unreaped zombies from prior build steps that permanently block root's
  `cgroup.subtree_control` write, fixed by `docker run --init`, not by more script logic — both
  measured directly, not theorized). `tests/test_native_jail_backend_linux.cpp`: same shape as the
  Windows test (create/exec/destroy, `ok`/`timeout`/`crash`/`oom`, fail-closed on a destroyed
  handle) plus a bulk-read fix in the setup script itself (bash's `read` builtin does small/
  byte-at-a-time reads against `cgroup.procs`, a kernel seq_file, and was observed to silently stop
  after the first entry — `cat`/command-substitution's bulk read is what actually works). Gated
  behind `AGENTENGINE_LINUX_SANDBOX_TESTS` (default OFF) since it needs `CAP_SYS_ADMIN` and a
  delegated cgroup v2 root, not assumed available on an ordinary CI runner. 6/6 repeated clean runs
  after the swap-cap fix; full suite 18/19 on Linux (the one failure being C4's already-tracked
  `test_real_filesystem_adapter` gap); Windows full suite reconfirmed unaffected, 23/23.
- **C3 (done, Windows fs-escape / Linux deferred).** The systematic 008 §7 abuse-case corpus, scoped
  to what's buildable without 010 (decision 3) — fork bomb, OOM, infinite loop → `wall_ms` kill,
  fs-escape attempt, unbounded output — as the §9 G2 gate itself: every case contained, with a
  measured kill/detection time, and a positive control (limits deliberately disabled) that
  demonstrably succeeds, so the contained result isn't vacuous. C2's own tests already proved
  create/exec/destroy works; this is the abuse-case gate layered on top, in two new files
  (`tests/test_native_jail_abuse_corpus_{windows,linux}.cpp`) plus two new hostile-child probe
  modes (`escape <path>`, `flood`) added to `tests/helpers/hostile_child{,_posix}.cpp`.

  **Fork bomb (`spawn N self`, pids-limited).** Windows measured finding, not anticipated going in:
  the contained case returns `succeeded=0` *regardless* of `ResourceLimits::pids` — confirmed
  directly by first writing the positive control as "same probe, pids disabled" and watching it
  still return 0. Root cause: `native_jail_backend.cpp`'s `exec()` unconditionally sets
  `PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED` (008 §4
  "Exec (nested): denied"), which fails `CreateProcess` for any child the guest tries to launch
  before the Job Object's `ActiveProcessLimit` is ever consulted. So on Windows, fork-bomb
  containment is the nested-exec-denial policy, not the `pids` axis — `pids` is defense-in-depth for
  a path this backend doesn't currently expose. The real positive control ended up being: launch the
  identical probe fully unsandboxed (`sandbox_profile::none`, via a small `run_raw_unsandboxed()`
  helper added to the test — plain `CreateProcessA`, no AppContainer, no Job Object), which succeeds
  10/10. Linux has no equivalent global nested-exec denial (the seccomp filter deliberately does NOT
  deny `clone`/`fork`/`execve` — it has to survive its own later `execve`), so Linux fork-bomb
  containment genuinely is the `pids.max` cgroup limit: contained case measured `succeeded=1` of 40
  requested (pids=3), positive control (pids disabled) `succeeded=15` of 15 requested — the two
  backends contain the identical abuse case through two different mechanisms, both real, both now
  measured.

  **OOM, infinite loop.** Same shape as C2's own tests but now with an explicit positive control
  each: `memory_bytes`/`wall_ms` disabled (or, for `wall_ms`, substituted with a materially longer
  budget — see below) makes the identical workload succeed/run longer, proving the contained result
  isn't incidental. Measured Windows kill times: OOM ~17-21 ms, infinite-loop timeout 505-510 ms
  against a 500 ms budget. Linux: OOM ~106 ms (well under the 25 s budget C2-Linux's own measured
  worst case required), infinite-loop 527 ms against 500 ms.

  A literal "wall_ms disabled" positive control is unsafe to run (`spin` never exits on its own, and
  `wall_ms==0` means wait indefinitely — CLAUDE.md Machine Safety forbids a test that can hang
  forever) — the safe stand-in used on both platforms: run the identical `spin` workload under a
  materially longer wall_ms (1800 ms vs. 500 ms) and confirm it runs the full extra distance rather
  than finishing early on its own, which is what would happen if the short kill above were
  incidental rather than real enforcement. Documented in both test files as an explicit, reasoned
  substitution, not a silently weaker check.

  **fs-escape attempt (Windows only this pass).** Building the Linux side of this case surfaced a
  real, previously-undocumented gap: `LinuxNativeJailBackend`'s `CLONE_NEWNS` gives the guest a
  private mount namespace but nothing populates it with a restricted view (no
  `pivot_root`/`chroot`/bind-mount jail) — the guest can read/write anything the invoking user can,
  anywhere on the host. Raised to the project owner before writing a test that would either fail
  honestly or require building real containment first; the decision (2026-08-05) was to defer real
  Linux fs containment and scope C3's Linux half to the four cases that are genuinely contained
  today, leaving fs-escape-attempt Windows-only. Documented in 008 §7 (a new gap bullet alongside
  the existing Windows ACE-leak one) and tracked as
  [GitHub issue #5](https://github.com/thnak/AgentEngine/issues/5). On Windows: a file outside the
  granted mount reports `ESCAPE_DENIED` (measured ~9-10 ms); the positive control — the same backend
  mechanism, deliberately granting the secret directory too (AppContainer's equivalent of "disable
  the limit") — reports `ESCAPE_OK`. The known ADR-004 §6.1 gap (`win.ini` readable via the
  OS-default `ALL APPLICATION PACKAGES` ACE regardless of any grant) is asserted explicitly too, not
  silently skipped, so a future ACL-mediation fix shows up as a diff here.

  **Unbounded output.** A `flood` probe (writes continuously, never exits) proves
  `ResourceLimits::output_bytes` truncates captured stdout to exactly the configured cap (4096
  bytes, both platforms) regardless of how much the flood actually wrote before being killed by
  `wall_ms`. Positive control: the same probe under a materially looser cap (2 MiB — never literally
  "unbounded"; `drain_pipe_bounded`'s own 16 MiB safety floor deliberately forbids that even when
  `output_bytes==0`, since unbounded stdout is itself a host-safety hazard, 008 §2 item 2) captures
  far more (~1 MiB in the time available) — proving the tight cap above is real containment, not the
  flood incidentally producing little output. This surfaced a genuine architecture question in both
  backends: `exec()` drains pipes only AFTER `wait_or_kill`/the wait loop returns, not concurrently,
  so a flood child blocks in `write()` once the kernel pipe buffer fills, and the buffered amount at
  kill time depends on that buffer's size, not on the flood's true output rate. Fixed by explicitly
  sizing both platforms' pipe buffers to 1 MiB (`CreatePipe`'s `nSize` on Windows, `fcntl(...,
  F_SETPIPE_SZ, ...)` on Linux, best-effort there) rather than relying on the OS default (historically
  a few KB on Windows, ~64 KiB on Linux) — otherwise the positive control's "materially more" claim
  would be bounded by an incidental kernel default, not a meaningful signal.

  Full regression check after all of the above: Windows 24/24 (was 23/23 before C3 added its own
  test); Linux 19/20 (still only the pre-existing, already-tracked `test_real_filesystem_adapter`
  case-fold gap — C3 added a 20th test and it passed clean).
- **C4 (done).** Cross-platform parity proof (008 §9 G1) — the same probe corpus on Windows and
  Linux (Docker, the established M0/M1 verification pattern), same outcome classification. Named
  directly in the roadmap's exit criterion, not optional.

  G1's own text is specific: "**one** hostile test corpus runs against every available backend on
  every platform... and produces the same outcome classification for every case." Made that a
  structural fact, not an assertion that happens to hold because two independently-written test
  files were kept in sync by hand: `tests/helpers/abuse_case_corpus.hpp` is ONE array (case name,
  probe args, `ResourceLimits`, expected `exec_outcome_class`) included verbatim by both
  `tests/test_native_jail_parity_windows.cpp` and `tests/test_native_jail_parity_linux.cpp`, each
  compiled against its own platform's `SandboxBackend`. Five cases — well-behaved, ordinary
  failure, infinite loop, OOM, unbounded output — chosen because they're exactly the ones that
  reduce to a single `exec_outcome_class` comparison; C3's fork-bomb (a succeeded-count, not a
  classification) and fs-escape (an ESCAPE_OK/DENIED string, Windows-only per the tracked Linux gap)
  stay in C3's own corpus, where "semantics identical, strength documented" (G1's own qualifier) is
  the more honest frame for two backends that contain the same abuse through different mechanisms.
  Result, both platforms, all five cases: `well_behaved→ok`, `ordinary_failure→crash`,
  `infinite_loop→timeout`, `oom→oom`, `unbounded_output→timeout` — identical classification, zero
  mismatches. `wall_ms` is deliberately NOT part of the shared table (ADR-004 §10.5 and C2/C3's own
  measurements already established this axis has large, real, platform-specific latency — Windows
  Job Object memory limit ~14-22ms vs. Linux cgroups v2 OOM-kill ~2-8s for the identical workload);
  each platform's consuming test supplies its own `wall_ms`, with Linux's OOM case getting the same
  25s budget C2-Linux's own measurements required. Baking one number into the "shared" part would
  either be needlessly tight for Windows or flaky on Linux — the actual G1 claim (same probe, same
  classification) doesn't need it to be shared.

  **The long-tracked `test_real_filesystem_adapter` case-fold gap, closed.** Root cause: the test's
  own assertion, not the adapter. ADR-001 §2.5.5 finding 9 requires "the capability-scope comparison
  and the actual I/O call must operate on the same canonicalized, case-folded representation" —
  internal consistency between `RealFileSystemAdapter`'s escape-boundary check and its own real I/O
  call, which `real_filesystem_adapter.cpp`'s `is_within_root`/`generic_lower` already provide
  correctly on both platforms. The test instead asserted that the *OS's file lookup itself* is
  case-insensitive — true on Windows/NTFS (this project's own near-term target), false on Linux's
  default ext4, which is case-sensitive. Fixed by making the assertion platform-conditional: Windows
  still expects a differently-cased path to resolve to the same file (real NTFS behavior); Linux now
  expects the opposite (a differently-cased path is a genuinely different, nonexistent file) — proving
  the adapter doesn't invent its own case-folding for actual file lookups and silently diverge from
  whatever the real OS does, which is the divergence finding 9 actually warns against. `decisions/
  ADR-001-shellrunner-grammar-and-dispatch.md`'s own recorded prove-phase transcript (§8.9) is Windows-
  only historical evidence and was left untouched — this is a live-test fix, not a retroactive edit
  to a frozen record.

  Full regression check: Windows 24→27 tests (added `test_native_jail_parity_windows`; one flaky
  `-j4` failure across the three concurrent native-jail test binaries, not reproduced in 5
  subsequent `-j4` runs — consistent with real but rare resource contention among multiple
  concurrent AppContainer/Job Object test processes under parallel load, not a regression; every
  serial run and every other parallel run was clean). Linux 20→21 tests (added
  `test_native_jail_parity_linux`), **all passing including `test_real_filesystem_adapter` for the
  first time** — the only Linux gap tracked since C2 is now closed. **L**
- **C5.** No-ambient-authority probe (008 §9 G3) specifically against `native-jail`. **M** **(done)**

  Three axes probed against both platforms (env, network, process enumeration); filesystem is NOT
  retested here -- C3's fs-escape-attempt case already is that axis's G3 proof for Windows, and
  Linux's fs containment is the already-tracked GitHub issue #5 gap, not duplicated. New probe modes
  (`probe_env`/`probe_net <port>`/`probe_proc <pid>`) added to both `hostile_child`(`_posix`)
  binaries; each axis follows C3's shape -- a contained probe, then a positive control proving the
  containment is real (a canary env var / loopback listener / host PID that an ordinary unsandboxed
  probe DOES see).

  **Real bug found and fixed, not just measured:** `NativeJailBackend::exec()`
  (`native_jail_backend.cpp`) called `CreateProcessW` with `lpEnvironment=nullptr`, which Win32
  defines as "inherit the calling process's environment" -- the guest was getting the FULL host
  process environment, including anything the launching process set for its own use (API keys,
  tokens, ...). Fixed with an explicit, fixed, minimal `CREATE_UNICODE_ENVIRONMENT` block, mirroring
  `LinuxNativeJailBackend`'s own fixed `envp[] = {"PATH=/usr/bin:/bin", nullptr}` (which already made
  the Linux side of this axis correct, no fix needed there). Non-obvious wrinkle hit while building
  the fix: a block containing only `SystemRoot`/`Path` made `CreateProcessW` itself fail with
  `ERROR_ENVVAR_NOT_FOUND` (203) specifically when combined with
  `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES` (i.e. only under a real AppContainer launch) --
  AppContainer process setup apparently resolves the per-package data folder under
  `%LOCALAPPDATA%\Packages\...` from the caller-supplied block rather than deriving it from the
  token. Fixed by including `LOCALAPPDATA`/`USERPROFILE` (host system-path facts, not secrets) in the
  minimal block alongside `SystemRoot`/`Path`. Confirmed via a standalone repro
  (`CreateProcessW` + `SECURITY_CAPABILITIES` isolated from the rest of the backend) before touching
  the real code, and reproduced/fixed on the real backend after.

  **Measured, both platforms:**
  - **env** -- contained on both. A host secret canary env var is invisible to the guest on Windows
    (after the fix above) and Linux (already true); an unsandboxed positive control DOES see it on
    both, proving the canary/probe mechanism itself is real.
  - **network** -- contained on both, for different real reasons. Windows: zero AppContainer
    capabilities are granted (no `internetClient`), so Windows Firewall denies even a host loopback
    listener the guest tries to reach. Linux: `CLONE_NEWNET`'s fresh network namespace has no
    interface configured (not even loopback brought up), so there is no route at all. Unsandboxed
    positive controls reach the same listener on both.
  - **process enumeration** -- **contained on Windows, a real gap on Linux.** Originally assumed
    Windows would leak (AppContainer's ACL model governs object access, not
    `CreateToolhelp32Snapshot` enumeration) -- measured the opposite: a Windows guest enumerates only
    a handful of its own processes (measured: 2-3) and a host canary process is genuinely invisible,
    positive-controlled by an unsandboxed prober that DOES see it (measured: ~400+ processes visible,
    canary found). Linux is the mirror image: the guest's `/proc` is never remounted inside its own
    `CLONE_NEWPID` namespace, so it reads the HOST's procfs instance and DOES see the host canary --
    the same root cause as the already-tracked fs-containment gap (`CLONE_NEWNS` alone populates
    nothing), not a new one. GitHub issue #5 updated (title and scope) to cover both the fs-escape
    gap it was filed for and this `/proc` finding -- one fix (populate the mount namespace via
    `pivot_root`/`chroot`, remount `/proc` inside it) closes both.

  Full regression check: Windows 27→28 tests (added `test_native_jail_ambient_authority_windows`),
  Linux 21→22 tests (added `test_native_jail_ambient_authority_linux`), all passing on both
  platforms (2 full `-j4` Windows runs clean, one fresh-container Linux run clean).
- **C6.** (done) Teardown-cycle proof (008 §9 G4) — scoped down from the RFC's full 10⁵ cycles to a
  machine-safe bounded count (CLAUDE.md's build/test resource caps apply), rationale documented, same
  pattern as M1 deferring 001's 10⁴-session gate. **M**

  New tests: `tests/test_native_jail_teardown_cycles_windows.cpp`,
  `tests/test_native_jail_teardown_cycles_linux.cpp`. 300 create/exec/destroy cycles (not 10⁵) against
  each `native-jail` half, after a 5-cycle warm-up (absorbs one-time setup cost — first AppContainer
  profile creation, first DACL grant, first delegated-cgroup-root controller enable — so the
  before/after delta measures steady-state per-cycle behavior, not first-call cost misclassified as a
  leak).

  This project has no ASan build configured (checked: no sanitizer flag anywhere in
  CMakeLists.txt/tests/CMakeLists.txt) — the census half of G4's parenthetical is what these tests
  perform; an ASan-instrumented build remains open follow-up work, not silently assumed covered (now
  noted in 008 §9's G4 bullet directly).

  Censused resources, chosen as the real per-cycle-allocated thing each backend's own code opens or
  mutates, not a generic proxy:
  - Windows: process handle count (`GetProcessHandleCount`) — catches a leaked Job Object/process/
    thread/pipe HANDLE; process private-bytes usage (`GetProcessMemoryInfo`) — most sensitive to
    exec()'s own two 1 MiB `CreatePipe` buffers per call, the highest-risk single resource;
    AppContainer DACL entry count on the reused mount — catches ACE accumulation from calling
    `grant_path` once per cycle on the same mount; `job_object_hostile_child.exe` process count
    (delta-based, not absolute-zero — see finding below).
  - Linux: open file descriptor count (`/proc/self/fd`) — catches a leaked pipe fd (six per `exec()`
    call: sync + stdout + stderr pipe ends); RSS (`/proc/self/status` `VmRSS`) — most sensitive to
    exec()'s own 1 MiB child-stack `mmap()` per call; delegated cgroup root directory-entry count —
    catches a leaked `CgroupLimits` directory.

  Non-vacuousness: each platform runs two small positive controls BEFORE the real loop, through the
  exact same measurement functions the real assertions use, deliberately producing a known-sized leak
  (50 leaked handles/fds; on Windows, `grant_path` called with two DIFFERING `read_write` values on a
  throwaway path — the case `app_container_profile.hpp`'s own header already documents as
  non-idempotent — to prove ACE accumulation really is detectable; on Linux, a raw `mkdir`/`rmdir`
  under the delegated root to prove the directory-entry count really does move). Both platforms'
  positive controls passed before the real loop ran.

  Measured results, both platforms, zero growth on every axis after 300 cycles: Windows — handles
  114→114, private bytes unchanged (a few KB of allocator noise across repeated runs, always far under
  the 50 MiB slack), DACL entries 7→7 (confirms `grant_path`'s documented merge behavior for repeated
  SAME-value calls, now measured rather than assumed). Linux — fds 4→4, RSS 3584→3712 KB (well under
  the 50 MiB slack), cgroup delegated-root entry count 38→38 (see finding below for why 38, not 0).

  Two real findings this task surfaced, both about the TEST's own methodology, not the backends:
  1. An early version of the Windows test asserted `job_object_hostile_child.exe` count was exactly
     zero both before and after the loop. Under a parallel `ctest -j4` run this failed —
     `test_job_object_limits` and `test_native_jail_parity_windows` share the same hostile-child
     binary and can legitimately have their own instances alive at that moment. Fixed by making this
     check delta-based (before vs. after THIS test's own loop, like the other three axes) and marking
     the test `RUN_SERIAL` in `tests/CMakeLists.txt` to minimize the overlap in the first place. Not a
     backend leak — confirmed by re-running clean 3/3 times after the fix.
  2. The Linux cgroup-directory census baseline is 38, not ~0: `/sys/fs/cgroup/agentengine` (the
     delegated root) is itself a cgroup, and any cgroup directory always contains dozens of standard
     interface files (`cgroup.procs`, `memory.max`, `pids.max`, ...) alongside whatever child-cgroup
     subdirectories exist. Those files don't come or go, so the count stays exactly as sensitive to a
     leaked child subdirectory as a from-zero count would be — confirmed directly by the positive
     control (38→39→38 across a real mkdir/rmdir). Documented in the test file's own header so a
     future reader isn't confused by a non-zero baseline.

  Verified: Windows full `ctest -j4` run 3/3 clean (one unrelated pre-existing flake in
  `test_native_jail_backend_windows`, confirmed to pass standalone and on a subsequent full run — the
  already-known `-j4` timing-sensitivity noted in prior C-phase sessions, not introduced by this task)
  — 27/27 passing, up from 26. Linux: fresh Docker container, full build, `ctest` 23/23 (22 pass + 1
  intentional skip), up from 22.

### Phase D — WASM plugin host (009), once C exists to run it in

- **D1.** (done) Wasmtime 47.0.3 wired in as `AGENTENGINE_WITH_WASM`, Windows + Linux (decision 5). **M**

  Root `CMakeLists.txt`: `AGENTENGINE_WITH_WASM` (off by default, matching
  `AGENTENGINE_BUILD_PYTHON_RUNNER`'s posture for the project's other tier-2 heavy dependency).
  `AGENTENGINE_VENDOR_WASMTIME=ON` (the default) fetches wasmtime 47.0.3's official C API release
  archives directly from `bytecodealliance/wasmtime`'s GitHub releases via `FetchContent` — both
  archives' SHA256 verified by hand against a fresh download during this task (Windows:
  `952dfbc8...bb77c5`, Linux: `aaa3621f...aefd732`; full hashes in `CMakeLists.txt`) before pinning,
  not copied from anywhere unverified. Override: `-DAGENTENGINE_VENDOR_WASMTIME=OFF
  -DAGENTENGINE_WASMTIME_ROOT=<path>` for a local install. New target `agentengine::wasmtime_vendor`
  (an `IMPORTED` library wrapping the vendored include/lib) — the full-featured `lib/` build, not
  `min/lib/`'s reduced Pulley-only build, confirmed via the release's own `include/wasmtime/conf.h`
  to be the only variant with `WASMTIME_FEATURE_COMPONENT_MODEL`, which 009's plugin ABI needs.

  Deliberately narrow scope, matching `src/backends/wasm/README.md`'s own "no real logic yet" note:
  this task imports and proves the dependency, nothing under `src/backends/wasm/` was written — no
  `SandboxBackend` implementation exists yet (that's D3, security-critical, goes through
  design→red-team→prove→judge per CLAUDE.md before it's real code). New test:
  `tests/test_wasmtime_smoke.cpp`, gated behind `AGENTENGINE_WITH_WASM`, linking directly against
  `agentengine::wasmtime_vendor` — compiles a trivial WAT module at run time, instantiates it with
  zero imports, calls its one export, and asserts the *actual returned value* (2 + 3 = 5), not just
  that the binary linked and ran.

  One real finding, Linux only: the released `libwasmtime.so` has no embedded `SONAME`. Per glibc
  `ld.so`'s documented behavior, a `DT_NEEDED` entry containing a slash is treated as a literal
  (CWD-relative, if not absolute) pathname and bypasses `RPATH`/`RUNPATH`/`LD_LIBRARY_PATH` search
  entirely — measured directly: a first attempt (dynamic-linking both platforms, `BUILD_RPATH` on
  Linux) produced a test binary whose `DT_NEEDED` was the literal build-relative path
  `_deps/ae_vendored_wasmtime-src/lib/libwasmtime.so`, which only ran when launched from the exact
  directory that path resolves against (an accident, not a fix) and failed under `ctest` (whose test
  CWD is the test's own binary directory, not the top-level build directory) with "cannot open
  shared object file". Fixed by linking Linux against the static archive (`libwasmtime.a`) instead
  — self-contained, nothing to locate at run time — needing `pthread`/`dl`/`m` on the final link
  (measured directly, Rust's usual static-lib convention), declared as the imported target's own
  `INTERFACE_LINK_LIBRARIES` so no test file needs its own link-library wiring. Windows keeps
  dynamic linking (`wasmtime.dll` + import lib, `PATH`-wired at test-run time via each test's
  `ENVIRONMENT` property, same posture as `AGENTENGINE_PYTHON_DLL_DIR`) — its DLL link worked
  correctly from the start, no equivalent problem measured there.

  Verified: default build (`AGENTENGINE_WITH_WASM` left OFF) configures and builds unaffected — the
  option genuinely gates the dependency out, not just skips using it. With the option ON: Windows
  full `ctest -j4` run 2/2 clean, 28/28 passing (up from 27). Linux: fresh Docker container, full
  build, `ctest` 24/24 (23 pass + 1 intentional skip, up from 22), including the smoke test verified
  both directly from an unrelated working directory and via `ctest` itself (the exact scenario the
  `SONAME` finding above broke before the static-link fix).
- **D2.** (done) `wit/ae-tool.wit` authored — the `ae:tool` world (009 §2), closing the "contract of
  record is currently empty" gap this survey found. **M**

  Package `ae:tool@1.0.0`, three interfaces (`types`, `host`, `guest`) and one world (`tool`,
  matching manifest.toml's `world = "ae:tool@1.0.0"` string, 009 §3, under the single-world-package
  shorthand). `types` mirrors 003's native content model field-for-field (`blob-ref` ==
  `BlobRef{digest, media_type, size, store}`; `content-item`'s `tainted` flag == `ContentItem.tainted`
  — confirmed against `include/agentengine/core/content.hpp`, not just the RFC text) so the host's
  006 §3 step-9 normalize is a direct mapping. `host` exposes 009 §5's import list (`log`, `metrics`,
  `fs`, `http`, `secrets`, `clock`, `random`, `blob`, `tool-call`) as functions taking a
  `capability-handle` — a WIT `resource`, not an opaque string or integer, specifically because a
  `resource` can only be constructed host-side and transferred in, never fabricated guest-side: this
  is I2 made an ABI-level fact for this world, not merely a convention documented alongside it.
  `guest` exports `list-tools` (schema discovery — JSON Schema 2020-12 text per tool, per 006 §1) and
  `invoke` (006 §3 step 8 exactly — the host has already resolved/validated/tainted/authorized/
  approved/admitted/bound before calling it; normalize/account happen after it returns).

  Deliberately narrow scope, same posture as D1: **no host implementation** (that's D3,
  security-critical, goes through design→red-team→prove→judge per CLAUDE.md before it is real code)
  and **no progress reporting / scheduling / background tools** (006 §6a/§6b) — this milestone's own
  "explicitly deferred past M2" list already excludes those (they need 019's durability machinery,
  and "an M2 native tool call is synchronous and immediate"), so the world does not speculate a shape
  for them now. Three design choices the RFCs leave unstated are called out explicitly in the file's
  own header comment rather than silently assumed: bespoke `ae:tool/host` imports vs. raw
  `wasi:filesystem`/`wasi:http`/etc. (neither 008 nor 009 says either way — chosen because 009 §5's
  imports are named and capability-intersected per invocation, a shape standard WASI interfaces can't
  express); capability handles as WIT `resource`s; and the taint flag's necessarily lossy crossing
  (WIT has no equivalent to the native `Tainted<T>` compile-time wrapper, 007 §4's G3 property).

  Verified with a real WIT toolchain, not by eye: a local `wasm-tools 1.248.0` install (not vendored
  into the build — this is a one-off authoring check, the same ad hoc-verification posture as D1's
  Docker passes) parses the file cleanly (`wasm-tools component wit`) and — the stronger check —
  resolves the `tool` world into valid Component Model metadata against a real (empty) core module
  (`wasm-tools component embed --world tool`, exit 0), proving the world's imports/exports/resources
  form a semantically valid Component Model contract, not merely syntactically parseable text.
- **D3.** Minimal WASM component host: load, verify manifest-vs-imports (009 §4/§10 G2), instantiate
  under the `wasm` `SandboxBackend` profile, invoke, destroy. **XL**
- **D4.** One real `ae:tool` component (a trivial echo/add tool from a Component-Model-capable
  toolchain) loads and executes identically across platforms — 009 §10 G1, the milestone's other
  named exit-criterion item. **L**
- **D5.** Manifest-capability-mismatch negative proof (miniature G2). **S**

### Phase E — Agent CRTP surface (002), now that there's something to author

- **E1.** Remaining policy tags (`Concurrency`, `Retry`, `Memory`, `Middleware`, `Stateless`,
  `OutputSchema`) added as empty/near-empty stub types for API completeness, matching 002 §3's table
  — real behavior for most is out of scope until the milestone that owns it (documented per tag).
  **M**
- **E2.** `register_agent<A>()` — the real metadata compiler: builds the agent metadata table, runs
  002 §6's 8 named validation checks. Checks needing machinery this milestone doesn't build
  (credentials/004, handoff-cycle/014) are stubbed to always-pass with a tracked comment, not
  silently skipped. **L**
- **E3.** An agent declaring `Tools<TrivialNativeTool>` and a matching `Capabilities<...>` ceiling
  actually runs one tool call end-to-end through B's pipeline — the headline exit-criterion sentence,
  made real. Mostly wiring; A and B do the real work. **M**
- **E4.** 002 §8 G3 miniature — validation rejects at least the capability-ceiling-mismatch and
  tool-name-collision defect classes with a specific diagnostic, negative test per class (full
  8-class suite deferred alongside E2's scoping). **M**

### Phase F — Cross-cutting ADR-track tasks (flagged explicitly by the roadmap for M2)

- **F1.** First-party egress proxy design (008 §10 Q3) — full design→red-team→prove→judge, produces
  an ADR. Security-critical: host-mediated egress for every profile depends on it being right. **XL**
- **F2.** Policy-reachability tool (007 §9 G6) — new CI tooling enumerating `{capability kind, tool,
  taint level}` against whatever mechanical enforcement A3 actually implements (decision 4's scope,
  not the full declarative language). **L**

## What's explicitly deferred past M2

- 007 §5's full declarative policy DSL — mechanical possession/attenuation only this milestone
  (decision 4).
- 006 §5/§6a/§6b: parallel-batch scheduling (G4), progress reporting (G5), schedule/watch/suspend
  (G6), background tasks (G7–G9) — all need 019 (durability, M4) to mean anything; an M2 native tool
  call is synchronous and immediate.
- 008's full G4 (10⁵ cycles — C6 uses a bounded count instead), G5 (cold-start vs. 023 budgets — 023
  itself stays `TBD-baselined` until M8), G6 (downgrade visibility), G8 (snapshot fidelity, `wasm`
  only, needs D built out further than M2's minimal host).
- 009's G4/G4a (warm-invocation/streaming budgets vs. 023), G6 (one real C/C++ library shipped as a
  plugin, 009 §7).
- 002's G1 (objdump zero-cost parity), G2 (YAML/C++ metadata byte-identity — needs 015), G4
  (handoff/sub-agent/remote-agent compile-time indistinguishability — needs 014).
- ADR-003's residual, still-open risks (gadget-chaining variant, fail-closed C-reentrancy,
  `sys.path`-shadowing precondition) — inherited into M3 when `PythonRunner` is actually built, not
  M2's problem since M2 never touches CPython (decision 3).
- The `remote` sandbox profile entirely (M9).

## Handover & kick-off

Milestone 2 starts 2026-08-05, immediately following M1. Explicit deviation from M1's single-pass
delivery: given the real 006↔007↔008↔009 cycle and this milestone's size relative to M1's, phases
A–F are implemented and verified **sequentially** — each phase gets its own build + test +
Docker-Linux-verification pass before the next phase starts, with a check-in between phases rather
than one atomic delivery. Same rigor as M0/M1, applied at finer grain because M2 is materially
bigger.
